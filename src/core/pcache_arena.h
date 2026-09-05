/* PROVENANCE: vendored from the OpenSIPS cachedb_perf module
 * upstream: /dn/wt-pullshare-hg
 * branch: scratch/pullshare-t1
 * commit: 17bc82132e971cfd03ea0d9e1c150a83c5b42d59
 * path: modules/cachedb_perf/pcache_arena.h   synced: 2026-08-24
 * Local modifications ARE expected (compat-shim rewiring).
 * Check upstream drift with tools/sync-core.sh status|diff. */
/*
 * cachedb_perf - high-performance local memory cache
 *
 * Copyright (C) 2026 Yury Kirsanov
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * opensips is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#ifndef PCACHE_ARENA_H
#define PCACHE_ARENA_H

/*
 * Slab arena (DESIGN 3.3): entries live in fixed-size cells inside chunks
 * taken from shm and NEVER returned while the server runs - that is what
 * makes the lock-free read path (DESIGN 3.2) legal.  A chunk is permanently
 * bound to one size class; cells never straddle or move.
 *
 * The cell contract:
 *   - byte 0 of every cell is the CLASS ID, stamped for the whole chunk at
 *     carve time and never written again.  Callers lay their record out
 *     with byte 0 as a read-only class field.  This is how the copy-out
 *     clamp finds its bound through a stale pointer without aligned chunks:
 *     pcache_cell_bound() range-checks the byte and returns the cell size.
 *   - bytes 8..15 carry the free-list link while a cell is free; a live
 *     cell owns everything from byte 1 up.
 *
 * Allocation state is per-process (pkg, lazy): a bump chunk plus a private
 * free stack per class - zero shm traffic and zero atomics on the fast
 * path.  Owner frees go to the private stack (LIFO reuse); oversized
 * stacks donate half to a per-class global pool, which also serves refills
 * and takes cross-process frees (expiry / maintenance worker).
 */

#include "../compat/mi/item.h"

#define PCACHE_CELL_MAX   65536   /* largest cell; bigger allocs fail (v1) */
#define PCACHE_NCLASSES   21

/* Memory backing, decided in pcache_arena_init() (mod_init, pre-fork):
 *   PCACHE_BACKING_OWN     - this file's chunked allocator (chunks from
 *                            shm_malloc or the dedicated reservation)
 *   PCACHE_BACKING_CORE    - the core shm allocator is HG_MALLOC: every cell
 *                            is an HG slab cell in the shm arena
 *   PCACHE_BACKING_OWN_HG  - arena_hugepage_mb set on an HG_MALLOC build: the
 *                            arena is an HG arena of its own, fully managed
 *                            by HG (classes, GC, growth/shrink, maintenance)
 * Policy: the "memory_backing" modparam (auto|core|own-hg|own). */
enum pcache_backing { PCACHE_BACKING_OWN = 0, PCACHE_BACKING_CORE,
                      PCACHE_BACKING_OWN_HG };
extern char *pcache_backing_policy;          /* modparam memory_backing */
extern int pcache_arena_hugepage_cap_mb;     /* modparam, 0 = fixed */
extern char *pcache_arena_profile;           /* modparam arena_profile */
extern int pcache_reclaim_keep;              /* drained chunks kept per class */
extern int pcache_reclaim_quiet_s;           /* quiet window before give-back */
extern int pcache_reclaim_cooloff_s;         /* no give-back after a carve */
extern int pcache_reclaim_giveback;          /* 0 = retire/re-cut only */
int pcache_arena_backing(void);
const char *pcache_arena_backing_str(void);
void pcache_arena_backing_notice(void);

int pcache_arena_init(void);
void pcache_arena_destroy(void);

/* reset inherited allocator state after fork: donates any pre-fork bump
 * chunk / private cells to the global pool.  Two processes must never
 * share a bump pointer. */
void pcache_arena_child_init(void);
void pcache_arena_flush_private(void);       /* a done process sends cells home */
void pcache_arena_reclaim_tick(void);        /* the reclaim process, 1/s */
int pcache_arena_mi(mi_item_t *aobj);        /* reclaim view for perf_stats */

/* a cell of at least @size bytes (including the class byte), or NULL if
 * size > PCACHE_CELL_MAX or shm is exhausted */
void *pcache_cell_alloc(unsigned int size);

/* a raw, 64-byte-aligned, never-freed region for index structures (bucket
 * segments, directories).  Same backing seam and never-returned guarantee
 * as chunks; NOT carved into cells and NOT zeroed. */
void *pcache_region_alloc(size_t size);

/* owner free: private stack of the calling process */
void pcache_cell_free(void *cell);

/* cross-process free (expiry sweep, maintenance worker): global pool */
void pcache_cell_free_global(void *cell);

/* clamp bound for a possibly-stale cell pointer: the cell size of the
 * class in byte 0, or 0 if the byte is not a valid class id */
unsigned int pcache_cell_bound(const void *cell);

/* monotone address watermarks over all chunks (DESIGN 3.2 rule 2) */
void pcache_arena_extents(unsigned long *lo, unsigned long *hi);

void pcache_arena_stats(unsigned int *nchunks, unsigned long *bytes);

/* the memory tier the huge-page reservation actually achieved (1 hugetlb ..
 * 4 plain 4K), as opposed to the pcache_mem.tier probe - CP-11 */
int pcache_arena_tier(void);

/* see the implementation comment in pcache_arena.c - @active must be
 * checked before trusting total/used/free */
/* live cell bytes (perfcached addition): what the data actually holds
 * now - unlike capacity 'used', which is carved-chunk-granular and only
 * falls when reclaim retires */
unsigned long long pcache_arena_live_bytes(void);
/* total memory held from the host, and the ceiling it is held under */
unsigned long pcache_arena_held_bytes(void);
/* HARD ceiling in BYTES on memory held from the host; 0 = unlimited.
 * Set it before pcache_arena_init().  arena_mb alone bounds only the
 * huge-page reservation - past it the arena carves from shm_malloc,
 * which is plain malloc in a standalone build, so without this a node
 * has no configured capacity bound at all. */
extern unsigned long pcache_arena_max_bytes;

/* S47: never give held memory back below this many bytes (the HG-v3
 * hsize_min adoption).  0 = no floor.  A cache that empties overnight
 * otherwise repays every commit on the morning burst. */
extern unsigned long pcache_arena_floor_bytes;

/* S47: the pressure surface for stats.memory - the operator's view of
 * the cliff and the recovery.  tier is a static string owned by the
 * allocator (never freed by the caller). */
struct pcache_arena_pressure {
	const char *tier;
	unsigned long refused;         /* writes refused arena-full (nomem) */
	unsigned long retired;         /* chunks retired cumulatively */
	unsigned long pages_freed;
	unsigned long released_bytes;  /* cumulative give-back */
	unsigned long cold_bytes;      /* currently punched out */
	unsigned long flushes;         /* hoard-flush broadcasts */
	int giveback_off;              /* give-back latched off (madvise fail) */
};
void pcache_arena_pressure(struct pcache_arena_pressure *out);

void pcache_arena_hugepage_capacity(int *active, unsigned long *total,
		unsigned long *used, unsigned long *free);

/* modparam-triggered startup selftest; returns -1 on any mismatch */
int pcache_arena_selftest(void);

#endif /* _PCACHE_ARENA_H_ */
