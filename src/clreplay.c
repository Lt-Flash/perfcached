/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clreplay.c - a sealed cluster frame is taken once.  See clreplay.h. */
#include <stdlib.h>
#include <string.h>

#include "clreplay.h"
#include "cluster.h"                    /* pc_cluster_replay_counts */

struct win {
	int lock;
	uint64_t incarn, hi;
	struct win *next;                  /* on the free list */
	uint64_t bits[CLREPLAY_WIN / 64];
};

#define TOMB 1ULL                          /* a slot let go: lookups go on past it */

struct sender {
	uint64_t key;                      /* 1 << 48 | ip << 16 | port; 0 = free */
	uint64_t incarn;
	long long seen_ms;
	struct win *w[CLREPLAY_STREAMS];
};

/* slots and windows are never freed, only let go and reused under tab_lock, so a
 * thread holding a stale pointer locks valid memory and finds it no longer its own */
static struct sender tab[CLREPLAY_SENDERS];
static struct win *freew;
static int tab_lock, nwins;
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

/* under tab_lock: senders not heard for CLREPLAY_STALE_MS give up their slot and windows;
 * each window is taken off under its lock, so an admit inside it finishes first */
static int evict_stale(long long now)
{
	int i, j, n = 0;

	for (i = 0; i < CLREPLAY_SENDERS; i++) {
		struct sender *s = &tab[i];

		if (__atomic_load_n(&s->key, __ATOMIC_RELAXED) <= TOMB ||
		    now - __atomic_load_n(&s->seen_ms, __ATOMIC_RELAXED) <= CLREPLAY_STALE_MS)
			continue;
		__atomic_store_n(&s->key, TOMB, __ATOMIC_RELEASE);
		for (j = 0; j < CLREPLAY_STREAMS; j++) {
			struct win *w = s->w[j];

			if (!w)
				continue;
			spin(&w->lock);
			__atomic_store_n(&s->w[j], NULL, __ATOMIC_RELEASE);
			unspin(&w->lock);
			w->next = freew;
			freew = w;
		}
		n++;
	}
	return n;
}

/* under tab_lock: the sender's slot, or the first free or let-go one on its chain */
static struct sender *claim(uint64_t key, unsigned int h, long long now)
{
	struct sender *s, *spare = NULL;
	unsigned int i;

	for (i = 0; i < CLREPLAY_SENDERS; i++) {
		uint64_t sk;

		s = &tab[(h + i) % CLREPLAY_SENDERS];
		sk = __atomic_load_n(&s->key, __ATOMIC_ACQUIRE);
		if (sk == key)
			return s;
		if (sk > TOMB)
			continue;
		if (!spare)
			spare = s;
		if (!sk)
			break;
	}
	if (spare) {
		__atomic_store_n(&spare->incarn, 0, __ATOMIC_RELAXED);
		__atomic_store_n(&spare->seen_ms, now, __ATOMIC_RELAXED);
		__atomic_store_n(&spare->key, key, __ATOMIC_RELEASE);
	}
	return spare;
}

static struct sender *sender_of(uint64_t key, long long now)
{
	unsigned int h = (unsigned int)((key * 0x9E3779B97F4A7C15ULL) >> 40);
	struct sender *s;
	unsigned int i;

	for (i = 0; i < CLREPLAY_SENDERS; i++) {
		uint64_t sk;

		s = &tab[(h + i) % CLREPLAY_SENDERS];
		sk = __atomic_load_n(&s->key, __ATOMIC_ACQUIRE);
		if (sk == key)
			return s;
		if (!sk)
			break;
	}
	spin(&tab_lock);
	if (!(s = claim(key, h, now)) && evict_stale(now))
		s = claim(key, h, now);
	unspin(&tab_lock);
	return s;
}

static struct win *win_new(void)
{
	struct win *w = freew;

	if (w)
		freew = w->next;
	else if (nwins < CLREPLAY_WINS && (w = calloc(1, sizeof *w)))
		nwins++;
	return w;
}

/* @s's window for @st, made if it has none; NULL with *full set when there is no
 * room, NULL alone when @s was let go under us */
