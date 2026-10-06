/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * store.c — the collection registry (task S8).  See store.h.
 */
#include <string.h>
#include <time.h>

#include "config.h"
#include "quiesce.h"
#include "store.h"
#include "events.h"
#include <pthread.h>
#include <unistd.h>
#include "compat/dprint.h"
#include "compat/compat.h"
#include "compat/timer.h"
#include <stdio.h>
#include <stdlib.h>
#include "core/pcache_arena.h"      /* S120: pcache_arena_cell_size */

/* S69: writers (create, drop) serialize here; readers take nothing.  An
 * entry is filled before reg_n is published, and reg_n never shrinks, so
 * a reader either does not see a new entry yet or sees it complete. */
static pthread_mutex_t reg_mx = PTHREAD_MUTEX_INITIALIZER;

static struct {
	/* S69: heap-owned, and swapped as a POINTER so a rename cannot be
	 * read half-done by a lookup that takes no lock.  The old string is
	 * never freed - one per rename, and a reader may still be holding
	 * it. */
	char *name;
	int live;                          /* S69: 0 = dropped, slot reusable */
	int runtime;                       /* S69: created by a client, persisted */
	int buckets_log2;                  /* S69: what the table was made at */
	int floor_log2;                    /* S328: what the OPERATOR made it at
	                                    * - the config, a create, an
	                                    * explicit resize; never an
	                                    * automatic one.  FLOOR mode does
	                                    * not shrink below it */
	unsigned long long gen;            /* S69: the Lamport value of the
	                                    * create or drop that last moved
	                                    * this NAME - the fleet's tie
	                                    * break, and what makes a drop
	                                    * outrank the create it removed */
	pcache_htable_t *ht;
	/* S150 B: bumped at every publish of `ht` or `live` - create, drop,
	 * the resize swap - so a walk that resumes across turns can tell
	 * the table under it moved (the KEYS handle in proto.c) */
	unsigned long long pub;
	int pull;
	int proxy;
	int shard;
	int eager;
	/* S67's stored_bytes/stored_n/size_hist moved out to colst[] (S237) */
	/* S109: the walked figure and its pacing (wall seconds) */
	unsigned long long held_bytes;
	unsigned int held_at, held_next, held_walk_us;
	/* S116: the value sizes HELD, from the same walk, in the eight
	 * classes of size_hist.  Two slots and a generation: the walk fills
	 * the slot the readers are not on, then publishes the generation, so
	 * a reader never sees half of one walk and half of the next. */
	unsigned long long held_hist[2][8];
	unsigned int held_gen;
	/* S120: the budget's parts - the index regions this table was carved
	 * (exact: noted at creation and at every growth step) and the records
	 * as the cells they occupy (from the walk, class-rounded) */
	unsigned long index_bytes;
	/* S69: the resize in flight for this collection, if any */
	pcache_htable_t *rs_to;
	unsigned int rs_cursor;
	int rs_target, rs_pass, rs_swapped;
	unsigned long long rs_moved;
	/* S255: a fleet resize that arrived while this one ran - started when
	 * it finishes, the latest one winning - and the generation of the
	 * newest fleet resize seen, so a late or repeated announce of an
	 * older one cannot undo it */
	int rs_pending;
	unsigned long long rs_gen;
	unsigned long long held_cells;
	int autoscale, as_target, as_set;   /* S239: mode, AUTO's target, set by the verb */
	int redis_types;                    /* S280: PC_TYPES_* */
} reg[PC_MAX_COLLECTIONS];
static int reg_n;

static void retire_push(pcache_htable_t *ht, int idx);   /* S150 C */

/* S69: what a runtime-created collection inherits - the cluster's mode,
 * set once at startup (a fleet is ONE mode over ONE collection set) */
int pc_col_default_pull, pc_col_default_proxy, pc_col_default_shard,
	pc_col_default_eager;

/*
 * S67: what is being STORED, kept at store time so the page can say "are
 * we storing what we think we are" without a walk - a size distribution
 * in log2 classes, and the running total the page turns into a mean and
 * an estimate of memory held.  Expiry and eviction happen inside the
 * table and are not subtracted here: this is the write stream, and the
 * page says so.
 *
 * S237: NOT in reg[].  Every SET incremented these with plain `+=` in the
 * registry entry whose name, live, ht and mode flags every request's
 * lookup reads - so each write invalidated the lines every other
 * worker's pc_store_find/pc_store_ht/pc_store_shard_enabled needed (23%
 * of a saturated SET at 12 workers, DESIGN 12ga), and concurrent `+=`
 * from several workers lost counts.  Here, per collection, one padded
 * stripe per thread (round-robin, shared past COLST_STRIPES), added to
 * atomically and summed on read.
 */
#define COLST_STRIPES 16
static struct {
	unsigned long long bytes, n, hist[8];
	char pad[128 - 10 * sizeof(unsigned long long)];
} colst[PC_MAX_COLLECTIONS][COLST_STRIPES] __attribute__((aligned(128)));
/* S251: 128-aligned so each stripe owns its adjacent-line prefetch pair:
 * a stripe's 80 bytes of counters span two lines, and at 64-alignment
 * the second shared a pair with the next thread's stripe */
static unsigned int colst_next;
static __thread int colst_mine = -1;

static void colst_reset(int i)
{
	int s, c;

	for (s = 0; s < COLST_STRIPES; s++) {
		__atomic_store_n(&colst[i][s].bytes, 0, __ATOMIC_RELAXED);
		__atomic_store_n(&colst[i][s].n, 0, __ATOMIC_RELAXED);
		for (c = 0; c < 8; c++)
			__atomic_store_n(&colst[i][s].hist[c], 0,
				__ATOMIC_RELAXED);
	}
}

void pc_store_note_set(pcache_htable_t *ht, size_t klen, size_t vlen)
{
	int i, c = 0, s = colst_mine;
	size_t v = vlen;

	for (i = 0; i < reg_n; i++)
		if (reg[i].ht == ht)
			break;
	if (i == reg_n)
		return;
	/* classes: <=64, <=256, <=1K, <=4K, <=16K, <=64K, <=256K, more */
	while (c < 7 && v > (64u << (2 * c)))
		c++;
	if (s < 0) {
		s = (int)(__atomic_fetch_add(&colst_next, 1, __ATOMIC_RELAXED) %
			COLST_STRIPES);
		colst_mine = s;
	}
	__atomic_fetch_add(&colst[i][s].bytes, klen + vlen, __ATOMIC_RELAXED);
	__atomic_fetch_add(&colst[i][s].n, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&colst[i][s].hist[c], 1, __ATOMIC_RELAXED);
}

/* S109: what a collection HOLDS, from a walk - not from this node's own
 * write stream (S67's estimate went blank on a node whose records all
 * arrived by the push, the sweep or the bootstrap pull).  The
 * maintenance thread walks each collection whole with the S40 cursor,
 * 1024 buckets per step under the bucket seqlock, values copied out as
 * SCAN does (the keys-only walk reports no length by design, and a
 * length-only flag would be a change to the vendored core), and
 * publishes the sum.  It paces itself on its own cost: the next walk
 * is 20x the last one's duration away, never sooner than 5 s nor later
 * than 60 s, so a large collection costs this thread at most 5%.  The
 * figure is exact when taken and a few seconds old when read. */
