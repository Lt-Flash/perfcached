/* arenacaptest.c - the capacity /stats reports: a slot the give-back punched out holds no chunk
 * and is room again, so it must not read as used; an odd-MB arena's top group is tracked; a
 * record above 64 KB has a cell size; the reclaim tick leaves a chunk a free is announcing; a thread's
 * private stack of large cells is bounded in bytes, and a carve refused at the ceiling asks every
 * thread to send its hoard home; a thread that exits sends its cells home. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/wait.h>
#include "../src/compat/compat.h"
#include "../src/compat/mem/mem.h"
#include "../src/compat/str.h"
#include "../src/core/pcache_htable.h"
#include "../src/core/pcache_arena.h"

extern int pcache_arena_hugepage_mb;
extern unsigned long pcache_arena_max_bytes;
extern int pcache_reclaim_quiet_s, pcache_reclaim_keep, pcache_reclaim_giveback,
           pcache_reclaim_cooloff_s;
void pcache_mem_probe(void);

#define NCELL  48                    /* one top-class cell a slot: six 2 MB groups */
#define CSIZE  262000

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static unsigned long used_now(void)
{
	unsigned long total = 0, used = 0, freeb = 0;
	int active = 0;

	pcache_arena_hugepage_capacity(&active, &total, &used, &freeb);
	return active ? used : 0;
}

/* B, in a child (one arena a process): 131 MB is 65 groups and a half.  The commit map,
 * counted rounding down, leaves the half group unmarked though the initial commit covers
 * it, so the region carve at the top commits it again (at 129 MB its bit is past the map) */
static int odd_mb(void)
{
	struct pcache_arena_pressure pr;
	pcache_htable_t *ht;

	compat_thread_register(0);
	pcache_mem_probe();
	pcache_backing_policy = "own";
	pcache_arena_hugepage_mb = 131;
	if (pcache_arena_init() != 0) {
		printf("arena init failed\n");
		return 2;
	}
	pcache_ht_default_stat_slots(32);
	ht = pcache_htable_new(4);
	pcache_arena_pressure(&pr);
	CHK(ht != NULL, "B1 a table is carved at the top of a 131 MB arena");
	CHK(pr.committed_bytes <= pr.reserved_bytes,
		"B2 and the committed figure stays inside the reservation (%lu of %lu KB)",
		pr.committed_bytes >> 10, pr.reserved_bytes >> 10);
	return fails ? 1 : 0;
}

/* D: a free paused after its count and before its announce, while the tick runs */
#define D_CELLS ((1U << 20) / CSIZE)         /* one refill of the top class */
static volatile int d_armed, d_inside, d_go;

static void d_hook(void *chunk)
{
	(void)chunk;
	if (!d_armed)
		return;
	d_armed = 0;
	__atomic_store_n(&d_inside, 1, __ATOMIC_RELEASE);
	while (!__atomic_load_n(&d_go, __ATOMIC_ACQUIRE))
		usleep(1000);
}

static void *d_free(void *cell)
{
	pcache_cell_free_global(cell);
	return NULL;
}

static int home_race(void)
{
	void *cell[32], *got, *small;
	unsigned int i, n0 = 0, n1 = 0;
	unsigned long b;
	pthread_t t;

	compat_thread_register(0);
	pcache_mem_probe();
	pcache_backing_policy = "own";
	pcache_arena_hugepage_mb = 64;
	if (pcache_arena_init() != 0) {
		printf("arena init failed\n");
		return 2;
	}
	pcache_reclaim_giveback = 0;
	pcache_reclaim_keep = 0;
	/* one cell a chunk; a refill (1 MB of them) ends with cell[0]'s chunk current and empty */
	for (i = 0; i < D_CELLS; i++)
		if (!(cell[i] = pcache_cell_alloc(CSIZE)))
			return 2;
	for (i = 0; i < D_CELLS; i++)
		pcache_cell_free_global(cell[i]);
	for (i = 0; i < D_CELLS; i++)
		if (!(cell[i] = pcache_cell_alloc(CSIZE)))
			return 2;
	pcache_arena_home_hook = d_hook;
	d_armed = 1;
	if (pthread_create(&t, NULL, d_free, cell[0]) != 0)
		return 2;
	while (!__atomic_load_n(&d_inside, __ATOMIC_ACQUIRE))
		usleep(1000);
	pcache_arena_reclaim_tick();
	__atomic_store_n(&d_go, 1, __ATOMIC_RELEASE);
	pthread_join(t, NULL);
	/* a retired slot on the class's stack: a re-cut for small cells, one sent home */
	small = pcache_cell_alloc(100);
	if (!small)
		return 2;
	pcache_cell_free_global(small);
	got = pcache_cell_alloc(CSIZE);
	CHK(got == cell[0] && pcache_cell_bound(got) == PCACHE_CELL_MAX,
		"D1 the chunk a free was announcing is not retired under it: the next cell is the freed one (%p, %p freed; bound %u)",
		got, cell[0], got ? pcache_cell_bound(got) : 0);
	if (got == cell[0]) {
		pcache_cell_free_global(got);
		pcache_arena_stats(&n0, &b);
		pcache_arena_reclaim_tick();
		pcache_arena_stats(&n1, &b);
		CHK(n1 + 1 == n0, "D2 with the free done, the next tick retires it (%u -> %u chunks)",
			n0, n1);
	}
	return fails ? 1 : 0;
}

