/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * store.c — the collection registry (task S8).  See store.h.
 */
#include <string.h>
#include <time.h>

#include "config.h"
#include "store.h"

static struct {
	const char *name;
	pcache_htable_t *ht;
	int pull;
	int proxy;
	int shard;
	int eager;
	/* S67: what is being STORED, kept at store time so the page can say
	 * "are we storing what we think we are" without a walk - a size
	 * distribution in log2 classes, and the running total the page turns
	 * into a mean and an estimate of memory held.  Expiry and eviction
	 * happen inside the table and are not subtracted here: this is the
	 * write stream, and the page says so. */
	unsigned long long stored_bytes, stored_n, size_hist[8];
	/* S109: the walked figure and its pacing (wall seconds) */
	unsigned long long held_bytes;
	unsigned int held_at, held_next, held_walk_us;
} reg[PC_MAX_COLLECTIONS];
static int reg_n;

void pc_store_note_set(pcache_htable_t *ht, size_t klen, size_t vlen)
{
	int i, c = 0;
	size_t v = vlen;

	for (i = 0; i < reg_n; i++)
		if (reg[i].ht == ht)
			break;
	if (i == reg_n)
		return;
	/* classes: <=64, <=256, <=1K, <=4K, <=16K, <=64K, <=256K, more */
	while (c < 7 && v > (64u << (2 * c)))
		c++;
	reg[i].stored_bytes += klen + vlen;
	reg[i].stored_n++;
	reg[i].size_hist[c]++;
}

/* S109: what a collection HOLDS, from a walk - not from this node's own
 * write stream (S67's estimate went blank on a node whose records all
 * arrived by the push, the sweep or the bootstrap pull).  The
 * maintenance thread walks each collection whole with the S40 cursor,
 * 1024 buckets per step under the bucket seqlock, values copied out as
 * SCAN does (the keys-only walk reports no length by design, and a
 * length-only flag would be a change to the vendored core), and
 * publishes the sum.  It paces itself on its own cost: the next walk
 * is 20x the last one's duration away, never sooner than 5 s nor later
 * than 60 s, so a large collection costs this thread at most 5%.  The
 * figure is exact when taken and a few seconds old when read. */
#define PC_HELD_MIN_S 5
#define PC_HELD_MAX_S 60

static int held_cb(const str *key, const str *val, unsigned int expires,
		void *ctx)
{
	unsigned long long *sum = ctx;

	(void)expires;                     /* held until the sweep frees it */
	*sum += (unsigned long long)key->len + (unsigned long long)val->len;
	return 0;
}

void pc_store_held_tick(void)
{
	unsigned int now = (unsigned int)time(NULL);
	int i;

	for (i = 0; i < reg_n; i++) {
		unsigned long long sum = 0, us;
		unsigned int cursor = 0, span;
		struct timespec t0, t1;

		if (now < reg[i].held_next)
			continue;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		do {
			if (pcache_ht_scan_ex(reg[i].ht, &cursor, 1024, 0,
			        held_cb, &sum) < 0)
				break;
		} while (cursor);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		us = (unsigned long long)(t1.tv_sec - t0.tv_sec) * 1000000ULL +
			(unsigned long long)(t1.tv_nsec - t0.tv_nsec) / 1000ULL;
		__atomic_store_n(&reg[i].held_bytes, sum, __ATOMIC_RELEASE);
		__atomic_store_n(&reg[i].held_at, now, __ATOMIC_RELEASE);
		reg[i].held_walk_us = (unsigned int)us;
		span = (unsigned int)(us * 20ULL / 1000000ULL);
		if (span < PC_HELD_MIN_S)
			span = PC_HELD_MIN_S;
		if (span > PC_HELD_MAX_S)
			span = PC_HELD_MAX_S;
		reg[i].held_next = now + span;
	}
}

unsigned long long pc_store_held(int i, unsigned int *age_s)
{
	unsigned int at;

	if (i < 0 || i >= reg_n) {
		if (age_s)
			*age_s = (unsigned int)-1;
		return 0;
	}
	at = __atomic_load_n(&reg[i].held_at, __ATOMIC_ACQUIRE);
	if (age_s)
		*age_s = at ? (unsigned int)time(NULL) - at : (unsigned int)-1;
	return __atomic_load_n(&reg[i].held_bytes, __ATOMIC_ACQUIRE);
}

int pc_store_size_stats(int i, unsigned long long *bytes,
		unsigned long long *n, unsigned long long hist[8])
{
	int c;

	if (i < 0 || i >= reg_n)
		return -1;
	*bytes = reg[i].stored_bytes;
	*n = reg[i].stored_n;
	for (c = 0; c < 8; c++)
		hist[c] = reg[i].size_hist[c];
	return 0;
}

int pc_store_register(const char *name, pcache_htable_t *ht, int pull,
		int proxy, int shard, int eager)
{
	if (reg_n >= PC_MAX_COLLECTIONS)
		return -1;
	reg[reg_n].name = name;
	reg[reg_n].ht = ht;
	/* proxy AND shard reads ride the pull path (unicast to the holder
	 * / owner); shard needs no locator and no broadcast. */
	reg[reg_n].pull = pull || proxy || shard;
	reg[reg_n].proxy = proxy;
	reg[reg_n].shard = shard;
	reg[reg_n].eager = eager;
	reg_n++;
	return 0;
}

int pc_store_eager_enabled(pcache_htable_t *ht)
{
	int i;

	for (i = 0; i < reg_n; i++)
		if (reg[i].ht == ht)
			return reg[i].eager;
	return 0;
}

int pc_store_shard_enabled(pcache_htable_t *ht)
{
	int i;

	for (i = 0; i < reg_n; i++)
		if (reg[i].ht == ht)
			return reg[i].shard;
	return 0;
}

int pc_store_proxy_enabled(pcache_htable_t *ht)
{
	int i;

	for (i = 0; i < reg_n; i++)
		if (reg[i].ht == ht)
			return reg[i].proxy;
	return 0;
}

int pc_store_pull_enabled(pcache_htable_t *ht)
{
	int i;

	for (i = 0; i < reg_n; i++)
		if (reg[i].ht == ht)
			return reg[i].pull;
	return 0;
}

pcache_htable_t *pc_store_find(const char *name, size_t nlen)
{
	int i;

	for (i = 0; i < reg_n; i++)
		if (strlen(reg[i].name) == nlen && !memcmp(reg[i].name, name, nlen))
			return reg[i].ht;
	return NULL;
}

int pc_store_count(void)              { return reg_n; }
const char *pc_store_name(int i)      { return reg[i].name; }
pcache_htable_t *pc_store_ht(int i)   { return reg[i].ht; }

void pc_store_reset(void)             { reg_n = 0; }
