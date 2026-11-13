/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* memguard.h - what the node's memory allows its clients: the admission guard, the
 * memory-aware max_clients default, and the heap give-back (malloc_trim after a burst). */
#ifndef PC_MEMGUARD_H
#define PC_MEMGUARD_H

#include <stddef.h>

struct pc_config;

/* What a connection costs between its accept and its idle release, kernel share included
 * (12.0 KB heap + 3.6 KB kernel measured, rounded up) - the admission budget's unit. */
#define PC_CONN_COST_ACCEPT  (16u << 10)
/* What an IDLE connection costs once its buffers are released (pc_conn + its CLIENT LIST row
 * + the kernel's share, measured, rounded up) - the max_clients default's unit. */
#define PC_CONN_COST_IDLE    (6u << 10)

/* the floor's default and bounds (memory_floor = N% form) */
#define PC_MEMFLOOR_PCT_DEFAULT  15
#define PC_MEMFLOOR_MIN          (128ull << 20)
#define PC_MEMFLOOR_MAX          (4ull << 30)

struct pc_memguard_stats {
	int enabled;                   /* 0: memory_floor = off */
	char source[96];               /* where the binding figure came from */
	unsigned long long limit;      /* the memory the node may use, bytes */
	unsigned long long available;  /* what it may still use */
	unsigned long long floor;      /* refuse below this */
	unsigned long long arena_reserve;  /* what the arena may still take from available */
	unsigned long long hugetlb_pool;   /* the static hugetlb pool, outside limit */
	unsigned long long hugetlb_pool_free;
	long long room;                /* available - floor - arena_reserve */
	long long budget;              /* connections admissible until the next sample */
	int refusing;
	unsigned int refusing_since;   /* get_ticks() at the first refusal */
	unsigned long long refused;    /* connections refused at the floor */
	unsigned long long engaged;    /* times the guard started refusing */
	/* the heap give-back */
	unsigned long long freed_bytes;    /* connection memory freed, cumulative */
	unsigned long long idle_released;  /* idle connections that gave back buffers */
	unsigned long long trims;          /* malloc_trim() calls */
	long long trim_last_bytes;         /* RSS the last one gave back */
	int trim_supported;                /* glibc's allocator: 1 */
	long long max_clients_memory;      /* the memory-derived default, -1 = none */
};

/* Startup, after the arena and the WAL exist, before any worker: first sample and budget.
 * Returns (limit - floor - arena - base) / PC_CONN_COST_IDLE, -1 if no limit is found. */
long pc_memguard_init(const struct pc_config *cfg);
#define PC_MEMGUARD_NOFIT (-2)         /* a cgroup limit it can never fit */
/* -C: the same sum before the arena exists (its process part a margin); 1 = it does not
 * fit, @out says why */
int pc_memguard_precheck(const struct pc_config *cfg, char *out, size_t n);

/* The maintenance thread, once a second: sample the tightest of MemAvailable and our cgroups,
 * re-budget, stop refusing at floor + 5% of the limit, trim the heap after a burst. */
void pc_memguard_tick(void);

/* The accept path: 1 = admit, 0 = refuse (counted, logged once per spell). */
int pc_memguard_admit(void);

/* proto.c: @bytes of connection memory were freed; @idle: by an idle
 * release (1 connection), not a close. */
void pc_memguard_freed(size_t bytes, int idle);

void pc_memguard_stats(struct pc_memguard_stats *out);

#endif /* PC_MEMGUARD_H */
