/* respreadtest.c - S474: a spreading handle takes its share of a member that comes up, told by the push or by the list.
 * A member not serving stays known and gets none.  Usage: respreadtest host host4 p1 p2 p3 p4 secret; the shell acts on the cues. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "perfd.h"

#define NEACH 60               /* round robin: 1 in 4 of 60 is 15, 4 to 28 misses by chance about 1 run in 10,000 */
#define NFO 6
#define NALL (NEACH * 2 + NFO)
#define NKEYS 50
static perfd_t *h[NALL];       /* 0..59 told by the push, 60..119 by the list, the rest the default policy */
static int was[NFO];
static int pass, fail, errs, wrong, turn;
static char val[NKEYS][1024];
#define OK(c, ...) do { if (c) { pass++; printf("  ok   "); } else { fail++; printf("  FAIL "); } \
	printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static int on_port(perfd_t *p)
{
	int i, port = 0, active = 0;

	for (i = 0; i < perfd_member_count(p); i++)
		if (perfd_member_info(p, i, NULL, 0, &port, NULL, &active) == 0 && active)
			return port;
	return -1;
}

static int count_on(int from, int to, int port)
{
	int i, n = 0;

	for (i = from; i < to; i++)
		n += on_port(h[i]) == port;
	return n;
}

static void sweep(void)
{
	static char buf[2048];
	char k[16];
	int i, kn = turn++ % NKEYS;

	snprintf(k, sizeof k, "k%d", kn);
	for (i = 0; i < NALL; i++) {
		size_t n = 0;
		int rc = perfd_get_buf(h[i], "c", k, buf, sizeof buf, &n, NULL);

		if (rc < 0) {
			if (!errs)
				printf("  ..   first error: %s\n", perfd_error(h[i]));
			errs++;
		} else if (rc != 1 || n != sizeof val[kn] || memcmp(buf, val[kn], n)) {
			wrong++;
		}
	}
	usleep(10000);
}

static void settle(double secs)
{
	double t0 = now();

	while (now() - t0 < secs)
		sweep();
}

/* a node takes its time to come up: until each group has its share or the limit, then a little longer for the rest */
static double wait_share(int port, double limit)
{
	double t0 = now(), took;

	while ((count_on(0, NEACH, port) < 4 || count_on(NEACH, NEACH * 2, port) < 4) && now() - t0 < limit)
		sweep();
	took = now() - t0;
	settle(2);
	return took;
}

static int all_know(int members)
{
	int i;

	for (i = 0; i < NEACH * 2; i++)
		if (perfd_member_count(h[i]) != members)
			return 0;
	return 1;
}

