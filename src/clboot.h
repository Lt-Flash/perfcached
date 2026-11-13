/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clboot.h - the bootstrap decision: a node joining with nothing pulls a full walk
 * from a peer before it is ready; which peer, when to re-pick, when to stop waiting. */
#ifndef PC_CLBOOT_H
#define PC_CLBOOT_H

#include <netinet/in.h>
#include <pthread.h>
#include "pcaddr.h"

#define BOOT_MAX_CAND   8
#define BOOT_WAIT_MS    3000   /* how long to wait for a ready peer */
#define BOOT_PICK_MS    1200   /* one beat and a margin after the join */

/* what the tick should do; cluster.c logs and sets state accordingly */
#define CLBOOT_WAIT       0    /* nothing this tick */
#define CLBOOT_PICK       1    /* pick candidates now (caller scans peers) */
#define CLBOOT_GIVE_UP    2    /* three rounds failed - ready, rely on push */
#define CLBOOT_EMPTY      3    /* every ready peer holds nothing - ready */
#define CLBOOT_DEADLINE   4    /* no ready peer in time - ready, rely on push */

/* pending, ncand, cand*, round_failed: under the caller's BORROWED bulk lock;
 * done: atomic; the rest: peer thread only */
struct clboot {
	int pending;                   /* a pull is with the bulk thread */
	int ncand;
	pc_addr cand[BOOT_MAX_CAND];
	int cand_node[BOOT_MAX_CAND];
	long long deadline_ms;
	long long pick_ms;
	int done;                      /* atomic: pulled a full walk */
	int rounds;                    /* candidate lists tried so far */
	int round_failed;              /* bulk thread: re-pick */
};

/* arm the two clocks - a joiner, or a founder that holds nothing.  Peer. */
void clboot_arm(struct clboot *b, long long now);

/* a second bootstrap, a resync long after the first: done, rounds and a stale round
 * failure cleared, the clocks armed.  Cluster thread, with no pull pending. */
void clboot_rearm(struct clboot *b, long long now);

/* `done` is read from several threads.  Any thread. */
int  clboot_done(const struct clboot *b);
void clboot_set_done(struct clboot *b);

/* handoff state, under the BORROWED lock */
int  clboot_pending(struct clboot *b, pthread_mutex_t *mx);
int  clboot_take_round_failed(struct clboot *b, pthread_mutex_t *mx);
void clboot_tick_state(struct clboot *b, pthread_mutex_t *mx,
		int *pending, int *failed_round);
void clboot_handoff(struct clboot *b, pthread_mutex_t *mx,
		const pc_addr *addr, const int *id, int n);
int  clboot_take_cands(struct clboot *b, pthread_mutex_t *mx,
		pc_addr *cand, int *node);
void clboot_pull_ok(struct clboot *b, pthread_mutex_t *mx);
void clboot_pull_failed(struct clboot *b, pthread_mutex_t *mx);

/* The decision, in two halves around the pick the caller makes (it scans the peers);
 * `picked`: 0 nobody qualifies, 1 a pull is away, 2 every ready peer holds nothing. */
int clboot_before_pick(struct clboot *b, long long now, int pending,
		int failed_round);
int clboot_after_pick(struct clboot *b, long long now, int failed_round,
		int picked);

#endif /* PC_CLBOOT_H */
