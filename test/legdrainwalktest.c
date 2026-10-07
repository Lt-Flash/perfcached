/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * legdrainwalktest.c - S330: a walk of the overflow leg must hand over
 * every record while the leg drain moves records out of the very chain
 * it is walking.
 *
 * The walk drops the leg lock around every callback, and the drain
 * (S172, the maintenance thread) unlinks records from a chain in that
 * window.  The walk used to find its place again by POSITION: with two
 * nodes at or before it gone, the position named the node after the
 * next one, and that next one - still in the leg - was never handed
 * over.  CI saw it as 105 of 5,000 stable keys missing from a SCAN under
 * ASan (scantest "storm scan 0"); the same walk feeds the RDB snapshot
 * and the resize copy.
 *
 * Deterministic here: the drain runs from INSIDE the callback, which is
 * exactly the moment another thread can run it.  The shape:
 *   - 16 buckets; one leg chain C, whose eight keys all route to bucket
 *     B (a chain fixes the hash's low 14 bits, so its bucket too)
 *   - B filled with six other keys first, so C's eight land in the leg;
 *     then the six removed, so B has room and nothing drained yet
 *   - a walk whose callback, on the second record it is handed, drains
 *     the leg: six of C's nodes - the two emitted among them - move into
 *     B, and two stay in the chain
 * Every one of the eight must be handed over, by the cursored SCAN and
 * by the whole-table walk (the snapshot's).  FAIL-FIRST: the position
 * walk skips one of the two left in the chain.
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

#define NC 8                      /* keys in chain C */
#define NF PCACHE_SLOTS           /* fillers of bucket B */

static char ckey[NC][24], fkey[NF][24];

struct walk {
	pcache_htable_t *ht;
	int calls, drained_at, moved;
	unsigned char seen[NC];
};

static int c_index(const str *key)
{
	int i;

	for (i = 0; i < NC; i++)
		if (key->len == (int)strlen(ckey[i]) &&
		        !memcmp(key->s, ckey[i], (size_t)key->len))
			return i;
	return -1;
}

/* the second record handed over is the moment the maintenance thread's
 * drain runs: the lock is dropped, as it is for every callback */
static int drain_cb(const str *key, const str *val,
		const struct pcache_rec_meta *mt, void *ctx)
{
	struct walk *w = ctx;
	int i = c_index(key);

	(void)val; (void)mt;
	if (i >= 0)
		w->seen[i] = 1;
	if (++w->calls == 2 && !w->drained_at) {
		w->drained_at = w->calls;
		w->moved = (int)pcache_ht_drain_leg(w->ht, 2 * PCACHE_OVF_BUCKETS);
	}
	return 0;
}

static int iter_cb(const str *key, const str *val, unsigned int expires,
		void *ctx)
{
	(void)expires;
	return drain_cb(key, val, NULL, ctx);
}

/* one table in the shape above; NULL on a setup failure (said why) */
static pcache_htable_t *build(void)
{
	pcache_htable_t *ht = pcache_htable_new(4);   /* 16 buckets */
	unsigned int want = 0, h;
	int nc = 0, nf = 0, n;
	char kb[24];
	str k, v = { "v", 1 };

	if (!ht) {
		printf("create failed\n");
		return NULL;
	}
	/* chain C = the first key's; fillers: same bucket, another chain */
	for (n = 0; (nc < NC || nf < NF) && n < 50000000; n++) {
		k.len = snprintf(kb, sizeof kb, "s330-%d", n);
		k.s = kb;
		h = pcache_key_hash(&k);
		if (n == 0)
			want = h & (PCACHE_OVF_BUCKETS - 1);
		if ((h & (PCACHE_OVF_BUCKETS - 1)) == want) {
			if (nc < NC)
				strcpy(ckey[nc++], kb);
		} else if ((h & 15) == (want & 15) && nf < NF) {
			strcpy(fkey[nf++], kb);
		}
	}
	if (nc < NC || nf < NF) {
		printf("could not find %d chain keys and %d fillers\n", NC, NF);
		return NULL;
	}
	for (n = 0; n < NF; n++) {
		k.s = fkey[n];
		k.len = strlen(fkey[n]);
		if (pcache_ht_store(ht, &k, &v, 0) != 0)
			return NULL;
	}
	for (n = 0; n < NC; n++) {
		k.s = ckey[n];
		k.len = strlen(ckey[n]);
		if (pcache_ht_store(ht, &k, &v, 0) != 0)
			return NULL;
	}
	if (ht->ovf_count != NC) {
		printf("setup: %u records in the leg, wanted %d\n", ht->ovf_count, NC);
		return NULL;
	}
	for (n = 0; n < NF; n++) {
		k.s = fkey[n];
		k.len = strlen(fkey[n]);
		if (pcache_ht_remove(ht, &k) != 1)
			return NULL;
	}
	return ht;
}

static int all(const struct walk *w)
{
	int i, n = 0;

	for (i = 0; i < NC; i++)
		n += w->seen[i];
	return n;
}

int main(void)
{
	pcache_htable_t *ht;
	struct walk w;
	unsigned int cursor;
	int guard;

	pcache_mem_probe();
	pcache_arena_hugepage_mb = 64;
	if (pcache_arena_init() != 0) {
		printf("arena init failed\n");
		return 2;
	}
	pcache_ht_default_stat_slots(32);

	/* 1. the cursored SCAN - the client's, S9's at-least-once promise */
	ht = build();
	if (!ht)
		return 2;
	memset(&w, 0, sizeof w);
	w.ht = ht;
	cursor = 0;
	guard = 0;
	do {
		if (pcache_ht_scan(ht, &cursor, 64, iter_cb, &w) != 0) {
			printf("scan failed\n");
			return 2;
		}
	} while (cursor && ++guard < 100000);
	CHK(w.drained_at == 2 && w.moved == PCACHE_SLOTS,
		"scan: the drain ran inside the walk and moved %d of the chain's %d "
		"(want %d)", w.moved, NC, PCACHE_SLOTS);
	CHK(all(&w) == NC, "scan: every key of the chain was handed over "
		"(%d of %d)", all(&w), NC);

	/* 2. the whole-table walk - the snapshot's, the resize copy's */
	ht = build();
	if (!ht)
		return 2;
	memset(&w, 0, sizeof w);
	w.ht = ht;
	if (pcache_ht_iter_meta(ht, drain_cb, &w) != 0) {
		printf("walk failed\n");
		return 2;
	}
	CHK(w.drained_at == 2 && w.moved == PCACHE_SLOTS,
		"walk: the drain ran inside the walk and moved %d of the chain's %d",
		w.moved, NC);
	CHK(all(&w) == NC, "walk: every key of the chain was handed over "
		"(%d of %d) - what a snapshot or a resize copy carries",
		all(&w), NC);

	printf("legdrainwalktest: %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
	return fails ? 1 : 0;
}
