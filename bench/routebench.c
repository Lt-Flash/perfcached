/*
 * routebench.c — what does the forward hop actually cost?
 *
 * shard looks slow in the mode matrix because the load generator
 * there is dialect-level: it talks to ONE node, so on a shard
 * collection roughly (members-1)/members of every write has to be
 * forwarded to its owner and waited for.  The hashing is free; the
 * round trip is not.
 *
 * This measures the same work through libperfd with per-key routing
 * (task S35) off and then on, against the same fleet, so the forward
 * cost is isolated from everything else.  The daemons' own fwd_sent
 * counters are the cross-check - a routed client should drive them to
 * (near) zero.
 *
 * usage: routebench <host> <port> <secret> <keys> <0|1 route>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../lib/perfd.h"

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	const char *sec[2];
	perfd_opts o;
	perfd_t *p;
	char val[200];
	int keys, route, i;
	double t0, wr, rd;

	if (argc < 6) {
		fprintf(stderr, "usage: %s host port secret keys route\n",
			argv[0]);
		return 2;
	}
	keys = atoi(argv[4]);
	route = atoi(argv[5]);
	sec[0] = argv[3];
	sec[1] = NULL;
	memset(val, 'v', sizeof val);

	memset(&o, 0, sizeof o);
	o.secrets = sec;
	o.spares = -1;                 /* connect to the whole fleet */
	o.route_keys = route;
	p = perfd_connect(argv[1], atoi(argv[2]), &o);
	if (!p) {
		fprintf(stderr, "connect: %s\n", perfd_error(NULL));
		return 2;
	}
	if (perfd_routing(p) != route) {
		fprintf(stderr, "routing=%d, wanted %d (contract mismatch?)\n",
			perfd_routing(p), route);
		return 2;
	}

	t0 = now_s();
	for (i = 0; i < keys; i++) {
		char k[32];

		snprintf(k, sizeof k, "rb%08d", i);
		if (perfd_set(p, "c", k, val, sizeof val, 0) != 0) {
			fprintf(stderr, "set %d: %s\n", i, perfd_error(p));
			return 1;
		}
	}
	wr = now_s() - t0;

	t0 = now_s();
	for (i = 0; i < keys; i++) {
		char k[32], *v = NULL;
		size_t vl = 0;
		long long ttl = 0;

		snprintf(k, sizeof k, "rb%08d", i);
		if (perfd_get(p, "c", k, (void **)&v, &vl, &ttl) != 1) {
			fprintf(stderr, "get %d missed: %s\n", i,
				perfd_error(p));
			return 1;
		}
		free(v);
	}
	rd = now_s() - t0;

	printf("route=%d  writes %.0f/s  reads %.0f/s  "
		"(w %.1fus  r %.1fus per op)  owner-misses %llu\n",
		route, keys / wr, keys / rd,
		wr * 1e6 / keys, rd * 1e6 / keys,
		perfd_route_missed(p));
	perfd_free(p);
	return 0;
}
