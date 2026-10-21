/*
 * scancost.c — what one pcache_ht_scan() call costs, in-process.
 *
 * enumbench measured the per-SCAN-call gap over the wire (x1.2-1.6 vs
 * redis at matched density), but the wire and redis-cli put a ~50us
 * floor under every number.  This strips both: fill a table, sweep it
 * with the real cursor calls, report us/call and ns/key.  Run it
 * before AND after touching the scan path, same command, or the
 * "improvement" is a guess (S40).
 *
 * Build: make scancost   Run: ./scancost [nkeys] [val] [log2]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../src/compat/compat.h"
#include "../src/core/pcache_htable.h"
#include "../src/core/pcache_arena.h"
#include "../src/core/pcache_mem.h"

extern int pcache_arena_hugepage_mb;   /* selftest declares it the same way */

static double now_s(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static unsigned long long g_keys;
static int count_cb(const str *key, const str *val, unsigned int exp,
		void *p)
{
	(void)key; (void)val; (void)exp; (void)p;
	g_keys++;
	return 0;
}

static void sweep2(pcache_htable_t *ht, unsigned int count, int reps,
		unsigned int flags, const char *label)
{
	unsigned int cursor;
	unsigned long long calls = 0;
	double t0, dt;
	int r, rc;

	g_keys = 0;
	t0 = now_s();
	for (r = 0; r < reps; r++) {
		cursor = 0;
		do {
			rc = pcache_ht_scan_ex(ht, &cursor, count, flags,
				count_cb, NULL);
			calls++;
		} while (rc == 0 && cursor);
	}
	dt = now_s() - t0;
	printf("  COUNT %-5u %-7s %8.2f us/call  %7.1f ns/key  "
		"(%llu calls, %llu keys)\n",
		count, label, dt / calls * 1e6,
		g_keys ? dt / g_keys * 1e9 : 0, calls, g_keys);
}

int main(int argc, char **argv)
{
	unsigned int nkeys = argc > 1 ? (unsigned)atoi(argv[1]) : 100000;
	unsigned int vlen = argc > 2 ? (unsigned)atoi(argv[2]) : 200;
	unsigned int log2 = argc > 3 ? (unsigned)atoi(argv[3]) : 17;
	pcache_htable_t *ht;
	char kbuf[64], *vbuf;
	str k, v;
	unsigned int i;

	pcache_mem_probe();
	pcache_backing_policy = "own";
	pcache_arena_hugepage_mb = 512;
	if (pcache_arena_init() != 0) {
		fprintf(stderr, "arena init failed\n");
		return 1;
	}
	ht = pcache_htable_new(log2);
	if (!ht) {
		fprintf(stderr, "table init failed\n");
		return 1;
	}
	vbuf = malloc(vlen);
	memset(vbuf, 'x', vlen);
	v.s = vbuf; v.len = (int)vlen;
	for (i = 0; i < nkeys; i++) {
		k.s = kbuf;
		k.len = snprintf(kbuf, sizeof kbuf, "key:%012u", i);
		if (pcache_ht_store(ht, &k, &v, 0) != 0) {
			fprintf(stderr, "fill refused at %u\n", i);
			return 1;
		}
	}
	printf("scancost: %u keys x %uB, %u buckets\n",
		nkeys, vlen, pcache_ht_nbuckets(ht));
	sweep2(ht, 10, 3, 0, "values");
	sweep2(ht, 10, 3, PCACHE_SCAN_NOVAL, "noval");
	sweep2(ht, 1000, 20, 0, "values");
	sweep2(ht, 1000, 20, PCACHE_SCAN_NOVAL, "noval");
	sweep2(ht, 1024, 20, 0, "values");
	sweep2(ht, 1024, 20, PCACHE_SCAN_NOVAL, "noval");
	return 0;
}
