/* clretaintest.c - S238: the retained-tombstone table (clretain.c).
 *
 *   A  get: a noted tombstone answers with the highest version noted, and
 *      nothing after retain_s
 *   B  each: replays exactly the entries planted in (since, upto]
 *   C  the cap: past `max`, the OLDEST goes; one evicted before retain_s
 *      is counted as dropped early, one past it is not
 *   D  expire: drops what is older than retain_s, keeps the rest
 *   E  memory at the default cap (250,000 x a realistic key): the bytes
 *      the table reports, and what the process actually grew by (RSS) -
 *      the figure the README quotes
 * Milliseconds, and a quarter-million allocations. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/clretain.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static long rss_kb(void)
{
	FILE *f = fopen("/proc/self/status", "r");
	char line[256];
	long v = -1;

	if (!f)
		return -1;
	while (fgets(line, sizeof line, f))
		if (!strncmp(line, "VmRSS:", 6))
			v = strtol(line + 6, NULL, 10);
	fclose(f);
	return v;
}

struct acc { unsigned int n; unsigned long long vsum; };
static void count_cb(const char *col, size_t cn, const char *key, size_t kn,
		uint64_t ver, void *ctx)
{
	struct acc *a = ctx;

	(void)col; (void)cn; (void)key; (void)kn;
	a->n++;
	a->vsum += ver;
}

int main(void)
{
	struct clretain_stats st;
	struct acc a;
	char k[32];
	long r0, r1;
	int i;

	/* ---- A..D on a small table: retain 100 s, 640 entries (10 a shard) */
	clretain_init(100, 640);
	clretain_note("c", 1, "k1", 2, 5, 1000);
	clretain_note("c", 1, "k1", 2, 3, 1001);     /* lower: the version stays */
	CHK(clretain_get("c", 1, "k1", 2, 1050) == 5, "A: the highest noted version answers (5)");
	CHK(clretain_get("c", 1, "k2", 2, 1050) == 0, "A: an unknown key answers 0");
	CHK(clretain_get("d", 1, "k1", 2, 1050) == 0, "A: the collection is part of the key");
	CHK(clretain_get("c", 1, "k1", 2, 1101) == 0, "A: past retain_s it answers nothing (planted 1001, asked 1101)");

	clretain_init(100, 640);
	for (i = 0; i < 300; i++) {
		snprintf(k, sizeof k, "b%03d", i);
		clretain_note("c", 1, k, strlen(k), (uint64_t)i + 1, 2000 + (unsigned)i / 10);
	}
	memset(&a, 0, sizeof a);
	CHK(clretain_each(2009, 2019, count_cb, &a) == 100 && a.n == 100,
		"B: (2009, 2019] replays exactly the 100 planted at 2010..2019 (%u)", a.n);
	memset(&a, 0, sizeof a);
	clretain_each(0, 5000, count_cb, &a);
	CHK(a.n == 300, "B: the whole table replays once (%u of 300)", a.n);

	/* C: 640 slots, 64 shards of 10; plant 2,000 over 20 s - most are
	 * evicted by the cap inside retain_s */
	clretain_init(100, 640);
	for (i = 0; i < 2000; i++) {
		snprintf(k, sizeof k, "c%05d", i);
		clretain_note("c", 1, k, strlen(k), (uint64_t)i + 1, 3000 + (unsigned)i / 100);
	}
	clretain_get_stats(&st, 3020);
	CHK(st.entries <= 640 && st.dropped_early >= 2000 - 640,
		"C: the cap holds (%llu of max 640) and early drops are counted (%llu)",
		st.entries, st.dropped_early);
	CHK(clretain_get("c", 1, "c01999", 6, 3020) == 2000 && clretain_get("c", 1, "c00000", 6, 3020) == 0,
		"C: the newest survives, the oldest went first");

	/* D: expire */
	clretain_init(100, 640);
	clretain_note("c", 1, "old", 3, 1, 4000);
	clretain_note("c", 1, "new", 3, 2, 4090);
	clretain_expire(4100);
	clretain_get_stats(&st, 4100);
	CHK(st.entries == 1 && clretain_get("c", 1, "new", 3, 4100) == 2,
		"D: expire at 4100 dropped the 4000 entry and kept the 4090 one");

	/* ---- E: memory at the default cap ------------------------------- */
	r0 = rss_kb();
	clretain_init(900, 250000);
	for (i = 0; i < 250000; i++) {
		snprintf(k, sizeof k, "call:%08d:branch", i);  /* 20 bytes */
		clretain_note("0", 1, k, strlen(k), (uint64_t)i + 1, 10000);
	}
	r1 = rss_kb();
	clretain_get_stats(&st, 10000);
	printf("  --   E: 250,000 retained, 20-byte keys: the table reports %.1f MB; "
		"the process grew %.1f MB (malloc headers and the hash included)\n",
		st.bytes / 1048576.0, (r1 - r0) / 1024.0);
	CHK(st.entries == 250000 && (r1 - r0) / 1024 < 40,
		"E: 250,000 held, and under 40 MB of process memory at the default cap");
	printf("clretaintest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
