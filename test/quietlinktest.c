/* quietlinktest.c - a quiet link closed by its node's death still takes a counter update; a routed
 * one whose node froze mid-request is not sent again.  Args: host1-4 port secret pid1 pid3 pid4 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "perfd.h"

#define QUIET_US 500000        /* past the library's 100 ms, and the FIN in */
#define CONVERGE_S 25.0        /* two repair sweeps */
static int pass, fail;
#define OK(c, ...) do { if (c) { pass++; printf("  ok   "); } else { fail++; printf("  FAIL "); } \
	printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static double now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

static perfd_t *dial(const char *host, int port, const char *const *sec, int route, int spares)
{
	perfd_opts o;

	perfd_opts_init(&o);
	o.secrets = sec;
	o.idle_ping_ms = -1;           /* no ping finds the dead link first */
	o.route_keys = route;
	o.io_timeout_ms = route ? 1000 : 0;    /* a frozen node's add gives up in 1 s */
	o.spares = spares;
	return perfd_connect(host, port, &o);
}

/* a key @r routes to @host, written as "<pfx><n>" into @k */
static int key_on(perfd_t *r, const char *host, const char *pfx, char *k, size_t kcap)
{
	char addr[64];
	int i;

	for (i = 0; i < 200; i++) {
		snprintf(k, kcap, "%s%d", pfx, i);
		if (perfd_member_info(r, perfd_owner_of(r, "c", k), addr, sizeof addr, NULL, NULL, NULL) == 0 &&
		        !strcmp(addr, host))
			return 1;
	}
	return 0;
}

/* where @h is now, as its member list says */
static const char *active_addr(perfd_t *h)
{
	static char addr[4][64];
	static int k;
	int i, act = 0;

	k = (k + 1) % 4;
	strcpy(addr[k], "?");
	for (i = 0; i < perfd_member_count(h); i++)
		if (perfd_member_info(h, i, addr[k], sizeof addr[k], NULL, NULL, &act) == 0 && act)
			break;
	return addr[k];
}

/* the value at @path of @key on a bare handle once it reads @want, within @secs; *took in ms */
static long long settled(perfd_t *m, const char *key, const char *path, long long want, double secs, double *took)
{
	long long v = -1;
	double t0 = now_ms();

	while (now_ms() - t0 < secs * 1e3) {
		char *f = NULL;

		if (path) {
			v = perfd_jget(m, "c", key, path, &f) == 1 && f ? atoll(f) : -1;
			free(f);
		} else {               /* a GET: an add of 0 is a write, and races the one under test */
			char buf[32];
			size_t n = 0;

			v = perfd_get_buf(m, "c", key, buf, sizeof buf - 1, &n, NULL) == 1 ? (buf[n] = 0, atoll(buf)) : -1;
		}
		if (v == want)
			break;
		usleep(100000);
	}
	*took = now_ms() - t0;
	return v;
}

