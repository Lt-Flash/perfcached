/*
 * insbench — how much does an insert cost the core, in process?
 *
 * The occupancy bitmap adds an atomic RMW on the 0->1 and 1->0 bucket
 * transitions, and over the wire that would vanish under protocol cost.
 * This times pcache_ht_store() and pcache_ht_remove() directly, at a
 * table size the caller picks, so the transition cost is visible.
 *
 * usage: insbench <buckets_log2> <nkeys> <reps>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/compat/compat.h"
#include "../src/compat/str.h"
#include "../src/compat/timer.h"
#include "../src/core/pcache_mem.h"
#include "../src/core/pcache_arena.h"
#include "../src/core/pcache_htable.h"

extern char *pcache_backing_policy;
extern int pcache_arena_hugepage_mb;

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
	pcache_htable_t *ht;
	unsigned int log2 = argc > 1 ? (unsigned int)atoi(argv[1]) : 18;
	int nkeys = argc > 2 ? atoi(argv[2]) : 50000;
	int reps = argc > 3 ? atoi(argv[3]) : 5;
	char kb[32], vb[256];
	str k, v;
	int i, r;

	pcache_backing_policy = "own";
	pcache_arena_hugepage_mb = 512;
	if (pcache_arena_init() != 0) {
		fprintf(stderr, "arena init failed\n");
		return 1;
	}
	ht = pcache_htable_new(log2);
	if (!ht) {
		fprintf(stderr, "htable failed\n");
		return 1;
	}
	memset(vb, 'V', sizeof vb);
	v.s = vb; v.len = sizeof vb;

	for (r = 0; r < reps; r++) {
		double t0, t1, t2;

		t0 = now_ms();
		for (i = 0; i < nkeys; i++) {
			k.len = snprintf(kb, sizeof kb, "k%08d", i);
			k.s = kb;
			pcache_ht_store(ht, &k, &v, 0);
		}
		t1 = now_ms();
		for (i = 0; i < nkeys; i++) {
			k.len = snprintf(kb, sizeof kb, "k%08d", i);
			k.s = kb;
			pcache_ht_remove(ht, &k);
		}
		t2 = now_ms();
		printf("rep %d: store %7.1f ms (%5.0f ns/op)   "
		       "remove %7.1f ms (%5.0f ns/op)\n",
		       r, t1 - t0, (t1 - t0) * 1e6 / nkeys,
		       t2 - t1, (t2 - t1) * 1e6 / nkeys);
		fflush(stdout);
	}
	return 0;
}
