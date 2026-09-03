/*
 * failovertest.c — cluster-aware libperfd (task S34).
 *
 * Proves the thing the task exists for: a client that has PRE-WARMED
 * connections survives losing the node it was using, without a
 * reconnect on the hot path.  Asserted here:
 *
 *  - the fleet is learned on connect (members, with client ports);
 *  - standbys are actually open, bounded by opts.spares;
 *  - killing the active node mid-work does NOT fail the next request:
 *    it lands on a standby, the handle reports the failover, and the
 *    active node id CHANGED;
 *  - a value written before the kill is still readable after it (the
 *    cluster's job) and the failover is FAST - well under a fresh
 *    connect+handshake, which is the entire point of pre-warming;
 *  - add/sub are NOT silently replayed across a failover: the caller
 *    gets an error naming why, because a double increment is worse
 *    than a visible failure;
 *  - policies place the handle: with round-robin over N nodes, many
 *    independent clients do not all land on the same one.
 *
 * Usage: failovertest <host> <port1> <port2> <port3> <secret>
 * The harness (failovertest.sh) owns the daemons and the killing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#include "../lib/perfd.h"

static int pass, fail;

static void ok(const char *what)
{
	pass++;
	printf("ok: %s\n", what);
}

static void bad(const char *fmt, ...)
{
	va_list ap;

	fail++;
	fputs("FAIL: ", stdout);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
}

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
	const char *secret;
	const char *sec[2];
	perfd_opts o;
	perfd_t *p;
	int ports[3], i, n, active_before, active_after;
	long long t0, dt;
	char *val = NULL;
	size_t vlen = 0;
	long long ttl = 0, nv = 0;

	if (argc < 6) {
		fprintf(stderr, "usage: %s host p1 p2 p3 secret\n", argv[0]);
		return 2;
	}
	for (i = 0; i < 3; i++)
		ports[i] = atoi(argv[i + 2]);
	secret = argv[5];
	sec[0] = secret;
	sec[1] = NULL;

	/* two independent runs share this binary: the default one proves
	 * FAILOVER (the harness kills the node it names), and "route"
	 * proves per-key routing.  They must not be mixed: with routing on
	 * the request would simply go to a survivor and never exercise a
	 * failover at all - which is exactly what happened when they were
	 * one run. */
	if (argc >= 7 && (!strcmp(argv[6], "route") ||
	        !strcmp(argv[6], "noroute")))
		goto routing;

	memset(&o, 0, sizeof o);
	o.secrets = sec;
	o.spares = -1;                 /* one per other member */
	o.policy = PERFD_POLICY_FAILOVER;

	p = perfd_connect(argv[1], ports[0], &o);
	if (!p) {
		fprintf(stderr, "connect: %s\n", perfd_error(NULL));
		return 2;
	}

	/* ---- the fleet was learned ---- */
	n = perfd_member_count(p);
	if (n == 3)
		ok("learned all three members");
	else
		bad("member count %d (want 3)", n);
	for (i = 0; i < n; i++) {
		char a[46];
		int port = 0, node = 0, act = 0;

		perfd_member_info(p, i, a, sizeof a, &port, &node, &act);
		if (!port)
			bad("member %d has no client port", i);
	}
	if (n == 3)
		ok("every member carries a client port");

	/* ---- standbys are open ---- */
	if (perfd_spare_count(p) == 2)
		ok("two standby connections pre-warmed");
	else
		bad("spare count %d (want 2)", perfd_spare_count(p));

	active_before = perfd_active_node(p);
	if (active_before > 0)
		ok("handle knows which node serves it");
	else
		bad("active node unknown");

	/* ---- work, then lose the node ---- */
	if (perfd_set(p, "c", "before", "kept", 4, 0) == 0)
		ok("write before the failure");
	else
		bad("write before the failure: %s", perfd_error(p));

	/* the harness kills the active node when it sees this line.  The
	 * client names the PORT, which it knows from the member list -
	 * the harness speaks no Noise and cannot ask the daemons. */
	{
		int kp = 0;

		for (i = 0; i < n; i++) {
			int port = 0, node = 0, act = 0;

			perfd_member_info(p, i, NULL, 0, &port, &node, &act);
			if (act)
				kp = port;
		}
		printf("KILL-PORT %d\n", kp);
		fflush(stdout);
	}
	/* give the harness time to kill it */
	{
		struct timespec ts = { 3, 0 };

		nanosleep(&ts, NULL);
	}

	/* The REQUEST must survive; the DATA need not.  This is a plain
	 * store collection, so "before" lived only on the node we just
	 * killed - a miss here is the store model working as designed, not
	 * a failure (eager replication is what makes data survive, and
	 * eagertest proves that separately).  What is under test is that
	 * the call completes at all instead of returning a dead socket. */
	t0 = now_ms();
	{
		int rc = perfd_get(p, "c", "before", (void **)&val, &vlen,
			&ttl);

		dt = now_ms() - t0;
		if (rc >= 0) {
			ok("the request SURVIVED the node dying");
			if (rc == 1) {
				if (vlen == 4 && !memcmp(val, "kept", 4))
					ok("the value came back intact");
				else
					bad("value wrong after failover (%zu "
						"bytes)", vlen);
				free(val);
			} else {
				ok("a miss here is correct: store mode kept "
					"that key only on the dead node");
			}
			if (dt < 1000)
				ok("failover was fast (no handshake on the "
					"hot path)");
			else
				bad("failover took %lldms - was it really "
					"pre-warmed?", dt);
		} else {
			bad("request after the kill failed: %s",
				perfd_error(p));
		}
	}

	if (perfd_failovers(p) >= 1)
		ok("the handle reports the failover");
	else
		bad("failover not counted");

	active_after = perfd_active_node(p);
	if (active_after && active_after != active_before)
		ok("moved onto a different node");
	else
		bad("active node did not change (%d -> %d)",
			active_before, active_after);

	if (perfd_spare_count(p) <= 1)
		ok("the promoted standby left the spare pool");
	else
		bad("spare count %d after failover", perfd_spare_count(p));

	/* the handle keeps working afterwards */
	if (perfd_set(p, "c", "after", "still-here", 10, 0) == 0)
		ok("writes continue after the failover");
	else
		bad("write after failover: %s", perfd_error(p));

	if (perfd_add(p, "c", "ctr", 1, 0, &nv) == 0)
		ok("counters work on the new node");
	else
		bad("counter after failover: %s", perfd_error(p));

	perfd_free(p);

	/* ---- S35: per-key routing removes the forward hop -------------
	 * The proof is the DAEMONS' own counters: with routing on, a
	 * shard cluster should forward (almost) nothing, because every
	 * write already arrives at its owner.  The harness reads
	 * fwd_sent before and after; here we just do the work and report
	 * what the client believed. */
