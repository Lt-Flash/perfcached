/* S150 C: a retired table's slots come back.  Retire a table and its
 * regions join the arena's free set: the next table of any size takes
 * them before the frontier moves (regions_bytes and held stay FLAT
 * across up/down cycles), and past the cool-off the give-back punches
 * whole quiet groups of them out above a keep of one group - and the
 * process RSS says so, not a counter.  A punched group comes back on
 * the next take and the table built on it works. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
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

static unsigned long rss_bytes(void)
{
	unsigned long size = 0, resident = 0;
	FILE *f = fopen("/proc/self/statm", "r");

	if (!f)
		return 0;
	if (fscanf(f, "%lu %lu", &size, &resident) != 2)
		resident = 0;
	fclose(f);
	return resident * (unsigned long)sysconf(_SC_PAGESIZE);
}

int main(void)
{
	struct pcache_arena_pressure pr;
	pcache_htable_t *a, *b;
	unsigned long slot, rg1, held1, freed, rss0, rss1, rss2, cold;
	unsigned int k, t;
	str key, val, out;

	pcache_mem_probe();
	pcache_arena_hugepage_mb = 64;
	if (pcache_arena_init() != 0) {
		printf("arena init failed\n");
		return 2;
	}
	pcache_ht_default_stat_slots(32);
	pcache_reclaim_giveback = 1;
	pcache_reclaim_quiet_s = 1;         /* a slot is quiet a tick after it is given back */
	pcache_reclaim_cooloff_s = 0;
	slot = pcache_arena_region_cost(1);

	/* 0. settle: the reservation's never-carved tail is punched once
	 * (S119) by the same tick - get that out of the way, so the RSS
	 * figures below move for the region zone alone */
	for (t = 0; t < 12; t++)
		pcache_arena_reclaim_tick();
	rss0 = rss_bytes();
	pcache_arena_reclaim_tick();
	pcache_arena_reclaim_tick();
	rss1 = rss_bytes();
	CHK(rss0 < rss1 + (1UL << 20) && rss1 < rss0 + (1UL << 20),
		"settled: two more ticks move RSS by less than 1 MB (%lu -> %lu KB)",
		rss0 >> 10, rss1 >> 10);

	/* 1. retire: the regions join the free set, still resident */
	a = pcache_htable_new(16);
	if (!a) {
		printf("create failed\n");
		return 2;
	}
	rg1 = pcache_arena_regions_bytes();
	held1 = pcache_arena_held_bytes();
	freed = pcache_htable_retire(a);
	pcache_arena_pressure(&pr);
	CHK(freed == 20 * slot, "retiring a 2^16 table gives back its 20 slots (%lu)",
		freed / slot);
	CHK(pcache_arena_regions_bytes() == rg1 - 20 * slot,
		"regions_bytes dropped by the 20 slots");
	CHK(pr.regions_free_warm_bytes == 20 * slot && pr.regions_retired == 20,
		"the free set holds 20 warm slots, retired counter 20");
	CHK(pcache_arena_held_bytes() == held1,
		"held is unchanged: retired slots are still resident");
	CHK(pr.warm_free_bytes >= 20 * slot,
		"and they are inside warm_free, so the card still adds up");

	/* 2. the next table takes them before the frontier moves */
	b = pcache_htable_new(16);
	if (!b) {
		printf("re-create failed\n");
		return 2;
	}
	pcache_arena_pressure(&pr);
	CHK(pcache_arena_regions_bytes() == rg1 && pcache_arena_held_bytes() == held1,
		"a new 2^16 table: regions_bytes and held back to the first figure - no carve");
	CHK(pr.region_reuse == 20 && pr.regions_free_warm_bytes == 0,
		"all 20 regions came from the free set (reuse %lu)", pr.region_reuse);

	/* 3. up/down cycles stay flat */
	for (k = 0; k < 5; k++) {
		pcache_htable_retire(b);
		b = pcache_htable_new(16);
		if (!b) {
			printf("cycle %u failed\n", k);
			return 2;
		}
	}
	CHK(pcache_arena_regions_bytes() == rg1 && pcache_arena_held_bytes() == held1,
		"five retire/create cycles: regions_bytes and held flat");

	/* 4. a smaller table is served from the same set */
	pcache_htable_retire(b);
	b = pcache_htable_new(12);              /* 5 slots */
	pcache_arena_pressure(&pr);
	CHK(b && pcache_arena_held_bytes() == held1 && pr.regions_free_warm_bytes == 15 * slot,
		"a 2^12 table took 5 of the 20 free slots, 15 stay warm - no keying by size");
	pcache_htable_retire(b);

	/* 5. the punch: past the cool-off, whole groups above a keep of one
	 * group.  20 free slots = two whole groups at the top of the
	 * reservation and half a third; the keep holds one back. */
	rss0 = rss_bytes();
	for (t = 0; t < 4; t++)
		pcache_arena_reclaim_tick();
	pcache_arena_pressure(&pr);
	rss1 = rss_bytes();
	cold = pr.regions_free_cold_bytes;
	CHK(cold == 8 * slot,
		"one group of 8 retired slots punched out, one kept (cold %lu slots)", cold / slot);
	CHK(pr.regions_free_warm_bytes == 12 * slot, "12 stay warm (%lu)",
		pr.regions_free_warm_bytes / slot);
	CHK(pcache_arena_held_bytes() == held1 - cold,
		"held dropped by what was punched (%lu -> %lu)", held1, pcache_arena_held_bytes());
	/* against the EXPECTED group, not the measured cold figure: a tick
	 * that punched nothing must fail here too, not pass on 0 == 0 */
	CHK(rss0 >= rss1 + 8 * slot - 2 * slot && rss0 <= rss1 + 8 * slot + (1UL << 20),
		"the process RSS dropped by the punched group and not more: %lu -> %lu KB (punched %lu KB)",
		rss0 >> 10, rss1 >> 10, cold >> 10);

	/* 6. a punched group comes back on the next take, and the table
	 * built on it works */
	b = pcache_htable_new(16);
	pcache_arena_pressure(&pr);
	rss2 = rss_bytes();
	CHK(b && pr.regions_free_cold_bytes == 0 && pr.regions_free_warm_bytes == 0 &&
	        pcache_arena_held_bytes() == held1,
		"a 2^16 table took the 12 warm and the 8 punched slots back: held %lu", held1);
	CHK(rss2 >= rss1, "RSS came back with it: %lu KB", rss2 >> 10);
	key.s = "k"; key.len = 1; val.s = "v"; val.len = 1;
	CHK(pcache_ht_store(b, &key, &val, 0) == 0 &&
	        pcache_ht_fetch(b, &key, &out) == 0 && out.len == 1 && out.s[0] == 'v',
		"store and fetch on the rebuilt table");
	pkg_free(out.s);

	printf("regiontest: %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
	return fails ? 1 : 0;
}
