/* SPDX-License-Identifier: GPL-2.0-or-later */
/* apooltest.c - the async pool against a stand-in server in this process (plaintext, a thread):
 * ready, wait, cancel and the late reply, a failed or closed handle rebuilt, a full pool, fork,
 * a dial bounded for the caller, and by dial_wait_ms. */
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include "../lib/perfd.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* ---- the stand-in: GET k* found ("v:" + key), miss* absent, hold* held until released,
 * close* closes the link, err* an error frame ---- */

#define MAXC 64
struct conn { int fd; unsigned char in[65536]; size_t n; };
struct held { int fd; unsigned long long id; char key[64]; };

static int lfd, wake[2];
static int accepts, drops_done, releases_done;
static int want_drop, want_release, want_stop;
static struct conn conns[MAXC];
static struct held held[64];
static int nheld;

static long long ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void le32(unsigned char *b, unsigned int v)
{
	b[0] = (unsigned char)v; b[1] = (unsigned char)(v >> 8);
	b[2] = (unsigned char)(v >> 16); b[3] = (unsigned char)(v >> 24);
}

static void reply(int fd, unsigned long long id, int err, const void *body, size_t n)
{
	unsigned char f[256];
	int i;

	f[0] = 0x9E; f[1] = 1; f[2] = 2; f[3] = err ? 1 : 0;
	le32(f + 4, (unsigned int)n);
	for (i = 0; i < 8; i++)
		f[8 + i] = (unsigned char)(id >> (8 * i));
	memcpy(f + 16, body, n);
	if (send(fd, f, 16 + n, MSG_NOSIGNAL) < 0)
		return;                        /* a link the client dropped */
}

static void found(int fd, unsigned long long id, const char *key)
{
	unsigned char b[128];
	size_t kn = strlen(key);

	b[0] = 1;
	le32(b + 1, 0xFFFFFFFFu);
	memcpy(b + 5, "v:", 2);
	memcpy(b + 7, key, kn);
	reply(fd, id, 0, b, 7 + kn);
}

static void cclose(struct conn *c)
{
	int i;

	for (i = 0; i < nheld; i++)            /* a later link may reuse the fd number */
		if (held[i].fd == c->fd)
			held[i--] = held[--nheld];
	close(c->fd);
	c->fd = -1;
	c->n = 0;
}

static void serve(struct conn *c)
{
	while (c->n >= 16) {
		unsigned int plen = c->in[4] | c->in[5] << 8 | c->in[6] << 16 | (unsigned)c->in[7] << 24;
		unsigned long long id = 0;
		unsigned char *b = c->in + 16;
		char key[64];
		size_t cn, kn;
		int i;

		if (c->n < 16 + plen)
			return;
		for (i = 0; i < 8; i++)
			id |= (unsigned long long)c->in[8 + i] << (8 * i);
		cn = b[1];
		kn = b[2] | b[3] << 8;
		snprintf(key, sizeof key, "%.*s", (int)kn, (char *)b + 4 + cn);
		memmove(c->in, c->in + 16 + plen, c->n - 16 - plen);
		c->n -= 16 + plen;
		if (!strncmp(key, "miss", 4)) {
			unsigned char z = 0;

			reply(c->fd, id, 0, &z, 1);
		} else if (!strncmp(key, "hold", 4) && nheld < 64) {
			held[nheld].fd = c->fd;
			held[nheld].id = id;
			snprintf(held[nheld].key, sizeof held[nheld].key, "%s", key);
			nheld++;
		} else if (!strncmp(key, "close", 5)) {
			cclose(c);
			return;
		} else if (!strncmp(key, "err", 3))
			reply(c->fd, id, 1, "boom", 4);
		else
			found(c->fd, id, key);
	}
}