#define PC_HELD_MIN_S 5
#define PC_HELD_MAX_S 60

struct held_acc {
	unsigned long long sum;            /* key + value bytes */
	unsigned long long hist[8];        /* S116: value sizes, size_hist's classes */
	unsigned long long cells;          /* S120: the cells the records occupy */
};

static int held_cb(const str *key, const str *val, unsigned int expires,
		void *ctx)
{
	struct held_acc *a = ctx;
	size_t v = val->len;
	int c = 0;

	(void)expires;                     /* held until the sweep frees it */
	a->sum += (unsigned long long)key->len + (unsigned long long)val->len;
	/* classes: <=64, <=256, <=1K, <=4K, <=16K, <=64K, <=256K, more */
	while (c < 7 && v > (64u << (2 * c)))
		c++;
	a->hist[c]++;
	a->cells += pcache_arena_cell_size(PCACHE_REC_SIZE(key->len, val->len));
	return 0;
}

void pc_store_held_tick(void)
{
	unsigned int now = (unsigned int)time(NULL);
	int i;

	for (i = 0; i < reg_n; i++) {
		struct held_acc acc;
		unsigned long long us;
		unsigned int cursor = 0, span, g;
		struct timespec t0, t1;

		/* S69: a dropped collection has nothing to walk, and a
		 * tombstone read back from the collections file has no table
		 * at all */
		if (!__atomic_load_n(&reg[i].live, __ATOMIC_ACQUIRE) ||
		        !reg[i].ht)
			continue;
		if (now < reg[i].held_next)
			continue;
		memset(&acc, 0, sizeof acc);
		clock_gettime(CLOCK_MONOTONIC, &t0);
		do {
			if (pcache_ht_scan_ex(reg[i].ht, &cursor, 1024, 0,
			        held_cb, &acc) < 0)
				break;
		} while (cursor);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		/* Subtract as ONE signed nanosecond quantity.  Taking the two
		 * components apart and casting the nanosecond difference to
		 * unsigned turns every walk whose end nanoseconds are below
		 * its start - about half of them - into 1.8e16 us, which
		 * truncates to roughly 1,271 seconds.  Measured: a five-second
		 * walk reported 1,276.283 s.  It was invisible while nobody
		 * read the figure, but the PACING reads it: span is twenty
		 * times the walk's cost, so a wrapped reading pinned it to
		 * PC_HELD_MAX_S whatever the walk really took. */
		{
			long long dns =
				(long long)(t1.tv_sec - t0.tv_sec) * 1000000000LL +
				(long long)(t1.tv_nsec - t0.tv_nsec);

			us = dns > 0 ? (unsigned long long)dns / 1000ULL : 0;
		}
		/* S116: the histogram goes to the slot readers are not on */
		g = reg[i].held_gen + 1;
		memcpy(reg[i].held_hist[g & 1], acc.hist, sizeof acc.hist);
		__atomic_store_n(&reg[i].held_gen, g, __ATOMIC_RELEASE);
		__atomic_store_n(&reg[i].held_cells, acc.cells, __ATOMIC_RELEASE);
		__atomic_store_n(&reg[i].held_bytes, acc.sum, __ATOMIC_RELEASE);
		__atomic_store_n(&reg[i].held_at, now, __ATOMIC_RELEASE);
		reg[i].held_walk_us = (unsigned int)us;
		span = (unsigned int)(us * 20ULL / 1000000ULL);
		if (span < PC_HELD_MIN_S)
			span = PC_HELD_MIN_S;
		if (span > PC_HELD_MAX_S)
			span = PC_HELD_MAX_S;
		reg[i].held_next = now + span;
	}
}

/* S131: how long the last held walk took.  The pacing bounds the walk's
 * DUTY CYCLE (it sleeps twenty times its own cost, capped at 60 s) and
 * not its BLOCKING TIME: every other maintenance duty - the expiry
 * sweep, the splitter, the arena reclaim, S69's resize tick - waits
 * behind one walk, so a walk that takes 40 s is 40 s in which a table
 * cannot grow and nothing expires.  That number was recorded and never
 * reported, so the stall had no symptom but its consequences. */
unsigned int pc_store_held_walk_us(int i)
{
	if (i < 0 || i >= reg_n)
		return 0;
	return __atomic_load_n(&reg[i].held_walk_us, __ATOMIC_RELAXED);
}

unsigned long long pc_store_held(int i, unsigned int *age_s)
{
	unsigned int at;

	if (i < 0 || i >= reg_n) {
		if (age_s)
			*age_s = (unsigned int)-1;
		return 0;
	}
	at = __atomic_load_n(&reg[i].held_at, __ATOMIC_ACQUIRE);
	if (age_s)
		*age_s = at ? (unsigned int)time(NULL) - at : (unsigned int)-1;
	return __atomic_load_n(&reg[i].held_bytes, __ATOMIC_ACQUIRE);
}

/* S120: the index regions a table was carved, noted by whoever carved -
 * the daemon at creation, the maintenance thread at growth */
void pc_store_note_index(pcache_htable_t *ht, unsigned long bytes)
{
	int i;

	for (i = 0; i < reg_n; i++)
		if (reg[i].ht == ht) {
			reg[i].index_bytes += bytes;
			return;
		}
}

unsigned long pc_store_index_bytes(int i)
{
	return (i < 0 || i >= reg_n) ? 0 : reg[i].index_bytes;
}

unsigned long long pc_store_held_cells(int i)
{
	if (i < 0 || i >= reg_n)
		return 0;
	return __atomic_load_n(&reg[i].held_cells, __ATOMIC_ACQUIRE);
}

/* S116: the value-size histogram of the last walk; -1 before the first
 * walk (the page shows a dash then, as it does for held). */
int pc_store_held_hist(int i, unsigned long long hist[8])
{
	unsigned int g;

	if (i < 0 || i >= reg_n ||
	    !__atomic_load_n(&reg[i].held_at, __ATOMIC_ACQUIRE))
		return -1;
	g = __atomic_load_n(&reg[i].held_gen, __ATOMIC_ACQUIRE);
	memcpy(hist, reg[i].held_hist[g & 1], 8 * sizeof hist[0]);
	return 0;
}

int pc_store_size_stats(int i, unsigned long long *bytes,
		unsigned long long *n, unsigned long long hist[8])
{
	int c, s;

	if (i < 0 || i >= reg_n)
		return -1;
	*bytes = *n = 0;
	for (c = 0; c < 8; c++)
		hist[c] = 0;
	for (s = 0; s < COLST_STRIPES; s++) {      /* S237 */
		*bytes += __atomic_load_n(&colst[i][s].bytes, __ATOMIC_RELAXED);
		*n += __atomic_load_n(&colst[i][s].n, __ATOMIC_RELAXED);
		for (c = 0; c < 8; c++)
			hist[c] += __atomic_load_n(&colst[i][s].hist[c],
				__ATOMIC_RELAXED);
	}
	return 0;
}

