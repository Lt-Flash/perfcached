/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * store.c — the collection registry (task S8).  See store.h.
 */
#include <string.h>

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
