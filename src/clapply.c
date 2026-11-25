/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clapply.c - replica apply sharded by key hash: one byte ring per apply thread, the receiver
 * its only producer; an item never wraps, a header with len 0 at the end says "start over". */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "clapply.h"
#include "fnv1a.h"

#if defined(__i386__) || defined(__x86_64__)
#define clapply_pause() __builtin_ia32_pause()
#else
#define clapply_pause() do {} while (0)
#endif

/* a consumer spins a few tens of us before it sleeps - the gap between records on one ring -
 * so `sleeping` stays clear and the producer, which also drains the socket, skips the wake */
#define CLAPPLY_SPIN 4096

struct hdr {
	unsigned int len;              /* whole item incl. this header; 0 = wrap */
	unsigned int collen, klen, vlen, ttl_left;
	unsigned long long ver;
	struct clapply_grp *grp;
	int passive;
	unsigned char flags;
};

#define HDR ((unsigned int)sizeof(struct hdr))

int clapply_init(struct clapply *a, int n,
		int (*apply)(const struct clapply_rec *, void *),
		void (*ack)(const struct clapply_grp *, void *), void *ctx)
{
	int i;

	memset(a, 0, sizeof *a);
	if (n < 2)
		return 0;
	if (n > CLAPPLY_MAX)
		n = CLAPPLY_MAX;
	for (i = 0; i < n; i++) {
		struct clapply_ring *r = &a->ring[i];

		r->buf = malloc(CLAPPLY_RING_SZ);
		if (!r->buf) {
			clapply_free(a);
			return -1;
		}
		r->cap = CLAPPLY_RING_SZ;
		pthread_mutex_init(&r->mx, NULL);
		pthread_cond_init(&r->cv, NULL);
	}
	a->n = n;
	a->apply = apply;
	a->ack = ack;
	a->ctx = ctx;
	return n;
}

void clapply_free(struct clapply *a)
{
	int i;

	for (i = 0; i < CLAPPLY_MAX; i++)
		free(a->ring[i].buf);
	memset(a, 0, sizeof *a);
}

int clapply_shard(const struct clapply *a, const unsigned char *col,
		unsigned int collen, const unsigned char *key, unsigned int klen)
{
	uint64_t h = fnv1a64_more(FNV1A64_BASIS, col, collen);

	h = fnv1a64_byte(h, 0);
	h = fnv1a64_more(h, key, klen);
	return a->n > 0 ? (int)(h % (uint64_t)a->n) : 0;
}

struct clapply_grp *clapply_open(struct clapply *a,
		const pc_addr *from, uint32_t req)
{
	struct clapply_grp *g = calloc(1, sizeof *g);

	(void)a;
	if (!g)
		return NULL;
	g->from = *from;
	g->req = req;
	g->remaining = 1;              /* the receiver's own hold */
	return g;
}

/* the last one out sends the ack */
static void grp_release(struct clapply *a, struct clapply_grp *g)
{
	if (!g)
		return;
	if (__atomic_sub_fetch(&g->remaining, 1, __ATOMIC_ACQ_REL) == 0) {
		if (a->ack)
			a->ack(g, a->ctx);
		__atomic_fetch_add(&a->grp_acked, 1, __ATOMIC_RELAXED);
		free(g);
	}
}

void clapply_close(struct clapply *a, struct clapply_grp *g)
{
	grp_release(a, g);
}

static void nap(void)
{
	struct timespec ts = { 0, 200000L };

	nanosleep(&ts, NULL);
}

int clapply_push(struct clapply *a, const struct clapply_rec *rec,
		struct clapply_grp *g)
{
	int idx = clapply_shard(a, rec->col, rec->collen, rec->key, rec->klen);
	struct clapply_ring *r = &a->ring[idx];
	unsigned int need = HDR + rec->collen + rec->klen + rec->vlen;
	unsigned int tail = r->tail;
	struct hdr h;
	unsigned char *p;

