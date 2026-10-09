/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * drainhinttest.c - S349: a record the leg drain moves back into its
 * bucket must be reaped by the expiry sweep when it expires.
 *
 * The sweep skips every bucket whose hint (the earliest expiry in it) is
 * 0 or later than now - it is what keeps a sweep of a large table cheap.
 * Every path that puts a record into a bucket lowers the hint to that
 * record's expiry; the leg drain (S172) did not.  A drained record was
 * then reaped only when the bucket's hint came due for some other
 * record, and never at all in a bucket whose other records do not
 * expire: it stayed counted in `entries`, held its memory, and (to a
 * client) read as absent.
 *
 * The shape, in a 16-bucket table: bucket B filled with six fillers; one
 * key K of B stored next, so it lands in the overflow leg with a short
 * expiry; one filler removed, so B has room; the drain moves K into B;
 * the sweep runs past K's expiry.
 *   1. fillers with no expiry  - B's hint is 0: K must still be reaped
 *   2. fillers that expire late - B's hint is later than K: likewise
 * Positive control: K stored straight into a bucket is reaped by the same
 * sweep, so a zero here means the drain path, not the instrument.
 * FAIL-FIRST: before S349 the sweep frees 0 in both cases.
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
		k.len = snprintf(kb, sizeof kb, "s349-%d", n);
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

/*
 * One round: the fillers expire at @fill_exp (0 = never), K at @k_exp.
 * Returns what the sweep at @now freed, or -1 on a setup failure (said
 * why).
 */
static int round_drained(unsigned int fill_exp, unsigned int k_exp,
		unsigned int now, unsigned long *left)
{
	pcache_htable_t *ht = pcache_htable_new(4);   /* 16 buckets */
	str k;
	unsigned int moved, freed;
	int n;

	if (!ht) {
		printf("create failed\n");
		return -1;
	}
	for (n = 0; n < NF; n++)
		if (put(ht, fkey[n], fill_exp) != 0)
			return -1;
	if (put(ht, kkey, k_exp) != 0)
		return -1;
	if (ht->ovf_count != 1) {
		printf("setup: %u records in the leg, wanted 1\n", ht->ovf_count);
		return -1;
	}
	k.s = fkey[0];
	k.len = strlen(fkey[0]);
	if (pcache_ht_remove(ht, &k) != 1)
		return -1;
	moved = pcache_ht_drain_leg(ht, 2 * PCACHE_OVF_BUCKETS);
	if (moved != 1 || ht->ovf_count != 0) {
		printf("setup: the drain moved %u, %u left in the leg\n", moved,
			ht->ovf_count);
		return -1;
	}
	freed = pcache_ht_sweep(ht, now, NULL, NULL);
	*left = pcache_ht_entries(ht);
	return (int)freed;
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

	/* positive control: K straight into a bucket, the same sweep */
	ht = pcache_htable_new(4);
	if (!ht || put(ht, kkey, soon) != 0)
		return 2;
	freed = pcache_ht_sweep(ht, now, NULL, NULL);
	CHK(freed == 1 && pcache_ht_entries(ht) == 0,
		"control: a record stored into its bucket is reaped when due "
		"(freed %u, %lu left)", freed, pcache_ht_entries(ht));

	/* 1. B's other records never expire: its hint is 0 */
	r = round_drained(0, soon, now, &left);
	if (r < 0)
		return 2;
	CHK(r == 1 && left == NF - 1, "drained into a bucket of non-expiring "
		"records: reaped when due (freed %d, %lu left, want 1 and %d)",
		r, left, NF - 1);

	/* 2. B's other records expire later than K */
	r = round_drained(late, soon, now, &left);
	if (r < 0)
		return 2;
	CHK(r == 1 && left == NF - 1, "drained into a bucket that expires "
		"later: reaped when due, not at the bucket's own expiry (freed "
		"%d, %lu left, want 1 and %d)", r, left, NF - 1);

	printf("%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
	return fails ? 1 : 0;
}
