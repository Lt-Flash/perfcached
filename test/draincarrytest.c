/*
 * draincarrytest.c - S459: while a resize drains, the new table answers for
 * the records the old one still holds.  A read that misses the new table
 * reads the old one; a write to a key only the old one holds (an add, a TTL
 * bump, a store) carries the record over first; a delete - old table first,
 * then the new one, as the store does it - is never undone by a carry; an
 * expired record is not carried; with the link cleared the old table is
 * not consulted.  Last, a delete and a carry of the same 20,000 keys race
 * on two threads: no key may survive in the new table.  G (OM2-9): a write
 * drops the drained copy, which never answers after the new record expires.
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
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

static str S(const char *s)
{
	str k;

	k.s = (char *)s;
	k.len = (int)strlen(s);
	return k;
}

static int put(pcache_htable_t *ht, const char *k, const char *v,
		unsigned int exp)
{
	str key = S(k), val = S(v);

	return pcache_ht_store(ht, &key, &val, exp);
}

/* the value as a C string, "" when absent */
static const char *get(pcache_htable_t *ht, const char *k)
{
	static char buf[64];
	unsigned int vlen = 0, need = 0;
	str key = S(k);

	if (pcache_ht_fetch_buf(ht, &key, buf, sizeof buf - 1, &vlen, &need) < 0)
		return "";
	buf[vlen] = 0;
	return buf;
}

#define NRACE 20000
static pcache_htable_t *r_old, *r_new;
static volatile int r_go;

static void *race_carry(void *a)
{
	char kb[24];
	str k;
	int i;

	(void)a;
	compat_thread_register(1);
	while (!r_go)
		;
	for (i = 0; i < NRACE; i++) {
		k.len = snprintf(kb, sizeof kb, "race-%d", i);
		k.s = kb;
		(void)pcache_ht_touch(r_new, &k, 0);
	}
	return NULL;
}

static void *race_delete(void *a)
{
	char kb[24];
	str k;
	int i;

	(void)a;
	compat_thread_register(2);
	while (!r_go)
		;
	for (i = 0; i < NRACE; i++) {
		k.len = snprintf(kb, sizeof kb, "race-%d", i);
		k.s = kb;
		(void)pcache_ht_remove(r_old, &k);
		(void)pcache_ht_remove(r_new, &k);
	}
	return NULL;
}

