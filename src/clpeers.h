/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clpeers.h - the peer table: its slots, the liveness predicate, and the
 * duplicate-id grace window (M1).
 *
 * The table is the membership plane's state: the peer thread writes it,
 * workers read `.node` with acquire, and cluster.h promises them that.
 * It lives outside cluster.c so the table's own rules - liveness at the
 * boundary, slot reuse, duplicate-id arbitration - can be exercised
 * without a fleet.
 *
 * What is deliberately NOT here: anything that sends, logs or notifies.
 * A scan followed by a sealed datagram (broadcast_live), a departure
 * that tells clients and the shard map (peer_gone), and the bootstrap
 * reserve note stay in cluster.c as orchestration.  This module answers
 * questions about the table and mutates it, and nothing else - which is
 * what lets its test link against clpeers.o alone.
 *
 * Every entry point takes `now` rather than reading a clock, so a test
 * can place a peer exactly either side of PEER_UP_MS.
 */
#ifndef PC_CLPEERS_H
#define PC_CLPEERS_H

#include <netinet/in.h>

#include "config.h"
#include "cluster.h"
#include "psrelay.h"                  /* PS11: struct psr_path */
#include "psinterest.h"                /* PS12: struct psv_view */
#define PEER_UP_MS      10000  /* 10 beats; was 5 - same margin
                                * arithmetic as MASTER_DEAD_MS.  A dead
                                * peer is noticed in 10s instead of 5;
                                * per-op paths never wait on this (pull
                                * timeouts are their own, much shorter
                                * clocks). */
#define DUP_GRACE_MS (3LL * PEER_UP_MS)

