/*
 * natbench — a native-dialect load generator that can actually saturate
 * the daemon, and says so when it cannot.
 *
 * Why this exists.  pcbench has -d but speaks plaintext, and a
 * non-loopback listener refuses that, so it cannot drive a remote
 * server at depth.  concbench authenticates but is synchronous - one
 * operation per connection at a time.  A first attempt at an async
 * client used one OS thread per connection with its own poll(), and
 * capped at ~500k ops/s whether or not a network was in the path, while
 * pcbench drove the same server at 6.15M on loopback: it was measuring
 * itself.  That is the failure this file is built to avoid.
 *
 * So: few threads, MANY connections each, one poll() per loop over all
 * of a thread's sockets, @depth requests in flight per connection.  And
 * it reports its own CPU and its hit count, because a client that is
 * saturated - or that is reading keys nobody stored - produces a
 * confident number that means nothing.
 *
 * usage: natbench <host> <port> <secret|-> <col> <conns> <threads>
 *                 <depth> <secs> <keyspace> <valsize> <getpct>
 *                 <binary 0|1> [latrate] [spares]
 *
 * spares: -1 (default) lets libperfd open standbys; 0 pins every
 * connection to <host>.  Standbys are a PREREQUISITE for routing, not a
 * trigger.
 * route:  1 turns on per-key routing (opts.route_keys), which is what
 * actually makes the client compute each key's owner and skip the
 * daemon's forward hop.  Default 0 - the un-routed path, which is what
 * every benchmark before this measured.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <poll.h>
#include <pthread.h>
#include <sys/resource.h>

#include "../lib/perfd.h"

#define HIST_US  65536                 /* 1us buckets to 65ms, then clamp */

static const char *g_host, *g_sec, *g_col;
static int g_port, g_depth, g_secs, g_keys, g_val, g_getpct, g_bin;
/* Latency sampling rate: 1 = time every request (default), N = every
 * Nth, 0 = off.  Two clock_gettime() calls per request cost a measured
 * 9.3% of this client's CPU at depth 256, and the callback that
 * consumes them another 9% - a quarter of the client's time is the
 * instrument.  When the question is how fast the LIBRARY goes, that
 * has to be able to step out of the way. */
static int g_lat = 1;

/* Standbys, and therefore per-key ROUTING (S35): libperfd computes each
 * key's owner and talks to it directly, which removes the daemon's
 * forward hop.  -1 = as many as the fleet offers, 0 = none.
 *
 * A capacity measurement needs 0.  With routing on, the CLIENT does the
 * distributing, so every mode spreads across the fleet and store looks
 * exactly like shard - which is how the first capacity run reported
 * store holding 115,000 keys across three nodes when store is supposed
 * to keep them local.  Turn it off to see what the DAEMON's placement
 * does on its own. */
static int g_spares = -1;

/* opts.route_keys - per-key routing (S35), and it is OPT-IN.  Setting
 * spares alone is not enough: route_want comes from this flag, so a
 * client that asks for standbys and nothing else still sends every
 * request to the node it dialled and lets the daemon forward.  Every
 * shard number this harness has ever produced was that un-routed path,
 * which is why shard measured 6.6x behind store with the daemon's own
 * fwd_sent counter at 845,762 against store's 0. */
static int g_route = 0;

/* THE ROUTED CLIENT.  libperfd computes owners but cannot route on the
 * async API: routing needs a connection per node, and the library must
 * not open connections an async caller would never poll.  So the
 * APPLICATION does it - which is the shape a real async client has to
 * take anyway.
 *
 * Each connection dials a different member and draws only from that
 * member's keys, so no request ever needs forwarding.  The owner of
 * every key is asked once at startup via perfd_owner_of(), the same
 * computation the daemon uses, rather than reimplemented here. */
#define NB_MAXMEM 32
static perfd_t *g_map;                 /* holds the learned fleet */
static int g_nmem;
static char g_maddr[NB_MAXMEM][64];
static int g_mport[NB_MAXMEM];
static int *g_klist[NB_MAXMEM];        /* keys this member owns */
static int g_kcount[NB_MAXMEM];

struct req {                           /* one in-flight slot, reused */
	double t0;
	struct conn *c;
	int free;
};

