/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clpush.c - the per-thread write-path push groups (M5).  See clpush.h.
 *
 * The bodies came out of pc_repl_push(), wgroup_send() and
 * repl_flush_mine() unchanged.  What differs: the groups are reached
 * through a pointer, the clock is passed in, the wire geometry arrives
 * at init, and the send is a callback so no datagram is sealed here.
 */
#include <stdlib.h>
#include <string.h>

#include "clpush.h"

void clpush_init(struct clpush *p, int npeers, size_t hdr, size_t cap,
		size_t flush_bytes, int flush_ms)
{
	memset(p, 0, sizeof *p);
	p->npeers = npeers;
	p->hdr = hdr;
	p->cap = cap;
	p->flush_bytes = flush_bytes;
	p->flush_ms = flush_ms;
}

void clpush_fini(struct clpush *p)
{
	int t, i;

	for (t = 0; t < PC_TIDX_MAX; t++) {
		if (!p->g[t])
			continue;
		for (i = 0; i < p->npeers; i++)
			free(p->g[t][i].q);
		free(p->g[t]);
		p->g[t] = NULL;
	}
	p->open = 0;
}

int clpush_open(const struct clpush *p)
{
	return __atomic_load_n(&p->open, __ATOMIC_RELAXED);
}

void clpush_sent(struct clpush *p, struct wgroup *g)
{
	__atomic_fetch_sub(&p->open, 1, __ATOMIC_RELAXED);
	g->qlen = p->hdr;
	g->qcount = 0;
	g->qopen_ms = 0;
}

static int addr_same(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
	return a->sin_addr.s_addr == b->sin_addr.s_addr &&
		a->sin_port == b->sin_port;
}

void clpush_append(struct clpush *p, int tidx, int i,
		const struct sockaddr_in *to, const unsigned char *rec,
		size_t n, long long now, clpush_send_fn send, void *ctx)
{
	struct wgroup *gs, *g;

	if (tidx < 0 || tidx >= PC_TIDX_MAX || i < 0 || i >= p->npeers)
		return;                    /* not a slot thread: never pushes */
	if (!(gs = p->g[tidx])) {
		gs = calloc((size_t)p->npeers, sizeof *gs);
		if (!gs)
			return;
		p->g[tidx] = gs;           /* this thread's alone, forever */
	}
	g = &gs[i];
	if (!g->q) {
		/* hdr + flush_bytes + this record.  The old comment here
		 * claimed nothing larger was reachable because "the
		 * pre-append guard keeps qlen + n inside cap" - but cap is
		 * the WIRE's gather ceiling, not what was allocated, and the
		 * guard does not run at all when the group is empty.  A small
		 * first record followed by a larger one overran this block.
		 * The size is recorded now and grown below when it has to be. */
		g->qcap = p->hdr + p->flush_bytes + n;
		g->q = malloc(g->qcap);
		if (!g->q) {
			g->qcap = 0;
			return;
		}
		g->qlen = p->hdr;
		g->qcount = 0;
	}
	if (g->qcount && !addr_same(&g->to, to)) {
		/* the slot changed hands: what was gathered for the old peer
		 * is dropped - it is gone, and the sweep owes the newcomer
		 * everything anyway */
		g->qlen = p->hdr;
		g->qcount = 0;
		__atomic_fetch_sub(&p->open, 1, __ATOMIC_RELAXED);
	}
	if (g->qcount && (g->qlen + n > p->cap ||
	        now - g->qopen_ms >= p->flush_ms))
		send(g, ctx);
	if (!g->qcount) {
		g->qopen_ms = now;
		g->to = *to;
		__atomic_fetch_add(&p->open, 1, __ATOMIC_RELAXED);
	}
	/* grow rather than trust the geometry: whatever the guards above
	 * decided, this write must fit what was actually allocated */
	if (g->qlen + n > g->qcap) {
		size_t want = g->qlen + n + p->flush_bytes;
		unsigned char *bigger = realloc(g->q, want);

		if (!bigger)
			return;            /* the record is dropped, not written */
		g->q = bigger;
		g->qcap = want;
	}
	memcpy(g->q + g->qlen, rec, n);
	g->qlen += n;
	g->qcount++;
	if (g->qlen >= p->flush_bytes || n > p->cap)
		send(g, ctx);
}

void clpush_flush_mine(struct clpush *p, int tidx, long long now,
		clpush_send_fn send, void *ctx)
{
	struct wgroup *gs;
	int i;

	if (tidx < 0 || tidx >= PC_TIDX_MAX || !(gs = p->g[tidx]))
		return;
	for (i = 0; i < p->npeers; i++)
		if (gs[i].qcount && now - gs[i].qopen_ms >= p->flush_ms)
			send(&gs[i], ctx);
}
