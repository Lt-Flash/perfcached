/* SPDX-License-Identifier: GPL-2.0-or-later */
/* apoollivetest.c - the async pool on a three-node fleet (test/apoollivetest.sh): owner routing,
 * ready/wait/cancel, a late reply, rebuilds after a node restart, eviction, fork. */
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include "../lib/perfd.h"

static int pass, fail;
static const char *cmdp, *ackp;
static FILE *cmdf;

static void res(int c, const char *fmt, ...)
{
	va_list ap;

	printf(c ? "ok: " : "FAIL: ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	if (c)
		pass++;
	else
		fail++;
}

static long long ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* the shell runs "<stop|cont|kill|start> <node>" and answers when done */
static void ctl(const char *what, int node)
{
	char line[64];
	FILE *a;

	if (!cmdf)
		return;
	fprintf(cmdf, "%s %d\n", what, node);
	fflush(cmdf);
	if ((a = fopen(ackp, "r"))) {
		if (!fgets(line, sizeof line, a))
			line[0] = 0;
		fclose(a);
	}
}

struct got { int calls, failed; char val[128]; };

static void cb(char *result, size_t len, const char *errmsg, void *ctx)
{
	struct got *g = ctx;

	g->calls++;
	g->failed = !result;
	if (!result)
		snprintf(g->val, sizeof g->val, "error: %s", errmsg ? errmsg : "?");
	else if (len >= 5 && result[0] == 1)
		snprintf(g->val, sizeof g->val, "%.*s", (int)(len - 5), result + 5);
	else
		snprintf(g->val, sizeof g->val, "(absent)");
	perfd_free_value(result);
}

static struct perfd_apool_stats stats(perfd_apool *p)
{
	struct perfd_apool_stats s;

	memset(&s, 0, sizeof s);
	perfd_apool_stats(p, &s);
	return s;
}

static perfd_t *B;                     /* the blocking reference: sets keys, names owners */

/* the owner's address by B's view; its node number is the last octet */
static int owner(const char *key, char *addr, size_t cap)
{
	int ix = perfd_owner_of(B, "c", key);

	addr[0] = 0;
	if (ix < 0 || perfd_member_info(B, ix, addr, cap, NULL, NULL, NULL) != 0)
		return -1;
	return atoi(strrchr(addr, '.') ? strrchr(addr, '.') + 1 : "0");
}

static int node_of(perfd_apool *p, int fd, char *addr, size_t cap)
{
	addr[0] = 0;
	if (perfd_apool_node(p, fd, addr, cap, NULL) != 0)
		return -1;
	return atoi(strrchr(addr, '.') ? strrchr(addr, '.') + 1 : "0");
}

/* one get answered through wait: 1 with the right value */
static int get_ok(perfd_apool *p, const char *key, struct got *g, int *nodep)
{
	char want[64], a[64];
	int fd;

	memset(g, 0, sizeof *g);
	snprintf(want, sizeof want, "v-%s", key);
	fd = perfd_apool_get(p, key, cb, g);
	if (nodep)
		*nodep = fd >= 0 ? node_of(p, fd, a, sizeof a) : -1;
	if (fd >= 0 && perfd_apool_wait(p, fd, 3000) != 1)
		return 0;
	if (fd == -1)
		snprintf(g->val, sizeof g->val, "get -1: %s", perfd_error(NULL));
	return g->calls == 1 && !strcmp(g->val, want);
}

/* the first of k0.. that B places on @node (or not on it, @not) */
static void key_on(int node, int not, char *key, size_t cap)
{
	char a[64];
	int i;

	for (i = 0; i < 200; i++) {
		snprintf(key, cap, "k%d", i);
		if ((owner(key, a, sizeof a) == node) != not)
			return;
	}
}

int main(int argc, char **argv)
{
	const char *sec[2] = { NULL, NULL }, *hosts[3];
	char hbuf[3][64], key[32], a[64], a2[64];
	int ports[3], i, fd, n, ok, nodes[4] = { 0 }, X;
	perfd_opts o = PERFD_OPTS_INIT;
	perfd_apool *P;
	struct got g, g2;

	if (argc != 7) {
		fprintf(stderr, "usage: apoollivetest secret cmdfifo ackfifo host:port host:port host:port\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	sec[0] = argv[1];
	cmdp = argv[2];
	ackp = argv[3];
	if (!(cmdf = fopen(cmdp, "w"))) {          /* the shell reads it until we exit */
		perror("apoollivetest: the command FIFO");
		return 1;
	}
	for (i = 0; i < 3; i++) {
		char *c;

		snprintf(hbuf[i], sizeof hbuf[i], "%s", argv[4 + i]);
		c = strrchr(hbuf[i], ':');
		ports[i] = c ? atoi(c + 1) : 0;
		if (c)
			*c = 0;
		hosts[i] = hbuf[i];
	}
	o.secrets = sec;
	o.route_keys = 1;
	o.refresh_ms = 1000;
	o.connect_timeout_ms = 2000;
	if (!(B = perfd_connect(hosts[0], ports[0], &o)) || !perfd_routing(B)) {
		res(0, "the reference handle does not route (%s)", B ? "no routing" : perfd_error(NULL));
		goto out;
	}
	for (i = 0, ok = 0; i < 200; i++) {
		char v[48];

		snprintf(key, sizeof key, "k%d", i);
		snprintf(v, sizeof v, "v-%s", key);
		ok += perfd_set(B, "c", key, v, strlen(v), 0) == 0;
	}
	res(ok == 200, "200 keys set through the reference handle (%d)", ok);

	printf("--- owner routing\n");
	P = perfd_apool_new_seeds(hosts, ports, 3, "c", &o, 4);
	if (!P) {
		res(0, "no pool: %s", perfd_error(NULL));
		goto out;
	}
	for (i = 0, ok = 0, n = 0; i < 60; i++) {
		int want, got;

		snprintf(key, sizeof key, "k%d", i);
		want = owner(key, a, sizeof a);
		n += get_ok(P, key, &g, &got);
		ok += got == want && want > 0;
		if (got >= 1 && got <= 3)
			nodes[got] = 1;
	}
	res(ok == 60, "60 gets each went to its key's owner (%d)", ok);
	res(n == 60, "...and each was answered with its value through perfd_apool_wait (%d)", n);
	res(nodes[1] && nodes[2] && nodes[3], "...spread over all three nodes");
	res(stats(P).routing == 1 && stats(P).dials <= 4, "the pool routes, on %llu dials (router + "
		"one handle per owner)", stats(P).dials);

	printf("--- readiness\n");
	for (i = 60, ok = 0; i < 80; i++) {
		long long until = ms() + 3000;

		snprintf(key, sizeof key, "k%d", i);
		memset(&g, 0, sizeof g);
		fd = perfd_apool_get(P, key, cb, &g);
		while (fd >= 0 && ms() < until) {
			struct pollfd pf = { fd, POLLIN, 0 };

			if (poll(&pf, 1, 100) > 0 && perfd_apool_ready(P, fd) != 0)
				break;
		}
		snprintf(a, sizeof a, "v-%s", key);
		ok += g.calls == 1 && !strcmp(g.val, a);
	}
	res(ok == 20, "20 gets completed by perfd_apool_ready on readability (%d)", ok);
	memset(&g, 0, sizeof g);
	fd = perfd_apool_get(P, "absent-key", cb, &g);
	ok = fd >= 0 && perfd_apool_wait(P, fd, 3000) == 1;
	res(ok && !strcmp(g.val, "(absent)"),
		"a missing key is the absent reply (%s)", g.val);

	printf("--- a wait that times out, a cancel, the late reply\n");
	key_on(2, 0, key, sizeof key);
	X = owner(key, a, sizeof a);
	get_ok(P, key, &g, NULL);              /* its handle warm */
	{
		unsigned long long dials = stats(P).dials, rebuilt = stats(P).rebuilt;

		ctl("stop", X);
		memset(&g2, 0, sizeof g2);
		fd = perfd_apool_get(P, key, cb, &g2);
		res(fd >= 0 && perfd_apool_wait(P, fd, 300) == 0 && g2.calls == 0,
			"%s on stopped node %d: wait(300) returns 0, the callback not run", key, X);
		ctl("cont", X);
		usleep(300000);
		res(perfd_apool_ready(P, fd) == -1 && g2.calls == 0,
			"...its reply came after: ready on the old fd is -1, the callback never ran");
		ok = get_ok(P, key, &g, &n);
		res(ok && n == X, "...the next get of %s is answered by node %d (%s)",
			key, X, g.val);
		res(stats(P).dials == dials + 1 && stats(P).rebuilt == rebuilt + 1,
			"...on a rebuilt handle (dials +%llu, rebuilt +%llu)", stats(P).dials - dials,
			stats(P).rebuilt - rebuilt);
		ctl("stop", X);
		memset(&g2, 0, sizeof g2);
		fd = perfd_apool_get(P, key, cb, &g2);
		perfd_apool_cancel(P, fd);
		ctl("cont", X);
		usleep(300000);
		ok = fd >= 0 && perfd_apool_ready(P, fd) == -1 && get_ok(P, key, &g, NULL);
		res(ok && g2.calls == 0, "cancel, then the late reply: the next get has its own value, the "
			"cancelled callback never ran (%s)", g.val);
	}

	printf("--- a node restart\n");
	{
		unsigned long long rebuilt = stats(P).rebuilt;
		long long until;
		int back = 0;

		ctl("stop", X);
		memset(&g2, 0, sizeof g2);
		fd = perfd_apool_get(P, key, cb, &g2);
		ctl("kill", X);
		ok = fd >= 0 && perfd_apool_wait(P, fd, 5000) == 1;
		res(ok && g2.failed,
			"a get in flight on node %d when it is killed: the callback gets the error (%s)",
			X, g2.val);
		ctl("start", X);
		ok = get_ok(P, key, &g, NULL);
		res(ok, "after node %d restarted, the first get of %s is "
			"answered (%s)", X, key, g.val);
		until = ms() + 15000;
		while (ms() < until && !back) {
			perfd_maintain(B);
			if (get_ok(P, key, &g, &n) && n == X && owner(key, a, sizeof a) == X)
				back = 1;
			else
				usleep(200000);
		}
		res(back, "...and routes to node %d again once the router learned it", X);
		/* now node X holds an idle pool handle: kill and restart it under that handle */
		ctl("kill", X);
		ctl("start", X);
		until = ms() + 15000;
		ok = 1;
		back = 0;
		for (i = 0; ms() < until && !back; i++) {
			perfd_maintain(B);
			if (!get_ok(P, key, &g, &n)) {
				ok = 0;
				break;
			}
			if (n == X)
				back = 1;
			else
				usleep(200000);
		}
		res(ok && back, "an idle handle to a restarted node is rebuilt before use: %d gets, every "
			"one answered, the last by node %d (%s)", i, X, g.val);
		res(stats(P).rebuilt > rebuilt, "...the failed handles were rebuilt (%llu)",
			stats(P).rebuilt - rebuilt);
	}

	printf("--- a full pool evicts an idle handle of another node\n");
	{
		perfd_apool *Q = perfd_apool_new_seeds(hosts, ports, 3, "c", &o, 1);
		char k2[32];
		int na, nb;

		key_on(1, 0, key, sizeof key);
		key_on(1, 1, k2, sizeof k2);
		na = owner(key, a, sizeof a);
		nb = owner(k2, a2, sizeof a2);
		ok = Q && get_ok(Q, key, &g, &n);
		res(ok && n == na, "max_conns 1: %s answered by node %d",
			key, na);
		ok = Q && get_ok(Q, k2, &g, &n);
		res(ok && n == nb && nb != na && stats(Q).evicted == 1 &&
			stats(Q).slots == 1, "...%s by node %d, the idle handle evicted (%llu)", k2, nb,
			Q ? stats(Q).evicted : 0);
		memset(&g, 0, sizeof g);
		memset(&g2, 0, sizeof g2);
		fd = Q ? perfd_apool_get(Q, key, cb, &g) : -1;
		ok = fd >= 0 && perfd_apool_get(Q, k2, cb, &g2) == -1;
		res(ok && g2.calls == 0 &&
			strstr(perfd_error(NULL), "in flight"), "with its one handle busy a get is -1 (%s)",
			perfd_error(NULL));
		res(fd >= 0 && perfd_apool_wait(Q, fd, 3000) == 1 && g.calls == 1,
			"...and the busy one completes");
		perfd_apool_free(Q);
	}

	printf("--- fork\n");
	{
		struct got gh;
		int hfd, st = -1;
		pid_t pid;

		key_on(3, 0, key, sizeof key);
		get_ok(P, key, &g, NULL);
		memset(&gh, 0, sizeof gh);
		hfd = perfd_apool_get(P, key, cb, &gh);
		pid = fork();
		if (pid == 0) {
			int bad = 0, cn;

			alarm(30);
			if (perfd_apool_ready(P, hfd) != -1)
				bad |= 1;
			if (!get_ok(P, "k150", &g, &cn) || cn != owner("k150", a, sizeof a))
				bad |= 2;
			if (gh.calls)
				bad |= 4;
			if (stats(P).resets != 1)
				bad |= 8;
			_exit(bad);
		}
		if (pid > 0)
			waitpid(pid, &st, 0);
		res(pid > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0, "the child resets the pool: "
			"the parent's request is not its, its own get goes to the owner (bits %d)",
			WIFEXITED(st) ? WEXITSTATUS(st) : -1);
		snprintf(a, sizeof a, "v-%s", key);
		ok = hfd >= 0 && perfd_apool_wait(P, hfd, 3000) == 1;
		res(ok && !strcmp(gh.val, a) &&
			stats(P).resets == 0, "the parent's request completes on its own socket (%s)",
			gh.val);
	}
	perfd_apool_free(P);

	printf("--- no routing\n");
	{
		perfd_opts q = o;
		perfd_apool *R;

		q.route_keys = 0;
		R = perfd_apool_new_seeds(hosts, ports, 3, "c", &q, 2);
		ok = R && get_ok(R, "k5", &g, &n);
		res(ok && n >= 1 && n <= 3 && stats(R).routing == 0 &&
			stats(R).dials == 1, "route_keys 0: a seed answers, no router dialled (node %d)", n);
		perfd_apool_free(R);
	}
out:
	if (cmdf)
		fclose(cmdf);
	perfd_free(B);
	printf("apoollivetest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
