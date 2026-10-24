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

struct acc { unsigned int n, x; unsigned long long vsum; };
static void count_cb(const char *col, size_t cn, const char *key, size_t kn,
		uint64_t ver, int expiry, void *ctx)
{
	struct acc *a = ctx;

	(void)col; (void)cn; (void)key; (void)kn;
	a->n++;
	a->x += expiry != 0;               /* S446: the kind, for the replay */
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
	/* S392: a witness vouches only after the newest delete the cap lost -
	 * past retain_s's floor (2920), at the planting second of the newest
	 * entry evicted early (the table holds ~640 of 100 a second: ~3013) */
	{
		unsigned int cs = clretain_complete_since(3020);

		CHK(cs > 2920 && cs >= 3010 && cs < 3020,
			"C: complete-since moved to the newest early drop (%u, floor 2920)", cs);
	}

	/* C2: S443 - expiries counted apart from deletes.  Five of each on a
	 * fresh table; a delete noted for an expired key turns it into a
	 * delete; then expiries past the cap - their early drops are
	 * expiries, and a delete evicted early is not */
	{
		struct clretain_stats s0, s1;
		char kk[16];

		clretain_init(100, 640);
		clretain_get_stats(&s0, 4000);
		for (i = 0; i < 5; i++) {
			snprintf(kk, sizeof kk, "x%d", i);
			clretain_note_expiry("c", 1, kk, strlen(kk), 10, 4000);
			snprintf(kk, sizeof kk, "d%d", i);
			clretain_note("c", 1, kk, strlen(kk), 10, 4000);
		}
		clretain_get_stats(&s1, 4000);
		CHK(s1.entries == 10 && s1.expiries == 5 &&
		    s1.noted_expiries - s0.noted_expiries == 5 &&
		    s1.noted - s0.noted == 10,
			"C2: 10 kept, 5 of them expiries; 10 noted, 5 expiries (%llu, %llu)",
			s1.entries, s1.expiries);
		clretain_note("c", 1, "x0", 2, 11, 4001);
		clretain_note_expiry("c", 1, "d0", 2, 11, 4001);
		clretain_get_stats(&s1, 4001);
		CHK(s1.entries == 10 && s1.expiries == 4,
			"C2: a delete outranks an expiry, an expiry never a delete (%llu expiries)",
			s1.expiries);
		clretain_get_stats(&s0, 4001);
		for (i = 0; i < 2000; i++) {
			snprintf(k, sizeof k, "e%05d", i);
			clretain_note_expiry("c", 1, k, strlen(k), (uint64_t)i + 20, 4002 + (unsigned)i / 100);
		}
		clretain_get_stats(&s1, 4022);
		/* the seeded ten are now 6 deletes (d0..d4, and x0) and 4
		 * expiries; whichever of the deletes the cap pushed out are the
		 * only early drops that are not expiries - since S444, none: an
		 * expiry never pushes out a delete (C3) */
		{
			unsigned long long dr = s1.dropped_early - s0.dropped_early,
				dx = s1.dropped_early_expiries - s0.dropped_early_expiries,
				dels_left = s1.entries - s1.expiries;

			CHK(dr >= 2000 - 640 && dels_left <= 6 &&
			    dx == dr - (6 - dels_left),
				"C2: 2,000 expiries past the cap - %llu dropped early, %llu "
				"of them expiries: the rest are the %llu seeded deletes it "
				"pushed out", dr, dx, 6 - dels_left);
		}
	}

	/* C3: S444 - an expiry never pushes out a delete.  100 deletes, then
	 * 2,000 expiries past the cap: every delete is still held, and every
	 * early drop is an expiry */
	{
		struct clretain_stats s0, s1;
		int held = 0;

		clretain_init(100, 640);
		clretain_get_stats(&s0, 5000);
		for (i = 0; i < 100; i++) {
			snprintf(k, sizeof k, "del%03d", i);
			clretain_note("c", 1, k, strlen(k), (uint64_t)i + 1, 5000);
		}
		for (i = 0; i < 2000; i++) {
			snprintf(k, sizeof k, "exp%05d", i);
			clretain_note_expiry("c", 1, k, strlen(k), (uint64_t)i + 200, 5001 + (unsigned)i / 100);
		}
		for (i = 0; i < 100; i++) {
			snprintf(k, sizeof k, "del%03d", i);
			held += clretain_get("c", 1, k, strlen(k), 5021) == (uint64_t)i + 1;
		}
		clretain_get_stats(&s1, 5021);
		CHK(held == 100, "C3: 100 deletes, then 2,000 expiries past the cap - every delete still held (%d)", held);
		CHK(s1.dropped_early - s0.dropped_early >= 2000 + 100 - 640 &&
		    s1.dropped_early_expiries - s0.dropped_early_expiries ==
		    s1.dropped_early - s0.dropped_early,
			"C3: and every early drop was an expiry (%llu of %llu)",
			s1.dropped_early_expiries - s0.dropped_early_expiries,
			s1.dropped_early - s0.dropped_early);
		CHK(s1.entries <= 640 && s1.entries - s1.expiries == 100,
			"C3: the cap holds (%llu), 100 of them deletes", s1.entries);
		{
			struct acc a3;

			memset(&a3, 0, sizeof a3);
			clretain_each(0, 6000, count_cb, &a3);
			CHK(a3.n == s1.entries && a3.x == s1.expiries,
				"C3 S446: a replay walk says which are expiries (%u of %u)", a3.x, a3.n);
		}
		/* a delete still pushes out an expiry, and a delete when the
		 * shard has nothing else */
		for (i = 0; i < 1400; i++) {      /* ~22 a shard against ~8 expiries */
			snprintf(k, sizeof k, "d2_%04d", i);
			clretain_note("c", 1, k, strlen(k), (uint64_t)i + 5000, 5022);
		}
		clretain_get_stats(&s1, 5022);
		CHK(s1.entries <= 640 && s1.expiries < 10,
			"C3: 1,400 more deletes take the expiries' room first (%llu kept, %llu expiries)",
			s1.entries, s1.expiries);
	}

	/* D: expire */
	clretain_init(100, 640);
	clretain_note("c", 1, "old", 3, 1, 4000);
	clretain_note("c", 1, "new", 3, 2, 4090);
	clretain_expire(4100);
	clretain_get_stats(&st, 4100);
	CHK(st.entries == 1 && clretain_get("c", 1, "new", 3, 4100) == 2,
		"D: expire at 4100 dropped the 4000 entry and kept the 4090 one");
	CHK(clretain_complete_since(4100) == 4000,
		"D: S392 - nothing lost: complete since retain_s's floor (%u, want 4000)",
		clretain_complete_since(4100));

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