static struct win *win_of(struct sender *s, uint64_t key, const struct clwire_id *id,
		long long now, int *full)
{
	struct win *w;

	*full = 0;
	spin(&tab_lock);
	if (__atomic_load_n(&s->key, __ATOMIC_RELAXED) != key)
		w = NULL;
	else if (!(w = s->w[id->stream])) {
		if (!(w = win_new()) && evict_stale(now) &&
		    __atomic_load_n(&s->key, __ATOMIC_RELAXED) == key)
			w = win_new();
		if (!w)
			*full = __atomic_load_n(&s->key, __ATOMIC_RELAXED) == key;
		else {
			w->incarn = id->incarn;
			w->hi = 0;
			memset(w->bits, 0, sizeof w->bits);
			__atomic_store_n(&s->w[id->stream], w, __ATOMIC_RELEASE);
		}
	}
	unspin(&tab_lock);
	return w;
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

int clreplay_admit(const struct clwire_id *id, uint32_t from_ip, long long now)
{
	uint64_t key = 1ULL << 48 | (uint64_t)id->ip << 16 | id->port, cur;
	struct sender *s;
	struct win *w;
	int rc, full, tries;

	if (id->ip != from_ip)             /* it speaks for its own host only */
		return counted(CLREPLAY_HOST);
	if (id->stream >= CLREPLAY_STREAMS)
		return counted(CLREPLAY_FULL);
	for (tries = 0; ; tries++) {
		if (tries == 3 || !(s = sender_of(key, now)))
			return counted(CLREPLAY_FULL);
		if (!(w = __atomic_load_n(&s->w[id->stream], __ATOMIC_ACQUIRE)) &&
		    !(w = win_of(s, key, id, now, &full))) {
			if (full)
				return counted(CLREPLAY_FULL);
			continue;
		}
		spin(&w->lock);
		if (__atomic_load_n(&s->w[id->stream], __ATOMIC_ACQUIRE) == w &&
		    __atomic_load_n(&s->key, __ATOMIC_ACQUIRE) == key)
			break;
		unspin(&w->lock);              /* let go under us: look again */
	}
	cur = __atomic_load_n(&s->incarn, __ATOMIC_ACQUIRE);
	if (id->incarn < cur) {
		/* the newer one gone quiet: this sender's clock stepped back */
		if (now - __atomic_load_n(&s->seen_ms, __ATOMIC_RELAXED) < CLREPLAY_STALE_MS) {
			unspin(&w->lock);
			return counted(CLREPLAY_INCARN);
		}
		__atomic_store_n(&s->incarn, id->incarn, __ATOMIC_RELEASE);
	} else
		while (id->incarn > cur && !__atomic_compare_exchange_n(&s->incarn,
		        &cur, id->incarn, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			;
	if (id->incarn != w->incarn) {
		/* below the sender's incarnation only: after a step back a window
		 * may still hold the higher one */
		if (id->incarn < __atomic_load_n(&s->incarn, __ATOMIC_ACQUIRE)) {
			unspin(&w->lock);
			return counted(CLREPLAY_INCARN);
		}
		w->incarn = id->incarn;        /* a new process on this stream */
		w->hi = 0;
		memset(w->bits, 0, sizeof w->bits);
	}
	rc = win_admit(w, id->seq);
	if (rc == CLREPLAY_OK &&           /* once a second: the line is shared */
	    now - __atomic_load_n(&s->seen_ms, __ATOMIC_RELAXED) >= 1000)
		__atomic_store_n(&s->seen_ms, now, __ATOMIC_RELAXED);
	unspin(&w->lock);
	return rc == CLREPLAY_OK ? rc : counted(rc);
}

/* cluster.h's view of the counts */
void pc_cluster_replay_counts(unsigned long long *dup, unsigned long long *old,
		unsigned long long *incarn, unsigned long long *full,
		unsigned long long *host)
{
	unsigned long long c[CLREPLAY_NCOUNT];

	clreplay_counts(c);
	*dup = c[CLREPLAY_DUP];
	*old = c[CLREPLAY_OLD];
	*incarn = c[CLREPLAY_INCARN];
	*full = c[CLREPLAY_FULL];
	*host = c[CLREPLAY_HOST];
}

void clreplay_counts(unsigned long long out[CLREPLAY_NCOUNT])
{
	int i;

	for (i = 0; i < CLREPLAY_NCOUNT; i++)
		out[i] = __atomic_load_n(&cnt[i], __ATOMIC_RELAXED);
}
