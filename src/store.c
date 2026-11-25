/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* store.c - the collection registry: lookup, runtime create/drop/rename/resize, and each
 * collection's figures (written, held, hot keys, distinct-key sketch).  See store.h. */
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "quiesce.h"
#include "store.h"
#include "colname.h"
#include "events.h"
#include <pthread.h>
#include <unistd.h>
#include "compat/dprint.h"
#include "compat/compat.h"
#include "compat/timer.h"
#include <stdio.h>
#include <stdlib.h>
#include "core/pcache_arena.h"
#include "fnv1a.h"
#include "statefile.h"                  /* pc_state_sync_dir */

/* writers (create, drop) serialize here; readers take nothing: an entry is filled before reg_n
 * is published and reg_n never shrinks, so a reader sees a new entry complete or not at all */
static pthread_mutex_t reg_mx = PTHREAD_MUTEX_INITIALIZER;

static struct {
	/* heap-owned and swapped as a pointer, so a lock-free lookup never reads a rename half-done;
	 * the old string is never freed (a reader may still hold it) */
	char *name;
	int live;                          /* 0 = dropped, slot reusable */
	int runtime;                       /* created by a client, persisted */
	/* the name the config registered it under, kept across renames and drops; NULL for an
	 * entry the config never named - the interchange digest's set */
	const char *cfgname;
	int buckets_log2;                  /* what the table was made at */
	int floor_log2;                    /* the size explicitly given (config, create,
	                                    * explicit resize); FLOOR mode does not shrink below it */
	unsigned long long gen;            /* Lamport value of the create or drop that last
	                                    * moved this NAME: a drop outranks its create */
	pcache_htable_t *ht;
	/* bumped at every publish of `ht` or `live` (create, drop, the resize swap), so a walk
	 * resuming across turns can tell the table moved (the KEYS handle in proto.c) */
	unsigned long long pub;
	int pull;
	int proxy;
	int shard;
	int eager;
	/* the walked figure and its pacing (wall seconds) */
	unsigned long long held_bytes;
	unsigned int held_at, held_next, held_walk_us;
	/* value sizes held, from the same walk, in size_hist's classes: the walk fills the slot
	 * readers are not on, then publishes the generation */
	unsigned long long held_hist[2][8];
	unsigned int held_gen;
	/* index regions carved for this table (exact: noted at creation and at every growth step);
	 * held_cells below is the records' cells, from the walk */
	unsigned long index_bytes;
	/* the resize in flight for this collection, if any */
	pcache_htable_t *rs_to;
	unsigned int rs_cursor;
	int rs_target, rs_pass, rs_swapped;
	int rs_closing;              /* the old table is being emptied */
	unsigned long rs_unfit;      /* copies this pass found no room for */
	int rs_unfit_said;
	unsigned long long rs_moved;
	/* a fleet resize that arrived while this one ran (started when it finishes, the latest
	 * winning), and the newest fleet resize generation seen, so a late announce cannot undo it */
	int rs_pending;
	unsigned long long rs_gen;
	unsigned long long held_cells;
	int autoscale, as_target, as_set;   /* mode, AUTO's target, set by the verb */
	unsigned long long as_gen;          /* the set mode's generation */
	int redis_types;                    /* PC_TYPES_* */
	int nondurable;                     /* a cache - off the WAL and the snapshot */
} reg[PC_MAX_COLLECTIONS];
static int reg_n;

static void retire_push(pcache_htable_t *ht, int idx);
static void reg_forget_dead(const char *name, size_t nlen, int keep);

/* what a runtime-created collection inherits: the cluster's mode, set once at startup */
int pc_col_default_pull, pc_col_default_proxy, pc_col_default_shard,
	pc_col_default_eager;

/* the write stream's sizes (expiry and eviction not subtracted), kept out of reg[], whose lines
 * every lookup reads: one padded stripe per thread, added to atomically and summed on read */
#define COLST_STRIPES 16
static struct {
	unsigned long long bytes, n, hist[8];
	char pad[128 - 10 * sizeof(unsigned long long)];
} colst[PC_MAX_COLLECTIONS][COLST_STRIPES] __attribute__((aligned(128)));
/* 128-aligned so each stripe owns its adjacent-line prefetch pair
 * (80 bytes of counters span two lines) */
static unsigned int colst_next;
static __thread int colst_mine = -1;

/* hot keys: sampled by a per-thread xorshift, not a counter, so a client cycling through keys is
 * not aliased onto one; a sampler that finds the sketch's flag held counts `busy` and drops */
#define HOT_N      32
#define HOT_KEY    64              /* bytes of the key kept; klen is whole */
#define HOT_RATE   64
#define HOT_HALF_S 60
struct hot_ent {
	uint64_t h;
	unsigned int cnt, err, klen;
	char k[HOT_KEY];
};
static struct {
	unsigned char lk;
	unsigned int n;
	unsigned long long sampled, total, busy;
	struct hot_ent e[HOT_N];
} hot[PC_MAX_COLLECTIONS];
static __thread uint64_t hot_rng;

static void hot_lock(int i)
{
	while (__atomic_test_and_set(&hot[i].lk, __ATOMIC_ACQUIRE))
		;
}

static void hot_unlock(int i)
{
	__atomic_clear(&hot[i].lk, __ATOMIC_RELEASE);
}

static void hot_reset(int i)
{
	hot_lock(i);
	hot[i].n = 0;
	hot[i].sampled = hot[i].total = 0;
	__atomic_store_n(&hot[i].busy, 0, __ATOMIC_RELAXED);
	hot_unlock(i);
}

