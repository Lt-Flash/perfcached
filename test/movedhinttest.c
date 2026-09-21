/* movedhinttest.c - RV-10: a forwarded reply tells a routing client it is
 * behind, and the client catches up in ONE request.
 *
 * A request that lands on the wrong node is forwarded and answered -
 * a stale client map costs a hop, never correctness.  But nothing in the
 * reply said so: a routing client that missed the membership push kept
 * paying the hop on every mis-owned key until its own periodic refresh
 * (opts.refresh_ms, 30 s by default) came round.  Under shard and spread
 * a reply the daemon had to forward now carries `"moved":{term,seq}` -
 * the stamp of the map it routed by - and a handle holding another stamp
 * re-learns its members right behind that call.
 *
 * The stale map is BUILT here: the handle connects to a two-node shard
 * fleet with the pushes ignored (opts.ignore_member_push) and the
 * periodic refresh pushed out to an hour, so only the hint can help it;
 * then a third node joins (the driver starts it on the ADD-NODE cue) and
 * a third of the keyspace changes owner.  Asserted: the handle learns
 * the third member within a handful of requests, it counts the hint, and
 * once it has caught up the fleet forwards nothing more for it.
 * Against a daemon without the hint the handle keeps its two members for
 * the whole run.
 * Usage: movedhinttest host port secret   (see movedhinttest.sh) */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../lib/perfd.h"

static int pass, fail;

static void okf(const char *fmt, ...)
{
	va_list ap;

	printf("ok: ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
	pass++;
}

static void bad(const char *fmt, ...)
{
	va_list ap;

	printf("FAIL: ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
	fail++;
}

static void msleep(int ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

/* cluster.fwd_sent off one node's stats, -1 = unreadable */
static long long fwd_sent(perfd_t *m)
{
	char *r = perfd_command(m, "stats", NULL), *q;
	long long v = -1;

	if (!r)
		return -1;
	q = strstr(r, "\"fwd_sent\":");
	if (q)
		v = atoll(q + 11);
	free(r);
	return v;
}

int main(int argc, char **argv)
{
	const char *sec[2];
	perfd_opts o;
	perfd_t *p, *w = NULL, *m[3] = { NULL, NULL, NULL };
	char key[48], addr[64];
	long long f0 = 0, f1 = 0, v;
	int i, n, conv_at = -1, unreadable = 0;

	if (argc < 4) {
		fprintf(stderr, "usage: %s host port secret\n", argv[0]);
		return 2;
	}
	sec[0] = argv[3];
	sec[1] = NULL;

	/* the handle that will be left behind: it routes, it ignores the
	 * pushes, and its periodic refresh is an hour away */
	memset(&o, 0, sizeof o);
	o.secrets = sec;
	o.spares = -1;
	o.route_keys = 1;
	o.refresh_ms = 3600000;
	o.ignore_member_push = 1;
	o.policy = PERFD_POLICY_FAILOVER;
	p = perfd_connect(argv[1], atoi(argv[2]), &o);
	if (!p) {
		fprintf(stderr, "connect: %s\n", perfd_error(NULL));
		return 2;
	}
	n = perfd_member_count(p);
	if (n == 2 && perfd_routing(p))
		okf("the handle learned the two-node fleet and routes by it");
	else {
		bad("want 2 members and routing on, got %d members, routing %d",
			n, perfd_routing(p));
		goto done;
	}

	/* the fleet changes under it */
	printf("ADD-NODE\n");
	fflush(stdout);
	for (i = 0; i < 100; i++) {
		memset(&o, 0, sizeof o);
		o.secrets = sec;
		o.spares = -1;
		w = perfd_connect(argv[1], atoi(argv[2]), &o);
		if (w && perfd_member_count(w) == 3)
			break;
		perfd_free(w);
		w = NULL;
		msleep(300);
	}
	if (!w) {
		bad("the third node never appeared in a fresh handle's member list");
		goto done;
	}
	msleep(3000);                          /* the reshard settles */
	if (perfd_member_count(p) == 2)
		okf("the fleet has three members; the handle still holds two - "
			"its map is stale, the pushes ignored");
	else
		bad("the handle is not stale (%d members) - the premise failed",
			perfd_member_count(p));

	/* one bare witness per member, as the fresh handle learned them */
	for (i = 0; i < 3; i++) {
		int port = 0;

		if (perfd_member_info(w, i, addr, sizeof addr, &port, NULL,
		        NULL) != 0)
			continue;
		memset(&o, 0, sizeof o);
		o.secrets = sec;
		o.spares = PERFD_SPARES_NONE;
		m[i] = perfd_connect(addr, port, &o);
	}

	/* ---- the stale handle works; how long does it stay stale? ---- */
	for (i = 0; i < 60; i++) {
		if (i == 40) {                     /* the tail starts here */
			int k;

			for (f0 = 0, k = 0; k < 3; k++) {
				v = m[k] ? fwd_sent(m[k]) : -1;
				if (v < 0)
					unreadable++;
				else
					f0 += v;
			}
		}
		snprintf(key, sizeof key, "rv10-%d", i);
		if (perfd_set(p, "c", key, "v", 1, 60) != 0) {
			bad("set %s: %s", key, perfd_error(p));
			goto done;
		}
		if (conv_at < 0 && perfd_member_count(p) == 3)
			conv_at = i;
	}
	for (i = 0; i < 3; i++) {
		v = m[i] ? fwd_sent(m[i]) : -1;
		if (v < 0)
			unreadable++;
		else
			f1 += v;
	}

	if (conv_at >= 0 && conv_at <= 25)
		okf("the stale handle learned the third member after %d "
			"request(s) - no push, no periodic refresh", conv_at + 1);
	else
		bad("the stale handle %s (members now %d)", conv_at < 0
			? "never learned the third member in 60 requests"
			: "took more than 25 requests to catch up",
			perfd_member_count(p));
	if (perfd_moved_hints(p) >= 1)
		okf("it counted the hint that told it (perfd_moved_hints = %llu)",
			perfd_moved_hints(p));
	else
		bad("perfd_moved_hints is 0 - nothing told the handle it was behind");
	if (unreadable)
		bad("%d stats reads failed - the forward count below is partial "
			"and must not be read as a result", unreadable);
	else if (f1 - f0 <= 2)
		okf("and once caught up the fleet forwarded nothing more for it "
			"(fwd_sent +%lld over the last 20 requests)", f1 - f0);
	else
		bad("the fleet still forwarded %lld of the last 20 requests",
			f1 - f0);

done:
	for (i = 0; i < 3; i++)
		perfd_free(m[i]);
	perfd_free(w);
	perfd_free(p);
	printf("movedhinttest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
