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
		int proxy, int shard, int eager, int buckets_log2);

/* S69: what a runtime-created collection inherits.  A fleet is one mode
 * over one collection set, so a created collection takes the cluster's
 * mode rather than a per-collection variant of its own; the daemon sets
 * these once from the config it started with. */
extern int pc_col_default_pull, pc_col_default_proxy, pc_col_default_shard,
	pc_col_default_eager;

/* S69: the longest collection name the registry stores (the name is
 * copied - a runtime create has no config string to point at) */
#define PC_COL_NAME_MAX 64

/* S69: create a collection while the daemon serves.  The table is made
 * here, the entry is published so readers see it whole or not at all, and
 * a slot left by an earlier drop of a table THIS SIZE is reused rather
 * than carving a second index.  0 = created, -1 = the name is taken, the
 * registry is full, or the table could not be made. */
int pc_store_create(const char *name, size_t nlen, int buckets_log2);
int pc_store_create_gen(const char *name, size_t nlen, int buckets_log2,
		unsigned long long gen);

/* S69: drop a collection.  The entry is tombstoned first, so every
 * lookup after this says "no such collection", and its records are then
 * freed by ordinary removes - a walk already in flight sees a table
 * emptying under it, which is the concurrent case the table already
 * handles, and nothing is freed out from under a pointer.  The table and
 * its index regions stay carved for the next create of the same size.
 * Returns the records freed, or -1 if there is no such collection. */
long pc_store_drop(const char *name, size_t nlen);
long pc_store_drop_gen(const char *name, size_t nlen, unsigned long long gen);
void pc_store_note_drop(const char *name, size_t nlen, unsigned long long gen);
int pc_store_entry(int i, const char **name, int *buckets_log2,
		unsigned long long *gen, int *live);

/* S69: resize a collection while it serves, in either direction.  One
 * mechanism for both: a second table at the new size is filled from the
 * live one in bounded passes, the registry's pointer is swapped - that
 * swap is the commit point, and it is one store - and two more passes
 * collect what the swap window left behind.  Nothing is logged and
 * nothing is pushed: the migration is an internal reshaping of one
 * name's storage, and a crash mid-way recovers to the pre-resize table
 * with every record, because recovery rebuilds from the snapshot and the
 * WAL under that name and the collections file still records the old
 * size.  0 = started; -1 no such collection; -2 one is already running;
 * -3 the target is below what the splitter would immediately undo;
 * -4 the table could not be made; -5 it is already that size. */
int pc_store_resize_start(const char *name, size_t nlen, int target_log2);

/* S69: rename a live collection - one pointer swap, and the collections
 * file is left naming it as the replay window starts (see store.c) */
int pc_store_rename(const char *from, size_t flen, const char *to, size_t tlen,
		unsigned long long gen);

/* one tick's worth of migration, from the maintenance thread */
void pc_store_resize_tick(void);

/* S69: the table a resize is filling, so a DELETE can be applied to both
 * - a key deleted after the copier passed it would otherwise come back
 * at the swap.  NULL when no resize is running for @ht. */
pcache_htable_t *pc_store_resize_shadow(pcache_htable_t *ht);

/* S69: is entry @i being resized, and how far along?  For stats. */
int pc_store_resizing(int i, int *target_log2, unsigned long long *moved);

/* S69: is entry @i live?  Enumerators (stats, metrics, the snapshot, the
 * interchange digest, the maintenance loop) must skip the tombstones. */
int pc_store_live(int i);

/* S69: entry @i's table size and whether it was created at runtime */
int pc_store_buckets_log2(int i);
int pc_store_runtime(int i);

/* S69: the created set, persisted under the state directory so a restart
 * does not put the operator back in the config file.  Load runs BEFORE
 * the WAL replay and the snapshot import, both of which address
 * collections by name and drop records for a name they cannot resolve. */
int pc_store_persist(void);
int pc_store_load(const char *state_dir);

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
/* S131: microseconds the last held walk took - the time every other
 * maintenance duty spent waiting behind it */
unsigned int pc_store_held_walk_us(int i);
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
