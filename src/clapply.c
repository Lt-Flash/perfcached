/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clapply.c - S217: replica apply sharded by key hash.  See clapply.h.
 *
 * The ring is a byte ring, one producer (the receiver) and one consumer
 * (the apply thread): head and tail are single-writer atomics, the same
 * shape as the WAL's per-writer rings.  An item is a fixed header and
 * the record's bytes; an item never wraps - a header with len 0 at the
 * end of the buffer says "start over".
 */
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

/*
 * S220: how long a consumer LOOKS before it sleeps.
 *
 * It used to take the mutex the instant its ring ran dry.  At ~1 us a
 * record, with N rings each seeing 1/N of them, that is a sleep and a
 * wake per record - and the wake is the PRODUCER's to pay, on the one
 * thread that must also drain the socket.  Measured on an idle 16-core
 * receiver under ~1.3M records/s offered, apply_threads = 4:
 *
 *                      applied     drops   us/rec   ctx switches
 *   sleeping at once      55%     27,716     3.16   1 per 13 records
 *   spinning first        99%        748     2.19   1 per 93 records
 *
 * perf put `clapply_push` at 9.5% of the whole daemon on the receiving
 * thread, and compiling the wake-up out entirely cut CPU per record
 * from 3.16 us to 1.43 - over half the receiver's time was the signal
 * path.  Removing it is not the answer though: with nothing to wake
 * them the consumers wait out their 50 ms and the rings back up (45%
 * applied, 34,781 dropped).  Looking first is.
 *
 * 4096 iterations is a few tens of microseconds - sized to cover the
 * gap between records on ONE ring at these rates, so the common case
 * needs no syscall at all and, because `sleeping` stays clear, the
 * producer skips the mutex too.  512 was tried first and was not
 * enough at four rings (75% applied, 16,101 dropped).
 *
 * What it costs when there is nothing to do: each consumer wakes on
 * its 50 ms timer, looks for a few tens of microseconds and sleeps
 * again - measured at 14 ticks per 20 s for four threads, 0.7% of one
 * core, against a daemon with no apply threads at all.  A consumer is
 * on a core of its own, which is the premise of having asked for the
 * thread.
 */
#define CLAPPLY_SPIN 4096

struct hdr {
	unsigned int len;              /* whole item incl. this header; 0 = wrap */
	unsigned int collen, klen, vlen, ttl_left;
	unsigned long long ver;
	struct clapply_grp *grp;
	int passive;
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
		const struct sockaddr_in *from, uint32_t req)
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

		/* An item never wraps.  Strictly below cap, so tail never sits
		 * AT the end: the indices stay in [0, cap) and full and empty
		 * stay distinct (one byte is kept back). */
		if (tail + need < r->cap) {
			if (room >= need)
				break;
		} else if (head <= tail && head > 0) {
			/* It does not fit before the end: a wrap marker goes
			 * there (when a header fits; the consumer treats a tail
			 * end too short for one the same way) and the item
			 * starts at 0.  Only with the unread data all BEHIND
			 * tail - head <= tail - and head off 0: with head at 0
			 * the wrapped tail would meet it and read as empty over
			 * unread data.  Whether the item then fits at 0 is the
			 * ordinary test above, on the next turn; the consumer
			 * moves head to 0 when it reaches the marker, and an
			 * empty ring wraps in one turn. */
			memset(&h, 0, sizeof h);   /* len 0 = wrap */
			if (r->cap - tail >= HDR)
				memcpy(r->buf + tail, &h, HDR);
			tail = 0;
			__atomic_store_n(&r->tail, 0, __ATOMIC_SEQ_CST);
			continue;
		}
		if (a->stop && *a->stop) {
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
	p = r->buf + tail;
	memcpy(p, &h, HDR);
	memcpy(p + HDR, rec->col, rec->collen);
	memcpy(p + HDR + rec->collen, rec->key, rec->klen);
	if (rec->vlen)
		memcpy(p + HDR + rec->collen + rec->klen, rec->val, rec->vlen);
	/* SEQ_CST on the tail store and the sleeping load, and on their
	 * mirror in clapply_take(): with release/acquire alone each side
	 * could miss the other's store and the consumer sleep through a
	 * push (bounded by its 50 ms timed wait, but a bound is not an
	 * answer) */
	__atomic_store_n(&r->tail, tail + need, __ATOMIC_SEQ_CST);
	a->dispatched++;
	/* wake a sleeping consumer */
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
		/* S220: look before sleeping - see CLAPPLY_SPIN */
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
	out->grp = h.grp;
	__atomic_store_n(&r->busy_since_ms, now_ms, __ATOMIC_RELAXED);
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
