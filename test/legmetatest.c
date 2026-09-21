/* legmetatest.c - S173: the metadata walk must report a record in the
 * OVERFLOW LEG with the same version, flags and write tick as one in a
 * bucket.
 *
 * A key lives in its bucket or, when that bucket is full, in the leg.
 * Both are emitted by the same walk and every consumer of it reads the
 * metadata: the eager sweep filters on the write tick and the PASSIVE
 * flag and sends the version, the RDB writes all three to disk, the
 * bootstrap serve and the collection resize carry them across.
 *
 * Measured on the fleet 2026-09-20 with 39,390 of 63,951 records in the
 * leg: the two nodes holding copies pushed 6.5M records apiece to peers
 * that already had them, every sweep, and every one came back refused as
 * older - the copies looked local (the PASSIVE bit read off the write
 * tick's low byte), looked newly written (the write tick read off the
 * version) and carried a version of 2 (the flags).
 *
 * Fail-first: with the leg's emit call taking its arguments in the
 * struct's order rather than the function's, every leg record here
 * reports version 2, flags from the tick and a tick in the millions.
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

#define N        400            /* 16 buckets x 6 slots = 96 fit; the rest leg */
#define VER0     1000000ULL     /* far above any tick this test can reach */

struct seen {
	unsigned int wt;
	unsigned char fl;
	unsigned long long ver;
	int hits;
};

struct walk {
	struct seen s[N];
	int emitted, unknown;
};

static int meta_cb(const str *key, const str *val, unsigned int exp,
		unsigned int wtick, unsigned char rflags, unsigned long long ver,
		void *ctx)
{
	struct walk *w = ctx;
	int i;

	(void)val;
	(void)exp;
	w->emitted++;
	if (key->len < 2 || sscanf(key->s + 1, "%d", &i) != 1 ||
	        i < 0 || i >= N) {
		w->unknown++;
		return 0;
	}
	w->s[i].wt = wtick;
	w->s[i].fl = rflags;
	w->s[i].ver = ver;
	w->s[i].hits++;
	return 0;
}

int main(void)
{
	pcache_htable_t *ht;
	struct walk w;
	char kb[24], vb[16];
	unsigned int leg, badver = 0, badfl = 0, badwt = 0, missing = 0;
	int i;
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

	/* every record identical in kind - a PASSIVE copy carrying the
	 * sender's version - so bucket and leg differ in nothing but where
	 * they landed.  No grow_at call: nothing moves them back. */
	for (i = 0; i < N; i++) {
		k.len = snprintf(kb, sizeof kb, "k%d", i);
		k.s = kb;
		v.len = snprintf(vb, sizeof vb, "v%d", i);
		v.s = vb;
		if (pcache_ht_store_ver(ht, &k, &v, 0, PCACHE_F_PASSIVE,
		        VER0 + (unsigned long long)i) != 0) {
			printf("store %d failed\n", i);
			return 2;
		}
	}
	leg = ht->ovf_count;
	CHK(leg > N / 2, "%u of %d records are in the overflow leg", leg, N);

	memset(&w, 0, sizeof w);
	if (pcache_ht_iter_meta(ht, meta_cb, &w) != 0) {
		printf("walk failed\n");
		return 2;
	}
	CHK(w.emitted >= N && w.unknown == 0,
		"the walk emitted every record (%d, %d unrecognized)",
		w.emitted, w.unknown);

	for (i = 0; i < N; i++) {
		if (!w.s[i].hits) {
			missing++;
			continue;
		}
		if (w.s[i].ver != VER0 + (unsigned long long)i)
			badver++;
		if (w.s[i].fl != PCACHE_F_PASSIVE)
			badfl++;
		if (w.s[i].wt >= VER0)      /* a tick, not a version */
			badwt++;
	}
	CHK(missing == 0, "every record was offered (%u missing)", missing);
	CHK(badver == 0, "every record kept its version (%u wrong of %d)",
		badver, N);
	CHK(badfl == 0, "every record kept its PASSIVE flag (%u wrong of %d)",
		badfl, N);
	CHK(badwt == 0, "no record's write tick is a version (%u of %d)",
		badwt, N);

	printf("legmetatest: %s\n", fails ? "FAILED" : "ok");
	return fails ? 1 : 0;
}
