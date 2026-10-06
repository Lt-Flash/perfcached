/* evactest.c - S190: the survivors of a mass expiry are moved out of the
 * chunks they pin, and the chunks retire.
 *
 * The soak: 62,000 records with one TTL, all gone at once, 2,000
 * long-lived records left behind spread across the ~90 chunks that held
 * them, ~22 per chunk.  A chunk retires only when every cell in it is
 * free, so not one could - 57.5 MB held for 2.2 MB live, indefinitely.
 *
 * The shape here: one class, N records stored, every 40th one kept, the
 * rest removed - the survivors land in every chunk.  Then the reclaim
 * tick (the arena's verdict on which chunks are SPARSE) and the
 * evacuator (the table's walk) run as the maintenance thread would.
 * Asserted:
 *  - before: the chunk count is what the whole load needed, and stays
 *    there across reclaim ticks alone - the pin is real;
 *  - the reclaim tick marks the chunks sparse;
 *  - the evacuator moves the survivors that are in a sparse chunk, and
 *    settles - it does not shuffle records between sparse chunks for
 *    ever, which would be worse than the hold it fixes;
 *  - every survivor still reads back with its value afterwards;
 *  - the next reclaim ticks retire the emptied chunks: the class holds
 *    about what the survivors need, not what the load did;
 *  - a table with nothing sparse moves nothing (the ordinary case costs
 *    nothing but the walk);
 *  - and the one that matters at runtime: with READERS hammering the
 *    survivors throughout a second round, not one read misses a record
 *    that exists or returns a torn value.  The move swaps the slot under
 *    the bucket lock with the version bumped around it, so a reader
 *    copying from the old cell sees the version move and retries - the
 *    contract a replacing store already relies on;
 *  - the census (RV-3 ledger, debug build only) holds throughout.
 * Fail-first: with the evacuator compiled to a no-op the chunk count
 * never comes down.
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../src/compat/compat.h"
#include "../src/compat/mem/mem.h"
#include "../src/compat/str.h"
#include "../src/core/pcache_htable.h"
#include "../src/core/pcache_arena.h"

extern int pcache_arena_hugepage_mb;
extern int pcache_reclaim_quiet_s, pcache_reclaim_keep, pcache_reclaim_giveback,
           pcache_reclaim_cooloff_s;
void pcache_mem_probe(void);

#define N      60000
#define KEEP   40                   /* every KEEP-th record survives */
#define VLEN   200                  /* + ~30 header = the 256-byte class */

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static unsigned int chunks(void)
{
	unsigned int n = 0;
	unsigned long b = 0;

	pcache_arena_stats(&n, &b);
	return n;
}

/* readers, for the concurrent round */
static pcache_htable_t *rht;
static volatile int rstop;
static unsigned long r_hits, r_missing, r_torn;

static void *reader(void *arg)
{
	unsigned int seed = (unsigned int)(unsigned long)arg * 2654435761u + 1;
	char kb[32], vb[VLEN];
	str key, out;

	memset(vb, 'v', sizeof vb);
	while (!rstop) {
		unsigned int i = (rand_r(&seed) % (N / KEEP)) * KEEP;

		key.len = snprintf(kb, sizeof kb, "k%06u", i);
		key.s = kb;
		if (pcache_ht_fetch(rht, &key, &out) != 0) {
			__atomic_fetch_add(&r_missing, 1, __ATOMIC_RELAXED);
			continue;
		}
		if (out.len != VLEN || memcmp(out.s, vb, VLEN) != 0)
			__atomic_fetch_add(&r_torn, 1, __ATOMIC_RELAXED);
		else
			__atomic_fetch_add(&r_hits, 1, __ATOMIC_RELAXED);
		pkg_free(out.s);
	}
	return NULL;
}

