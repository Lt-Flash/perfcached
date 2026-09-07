/*
 * keysnative — time the `keys` verb over the NATIVE dialect.
 *
 * Every enumeration figure so far came through the RESP door, where
 * reply encoding is a large share of the storm and dilutes whatever the
 * table walk costs.  libperfd is the dialect this project actually
 * promotes, so the walk's share - and therefore what a walk optimisation
 * is worth - has to be measured here too.
 *
 * usage: keysnative <host> <port> <secret|-> <col> <limit> <reps> [binary]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../lib/perfd.h"

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
	const char *host = argc > 1 ? argv[1] : "127.0.0.1";
	int port = argc > 2 ? atoi(argv[2]) : 17600;
	const char *secret = argc > 3 ? argv[3] : "-";
	const char *col = argc > 4 ? argv[4] : "0";
	int limit = argc > 5 ? atoi(argv[5]) : 50000;
	int reps = argc > 6 ? atoi(argv[6]) : 10;
	int binary = argc > 7 ? atoi(argv[7]) : 0;
	const char *sec[2] = { secret, NULL };
	perfd_opts o;
	perfd_t *h;
	int i;

	memset(&o, 0, sizeof o);
	o.secrets = strcmp(secret, "-") ? sec : NULL;
	o.spares = -1;
	o.binary = binary;
	h = perfd_connect(host, port, &o);
	if (!h) {
		fprintf(stderr, "connect failed\n");
		return 1;
	}
	for (i = 0; i < reps; i++) {
		char **keys = NULL;
		double t0 = now_ms();
		int n = perfd_keys(h, col, NULL, limit, &keys);
		double ms = now_ms() - t0;

		if (n < 0) {
			fprintf(stderr, "keys failed: %s\n", perfd_error(h));
			return 1;
		}
		printf("%.2f %d\n", ms, n);
		fflush(stdout);
		perfd_free_keys(keys, n);
	}
	perfd_free(h);
	return 0;
}
