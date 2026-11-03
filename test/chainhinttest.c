/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * chainhinttest.c - S349, OC-6 (DESIGN 12oc): a record in a bucket's chain
 * must be reaped by the expiry sweep when it expires.
 *
 * The sweep skips every bucket whose hint (the earliest expiry in it) is 0
 * or later than now - that is what keeps a sweep of a large table cheap.
 * Since OC a bucket's hint covers its chain: every path that puts a record
 * anywhere in the chain, or gives one a TTL, lowers it.  S349 found the leg
 * drain forgetting to (a record reaped never, in a bucket of records that
 * do not expire); the chains must not repeat it.
 *
 * The shape, in a 16-bucket table: bucket B filled with six fillers, and a
 * seventh key K of B, which goes to B's chain:
 *   1. fillers with no expiry, K with a short one   - K is reaped
 *   2. fillers that expire late, K with a short one - likewise
 *   3. K stored with no TTL, then EXPIRE gives it one - likewise
 * Positive control: K alone in a bucket is reaped by the same sweep.  Every
 * round also checks the chains' shape.
 */
#include <stdio.h>
#include <string.h>
#include "../src/compat/compat.h"
#include "../src/compat/mem/mem.h"
#include "../src/compat/str.h"
#include "../src/compat/timer.h"
#include "../src/core/pcache_htable.h"
#include "../src/core/pcache_arena.h"

extern int pcache_arena_hugepage_mb;
void pcache_mem_probe(void);

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

#define NF PCACHE_SLOTS           /* fillers of bucket B */

static char fkey[NF][24], kkey[24];

/* NF fillers and one more key, all routed to the same bucket */
static int find_keys(void)
{
	unsigned int want = 0, h;
	int nf = 0, nk = 0, n;
	char kb[24];
	str k;

	for (n = 0; (nf < NF || !nk) && n < 50000000; n++) {
		k.len = snprintf(kb, sizeof kb, "oc6-%d", n);
		k.s = kb;
		h = pcache_key_hash(&k);
		if (n == 0)
			want = h & 15;
		if ((h & 15) != want)
			continue;
		if (nf < NF)
			strcpy(fkey[nf++], kb);
		else if (!nk) {
			strcpy(kkey, kb);
			nk = 1;
		}
	}
	return nf == NF && nk;
}

static int put(pcache_htable_t *ht, const char *key, unsigned int expires)
{
	str k = { (char *)key, (int)strlen(key) }, v = { "v", 1 };

	return pcache_ht_store(ht, &k, &v, expires);
}

/* one round: fillers at @fill_exp, K at @k_exp (0 = none; @touch_to then
 * gives it one by EXPIRE); what the sweep at @now freed, -1 on setup */
static int round_chained(unsigned int fill_exp, unsigned int k_exp,
		unsigned int touch_to, unsigned int now, unsigned long *left)
{
	pcache_htable_t *ht = pcache_htable_new(4);   /* 16 buckets */
	const char *why;
	str k;
	int n;

	if (!ht)
		return -1;
	for (n = 0; n < NF; n++)
		if (put(ht, fkey[n], fill_exp) != 0)
			return -1;
	if (put(ht, kkey, k_exp) != 0)
		return -1;
	if (pcache_ht_overflow(ht) != 2) {      /* slot 5's record and K */
		printf("setup: %u records chained, wanted 2\n",
			pcache_ht_overflow(ht));
		return -1;
	}
	if (touch_to) {
		k.s = kkey;
		k.len = strlen(kkey);
		if (pcache_ht_touch(ht, &k, touch_to) != 1)
			return -1;
	}
	n = (int)pcache_ht_sweep(ht, now, NULL, NULL);
	*left = pcache_ht_entries(ht);
	why = pcache_ht_chain_check(ht);
	CHK(!why, "the chains keep their shape (%s)", why ? why : "yes");
	return n;
}

int main(void)
{
	pcache_htable_t *ht;
	unsigned int t0, soon, late, now, freed;
	unsigned long left;
	int r;

	pcache_mem_probe();
	pcache_arena_hugepage_mb = 64;
	if (pcache_arena_init() != 0) {
		printf("arena init failed\n");
		return 2;
	}
	pcache_ht_default_stat_slots(32);
	if (!find_keys()) {
		printf("could not find %d fillers and one key for one bucket\n", NF);
		return 2;
	}
	t0 = get_ticks();
	soon = t0 + 100;               /* K's expiry */
	late = t0 + 100000;            /* the fillers', in round 2 */
	now = t0 + 200;                /* the sweep: K is due, the fillers not */

	/* positive control: K alone in a bucket, the same sweep */
	ht = pcache_htable_new(4);
	if (!ht || put(ht, kkey, soon) != 0)
		return 2;
	freed = pcache_ht_sweep(ht, now, NULL, NULL);
	CHK(freed == 1 && pcache_ht_entries(ht) == 0,
		"control: a record stored into its bucket is reaped when due "
		"(freed %u, %lu left)", freed, pcache_ht_entries(ht));

	r = round_chained(0, soon, 0, now, &left);
	if (r < 0)
		return 2;
	CHK(r == 1 && left == NF, "chained behind records that never expire: "
		"reaped when due (freed %d, %lu left, want 1 and %d)", r, left, NF);

	r = round_chained(late, soon, 0, now, &left);
	if (r < 0)
		return 2;
	CHK(r == 1 && left == NF, "chained behind records that expire later: "
		"reaped when due, not at the bucket's own expiry (freed %d, %lu "
		"left, want 1 and %d)", r, left, NF);

	r = round_chained(0, 0, soon, now, &left);
	if (r < 0)
		return 2;
	CHK(r == 1 && left == NF, "chained without a TTL, then given one by "
		"EXPIRE: reaped when due (freed %d, %lu left, want 1 and %d)",
		r, left, NF);

	printf("%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
	return fails ? 1 : 0;
}
