/*
 * rpsbench.c - a RESP pub/sub driver: the same client against Redis and
 * perfcached's RESP door, written from the protocol (no Redis code).
 *
 *   rpsbench [-H host] [-p port] [-B sub_host] [-b sub_port] [-S subs]
 *            [-s sub_threads] [-P pub_conns] [-t pub_threads] [-d secs]
 *            [-v valsize] [-r rate/s] [-w window] [-x server_pid] [-G]
 *
 * -B/-b send the subscribers to another node than the publishers (a
 * fleet's relay, a cluster's bus); by default both use -H/-p.  Run it on
 * one host either way, so the latency is read against one clock.
 *
 * Subscribers SUBSCRIBE to "bench" first; publishers start only when every
 * subscription is confirmed.  Each payload carries [send ns][publisher
 * id][seq], so every subscriber measures latency (one clock read per
 * read() batch, so a batch's messages share its arrival time) and counts
 * per-publisher gaps and reorders.  -r 0 is a closed loop with <window>
 * PUBLISHes in flight per connection; -r N paces to N/s in total.  One
 * second of warm-up, then <secs> measured, then a drain: publishers settle
 * their in-flight, subscribers catch up.
 *
 * FAN-OUT INVERTS REQUEST/RESPONSE: one publish is N deliveries, so the
 * driver saturates first.  It reports its own CPU (drv_cores) and its
 * busiest thread (busiest_thr): near 1.00 means that cell measured the
 * client.  With -x it reads the server process's CPU and context switches
 * over the same window.
 *
 * One RESULT line: pub_s (publishes acknowledged/s), deliv_s (messages
 * read/s), p50/p99/p999/max latency in µs, acked, expected (acked x subs),
 * got, lost (expected - got after the drain), recv_ok (the receiver
 * counts PUBLISH answered sum to expected), gaps, ooo (reordered), eof
 * (subscribers the server closed), bad, errs (error replies), drain_s,
 * srv_cores, srv_ctxsw_s, drv_cores, busiest_thr.
 *
 * -G publishes a sequence gap every 1000 messages: the gap counter's
 * fail check (4 publishers x 4 subscribers x 37 = 592 over 150k).
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define HB 1216
#define RBUF (256 * 1024)
#define OBUF (256 * 1024)
#define HDR 20

static const char *g_host = "127.0.0.1", *g_sub_host;
static int g_port = 6379, g_sub_port, g_subs = 1, g_sthr = 1, g_pubs = 4, g_pthr = 1;
static int g_secs = 8, g_val = 64, g_win = 16, g_gapfault;
static double g_rate;
static long g_srvpid;
static atomic_int g_phase, g_subscribed;   /* 0 setup 1 warm 2 measure 3 stop 4 done */
static atomic_ullong g_pace0;
static char *g_cmd;
static size_t g_cmdlen, g_payoff;

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int hidx(uint64_t us)
{
	if (us < 64)
		return (int)us;
	int msb = 63 - __builtin_clzll(us);
	int i = 64 + (msb - 6) * 32 + (int)((us >> (msb - 5)) & 31);
	return i < HB ? i : HB - 1;
}

static uint64_t hval(int i)
{
	if (i < 64)
		return (uint64_t)i;
	int msb = (i - 64) / 32 + 6, sub = (i - 64) % 32;
	return (1ull << msb) | ((uint64_t)sub << (msb - 5));
}

static int dial(const char *host, int port)
{
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
	if (inet_pton(AF_INET, host, &a.sin_addr) != 1) {
		fprintf(stderr, "bad host %s\n", host);
		exit(2);
	}
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
		perror("connect");
		exit(2);
	}
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	return fd;
}

static double cpu_s(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	char b[2048];
	size_t n = fread(b, 1, sizeof b - 1, f);
	fclose(f);
	b[n] = 0;
	char *p = strrchr(b, ')');
	unsigned long ut, st;
	if (!p || sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) != 2)
		return -1;
	return (double)(ut + st) / (double)sysconf(_SC_CLK_TCK);
}

