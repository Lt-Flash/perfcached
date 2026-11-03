/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clreplay.c - CH-1: a sealed cluster frame is taken once.  See clreplay.h.
 */
#include <stdlib.h>
#include <string.h>

#include "clreplay.h"

struct win {
	int lock;
	uint64_t incarn, hi;
	uint64_t bits[CLREPLAY_WIN / 64];
};

struct sender {
	uint64_t key;                      /* 1 << 48 | ip << 16 | port; 0 = free */
	uint64_t incarn;
	long long seen_ms;
	struct win *w[CLREPLAY_STREAMS];
};

static struct sender tab[CLREPLAY_SENDERS];
static int tab_lock;
static unsigned long long cnt[CLREPLAY_NCOUNT];

static void spin(int *l)
{
	while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE))
		while (__atomic_load_n(l, __ATOMIC_RELAXED))
			;
}

static void unspin(int *l)
{
	__atomic_store_n(l, 0, __ATOMIC_RELEASE);
}

static struct sender *sender_of(uint32_t ip, uint16_t port)
{
	uint64_t key = 1ULL << 48 | (uint64_t)ip << 16 | port;
	unsigned int h = (unsigned int)((key * 0x9E3779B97F4A7C15ULL) >> 40);
	unsigned int i, k;

	for (i = 0; i < CLREPLAY_SENDERS; i++) {
		struct sender *s = &tab[(h + i) % CLREPLAY_SENDERS];
		uint64_t sk = __atomic_load_n(&s->key, __ATOMIC_ACQUIRE);

		if (sk == key)
			return s;
		if (sk)
			continue;
		spin(&tab_lock);               /* a free slot: claim it, or find it taken */
		for (k = i; k < CLREPLAY_SENDERS; k++) {
			s = &tab[(h + k) % CLREPLAY_SENDERS];
			sk = __atomic_load_n(&s->key, __ATOMIC_ACQUIRE);
			if (sk == key || !sk) {
				if (!sk)
					__atomic_store_n(&s->key, key, __ATOMIC_RELEASE);
				unspin(&tab_lock);
				return s;
			}
		}
		unspin(&tab_lock);
		return NULL;
	}
	return NULL;
}

static void bit_clear_range(struct win *w, uint64_t from, uint64_t to)
{
	uint64_t i;

	for (i = from; i < to; i++) {
		uint64_t b = i % CLREPLAY_WIN;

		if (!(b & 63) && to - i >= 64) {
			w->bits[b / 64] = 0;
			i += 63;
		} else
			w->bits[b / 64] &= ~(1ULL << (b & 63));
	}
}

static int win_admit(struct win *w, uint64_t seq)
{
	uint64_t b = seq % CLREPLAY_WIN, m = 1ULL << (b & 63);

	if (seq > w->hi) {
		if (seq - w->hi > CLREPLAY_WIN)
			memset(w->bits, 0, sizeof w->bits);
		else
			bit_clear_range(w, w->hi + 1, seq);
		w->bits[b / 64] |= m;
		w->hi = seq;
		return CLREPLAY_OK;
	}
	if (w->hi - seq >= CLREPLAY_WIN)
		return CLREPLAY_OLD;
	if (w->bits[b / 64] & m)
		return CLREPLAY_DUP;
	w->bits[b / 64] |= m;
	return CLREPLAY_OK;
}

static int counted(int rc)
{
	__atomic_fetch_add(&cnt[rc], 1, __ATOMIC_RELAXED);
	return rc;
}

int clreplay_admit(const struct clwire_id *id, long long now)
{
	struct sender *s = sender_of(id->ip, id->port);
	struct win *w, *fresh;
	uint64_t cur;
	int rc, lower = 0;

	if (!s || id->stream >= CLREPLAY_STREAMS)
		return counted(CLREPLAY_FULL);
	cur = __atomic_load_n(&s->incarn, __ATOMIC_ACQUIRE);
	if (id->incarn < cur) {
		/* the newer one gone quiet: this sender's clock stepped back */
		if (now - __atomic_load_n(&s->seen_ms, __ATOMIC_RELAXED) < CLREPLAY_STALE_MS)
			return counted(CLREPLAY_INCARN);
		lower = 1;
		__atomic_store_n(&s->incarn, id->incarn, __ATOMIC_RELEASE);
	} else
		while (id->incarn > cur && !__atomic_compare_exchange_n(&s->incarn,
		        &cur, id->incarn, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			;
	if (!(w = __atomic_load_n(&s->w[id->stream], __ATOMIC_ACQUIRE))) {
		if (!(fresh = calloc(1, sizeof *fresh)))
			return counted(CLREPLAY_FULL);
		fresh->incarn = id->incarn;
		w = NULL;
		if (__atomic_compare_exchange_n(&s->w[id->stream], &w, fresh, 0,
		        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			w = fresh;
		else
			free(fresh);
	}
	spin(&w->lock);
	if (id->incarn != w->incarn) {
		if (id->incarn < w->incarn && !lower) {
			unspin(&w->lock);
			return counted(CLREPLAY_INCARN);
		}
		w->incarn = id->incarn;        /* a new process on this stream */
		w->hi = 0;
		memset(w->bits, 0, sizeof w->bits);
	}
	rc = win_admit(w, id->seq);
	unspin(&w->lock);
	if (rc == CLREPLAY_OK &&           /* once a second: the line is shared */
	    now - __atomic_load_n(&s->seen_ms, __ATOMIC_RELAXED) >= 1000)
		__atomic_store_n(&s->seen_ms, now, __ATOMIC_RELAXED);
	return rc == CLREPLAY_OK ? rc : counted(rc);
}

void clreplay_counts(unsigned long long out[CLREPLAY_NCOUNT])
{
	int i;

	for (i = 0; i < CLREPLAY_NCOUNT; i++)
		out[i] = __atomic_load_n(&cnt[i], __ATOMIC_RELAXED);
}