int main(int argc, char **argv)
{
	const char *sec[2] = { NULL, NULL };
	perfd_t *a, *b, *r, *m;
	char kr[16], kc[16];
	long long nv = -1;
	int port, rc;
	double deadline, t;

	if (argc != 10)
		return 2;
	port = atoi(argv[5]);
	sec[0] = argv[6];
	a = dial(argv[1], port, sec, 0, 0);
	b = dial(argv[1], port, sec, 0, 0);
	r = dial(argv[1], port, sec, 1, 0);
	m = dial(argv[2], port, sec, 0, PERFD_SPARES_NONE);
	if (!a || !b || !r || !m) {
		printf("  FAIL connect: %s\n", perfd_error(NULL));
		return 1;
	}
	deadline = now_ms() + 10e3;
	do {
		rc = perfd_ping(a) | perfd_ping(b) | perfd_ping(r);
		if (perfd_spare_count(a) == 3 && perfd_spare_count(b) == 3 && perfd_spare_count(r) == 3)
			break;
		usleep(100000);
	} while (now_ms() < deadline);
	OK(rc == 0 && perfd_spare_count(a) + perfd_spare_count(b) + perfd_spare_count(r) == 9,
		"each handle holds a standby to the other three members");
	rc = key_on(r, argv[3], "c", kc, sizeof kc);
	OK(rc && key_on(r, argv[4], "r", kr, sizeof kr), "keys nodes 3 and 4 should hold: %s, %s", kc, kr);
	rc = perfd_add(a, "c", "n", 1, 0, &nv);
	OK(rc == 0 && nv == 1 && perfd_jset(b, "c", "j", "$", "{\"x\":0}", 0) == 0 &&
		perfd_jset(r, "c", kr, "$", "{\"x\":0}", 0) == 0, "the counter and both documents written");

	kill((pid_t)atoi(argv[8]), SIGSTOP);
	t = now_ms();
	rc = perfd_add(r, "c", kc, 1, 0, &nv);
	t = now_ms() - t;
	kill((pid_t)atoi(argv[8]), SIGCONT);
	OK(rc != 0, "a routed add, its node frozen past the timeout: after %.0f ms %s", t,
		rc ? perfd_error(r) : "sent again on the active link");

	kill((pid_t)atoi(argv[9]), SIGKILL);
	usleep(QUIET_US);
	rc = perfd_jincr(r, "c", kr, "$.x", 1, &nv);
	OK(rc == 0 && nv == 1, "a routed jincr, its standby's node killed while the link was quiet: rc %d, x %lld %s",
		rc, nv, rc ? perfd_error(r) : "");
	/* eager acknowledges before its peers have it: node 1 must not die inside the push window */
	nv = settled(m, kr, "$.x", 1, 3, &t);
	OK(nv == 1, "node 2 has it (%s.x = %lld) before node 1 is killed", kr, nv);

	kill((pid_t)atoi(argv[7]), SIGKILL);
	usleep(QUIET_US);
	nv = -1;
	rc = perfd_add(a, "c", "n", 1, 0, &nv);
	OK(rc == 0 && nv == 2, "an add, the active's node killed while the link was quiet: rc %d, n %lld %s",
		rc, nv, rc ? perfd_error(a) : "");
	nv = -1;
	rc = perfd_jincr(b, "c", "j", "$.x", 1, &nv);
	OK(rc == 0 && nv == 1, "a jincr, the same: rc %d, x %lld %s", rc, nv, rc ? perfd_error(b) : "");
	OK(perfd_failovers(a) == 1 && perfd_failovers(b) == 1, "each moved to a standby once: %llu to %s, %llu to %s",
		perfd_failovers(a), active_addr(a), perfd_failovers(b), active_addr(b));

	/* read on the survivor the add did NOT land on; a push lost there is the repair sweep's, every 10 s */
	{
		const char *other = strcmp(active_addr(a), argv[2]) ? argv[2] : argv[3];
		perfd_t *o = strcmp(other, argv[2]) ? dial(other, port, sec, 0, PERFD_SPARES_NONE) : m;

		nv = o ? settled(o, "n", NULL, 2, CONVERGE_S, &t) : -1;
		OK(nv == 2, "%s, where the add did not land, reads n = %lld after %.0f ms", other, nv, t);
		if (nv != 2) {         /* which copy is newer, by the record's own version */
			perfd_t *at = dial(active_addr(a), port, sec, 0, PERFD_SPARES_NONE);
			char *e1 = o ? perfd_command(o, "exists", "{\"col\":\"c\",\"key\":\"n\"}") : NULL;
			char *e2 = at ? perfd_command(at, "exists", "{\"col\":\"c\",\"key\":\"n\"}") : NULL;

			printf("  ..   n on %s: %s; on %s: %s\n", other, e1 ? e1 : "?", active_addr(a), e2 ? e2 : "?");
			free(e1);
			free(e2);
			if (at)
				perfd_free(at);
		}
		nv = o ? settled(o, "j", "$.x", 1, CONVERGE_S, &t) : -1;
		OK(nv == 1, "and j.x = %lld after %.0f ms", nv, t);
		if (o && o != m)
			perfd_free(o);
	}
	nv = settled(m, kc, NULL, 1, 3, &t);
	OK(nv == 0 || nv == 1, "and %s = %lld: the frozen node's add applied at most once", kc, nv);
	perfd_free(m);
	perfd_free(a);
	perfd_free(b);
	perfd_free(r);
	printf("quietlinktest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
