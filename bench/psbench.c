/*
 * psbench - the pub/sub fan-out bench (PS7).
 *
 * Why this exists.  natbench measures request/response: one reply per
 * request, so the client's read side is bounded by what it asked for.
 * Fan-out inverts that - one publish becomes N deliveries - so the
 * load generator is the first thing to saturate, and a bench that does
 * not say so reports the CLIENT's ceiling as the server's.  This one
 * therefore reports its own CPU, counts what every subscriber received
 * separately from what was published, and refuses to call anything a
 * loss unless it can name the expectation it fell short of.
 *
 * Two modes, so the publisher and the subscribers can sit on different
 * hosts (and off the servers' hosts):
 *
 *   psbench sub <host> <port> <secret|-> <channel> <conns> <threads> <secs> [pattern 0|1]
 *   psbench pub <host> <port> <secret|-> <channel> <conns> <threads> <secs> <valsize> [rate/s]
 *
 * Both speak the native JSON dialect through libperfd's async surface:
 * one epoll per thread over that thread's handles, no blocking call on
 * a path that must keep reading.  A subscriber submits one subscribe
 * and then only reads; a publisher keeps <depth> publishes in flight
 * per connection, or paces to <rate> when one is given.
 *
 * What it prints, and what each number means:
 *   published/s      publishes the servers ACCEPTED (replies counted)
 *   receivers/publish  the local receiver count the node answered with
 *   delivered/s      messages the subscribers actually read
 *   expected/s       published/s * subscribers-on-this-driver
 *   shortfall        expected - delivered, as a percentage
 *   client CPU       this process's own CPU: over ~1 core per thread
 *                    means the figure above is the DRIVER's ceiling
 */
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "../lib/perfd.h"

static const char *g_host, *g_sec, *g_chan;
static int g_port, g_conns, g_threads, g_secs, g_val, g_rate, g_pat, g_pub;
static volatile int g_stop;

struct conn {
	perfd_t *p;
	int subscribed, inflight, idx;
	unsigned long long got, sent, acked, recvsum;
	double t_first, t_last;            /* first and last delivery or reply */
};

struct thr {
	pthread_t th;
	struct conn *c;
	int n, ep;
	unsigned long long got, sent, acked, recvsum, errs;
};

