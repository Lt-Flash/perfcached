/* SPDX-License-Identifier: GPL-2.0-or-later */
/* dumpchunktest.c - perfdump's chunk bookkeeping (perfdump.c compiled in, its main renamed):
 * a manifest of more files than a fixed token array holds is read back by --inspect; a chunk's
 * path is built from its record while the table cannot move; a walker that cannot be started
 * fails its job. */
#include <pthread.h>

static int test_unlock(pthread_mutex_t *m);
static int test_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg);
#define pthread_mutex_unlock test_unlock
#define pthread_create test_create
#define main perfdump_main
#include "../cli/perfdump.c"
#undef main
#undef pthread_mutex_unlock
#undef pthread_create

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* armed: the next unlock of this table's mutex is another walker growing it - the table moves
 * and the old block is left holding a stale name */
static struct shared *move_on_unlock;
static struct chunkrec *moved_from;

static int test_unlock(pthread_mutex_t *m)
{
	struct shared *sh = move_on_unlock;
	int rc = pthread_mutex_unlock(m), i;

	if (sh && m == &sh->mx) {
		struct chunkrec *n = malloc((size_t)sh->capchunks * sizeof *n);

		move_on_unlock = NULL;
		if (n) {
			memcpy(n, sh->chunks, (size_t)sh->capchunks * sizeof *n);
			for (i = 0; i < sh->capchunks; i++)
				snprintf(sh->chunks[i].file, sizeof sh->chunks[i].file, "stale");
			moved_from = sh->chunks;
			sh->chunks = n;
		}
	}
	return rc;
}

/* set: every thread create fails as at the thread limit (EAGAIN) */
static int create_fails;

static int test_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg)
{
	return create_fails ? EAGAIN : pthread_create(t, a, fn, arg);
}

/* a chunk of no records as chunk_close leaves it (the magic, the trailer); its crc */
static uint32_t empty_chunk(const char *dir, const char *name)
{
	unsigned char t[12];
	char path[512];
	uint32_t crc;
	FILE *f;

	put32(t, 0xFFFFFFFFu);
	put64(t + 4, 0);
	crc = crc32_upd(0, PCD_MAGIC, 4);
	crc = crc32_upd(crc, t, sizeof t);
	snprintf(path, sizeof path, "%s/%s", dir, name);
	f = fopen(path, "wb");
	if (f) {
		if (fwrite(PCD_MAGIC, 1, 4, f) != 4 || fwrite(t, 1, sizeof t, f) != sizeof t)
			printf("  ..   short write on %s\n", path);
		fclose(f);
	}
	return crc;
}

/* perfdump_main(@ac, @av) with its per-file lines kept off the log */
static int quiet_main(int ac, char **av)
{
	int save, nul, rc;

	fflush(stdout);
	save = dup(1);
	nul = open("/dev/null", O_WRONLY);
	if (nul >= 0) {
		dup2(nul, 1);
		close(nul);
	}
	rc = perfdump_main(ac, av);
	fflush(stdout);
	if (save >= 0) {
		dup2(save, 1);
		close(save);
	}
	return rc;
}

int main(void)
{
	static char dir[] = "/var/tmp/dumpchunktest.XXXXXX";
	struct shared sh;
	struct opts o;
	char cmd[600];
	int i;

	if (!mkdtemp(dir)) {
		printf("  FAIL mkdtemp\n");
		return 1;
	}
	crc_init();

	printf("--- A. a manifest of 8,000 files (19 tokens each) reads back\n");
	{
		const int nf = 8000;
		char *av[] = { "perfdump", "--inspect", dir, NULL };

		memset(&sh, 0, sizeof sh);
		memset(&o, 0, sizeof o);
		o.out = dir;
		o.threads = 1;
		sh.o = &o;
		sh.ncols = 1;
		snprintf(sh.cols[0], sizeof sh.cols[0], "c");
		sh.nb[0] = 1024;
		sh.chunks = calloc((size_t)nf, sizeof *sh.chunks);
		if (!sh.chunks) {
			printf("  FAIL calloc\n");
			return 1;
		}
		for (i = 0; i < nf; i++) {
			struct chunkrec *r = &sh.chunks[i];

			snprintf(r->col, sizeof r->col, "c");
			snprintf(r->file, sizeof r->file, "c.%04d.pcd", i);
			r->complete = 1;
			r->crc = empty_chunk(dir, r->file);
		}
		sh.nchunks = nf;
		CHK(manifest_write(&sh) == 0, "A1 the manifest of %d files is written", nf);
		CHK(quiet_main(3, av) == 0, "A2 perfdump --inspect reads it and finds every file ok");
		free(sh.chunks);
	}

	printf("--- B. chunk_open names the file from its record before another walker can move the table\n");
	{
		struct job j;
		struct chunk c;
		char want[600];

		memset(&sh, 0, sizeof sh);
		memset(&o, 0, sizeof o);
		memset(&j, 0, sizeof j);
		memset(&c, 0, sizeof c);
		o.out = dir;
		o.threads = 2;
		sh.o = &o;
		pthread_mutex_init(&sh.mx, NULL);
		sh.ncols = 1;
		snprintf(sh.cols[0], sizeof sh.cols[0], "b");
		sh.nb[0] = 1024;
		j.sh = &sh;
		j.hi = 1024;
		move_on_unlock = &sh;
		snprintf(want, sizeof want, "%s/b.0000.pcd", dir);
		CHK(chunk_open(&sh, &j, &c) == 0 && !strcmp(c.path, want),
			"B1 the chunk's path is its own name (%s), not the moved table's old block", c.path);
		if (c.f)
			fclose(c.f);
		free(moved_from);
		free(sh.chunks);
		pthread_mutex_destroy(&sh.mx);
	}

	printf("--- C. a walker that cannot be started fails its job\n");
	{
		struct job jobs[2];
		pthread_t th[2];
		int n;

		memset(&sh, 0, sizeof sh);
		memset(&o, 0, sizeof o);
		memset(jobs, 0, sizeof jobs);
		memset(th, 0, sizeof th);
		o.out = dir;
		o.threads = 2;
		sh.o = &o;
		pthread_mutex_init(&sh.mx, NULL);
		sh.ncols = 1;
		snprintf(sh.cols[0], sizeof sh.cols[0], "w");
		sh.nb[0] = 4;
		create_fails = 1;
		n = start_collection(&sh, 0, jobs, th);
		create_fails = 0;
		CHK(n == 2 && jobs[0].failed && jobs[1].failed,
			"C1 both walkers refused by pthread_create: their jobs are failed (%d, %d of %d)",
			jobs[0].failed, jobs[1].failed, n);
		pthread_mutex_destroy(&sh.mx);
	}

	snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
	if (system(cmd) != 0)
		printf("  ..   could not remove %s\n", dir);
	printf("dumpchunktest: %s\n", fails ? "FAILED" : "all passed");
	return fails ? 1 : 0;
}
