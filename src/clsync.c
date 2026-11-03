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
#include "clcodec.h"

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

/* ---- the wire envelope.  See clsync.h. ---------------------------- */

size_t clsync_env_hist_at(uint32_t maplen)
{
	return (size_t)CLSYNC_ENV_MAP_AT + maplen + 4;
}

size_t clsync_env_finish(unsigned char *buf, size_t cap, unsigned char type,
		uint32_t maplen, uint32_t histlen)
{
	size_t hat = clsync_env_hist_at(maplen);
	size_t tot = hat + histlen;
	struct pc_wcur c;

	if (tot > cap)
		return 0;
	pc_wcur_init(&c, buf, CLSYNC_ENV_HDR);
	pc_w8(&c, type);
	pc_w32(&c, maplen);
	if (!c.ok)
		return 0;
	/* the map payload already sits between them */
	pc_p32(buf + hat - 4, histlen);
	return tot;
}

int clsync_env_parse(const unsigned char *p, size_t n,
		const unsigned char **map, uint32_t *maplen,
		const unsigned char **hist, uint32_t *histlen)
{
	struct pc_rcur c;
	uint32_t ml, hl;
	size_t hat;

	pc_rcur_init(&c, p, n);
	if (!pc_rskip(&c, 1) || !pc_rfits(&c, 4))
		return 0;
	ml = pc_ru32(&c);
	/* the map must be wholly present AND leave room for the history's
	 * own length prefix */
	if ((size_t)CLSYNC_ENV_MAP_AT + ml + 4 > n)
		return 0;
	hat = clsync_env_hist_at(ml);
	hl = pc_g32(p + hat - 4);
	/* EXACTLY the end: a trailing byte means the frame is not what it
	 * says it is */
	if (hat + hl != n)
		return 0;
	*map = p + CLSYNC_ENV_MAP_AT;
	*maplen = ml;
	*hist = p + hat;
	*histlen = hl;
	return 1;
}

size_t clsync_ack_write(unsigned char *buf, size_t cap, unsigned char type,
		uint32_t term, uint32_t seq)
{
	struct pc_wcur c;

	pc_wcur_init(&c, buf, cap);
	pc_w8(&c, type);
	pc_w32(&c, term);
	pc_w32(&c, seq);
	return c.ok ? pc_wcur_len(&c, buf) : 0;
}

int clsync_ack_parse(const unsigned char *p, size_t n,
		uint32_t *term, uint32_t *seq)
{
	struct pc_rcur c;

	pc_rcur_init(&c, p, n);
	if (!pc_rskip(&c, 1) || !pc_rfits(&c, 4 + 4))
		return 0;
	*term = pc_ru32(&c);
	*seq = pc_ru32(&c);
	return 1;
}