struct conn {
	perfd_t *h;
	int mem;                       /* member index this conn dials */
	struct req *slots;
	int nfree, *freelist;
	struct thr *t;
	unsigned int seed;
	int dead;
};

struct thr {
	struct conn *cs;
	int nconn;
	struct pollfd *pfd;
	long long done, errs, hits, timed;
	unsigned int *hist;
	double worst;
	char *val;
};

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void on_reply(char *result, size_t len, const char *errmsg, void *ctx)
{
	struct req *r = ctx;
	struct conn *c = r->c;
	struct thr *t = c->t;
	if (r->t0 > 0.0) {
		double us = (now_s() - r->t0) * 1e6;
		unsigned int b = us < 0 ? 0 :
			(us >= HIST_US - 1 ? HIST_US - 1 : (unsigned int)us);

		t->hist[b]++;
		t->timed++;
		if (us > t->worst)
			t->worst = us;
	}
	t->done++;
	if (errmsg)
		t->errs++;
	/* A miss is not an error, and a keyspace nobody filled produces a
	 * run of confident numbers for no work.  Count real payloads. */
	if (result && len >= (size_t)g_val)
		t->hits++;
	if (result)
		free(result);
	r->free = 1;
	c->freelist[c->nfree++] = (int)(r - c->slots);
}

static int top_up(struct conn *c)
{
	char params[512];
	struct thr *t = c->t;
	int n_sub = 0;

	while (c->nfree > 0) {
		int si = c->freelist[c->nfree - 1];
		struct req *r = &c->slots[si];
		unsigned int k;

		if (g_nmem > 0 && g_kcount[c->mem] > 0)
			k = (unsigned int)g_klist[c->mem]
				[rand_r(&c->seed) % (unsigned int)g_kcount[c->mem]];
		else
			k = rand_r(&c->seed) % (unsigned int)g_keys;
		int n, is_get = (int)(rand_r(&c->seed) % 100) < g_getpct;

		r->t0 = (g_lat && (n_sub + (int)(c->seed & 0xff)) % g_lat == 0)
			? now_s() : 0.0;
		r->c = c;
		r->free = 0;
		if (g_bin) {
			/* typed: no JSON is formatted here and none is
			 * parsed on the reply */
			char kb[32];

			snprintf(kb, sizeof kb, "key:%012u", k);
			if (perfd_submit_kv(c->h,
			        is_get ? PERFD_V_GET : PERFD_V_SET,
			        g_col, kb, is_get ? NULL : t->val,
			        is_get ? 0 : (size_t)g_val, 0, 0,
			        on_reply, r) != 0)
				return -1;
		} else {
			if (is_get)
				n = snprintf(params, sizeof params,
					"{\"col\":\"%s\",\"key\":\"key:%012u\"}",
					g_col, k);
			else
				n = snprintf(params, sizeof params,
					"{\"col\":\"%s\",\"key\":\"key:%012u\","
					"\"value\":\"%s\"}", g_col, k, t->val);
			if (n < 0 || n >= (int)sizeof params)
				return -1;
			if (perfd_submit(c->h, is_get ? "get" : "set", params,
			        on_reply, r) != 0)
				return -1;
		}
		c->nfree--;
		n_sub++;
	}
	/* One write for the whole batch.  Without opts.defer_push each
	 * submit writes on its own, so a depth of 64 costs 64 write()
	 * syscalls - which is what made a profile of this client 24%+
	 * kernel and capped it well below what the daemon can serve. */
	if (n_sub && perfd_push(c->h) < 0)
		return -1;
	return 0;
}

static short want(perfd_t *h)
{
	int ev = perfd_events(h);

	return (short)(((ev & PERFD_EV_READ) ? POLLIN : 0) |
	               ((ev & PERFD_EV_WRITE) ? POLLOUT : 0));
}