static double now_s(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* every delivery arrives here: an id-less notification, whose bytes are
 * NOT NUL-terminated at len */
static void on_notify(const char *json, size_t len, void *ctx)
{
	struct conn *c = ctx;

	(void)json;
	if (len) {
		c->t_last = now_s();
		if (!c->t_first)
			c->t_first = c->t_last;
		c->got++;
	}
}

static void sub_done(char *result, size_t len, const char *errmsg, void *ctx)
{
	struct conn *c = ctx;

	(void)len;
	if (result && !errmsg)
		c->subscribed = 1;
	free(result);
}

static void pub_done(char *result, size_t len, const char *errmsg, void *ctx)
{
	struct conn *c = ctx;
	const char *r;

	(void)len;
	c->inflight--;
	if (result && !errmsg) {
		c->t_last = now_s();
		if (!c->t_first)
			c->t_first = c->t_last;
		c->acked++;
		r = strstr(result, "\"receivers\":");
		if (r)
			c->recvsum += strtoull(r + 12, NULL, 10);
	}
	free(result);
}

static void arm(int ep, struct conn *c)
{
	struct epoll_event ev;
	int want = perfd_events(c->p), fd = perfd_fd(c->p);

	if (fd < 0)
		return;
	ev.events = (want & PERFD_EV_READ ? EPOLLIN : 0) |
		(want & PERFD_EV_WRITE ? EPOLLOUT : 0);
	ev.data.ptr = c;
	if (epoll_ctl(ep, EPOLL_CTL_MOD, fd, &ev) < 0)
		epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
}

static void publish_one(struct conn *c, const char *payload)
{
	char params[256 + 65536];
	int n = snprintf(params, sizeof params,
		"{\"channel\":\"%s\",\"payload\":\"%s\"}", g_chan, payload);

	if (n > 0 && perfd_submit(c->p, "publish", params, pub_done, c) == 0) {
		c->sent++;
		c->inflight++;
	}
}

static void *run(void *arg)
{
	struct thr *t = arg;
	char *payload = NULL;
	double t0 = now_s(), next = t0;
	int i;

	if (g_pub) {
		payload = malloc((size_t)g_val + 1);
		if (!payload)
			return NULL;
		memset(payload, 'x', (size_t)g_val);
		payload[g_val] = 0;
	}
	for (i = 0; i < t->n; i++)
		arm(t->ep, &t->c[i]);
	while (!g_stop) {
		struct epoll_event evs[256];
		int n = epoll_wait(t->ep, evs, 256, 20), k;

		for (k = 0; k < n; k++) {
			struct conn *c = evs[k].data.ptr;

			if (evs[k].events & EPOLLOUT)
				perfd_write_ready(c->p);
			if (evs[k].events & (EPOLLIN | EPOLLHUP | EPOLLERR))
				perfd_read_ready(c->p);
		}
		for (i = 0; i < t->n; i++) {
			struct conn *c = &t->c[i];

			if (perfd_state(c->p) != PERFD_ST_READY)
				continue;
			if (!g_pub && !c->subscribed) {
				char params[512];

				snprintf(params, sizeof params, "{\"%s\":\"%s\"}",
					g_pat ? "pattern" : "channel", g_chan);
				perfd_submit(c->p, g_pat ? "psubscribe" : "subscribe",
					params, sub_done, c);
				c->subscribed = -1;      /* submitted */
			} else if (g_pub) {
				/* 16 in flight whether or not a rate is set: at
				 * depth 1 a paced publisher is capped at
				 * connections / RTT, below the rates it is asked
				 * for, and would report its own ceiling */
				int depth = 16;

				while (c->inflight < depth) {
					if (g_rate) {
						double now = now_s();

						/* the handshake ran before the first
						 * publish; do not burst to catch up for
						 * it - 50 ms of backlog at most */
						if (next < now - 0.05)
							next = now - 0.05;
						if (now < next)
							break;
						/* the rate is the RUN's, so each thread
						 * paces at rate/threads */
						next += (double)g_threads / (double)g_rate;
					}
					publish_one(c, payload);
				}
			}
			arm(t->ep, c);
		}
	}
	for (i = 0; i < t->n; i++) {
		t->got += t->c[i].got;
		t->sent += t->c[i].sent;
		t->acked += t->c[i].acked;
		t->recvsum += t->c[i].recvsum;
	}
	free(payload);
	return NULL;
}

int main(int argc, char **argv)
{
	struct thr *ts;
	perfd_opts o;
	const char *sec[2];
	double t0, el, cpu;
	struct rusage ru;
	unsigned long long got = 0, sent = 0, acked = 0, recvsum = 0;
	double first = 0, last = 0, act;
	int i, per, made = 0, zero = 0;

	if (argc < 9) {
		fprintf(stderr, "usage: psbench sub|pub <host> <port> <secret|-> "
			"<channel> <conns> <threads> <secs> [valsize|pattern] [rate]\n");
		return 2;
	}
	g_pub = !strcmp(argv[1], "pub");
	g_host = argv[2]; g_port = atoi(argv[3]); g_sec = argv[4];
	g_chan = argv[5]; g_conns = atoi(argv[6]); g_threads = atoi(argv[7]);
	g_secs = atoi(argv[8]);
	if (g_pub) {
		g_val = argc > 9 ? atoi(argv[9]) : 64;
		g_rate = argc > 10 ? atoi(argv[10]) : 0;
		if (g_val < 1 || g_val > 60000)
			g_val = 64;
	} else {
		g_pat = argc > 9 ? atoi(argv[9]) : 0;
	}
	if (g_conns < 1 || g_threads < 1 || g_secs < 1)
		return 2;
	if (g_threads > g_conns)
		g_threads = g_conns;
	sec[0] = g_sec; sec[1] = NULL;
	memset(&o, 0, sizeof o);
	o.secrets = strcmp(g_sec, "-") ? sec : NULL;
	o.spares = 0;                  /* a capacity measurement pins the host */
	ts = calloc((size_t)g_threads, sizeof *ts);
	if (!ts)
		return 1;
	per = (g_conns + g_threads - 1) / g_threads;
	for (i = 0; i < g_threads; i++) {
		int j, n = per;

		if (i == g_threads - 1)
			n = g_conns - per * i;
		ts[i].n = n;
		ts[i].c = calloc((size_t)n, sizeof *ts[i].c);
		ts[i].ep = epoll_create1(0);
		if (!ts[i].c || ts[i].ep < 0)
			return 1;
		for (j = 0; j < n; j++) {
			ts[i].c[j].p = perfd_connect_async(g_host, g_port, &o);
			ts[i].c[j].idx = made;
			if (!ts[i].c[j].p) {
				fprintf(stderr, "connect failed: %s\n", perfd_error(NULL));
				return 1;
			}
			if (!g_pub)
				perfd_set_notify(ts[i].c[j].p, on_notify, &ts[i].c[j]);
			made++;
		}
	}
	printf("psbench %s: %d connections over %d threads to %s:%d, %s=%s, %ds\n",
		g_pub ? "PUB" : "SUB", g_conns, g_threads, g_host, g_port,
		g_pat ? "pattern" : "channel", g_chan, g_secs);
	t0 = now_s();
	for (i = 0; i < g_threads; i++)
		pthread_create(&ts[i].th, NULL, run, &ts[i]);
	while (now_s() - t0 < (double)g_secs)
		usleep(50000);
	g_stop = 1;
	for (i = 0; i < g_threads; i++)
		pthread_join(ts[i].th, NULL);
	el = now_s() - t0;
	for (i = 0; i < g_threads; i++) {
		int j;

		got += ts[i].got; sent += ts[i].sent;
		acked += ts[i].acked; recvsum += ts[i].recvsum;
		for (j = 0; j < ts[i].n; j++) {
			struct conn *c = &ts[i].c[j];

			if (!g_pub && !c->got)
				zero++;
			if (c->t_first && (!first || c->t_first < first))
				first = c->t_first;
			if (c->t_last > last)
				last = c->t_last;
		}
	}
	getrusage(RUSAGE_SELF, &ru);
	cpu = (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6 +
		(double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1e6;
	/* the ACTIVE window - first to last event.  A subscriber that waits
	 * for the publisher, or outlives it, is idle for part of its run, and
	 * dividing its CPU by the whole run hid a pegged driver behind a
	 * comfortable average. */
	act = last > first ? last - first : el;
	if (act < 0.1)
		act = el;
	printf("  active %.1fs of %.1fs\n", act, el);
	if (g_pub) {
		printf("  published %llu in %.1fs = %.0f/s  (submitted %llu)\n",
			acked, act, (double)acked / act, sent);
		printf("  receivers per publish (this node): %.2f\n",
			acked ? (double)recvsum / (double)acked : 0.0);
	} else {
		printf("  delivered %llu in %.1fs = %.0f/s over %d subscribers\n",
			got, act, (double)got / act, g_conns);
		printf("  per subscriber: %.0f/s   subscribers that got NOTHING: %d\n",
			(double)got / act / (double)g_conns, zero);
		if (zero)
			printf("  *** %d of %d subscribers received nothing - fan-out is\n"
			       "  *** incomplete, so the rate above is not a fan-out figure.\n",
			       zero, g_conns);
	}
	printf("  client CPU %.1fs over %.1fs active = %.1f cores (%d threads)\n",
		cpu, act, cpu / act, g_threads);
	if (cpu / act > (double)g_threads * 0.9)
		printf("  *** the driver is at its own ceiling - this measures the CLIENT.\n");
	return 0;
}
