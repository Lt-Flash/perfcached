/* SPDX-License-Identifier: GPL-2.0-or-later */
/* loadrefusetest.c - perfload's verdict on a restore reply (perfload.c compiled in, its main
 * renamed): a chunk whose records the daemon refused, or whose reply cannot be read, is not done;
 * a manifest of many files is read whole; perfload.done skips a chunk only for the target and
 * the mode that loaded it; a thread that cannot be started fails its job and is not joined. */
#include <pthread.h>

static int test_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg);
static int test_join(pthread_t t, void **ret);
#define pthread_create test_create
#define pthread_join test_join
#define main perfload_main
#include "../cli/perfload.c"
#undef main
#undef pthread_create
#undef pthread_join

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* fail_create = N: the Nth create from here on fails as at the thread limit; a join of an id
 * no create returned is counted, not done */
static int fail_create, creates, bogus_joins, nlive;
static pthread_t live[64];

static int test_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg)
{
	int rc;

	if (fail_create && ++creates == fail_create)
		return EAGAIN;
	rc = pthread_create(t, a, fn, arg);
	if (rc == 0 && nlive < 64)
		live[nlive++] = *t;
	return rc;
}

static int test_join(pthread_t t, void **ret)
{
	int i;

	for (i = 0; i < nlive; i++)
		if (pthread_equal(live[i], t)) {
			live[i] = live[--nlive];
			return pthread_join(t, ret);
		}
	bogus_joins++;
	return ESRCH;
}

static void *idle(void *a)
{
	return a;
}

/* a restore reply map of @n "name", count pairs */
static size_t reply(unsigned char *buf, size_t cap, int n, const char *const *k, const long long *v)
{
	struct pc_tw w;
	int i;

	pc_tw_init(&w, buf, cap);
	pc_tw_map(&w);
	for (i = 0; i < n; i++) {
		pc_tw_key(&w, k[i]);
		pc_tw_i64(&w, v[i]);
	}
	pc_tw_end(&w);
	return pc_tw_done(&w) == 0 ? w.n : 0;
}

/* one chunk of five records answered by @res: 1 = perfload marked it done */
static int chunk_done(struct job *j, const unsigned char *res, size_t rn)
{
	static unsigned char tree[1];
	struct sent s = { tree, NULL, NULL, 0, 5 };
	struct file f;

	memset(&f, 0, sizeof f);
	snprintf(f.name, sizeof f.name, "dc-0001.pcd");
	f.records = 5;
	drain_reply(j, &s, res, rn);
	chunk_end(j, &f);
	return f.done;
}

