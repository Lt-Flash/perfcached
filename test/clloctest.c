/*
 * clloctest.c - the locator and negative caches (M3).
 *
 * Both tables are silent when wrong, which is the reason they are worth
 * a test at all:
 *   - a negative entry that outlives its TTL keeps answering "absent"
 *     for a key that came back
 *   - a negative entry that a set fails to clear does the same thing
 *     permanently: the tombstone pair is plant-on-delete,
 *     clear-on-set, and reversing it is invisible until someone reads
 *   - a locator that evicts on collision sends every evicted key back
 *     through placement, which is what 4-way associativity exists to
 *     stop; direct-mapped lost ~30% of a 200k keyspace that way
 *
 * No clock: `now` is injected, so TTL decay is placed exactly on its
 * boundary rather than slept through.
 *
 * Build: cc -o clloctest test/clloctest.c src/clloc.o
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/clloc.h"

static int pass, fail;
static void chk(int cond, const char *what)
{
	if (cond) { pass++; return; }
	fail++;
	printf("  FAIL %s\n", what);
}

static struct clloc L;                     /* ~5MB: never a stack local */
#define K(s) (s), strlen(s)

int main(void)
{
	const long long now = 100000;
	unsigned long long c0;
	int i, found, base_keys[8], nb = 0;

	printf("=== clloctest ===\n");

	/* ---- A. the shared hash ------------------------------------------ */
	chk(clloc_hash(K("col"), K("key")) == clloc_hash(K("col"), K("key")),
		"A1 the hash is stable for the same collection and key");
	chk(clloc_hash(K("col"), K("key")) != clloc_hash(K("col"), K("keyx")),
		"A1 a different key hashes differently");
	chk(clloc_hash(K("ab"), K("c")) != clloc_hash(K("a"), K("bc")),
		"A1 the collection/key boundary is part of the hash - "
		"(ab,c) is not (a,bc)");
	chk(clloc_hash(K(""), K("")) != 0, "A2 the hash is never zero");

	/* ---- B. the negative cache --------------------------------------- */
	chk(!clloc_neg_hit(&L, K("c"), K("absent"), now),
		"B1 a key never seen is not a hit");
	clloc_neg_set(&L, K("c"), K("gone"), now, 500);
	chk(clloc_neg_hit(&L, K("c"), K("gone"), now),
		"B2 a planted key hits inside its TTL");
	chk(clloc_neg_hit(&L, K("c"), K("gone"), now + 499),
		"B3 one ms before expiry it still hits");
	chk(!clloc_neg_hit(&L, K("c"), K("gone"), now + 500),
		"B3 at exactly the expiry it does NOT - the test is strict >");
	clloc_neg_set(&L, K("c"), K("gone2"), now, 500);
	clloc_neg_clear(&L, K("c"), K("gone2"));
	chk(!clloc_neg_hit(&L, K("c"), K("gone2"), now),
		"B4 clear removes the entry before its TTL");
	c0 = clloc_neg_hits(&L);
	(void)clloc_neg_hit(&L, K("c"), K("never-planted"), now);
	chk(clloc_neg_hits(&L) == c0, "B5 a miss does not count as a hit");
	clloc_neg_set(&L, K("c"), K("counted"), now, 500);
	(void)clloc_neg_hit(&L, K("c"), K("counted"), now);
	chk(clloc_neg_hits(&L) == c0 + 1, "B5 a hit does");

	/* ---- C. the locator ---------------------------------------------- */
	chk(clloc_get(&L, K("c"), K("nowhere")) == 0,
		"C1 an unplaced key locates nowhere");
	clloc_set(&L, K("c"), K("k1"), 7);
	chk(clloc_get(&L, K("c"), K("k1")) == 7, "C2 a placed key locates to its node");
	clloc_set(&L, K("c"), K("k1"), 9);
	chk(clloc_get(&L, K("c"), K("k1")) == 9,
		"C3 re-placing updates the entry in place");
	c0 = clloc_loc_clears(&L);
	clloc_clear(&L, K("c"), K("k1"));
	chk(clloc_get(&L, K("c"), K("k1")) == 0, "C4 clear unplaces it");
	chk(clloc_loc_clears(&L) == c0 + 1, "C4 and counts the clear");

	/* C5/C6: find LOC_WAYS+1 keys that share a set, then show the set
	 * holds all four and the fifth evicts exactly one way. */
	{
		/* Two passes, because five keys in DIFFERENT sets prove
		 * nothing: count per set until one reaches LOC_WAYS + 1,
		 * then collect exactly that set's keys. */
		static unsigned short cnt[LOC_SLOTS / LOC_WAYS];
		int target = -1;
		char kb[32];

		for (i = 0; i < 4000000 && target < 0; i++) {
			int b;

			snprintf(kb, sizeof kb, "w%d", i);
			b = (int)(clloc_hash(K("c"), kb, strlen(kb)) %
				(LOC_SLOTS / LOC_WAYS));
			if (++cnt[b] == 5)
				target = b;
		}
		for (i = 0; i < 4000000 && nb < 5 && target >= 0; i++) {
			int b;

			snprintf(kb, sizeof kb, "w%d", i);
			b = (int)(clloc_hash(K("c"), kb, strlen(kb)) %
				(LOC_SLOTS / LOC_WAYS));
			if (b == target)
				base_keys[nb++] = i;
		}
	}
	chk(nb >= 5, "C5 found five keys sharing one locator set");
	if (nb >= 5) {
		char kb[32];

		for (i = 0; i < 4; i++) {
			snprintf(kb, sizeof kb, "w%d", base_keys[i]);
			clloc_set(&L, K("c"), kb, strlen(kb), 10 + i);
		}
		found = 0;
		for (i = 0; i < 4; i++) {
			snprintf(kb, sizeof kb, "w%d", base_keys[i]);
			if (clloc_get(&L, K("c"), kb, strlen(kb)) == 10 + i)
				found++;
		}
		chk(found == 4,
			"C5 all four ways of one set hold at once - this is what "
			"direct-mapped could not do");
		snprintf(kb, sizeof kb, "w%d", base_keys[4]);
		clloc_set(&L, K("c"), kb, strlen(kb), 99);
		chk(clloc_get(&L, K("c"), kb, strlen(kb)) == 99,
			"C6 a fifth key into a full set is placed");
		found = 0;
		for (i = 0; i < 4; i++) {
			snprintf(kb, sizeof kb, "w%d", base_keys[i]);
			if (clloc_get(&L, K("c"), kb, strlen(kb)) == 10 + i)
				found++;
		}
		chk(found == 3, "C6 and evicts exactly one of the four, not the set");
	}

	/* ---- D. the tombstone pair --------------------------------------- */
	clloc_neg_set(&L, K("c"), K("tomb"), now, 10000);
	chk(clloc_neg_hit(&L, K("c"), K("tomb"), now),
		"D1 a delete plants: the next read is suppressed");
	clloc_neg_clear(&L, K("c"), K("tomb"));
	chk(!clloc_neg_hit(&L, K("c"), K("tomb"), now),
		"D1 and a set clears: the resurrected key is visible at once");

	printf("clloctest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
