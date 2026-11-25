/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* store.h - the collection registry: lookups are lock-free reads, and an entry is published
 * whole or not at all */
#ifndef PC_STORE_H
#define PC_STORE_H

#include "core/pcache_htable.h"

/* table sizing mode, after Ceph's pg_autoscale_mode: AUTO resizes by key count, WARN only says
 * what AUTO would do, OFF is silent; under WARN and OFF keys past the slots go to overflow */
extern int pc_autoscale_default;            /* [daemon] autoscale */
/* [daemon] redis_types, and a collection's own (config.h) */
extern int pc_types_default;
void pc_store_set_types_idx(int i, int m);
/* `durable = no` - a cache collection, off the WAL and the snapshot */
void pc_store_set_nondurable_idx(int i, int on);
int pc_store_durable_idx(int i);
int pc_store_durable(pcache_htable_t *ht);
int pc_store_durable_name(const char *name, size_t nlen);
int pc_store_types_strict(pcache_htable_t *ht);
int pc_store_autoscale(int i);
void pc_store_set_autoscale_idx(int i, int m);
/* set by name at fleet generation @gen, persisted by the caller: an older @gen is refused, an equal
 * one takes the higher mode.  1 applied, 0 refused as older, -1 no such collection, -2 not a mode */
int pc_store_autoscale_apply(const char *name, size_t nlen, int m,
		unsigned long long gen, int *was);
/* the generation of the mode set at runtime, 0 when the config's holds */
unsigned long long pc_store_autoscale_gen(int i);
int pc_store_autoscale_isset(int i);
/* collection @i's mode set at runtime, for the resync to a peer.
 * 0 while @i is live and has one, -1 otherwise */
int pc_store_autoscale_entry(int i, const char **name, int *m,
		unsigned long long *gen);
/* digest of the runtime-set modes, equal on members that hold the same (the keepalive carries
 * it); *@n = how many collections have one, 0 and a digest of 0 when none */
uint32_t pc_store_autoscale_sig(int *n);
/* what AUTO would resize it to, as log2 buckets - 0 for "leave it" */
int pc_store_autoscale_target(int i);
void pc_store_set_autoscale_target(int i, int t);

/* register a collection (startup only, single-threaded): 0, or -1 if the table is full */
int pc_store_register(const char *name, pcache_htable_t *ht, int pull,
		int proxy, int shard, int eager, int buckets_log2);

/* what a runtime-created collection inherits: the cluster's mode, set once from the boot config */
extern int pc_col_default_pull, pc_col_default_proxy, pc_col_default_shard,
	pc_col_default_eager;

/* the longest name the registry stores (copied: a runtime create has no config string) */
#define PC_COL_NAME_MAX 64

/* create while serving: published whole or not at all, reusing a dropped slot of the same size.
 * 0 = created, -1 = the name is taken, the registry is full or the table could not be made */
int pc_store_create(const char *name, size_t nlen, int buckets_log2);
int pc_store_create_gen(const char *name, size_t nlen, int buckets_log2,
		unsigned long long gen);

/* tombstoned first, then the records freed by ordinary removes, so no pointer loses its target;
 * the table stays carved for the next create of that size.  Records freed, -1 if none, -2 if
 * @gen is older than the live collection's (a stale drop) */
long pc_store_drop(const char *name, size_t nlen);
long pc_store_drop_gen(const char *name, size_t nlen, unsigned long long gen);
void pc_store_note_drop(const char *name, size_t nlen, unsigned long long gen);
int pc_store_entry(int i, const char **name, int *buckets_log2,
		unsigned long long *gen, int *live);

/* resize while serving: bounded copy passes, one pointer swap commits; unlogged, so a crash keeps
 * the old size.  0 started, -1 none, -2 busy, -3 too small, -4 no table, -5 same size */
int pc_store_resize_start(const char *name, size_t nlen, int target_log2);
/* a resize the fleet announced at @gen - ordered by @gen, held while
 * one runs.  1 = older than the newest applied (ignored), 2 = held. */
int pc_store_resize_request(const char *name, size_t nlen, int target_log2,
		unsigned long long gen);
void pc_store_resize_gen_note(const char *name, size_t nlen,
		unsigned long long gen);

/* rename a live collection - one pointer swap, and the collections
 * file is left naming it as the replay window starts (see store.c) */
int pc_store_rename(const char *from, size_t flen, const char *to, size_t tlen,
		unsigned long long gen);

/* one tick's worth of migration, from the maintenance thread */
void pc_store_resize_tick(void);
/* the retirement queue (maint, after the resize tick), its
 * figures, and the publish generation a resumable walk checks against */
void pc_store_retire_tick(void);
void pc_store_retire_figures(unsigned int *pending, unsigned long long *cleared,
		unsigned long long *returned);
unsigned long long pc_store_pub_gen(int i);
/* the table and its publish generation as one consistent pair (dump) */
pcache_htable_t *pc_store_find_pub(const char *name, size_t nlen,
		unsigned long long *pub);
/* the slot publishing @ht and its generation, one pair; -1 if none does */
int pc_store_slot_pub(pcache_htable_t *ht, unsigned long long *pub);

/* the table a resize is filling, so a DELETE reaches both and no key returns at the swap;
 * NULL when no resize is running for @ht */
