/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* pcache_opool.c - OC-2: overflow buckets for one table's chains (see the header) */
#include <string.h>
#include <stdlib.h>

#include "../compat/dprint.h"
#include "pcache_arena.h"
#include "pcache_htable.h"
#include "pcache_opool.h"

_Static_assert(sizeof(pcache_bucket_t) == PCACHE_OBKT_SIZE,
	"an overflow bucket is a bucket");

#define OPOOL_REGION ((size_t)PCACHE_OPOOL_PER * PCACHE_OBKT_SIZE)

void pcache_opool_init(pcache_opool_t *p)
{
	memset(p, 0, sizeof *p);
	lock_init(&p->lock);
}

/* under the lock: one more region onto the free list */
static int opool_carve(pcache_opool_t *p)
{
	pcache_bucket_t *b;
	void **rg;
	unsigned int i;

	if (p->nregions == p->regions_cap) {
		unsigned int cap = p->regions_cap ? 2 * p->regions_cap : 16;

		rg = realloc(p->regions, cap * sizeof *rg);
		if (!rg)
			return -1;
		p->regions = rg;
		p->regions_cap = cap;
	}
	b = pcache_region_alloc(OPOOL_REGION);
	if (!b)
		return -1;
	p->regions[p->nregions] = b;
	__atomic_store_n(&p->nregions, p->nregions + 1, __ATOMIC_RELAXED);  /* read lock-free */
	for (i = 0; i < PCACHE_OPOOL_PER; i++) {
		b[i].slot[0] = (pcache_rec_t *)p->free;
		p->free = &b[i];
	}
	p->carved += PCACHE_OPOOL_PER;
	return 0;
}

pcache_bucket_t *pcache_opool_get(pcache_opool_t *p)
{
	pcache_bucket_t *b;

	lock_get(&p->lock);
	if (!p->free && opool_carve(p) != 0) {
		lock_release(&p->lock);
		return NULL;
	}
	b = p->free;
	p->free = (pcache_bucket_t *)b->slot[0];
	__atomic_store_n(&p->in_use, p->in_use + 1, __ATOMIC_RELAXED);
	lock_release(&p->lock);
	memset(b, 0, sizeof *b);
	return b;
}

void pcache_opool_put(pcache_opool_t *p, pcache_bucket_t *b)
{
	/* a stale reader may still look: no tags, no link - it sees nothing */
	memset(b, 0, sizeof *b);
	lock_get(&p->lock);
	b->slot[0] = (pcache_rec_t *)p->free;
	p->free = b;
	__atomic_store_n(&p->in_use, p->in_use - 1, __ATOMIC_RELAXED);
	lock_release(&p->lock);
}

unsigned long pcache_opool_in_use(pcache_opool_t *p)
{
	return __atomic_load_n(&p->in_use, __ATOMIC_RELAXED);
}

unsigned long pcache_opool_bytes(pcache_opool_t *p)
{
	return (unsigned long)__atomic_load_n(&p->nregions, __ATOMIC_RELAXED) *
		pcache_arena_region_cost(OPOOL_REGION);
}

unsigned long pcache_opool_retire(pcache_opool_t *p)
{
	unsigned long freed = 0;
	unsigned int i;

	for (i = 0; i < p->nregions; i++)
		freed += pcache_region_free(p->regions[i], OPOOL_REGION);
	free(p->regions);
	memset(p, 0, sizeof *p);
	return freed;
}
