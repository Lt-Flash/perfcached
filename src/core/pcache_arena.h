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

/* S174: the largest cell, and so the largest RECORD (header + key +
 * value) the store accepts.  A chunk is ONE 256 KB slot carrying a
 * 64-byte header, and cells are cut from what is left, so 262,080 is
 * the largest class that can exist - exactly one cell per chunk, no
 * waste - and 262,144 would cut ZERO cells.  The .c asserts that
 * against the slot geometry rather than leaving it to a reader.
 *
 * It was 65,536 until 2026-09-20, which refused 19 of the operator's
 * 61,944 production values, the largest 218,265 bytes.  Raising it
 * grows every copy-out buffer sized from it - the per-thread fetch
 * scratch, the walks' snapshot buffers - but those are malloc'd and
 * only the bytes a record actually occupies are touched, so the cost is
 * ADDRESS SPACE, not residency: measured on a four-worker node with
 * every worker made to GET and to scan with values, VmData went 89.4 ->
 * 92.2 MB (+2.8) and RSS did not move outside noise (68.7 -> 68.9 MB).
 * Residency arrives with the big values themselves, which is the point
 * of having them. */
#define PCACHE_CELL_MAX   262080
#define PCACHE_NCLASSES   27
/* S235: live-bytes counter stripes, one per thread up to this many */
#define PCACHE_LIVE_STRIPES 64

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
/*
 * S188: top up the commit-ahead window, and nothing else.
 *
 * The window (S166) was refilled by the reclaim tick at 1 Hz, and a
 * bulk load outruns that by an order of magnitude.  Measured on the
 * fleet 2026-09-20: 62,000 records in 0.8 s grew the arena from 1.5 MB
 * to 33.75 MB live, crossing a 2 MB group boundary about every 50 ms,
 * and twenty crossings fell through to the WRITE PATH (commits_inline
 * 20 against commits_ahead 13).  Each is a 2 MB populate on the worker
 * that took the client write: the fleet logged an 8.3 ms bin.set and
 * an 8.7 ms set.
 *
 * The maintenance thread already wakes every 100 ms and only does its
 * 1 Hz duties when the tick changes, so this is the same work at ten
 * times the cadence.  Idle costs one predicate: ahead_wanted() is
 * false when no carve is recent, and the call returns having taken
 * nothing.
 */
void pcache_arena_ahead_tick(void);
int pcache_arena_mi(mi_item_t *aobj);        /* reclaim view for perf_stats */

/* a cell of at least @size bytes (including the class byte), or NULL if
 * size > PCACHE_CELL_MAX or shm is exhausted */
void *pcache_cell_alloc(unsigned int size);

/* a raw, 64-byte-aligned, never-freed region for index structures (bucket
 * segments, directories).  Same backing seam and never-returned guarantee
 * as chunks; NOT carved into cells and NOT zeroed. */
void *pcache_region_alloc(size_t size);
/* S150 C: give an index region back.  A reservation region's slots join
 * the region zone's free set - the next carve takes them before it
 * moves the frontier, and the give-back tick punches whole quiet groups
 * of them out, past the cool-off and above a small keep.  Only once the
 * table's readers have parked (quiesce.h): the caller's contract.  A
 * region from the shm or HG fallback is left as it was, never freed.
 * Returns the bytes that joined the free set. */
unsigned long pcache_region_free(void *p, size_t size);

/* owner free: private stack of the calling process */
void pcache_cell_free(void *cell);

/* cross-process free (expiry sweep, maintenance worker): global pool */
void pcache_cell_free_global(void *cell);

/* clamp bound for a possibly-stale cell pointer: the cell size of the
 * class in byte 0, or 0 if the byte is not a valid class id */
unsigned int pcache_cell_bound(const void *cell);
/* S190: is @cell in a chunk the reclaim tick marked SPARSE (at most an
 * eighth live, quiet for the window)?  And a cell from the densest chunk
 * of the class, for moving such a record out - see the .c */
int pcache_cell_evacuable(const void *cell);
unsigned int pcache_arena_sparse_chunks(void);   /* as of the last reclaim tick */
void *pcache_cell_alloc_dense(unsigned int size);

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
/* S120: the cell size a request of n bytes takes (0 beyond the classes),
 * and the index regions carved so far - the budget's two exact figures */
unsigned int pcache_arena_cell_size(size_t n);
unsigned long pcache_arena_regions_bytes(void);
/* total memory held from the host, and the ceiling it is held under */
unsigned long pcache_arena_held_bytes(void);
/* S128: what a region of `size` actually CHARGES the arena - the
 * whole-slot rounding the reservation carve pays (header-less since
 * S155: a segment's slot of buckets is one slot).  An index sizes itself
 * with this, so a preflight and the carve that follows it agree on the
 * figure. */
unsigned long pcache_arena_region_cost(unsigned long size);
/* S128: 0 if `bytes` more can be held under the ceiling, -1 if not (and
 * it says so).  A whole index tests itself against this BEFORE its first
 * carve: regions are never freed, so a table that ran out of room
 * half-built would leave its segments behind. */