/* E: one thread frees a whole arena of top-class cells to its private stack and stays alive */
static volatile int e_freed, e_done;
static unsigned int e_n, e_bcasts;

static void e_bcast(void (*fn)(int, void *), void *param)
{
	(void)fn; (void)param;
	e_bcasts++;
}

static void *e_hoarder(void *arg)
{
	static void *cell[300];
	unsigned int i;

	(void)arg;
	while (e_n < 300 && (cell[e_n] = pcache_cell_alloc(CSIZE)) != NULL)
		e_n++;
	for (i = 0; i < e_n; i++)
		pcache_cell_free(cell[i]);
	__atomic_store_n(&e_freed, 1, __ATOMIC_RELEASE);
	while (!__atomic_load_n(&e_done, __ATOMIC_ACQUIRE))
		usleep(1000);
	return NULL;
}

static int hoard(void)
{
	unsigned int m = 0, b0;
	pthread_t t;

	compat_thread_register(0);
	pcache_mem_probe();
	pcache_backing_policy = "own";
	pcache_arena_hugepage_mb = 64;
	if (pcache_arena_init() != 0) {
		printf("arena init failed\n");
		return 2;
	}
	pcache_reclaim_giveback = 0;
	pcache_arena_max_bytes = 64UL << 20;   /* the daemon's ceiling with no cap; 0 grows into shm */
	compat_set_broadcast(e_bcast);
	if (pthread_create(&t, NULL, e_hoarder, NULL) != 0)
		return 2;
	while (!__atomic_load_n(&e_freed, __ATOMIC_ACQUIRE))
		usleep(1000);
	pcache_arena_reclaim_tick();       /* the hoarder's own refusal was last tick's */
	e_bcasts = 0;
	while (m < 300 && pcache_cell_alloc(CSIZE))
		m++;
	b0 = e_bcasts;
	(void)pcache_cell_alloc(CSIZE);
	__atomic_store_n(&e_done, 1, __ATOMIC_RELEASE);
	pthread_join(t, NULL);
	CHK(e_n > 100 && m + 4 >= e_n,
		"E1 a thread that freed %u cells of %u bytes keeps at most 1 MB of them: another thread took %u",
		e_n, CSIZE, m);
	CHK(b0 == 1 && e_bcasts == 1,
		"E2 the carve refused at the ceiling asked the threads to send their hoards home, once a tick (%u, then %u)",
		b0, e_bcasts);
	return fails ? 1 : 0;
}

/* F: a thread allocates and frees a small and a large cell, privately, and exits */
static void *f_worker(void *arg)
{
	void *a = pcache_cell_alloc(100), *b = pcache_cell_alloc(CSIZE);

	(void)arg;
	if (a)
		pcache_cell_free(a);
	if (b)
		pcache_cell_free(b);
	return NULL;
}

static int thread_exit(void)
{
	unsigned int n0 = 0, n1 = 0;
	unsigned long b;
	pthread_t t;

	compat_thread_register(0);
	pcache_mem_probe();
	pcache_backing_policy = "own";
	pcache_arena_hugepage_mb = 64;
	if (pcache_arena_init() != 0) {
		printf("arena init failed\n");
		return 2;
	}
	pcache_reclaim_giveback = 0;
	pcache_reclaim_keep = 0;
	if (pthread_create(&t, NULL, f_worker, NULL) != 0)
		return 2;
	pthread_join(t, NULL);
	pcache_arena_stats(&n0, &b);
	pcache_arena_reclaim_tick();
	pcache_arena_stats(&n1, &b);
	CHK(n0 >= 2 && n1 + 2 == n0,
		"F1 the chunks of a thread that exited are home and retire (%u -> %u chunks)", n0, n1);
	return fails ? 1 : 0;
}