int main(void)
{
	static char dir[] = "/var/tmp/loadrefusetest.XXXXXX";
	static const char *const ok5[] = { "stored" }, *const ref[] = { "stored", "refused" };
	static const char *const bad[] = { "stored", "bad" };
	static const long long v5[] = { 5 }, v32[] = { 3, 2 }, v41[] = { 4, 1 };
	unsigned char buf[256];
	struct opts o;
	struct shared sh;
	struct job j;
	char cmd[128];
	size_t n;

	if (!mkdtemp(dir)) {
		printf("  FAIL mkdtemp\n");
		return 1;
	}
	memset(&o, 0, sizeof o);
	memset(&sh, 0, sizeof sh);
	memset(&j, 0, sizeof j);
	o.dir = dir;
	o.host = "127.0.0.1";
	o.port = 6479;
	o.policy = "newer";
	sh.o = &o;
	pthread_mutex_init(&sh.mx, NULL);
	j.sh = &sh;

	n = reply(buf, sizeof buf, 1, ok5, v5);
	CHK(n && chunk_done(&j, buf, n), "every record stored: the chunk is done");
	n = reply(buf, sizeof buf, 2, ref, v32);
	CHK(n && !chunk_done(&j, buf, n), "2 of 5 refused (full, or oversized): the chunk is not done");
	n = reply(buf, sizeof buf, 2, bad, v41);
	CHK(n && !chunk_done(&j, buf, n), "1 of 5 bad: the chunk is not done");
	CHK(!chunk_done(&j, (const unsigned char *)"\xff\xff", 2), "a reply that does not parse: not done");
	n = reply(buf, sizeof buf, 1, ok5, v5);
	CHK(n && chunk_done(&j, buf, n), "and the next clean chunk is done again");
	CHK(sh.refused == 2 && sh.bad == 1, "the tally still counts them (refused %llu, bad %llu)",
		sh.refused, sh.bad);

	/* a manifest of 8,000 files - more than a fixed array of 2^17 tokens holds at 19 a file */
	{
		struct shared ms;
		char path[600];
		FILE *f;
		int i, rc;

		snprintf(path, sizeof path, "%s/manifest.json", dir);
		f = fopen(path, "w");
		if (f) {
			fprintf(f, "{\"format\":\"perfdump-format\",\"format_version\":%d,"
				"\"tool\":\"perfdump 0.2\",\"source\":\"x:1\",\"started\":1,\"threads\":1,"
				"\"collections\":[{\"name\":\"c\",\"buckets\":1024}],\"files\":[", PCD_FORMAT);
			for (i = 0; i < 8000; i++)
				fprintf(f, "%s{\"file\":\"c.%04d.pcd\",\"collection\":\"c\",\"bucket_lo\":0,"
					"\"bucket_hi\":1024,\"records\":0,\"bytes\":4,\"crc32\":1,\"zstd\":0,"
					"\"complete\":true}", i ? "," : "", i);
			fprintf(f, "],\"records\":0,\"bytes\":0}\n");
			fclose(f);
		}
		memset(&ms, 0, sizeof ms);
		ms.o = &o;
		rc = manifest_read(&ms);
		CHK(rc == 0 && ms.nfiles == 8000, "a manifest of 8000 files is read whole (rc %d, %d files)",
			rc, ms.nfiles);
		free(ms.files);
	}

	/* perfload.done skips a chunk only for a run to the same target in the same mode */
	{
		static char d2[] = "/var/tmp/loadrefusetest.XXXXXX";
		struct opts o2 = o;
		struct shared ds;
		struct file fs[3];
		char path[600], last[600] = "", line[600];
		FILE *f;
		int i;

		if (!mkdtemp(d2)) {
			printf("  FAIL mkdtemp\n");
			return 1;
		}
		o2.dir = d2;
		memset(&ds, 0, sizeof ds);
		ds.o = &o2;
		pthread_mutex_init(&ds.mx, NULL);
		memset(fs, 0, sizeof fs);
		for (i = 0; i < 3; i++) {
			snprintf(fs[i].name, sizeof fs[i].name, "dc-%04d.pcd", i + 1);
			fs[i].records = 5;
		}
		ds.files = fs;
		ds.nfiles = 3;
		snprintf(path, sizeof path, "%s/perfload.done", d2);
		f = fopen(path, "w");
		if (f) {
			fputs("dc-0001.pcd 5 127.0.0.1:6479 newer\n"
				"dc-0002.pcd 5 192.0.2.9:6479 newer\n"
				"dc-0003.pcd 5 127.0.0.1:6479 set\n", f);
			fclose(f);
		}
		done_read(&ds);
		CHK(fs[0].done && !fs[1].done && !fs[2].done && ds.skipped == 1,
			"a restore run to 127.0.0.1:6479 skips only the chunk it loaded there "
			"(done %d %d %d, skipped %d)", fs[0].done, fs[1].done, fs[2].done, ds.skipped);

		o2.via_set = 1;
		done_mark(&ds, &fs[1]);
		if (ds.donef)
			fclose(ds.donef);
		ds.donef = NULL;
		f = fopen(path, "r");
		while (f && fgets(line, sizeof line, f))
			snprintf(last, sizeof last, "%s", line);
		if (f)
			fclose(f);
		CHK(!strcmp(last, "dc-0002.pcd 5 127.0.0.1:6479 set\n"),
			"a --via-set run records its mode as 'set': %s", last);

		for (i = 0; i < 3; i++)
			fs[i].done = 0;
		ds.skipped = 0;
		done_read(&ds);
		CHK(!fs[0].done && fs[1].done && fs[2].done && ds.skipped == 2,
			"a --via-set run skips only what --via-set runs loaded there (done %d %d %d)",
			fs[0].done, fs[1].done, fs[2].done);
		pthread_mutex_destroy(&ds.mx);
		snprintf(cmd, sizeof cmd, "rm -rf '%s'", d2);
		if (system(cmd) != 0)
			printf("  (could not remove %s)\n", d2);
	}

	/* a pass whose second thread cannot be started: its job fails, only started threads are joined */
	{
		struct job pj[3];
		pthread_t pth[3];
		int i;

		memset(pj, 0, sizeof pj);
		memset(pth, 0, sizeof pth);
		for (i = 0; i < 3; i++) {
			pj[i].sh = &sh;
			pj[i].idx = i;
		}
		creates = bogus_joins = 0;
		fail_create = 2;
		run_pass(pth, pj, 3, idle);
		fail_create = 0;
		CHK(!pj[0].failed && pj[1].failed && !pj[2].failed && bogus_joins == 0 && nlive == 0,
			"the thread pthread_create refused fails its job and is not joined "
			"(failed %d %d %d, joins of no thread %d, left unjoined %d)",
			pj[0].failed, pj[1].failed, pj[2].failed, bogus_joins, nlive);
	}

	if (sh.donef)
		fclose(sh.donef);
	snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
	if (system(cmd) != 0)
		printf("  (could not remove %s)\n", dir);
	printf("loadrefusetest: %s\n", fails ? "FAILED" : "all passed");
	return fails ? 1 : 0;
}
