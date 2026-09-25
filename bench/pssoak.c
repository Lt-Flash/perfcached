/*
 * pssoak.c - a pub/sub soak driver: publishers, subscribers (exact,
 * pattern, UDP push) and churn over libperfd async handles, each surviving
 * its node's restart by reconnecting and subscribing again, each printing
 * its counters every 10 s so a run can be read as a time series.
 *
 *   pssoak pub   <host> <port> <secret|-> <id> <secs> <rate> <conns>
 *   pssoak sub   <host> <port> <secret|-> <id> <secs> <a|b> <exact|pattern|udp> <conns>
 *   pssoak churn <host> <port> <secret|-> <id> <secs> <ops/s>
 *
 * CHANNELS.  A publisher cycles k = 0..99 and publishes, per step,
 * soak.a.<k>, soak.b.<k>, soak.none.<k % 10> and churn.<k % 50>; its
 * payload is "P<id>.<conn>:<seq>" with seq counted per (publisher
 * connection, channel), so a subscriber can tell a gap from a reorder.
 * Subscribers of node a hold soak.a.*: exact = soak.a.0-9, pattern =
 * soak.a.*, udp = soak.a.10-19 over UDP pushes; gaps are tracked per
 * subscriber connection.  Nobody holds soak.none.*; churn subscribes
 * churn.k and drops churn.k+25 in turn, holding about 25.
 *
 * STATUS LINE, every 10 s and at the end:
 *   T <unix ms> <role> <id> key=value ...
 */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>
#include "perfd.h"

#define MAXC      16
#define NPUB      64                   /* publisher (id * 16 + conn) slots */
#define NCH       100

static const char *g_host, *g_sec;
static int g_port, g_id;
static volatile int g_stop;

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static long long wall_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

struct conn {
	perfd_t *p;
	int idx;
	int state;                         /* 0 down, 1 connecting, 2 subscribing, 3 running */
	long long retry_ms;
	int inflight;
	int pending;                       /* subscribe replies outstanding */
	int udp_asked;
	unsigned long long seq[NCH * 4];   /* pub: per channel slot */
	int k;                             /* pub: the rotation */
	int churn_i;
};

/* counters */
static unsigned long long c_pub_ok, c_pub_err, c_recv, c_gaps, c_reorder,
	c_reconnects, c_prunes, c_udp_fail, c_sub_err, c_churn_ops;
/* UDP figures of handles already dropped: a handle's own start at zero */
static unsigned long long u_recv, u_gaps, u_dups, u_rej;
static long long g_last[MAXC][NPUB][NCH];  /* sub: per connection, publisher slot, channel */
static char g_node;                    /* sub: a | b */
static int g_mode;                     /* sub: 0 exact, 1 pattern, 2 udp */
static int g_role;                     /* 0 pub, 1 sub, 2 churn */
static struct conn g_c[MAXC];
static int g_n;

static void on_notify(const char *json, size_t len, void *ctx)
{
	char buf[1024];
	size_t n = len < sizeof buf - 1 ? len : sizeof buf - 1;
	const char *ch, *pl;
	int chn, pid, pconn;
	long long seq;
	char node;

	memcpy(buf, json, n);
	buf[n] = 0;
	if (strstr(buf, "\"method\":\"pubsub_udp_pruned\"")) {
		c_prunes++;
		return;
	}
	if (g_role != 1 || (!strstr(buf, "\"method\":\"message\"") &&
	        !strstr(buf, "\"method\":\"pmessage\"")))
		return;
	c_recv++;
	ch = strstr(buf, "\"channel\":\"soak.");
	pl = strstr(buf, "\"payload\":\"P");
	if (!ch || !pl || sscanf(ch + 16, "%c.%d", &node, &chn) != 2 ||
	    sscanf(pl + 12, "%d.%d:%lld", &pid, &pconn, &seq) != 3 ||
	    node != g_node || chn < 0 || chn >= NCH || pid < 0 ||
	    pid * 16 + pconn >= NPUB || pconn < 0 || pconn >= 16)
		return;
	{
		long long *last = &g_last[((struct conn *)ctx)->idx][pid * 16 + pconn][chn];

		if (*last && seq > *last + 1)
			c_gaps += (unsigned long long)(seq - *last - 1);
		if (*last && seq <= *last)
			c_reorder++;
		if (seq > *last)
			*last = seq;
	}
}

static void on_pub(char *result, size_t len, const char *errmsg, void *ctx)
{
	struct conn *c = ctx;

	(void)len;
	c->inflight--;
	if (result && !errmsg)
		c_pub_ok++;
	else
		c_pub_err++;
	free(result);
}

static void on_sub(char *result, size_t len, const char *errmsg, void *ctx)
{
	struct conn *c = ctx;

	(void)len;
	if (!result || errmsg)
		c_sub_err++;
	free(result);
	if (c->pending > 0)
		c->pending--;
}

