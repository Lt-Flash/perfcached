/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * store.h — the collection registry (task S8).  Collections are created
 * once at startup, before workers spawn, and never change, so lookups are
 * lock-free reads of a fixed table.
 */
#ifndef PC_STORE_H
#define PC_STORE_H

#include "core/pcache_htable.h"

/* register a collection (startup only, single-threaded).  Returns 0, or
 * -1 if the table is full. */
int pc_store_register(const char *name, pcache_htable_t *ht, int pull,
		int proxy, int shard, int eager);

/* find a collection by NUL-terminated-in-buffer name span; NULL if none */
pcache_htable_t *pc_store_find(const char *name, size_t nlen);

/* iterate registered collections (for the stats verb over all) */
int pc_store_count(void);
/* S67: store-time size accounting (the write stream, no walk) */
void pc_store_note_set(pcache_htable_t *ht, size_t klen, size_t vlen);
int pc_store_size_stats(int i, unsigned long long *bytes,
		unsigned long long *n, unsigned long long hist[8]);
const char *pc_store_name(int i);
pcache_htable_t *pc_store_ht(int i);
/* S109: what a collection HOLDS - key + value bytes of every record -
 * from a walk the maintenance thread repeats, paced by its own cost.
 * pc_store_held() returns the last figure; *age_s = seconds since it
 * was taken, (unsigned)-1 before the first walk. */
void pc_store_held_tick(void);
unsigned long long pc_store_held(int i, unsigned int *age_s);
/* S116: the value sizes held, in size_hist's eight classes, from the same
 * walk; -1 before the first walk */
int pc_store_held_hist(int i, unsigned long long hist[8]);
/* S120: the budget's parts per collection - index regions carved for the
 * table (exact) and the records as the cells they occupy (from the walk) */
void pc_store_note_index(pcache_htable_t *ht, unsigned long bytes);
unsigned long pc_store_index_bytes(int i);
unsigned long long pc_store_held_cells(int i);
int pc_store_pull_enabled(pcache_htable_t *ht);
int pc_store_proxy_enabled(pcache_htable_t *ht);
int pc_store_shard_enabled(pcache_htable_t *ht);
int pc_store_eager_enabled(pcache_htable_t *ht);

void pc_store_reset(void);
/* S123: the size tallies (stored_bytes, stored_n, size_hist) start again;
 * the held walk's figures are gauges and stay */
void pc_store_size_reset(void);

#endif /* PC_STORE_H */