struct peer {
	struct sockaddr_in addr;
	int node;                      /* learned from heartbeats, 0 unknown */
	long long last_seen_ms;
	/* S127: when peer_gone() vacated this slot.  The purge clears
	 * ->node, so a grace anchored on liveness alone is bypassed the
	 * instant a peer is purged - which is most of the time, because
	 * the purge is what a real departure looks like. */
	long long gone_ms;
	unsigned int free_mb;
	unsigned int total_mb;
	int client_port;               /* S34: where CLIENTS dial this peer
	                                * (learned from its ALIVE) */
	int resp_port;                 /* S49: its RESP door, 0 if it has
	                                * none or its build predates this */
	int http_port;                 /* S78: its HTTP door, 0 = none or not reported */
	unsigned int entries;          /* S82: records it last reported holding */
	unsigned char start_kind;      /* S82: PC_START_* from its last ALIVE */
	unsigned char cold_fresh;      /* S83: restarted cold; its next
	                                * ESTABLISHED means it completed on
	                                * its own - no resync from zero owed */
	unsigned char col_synced;      /* S69: this peer has been sent the
	                                * created set once, when it appeared */
	unsigned char clients_told;    /* S107: this node's clients were told
	                                * it joined; told again when it goes */
	int told_node;                 /* S107: the id they were told - a
	                                * restart at this address under a new
	                                * identity changes it in place */
	/* S105: the open write-path push group to this peer.  Workers
	 * append under qmx; whoever fills it, or the cluster loop's timer,
	 * seals and sends it.  Allocated on first use. */
	/* S110: the write-path push group is no longer here - it is per
	 * THREAD per peer (struct wgroup below), so no lock sits on the
	 * write path.  Measured with the lock: 57% of every worker's time
	 * in futex, throughput flat from 50 to 400 clients. */
	/* S160: its open clients as its last ALIVE reported them; cl_reported
	 * = 0 for a build before the field */
	unsigned int cl_open, cl_resp, cl_bin, cl_json, cl_nresp;
	unsigned char cl_reported;
	/* S163: its collection block from its last ALIVE */
	unsigned char cols_reported, ncols, ncols_total;
	struct clmemb_col cols[CLMEMB_COLS_MAX];
	/* PS11: its pub/sub relay port and whether it has proven it answers
	 * there; ps_since_ms / ps_warned say "advertised, never answered" once */
	struct psr_path ps;
	long long ps_since_ms;
	unsigned char ps_warned;
	/* PS12: what it wants relayed.  pv_mode and the filter's bits are read
	 * by publishing workers under ps_int_seq; the rest is the cluster
	 * thread's */
	unsigned char pv_mode;         /* PV_ALL / PV_FILTER / PV_BROADCAST */
	uint8_t *pv_bloom;             /* PSB_BYTES, from its first full state */
	struct psv_view pv;
	uint64_t pv_hb;                /* the interest version it last told us */
	unsigned char pv_everything;   /* its full state said: everything */
	long long pv_uncovered_ms, pv_req_ms;
	long long boot_ms;             /* S78: last_seen_ms minus its reported uptime;
	                                * 0 = older build.  Uptime is derived from it,
	                                * so a stale heartbeat does not freeze it */
	int mem_tier;                  /* PCACHE_MEM_*, from its heartbeat */
	int nstate;                    /* B1: PC_NST_*, learned from its
	                                * ALIVE.  A peer that predates the
	                                * field sends a shorter datagram and
	                                * reads as READY, which is what it
	                                * behaved as. */
	unsigned int live_kb;          /* live cell KB: live/total is the
	                                * rebalancer's leveling metric (free
	                                * and used are chunk-granular and
	                                * peak-sticky until reclaim) */
	/* eager store: the budgeted sweep is RESUMABLE - a cursor per
	 * collection, plus the tick the current cycle started at and
	 * whether anything was lost during it.  Without the cursor a
	 * budget cut restarted at bucket 0 forever, so a keyspace larger
	 * than one budget never replicated past it (measured: exactly
	 * 4MiB, re-sent every tick, indefinitely). */
	unsigned int repl_cursor[PC_MAX_COLLECTIONS];
	unsigned int repl_cycle[PC_MAX_COLLECTIONS];
	unsigned char repl_dirty[PC_MAX_COLLECTIONS];
	/* eager store: per-collection replication high-water (a wtick).
	 * Records newer than the mark still need pushing to this peer.
	 * Zeroed when the slot is (re)claimed: a rejoin resyncs fully. */
	/* S127: the spread holder set changed and this peer must be
	 * re-offered what placement now makes it hold.  A flag of its own
	 * rather than backfill[], which carries two EAGER-era rules that
	 * are wrong here: only the lowest live id may send one (true when
	 * every peer holds every record, false when each holds K of them),
	 * and one armed for an ESTABLISHED peer is dropped as stale (S102) -
	 * but a set-change repair targets established peers by definition. */
	unsigned char setrepair[PC_MAX_COLLECTIONS];
	unsigned int repl_mark[PC_MAX_COLLECTIONS];
	/* S174: the fleet-wide count of unconfirmed oversized-copy batches
	 * as it stood when this cycle began.  The mark advances only if it
	 * has not moved - the bulk plane answers after the slice returns,
	 * so its outcome cannot ride the slice's own `clean`. */
	unsigned int repl_bulk0[PC_MAX_COLLECTIONS];
	/* Identity survives this peer's restarts; incarnation does not.
	 * A change in EITHER means its memory is gone and everything owed
	 * to it must be resent - which the node id cannot tell us, since
	 * a rejoiner keeps its id.  has_ident = 0 for an older build. */
	int has_ident;
	unsigned char ident[16];
	unsigned int incarn;
	/* set when this peer's incarnation changes: it came back empty, so
	 * it needs the PASSIVE copies too - once, not for ever */
	unsigned char backfill[PC_MAX_COLLECTIONS];
	/* S82: when this peer last arrived empty - the sender holdoff is
	 * measured from here, not from the flag, which can go stale */
	long long fresh_ms;
	/* S81: the sweep cycle in progress to this peer has been a
	 * backfill from its FIRST bucket.  Only such a cycle, walked
	 * clean, may clear backfill[]: one that started as an ordinary
	 * cycle skipped the copies at the head of the table, and one that
	 * lost the sender role part-way will skip them at the tail. */
	unsigned char repl_bfcycle[PC_MAX_COLLECTIONS];
};

/* The membership plane's state.  `C` embeds one of these; every function
 * below takes the pointer. */