/* voluntary + involuntary context switches summed over every thread of pid */
static double ctxsw(long pid)
{
	char p[300], line[256];
	double sum = 0;
	snprintf(p, sizeof p, "/proc/%ld/task", pid);
	DIR *d = opendir(p);
	if (!d)
		return -1;
	struct dirent *de;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		snprintf(p, sizeof p, "/proc/%ld/task/%s/status", pid, de->d_name);
		FILE *f = fopen(p, "r");
		if (!f)
			continue;
		while (fgets(line, sizeof line, f)) {
			unsigned long v;
			if (sscanf(line, "voluntary_ctxt_switches: %lu", &v) == 1 ||
			    sscanf(line, "nonvoluntary_ctxt_switches: %lu", &v) == 1)
				sum += (double)v;
		}
		fclose(f);
	}
	closedir(d);
	return sum;
}

static double thr_cpu(long tid)
{
	char p[64];
	snprintf(p, sizeof p, "/proc/self/task/%ld/stat", tid);
	return cpu_s(p);
}

/* ---- subscribers ---- */

struct sconn {
	int fd, dead;
	size_t len;
	char *buf;
	uint64_t *next;            /* per publisher id: next expected seq */
};

struct sthr {
	pthread_t th;
	int n;
	atomic_long tid;
	struct sconn *c;
	atomic_ullong got, gaps, ooo, eof, bad;
	uint64_t hist[HB];
};

/* $len\r\n<bytes>\r\n : 1 complete, 0 need more, -1 protocol error */
static int bulk(const char *p, const char *e, const char **s, long *n, const char **next)
{
	if (p >= e)
		return 0;
	if (*p != '$')
		return -1;
	const char *q = memchr(p, '\r', (size_t)(e - p));
	if (!q || q + 1 >= e)
		return 0;
	long len = strtol(p + 1, NULL, 10);
	const char *d = q + 2;
	if (len < 0)
		return -1;
	if (d + len + 2 > e)
		return 0;
	*s = d;
	*n = len;
	*next = d + len + 2;
	return 1;
}

static int sub_parse(struct sthr *T, struct sconn *c, int measuring, uint64_t t)
{
	const char *p = c->buf, *e = c->buf + c->len;
	uint64_t got = 0;
	for (;;) {
		const char *f = p, *k, *ch, *pl, *q;
		long kn, chn, pln;
		int r;
		if (f >= e)
			break;
		if (*f != '*')
			return -1;
		q = memchr(f, '\r', (size_t)(e - f));
		if (!q || q + 1 >= e)
			break;
		if (strtol(f + 1, NULL, 10) != 3)
			return -1;
		f = q + 2;
		if ((r = bulk(f, e, &k, &kn, &f)) <= 0) {
			if (r < 0)
				return -1;
			break;
		}
		if ((r = bulk(f, e, &ch, &chn, &f)) <= 0) {
			if (r < 0)
				return -1;
			break;
		}
		if (f >= e)
			break;
		if (*f == ':') {
			q = memchr(f, '\r', (size_t)(e - f));
			if (!q || q + 1 >= e)
				break;
			f = q + 2;
			if (kn == 9 && !memcmp(k, "subscribe", 9))
				atomic_fetch_add(&g_subscribed, 1);
		} else {
			if ((r = bulk(f, e, &pl, &pln, &f)) <= 0) {
				if (r < 0)
					return -1;
				break;
			}
			if (kn != 7 || memcmp(k, "message", 7) || pln < HDR) {
				atomic_fetch_add(&T->bad, 1);
			} else {
				uint64_t ts, seq;
				uint32_t id;
				memcpy(&ts, pl, 8);
				memcpy(&id, pl + 8, 4);
				memcpy(&seq, pl + 12, 8);
				got++;
				if (measuring)
					T->hist[hidx(t > ts ? (t - ts) / 1000 : 0)]++;
				if (id >= (uint32_t)g_pubs) {
					atomic_fetch_add(&T->bad, 1);
				} else if (seq == c->next[id]) {
					c->next[id] = seq + 1;
				} else if (seq > c->next[id]) {
					atomic_fetch_add(&T->gaps, seq - c->next[id]);
					c->next[id] = seq + 1;
				} else {
					atomic_fetch_add(&T->ooo, 1);
				}
			}
		}
		p = f;
	}
	if (got)
		atomic_fetch_add(&T->got, got);
	size_t used = (size_t)(p - c->buf);
	if (used) {
		memmove(c->buf, p, c->len - used);
		c->len -= used;
	}
	return 0;
}

