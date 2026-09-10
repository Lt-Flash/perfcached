/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clsync.c — the staging state machine.
 *
 * Small on purpose: this is where "the backup is never behind" is
 * either true or not, so it is kept exercisable without two daemons.
 */
#include <string.h>

#include "clsync.h"

void pc_clsync_init(struct pc_clsync *s, uint32_t term, uint32_t published_seq)
{
	if (!s)
		return;
	memset(s, 0, sizeof *s);
	s->state = PC_CLSYNC_IDLE;
	s->term = term;
	s->published_seq = published_seq;
	s->next_seq = published_seq + 1;
}

void pc_clsync_set_backup(struct pc_clsync *s, int have_backup)
{
	if (!s)
		return;
	/* Deliberately does NOT touch a change in flight.  A backup that
	 * goes quiet may already have acked, with the ack still on the
	 * wire; abandoning the change here would burn its sequence and
	 * risk publishing different content at an epoch the backup has
	 * already committed to. */
	s->have_backup = have_backup ? 1 : 0;
}

int pc_clsync_stage(struct pc_clsync *s, uint32_t *out_seq)
{
	if (!s || s->state != PC_CLSYNC_IDLE)
		return -1;                 /* one in flight: an ack must never
		                            * be ambiguous about its subject */
	s->staged_seq = s->next_seq++;
	s->staged_n++;
	if (s->have_backup) {
		s->state = PC_CLSYNC_STAGED;
		s->staged_unsynced = 0;
	} else {
		/* nobody to ack: publish rather than block.  A cluster short a
		 * controller must still be able to admit a node. */
		s->state = PC_CLSYNC_ACKED;
		s->staged_unsynced = 1;
		s->unsynced_n++;
	}
	if (out_seq)
		*out_seq = s->staged_seq;
	return 0;
}

int pc_clsync_ack(struct pc_clsync *s, uint32_t term, uint32_t seq)
{
	if (!s || s->state != PC_CLSYNC_STAGED)
		return -1;
	if (term != s->term || seq != s->staged_seq)
		return -1;                 /* a late ack for an abandoned change
		                            * must not commit its replacement */
	s->state = PC_CLSYNC_ACKED;
	s->acked_n++;
	return 0;
}

int pc_clsync_publishable(const struct pc_clsync *s)
{
	return s && s->state == PC_CLSYNC_ACKED;
}

int pc_clsync_published(struct pc_clsync *s)
{
	if (!pc_clsync_publishable(s))
		return -1;
	s->published_seq = s->staged_seq;
	s->state = PC_CLSYNC_IDLE;
	s->staged_unsynced = 0;
	s->published_n++;
	return 0;
}

int pc_clsync_abort(struct pc_clsync *s)
{
	if (!s || s->state == PC_CLSYNC_IDLE)
		return -1;
	/* next_seq is NOT rewound - see the header.  A reused sequence can
	 * put two different maps at one epoch. */
	s->state = PC_CLSYNC_IDLE;
	s->staged_unsynced = 0;
	s->aborted_n++;
	return 0;
}

/* ---- backup side --------------------------------------------------- */

void pc_clsync_hold(struct pc_clsync *s, uint32_t term, uint32_t seq)
{
	if (!s)
		return;
	s->hold_pending = 1;
	s->hold_term = term;
	s->hold_seq = seq;
}

void pc_clsync_saw(struct pc_clsync *s, uint32_t term, uint32_t seq)
{
	if (!s || !s->hold_pending)
		return;
	/* the fleet has reached it (or past it) - nothing owed.  Compared
	 * as an epoch, term first: a LATER term supersedes whatever we were
	 * holding for a master that no longer exists. */
	if (term > s->hold_term ||
	        (term == s->hold_term && seq >= s->hold_seq))
		s->hold_pending = 0;
}

int pc_clsync_owed(const struct pc_clsync *s, uint32_t *term, uint32_t *seq)
{
	if (!s || !s->hold_pending)
		return 0;
	if (term)
		*term = s->hold_term;
	if (seq)
		*seq = s->hold_seq;
	return 1;
}
