/* capracetest.c - RV-1: the arena's ceiling is a reservation, not a check.
 *
 * A region carve past the huge-page reservation tested the ceiling with
 * a relaxed read OUTSIDE the arena lock, then shm_malloc'd, then took
 * the lock and added its bytes.  Two carves near the cap - a `create`
 * on a worker and the splitter on the maintenance thread - could both
 * pass the test and both land, putting the node past the arena_cap_mb
 * its operator reads as a bound.  The record-chunk carve always did its
 * accounting under the lock; only regions had the window.
 *
 * The shape that makes it show every time: leave room under the ceiling
 * for EXACTLY ONE more region, release N threads at a barrier to carve
 * one region each, and look at what landed.  One may succeed; the held
 * bytes may never pass the ceiling.  Repeated for ROUNDS ceilings, so a
 * carve that only tests is caught within a handful of rounds and a
 * carve that reserves passes all of them by construction.  The positive
 * control is the success count: exactly one per round, so "never over"
 * cannot mean "everything was refused".  Milliseconds. */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "../src/compat/compat.h"
#include "../src/compat/mem/mem.h"
#include "../src/compat/str.h"
#include "../src/core/pcache_htable.h"
#include "../src/core/pcache_arena.h"

extern int pcache_arena_hugepage_mb;
void pcache_mem_probe(void);

#define THREADS 8
#define ROUNDS  300
#define RSIZE   (100UL << 10)          /* well under a slot: the fallback
                                        * charges size + its header, the
                                        * reservation charges a whole slot */

static pthread_barrier_t go, done;
static int succ;                       /* carves that landed this round */
static int fails;

#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static void *carver(void *arg)
{
	int r;

	(void)arg;
	for (r = 0; r < ROUNDS; r++) {
		pthread_barrier_wait(&go);
		if (pcache_region_alloc(RSIZE))
			__atomic_add_fetch(&succ, 1, __ATOMIC_RELAXED);
		pthread_barrier_wait(&done);
	}
	return NULL;
}

int main(void)
{
	pthread_t th[THREADS];
	unsigned long held, before, need = 0, worst_over = 0;
	int i, r, over_rounds = 0, multi_rounds = 0, none_rounds = 0, total = 0;

	pcache_mem_probe();
	pcache_backing_policy = "own";
	pcache_arena_hugepage_mb = 2;      /* a reservation small enough to leave */
	if (pcache_arena_init() != 0) {
		printf("arena init failed\n");
		return 2;
	}

	/* walk off the reservation: a carve that costs size + header rather
	 * than whole slots came from the shm fallback, where the ceiling is */
	pcache_arena_max_bytes = 0;
	for (i = 0; i < 64 && !need; i++) {
		before = pcache_arena_held_bytes();
		if (!pcache_region_alloc(RSIZE)) {
			printf("pre-fill carve %d failed\n", i);
			return 2;
		}
		held = pcache_arena_held_bytes();
		if (held - before < pcache_arena_region_cost(RSIZE))
			need = held - before;
	}
	CHK(need > RSIZE && need < pcache_arena_region_cost(RSIZE),
		"past the reservation: a %lu KB region now costs %lu bytes, "
		"not a %lu KB slot", RSIZE >> 10, need,
		pcache_arena_region_cost(RSIZE) >> 10);
	if (!need)
		return 1;

	pthread_barrier_init(&go, NULL, THREADS + 1);
	pthread_barrier_init(&done, NULL, THREADS + 1);
	for (i = 0; i < THREADS; i++)
		pthread_create(&th[i], NULL, carver, NULL);

	for (r = 0; r < ROUNDS; r++) {
		held = pcache_arena_held_bytes();
		/* room for exactly one more region, not two */
		pcache_arena_max_bytes = held + need + need / 2;
		__atomic_store_n(&succ, 0, __ATOMIC_RELAXED);
		pthread_barrier_wait(&go);
		pthread_barrier_wait(&done);
		held = pcache_arena_held_bytes();
		if (held > pcache_arena_max_bytes) {
			over_rounds++;
			if (held - pcache_arena_max_bytes > worst_over)
				worst_over = held - pcache_arena_max_bytes;
		}
		if (succ > 1)
			multi_rounds++;
		if (succ == 0)
			none_rounds++;
		total += succ;
	}
	for (i = 0; i < THREADS; i++)
		pthread_join(th[i], NULL);

	CHK(over_rounds == 0,
		"%d threads at a ceiling with room for one, %d rounds: held "
		"bytes never passed the ceiling (%d rounds over, worst by %lu KB)",
		THREADS, ROUNDS, over_rounds, worst_over >> 10);
	CHK(multi_rounds == 0,
		"never more than one carve landed in a round (%d rounds with "
		"several)", multi_rounds);
	CHK(none_rounds == 0 && total == ROUNDS,
		"positive control: exactly one carve landed in every round "
		"(%d landed over %d rounds, %d rounds with none)", total, ROUNDS,
		none_rounds);

	printf("capracetest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
