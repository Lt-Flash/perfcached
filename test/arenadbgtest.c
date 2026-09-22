/* arenadbgtest.c - RV-3: the allocator's invariants as things that fail.
 *
 * The lock-free free list's no-ABA argument rests on "a cell is never
 * pushed twice while it is on the list".  Nothing checked it, and the
 * thing that breaks it - a double free - corrupts quietly and shows up
 * later, somewhere else, as two users of one cell.
 *
 * Built with -DPCACHE_ARENA_DEBUG (a release build compiles none of it:
 * the arena's object is the same size with and without the source), so
 * every move a cell makes is checked against a side table of where it
 * is.  Asserted here:
 *  - a storm - threads allocating and freeing mixed sizes, handing cells
 *    to EACH OTHER so frees cross threads, private stacks donating home
 *    and refilling from it - ends with the census holding: every cell in
 *    exactly one place, the lists what the table says, the live figure
 *    adding up;
 *  - the census holds with cells still out, and counts them;
 *  - a double free ABORTS, in the thread that did it;
 *  - a second push home - the ABA precondition itself - ABORTS;
 *  - and with the abort switched off the same double free is CAUGHT BY
 *    THE CENSUS, so the census is not a function that returns 0.
 * The three failing legs run in forked children.
 */
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../src/compat/compat.h"
#include "../src/compat/mem/mem.h"
#include "../src/compat/str.h"
#include "../src/core/pcache_htable.h"
#include "../src/core/pcache_arena.h"

extern int pcache_arena_hugepage_mb;
void pcache_mem_probe(void);

#define THREADS 6
#define OPS     40000
#define RING    384
#define SHARED  64

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static const unsigned int sizes[] = { 40, 96, 200, 600, 1500, 3000 };
static void *shared[SHARED];               /* cells change hands here */

static void *storm(void *arg)
{
	unsigned int seed = (unsigned int)(unsigned long)arg * 2654435761u + 1;
	void *ring[RING] = { 0 };
	int i;

	for (i = 0; i < OPS; i++) {
		unsigned int r = rand_r(&seed);
		unsigned int slot = r % RING;
		void *cell = pcache_cell_alloc(sizes[(r >> 8) % 6]);

		if (!cell)
			continue;
		if ((r >> 16) % 5 == 0) {
			/* hand it to whoever comes next, free what was there:
			 * the cell is freed by a thread that did not allocate it */
			void *got = __atomic_exchange_n(&shared[(r >> 20) % SHARED],
				cell, __ATOMIC_ACQ_REL);

			if (got)
				pcache_cell_free(got);
			continue;
		}
		if (ring[slot])
			pcache_cell_free(ring[slot]);
		ring[slot] = cell;
	}
	for (i = 0; i < RING; i++)
		if (ring[i])
			pcache_cell_free(ring[i]);
	return NULL;
}

/* run @fn in a child; how it ended */
static int in_child(int (*fn)(void))
{
	pid_t pid;
	int st = 0;

	fflush(stdout);
	pid = fork();
	if (pid == 0) {
		int fd = open("/dev/null", 1);

		if (fd >= 0) {
			dup2(fd, 2);                   /* the CRIT line is expected */
			close(fd);
		}
		_exit(fn());
	}
	waitpid(pid, &st, 0);
	return st;
}

static int double_free(void)
{
	void *c = pcache_cell_alloc(100);

	pcache_cell_free(c);
	pcache_cell_free(c);                   /* must not return */
	return 0;
}

static int double_home(void)
{
	void *c = pcache_cell_alloc(100);

	pcache_cell_free_global(c);            /* home, lock-free */
	pcache_cell_free_global(c);            /* home AGAIN: must not return */
	return 0;
}

static int census_catches(void)
{
	char why[256];
	void *c = pcache_cell_alloc(100);

	pcache_arena_dbg_abort = 0;            /* let the corruption happen */
	pcache_cell_free(c);
	pcache_cell_free(c);                   /* the stack now loops on itself */
	return pcache_arena_census(why, sizeof why) == -1 ? 42 : 0;
}

int main(void)
{
	pthread_t th[THREADS];
	char why[256];
	void *held[500];
	int i, st;

	pcache_mem_probe();
	pcache_backing_policy = "own";
	pcache_arena_hugepage_mb = 16;
	if (pcache_arena_init() != 0) {
		printf("arena init failed\n");
		return 2;
	}

	/* ---- the storm ---- */
	for (i = 0; i < THREADS; i++)
		pthread_create(&th[i], NULL, storm, (void *)(unsigned long)(i + 1));
	for (i = 0; i < THREADS; i++)
		pthread_join(th[i], NULL);
	for (i = 0; i < SHARED; i++)
		if (shared[i])
			pcache_cell_free(shared[i]);
	st = pcache_arena_census(why, sizeof why);
	CHK(st == 0 && pcache_arena_dbg_violations() == 0,
		"%d threads x %d allocs/frees, cells changing hands: the census "
		"holds - every cell in exactly one place (%s)", THREADS, OPS,
		st ? why : "0 violations");

	/* ---- with cells still out ---- */
	for (i = 0; i < 500; i++)
		held[i] = pcache_cell_alloc(sizes[i % 6]);
	st = pcache_arena_census(why, sizeof why);
	CHK(st == 0, "and with 500 cells out the live figure still adds up (%s)",
		st ? why : "holds");
	for (i = 0; i < 500; i++)
		pcache_cell_free(held[i]);
	st = pcache_arena_census(why, sizeof why);
	CHK(st == 0, "and after they come back (%s)", st ? why : "holds");

	/* ---- the things that must fail ---- */
	st = in_child(double_free);
	CHK(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT,
		"a double free aborts where it happens (child %s %d)",
		WIFSIGNALED(st) ? "killed by signal" : "exited",
		WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st));
	st = in_child(double_home);
	CHK(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT,
		"a second push home - the no-ABA precondition - aborts (child "
		"%s %d)", WIFSIGNALED(st) ? "killed by signal" : "exited",
		WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st));
	st = in_child(census_catches);
	CHK(WIFEXITED(st) && WEXITSTATUS(st) == 42,
		"with the abort off, the census catches the same double free "
		"(child %s %d; 42 = caught)",
		WIFSIGNALED(st) ? "killed by signal" : "exited",
		WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st));

	printf("arenadbgtest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
