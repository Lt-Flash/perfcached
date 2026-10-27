/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * legracetest.c - S172: a lock-free read must find a record the leg drain
 * moves from the overflow leg into its bucket while the read is between
 * the two.
 *
 * fetch_core settles its bucket first - a miss - and looks in the leg
 * after.  The drain (the maintenance thread) takes the bucket lock and
 * the leg lock, puts the record in the bucket, and unlinks it from the
 * leg.  A read whose bucket look came before the move and whose leg look
 * came after it found the key in neither, and answered "absent" for a key
 * present the whole time; when the move emptied the leg the read did not
 * look in the leg at all.  Found reading the core for the in-process
 * module that shares it, and shown there failing first.
 *
 * Deterministic here: this build has PCACHE_TEST_HOOKS, and the drain
 * runs from the hook fetch_core calls between its bucket miss and its
 * leg lookup - exactly the moment another thread can run it.  The shape:
 *   - 16 buckets; bucket B filled with PCACHE_SLOTS other keys, so the
 *     keys under test land in the leg; then one filler removed, so B has
 *     room for one record and nothing has drained yet
 *   A  two keys in the leg, K on an earlier chain than K2: the drain
 *      moves K into B, which is then full, and K2 stays - a read of K,
 *      with the leg still holding a record
 *   B  K alone in the leg: the drain moves it and the leg is empty - a
 *      read of K that would skip the leg
 *   C  as B, through the probe (no copy) - the EXISTS / TTL path
 * Each checks the drain ran in the window and moved K (the race was
 * there to lose), and the read found K's value.
 *
 * The locked readers (touch, getver, setver) look in the leg under the
 * bucket lock now, as store, incr and remove always did - a structural
 * change with no window left to hook.
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

#define NF PCACHE_SLOTS           /* fillers of bucket B */

static char kkey[24], k2key[24], fkey[NF][24];

/* the hook: on the first read of K, the drain runs - once */
static struct {
	const char *key;
	int calls, moved;
} H;

static void leg_hook(pcache_htable_t *ht, const str *key)
{
	if (!H.key || H.calls || key->len != (int)strlen(H.key) ||
	        memcmp(key->s, H.key, (size_t)key->len))
		return;
	H.calls++;
	H.moved = (int)pcache_ht_drain_leg(ht, 2 * PCACHE_OVF_BUCKETS);
}

static int store(pcache_htable_t *ht, const char *key, const char *val)
{
	str k, v;

	k.s = (char *)key;
	k.len = strlen(key);
	v.s = (char *)val;
	v.len = strlen(val);
	return pcache_ht_store(ht, &k, &v, 0);
}

/* K, K2 (when @two) and the fillers: one bucket of 16, K's chain before
 * K2's; NULL on a setup failure (said why) */
static pcache_htable_t *build(int two)
{
	pcache_htable_t *ht = pcache_htable_new(4);   /* 16 buckets */
	unsigned int bkt = 0, ck = 0, h, c;
	int have_k = 0, have_k2 = !two, nf = 0, n;
	char kb[24];
	str k;

	if (!ht) {
		printf("create failed\n");
		return NULL;
	}
	for (n = 0; (!have_k || !have_k2 || nf < NF) && n < 50000000; n++) {
		k.len = snprintf(kb, sizeof kb, "s172-%d", n);
		k.s = kb;
		h = pcache_key_hash(&k);
		c = h & (PCACHE_OVF_BUCKETS - 1);
		if (!have_k) {
			/* an early chain, so the drain's walk from 0 reaches it
			 * before K2's */
			if (c >= PCACHE_OVF_BUCKETS / 4)
				continue;
			strcpy(kkey, kb);
			bkt = h & 15;
			ck = c;
			have_k = 1;
			continue;
		}
		if ((h & 15) != bkt)
			continue;
		if (!have_k2 && c > ck) {
			strcpy(k2key, kb);
			have_k2 = 1;
		} else if (c != ck && nf < NF) {
			strcpy(fkey[nf++], kb);
		}
	}
	if (!have_k || !have_k2 || nf < NF) {
		printf("could not find the keys: K %d, K2 %d, %d of %d fillers\n",
			have_k, have_k2, nf, NF);
		return NULL;
	}
	for (n = 0; n < NF; n++)
		if (store(ht, fkey[n], "f") != 0)
			return NULL;
	if (store(ht, kkey, "value-of-k") != 0 ||
	        (two && store(ht, k2key, "value-of-k2") != 0))
		return NULL;
	if (ht->ovf_count != (unsigned int)(two ? 2 : 1)) {
		printf("setup: %u records in the leg, wanted %d\n", ht->ovf_count,
			two ? 2 : 1);
		return NULL;
	}
	k.s = fkey[0];
	k.len = strlen(fkey[0]);
	if (pcache_ht_remove(ht, &k) != 1)
		return NULL;
	return ht;
}

int main(void)
{
	pcache_htable_t *ht;
	char buf[64];
	unsigned int vlen, needed;
	int rc;
	str k;

	pcache_mem_probe();
	pcache_arena_hugepage_mb = 64;
	if (pcache_arena_init() != 0) {
		printf("arena init failed\n");
		return 2;
	}
	pcache_ht_default_stat_slots(32);
	pcache_ht_test_leg_hook = leg_hook;

	/* A: the leg keeps a record */
	if (!(ht = build(1)))
		return 2;
	memset(&H, 0, sizeof H);
	H.key = kkey;
	k.s = kkey;
	k.len = strlen(kkey);
	vlen = needed = 0;
	rc = pcache_ht_fetch_buf(ht, &k, buf, sizeof buf, &vlen, &needed);
	CHK(H.calls == 1 && H.moved == 1 && ht->ovf_count == 1,
		"A: the drain ran between the bucket miss and the leg lookup, moved "
		"K, and left K2 in the leg (ran %d, moved %d, leg %u)", H.calls,
		H.moved, ht->ovf_count);
	CHK(rc == 0 && vlen == 10 && !memcmp(buf, "value-of-k", 10),
		"A: the read found K in its bucket (rc %d, %u bytes)", rc, vlen);

	/* B: the move empties the leg */
	if (!(ht = build(0)))
		return 2;
	memset(&H, 0, sizeof H);
	H.key = kkey;
	k.s = kkey;
	k.len = strlen(kkey);
	vlen = needed = 0;
	rc = pcache_ht_fetch_buf(ht, &k, buf, sizeof buf, &vlen, &needed);
	CHK(H.calls == 1 && H.moved == 1 && ht->ovf_count == 0,
		"B: the drain moved K, the leg's only record (ran %d, moved %d, "
		"leg %u)", H.calls, H.moved, ht->ovf_count);
	CHK(rc == 0 && vlen == 10 && !memcmp(buf, "value-of-k", 10),
		"B: the read found K although the leg it was in is empty (rc %d, "
		"%u bytes)", rc, vlen);

	/* C: the probe - existence, no copy */
	if (!(ht = build(0)))
		return 2;
	memset(&H, 0, sizeof H);
	H.key = kkey;
	k.s = kkey;
	k.len = strlen(kkey);
	vlen = 0;
	rc = pcache_ht_probe(ht, &k, &vlen, NULL, NULL);
	CHK(H.calls == 1 && H.moved == 1 && rc == 0 && vlen == 10,
		"C: the probe found K across the move (ran %d, moved %d, rc %d, "
		"%u bytes)", H.calls, H.moved, rc, vlen);

	printf("legracetest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