void pc_store_hot_note(pcache_htable_t *ht, const char *k, size_t kl)
{
	uint64_t x = hot_rng, h;
	unsigned int j, m, n, cl;
	int i, rn;

	if (!x)
		x = (uint64_t)(uintptr_t)&hot_rng * 0x9e3779b97f4a7c15ULL | 1;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	hot_rng = x;
	if (x & (HOT_RATE - 1))
		return;
	rn = __atomic_load_n(&reg_n, __ATOMIC_ACQUIRE);
	for (i = 0; i < rn; i++)
		if (__atomic_load_n(&reg[i].ht, __ATOMIC_ACQUIRE) == ht)
			break;
	if (i == rn || !k)
		return;
	h = fnv1a64(k, kl);
	cl = kl < HOT_KEY ? (unsigned int)kl : HOT_KEY;
	if (__atomic_test_and_set(&hot[i].lk, __ATOMIC_ACQUIRE)) {
		__atomic_fetch_add(&hot[i].busy, 1, __ATOMIC_RELAXED);
		return;
	}
	hot[i].sampled++;
	hot[i].total++;
	n = hot[i].n;
	for (j = 0, m = 0; j < n; j++) {
		struct hot_ent *e = &hot[i].e[j];

		if (e->h == h && e->klen == kl && !memcmp(e->k, k, cl)) {
			e->cnt++;
			hot_unlock(i);
			return;
		}
		if (e->cnt < hot[i].e[m].cnt)
			m = j;
	}
	if (n < HOT_N) {
		m = n;
		hot[i].n = n + 1;
		hot[i].e[m].cnt = 1;
		hot[i].e[m].err = 0;
	} else {                        /* the least counted gives its place */
		hot[i].e[m].err = hot[i].e[m].cnt;
		hot[i].e[m].cnt++;
	}
	hot[i].e[m].h = h;
#if SIZE_MAX > UINT_MAX                 /* a 32-bit size_t never exceeds it: -Wtype-limits */
	hot[i].e[m].klen = kl > UINT_MAX ? UINT_MAX : (unsigned int)kl;
#else
	hot[i].e[m].klen = (unsigned int)kl;
#endif
	memcpy(hot[i].e[m].k, k, cl);
	hot_unlock(i);
}

/* 1/s from the maintenance thread: every HOT_HALF_S the counts halve and
 * the entries that reach 0 leave */
static int hot_half_s(void)         /* a suite shortens it, never a fleet */
{
	static int v;

	if (!v) {
		const char *e = getenv("PERFCACHED_TEST_HOT_HALF_S");

		v = e && atoi(e) > 0 ? atoi(e) : HOT_HALF_S;
		if (v != HOT_HALF_S)
			LM_WARN("store: PERFCACHED_TEST_HOT_HALF_S=%d - the hot-key "
				"counts halve every %d s, not %d (a test setting)\n",
				v, v, HOT_HALF_S);
	}
	return v;
}

void pc_store_hot_tick(void)
{
	static time_t last;
	time_t now = time(NULL);
	int i, rn = __atomic_load_n(&reg_n, __ATOMIC_ACQUIRE);
	unsigned int j, w;

	if (!last)
		last = now;
	if (now - last < hot_half_s())
		return;
	last = now;
	for (i = 0; i < rn; i++) {
		hot_lock(i);
		for (j = 0, w = 0; j < hot[i].n; j++) {
			struct hot_ent e = hot[i].e[j];

			e.cnt /= 2;
			e.err /= 2;
			if (e.cnt)
				hot[i].e[w++] = e;
		}
		hot[i].n = w;
		hot[i].sampled /= 2;
		hot_unlock(i);
	}
}

static int hot_cmp(const void *a, const void *b)
{
	const struct pc_hot_row *x = a, *y = b;

	return x->est < y->est ? 1 : x->est > y->est ? -1 : 0;
}

int pc_store_hot_get(int i, struct pc_hot_row *out, int cap,
		unsigned long long *sampled, unsigned long long *total,
		unsigned long long *busy)
{
	int j, n;

	if (i < 0 || i >= PC_MAX_COLLECTIONS)
		return 0;
	hot_lock(i);
	n = (int)hot[i].n < cap ? (int)hot[i].n : cap;
	for (j = 0; j < n; j++) {
		const struct hot_ent *e = &hot[i].e[j];

		out[j].est = e->cnt;
		out[j].err = e->err;
		out[j].klen = e->klen;
		out[j].kept = e->klen < HOT_KEY ? e->klen : HOT_KEY;
		memcpy(out[j].key, e->k, out[j].kept);
	}
	*sampled = hot[i].sampled;
	*total = hot[i].total;
	*busy = __atomic_load_n(&hot[i].busy, __ATOMIC_RELAXED);
	hot_unlock(i);
	qsort(out, (size_t)n, sizeof *out, hot_cmp);
	return n;
}

int pc_store_hot_rate(void)
{
	return HOT_RATE;
}

/* the held walk's distinct-key sketches - see pc_store_sketch */
static unsigned char hll[PC_MAX_COLLECTIONS][2][PC_HLL_M];
static unsigned int hll_gen[PC_MAX_COLLECTIONS];
static unsigned int hll_at[PC_MAX_COLLECTIONS];

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
	hot_reset(i);
	__atomic_store_n(&hll_gen[i], 0, __ATOMIC_RELEASE);   /* a new slot has no walk */
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

/* held: the maintenance thread walks each collection whole, values copied as SCAN does, and
 * paces itself at 20x the walk's cost (5..60 s), so it costs that thread at most 5% */
#define PC_HELD_MIN_S 5
#define PC_HELD_MAX_S 60

/* the HLL of the keys each held walk saw (p = 12, 1.6% error), rebuilt by every walk; the hash
 * is the same on every node, so the dashboard merges members' sketches by register max */

static inline uint64_t hll_hash(const char *k, size_t n)
{
	uint64_t h = fnv1a64(k, n);

	h ^= h >> 33;
	h *= 0xff51afd7ed558ccdULL;
	h ^= h >> 33;
	h *= 0xc4ceb9fe1a85ec53ULL;
	h ^= h >> 33;
	return h;
}