int main(void)
{
	pcache_htable_t *ht;
	char kb[32], vb[VLEN];
	str key, val, out;
	unsigned int i, before, sparse_seen = 0, moved = 0, surv = 0, ok_read = 0,
	             t, after, ticks_to_retire = 0;
	struct pcache_arena_pressure pr;

	pcache_mem_probe();
	pcache_backing_policy = "own";
	pcache_arena_hugepage_mb = 64;
	if (pcache_arena_init() != 0) {
		printf("arena init failed\n");
		return 2;
	}
	pcache_ht_default_stat_slots(32);
	pcache_reclaim_giveback = 1;
	pcache_reclaim_quiet_s = 1;
	pcache_reclaim_cooloff_s = 0;
	pcache_reclaim_keep = 1;
	ht = pcache_htable_new(16);
	memset(vb, 'v', sizeof vb);
	val.s = vb;
	val.len = VLEN;

	/* ---- the load, and the mass expiry ---- */
	for (i = 0; i < N; i++) {
		key.len = snprintf(kb, sizeof kb, "k%06u", i);
		key.s = kb;
		if (pcache_ht_store(ht, &key, &val, 0) != 0) {
			printf("store %u failed\n", i);
			return 2;
		}
	}
	before = chunks();
	for (i = 0; i < N; i++) {
		if (i % KEEP == 0) {
			surv++;
			continue;
		}
		key.len = snprintf(kb, sizeof kb, "k%06u", i);
		key.s = kb;
		pcache_ht_remove(ht, &key);
	}
	pcache_arena_flush_private();          /* the frees go home, as a quiet
	                                        * node's eventually do */
	for (t = 0; t < 4; t++)
		pcache_arena_reclaim_tick();
	CHK(chunks() >= before - 2 && before >= 40,
		"%u records in %u chunks; %u survivors (1 in %d) removed the rest and "
		"reclaim alone retired nothing: %u chunks still held", N, before, surv,
		KEEP, chunks());

	/* ---- the verdict ---- */
	pcache_arena_pressure(&pr);
	sparse_seen = (unsigned int)pr.sparse_chunks;
	CHK(sparse_seen >= before / 2,
		"the reclaim tick marked %u chunks sparse (at most an eighth live, "
		"quiet for the window)", sparse_seen);

	/* ---- the evacuation, as the maintenance thread runs it ---- */
	{
		unsigned int quiet = 0, last = 0;

		for (t = 0; t < 200 && quiet < 5; t++) {
			unsigned int m = pcache_ht_evacuate(ht, 4096, 512);

			moved += m;
			pcache_arena_reclaim_tick();
			if (!ticks_to_retire && chunks() < before / 4)
				ticks_to_retire = t + 1;
			/* CONVERGENCE, not a fixed count: a background task that
			 * shuffled records between sparse chunks for ever would
			 * be worse than the hold it fixes */
			quiet = m ? 0 : quiet + 1;
			last = m;
		}
		CHK(quiet >= 5 && t < 200,
			"the evacuator settled after %u round(s) and then moved nothing "
			"for five more (last round %u)", t, last);
	}
	after = chunks();
	/* at most one move per survivor: a record already in a chunk that is
	 * not sparse is left where it is */
	CHK(moved > surv / 2 && moved <= surv,
		"the evacuator moved %u of the %u survivors - the rest were already "
		"in a chunk worth keeping", moved, surv);
	for (i = 0; i < N; i += KEEP) {
		key.len = snprintf(kb, sizeof kb, "k%06u", i);
		key.s = kb;
		if (pcache_ht_fetch(ht, &key, &out) == 0) {
			if (out.len == VLEN && memcmp(out.s, vb, VLEN) == 0)
				ok_read++;
			pkg_free(out.s);
		}
	}
	CHK(ok_read == surv, "every survivor still reads back with its value (%u of %u)",
		ok_read, surv);
	CHK(after <= surv / 900 + 4 && after < before / 4,
		"the emptied chunks retired: %u chunks held for %u survivors, was %u "
		"(after %u tick(s))", after, surv, before, ticks_to_retire);
	pcache_arena_pressure(&pr);
	CHK(pr.evacuated >= moved, "the arena handed out %lu dense cells for them",
		pr.evacuated);

	/* ---- the ordinary case: nothing sparse, nothing moved ---- */
	{
		unsigned int m = 0, c0 = chunks();

		for (t = 0; t < 6; t++) {
			m += pcache_ht_evacuate(ht, 65536, 512);
			pcache_arena_reclaim_tick();
		}
		CHK(m == 0 && chunks() == c0,
			"at rest a full walk moves nothing and holds the chunk count "
			"(%u moved, %u chunks)", m, chunks());
	}
	/* ---- the concurrent round: readers throughout the move ---- */
	{
		pthread_t th[4];
		unsigned int m2 = 0, b2;

		/* a keyspace of its OWN: the first round's survivors are
		 * consolidated already, so re-loading theirs would leave
		 * nothing sparse to move */
		for (i = 0; i < N; i++) {
			key.len = snprintf(kb, sizeof kb, "j%06u", i);
			key.s = kb;
			(void)pcache_ht_store(ht, &key, &val, 0);
		}
		for (i = 0; i < N; i++) {
			if (i % KEEP == 0)
				continue;
			key.len = snprintf(kb, sizeof kb, "j%06u", i);
			key.s = kb;
			pcache_ht_remove(ht, &key);
		}
		pcache_arena_flush_private();
		for (t = 0; t < 3; t++)
			pcache_arena_reclaim_tick();
		b2 = chunks();
		rht = ht;
		rstop = 0;
		for (i = 0; i < 4; i++)
			pthread_create(&th[i], NULL, reader, (void *)(unsigned long)(i + 1));
		for (t = 0; t < 120 && (t < 8 || pcache_arena_sparse_chunks()); t++) {
			m2 += pcache_ht_evacuate(ht, 4096, 512);
			pcache_arena_reclaim_tick();
			usleep(1000);
		}
		rstop = 1;
		for (i = 0; i < 4; i++)
			pthread_join(th[i], NULL);
		CHK(m2 > 0 && r_hits > 10000 && r_missing == 0 && r_torn == 0,
			"%lu reads by 4 threads while %u records moved under them: "
			"%lu missing, %lu torn (chunks %u -> %u)",
			r_hits, m2, r_missing, r_torn, b2, chunks());
	}
#ifdef PCACHE_ARENA_DEBUG
	{
		char why[256];

		CHK(pcache_arena_census(why, sizeof why) == 0,
			"the ledger holds after the moves (%s)", why[0] ? why : "0 violations");
	}
#endif
	printf("evactest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
