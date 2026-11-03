/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clloc.c - the locator and negative caches (M3).  See clloc.h.
 *
 * Every body came out of cluster.c unchanged; what differs is that the
 * tables are reached through a pointer and the clock is passed in.
 */
#include "clloc.h"
#include "fnv1a.h"

uint64_t clloc_hash(const char *col, size_t cn, const char *key,
		size_t kn)
{
	/* the 0x2f separator is what keeps ("ab","c") apart from ("a","bc") */
	uint64_t h = fnv1a64_more(fnv1a64_byte(fnv1a64(col, cn), 0x2f), key, kn);

	return h ? h : 1;
}

/*
 * RV-15: the negative cache is 4-way set-associative, and a full set
 * evicts the entry CLOSEST TO EXPIRY.  It was direct-mapped: a delete
 * whose key shared a slot with a live tombstone overwrote it, however
 * fresh - and a fresh tombstone is exactly the one guarding its key
 * against a copy of the deleted record still crossing on the wire
 * (S209).  Measured at ~55k deletes/s on two nodes: 19-70% of tombstones
 * displaced before their 2 s lifetime.  Same memory, same capacity; a
 * fresh tombstone now goes only after four newer entries land in its
 * set.
 */
static size_t neg_base(uint64_t h)
{
	return (size_t)(h % (NEG_SLOTS / NEG_WAYS)) * NEG_WAYS;
}

