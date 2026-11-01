/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * pcache_opool.h - OC-2 (DESIGN 12oc): one table's overflow buckets.
 *
 * A bucket whose six slots are full links an overflow bucket of the same
 * 64-byte shape; those come from here.  Carved PCACHE_OPOOL_PER to a region
 * from the arena, handed out zeroed, taken back onto a free list.  The lock
 * is taken only to take or return a bucket - never on a lookup.
 */
#ifndef PCACHE_OPOOL_H
#define PCACHE_OPOOL_H

#include <stddef.h>
#include "../compat/locking.h"

#define PCACHE_OBKT_SIZE   64
#define PCACHE_OPOOL_PER   4096          /* buckets a region: one 256 KB slot */

struct pcache_bucket;

typedef struct pcache_opool {
	gen_lock_t              lock;
	struct pcache_bucket   *free;        /* linked through slot[0] */
	void                  **regions;     /* for retire */
	unsigned int            nregions, regions_cap;
	unsigned long           in_use;      /* handed out, under the lock */
	unsigned long           carved;      /* buckets carved, under the lock */
} pcache_opool_t;

void pcache_opool_init(pcache_opool_t *p);
/* a zeroed bucket, or NULL when the arena refuses another region */
struct pcache_bucket *pcache_opool_get(pcache_opool_t *p);
/* back onto the free list - never under a bucket lock (3.5b) */
void pcache_opool_put(pcache_opool_t *p, struct pcache_bucket *b);
unsigned long pcache_opool_in_use(pcache_opool_t *p);
unsigned long pcache_opool_bytes(pcache_opool_t *p);
/* give every region back; bytes returned */
unsigned long pcache_opool_retire(pcache_opool_t *p);

#endif /* PCACHE_OPOOL_H */