int main(void)
{
	struct pcache_arena_pressure pr;
	unsigned long used0, used1, used2, cold0;
	void *cell[NCELL];
	unsigned int i, t;
	pid_t pid;
	int st = 0;

	fflush(stdout);
	pid = fork();
	if (pid == 0) {
		st = odd_mb();
		fflush(stdout);
		_exit(st);
	}
	if (pid < 0 || waitpid(pid, &st, 0) != pid || !WIFEXITED(st) ||
	    WEXITSTATUS(st) != 0) {
		printf("  FAIL B the odd-MB round (status %d)\n", st);
		fails++;
	}
	fflush(stdout);
	pid = fork();
	if (pid == 0) {
		st = home_race();
		fflush(stdout);
		_exit(st);
	}
	if (pid < 0 || waitpid(pid, &st, 0) != pid || !WIFEXITED(st) ||
	    WEXITSTATUS(st) != 0) {
		printf("  FAIL D the free against the reclaim tick (status %d)\n", st);
		fails++;
	}
	fflush(stdout);
	pid = fork();
	if (pid == 0) {
		st = thread_exit();
		fflush(stdout);
		_exit(st);
	}
	if (pid < 0 || waitpid(pid, &st, 0) != pid || !WIFEXITED(st) ||
	    WEXITSTATUS(st) != 0) {
		printf("  FAIL F the cells of a thread that exits (status %d)\n", st);
		fails++;
	}
	fflush(stdout);
	pid = fork();
	if (pid == 0) {
		st = hoard();
		fflush(stdout);
		_exit(st);
	}
	if (pid < 0 || waitpid(pid, &st, 0) != pid || !WIFEXITED(st) ||
	    WEXITSTATUS(st) != 0) {
		printf("  FAIL E a hoard of large cells on one thread (status %d)\n", st);
		fails++;
	}
	compat_thread_register(0);
	pcache_mem_probe();
	pcache_backing_policy = "own";
	pcache_arena_hugepage_mb = 64;
	if (pcache_arena_init() != 0) {
		printf("arena init failed\n");
		return 2;
	}
	pcache_reclaim_giveback = 1;
	pcache_reclaim_quiet_s = 1;
	pcache_reclaim_cooloff_s = 0;
	pcache_reclaim_keep = 0;

	/* A: punched slots are not chunks */
	for (t = 0; t < 12; t++)             /* the never-carved tail goes first */
		pcache_arena_reclaim_tick();
	pcache_arena_pressure(&pr);
	cold0 = pr.cold_bytes;
	used0 = used_now();
	for (i = 0; i < NCELL; i++)
		if (!(cell[i] = pcache_cell_alloc(CSIZE))) {
			printf("alloc %u failed\n", i);
			return 2;
		}
	used1 = used_now();
	CHK(used1 >= used0 + (unsigned long)NCELL * CSIZE,
		"A1 %u cells of %u bytes read as used (%lu -> %lu KB)", NCELL, CSIZE,
		used0 >> 10, used1 >> 10);
	for (i = 0; i < NCELL; i++)
		pcache_cell_free(cell[i]);
	pcache_arena_flush_private();
	for (t = 0; t < 60; t++)
		pcache_arena_reclaim_tick();
	pcache_arena_pressure(&pr);
	CHK(pr.cold_bytes >= cold0 + (4UL << 20),
		"A2 the drained chunks retired and their groups were punched (cold %lu -> %lu KB)",
		cold0 >> 10, pr.cold_bytes >> 10);
	used2 = used_now();
	CHK(used2 <= used0 + (4UL << 20),
		"A3 and they no longer read as used (%lu KB, %lu before the cells; a punched header reads as a chunk of class 0)",
		used2 >> 10, used0 >> 10);

	/* C: the cell a record above 64 KB occupies, which the budget sums */
	CHK(pcache_arena_cell_size(CSIZE) == PCACHE_CELL_MAX &&
		pcache_arena_cell_size(65537) >= 65537,
		"C1 a %u-byte record takes a %u-byte cell, a 65,537-byte one %u (0 = not counted)",
		CSIZE, pcache_arena_cell_size(CSIZE), pcache_arena_cell_size(65537));
	CHK(pcache_arena_cell_size(PCACHE_CELL_MAX + 1) == 0, "C2 and past the largest class, 0");

	printf("arenacaptest: %s\n", fails ? "FAILED" : "all ok");
	return fails ? 1 : 0;
}
