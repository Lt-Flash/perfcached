/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clboot.h - the S83 bootstrap's DECISION half (M4).
 *
 * A node that joins holding nothing pulls a full walk from a peer before
 * it reports ready.  Which peer, when to re-pick, and when to stop
 * waiting is arithmetic over a clock; the pull itself is transport and
 * stays in the bulk plane (M7).  This module is the arithmetic.
 *
 * TWO CONCURRENCY REGIMES, preserved exactly as the tree has them:
 *   - deadline_ms, pick_ms and rounds are peer-thread only, unguarded
 *   - done is atomic: other threads read it
 *   - pending, ncand, cand[], cand_node[] and round_failed are the
 *     HANDOFF to the bulk thread and are guarded by a mutex
 *
 * That mutex is BORROWED, not owned: in cluster.c it is C.bmx, which
 * also guards the bulk transport and migration, so it belongs to M7.
 * Every handoff entry point therefore takes a pthread_mutex_t *.  When
 * M7 lands and owns the bulk plane, this becomes its lock to hand in.
 *
 * The module never logs and never changes node state: the decision
 * returns an ACTION and cluster.c does both, because the three messages
 * this replaces are what an operator reads when a bootstrap gives up.
 */
#ifndef PC_CLBOOT_H
#define PC_CLBOOT_H

#include <netinet/in.h>
#include <pthread.h>

#define BOOT_MAX_CAND   8
#define BOOT_WAIT_MS    3000   /* how long to wait for a ready peer */
#define BOOT_PICK_MS    1200   /* one beat and a margin after the join */

/* what the tick should do; cluster.c logs and sets state accordingly */
#define CLBOOT_WAIT       0    /* nothing this tick */
#define CLBOOT_PICK       1    /* pick candidates now (caller scans peers) */
#define CLBOOT_GIVE_UP    2    /* three rounds failed - ready, rely on push */
#define CLBOOT_EMPTY      3    /* every ready peer holds nothing - ready */
#define CLBOOT_DEADLINE   4    /* no ready peer in time - ready, rely on push */

struct clboot {
	int pending;                   /* a pull is with the bulk thread */
	int ncand;
	struct sockaddr_in cand[BOOT_MAX_CAND];
	int cand_node[BOOT_MAX_CAND];
	long long deadline_ms;
	long long pick_ms;
	int done;                      /* atomic: pulled a full walk */
	int rounds;                    /* candidate lists tried so far */
	int round_failed;              /* bulk thread: re-pick */
};

/* arm the two clocks - a joiner, or a founder that holds nothing.  Peer. */
void clboot_arm(struct clboot *b, long long now);

/* `done` is read from several threads.  Any thread. */
int  clboot_done(const struct clboot *b);
void clboot_set_done(struct clboot *b);

/* handoff state, under the BORROWED lock */
int  clboot_pending(struct clboot *b, pthread_mutex_t *mx);
int  clboot_take_round_failed(struct clboot *b, pthread_mutex_t *mx);
void clboot_tick_state(struct clboot *b, pthread_mutex_t *mx,
		int *pending, int *failed_round);
void clboot_handoff(struct clboot *b, pthread_mutex_t *mx,
		const struct sockaddr_in *addr, const int *id, int n);
int  clboot_take_cands(struct clboot *b, pthread_mutex_t *mx,
		struct sockaddr_in *cand, int *node);
void clboot_pull_ok(struct clboot *b, pthread_mutex_t *mx);
void clboot_pull_failed(struct clboot *b, pthread_mutex_t *mx);

/* The decision, in the two halves the tick really has: before the pick
 * (which the caller performs, because it scans the peer table) and after
 * it.  `picked` is the caller's result: 0 nobody qualifies, 1 a pull is
 * away, 2 every ready peer holds nothing.  Peer thread. */
int clboot_before_pick(struct clboot *b, long long now, int pending,
		int failed_round);
int clboot_after_pick(struct clboot *b, long long now, int failed_round,
		int picked);

#endif /* PC_CLBOOT_H */
