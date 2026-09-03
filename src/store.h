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
const char *pc_store_name(int i);
pcache_htable_t *pc_store_ht(int i);
int pc_store_pull_enabled(pcache_htable_t *ht);
int pc_store_proxy_enabled(pcache_htable_t *ht);
int pc_store_shard_enabled(pcache_htable_t *ht);
int pc_store_eager_enabled(pcache_htable_t *ht);

void pc_store_reset(void);

#endif /* PC_STORE_H */