int pc_store_sketch(int i, unsigned char *out, unsigned int *walked_at)
{
	unsigned int g, g2;
	int tries;

	if (i < 0 || i >= PC_MAX_COLLECTIONS)
		return -1;
	for (tries = 0; tries < 4; tries++) {
		g = __atomic_load_n(&hll_gen[i], __ATOMIC_ACQUIRE);
		if (!g)
			return -1;             /* no walk since the slot was made */
		memcpy(out, hll[i][g & 1], PC_HLL_M);
		*walked_at = __atomic_load_n(&hll_at[i], __ATOMIC_RELAXED);
		g2 = __atomic_load_n(&hll_gen[i], __ATOMIC_ACQUIRE);
		if (g2 == g)
			return 0;
	}
	return -1;
}

struct held_acc {
	unsigned char *hll;                /* the registers this walk fills */
	unsigned long long sum;            /* key + value bytes */
	unsigned long long hist[8];        /* value sizes, size_hist's classes */
	unsigned long long cells;          /* the cells the records occupy */
};

static int held_cb(const str *key, const str *val, unsigned int expires,
		void *ctx)
{
	struct held_acc *a = ctx;
	size_t v = val->len;
	int c = 0;

	(void)expires;                     /* held until the sweep frees it */
	if (a->hll) {
		uint64_t h = hll_hash(key->s, (size_t)key->len);
		unsigned int idx = (unsigned int)(h >> (64 - PC_HLL_P));
		uint64_t w = (h << PC_HLL_P) | (1ULL << (PC_HLL_P - 1));
		unsigned char rho = (unsigned char)(__builtin_clzll(w) + 1);

		if (rho > a->hll[idx])
			a->hll[idx] = rho;
	}
	a->sum += (unsigned long long)key->len + (unsigned long long)val->len;
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

		/* a dropped collection has nothing to walk, and a tombstone
		 * read back from the collections file has no table at all */
		if (!__atomic_load_n(&reg[i].live, __ATOMIC_ACQUIRE) ||
		        !reg[i].ht)
			continue;
		if (now < reg[i].held_next)
			continue;
		memset(&acc, 0, sizeof acc);
		acc.hll = hll[i][(__atomic_load_n(&hll_gen[i], __ATOMIC_RELAXED) + 1) & 1];
		memset(acc.hll, 0, PC_HLL_M);
		clock_gettime(CLOCK_MONOTONIC, &t0);
		do {
			if (pcache_ht_scan_ex(reg[i].ht, &cursor, 1024, 0,
			        held_cb, &acc) < 0)
				break;
		} while (cursor);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		/* one signed nanosecond quantity: an unsigned nanosecond difference wraps on about half
		 * the walks, and the pacing below reads this figure */
		{
			long long dns =
				(long long)(t1.tv_sec - t0.tv_sec) * 1000000000LL +
				(long long)(t1.tv_nsec - t0.tv_nsec);

			us = dns > 0 ? (unsigned long long)dns / 1000ULL : 0;
		}
		/* the histogram goes to the slot readers are not on */
		g = reg[i].held_gen + 1;
		memcpy(reg[i].held_hist[g & 1], acc.hist, sizeof acc.hist);
		__atomic_store_n(&reg[i].held_gen, g, __ATOMIC_RELEASE);
		__atomic_store_n(&reg[i].held_cells, acc.cells, __ATOMIC_RELEASE);
		__atomic_store_n(&reg[i].held_bytes, acc.sum, __ATOMIC_RELEASE);
		__atomic_store_n(&reg[i].held_at, now, __ATOMIC_RELEASE);
		__atomic_store_n(&hll_at[i], now, __ATOMIC_RELAXED);
		__atomic_store_n(&hll_gen[i], __atomic_load_n(&hll_gen[i], __ATOMIC_RELAXED) + 1,
			__ATOMIC_RELEASE);
		reg[i].held_walk_us = (unsigned int)us;
		span = (unsigned int)(us * 20ULL / 1000000ULL);
		if (span < PC_HELD_MIN_S)
			span = PC_HELD_MIN_S;
		if (span > PC_HELD_MAX_S)
			span = PC_HELD_MAX_S;
		reg[i].held_next = now + span;
	}
}

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

/* called by whoever carved: the daemon at creation, the maintenance thread at growth */
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
	for (s = 0; s < COLST_STRIPES; s++) {
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
	reg[reg_n].cfgname = reg[reg_n].name;   /* never freed: a rename swaps the pointer */
	reg[reg_n].live = 1;
	reg[reg_n].ht = ht;
	/* proxy AND shard reads ride the pull path (unicast to the holder
	 * / owner); shard needs no locator and no broadcast. */
	reg[reg_n].pull = pull || proxy || shard;
	reg[reg_n].proxy = proxy;
	reg[reg_n].shard = shard;
	reg[reg_n].eager = eager;
	reg[reg_n].buckets_log2 = buckets_log2;
	reg[reg_n].floor_log2 = buckets_log2;
	reg[reg_n].autoscale = pc_autoscale_default;
	reg[reg_n].redis_types = pc_types_default;
	reg[reg_n].as_target = reg[reg_n].as_set = 0;
	reg[reg_n].nondurable = 0;
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

/* an eager collection always pulls, whatever its mode: restarted after a crash it lacks every
 * copy it received (they are off its WAL) until the sweep re-sends them */
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

/* 0 ok, -1 no such collection, -2 the new name is taken, -3 out of memory or a bad name.  The caller
 * persists the file; a load maps older names through the WAL's rename records */
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
	reg_forget_dead(to, tlen, found);
	__atomic_store_n(&reg[found].name, nm, __ATOMIC_RELEASE);
	reg[found].runtime = 1;            /* it is ours to persist now */
	reg[found].gen = gen;
	pthread_mutex_unlock(&reg_mx);
	return 0;
}

