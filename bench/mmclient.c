/*
 * mmclient.c — the mode matrix driven by a REAL client.
 *
 * pcbench speaks the wire directly from one connection, which measures
 * the daemon's pipelined ceiling but not what an application sees: it
 * cannot fail over, and it cannot route a key to its owner, so on
 * shard it pays a forward on most operations.  This drives the
 * same work through libperfd, so the matrix can show both the dumb and
 * the cluster-aware answer for every mode.
 *
 * One operation at a time, deliberately: an application's latency is
 * per request, and a pipelined number hides exactly the round trip
 * this is meant to expose.
 *
 * usage: mmclient <host> <port> <secret> <col> <keys> <valsize>
 *                 <getpct> <route 0|1> [binary 0|1] [nofill 0|1]
 * prints: fill=<ops/s> mix=<ops/s> p50=<us> p99=<us> missed=<n>
 *
 * nofill skips the write phase and reads a keyspace someone else
 * filled.  Keys are named from the index alone, so a fill through one
 * node and a read through another address exactly the same records -
 * which is the only way to measure what a read costs on a node that
 * did NOT write the data.
 *
 * The dialect matters as much as the routing: the binary frames carry
 * raw bytes with a fixed header, while the JSON-RPC line has to be
 * built, parsed and (for a non-UTF-8 value) base64'd at both ends.
 * Both are measured so the wire cost is visible next to the cluster
 * cost rather than mixed into it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../lib/perfd.h"

#define HIST 200000                    /* microsecond buckets */

static unsigned int hist[HIST];
static unsigned long hist_n;

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void note(double dt)
{
	long us = (long)(dt * 1e6);

	if (us < 0)
		us = 0;
	if (us >= HIST)
		us = HIST - 1;
	hist[us]++;
	hist_n++;
}

static long pct(double p)
{
	unsigned long want = (unsigned long)(hist_n * p), seen = 0;
	long i;

	for (i = 0; i < HIST; i++) {
		seen += hist[i];
		if (seen >= want)
			return i;
	}
	return HIST - 1;
}

int main(int argc, char **argv)
{
	const char *sec[2];
	perfd_opts o;
	perfd_t *p;
	char *val;
	const char *col;
	int keys, vlen, getpct, route, binary, nofill, i, missed = 0;
	double t0, fill_s, mix_s;

	if (argc < 9) {
		fprintf(stderr, "usage: %s host port secret col keys vlen "
			"getpct route\n", argv[0]);
		return 2;
	}
	col = argv[4];
	keys = atoi(argv[5]);
	vlen = atoi(argv[6]);
	getpct = atoi(argv[7]);
	route = atoi(argv[8]);
	binary = argc > 9 ? atoi(argv[9]) : 0;
	nofill = argc > 10 ? atoi(argv[10]) : 0;
	sec[0] = argv[3];
	sec[1] = NULL;
	val = malloc((size_t)vlen);
	if (!val)
		return 2;
	memset(val, 'v', (size_t)vlen);

	memset(&o, 0, sizeof o);
	o.secrets = sec;
	o.spares = -1;                 /* learn and hold the whole fleet */
	o.route_keys = route;
	o.binary = binary;             /* the data verbs ride raw frames */
	p = perfd_connect(argv[1], atoi(argv[2]), &o);
	if (!p) {
		fprintf(stderr, "connect: %s\n", perfd_error(NULL));
		return 2;
	}

	/* ---- fill ---- */
	fill_s = 0;
	if (!nofill) {
		t0 = now_s();
		for (i = 0; i < keys; i++) {
			char k[32];

			snprintf(k, sizeof k, "mm%08d", i);
			if (perfd_set(p, col, k, val, (size_t)vlen, 0) != 0) {
				fprintf(stderr, "set %d: %s\n", i,
					perfd_error(p));
				return 1;
			}
		}
		fill_s = now_s() - t0;
	}

	/* ---- mix, with per-operation latency ---- */
	t0 = now_s();
	for (i = 0; i < keys; i++) {
		char k[32];
		double a;

		snprintf(k, sizeof k, "mm%08d", (i * 7919) % keys);
		a = now_s();
		if (i % 100 < getpct) {
			char *v = NULL;
			size_t vl = 0;
			long long ttl = 0;
			int rc = perfd_get(p, col, k, (void **)&v, &vl, &ttl);

			if (rc < 0) {
				fprintf(stderr, "get: %s\n", perfd_error(p));
				return 1;
			}
			if (rc == 0)
				missed++;
			free(v);
		} else if (perfd_set(p, col, k, val, (size_t)vlen, 0) != 0) {
			fprintf(stderr, "set: %s\n", perfd_error(p));
			return 1;
		}
		note(now_s() - a);
	}
	mix_s = now_s() - t0;

	printf("fill=%.0f mix=%.0f p50=%ld p99=%ld missed=%d routing=%d "
		"binary=%d\n", fill_s > 0 ? keys / fill_s : 0,
		keys / mix_s, pct(0.50),
		pct(0.99), missed, perfd_routing(p), binary);
	perfd_free(p);
	return 0;
}