static void *sub_main(void *arg)
{
	struct sthr *T = arg;
	atomic_store(&T->tid, (long)syscall(SYS_gettid));
	int ep = epoll_create1(0);
	char cmd[128];
	int cl = snprintf(cmd, sizeof cmd, "*2\r\n$9\r\nSUBSCRIBE\r\n$5\r\nbench\r\n");
	for (int i = 0; i < T->n; i++) {
		struct sconn *c = &T->c[i];
		c->fd = dial(g_sub_host, g_sub_port);
		c->buf = malloc(RBUF);
		c->next = calloc((size_t)g_pubs, sizeof *c->next);
		if (write(c->fd, cmd, (size_t)cl) != cl) {
			perror("subscribe write");
			exit(2);
		}
		struct epoll_event ev = { .events = EPOLLIN, .data.ptr = c };
		epoll_ctl(ep, EPOLL_CTL_ADD, c->fd, &ev);
	}
	struct epoll_event evs[256];
	while (atomic_load(&g_phase) < 4) {
		int n = epoll_wait(ep, evs, 256, 10);
		int measuring = atomic_load(&g_phase) == 2;
		for (int i = 0; i < n; i++) {
			struct sconn *c = evs[i].data.ptr;
			for (int rounds = 0; rounds < 8 && !c->dead; rounds++) {
				ssize_t r = read(c->fd, c->buf + c->len, RBUF - c->len);
				if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) {
					atomic_fetch_add(&T->eof, 1);
					epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, NULL);
					close(c->fd);
					c->dead = 1;
					break;
				}
				if (r < 0)
					break;
				c->len += (size_t)r;
				if (sub_parse(T, c, measuring, now_ns()) < 0) {
					fprintf(stderr, "subscriber protocol error\n");
					exit(3);
				}
				if (c->len == RBUF) {
					fprintf(stderr, "frame larger than buffer\n");
					exit(3);
				}
				if ((size_t)r < RBUF / 2)
					break;
			}
		}
	}
	return NULL;
}

/* ---- publishers ---- */

struct pconn {
	int fd, id;
	uint64_t seq, sent, inflight;
	size_t ilen, olen, ooff;
	char in[8192];
	char *out;
};

struct pthr {
	pthread_t th;
	int n;
	atomic_long tid;
	struct pconn *c;
	atomic_ullong acked, recv, errs;
	atomic_int idle;
};

static void fill(struct pconn *c, uint64_t t, double per_rate)
{
	uint64_t cap = (uint64_t)g_win > c->inflight ? (uint64_t)g_win - c->inflight : 0;
	if (per_rate > 0) {
		uint64_t due = (uint64_t)((double)(t - atomic_load(&g_pace0)) / 1e9 * per_rate);
		uint64_t d = due > c->sent ? due - c->sent : 0;
		if (d < cap)
			cap = d;
	}
	if (!cap)
		return;
	if (c->ooff) {
		memmove(c->out, c->out + c->ooff, c->olen - c->ooff);
		c->olen -= c->ooff;
		c->ooff = 0;
	}
	while (cap-- && c->olen + g_cmdlen <= OBUF) {
		char *m = c->out + c->olen;
		uint32_t id = (uint32_t)c->id;
		memcpy(m, g_cmd, g_cmdlen);
		memcpy(m + g_payoff, &t, 8);
		memcpy(m + g_payoff + 8, &id, 4);
		memcpy(m + g_payoff + 12, &c->seq, 8);
		c->olen += g_cmdlen;
		c->seq += (g_gapfault && c->sent % 1000 == 999) ? 2 : 1;
		c->sent++;
		c->inflight++;
	}
}

