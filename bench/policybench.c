/*
 * policybench.c — where do clients actually land?
 *
 * libperfd's spreading policies (S34) choose which node a handle works
 * through.  The question a table has to answer is not "does the code
 * run" but "where did N independent clients end up, and what did it
 * cost them", so this opens N handles under one policy, records the
 * node each one settled on, and drives the same work through every one.
 *
 * Clients are independent on purpose: round-robin picks a RANDOM start
 * per client precisely so a thousand of them spread without
 * coordinating, and that only shows up across many handles.
 *
 * usage: policybench <host> <port> <secret> <col> <policy> <clients> <ops>
 *        policy: failover | rr | leastconn | weighted
 * prints: policy=<name> spread=<n1:c1,n2:c2,...> distinct=<n>
 *         ops_per_s=<total> p50=<us>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../lib/perfd.h"

#define MAXC 64

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static int cmp_l(const void *a, const void *b)
{
	long x = *(const long *)a, y = *(const long *)b;

	return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
	const char *sec[2], *pname;
	perfd_opts o;
	perfd_t *h[MAXC];
	long *lat;
	int nodes[MAXC], seen[MAXC], nseen = 0, counts[MAXC];
	int policy, nclients, ops, i, j, n;
	double t0, dt;
	char val[128];

	if (argc < 8) {
		fprintf(stderr, "usage: %s host port secret col policy "
			"clients ops\n", argv[0]);
		return 2;
	}
	pname = argv[5];
	if (!strcmp(pname, "failover"))
		policy = PERFD_POLICY_FAILOVER;
	else if (!strcmp(pname, "rr"))
		policy = PERFD_POLICY_ROUND_ROBIN;
	else if (!strcmp(pname, "leastconn"))
		policy = PERFD_POLICY_LEAST_CONN;
	else if (!strcmp(pname, "weighted"))
		policy = PERFD_POLICY_WEIGHTED;
	else {
		fprintf(stderr, "policy: failover|rr|leastconn|weighted\n");
		return 2;
	}
	nclients = atoi(argv[6]);
	if (nclients > MAXC)
		nclients = MAXC;
	ops = atoi(argv[7]);
	sec[0] = argv[3];
	sec[1] = NULL;
	memset(val, 'p', sizeof val);
	lat = malloc((size_t)nclients * (size_t)ops * sizeof *lat);
	if (!lat)
		return 2;

	for (i = 0; i < nclients; i++) {
		memset(&o, 0, sizeof o);
		o.secrets = sec;
		o.spares = -1;
		o.policy = policy;
		h[i] = perfd_connect(argv[1], atoi(argv[2]), &o);
		if (!h[i]) {
			fprintf(stderr, "client %d: %s\n", i,
				perfd_error(NULL));
			return 2;
		}
		nodes[i] = perfd_active_node(h[i]);
	}

	/* interleave the clients so the work is spread in time as well as
	 * across handles - a burst down one handle would measure that
	 * handle, not the policy */
	n = 0;
	t0 = now_s();
	for (j = 0; j < ops; j++)
		for (i = 0; i < nclients; i++) {
			char k[32];
			double a = now_s();

			snprintf(k, sizeof k, "pb%02d%06d", i, j);
			if (perfd_set(h[i], argv[4], k, val, sizeof val, 0)
			        != 0) {
				fprintf(stderr, "set c%d: %s\n", i,
					perfd_error(h[i]));
				return 1;
			}
			lat[n++] = (long)((now_s() - a) * 1e6);
		}
	dt = now_s() - t0;

	/* the spread */
	memset(counts, 0, sizeof counts);
	for (i = 0; i < nclients; i++) {
		int f = -1;

		for (j = 0; j < nseen; j++)
			if (seen[j] == nodes[i])
				f = j;
		if (f < 0) {
			seen[nseen] = nodes[i];
			f = nseen++;
		}
		counts[f]++;
	}
	qsort(lat, (size_t)n, sizeof *lat, cmp_l);

	printf("policy=%-9s clients=%d spread=", pname, nclients);
	for (j = 0; j < nseen; j++)
		printf("%snode%d:%d", j ? "," : "", seen[j], counts[j]);
	printf(" distinct=%d ops_per_s=%.0f p50=%ldus p99=%ldus\n",
		nseen, n / dt, lat[n / 2], lat[(int)(n * 0.99)]);

	for (i = 0; i < nclients; i++)
		perfd_free(h[i]);
	free(lat);
	return 0;
}
