/* S155: the index is carved in whole 256 KB arena slots, and a bucket
 * segment (4,096 x 64 B) is EXACTLY one slot - so the 16-byte region
 * header that sat in front of every region pushed each segment into a
 * second slot, and the 16 KB hint block took a third of its own.
 * Header-less reservation regions and sixteen hint blocks to a slot: a
 * segment costs a slot and a sixteenth.  This pins that geometry, and
 * the oracle the freelist (S150) is measured against: what
 * pcache_htable_index_bytes() quotes is what the arena's regions_bytes
 * moves by, at a create and at a split. */
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

/* header, stat lines and overflow heads: one slot each; n segments at
 * one slot each; a hint slab for every sixteen segments */
static unsigned long expect_slots(unsigned int log2)
{
	unsigned long n = (1UL << log2) >> PCACHE_SEG_BITS;

	if (n == 0)
		n = 1;
	return 3 + n + (n + 15) / 16;
}

int main(void)
{
	unsigned long slot, rg0, rg1, want, got;
	unsigned int l, i, splits;
	pcache_htable_t *ht;
	char kb[24];
	str k, v;

	pcache_mem_probe();
	pcache_arena_hugepage_mb = 64;
	if (pcache_arena_init() != 0) {
		printf("arena init failed\n");
		return 2;
	}
	pcache_ht_default_stat_slots(32);    /* the daemon's shape: one slot of lines */

	slot = pcache_arena_region_cost(1);
	CHK(slot == 256UL << 10, "a one-byte region costs one slot: %lu", slot);
	CHK((unsigned long)PCACHE_SEG_SIZE * sizeof(pcache_bucket_t) == slot,
		"a bucket segment is exactly one slot of buckets");
	CHK(16UL * PCACHE_SEG_SIZE * sizeof(unsigned int) == slot,
		"sixteen hint blocks are exactly one slot");
	got = pcache_arena_region_cost(slot);
	CHK(got == slot, "a slot-sized region costs ONE slot, not two: %lu",
		got);

	for (l = 4; l <= 24; l += 4) {
		want = expect_slots(l) * slot;
		got = pcache_htable_index_bytes(l);
		CHK(got == want, "2^%-2u = %4lu slots = %8.2f MB (quoted %.2f)",
			l, expect_slots(l), want / 1048576.0, got / 1048576.0);
	}
	got = pcache_htable_index_bytes(17) - pcache_htable_index_bytes(16);
	CHK(got == 17 * slot,
		"16 -> 17: sixteen segments and one slab more (%lu slots)",
		got / slot);

	/* the oracle: the quote is the carve, one slab and two */
	for (l = 16; l <= 17; l++) {
		rg0 = pcache_arena_regions_bytes();
		ht = pcache_htable_new(l);
		if (!ht) {
			printf("create 2^%u failed\n", l);
			return 2;
		}
		rg1 = pcache_arena_regions_bytes();
		CHK(rg1 - rg0 == pcache_htable_index_bytes(l),
			"create 2^%u carved %lu slots = quoted %lu", l,
			(rg1 - rg0) / slot, pcache_htable_index_bytes(l) / slot);
	}

	/* a split into a new segment carves the segment and nothing else:
	 * its hint block comes from the slab the create carved */
	ht = pcache_htable_new(12);
	if (!ht) {
		printf("create 2^12 failed\n");
		return 2;
	}
	v.s = "v";
	v.len = 1;
	for (i = 0; i < 18500; i++) {        /* past 75% of 4,096 x 6 slots */
		k.len = snprintf(kb, sizeof kb, "k%u", i);
		k.s = kb;
		if (pcache_ht_store(ht, &k, &v, 0) != 0) {
			printf("store %u failed\n", i);
			return 2;
		}
	}
	rg0 = pcache_arena_regions_bytes();
	splits = pcache_ht_grow_at(ht, 75, 1);
	rg1 = pcache_arena_regions_bytes();
	CHK(splits == 1, "one split at 75%%: %u", splits);
	CHK(rg1 - rg0 == slot, "the first split carved one slot: %lu",
		(rg1 - rg0) / slot);

	printf("indextest: %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
	return fails ? 1 : 0;
}
