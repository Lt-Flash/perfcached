/* PROVENANCE: vendored from the OpenSIPS cachedb_perf module
 * upstream: /dn/wt-pullshare-hg
 * branch: scratch/pullshare-t1
 * commit: 17bc82132e971cfd03ea0d9e1c150a83c5b42d59
 * path: modules/cachedb_perf/pcache_arena.c   synced: 2026-08-24
 * Local modifications ARE expected (compat-shim rewiring).
 * Check upstream drift with tools/sync-core.sh status|diff. */
/*
 * Copyright (C) 2026 OpenSIPS Solutions
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * cachedb_perf memory: where the cache's cells come from.
 *
 * Three backings, decided once in pcache_arena_init() (mod_init, pre-fork):
 *
 *   core    the core shm allocator is HG_MALLOC - every cell is an HG slab
 *           cell; classes, per-process caches, GC, growth and maintenance
 *           are HG's, nothing here but the class byte.
 *   own-hg  a dedicated arena on an HG_MALLOC build - created through the
 *           core's module-arena facade as an HG arena of our own.
 *   own     any other allocator: this file's slot allocator, below.
 *
 * THE OWN BACKING (DESIGN 3.3, reworked for reclaim - tasks C1..C9)
 *
 * Memory is cut into 256 KB SLOTS, each 256 KB-aligned.  A slot is one
 * CHUNK: a 64-byte header and cells of one size class.  A cell's chunk is
 * therefore a mask of its address - no per-cell back pointer, the record
 * header has no room for one.  Slots come from the dedicated reservation
 * (arena_hugepage_mb: 2 MB-aligned, huge-page backed, a bump frontier plus
 * the free lists) or from shm PAGES (16 slots carved out of one shm_malloc
 * block, plus one slot of alignment slack).
 *
 * Cells: byte 0 is the class id, stamped when the chunk is cut and never
 * written again while the chunk keeps that class (DESIGN 3.2 rule 1);
 * bytes 8..15 carry the free-list link.  A process allocates from a private
 * free stack, then from the bump of the chunk it cut, then - under the
 * arena lock - by pulling a batch of cells home on some chunk of the class.
 * A free goes to the private stack; past PCACHE_PRIVATE_MAX the surplus is
 * sent HOME: pushed on its own chunk's free list (lock-free), which is the
 * whole point - a chunk whose every cell is home is provably drained, no
 * process can hold a pointer it will later free.  Chunks with cells home
 * sit on a per-class "avail" stack (lock-free, one membership flag) where
 * the allocator finds them.
 *
 * Reclaim (the module's own process, one tick a second, never inline and
 * never a timer job): drained chunks beyond reclaim_keep per class are
 * RETIRED - the slot goes to the free lists and is re-cut for ANY class on
 * the next carve (cross-class reuse: the footprint follows the peak total,
 * not the sum of per-class peaks).  Free slots that form a whole 2 MB group
 * of the reservation, or a whole shm page, are given back to the host after
 * a quiet window and a cool-off since the last carve: MADV_DONTNEED on the
 * reservation (PORTED S3: private anon now - REMOVE is shmem-only; same
 * semantics: the mapping stays, a later carve re-faults), shm_free of the
 * page.  Nothing is ever unmapped, so a lock-free reader still holding a
 * pointer into a retired, re-cut or punched-out slot reads mapped memory:
 * the copy-out chain (extents, class bound, hash, key, version re-check)
 * rejects what it finds there, exactly as for any stale pointer.
 */

#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>

#include "../compat/dprint.h"
#include "../compat/locking.h"
#include "../compat/mem/mem.h"
#include "../compat/mem/shm_mem.h"
#include "../compat/mi/mi.h"
#include "../compat/ipc.h"
#include "pcache_arena.h"
#include "pcache_mem.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* S94: prefault a range, reporting where a touch would have raised
 * SIGBUS.  Linux 5.14; older headers do not spell it */
#ifndef MADV_POPULATE_WRITE
#define MADV_POPULATE_WRITE 23
#endif

/* the core's module-arena facade (HG_MALLOC v3 trees); older cores have no
 * such header - then only the own backing exists */
#if defined(__has_include)
# if __has_include("../compat/mem/mem_arena.h")
#  include "../compat/mem/mem_arena.h"
#  define PCACHE_HAVE_MEM_ARENA 1
# endif
#endif
#ifndef PCACHE_HAVE_MEM_ARENA
typedef void mem_arena_t;
static inline mem_arena_t *shm_arena_create(char *n, unsigned long i,
		unsigned long c) { return NULL; }
static inline int shm_arena_set_profile(mem_arena_t *a, const char *p)
{ return -1; }
static inline mem_arena_t *shm_arena_core(void) { return NULL; }
static inline void mem_arena_extents(const mem_arena_t *a, unsigned long *lo,
		unsigned long *hi) { *lo = 0; *hi = 0; }
static inline int mem_arena_tier(const mem_arena_t *a) { return 0; }
static inline void mem_arena_usage(mem_arena_t *a, unsigned long *c,
		unsigned long *k, unsigned long *l) { *c = 0; *k = 0; *l = 0; }
#define mem_arena_malloc(a, s)     ((void *)0)
#define mem_arena_free(a, p)       do { } while (0)
#endif

/* PORTED (S3): give-back is MADV_DONTNEED - MADV_REMOVE only serves
 * shmem/hugetlbfs and EINVALs on the private-anon backing used now.
 * DONTNEED frees anon pages immediately (hugetlb ones back to the pool)
 * while the mapping stays - the never-unmap invariant holds. */

char *pcache_backing_policy = "auto";
int pcache_arena_hugepage_cap_mb = 0;

/* HARD ceiling on memory held from the host, in bytes; 0 = unlimited.
 *
 * arena_mb was only ever the huge-page RESERVATION size.  Past it
 * slot_take() fell through to shm PAGES, and shm_malloc in the
 * standalone daemon is plain malloc, so a node grew with no configured
 * bound at all: measured 348 MB of RSS with arena_mb = 64, serving
 * 200,000 keys of 1 KB from a "64 MB" arena while arena_free read 0.
 *
 * config.h has documented arena_cap_mb as "0 = fixed at arena_mb" all
 * along, so the growth was a bug against the stated contract rather
 * than a design choice - an operator sizing a fleet by arena_mb was
 * sizing nothing. */
unsigned long pcache_arena_max_bytes = 0;
unsigned long pcache_arena_floor_bytes = 0;      /* S47 shrink floor */
unsigned long pcache_arena_shrink_step_bytes = 0; /* S100: 0 = sized to the arena */
static unsigned long arena_refused;    /* carves refused at the ceiling */
static unsigned int arena_refuse_log_tick;
static unsigned long arena_pool_empty; /* S94: cold groups not re-committed */
static unsigned int arena_pool_log_tick;
/* S98: the ceiling as a STATE.  Unix time of the first refusal of the
 * current spell at the ceiling, cleared by the next carve that succeeds;
 * 0 = not at it.  A counter says it happened; this says it is happening
 * now, and since when. */
static unsigned long arena_ceiling_since;
char *pcache_arena_profile = NULL;
int pcache_reclaim_keep = 1;        /* drained chunks kept per class */
int pcache_reclaim_quiet_s = 5;     /* slots free this long before give-back */
int pcache_reclaim_cooloff_s = 10;  /* no give-back this long after a carve */
int pcache_reclaim_giveback = 1;    /* 0: retire and re-cut only, keep resident */

static int backing = PCACHE_BACKING_OWN;
static mem_arena_t *hg_handle;            /* CORE: the shm block; OWN_HG: ours */

/* CP-20: MB to reserve for the huge-page arena; 0 = disabled (shm_malloc).
 * Set by the cachedb_perf "arena_hugepage_mb" modparam. */
int pcache_arena_hugepage_mb = 0;

/* ~x1.5 ladder, all multiples of 32 so cells stay 8-aligned.
 *
 * S174: the ladder stops being ~x1.5 above 65,536, because a chunk is
 * one 256 KB slot and the cells-per-chunk arithmetic is unforgiving
 * there: 98,304 cuts two cells (25% of the chunk unused), 131,072 cuts
 * ONE and wastes half, 196,608 cuts one and wastes a quarter, and
 * 262,080 cuts one with nothing left over.  So the two rungs that pay
 * their way are 98,304 and 262,080, and the ladder ends on the largest
 * cell a chunk can hold.  The operator's production keyspace has 19
 * values above the old ceiling and 3 above 196,608 - a 192 KB top
 * class would still have refused those three. */
/* S181: 320, 448, 640 and 896 are not part of the ladder's geometry -
 * they are where the operator's production keyspace SITS.  Measured
 * over 61,944 records (median 304 bytes, mean 509): 37,256 records
 * round into 384 and 22,298 into 768, which is 72% of all rounding
 * waste, and those two classes pack their chunks densely (254 and 208
 * free cells in the last one).  So four rungs there do not shave bytes,
 * they remove whole 256 KB chunks: 42.25 MB held drops to 38.25 MB, a
 * 9.5% cut, for four classes.
 *
 * The blanket alternatives measure WORSE in chunks, which is the number
 * that becomes RSS: a 1.25x ladder holds 42.75 MB (36 classes, each one
 * touched pinning at least one chunk), and merging the sparse tail
 * holds 42.50 MB because the records it displaces add chunks to the
 * dense classes faster than they free the thin ones.  DESIGN 12es has
 * the tables. */
static const unsigned int cell_sizes[PCACHE_NCLASSES] = {
	64, 96, 128, 192, 256, 320, 384, 448, 512, 640, 768, 896,
	1024, 1536, 2048,
	3072, 4096, 6144, 8192, 12288, 16384, 24576, 32768, 49152, 65536,
	98304, 262080
};

#define PCACHE_SLOT_SHIFT     18
#define PCACHE_SLOT           (1UL << PCACHE_SLOT_SHIFT)   /* 256 KB */
#define PCACHE_SLOT_MASK      (~(PCACHE_SLOT - 1))
#define PCACHE_CHUNK_HDR      64
#define PCACHE_PAGE_SLOTS     16            /* slots per shm page (4 MB) */
#define PCACHE_GROUP_SLOTS    8             /* slots per 2 MB huge page */
#define PCACHE_HPS            (PCACHE_SLOT * PCACHE_GROUP_SLOTS)

/* S174: a chunk is one slot minus its header, and a class larger than
 * what is left cuts ZERO cells - the allocator would hand out nothing
 * and the ceiling would be a lie.  262,144 is exactly that mistake. */
_Static_assert(PCACHE_CELL_MAX <= PCACHE_SLOT - PCACHE_CHUNK_HDR,
	"the largest cell must fit in a chunk: one slot less its header");
_Static_assert(PCACHE_CELL_MAX % 32 == 0,
	"cells are multiples of 32 so they stay 8-aligned");
/* S166: 2 MB groups kept committed AHEAD of the chunk frontier.
 *
 * A carve that crosses into a group the arena has not committed calls
 * group_commit(), which populates the whole 2 MB - and it does it on the
 * worker that took the client's write.  Measured 2026-09-20: 1.0-3.2 ms
 * on 4K pages and 2.2-4.0 ms on THP, on an idle 16-vCPU box; the fleet's
 * 4-vCPU LXC logged a 10.1 ms SET the same way.  Overwriting existing
 * keys never pays it, so it shows as a rare, unexplained tail.
 *
 * The reclaim tick (1/s, the maintenance thread) keeps this many groups
 * committed above the frontier, so the write path finds the memory
 * there.  Four groups = 8 MB: room for a burst between two ticks at the
 * rates this daemon serves, and small enough that a node which never
 * grows holds 8 MB it might not use.  The give-back's tail walk leaves
 * the window alone (S119, below), or the two would trade the same
 * pages every tick.  A carve that still finds an uncommitted group -
 * growth faster than the tick - pays as before and is COUNTED, so the
 * window's size can be argued from figures rather than taste. */
#define PCACHE_AHEAD_GROUPS   4
#define PCACHE_REFILL_BATCH   32            /* cells pulled home per refill */
#define PCACHE_PRIVATE_MAX    256           /* private stack size that triggers */
#define PCACHE_DONATE         128           /*   sending this many cells home  */
#define PCACHE_CLS_FREE       0xffffffffU   /* chunk header: the slot is free */

typedef struct pcache_page pcache_page_t;

/* one slot = one chunk; the header is the slot's first 64 bytes */
typedef struct pcache_chunk {
	struct pcache_chunk *link;   /* class avail stack, or a free-slot list */
	void *free_head;             /* cells home on this chunk (lock-free push) */
	pcache_page_t *page;         /* owning shm page, NULL = the reservation */
	unsigned int cls;            /* class, or PCACHE_CLS_FREE */
	unsigned int cell_size;
	unsigned int cells;
	unsigned int nfree;          /* cells on free_head */
	unsigned int in_avail;       /* 1 while on (or heading to) the avail stack */
	unsigned int cold;           /* memory punched out; a carve re-faults it */
	unsigned int free_at;        /* reclaim tick the slot became free */
	unsigned int home_since;     /* reclaim tick the first cell came home */
#ifdef PCACHE_ARENA_DEBUG
	unsigned char *dbg_state;    /* RV-3: one byte per cell, where it is */
#endif
	/* padded to PCACHE_CHUNK_HDR; cells follow */
} pcache_chunk_t;
_Static_assert(sizeof(pcache_chunk_t) <= PCACHE_CHUNK_HDR,
	"the chunk header outgrew the 64 bytes in front of the cells");

/* a shm block holding PCACHE_PAGE_SLOTS aligned slots */
struct pcache_page {
	pcache_page_t *next;
	char *raw;                   /* the shm_malloc block */
	char *base;                  /* first slot (256 KB-aligned) */
	pcache_chunk_t *free;        /* this page's free slots */
	unsigned int nslots;
	unsigned int nfree;          /* slots on the free list */
};

typedef struct pcache_region {
	struct pcache_region *next;
	unsigned long size;
} pcache_region_t;