pcache_htable_t *pc_store_resize_shadow(pcache_htable_t *ht);
/* a delete during a resize empties the table the copier reads FIRST (the copy is made under that
 * table's bucket lock), then the other: returns the first, *@second the other or NULL */
pcache_htable_t *pc_store_del_order(pcache_htable_t *ht, pcache_htable_t **second);
void pc_store_test_del_pause(const str *key);
/* pcache_ht_scan_ex for a CLIENT cursor that survives a resize: a swap restarts the walk (SCAN may
 * repeat, never skip) and a draining resize walks the old table first.  0 = start / complete */
int pc_store_scan(pcache_htable_t *ht, unsigned int *cursor,
		unsigned int max_buckets, unsigned int flags,
		pcache_iter_cb cb, void *ctx);

/* is entry @i being resized, and how far along?  For stats. */
int pc_store_resizing(int i, int *target_log2, unsigned long long *moved);

/* is entry @i live?  Enumerators (stats, metrics, the snapshot, the
 * interchange digest, the maintenance loop) must skip the tombstones. */
int pc_store_live(int i);

/* entry @i's table size and whether it was created at runtime */
int pc_store_buckets_log2(int i);
/* the size the FLOOR mode will not shrink below, set by an explicit resize */
int pc_store_floor_log2(int i);
void pc_store_set_floor(const char *name, size_t nlen, int log2);
int pc_store_runtime(int i);
/* the name the config registered entry @i under (renames and drops keep it), NULL if none */
const char *pc_store_cfg_name(int i);

/* the created set, persisted under the state directory; load runs BEFORE the WAL replay and the
 * snapshot import, which drop records for a name they cannot resolve */
int pc_store_persist(void);
/* the same off the caller's thread: pc-maint writes it on its next tick */
void pc_store_persist_later(void);
void pc_store_persist_tick(void);
int pc_store_load(const char *state_dir);

/* find a collection by NUL-terminated-in-buffer name span; NULL if none */
pcache_htable_t *pc_store_find(const char *name, size_t nlen);

/* iterate registered collections (for the stats verb over all) */
int pc_store_count(void);
/* store-time size accounting (the write stream, no walk) */
void pc_store_note_set(pcache_htable_t *ht, size_t klen, size_t vlen);
/* hot keys: 1 request in pc_store_hot_rate() sampled into a space-saving sketch per collection,
 * counts halved each minute; rows heaviest first (est >= true >= est - err, in samples) */
struct pc_hot_row {
	unsigned int est, err, klen, kept;     /* kept: bytes of key[] valid */
	char key[64];
};
void pc_store_hot_note(pcache_htable_t *ht, const char *k, size_t kl);
void pc_store_hot_tick(void);
int pc_store_hot_get(int i, struct pc_hot_row *out, int cap,
		unsigned long long *sampled, unsigned long long *total,
		unsigned long long *busy);
int pc_store_hot_rate(void);
/* the distinct-key sketch of @i's last held walk (PC_HLL_M one-byte HyperLogLog registers, hash
 * fnv1a64 + fmix64) into @out, plus the walk's unix time; -1 when there has been no walk */
#define PC_HLL_P 12
#define PC_HLL_M (1u << PC_HLL_P)
int pc_store_sketch(int i, unsigned char *out, unsigned int *walked_at);
int pc_store_size_stats(int i, unsigned long long *bytes,
		unsigned long long *n, unsigned long long hist[8]);
const char *pc_store_name(int i);
pcache_htable_t *pc_store_ht(int i);
/* collection @i's live table and the one a swapped resize still drains
 * (or NULL), held so until pc_store_walk_end - the old one is walked first */
void pc_store_walk_begin(int i, pcache_htable_t **live, pcache_htable_t **old);
void pc_store_walk_end(void);
int pc_store_slot_of(pcache_htable_t *ht);
/* key + value bytes a collection holds, from a walk the maintenance thread repeats; *age_s =
 * seconds since it was taken, (unsigned)-1 before the first walk */
void pc_store_held_tick(void);
unsigned long long pc_store_held(int i, unsigned int *age_s);
/* microseconds the last held walk took - the time every other
 * maintenance duty spent waiting behind it */
unsigned int pc_store_held_walk_us(int i);
/* the value sizes held, in size_hist's eight classes, from the same
 * walk; -1 before the first walk */
int pc_store_held_hist(int i, unsigned long long hist[8]);
/* the budget's parts per collection - index regions carved for the
 * table (exact) and the records as the cells they occupy (from the walk) */
void pc_store_note_index(pcache_htable_t *ht, unsigned long bytes);
unsigned long pc_store_index_bytes(int i);
unsigned long long pc_store_held_cells(int i);
int pc_store_pull_enabled(pcache_htable_t *ht);
int pc_store_proxy_enabled(pcache_htable_t *ht);
int pc_store_shard_enabled(pcache_htable_t *ht);
int pc_store_eager_enabled(pcache_htable_t *ht);

void pc_store_reset(void);
/* the size tallies (stored_bytes, stored_n, size_hist) start again;
 * the held walk's figures are gauges and stay */
void pc_store_size_reset(void);

#endif /* PC_STORE_H */