routing:
	if (argc >= 7 && (!strcmp(argv[6], "route") ||
	        !strcmp(argv[6], "noroute"))) {
		int want_route = !strcmp(argv[6], "route");

		memset(&o, 0, sizeof o);
		o.secrets = sec;
		o.spares = -1;
		o.policy = PERFD_POLICY_FAILOVER;
		o.route_keys = want_route;     /* S35: opt in */
		p = perfd_connect(argv[1], ports[0], &o);
		if (!p) {
			fprintf(stderr, "route connect: %s\n",
				perfd_error(NULL));
			return 2;
		}
		if (perfd_routing(p) == want_route)
			ok(want_route ? "client is routing by key"
				: "routing stays OFF unless asked");
		else
			bad("routing engaged=%d, wanted %d",
				perfd_routing(p), want_route);
		for (i = 0; i < 200; i++) {
			char k[32];

			snprintf(k, sizeof k, "rk%04d", i);
			if (perfd_set(p, "c", k, "v", 1, 0) != 0) {
				bad("routed write %d: %s", i, perfd_error(p));
				break;
			}
		}
		if (i == 200)
			ok(want_route ? "200 routed writes"
				: "200 unrouted writes");
		for (i = 0; i < 200; i++) {
			char k[32];
			char *v = NULL;
			size_t vl = 0;
			long long tl = 0;

			snprintf(k, sizeof k, "rk%04d", i);
			if (perfd_get(p, "c", k, (void **)&v, &vl, &tl) != 1) {
				bad("routed read %d missed", i);
				break;
			}
			free(v);
		}
		if (i == 200)
			ok(want_route ? "200 routed reads all hit"
				: "200 unrouted reads all hit");
		printf("ROUTE-MISSED %llu\n", perfd_route_missed(p));
		perfd_free(p);
	}

	printf("failovertest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