static void on_churn(char *result, size_t len, const char *errmsg, void *ctx)
{
	struct conn *c = ctx;

	(void)len;
	(void)errmsg;
	free(result);
	c->inflight--;
	c_churn_ops++;
}

static void drop(struct conn *c, long long now)
{
	struct perfd_udp_stats us;

	if (c->p && perfd_pubsub_udp_stats(c->p, &us) == 0) {
		u_recv += us.received;
		u_gaps += us.gaps;
		u_dups += us.duplicates;
		u_rej += us.rejected;
	}
	if (c->p)
		perfd_free(c->p);
	c->p = NULL;
	c->state = 0;
	c->retry_ms = now + 1000;
	c->inflight = 0;
	c->pending = 0;
	c->udp_asked = 0;
}

static void start(struct conn *c)
{
	perfd_opts o;
	const char *sec[2] = { g_sec, NULL };

	memset(&o, 0, sizeof o);
	o.secrets = strcmp(g_sec, "-") ? sec : NULL;
	c->p = perfd_connect_async(g_host, g_port, &o);
	c->state = c->p ? 1 : 0;
	if (c->p)
		perfd_set_notify(c->p, on_notify, c);
}

static void subscribe_all(struct conn *c)
{
	char params[128];
	int i;

	if (g_mode == 1) {
		snprintf(params, sizeof params, "{\"pattern\":\"soak.%c.*\"}", g_node);
		if (perfd_submit(c->p, "psubscribe", params, on_sub, c) == 0)
			c->pending++;
		return;
	}
	for (i = 0; i < 10; i++) {
		snprintf(params, sizeof params, "{\"channel\":\"soak.%c.%d\"}", g_node,
			g_mode == 2 ? 10 + i : i);
		if (perfd_submit(c->p, "subscribe", params, on_sub, c) == 0)
			c->pending++;
	}
}

static void step(struct conn *c, long long now, double *next, double per_ms)
{
	int st;

	if (!c->p) {
		if (now >= c->retry_ms) {
			if (c->retry_ms)
				c_reconnects++;
			start(c);
		}
		return;
	}
	st = perfd_state(c->p);
	if (st == PERFD_ST_FAILED) {
		drop(c, now);
		return;
	}
	if (st != PERFD_ST_READY)
		return;
	if (c->state == 1) {
		c->state = g_role == 1 ? 2 : 3;
		if (g_role == 1)
			subscribe_all(c);
	}
	if (g_role == 1) {
		struct perfd_udp_stats us;

		if (c->state == 2 && !c->pending) {
			if (g_mode == 2 && !c->udp_asked) {
				if (perfd_pubsub_udp(c->p, 0, 3000) != 0)
					c_udp_fail++;
				c->udp_asked = 1;
			}
			c->state = 3;
		}
		if (g_mode == 2 && c->udp_asked) {
			if (perfd_pubsub_udp_ready(c->p) < 0 ||
			    (perfd_pubsub_udp_stats(c->p, &us) == 0 && us.pruned)) {
				/* a pruned or failed stream: start over, as an
				 * application would after being told */
				drop(c, now);
				return;
			}
		}
		return;
	}
	if (g_role == 2) {
		char params[64];

		while (c->inflight < 4 && *next <= (double)now) {
			int n = c->churn_i++;

			/* subscribe churn.k, then drop churn.k+25: about 25 held,
			 * every one of them joined and left in turn */
			snprintf(params, sizeof params, "{\"channel\":\"churn.%d\"}",
				n % 2 ? (n / 2 + 25) % 50 : (n / 2) % 50);
			if (perfd_submit(c->p, n % 2 ? "unsubscribe" : "subscribe", params,
			        on_churn, c) == 0)
				c->inflight++;
			*next += per_ms;
		}
		return;
	}
	/* publisher */
	while (c->inflight < 16 && *next <= (double)now) {
		static const char *fam[4] = { "soak.a.%d", "soak.b.%d", "soak.none.%d", "churn.%d" };
		int f;

		for (f = 0; f < 4; f++) {
			char chan[32], params[160];
			int ci = f == 0 || f == 1 ? c->k : f == 2 ? c->k % 10 : c->k % 50;
			int slot = f * NCH + ci;

			snprintf(chan, sizeof chan, fam[f], ci);
			snprintf(params, sizeof params,
				"{\"channel\":\"%s\",\"payload\":\"P%d.%d:%llu\"}", chan, g_id,
				c->idx, ++c->seq[slot]);
			if (perfd_submit(c->p, "publish", params, on_pub, c) == 0)
				c->inflight++;
			else
				c_pub_err++;
		}
		c->k = (c->k + 1) % NCH;
		*next += per_ms * 4;
	}
}

