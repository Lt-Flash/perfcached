/*
 * psengine.c - the pub/sub engine in isolation: how publish throughput
 * scales with the number of publishing workers, what patterns cost, and
 * how long a SUBSCRIBE waits while publishers run.
 *
 * Why this exists.  The engine was reviewed as "one mutex for every
 * publish, subscribe and delivery", and the fix for that is a redesign.
 * A redesign chosen without a number is a guess, so this links the real
 * src/pubsub.o against stubs for the three things around it (the relay,
 * the connection writer, the worker id) and measures only the engine:
 * no sockets, no framing cost on the wire, no network.
 *
 *   psengine <workers> <secs> <patterns> [churn 0|1|2]
 *
 * Each worker thread owns CHANNELS fake subscriber connections, one per
 * channel, so every channel has one subscriber on every worker and every
 * publish is delivered once per worker - half of the work lands on other
 * workers' queues, which is the path the lock sits on.  <patterns> adds
 * that many pattern subscriptions with distinct literal prefixes on
 * worker 1, plus four that start with '*' (no prefix to index).  With
 * churn, one extra thread subscribes and unsubscribes on its own worker
 * id in a loop and reports how long each call took.  With churn 2 it churns
 * channels and patterns the publishers ARE hitting and nobody else holds, so
 * shared entries are retired while readers use them - the case to run under
 * AddressSanitizer.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#include "pubsub.h"
#include "quiesce.h"

#define CHANNELS 64

static __thread int my_worker = -1;
static volatile int g_stop;
static int g_solo;
static unsigned long long g_pushed[600];

int pc_worker_id(void) { return my_worker; }
int pc_cluster_pubsub_relay(const char *c, size_t cl, const char *d, size_t dl)
{
	(void)c; (void)cl; (void)d; (void)dl;
	return 0;
}
int pc_conn_pubsub_push(void *conn, const char *pat, size_t plen,
		const char *chan, size_t clen, const char *data, size_t dlen,
		const char *f, size_t flen, struct pc_ps_note *note)
{
	(void)conn; (void)pat; (void)plen; (void)chan; (void)clen;
	(void)data; (void)dlen; (void)f; (void)flen; (void)note;
	g_pushed[my_worker]++;
	return 0;
}
int pc_conn_kill(void *conn, const char *why) { (void)conn; (void)why; return 1; }
void pc_cluster_pubsub_interest_add(uint64_t v, int pattern, const char *name,
		size_t nlen, uint32_t h1, uint32_t h2)
{
	(void)v; (void)pattern; (void)name; (void)nlen; (void)h1; (void)h2;
}
void pc_conn_subs_note(void *conn, int n) { (void)conn; (void)n; }

static double now_s(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

struct wk {
	pthread_t th;
	int id, efd;
	unsigned long long published;
	char conns[CHANNELS];          /* addresses only: the fake connections */
};

static void *worker(void *arg)
{
	struct wk *w = arg;
	unsigned int seed = (unsigned)w->id * 2654435761u;
	char chan[32];
	uint64_t junk;
	int n;

	my_worker = w->id;
	pc_qs_attach();
	pc_qs_enter();
	while (!g_stop) {
		int k;

		for (k = 0; k < 64; k++) {
			/* with churn, one publish in four goes to a channel that
			 * ONLY the churner subscribes to, so the shared entries it
			 * creates and retires are read while they are retired */
			if (g_solo && (k & 3) == 0)
				n = snprintf(chan, sizeof chan, "solo%d", (int)(rand_r(&seed) % 8));
			else
				n = snprintf(chan, sizeof chan, "ch%d", (int)(rand_r(&seed) % CHANNELS));
			pc_pubsub_publish(chan, (size_t)n, "payload", 7, 0);
			w->published++;
		}
		if (read(w->efd, &junk, sizeof junk) > 0)
			pc_pubsub_drain(w->id);
		pc_qs_exit();
		pc_qs_enter();
	}
	pc_pubsub_drain(w->id);
	pc_qs_exit();
	return NULL;
}

struct churn {
	pthread_t th;
	int id;
	unsigned long long ops;
	double worst, total;
	double lat[1 << 16];
	int nlat;
};

