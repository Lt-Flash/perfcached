/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * chainwalktest.c - OC-7 (DESIGN 12oc): a walk hands over every record of
 * a chain exactly once while the chain is changed under it.
 *
 * A remove fills its hole with the chain's LAST record, so a walker that
 * trusted positions would pass the hole before the move and never meet the
 * record that moved into it - the leg's S330, in a new form.  The chain
 * walk trusts a position only while the bucket's version stands, and walks
 * the chain again from its start when it moves, skipping what it handed
 * over.
 *
 * The shape: a 16-bucket table, 60 keys of one bucket - five home, the rest
 * chained.  The callback removes, every third call, a record it was handed
 * earlier, so the tail fill keeps moving unseen records behind the walker.
 * Every record never removed must be handed over exactly once - through
 * iter_meta and through scan.  Each run ends with the chains' shape check.
 */
#include <stdio.h>
#include <string.h>
#include "../src/compat/compat.h"
#include "../src/compat/mem/mem.h"
#include "../src/compat/str.h"
#include "../src/core/pcache_htable.h"
#include "../src/core/pcache_arena.h"

extern int pcache_arena_hugepage_mb;
void pcache_mem_probe(void);

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

#define N 60

static char keys[N][24];
static pcache_htable_t *ht;
static int seen[N], removed[N], calls, order[N], norder;

static int idx_of(const str *key)
{
	int i;

	for (i = 0; i < N; i++)
		if ((int)strlen(keys[i]) == key->len &&
		    !memcmp(keys[i], key->s, key->len))
			return i;
	return -1;
}

/* every third call: remove the earliest record handed over and still there */
static void step(const str *key)
{
	int i = idx_of(key), k;
	str rk;

	if (i < 0)
		return;
	seen[i]++;
	order[norder++] = i;
	if (++calls % 3)
		return;
	for (k = 0; k < norder; k++) {
		int j = order[k];

		if (j != i && !removed[j]) {
			rk.s = keys[j];
			rk.len = strlen(keys[j]);
			if (pcache_ht_remove(ht, &rk) == 1)
				removed[j] = 1;
			return;
		}
	}
}

static int meta_cb(const str *key, const str *val,
		const struct pcache_rec_meta *m, void *ctx)
{
	(void)val; (void)m; (void)ctx;
	step(key);
	return 0;
}

static int scan_cb(const str *key, const str *val, unsigned int exp,
		void *ctx)
{
	(void)val; (void)exp; (void)ctx;
	step(key);
	return 0;
}

static int fill(void)
{
	str k, v = { "v", 1 };
	unsigned int want = 0, h;
	int n = 0, c;
	char kb[24];

	ht = pcache_htable_new(4);
	if (!ht)
		return -1;
	for (c = 0; n < N && c < 50000000; c++) {
		k.len = snprintf(kb, sizeof kb, "oc7-%d", c);
		k.s = kb;
		h = pcache_key_hash(&k);
		if (c == 0)
			want = h & 15;
		if ((h & 15) != want)
			continue;
		strcpy(keys[n], kb);
		if (pcache_ht_store(ht, &k, &v, 0) != 0)
			return -1;
		n++;
	}
	memset(seen, 0, sizeof seen);
	memset(removed, 0, sizeof removed);
	calls = norder = 0;
	return n == N ? 0 : -1;
}

static void verdict(const char *how)
{
	int i, kept = 0, missed = 0, twice = 0, gone = 0;
	const char *why = pcache_ht_chain_check(ht);

	for (i = 0; i < N; i++) {
		if (removed[i]) {
			gone++;
			continue;
		}
		kept++;
		if (!seen[i])
			missed++;
		else if (seen[i] > 1)
			twice++;
	}
	CHK(gone > 5 && missed == 0 && twice == 0, "%s: %d records never "
		"removed, every one handed over exactly once (%d missed, %d "
		"twice; %d removed behind the walker)", how, kept, missed, twice,
		gone);
	CHK(!why, "%s: the chains keep their shape (%s)", how, why ? why : "yes");
}

int main(void)
{
	unsigned int cursor;
	int guard;

	pcache_mem_probe();
	pcache_arena_hugepage_mb = 64;
	if (pcache_arena_init() != 0) {
		printf("arena init failed\n");
		return 2;
	}
	pcache_ht_default_stat_slots(32);

	if (fill() != 0)
		return 2;
	CHK(pcache_ht_overflow(ht) == N - (PCACHE_SLOTS - 1), "setup: %u of %d "
		"records chained", pcache_ht_overflow(ht), N);
	CHK(pcache_ht_iter_meta(ht, meta_cb, NULL) == 0, "iter_meta ran");
	verdict("iter_meta");

	if (fill() != 0)
		return 2;
	cursor = 0;
	guard = 0;
	do {
		if (pcache_ht_scan(ht, &cursor, 1, scan_cb, NULL) != 0)
			break;
	} while (cursor && ++guard < 100);
	verdict("scan, a bucket a call");

	printf("%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
	return fails ? 1 : 0;
}
