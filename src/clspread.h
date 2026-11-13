/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* Spread's decisions as functions of their inputs: cluster.c passes the cluster state and
 * keeps the walks, possession probes, deletes and log lines. */
#ifndef PC_CLSPREAD_H
#define PC_CLSPREAD_H

struct pc_clmap;
struct clpeers;

/* 1 = @c may drop surplus copies: spread K, outside grace, map @m (NULL = none)
 * covers every slot holder and lists @self_id, no live peer recovering or mid-repair. */
int clspread_settled(int k, int in_grace, const struct pc_clmap *m,
		int self_id, const struct clpeers *pt, int c, long long now);

/* 1 once @settled has been true this many consecutive ticks (beats: 30 s, past a map reaching every
 * node - two views of the holders must not cross); a false reading zeroes *settled_for, *cursor */
#define CLSPREAD_SETTLE_TICKS 30
int clspread_hold(unsigned int *settled_for, unsigned int *cursor,
		int settled);

/* 1 = ask a holder whether our copy may go, 0 = keep it.  Keys longer than
 * CLSPREAD_RECLAIM_KEYMAX are always kept: the walk's buffer is on the stack. */
#define CLSPREAD_RECLAIM_KEYMAX 256
int clspread_reclaim_candidate(const char *key, int klen, int k);

/* who is in the set: a fingerprint of the slot-holding peers' placement
 * identity (address and cluster port, never the node id) */
unsigned long long clspread_set_fp(const struct clpeers *pt, long long now);

/* K against the live members, believed after it holds */
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