int pc_store_count(void)              { return __atomic_load_n(&reg_n, __ATOMIC_ACQUIRE); }
const char *pc_store_name(int i)      { return reg[i].name; }
pcache_htable_t *pc_store_ht(int i)   { return reg[i].ht; }
/* the published table and its generation under one lock: lock-free, a resize swap bumps `pub`
 * before the pointer, and `dump` checks every later chunk against this generation */
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

/* the slot publishing @ht and its generation under one lock (-1: swapped out or dropped since);
 * lock-free, a KEYS walk could pair the old table with the new generation and miss the swap */
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
int pc_store_floor_log2(int i)        { return reg[i].floor_log2; }

/* only an explicit resize calls this (the verb, or the fleet's announce),
 * so FLOOR cannot ratchet itself */
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
const char *pc_store_cfg_name(int i)  { return reg[i].cfgname; }

/* ---- runtime creation and removal, on a worker while the daemon serves: a new entry is complete
 * before the count publishes it, and a dropped one stops being findable before a record is freed */

static int reg_slot_for(const char *name, size_t nlen)
{
	int i, slot = -1;

	for (i = 0; i < reg_n; i++)
		if (reg[i].live && strlen(reg[i].name) == nlen &&
		        !memcmp(reg[i].name, name, nlen))
			return -1;                 /* the name is taken */
	/* a dead slot is reusable once its table went back to the arena (pc_store_retire_tick);
	 * the size does not matter, the carve takes retired slots first; the name's own first */
	for (i = 0; i < reg_n; i++) {
		if (reg[i].live || reg[i].ht || reg[i].rs_to)
			continue;
		if (reg[i].name && strlen(reg[i].name) == nlen &&
		        !memcmp(reg[i].name, name, nlen))
			return i;
		if (slot < 0)
			slot = i;
	}
	if (slot >= 0)
		return slot;
	return reg_n < PC_MAX_COLLECTIONS ? reg_n : -2;
}

static char reg_noname[1];

/* reg_mx held: the name's tombstones other than @keep are forgotten, so a re-announce of one
 * cannot drop the entry that now holds the name */
static void reg_forget_dead(const char *name, size_t nlen, int keep)
{
	int i;

	for (i = 0; i < reg_n; i++)
		if (i != keep && !reg[i].live && reg[i].name &&
		        strlen(reg[i].name) == nlen &&
		        !memcmp(reg[i].name, name, nlen)) {
			__atomic_store_n(&reg[i].name, reg_noname, __ATOMIC_RELEASE);
			reg[i].gen = 0;
		}
}

int pc_store_create(const char *name, size_t nlen, int buckets_log2)
{
	return pc_store_create_gen(name, nlen, buckets_log2, pc_lamport_tick());
}

/* a create from the fleet keeps the originator's stamp so every node orders it the same
 * way; a name whose tombstone is newer is left dropped */
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
	/* always a fresh carve - the arena serves it from retired
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
		if (i == reg_n)
			reg[i].cfgname = NULL;
		__atomic_store_n(&reg[i].name, nm, __ATOMIC_RELEASE);
	}
	reg_forget_dead(name, nlen, i);
	reg[i].ht = ht;
	reg[i].pub++;                      /* a publish */
	reg[i].buckets_log2 = buckets_log2;
	reg[i].floor_log2 = buckets_log2;
	reg[i].runtime = 1;
	pc_ev_configure(i, PC_EV_INHERIT);
	pc_ev_configure_notify(i, PC_EV_INHERIT);
	reg[i].gen = gen;
	/* the fleet is one mode over one collection set: a created collection
	 * takes the cluster's, never a per-collection variant of its own */
	reg[i].pull = pc_col_default_pull;
	reg[i].proxy = pc_col_default_proxy;
	reg[i].shard = pc_col_default_shard;
	reg[i].eager = pc_col_default_eager;
	reg[i].autoscale = pc_autoscale_default;
	reg[i].redis_types = pc_types_default;
	/* a created collection is durable - a cache is the config's word,
	 * and a slot a dropped cache left must not hand that on */
	__atomic_store_n(&reg[i].nondurable, 0, __ATOMIC_RELAXED);
	reg[i].rs_pending = 0;
	reg[i].rs_gen = 0;
	reg[i].rs_cursor = 0;
	reg[i].rs_target = reg[i].rs_pass = reg[i].rs_swapped = 0;
	reg[i].rs_closing = reg[i].rs_unfit_said = 0;
	reg[i].rs_unfit = 0;
	reg[i].rs_moved = 0;
	reg[i].as_target = reg[i].as_set = 0;
	reg[i].as_gen = 0;
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

/* passes until one removes nothing: the walk hands over every record in one pass, but a drop
 * racing a writer still wants a second look (a record left would reappear on reuse) */
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
	if (gen < reg[found].gen) {        /* older than the create: stale news */
		pthread_mutex_unlock(&reg_mx);
		return -2;
	}
	/* off the map first: from here every lookup says "no such
	 * collection", so nothing new lands in the table being emptied */
	__atomic_store_n(&reg[found].live, 0, __ATOMIC_RELEASE);
	reg[found].pub++;                  /* a publish */
	reg[found].runtime = 0;
	reg[found].gen = gen;              /* the tombstone outranks the create */
	pthread_mutex_unlock(&reg_mx);

	/* ordinary removes: a concurrent walk sees the table empty under it, and nothing is
	 * freed under a pointer */
	{
		long left;

		pcache_ht_set_drain(reg[found].ht, NULL);
		left = drain_table(reg[found].ht);

		/* the table goes back to the arena once every thread that could hold a pointer into it
		 * has parked; until then the slot keeps it and reg_slot_for passes it by */
		pthread_mutex_lock(&reg_mx);
		retire_push(reg[found].ht, found);
		pthread_mutex_unlock(&reg_mx);
		return left;
	}
}

