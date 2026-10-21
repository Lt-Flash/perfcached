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
		for (i = 0; i < p->npeers; i++) {
			free(p->g[t][i].q);
			free(p->g[t][i].nh);
		}
		free(p->g[t]);
		p->g[t] = NULL;
	}
	p->open = 0;
	if (p->acks) {
		for (i = 0; i < CLPUSH_ACKS; i++)
			free(p->acks[i].nh);
		free(p->acks);
		p->acks = NULL;
	}
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
	g->nhlen = 0;              /* parked, or nothing to confirm */
	g->nhcount = 0;
}

static int addr_same(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
	return a->sin_addr.s_addr == b->sin_addr.s_addr &&
		a->sin_port == b->sin_port;
}

/* S253: note a non-holder key in the group it is travelling in.  A
 * failed grow leaves it un-noted, which keeps the copy. */
static void nh_note(struct wgroup *g, const char *col, size_t collen,
		const char *key, size_t klen, unsigned long long ver)
{
	size_t need = 11 + collen + klen;
	unsigned char *w;
	int b;

	if (g->nhlen + need > g->nhcap) {
		size_t want = (g->nhlen + need) * 2 + 1024;
		unsigned char *bigger = realloc(g->nh, want);

		if (!bigger)
			return;
		g->nh = bigger;
		g->nhcap = want;
	}
	w = g->nh + g->nhlen;
	w[0] = (unsigned char)collen;
	w[1] = (unsigned char)(klen >> 8);
	w[2] = (unsigned char)klen;
	for (b = 0; b < 8; b++)
		w[3 + b] = (unsigned char)(ver >> (56 - 8 * b));
	memcpy(w + 11, col, collen);
	memcpy(w + 11 + collen, key, klen);
	g->nhlen += need;
	g->nhcount++;
}

static void append(struct clpush *p, int tidx, int i,
		const struct sockaddr_in *to, const unsigned char *rec,
		size_t n, long long now, clpush_send_fn send, void *ctx,
		const char *col, size_t collen, const char *key, size_t klen,
		unsigned long long ver)
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
		g->nhlen = 0;          /* never sent: those copies stay */
		g->nhcount = 0;
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
	/* before the send below: the key must travel in the group its
	 * record is in, or the ack would confirm the wrong datagram */
	if (key)
		nh_note(g, col, collen, key, klen, ver);
	if (g->qlen >= p->flush_bytes || n > p->cap)
		send(g, ctx);
}

void clpush_append(struct clpush *p, int tidx, int i,
		const struct sockaddr_in *to, const unsigned char *rec,
		size_t n, long long now, clpush_send_fn send, void *ctx)
{
	append(p, tidx, i, to, rec, n, now, send, ctx, NULL, 0, NULL, 0, 0);
}

void clpush_append_nh(struct clpush *p, int tidx, int i,
		const struct sockaddr_in *to, const unsigned char *rec,
		size_t n, long long now, clpush_send_fn send, void *ctx,
		const char *col, size_t collen, const char *key, size_t klen,
		unsigned long long ver)
{
	if (collen > 255 || klen > 0xFFFF)
		key = NULL;
	append(p, tidx, i, to, rec, n, now, send, ctx, col, collen, key,
		klen, ver);
}

/* ---- S253: the ack table ------------------------------------------------
 * Workers park, the cluster thread takes and expires; a slot's state is
 * the only thing they share, moved by CAS so a worker never waits on the
 * cluster thread and a slot is never read half-filled. */
static struct clpush_ack *acks_of(struct clpush *p)
{
	struct clpush_ack *a = __atomic_load_n(&p->acks, __ATOMIC_ACQUIRE);

	if (!a) {
		struct clpush_ack *fresh = calloc(CLPUSH_ACKS, sizeof *fresh);

		if (!fresh)
			return NULL;
		if (__atomic_compare_exchange_n(&p->acks, &a, fresh, 0,
		        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			a = fresh;
		else
			free(fresh);           /* another thread won; a is its */
	}
	return a;
}

uint32_t clpush_ack_park(struct clpush *p, struct wgroup *g, long long now)
{
	struct clpush_ack *tab, *a;
	uint32_t req;
	int want = 0;

	if (!g->nhcount || !(tab = acks_of(p)))
		return 0;
	req = CLPUSH_ACK_TAG |
		(__atomic_add_fetch(&p->ack_seq, 1, __ATOMIC_RELAXED) &
		 ~CLPUSH_ACK_TAG);
	a = &tab[req & (CLPUSH_ACKS - 1)];
	if (!__atomic_compare_exchange_n(&a->state, &want, 1, 0,
	        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		return 0;                  /* still waiting on an older group */
	a->req = req;
	a->count = g->qcount;
	a->sent_ms = now;
	a->to = g->to;
	a->nh = g->nh;                 /* the slot owns the keys now */
	a->nhlen = g->nhlen;
	a->nhcount = g->nhcount;
	g->nh = NULL;
	g->nhcap = 0;
	__atomic_store_n(&a->state, 2, __ATOMIC_RELEASE);
	return req;
}

struct clpush_ack *clpush_ack_take(struct clpush *p, uint32_t req,
		const struct sockaddr_in *from)
{
	struct clpush_ack *tab = __atomic_load_n(&p->acks, __ATOMIC_ACQUIRE);
	struct clpush_ack *a;
	int want = 2;

	if (!tab || !(req & CLPUSH_ACK_TAG))
		return NULL;
	a = &tab[req & (CLPUSH_ACKS - 1)];
	if (!__atomic_compare_exchange_n(&a->state, &want, 3, 0,
	        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		return NULL;
	if (a->req != req || (from && !addr_same(&a->to, from))) {
		/* a late ack for a slot reused since, or a stranger's */
		__atomic_store_n(&a->state, 2, __ATOMIC_RELEASE);
		return NULL;
	}
	return a;
}

void clpush_ack_done(struct clpush *p, struct clpush_ack *a)
{
	(void)p;
	free(a->nh);
	a->nh = NULL;
	a->nhlen = 0;
	a->nhcount = 0;
	__atomic_store_n(&a->state, 0, __ATOMIC_RELEASE);
}

unsigned int clpush_ack_expire(struct clpush *p, long long now, int ms)
{
	struct clpush_ack *tab = __atomic_load_n(&p->acks, __ATOMIC_ACQUIRE);
	unsigned int lost = 0;
	int i;

	if (!tab)
		return 0;
	for (i = 0; i < CLPUSH_ACKS; i++) {
		struct clpush_ack *a = &tab[i];
		int want = 2;

		if (__atomic_load_n(&a->state, __ATOMIC_ACQUIRE) != 2 ||
		        now - a->sent_ms < ms)
			continue;
		if (!__atomic_compare_exchange_n(&a->state, &want, 3, 0,
		        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
			continue;
		lost += a->nhcount;
		clpush_ack_done(p, a);
	}
	return lost;
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
