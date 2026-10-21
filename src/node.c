/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * node.c - the node's lifecycle state, its reason, and the WAL's say in
 * it (S250: moved out of cluster.c).
 *
 * A standalone daemon has a lifecycle too - STARTING, RECOVERING while it
 * replays, READY, HEALING while a snapshot makes lost WAL writes durable
 * (S229), FAILED when the storage cannot keep up (S215) - and it drives it
 * without a cluster.  So this lives in both builds; cluster.c reads and
 * publishes it, and owns only the STALLED overlay's detector (S216), which
 * sets the flag here through pc_node_set_stalled().
 */
#include "cluster.h"
#include "wal.h"
#include "compat/dprint.h"

static int node_state;                 /* PC_NST_*, STARTING = 0 */
static int node_stalled;               /* S216: the STALLED overlay is up */

int pc_node_stalled(void)
{
	return __atomic_load_n(&node_stalled, __ATOMIC_RELAXED);
}

void pc_node_set_stalled(int on)
{
	__atomic_store_n(&node_stalled, on ? 1 : 0, __ATOMIC_RELAXED);
}

/* S186: the node's own reason, and the words for a code.  Set beside
 * the state change that caused it; read by the ALIVE build and /stats.
 * A card that says FAILED and nothing else sends an operator to the
 * logs of a node that may not be the one they are looking at. */
static int node_reason = PC_NRE_NONE;
static int heal_releasing;             /* S229: pc_node_wal_heal(0) only */

/* S216: STALLED is an overlay, not a transition - it masks READY while
 * the heartbeat thread sees no progress, and nothing underneath moves.
 * See PC_NST_STALLED in cluster.h. */
static int stalled_now(void);

int pc_node_reason(void)
{
	if (stalled_now())
		return PC_NRE_APPLY_STALL;
	return __atomic_load_n(&node_reason, __ATOMIC_RELAXED);
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

/* One writer for the whole machine, so every transition is logged in the
 * same shape and a state can never be set from two places that disagree
 * about what it means.  DRAINING is terminal: a goodbye has gone out and
 * peers are purging us, so a late ASSIGN must not walk it back to READY. */
void pc_node_state_set(int st)
{
	int cur = __atomic_load_n(&node_state, __ATOMIC_RELAXED);

	/*
	 * DRAINING and FAILED are both TERMINAL, and FAILED is terminal for
	 * a reason that only shows up in shard mode: ownership is computed
	 * from the member set, so a node going FAILED re-shards the cluster
	 * and a node coming BACK re-shards it again - two full rebalances
	 * for one fault, and a flapping node would thrash the fleet
	 * indefinitely.  The cheap-looking recovery is the expensive one.
	 *
	 * So a FAILED node stays failed until an operator restarts it,
	 * which is also the honest reading of what put it there: its
	 * storage could not keep up, and nothing about that fixes itself.
	 *
	 * FAILED -> DRAINING is the one transition still allowed: a failed
	 * node must still be able to shut down cleanly and say goodbye.
	 */
	if (cur == st || cur == PC_NST_DRAINING)
		return;
	if (cur == PC_NST_FAILED && st != PC_NST_DRAINING)
		return;
	/* S229: only the heal ends a heal (or FAILED, or a clean stop) - a
	 * late ASSIGN or a finished join must not put a node that is still
	 * making its writes durable back into selection */
	if (cur == PC_NST_HEALING && st != PC_NST_FAILED &&
	        st != PC_NST_DRAINING && !heal_releasing)
		return;
	__atomic_store_n(&node_state, st, __ATOMIC_RELAXED);
	/*
	 * S193: every state carries its reason, not just the one that had a
	 * caller remember to set it.  S186 wired PC_NRE_WAL_DROP at the WAL
	 * failure and left recovering, joining and draining with codes
	 * nobody assigned - so a card in any of those states said the state
	 * and nothing else, which is the complaint S186 existed to fix.
	 *
	 * A caller that has already said something MORE specific for the
	 * state it is moving to keeps it: pc_node_reason_set() before
	 * pc_node_state_set() is how the WAL failure says WHY it failed,
	 * and a generic "it failed" must not overwrite that.
	 */
	{
		int want = st == PC_NST_RECOVERING ? PC_NRE_RECOVERING
			: st == PC_NST_STARTING   ? PC_NRE_JOINING
			: st == PC_NST_DRAINING   ? PC_NRE_DRAINING
			: st == PC_NST_HEALING    ? PC_NRE_WAL_HEAL  /* S229 */
			: PC_NRE_NONE;         /* READY explains itself */
		/* the reason UNDERNEATH - not the S216 overlay's, which is
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

/*
 * The WAL has been discarding acknowledged writes for several seconds:
 * leave the client map so the load goes to nodes that can honour it.
 *
 * Same two steps a clean shutdown takes - DRAINING makes
 * pc_clsel_eligible() reject us, the goodbye makes peers purge us now
 * rather than after a liveness timeout - but the process stays up and
 * keeps serving READS, which are still correct: what is unsafe here is
 * accepting new writes, not answering for data already held.
 */
void pc_node_wal_shed(int why)
{
	/* S215: no exemption for a JOINING node.  There was one, left over
	 * from when shedding meant DRAINING plus a goodbye (eb0a156), which
	 * a node with no membership could not do.  FAILED needs none - and
	 * the WAL sheds ONCE, so returning here lost the event for good,
	 * including the startup overrun pc_wal_on_shed() delivers late on
	 * purpose: pc_cluster_init() installs this while the role is still
	 * JOINING. */
	/*
	 * FAILED, not DRAINING, and NO goodbye.  Draining means departure -
	 * hand the data over and go - and a node that borrows it to say
	 * "I am broken" strands every key it holds that has no TTL.  A
	 * goodbye would compound that by making peers purge us outright.
	 *
	 * FAILED keeps us a member whose state everyone can see: out of
	 * pc_clsel_eligible() so no client picks us for new work, still
	 * heartbeating so the fleet watches us recover, still answering
	 * reads because the data we hold is correct.
	 */
	/* the reason BEFORE the state (S193) - S186 defined PC_NRE_WAL_DROP
	 * and nothing ever set it, so a node failed by its WAL said "failed
	 * without recording a reason" */
	pc_node_reason_set(why == PC_WAL_SHED_IO ? PC_NRE_WAL_IO
		: PC_NRE_WAL_DROP);
	pc_node_state_set(PC_NST_FAILED);
	LM_CRIT("node state -> FAILED: %s - still answering reads, "
		"refusing writes until the storage or the [wal] sizing is "
		"fixed and the node is restarted\n", why == PC_WAL_SHED_IO
		? "the device refused a WAL write or sync"
		: "the WAL lost acknowledged writes");
}

/* S229: the WAL's heal.  HEALING on the first loss it can heal, READY
 * again when the snapshot covering it is published.  A node that was not
 * READY (still recovering or joining) keeps its state: it is not serving
 * writes yet, and the heal completes before it is. */
void pc_node_wal_heal(int phase)
{
	int cur = __atomic_load_n(&node_state, __ATOMIC_RELAXED);

	if (phase) {
		if (cur != PC_NST_READY)
			return;
		pc_node_reason_set(PC_NRE_WAL_HEAL);
		pc_node_state_set(PC_NST_HEALING);
		return;
	}
	if (cur != PC_NST_HEALING)
		return;
	heal_releasing = 1;
	pc_node_state_set(PC_NST_READY);
	heal_releasing = 0;
}