static void *worker(void *arg)
{
	struct thr *t = arg;
	const char *sec[2] = { g_sec, NULL };
	perfd_opts o;
	double end;
	int i, ready = 0;

	memset(&o, 0, sizeof o);
	o.secrets = strcmp(g_sec, "-") ? sec : NULL;
	o.spares = g_spares;
	o.route_keys = g_route;
	o.binary = g_bin;
	/* batching is libperfd's default now; nothing to ask for */

	for (i = 0; i < t->nconn; i++) {
		struct conn *c = &t->cs[i];

		c->t = t;
		c->seed = (unsigned int)(uintptr_t)c + (unsigned int)i * 7919u;
		/* spread across the fleet when routing; otherwise every
		 * connection dials the host given on the command line and
		 * the daemon forwards, which is the un-routed baseline */
		c->mem = g_nmem > 0 ? i % g_nmem : 0;
		c->h = g_nmem > 0
			? perfd_connect_async(g_maddr[c->mem], g_mport[c->mem], &o)
			: perfd_connect_async(g_host, g_port, &o);
		if (!c->h) {
			c->dead = 1;
			continue;
		}
		c->slots = calloc((size_t)g_depth, sizeof *c->slots);
		c->freelist = calloc((size_t)g_depth, sizeof *c->freelist);
		if (!c->slots || !c->freelist)
			return NULL;
		for (int s = 0; s < g_depth; s++)
			c->freelist[s] = s;
		c->nfree = g_depth;
	}

	/* drive every handshake to READY before the clock starts */
	end = now_s() + 20.0;
	while (now_s() < end) {
		int pending = 0, np = 0;

		for (i = 0; i < t->nconn; i++) {
			struct conn *c = &t->cs[i];

			if (c->dead || !c->h)
				continue;
			if (perfd_state(c->h) == PERFD_ST_CONNECTING) {
				t->pfd[np].fd = perfd_fd(c->h);
				t->pfd[np].events = want(c->h);
				t->pfd[np].revents = 0;
				np++; pending++;
			}
		}
		if (!pending)
			break;
		if (poll(t->pfd, (nfds_t)np, 200) <= 0)
			continue;
		np = 0;
		for (i = 0; i < t->nconn; i++) {
			struct conn *c = &t->cs[i];

			if (c->dead || !c->h ||
			        perfd_state(c->h) != PERFD_ST_CONNECTING)
				continue;
			if (t->pfd[np].revents & POLLOUT)
				perfd_write_ready(c->h);
			if (t->pfd[np].revents & POLLIN)
				perfd_read_ready(c->h);
			np++;
		}
	}
	for (i = 0; i < t->nconn; i++)
		if (t->cs[i].h && perfd_state(t->cs[i].h) == PERFD_ST_READY)
			ready++;
	if (!ready) {
		fprintf(stderr, "no connection reached READY: %s\n",
			t->cs[0].h ? perfd_error(t->cs[0].h) : "connect failed");
		return NULL;
	}

	end = now_s() + g_secs;
	while (now_s() < end) {
		int np = 0;

		for (i = 0; i < t->nconn; i++) {
			struct conn *c = &t->cs[i];

			if (c->dead || !c->h ||
			        perfd_state(c->h) != PERFD_ST_READY)
				continue;
			if (top_up(c) != 0) {
				c->dead = 1;
				continue;
			}
			t->pfd[np].fd = perfd_fd(c->h);
			t->pfd[np].events = want(c->h);
			t->pfd[np].revents = 0;
			np++;
		}
		if (!np)
			break;
		if (poll(t->pfd, (nfds_t)np, 50) <= 0)
			continue;
		np = 0;
		for (i = 0; i < t->nconn; i++) {
			struct conn *c = &t->cs[i];

			if (c->dead || !c->h ||
			        perfd_state(c->h) != PERFD_ST_READY)
				continue;
			if ((t->pfd[np].revents & POLLOUT) &&
			        perfd_write_ready(c->h) != 0)
				c->dead = 1;
			else if ((t->pfd[np].revents & POLLIN) &&
			        perfd_read_ready(c->h) != 0)
				c->dead = 1;
			np++;
		}
	}
	return NULL;
}

static double pct(const unsigned int *h, long long tot, double p)
{
	long long want_n = (long long)(tot * p), seen = 0;
	unsigned int i;

	for (i = 0; i < HIST_US; i++) {
		seen += h[i];
		if (seen >= want_n)
			return i / 1000.0;
	}
	return (HIST_US - 1) / 1000.0;
}

