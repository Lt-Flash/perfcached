/* rollload - UP7c (DESIGN 12ns, 12ne UP10): load through libperfd while a
 * fleet is rolled, counting what the roll must not cost.
 *
 *   rollload <host> <port> <secret> <col> <threads> <keys> <seconds> <outfile>
 *
 * Each thread holds a cluster-aware handle (spares, so a node's restart is
 * a failover inside the library) and loops until SIGTERM or <seconds>:
 * half of its requests GET a pre-filled key "roll:p:<k>" whose value is
 * "p<k>" - a miss or another value is a loss; half SET "roll:w:<k>" to
 * "w<k>" (the same value every time, so any copy is right) and record
 * that the key was written.  At the end it writes to <outfile>:
 *   ops=N errors=N misses=N wrong=N failovers=N written=<bitmap as hex>
 * and exits 0 when errors, misses and wrong are all 0.  set, get and del
 * are idempotent, so the library retries them once on a standby when the
 * node it used goes away - the roll's restarts must never surface. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include "../lib/perfd.h"

static volatile sig_atomic_t stop;
static const char *g_host, *g_secret, *g_col;
static int g_port, g_keys;
static unsigned char *written;             /* one byte per w key */
static pthread_mutex_t wl = PTHREAD_MUTEX_INITIALIZER;

struct th {
	int idx;
	unsigned long long ops, errors, misses, wrong, failovers;
	char last_error[200];
};

static void on_term(int sig)
{
	(void)sig;
	stop = 1;
}

static void *worker(void *arg)
{
	struct th *t = arg;
	/* "-": no secret - a plaintext door (the suites' loopback nodes do
	 * no crypto: a Noise handshake there reads as a bad frame) */
	const char *secrets[2] = { g_secret, NULL };
	perfd_opts o = PERFD_OPTS_INIT;
	perfd_t *p;
	unsigned int seed = (unsigned int)t->idx * 2654435761u + 11u;
	char key[64], want[32], buf[64];

	o.secrets = strcmp(g_secret, "-") ? secrets : NULL;
	o.spares = 2;
	o.route_keys = 1;                 /* shard: each request to the key's owner */
	o.connect_timeout_ms = 5000;
	o.io_timeout_ms = 5000;
	p = perfd_connect(g_host, g_port, &o);
	if (!p) {
		t->errors++;
		snprintf(t->last_error, sizeof t->last_error, "connect: %s", perfd_error(NULL));
		return NULL;
	}
	while (!stop) {
		int k, r;
		size_t vlen = 0;

		seed = seed * 1103515245u + 12345u;
		k = (int)((seed >> 8) % (unsigned int)g_keys);
		if (seed & 1) {
			snprintf(key, sizeof key, "roll:p:%d", k);
			snprintf(want, sizeof want, "p%d", k);
			r = perfd_get_buf(p, g_col, key, buf, sizeof buf, &vlen, NULL);
			if (r == 1) {
				if (vlen != strlen(want) || memcmp(buf, want, vlen) != 0)
					t->wrong++;
			} else if (r == 0) {
				t->misses++;
			} else {
				t->errors++;
				snprintf(t->last_error, sizeof t->last_error, "get %s: %s", key, perfd_error(p));
			}
		} else {
			snprintf(key, sizeof key, "roll:w:%d", k);
			snprintf(want, sizeof want, "w%d", k);
			r = perfd_set(p, g_col, key, want, strlen(want), 0);
			if (r == 0) {
				pthread_mutex_lock(&wl);
				written[k] = 1;
				pthread_mutex_unlock(&wl);
			} else {
				t->errors++;
				snprintf(t->last_error, sizeof t->last_error, "set %s: %s", key, perfd_error(p));
			}
		}
		t->ops++;
	}
	t->failovers = perfd_failovers(p);
	perfd_free(p);
	return NULL;
}

int main(int argc, char **argv)
{
	int nth, secs, i, rc;
	struct th *ts;
	pthread_t *ids;
	unsigned long long ops = 0, errors = 0, misses = 0, wrong = 0, fo = 0;
	const char *outfile;
	FILE *f;
	time_t t0;

	if (argc != 9) {
		fprintf(stderr, "usage: rollload <host> <port> <secret|-> <col> <threads> <keys> <seconds> <outfile>\n");
		return 2;
	}
	g_host = argv[1]; g_port = atoi(argv[2]); g_secret = argv[3]; g_col = argv[4];
	nth = atoi(argv[5]); g_keys = atoi(argv[6]); secs = atoi(argv[7]); outfile = argv[8];
	if (nth < 1 || nth > 64 || g_keys < 1 || g_keys > 1000000 || secs < 1)
		return 2;
	written = calloc((size_t)g_keys, 1);
	ts = calloc((size_t)nth, sizeof *ts);
	ids = calloc((size_t)nth, sizeof *ids);
	if (!written || !ts || !ids)
		return 2;
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	for (i = 0; i < nth; i++) {
		ts[i].idx = i;
		pthread_create(&ids[i], NULL, worker, &ts[i]);
	}
	t0 = time(NULL);
	while (!stop && time(NULL) - t0 < secs)
		usleep(100000);
	stop = 1;
	for (i = 0; i < nth; i++) {
		pthread_join(ids[i], NULL);
		ops += ts[i].ops; errors += ts[i].errors; misses += ts[i].misses;
		wrong += ts[i].wrong; fo += ts[i].failovers;
	}
	f = fopen(outfile, "w");
	if (!f)
		return 2;
	fprintf(f, "ops=%llu errors=%llu misses=%llu wrong=%llu failovers=%llu seconds=%lld\n",
		ops, errors, misses, wrong, fo, (long long)(time(NULL) - t0));
	fprintf(f, "written=");
	for (i = 0; i < g_keys; i++)
		fputc(written[i] ? '1' : '0', f);
	fputc('\n', f);
	for (i = 0; i < nth; i++)
		if (ts[i].last_error[0])
			fprintf(f, "thread %d last error: %s\n", i, ts[i].last_error);
	fclose(f);
	rc = errors || misses || wrong ? 1 : 0;
	printf("rollload: ops %llu, errors %llu, misses %llu, wrong %llu, failovers %llu -> %s\n",
		ops, errors, misses, wrong, fo, rc ? "FAILED" : "clean");
	free(written); free(ts); free(ids);
	return rc;
}
