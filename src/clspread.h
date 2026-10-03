/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clspread.h - spread's decisions (M17, wave 4).
 *
 * Spread (S127) keeps K copies of each key on its placement set.  Most of
 * what went wrong with it went wrong in a DECISION, not a transfer:
 * copies dropped before the real holder had them, a repair armed by a
 * purge inside its own grace, a warning that flapped with a peer.  The
 * decisions were spread across cluster.c's walks and ticks and only a
 * fleet could test them.  Here they are functions of their inputs:
 * cluster.c reads the cluster state and passes it, and keeps the walks,
 * the possession probes, the deletes and the log lines.
 *
 * The rule the whole file exists to pin (29c989e): reclaim acts only on
 * the SHARED map - never on this node's local view of the set.
 */
#ifndef PC_CLSPREAD_H
#define PC_CLSPREAD_H

struct pc_clmap;
struct clpeers;

/* Is collection @c settled enough to drop surplus copies?  1 only with a
 * spread K, outside the shard grace, on a map (@m, NULL = none usable)
 * that covers every slot-holding peer this node sees AND lists
 * @self_id, and with no live peer recovering, re-placing @c, or mid-way
 * through (or dirty after) a repair cycle for @c. */
int clspread_settled(int k, int in_grace, const struct pc_clmap *m,
		int self_id, const struct clpeers *pt, int c, long long now);

/* Settled must HOLD: 1 once @settled has been true this many
 * consecutive ticks.  A false reading zeroes *settled_for and restarts
 * the walk (*cursor = 0). */
#define CLSPREAD_SETTLE_TICKS 10
int clspread_hold(unsigned int *settled_for, unsigned int *cursor,
		int settled);

/* The reclaim walk's verdict on one record this node holds: 1 = ask a
 * holder whether our copy may go, 0 = keep it.  Keys longer than
 * CLSPREAD_RECLAIM_KEYMAX are always kept - the walk's buffer is stack. */
#define CLSPREAD_RECLAIM_KEYMAX 256
int clspread_reclaim_candidate(const char *key, int klen, int k);

/* who is in the set: a fingerprint of the slot-holding peers' placement
 * identity (address and cluster port, never the node id) */
unsigned long long clspread_set_fp(const struct clpeers *pt, long long now);

/* S157: K against the live members, believed after it holds */
#define CLSPREAD_SHORT_HOLD 3
struct clspread_short {
	int last;                      /* the reading, -1 = none yet */
	int held;                      /* ticks it has held */
	int now;                       /* the figure reported (atomic) */
};
#define CLSPREAD_SHORT_INIT { -1, 0, 0 }
#define CLSPREAD_QUIET 0
#define CLSPREAD_SHORT 1               /* the fleet fell short of K: warn */
#define CLSPREAD_MET   2               /* K is met again: say so */
/* one 1 Hz reading: @ready = this node is READY, @live = self + live
 * peers.  Returns what to say, if anything. */
int clspread_short_step(struct clspread_short *s, int k, int ready,
		int in_grace, int live);
int clspread_short_now(const struct clspread_short *s);

#endif /* PC_CLSPREAD_H */