static void neg_wlock(struct clloc *l, size_t i)
{
	uint32_t s = __atomic_load_n(&l->neg[i].seq, __ATOMIC_RELAXED);

	while ((s & 1) || !__atomic_compare_exchange_n(&l->neg[i].seq, &s, s + 1,
	        1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		s = __atomic_load_n(&l->neg[i].seq, __ATOMIC_RELAXED) & ~1u;
	__atomic_thread_fence(__ATOMIC_RELEASE);
}

static void neg_wunlock(struct clloc *l, size_t i)
{
	__atomic_store_n(&l->neg[i].seq,
		__atomic_load_n(&l->neg[i].seq, __ATOMIC_RELAXED) + 1, __ATOMIC_RELEASE);
}

static void neg_put(struct clloc *l, size_t i, uint64_t h, long long exp,
		uint64_t ver)
{
	__atomic_store_n(&l->neg[i].h, h, __ATOMIC_RELAXED);
	__atomic_store_n(&l->neg[i].exp_ms, exp, __ATOMIC_RELAXED);
	__atomic_store_n(&l->neg[i].ver, ver, __ATOMIC_RELAXED);
}

/* one entry read whole: 0 while it is being written or it changed under us */
static int neg_read(const struct clloc *l, size_t i, uint64_t *h,
		long long *exp, uint64_t *ver)
{
	uint32_t s1 = __atomic_load_n(&l->neg[i].seq, __ATOMIC_ACQUIRE);

	if (s1 & 1)
		return 0;
	*h = __atomic_load_n(&l->neg[i].h, __ATOMIC_RELAXED);
	*exp = __atomic_load_n(&l->neg[i].exp_ms, __ATOMIC_RELAXED);
	*ver = __atomic_load_n(&l->neg[i].ver, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	return __atomic_load_n(&l->neg[i].seq, __ATOMIC_RELAXED) == s1;
}

/* the way holding @h live at @now, or -1; @ver its version */
static int neg_find(const struct clloc *l, size_t b, uint64_t h, long long now,
		uint64_t *ver)
{
	uint64_t eh, ev;
	long long ex;
	size_t w;

	for (w = 0; w < NEG_WAYS; w++)
		if (neg_read(l, b + w, &eh, &ex, &ev) && eh == h && ex > now) {
			if (ver)
				*ver = ev;
			return (int)w;
		}
	return -1;
}

/* where @h goes: its own way, else a free or expired one, else the one
 * closest to expiry - counted as displaced (a tombstone lost, if it
 * carried a version) */
static size_t neg_slot_for(struct clloc *l, size_t b, uint64_t h, long long now)
{
	size_t w, old = b;

	/* a choice only: the write that follows takes the entry's lock */
	for (w = 0; w < NEG_WAYS; w++)
		if (__atomic_load_n(&l->neg[b + w].h, __ATOMIC_RELAXED) == h)
			return b + w;
	for (w = 0; w < NEG_WAYS; w++)
		if (!__atomic_load_n(&l->neg[b + w].h, __ATOMIC_RELAXED) ||
		    __atomic_load_n(&l->neg[b + w].exp_ms, __ATOMIC_RELAXED) <= now)
			return b + w;
	for (w = 1; w < NEG_WAYS; w++)
		if (__atomic_load_n(&l->neg[b + w].exp_ms, __ATOMIC_RELAXED) <
		    __atomic_load_n(&l->neg[old].exp_ms, __ATOMIC_RELAXED))
			old = b + w;
	__atomic_fetch_add(&l->neg_displaced, 1, __ATOMIC_RELAXED);
	if (__atomic_load_n(&l->neg[old].ver, __ATOMIC_RELAXED))
		__atomic_fetch_add(&l->tomb_displaced, 1, __ATOMIC_RELAXED);
	return old;
}

int clloc_neg_hit(struct clloc *l, const char *col, size_t cn,
		const char *key, size_t kn, long long now)
{
	uint64_t h = clloc_hash(col, cn, key, kn);

	if (neg_find(l, neg_base(h), h, now, NULL) >= 0) {
		__atomic_fetch_add(&l->neg_hits, 1, __ATOMIC_RELAXED);
		return 1;
	}
	return 0;
}

/* S467: raise neg_until_ms to @until - never lower it */
static void neg_until(struct clloc *l, long long until)
{
	long long cur = __atomic_load_n(&l->neg_until_ms, __ATOMIC_RELAXED);

	while (until > cur &&
	       !__atomic_compare_exchange_n(&l->neg_until_ms, &cur, until, 0,
	           __ATOMIC_RELEASE, __ATOMIC_RELAXED))
		;
}

void clloc_neg_set(struct clloc *l, const char *col, size_t cn,
		const char *key, size_t kn, long long now, int ms)
{
	uint64_t h = clloc_hash(col, cn, key, kn);
	size_t i = neg_slot_for(l, neg_base(h), h, now);

	neg_wlock(l, i);
	neg_put(l, i, h, now + ms, 0);
	neg_wunlock(l, i);
	neg_until(l, now + ms);
}

void clloc_neg_set_ver(struct clloc *l, const char *col, size_t cn,
		const char *key, size_t kn, long long now, int ms, uint64_t ver)
{
	uint64_t h = clloc_hash(col, cn, key, kn);
	size_t i = neg_slot_for(l, neg_base(h), h, now);

	/* a later delete of the same key carries a later version; an older
	 * tombstone arriving late must not lower the bar */
	neg_wlock(l, i);
	if (l->neg[i].h == h && l->neg[i].exp_ms > now && l->neg[i].ver > ver)
		ver = l->neg[i].ver;
	neg_put(l, i, h, now + ms, ver);
	neg_wunlock(l, i);
	neg_until(l, now + ms);
}

int clloc_neg_ver(struct clloc *l, const char *col, size_t cn,
		const char *key, size_t kn, long long now, uint64_t *ver)
{
	uint64_t h = clloc_hash(col, cn, key, kn);
	*ver = 0;
	return neg_find(l, neg_base(h), h, now, ver) >= 0;
}

void clloc_neg_clear(struct clloc *l, const char *col, size_t cn,
		const char *key, size_t kn)
{
	uint64_t h = clloc_hash(col, cn, key, kn);
	size_t b = neg_base(h), w;

	for (w = 0; w < NEG_WAYS; w++)
		if (__atomic_load_n(&l->neg[b + w].h, __ATOMIC_RELAXED) == h) {
			neg_wlock(l, b + w);
			if (l->neg[b + w].h == h)
				__atomic_store_n(&l->neg[b + w].h, 0, __ATOMIC_RELAXED);
			neg_wunlock(l, b + w);
		}
}

static size_t loc_base(uint64_t h)
{
	return (size_t)(h % (LOC_SLOTS / LOC_WAYS)) * LOC_WAYS;
}

int clloc_get(struct clloc *l, const char *col, size_t cn,
		const char *key, size_t kn)
{
	uint64_t h = clloc_hash(col, cn, key, kn);
	size_t b = loc_base(h), w;

	for (w = 0; w < LOC_WAYS; w++)
		if (l->loc[b + w].h == h && l->loc[b + w].node) {
			__atomic_fetch_add(&l->loc_hits, 1, __ATOMIC_RELAXED);
			return l->loc[b + w].node;
		}
	return 0;
}

void clloc_set(struct clloc *l, const char *col, size_t cn,
		const char *key, size_t kn, int node)
{
	uint64_t h = clloc_hash(col, cn, key, kn);
	size_t b = loc_base(h), w;

	for (w = 0; w < LOC_WAYS; w++)
		if (l->loc[b + w].h == h) {
			l->loc[b + w].node = (uint16_t)node;
			return;
		}
	for (w = 0; w < LOC_WAYS; w++)
		if (!l->loc[b + w].node) {
			l->loc[b + w].h = h;
			l->loc[b + w].node = (uint16_t)node;
			return;
		}
	w = (size_t)((h >> 32) & (LOC_WAYS - 1));
	l->loc[b + w].h = h;
	l->loc[b + w].node = (uint16_t)node;
}

void clloc_clear(struct clloc *l, const char *col, size_t cn,
		const char *key, size_t kn)
{
	uint64_t h = clloc_hash(col, cn, key, kn);
	size_t b = loc_base(h), w;

	for (w = 0; w < LOC_WAYS; w++)
		if (l->loc[b + w].h == h) {
			l->loc[b + w].node = 0;
			__atomic_fetch_add(&l->loc_clears, 1, __ATOMIC_RELAXED);
			return;
		}
}

unsigned long clloc_forget_holders(struct clloc *l)
{
	unsigned long n = 0;
	size_t i;

	for (i = 0; i < LOC_SLOTS; i++)
		if (l->loc[i].node) {
			l->loc[i].node = 0;
			n++;
		}
	__atomic_fetch_add(&l->loc_forgets, n, __ATOMIC_RELAXED);
	return n;
}

unsigned long long clloc_loc_forgets(const struct clloc *l) { return l->loc_forgets; }
unsigned long long clloc_neg_hits(const struct clloc *l)  { return l->neg_hits; }
unsigned long long clloc_neg_displaced(const struct clloc *l)  { return l->neg_displaced; }
unsigned long long clloc_tomb_displaced(const struct clloc *l) { return l->tomb_displaced; }
unsigned long long clloc_loc_hits(const struct clloc *l)  { return l->loc_hits; }
unsigned long long clloc_loc_clears(const struct clloc *l) { return l->loc_clears; }