int main(void)
{
	pcache_htable_t *old, *nw;
	pcache_ht_totals_t t0, t1;
	unsigned long long vo = 0, vn = 0;
	unsigned int now;
	long long nv = 0;
	pthread_t a, b;
	char kb[24];
	str k;
	int i, left;

	compat_thread_register(0);
	pcache_mem_probe();
	pcache_arena_hugepage_mb = 64;
	if (pcache_arena_init() != 0) {
		printf("arena init failed\n");
		return 2;
	}
	pcache_ht_default_stat_slots(32);
	old = pcache_htable_new(4);
	nw = pcache_htable_new(6);
	if (!old || !nw) {
		printf("no tables\n");
		return 2;
	}
	now = get_ticks();
	put(old, "k-read", "from-old", 0);
	put(old, "k-touch", "kept", 0);
	put(old, "k-store", "older", 0);
	put(old, "k-del", "gone", 0);
	put(old, "k-exp", "due", now + 1);  /* ticks count from the start: 0 = never */
	put(old, "k-off", "unseen", 0);
	k = S("k-add");
	pcache_ht_add(old, &k, 5, 0, &nv);
	pcache_ht_set_drain(nw, old);

	/* A: reads */
	pcache_ht_totals(nw, &t0);
	CHK(!strcmp(get(nw, "k-read"), "from-old"),
		"A1 a read the new table misses answers from the table it drains");
	pcache_ht_totals(nw, &t1);
	CHK(t1.hits == t0.hits + 1 && t1.misses == t0.misses,
		"A2 and counts a hit, not a miss (hits %lu -> %lu, misses %lu -> %lu)",
		t0.hits, t1.hits, t0.misses, t1.misses);
	k = S("k-read");
	CHK(pcache_ht_probe(nw, &k, NULL, NULL, NULL) == 0,
		"A3 a probe says it is there");
	pcache_ht_getver(old, &k, &vo);
	CHK(pcache_ht_getver(nw, &k, &vn) == 0 && vn && vn == vo,
		"A4 its version reads as the old table's (%llu)", vo);
	CHK(!strcmp(get(old, "k-read"), "from-old") && pcache_ht_entries(nw) == 0,
		"A5 a read carries nothing (the new table holds %lu)",
		pcache_ht_entries(nw));

	/* B: writes carry the record first */
	k = S("k-add");
	CHK(pcache_ht_add(nw, &k, 1, 0, &nv) == 0 && nv == 6,
		"B1 an add to a counter only the old table holds counts on from it: 5 + 1 = %lld", nv);
	k = S("k-touch");
	CHK(pcache_ht_touch(nw, &k, now + 100000) == 1 &&
		!strcmp(get(nw, "k-touch"), "kept"),
		"B2 a TTL bump finds the key and keeps its value");
	pcache_ht_getver(old, &k, &vo);
	pcache_ht_set_drain(nw, NULL);
	vn = 0;
	i = pcache_ht_getver(nw, &k, &vn);
	CHK(!strcmp(get(nw, "k-touch"), "kept") && i == 0 && vn > vo,
		"B3 the carried record is in the new table itself, and the bump, a write (S213), outranks the old copy (version %llu over %llu)",
		vn, vo);
	pcache_ht_set_drain(nw, old);
	put(nw, "k-store", "newer", 0);
	CHK(!strcmp(get(nw, "k-store"), "newer"), "B4 a store overwrites the carried copy");
	{
		str v = S("older");

		k = S("k-store");
		pcache_ht_getver(old, &k, &vo);
		pcache_ht_store_copy(nw, &k, &v, 0, 0, vo, 0);
		CHK(!strcmp(get(nw, "k-store"), "newer"),
			"B5 and the drain's later copy of the old record does not undo it");
	}

	/* C: a delete is not undone */
	k = S("k-del");
	pcache_ht_remove(old, &k);
	pcache_ht_remove(nw, &k);
	CHK(!strcmp(get(nw, "k-del"), ""), "C1 a deleted key reads absent");
	CHK(pcache_ht_add(nw, &k, 1, 0, &nv) == 0 && nv == 1,
		"C2 an add after the delete starts from nothing (%lld)", nv);

	/* D: an expired record is neither read nor carried */
	for (i = 0; i < 40 && get_ticks() <= now + 1; i++)
		usleep(100000);
	CHK(!strcmp(get(nw, "k-exp"), ""), "D1 an expired record in the old table reads absent");
	k = S("k-exp");
	CHK(pcache_ht_touch(nw, &k, 0) == 0, "D2 and a TTL bump does not bring it back");

	/* E: the link cleared - the old table is not consulted */
	pcache_ht_set_drain(nw, NULL);
	CHK(!strcmp(get(nw, "k-off"), ""),
		"E1 with no drain the old table is not read (positive control for A1)");
	k = S("k-off");
	CHK(pcache_ht_add(nw, &k, 1, 0, &nv) == 0 && nv == 1,
		"E2 and not carried from: an add starts at %lld (carried, \"unseen\" would refuse it)", nv);

	/* F: a carry and a delete of the same keys, racing */
	r_old = pcache_htable_new(8);
	r_new = pcache_htable_new(10);
	for (i = 0; i < NRACE; i++) {
		k.len = snprintf(kb, sizeof kb, "race-%d", i);
		k.s = kb;
		put(r_old, kb, "v", 0);
	}
	pcache_ht_set_drain(r_new, r_old);
	pthread_create(&a, NULL, race_carry, NULL);
	pthread_create(&b, NULL, race_delete, NULL);
	r_go = 1;
	pthread_join(a, NULL);
	pthread_join(b, NULL);
	pcache_ht_set_drain(r_new, NULL);
	left = 0;
	for (i = 0; i < NRACE; i++) {
		k.len = snprintf(kb, sizeof kb, "race-%d", i);
		k.s = kb;
		if (pcache_ht_probe(r_new, &k, NULL, NULL, NULL) == 0)
			left++;
	}
	CHK(left == 0, "F1 %d racing carries and deletes: %d key(s) came back", NRACE, left);

	/* G: g-carry is carried by a short-TTL write; g-copied was copied
	 * before the swap, then given a short TTL */
	old = pcache_htable_new(4);
	nw = pcache_htable_new(6);
	now = get_ticks();
	put(old, "g-carry", "old", 0);
	put(old, "g-copied", "old", 0);
	k = S("g-copied");
	pcache_ht_getver(old, &k, &vo);
	{
		str v = S("old");

		pcache_ht_store_copy(nw, &k, &v, 0, 0, vo, 0);
	}
	pcache_ht_set_drain(nw, old);
	put(nw, "g-carry", "new", now + 1);
	CHK(pcache_ht_touch(nw, &k, now + 1) == 1 && !strcmp(get(nw, "g-carry"), "new") &&
		!strcmp(get(nw, "g-copied"), "old"), "G1 before the expiry the new table answers");
	CHK(!strcmp(get(old, "g-carry"), "") && !strcmp(get(old, "g-copied"), ""),
		"G2 and the drained table no longer holds either key");
	for (i = 0; i < 40 && get_ticks() <= now + 1; i++)
		usleep(100000);
	CHK(!strcmp(get(nw, "g-carry"), "") && !strcmp(get(nw, "g-copied"), ""),
		"G3 after the expiry neither answers - not the drained table's immortal copy");
	k = S("g-carry");
	CHK(pcache_ht_getver(nw, &k, &vn) == -2, "G4 nor does its version");
	pcache_ht_set_drain(nw, NULL);

	/* H: h-stale was copied before the swap and written again (version
	 * vo + 10) in the table the swap retires - the new table's copy is older */
	old = pcache_htable_new(4);
	nw = pcache_htable_new(6);
	put(old, "h-stale", "v1", 0);
	k = S("h-stale");
	pcache_ht_getver(old, &k, &vo);
	{
		str v1 = S("v1"), v2 = S("v2"), vm = S("v-mid");
		unsigned long long vx = 0;

		pcache_ht_store_copy(nw, &k, &v1, 0, 0, vo, 0);
		pcache_ht_store_ver(old, &k, &v2, 0, 0, vo + 10);
		pcache_ht_set_drain(nw, old);
		CHK(!strcmp(get(nw, "h-stale"), "v2"),
			"H1 a read answers the drained table's newer copy, not the older one copied before the swap");
		CHK(pcache_ht_getver(nw, &k, &vx) == 0 && vx == vo + 10,
			"H2 and so does its version (%llu)", vx);
		CHK(pcache_ht_store_ver(nw, &k, &vm, 0, 0, vo + 5) == PCACHE_E_OLDER &&
			!strcmp(get(nw, "h-stale"), "v2"),
			"H3 a copy older than the drained one is refused - the write carries it first");
		pcache_ht_set_drain(nw, NULL);
		CHK(!strcmp(get(nw, "h-stale"), "v2"), "H4 and the new table holds it");
	}

	printf("draincarrytest: %s\n", fails ? "FAILED" : "all ok");
	return fails ? 1 : 0;
}