static void *server(void *arg)
{
	(void)arg;
	for (;;) {
		struct pollfd pf[MAXC + 2];
		int idx[MAXC + 2], n = 0, i;

		if (__atomic_load_n(&want_stop, __ATOMIC_ACQUIRE))
			return NULL;
		if (__atomic_load_n(&want_drop, __ATOMIC_ACQUIRE)) {
			for (i = 0; i < MAXC; i++)
				if (conns[i].fd >= 0)
					cclose(&conns[i]);
			nheld = 0;
			__atomic_store_n(&want_drop, 0, __ATOMIC_RELEASE);
			__atomic_add_fetch(&drops_done, 1, __ATOMIC_ACQ_REL);
		}
		if (__atomic_load_n(&want_release, __ATOMIC_ACQUIRE)) {
			for (i = 0; i < nheld; i++)
				found(held[i].fd, held[i].id, held[i].key);
			nheld = 0;
			__atomic_store_n(&want_release, 0, __ATOMIC_RELEASE);
			__atomic_add_fetch(&releases_done, 1, __ATOMIC_ACQ_REL);
		}
		pf[n].fd = lfd; pf[n].events = POLLIN; idx[n++] = -1;
		pf[n].fd = wake[0]; pf[n].events = POLLIN; idx[n++] = -2;
		for (i = 0; i < MAXC; i++)
			if (conns[i].fd >= 0) {
				pf[n].fd = conns[i].fd;
				pf[n].events = POLLIN;
				idx[n++] = i;
			}
		if (poll(pf, (nfds_t)n, 100) <= 0)
			continue;
		for (i = 0; i < n; i++) {
			if (!pf[i].revents)
				continue;
			if (idx[i] == -1) {
				int fd = accept(lfd, NULL, NULL), j;

				if (fd < 0)
					continue;
				for (j = 0; j < MAXC && conns[j].fd >= 0; j++)
					;
				if (j == MAXC) {
					close(fd);
					continue;
				}
				conns[j].fd = fd;
				conns[j].n = 0;
				__atomic_add_fetch(&accepts, 1, __ATOMIC_ACQ_REL);
			} else if (idx[i] == -2) {
				char x[64];

				if (read(wake[0], x, sizeof x) < 0)
					continue;
			} else {
				struct conn *c = &conns[idx[i]];
				ssize_t r = read(c->fd, c->in + c->n, sizeof c->in - c->n);

				if (r <= 0) {
					cclose(c);
					continue;
				}
				c->n += (size_t)r;
				serve(c);
			}
		}
	}
}

static void poke(int *flag, int *done)
{
	int before = __atomic_load_n(done, __ATOMIC_ACQUIRE);
	long long t0 = ms();

	__atomic_store_n(flag, 1, __ATOMIC_RELEASE);
	if (write(wake[1], "x", 1) < 0)
		return;
	while (__atomic_load_n(done, __ATOMIC_ACQUIRE) == before && ms() - t0 < 3000)
		usleep(1000);
}

static int listen_on(int *port)
{
	struct sockaddr_in a;
	socklen_t al = sizeof a;
	int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;

	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 64) != 0 ||
	    getsockname(fd, (struct sockaddr *)&a, &al) != 0)
		return -1;
	*port = ntohs(a.sin_port);
	return fd;
}

/* ---- the client side ---- */

struct got { int calls; int failed; char val[128]; size_t len; };

static void cb(char *result, size_t len, const char *errmsg, void *ctx)
{
	struct got *g = ctx;

	g->calls++;
	g->len = 0;
	g->val[0] = 0;
	if (!result) {
		g->failed = 1;
		snprintf(g->val, sizeof g->val, "%s", errmsg ? errmsg : "?");
	} else if (len >= 5 && result[0] == 1) {
		g->len = len - 5;
		snprintf(g->val, sizeof g->val, "%.*s", (int)(len - 5), result + 5);
	} else
		snprintf(g->val, sizeof g->val, "(absent)");
	perfd_free_value(result);
}

/* poll @fd and call ready until the callback ran; 1 ran, 0 not within @wait_ms */
static int drive(perfd_apool *pool, int fd, int wait_ms)
{
	long long until = ms() + wait_ms;

	while (ms() < until) {
		struct pollfd pf = { fd, POLLIN, 0 };
		int r;

		if (poll(&pf, 1, 50) <= 0)
			continue;
		r = perfd_apool_ready(pool, fd);
		if (r != 0)
			return r == 1;
	}
	return 0;
}

static struct perfd_apool_stats stats(perfd_apool *pool)
{
	struct perfd_apool_stats s;

