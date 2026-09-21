/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * legwalkremovetest.c - S198: a walk whose callback REMOVES the record it
 * is handed must still hand over every record.
 *
 * That is what a proxy migration is: mig_cb stubs (removes) each victim
 * before it is sent, from inside pcache_ht_iter_meta().  proxytest on
 * rc16 moved 236 of 400 small records off a 16-bucket table (96 in the
 * buckets, 304 in the overflow leg) and never the rest, on every runner,
 * every time.  A non-mutating walk of the same table sees all 400
 * (KEYS), so the loss is in walking while removing.
 *
 * Same table shape as legmetatest: 16 buckets x 6 slots, 400 records.
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
	int emitted, removed, twice, unknown, remove_failed;
};

static int idx_of(const str *key)
{
	int i;

	if (key->len < 2 || sscanf(key->s + 1, "%d", &i) != 1 || i < 0 || i >= N)
		return -1;
	return i;
}

/* the migration's shape: stub (remove) the record you were just handed */
static int remove_cb(const str *key, const str *val, unsigned int exp,
		unsigned int wtick, unsigned char rflags, unsigned long long ver,
		void *ctx)
{
	struct walk *w = ctx;
	int i = idx_of(key);

	(void)val; (void)exp; (void)wtick; (void)rflags; (void)ver;
	w->emitted++;
	if (i < 0) {
		w->unknown++;
		return 0;
	}
	if (w->seen[i]++)
		w->twice++;
	if (pcache_ht_remove(w->ht, key) == 1)
		w->removed++;
	else
		w->remove_failed++;
	return 0;
}

static int count_cb(const str *key, const str *val, unsigned int exp,
		unsigned int wtick, unsigned char rflags, unsigned long long ver,
		void *ctx)
{
	int *n = ctx;

	(void)key; (void)val; (void)exp; (void)wtick; (void)rflags; (void)ver;
	(*n)++;
	return 0;
}

int main(void)
{
	pcache_htable_t *ht;
	struct walk w;
	char kb[24], vb[16];
	unsigned int leg;
	int i, left = 0, never = 0;
	str k, v;

	pcache_mem_probe();
	pcache_arena_hugepage_mb = 64;
	if (pcache_arena_init() != 0) {
		printf("arena init failed\n");
		return 2;
	}
	pcache_ht_default_stat_slots(32);
	ht = pcache_htable_new(4);            /* 16 buckets: the leg fills fast */
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

	memset(&w, 0, sizeof w);
	w.ht = ht;
	if (pcache_ht_iter_meta(ht, remove_cb, &w) != 0) {
		printf("walk failed\n");
		return 2;
	}
	for (i = 0; i < N; i++)
		if (!w.seen[i])
			never++;
	pcache_ht_iter_meta(ht, count_cb, &left);

	CHK(w.emitted == N && w.unknown == 0,
		"a walk that removes as it goes still hands over every record "
		"(%d of %d emitted, %d unrecognized)", w.emitted, N, w.unknown);
	CHK(never == 0, "no record was skipped (%d never visited)", never);
	CHK(w.twice == 0, "no record was handed over twice (%d were)", w.twice);
	CHK(w.removed == N && w.remove_failed == 0,
		"every visited record was removable (%d removed, %d refused)",
		w.removed, w.remove_failed);
	CHK(left == 0 && ht->ovf_count == 0,
		"the table is empty afterwards (%d left, leg %u)", left,
		ht->ovf_count);
	printf("legwalkremovetest: %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
	return fails ? 1 : 0;
}
