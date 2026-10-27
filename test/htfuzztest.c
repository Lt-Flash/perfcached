/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * htfuzztest.c - TM-1 (DESIGN 12ob): a shadow-model fuzzer for the table
 * and the arena under it.
 *
 * A seeded sequence of operations - store, remove, fetch, probe, touch,
 * getver, a counter add - against a table small enough that the overflow
 * leg carries much of the keyspace, with the maintenance thread's work
 * interleaved: the leg drain (S172), a split, the evacuator (S190), the
 * reclaim tick, and now and then a burst that fills one size class over
 * several chunks and a mass removal that leaves chunks sparse (one class
 * in one chunk - the allocator's current one - never turns sparse, so
 * without the burst the evacuator has nothing to move).  Every result is
 * checked against a plain model that knows nothing of the insides: a
 * key's generation and length, or a counter's integer.  The maintenance steps also run from the S172 test hook - a
 * read's window between its bucket miss and its leg lookup, the moment
 * another thread can run them - so the race classes of 2026-10-22 are in
 * reach without threads.
 *
 * Checked:
 *  - each operation's answer: a value byte-exact, present when the model
 *    has it and absent when it does not, a counter's new value;
 *  - every so many steps, a sweep: every key read back, the whole table
 *    walked for keys the model does not hold, and (a debug build) the
 *    arena ledger's census;
 *  - every so many steps, a quiet phase: the reclaim tick and the
 *    evacuator, over and over with nothing else running, settle - and
 *    move no record more than once (S190's ping-pong moved a class's
 *    last survivors for ever);
 *  - that the evacuator moved records at all in each seed, and some of
 *    them inside a read's window over the three - a run that never
 *    reached S190's path is a failure, not a pass.
 * A failure says the seed, the step and the last 32 operations, and
 * stops; HTFUZZ_SEED and HTFUZZ_OPS replay it.  The default run is three
 * seeds of 40,000 steps.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/compat/compat.h"
#include "../src/compat/mem/mem.h"
#include "../src/compat/str.h"
#include "../src/core/pcache_htable.h"
#include "../src/core/pcache_arena.h"

extern int pcache_arena_hugepage_mb;
void pcache_mem_probe(void);

#define NK     4096            /* value keys */
#define NC     256             /* counter keys */
#define SWEEP  1500            /* steps between sweeps */
#define QUIET  5000            /* steps between quiet phases */
#define RING   32
#define BURST  1200            /* stores of one burst: ~5 chunks of a class */
#define BLEN   900             /* a burst's value length: one class */

static struct {                /* the model */
	unsigned int gen[NK];      /* 0 = absent */
	unsigned short len[NK];
	int cnt_live[NC];
	long long cnt[NC];
} M;

static pcache_htable_t *ht;
static unsigned long long rng;
static unsigned long seed, step;
static char ring[RING][64];
static unsigned int ring_n;
static unsigned long hook_runs, hook_drained, hook_moved, hook_splits;
static unsigned long evac_moved;      /* by every path, hook or not */
static unsigned long hook_moved_all;  /* in a read's window, all seeds */

static unsigned long long rnd(void)
{
	/* xorshift64* */
	rng ^= rng >> 12;
	rng ^= rng << 25;
	rng ^= rng >> 27;
	return rng * 0x2545F4914F6CDD1DULL;
}

static unsigned int below(unsigned int n)
{
	return (unsigned int)(rnd() % n);
}

static void note(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(ring[ring_n++ % RING], sizeof ring[0], fmt, ap);
	va_end(ap);
}

static void fail(const char *fmt, ...)
{
	va_list ap;
	unsigned int i, n = ring_n < RING ? ring_n : RING;

	printf("  FAIL seed %lu step %lu: ", seed, step);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n  the last %u operations, oldest first:\n", n);
	for (i = ring_n - n; i != ring_n; i++)
		printf("    %s\n", ring[i % RING]);
	printf("  replay: HTFUZZ_SEED=%lu HTFUZZ_OPS=%lu ./htfuzztest (a seed after "
		"the first ran on an arena the earlier seeds used, so alone it can "
		"differ)\n", seed, step + 1);
	printf("htfuzztest: FAILED\n");
	exit(1);
}

