/* legmetatest.c - the metadata walk gives a record in the OVERFLOW LEG the same version, flags
 * and write tick as one in a bucket: the sweep, the RDB, bootstrap and resize all read them. */
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

static int meta_cb(const str *key, const str *val,
		const struct pcache_rec_meta *mt, void *ctx)
{
	struct walk *w = ctx;
	int i;

	(void)val;
	w->emitted++;
	if (key->len < 2 || sscanf(key->s + 1, "%d", &i) != 1 ||
	        i < 0 || i >= N) {
		w->unknown++;
		return 0;
	}
	w->s[i].wt = mt->wtick;
	w->s[i].fl = mt->rflags;
	w->s[i].ver = mt->ver;
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

	/* every record a PASSIVE copy carrying the sender's version, so bucket and leg
	 * differ only in where they landed; no grow_at call moves them back */
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
	leg = pcache_ht_overflow(ht);
	CHK(leg > N / 2, "%u of %d records are in chains", leg, N);

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