int pc_store_register(const char *name, pcache_htable_t *ht, int pull,
		int proxy, int shard, int eager, int buckets_log2)
{
	if (reg_n >= PC_MAX_COLLECTIONS || !name || strlen(name) >= PC_COL_NAME_MAX)
		return -1;
	reg[reg_n].name = strdup(name);
	if (!reg[reg_n].name)
		return -1;
	reg[reg_n].live = 1;               /* S69 */
	reg[reg_n].ht = ht;
	/* proxy AND shard reads ride the pull path (unicast to the holder
	 * / owner); shard needs no locator and no broadcast. */
	reg[reg_n].pull = pull || proxy || shard;
	reg[reg_n].proxy = proxy;
	reg[reg_n].shard = shard;
	reg[reg_n].eager = eager;
	reg[reg_n].buckets_log2 = buckets_log2;                      /* S69 */
	reg[reg_n].floor_log2 = buckets_log2;                        /* S328 */
	reg[reg_n].autoscale = pc_autoscale_default;                 /* S239 */
	reg[reg_n].redis_types = pc_types_default;                   /* S280 */
	reg[reg_n].as_target = reg[reg_n].as_set = 0;
	__atomic_store_n(&reg_n, reg_n + 1, __ATOMIC_RELEASE);
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

/* S305: an EAGER collection always pulls.  The fleet-wide layout got it
 * by accident of representation - [cluster] mode = eager is store mode
 * plus the eager flag, and store stamps pull = 1 - while a collection's
 * own mode = eager, and one created at runtime (pc_col_default_pull is
 * proxy || shard), never did.  Such a node, restarted after a crash,
 * lacks every copy it received (they are off its WAL) until the sweep
 * re-sends them, and answered each of those misses "absent" (300 of 300
 * in eagerrestarttest part 1).  Steady state is unchanged: S303 answers
 * a settled eager miss before any pull is considered. */
int pc_store_pull_enabled(pcache_htable_t *ht)
{
	int i;

	for (i = 0; i < reg_n; i++)
		if (reg[i].ht == ht)
			return reg[i].pull || reg[i].eager;
	return 0;
}

pcache_htable_t *pc_store_find(const char *name, size_t nlen)
{
	int i, n = __atomic_load_n(&reg_n, __ATOMIC_ACQUIRE);

	for (i = 0; i < n; i++) {
		const char *nm;

		if (!__atomic_load_n(&reg[i].live, __ATOMIC_ACQUIRE))
			continue;
		nm = __atomic_load_n(&reg[i].name, __ATOMIC_ACQUIRE);
		if (nm && strlen(nm) == nlen && !memcmp(nm, name, nlen))
			return __atomic_load_n(&reg[i].ht, __ATOMIC_ACQUIRE);
	}
	return NULL;
}

/* S69: rename a collection.  One pointer swap, so a lookup sees the old
 * name or the new one and never a half-written string.
 *
 * The WAL is why the collections FILE is not updated here: records
 * written before the rename are logged under the OLD name, so a replay
 * must meet that name before the rename record moves it.  The file
 * therefore keeps naming the collection as it was at the start of the
 * replay window, and is brought forward when a checkpoint retires that
 * window (pc_store_persist is called after a successful snapshot) or at
 * once when no WAL is running at all.  0 ok; -1 no such collection;
 * -2 the new name is taken; -3 out of memory or a bad name. */
int pc_store_rename(const char *from, size_t flen, const char *to, size_t tlen,
		unsigned long long gen)
{
	int i, n, found = -1;
	char *nm;

	if (tlen == 0 || tlen >= PC_COL_NAME_MAX)
		return -3;
	pthread_mutex_lock(&reg_mx);
	n = reg_n;
	for (i = 0; i < n; i++) {
		if (!reg[i].live || !reg[i].name)
			continue;
		if (strlen(reg[i].name) == tlen && !memcmp(reg[i].name, to, tlen)) {
			pthread_mutex_unlock(&reg_mx);
			return -2;
		}
		if (strlen(reg[i].name) == flen && !memcmp(reg[i].name, from, flen))
			found = i;
	}
	if (found < 0) {
		pthread_mutex_unlock(&reg_mx);
		return -1;
	}
	nm = malloc(tlen + 1);
	if (!nm) {
		pthread_mutex_unlock(&reg_mx);
		return -3;
	}
	memcpy(nm, to, tlen);
	nm[tlen] = '\0';
	__atomic_store_n(&reg[found].name, nm, __ATOMIC_RELEASE);
	reg[found].runtime = 1;            /* it is ours to persist now */
	reg[found].gen = gen;
	pthread_mutex_unlock(&reg_mx);
	return 0;
}

int pc_store_count(void)              { return __atomic_load_n(&reg_n, __ATOMIC_ACQUIRE); }
const char *pc_store_name(int i)      { return reg[i].name; }
pcache_htable_t *pc_store_ht(int i)   { return reg[i].ht; }
/* S153: the registry slot a table belongs to, -1 if none */
/* The published table of a live collection AND its publish generation,
 * read together under the registry lock.  Lock-free, the two can
 * disagree: a resize swap bumps `pub` BEFORE it stores the new pointer,
 * so a reader between the two pairs the new generation with the old
 * table.  `dump` hands the generation to its caller as the token every
 * later chunk is checked against, so the pair must be exact - one lock
 * per chunk, not per record. */
pcache_htable_t *pc_store_find_pub(const char *name, size_t nlen,
		unsigned long long *pub)
{
	pcache_htable_t *ht = NULL;
	int i;

	pthread_mutex_lock(&reg_mx);
	for (i = 0; i < reg_n; i++)
		if (reg[i].live && reg[i].name && strlen(reg[i].name) == nlen &&
		        !memcmp(reg[i].name, name, nlen)) {
			ht = reg[i].ht;
			*pub = reg[i].pub;
			break;
		}
	pthread_mutex_unlock(&reg_mx);
	return ht;
}

/* The slot that publishes @ht AND that slot's publish generation, read
 * together under the registry lock; -1 when no slot publishes @ht any
 * more (it was swapped out or dropped since the caller resolved it).  A
 * KEYS walk takes its handle here: slot_of then pub_gen, lock-free, could
 * pair the OLD table with the NEW generation across a swap, and that walk
 * would read the retired table to the end without ever seeing the swap. */
int pc_store_slot_pub(pcache_htable_t *ht, unsigned long long *pub)
{
	int i, slot = -1;

	pthread_mutex_lock(&reg_mx);
	for (i = 0; i < reg_n; i++)
		if (reg[i].live && reg[i].ht == ht) {
			slot = i;
			*pub = reg[i].pub;
			break;
		}
	pthread_mutex_unlock(&reg_mx);
	return slot;
}

int pc_store_slot_of(pcache_htable_t *ht)
{
	int i, n = pc_store_count();

	for (i = 0; i < n; i++)
		if (reg[i].ht == ht)
			return i;
	return -1;
}
int pc_store_live(int i)              { return __atomic_load_n(&reg[i].live, __ATOMIC_ACQUIRE); }
int pc_store_buckets_log2(int i)      { return reg[i].buckets_log2; }
int pc_store_floor_log2(int i)        { return reg[i].floor_log2; }    /* S328 */

/* S328: an operator's explicit resize is the new floor (the resize verb
 * here, the fleet's announce in pc_store_resize_request) - an automatic
 * resize never calls this, so FLOOR cannot ratchet itself up or down */
void pc_store_set_floor(const char *name, size_t nlen, int log2)
{
	int i;

	pthread_mutex_lock(&reg_mx);
	for (i = 0; i < reg_n; i++)
		if (reg[i].live && strlen(reg[i].name) == nlen &&
		        !memcmp(reg[i].name, name, nlen)) {
			reg[i].floor_log2 = log2;
			break;
		}
	pthread_mutex_unlock(&reg_mx);
}
int pc_store_runtime(int i)           { return reg[i].runtime; }

/* S69 ------------------------------------------------------------------
 * Runtime creation and removal.  Everything below runs on a worker while
 * the daemon serves, so the ordering matters more than the code length:
 * a new entry is complete before the count that publishes it, and a
 * dropped entry stops being findable before a single record is freed. */

static int reg_slot_for(const char *name, size_t nlen)
{
	int i;

	for (i = 0; i < reg_n; i++) {
		if (reg[i].live) {
			if (strlen(reg[i].name) == nlen &&
			        !memcmp(reg[i].name, name, nlen))
				return -1;         /* the name is taken */
			continue;
		}
		/* S150 C: a tombstone's table went back to the arena once its
		 * readers had parked (pc_store_retire_tick); a dead slot is
		 * reusable once it holds no table.  The size no longer matters:
		 * the carve takes retired slots before it moves the frontier. */
		if (!reg[i].ht)
			return i;
	}
	return reg_n < PC_MAX_COLLECTIONS ? reg_n : -2;
}

int pc_store_create(const char *name, size_t nlen, int buckets_log2)
{
	return pc_store_create_gen(name, nlen, buckets_log2, pc_lamport_tick());
}

/* S69: the same, with the generation supplied - a create arriving from
 * the fleet keeps the originator's stamp so every node orders it the
 * same way.  A name whose tombstone is NEWER is left dropped. */
int pc_store_create_gen(const char *name, size_t nlen, int buckets_log2,
		unsigned long long gen)
{
	pcache_htable_t *ht;
	unsigned long rg0;
	int i, fresh;

	if (nlen == 0 || nlen >= PC_COL_NAME_MAX || buckets_log2 < 4 ||
	        buckets_log2 > 24)
		return -1;
	pthread_mutex_lock(&reg_mx);
	/* a drop this node already knows about, stamped later than this
	 * create: the create is the stale news of the two */
	for (i = 0; i < reg_n; i++)
		if (!reg[i].live && strlen(reg[i].name) == nlen &&
		        !memcmp(reg[i].name, name, nlen) && reg[i].gen > gen) {
			pthread_mutex_unlock(&reg_mx);
			return -4;
		}
	i = reg_slot_for(name, nlen);
	if (i < 0) {
		pthread_mutex_unlock(&reg_mx);
		return i == -2 ? -2 : -1;
	}
	/* S150 C: always a fresh carve - the arena serves it from retired
	 * slots before it moves the frontier */
	fresh = 1;
	rg0 = pcache_arena_regions_bytes();
	ht = pcache_htable_new((unsigned int)buckets_log2);
	if (!ht) {
		pthread_mutex_unlock(&reg_mx);
		return -3;
	}
	{
		char *nm = malloc(nlen + 1);

		if (!nm) {
			pthread_mutex_unlock(&reg_mx);
			return -3;
		}
		memcpy(nm, name, nlen);
		nm[nlen] = '\0';
		__atomic_store_n(&reg[i].name, nm, __ATOMIC_RELEASE);
	}
	reg[i].ht = ht;
	reg[i].pub++;                      /* S150 B: a publish */
	reg[i].buckets_log2 = buckets_log2;
	reg[i].floor_log2 = buckets_log2;              /* S328 */
	reg[i].runtime = 1;
	pc_ev_configure(i, PC_EV_INHERIT);              /* S153 */
	pc_ev_configure_notify(i, PC_EV_INHERIT);       /* PS8 */
	reg[i].gen = gen;
	/* the fleet is one mode over one collection set: a created collection
	 * takes the cluster's, never a per-collection variant of its own */
	reg[i].pull = pc_col_default_pull;
	reg[i].proxy = pc_col_default_proxy;
	reg[i].shard = pc_col_default_shard;
	reg[i].eager = pc_col_default_eager;
	reg[i].autoscale = pc_autoscale_default;        /* S239 */
	reg[i].redis_types = pc_types_default;          /* S280 */
	reg[i].rs_pending = 0;                          /* S255 */
	reg[i].rs_gen = 0;
	reg[i].as_target = reg[i].as_set = 0;
	colst_reset(i);
	reg[i].held_bytes = 0;
	reg[i].held_at = reg[i].held_next = reg[i].held_walk_us = 0;
	memset(reg[i].held_hist, 0, sizeof reg[i].held_hist);
	reg[i].held_cells = 0;
	if (fresh) {
		reg[i].index_bytes = pcache_arena_regions_bytes() - rg0;
		if (i == reg_n) {
			__atomic_store_n(&reg[i].live, 1, __ATOMIC_RELEASE);
			__atomic_store_n(&reg_n, reg_n + 1, __ATOMIC_RELEASE);
			pthread_mutex_unlock(&reg_mx);
			return 0;
		}
	}
	__atomic_store_n(&reg[i].live, 1, __ATOMIC_RELEASE);
	pthread_mutex_unlock(&reg_mx);
	return 0;
}


struct drop_walk {
	pcache_htable_t *ht;
	long n;
};


static int drop_cb(const str *key, const str *val, unsigned int exp, void *p)
{
	struct drop_walk *d = p;

	(void)val;
	(void)exp;
	if (pcache_ht_remove(d->ht, key) == 1)
		d->n++;
	return 0;
}

/* S69: empty a table.  Removing a record DURING a walk moved its
 * neighbours, so a single pass skipped some of them - a 20,000-record
 * table came out 17,452 removed the first time this ran - and a table
 * left holding records is a leak whose cells never return, and worse,
 * records that would reappear if the slot were reused.  So: passes until
 * one removes nothing.
 * S199: that skip was the walk's, not this caller's - the same defect
 * cost the proxy migration 164 of 400 records on rc16 (S198) - and the
 * walk now hands over every record in one pass (scanremovetest).  The
 * loop stays: it is cheap, and a drop that races a writer still wants
 * a second look. */
static long drain_table(pcache_htable_t *ht)
{
	struct drop_walk d;
	long total = 0;

	d.ht = ht;
	for (;;) {
		unsigned int cursor = 0;

		d.n = 0;
		do {
			if (pcache_ht_scan_ex(ht, &cursor, 4096,
			        PCACHE_SCAN_NOVAL, drop_cb, &d) < 0)
				break;
		} while (cursor);
		if (d.n == 0)
			return total;
		total += d.n;
	}
}

long pc_store_drop(const char *name, size_t nlen)
{
	return pc_store_drop_gen(name, nlen, pc_lamport_tick());
}

long pc_store_drop_gen(const char *name, size_t nlen, unsigned long long gen)
{
	int i, n, found = -1;

	pthread_mutex_lock(&reg_mx);
	n = reg_n;
	for (i = 0; i < n; i++)
		if (reg[i].live && strlen(reg[i].name) == nlen &&
		        !memcmp(reg[i].name, name, nlen)) {
			found = i;
			break;
		}
	if (found < 0) {
		pthread_mutex_unlock(&reg_mx);
		return -1;
	}
	/* off the map first: from here every lookup says "no such
	 * collection", so nothing new lands in the table being emptied */
	__atomic_store_n(&reg[found].live, 0, __ATOMIC_RELEASE);
	reg[found].pub++;                  /* S150 B: a publish */
	reg[found].runtime = 0;
	reg[found].gen = gen;              /* the tombstone outranks the create */
	pthread_mutex_unlock(&reg_mx);

	/* ordinary removes: a walk another thread started on this table sees
	 * it empty under it, which is the concurrent case it already
	 * handles.  Nothing is freed out from under a pointer - the table
	 * and its index stay carved for the next create of this size. */
	{
		long left = drain_table(reg[found].ht);

		/* S150 C: the table goes back to the arena once every thread
		 * that could still hold a pointer into it has parked; until
		 * then the slot keeps it, and reg_slot_for passes the slot by */
		pthread_mutex_lock(&reg_mx);
		retire_push(reg[found].ht, found);
		pthread_mutex_unlock(&reg_mx);
		return left;
	}
}

/* S150 B+C: tables the registry no longer publishes - what a resize
 * left, a dropped collection's - waiting for every thread that could
 * hold a pointer into them to park (pc_qs_clear).  A cleared table's
 * regions go back to the arena (pcache_htable_retire), its charge comes
 * off the collection, and the slot that still named it forgets it. */
#define RETQ_MAX (PC_MAX_COLLECTIONS * 2)
static struct {
	pcache_htable_t *ht;
	unsigned long long stamp;
	int idx;                       /* the registry slot it belonged to */
} retq[RETQ_MAX];
static unsigned int retq_n;
static unsigned long long retq_cleared, retq_overflow, retq_returned;

static void retire_push(pcache_htable_t *ht, int idx)   /* reg_mx held */
{
	if (retq_n == RETQ_MAX) {
		retq_overflow++;           /* let go unwaited and unreturned */
		return;
	}
	retq[retq_n].ht = ht;
	retq[retq_n].idx = idx;
	retq[retq_n].stamp = pc_qs_stamp();
	__atomic_store_n(&retq_n, retq_n + 1, __ATOMIC_RELEASE);
}

void pc_store_retire_tick(void)
{
	unsigned int i = 0;

	pthread_mutex_lock(&reg_mx);
	while (i < retq_n) {
		pcache_htable_t *ht = retq[i].ht;
		int idx = retq[i].idx;
		unsigned long freed;

		if (!pc_qs_clear(retq[i].stamp)) {
			i++;
			continue;
		}
		retq[i] = retq[retq_n - 1];
		__atomic_store_n(&retq_n, retq_n - 1, __ATOMIC_RELEASE);
		if (idx >= 0 && idx < reg_n && reg[idx].ht == ht)
			reg[idx].ht = NULL;    /* a tombstone: the slot is reusable */
		freed = pcache_htable_retire(ht);
		if (idx >= 0 && idx < reg_n)
			reg[idx].index_bytes -= freed < reg[idx].index_bytes
				? freed : reg[idx].index_bytes;
		retq_returned += freed;
		__atomic_fetch_add(&retq_cleared, 1, __ATOMIC_RELAXED);
	}
	pthread_mutex_unlock(&reg_mx);
}

void pc_store_retire_figures(unsigned int *pending, unsigned long long *cleared,
		unsigned long long *returned)
{
	*pending = __atomic_load_n(&retq_n, __ATOMIC_ACQUIRE);
	*cleared = __atomic_load_n(&retq_cleared, __ATOMIC_RELAXED);
	*returned = __atomic_load_n(&retq_returned, __ATOMIC_RELAXED);
}

unsigned long long pc_store_pub_gen(int i)
{
	return __atomic_load_n(&reg[i].pub, __ATOMIC_ACQUIRE);
}


/* S69 ------------------------------------------------------------------
 * Resize, in either direction, by migration.
 *
 * Two passes fill the new table from the live one, the registry pointer
 * is swapped, and two more passes follow it.  The version rule does the
 * hard part: a record copied twice is refused the second time, and a
 * record a client wrote to the NEW table after the swap outranks the
 * copy the migration is still carrying, so the passes cannot undo a live
 * write.  Deletes go to both tables while this runs (see
 * pc_store_resize_shadow), because a key deleted after the copier passed
 * it would otherwise be resurrected by the swap.
 *
 * The budget is records per tick, not buckets: a table's records are
 * what the copy costs, and the maintenance thread has an expiry sweep
 * and a give-back to run in the same second. */
#define RS_PRE_PASSES   2
#define RS_POST_PASSES  2
#define RS_BUDGET   50000

struct rs_ctx {
	unsigned long expired;         /* S184: dropped for having expired */
	pcache_htable_t *to;
	unsigned int now, budget, n;
	unsigned long long moved;
};

static int rs_copy_cb(const str *key, const str *val,
		const struct pcache_rec_meta *mt, void *p)
{
	struct rs_ctx *c = p;

	if (c->n >= c->budget)
		return -1;                 /* this tick's share is spent */
	c->n++;
	if (mt->expires && mt->expires <= c->now) {
		/* S184: it does not travel - and it is GONE, so say so.  A
		 * resize that drops 16,726 expired records used to report
		 * nothing at all: the sweep had not reaped them, and the
		 * new table never saw them. */
		c->expired++;
		return 0;
	}
	/* the record's own version and expiry, PASSIVE if it is a replica,
	 * and its TYPE (S280) - a copy that arrives at the new table must be
	 * the same record, not a fresh local write */
	/* S275: and its WRITE TICK - which says whether it is a recovered
	 * record the reconcile has not ruled on (S245).  Restamped, a
	 * replayed key the fleet had deleted read as freshly written and the
	 * sweep pushed it back to every peer: measured, 236 of 1500 deleted
	 * keys back on the whole fleet after a shrink mid-reconcile. */
	if (pcache_ht_store_copy(c->to, key, val, mt->expires,
	        (unsigned char)(mt->rflags & (PCACHE_F_PASSIVE | PCACHE_F_TYPES)),
	        mt->ver, mt->wtick) == 0)
		c->moved++;
	return 0;
}

int pc_store_resize_start(const char *name, size_t nlen, int target_log2)
{
	pcache_ht_totals_t tot;
	pcache_htable_t *to;
	unsigned long rg0;
	int i, n, found = -1;

	if (target_log2 < 4 || target_log2 > 24)
		return -3;
	pthread_mutex_lock(&reg_mx);
	n = reg_n;
	for (i = 0; i < n; i++)
		if (reg[i].live && strlen(reg[i].name) == nlen &&
		        !memcmp(reg[i].name, name, nlen)) {
			found = i;
			break;
		}
	if (found < 0) {
		pthread_mutex_unlock(&reg_mx);
		return -1;
	}
	if (reg[found].rs_to) {
		pthread_mutex_unlock(&reg_mx);
		return -2;
	}
	/* S150 D: the size it was MADE at is not the size it has grown to -
	 * a table split up from 2^12 may be resized back to 2^12 */
	if (reg[found].buckets_log2 == target_log2 &&
	    pcache_ht_nbuckets(reg[found].ht) <= (1U << target_log2)) {
		pthread_mutex_unlock(&reg_mx);
		return -5;
	}
	/* a target the splitter would immediately undo is refused rather
	 * than performed and lost: it grows while a bucket holds more than
	 * four records */
	pcache_ht_totals(reg[found].ht, &tot);
	if (tot.entries > ((unsigned long long)4 << target_log2)) {
		pthread_mutex_unlock(&reg_mx);
		return -3;
	}
	rg0 = pcache_arena_regions_bytes();
	to = pcache_htable_new((unsigned int)target_log2);
	if (!to) {
		pthread_mutex_unlock(&reg_mx);
		return -4;
	}
	/* S331: the upstream shadow goes with the collection, not the table:
	 * a resize (autoscale's included) must not stop read-through */
	pcache_ht_set_shadow(to, __atomic_load_n(&reg[found].ht->shadow,
		__ATOMIC_ACQUIRE));
	pcache_ht_set_shadow_gate(to, reg[found].ht->shadow_ok,
		reg[found].ht->shadow_ctx);
	reg[found].index_bytes += pcache_arena_regions_bytes() - rg0;
	reg[found].rs_to = to;
	reg[found].rs_target = target_log2;
	reg[found].rs_cursor = 0;
	reg[found].rs_pass = 0;
	reg[found].rs_swapped = 0;
	reg[found].rs_moved = 0;
	pthread_mutex_unlock(&reg_mx);
	return 0;
}

/*
 * S255: a resize the FLEET asked for (handle_col_set), as opposed to one
 * a client asked this node for.  Two things pc_store_resize_start cannot
 * do for it:
 *   - order: @gen is the announce's generation, strictly rising (the
 *     sender ticks its clock for it).  One older than the newest applied
 *     is a late or repeated announce of a resize since superseded, and is
 *     ignored - which is what lets a sender repeat its announce;
 *   - wait: a node still running the previous resize used to REFUSE the
 *     new one, silently (-2, discarded), and kept the old size for good
 *     while the rest of the fleet moved on.  It is held instead, and
 *     started when the running one finishes; a newer one replaces it.
 * Returns what pc_store_resize_start returns, 1 for ignored (older), 2
 * for held.
 */
int pc_store_resize_request(const char *name, size_t nlen, int target_log2,
		unsigned long long gen)
{
	int i, rc;

	pthread_mutex_lock(&reg_mx);
	for (i = 0; i < reg_n; i++)
		if (reg[i].live && strlen(reg[i].name) == nlen &&
		        !memcmp(reg[i].name, name, nlen))
			break;
	if (i == reg_n) {
		pthread_mutex_unlock(&reg_mx);
		return -1;
	}
	if (gen && gen < reg[i].rs_gen) {
		pthread_mutex_unlock(&reg_mx);
		return 1;
	}
	if (gen)
		reg[i].rs_gen = gen;
	/* S328: the fleet's resize is an operator's, so its size becomes the
	 * floor here too - once it is ACCEPTED, started or held; a refused
	 * one moves nothing */
	if (reg[i].rs_to) {
		reg[i].rs_pending = target_log2;
		reg[i].floor_log2 = target_log2;
		pthread_mutex_unlock(&reg_mx);
		return 2;
	}
	pthread_mutex_unlock(&reg_mx);
	rc = pc_store_resize_start(name, nlen, target_log2);
	if (rc == -2) {                    /* one started in between */
		pthread_mutex_lock(&reg_mx);
		reg[i].rs_pending = target_log2;
		reg[i].floor_log2 = target_log2;
		pthread_mutex_unlock(&reg_mx);
		return 2;
	}
	if (rc == 0)
		pc_store_set_floor(name, nlen, target_log2);
	return rc;
}

/* S255: a resize this node started for a client and announced at @gen */
void pc_store_resize_gen_note(const char *name, size_t nlen,
		unsigned long long gen)
{
	int i;

	pthread_mutex_lock(&reg_mx);
	for (i = 0; i < reg_n; i++)
		if (reg[i].live && strlen(reg[i].name) == nlen &&
		        !memcmp(reg[i].name, name, nlen)) {
			if (gen > reg[i].rs_gen)
				reg[i].rs_gen = gen;
			reg[i].rs_pending = 0;     /* a client's own call is newest */
			break;
		}
	pthread_mutex_unlock(&reg_mx);
}

pcache_htable_t *pc_store_resize_shadow(pcache_htable_t *ht)
{
	int i, n = pc_store_count();

	for (i = 0; i < n; i++)
		if (reg[i].ht == ht)
			return __atomic_load_n(&reg[i].rs_to, __ATOMIC_ACQUIRE);
	return NULL;
}

/*
 * S257: a client's scan across a resize.
 *
 * pcache_ht_scan_ex's cursor is a bucket index into the table it is given,
 * and its contract assumed tables only grow in place (3.4).  Since S69 a
 * resize is a MIGRATION and a swap, either way: after a shrink a cursor
 * past the new table's size walked nothing and reported the scan complete
 * (the spread census: node 1 at 294k keys of 641k), after a grow it
 * indexed buckets holding other keys.  Here the store, which knows the
 * swap, packs the cursor:
 *   bit  31     the overflow-leg flag, where it always was (S121 -
 *               PCACHE_CURSOR_OVF; legwalktest reads it off the wire)
 *   bits 30-28  the table's generation, 1..7 (a swap changes it; a cursor
 *               from another generation restarts at 0 - SCAN may repeat a
 *               key, it may not skip one)
 *   bit  27     walking the OLD table of a swapped resize still draining
 *   bits 26-0   the inner bucket / chain index
 * While a swapped resize drains, the walk takes the old table FIRST, then
 * the live one: a record the drain moves during the walk is seen before it
 * leaves or after it arrives.  Before the swap the live table holds every
 * record, and the swap itself restarts the walk.
 */
#define SC_GEN_SHIFT 28
#define SC_GEN_MASK  0x70000000u
#define SC_OLD       0x08000000u
#define SC_IDX       0x07FFFFFFu

int pc_store_scan(pcache_htable_t *ht, unsigned int *cursor,
		unsigned int max_buckets, unsigned int flags,
		pcache_iter_cb cb, void *ctx)
{
	pcache_htable_t *live, *old, *t;
	unsigned int gen, inner, c = *cursor;
	int i, n = pc_store_count(), on_old, rc;

	for (i = 0; i < n; i++)
		if (reg[i].ht == ht || reg[i].rs_to == ht)
			break;
	if (i == n)                        /* not a registered table */
		return pcache_ht_scan_ex(ht, cursor, max_buckets, flags, cb, ctx);
	pthread_mutex_lock(&reg_mx);
	gen = (unsigned int)(reg[i].pub % 7) + 1;
	live = reg[i].ht;
	old = reg[i].rs_swapped ? reg[i].rs_to : NULL;
	pthread_mutex_unlock(&reg_mx);
	if (c && ((c & SC_GEN_MASK) >> SC_GEN_SHIFT) != gen)
		c = 0;                         /* a swap since: from the top */
	if (!c) {
		on_old = old != NULL;
		inner = 0;
	} else {
		on_old = (c & SC_OLD) != 0;
		inner = (c & SC_IDX) | (c & PCACHE_CURSOR_OVF);
	}
	if (on_old && !old) {              /* drained: all of it is live now */
		on_old = 0;
		inner = 0;
	}
	t = on_old ? old : live;
	rc = pcache_ht_scan_ex(t, &inner, max_buckets, flags, cb, ctx);
	if ((inner & ~PCACHE_CURSOR_OVF) > SC_IDX)
		return -1;                     /* a table past 2^27 buckets */
	if (!inner) {
		if (!on_old) {
			*cursor = 0;               /* complete */
			return rc;
		}
		/* the old table is done: the live one, from its start - gen
		 * is never 0, so this cursor is never read as "complete" */
		*cursor = gen << SC_GEN_SHIFT;
		return rc;
	}
	*cursor = (gen << SC_GEN_SHIFT) | (on_old ? SC_OLD : 0) |
		(inner & PCACHE_CURSOR_OVF) | (inner & ~PCACHE_CURSOR_OVF);
	return rc;
}

int pc_store_resizing(int i, int *target_log2, unsigned long long *moved)
{
	if (i < 0 || i >= pc_store_count() || !reg[i].rs_to)
		return 0;
	*target_log2 = reg[i].rs_target;
	*moved = reg[i].rs_moved;
	return 1;
}

void pc_store_resize_tick(void)
{
	int i, n = pc_store_count();

	for (i = 0; i < n; i++) {
		struct rs_ctx c;
		pcache_htable_t *from, *to;
		int pend = 0;

		if (!__atomic_load_n(&reg[i].live, __ATOMIC_ACQUIRE) ||
		        !reg[i].rs_to)
			continue;
		/* before the swap: from the live table into the new one.
		 * After it the two have changed places - rs_to holds the OLD
		 * table - and the passes drain the old into the live one. */
		from = reg[i].rs_swapped ? reg[i].rs_to : reg[i].ht;
		to = reg[i].rs_swapped ? reg[i].ht : reg[i].rs_to;
		c.to = to;
		c.now = (unsigned int)get_ticks();
		c.budget = RS_BUDGET;
		c.n = 0;
		c.moved = 0;
		c.expired = 0;
		if (pcache_ht_iter_meta_from(from, &reg[i].rs_cursor,
		        rs_copy_cb, &c) < 0 && !c.n)
			continue;              /* the walk failed: try next tick */
		reg[i].rs_moved += c.moved;
		if (c.expired)                 /* S184: on the LIVE table */
			pcache_ht_note_expired(reg[i].ht, c.expired);
		if (reg[i].rs_cursor)
			continue;              /* mid-pass */
		reg[i].rs_pass++;
		if (!reg[i].rs_swapped) {
			if (reg[i].rs_pass < RS_PRE_PASSES)
				continue;
			/* THE COMMIT POINT: one store, and every lookup from
			 * here lands in the new table.  The old one keeps
			 * taking the writes of requests that resolved before
			 * this instant, which the passes below collect. */
			pthread_mutex_lock(&reg_mx);
			{
				pcache_htable_t *old = reg[i].ht;
				pcache_ht_totals_t oa, oc;

				/* S184: the new table's counters start at
				 * zero, so a collection's lifetime figures
				 * reset every time it resized.  Carry them
				 * before the swap, while `old` is still the
				 * published table. */
				pcache_ht_totals(old, &oa);
				pcache_ht_totals_client(old, &oc);
				pcache_ht_carry_totals(reg[i].rs_to, &oa, &oc);

				reg[i].pub++;      /* S150 B: a publish */
				__atomic_store_n(&reg[i].ht, reg[i].rs_to,
					__ATOMIC_RELEASE);
				__atomic_store_n(&reg[i].rs_to, old,
					__ATOMIC_RELEASE);
			}
			reg[i].buckets_log2 = reg[i].rs_target;
			reg[i].rs_swapped = 1;
			reg[i].rs_pass = 0;
			pthread_mutex_unlock(&reg_mx);
			LM_NOTICE("collection '%s': resized to 2^%d buckets, "
				"%llu record(s) carried; draining what the swap "
				"left behind\n", reg[i].name, reg[i].rs_target,
				reg[i].rs_moved);
			continue;
		}
		if (reg[i].rs_pass < RS_POST_PASSES)
			continue;
		/* done: the old table is emptied and kept carved for the next
		 * create or resize of its size */
		{
			pcache_htable_t *old = reg[i].rs_to;
			long left = drain_table(old);

			pthread_mutex_lock(&reg_mx);
			__atomic_store_n(&reg[i].rs_to, (pcache_htable_t *)NULL,
				__ATOMIC_RELEASE);
			reg[i].rs_swapped = 0;
			reg[i].rs_cursor = 0;
			/* S150 B: unpublished on both paths now (the swap took it
			 * off `ht`, this took it off the shadow); it waits for the
			 * threads that may still hold it, then goes back (C) */
			retire_push(old, i);
			pend = reg[i].rs_pending;      /* S255 */
			reg[i].rs_pending = 0;
			pthread_mutex_unlock(&reg_mx);
			LM_NOTICE("collection '%s': resize complete at 2^%d "
				"buckets (%llu carried, %ld left behind in the "
				"old table)\n", reg[i].name,
				reg[i].buckets_log2, reg[i].rs_moved, left);
			/* S255: a fleet resize that found this one running */
			if (pend && pend != reg[i].buckets_log2) {
				int prc = pc_store_resize_start(reg[i].name,
					strlen(reg[i].name), pend);

				LM_NOTICE("collection '%s': the fleet's resize to "
					"2^%d, held while this one ran, %s (%d)\n",
					reg[i].name, pend, prc == 0 ? "starts now"
					: "was refused", prc);
			}
		}
		pc_store_persist();
	}
}

/* S69: remember that this NAME was dropped at @gen, without there being
 * a table to empty - a tombstone read back from the file, or a drop
 * announced by a peer for a collection this node never had. */
void pc_store_note_drop(const char *name, size_t nlen, unsigned long long gen)
{
	int i;

	if (nlen == 0 || nlen >= PC_COL_NAME_MAX)
		return;
	pthread_mutex_lock(&reg_mx);
	for (i = 0; i < reg_n; i++)
		if (strlen(reg[i].name) == nlen &&
		        !memcmp(reg[i].name, name, nlen)) {
			if (reg[i].gen <= gen)
				reg[i].gen = gen;
			pthread_mutex_unlock(&reg_mx);
			return;
		}
	if (reg_n < PC_MAX_COLLECTIONS) {
		char *nm = malloc(nlen + 1);

		if (!nm) {
			pthread_mutex_unlock(&reg_mx);
			return;
		}
		memcpy(nm, name, nlen);
		nm[nlen] = '\0';
		reg[reg_n].name = nm;
		reg[reg_n].gen = gen;
		reg[reg_n].live = 0;
		reg[reg_n].ht = NULL;
		__atomic_store_n(&reg_n, reg_n + 1, __ATOMIC_RELEASE);
	}
	pthread_mutex_unlock(&reg_mx);
}

/* S69: the created set, for a peer that has just come up - name,
 * size, generation and whether it is a tombstone.  Returns 0 while @i is
 * in range. */
int pc_store_entry(int i, const char **name, int *buckets_log2,
		unsigned long long *gen, int *live)
{
	if (i < 0 || i >= pc_store_count() || !reg[i].gen)
		return -1;                 /* not a runtime name: nothing to say */
	*name = reg[i].name;
	*buckets_log2 = reg[i].buckets_log2;
	*gen = reg[i].gen;
	*live = reg[i].live;
	return 0;
}

/* ---- the created set on disk ----------------------------------------
 *
 *   perfcached-collections 1
 *   collection sbcha 18
 *   collection sessions 12
 *
 * Written whole and renamed over the old file, so a torn write leaves the
 * previous set rather than half of this one.  Only runtime creations are
 * listed: a collection from the config file belongs to the config file,
 * and writing it here would resurrect it after the operator removed it. */
#define COLFILE_HEADER "perfcached-collections 1"

static char colfile_dir[512];   /* remembered by pc_store_load() */

int pc_store_persist(void)
{
	char path[600], tmp[620];
	FILE *f;
	int i, n = pc_store_count(), rc = 0;

	if (!colfile_dir[0])
		return 0;                  /* ephemeral by configuration */
	snprintf(path, sizeof path, "%s/collections", colfile_dir);
	snprintf(tmp, sizeof tmp, "%s/collections.tmp", colfile_dir);
	f = fopen(tmp, "w");
	if (!f)
		return -1;
	fprintf(f, "%s\n", COLFILE_HEADER);
	pthread_mutex_lock(&reg_mx);
	for (i = 0; i < n; i++) {
		if (!reg[i].gen)
			continue;              /* never touched at runtime */
		if (reg[i].live && !reg[i].runtime)
			continue;              /* the config file's, not ours */
		/* tombstones are written too: a drop that a peer missed must
		 * not be undone by that peer's re-announce after a restart */
		fprintf(f, "collection %s %d %llu %d\n", reg[i].name,
			reg[i].buckets_log2, reg[i].gen, reg[i].live ? 1 : 0);
	}
	/* S239: a mode the verb set, config collections included - a line
	 * an older build skips (it is not a "collection" line) */
	for (i = 0; i < n; i++)
		if (reg[i].live && reg[i].as_set)
			fprintf(f, "autoscale %s %s\n", reg[i].name,
				pc_autoscale_name(reg[i].autoscale));
	pthread_mutex_unlock(&reg_mx);
	if (fflush(f) != 0 || fsync(fileno(f)) != 0)
		rc = -1;
	fclose(f);
	if (rc == 0 && rename(tmp, path) != 0)
		rc = -1;
	if (rc != 0)
		unlink(tmp);
	return rc;
}

int pc_store_load(const char *state_dir)
{
	char path[600], line[256], name[PC_COL_NAME_MAX + 32];
	FILE *f;
	int bl, n = 0;

	if (!state_dir || !*state_dir)
		return 0;
	snprintf(colfile_dir, sizeof colfile_dir, "%s", state_dir);
	snprintf(path, sizeof path, "%s/collections", state_dir);
	f = fopen(path, "r");
	if (!f)
		return 0;                  /* none yet: not an error */
	if (!fgets(line, sizeof line, f) ||
	        strncmp(line, COLFILE_HEADER, strlen(COLFILE_HEADER))) {
		fclose(f);
		LM_ERR("collections file %s does not start with '%s' - "
			"refusing to guess at it\n", path, COLFILE_HEADER);
		return -1;
	}
	while (fgets(line, sizeof line, f)) {
		unsigned long long gen = 0;
		int live = 1, rc;
		char mode[16];

		/* S239: the verb's mode, applied over the config's */
		if (sscanf(line, "autoscale %63s %15s", name, mode) == 2) {
			int m = pc_autoscale_parse(mode), i;

			if (m >= 0)
				for (i = 0; i < reg_n; i++)
					if (reg[i].live && reg[i].name &&
					    !strcmp(reg[i].name, name)) {
						reg[i].autoscale = m;
						reg[i].as_set = 1;
					}
			continue;
		}
		if (sscanf(line, "collection %63s %d %llu %d", name, &bl,
		        &gen, &live) < 2)
			continue;
		pc_lamport_observe(gen);   /* the clock covers what it stamped */
		if (!live) {
			/* a tombstone: the name and its generation, no table */
			pc_store_note_drop(name, strlen(name), gen);
			continue;
		}
		if (pc_store_find(name, strlen(name)))
			continue;          /* the config declares it too */
		rc = pc_store_create_gen(name, strlen(name), bl, gen);
		if (rc != 0 && rc != -4) {
			LM_ERR("collections file: '%s' (2^%d buckets) could "
				"not be created\n", name, bl);
			fclose(f);
			return -1;
		}
		n++;
	}
	fclose(f);
	if (n)
		LM_NOTICE("collections: %d created at runtime and carried "
			"across the restart from %s\n", n, path);
	return 0;
}

void pc_store_reset(void)             { reg_n = 0; }

/* ---- S280: redis_types ------------------------------------------------- */

int pc_types_default = PC_TYPES_STRICT;

void pc_store_set_types_idx(int i, int m)
{
	if (i >= 0 && i < reg_n)
		__atomic_store_n(&reg[i].redis_types, m, __ATOMIC_RELAXED);
}

int pc_store_types_strict(pcache_htable_t *ht)
{
	int i;

	for (i = 0; i < reg_n; i++)
		if (reg[i].ht == ht)
			return __atomic_load_n(&reg[i].redis_types,
				__ATOMIC_RELAXED) == PC_TYPES_STRICT;
	return pc_types_default == PC_TYPES_STRICT;
}

/* ---- S239: autoscale --------------------------------------------------- */

int pc_autoscale_default = PC_AUTOSCALE_AUTO;

int pc_store_autoscale(int i)
{
	return __atomic_load_n(&reg[i].autoscale, __ATOMIC_RELAXED);
}

void pc_store_set_autoscale_idx(int i, int m)
{
	__atomic_store_n(&reg[i].autoscale, m, __ATOMIC_RELAXED);
}

int pc_store_autoscale_set(const char *name, size_t nlen, int m)
{
	int i, found = -1;

	pthread_mutex_lock(&reg_mx);
	for (i = 0; i < reg_n; i++)
		if (reg[i].live && reg[i].name && strlen(reg[i].name) == nlen &&
		    !memcmp(reg[i].name, name, nlen))
			found = i;
	if (found >= 0) {
		__atomic_store_n(&reg[found].autoscale, m, __ATOMIC_RELAXED);
		reg[found].as_set = 1;
	}
	pthread_mutex_unlock(&reg_mx);
	if (found < 0)
		return -1;
	(void)pc_store_persist();
	return 0;
}

int pc_store_autoscale_target(int i)
{
	return __atomic_load_n(&reg[i].as_target, __ATOMIC_RELAXED);
}

void pc_store_set_autoscale_target(int i, int t)
{
	__atomic_store_n(&reg[i].as_target, t, __ATOMIC_RELAXED);
}

void pc_store_size_reset(void)
{
	int i;

	for (i = 0; i < reg_n; i++)
		colst_reset(i);
}
