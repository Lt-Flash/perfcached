/* S151: client-only totals and reach are a RANGE of pstats slots, and the
 * range is the whole story.  process_no is the thread's slot; setting it
 * here plays each thread in turn on one thread, which is exactly what
 * HT_ST sees.  Epochs are driven through compat_ticks_offset. */
#include <stdio.h>
#include <string.h>
#include "../src/compat/compat.h"
#include "../src/compat/mem/mem.h"
#include "../src/compat/str.h"
#include "../src/compat/timer.h"
#include "../src/core/pcache_htable.h"
#include "../src/core/pcache_arena.h"

extern int pcache_arena_hugepage_mb;
extern unsigned int compat_ticks_offset;
extern __thread int process_no;
void pcache_mem_probe(void);

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static void get(pcache_htable_t *ht, int slot, const char *key)
{
	str k, out;

	process_no = slot;
	k.s = (char *)key; k.len = (int)strlen(key);
	if (pcache_ht_fetch(ht, &k, &out) == 0)
		pkg_free(out.s);
	process_no = 0;
}

int main(void)
{
	pcache_htable_t *ht, *plain;
	pcache_ht_totals_t all, cli;
	unsigned int i, w;
	char kb[16];
	str k, v;

	pcache_mem_probe();
	pcache_backing_policy = "own";
	pcache_arena_hugepage_mb = 64;
	if (pcache_arena_init() != 0) { printf("arena init failed\n"); return 2; }

	/* the daemon's registration: workers are slots 1 and 2 */
	pcache_ht_default_client_slots(1, 3);
	ht = pcache_htable_new(10);
	if (!ht) { printf("table failed\n"); return 2; }
	v.s = "v"; v.len = 1;
	process_no = 1;
	for (i = 0; i < 50; i++) {
		k.len = snprintf(kb, sizeof kb, "k%u", i); k.s = kb;
		if (pcache_ht_store(ht, &k, &v, 0) != 0) { printf("store failed\n"); return 2; }
	}
	process_no = 0;
	compat_ticks_offset += 2 * PCACHE_REACH_EPOCH_S;

	/* client worker 1: 10 hits, 5 misses.  worker 2: 4 hits on keys
	 * worker 1 already touched.  slot 0 (recovery): 7 hits.  slot 6
	 * (the peer rx thread when workers = 2): 3 hits on NEW keys, 2 misses */
	for (i = 0; i < 10; i++) { snprintf(kb, sizeof kb, "k%u", i);   get(ht, 1, kb); }
	for (i = 0; i < 5;  i++) { snprintf(kb, sizeof kb, "nope%u", i); get(ht, 1, kb); }
	for (i = 0; i < 4;  i++) { snprintf(kb, sizeof kb, "k%u", i);   get(ht, 2, kb); }
	for (i = 0; i < 7;  i++) { snprintf(kb, sizeof kb, "k%u", i);   get(ht, 0, kb); }
	for (i = 40; i < 43; i++) { snprintf(kb, sizeof kb, "k%u", i);  get(ht, 6, kb); }
	for (i = 0; i < 2;  i++) { snprintf(kb, sizeof kb, "gone%u", i); get(ht, 6, kb); }

	pcache_ht_totals(ht, &all);
	pcache_ht_totals_client(ht, &cli);
	CHK(all.hits == 24 && all.misses == 7,
		"all origins:    hits=%lu misses=%lu (want 24/7)", all.hits, all.misses);
	CHK(cli.hits == 14 && cli.misses == 5,
		"client only:    hits=%lu misses=%lu (want 14/5)", cli.hits, cli.misses);
	CHK(all.entries == 50 && cli.entries == 50,
		"entries is the TABLE's gauge in both: %lu / %lu", all.entries, cli.entries);

	compat_ticks_offset += PCACHE_REACH_EPOCH_S;    /* close the window */
	{
		unsigned long ra = pcache_ht_reach(ht, &w), rc = pcache_ht_reach_client(ht, &w);
		CHK(rc == 10, "reach client:   %lu distinct served (want 10)", rc);
		CHK(ra == 13, "reach all:      %lu (want 13: the peer's 3 new keys count there)", ra);
	}

	/* reset folds BOTH baselines; afterwards the two views diverge again
	 * only by what the non-client slots do */
	pcache_ht_stats_reset(ht);
	pcache_ht_totals(ht, &all); pcache_ht_totals_client(ht, &cli);
	CHK(all.hits == 0 && cli.hits == 0 && all.misses == 0 && cli.misses == 0,
		"after reset both views read zero");
	get(ht, 2, "k1"); get(ht, 2, "k2"); get(ht, 6, "k3");
	pcache_ht_totals(ht, &all); pcache_ht_totals_client(ht, &cli);
	CHK(all.hits == 3 && cli.hits == 2,
		"after reset: all=%lu client=%lu (want 3/2)", all.hits, cli.hits);
	CHK(all.entries == 50, "entries survived the reset: %lu", all.entries);

	/* FAIL-FIRST: with no range registered, client == all.  This is what
	 * a library user sees, and it is what the daemon saw before S151. */
	pcache_ht_default_client_slots(0, 0);
	plain = pcache_htable_new(8);
	process_no = 1; k.s = "x"; k.len = 1; pcache_ht_store(plain, &k, &v, 0); process_no = 0;
	get(plain, 1, "x"); get(plain, 6, "x");
	pcache_ht_totals(plain, &all); pcache_ht_totals_client(plain, &cli);
	CHK(all.hits == 2 && cli.hits == 2,
		"unregistered range: client==all (%lu==%lu) - the range IS the fix", cli.hits, all.hits);

	printf("origintest: %d failed\n", fails);
	return fails ? 1 : 0;
}
