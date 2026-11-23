/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* node.c - the node's lifecycle state, its reason, and the WAL's say in it; in both builds, as a
 * standalone daemon has a lifecycle too.  cluster.c owns only the STALLED overlay's detector. */
#include <pthread.h>
#include "cluster.h"
#include "wal.h"
#include "compat/dprint.h"

static int node_state;                 /* PC_NST_*, STARTING = 0 */
static int node_stalled;               /* the STALLED overlay is up */

int pc_node_stalled(void)
{
	return __atomic_load_n(&node_stalled, __ATOMIC_RELAXED);
}

void pc_node_set_stalled(int on)
{
	__atomic_store_n(&node_stalled, on ? 1 : 0, __ATOMIC_RELAXED);
}

/* the node's own reason, set beside the state change that caused it; read by the ALIVE build and
 * /stats, so a FAILED card says why */
static int node_reason = PC_NRE_NONE;
/* every transition checks and stores under it: the WAL thread and the cluster thread both set */
static pthread_mutex_t node_mx = PTHREAD_MUTEX_INITIALIZER;

/* STALLED is an overlay, not a transition: it masks READY while the heartbeat thread sees no
 * progress, and nothing underneath moves (PC_NST_STALLED in cluster.h) */
static int stalled_now(void);

int pc_node_reason(void)
{
	int r;

	if (stalled_now())
		return PC_NRE_APPLY_STALL;
	r = __atomic_load_n(&node_reason, __ATOMIC_RELAXED);
	/* the initial STARTING never went through pc_node_state_set(), which gives the rest theirs */
	if (!r && __atomic_load_n(&node_state, __ATOMIC_RELAXED) == PC_NST_STARTING)
		return PC_NRE_JOINING;
	return r;
}

void pc_node_reason_set(int code)
{
	__atomic_store_n(&node_reason, code, __ATOMIC_RELAXED);
}

const char *pc_node_reason_text(int code)
{
	switch (code) {
	case PC_NRE_WAL_DROP:
		return "the write-ahead log dropped acknowledged writes - "
			"raise [wal] ring_kb or fix the storage, then restart";
	case PC_NRE_WAL_IO:
		return "the device refused a write-ahead-log write or sync - "
			"the log names the error; fix the storage, then "
			"restart";
	case PC_NRE_WAL_HEAL:
		return "the write-ahead log lost acknowledged writes and a "
			"snapshot is making them durable again - writes refused, "
			"reads served; it returns to service by itself";
	case PC_NRE_APPLY_STALL:
		return "its cluster thread has stopped making progress - "
			"replication, pulls and the control plane are not "
			"being served here; it returns by itself when the "
			"thread moves again";
	case PC_NRE_RECOVERING: return "replaying its own state";
	case PC_NRE_JOINING:    return "waiting for the fleet's map";
	case PC_NRE_CONNECTING: return "proving its links to every member before it serves";
	case PC_NRE_DRAINING:   return "handing its data over";
	case PC_NRE_FAILED_OTHER:
		return "failed without recording a reason - the log at the "
			"moment of the transition is the only record";
	default:                return "";
	}
}

const char *pc_node_state_name(int st)
{
	switch (st) {
	case PC_NST_STARTING:    return "starting";
	case PC_NST_RECOVERING:  return "recovering";
	case PC_NST_READY:       return "ready";
	case PC_NST_DRAINING:    return "draining";
	case PC_NST_FAILED:      return "failed";
	case PC_NST_STALLED:     return "stalled";
	case PC_NST_HEALING:     return "healing";
	case PC_NST_CONNECTING:  return "connecting";
	default:
		break;
	}
	return "?";
}

static int stalled_now(void)
{
	return __atomic_load_n(&node_stalled, __ATOMIC_RELAXED) &&
		__atomic_load_n(&node_state, __ATOMIC_RELAXED) == PC_NST_READY;
}

int pc_node_state(void)
{
	if (stalled_now())
		return PC_NST_STALLED;
	return __atomic_load_n(&node_state, __ATOMIC_RELAXED);
}

#ifdef PC_TESTHOOKS
void (*pc_node_test_mid)(void);        /* runs between a transition's checks and its store */
#endif

/* the one writer, so every transition is logged in one shape; DRAINING is terminal: a goodbye went
 * out and peers are purging us, so a late ASSIGN must not walk it back to READY */
