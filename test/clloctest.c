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

	/* B-S209. a negative entry carries the delete's VERSION, so a copy
	 * of the record arriving after the tombstone can be told from a
	 * genuinely newer write; an unversioned plant reads as 0 */
	{
		uint64_t v = 7;

		chk(!clloc_neg_ver(&L, K("c"), K("nover"), now, &v) && v == 0,
			"B-S209 no entry: no version, not live");
		clloc_neg_set(&L, K("c"), K("plain"), now, 500);
		chk(clloc_neg_ver(&L, K("c"), K("plain"), now, &v) && v == 0,
			"B-S209 an unversioned plant is live with version 0");
		clloc_neg_set_ver(&L, K("c"), K("vk"), now, 500, 3785530ULL);
		chk(clloc_neg_ver(&L, K("c"), K("vk"), now, &v) && v == 3785530ULL,
			"B-S209 a versioned plant reads back its version");
		chk(clloc_neg_hit(&L, K("c"), K("vk"), now),
			"B-S209 and still answers absent to a pull");
		clloc_neg_set_ver(&L, K("c"), K("vk"), now, 500, 100ULL);
		chk(clloc_neg_ver(&L, K("c"), K("vk"), now, &v) && v == 3785530ULL,
			"B-S209 an OLDER tombstone arriving late does not lower the bar");
		clloc_neg_set_ver(&L, K("c"), K("vk"), now, 500, 4000000ULL);
		chk(clloc_neg_ver(&L, K("c"), K("vk"), now, &v) && v == 4000000ULL,
			"B-S209 a newer delete raises it");
		chk(!clloc_neg_ver(&L, K("c"), K("vk"), now + 500, &v),
			"B-S209 and it lapses with the entry");
	}
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

	/* ---- E/F. RV-15: a 4-way set, oldest-expiry out, counted --------- */
	{
		char ks[6][16];
		uint64_t h0, v = 0;
		unsigned long long n0, t0;
		int j, n = 0;

		/* six keys that land in the same set */
		snprintf(ks[0], sizeof ks[0], "y0");
		h0 = clloc_hash(K("c"), ks[0], strlen(ks[0]));
		for (j = 1; n < 5; j++) {
			char kb2[16];
			uint64_t hb;

			snprintf(kb2, sizeof kb2, "y%d", j);
			hb = clloc_hash(K("c"), kb2, strlen(kb2));
			if (hb != h0 && hb % (NEG_SLOTS / NEG_WAYS) ==
			        h0 % (NEG_SLOTS / NEG_WAYS))
				snprintf(ks[++n], sizeof ks[0], "%s", kb2);
		}
		n0 = clloc_neg_displaced(&L); t0 = clloc_tomb_displaced(&L);
		/* E1-E2: the set's four ways fill without displacing anything,
		 * expiries staggered so the oldest is known: y0 expires first */
		for (j = 0; j < 4; j++)
			clloc_neg_set_ver(&L, K("c"), ks[j], strlen(ks[j]), now + j, 2000, 10 + j);
		clloc_neg_set_ver(&L, K("c"), ks[1], strlen(ks[1]), now + 4, 2000, 20);
		chk(clloc_neg_displaced(&L) == n0,
			"E1 a set's four ways fill, and the same key again, with "
			"nothing displaced");
		/* the fifth evicts the one closest to expiry (y0), only that */
		clloc_neg_set_ver(&L, K("c"), ks[4], strlen(ks[4]), now + 5, 2000, 30);
		chk(clloc_neg_displaced(&L) == n0 + 1 && clloc_tomb_displaced(&L) == t0 + 1,
			"E2 a fifth key into a full set displaces one entry, counted "
			"as a tombstone lost");
		chk(!clloc_neg_ver(&L, K("c"), ks[0], strlen(ks[0]), now + 5, &v) &&
		    clloc_neg_ver(&L, K("c"), ks[4], strlen(ks[4]), now + 5, &v) && v == 30 &&
		    clloc_neg_ver(&L, K("c"), ks[3], strlen(ks[3]), now + 5, &v) && v == 13,
			"E2 and the victim is the entry closest to expiry, not the "
			"fresh ones");
		/* E3: once they expire, their ways are reused without a count */
		clloc_neg_set_ver(&L, K("c"), ks[5], strlen(ks[5]), now + 10000, 2000, 40);
		chk(clloc_neg_displaced(&L) == n0 + 1,
			"E3 an EXPIRED entry's way is reused, not displaced");
		/* E4: a plain negative entry (a pull miss) that must evict a
		 * live tombstone counts as a tombstone lost too */
		for (j = 1; j < 4; j++)
			clloc_neg_set_ver(&L, K("c"), ks[j], strlen(ks[j]), now + 10001, 2000, 50 + j);
		clloc_neg_set(&L, K("c"), ks[0], strlen(ks[0]), now + 10002, 2000);
		chk(clloc_neg_displaced(&L) == n0 + 2 && clloc_tomb_displaced(&L) == t0 + 2,
			"E4 a pull miss that evicts a live tombstone counts as a "
			"tombstone lost");

		/* F: what RV-15 exists for - a FRESH tombstone survives the
		 * colliding deletes right behind it.  Direct-mapped, the first
		 * of them overwrote it; now three cannot (four ways) */
		clloc_neg_set_ver(&L, K("c"), ks[4], strlen(ks[4]), now + 20000, 2000, 99);
		for (j = 1; j < 4; j++)
			clloc_neg_set_ver(&L, K("c"), ks[j], strlen(ks[j]), now + 20000 + j, 2000, 60 + j);
		chk(clloc_neg_ver(&L, K("c"), ks[4], strlen(ks[4]), now + 20004, &v) && v == 99,
			"F1 a fresh tombstone survives three colliding deletes planted "
			"right behind it (direct-mapped, the first one erased it)");
		clloc_neg_set_ver(&L, K("c"), ks[0], strlen(ks[0]), now + 20005, 2000, 70);
		chk(!clloc_neg_ver(&L, K("c"), ks[4], strlen(ks[4]), now + 20005, &v) &&
		    clloc_neg_ver(&L, K("c"), ks[3], strlen(ks[3]), now + 20005, &v),
			"F2 a fourth evicts the OLDEST of them - the tombstone planted "
			"first - and never one planted after it");
	}

	printf("clloctest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