	if (need + HDR > r->cap)
		return -1;                 /* cannot ever fit: the caller applies it */
	if (g)
		__atomic_fetch_add(&g->remaining, 1, __ATOMIC_ACQ_REL);
	for (;;) {
		unsigned int head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
		unsigned int room = (head + r->cap - tail - 1) % r->cap;

		/* strictly below cap, so tail never sits at the end and full and
		 * empty stay distinct (one byte is kept back) */
		if (tail + need < r->cap) {
			if (room >= need)
				break;
		} else if (head <= tail && head > 0) {
			/* wrap only with the unread data all behind tail and head off 0:
			 * with head at 0 the wrapped tail would meet it and read as empty */
			memset(&h, 0, sizeof h);
			if (r->cap - tail >= HDR)
				memcpy(r->buf + tail, &h, HDR);
			tail = 0;
			__atomic_store_n(&r->tail, 0, __ATOMIC_SEQ_CST);
			continue;
		}
		volatile int *st = __atomic_load_n(&a->stop, __ATOMIC_ACQUIRE);

		if (st && *st) {
			if (g)
				__atomic_fetch_sub(&g->remaining, 1, __ATOMIC_ACQ_REL);
			return -1;             /* shutting down: the caller applies it */
		}
		r->blocks++;
		nap();                     /* backpressure, never a drop */
	}
	h.len = need;
	h.collen = rec->collen;
	h.klen = rec->klen;
	h.vlen = rec->vlen;
	h.ttl_left = rec->ttl_left;
	h.ver = rec->ver;
	h.grp = g;
	h.passive = rec->passive;
	h.flags = rec->flags;
	p = r->buf + tail;
	memcpy(p, &h, HDR);
	memcpy(p + HDR, rec->col, rec->collen);
	memcpy(p + HDR + rec->collen, rec->key, rec->klen);
	if (rec->vlen)
		memcpy(p + HDR + rec->collen + rec->klen, rec->val, rec->vlen);
	/* SEQ_CST here and in clapply_take(): with release/acquire alone each side
	 * could miss the other's store and the consumer sleep through a push */
	__atomic_store_n(&r->tail, tail + need, __ATOMIC_SEQ_CST);
	a->dispatched++;
	if (__atomic_load_n(&r->sleeping, __ATOMIC_SEQ_CST)) {
		pthread_mutex_lock(&r->mx);
		pthread_cond_signal(&r->cv);
		pthread_mutex_unlock(&r->mx);
	}
	return idx;
}

int clapply_take(struct clapply *a, int idx, volatile int *stop,
		struct clapply_item *out, long long now_ms)
{
	struct clapply_ring *r = &a->ring[idx];
	struct hdr h;
	unsigned int head;

	if (!__atomic_load_n(&a->stop, __ATOMIC_ACQUIRE))   /* the push's escape at shutdown */
		__atomic_store_n(&a->stop, stop, __ATOMIC_RELEASE);
	for (;;) {
		unsigned int tail;

		head = r->head;
		tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
		if (head != tail) {
			/* a wrap marker, or the end of the buffer with no
			 * room for a header at all */
			if (r->cap - head < HDR) {
				head = 0;
				__atomic_store_n(&r->head, 0, __ATOMIC_RELEASE);
				continue;
			}
			memcpy(&h, r->buf + head, HDR);
			if (h.len == 0) {
				head = 0;
				__atomic_store_n(&r->head, 0, __ATOMIC_RELEASE);
				continue;
			}
			break;
		}
		if (*stop)
			return 0;
		/* look before sleeping - see CLAPPLY_SPIN */
		{
			unsigned int sp;

			for (sp = 0; sp < CLAPPLY_SPIN; sp++) {
				if (__atomic_load_n(&r->tail, __ATOMIC_ACQUIRE) != head)
					break;
				clapply_pause();
			}
			if (__atomic_load_n(&r->tail, __ATOMIC_ACQUIRE) != head)
				continue;      /* a record arrived: take it */
		}
		pthread_mutex_lock(&r->mx);
		__atomic_store_n(&r->sleeping, 1, __ATOMIC_SEQ_CST);
		/* re-check under the flag: a push between the load above
		 * and the flag would otherwise be slept through */
		if (__atomic_load_n(&r->tail, __ATOMIC_SEQ_CST) == head && !*stop) {
			struct timespec ts;

			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_nsec += 50L * 1000000;
			if (ts.tv_nsec >= 1000000000L) {
				ts.tv_sec++;
				ts.tv_nsec -= 1000000000L;
			}
			pthread_cond_timedwait(&r->cv, &r->mx, &ts);
		}
		__atomic_store_n(&r->sleeping, 0, __ATOMIC_RELEASE);
		pthread_mutex_unlock(&r->mx);
		if (a->idle)
			a->idle();                 /* nothing pending: the owner's idle work */
	}
	out->r.col = r->buf + head + HDR;
	out->r.key = out->r.col + h.collen;
	out->r.val = out->r.key + h.klen;
	out->r.collen = h.collen;
	out->r.klen = h.klen;
	out->r.vlen = h.vlen;
	out->r.ttl_left = h.ttl_left;
	out->r.ver = h.ver;
	out->r.passive = h.passive;
	out->r.flags = h.flags;
	out->grp = h.grp;
	/* the item's clock is NOW, not the caller's pre-wait now_ms */
	__atomic_store_n(&r->busy_since_ms, a->clock ? a->clock() : now_ms,
		__ATOMIC_RELAXED);
	__atomic_store_n(&r->wait_since_ms, 0, __ATOMIC_RELAXED);
	return 1;
}

void clapply_done(struct clapply *a, int idx, struct clapply_item *it)
{
	struct clapply_ring *r = &a->ring[idx];
	unsigned int len = HDR + it->r.collen + it->r.klen + it->r.vlen;

	__atomic_store_n(&r->head, r->head + len, __ATOMIC_RELEASE);
	r->items++;
	__atomic_store_n(&r->busy_since_ms, 0, __ATOMIC_RELAXED);
	grp_release(a, it->grp);
}

unsigned int clapply_backlog(const struct clapply *a, int idx)
{
	const struct clapply_ring *r = &a->ring[idx];
	unsigned int head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
	unsigned int tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);

	return (tail + r->cap - head) % r->cap;
}