static void kname(char *b, size_t cap, unsigned int i)
{
	snprintf(b, cap, "fz%05u", i);
}

static void cname(char *b, size_t cap, unsigned int i)
{
	snprintf(b, cap, "fc%03u", i);
}

/* key @i at generation @g: @len bytes no other (key, generation) shares */
static void vbytes(char *v, unsigned int i, unsigned int g, unsigned int len)
{
	unsigned int x = i * 2654435761u ^ g * 40503u, k;

	for (k = 0; k < len; k++) {
		x = x * 1103515245u + 12345u;
		v[k] = (char)(x >> 16);
	}
}

static unsigned int vlen_pick(void)
{
	unsigned int r = below(100);

	/* three classes of sizes, so the arena holds several */
	return r < 50 ? 1 + below(40) : r < 85 ? 41 + below(200) : 241 + below(760);
}

/* ---- maintenance, as the maintenance thread runs it ---------------- */
static unsigned int maint_drain(void)
{
	return pcache_ht_drain_leg(ht, 512);
}

static unsigned int maint_evac(void)
{
	unsigned int m;

	pcache_arena_flush_private();
	pcache_arena_reclaim_tick();
	m = pcache_ht_evacuate(ht, 256, 64);
	evac_moved += m;
	return m;
}

static unsigned int maint_split(void)
{
	return pcache_ht_grow_at(ht, 60, 2);
}

/* the S172 hook: some reads have the maintenance thread run in their
 * window - a drain, a split then a drain, or the evacuator */
static void leg_hook(pcache_htable_t *t, const str *key)
{
	(void)t; (void)key;
	if (below(16))
		return;
	hook_runs++;
	switch (below(4)) {
	case 0:
	case 1:
		hook_drained += maint_drain();
		break;
	case 2:
		hook_splits += maint_split();
		hook_drained += maint_drain();
		break;
	default:
		hook_moved += maint_evac();
		break;
	}
}

/* ---- the checks ----------------------------------------------------- */
static void check_value(unsigned int i, const char *what)
{
	char kb[16], buf[1024], want[1024];
	unsigned int vlen = 0, needed = 0;
	str k;
	int rc;

	kname(kb, sizeof kb, i);
	k.s = kb;
	k.len = (int)strlen(kb);
	rc = pcache_ht_fetch_buf(ht, &k, buf, sizeof buf, &vlen, &needed);
	if (!M.gen[i]) {
		if (rc != -2)
			fail("%s %s: absent in the model, the table answers %d (%u bytes)",
				what, kb, rc, vlen);
		return;
	}
	if (rc != 0)
		fail("%s %s: gen %u, %u bytes in the model, the table answers %d",
			what, kb, M.gen[i], M.len[i], rc);
	vbytes(want, i, M.gen[i], M.len[i]);
	if (vlen != M.len[i] || memcmp(buf, want, vlen))
		fail("%s %s: gen %u - %u bytes back, %u in the model%s", what, kb,
			M.gen[i], vlen, M.len[i], vlen == M.len[i] ? ", bytes differ" : "");
}

static void check_counter(unsigned int i, const char *what)
{
	char kb[16], buf[64], want[32];
	unsigned int vlen = 0, needed = 0;
	str k;
	int rc;

	cname(kb, sizeof kb, i);
	k.s = kb;
	k.len = (int)strlen(kb);
	rc = pcache_ht_fetch_buf(ht, &k, buf, sizeof buf, &vlen, &needed);
	if (!M.cnt_live[i]) {
		if (rc != -2)
			fail("%s %s: no counter in the model, the table answers %d",
				what, kb, rc);
		return;
	}
	snprintf(want, sizeof want, "%lld", M.cnt[i]);
	if (rc != 0 || vlen != strlen(want) || memcmp(buf, want, vlen))
		fail("%s %s: the model says %s, the table answers %d '%.*s'", what,
			kb, want, rc, (int)(vlen < sizeof buf ? vlen : 0), buf);
}