static void *churner(void *arg)
{
	struct churn *c = arg;
	char conn;
	int i = 0;

	my_worker = c->id;
	pc_qs_attach();
	while (!g_stop) {
		char name[32], pat[32];
		int n = g_solo ? snprintf(name, sizeof name, "solo%d", i % 8)
			: snprintf(name, sizeof name, "churn%d", i % 97);
		int pn = snprintf(pat, sizeof pat, "solo%d*", i % 8);
		double t0 = now_s(), dt;

		i++;
		pc_qs_enter();
		pc_pubsub_subscribe(&conn, c->id, name, (size_t)n, 0);
		if (g_solo)
			pc_pubsub_subscribe(&conn, c->id, pat, (size_t)pn, 1);
		pc_pubsub_unsubscribe(&conn, name, (size_t)n, 0);
		if (g_solo)
			pc_pubsub_unsubscribe(&conn, pat, (size_t)pn, 1);
		pc_qs_exit();
		dt = now_s() - t0;
		c->total += dt;
		if (dt > c->worst)
			c->worst = dt;
		if (c->nlat < (int)(sizeof c->lat / sizeof c->lat[0]))
			c->lat[c->nlat++] = dt;
		c->ops++;
	}
	return NULL;
}

static int cmp_d(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
	int nw, secs, npat, churn, i, k;
	struct wk *w;
	struct churn *ch = NULL;
	double t0, el;
	unsigned long long pub = 0, pushed = 0;
	char pconn[8];

	if (argc < 4) {
		fprintf(stderr, "usage: psengine <workers> <secs> <patterns> [churn 0|1]\n");
		return 2;
	}
	nw = atoi(argv[1]); secs = atoi(argv[2]); npat = atoi(argv[3]);
	churn = argc > 4 ? atoi(argv[4]) : 0;
	g_solo = churn == 2;               /* churn 2: churn what is being published */
	if (nw < 1 || nw > 500 || secs < 1)
		return 2;
	pc_pubsub_init(nw + 2);
	w = calloc((size_t)nw + 1, sizeof *w);
	for (i = 1; i <= nw; i++) {
		w[i].id = i;
		w[i].efd = eventfd(0, EFD_NONBLOCK);
		pc_pubsub_worker_register(i, w[i].efd);
		my_worker = i;
		for (k = 0; k < CHANNELS; k++) {
			char name[32];
			int n = snprintf(name, sizeof name, "ch%d", k);

			pc_pubsub_subscribe(&w[i].conns[k], i, name, (size_t)n, 0);
		}
	}
	my_worker = 1;
	for (k = 0; k < npat; k++) {
		char name[48];
		int n = snprintf(name, sizeof name, "grp%05d.*", k);

		pc_pubsub_subscribe(&pconn[0], 1, name, (size_t)n, 1);
	}
	if (npat)
		for (k = 0; k < 4; k++) {
			char name[16];
			int n = snprintf(name, sizeof name, "*zz%d", k);

			pc_pubsub_subscribe(&pconn[1], 1, name, (size_t)n, 1);
		}
	my_worker = -1;
	t0 = now_s();
	for (i = 1; i <= nw; i++)
		pthread_create(&w[i].th, NULL, worker, &w[i]);
	if (churn) {
		ch = calloc(1, sizeof *ch);
		ch->id = nw + 1;
		pthread_create(&ch->th, NULL, churner, ch);
	}
	sleep((unsigned)secs);
	g_stop = 1;
	for (i = 1; i <= nw; i++)
		pthread_join(w[i].th, NULL);
	if (ch)
		pthread_join(ch->th, NULL);
	el = now_s() - t0;
	for (i = 1; i <= nw; i++) {
		pub += w[i].published;
		pushed += g_pushed[i];
	}
	printf("workers=%d patterns=%d: %.0f publishes/s (%.0f per worker), %.0f deliveries/s\n",
		nw, npat, (double)pub / el, (double)pub / el / nw, (double)pushed / el);
	if (ch && ch->ops) {
		qsort(ch->lat, (size_t)ch->nlat, sizeof ch->lat[0], cmp_d);
		printf("  subscribe+unsubscribe under the storm: %llu ops, p50 %.1f us, p99 %.1f us, max %.1f us\n",
			ch->ops, ch->lat[ch->nlat / 2] * 1e6,
			ch->lat[(int)(ch->nlat * 0.99)] * 1e6, ch->worst * 1e6);
	}
	return 0;
}