typedef struct pcache_arena {
	gen_lock_t lock;                        /* refill, carve, retire, stats */
	unsigned long bytes;                    /* memory held from the host */
	unsigned long long live;                /* live CELL bytes, own backing
	                                         * (bytes is chunk-granular and
	                                         * peak-sticky until reclaim -
	                                         * the rebalancer levels on
	                                         * THIS, perfcached addition) */
	pcache_region_t *regions;               /* raw index regions (shm) */
	unsigned long regions_bytes;
	unsigned long lo, hi;                   /* extent watermarks */

	/* own backing: classes */
	pcache_chunk_t *avail[PCACHE_NCLASSES]; /* chunks with cells home (lock-free) */
	pcache_chunk_t *cur[PCACHE_NCLASSES];   /* the allocator's current chunk */
	unsigned int chunks_cls[PCACHE_NCLASSES];       /* chunks cut for the class */
	unsigned int chunks_peak_cls[PCACHE_NCLASSES];

	/* own backing: slots.  The reservation's free slots are bitmaps (one
	 * bit per slot; a carve takes the LOWEST free slot so the top groups
	 * drain), a page's free slots are its own list (a carve takes from
	 * the FULLEST page so whole pages drain).  Packing is what makes the
	 * give-back units - 2 MB groups, 4 MB pages - ever become empty. */
	unsigned long *rwarm;                   /* reservation: free, resident */
	unsigned long *rcold;                   /* reservation: free, punched out */
	unsigned int rslots;                    /* bits in each bitmap */
	/* S150 C: the region zone's free set, above rtop.  Its own maps: a
	 * chunk carve takes the lowest free bit of rwarm, and a region slot
	 * in that map would put records above rtop, in a group with index
	 * tables, which the give-back relies on never happening.  A region
	 * slot has no chunk header, so its quiet time is kept beside it. */
	unsigned long *zwarm;                   /* region zone: free, resident */
	unsigned long *zcold;                   /* region zone: free, punched out */
	unsigned int *zfree_at;                 /* tick the slot was given back */
	unsigned int nzfree_warm, nzfree_cold;
	unsigned long zregions_retired;         /* cumulative slots given back */
	unsigned long zregion_reuse;            /* cumulative takes from the set */
	unsigned int nfree_warm, nfree_cold;    /* reservation + pages / reservation */
	pcache_page_t *pages;
	unsigned int npages;
	unsigned int slots_total;               /* reservation slots cut + page slots */
	unsigned int chunks_used;               /* slots holding a chunk */
	unsigned int tick;                      /* reclaim ticks (1/s) */
	unsigned int carve_tick;                /* tick of the last carve */
	unsigned int giveback_off;              /* give-back refused: stay resident */
	unsigned long chunks_retired;           /* cumulative */
	unsigned long flush_broadcasts;         /* "send your hoard home" rounds */
	unsigned int flush_tick;                /* tick of the last broadcast */
	unsigned long pages_freed;              /* cumulative */
	unsigned long released_bytes;           /* cumulative give-back */
	unsigned long cold_bytes;               /* currently punched out */
	unsigned long punch_calls;              /* S96: punches issued, one per run */
	unsigned long punch_groups;             /* S96: groups those punches covered */
	/* S119: the never-carved tail of the reservation, punched once */
	unsigned long tail_next;                /* next tail offset to punch (bytes) */
	unsigned long ahead_from;               /* S177: lowest offset the
	                                         * commit-ahead window holds,
	                                         * so the tail walk can be
	                                         * re-armed over it when the
	                                         * window stops being wanted */
	unsigned int  tail_done;                /* the whole tail has been punched */
	unsigned long tail_released;            /* cumulative tail bytes punched */

	/* CP-20 huge-page reservation: a never-unmapped, PMD-aligned private
	 * anon region (PORTED S3); slots bump from it at hoff, regions too */
	char                 *hbase;
	unsigned long         hsize;            /* the VA reservation: the cap (S97) */
	unsigned long        *hcommit;          /* S97: one bit per 2 MB group, set
	                                         * while the group is committed */
	unsigned long         committed;        /* S97: bytes committed now */
	unsigned int          pinned;           /* S97: mlock held at init: pin
	                                         * every group committed later */
	unsigned int          pin_failed;       /* said so once */
	unsigned long         hoff;             /* chunk frontier, grows UP (lock) */
	unsigned long         rtop;             /* region frontier, grows DOWN: the
	                                         * index tables never share a 2 MB
	                                         * group with chunks, so groups of
	                                         * retired chunks can be punched out */
	enum pcache_mem_tier  htier;
	unsigned long         hlocked_mb;
	/* S166: groups committed ahead of the frontier by the reclaim tick,
	 * and groups a carve had to commit itself (the write path paying) */
	unsigned long         commits_ahead;
	unsigned long         commits_inline;
	unsigned long         commits_index;   /* S168: the region path's own */
} pcache_arena_t;

/* per-process allocation state - pkg, lazily created, reset on fork */
struct pcache_palloc {
	struct {
		char *bump;                  /* next unused cell in own chunk */
		unsigned int left;
		void *free_head;             /* private free stack */
		unsigned int nfree;
	} cls[PCACHE_NCLASSES];
};

static pcache_arena_t *arena;                  /* shm, set pre-fork */
static __thread struct pcache_palloc *my_palloc;  /* per THREAD (was: per process) */
/* idx = ceil(size/32), so it spans the ceiling: a literal here is how a
 * raised PCACHE_CELL_MAX would read past the end of it rather than fail
 * a test (S174) */
#define PCACHE_S2C_N      (PCACHE_CELL_MAX / 32 + 1)
static unsigned char size2class[PCACHE_S2C_N];

/* free-list link: bytes 8..15, never byte 0 (the class id) */
static inline void *cell_next(void *cell)
{
	return *(void **)((char *)cell + 8);
}

static inline void cell_set_next(void *cell, void *next)
{
	*(void **)((char *)cell + 8) = next;
}

static inline pcache_chunk_t *cell_chunk(const void *cell)
{
	return (pcache_chunk_t *)((unsigned long)cell & PCACHE_SLOT_MASK);
}

/*
 * RV-3: WHERE EVERY CELL IS, as something that can fail.
 *
 * The lock-free pop below has no ABA "because a cell cannot be pushed
 * twice while it is on the list" - an argument, and nothing checked it.
 * A double free would do exactly that, and so would a private stack
 * that handed the same cell over twice; either ends as two users of one
 * cell, found much later and somewhere else.
 *
 * The state is NOT a bit in the cell: a cell's bytes belong to its user
 * while it is out (byte 1 is a record's flags, but overflow nodes are
 * cells too), so nothing inside it can be trusted to read "clear" at
 * free time.  It is a byte per cell in a side table hung off the chunk
 * header's spare 8 bytes - exact whatever the cell holds.  Each move is
 * made by the cell's owner at that moment (the freeing thread, the
 * thread that just won the pop), so plain stores are enough.
 * Debug builds only; a release build compiles to the code it had.
 */
#ifdef PCACHE_ARENA_DEBUG
enum { DBG_OUT = 0, DBG_PRIVATE = 1, DBG_HOME = 2, DBG_SEEN = 0x80 };
#define DBG_M(st) (1u << (st))
int pcache_arena_dbg_abort = 1;
static unsigned long dbg_violations;

unsigned long pcache_arena_dbg_violations(void)
{
	return __atomic_load_n(&dbg_violations, __ATOMIC_RELAXED);
}

static const char *dbg_name(unsigned st)
{
	st &= ~(unsigned)DBG_SEEN;
	return st == DBG_OUT ? "out" : st == DBG_PRIVATE ? "on a private stack"
		: st == DBG_HOME ? "home on its chunk's list" : "?";
}

static unsigned char *dbg_slot(const void *cell)
{
	pcache_chunk_t *ch = cell_chunk(cell);

	if (!ch->dbg_state || ch->cls >= PCACHE_NCLASSES)
		return NULL;
	return &ch->dbg_state[(unsigned long)((const char *)cell -
		((const char *)ch + PCACHE_CHUNK_HDR)) / ch->cell_size];
}

/* the cell moves to @to; it must have been in one of @from */
static void dbg_move(const void *cell, unsigned from, unsigned to,
		const char *what)
{
	unsigned char *s = dbg_slot(cell);

	if (!s)
		return;
	if (!(DBG_M(*s & ~DBG_SEEN) & from)) {
		LM_CRIT("arena invariant: cell %p %s while it is %s - %s\n",
			cell, what, dbg_name(*s),
			*s == DBG_HOME && to == DBG_HOME
			? "pushed home TWICE, the case the lock-free pop's "
			  "no-ABA argument rules out"
			: "a double free, or a list handing one cell over twice");
		__atomic_add_fetch(&dbg_violations, 1, __ATOMIC_RELAXED);
		if (pcache_arena_dbg_abort)
			abort();
	}
	*s = (unsigned char)to;
}

/* every thread's private allocator state, so the census can walk the
 * private stacks: pushed once per thread, never removed (a thread that
 * exited still hoards the cells on its stack - they are somewhere) */
struct dbg_pl {
	struct pcache_palloc *pl;
	struct dbg_pl *next;
};
static struct dbg_pl *dbg_pls;

static void dbg_register(struct pcache_palloc *pl)
{
	struct dbg_pl *n = malloc(sizeof *n);

	if (!n)
		return;
	n->pl = pl;
	do {
		n->next = __atomic_load_n(&dbg_pls, __ATOMIC_RELAXED);
	} while (!__atomic_compare_exchange_n(&dbg_pls, &n->next, n, 0,
	                                      __ATOMIC_RELEASE, __ATOMIC_RELAXED));
}
#else
#define dbg_move(cell, from, to, what) ((void)0)
#define dbg_register(pl) ((void)0)
#endif

/*
 * A cell goes home: onto its chunk's free list.  Lock-free, any number of
 * pushers; the only popper is the allocator under the arena lock, so the
 * pop side has no ABA (a cell cannot be pushed twice while it is on the
 * list - RV-3: checked, in a debug build).  The chunk is announced to the
 * class the first time its count leaves zero - one membership flag keeps
 * it on the avail stack once.
 */