static void state_set_locked(int st, int heal_releasing)
{
	int cur = __atomic_load_n(&node_state, __ATOMIC_RELAXED);

	/* FAILED is TERMINAL too, bar a clean stop: in shard mode failing and returning re-shards
	 * twice a fault, and a flapping node thrashes the fleet - it waits for a restart */
	if (cur == st || cur == PC_NST_DRAINING)
		return;
	if (cur == PC_NST_FAILED && st != PC_NST_DRAINING)
		return;
	/* only the heal ends a heal (or FAILED, or a clean stop): a late ASSIGN or a finished join
	 * must not put a node still making its writes durable back into selection */
	if (cur == PC_NST_HEALING && st != PC_NST_FAILED &&
	        st != PC_NST_DRAINING && !heal_releasing)
		return;
#ifdef PC_TESTHOOKS
	if (pc_node_test_mid)
		pc_node_test_mid();
#endif
	__atomic_store_n(&node_state, st, __ATOMIC_RELAXED);
	/* every state carries its reason; one set MORE specifically for the state moved to stands
	 * (pc_node_reason_set() first is how the WAL says WHY it failed) */
	{
		int want = st == PC_NST_RECOVERING ? PC_NRE_RECOVERING
			: st == PC_NST_STARTING   ? PC_NRE_JOINING
			: st == PC_NST_DRAINING   ? PC_NRE_DRAINING
			: st == PC_NST_HEALING    ? PC_NRE_WAL_HEAL
			: st == PC_NST_CONNECTING ? PC_NRE_CONNECTING
			: PC_NRE_NONE;         /* READY explains itself */
		/* the reason UNDERNEATH - not the overlay's, which is
		 * nobody's to keep */
		int have = __atomic_load_n(&node_reason, __ATOMIC_RELAXED);

		if (st == PC_NST_FAILED) {
			/* the caller's reason stands; a bare FAILED with none
			 * is still better than silence */
			if (!have)
				pc_node_reason_set(PC_NRE_FAILED_OTHER);
		} else if (have != want) {
			pc_node_reason_set(want);
		}
	}
	LM_NOTICE("cluster: node state %s -> %s%s%s\n", pc_node_state_name(cur),
		pc_node_state_name(st),
		*pc_node_reason_text(pc_node_reason()) ? " - " : "",
		pc_node_reason_text(pc_node_reason()));
}

void pc_node_state_set(int st)
{
	pthread_mutex_lock(&node_mx);
	state_set_locked(st, 0);
	pthread_mutex_unlock(&node_mx);
}

/* the WAL has been discarding acknowledged writes: leave the client map so the load goes to nodes
 * that can honour it, while still serving READS - unsafe is accepting writes, not the data held */
void pc_node_wal_shed(int why)
{
	/* no exemption for a JOINING node: the WAL sheds ONCE (the startup overrun arrives late,
	 * still JOINING).  FAILED, not DRAINING: draining strands every key with no TTL */
	/* the reason BEFORE the state, or a node failed by its WAL says "failed without recording a
	 * reason"; a DRAINING node refuses the move and keeps its own */
	pthread_mutex_lock(&node_mx);
	if (__atomic_load_n(&node_state, __ATOMIC_RELAXED) == PC_NST_DRAINING) {
		pthread_mutex_unlock(&node_mx);
		return;
	}
	pc_node_reason_set(why == PC_WAL_SHED_IO ? PC_NRE_WAL_IO
		: PC_NRE_WAL_DROP);
	state_set_locked(PC_NST_FAILED, 0);
	pthread_mutex_unlock(&node_mx);
	LM_CRIT("node state -> FAILED: %s - still answering reads, "
		"refusing writes until the storage or the [wal] sizing is "
		"fixed and the node is restarted\n", why == PC_WAL_SHED_IO
		? "the device refused a WAL write or sync"
		: "the WAL lost acknowledged writes");
}

/* the WAL's heal: HEALING on the first loss it can heal, READY when the snapshot covering it is
 * published; a node not READY yet keeps its state (the heal completes before it serves writes) */
void pc_node_wal_heal(int phase)
{
	int cur;

	pthread_mutex_lock(&node_mx);
	cur = __atomic_load_n(&node_state, __ATOMIC_RELAXED);
	if (phase && cur == PC_NST_READY) {
		pc_node_reason_set(PC_NRE_WAL_HEAL);
		state_set_locked(PC_NST_HEALING, 0);
	} else if (!phase && cur == PC_NST_HEALING) {
		state_set_locked(PC_NST_READY, 1);
	}
	pthread_mutex_unlock(&node_mx);
}

