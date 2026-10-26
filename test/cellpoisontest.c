/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * cellpoisontest.c - TM-2 (DESIGN 12ob): what AddressSanitizer is told
 * about the arena's cells, read back from ASan's own shadow.
 *
 * In an ASan build (check-asan; a plain build says so and skips):
 *  A  a cell handed out is open whole - the table rewrites a record in
 *     place up to the cell's bound
 *  B  the chunk's next cell, not handed out yet, is closed past its head
 *  C  a freed cell is closed past PCACHE_CELL_OPEN, and its class byte and
 *     link stay open
 *  D  handed out again, it is open whole again
 *  E  sent home (the process's private cells flushed), still closed
 *  F  a chunk retired is closed whole, head and all; the next carve opens
 *     what it hands out
 * Fail-first: an arena that tells ASan nothing leaves the freed cell open
 * (C), and the retired chunk (F).
 */
#include <stdio.h>
#include <string.h>
#include "../src/compat/compat.h"
#include "../src/compat/mem/mem.h"
#include "../src/core/pcache_arena.h"
#include "../src/core/pcache_asan.h"

extern int pcache_arena_hugepage_mb;
void pcache_mem_probe(void);

#ifdef PCACHE_ASAN
static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static int open_whole(const void *p, unsigned long n)
{
	return __asan_region_is_poisoned((void *)p, n) == NULL;
}

static int closed(const void *p)
{
	return __asan_address_is_poisoned(p) != 0;
}
#endif

int main(void)
{
#ifndef PCACHE_ASAN
	printf("cellpoisontest: skipped - not an ASan build (check-asan runs it)\n");
	return 0;
#else
	enum { BIG = 4096, NBIG = 64 };
	char *a, *b, *big[NBIG], *n;
	unsigned int sz, i, t;

	pcache_mem_probe();
	pcache_backing_policy = "own";
	pcache_arena_hugepage_mb = 64;
	if (pcache_arena_init() != 0) {
		printf("arena init failed\n");
		return 2;
	}
	pcache_reclaim_giveback = 0;
	pcache_reclaim_quiet_s = 0;
	pcache_reclaim_cooloff_s = 0;
	pcache_reclaim_keep = 0;           /* a drained chunk retires at once */

	/* A, B: the first cell of a class comes off a fresh chunk's bump */
	a = pcache_cell_alloc(100);
	sz = pcache_cell_bound(a);
	CHK(a && sz >= 100, "a cell for 100 bytes: %u", sz);
	CHK(open_whole(a, sz), "A: handed out, all %u bytes are open", sz);
	CHK(!closed(a + sz) && !closed(a + sz + 8) &&
		closed(a + sz + PCACHE_CELL_OPEN) && closed(a + 2 * sz - 1),
		"B: the next cell, not handed out: its head open, the rest closed");

	/* C */
	pcache_cell_free(a);
	CHK(!closed(a) && !closed(a + 8) && closed(a + PCACHE_CELL_OPEN) &&
		closed(a + sz - 1),
		"C: freed - the class byte and the link open, bytes %d..%u closed",
		PCACHE_CELL_OPEN, sz - 1);

	/* D: the private stack hands the same cell back */
	b = pcache_cell_alloc(100);
	CHK(b == a && open_whole(b, sz), "D: handed out again (%s), open whole",
		b == a ? "the same cell" : "another cell");

	/* E */
	pcache_cell_free(b);
	pcache_arena_flush_private();
	CHK(closed(b + PCACHE_CELL_OPEN) && !closed(b),
		"E: sent home, still closed past its head");

	/* F: a whole chunk of one class, freed and retired */
	for (i = 0; i < NBIG; i++)
		big[i] = pcache_cell_alloc(BIG - 64);
	for (i = 0; i < NBIG; i++)
		if (big[i])
			pcache_cell_free(big[i]);
	pcache_arena_flush_private();
	for (t = 0; t < 4; t++)
		pcache_arena_reclaim_tick();
	CHK(big[0] && closed(big[0]) && closed(big[0] + 8),
		"F: the chunk retired - its first cell is closed, head and all");
	n = pcache_cell_alloc(40);
	sz = n ? pcache_cell_bound(n) : 0;
	CHK(n && open_whole(n, sz),
		"F: the next carve's cell is open whole (%u bytes%s)", sz,
		n && (unsigned long)n >> 18 == (unsigned long)big[0] >> 18 ?
		", in the retired slot" : "");

	printf("cellpoisontest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
#endif
}