int main(int ac, char **av)
{
	const char *secs[2];
	int i, j, p2, p4, a, b;
	double t0;

	if (ac != 8)
		return 2;
	p2 = atoi(av[4]);
	p4 = atoi(av[6]);
	secs[0] = av[7];
	secs[1] = NULL;
	if (perfd_psk_prepare(secs) != 0)
		return 2;
	{       /* just after a fleet forms the list can be short and peers not yet ready: connect when both are done */
		perfd_opts o;
		perfd_t *w;

		perfd_opts_init(&o);
		o.secrets = secs;
		o.refresh_ms = 200;
		w = perfd_connect(av[1], atoi(av[3]), &o);
		t0 = now();
		while (w && (perfd_member_count(w) < 3 || perfd_spare_count(w) < 2) && now() - t0 < 30) {
			usleep(100000);
			if (perfd_ping(w) != 0)
				break;
		}
		OK(w && perfd_member_count(w) == 3 && perfd_spare_count(w) == 2, "three members, two of them standbys, after %.1f s", now() - t0);
		if (w)
			perfd_free(w);
	}
	for (i = 0; i < NALL; i++) {
		perfd_opts o;

		perfd_opts_init(&o);
		o.secrets = secs;
		if (i < NEACH * 2) {
			o.policy = PERFD_POLICY_ROUND_ROBIN;
			o.idle_ping_ms = 1000;     /* a dead standby is seen in time for the restart */
		}
		if (i >= NEACH && i < NEACH * 2) {
			o.ignore_member_push = 1;
			o.refresh_ms = 500;
		}
		h[i] = perfd_connect(av[1], atoi(av[3]), &o);
		if (!h[i]) {
			printf("  FAIL connect %d: %s\n", i, perfd_error(NULL));
			return 1;
		}
	}
	for (j = 0; j < NKEYS; j++) {
		char k[16];

		snprintf(k, sizeof k, "k%d", j);
		memset(val[j], 'a' + j % 26, sizeof val[j]);
		if (perfd_set(h[0], "c", k, val[j], sizeof val[j], 0) != 0) {
			printf("  FAIL seed: %s\n", perfd_error(h[0]));
			return 1;
		}
	}
	t0 = now();            /* eager: every node holds them before anything is counted */
	do {
		errs = wrong = 0;
		for (j = 0; j < NKEYS; j++)
			sweep();
	} while ((errs || wrong) && now() - t0 < 30);
	OK(!errs && !wrong, "the seed reads back from every handle (%d errors, %d wrong)", errs, wrong);
	OK(count_on(0, NEACH * 2, p2) >= 15 && count_on(0, NEACH * 2, p4) == 0,
		"premise: round robin spread over three nodes (%d of %d on node 2)", count_on(0, NEACH * 2, p2), NEACH * 2);
	for (i = 0; i < NFO; i++)
		was[i] = on_port(h[NEACH * 2 + i]);
	errs = wrong = 0;

	printf("ADD-NODE\n");
	fflush(stdout);
	t0 = now();
	while (!all_know(4) && now() - t0 < 45)
		sweep();
	OK(all_know(4), "every handle learned the fourth member, in %.1f s", now() - t0);
	printf("  ..   moves seen after %.1f s more\n", wait_share(p4, 60));
	a = count_on(0, NEACH, p4);
	b = count_on(NEACH, NEACH * 2, p4);
	OK(a >= 4 && a <= 28, "told by the push: %d of %d moved to the new node (1 in 4 is 15)", a, NEACH);
	OK(b >= 4 && b <= 28, "told by the list: %d of %d moved to the new node (1 in 4 is 15)", b, NEACH);
	for (i = j = 0; i < NFO; i++)
		j += on_port(h[NEACH * 2 + i]) != was[i];
	OK(j == 0, "the default policy stayed where it was: %d of %d moved", j, NFO);
	OK(!errs && !wrong, "no call failed or read wrong across the join (%d errors, %d wrong)", errs, wrong);
	errs = wrong = 0;

	printf("KILL-NODE\n");
	fflush(stdout);
	sleep(1);
	t0 = now();
	while (count_on(0, NALL, p2) && now() - t0 < 30)
		sweep();
	OK(count_on(0, NALL, p2) == 0, "every handle left the dead node, in %.1f s", now() - t0);
	settle(3);
	OK(!errs && !wrong, "no call failed or read wrong across the kill (%d errors, %d wrong)", errs, wrong);
	errs = wrong = 0;

	printf("RESTART-NODE\n");
	fflush(stdout);
	printf("  ..   moves back seen after %.1f s\n", wait_share(p2, 90));
	a = count_on(0, NEACH, p2);
	b = count_on(NEACH, NEACH * 2, p2);
	OK(a >= 4 && a <= 28, "told by the push: %d of %d came back to the restarted node (1 in 4 is 15)", a, NEACH);
	OK(b >= 4 && b <= 28, "told by the list: %d of %d came back to the restarted node (1 in 4 is 15)", b, NEACH);
	OK(count_on(NEACH * 2, NALL, p2) == 0, "the default policy did not go back: %d of %d", count_on(NEACH * 2, NALL, p2), NFO);
	OK(!errs && !wrong, "no call failed or read wrong across the return (%d errors, %d wrong)", errs, wrong);
	errs = wrong = 0;

	/* FAILED: as healing, it serves reads, refuses writes, and the client takes both as not usable */
	printf("FAIL-NODE\n");
	fflush(stdout);
	sleep(1);
	{
		perfd_opts o;
		perfd_t *d;

		perfd_opts_init(&o);
		o.secrets = secs;
		o.spares = PERFD_SPARES_NONE;
		d = perfd_connect(av[2], p4, &o);
		if (d) {
			if (perfd_set(d, "c", "fail-it", "x", 1, 0) != 0)
				printf("  ..   the write that fails it: %s\n", perfd_error(d));
			perfd_free(d);
		}
	}
	a = count_on(0, NEACH * 2, p4);
	t0 = now();
	while (count_on(0, NEACH * 2, p4) && now() - t0 < 30)
		sweep();
	OK(a > 0 && count_on(0, NEACH * 2, p4) == 0, "%d handles left the failed node, in %.1f s, all round robin ones are off it", a, now() - t0);
	settle(3);
	OK(count_on(0, NEACH * 2, p4) == 0, "and none went back to it while it stayed failed (%d)", count_on(0, NEACH * 2, p4));
	OK(all_know(4), "it is still known: every handle lists four members");
	OK(!errs && !wrong, "no call failed or read wrong across the failure (%d errors, %d wrong)", errs, wrong);
	errs = wrong = 0;

	printf("RESTORE-NODE\n");
	fflush(stdout);
	printf("  ..   moves back seen after %.1f s\n", wait_share(p4, 90));
	a = count_on(0, NEACH, p4);
	b = count_on(NEACH, NEACH * 2, p4);
	OK(a >= 4 && a <= 28, "told by the push: %d of %d came back when it served again (1 in 4 is 15)", a, NEACH);
	OK(b >= 4 && b <= 28, "told by the list: %d of %d came back when it served again (1 in 4 is 15)", b, NEACH);
	OK(!errs && !wrong, "no call failed or read wrong across the restore (%d errors, %d wrong)", errs, wrong);
	for (i = 0; i < NALL; i++)
		perfd_free(h[i]);
	printf("respreadtest: %d passed, %d failed\n", pass, fail);
	return fail != 0;
}