	memset(&s, 0, sizeof s);
	perfd_apool_stats(pool, &s);
	return s;
}

int main(void)
{
	pthread_t th;
	perfd_opts o = PERFD_OPTS_INIT;
	const char *hosts[1] = { "127.0.0.1" };
	int ports[1], i, fd, fd2;
	perfd_apool *pool;
	struct got g;

	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGPIPE, SIG_IGN);
	for (i = 0; i < MAXC; i++)
		conns[i].fd = -1;
	if ((lfd = listen_on(&ports[0])) < 0 || pipe(wake) != 0 ||
	    pthread_create(&th, NULL, server, NULL) != 0) {
		printf("apooltest: no stand-in server: %s\n", strerror(errno));
		return 1;
	}
	o.connect_timeout_ms = 2000;

	printf("--- A. arguments\n");
	CHK(!perfd_apool_new_seeds(hosts, ports, 0, "c", &o, 4) &&
		perfd_error_kind(NULL) == PERFD_E_ARG, "A1 no seeds is refused as PERFD_E_ARG");
	{
		int bad = 0;

		CHK(!perfd_apool_new_seeds(hosts, &bad, 1, "c", &o, 4), "A2 port 0 is refused");
	}
	CHK(!perfd_apool_new_seeds(hosts, ports, 1, "c", &o, 0), "A3 max_conns 0 is refused");
	CHK(!perfd_apool_new_seeds(hosts, ports, 1, "", &o, 4), "A4 an empty collection is refused");

	pool = perfd_apool_new_seeds(hosts, ports, 1, "c", &o, 2);
	CHK(pool != NULL, "A5 a pool on one seed (%s)", pool ? "" : perfd_error(NULL));
	if (!pool)
		return 1;

	printf("--- B. ready and wait\n");
	memset(&g, 0, sizeof g);
	fd = perfd_apool_get(pool, "k1", cb, &g);
	CHK(fd >= 0, "B1 get k1 answers an fd to watch (%d: %s)", fd, fd < 0 ? perfd_error(NULL) : "");
	CHK(g.calls == 0, "B2 the callback has not run inside the call");
	CHK(drive(pool, fd, 3000) && g.calls == 1 && !strcmp(g.val, "v:k1"),
		"B3 on readiness perfd_apool_ready runs the callback once with the value (%d, '%s')",
		g.calls, g.val);
	CHK(perfd_apool_ready(pool, fd) == -1 && g.calls == 1,
		"B4 a second ready on the answered fd is -1 and runs nothing");
	memset(&g, 0, sizeof g);
	fd2 = perfd_apool_get(pool, "k2", cb, &g);
	CHK(fd2 == fd && stats(pool).dials == 1, "B5 the next get reuses the warm handle (fd %d, %llu dials)",
		fd2, stats(pool).dials);
	CHK(perfd_apool_wait(pool, fd2, 3000) == 1 && g.calls == 1 && !strcmp(g.val, "v:k2"),
		"B6 perfd_apool_wait returns 1 once the callback ran ('%s')", g.val);
	memset(&g, 0, sizeof g);
	fd = perfd_apool_get(pool, "miss1", cb, &g);
	CHK(fd >= 0 && perfd_apool_wait(pool, fd, 3000) == 1 && !strcmp(g.val, "(absent)"),
		"B7 a miss is the one-byte absent reply ('%s')", g.val);
	memset(&g, 0, sizeof g);
	fd = perfd_apool_get(pool, "err1", cb, &g);
	CHK(fd >= 0 && perfd_apool_wait(pool, fd, 3000) == 1 && g.failed && strstr(g.val, "boom"),
		"B8 a daemon error reaches the callback as NULL + errmsg ('%s')", g.val);
	{
		char a[64] = "";
		int p = 0;

		memset(&g, 0, sizeof g);
		fd = perfd_apool_get(pool, "k3", cb, &g);
		CHK(perfd_apool_node(pool, fd, a, sizeof a, &p) == 0 && !strcmp(a, "127.0.0.1") &&
			p == ports[0], "B9 perfd_apool_node names the seed (%s:%d)", a, p);
		perfd_apool_wait(pool, fd, 3000);
		CHK(perfd_apool_node(pool, fd, a, sizeof a, &p) == -1,
			"B10 ...and -1 once the request is answered");
	}

	printf("--- C. a wait that times out cancels; the late reply reaches nobody\n");
	{
		int acc = __atomic_load_n(&accepts, __ATOMIC_ACQUIRE);
		long long t0;

		memset(&g, 0, sizeof g);
		fd = perfd_apool_get(pool, "hold1", cb, &g);
		t0 = ms();
		CHK(fd >= 0 && perfd_apool_wait(pool, fd, 200) == 0 && g.calls == 0,
			"C1 wait(200 ms) on a held request returns 0 and runs nothing");
		CHK(ms() - t0 >= 190 && ms() - t0 < 1500, "C2 ...after its timeout (%lld ms)", ms() - t0);
		CHK(stats(pool).cancelled == 1, "C3 the request counts as cancelled");
		poke(&want_release, &releases_done);
		usleep(100000);
		CHK(perfd_apool_ready(pool, fd) == -1 && g.calls == 0,
			"C4 the reply arrived after: ready on the old fd is -1, the callback never ran");
		memset(&g, 0, sizeof g);
		fd2 = perfd_apool_get(pool, "k4", cb, &g);
		CHK(fd2 >= 0 && perfd_apool_wait(pool, fd2, 3000) == 1 && !strcmp(g.val, "v:k4"),
			"C5 the next get is answered with its own value, not the late one ('%s')", g.val);
		CHK(__atomic_load_n(&accepts, __ATOMIC_ACQUIRE) == acc + 1 && stats(pool).rebuilt >= 1,
			"C6 ...on a handle dialled anew (accepts %d -> %d, rebuilt %llu)", acc,
			__atomic_load_n(&accepts, __ATOMIC_ACQUIRE), stats(pool).rebuilt);
	}

	printf("--- D. perfd_apool_cancel\n");
	{
		struct got g2;
		int acc;

		memset(&g2, 0, sizeof g2);
		fd = perfd_apool_get(pool, "hold2", cb, &g2);
		acc = __atomic_load_n(&accepts, __ATOMIC_ACQUIRE);
		perfd_apool_cancel(pool, fd);
		CHK(perfd_apool_ready(pool, fd) == -1 && perfd_apool_wait(pool, fd, 10) == -1,
			"D1 a cancelled fd has no request: ready and wait are -1");
		poke(&want_release, &releases_done);
		usleep(100000);
		memset(&g, 0, sizeof g);
		fd2 = perfd_apool_get(pool, "k5", cb, &g);
		CHK(fd2 >= 0 && perfd_apool_wait(pool, fd2, 3000) == 1 && !strcmp(g.val, "v:k5") &&
			g2.calls == 0, "D2 cancel then a late reply: the cancelled callback never runs, "
			"the next get gets its own value ('%s', %d)", g.val, g2.calls);
		CHK(__atomic_load_n(&accepts, __ATOMIC_ACQUIRE) == acc + 1,
			"D3 the cancelled handle was not reused (a new connection)");
	}

	printf("--- E. a failed handle is rebuilt\n");
	{
		int acc = __atomic_load_n(&accepts, __ATOMIC_ACQUIRE);

		memset(&g, 0, sizeof g);
		fd = perfd_apool_get(pool, "close1", cb, &g);
		CHK(fd >= 0 && perfd_apool_wait(pool, fd, 3000) == 1 && g.failed && g.calls == 1,
			"E1 the link closed under a request: the callback gets the error once ('%s')", g.val);
		memset(&g, 0, sizeof g);
		fd2 = perfd_apool_get(pool, "k6", cb, &g);
		CHK(fd2 >= 0 && perfd_apool_wait(pool, fd2, 3000) == 1 && !strcmp(g.val, "v:k6") &&
			__atomic_load_n(&accepts, __ATOMIC_ACQUIRE) == acc + 1,
			"E2 the next get succeeds on a new connection ('%s')", g.val);
		/* an idle handle whose server went away: found closed before the request, not after */
		poke(&want_drop, &drops_done);
		usleep(100000);
		memset(&g, 0, sizeof g);
		fd2 = perfd_apool_get(pool, "k7", cb, &g);
		CHK(fd2 >= 0 && perfd_apool_wait(pool, fd2, 3000) == 1 && !g.failed &&
			!strcmp(g.val, "v:k7"), "E3 an idle handle closed by its server is replaced before "
			"the get, which succeeds ('%s')", g.val);
	}

	printf("--- F. a full pool\n");
	{
		perfd_apool *one = perfd_apool_new_seeds(hosts, ports, 1, "c", &o, 1);
		struct got g2;

		memset(&g, 0, sizeof g);
		memset(&g2, 0, sizeof g2);
		fd = one ? perfd_apool_get(one, "hold3", cb, &g) : -1;
		fd2 = one ? perfd_apool_get(one, "k8", cb, &g2) : 0;
		CHK(fd >= 0 && fd2 == -1 && g2.calls == 0 && strstr(perfd_error(NULL), "in flight"),
			"F1 with its one handle busy a get is -1, the callback not called ('%s')",
			perfd_error(NULL));
		CHK(stats(one).busy == 1 && stats(one).slots == 1, "F2 stats: one handle, busy");
		perfd_apool_free(one);
		CHK(g.calls == 0, "F3 perfd_apool_free runs no in-flight callback");
		poke(&want_release, &releases_done);
	}

	printf("--- G. after fork() the child resets the pool\n");
	{
		struct got gh;
		int hfd, acc, st = -1;
		pid_t pid;

		memset(&gh, 0, sizeof gh);
		hfd = perfd_apool_get(pool, "hold4", cb, &gh);
		acc = __atomic_load_n(&accepts, __ATOMIC_ACQUIRE);
		pid = fork();
		if (pid == 0) {
			struct perfd_apool_stats s;
			int bad = 0, cfd;

			alarm(20);
			memset(&g, 0, sizeof g);
			if (perfd_apool_ready(pool, hfd) != -1)
				bad |= 1;              /* the parent's request is not the child's */
			cfd = perfd_apool_get(pool, "k9", cb, &g);
			if (cfd < 0 || perfd_apool_wait(pool, cfd, 3000) != 1 || strcmp(g.val, "v:k9"))
				bad |= 2;
			if (gh.calls)
				bad |= 4;
			s = stats(pool);
			if (s.resets != 1)
				bad |= 8;
			_exit(bad);
		}
		if (pid > 0)
			waitpid(pid, &st, 0);
		CHK(pid > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0,
			"G1 the child dropped the parent's request, dialled its own and got k9 (bits %d)",
			WIFEXITED(st) ? WEXITSTATUS(st) : -1);
		CHK(__atomic_load_n(&accepts, __ATOMIC_ACQUIRE) == acc + 1,
			"G2 the child opened a connection of its own");
		poke(&want_release, &releases_done);
		CHK(perfd_apool_wait(pool, hfd, 3000) == 1 && !strcmp(gh.val, "v:hold4") &&
			stats(pool).resets == 0, "G3 the parent's held request completes on its own socket "
			"('%s')", gh.val);
	}
	perfd_apool_free(pool);

	printf("--- H. the warm-up is bounded by connect_timeout\n");
	{
		int qport, qfd = listen_on(&qport);
		const char *sec[2] = { "apool-secret", NULL };
		const char *qh[1] = { "127.0.0.1" };
		perfd_opts q = PERFD_OPTS_INIT;
		perfd_apool *qp;
		long long t0, el;

		q.secrets = sec;
		q.connect_timeout_ms = 300;
		qp = perfd_apool_new_seeds(qh, &qport, 1, "c", &q, 2);
		memset(&g, 0, sizeof g);
		t0 = ms();
		fd = qfd >= 0 && qp ? perfd_apool_get(qp, "k1", cb, &g) : 0;
		el = ms() - t0;
		CHK(fd == -1 && g.calls == 0 && el >= 290 && el < 5000,
			"H1 a node that takes the TCP and never answers the handshake: -1 after %lld ms "
			"(connect_timeout 300 + the key derivation)", el);
		t0 = ms();
		fd = qp ? perfd_apool_get(qp, "k1", cb, &g) : 0;
		el = ms() - t0;
		CHK(fd == -1 && el < 100 && strstr(perfd_error(NULL), "backing off"),
			"H2 the next get fails at once while backing off (%lld ms, '%s')", el, perfd_error(NULL));
		perfd_apool_free(qp);
		if (qfd >= 0)
			close(qfd);
	}

	printf("--- I. a dial does not hold the get for connect_timeout\n");
	{
		int qport, qfd = listen_on(&qport);
		const char *sec[2] = { "apool-secret", NULL };
		const char *qh[1] = { "127.0.0.1" };
		perfd_opts q = PERFD_OPTS_INIT;
		perfd_apool *qp;
		unsigned long long d0;
		long long t0, el;

		q.secrets = sec;
		q.connect_timeout_ms = 3000;
		perfd_psk_prepare(sec);            /* the derivation is not the dial */
		qp = perfd_apool_new_seeds(qh, &qport, 1, "c", &q, 2);
		memset(&g, 0, sizeof g);
		t0 = ms();
		fd = qfd >= 0 && qp ? perfd_apool_get(qp, "k1", cb, &g) : 0;
		el = ms() - t0;
		CHK(fd == -1 && g.calls == 0 && el >= 400 && el < 1500,
			"I1 a node that takes the TCP and never answers: -1 after %lld ms, not "
			"connect_timeout's 3000 ('%s')", el, perfd_error(NULL));
		d0 = qp ? stats(qp).dials : 0;
		t0 = ms();
		fd = qp ? perfd_apool_get(qp, "k2", cb, &g) : 0;
		el = ms() - t0;
		CHK(fd == -1 && el < 100 && strstr(perfd_error(NULL), "still connecting") &&
			qp && stats(qp).dials == d0, "I2 while that dial is still connecting a get "
			"fails at once and dials nothing new (%lld ms, '%s')", el, perfd_error(NULL));
		usleep(3200000);
		t0 = ms();
		fd = qp ? perfd_apool_get(qp, "k3", cb, &g) : 0;
		el = ms() - t0;
		CHK(fd == -1 && el < 100 && qp && stats(qp).dial_failures == 1 &&
			strstr(perfd_error(NULL), "backing off"), "I3 past connect_timeout the dial "
			"counts as failed and the pool backs off (%lld ms, %llu failures, '%s')", el,
			qp ? stats(qp).dial_failures : 0, perfd_error(NULL));
		CHK(g.calls == 0, "I4 no callback ran for a get that answered -1");
		perfd_apool_free(qp);
		if (qfd >= 0)
			close(qfd);
	}

	printf("--- J. dial_wait_ms sets that wait\n");
	{
		static const int waits[2] = { 150, -1 };
		const char *sec[2] = { "apool-secret", NULL };
		const char *qh[1] = { "127.0.0.1" };
		int w;

		for (w = 0; w < 2; w++) {
			int qport, qfd = listen_on(&qport);
			perfd_opts q = PERFD_OPTS_INIT;
			perfd_apool *qp;
			long long t0, el;

			q.secrets = sec;
			q.connect_timeout_ms = 1200;
			q.dial_wait_ms = waits[w];
			qp = perfd_apool_new_seeds(qh, &qport, 1, "c", &q, 2);
			memset(&g, 0, sizeof g);
			t0 = ms();
			fd = qfd >= 0 && qp ? perfd_apool_get(qp, "k1", cb, &g) : 0;
			el = ms() - t0;
			if (w == 0)
				CHK(fd == -1 && el >= 100 && el < 400, "J1 dial_wait_ms = 150: "
					"-1 after %lld ms, not the default 500", el);
			else
				CHK(fd == -1 && el >= 1100 && el < 2500, "J2 dial_wait_ms = -1: "
					"the get waits all of connect_timeout's 1200 (%lld ms)", el);
			perfd_apool_free(qp);
			if (qfd >= 0)
				close(qfd);
		}
	}

	__atomic_store_n(&want_stop, 1, __ATOMIC_RELEASE);
	if (write(wake[1], "x", 1) < 0)
		fails++;
	pthread_join(th, NULL);
	printf("apooltest: %s (%d failed)\n", fails ? "FAIL" : "ok", fails);
	return fails ? 1 : 0;
}
