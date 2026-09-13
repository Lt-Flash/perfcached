/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clloc.h - the locator cache and the negative cache (M3).
 *
 * Two fixed-size tables over the same key hash, and the tombstone pair
 * that keeps them honest: a delete PLANTS a negative entry so the next
 * read does not re-pull a key the fleet no longer has, and a set CLEARS
 * it so a resurrected key is visible at once.  Getting that pair the
 * wrong way round is silent - the key simply stays missing, or is
 * pulled for ever - which is why it is worth a module with a test.
 *
 * Pure over its own struct: no socket, no clock, no log, nothing from
 * cluster.h.  `now` is a parameter to every entry point that expires
 * anything, so TTL decay is exercised without sleeping.
 *
 * The counters live here too (rule 3).  They are COPIED into the
 * published stats structs by cluster.c\'s stats assembly, because
 * pc_cl_stats and pc_proxy_stats are a contract that verbs.c and
 * statuspage.c read.
 */
#ifndef PC_CLLOC_H
#define PC_CLLOC_H

#include <stddef.h>
#include <stdint.h>

#define NEG_SLOTS     65536
#define LOC_SLOTS   262144
#define LOC_WAYS    4

/* The locator/negative plane\'s state.  `C` embeds one; every function
 * below takes the pointer.  ~5MB of tables, so never a stack local. */
struct clloc {
	struct { uint64_t h; long long exp_ms; } neg[NEG_SLOTS];
	struct { uint64_t h; uint16_t node; } loc[LOC_SLOTS];
	unsigned long long neg_hits, loc_hits, loc_clears;
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
void clloc_neg_clear(struct clloc *l, const char *col, size_t cn,
		const char *key, size_t kn);

/* 4-way associative: direct-mapped slots evicted ~30% of a 200k
 * keyspace by collision, and every evicted entry sent a re-write back
 * through placement (the fork the probe now guards).  Same memory,
 * sets of LOC_WAYS; the victim way is a second hash bit-slice. */
int  clloc_get(struct clloc *l, const char *col, size_t cn,
		const char *key, size_t kn);
void clloc_set(struct clloc *l, const char *col, size_t cn,
		const char *key, size_t kn, int node);
void clloc_clear(struct clloc *l, const char *col, size_t cn,
		const char *key, size_t kn);

unsigned long long clloc_neg_hits(const struct clloc *l);
unsigned long long clloc_loc_hits(const struct clloc *l);
unsigned long long clloc_loc_clears(const struct clloc *l);

#endif /* PC_CLLOC_H */