static void cell_home(void *cell)
{
	pcache_chunk_t *ch = cell_chunk(cell);
	unsigned int c = ch->cls;
	void *old;
	pcache_chunk_t *top;

	dbg_move(cell, DBG_M(DBG_OUT) | DBG_M(DBG_PRIVATE), DBG_HOME,
		"sent home");
	do {
		old = __atomic_load_n(&ch->free_head, __ATOMIC_RELAXED);
		cell_set_next(cell, old);
	} while (!__atomic_compare_exchange_n(&ch->free_head, &old, cell, 0,
	                                      __ATOMIC_RELEASE, __ATOMIC_RELAXED));

	if (__atomic_add_fetch(&ch->nfree, 1, __ATOMIC_ACQ_REL) != 1)
		return;
	__atomic_store_n(&ch->home_since, arena->tick, __ATOMIC_RELAXED);
	old = NULL;
	if (!__atomic_compare_exchange_n(&ch->in_avail, (unsigned int *)&old,
	        1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
		return;
	do {
		top = __atomic_load_n(&arena->avail[c], __ATOMIC_RELAXED);
		ch->link = top;
	} while (!__atomic_compare_exchange_n(&arena->avail[c], &top, ch, 0,
	                                      __ATOMIC_RELEASE, __ATOMIC_RELAXED));
}

/* pop one cell from a chunk - arena lock held (single popper) */
static void *chunk_pop(pcache_chunk_t *ch)
{
	void *old, *nxt;

	do {
		old = __atomic_load_n(&ch->free_head, __ATOMIC_ACQUIRE);
		if (!old)
			return NULL;
		nxt = cell_next(old);
	} while (!__atomic_compare_exchange_n(&ch->free_head, &old, nxt, 0,
	                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
	__atomic_sub_fetch(&ch->nfree, 1, __ATOMIC_RELAXED);
	dbg_move(old, DBG_M(DBG_HOME), DBG_PRIVATE, "popped from home");
	return old;
}

/* pop a chunk from the class avail stack - arena lock held */
static pcache_chunk_t *avail_pop(int c)
{
	pcache_chunk_t *top, *nxt;

	do {
		top = __atomic_load_n(&arena->avail[c], __ATOMIC_ACQUIRE);
		if (!top)
			return NULL;
		nxt = top->link;
	} while (!__atomic_compare_exchange_n(&arena->avail[c], &top, nxt, 0,
	                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
	__atomic_store_n(&top->in_avail, 0, __ATOMIC_RELEASE);
	return top;
}

static void avail_push(int c, pcache_chunk_t *ch)
{
	pcache_chunk_t *top;

	do {
		top = __atomic_load_n(&arena->avail[c], __ATOMIC_RELAXED);
		ch->link = top;
	} while (!__atomic_compare_exchange_n(&arena->avail[c], &top, ch, 0,
	                                      __ATOMIC_RELEASE, __ATOMIC_RELAXED));
}

static inline void extents_add(unsigned long lo, unsigned long hi)
{
	if (lo < arena->lo)
		arena->lo = lo;
	if (hi > arena->hi)
		arena->hi = hi;
}

#define BITS_PER_LONG   (8 * sizeof(unsigned long))

static inline void bit_set(unsigned long *m, unsigned int i)
{
	m[i / BITS_PER_LONG] |= 1UL << (i % BITS_PER_LONG);
}

static inline void bit_clr(unsigned long *m, unsigned int i)
{
	m[i / BITS_PER_LONG] &= ~(1UL << (i % BITS_PER_LONG));
}

static inline int bit_get(const unsigned long *m, unsigned int i)
{
	return (m[i / BITS_PER_LONG] >> (i % BITS_PER_LONG)) & 1;
}

/* the lowest set bit below @n, or -1 */
static int bit_first(const unsigned long *m, unsigned int n)
{
	unsigned int w, words = (n + BITS_PER_LONG - 1) / BITS_PER_LONG;

	for (w = 0; w < words; w++)
		if (m[w])
			return w * BITS_PER_LONG + __builtin_ctzl(m[w]);
	return -1;
}

static inline unsigned int rslot_idx(const pcache_chunk_t *ch)
{
	return ((char *)ch - arena->hbase) >> PCACHE_SLOT_SHIFT;
}

static inline pcache_chunk_t *rslot_at(unsigned int i)
{
	return (pcache_chunk_t *)(arena->hbase + (unsigned long)i * PCACHE_SLOT);
}

/* a new shm page: PCACHE_PAGE_SLOTS aligned slots on its free list.
 * Arena lock held. */
static int page_add(void)
{
	pcache_page_t *pg;
	pcache_chunk_t *ch;
	unsigned long size = (PCACHE_PAGE_SLOTS + 1) * PCACHE_SLOT;
	unsigned int i;

	pg = shm_malloc(sizeof *pg);
	if (!pg) {
		LM_ERR("no more shm memory for a page descriptor\n");
		return -1;
	}
	pg->raw = shm_malloc(size);
	if (!pg->raw) {
		LM_ERR("no more shm memory for a %lu byte page\n", size);
		shm_free(pg);
		return -1;
	}
	pg->base = (char *)(((unsigned long)pg->raw + PCACHE_SLOT - 1)
	                    & PCACHE_SLOT_MASK);
	pg->nslots = PCACHE_PAGE_SLOTS;
	pg->nfree = PCACHE_PAGE_SLOTS;
	pg->next = arena->pages;
	arena->pages = pg;
	arena->npages++;
	arena->bytes += size;
	extents_add((unsigned long)pg->raw, (unsigned long)pg->raw + size);

	pg->free = NULL;
	for (i = 0; i < pg->nslots; i++) {
		ch = (pcache_chunk_t *)(pg->base + (unsigned long)i * PCACHE_SLOT);
		memset(ch, 0, PCACHE_CHUNK_HDR);
		ch->page = pg;
		ch->cls = PCACHE_CLS_FREE;
		ch->link = pg->free;
		pg->free = ch;
	}
	arena->nfree_warm += pg->nslots;
	arena->slots_total += pg->nslots;
	return 0;
}

/* S94: bring a punched-out group back BEFORE anyone writes to it.
 *
 * A punched hugetlb page went back to the kernel's pool.  If the pool is
 * empty when the page is touched again there is no ENOMEM on that path:
 * the fault is a SIGBUS and the daemon dies.  The other tiers re-fault
 * ordinary anonymous memory and cannot die this way, so they are left
 * to the first write as before.  MADV_POPULATE_WRITE (Linux 5.14)
 * prefaults the group and answers EFAULT exactly where a touch would
 * have killed us; a kernel without it answers EINVAL, and then the
 * pool's own free count is consulted instead - a race against an empty
 * pool rather than a blind fault, and only on kernels that old.
 * 0 = secured, -1 = not (counted, logged; the caller carves elsewhere). */
static int group_secure(unsigned int g)
{
	char *p = arena->hbase + (unsigned long)g * PCACHE_HPS;
	char path[128], buf[32];
	long freep = -1;
	unsigned long n;
	int fd, r;

	if (arena->htier != PCACHE_MEM_HUGETLB)
		return 0;
	if (madvise(p, PCACHE_HPS, MADV_POPULATE_WRITE) == 0)
		return 0;
	if (errno == EINVAL) {
		snprintf(path, sizeof path,
			"/sys/kernel/mm/hugepages/hugepages-%lukB/free_hugepages",
			pcache_mem.hugetlb_kb);
		fd = open(path, O_RDONLY);
		if (fd >= 0) {
			r = (int)read(fd, buf, sizeof buf - 1);
			close(fd);
			if (r > 0) {
				buf[r] = 0;
				freep = atol(buf);
			}
		}
		if (freep >= 1) {
			/* the touch IS the commit, done here so the caller's
			 * bookkeeping describes memory that is really back */
			*(volatile char *)p = 0;
			return 0;
		}
	}
	n = ++arena_pool_empty;
	if (n == 1 || arena->tick - arena_pool_log_tick >= 10) {
		arena_pool_log_tick = arena->tick;
		LM_ERR("hugetlb pool empty: a punched-out 2 MB group cannot be "
			"re-committed (%lu refused so far).  The write goes to the "
			"4K overflow or is refused at the ceiling - it is NOT "
			"faulted blind, that would be a SIGBUS.  Raise "
			"vm.nr_hugepages or stop the pool's other consumer.\n", n);
	}
	return -1;
}

/* S97: commit group @g of the reservation if it is not - populated
 * (secured against an empty hugetlb pool by group_secure's answer) and
 * pinned when the arena is.  Idempotent; 0 = committed, -1 = refused. */
static int group_commit(unsigned int g)
{
	char *p = arena->hbase + (unsigned long)g * PCACHE_HPS;
	int rc;

	if (bit_get(arena->hcommit, g))
		return 0;
	if (group_secure(g) < 0)
		return -1;
	if (arena->htier != PCACHE_MEM_HUGETLB) {
		rc = pcache_mem_commit(p, PCACHE_HPS, arena->pinned);
		if (rc < 0)
			return -1;
		if (rc > 0 && !arena->pin_failed) {
			arena->pin_failed = 1;
			LM_WARN("mlock of a 2 MB arena group failed (%s): the "
				"groups committed from now on are unpinned\n",
				strerror(errno));
		}
		if (rc > 0)
			arena->pinned = 0;
	}
	bit_set(arena->hcommit, g);
	arena->committed += PCACHE_HPS;
	if (arena->pinned)
		arena->hlocked_mb = arena->committed >> 20;
	return 0;
}

/* S166: the carve's own commit - the same call, counted when it finds a
 * group the tick had not already committed, because that is the write
 * path populating 2 MB with a client waiting on it. */
static int carve_commit(unsigned int g)
{
	int fresh = !bit_get(arena->hcommit, g);
	int rc = group_commit(g);

	if (rc == 0 && fresh)
		arena->commits_inline++;
	return rc;
}

/*
 * S177: is the window above the frontier WANTED right now?
 *
 * It is committed for a carve that is coming, and this file already has
 * a notion of "coming": the cooloff the give-back waits out after the
 * last carve.  Past it the node is idle, nothing is approaching the
 * frontier, and the groups are exactly the resident tail S119 exists to
 * return - measured, 10 MB of RSS above `held` on an idle node, which
 * is what tailtest caught on the rc10 tag (in both `check` and
 * `check-asan`, and on rc9 before it, unnoticed).
 *
 * Two ticks is the floor because a carve lands BETWEEN ticks: at a
 * cooloff of 1 a workload carving continuously still reads as one tick
 * old, and a window that closed on it would put the populate back on
 * the write path - which is precisely what S166 removed.
 *
 * BOTH the commit and the tail walk's exclusion ask this one question,
 * or they would fight: with a short cooloff the give-back runs while
 * the window is still wanted, and one would punch what the other had
 * just committed, every tick.
 */
static int ahead_wanted(void)
{
	unsigned int win = (unsigned int)pcache_reclaim_cooloff_s;

	if (win < 2)
		win = 2;
	return arena->tick - arena->carve_tick <= win;
}

/* S166: keep PCACHE_AHEAD_GROUPS groups committed above the chunk
 * frontier, so a carve crossing a boundary finds the pages there.  The
 * reclaim tick calls it, 1/s, off the write path and under the arena
 * lock like everything else in that tick.  It stops at the region
 * frontier (the index zone grows down toward it) and at the first
 * refusal - a refused commit is not fatal: the carve falls back to the
 * pages below, exactly as it does past the reservation. */
static void commit_ahead(void)
{
	unsigned long off;
	unsigned int g, n;

	if (!arena->hbase)
		return;                        /* no reservation: the core allocates */
	/* NOT hugetlb.  Its groups come from a finite pool shared with the
	 * rest of the host, and securing one is group_secure's job - it
	 * counts and logs a pool that cannot answer.  Taking 8 MB of that
	 * pool on the chance a carve wants it would starve a neighbour and
	 * would move pool_empty for memory nobody asked for yet.  A hugetlb
	 * arena is also normally pinned, and a pinned arena never gives
	 * back, so after its first pass the groups are already committed. */
	if (arena->htier == PCACHE_MEM_HUGETLB)
		return;
	if (!ahead_wanted())
		return;
	off = arena->hoff / PCACHE_HPS * PCACHE_HPS;
	for (n = 0; n <= PCACHE_AHEAD_GROUPS; n++, off += PCACHE_HPS) {
		if (off + PCACHE_HPS > arena->rtop)
			break;                 /* the region zone's side */
		g = (unsigned int)(off / PCACHE_HPS);
		/* S177: remember where the window is, committed here or NOT.
		 * At start the daemon populates the whole reservation, so
		 * these groups are already committed and this function does
		 * nothing - but the tail walk is still excluding them, and
		 * when they stop being wanted something has to send it back
		 * over them.  Recording the extent on the wanted path rather
		 * than the commit path is what makes the start case work:
		 * without it an idle node kept 10 MB resident for ever, with
		 * held at 1.25 MB, which is exactly what tailtest asserts
		 * against. */
		if (!arena->ahead_from || off < arena->ahead_from)
			arena->ahead_from = off;
		if (bit_get(arena->hcommit, g))
			continue;              /* already there */
		if (group_commit(g) != 0)
			break;
		arena->commits_ahead++;
	}
	/* S168, WITHDRAWN 2026-09-20: a window below rtop as well.  It
	 * bought nothing and cost two things.  The index zone is committed
	 * by a table split on the MAINTENANCE thread - the same thread this
	 * tick runs on - so the window moved a 2 ms populate from the growth
	 * slice to the reclaim tick, one second apart, and paid for it with
	 * 8 MB held for an index that may never grow.  It also broke what
	 * regiontest asserts: that punching a retired group drops process
	 * RSS by that group, since the tick was re-committing while the
	 * give-back returned.  GitHub's runners caught it on rc8.  The
	 * region path is COUNTED instead (commits_index), so the cost is
	 * visible where it lands. */
}

/* S97: every group touching [lo, hi) committed; a refusal leaves the
 * ones before it committed (harmless: they are the reservation's) */
/* S168: the index zone's commit, counted like the carve's.  This one
 * runs on whoever allocates the region - the maintenance thread during
 * a split (pcache_ht_grow_at), or a worker creating a collection - so a
 * 2 MB populate here does not stall a client's write, but it does spend
 * the growth slice that the expiry sweep and the reclaim tick queue
 * behind (S131). */
static int groups_commit(unsigned long lo, unsigned long hi)
{
	unsigned int g;

	for (g = (unsigned int)(lo / PCACHE_HPS); g * (unsigned long)PCACHE_HPS < hi; g++) {
		int fresh = !bit_get(arena->hcommit, g);

		if (group_commit(g) < 0)
			return -1;
		if (fresh)
			arena->commits_index++;
	}
	return 0;
}

/* S94: a cold group comes back WHOLE.  A hugetlb page is 2 MB, so the
 * first slot faulted brings all eight back; counting one and leaving
 * seven "cold" was the old accounting's lie.  Secured first; then every
 * cold slot in it goes warm with a rebuilt header (the punch zeroed
 * them).  -1 = refused, nothing changed. */
static int group_recommit(unsigned int g)
{
	pcache_chunk_t *ch;
	unsigned int i, s;

	if (group_commit(g) < 0)
		return -1;
	for (i = 0; i < PCACHE_GROUP_SLOTS; i++) {
		s = g * PCACHE_GROUP_SLOTS + i;
		if (!bit_get(arena->rcold, s))
			continue;
		bit_clr(arena->rcold, s);
		bit_set(arena->rwarm, s);
		arena->nfree_cold--;
		arena->nfree_warm++;
		arena->cold_bytes -= PCACHE_SLOT;
		arena->bytes += PCACHE_SLOT;
		ch = rslot_at(s);
		memset(ch, 0, PCACHE_CHUNK_HDR);
		ch->cls = PCACHE_CLS_FREE;
		ch->cold = 0;
		ch->free_at = arena->tick;
	}
	return 0;
}

/* a free slot for a new chunk, in packing order: the reservation's lowest
 * resident free slot, then its lowest punched-out one (re-faults, keeps
 * the used range tight), then its frontier, then the fullest page, then a
 * new page.  Arena lock held. */
static pcache_chunk_t *slot_take(void)
{
	pcache_chunk_t *ch = NULL;
	pcache_page_t *pg, *best = NULL;
	int i;

	/* the (i = first()) >= 0 claim-and-test idiom, parenthesized */
	/* NOLINTNEXTLINE(bugprone-assignment-in-if-condition) */
	if (arena->hbase && (i = bit_first(arena->rwarm, arena->rslots)) >= 0) {
		bit_clr(arena->rwarm, i);
		arena->nfree_warm--;
		ch = rslot_at(i);
	}
	/* S94: a punched-out slot comes back a whole group at a time, and
	 * only once the memory is secured - group_recommit().  A refusal is
	 * not the end of the road: the frontier and the pages below still
	 * serve, exactly as they do past the reservation. */
	/* NOLINTNEXTLINE(bugprone-assignment-in-if-condition) */
	if (!ch && arena->hbase && (i = bit_first(arena->rcold, arena->rslots)) >= 0 &&
	    group_recommit((unsigned int)i / PCACHE_GROUP_SLOTS) == 0) {
		bit_clr(arena->rwarm, i);
		arena->nfree_warm--;
		ch = rslot_at(i);
	}
	if (!ch && arena->hbase && arena->hoff + PCACHE_SLOT <= arena->rtop &&
	    carve_commit((unsigned int)(arena->hoff / PCACHE_HPS)) == 0) {   /* S97 */
		ch = (pcache_chunk_t *)(arena->hbase + arena->hoff);
		arena->hoff += PCACHE_SLOT;
		arena->slots_total++;
		arena->bytes += PCACHE_SLOT;
		memset(ch, 0, PCACHE_CHUNK_HDR);
	}
	if (!ch) {
		for (pg = arena->pages; pg; pg = pg->next)
			if (pg->nfree && (!best || pg->nfree < best->nfree))
				best = pg;
		if (!best) {
			unsigned long need =
				(PCACHE_PAGE_SLOTS + 1) * PCACHE_SLOT;

			/* the configured ceiling.  Refusing is the correct
			 * failure: the caller turns a NULL here into
			 * "arena full - write dropped", which reaches the
			 * client as `cache full`.  Growing past it silently
			 * is what made arena_mb meaningless. */
			if (pcache_arena_max_bytes &&
			        arena->bytes + need > pcache_arena_max_bytes) {
				unsigned long n = ++arena_refused;

				if (!arena_ceiling_since)
					arena_ceiling_since = (unsigned long)time(NULL);

				/* the arena's own 1 Hz tick, not a clock:
				 * this is the allocator's hot path under
				 * exactly the pressure being reported */
				if (n == 1 || arena->tick -
				        arena_refuse_log_tick >= 10) {
					arena_refuse_log_tick = arena->tick;
					LM_ERR("arena at its configured ceiling "
						"(%lu MB held, max %lu MB) - %lu "
						"write(s) refused so far.  Raise "
						"[memory] arena_mb, or arena_cap_mb "
						"to let it grow past the huge-page "
						"reservation.\n",
						arena->bytes >> 20,
						pcache_arena_max_bytes >> 20, n);
				}
				return NULL;
			}
			if (page_add() < 0)
				return NULL;
			best = arena->pages;
		}
		ch = best->free;
		best->free = ch->link;
		best->nfree--;
		arena->nfree_warm--;
	}
	arena->chunks_used++;
	arena->carve_tick = arena->tick;
	arena_ceiling_since = 0;           /* S98: a carve succeeded - off it */
	return ch;
}

/* cut a chunk for class @c and hand it to the carving process as its bump
 * source - arena lock held.  The class byte of every cell is stamped HERE,
 * before the chunk is reachable by anyone - immutable from birth, so a
 * stale reader can always trust it (DESIGN 3.2 copy-out rule 1). */
static int carve_chunk(int c, struct pcache_palloc *pl)
{
	pcache_chunk_t *ch;
	unsigned int i;
	char *cells;

	ch = slot_take();
	if (!ch) {
		LM_ERR("no more memory for a chunk (class %d)\n", c);
		return -1;
	}
	ch->link = NULL;
	ch->free_head = NULL;
	ch->cls = c;
	ch->cell_size = cell_sizes[c];
	ch->cells = (PCACHE_SLOT - PCACHE_CHUNK_HDR) / cell_sizes[c];
	ch->nfree = 0;
	ch->in_avail = 0;
	ch->free_at = 0;
	ch->home_since = 0;
#ifdef PCACHE_ARENA_DEBUG
	ch->dbg_state = calloc(1, ch->cells);      /* every cell starts out */
#endif

	cells = (char *)ch + PCACHE_CHUNK_HDR;
	for (i = 0; i < ch->cells; i++)
		cells[(unsigned long)i * cell_sizes[c]] = (unsigned char)c;

	arena->chunks_cls[c]++;
	if (arena->chunks_cls[c] > arena->chunks_peak_cls[c])
		arena->chunks_peak_cls[c] = arena->chunks_cls[c];

	/* the whole chunk belongs to the carving process */
	pl->cls[c].bump = cells;
	pl->cls[c].left = ch->cells;

	LM_DBG("class %d: chunk %p, %u cells of %u\n", c, ch, ch->cells,
		cell_sizes[c]);
	return 0;
}

static struct pcache_palloc *get_palloc(void)
{
	if (!my_palloc) {
		my_palloc = pkg_malloc(sizeof *my_palloc);
		if (!my_palloc) {
			LM_ERR("no more pkg memory\n");
			return NULL;
		}
		memset(my_palloc, 0, sizeof *my_palloc);
		dbg_register(my_palloc);
	}
	return my_palloc;
}

/* send this process's bump remainder of class @c home */
static void bump_home(struct pcache_palloc *pl, int c)
{
	while (pl->cls[c].left) {
		cell_home(pl->cls[c].bump);
		pl->cls[c].bump += cell_sizes[c];
		pl->cls[c].left--;
	}
	pl->cls[c].bump = NULL;
}

int pcache_arena_init(void)
{
	int idx, c;

	arena = shm_malloc(sizeof *arena);
	if (!arena) {
		LM_ERR("no more shm memory\n");
		return -1;
	}
	memset(arena, 0, sizeof *arena);
	arena->lo = ~0UL;
	if (!lock_init(&arena->lock)) {
		LM_ERR("failed to init the arena lock\n");
		shm_free(arena);
		arena = NULL;
		return -1;
	}

	/* size -> class LUT, built pre-fork and inherited */
	for (idx = 0; idx < PCACHE_S2C_N; idx++) {
		for (c = 0; c < PCACHE_NCLASSES; c++)
			if (cell_sizes[c] >= (unsigned int)idx * 32)
				break;
		size2class[idx] = (unsigned char)c;   /* NCLASSES = impossible */
	}

	/*
	 * Which backing? An HG_MALLOC build can hand the whole job to HG: a
	 * dedicated arena becomes an HG arena of our own (own-hg), and with
	 * no arena asked for, cells go straight into the core shm arena when
	 * that allocator is HG (core). Only without HG does this file's
	 * slot allocator run (own). The policy modparam can force any.
	 */
	{
		const char *pol = pcache_backing_policy ? pcache_backing_policy : "auto";
		int want_arena = pcache_arena_hugepage_mb > 0;
		int try_own_hg = 0, try_core = 0;

		if (!strcasecmp(pol, "auto")) {
			try_own_hg = want_arena;
			try_core = !want_arena;
		} else if (!strcasecmp(pol, "own-hg")) {
			try_own_hg = 1;
		} else if (!strcasecmp(pol, "core")) {
			try_core = 1;
		} else if (strcasecmp(pol, "own")) {
			LM_ERR("memory_backing '%s' is not auto|core|own-hg|own\n", pol);
			return -1;
		}
		if (try_own_hg) {
			unsigned long init = (unsigned long)(pcache_arena_hugepage_mb > 0 ?
				pcache_arena_hugepage_mb : 64) << 20;
			unsigned long cap = (unsigned long)pcache_arena_hugepage_cap_mb << 20;

			hg_handle = shm_arena_create("cachedb_perf", init,
				cap > init ? cap : init);
			if (hg_handle) {
				backing = PCACHE_BACKING_OWN_HG;
				if (pcache_arena_profile &&
				    shm_arena_set_profile(hg_handle, pcache_arena_profile) < 0) {
					LM_ERR("arena_profile '%s' could not be attached\n",
						pcache_arena_profile);
					return -1;
				}
			} else if (strcasecmp(pol, "auto")) {
				LM_ERR("memory_backing=own-hg: no HG_MALLOC arena available "
					"(not an HG_MALLOC build, or the reservation failed)\n");
				return -1;
			}
		}
		if (backing == PCACHE_BACKING_OWN && try_core) {
			hg_handle = shm_arena_core();
			if (hg_handle) {
				backing = PCACHE_BACKING_CORE;
			} else if (strcasecmp(pol, "auto")) {
				LM_ERR("memory_backing=core: the shm allocator is not "
					"HG_MALLOC\n");
				return -1;
			}
		}
	}
	if (backing != PCACHE_BACKING_OWN) {
		LM_DBG("arena ready: %s\n", pcache_arena_backing_str());
		return 0;
	}

	if (pcache_reclaim_keep < 0 || pcache_reclaim_quiet_s < 0 ||
	    pcache_reclaim_cooloff_s < 0) {
		LM_ERR("reclaim_keep / reclaim_quiet_s / reclaim_cooloff_s must "
			"not be negative\n");
		return -1;
	}

	/* CP-20: reserve the huge-page arena, pre-fork, if requested */
	if (pcache_arena_hugepage_mb > 0) {
		/* S97: the reservation is the CAP (arena_cap_mb, else arena_mb)
		 * as untouched VA; arena_mb is the initial commit; the groups
		 * between commit by carve, up to the ceiling, all on the same
		 * tier - no 4K overflow inside the reservation */
		unsigned long commit = (unsigned long)pcache_arena_hugepage_mb << 20;

		arena->hsize = (unsigned long)pcache_arena_hugepage_cap_mb << 20;
		if (arena->hsize < commit)
			arena->hsize = commit;
		arena->hbase = pcache_mem_reserve(arena->hsize, commit, &arena->htier,
			&arena->hlocked_mb);
		if (!arena->hbase) {
			LM_WARN("huge-page arena reservation of %lu MB failed; "
				"falling back to shm_malloc (4K)\n",
				arena->hsize >> 20);
			arena->hsize = 0;
		} else {
			unsigned long ngroups = arena->hsize / PCACHE_HPS, g;

			arena->hcommit = shm_malloc(((ngroups + BITS_PER_LONG - 1)
				/ BITS_PER_LONG) * sizeof(unsigned long));
			if (!arena->hcommit) {
				LM_ERR("no more shm memory for the commit map\n");
				return -1;
			}
			memset(arena->hcommit, 0, ((ngroups + BITS_PER_LONG - 1)
				/ BITS_PER_LONG) * sizeof(unsigned long));
			for (g = 0; g < ngroups && g * (unsigned long)PCACHE_HPS < commit; g++)
				bit_set(arena->hcommit, (unsigned int)g);
			arena->committed = (commit + PCACHE_HPS - 1) / PCACHE_HPS * PCACHE_HPS;
			if (arena->committed > arena->hsize)
				arena->committed = arena->hsize;
			arena->pinned = arena->hlocked_mb > 0;
			arena->lo = (unsigned long)arena->hbase;
			arena->hi = (unsigned long)arena->hbase + arena->hsize;
			arena->rtop = arena->hsize;
			arena->rslots = arena->hsize >> PCACHE_SLOT_SHIFT;
			arena->rwarm = shm_malloc(2 * ((arena->rslots + BITS_PER_LONG - 1)
				/ BITS_PER_LONG) * sizeof(unsigned long));
			if (!arena->rwarm) {
				LM_ERR("no more shm memory for the slot bitmaps\n");
				return -1;
			}
			memset(arena->rwarm, 0, 2 * ((arena->rslots + BITS_PER_LONG - 1)
				/ BITS_PER_LONG) * sizeof(unsigned long));
			arena->rcold = arena->rwarm +
				(arena->rslots + BITS_PER_LONG - 1) / BITS_PER_LONG;
			/* S150 C: the region zone's maps and quiet stamps */
			arena->zwarm = shm_malloc(2 * ((arena->rslots + BITS_PER_LONG - 1)
				/ BITS_PER_LONG) * sizeof(unsigned long) +
				(unsigned long)arena->rslots * sizeof(unsigned int));
			if (!arena->zwarm) {
				LM_ERR("no more shm memory for the region maps\n");
				return -1;
			}
			memset(arena->zwarm, 0, 2 * ((arena->rslots + BITS_PER_LONG - 1)
				/ BITS_PER_LONG) * sizeof(unsigned long) +
				(unsigned long)arena->rslots * sizeof(unsigned int));
			arena->zcold = arena->zwarm +
				(arena->rslots + BITS_PER_LONG - 1) / BITS_PER_LONG;
			arena->zfree_at = (unsigned int *)(arena->zcold +
				(arena->rslots + BITS_PER_LONG - 1) / BITS_PER_LONG);
			LM_NOTICE("huge-page arena: %lu MB reserved on %s, %lu MB committed "
				"(%lu MB pinned from swapping); the rest commits by 2 MB group "
				"as records arrive\n",
				arena->hsize >> 20, pcache_mem_tier_str(arena->htier),
				arena->committed >> 20, arena->hlocked_mb);
		}
	}

	LM_DBG("arena ready: %d classes, %u B to %u B cells, %lu KB slots\n",
		PCACHE_NCLASSES, cell_sizes[0], cell_sizes[PCACHE_NCLASSES-1],
		PCACHE_SLOT >> 10);
	return 0;
}

/* S128: what a region of `size` costs the arena.  The reservation carve
 * takes whole slots, which is what the measured index figures show, so
 * that is the number a preflight must use.  S155: header-less, so a
 * segment's 256 KB of buckets is ONE slot; the shm fallback past the
 * reservation keeps its 16-byte header and charges size + 80 exactly,
 * a difference of bytes that its own backstop in pcache_region_alloc()
 * still tests. */
unsigned long pcache_arena_region_cost(unsigned long size)
{
	return ((size + PCACHE_SLOT - 1) >> PCACHE_SLOT_SHIFT) * PCACHE_SLOT;
}

/* S128: room under the ceiling for `bytes` more.  The caller is a whole
 * INDEX asking before it carves the first of its regions - see the note
 * in pcache_htable_new().  Refusing here is a create or a resize that
 * does not happen; carving anyway is a node past the memory figure its
 * operator sized, which is the defect this closes. */
static void ceiling_refused_log(unsigned long held, unsigned long bytes);

int pcache_arena_room_for(unsigned long bytes)
{
	unsigned long held;

	if (!arena || !pcache_arena_max_bytes)
		return 0;
	held = __atomic_load_n(&arena->bytes, __ATOMIC_RELAXED);
	/* S150 C: retired index slots kept resident are held, and an index
	 * takes them before it carves - so they are not more to hold */
	{
		unsigned long credit = (unsigned long)
			__atomic_load_n(&arena->nzfree_warm, __ATOMIC_RELAXED) * PCACHE_SLOT;

		if (bytes > credit)
			bytes -= credit;
		else
			bytes = 0;
	}
	if (held + bytes <= pcache_arena_max_bytes)
		return 0;
	ceiling_refused_log(held, bytes);
	return -1;
}

static void ceiling_refused_log(unsigned long held, unsigned long bytes)
{
	static unsigned long refused, log_tick;
	unsigned long n;

	/* Rate-limited, the same posture the chunk carve has and for the
	 * same reason: a create asks once, but the SPLITTER asks every
	 * maintenance tick for as long as a full table wants to grow, and
	 * an unlimited message there is a line a second for ever.  Say it
	 * on the first refusal and then at most every ten seconds, carrying
	 * the running count so the frequency is still readable.
	 *
	 * KB below a megabyte: a bucket segment is 256 KB, and "an index of
	 * 0 MB does not fit" is not a sentence anyone can act on. */
	n = ++refused;
	if (n == 1 || arena->tick - log_tick >= 10) {
		log_tick = arena->tick;
		if (bytes >= (1UL << 20))
			LM_ERR("an index of %lu MB does not fit under the "
				"arena's ceiling (%lu MB held, max %lu MB) - "
				"%lu refused so far.  Raise [memory] arena_mb, "
				"or arena_cap_mb to let it grow past the "
				"huge-page reservation.\n", bytes >> 20,
				held >> 20, pcache_arena_max_bytes >> 20, n);
		else
			LM_ERR("an index of %lu KB does not fit under the "
				"arena's ceiling (%lu MB held, max %lu MB) - "
				"%lu refused so far.  A table that cannot get "
				"a segment stops growing; raise [memory] "
				"arena_mb, or arena_cap_mb to let it grow past "
				"the huge-page reservation.\n", bytes >> 10,
				held >> 20, pcache_arena_max_bytes >> 20, n);
	}
}

/* S150 C: a punched-out region group comes back whole, like a chunk
 * group (group_recommit): every cold slot of it resident again, and
 * counted as held.  Only whole groups are ever punched, so a group with
 * one cold slot has eight. */
static int zgroup_recommit(unsigned int g)
{
	unsigned int i, s;

	if (group_commit(g) < 0)
		return -1;
	for (i = 0; i < PCACHE_GROUP_SLOTS; i++) {
		s = g * PCACHE_GROUP_SLOTS + i;
		if (!bit_get(arena->zcold, s))
			continue;
		bit_clr(arena->zcold, s);
		bit_set(arena->zwarm, s);
		arena->nzfree_cold--;
		arena->nzfree_warm++;
		arena->cold_bytes -= PCACHE_SLOT;
		arena->bytes += PCACHE_SLOT;
		arena->zfree_at[s] = arena->tick;
	}
	return 0;
}

unsigned long pcache_region_free(void *p, size_t size)
{
	unsigned long off, slots, i, n = 0;

	if (!arena || backing != PCACHE_BACKING_OWN || !arena->hbase ||
	    (char *)p < arena->hbase + arena->rtop ||
	    (char *)p >= arena->hbase + arena->hsize)
		return 0;                  /* shm or HG: left as it was */
	off = (unsigned long)((char *)p - arena->hbase);
	slots = (size + PCACHE_SLOT - 1) >> PCACHE_SLOT_SHIFT;
	lock_get(&arena->lock);
	for (i = off >> PCACHE_SLOT_SHIFT; i < (off >> PCACHE_SLOT_SHIFT) + slots &&
	     i < arena->rslots; i++) {
		if (bit_get(arena->zwarm, (unsigned int)i) ||
		    bit_get(arena->zcold, (unsigned int)i))
			continue;              /* given back twice: once is enough */
		bit_set(arena->zwarm, (unsigned int)i);
		arena->zfree_at[i] = arena->tick;
		arena->nzfree_warm++;
		arena->regions_bytes -= PCACHE_SLOT;
		arena->zregions_retired++;
		n += PCACHE_SLOT;
	}
	lock_release(&arena->lock);
	return n;
}

/*
 * Raw memory for an index region (bucket segments).  64-aligned, never
 * freed (task C10: tables grow and never shrink - small and bounded).
 * From the reservation's frontier in whole slots when it has room, shm
 * otherwise.
 *
 * S155: a reservation region carries NO header.  A bucket segment is
 * exactly one slot of buckets, and the 16-byte header that used to sit
 * in front of it pushed every segment into a second slot - 512 KB carved
 * for 256 KB used, on the largest thing the index is made of.  The
 * header was never read: size 0, not on the list (the list and the
 * destroy walk are the shm fallback's).  The payload is the slot
 * boundary, 64-aligned by construction.
 */
void *pcache_region_alloc(size_t size)
{
	pcache_region_t *rg;
	unsigned long need = size + sizeof(pcache_region_t) + 64, slots;
	char *aligned;

	if (backing != PCACHE_BACKING_OWN) {
		/* HG hands out whole regions too; nothing to register - the
		 * arena's extents and accounting are HG's */
		rg = mem_arena_malloc(hg_handle, need);
		if (!rg) {
			LM_ERR("no more arena memory for a %lu byte region\n", need);
			return NULL;
		}
		__atomic_fetch_add(&arena->bytes, need, __ATOMIC_RELAXED);
		return (char *)(((unsigned long)rg + sizeof(pcache_region_t) + 63)
		                & ~63UL);
	}

	lock_get(&arena->lock);
	slots = (size + PCACHE_SLOT - 1) >> PCACHE_SLOT_SHIFT;
	/* S150 C: a retired slot before the frontier - resident first, then
	 * a punched one (its whole group re-committed, as slot_take does).
	 * Every index region is one slot (S155); a larger one still carves. */
	if (slots == 1 && arena->hbase) {
		int i;

		/* NOLINTNEXTLINE(bugprone-assignment-in-if-condition) */
		if ((i = bit_first(arena->zwarm, arena->rslots)) < 0 &&
		    (i = bit_first(arena->zcold, arena->rslots)) >= 0 &&
		    zgroup_recommit((unsigned int)i / PCACHE_GROUP_SLOTS) < 0)
			i = -1;
		if (i >= 0) {
			bit_clr(arena->zwarm, i);
			arena->nzfree_warm--;
			arena->regions_bytes += PCACHE_SLOT;
			arena->zregion_reuse++;
			lock_release(&arena->lock);
			return arena->hbase + (unsigned long)i * PCACHE_SLOT;
		}
	}
	if (arena->hbase && arena->hoff + slots * PCACHE_SLOT <= arena->rtop &&
	    groups_commit(arena->rtop - slots * PCACHE_SLOT, arena->rtop) == 0) {   /* S97 */
		char *payload;

		arena->rtop -= slots * PCACHE_SLOT;
		payload = arena->hbase + arena->rtop;   /* S155: no header */
		arena->bytes += slots * PCACHE_SLOT;
		arena->regions_bytes += slots * PCACHE_SLOT;
		lock_release(&arena->lock);
		return payload;
	}
	lock_release(&arena->lock);

	/* S128: the configured ceiling, which this path did not test.  A
	 * chunk carve that leaves the reservation refuses here and the
	 * client is told the cache is full; a REGION carve fell through to
	 * shm_malloc with no test at all, so records were bounded and the
	 * tables holding them were not - one `create` put a node 21% past
	 * its arena_mb.  Whole indexes preflight themselves (see
	 * pcache_arena_room_for), so reaching this is a carve that raced
	 * the writers; refusing is still right.
	 *
	 * RV-1: and it is a RESERVATION, not a check.  The test used to be
	 * a relaxed read outside the lock, then shm_malloc, then the lock
	 * and the add - so two carves near the cap (a `create` on a worker,
	 * the splitter on the maintenance thread) both passed and both
	 * landed, past the arena_cap_mb an operator reads as a bound
	 * (capracetest: 8 threads, room for one - 200 of 300 rounds over,
	 * worst by 650 KB).  The bytes are taken under the lock BEFORE the
	 * allocation and given back if it fails, so a racing carve sees
	 * them.  Exact, too: the warm-slot credit room_for() gives a whole
	 * index does not apply here - this carve is already past the warm
	 * slots, every byte of it is new. */
	lock_get(&arena->lock);
	if (pcache_arena_max_bytes &&
	    arena->bytes + need > pcache_arena_max_bytes) {
		unsigned long held = arena->bytes;

		lock_release(&arena->lock);
		ceiling_refused_log(held, need);
		return NULL;
	}
	arena->bytes += need;
	lock_release(&arena->lock);

	rg = shm_malloc(need);
	if (!rg) {
		lock_get(&arena->lock);
		arena->bytes -= need;
		lock_release(&arena->lock);
		LM_ERR("no more shm memory for a %lu byte region\n", need);
		return NULL;
	}
	rg->size = need;
	aligned = (char *)(((unsigned long)rg + sizeof(pcache_region_t) + 63)
	                   & ~63UL);
	lock_get(&arena->lock);
	rg->next = arena->regions;
	arena->regions = rg;
	arena->regions_bytes += need;
	extents_add((unsigned long)rg, (unsigned long)rg + need);
	lock_release(&arena->lock);
	return aligned;
}

void pcache_arena_destroy(void)
{
	pcache_region_t *rg, *rnext;
	pcache_page_t *pg, *pnext;

	if (!arena)
		return;

	if (backing != PCACHE_BACKING_OWN) {
		/* the cells and regions are HG's; the arena mapping (ours or the
		 * core's) goes with the process */
		lock_destroy(&arena->lock);
		shm_free(arena);
		arena = NULL;
		return;
	}

	for (rg = arena->regions; rg; rg = rnext) {
		rnext = rg->next;
		shm_free(rg);
	}
	for (pg = arena->pages; pg; pg = pnext) {
		pnext = pg->next;
		shm_free(pg->raw);
		shm_free(pg);
	}
	if (arena->hbase)
		munmap(arena->hbase, arena->hsize);
	if (arena->rwarm)
		shm_free(arena->rwarm);
	lock_destroy(&arena->lock);
	shm_free(arena);
	arena = NULL;

	if (my_palloc) {
		pkg_free(my_palloc);
		my_palloc = NULL;
	}
}

void pcache_arena_child_init(void)
{
	struct pcache_palloc *pl = my_palloc;

	if (backing != PCACHE_BACKING_OWN || !pl)
		return;

	/*
	 * After fork every child holds a COW copy of the parent's private
	 * allocator state - the SAME bump pointer and the SAME free-list cell
	 * addresses.  A child must not keep them (two processes bumping one
	 * chunk would hand out the same cell), and it must NOT send them home
	 * either: every child inherited the identical copy, so each would push
	 * the same physical cells, landing one cell on a free list N times -
	 * later popped by several processes at once and written through
	 * concurrently (the CP-16 corruption: a value byte overwrites a
	 * neighbour's class id, and the next free reads an impossible class).
	 *
	 * The leftover cells belong to the parent.  The child simply discards
	 * its inherited copy and starts empty, carving its own chunk on first
	 * use.  The parent keeps its own small hoard.
	 *
	 * Bug fixed here (2026-08-07): this function's OWN comment already
	 * said "discards", but the code called pkg_free(pl) anyway - freeing
	 * pl (the pcache_palloc struct itself) is exactly the same class of
	 * mistake the comment warns about for its internal free-list cells:
	 * pl is COW-shared with the parent and every sibling child inherited
	 * the identical pointer, so pkg_free() is a WRITE into that shared
	 * page (hg_cell_free()/cell_set_next() links it into a free list).
	 * Under HG_MALLOC's hugepage-backed pkg arena this write-triggered
	 * COW fault reproducibly SIGBUSed (mem/hg_arena.c:98, always via
	 * cachedb_perf.c child_init -> here), first surfaced when a TCP-based
	 * protocol (proto_bin, for clusterer_controller) made this fork/free
	 * path run under HG_MALLOC for the first time. Fix: just drop the
	 * reference, exactly as documented - no free, no donation, nothing.
	 * pl's memory is reclaimed for free when the child process exits.
	 */
	my_palloc = NULL;
}

/* send every privately held cell of this process home: the free stacks
 * and the bump remainders.  For a process that is done allocating (and
 * for the selftest); the hot paths never call it. */
void pcache_arena_flush_private(void)
{
	struct pcache_palloc *pl = my_palloc;
	void *cell;
	int c;

	if (backing != PCACHE_BACKING_OWN || !pl)
		return;
	for (c = 0; c < PCACHE_NCLASSES; c++) {
		while ((cell = pl->cls[c].free_head) != NULL) {
			pl->cls[c].free_head = cell_next(cell);
			pl->cls[c].nfree--;
			cell_home(cell);
		}
		bump_home(pl, c);
	}
}

void *pcache_cell_alloc(unsigned int size)
{
	struct pcache_palloc *pl;
	pcache_chunk_t *ch;
	void *cell;
	unsigned int got;
	int c;

	if (size > PCACHE_CELL_MAX) {
		LM_DBG("%u bytes exceeds the largest cell (%d)\n",
			size, PCACHE_CELL_MAX);
		return NULL;
	}
	c = size2class[(size + 31) >> 5];

	if (backing != PCACHE_BACKING_OWN) {
		/* an HG slab cell, class-rounded so pcache_cell_bound() stays
		 * exact; byte 0 carries our class id exactly as in own chunks
		 * (HG's own header sits in front of the pointer it returns) */
		cell = mem_arena_malloc(hg_handle, cell_sizes[c]);
		if (!cell)
			return NULL;
		*(unsigned char *)cell = (unsigned char)c;
		__atomic_fetch_add(&arena->bytes, cell_sizes[c], __ATOMIC_RELAXED);
		__atomic_fetch_add(&arena->live, cell_sizes[c], __ATOMIC_RELAXED);
		return cell;
	}

	pl = get_palloc();
	if (!pl)
		return NULL;

	/* fast paths: no locks, no shared lines */
	cell = pl->cls[c].free_head;
	if (cell) {
		dbg_move(cell, DBG_M(DBG_PRIVATE), DBG_OUT, "allocated");
		pl->cls[c].free_head = cell_next(cell);
		pl->cls[c].nfree--;
		__atomic_fetch_add(&arena->live, cell_sizes[c],
			__ATOMIC_RELAXED);
		return cell;
	}
	if (pl->cls[c].left) {
		cell = pl->cls[c].bump;
		pl->cls[c].bump += cell_sizes[c];
		pl->cls[c].left--;
		__atomic_fetch_add(&arena->live, cell_sizes[c],
			__ATOMIC_RELAXED);
		return cell;
	}

	/* slow path: pull a batch home from the class's chunks, else carve */
	lock_get(&arena->lock);
	for (got = 0; got < PCACHE_REFILL_BATCH; ) {
		ch = arena->cur[c];
		if (!ch) {
			ch = avail_pop(c);
			if (!ch)
				break;
			arena->cur[c] = ch;
		}
		cell = chunk_pop(ch);
		if (!cell) {
			arena->cur[c] = NULL;     /* empty; the next free re-announces it */
			continue;
		}
		cell_set_next(cell, pl->cls[c].free_head);
		pl->cls[c].free_head = cell;
		pl->cls[c].nfree++;
		got++;
	}
	if (!got && carve_chunk(c, pl) < 0) {
		lock_release(&arena->lock);
		return NULL;
	}
	lock_release(&arena->lock);

	cell = pl->cls[c].free_head;
	if (cell) {
		dbg_move(cell, DBG_M(DBG_PRIVATE), DBG_OUT, "allocated");
		pl->cls[c].free_head = cell_next(cell);
		pl->cls[c].nfree--;
		__atomic_fetch_add(&arena->live, cell_sizes[c],
			__ATOMIC_RELAXED);
		return cell;
	}
	cell = pl->cls[c].bump;
	pl->cls[c].bump += cell_sizes[c];
	pl->cls[c].left--;
	__atomic_fetch_add(&arena->live, cell_sizes[c],
		__ATOMIC_RELAXED);
	return cell;
}

void pcache_cell_free(void *cell)
{
	struct pcache_palloc *pl;
	unsigned int c = *(unsigned char *)cell, i;
	void *d;

	if (c >= PCACHE_NCLASSES) {
		LM_CRIT("cell %p carries invalid class %u - leaking it\n",
			cell, c);
		return;
	}
	__atomic_fetch_sub(&arena->live, cell_sizes[c], __ATOMIC_RELAXED);

	if (backing != PCACHE_BACKING_OWN) {
		__atomic_fetch_sub(&arena->bytes, cell_sizes[c], __ATOMIC_RELAXED);
		mem_arena_free(hg_handle, cell);
		return;
	}

	/* RV-3: a free takes a cell that is OUT - anything else is a second
	 * free of it, caught here, in the thread that did it */
	dbg_move(cell, DBG_M(DBG_OUT), DBG_OUT, "freed");
	pl = get_palloc();
	if (!pl) {
		/* cannot even track it privately - send it home */
		cell_home(cell);
		return;
	}

	dbg_move(cell, DBG_M(DBG_OUT), DBG_PRIVATE, "freed");
	cell_set_next(cell, pl->cls[c].free_head);
	pl->cls[c].free_head = cell;
	pl->cls[c].nfree++;

	if (pl->cls[c].nfree > PCACHE_PRIVATE_MAX) {
		/* the surplus goes home, and so does an idle bump remainder -
		 * a process hoarding cells would otherwise pin their chunks */
		for (i = 0; i < PCACHE_DONATE; i++) {
			d = pl->cls[c].free_head;
			pl->cls[c].free_head = cell_next(d);
			pl->cls[c].nfree--;
			cell_home(d);
		}
		bump_home(pl, c);
	}
}

/* a free from a process that is not an allocator (the expiry sweep):
 * straight home, lock-free */
void pcache_cell_free_global(void *cell)
{
	unsigned int c = *(unsigned char *)cell;

	if (c >= PCACHE_NCLASSES) {
		LM_CRIT("cell %p carries invalid class %u - leaking it\n",
			cell, c);
		return;
	}
	__atomic_fetch_sub(&arena->live, cell_sizes[c], __ATOMIC_RELAXED);
	if (backing != PCACHE_BACKING_OWN) {
		__atomic_fetch_sub(&arena->bytes, cell_sizes[c], __ATOMIC_RELAXED);
		mem_arena_free(hg_handle, cell);
		return;
	}
	dbg_move(cell, DBG_M(DBG_OUT), DBG_OUT, "freed (global)");
	cell_home(cell);
}

/* the largest size this cell can hold, from its class byte; 0 = not a cell */
unsigned int pcache_cell_bound(const void *cell)
{
	unsigned char c = *(const unsigned char *)cell;

	if (c >= PCACHE_NCLASSES)
		return 0;
	return cell_sizes[c];
}

void pcache_arena_extents(unsigned long *lo, unsigned long *hi)
{
	if (backing != PCACHE_BACKING_OWN) {
		/* the HG reservation (the whole cap): every pointer HG ever
		 * hands out from this arena lies inside it */
		mem_arena_extents(hg_handle, lo, hi);
		return;
	}
	/* unlocked on purpose: readers validate with these on every lookup.
	 * The watermarks only ever widen (a page is never unmapped), so a
	 * torn pair can only be narrower than the truth - a live record read
	 * through a stale pair is just a miss that the next retry serves. */
	*lo = __atomic_load_n(&arena->lo, __ATOMIC_RELAXED);
	*hi = __atomic_load_n(&arena->hi, __ATOMIC_RELAXED);
}

/* ---- reclaim: the module's own process, one tick a second ------------ */

/* retire a drained chunk: its slot goes to the warm list - arena lock held */
static void chunk_retire(pcache_chunk_t *ch)
{
	unsigned int c = ch->cls;

	if (arena->cur[c] == ch)
		arena->cur[c] = NULL;
#ifdef PCACHE_ARENA_DEBUG
	free(ch->dbg_state);
	ch->dbg_state = NULL;
#endif
	ch->free_head = NULL;
	ch->nfree = 0;
	ch->in_avail = 0;
	ch->cls = PCACHE_CLS_FREE;
	ch->free_at = arena->tick;
	if (ch->page) {
		ch->link = ch->page->free;
		ch->page->free = ch;
		ch->page->nfree++;
	} else {
		bit_set(arena->rwarm, rslot_idx(ch));
	}
	arena->nfree_warm++;
	arena->chunks_cls[c]--;
	arena->chunks_used--;
	arena->chunks_retired++;
}

/* is every slot of [lo, hi) a free, resident, quiet one?  Arena lock held. */
static int range_quiet(char *lo, char *hi)
{
	pcache_chunk_t *ch;
	char *p;

	for (p = lo; p < hi; p += PCACHE_SLOT) {
		ch = (pcache_chunk_t *)p;
		if (ch->cls != PCACHE_CLS_FREE || ch->cold ||
		    arena->tick - ch->free_at < (unsigned int)pcache_reclaim_quiet_s)
			return 0;
	}
	return 1;
}

/* S47: true iff releasing `amount` more bytes keeps held >= the floor */
static int floor_allows(unsigned long amount)
{
	if (!pcache_arena_floor_bytes)
		return 1;
	return arena->bytes >= amount &&
		arena->bytes - amount >= pcache_arena_floor_bytes;
}

/* S100: the most give-back one tick may do, groups and pages together.
 * Unset, an eighth of the ceiling (of the reservation when there is no
 * ceiling), never below 8 MB - a cache that empties in a burst repays
 * the host over a few seconds rather than in one sweep, which is also
 * the worst moment for the carve path to find every group in flight.
 * A configured value below one page unit could never fit anything and
 * would silently disable give-back, so it is raised to that. */
static unsigned long shrink_step(void)
{
	unsigned long unit = (PCACHE_PAGE_SLOTS + 1) * PCACHE_SLOT;
	unsigned long base, step;

	if (pcache_arena_shrink_step_bytes)
		return pcache_arena_shrink_step_bytes > unit ?
			pcache_arena_shrink_step_bytes : unit;
	base = pcache_arena_max_bytes ? pcache_arena_max_bytes : arena->hsize;
	step = base / 8;
	return step > 4 * PCACHE_HPS ? step : 4 * PCACHE_HPS;
}

/* S96: a run of consecutive punchable groups, released in ONE madvise */
struct punch_run {
	unsigned int g0, n;
	int tail;                       /* S119: never-carved groups - no slot bookkeeping */
	int zone;                       /* S150 C: region-zone groups - the zone's maps */
};
#define PUNCH_RUNS_MAX 256              /* per tick; the rest waits a second */

/*
 * Give memory back to the host.  ENTERED with the arena lock held and
 * RETURNS with it held - and drops it in the middle (S95).
 *
 * The lock is the carve lock: every writer that needs a fresh cell
 * takes it.  It used to be held across one madvise per 2 MB group for
 * the whole reservation, so a drain that freed a few hundred MB stalled
 * every writer for the whole sweep.  Now:
 *
 *   phase 1, lock held    - DECIDE.  Scan, pick the groups and pages that
 *                           qualify, take them off the warm map.  A group
 *                           in flight is on NEITHER map, so slot_take()
 *                           cannot see it: nothing can take a slot the
 *                           punch is about to zero, and S94, which
 *                           re-commits through the cold map, cannot race
 *                           the punch on the same group.
 *   phase 2, lock DROPPED - the syscalls: one madvise per RUN of
 *                           consecutive groups (S96), then the page frees.
 *                           Writers carve meanwhile.
 *   phase 3, lock held    - the bookkeeping, for what actually happened.
 *
 * The never-unmap invariant is what makes phase 2 safe without the
 * lock: the mapping never changes, only residency, and the quiet window
 * has already excluded any reader still holding a pointer in.
 *
 * S101: a punched group's headers are NOT rebuilt.  They used to be,
 * "cold", right after the madvise - eight writes into the memory just
 * given back, which faulted it straight back in: the whole 2 MB on a
 * hugetlb page, and a THP where one was to be had.  cold_bytes said
 * cold, RSS said resident.  The cold map is the only truth for a cold
 * slot; its header is rebuilt when the group is secured and re-committed
 * (group_recommit), and nothing reads it before then.
 */
static void giveback_tick(void)
{
	struct punch_run runs[PUNCH_RUNS_MAX];
	unsigned int nruns = 0, done = 0, g, i, r, ngroups, keep_pages = 0;
	unsigned long planned = 0, step = shrink_step();
	pcache_page_t *pg, **ppg, *freed = NULL;
	char *p;
	int err = 0;

	/* S114: the spell at the ceiling ends when a carve WOULD succeed
	 * again, not only when one does.  After a drain nothing carves - the
	 * current chunks have room for what little is written - so the flag
	 * stood for ten hours beside half an arena of headroom (245-247,
	 * 2026-09-07).  Same predicate as the carve's refusal, lock held. */
	if (arena_ceiling_since && !(pcache_arena_max_bytes &&
	    arena->bytes + (PCACHE_PAGE_SLOTS + 1) * PCACHE_SLOT >
	    pcache_arena_max_bytes))
		arena_ceiling_since = 0;

	if (!pcache_reclaim_giveback ||
	    arena->tick - arena->carve_tick < (unsigned int)pcache_reclaim_cooloff_s)
		return;

	/* ---- phase 1: decide and mark, lock held ------------------------ */

	/* S47 shrink floor: never release below the floor.  `planned` is
	 * what this tick has already decided to give back, so the floor is
	 * tested against the bytes that WILL be held, not the bytes held
	 * now.  The giveback_off latch covers the reservation only: it is
	 * set when MADV_DONTNEED is refused there (a pinned arena - mlock
	 * and DONTNEED are exclusive), and the page phase, which returns
	 * malloc'd memory with free(), keeps working regardless. */
	if (arena->hbase && !arena->giveback_off) {
		ngroups = (unsigned int)(arena->hoff / PCACHE_HPS);
		for (g = 0; g < ngroups && nruns < PUNCH_RUNS_MAX; g++) {
			p = arena->hbase + (unsigned long)g * PCACHE_HPS;
			for (i = 0; i < PCACHE_GROUP_SLOTS; i++)
				if (!bit_get(arena->rwarm, g * PCACHE_GROUP_SLOTS + i))
					break;
			if (i < PCACHE_GROUP_SLOTS || !range_quiet(p, p + PCACHE_HPS))
				continue;
			if (planned + PCACHE_HPS > step)
				break;   /* S100: this tick's share is spent */
			if (!floor_allows(planned + PCACHE_HPS))
				break;   /* at the floor: stop punching groups */
			for (i = 0; i < PCACHE_GROUP_SLOTS; i++)
				bit_clr(arena->rwarm, g * PCACHE_GROUP_SLOTS + i);
			arena->nfree_warm -= PCACHE_GROUP_SLOTS;
			planned += PCACHE_HPS;
			if (nruns && !runs[nruns - 1].zone &&
			    runs[nruns - 1].g0 + runs[nruns - 1].n == g) {
				runs[nruns - 1].n++;         /* S96: extends the run */
			} else {
				runs[nruns].g0 = g;
				runs[nruns].n = 1;
				runs[nruns].tail = 0;
				runs[nruns].zone = 0;
				nruns++;
			}
		}
	}

	/* S119: the never-carved tail of the reservation.  The reservation is
	 * pinned and populated at start, so every byte of it is resident
	 * before a record arrives; chunks are carved upward from hoff and
	 * regions downward from rtop, and the loop above visits only the
	 * groups below hoff.  On a node that never needed its whole
	 * reservation the tail between the two frontiers stayed resident for
	 * the life of the process (245: 70 MB beside 102 MB held).  Whole
	 * groups between the frontiers are punched once, in the tick's
	 * share, under the same latch and cooloff as the cold groups; a
	 * later carve into them re-faults, as it does for a retired group.
	 * Never counted in `bytes`, so no floor test and no cold
	 * bookkeeping: only the release counters move. */
	/* S150 C: the region zone's free groups - a retired index, every
	 * slot of the group given back and quiet, past the cool-off like
	 * the rest, and only above a keep of one group so a table's next
	 * split takes a resident slot rather than re-faulting what it just
	 * gave.  Whole groups only: a group straddling rtop, or holding a
	 * live region, never qualifies. */
	if (arena->hbase && !arena->giveback_off &&
	    arena->nzfree_warm > 2 * PCACHE_GROUP_SLOTS) {
		unsigned int gn = (unsigned int)(arena->hsize / PCACHE_HPS);

		for (g = (unsigned int)(arena->rtop / PCACHE_HPS);
		     g < gn && nruns < PUNCH_RUNS_MAX &&
		     arena->nzfree_warm >= 2 * PCACHE_GROUP_SLOTS; g++) {
			for (i = 0; i < PCACHE_GROUP_SLOTS; i++) {
				unsigned int s = g * PCACHE_GROUP_SLOTS + i;

				if (!bit_get(arena->zwarm, s) ||
				    arena->tick - arena->zfree_at[s] <
				        (unsigned int)pcache_reclaim_quiet_s)
					break;
			}
			if (i < PCACHE_GROUP_SLOTS)
				continue;
			if (planned + PCACHE_HPS > step)
				break;
			if (!floor_allows(planned + PCACHE_HPS))
				break;
			for (i = 0; i < PCACHE_GROUP_SLOTS; i++)
				bit_clr(arena->zwarm, g * PCACHE_GROUP_SLOTS + i);
			arena->nzfree_warm -= PCACHE_GROUP_SLOTS;
			planned += PCACHE_HPS;
			if (nruns && runs[nruns - 1].zone &&
			    runs[nruns - 1].g0 + runs[nruns - 1].n == g) {
				runs[nruns - 1].n++;
			} else {
				runs[nruns].g0 = g;
				runs[nruns].n = 1;
				runs[nruns].tail = 0;
				runs[nruns].zone = 1;
				nruns++;
			}
		}
	}
	/* S177: the window has stopped being wanted - send the tail walk
	 * back over it.  Without this the walk finishes the rest of the
	 * tail while the window is still excluded, latches tail_done, and
	 * the groups stay resident for the life of the process: the 10 MB
	 * tailtest measured above `held` on an idle node. */
	if (arena->hbase && arena->ahead_from && !ahead_wanted()) {
		if (arena->tail_next > arena->ahead_from)
			arena->tail_next = arena->ahead_from;
		arena->tail_done = 0;
		arena->ahead_from = 0;
	}
	if (arena->hbase && !arena->giveback_off && !arena->tail_done) {
		unsigned long lo = (arena->hoff + PCACHE_HPS - 1) / PCACHE_HPS * PCACHE_HPS;
		unsigned long hi = arena->rtop / PCACHE_HPS * PCACHE_HPS;
		/* S166: the committed window above the frontier is deliberate
		 * while it is WANTED - punching it then would hand back what
		 * the tick just committed and put the populate back on the
		 * write path.  S177: once it is not (the node has gone quiet),
		 * the exclusion goes with it, and these groups are tail like
		 * any other.  One predicate, asked in both places. */
		if (ahead_wanted()) {
			unsigned long keep = arena->hoff / PCACHE_HPS * PCACHE_HPS +
				(unsigned long)(PCACHE_AHEAD_GROUPS + 1) * PCACHE_HPS;

			if (lo < keep)
				lo = keep;
		}

		if (arena->tail_next < lo)
			arena->tail_next = lo;
		while (arena->tail_next < hi && nruns < PUNCH_RUNS_MAX &&
		       planned + PCACHE_HPS <= step) {
			g = (unsigned int)(arena->tail_next / PCACHE_HPS);
			if (!bit_get(arena->hcommit, g)) {   /* S97: never committed: nothing to punch */
				arena->tail_next += PCACHE_HPS;
				continue;
			}
			if (nruns && runs[nruns - 1].tail &&
			    runs[nruns - 1].g0 + runs[nruns - 1].n == g) {
				runs[nruns - 1].n++;
			} else {
				runs[nruns].g0 = g;
				runs[nruns].n = 1;
				runs[nruns].tail = 1;
				runs[nruns].zone = 0;
				nruns++;
			}
			arena->tail_next += PCACHE_HPS;
			planned += PCACHE_HPS;
		}
		if (arena->tail_next >= hi)
			arena->tail_done = 1;
	}

	/* shm pages: a page whose every slot is free and quiet goes back to
	 * shm_free (keep reclaim_keep of them resident for the next growth).
	 * Unlinked and accounted here - a free() cannot fail - and released
	 * outside the lock along with the groups. */
	for (ppg = &arena->pages; (pg = *ppg) != NULL; ) {
		if (pg->nfree < pg->nslots ||
		    !range_quiet(pg->base, pg->base + (unsigned long)pg->nslots * PCACHE_SLOT)) {
			ppg = &pg->next;
			continue;
		}
		if (keep_pages < (unsigned int)pcache_reclaim_keep) {
			keep_pages++;
			ppg = &pg->next;
			continue;
		}
		if (planned + (PCACHE_PAGE_SLOTS + 1) * PCACHE_SLOT > step)
			break;                 /* S100: the rest waits a tick */
		if (!floor_allows(planned + (PCACHE_PAGE_SLOTS + 1) * PCACHE_SLOT)) {
			ppg = &pg->next;       /* at the floor: keep this page */
			continue;
		}
		arena->nfree_warm -= pg->nslots;
		arena->slots_total -= pg->nslots;
		arena->bytes -= (PCACHE_PAGE_SLOTS + 1) * PCACHE_SLOT;
		arena->released_bytes += (PCACHE_PAGE_SLOTS + 1) * PCACHE_SLOT;
		planned += (PCACHE_PAGE_SLOTS + 1) * PCACHE_SLOT;
		*ppg = pg->next;
		arena->npages--;
		arena->pages_freed++;
		pg->next = freed;
		freed = pg;
	}
	if (!nruns && !freed)
		return;

	/* ---- phase 2: the syscalls, lock DROPPED ------------------------ */
	lock_release(&arena->lock);
	for (done = 0; done < nruns; done++) {
		p = arena->hbase + (unsigned long)runs[done].g0 * PCACHE_HPS;
		/* S97: a pinned mapping refuses MADV_DONTNEED (EINVAL), and the
		 * growth now lives inside the reservation rather than in 4K
		 * pages a free() could return - so a pinned run is unpinned
		 * first.  Nothing is lost: a punched group is not resident, and
		 * group_commit() pins it again when a carve brings it back. */
		if (arena->pinned)
			(void)munlock(p, (size_t)runs[done].n * PCACHE_HPS);
		if (madvise(p, (size_t)runs[done].n * PCACHE_HPS, MADV_DONTNEED) < 0) {
			err = errno;
			break;
		}
	}
	while (freed) {
		pg = freed;
		freed = pg->next;
		shm_free(pg->raw);
		shm_free(pg);
	}
	lock_get(&arena->lock);

	/* ---- phase 3: bookkeeping for what happened, lock held ----------- */
	for (r = 0; r < done; r++) {
		for (g = runs[r].g0; g < runs[r].g0 + runs[r].n; g++)
			if (bit_get(arena->hcommit, g)) {   /* S97: punched = uncommitted */
				bit_clr(arena->hcommit, g);
				arena->committed -= PCACHE_HPS;
			}
		if (arena->pinned)
			arena->hlocked_mb = arena->committed >> 20;
		if (runs[r].tail) {            /* S119: never in the slot maps */
			arena->punch_calls++;
			arena->punch_groups += runs[r].n;
			arena->released_bytes += (unsigned long)runs[r].n * PCACHE_HPS;
			arena->tail_released += (unsigned long)runs[r].n * PCACHE_HPS;
			continue;
		}
		if (runs[r].zone) {            /* S150 C: the region zone's maps */
			for (g = runs[r].g0; g < runs[r].g0 + runs[r].n; g++) {
				for (i = 0; i < PCACHE_GROUP_SLOTS; i++)
					bit_set(arena->zcold, g * PCACHE_GROUP_SLOTS + i);
				arena->nzfree_cold += PCACHE_GROUP_SLOTS;
				arena->cold_bytes += PCACHE_HPS;
				arena->bytes -= PCACHE_HPS;
				arena->released_bytes += PCACHE_HPS;
			}
			arena->punch_calls++;
			arena->punch_groups += runs[r].n;
			continue;
		}
		for (g = runs[r].g0; g < runs[r].g0 + runs[r].n; g++) {
			for (i = 0; i < PCACHE_GROUP_SLOTS; i++)
				bit_set(arena->rcold, g * PCACHE_GROUP_SLOTS + i);
			arena->nfree_cold += PCACHE_GROUP_SLOTS;
			arena->cold_bytes += PCACHE_HPS;
			arena->bytes -= PCACHE_HPS;
			arena->released_bytes += PCACHE_HPS;
		}
		arena->punch_calls++;
		arena->punch_groups += runs[r].n;
	}
	if (done < nruns) {
		/* the run that was refused and every run after it stay resident
		 * and go back on the warm map.  The refusal seen here, EINVAL
		 * from a pinned mapping, fails before touching a page, so the
		 * headers are as they were. */
		LM_WARN("MADV_DONTNEED refused on the %s arena (%s) - "
			"free slots stay resident from now on\n",
			pcache_mem_tier_str(arena->htier), strerror(err));
		arena->giveback_off = 1;
		for (r = done; r < nruns; r++) {
			if (runs[r].tail) {        /* S119: nothing to put back */
				arena->tail_done = 0;
				continue;
			}
			if (runs[r].zone) {        /* S150 C: back to the zone's warm set */
				for (g = runs[r].g0; g < runs[r].g0 + runs[r].n; g++)
					for (i = 0; i < PCACHE_GROUP_SLOTS; i++)
						bit_set(arena->zwarm, g * PCACHE_GROUP_SLOTS + i);
				arena->nzfree_warm += PCACHE_GROUP_SLOTS * runs[r].n;
				continue;
			}
			for (g = runs[r].g0; g < runs[r].g0 + runs[r].n; g++)
				for (i = 0; i < PCACHE_GROUP_SLOTS; i++)
					bit_set(arena->rwarm, g * PCACHE_GROUP_SLOTS + i);
			arena->nfree_warm += PCACHE_GROUP_SLOTS * runs[r].n;
		}
	}
}

/*
 * "Send your hoard home": runs in EVERY process through the core's IPC,
 * between its messages.  A process's private free stack and the remainder
 * of the slot it is carving from pin their chunks - a few hundred cells
 * scattered over as many chunks keep those chunks from ever draining - so
 * when chunks linger partially home the reclaim process asks everyone to
 * let go; the next allocation simply refills from the arena.
 */
static void pcache_flush_rpc(int sender, void *param)
{
	pcache_arena_flush_private();
}

/*
 * One reclaim tick.  Drained chunks live on the class avail stacks (a
 * chunk with cells home is always there, or the allocator's current one,
 * or on its way): take the stack, retire the drained ones beyond
 * reclaim_keep, put the rest back.  A chunk whose announcement is still in
 * flight (flag set, not found) is left for the next tick.
 */
/* S188: the commit-ahead window, and nothing else - see the header for
 * why it does not wait for the reclaim tick's second. */
void pcache_arena_ahead_tick(void)
{
	if (backing != PCACHE_BACKING_OWN)
		return;
	lock_get(&arena->lock);
	/* A window that WIDENS while the write path is still committing for
	 * itself was built and measured here first: 6 inline commits became
	 * 5 on a 0.4 s burst, which is noise.  The feedback arrives on the
	 * NEXT tick and the burst is over by then, so it can only pay for a
	 * sustained load - and that is not something this session could
	 * demonstrate, so it is not shipped.  The hook is the pair of
	 * counters: commits_inline moving between two of these calls is
	 * exactly the signal a widening policy would read. */
	commit_ahead();                        /* its own ahead_wanted() guard */
	lock_release(&arena->lock);
}

void pcache_arena_reclaim_tick(void)
{
	pcache_chunk_t *ch, *next, *head;
	unsigned int kept, nfree, lingering = 0;
	int c;

	if (backing != PCACHE_BACKING_OWN)
		return;

	lock_get(&arena->lock);
	arena->tick++;
	for (c = 0; c < PCACHE_NCLASSES; c++) {
		kept = 0;
		head = __atomic_exchange_n(&arena->avail[c], NULL, __ATOMIC_ACQ_REL);
		for (ch = head; ch; ch = next) {
			next = ch->link;
			nfree = __atomic_load_n(&ch->nfree, __ATOMIC_ACQUIRE);
			if (nfree == ch->cells &&
			    kept++ >= (unsigned int)pcache_reclaim_keep) {
				chunk_retire(ch);
				continue;
			}
			/* partially home for a whole quiet window, nobody taking
			 * the cells and nobody bringing the rest: a hoard pins it */
			if (nfree && nfree < ch->cells &&
			    arena->tick - ch->home_since >= (unsigned int)pcache_reclaim_quiet_s)
				lingering++;
			avail_push(c, ch);
		}
		/* the allocator's current chunk is not on the stack */
		ch = arena->cur[c];
		if (ch && !__atomic_load_n(&ch->in_avail, __ATOMIC_ACQUIRE) &&
		    __atomic_load_n(&ch->nfree, __ATOMIC_ACQUIRE) == ch->cells &&
		    kept++ >= (unsigned int)pcache_reclaim_keep)
			chunk_retire(ch);
	}
	commit_ahead();                 /* S166: before the give-back decides */
	giveback_tick();
	if (lingering &&
	    arena->tick - arena->flush_tick >= (unsigned int)pcache_reclaim_quiet_s) {
		arena->flush_tick = arena->tick;
		arena->flush_broadcasts++;
		lock_release(&arena->lock);
		ipc_send_rpc_all(pcache_flush_rpc, NULL);
		return;
	}
	lock_release(&arena->lock);
}

/* ---- reporting --------------------------------------------------------- */

/* live CELL bytes - the proportional-leveling basis (chunk-level
 * 'bytes' is peak-sticky until reclaim retires drained chunks) */
unsigned long pcache_arena_held_bytes(void)
{
	return arena ? __atomic_load_n(&arena->bytes, __ATOMIC_RELAXED) : 0;
}

/* S120: the cell a request of n bytes occupies - the class rounding every
 * record pays, and what the budget sums; 0 beyond the largest class */
unsigned int pcache_arena_cell_size(size_t n)
{
	size_t idx = (n + 31) >> 5;
	unsigned char c;

	if (idx > 2048)
		return 0;
	c = size2class[idx];
	return c < PCACHE_NCLASSES ? cell_sizes[c] : 0;
}

unsigned long pcache_arena_regions_bytes(void)
{
	return arena ? arena->regions_bytes : 0;
}

unsigned long long pcache_arena_live_bytes(void)
{
	if (!arena)
		return 0;
	return __atomic_load_n(&arena->live, __ATOMIC_RELAXED);
}

void pcache_arena_stats(unsigned int *nchunks, unsigned long *bytes)
{
	if (backing != PCACHE_BACKING_OWN) {
		*nchunks = 0;                  /* no chunks of ours: HG's cells */
		*bytes = __atomic_load_n(&arena->bytes, __ATOMIC_RELAXED);
		return;
	}
	lock_get(&arena->lock);
	*nchunks = arena->chunks_used;
	*bytes = arena->bytes;
	lock_release(&arena->lock);
}

/* S47: the pressure surface.  Counters are RELAXED atomics / lock-free
 * reads - a torn number on a stats read is accepted, as everywhere in
 * the stats path; the operator wants trend, not a transactional snap. */
void pcache_arena_pressure(struct pcache_arena_pressure *out)
{
	out->tier = arena ? pcache_mem_tier_str(arena->htier) : "none";
	out->refused = __atomic_load_n(&arena_refused, __ATOMIC_RELAXED);
	out->pool_empty = __atomic_load_n(&arena_pool_empty, __ATOMIC_RELAXED);
	out->commits_ahead = arena->commits_ahead;          /* S166 */
	out->commits_inline = arena->commits_inline;
	out->commits_index = arena->commits_index;          /* S168 */
	out->locked_bytes = arena->pinned                   /* S167 */
		? (unsigned long)arena->hlocked_mb << 20 : 0;
	out->pin_lost = arena->pin_failed;
	out->at_ceiling_since = __atomic_load_n(&arena_ceiling_since, __ATOMIC_RELAXED);
	if (!arena) {
		out->retired = out->pages_freed = out->released_bytes = 0;
		out->cold_bytes = out->flushes = 0;
		out->punch_calls = out->punch_groups = 0;
		out->shrink_step = 0;
		out->giveback_off = 0;
		out->regions_bytes = out->warm_free_bytes = 0;
		out->class_chunk_bytes = out->page_slack_bytes = 0;
		out->committed_bytes = out->reserved_bytes = 0;
		out->tail_released_bytes = 0;
		out->regions_free_warm_bytes = out->regions_free_cold_bytes = 0;
		out->regions_retired = out->region_reuse = 0;
		return;
	}
	out->regions_bytes = arena->regions_bytes;             /* S114 */
	/* S150 C: a retired slot kept resident is free and warm - the card's
	 * rows still add up to held - and shown on its own beside that */
	out->warm_free_bytes = ((unsigned long)arena->nfree_warm +
		arena->nzfree_warm) * PCACHE_SLOT;
	out->regions_free_warm_bytes = (unsigned long)arena->nzfree_warm * PCACHE_SLOT;
	out->regions_free_cold_bytes = (unsigned long)arena->nzfree_cold * PCACHE_SLOT;
	out->regions_retired = arena->zregions_retired;
	out->region_reuse = arena->zregion_reuse;
	out->class_chunk_bytes = (unsigned long)arena->chunks_used * PCACHE_SLOT;   /* S118 */
	out->page_slack_bytes = (unsigned long)arena->npages * PCACHE_SLOT;
	out->committed_bytes = arena->hbase ? arena->committed : 0;                 /* S97 */
	out->reserved_bytes = arena->hbase ? arena->hsize : 0;
	out->tail_released_bytes = arena->tail_released;                            /* S119 */
	out->retired = arena->chunks_retired;
	out->pages_freed = arena->pages_freed;
	out->released_bytes = arena->released_bytes;
	out->cold_bytes = arena->cold_bytes;
	out->flushes = arena->flush_broadcasts;
	out->punch_calls = arena->punch_calls;
	out->punch_groups = arena->punch_groups;
	out->shrink_step = shrink_step();
	out->giveback_off = arena->giveback_off;
}

/* the reclaim view for perf_stats "arena" - arena lock taken here */
int pcache_arena_mi(mi_item_t *aobj)
{
	char buf[PCACHE_NCLASSES * 24], *p = buf;
	pcache_page_t *pg;
	pcache_chunk_t *ch;
	unsigned long off, out[PCACHE_NCLASSES], strand = 0;
	unsigned int i, c;
	int rc;

	if (backing != PCACHE_BACKING_OWN)
		return 0;

	memset(out, 0, sizeof out);
	lock_get(&arena->lock);
	/* cells out (not home) per class: the reservation's cut slots and
	 * every page slot */
	for (off = 0; off < arena->hoff; off += PCACHE_SLOT) {
		ch = (pcache_chunk_t *)(arena->hbase + off);
		if (ch->cls < PCACHE_NCLASSES)
			out[ch->cls] += ch->cells - ch->nfree;
	}
	for (pg = arena->pages; pg; pg = pg->next)
		for (i = 0; i < pg->nslots; i++) {
			ch = (pcache_chunk_t *)(pg->base + (unsigned long)i * PCACHE_SLOT);
			if (ch->cls < PCACHE_NCLASSES)
				out[ch->cls] += ch->cells - ch->nfree;
		}
	for (c = 0; c < PCACHE_NCLASSES; c++) {
		if (!arena->chunks_cls[c] && !arena->chunks_peak_cls[c])
			continue;
		if (!out[c])
			strand += arena->chunks_cls[c];   /* held by a class with nothing live */
		p += snprintf(p, sizeof buf - (p - buf), "%s%u:%u/%u/%lu",
			p == buf ? "" : " ", cell_sizes[c], arena->chunks_cls[c],
			arena->chunks_peak_cls[c], out[c]);
	}
	rc = add_mi_number(aobj, MI_SSTR("slot_kb"), PCACHE_SLOT >> 10) < 0 ||
	     add_mi_number(aobj, MI_SSTR("slots_total"), arena->slots_total) < 0 ||
	     add_mi_number(aobj, MI_SSTR("slots_free_warm"), arena->nfree_warm) < 0 ||
	     add_mi_number(aobj, MI_SSTR("slots_free_cold"), arena->nfree_cold) < 0 ||
	     add_mi_number(aobj, MI_SSTR("chunks_strand"), strand) < 0 ||
	     add_mi_number(aobj, MI_SSTR("chunks_retired"), arena->chunks_retired) < 0 ||
	     add_mi_number(aobj, MI_SSTR("flush_broadcasts"), arena->flush_broadcasts) < 0 ||
	     add_mi_number(aobj, MI_SSTR("pages"), arena->npages) < 0 ||
	     add_mi_number(aobj, MI_SSTR("pages_freed"), arena->pages_freed) < 0 ||
	     add_mi_number(aobj, MI_SSTR("released_bytes"), arena->released_bytes) < 0 ||
	     add_mi_number(aobj, MI_SSTR("cold_bytes"), arena->cold_bytes) < 0 ||
	     add_mi_number(aobj, MI_SSTR("pool_empty"), arena_pool_empty) < 0 ||
	     add_mi_number(aobj, MI_SSTR("at_ceiling_since"), arena_ceiling_since) < 0 ||
	     add_mi_number(aobj, MI_SSTR("punch_calls"), arena->punch_calls) < 0 ||
	     add_mi_number(aobj, MI_SSTR("punch_groups"), arena->punch_groups) < 0 ||
	     add_mi_number(aobj, MI_SSTR("shrink_step_bytes"), shrink_step()) < 0 ||
	     add_mi_number(aobj, MI_SSTR("reclaim_ticks"), arena->tick) < 0 ||
	     add_mi_number(aobj, MI_SSTR("last_carve_age_s"),
	         arena->tick - arena->carve_tick) < 0 ||
	     add_mi_string(aobj, MI_SSTR("classes"), buf, p - buf) < 0;
	lock_release(&arena->lock);
	return rc ? -1 : 0;
}

int pcache_arena_tier(void)
{
	if (backing == PCACHE_BACKING_OWN_HG)
		return mem_arena_tier(hg_handle);     /* same enum values */
	if (backing == PCACHE_BACKING_CORE)
		return PCACHE_MEM_NO_ARENA;           /* the core arena's tier */
	return arena->hbase ? (int)arena->htier : PCACHE_MEM_NO_ARENA;
}

int pcache_arena_backing(void)
{
	return backing;
}

const char *pcache_arena_backing_str(void)
{
	switch (backing) {
	case PCACHE_BACKING_CORE:
		return "core (HG_MALLOC shm arena cells)";
	case PCACHE_BACKING_OWN_HG:
		return "own arena managed by HG_MALLOC";
	default:
		return arena && arena->hbase ? "own slots in a dedicated reservation"
		                             : "own slots in shm";
	}
}

void pcache_arena_backing_notice(void)
{
	unsigned long committed, cap, live;

	switch (backing) {
	case PCACHE_BACKING_CORE:
		LM_NOTICE("memory backing IN USE: the core HG_MALLOC shm arena - "
			"every cache cell is an HG slab cell (classes, per-process "
			"caches, GC and re-typing, growth and maintenance are HG's); "
			"counted in core's shmem: stats%s\n",
			pcache_arena_hugepage_mb > 0 ? " (arena_hugepage_mb ignored "
			"under memory_backing=core)" : "");
		break;
	case PCACHE_BACKING_OWN_HG:
		mem_arena_usage(hg_handle, &committed, &cap, &live);
		LM_NOTICE("memory backing IN USE: a dedicated arena managed by "
			"HG_MALLOC - %lu MB committed, can grow to %lu MB%s; see "
			"hg_stats 'cachedb_perf'\n", committed >> 20, cap >> 20,
			pcache_arena_profile ? " under its auto-scaling profile" : "");
		break;
	default:
		if (pcache_arena_hugepage_mb > 0)
			LM_NOTICE("memory backing IN USE: a separate %d MB reservation, "
				"OUTSIDE OpenSIPS shared memory (arena_hugepage_mb), "
				"cachedb_perf's own slot allocator with reclaim (keep %d "
				"drained chunks per class, give back after %d s quiet / "
				"%d s cool-off%s)\n", pcache_arena_hugepage_mb,
				pcache_reclaim_keep, pcache_reclaim_quiet_s,
				pcache_reclaim_cooloff_s,
				pcache_reclaim_giveback ? "" : ", give-back OFF");
		else
			LM_NOTICE("memory backing IN USE: OpenSIPS shared memory "
				"(shm_malloc), cachedb_perf's own slot allocator with "
				"reclaim (keep %d drained chunks per class, pages back to "
				"shm after %d s quiet / %d s cool-off%s) - NOT a separate "
				"reservation; counted in core's own shmem: stats. Set "
				"arena_hugepage_mb to reserve a dedicated arena.\n",
				pcache_reclaim_keep, pcache_reclaim_quiet_s,
				pcache_reclaim_cooloff_s,
				pcache_reclaim_giveback ? "" : ", give-back OFF");
	}
}

static void pcache_arena_hg_capacity(int *active, unsigned long *total,
		unsigned long *used, unsigned long *free)
{
	unsigned long committed, cap, live;

	mem_arena_usage(hg_handle, &committed, &cap, &live);
	*active = 1;
	*total = cap;
	*used = live;
	*free = cap > live ? cap - live : 0;
}

/* the dedicated reservation: used = slots holding a chunk plus the index
 * regions cut from it (task C9: live slots, not the bump frontier) */
void pcache_arena_hugepage_capacity(int *active, unsigned long *total,
		unsigned long *used, unsigned long *free)
{
	unsigned long off, n = 0;
	pcache_chunk_t *ch;

	if (backing == PCACHE_BACKING_OWN_HG) {
		pcache_arena_hg_capacity(active, total, used, free);
		return;
	}
	if (backing != PCACHE_BACKING_OWN || !arena->hbase) {
		*active = 0;
		*total = 0;
		*used = 0;
		*free = 0;
		return;
	}
	lock_get(&arena->lock);
	for (off = 0; off < arena->hoff; off += PCACHE_SLOT) {
		ch = (pcache_chunk_t *)(arena->hbase + off);
		if (ch->cls != PCACHE_CLS_FREE)
			n++;
	}
	*active = 1;
	*total = arena->hsize;
	*used = n * PCACHE_SLOT + (arena->hsize - arena->rtop);   /* + the tables */
	*free = arena->hsize - *used;
	lock_release(&arena->lock);
}

#ifdef PCACHE_ARENA_DEBUG
/*
 * RV-3: the census.  The moves above prove no cell is pushed twice; this
 * proves the rest at a quiet moment - every cell is in EXACTLY one of:
 * out (with a user, or uncut in somebody's bump), on one private stack,
 * home on its own chunk's list.  The lists are walked and each cell
 * marked; a cell met twice is on two lists, a cell the table calls free
 * that no list holds is lost, a list longer than its counter is a
 * counter that lies, and the cells that are out must add up to the live
 * figure.  The caller makes it quiet; the arena lock only keeps the
 * carve and the reclaim tick out.
 */
static pcache_chunk_t **census_chunks(unsigned int *n_out)
{
	pcache_chunk_t **v, *ch;
	pcache_page_t *pg;
	unsigned long off;
	unsigned int n = 0, cap, i;

	cap = (unsigned int)(arena->hoff / PCACHE_SLOT);
	for (pg = arena->pages; pg; pg = pg->next)
		cap += pg->nslots;
	v = calloc(cap ? cap : 1, sizeof *v);
	if (!v)
		return NULL;
	for (off = 0; off < arena->hoff; off += PCACHE_SLOT) {
		ch = (pcache_chunk_t *)(arena->hbase + off);
		if (ch->cls < PCACHE_NCLASSES && ch->dbg_state)
			v[n++] = ch;
	}
	for (pg = arena->pages; pg; pg = pg->next)
		for (i = 0; i < pg->nslots; i++) {
			ch = (pcache_chunk_t *)(pg->base +
				(unsigned long)i * PCACHE_SLOT);
			if (ch->cls < PCACHE_NCLASSES && ch->dbg_state)
				v[n++] = ch;
		}
	*n_out = n;
	return v;
}

int pcache_arena_census(char *why, size_t cap)
{
	pcache_chunk_t **chunks, *ch;
	struct dbg_pl *r;
	unsigned long out_bytes = 0, bump_bytes = 0, live;
	unsigned int nch = 0, i, k, n;
	unsigned char *st;
	void *cell;
	int bad = 0, c;

#define CBAD(...) do { if (!bad && why && cap) snprintf(why, cap, __VA_ARGS__); \
	bad++; } while (0)
	if (why && cap)
		why[0] = 0;
	if (!arena || backing != PCACHE_BACKING_OWN)
		return 0;
	lock_get(&arena->lock);
	chunks = census_chunks(&nch);
	if (!chunks) {
		lock_release(&arena->lock);
		return 0;
	}

	/* 1. what is home */
	for (i = 0; i < nch; i++) {
		ch = chunks[i];
		n = 0;
		for (cell = ch->free_head; cell; cell = cell_next(cell)) {
			if (cell_chunk(cell) != ch) {
				CBAD("cell %p is on chunk %p's home list and "
					"belongs to chunk %p", cell, (void *)ch,
					(void *)cell_chunk(cell));
				break;
			}
			st = dbg_slot(cell);
			if (*st & DBG_SEEN) {
				CBAD("cell %p is on a list TWICE (met again on "
					"chunk %p's home list)", cell, (void *)ch);
				break;
			}
			if (*st != DBG_HOME)
				CBAD("cell %p is on chunk %p's home list but "
					"recorded as %s", cell, (void *)ch,
					dbg_name(*st));
			*st |= DBG_SEEN;
			if (++n > ch->cells) {
				CBAD("chunk %p's home list is longer than the "
					"chunk (%u cells)", (void *)ch, ch->cells);
				break;
			}
		}
		if (n != ch->nfree)
			CBAD("chunk %p: %u cells on the home list, its "
				"counter says %u", (void *)ch, n, ch->nfree);
	}

	/* 2. what the threads hoard, and what they have not cut yet */
	for (r = dbg_pls; r; r = r->next)
		for (c = 0; c < PCACHE_NCLASSES; c++) {
			n = 0;
			for (cell = r->pl->cls[c].free_head; cell;
			        cell = cell_next(cell)) {
				st = dbg_slot(cell);
				if (!st || (int)cell_chunk(cell)->cls != c) {
					CBAD("cell %p is on a class-%d private "
						"stack and is not a class-%d cell",
						cell, c, c);
					break;
				}
				if (*st & DBG_SEEN) {
					CBAD("cell %p is on a list TWICE (met "
						"again on a private stack)", cell);
					break;
				}
				if (*st != DBG_PRIVATE)
					CBAD("cell %p is on a private stack but "
						"recorded as %s", cell,
						dbg_name(*st));
				*st |= DBG_SEEN;
				if (++n > r->pl->cls[c].nfree) {
					CBAD("a class-%d private stack is longer "
						"than its counter (%u)", c,
						r->pl->cls[c].nfree);
					break;
				}
			}
			if (n != r->pl->cls[c].nfree)
				CBAD("a class-%d private stack holds %u cells, "
					"its counter says %u", c, n,
					r->pl->cls[c].nfree);
			for (k = 0; k < r->pl->cls[c].left; k++) {
				st = dbg_slot(r->pl->cls[c].bump +
					(unsigned long)k * cell_sizes[c]);
				if (st && *st != DBG_OUT)
					CBAD("an uncut cell of class %d is "
						"recorded as %s", c, dbg_name(*st));
			}
			bump_bytes += (unsigned long)r->pl->cls[c].left *
				cell_sizes[c];
		}

	/* 3. the table against the lists: free and on no list = lost */
	for (i = 0; i < nch; i++) {
		ch = chunks[i];
		for (k = 0; k < ch->cells; k++) {
			st = &ch->dbg_state[k];
			if (*st & DBG_SEEN)
				*st &= (unsigned char)~DBG_SEEN;
			else if (*st != DBG_OUT)
				CBAD("cell %u of chunk %p is recorded as %s and "
					"is on no list - lost", k, (void *)ch,
					dbg_name(*st));
			if (*st == DBG_OUT)
				out_bytes += ch->cell_size;
		}
	}

	/* 4. and the cells that are out are the live figure */
	live = __atomic_load_n(&arena->live, __ATOMIC_RELAXED);
	if (out_bytes - bump_bytes != live)
		CBAD("the live figure says %lu bytes; the cells that are out "
			"add up to %lu (%lu out less %lu uncut)", live,
			out_bytes - bump_bytes, out_bytes, bump_bytes);
#undef CBAD
	free(chunks);
	lock_release(&arena->lock);
	return bad ? -1 : 0;
}
#endif /* PCACHE_ARENA_DEBUG */

/*
 * startup selftest (modparam "arena_selftest"): exercises class mapping,
 * the stamp/bound contract, LIFO reuse, chunk growth, sending cells home,
 * refill, extents, the oversize edge, and reclaim: drained chunks retire
 * beyond reclaim_keep and their slots are re-cut for another class.  Ends
 * by dropping the private state through pcache_arena_child_init(), the
 * fork-reset path - so that gets exercised too.
 */
#define CHK(cond, ...) \
	do { \
		if (!(cond)) { \
			LM_ERR("arena selftest FAILED: " __VA_ARGS__); \
			return -1; \
		} \
	} while (0)

int pcache_arena_selftest(void)
{
	void *a, *b, **ptrs;
	unsigned long lo, hi, by0, by1, retired0, retired1;
	unsigned int n0, n1, n2, n3, i, used0, warm;
	const unsigned int N = 5000;
	int c;

	if (backing != PCACHE_BACKING_OWN) {
		LM_NOTICE("arena selftest: skipped (%s)\n", pcache_arena_backing_str());
		return 0;
	}

	/* class mapping, stamp, bound, LIFO reuse, boundary crossing */
	for (c = 0; c < PCACHE_NCLASSES; c++) {
		a = pcache_cell_alloc(cell_sizes[c]);
		CHK(a != NULL, "alloc(%u) failed\n", cell_sizes[c]);
		CHK(*(unsigned char *)a == c, "class stamp %d != %d\n",
			*(unsigned char *)a, c);
		CHK(pcache_cell_bound(a) == cell_sizes[c],
			"bound %u != %u\n", pcache_cell_bound(a), cell_sizes[c]);
		CHK(cell_chunk(a)->cls == (unsigned int)c,
			"cell %p of class %d not in a class %d chunk\n", a, c, c);
		memset((char *)a + 1, 0xAB, cell_sizes[c] - 1);
		pcache_cell_free(a);
		b = pcache_cell_alloc(cell_sizes[c]);
		CHK(b == a, "no LIFO reuse in class %d\n", c);
		pcache_cell_free(b);
		if (c < PCACHE_NCLASSES - 1) {
			a = pcache_cell_alloc(cell_sizes[c] + 1);
			CHK(*(unsigned char *)a == c + 1,
				"size %u not in class %d\n", cell_sizes[c] + 1, c + 1);
			pcache_cell_free(a);
		}
	}

	/* S174: the ladder's top rung and the advertised ceiling are two
	 * constants that must agree, and every class has to cut at least
	 * one cell out of a chunk - the arithmetic that makes 262,144
	 * impossible */
	CHK(cell_sizes[PCACHE_NCLASSES - 1] == PCACHE_CELL_MAX,
		"top class %u != the ceiling %d\n",
		cell_sizes[PCACHE_NCLASSES - 1], PCACHE_CELL_MAX);
	for (c = 0; c < PCACHE_NCLASSES; c++)
		CHK((PCACHE_SLOT - PCACHE_CHUNK_HDR) / cell_sizes[c] >= 1,
			"class %d (%u bytes) cuts no cells from a chunk\n",
			c, cell_sizes[c]);

	/* oversize and zero */
	CHK(pcache_cell_alloc(PCACHE_CELL_MAX + 1) == NULL, "oversize passed\n");
	a = pcache_cell_alloc(0);
	CHK(a && *(unsigned char *)a == 0, "zero-size alloc broken\n");
	pcache_cell_free(a);

	/* bulk: multiple chunks, uniqueness, extents */
	ptrs = pkg_malloc(N * sizeof *ptrs);
	CHK(ptrs != NULL, "no pkg for the pointer array\n");
	pcache_arena_stats(&n0, &by0);
	for (i = 0; i < N; i++) {
		ptrs[i] = pcache_cell_alloc(64);
		if (!ptrs[i]) {
			pkg_free(ptrs);
			CHK(0, "bulk alloc %u failed\n", i);
		}
		*(unsigned int *)((char *)ptrs[i] + 8) = i;
	}
	pcache_arena_stats(&n1, &by1);
	CHK(n1 > n0, "no chunk growth over %u allocs\n", N);
	pcache_arena_extents(&lo, &hi);
	for (i = 0; i < N; i++) {
		CHK(*(unsigned int *)((char *)ptrs[i] + 8) == i,
			"cell %u overlapped\n", i);
		CHK((unsigned long)ptrs[i] >= lo &&
			(unsigned long)ptrs[i] + 64 <= hi,
			"cell %u outside the extents\n", i);
		CHK(((unsigned long)ptrs[i] & ~PCACHE_SLOT_MASK) >= PCACHE_CHUNK_HDR,
			"cell %u inside a chunk header\n", i);
	}
	for (i = 0; i < N; i++)
		pcache_cell_free(ptrs[i]);
	/* the private stack must have sent cells home past the threshold */
	CHK(arena->avail[0] != NULL, "nothing went home after %u frees\n", N);

	/* full reuse: no new chunks on the second pass (refill path) */
	for (i = 0; i < N; i++) {
		ptrs[i] = pcache_cell_alloc(64);
		if (!ptrs[i]) {
			pkg_free(ptrs);
			CHK(0, "realloc %u failed\n", i);
		}
	}
	pcache_arena_stats(&n2, &by1);
	CHK(n2 == n1, "reuse pass grew chunks: %u -> %u\n", n1, n2);
	for (i = 0; i < N; i++)
		pcache_cell_free(ptrs[i]);
	pkg_free(ptrs);

	/* reclaim: with everything home, a tick retires the drained class-0
	 * chunks beyond reclaim_keep, and the next carve of ANOTHER class
	 * re-cuts one of the freed slots (cross-class reuse) */
	pcache_arena_flush_private();
	used0 = arena->chunks_used;
	retired0 = arena->chunks_retired;
	pcache_arena_reclaim_tick();
	retired1 = arena->chunks_retired;
	CHK(retired1 > retired0, "no chunk retired after a full drain "
		"(%u chunks in use, reclaim_keep %d)\n", used0, pcache_reclaim_keep);
	CHK(arena->chunks_used + (retired1 - retired0) == used0,
		"retire accounting: %u used + %lu retired != %u\n",
		arena->chunks_used, retired1 - retired0, used0);
	warm = arena->nfree_warm;
	CHK(warm >= retired1 - retired0, "retired slots not on the warm list "
		"(%u warm)\n", warm);
	/* the largest class holds 3 cells per slot and owns exactly one chunk
	 * here: the 4th allocation must cut a new chunk, and that cut must
	 * take a retired slot (warm count down by one), stamped for ITS class */
	{
		void *big[4];
		unsigned int bc = PCACHE_NCLASSES - 1;

		for (i = 0; i < 4; i++) {
			big[i] = pcache_cell_alloc(cell_sizes[bc]);
			CHK(big[i] != NULL, "alloc %u of %u after retire failed\n",
				i, cell_sizes[bc]);
			CHK(cell_chunk(big[i])->cls == bc &&
			    *(unsigned char *)big[i] == bc,
				"cell %u of class %u in a class %u chunk\n", i, bc,
				cell_chunk(big[i])->cls);
		}
		CHK(arena->nfree_warm == warm - 1, "the carve did not take a "
			"retired slot (%u -> %u warm)\n", warm, arena->nfree_warm);
		CHK(cell_chunk(big[3]) != cell_chunk(big[0]),
			"4th cell of %u came from the same chunk\n", cell_sizes[bc]);
		for (i = 0; i < 4; i++)
			pcache_cell_free(big[i]);
	}
	pcache_arena_stats(&n3, &by1);

	/* fork-reset path: drop the private state, then allocate fresh */
	pcache_arena_child_init();
	CHK(my_palloc == NULL, "child reset kept state\n");
	a = pcache_cell_alloc(64);
	CHK(a != NULL, "alloc after child reset failed\n");
	pcache_cell_free(a);

	LM_NOTICE("arena selftest: PASS (%u chunks after reclaim of %lu, "
		"%lu bytes, %d classes)\n", n3, retired1 - retired0, by1,
		PCACHE_NCLASSES);
	return 0;
}