int pcache_arena_room_for(unsigned long bytes);
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

/* S100: the most give-back one reclaim tick may do, groups and pages
 * together, in bytes.  0 = sized to the arena (an eighth of the ceiling,
 * never below 8 MB).  Set it before pcache_arena_init(). */
extern unsigned long pcache_arena_shrink_step_bytes;

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
	unsigned long pool_empty;      /* S94: cold groups NOT re-committed - hugetlb pool empty */
	/* S166: 2 MB groups the reclaim tick committed AHEAD of the chunk
	 * frontier, and groups a carve had to commit itself - the second is
	 * the write path populating 2 MB with a client waiting on it, and
	 * should stay at or near zero once the window is wide enough */
	unsigned long commits_ahead;
	unsigned long commits_inline;
	/* S168: groups committed by a REGION carve - a table split on the
	 * maintenance thread, or a collection being created.  Not a client's
	 * write, which is why it is counted apart from commits_inline. */
	unsigned long commits_index;
	/* S190: chunks marked sparse at the last reclaim tick, and cells the
	 * evacuator has been handed for survivors moved out of them */
	unsigned long sparse_chunks, evacuated;
	/* S167: bytes of the arena actually locked (mlock), and the latch
	 * set when a group could not be locked after a pinned start - a
	 * node that began resident and is now partly swappable, which
	 * nothing reported before */
	unsigned long locked_bytes;
	unsigned int pin_lost;
	unsigned long punch_calls;     /* S96: madvise calls that punched, one per run */
	unsigned long punch_groups;    /* S96: 2 MB groups those calls covered */
	unsigned long at_ceiling_since; /* S98: unix time the current spell at the ceiling began; 0 = not at it */
	unsigned long shrink_step;     /* S100: give-back per tick, as resolved */
	int giveback_off;              /* give-back latched off (madvise fail) */
	/* S114: what `held` is made of.  regions = the index regions (bucket
	 * directories, hint tables, counters), carved at table creation and
	 * on growth, never freed; warm_free = free slots kept resident (the
	 * per-class keep and whatever the give-back has not yet punched).
	 * The rest is S118's two figures below. */
	unsigned long regions_bytes;
	unsigned long warm_free_bytes;
	/* S118: the chunks the size classes own - one 256 KB slot each, the
	 * live records inside them and the free cells that belong to the
	 * class rather than to the arena - and the alignment slot every shm
	 * page carries.  held = regions + class_chunk + warm_free +
	 * page_slack, exactly: the card's rows add up. */
	unsigned long class_chunk_bytes;
	unsigned long page_slack_bytes;
	/* S97: bytes of the reservation committed (populated, and pinned when
	 * the arena is) - the initial commit plus every group a carve
	 * reached, less what the give-back punched.  RSS follows this, not
	 * the reservation. */
	unsigned long committed_bytes;
	unsigned long reserved_bytes;    /* the VA reservation: the cap */
	/* S119: bytes of the reservation's never-carved tail given back so
	 * far (also inside released_bytes) */
	unsigned long tail_released_bytes;
	/* S150 C: the region zone's free set - retired index slots kept
	 * resident (inside warm_free_bytes) and punched out (inside
	 * cold_bytes) - and the cumulative slots retired and takes served
	 * from the set instead of the frontier */
	unsigned long regions_free_warm_bytes;
	unsigned long regions_free_cold_bytes;
	unsigned long regions_retired;
	unsigned long region_reuse;
};
void pcache_arena_pressure(struct pcache_arena_pressure *out);

void pcache_arena_hugepage_capacity(int *active, unsigned long *total,
		unsigned long *used, unsigned long *free);

/* modparam-triggered startup selftest; returns -1 on any mismatch */
int pcache_arena_selftest(void);

#ifdef PCACHE_ARENA_DEBUG
/* RV-3: the allocator's invariants as things that can FAIL.  Built only
 * with -DPCACHE_ARENA_DEBUG (the sanitizer ladder, arenadbgtest); a
 * release build compiles none of it.
 *
 * Every cell's whereabouts live in a side table - out (with a user, or
 * uncut), on a private stack, or home on its chunk's list - and every
 * move is checked against it: a second free, and a second push home
 * (the precondition the lock-free pop's no-ABA argument rests on), abort
 * where they happen.  pcache_arena_census() then proves the other half
 * at a quiet moment: every cell is in EXACTLY one place, the lists are
 * what the table says, and the live figure adds up.  0 = holds, -1 = the
 * first violation is in @why.  The caller makes it quiet. */
int pcache_arena_census(char *why, size_t cap);
/* 1 (default) = abort on a violation; 0 = count it and carry on, which
 * is how a test shows the census catching what the abort would have */
extern int pcache_arena_dbg_abort;
unsigned long pcache_arena_dbg_violations(void);
#endif

#endif /* _PCACHE_ARENA_H_ */
