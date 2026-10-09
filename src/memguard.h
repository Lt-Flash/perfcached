/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * memguard.h - S366: what the node's memory allows its clients.
 *
 * 10-09, 245 (a 1 GB LXC, 0.5.6.4): 50,000 idle RESP connections, opened
 * over 38 s, took the container to its limit; the daemon thrashed for 30 s
 * and the cgroup OOM killer took it.  max_clients said 524,165 - derived
 * from descriptors alone - and refused nothing; the daemon logged nothing
 * about memory before it died.  A held connection cost 12.0 KB of heap
 * (pc_conn and its in/out/wire buffers, kept for its lifetime) and 3.6 KB
 * of kernel (socket, file, epoll item), measured.
 *
 * Three answers, all here or fed from here:
 *
 *   - the ADMISSION GUARD.  The maintenance thread reads, once a second,
 *     what the node may still use: the tightest of /proc/meminfo's
 *     MemAvailable (lxcfs makes that the container's own inside an LXC,
 *     where the limit sits on a parent cgroup the container cannot see)
 *     and every limited cgroup on the process's own path (v2 memory.max,
 *     v1 memory.limit_in_bytes: limit - usage + inactive file pages).
 *     It takes off the arena's not-yet-committed reservation - the store
 *     may still grow into it - and the floor; what is left, divided by
 *     what a connection costs between its accept and its idle release,
 *     is the number of connections admitted until the next sample.  The
 *     accept path takes one from that budget (one atomic); an empty
 *     budget refuses the connection the way max_clients does.  Leaving
 *     the refusing state needs a further 5% of the limit (hysteresis).
 *
 *   - the max_clients DEFAULT counts memory: min(descriptor-derived,
 *     (limit - floor - arena ceiling - process base) / idle cost).  The
 *     guard is what protects; the default is what the page shows as the
 *     limit, so the figure is one a node can actually hold.
 *
 *   - the HEAP GIVE-BACK.  An idle connection's buffers are released by its
 *     worker (proto.c); the bytes freed are reported here, and once a
 *     burst of them has stopped, malloc_trim() hands the pages back -
 *     glibc keeps a freed heap otherwise (245 sat at 606 MB after the
 *     50,000 closed, its baseline 154 MB).
 */
#ifndef PC_MEMGUARD_H
#define PC_MEMGUARD_H

#include <stddef.h>

struct pc_config;

/* What a connection costs between its accept and its idle release, kernel
 * share included - the admission budget's unit.  Measured on 245 (10-09):
 * 12.0 KB heap + 3.6 KB kernel; rounded up. */
#define PC_CONN_COST_ACCEPT  (16u << 10)
/* What an IDLE connection costs once its buffers are released - the
 * max_clients default's unit.  pc_conn + its CLIENT LIST row + the kernel's
 * share, measured with idlebuftest's rig on 222 (DESIGN 12ks); rounded up. */
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
	unsigned long long arena_reserve;  /* the arena's uncommitted ceiling */
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

/* Startup, after the arena and the WAL exist and before any worker: find
 * the sources, take the first sample, fill the guard's budget.  Returns
 * the memory-derived client limit (-1: no limit found to derive one from). */
long pc_memguard_init(const struct pc_config *cfg);

/* The maintenance thread, once a second: sample, re-budget, leave the
 * refusing state when there is room again, trim the heap after a burst. */
void pc_memguard_tick(void);

/* The accept path: 1 = admit, 0 = refuse (counted, logged once per spell). */
int pc_memguard_admit(void);

/* proto.c: @bytes of connection memory were freed; @idle: by an idle
 * release (1 connection), not a close. */
void pc_memguard_freed(size_t bytes, int idle);

void pc_memguard_stats(struct pc_memguard_stats *out);

#endif /* PC_MEMGUARD_H */