static void *pub_main(void *arg)
{
	struct pthr *T = arg;
	atomic_store(&T->tid, (long)syscall(SYS_gettid));
	int ep = epoll_create1(0);
	double per_rate = g_rate > 0 ? g_rate / g_pubs : 0;
	for (int i = 0; i < T->n; i++) {
		struct pconn *c = &T->c[i];
		c->fd = dial(g_host, g_port);
		c->out = malloc(OBUF);
		struct epoll_event ev = { .events = EPOLLIN, .data.ptr = c };
		epoll_ctl(ep, EPOLL_CTL_ADD, c->fd, &ev);
	}
	struct epoll_event evs[64];
	while (atomic_load(&g_phase) < 1)
		usleep(1000);
	for (;;) {
		int ph = atomic_load(&g_phase);
		if (ph >= 4)
			break;
		int n = epoll_wait(ep, evs, 64, 1);
		uint64_t acked = 0, recv = 0, errs = 0;
		for (int i = 0; i < n; i++) {
			struct pconn *c = evs[i].data.ptr;
			ssize_t r = read(c->fd, c->in + c->ilen, sizeof c->in - c->ilen);
			if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) {
				fprintf(stderr, "publisher connection closed\n");
				exit(3);
			}
			if (r < 0)
				continue;
			c->ilen += (size_t)r;
			char *p = c->in, *e = c->in + c->ilen;
			for (;;) {
				char *q = memchr(p, '\n', (size_t)(e - p));
				if (!q)
					break;
				if (*p == ':')
					recv += strtoull(p + 1, NULL, 10), acked++;
				else
					errs++;
				c->inflight--;
				p = q + 1;
			}
			memmove(c->in, p, (size_t)(e - p));
			c->ilen = (size_t)(e - p);
		}
		if (acked)
			atomic_fetch_add(&T->acked, acked);
		if (recv)
			atomic_fetch_add(&T->recv, recv);
		if (errs)
			atomic_fetch_add(&T->errs, errs);
		uint64_t t = now_ns();
		int busy = 0;
		for (int i = 0; i < T->n; i++) {
			struct pconn *c = &T->c[i];
			if (ph <= 2)
				fill(c, t, per_rate);
			if (c->olen > c->ooff) {
				ssize_t w = write(c->fd, c->out + c->ooff, c->olen - c->ooff);
				if (w > 0)
					c->ooff += (size_t)w;
				else if (w < 0 && errno != EAGAIN && errno != EINTR) {
					perror("publish write");
					exit(3);
				}
			}
			if (c->inflight)
				busy = 1;
		}
		atomic_store(&T->idle, ph == 3 && !busy);
	}
	return NULL;
}

struct snap {
	uint64_t t, acked, got;
	double drv, srv, cs, *thr;
};

static struct sthr *S;
static struct pthr *P;

static void take(struct snap *s)
{
	s->t = now_ns();
	s->acked = s->got = 0;
	for (int i = 0; i < g_pthr; i++)
		s->acked += atomic_load(&P[i].acked);
	for (int i = 0; i < g_sthr; i++)
		s->got += atomic_load(&S[i].got);
	s->drv = cpu_s("/proc/self/stat");
	char p[64];
	snprintf(p, sizeof p, "/proc/%ld/stat", g_srvpid);
	s->srv = g_srvpid ? cpu_s(p) : -1;
	s->cs = g_srvpid ? ctxsw(g_srvpid) : -1;
	for (int i = 0; i < g_sthr; i++)
		s->thr[i] = thr_cpu(atomic_load(&S[i].tid));
	for (int i = 0; i < g_pthr; i++)
		s->thr[g_sthr + i] = thr_cpu(atomic_load(&P[i].tid));
}

