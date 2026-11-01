/* S447: a carve while a tail punch is in flight must not land in the tail
 * being punched.
 *
 * The give-back plans its tail runs under the arena lock, drops the lock
 * for the madvise, and takes the groups off hcommit only after (phase 3).
 * A writer carving in between found the group still committed, carved its
 * chunk there and wrote records into it - and the madvise zeroed them: a
 * record served and then gone, or one a recovery had just replayed gone
 * (walordertest, v0.7.2-rc5 and rc6; rarely since 0.4.0-rc49).  The hook
 * (pcache_arena_tail_hook) runs exactly there: it carves cells until the
 * frontier passes into the tail in flight, writes a pattern into each, and
 * once the tick is done every cell must still hold it.
 * FAIL-FIRST: before the in-flight window, the cells carved inside the
 * punched tail read back as zeroes. */
#include <stdio.h>
#include <string.h>
#include "../src/compat/compat.h"
#include "../src/compat/mem/mem.h"
#include "../src/core/pcache_arena.h"

extern int pcache_arena_hugepage_mb;
void pcache_mem_probe(void);

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

#define NCELL  64
#define CSIZE  (96 * 1024)               /* two to a chunk */
static unsigned char *cells[NCELL];
static int ncells, inside, hooked;
static char *fl_lo, *fl_hi;

static void hook(char *lo, char *hi)
{
	int i;

	if (hooked++)
		return;                        /* the first punch is the test */
	fl_lo = lo;
	fl_hi = hi;
	for (i = 0; i < NCELL; i++) {
		unsigned char *c = pcache_cell_alloc(CSIZE);

		if (!c)
			break;
		memset(c, 0xA5, CSIZE);
		cells[ncells++] = c;
		if ((char *)c >= lo && (char *)c < hi)
			inside++;
		if ((char *)c > lo + 2 * 2097152)
			break;                     /* well past the tail's start */
	}
}

int main(void)
{
	int t, i, bad = 0;

	pcache_mem_probe();
	pcache_arena_hugepage_mb = 64;
	if (pcache_arena_init() != 0) {
		printf("arena init failed\n");
		return 2;
	}
	pcache_reclaim_giveback = 1;
	pcache_reclaim_quiet_s = 1;
	pcache_reclaim_cooloff_s = 0;
	pcache_arena_tail_hook = hook;
	/* the reservation's never-carved tail is punched once (S119) - the
	 * first tick that plans it is the one the hook runs in */
	for (t = 0; t < 12 && !hooked; t++)
		pcache_arena_reclaim_tick();
	pcache_arena_tail_hook = NULL;
	if (!hooked) {
		printf("tailflighttest: no tail punch on this backing - SKIPPED, and a skip is not a pass\n");
		return 0;
	}
	for (i = 0; i < ncells; i++) {
		int k;

		for (k = 0; k < CSIZE && cells[i][k] == 0xA5; k++)
			;
		if (k < CSIZE)
			bad++;
	}
	CHK(ncells > 0, "the hook carved %d cell(s) while the punch was in flight (%ld KB of tail)",
		ncells, (long)(fl_hi - fl_lo) / 1024);
	CHK(inside == 0, "none of them inside the tail in flight (%d were)", inside);
	CHK(bad == 0, "every cell still holds what was written into it (%d zeroed or torn)", bad);
	printf("tailflighttest: %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
	return fails ? 1 : 0;
}