static void status(const char *role)
{
	unsigned long long ur = u_recv, ug = u_gaps, ud = u_dups, uj = u_rej;
	int i, active = 0, up = 0;

	for (i = 0; i < g_n; i++) {
		struct perfd_udp_stats us;

		if (!g_c[i].p)
			continue;
		up += perfd_state(g_c[i].p) == PERFD_ST_READY;
		if (perfd_pubsub_udp_stats(g_c[i].p, &us) == 0) {
			active += us.active;
			ur += us.received;
			ug += us.gaps;
			ud += us.duplicates;
			uj += us.rejected;
		}
	}
	printf("T %lld %s %d up=%d pub_ok=%llu pub_err=%llu recv=%llu gaps=%llu "
		"reorder=%llu reconnects=%llu prunes=%llu udp_fail=%llu sub_err=%llu "
		"churn=%llu udp_active=%d udp_recv=%llu udp_gaps=%llu udp_dups=%llu "
		"udp_rej=%llu\n", wall_ms(), role, g_id, up, c_pub_ok, c_pub_err, c_recv,
		c_gaps, c_reorder, c_reconnects, c_prunes, c_udp_fail, c_sub_err,
		c_churn_ops, active, ur, ug, ud, uj);
	fflush(stdout);
}

static void on_signal(int s)
{
	(void)s;
	g_stop = 1;
}

int main(int argc, char **argv)
{
	int secs, rate = 0, i, ep;
	long long end, next_status;
	double per_ms = 0;
	const char *role;

	if (argc < 7) {
		fprintf(stderr, "usage: see bench/pssoak.c\n");
		return 2;
	}
	role = argv[1];
	g_host = argv[2];
	g_port = atoi(argv[3]);
	g_sec = argv[4];
	g_id = atoi(argv[5]);
	secs = atoi(argv[6]);
	if (!strcmp(role, "pub") && argc >= 9) {
		g_role = 0;
		rate = atoi(argv[7]);
		g_n = atoi(argv[8]);
	} else if (!strcmp(role, "sub") && argc >= 10) {
		g_role = 1;
		g_node = argv[7][0];
		g_mode = !strcmp(argv[8], "pattern") ? 1 : !strcmp(argv[8], "udp") ? 2 : 0;
		g_n = atoi(argv[9]);
	} else if (!strcmp(role, "churn") && argc >= 8) {
		g_role = 2;
		rate = atoi(argv[7]);
		g_n = 1;
	} else {
		fprintf(stderr, "usage: see bench/pssoak.c\n");
		return 2;
	}
	if (g_n < 1 || g_n > MAXC)
		return 2;
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGPIPE, SIG_IGN);
	setvbuf(stdout, NULL, _IOLBF, 0);
	for (i = 0; i < g_n; i++) {
		g_c[i].idx = i;
		start(&g_c[i]);
	}
	if (rate > 0)
		per_ms = 1000.0 * g_n / rate;   /* each connection paces its share */
	ep = epoll_create1(0);
	end = now_ms() + (long long)secs * 1000;
	next_status = now_ms() + 10000;
	{
		double nexts[MAXC];

		for (i = 0; i < g_n; i++)
			nexts[i] = (double)now_ms();
		while (!g_stop && now_ms() < end) {
			struct pollfd pf[2 * MAXC];
			int np = 0, map[2 * MAXC];
			long long now;

			for (i = 0; i < g_n; i++) {
				struct conn *c = &g_c[i];
				int ev;

				if (!c->p || perfd_fd(c->p) < 0)
					continue;
				ev = perfd_events(c->p);
				pf[np].fd = perfd_fd(c->p);
				pf[np].events = (short)((ev & PERFD_EV_READ ? POLLIN : 0) |
					(ev & PERFD_EV_WRITE ? POLLOUT : 0));
				map[np++] = i;
				if (perfd_pubsub_udp_fd(c->p) >= 0) {
					pf[np].fd = perfd_pubsub_udp_fd(c->p);
					pf[np].events = POLLIN;
					map[np++] = i;
				}
			}
			poll(pf, (nfds_t)np, 5);
			for (i = 0; i < np; i++) {
				struct conn *c = &g_c[map[i]];

				if (!c->p || pf[i].fd != perfd_fd(c->p))
					continue;
				if (pf[i].revents & POLLOUT)
					perfd_write_ready(c->p);
				if (c->p && (pf[i].revents & (POLLIN | POLLHUP | POLLERR)) &&
				    perfd_read_ready(c->p) < 0)
					drop(c, now_ms());
			}
			now = now_ms();
			for (i = 0; i < g_n; i++) {
				if (nexts[i] < (double)now - 50)
					nexts[i] = (double)now - 50;   /* no catch-up bursts */
				step(&g_c[i], now, &nexts[i], per_ms);
				if (g_c[i].p)
					perfd_push(g_c[i].p);
			}
			if (now >= next_status) {
				status(role);
				next_status += 10000;
			}
		}
	}
	status(role);
	for (i = 0; i < g_n; i++)
		if (g_c[i].p)
			perfd_free(g_c[i].p);
	close(ep);
	return 0;
}
