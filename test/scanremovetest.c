/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * scanremovetest.c - S199: the cursored scan hands over every record in
 * ONE pass while its callback removes each one.
 *
 * pcache_ht_scan_ex is the walk the collection drop removes through.
 * It carried the third copy of the bucket loop, the one S198's guard
 * never reached; S69 had met the skip there first ("17,452 of 20,000
 * removed") and papered over it with passes-until-nothing-moves.  All
 * three walkers now share walk_bucket(), so this is the same assertion
 * as legwalkremovetest against the other entry point: 16 buckets, 400
 * records, one pass, nothing left.
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

#define N 400

struct walk {
	pcache_htable_t *ht;
	unsigned char seen[N];
	int emitted, removed, twice, unknown, noval_ok;
};

static int remove_cb(const str *key, const str *val, unsigned int exp,
		void *ctx)
{
	struct walk *w = ctx;
	int i;

	(void)exp;
	w->emitted++;
	if (val->len == 0)
		w->noval_ok++;            /* NOVAL: the value stayed cold */
	if (key->len < 2 || sscanf(key->s + 1, "%d", &i) != 1 || i < 0 || i >= N) {
		w->unknown++;
		return 0;
	}
	if (w->seen[i]++)
		w->twice++;
	if (pcache_ht_remove(w->ht, key) == 1)
		w->removed++;
	return 0;
}

static int count_cb(const str *key, const str *val, unsigned int exp, void *ctx)
{
	(void)key; (void)val; (void)exp;
	(*(int *)ctx)++;
	return 0;
}

int main(void)
{
	pcache_htable_t *ht;
	struct walk w;
	char kb[24], vb[16];
	unsigned int leg, cursor = 0, calls = 0;
	int i, left = 0, never = 0;
	str k, v;

	pcache_mem_probe();
	pcache_arena_hugepage_mb = 64;
	if (pcache_arena_init() != 0) {
		printf("arena init failed\n");
		return 2;
	}
	pcache_ht_default_stat_slots(32);
	ht = pcache_htable_new(4);
	if (!ht) {
		printf("create failed\n");
		return 2;
	}
	for (i = 0; i < N; i++) {
		k.len = snprintf(kb, sizeof kb, "k%d", i);
		k.s = kb;
		v.len = snprintf(vb, sizeof vb, "v%d", i);
		v.s = vb;
		if (pcache_ht_store(ht, &k, &v, 0) != 0) {
			printf("store %d failed\n", i);
			return 2;
		}
	}
	leg = ht->ovf_count;
	CHK(leg > N / 2, "%u of %d records are in the overflow leg", leg, N);

	/* ONE pass: the drop's own shape - 4096 buckets a call, keys only,
	 * until the cursor comes back to 0 */
	memset(&w, 0, sizeof w);
	w.ht = ht;
	do {
		calls++;
		if (pcache_ht_scan_ex(ht, &cursor, 4096, PCACHE_SCAN_NOVAL,
		        remove_cb, &w) < 0) {
			printf("scan failed\n");
			return 2;
		}
	} while (cursor);
	for (i = 0; i < N; i++)
		if (!w.seen[i])
			never++;
	cursor = 0;
	do {
		pcache_ht_scan_ex(ht, &cursor, 4096, 0, count_cb, &left);
	} while (cursor);

	CHK(w.emitted == N && w.unknown == 0,
		"one pass of the cursored scan hands over every record while "
		"removing (%d of %d emitted, %d unrecognized, %u calls)",
		w.emitted, N, w.unknown, calls);
	CHK(never == 0, "no record was skipped (%d never visited)", never);
	CHK(w.twice == 0, "no record was handed over twice (%d were)", w.twice);
	CHK(w.noval_ok == w.emitted, "keys-only stayed keys-only (%d of %d empty values)",
		w.noval_ok, w.emitted);
	CHK(w.removed == N, "every record was removed in that pass (%d)", w.removed);
	CHK(left == 0 && ht->ovf_count == 0,
		"the table is empty afterwards (%d left, leg %u)", left, ht->ovf_count);
	printf("scanremovetest: %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
	return fails ? 1 : 0;
}