/* tables no longer published (a resize's old one, a dropped collection's) wait for every thread
 * that could hold a pointer to park (pc_qs_clear); then their regions go back to the arena */
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


/* ---- resize by migration: copy passes, the pointer swap, more passes; the version rule keeps
 * a copy from undoing a live write, and deletes go to both tables, the copier's source first
 * (pc_store_del_order) */
#define RS_PRE_PASSES   2
#define RS_POST_PASSES  2
#define RS_BUDGET   50000

struct rs_ctx {
	unsigned long expired;         /* dropped for having expired */
	unsigned long unfit;           /* not copied - no room, an error */
	pcache_htable_t *to;
	unsigned int now, budget, n;
	unsigned long long moved;
	int slot;
};

/* tests only: PERFCACHED_TEST_RS_COPY_FAIL=<file> - while it exists, an
 * "nf:" key's copy fails as one that finds the arena full does */
static int rs_test_nofit(const str *key)
{
	static int init;
	static const char *path;

	if (!init) {
		path = getenv("PERFCACHED_TEST_RS_COPY_FAIL");
		init = 1;
	}
	return path && key->len >= 3 && !memcmp(key->s, "nf:", 3) &&
		access(path, F_OK) == 0;
}

/* tests only: PERFCACHED_TEST_RS_COPY_PAUSE=<path> - the copy of a "pz:pre" key before the swap,
 * or of a "pz:post" key after it, waits (10 s at most) while <path>.pre / <path>.post exists */
static void rs_test_pause(const str *key, int swapped)
{
	static int init;
	static const char *path;
	const char *ph = swapped ? "post" : "pre";
	char f[PATH_MAX];
	int i;

	if (!init) {
		path = getenv("PERFCACHED_TEST_RS_COPY_PAUSE");
		init = 1;
	}
	if (!path || key->len != 3 + (int)strlen(ph) || memcmp(key->s, "pz:", 3) ||
	        memcmp(key->s + 3, ph, strlen(ph)))
		return;
	snprintf(f, sizeof f, "%s.%s", path, ph);
	if (access(f, F_OK) != 0)
		return;
	LM_NOTICE("TEST HOOK - resize copy paused on '%.*s'\n", key->len, key->s);
	for (i = 0; i < 1000 && access(f, F_OK) == 0; i++)
		usleep(10000);
}

