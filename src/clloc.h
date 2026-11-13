/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clloc.h - the locator and negative caches: a delete PLANTS a negative entry so reads stop pulling
 * the key, a set CLEARS it so a resurrected key shows at once.  Pure: `now` is always a parameter. */
#ifndef PC_CLLOC_H
#define PC_CLLOC_H

#include <stddef.h>
#include <stdint.h>

#define NEG_SLOTS     65536
#define NEG_WAYS      4          /* set-associative, oldest-expiry out */
#define LOC_SLOTS   262144
#define LOC_WAYS    4

/* the locator/negative plane's state: `C` embeds one; ~5MB of tables, so never a stack local */
struct clloc {
	/* a negative entry carries the VERSION of the delete that planted it (0 = unversioned), so a copy
	 * crossing it on the wire (pushes batch, tombstones do not) is told from a newer write */
	/* any thread writes and reads an entry - @seq, odd while one is
	 * written, publishes its three words whole */
	struct { uint64_t h; long long exp_ms; uint64_t ver; uint32_t seq; } neg[NEG_SLOTS];
	struct { uint64_t h; uint16_t node; } loc[LOC_SLOTS];
	unsigned long long neg_hits, loc_hits, loc_clears;
	unsigned long long loc_forgets;    /* holders forgotten in bulk */
	/* sets that evicted a LIVE entry for another key; tomb_displaced the dangerous subset - a live
	 * versioned tombstone, the key's only guard against a crossing copy */
	unsigned long long neg_displaced, tomb_displaced;
	/* the latest expiry of any negative entry set (ms, the caller's clock): past it a clear has nothing
	 * to do; raised only, by an atomic max */
	long long neg_until_ms;
};

/* the key hash both tables share; exposed so a test can reason about
 * collisions deliberately rather than by accident */
uint64_t clloc_hash(const char *col, size_t cn, const char *key, size_t kn);

/* negative cache: has this key been confirmed absent, and not yet
 * decayed at `now`?  Any thread. */
int  clloc_neg_hit(struct clloc *l, const char *col, size_t cn,
		const char *key, size_t kn, long long now);
void clloc_neg_set(struct clloc *l, const char *col, size_t cn,
		const char *key, size_t kn, long long now, int ms);
/* plant with the delete's version; clloc_neg_set keeps 0 */
void clloc_neg_set_ver(struct clloc *l, const char *col, size_t cn,
		const char *key, size_t kn, long long now, int ms, uint64_t ver);
/* 1 when a LIVE negative entry exists for the key; *ver is what the
 * delete carried (0 = unversioned).  Does not count as a hit. */
int  clloc_neg_ver(struct clloc *l, const char *col, size_t cn,
		const char *key, size_t kn, long long now, uint64_t *ver);
void clloc_neg_clear(struct clloc *l, const char *col, size_t cn,
		const char *key, size_t kn);

/* the locator: 4-way associative, since direct-mapped slots evicted ~30% of a 200k keyspace and each
 * eviction sent a re-write through placement; the victim way is a second hash bit-slice */
int  clloc_get(struct clloc *l, const char *col, size_t cn,
		const char *key, size_t kn);
void clloc_set(struct clloc *l, const char *col, size_t cn,
		const char *key, size_t kn, int node);
void clloc_clear(struct clloc *l, const char *col, size_t cn,
		const char *key, size_t kn);
/* forget every remembered holder (the negative cache stays): one aligned 16-bit store per entry, so a
 * racing reader sees the old holder or none.  Returns how many were forgotten. */
unsigned long clloc_forget_holders(struct clloc *l);

unsigned long long clloc_neg_hits(const struct clloc *l);
unsigned long long clloc_neg_displaced(const struct clloc *l);
unsigned long long clloc_tomb_displaced(const struct clloc *l);
unsigned long long clloc_loc_hits(const struct clloc *l);
unsigned long long clloc_loc_clears(const struct clloc *l);
unsigned long long clloc_loc_forgets(const struct clloc *l);

#endif /* PC_CLLOC_H */