struct clpeers {
	struct peer peers[PC_CL_MAXPEER];
	int n_peers;
	struct { struct in_addr addr; long long first_ms; }
		dup_seen[PC_CL_MAXPEER];   /* joiners with a duplicate id */
};

/* ---- the peer table, read through one predicate (S139) ----------------
 * Thirty open-coded scans spelled the liveness test four ways: with a
 * hoisted `now` and without, negated as a `continue` guard, and with or
 * without the acquire load on `p->node`.  It is one question and it is
 * asked here.  The acquire is the strongest form any caller used and is
 * what cluster.h promises workers; a plain read would weaken the paths
 * that race with membership, and on x86-64 it costs nothing.
 *
 * Callable from any thread: none of these takes a lock, so each sees
 * whatever the membership tick last published, which is what every
 * open-coded scan saw too. */
static inline int clpeers_live(const struct peer *p, long long now)
{
	return __atomic_load_n(&p->node, __ATOMIC_ACQUIRE) &&
	        now - p->last_seen_ms < PEER_UP_MS;
}

/*
 * S127: a peer that still holds its PLACEMENT SLOTS.  Wider than
 * clpeers_live() on purpose, and the difference is the whole point of
 * the grace period: if a node leaving the top-K set the instant it
 * misses a heartbeat, a ten-second blip promotes a new holder for a
 * fraction of the keyspace and the repair sweep copies it there - the
 * classic rebuild storm, paid for a node that is about to come back.
 *
 * So RANKING uses this window and SENDING still uses clpeers_live():
 * the set does not move while a peer is merely quiet, and nothing is
 * transmitted to a peer that is not answering.  During the grace the
 * record therefore has K-1 reachable copies rather than K, which is the
 * trade - a brief dip in redundancy against re-replicating on every
 * blip.  Shard makes the same trade through pc_shard_grace().
 */
#define PEER_SLOT_GRACE_MS  30000

static inline int clpeers_holds_slots(const struct peer *p, long long now)
{
	if (__atomic_load_n(&p->node, __ATOMIC_ACQUIRE))
		return now - p->last_seen_ms < PEER_UP_MS + PEER_SLOT_GRACE_MS;
	/* purged, but still inside the grace: its slots are its own until
	 * the window closes, and only then does the set change */
	return p->gone_ms && now - p->gone_ms < PEER_SLOT_GRACE_MS;
}

/* the live peer holding this node id, or NULL.  Any thread. */
struct peer *clpeers_by_id_live(struct clpeers *pt, int node, long long now);

/* the peer holding node id @node at @from's address, live or not, or
 * NULL - and always NULL for node 0 (see clpeers.c).  Asked of the
 * frames that name their sender: pub/sub interest (PS12), unknown-command
 * tables (S165).  Peer thread. */
struct peer *clpeers_by_id_from(struct clpeers *pt, unsigned node,
		const struct sockaddr_in *from);

/* the slot at this address, live or not, or NULL.  Peer thread. */
struct peer *clpeers_by_addr(struct clpeers *pt, const struct sockaddr_in *a);

/* how many peers are live at `now`.  Any thread. */
int clpeers_count_live(struct clpeers *pt, long long now);

/* find or create the peer slot for @addr; NULL when the table is full.
 * Stale slots (purged: node 0) are recycled in place - entries never
 * move, so worker-side iteration stays safe.
 *
 * *created is set only when a slot was APPENDED; that is the case the
 * caller follows with shard_note_change(), which is why this reports the
 * fact instead of calling it.  Peer thread. */
struct peer *clpeers_upsert(struct clpeers *pt, const struct sockaddr_in *a,
		int *created);

/* Duplicate-id arbitration.  A joiner whose id collides is tolerated for
 * DUP_GRACE_MS: within it the collision may be this node's own stale
 * view, past it the joiner is persistent and must be told.  Peer thread. */
int clpeers_dup_persisted(struct clpeers *pt, const struct sockaddr_in *from,
		long long now);
void clpeers_dup_forget(struct clpeers *pt, const struct sockaddr_in *from);
int clpeers_dup_seen_recent(struct clpeers *pt, const struct in_addr *addr,
		long long now);

#endif /* PC_CLPEERS_H */