/* the walk: every key it hands over is one the model holds, once */
static unsigned char walk_seen[NK + NC];
static unsigned long walk_n;

static int walk_cb(const str *key, const str *val, unsigned int expires,
		void *ctx)
{
	unsigned int i;
	char want[1024];

	(void)expires; (void)ctx;
	walk_n++;
	if (key->len == 7 && !memcmp(key->s, "fz", 2)) {
		i = (unsigned int)atoi(key->s + 2);
		if (i >= NK || !M.gen[i])
			fail("the walk handed over %.*s, which the model does not hold",
				key->len, key->s);
		if (walk_seen[i]++)
			fail("the walk handed over %.*s twice", key->len, key->s);
		vbytes(want, i, M.gen[i], M.len[i]);
		if ((unsigned int)val->len != M.len[i] || memcmp(val->s, want, M.len[i]))
			fail("the walk's value of %.*s differs from the model's",
				key->len, key->s);
		return 0;
	}
	if (key->len == 5 && !memcmp(key->s, "fc", 2)) {
		i = (unsigned int)atoi(key->s + 2);
		if (i >= NC || !M.cnt_live[i])
			fail("the walk handed over counter %.*s, which the model does "
				"not hold", key->len, key->s);
		if (walk_seen[NK + i]++)
			fail("the walk handed over %.*s twice", key->len, key->s);
		return 0;
	}
	fail("the walk handed over a key the fuzzer never wrote: %.*s",
		key->len, key->s);
	return 0;
}

static unsigned long live(void)
{
	unsigned long n = 0;
	unsigned int i;

	for (i = 0; i < NK; i++)
		n += M.gen[i] != 0;
	for (i = 0; i < NC; i++)
		n += M.cnt_live[i] != 0;
	return n;
}

static void sweep(void)
{
	unsigned int i;

	note("sweep");
	for (i = 0; i < NK; i++)
		check_value(i, "sweep");
	for (i = 0; i < NC; i++)
		check_counter(i, "sweep");
	memset(walk_seen, 0, sizeof walk_seen);
	walk_n = 0;
	if (pcache_ht_iter(ht, walk_cb, NULL) != 0)
		fail("the walk failed");
	if (walk_n != live())
		fail("the walk handed over %lu records, the model holds %lu",
			walk_n, live());
#ifdef PCACHE_ARENA_DEBUG
	{
		char why[256];

		if (pcache_arena_census(why, sizeof why) != 0)
			fail("the arena census: %s", why);
	}
#endif
}

/* S190: with nothing else running, the evacuator settles, and moves no
 * record more than once */
static void quiet(void)
{
	unsigned int r, m, still = 0;
	unsigned long total = 0, n = live();

	note("quiet phase, %lu live", n);
	pcache_arena_flush_private();
	for (r = 0; r < 40 && still < 3; r++) {
		pcache_arena_reclaim_tick();
		m = pcache_ht_evacuate(ht, pcache_ht_nbuckets(ht), 1u << 20);
		evac_moved += m;
		total += m;
		still = m ? 0 : still + 1;
	}
	if (still < 3)
		fail("the evacuator did not settle in 40 rounds with nothing else "
			"running (%lu moves, %lu live records)", total, n);
	if (total > n)
		fail("the evacuator moved %lu records in one quiet phase - more than "
			"the %lu live: some moved more than once", total, n);
}

