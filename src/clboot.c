/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clboot.c - the bootstrap's decision half (M4).  See clboot.h.
 *
 * Moved out of boot_tick() and boot_request() unchanged; what differs is
 * that the state is reached through a pointer, the clock is passed in,
 * the handoff lock is passed in, and the three outcomes that used to log
 * and set node state are returned instead.
 */
#include <string.h>

#include "clboot.h"

void clboot_arm(struct clboot *b, long long now)
{
	b->deadline_ms = now + BOOT_WAIT_MS;
	b->pick_ms = now + BOOT_PICK_MS;
}

int clboot_done(const struct clboot *b)
{
	return __atomic_load_n(&b->done, __ATOMIC_RELAXED);
}

void clboot_set_done(struct clboot *b)
{
	__atomic_store_n(&b->done, 1, __ATOMIC_RELAXED);
}

int clboot_pending(struct clboot *b, pthread_mutex_t *mx)
{
	int pending;

	pthread_mutex_lock(mx);
	pending = b->pending;
	pthread_mutex_unlock(mx);
	return pending;
}

/* Both halves of the tick's view, under ONE acquisition.  boot_tick took
 * the lock twice in a row for these - with a declaration stranded between
 * the two critical sections - so pending and round_failed could come from
 * different instants. */
void clboot_tick_state(struct clboot *b, pthread_mutex_t *mx, int *pending,
		int *failed_round)
{
	pthread_mutex_lock(mx);
	*pending = b->pending;
	*failed_round = b->round_failed;
	b->round_failed = 0;
	pthread_mutex_unlock(mx);
}

int clboot_take_round_failed(struct clboot *b, pthread_mutex_t *mx)
{
	int failed;

	pthread_mutex_lock(mx);
	failed = b->round_failed;
	b->round_failed = 0;
	pthread_mutex_unlock(mx);
	return failed;
}

void clboot_handoff(struct clboot *b, pthread_mutex_t *mx,
		const struct sockaddr_in *addr, const int *id, int n)
{
	int i;

	pthread_mutex_lock(mx);
	for (i = 0; i < n && i < BOOT_MAX_CAND; i++) {
		b->cand[i] = addr[i];
		b->cand_node[i] = id[i];
	}
	b->ncand = n;
	b->pending = 1;
	pthread_mutex_unlock(mx);
}

int clboot_take_cands(struct clboot *b, pthread_mutex_t *mx,
		struct sockaddr_in *cand, int *node)
{
	int n;

	pthread_mutex_lock(mx);
	n = b->ncand;
	memcpy(cand, b->cand, sizeof b->cand);
	memcpy(node, b->cand_node, sizeof b->cand_node);
	pthread_mutex_unlock(mx);
	return n;
}

void clboot_pull_ok(struct clboot *b, pthread_mutex_t *mx)
{
	clboot_set_done(b);
	pthread_mutex_lock(mx);
	b->pending = 0;
	pthread_mutex_unlock(mx);
}

/* every candidate of this round failed: hand the decision back to the
 * tick, which re-picks with the peers known by now, or gives up after
 * three rounds */
void clboot_pull_failed(struct clboot *b, pthread_mutex_t *mx)
{
	pthread_mutex_lock(mx);
	b->pending = 0;
	b->round_failed = 1;
	pthread_mutex_unlock(mx);
}

int clboot_before_pick(struct clboot *b, long long now, int pending,
		int failed_round)
{
	if (failed_round && ++b->rounds >= 3) {
		b->deadline_ms = 0;
		return CLBOOT_GIVE_UP;
	}
	if (!pending && now >= b->pick_ms)
		return CLBOOT_PICK;
	return CLBOOT_WAIT;
}

int clboot_after_pick(struct clboot *b, long long now, int failed_round,
		int picked)
{
	if (failed_round && !picked) {
		/* Nobody to re-pick yet: wait a beat more.  In boot_tick this
		 * extension was a DEAD STORE - the branch set br = 1 and the
		 * `else if (br)` two lines down zeroed the deadline again, so
		 * a re-pick that found nobody reported READY on the very next
		 * tick instead of waiting.  The comment says wait; now it
		 * does. */
		b->deadline_ms = now + BOOT_WAIT_MS;
		return CLBOOT_WAIT;
	}
	if (picked == 2) {
		/* a fleet that is itself empty - its birth, or a keyspace
		 * that expired - has nothing to hand over; the few records a
		 * beat may hide arrive by the push.  Waiting would only hold
		 * writers off an empty node. */
		b->deadline_ms = 0;
		clboot_set_done(b);
		return CLBOOT_EMPTY;
	}
	if (picked) {
		b->deadline_ms = 0;
		return CLBOOT_WAIT;
	}
	if (now >= b->deadline_ms) {
		b->deadline_ms = 0;
		return CLBOOT_DEADLINE;
	}
	return CLBOOT_WAIT;
}
