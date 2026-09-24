/* S148 coverage sketch: does it actually count DISTINCT served keys?
 * Epochs are driven through compat_ticks_offset so nothing sleeps. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../src/compat/compat.h"
#include "../src/compat/mem/mem.h"
#include "../src/compat/str.h"
#include "../src/compat/timer.h"
#include "../src/core/pcache_htable.h"
#include "../src/core/pcache_arena.h"

extern int pcache_arena_hugepage_mb;
void pcache_mem_probe(void);
extern unsigned int compat_ticks_offset;

static int fails;

/* serve @distinct keys, @reps times each, then close the window */
static unsigned long run(pcache_htable_t *ht, unsigned int distinct,
		unsigned int reps)
{
	unsigned int i, r, w;
	char kb[32];
	str k, out;

	/* land in a fresh epoch so this run cannot inherit the last one */
	compat_ticks_offset += PCACHE_REACH_EPOCH_S * 2;

	for (r = 0; r < reps; r++)
		for (i = 0; i < distinct; i++) {
			k.len = snprintf(kb, sizeof kb, "k%u", i);
			k.s = kb;
			if (pcache_ht_fetch(ht, &k, &out) == 0)
				pkg_free(out.s);
		}
	compat_ticks_offset += PCACHE_REACH_EPOCH_S;   /* close it */
	return pcache_ht_reach(ht, &w);
}

static void chk(const char *what, unsigned long got, unsigned long want,
		double tol)
{
	double err = want ? 100.0 * ((double)got - want) / want : (got ? 100 : 0);
	int ok = want ? (err < 0 ? -err : err) <= tol : got == 0;

	printf("  %-46s got %6lu  want %6lu  err %+6.1f%%  %s\n",
		what, got, want, err, ok ? "ok" : "FAIL");
	if (!ok)
		fails++;
}

int main(void)
{
	pcache_htable_t *ht;
	unsigned int i, w;
	char kb[32];
	str k, v;

	pcache_mem_probe();
	pcache_backing_policy = "own";
	pcache_arena_hugepage_mb = 128;
	if (pcache_arena_init() != 0) { printf("arena init failed\n"); return 2; }
	ht = pcache_htable_new(14);
	if (!ht) { printf("table failed\n"); return 2; }

	v.s = "v"; v.len = 1;
	for (i = 0; i < 4000; i++) {
		k.len = snprintf(kb, sizeof kb, "k%u", i); k.s = kb;
		if (pcache_ht_store(ht, &k, &v, 0) != 0) { printf("store failed\n"); return 2; }
	}
	printf("  table holds 4000 keys, window = %us\n\n", PCACHE_REACH_EPOCH_S);

	chk("1 distinct key, served once",        run(ht, 1, 1),    1,   0);
	chk("4 distinct keys",                    run(ht, 4, 1),    4,   0);
	chk("10 distinct keys",                   run(ht, 10, 1),   10,  10);
	chk("50 distinct keys",                   run(ht, 50, 1),   50,  15);
	chk("200 distinct keys",                  run(ht, 200, 1),  200, 25);
	chk("1000 distinct keys",                 run(ht, 1000, 1), 1000, 25);
	chk("4000 distinct keys",                 run(ht, 4000, 1), 4000, 25);

	printf("\n  the scenario this exists for:\n");
	chk("ONE key served 100,000 times",       run(ht, 1, 100000), 1, 0);
	chk("20 keys served 5,000 times each",    run(ht, 20, 5000), 20, 10);

	printf("\n  and the quiet cases:\n");
	compat_ticks_offset += PCACHE_REACH_EPOCH_S * 3;
	chk("a window nobody read",               pcache_ht_reach(ht, &w), 0, 0);

	printf("\n  %s\n", fails ? "FAILURES ABOVE" : "all within tolerance");
	return fails ? 1 : 0;
}
