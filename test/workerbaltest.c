/* workerbaltest.c - S460: libperfd places its connections on the daemon's
 * workers.  The kernel hashes each connection to a worker; a client that
 * opens a few long-lived connections gets a lumpy spread.  After connecting,
 * a handle asks `conninfo` and redials off a worker that holds more than one
 * connection beyond the least-loaded one.
 *   1  control: 8 handles with worker_balance = -1, five times - at least
 *      one round must put two on a worker (8!/8^8 = 0.24% a round does not),
 *      or this test could not see the fault it guards against
 *   2  8 balanced handles: one on each of the 8 workers
 *   3  16 balanced handles: two on each
 *   4  a balanced handle stores and reads back
 * Usage: workerbaltest host port secret   (a daemon with workers = 8) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "perfd.h"

static int pass, fail;
#define OK(c, m) do { if (c) { pass++; printf("  ok   %s\n", m); } \
	else { fail++; printf("  FAIL %s\n", m); } } while (0)
#define NMAX 16

static const char *host, *sec;
static int port;

static perfd_t *open_one(int balance)
{
	const char *secs[2] = { sec, NULL };
	perfd_opts o;

	memset(&o, 0, sizeof o);
	o.size = sizeof o;
	o.secrets = secs;
	o.spares = PERFD_SPARES_NONE;
	o.worker_balance = balance ? 0 : -1;
	return perfd_connect(host, port, &o);
}

/* an integer field of conninfo's JSON reply, -1 when absent */
static int field(const char *js, const char *key)
{
	char k[32];
	const char *at;

	snprintf(k, sizeof k, "\"%s\":", key);
	at = js ? strstr(js, k) : NULL;
	return at ? atoi(at + strlen(k)) : -1;
}

/* open @n handles; fill each worker's count of them; the number of workers */
static int spread(int n, int balance, int *per, perfd_t **h)
{
	int i, workers = 0;

	memset(per, 0, 64 * sizeof *per);
	for (i = 0; i < n; i++) {
		char *js;
		int w;

		h[i] = open_one(balance);
		if (!h[i]) {
			printf("  connect %d: %s\n", i, perfd_error(NULL));
			return -1;
		}
		js = perfd_command(h[i], "conninfo", NULL);
		w = field(js, "worker");
		workers = field(js, "workers");
		free(js);
		if (w < 0 || w >= 64)
			return -1;
		per[w]++;
	}
	return workers;
}

/* the daemon's native connections per worker (conninfo's "conns"); the total */
static int conns(perfd_t *p, int *v, int *n)
{
	char *js = perfd_command(p, "conninfo", NULL);
	const char *c = js ? strstr(js, "\"conns\":[") : NULL;
	int sum = 0;

	*n = 0;
	while (c && *n < 8 && (c = strpbrk(c + 1, "0123456789")) != NULL) {
		sum += v[(*n)++] = atoi(c);
		while (*c >= '0' && *c <= '9')
			c++;
	}
	free(js);
	return sum;
}

/* a closed connection leaves the daemon's count when its worker reads the
 * close: wait (3 s at most) until the count is the handles still open */
static int settle(perfd_t *p, int want, int *v, int *n)
{
	int i, sum = 0;

	for (i = 0; i < 150 && (sum = conns(p, v, n)) != want; i++)
		usleep(20000);
	return sum;
}

static void close_all(perfd_t **h, int n)
{
	int i;

	for (i = 0; i < n; i++)
		if (h[i])
			perfd_free(h[i]);
}

static void show(const char *what, const int *per, int workers)
{
	int i;

	printf("    %s:", what);
	for (i = 0; i < workers; i++)
		printf(" %d", per[i]);
	printf("\n");
}

int main(int argc, char **argv)
{
	perfd_t *h[NMAX];
	int per[64], workers, round, crowded = 0, i, even;
	char msg[160];

	if (argc != 4) {
		fprintf(stderr, "usage: workerbaltest host port secret\n");
		return 2;
	}
	host = argv[1];
	port = atoi(argv[2]);
	sec = argv[3];

	for (round = 0; round < 5; round++) {
		memset(h, 0, sizeof h);
		workers = spread(8, 0, per, h);
		if (workers != 8) {
			printf("  FAIL control: conninfo answered workers=%d, want 8\n", workers);
			return 1;
		}
		show("unbalanced", per, workers);
		for (i = 0; i < workers; i++)
			if (per[i] > 1)
				crowded++;
		close_all(h, 8);
	}
	snprintf(msg, sizeof msg, "1 control: unbalanced handles crowd a worker "
		"(%d crowded worker(s) over 5 rounds)", crowded);
	OK(crowded > 0, msg);

	memset(h, 0, sizeof h);
	workers = spread(8, 1, per, h);
	show("balanced 8", per, workers > 0 ? workers : 0);
	for (i = 0, even = workers == 8; i < 8 && even; i++)
		even = per[i] == 1;
	OK(even, "2 eight balanced handles: one on each of the 8 workers");

	{
		perfd_t *k[NMAX - 8];
		int v[8], n = 0, sum;

		memset(k, 0, sizeof k);
		/* the 40 control handles and any redial dropped must have left the count */
		sum = settle(h[0], 8, v, &n);
		if (sum != 8)
			printf("    before: %d connections counted, want 8\n", sum);
		for (i = 0; i < NMAX - 8; i++)
			k[i] = open_one(1);
		sum = settle(h[0], NMAX, v, &n);
		for (i = 0, even = n == 8 && sum == NMAX; i < 8 && even; i++)
			even = v[i] == 2;
		printf("    balanced 16 (%d counted):", sum);
		for (i = 0; i < n; i++)
			printf(" %d", v[i]);
		printf("\n");
		OK(even, "3 sixteen balanced handles: two on each worker");
		close_all(k, NMAX - 8);
	}

	{
		void *val = NULL;
		size_t vl = 0;
		long long ttl = 0;
		int ok = perfd_set(h[0], "0", "wbal-key", "wbal-val", 8, 0) == 0 &&
			perfd_get(h[0], "0", "wbal-key", &val, &vl, &ttl) == 1 &&
			vl == 8 && !memcmp(val, "wbal-val", 8);

		OK(ok, "4 a balanced handle stores and reads back");
		perfd_free_value(val);
	}
	close_all(h, 8);

	printf("%s: %d passed, %d failed\n", fail ? "FAIL" : "PASS", pass, fail);
	return fail ? 1 : 0;
}