int main(int argc, char **argv)
{
	int o;
	while ((o = getopt(argc, argv, "H:p:B:b:S:s:P:t:d:v:r:w:x:G")) != -1) {
		switch (o) {
		case 'H': g_host = optarg; break;
		case 'p': g_port = atoi(optarg); break;
		case 'B': g_sub_host = optarg; break;
		case 'b': g_sub_port = atoi(optarg); break;
		case 'S': g_subs = atoi(optarg); break;
		case 's': g_sthr = atoi(optarg); break;
		case 'P': g_pubs = atoi(optarg); break;
		case 't': g_pthr = atoi(optarg); break;
		case 'd': g_secs = atoi(optarg); break;
		case 'v': g_val = atoi(optarg); break;
		case 'r': g_rate = atof(optarg); break;
		case 'w': g_win = atoi(optarg); break;
		case 'x': g_srvpid = atol(optarg); break;
		case 'G': g_gapfault = 1; break;
		default: return 2;
		}
	}
	if (!g_sub_host)
		g_sub_host = g_host;
	if (!g_sub_port)
		g_sub_port = g_port;
	if (g_val < HDR || g_sthr > g_subs || g_pthr > g_pubs || g_sthr < 1 || g_pthr < 1) {
		fprintf(stderr, "bad arguments\n");
		return 2;
	}
	char hdr[128];
	int hl = snprintf(hdr, sizeof hdr, "*3\r\n$7\r\nPUBLISH\r\n$5\r\nbench\r\n$%d\r\n", g_val);
	g_payoff = (size_t)hl;
	g_cmdlen = (size_t)hl + (size_t)g_val + 2;
	g_cmd = malloc(g_cmdlen);
	memcpy(g_cmd, hdr, (size_t)hl);
	memset(g_cmd + hl, 'x', (size_t)g_val);
	memcpy(g_cmd + hl + g_val, "\r\n", 2);

	S = calloc((size_t)g_sthr, sizeof *S);
	P = calloc((size_t)g_pthr, sizeof *P);
	for (int i = 0; i < g_sthr; i++) {
		S[i].n = g_subs / g_sthr + (i < g_subs % g_sthr);
		S[i].c = calloc((size_t)S[i].n, sizeof *S[i].c);
		pthread_create(&S[i].th, NULL, sub_main, &S[i]);
	}
	uint64_t t0 = now_ns();
	while (atomic_load(&g_subscribed) < g_subs) {
		if (now_ns() - t0 > 10000000000ull) {
			fprintf(stderr, "only %d of %d subscriptions confirmed\n", atomic_load(&g_subscribed), g_subs);
			return 4;
		}
		usleep(1000);
	}
	for (int i = 0, id = 0; i < g_pthr; i++) {
		P[i].n = g_pubs / g_pthr + (i < g_pubs % g_pthr);
		P[i].c = calloc((size_t)P[i].n, sizeof *P[i].c);
		for (int j = 0; j < P[i].n; j++)
			P[i].c[j].id = id++;
		pthread_create(&P[i].th, NULL, pub_main, &P[i]);
	}
	usleep(200000);            /* publishers connect */
	struct snap a = { .thr = calloc((size_t)(g_sthr + g_pthr), sizeof(double)) };
	struct snap b = { .thr = calloc((size_t)(g_sthr + g_pthr), sizeof(double)) };
	atomic_store(&g_pace0, now_ns());
	atomic_store(&g_phase, 1);
	usleep(1000000);
	atomic_store(&g_phase, 2);
	take(&a);
	usleep((useconds_t)g_secs * 1000000u);
	take(&b);
	atomic_store(&g_phase, 3);

	/* drain: publishers settle their in-flight, subscribers catch up */
	t0 = now_ns();
	for (;;) {
		int idle = 1;
		for (int i = 0; i < g_pthr; i++)
			idle &= atomic_load(&P[i].idle);
		if (idle || now_ns() - t0 > 5000000000ull)
			break;
		usleep(10000);
	}
	uint64_t acked = 0, recv = 0, errs = 0, got = 0, last = ~0ull, same = now_ns();
	for (int i = 0; i < g_pthr; i++) {
		acked += atomic_load(&P[i].acked);
		recv += atomic_load(&P[i].recv);
		errs += atomic_load(&P[i].errs);
	}
	t0 = now_ns();
	for (;;) {
		got = 0;
		for (int i = 0; i < g_sthr; i++)
			got += atomic_load(&S[i].got);
		if (got >= acked * (uint64_t)g_subs)
			break;
		if (got != last) {
			last = got;
			same = now_ns();
		}
		if (now_ns() - same > 1000000000ull || now_ns() - t0 > 15000000000ull)
			break;
		usleep(20000);
	}
	double drain_s = (double)(now_ns() - t0) / 1e9;
	atomic_store(&g_phase, 4);
	for (int i = 0; i < g_sthr; i++)
		pthread_join(S[i].th, NULL);
	for (int i = 0; i < g_pthr; i++)
		pthread_join(P[i].th, NULL);

	uint64_t gaps = 0, ooo = 0, eof = 0, bad = 0, hist[HB] = { 0 }, hn = 0;
	for (int i = 0; i < g_sthr; i++) {
		gaps += atomic_load(&S[i].gaps);
		ooo += atomic_load(&S[i].ooo);
		eof += atomic_load(&S[i].eof);
		bad += atomic_load(&S[i].bad);
		for (int j = 0; j < HB; j++)
			hist[j] += S[i].hist[j], hn += S[i].hist[j];
	}
	uint64_t pct[4] = { 0 };
	double want[3] = { 0.50, 0.99, 0.999 };
	for (int q = 0; q < 3; q++) {
		uint64_t need = (uint64_t)(want[q] * (double)hn), run = 0;
		for (int j = 0; j < HB; j++) {
			run += hist[j];
			if (run > need) {
				pct[q] = hval(j);
				break;
			}
		}
	}
	for (int j = HB - 1; j >= 0; j--)
		if (hist[j]) {
			pct[3] = hval(j);
			break;
		}
	double secs = (double)(b.t - a.t) / 1e9, busiest = 0;
	for (int i = 0; i < g_sthr + g_pthr; i++) {
		double u = (b.thr[i] - a.thr[i]) / secs;
		if (u > busiest)
			busiest = u;
	}
	uint64_t expect = acked * (uint64_t)g_subs;
	printf("RESULT subs=%d pubs=%d rate=%.0f win=%d val=%d secs=%.2f pub_s=%.0f deliv_s=%.0f "
	       "p50_us=%llu p99_us=%llu p999_us=%llu max_us=%llu acked=%llu expected=%llu got=%llu lost=%lld "
	       "recv_ok=%d gaps=%llu ooo=%llu eof=%llu bad=%llu errs=%llu drain_s=%.2f "
	       "srv_cores=%.2f srv_ctxsw_s=%.0f drv_cores=%.2f busiest_thr=%.2f\n",
	       g_subs, g_pubs, g_rate, g_win, g_val, secs,
	       (double)(b.acked - a.acked) / secs, (double)(b.got - a.got) / secs,
	       (unsigned long long)pct[0], (unsigned long long)pct[1], (unsigned long long)pct[2],
	       (unsigned long long)pct[3], (unsigned long long)acked, (unsigned long long)expect,
	       (unsigned long long)got, (long long)(expect - got), recv == expect,
	       (unsigned long long)gaps, (unsigned long long)ooo, (unsigned long long)eof,
	       (unsigned long long)bad, (unsigned long long)errs, drain_s,
	       b.srv >= 0 ? (b.srv - a.srv) / secs : -1, b.cs >= 0 ? (b.cs - a.cs) / secs : -1,
	       (b.drv - a.drv) / secs, busiest);
	return 0;
}
