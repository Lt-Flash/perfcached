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

int clloc_neg_hit(struct clloc *l, const char *col, size_t cn,
		const char *key, size_t kn, long long now)
{
	uint64_t h = clloc_hash(col, cn, key, kn);
	size_t i = h % NEG_SLOTS;

	if (l->neg[i].h == h && l->neg[i].exp_ms > now) {
		__atomic_fetch_add(&l->neg_hits, 1, __ATOMIC_RELAXED);
		return 1;
	}
	return 0;
}

void clloc_neg_set(struct clloc *l, const char *col, size_t cn,
		const char *key, size_t kn, long long now, int ms)
{
	uint64_t h = clloc_hash(col, cn, key, kn);
	size_t i = h % NEG_SLOTS;

	l->neg[i].h = h;
	l->neg[i].exp_ms = now + ms;
}

void clloc_neg_clear(struct clloc *l, const char *col, size_t cn,
		const char *key, size_t kn)
{
	uint64_t h = clloc_hash(col, cn, key, kn);
	size_t i = h % NEG_SLOTS;

	if (l->neg[i].h == h)
		l->neg[i].h = 0;
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

unsigned long long clloc_neg_hits(const struct clloc *l)  { return l->neg_hits; }
unsigned long long clloc_loc_hits(const struct clloc *l)  { return l->loc_hits; }
unsigned long long clloc_loc_clears(const struct clloc *l) { return l->loc_clears; }