static int rs_copy_cb(const str *key, const str *val,
		const struct pcache_rec_meta *mt, void *p)
{
	struct rs_ctx *c = p;
	int rc;

	if (c->n >= c->budget)
		return -1;                 /* this tick's share is spent */
	if (!__atomic_load_n(&reg[c->slot].live, __ATOMIC_ACQUIRE))
		return -1;                 /* dropped under the pass */
	c->n++;
	rs_test_pause(key, reg[c->slot].rs_swapped);
	if (mt->expires && mt->expires <= c->now) {
		/* it does not travel, and it is gone: counted, so the resize says so */
		c->expired++;
		/* gone without a delete: the new table has no record to floor a later write's version
		 * on, so the clock carries it (pcache_ht_sweep says why) */
		pc_lamport_publish(mt->ver);
		/* and it superseded any older copy already made into the target; its own copy stays
		 * for the target's sweep, which notes the expiry */
		(void)pcache_ht_remove_below(c->to, key, mt->ver);
		return 0;
	}
	/* the same record, not a fresh local write: version, expiry, PASSIVE/CARRY, type and write
	 * tick (a recovered record the reconcile has not ruled on must stay one) */
	rc = rs_test_nofit(key) ? -2 : pcache_ht_store_copy_held(c->to, key, val,
		mt->expires,
		(unsigned char)(mt->rflags & (PCACHE_F_PASSIVE | PCACHE_F_CARRY)),
		mt->ver, mt->wtick);
	if (rc == 0)
		c->moved++;
	else if (rc != PCACHE_E_OLDER)
		c->unfit++;                /* the original is all there is */
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
	/* the size it was MADE at is not the size it has grown to -
	 * a table split up from 2^12 may be resized back to 2^12 */
	if (reg[found].buckets_log2 == target_log2 &&
	    pcache_ht_nbuckets(reg[found].ht) <= (1U << target_log2)) {
		pthread_mutex_unlock(&reg_mx);
		return -5;
	}
	/* a target the splitter would undo at once (it grows past four records a bucket) is
	 * refused rather than performed and lost */
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
	/* the upstream shadow goes with the collection, not the table:
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
	reg[found].rs_unfit = 0;
	reg[found].rs_unfit_said = 0;
	reg[found].rs_swapped = 0;
	reg[found].rs_moved = 0;
	pthread_mutex_unlock(&reg_mx);
	return 0;
}

/* @gen rises strictly (the sender ticks its clock), so an older one is a late or repeated announce;
 * one arriving mid-resize is held, not refused, or this node would keep the old size for good */
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
	/* the fleet's resize is an explicit one, so its size becomes the floor here too - once
	 * accepted (started or held); a refused one moves nothing */
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

/* a resize this node started for a client and announced at @gen */
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

/* a walk that must see every record (the snapshot's) holds the swap
 * and the drain's end; while it holds, the pair it was given stays as it is */
static int walk_holds;

void pc_store_walk_begin(int i, pcache_htable_t **live, pcache_htable_t **old)
{
	pthread_mutex_lock(&reg_mx);
	walk_holds++;
	*live = reg[i].ht;
	*old = reg[i].rs_swapped && !reg[i].rs_closing ? reg[i].rs_to : NULL;
	pthread_mutex_unlock(&reg_mx);
}

void pc_store_walk_end(void)
{
	pthread_mutex_lock(&reg_mx);
	walk_holds--;
	pthread_mutex_unlock(&reg_mx);
}

pcache_htable_t *pc_store_del_order(pcache_htable_t *ht, pcache_htable_t **second)
{
	int i, n = pc_store_count();

	*second = NULL;
	for (i = 0; i < n; i++)
		if (reg[i].ht == ht) {
			pcache_htable_t *sh = __atomic_load_n(&reg[i].rs_to, __ATOMIC_ACQUIRE);

			if (!sh)
				break;
			/* before the swap the copier reads the live table, after it the old one */
			if (__atomic_load_n(&reg[i].rs_swapped, __ATOMIC_RELAXED)) {
				*second = ht;
				return sh;
			}
			*second = sh;
			break;
		}
	return ht;
}

pcache_htable_t *pc_store_resize_shadow(pcache_htable_t *ht)
{
	int i, n = pc_store_count();

	for (i = 0; i < n; i++)
		if (reg[i].ht == ht)
			return __atomic_load_n(&reg[i].rs_to, __ATOMIC_ACQUIRE);
	return NULL;
}

/* client cursor: bit 31 the overflow-leg flag (PCACHE_CURSOR_OVF), bits 30-28 the table's
 * generation 1..7 (another restarts at 0), bit 27 the OLD table of a draining resize, 26-0 index */
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

/* a collection dropped mid-resize: its other table is emptied and retired here, on the thread
 * that runs the copy passes, and the slot is reusable only after */
static void rs_abandon(int i)
{
	pcache_htable_t *t = reg[i].rs_to;

	drain_table(t);
	pthread_mutex_lock(&reg_mx);
	__atomic_store_n(&reg[i].rs_to, (pcache_htable_t *)NULL,
		__ATOMIC_RELEASE);
	reg[i].rs_swapped = reg[i].rs_closing = reg[i].rs_pass = 0;
	reg[i].rs_cursor = 0;
	reg[i].rs_pending = 0;
	reg[i].rs_unfit = 0;
	retire_push(t, i);
	pthread_mutex_unlock(&reg_mx);
}

void pc_store_resize_tick(void)
{
	int i, n = pc_store_count();

	for (i = 0; i < n; i++) {
		struct rs_ctx c;
		pcache_htable_t *from, *to;
		int pend = 0;

		if (!__atomic_load_n(&reg[i].live, __ATOMIC_ACQUIRE)) {
			if (reg[i].rs_to)
				rs_abandon(i);
			continue;
		}
		if (!reg[i].rs_to)
			continue;
		/* before the swap: live into new; after it rs_to holds the OLD
		 * table and the passes drain it into the live one */
		from = reg[i].rs_swapped ? reg[i].rs_to : reg[i].ht;
		to = reg[i].rs_swapped ? reg[i].ht : reg[i].rs_to;
		c.to = to;
		c.now = (unsigned int)get_ticks();
		c.budget = RS_BUDGET;
		c.n = 0;
		c.moved = 0;
		c.expired = 0;
		c.unfit = 0;
		c.slot = i;
		/* under the source's bucket lock, so a delete (source first) is never undone */
		if (pcache_ht_iter_locked_from(from, &reg[i].rs_cursor,
		        rs_copy_cb, &c) < 0 && !c.n)
			continue;              /* the walk failed: try next tick */
		reg[i].rs_moved += c.moved;
		if (reg[i].rs_swapped)
			reg[i].rs_unfit += c.unfit;
		if (c.expired)                 /* on the LIVE table */
			pcache_ht_note_expired(reg[i].ht, c.expired);
		if (reg[i].rs_cursor)
			continue;              /* mid-pass */
		if (reg[i].rs_unfit) {
			/* the closing drain drops the old table - a pass that
			 * could not copy everything is made again, until it can */
			if (!reg[i].rs_unfit_said)
				LM_WARN("collection '%s': resize: %lu record(s) not copied "
					"into the new table - no room in the arena; the old "
					"table keeps them and the resize waits for room\n",
					reg[i].name, reg[i].rs_unfit);
			reg[i].rs_unfit_said = 1;
			reg[i].rs_unfit = 0;
			continue;
		}
		reg[i].rs_pass++;
		if (!reg[i].rs_swapped) {
			if (reg[i].rs_pass < RS_PRE_PASSES)
				continue;
			/* THE COMMIT POINT: every lookup from here lands in the new table; the old one still
			 * takes writes from requests resolved before, which the passes below collect */
			pthread_mutex_lock(&reg_mx);
			if (walk_holds || !reg[i].live) {   /* after the snapshot's walk */
				pthread_mutex_unlock(&reg_mx);
				continue;
			}
			{
				pcache_htable_t *old = reg[i].ht;
				pcache_ht_totals_t oa, oc;

				/* the new table's counters start at zero: carry the
				 * lifetime figures while `old` is still the published table */
				pcache_ht_totals(old, &oa);
				pcache_ht_totals_client(old, &oc);
				pcache_ht_carry_totals(reg[i].rs_to, &oa, &oc);
				/* and what the resets took off, so the
				 * raw totals /metrics reads carry on too */
				pcache_ht_carry_reset(reg[i].rs_to, old);

				/* before the publish: a reader of the new table falls
				 * back to the old one, and a writer carries the key over first */
				pcache_ht_set_drain(reg[i].rs_to, old);
				reg[i].pub++;      /* a publish */
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
		/* done: the old table is emptied, then retired */
		{
			pcache_htable_t *old = reg[i].rs_to;
			long left;

			pthread_mutex_lock(&reg_mx);
			if (walk_holds || !reg[i].live) {   /* after the snapshot's walk */
				pthread_mutex_unlock(&reg_mx);
				continue;
			}
			reg[i].rs_closing = 1;
			pthread_mutex_unlock(&reg_mx);
			pcache_ht_set_drain(reg[i].ht, NULL);
			left = drain_table(old);

			pthread_mutex_lock(&reg_mx);
			__atomic_store_n(&reg[i].rs_to, (pcache_htable_t *)NULL,
				__ATOMIC_RELEASE);
			reg[i].rs_swapped = 0;
			reg[i].rs_closing = 0;
			reg[i].rs_cursor = 0;
			/* unpublished on both paths now (the swap took it off `ht`, this off
			 * the shadow); it goes back once the threads that may hold it have parked */
			retire_push(old, i);
			pend = reg[i].rs_pending;
			reg[i].rs_pending = 0;
			pthread_mutex_unlock(&reg_mx);
			LM_NOTICE("collection '%s': resize complete at 2^%d "
				"buckets (%llu carried, %ld left behind in the "
				"old table)\n", reg[i].name,
				reg[i].buckets_log2, reg[i].rs_moved, left);
			/* a fleet resize that found this one running */
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

/* a tombstone with no table to empty: read back from the file, or a drop announced by a
 * peer for a collection this node never had */
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
		reg[reg_n].cfgname = NULL;
		reg[reg_n].gen = gen;
		reg[reg_n].live = 0;
		reg[reg_n].ht = NULL;
		__atomic_store_n(&reg_n, reg_n + 1, __ATOMIC_RELEASE);
	}
	pthread_mutex_unlock(&reg_mx);
}

/* the created set, for a peer that has just come up; 0 while @i is in range */
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

/* ---- the created set on disk: "collection <name> <log2> <gen> <live>" lines, written whole and
 * renamed over; config collections are not listed (that would resurrect one the config removed) */
#define COLFILE_HEADER "perfcached-collections 1"

static char colfile_dir[512];   /* remembered by pc_store_load() */
/* callers are on several threads and share one temp file: one write and rename at a time */
static pthread_mutex_t persist_mx = PTHREAD_MUTEX_INITIALIZER;
/* what the last write put on disk: the same set again is not written (a resync re-sends it) */
static char *colfile_last;
static size_t colfile_last_len;
static int persist_due;

static int store_persist_locked(void);

int pc_store_persist(void)
{
	int rc;

	if (!colfile_dir[0])
		return 0;                  /* ephemeral by configuration */
	pthread_mutex_lock(&persist_mx);
	rc = store_persist_locked();
	pthread_mutex_unlock(&persist_mx);
	return rc;
}

/* the fleet's applies only mark the file due: its fsync waits on storage, and the cluster thread
 * answers every pull and forward - pc-maint writes it (pc_store_persist_tick) */
void pc_store_persist_later(void)
{
	__atomic_store_n(&persist_due, 1, __ATOMIC_RELEASE);
}

void pc_store_persist_tick(void)
{
	if (__atomic_exchange_n(&persist_due, 0, __ATOMIC_ACQ_REL) &&
	        pc_store_persist() != 0)
		__atomic_store_n(&persist_due, 1, __ATOMIC_RELEASE);   /* the next tick retries */
}

static int store_persist_locked(void)
{
	char path[600], tmp[620];
	char *body = NULL;
	size_t len = 0;
	FILE *m, *f;
	int i, n = pc_store_count(), rc = 0;

	m = open_memstream(&body, &len);
	if (!m)
		return -1;
	fprintf(m, "%s\n", COLFILE_HEADER);
	pthread_mutex_lock(&reg_mx);
	for (i = 0; i < n; i++) {
		if (!reg[i].gen)
			continue;              /* never touched at runtime */
		if (reg[i].live && !reg[i].runtime)
			continue;              /* the config file's, not ours */
		/* tombstones are written too: a drop that a peer missed must
		 * not be undone by that peer's re-announce after a restart */
		fprintf(m, "collection %s %d %llu %d\n", reg[i].name,
			reg[i].buckets_log2, reg[i].gen, reg[i].live ? 1 : 0);
	}
	/* a mode the verb set, config collections included, with its generation: lines an
	 * older build skips (not "collection" lines) */
	for (i = 0; i < n; i++)
		if (reg[i].live && reg[i].as_set)
			fprintf(m, "autoscale %s %s %llu\n", reg[i].name,
				pc_autoscale_name(reg[i].autoscale), reg[i].as_gen);
	pthread_mutex_unlock(&reg_mx);
	if (fclose(m) != 0) {
		free(body);
		return -1;
	}
	if (colfile_last && len == colfile_last_len && !memcmp(body, colfile_last, len)) {
		free(body);
		return 0;
	}
	snprintf(path, sizeof path, "%s/collections", colfile_dir);
	snprintf(tmp, sizeof tmp, "%s/collections.tmp", colfile_dir);
	f = fopen(tmp, "w");
	if (!f) {
		free(body);
		return -1;
	}
	if (fwrite(body, 1, len, f) != len || fflush(f) != 0 || fsync(fileno(f)) != 0)
		rc = -1;
	if (fclose(f) != 0)
		rc = -1;
	if (rc == 0 && rename(tmp, path) != 0)
		rc = -1;
	if (rc != 0) {
		unlink(tmp);
		free(body);
		return rc;
	}
	/* the new name survives a power loss only once the directory is synced */
	if (pc_state_sync_dir(colfile_dir) != 0) {
		free(body);
		return -1;
	}
	free(colfile_last);
	colfile_last = body;
	colfile_last_len = len;
	return 0;
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

		/* the verb's mode, applied over the config's, with its
		 * generation (0 when the line has none) */
		if (sscanf(line, "autoscale %63s %15s %llu", name, mode,
		        &gen) >= 2) {
			int m = pc_autoscale_parse(mode), i;

			pc_lamport_observe(gen);
			if (m >= 0)
				for (i = 0; i < reg_n; i++)
					if (reg[i].live && reg[i].name &&
					    !strcmp(reg[i].name, name)) {
						reg[i].autoscale = m;
						reg[i].as_set = 1;
						reg[i].as_gen = gen;
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
		/* a name from before the rule is KEPT - its records are
		 * this node's data - and said, so it can be renamed */
		if (!pc_col_name_held_ok(name, strlen(name))) {
			char pb[4 * PC_COL_NAME_MAX + 1];

			LM_WARN("collections file: '%s' breaks the collection-name "
				"rule (%s) - kept as it is; rename it\n",
				pc_col_name_print(pb, sizeof pb, name, strlen(name)),
				PC_COL_NAME_RULE);
		}
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

/* ---- cache collections (`durable = no`): never WAL-logged or snapshotted, empty after a restart
 * on this node (peers' copies still reach it); set at start only, a reload refuses the change */
static int any_nondurable;             /* none: the WAL looks nothing up */

void pc_store_set_nondurable_idx(int i, int on)
{
	if (i < 0 || i >= reg_n)
		return;
	__atomic_store_n(&reg[i].nondurable, on ? 1 : 0, __ATOMIC_RELAXED);
	if (on)
		__atomic_store_n(&any_nondurable, 1, __ATOMIC_RELEASE);
}

int pc_store_durable_idx(int i)
{
	return i < 0 || i >= reg_n ||
		!__atomic_load_n(&reg[i].nondurable, __ATOMIC_RELAXED);
}

int pc_store_durable(pcache_htable_t *ht)
{
	return pc_store_durable_idx(pc_store_slot_of(ht));
}

/* by name - the WAL knows a collection only by its name.  One load
 * while no collection is a cache */
int pc_store_durable_name(const char *name, size_t nlen)
{
	int i, n;

	if (!__atomic_load_n(&any_nondurable, __ATOMIC_ACQUIRE))
		return 1;
	n = __atomic_load_n(&reg_n, __ATOMIC_ACQUIRE);
	for (i = 0; i < n; i++) {
		const char *nm;

		if (!__atomic_load_n(&reg[i].live, __ATOMIC_ACQUIRE))
			continue;
		nm = __atomic_load_n(&reg[i].name, __ATOMIC_ACQUIRE);
		if (nm && strlen(nm) == nlen && !memcmp(nm, name, nlen))
			return !__atomic_load_n(&reg[i].nondurable,
				__ATOMIC_RELAXED);
	}
	return 1;
}

/* ---- redis_types ------------------------------------------------------- */

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

/* ---- autoscale --------------------------------------------------------- */

int pc_autoscale_default = PC_AUTOSCALE_AUTO;

int pc_store_autoscale(int i)
{
	return __atomic_load_n(&reg[i].autoscale, __ATOMIC_RELAXED);
}

void pc_store_set_autoscale_idx(int i, int m)
{
	__atomic_store_n(&reg[i].autoscale, m, __ATOMIC_RELAXED);
}

int pc_store_autoscale_apply(const char *name, size_t nlen, int m,
		unsigned long long gen, int *was)
{
	int i, found = -1, rc = 1;

	if (m < PC_AUTOSCALE_AUTO || m > PC_AUTOSCALE_FLOOR)
		return -2;                 /* a mode off the wire this build lacks */
	pthread_mutex_lock(&reg_mx);
	for (i = 0; i < reg_n; i++)
		if (reg[i].live && reg[i].name && strlen(reg[i].name) == nlen &&
		    !memcmp(reg[i].name, name, nlen))
			found = i;
	if (found >= 0 && was)
		*was = reg[found].autoscale;
	if (found < 0)
		rc = -1;
	else if (reg[found].as_set &&
	         (gen < reg[found].as_gen ||
	          (gen == reg[found].as_gen && m <= reg[found].autoscale)))
		/* older, or the same change again (a resync re-sends it every few seconds); any
		 * runtime mode outranks the config's, whatever its stamp */
		rc = 0;
	else {
		__atomic_store_n(&reg[found].autoscale, m, __ATOMIC_RELAXED);
		reg[found].as_set = 1;
		reg[found].as_gen = gen;
	}
	pthread_mutex_unlock(&reg_mx);
	return rc;
}

unsigned long long pc_store_autoscale_gen(int i)
{
	unsigned long long g;

	pthread_mutex_lock(&reg_mx);
	g = reg[i].as_gen;
	pthread_mutex_unlock(&reg_mx);
	return g;
}

int pc_store_autoscale_isset(int i)
{
	int s;

	pthread_mutex_lock(&reg_mx);
	s = reg[i].as_set;
	pthread_mutex_unlock(&reg_mx);
	return s;
}

int pc_store_autoscale_entry(int i, const char **name, int *m,
		unsigned long long *gen)
{
	int rc = -1;

	if (i < 0 || i >= pc_store_count())
		return -1;
	pthread_mutex_lock(&reg_mx);
	if (reg[i].live && reg[i].name && reg[i].as_set) {
		*name = reg[i].name;
		*m = reg[i].autoscale;
		*gen = reg[i].as_gen;
		rc = 0;
	}
	pthread_mutex_unlock(&reg_mx);
	return rc;
}

uint32_t pc_store_autoscale_sig(int *n)
{
	uint32_t sum = 0;
	char b[PC_COL_NAME_MAX + 8];
	int i, k = 0;

	pthread_mutex_lock(&reg_mx);
	for (i = 0; i < reg_n; i++) {
		int l;

		if (!reg[i].live || !reg[i].name || !reg[i].as_set)
			continue;
		/* a sum of one hash each: the same on every member whatever
		 * order its registry holds the collections in */
		l = snprintf(b, sizeof b, "%s=%d", reg[i].name, reg[i].autoscale);
		if (l > 0)
			sum += (uint32_t)fnv1a64(b, (size_t)l < sizeof b ? (size_t)l
				: sizeof b - 1);
		k++;
	}
	pthread_mutex_unlock(&reg_mx);
	if (n)
		*n = k;
	return sum;
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