/* ---- one run --------------------------------------------------------- */
static void run(unsigned long s, unsigned long ops)
{
	char kb[16], vb[1024];
	unsigned int i, len, r;
	unsigned long long ver;
	long long nv;
	str k, v;
	int rc;

	seed = s;
	rng = s * 0x9E3779B97F4A7C15ULL + 1;
	memset(&M, 0, sizeof M);
	ring_n = 0;
	hook_runs = hook_drained = hook_moved = hook_splits = 0;
	evac_moved = 0;
	ht = pcache_htable_new(8);             /* 256 buckets: the leg works */
	if (!ht)
		fail("the table was not created");
	for (step = 0; step < ops; step++) {
		r = below(1000);
		if (r < 300) {                      /* store */
			i = below(NK);
			len = vlen_pick();
			kname(kb, sizeof kb, i);
			vbytes(vb, i, (unsigned int)step + 1, len);
			k.s = kb; k.len = (int)strlen(kb);
			v.s = vb; v.len = (int)len;
			note("store %s gen %lu, %u bytes", kb, step + 1, len);
			if ((rc = pcache_ht_store(ht, &k, &v, 0)) != 0)
				fail("store %s answered %d", kb, rc);
			M.gen[i] = (unsigned int)step + 1;
			M.len[i] = (unsigned short)len;
		} else if (r < 440) {               /* remove */
			i = below(NK);
			kname(kb, sizeof kb, i);
			k.s = kb; k.len = (int)strlen(kb);
			note("remove %s", kb);
			rc = pcache_ht_remove(ht, &k);
			if (rc != (M.gen[i] ? 1 : 0))
				fail("remove %s answered %d, the model %s it", kb, rc,
					M.gen[i] ? "holds" : "does not hold");
			M.gen[i] = 0;
		} else if (r < 690) {               /* fetch */
			i = below(NK);
			kname(kb, sizeof kb, i);
			note("fetch %s", kb);
			check_value(i, "fetch");
		} else if (r < 770) {               /* probe */
			i = below(NK);
			kname(kb, sizeof kb, i);
			k.s = kb; k.len = (int)strlen(kb);
			note("probe %s", kb);
			rc = pcache_ht_probe(ht, &k, &len, NULL, NULL);
			if (rc != (M.gen[i] ? 0 : -2) || (!rc && len != M.len[i]))
				fail("probe %s answered %d (%u bytes); the model: %s, %u bytes",
					kb, rc, len, M.gen[i] ? "held" : "absent", M.len[i]);
		} else if (r < 820) {               /* touch: a far deadline */
			i = below(NK);
			kname(kb, sizeof kb, i);
			k.s = kb; k.len = (int)strlen(kb);
			note("touch %s", kb);
			rc = pcache_ht_touch(ht, &k, 0x7FFFFFF0u);
			if (rc != (M.gen[i] ? 1 : 0))
				fail("touch %s answered %d, the model %s it", kb, rc,
					M.gen[i] ? "holds" : "does not hold");
		} else if (r < 860) {               /* getver */
			i = below(NK);
			kname(kb, sizeof kb, i);
			k.s = kb; k.len = (int)strlen(kb);
			note("getver %s", kb);
			rc = pcache_ht_getver(ht, &k, &ver);
			if (rc != (M.gen[i] ? 0 : -2))
				fail("getver %s answered %d, the model %s it", kb, rc,
					M.gen[i] ? "holds" : "does not hold");
		} else if (r < 920) {               /* a counter */
			i = below(NC);
			cname(kb, sizeof kb, i);
			k.s = kb; k.len = (int)strlen(kb);
			if (below(10)) {
				long long d = (long long)below(2001) - 1000;

				note("add %s %lld", kb, d);
				if ((rc = pcache_ht_add(ht, &k, d, 0, &nv)) != 0)
					fail("add %s answered %d", kb, rc);
				if (nv != (M.cnt_live[i] ? M.cnt[i] : 0) + d)
					fail("add %s %lld gave %lld; the model had %lld", kb, d,
						nv, M.cnt_live[i] ? M.cnt[i] : 0);
				M.cnt[i] = nv;
				M.cnt_live[i] = 1;
			} else {
				note("remove %s", kb);
				rc = pcache_ht_remove(ht, &k);
				if (rc != (M.cnt_live[i] ? 1 : 0))
					fail("remove %s answered %d", kb, rc);
				M.cnt_live[i] = 0;
			}
		} else if (r < 990) {               /* the maintenance thread */
			switch (below(4)) {
			case 0: note("drain %u", maint_drain()); break;
			case 1: note("split %u", maint_split()); break;
			case 2: note("evacuate %u", maint_evac()); break;
			default:
				pcache_arena_flush_private();
				pcache_arena_reclaim_tick();
				note("reclaim tick");
			}
		} else if (r == 998 && !below(4)) { /* a burst into one class */
			unsigned int n;

			for (n = 0; n < BURST; n++) {
				i = below(NK);
				kname(kb, sizeof kb, i);
				vbytes(vb, i, (unsigned int)step + 1, BLEN);
				k.s = kb; k.len = (int)strlen(kb);
				v.s = vb; v.len = BLEN;
				if ((rc = pcache_ht_store(ht, &k, &v, 0)) != 0)
					fail("burst: store %s answered %d", kb, rc);
				M.gen[i] = (unsigned int)step + 1;
				M.len[i] = BLEN;
			}
			note("burst: %u stores of %u bytes", BURST, BLEN);
		} else if (r == 999 && !below(4)) { /* a mass removal */
			unsigned int n = 0;

			for (i = 0; i < NK; i++) {
				if (!M.gen[i] || below(10) == 0)
					continue;           /* nine in ten go */
				kname(kb, sizeof kb, i);
				k.s = kb; k.len = (int)strlen(kb);
				if (pcache_ht_remove(ht, &k) != 1)
					fail("mass removal: %s was not there", kb);
				M.gen[i] = 0;
				n++;
			}
			note("mass removal: %u", n);
		} else {
			i = below(NK);
			kname(kb, sizeof kb, i);
			note("fetch %s", kb);
			check_value(i, "fetch");
		}
		if ((step + 1) % SWEEP == 0)
			sweep();
		if ((step + 1) % QUIET == 0) {
			quiet();
			sweep();
		}
	}
	sweep();
	quiet();
	sweep();
	if (!evac_moved)
		fail("the evacuator moved no record in %lu steps - the run never "
			"reached S190's path", ops);
	printf("  ok   seed %lu: %lu steps agree with the model (%lu live, %u "
		"buckets, %u in the leg; the evacuator moved %lu; the hook ran the "
		"maintenance thread %lu times: %lu drained, %lu split, %lu "
		"evacuated)\n", s, ops, live(), pcache_ht_nbuckets(ht),
		ht->ovf_count, evac_moved, hook_runs, hook_drained, hook_splits,
		hook_moved);
	hook_moved_all += hook_moved;
}

int main(void)
{
	const char *es = getenv("HTFUZZ_SEED"), *eo = getenv("HTFUZZ_OPS");
	unsigned long ops = eo ? strtoul(eo, NULL, 10) : 40000, s;

	pcache_mem_probe();
	pcache_backing_policy = "own";
	pcache_arena_hugepage_mb = 64;
	if (pcache_arena_init() != 0) {
		printf("arena init failed\n");
		return 2;
	}
	pcache_ht_default_stat_slots(32);
	pcache_reclaim_giveback = 1;
	pcache_reclaim_quiet_s = 1;
	pcache_reclaim_cooloff_s = 0;
	pcache_reclaim_keep = 1;
	pcache_ht_test_leg_hook = leg_hook;

	if (es) {
		run(strtoul(es, NULL, 10), ops);
	} else {
		for (s = 1; s <= 3; s++)
			run(s, ops);
		if (!hook_moved_all) {
			printf("  FAIL the evacuator never moved a record inside a "
				"read's window in three seeds - the S190 race was not "
				"reached\nhtfuzztest: FAILED\n");
			return 1;
		}
	}
	printf("htfuzztest: passed\n");
	return 0;
}