int main(int argc, char **argv)
{
	int nconn, nthr, i, j;
	struct thr *ts;
	pthread_t *th;
	unsigned int *hist;
	double t0, el, cpu;
	long long tot = 0, errs = 0, hits = 0, timed = 0;
	struct rusage ru0, ru1;
	char *val;

	if (argc < 13) {
		fprintf(stderr, "usage: %s host port secret col conns threads "
			"depth secs keyspace valsize getpct binary "
			"[latrate] [spares] [route]\n", argv[0]);
		return 2;
	}
	g_host = argv[1]; g_port = atoi(argv[2]); g_sec = argv[3];
	g_col = argv[4]; nconn = atoi(argv[5]); nthr = atoi(argv[6]);
	g_depth = atoi(argv[7]); g_secs = atoi(argv[8]); g_keys = atoi(argv[9]);
	g_val = atoi(argv[10]); g_getpct = atoi(argv[11]); g_bin = atoi(argv[12]);
	if (argc > 13)
		g_lat = atoi(argv[13]);
	if (argc > 14)
		g_spares = atoi(argv[14]);
	if (argc > 15)
		g_route = atoi(argv[15]);
	if (nthr < 1 || nconn < nthr) {
		fprintf(stderr, "need conns >= threads >= 1\n");
		return 2;
	}
	/* Learn the fleet ONCE, before any worker connects.  An async
	 * handle discovers members through its own pipeline, so this has
	 * to pump events until routing turns on rather than blocking on a
	 * reply.  Everything after is read-only, so the workers share it
	 * without a lock. */
	if (g_route) {
		perfd_opts mo;
		const char *msec[2] = { g_sec, NULL };
		double deadline;

		memset(&mo, 0, sizeof mo);
		mo.secrets = strcmp(g_sec, "-") ? msec : NULL;
		mo.route_keys = 1;
		g_map = perfd_connect_async(g_host, g_port, &mo);
		deadline = now_s() + 10.0;
		while (g_map && now_s() < deadline) {
			struct pollfd pf;
			int ev = perfd_events(g_map);

			if (perfd_routing(g_map))
				break;
			pf.fd = perfd_fd(g_map);
			pf.events = (short)(((ev & PERFD_EV_READ) ? POLLIN : 0) |
				((ev & PERFD_EV_WRITE) ? POLLOUT : 0));
			if (poll(&pf, 1, 100) > 0) {
				if (pf.revents & POLLOUT)
					perfd_write_ready(g_map);
				if (pf.revents & (POLLIN | POLLHUP | POLLERR))
					perfd_read_ready(g_map);
			}
		}
		if (!g_map || !perfd_routing(g_map)) {
			fprintf(stderr, "route=1 but the fleet never resolved "
				"(routing=%d) - falling back to un-routed\n",
				g_map ? perfd_routing(g_map) : -1);
			g_nmem = 0;
		} else {
			int m = perfd_member_count(g_map), mi;

			if (m > NB_MAXMEM)
				m = NB_MAXMEM;
			for (mi = 0; mi < m; mi++) {
				int port = 0, node = 0, act = 0;

				if (perfd_member_info(g_map, mi, g_maddr[g_nmem],
				        sizeof g_maddr[0], &port, &node,
				        &act) != 0)
					continue;
				g_mport[g_nmem] = port;
				g_nmem++;
			}
			/* bucket every key by its owner, using the library's
			 * own computation - not a reimplementation of it */
			for (mi = 0; mi < g_nmem; mi++)
				g_klist[mi] = malloc((size_t)g_keys * sizeof(int));
			for (i = 0; i < g_keys; i++) {
				char kb[32];
				int ow;

				snprintf(kb, sizeof kb, "key:%012u", (unsigned)i);
				ow = perfd_owner_of(g_map, g_col, kb);
				if (ow < 0 || ow >= g_nmem)
					ow = i % (g_nmem ? g_nmem : 1);
				if (g_klist[ow])
					g_klist[ow][g_kcount[ow]++] = i;
			}
			printf("  fleet: %d members, keys per member:", g_nmem);
			for (mi = 0; mi < g_nmem; mi++)
				printf(" %d", g_kcount[mi]);
			printf("\n");
		}
	}

	val = malloc((size_t)g_val + 1);
	if (!val)
		return 1;
	memset(val, 'V', (size_t)g_val);
	val[g_val] = 0;

	ts = calloc((size_t)nthr, sizeof *ts);
	th = calloc((size_t)nthr, sizeof *th);
	hist = calloc((size_t)nthr * HIST_US, sizeof *hist);
	if (!ts || !th || !hist)
		return 1;
	for (i = 0; i < nthr; i++) {
		int share = nconn / nthr + (i < nconn % nthr ? 1 : 0);

		ts[i].nconn = share;
		ts[i].cs = calloc((size_t)share, sizeof *ts[i].cs);
		ts[i].pfd = calloc((size_t)share, sizeof *ts[i].pfd);
		ts[i].hist = hist + (size_t)i * HIST_US;
		ts[i].val = val;
		if (!ts[i].cs || !ts[i].pfd)
			return 1;
	}

	getrusage(RUSAGE_SELF, &ru0);
	t0 = now_s();
	for (i = 0; i < nthr; i++)
		pthread_create(&th[i], NULL, worker, &ts[i]);
	for (i = 0; i < nthr; i++)
		pthread_join(th[i], NULL);
	el = now_s() - t0;
	getrusage(RUSAGE_SELF, &ru1);

	for (i = 1; i < nthr; i++)
		for (j = 0; j < HIST_US; j++)
			ts[0].hist[j] += ts[i].hist[j];
	for (i = 0; i < nthr; i++) {
		tot += ts[i].done; errs += ts[i].errs; hits += ts[i].hits;
		timed += ts[i].timed;
		if (ts[i].worst > ts[0].worst)
			ts[0].worst = ts[i].worst;
	}
	cpu = (ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec) +
	      (ru1.ru_utime.tv_usec - ru0.ru_utime.tv_usec) / 1e6 +
	      (ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec) +
	      (ru1.ru_stime.tv_usec - ru0.ru_stime.tv_usec) / 1e6;

	/* opts.binary does NOT reach the async path: perfd_submit() calls
	 * the JSON enqueue unconditionally, and enqueue_bin() is reachable
	 * only from bin_roundtrip_once(), which is synchronous.  So an
	 * async caller is on the TEXT dialect whatever it asks for, and
	 * saying "binary=1" here would be the instrument lying. */
	/* Whether per-key ROUTING actually engaged, which the client was
	 * never asked before.  A routed client talks to each key's owner
	 * and pays no forward; an unrouted one sends everything to the
	 * node it connected to and the daemon forwards.  Those are wildly
	 * different experiments, and "shard is slow" means nothing until
	 * you know which one ran.  route_missed counts requests whose
	 * owner was known but not connected - they went to the active node
	 * and paid the hop anyway. */
	{
		int routed = 0;
		unsigned long long missed = 0;

		for (i = 0; i < nthr; i++)
			for (j = 0; j < ts[i].nconn; j++)
				if (ts[i].cs[j].h) {
					if (perfd_routing(ts[i].cs[j].h))
						routed++;
					missed += perfd_route_missed(ts[i].cs[j].h);
				}
		printf("  routing: %d/%d connections, route_missed=%llu"
			" (spares=%d route=%d)\n", routed, nconn, missed,
			g_spares, g_route);
	}
	printf("conns=%d threads=%d depth=%d getpct=%d  wire=%s\n",
		nconn, nthr, g_depth, g_getpct, g_bin ? "BINARY" : "TEXT");
	printf("  %.0f ops/s over %.1fs   errors=%lld\n",
		el > 0 ? tot / el : 0.0, el, errs);
	if (timed)
		printf("  p50=%.3fms p99=%.3fms p999=%.3fms max=%.3fms "
			"(%lld timed)\n",
			pct(ts[0].hist, timed, 0.50), pct(ts[0].hist, timed, 0.99),
			pct(ts[0].hist, timed, 0.999), ts[0].worst / 1000.0,
			timed);
	else
		printf("  latency NOT sampled (arg 13 = 0)\n");
	printf("  client CPU %.1fs over %.1fs wall = %.1f cores\n",
		cpu, el, el > 0 ? cpu / el : 0.0);
	if (g_getpct > 0 && hits == 0)
		printf("  *** ZERO HITS - every read missed.  The keyspace is\n"
		       "  *** not filled with key:%%012u, so this measured nothing.\n");
	else if (g_getpct > 0)
		printf("  reads returning a %d-byte payload: %lld\n", g_val, hits);
	return errs ? 1 : 0;
}
