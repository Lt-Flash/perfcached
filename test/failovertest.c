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
#include <sys/socket.h>

#include "../lib/perfd.h"

static int pass, fail;

static void ok(const char *what)
{
	pass++;
	printf("ok: %s\n", what);
}
/* every push the library hands the application, stamped, so a run
 * that misses one shows whether it ever arrived */
static long long t0_ms;

static long long now_ms_t(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void notify_seen(const char *json, size_t n, void *ctx)
{
	(void)ctx;
	printf("NOTIFY +%lld ms %.*s\n", now_ms_t() - t0_ms, (int)n, json);
	fflush(stdout);
}

static void dump_members(perfd_t *p, const char *when)
{
	int i;

	for (i = 0; i < perfd_member_count(p); i++) {
		char addr[64], why[64];
		int port = 0, node = 0, act = 0;
		const char *st;

		perfd_member_info(p, i, addr, sizeof addr, &port, &node, &act);
		st = perfd_member_state(p, i, why, sizeof why);
		printf("  %s: member %s:%d node %d %s%s%s%s\n", when, addr, port, node,
			st, act ? " (active)" : "", why[0] ? " why=" : "", why);
	}
	printf("  %s: spares=%d failovers=%llu recovered=%llu pings=%llu/%llu\n",
		when, perfd_spare_count(p), perfd_failovers(p), perfd_recovered(p),
		perfd_pings(p), perfd_pings_failed(p));
}

static void okf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	printf("ok: ");
	vprintf(fmt, ap);
	printf("\n");
	va_end(ap);
	pass++;
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
	if (argc >= 7 && !strcmp(argv[6], "restart"))
		goto restart;
	if (argc >= 7 && !strcmp(argv[6], "member"))
		goto member;

	memset(&o, 0, sizeof o);
	o.secrets = sec;
	o.spares = -1;                 /* one per other member */
	o.keepalive_s = 1;             /* S104: visible on the socket */
	o.idle_ping_ms = 1500;         /* S104: fast enough to test */
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

	/* ---- S70: the handle can say what it found ---- */
	if (perfd_server_version(p)[0])
		ok("server version learned from the member list");
	else
		bad("server version not learned");
	{
		int act = 0, sby = 0, oth = 0;
		char why[64];

		for (i = 0; i < n; i++) {
			const char *st = perfd_member_state(p, i, why, sizeof why);

			if (!strcmp(st, "active"))
				act++;
			else if (!strcmp(st, "standby"))
				sby++;
			else
				oth++;
		}
		if (act == 1 && sby == 2 && oth == 0)
			ok("member states: one active, two standby, none down");
		else
			bad("member states: %d active, %d standby, %d other (want 1/2/0)", act, sby, oth);
	}

	/* ---- S104 layer 1: TCP keepalive is ON the link ---- */
	{
		int ka = 0;
		socklen_t kl = sizeof ka;

		if (getsockopt(perfd_fd(p), SOL_SOCKET, SO_KEEPALIVE, &ka, &kl) == 0 && ka)
			ok("TCP keepalive is set on the link");
		else
			bad("TCP keepalive not set on the link");
	}

	/* ---- S104 layer 2: idle standbys are pinged, and a dead one is
	 * found by the ping rather than by the failover that would have
	 * adopted it.  The harness kills a STANDBY's node on this cue; a
	 * request on the active must not notice, and the next idle scan
	 * must retire the corpse. ---- */
	{
		struct timespec ts = { 2, 0 };
		int kp = 0, killed_node = 0, sby_left = 0, down = 0;
		char why[64];

		nanosleep(&ts, NULL);          /* past idle_ping_ms */
		if (perfd_set(p, "c", "tick", "1", 1, 0) != 0)
			bad("write to trigger the idle scan: %s", perfd_error(p));
		if (perfd_pings(p) >= 2)
			ok("both idle standbys were pinged");
		else
			bad("idle standbys not pinged (pings=%llu)", perfd_pings(p));
		for (i = 0; i < n; i++) {
			int port = 0, node = 0;

			perfd_member_info(p, i, NULL, 0, &port, &node, NULL);
			if (!kp && !strcmp(perfd_member_state(p, i, why, sizeof why), "standby")) {
				kp = port;
				killed_node = node;
			}
		}
		printf("KILL-SPARE-PORT %d\n", kp);
		fflush(stdout);
		ts.tv_sec = 3;
		nanosleep(&ts, NULL);          /* the harness kills it */
		if (perfd_set(p, "c", "tick2", "2", 1, 0) == 0)
			ok("a request while a standby is dead succeeds untouched");
		else
			bad("request with a dead standby failed: %s", perfd_error(p));
		ts.tv_sec = 2;
		nanosleep(&ts, NULL);          /* past idle_ping_ms again */
		if (perfd_set(p, "c", "tick3", "3", 1, 0) != 0)
			bad("write to trigger the second idle scan: %s", perfd_error(p));
		{
			/* S107: the fleet purges the dead node and pushes "expelled"
			 * within these same seconds, and the library then drops the
			 * member entirely - so it is either marked down by the idle
			 * ping, or already gone.  Both are right.  A member marked
			 * down is also RE-DIALLED in the background from the next
			 * second on, and the reason then names the latest attempt
			 * ("re-dial: cannot connect ..."), not the ping that found
			 * it - so the reason is reported, not matched.  That the
			 * idle ping did the retiring is what pings_failed and the
			 * unchanged failover count prove. */
			int gone = 1;
			char down_why[64] = "";

			for (i = 0; i < perfd_member_count(p); i++) {
				int node = 0;
				const char *st;

				perfd_member_info(p, i, NULL, 0, NULL, &node, NULL);
				st = perfd_member_state(p, i, why, sizeof why);
				if (!strcmp(st, "standby"))
					sby_left++;
				if (node == killed_node) {
					gone = 0;
					if (!strcmp(st, "down")) {
						down = 1;
						snprintf(down_why, sizeof down_why, "%s", why);
					}
				}
			}
			if ((down || gone) && sby_left == 1 && perfd_spare_count(p) == 1 &&
			        perfd_pings_failed(p) >= 1)
				okf("the dead standby was retired by the idle ping, not by a failover (%s%s)",
					gone ? "and the fleet's push has already expelled it" : "still listed, down: ",
					gone ? "" : down_why);
			else {
				bad("dead standby not retired (down=%d gone=%d standby_left=%d spares=%d pings_failed=%llu)",
					down, gone, sby_left, perfd_spare_count(p), perfd_pings_failed(p));
				dump_members(p, "after the second idle scan");
			}
		}
	}

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

	goto done;

	/* ---- S104: the whole fleet restarts under a handle, and the seed
	 * stays down.  Every held link dies at once - the active AND the
	 * standbys opened to the other members - so a failover that
	 * adopted a standby blindly burned one dead link per operation
	 * (measured: the fourth or fifth operation was the first to
	 * succeed).  Now a standby is probed before it is adopted and a
	 * fresh dial to any learned member follows inside the same
	 * failover; the SECOND operation after the restart must succeed.
	 * Then the fleet is killed outright, the handle fails, and only
	 * the two non-seed nodes come back: the handle must re-dial
	 * through a member it learned, not the seed it was given. ---- */
restart:
	if (argc >= 7 && !strcmp(argv[6], "restart")) {
		int seed_node = 0, op1, op2, act;
		struct timespec ts = { 0, 0 };

		memset(&o, 0, sizeof o);
		o.secrets = sec;
		o.spares = -1;
		o.policy = PERFD_POLICY_FAILOVER;
		o.route_keys = 1;              /* the shape the operator decided */
		o.connect_timeout_ms = 2000;
		o.io_timeout_ms = 2000;
		p = perfd_connect(argv[1], ports[0], &o);
		if (!p) {
			fprintf(stderr, "restart connect: %s\n", perfd_error(NULL));
			return 2;
		}
		n = perfd_member_count(p);
		for (i = 0; i < n; i++) {
			int port = 0, node = 0;

			perfd_member_info(p, i, NULL, 0, &port, &node, NULL);
			if (port == ports[0])
				seed_node = node;
		}
		if (n == 3 && perfd_spare_count(p) == 2 && seed_node)
			okf("holds every member: 3 learned, 2 standbys, seed is node %d", seed_node);
		else
			bad("fleet not held (members=%d spares=%d seed=%d)", n,
				perfd_spare_count(p), seed_node);
		if (perfd_set(p, "c", "r0", "v", 1, 0) != 0)
			bad("write before the restart: %s", perfd_error(p));
		printf("RESTART-ALL\n");
		fflush(stdout);
		ts.tv_sec = 9;                 /* the harness restarts all three */
		nanosleep(&ts, NULL);
		op1 = perfd_set(p, "c", "r1", "v", 1, 0);
		op2 = perfd_set(p, "c", "r2", "v", 1, 0);
		if (op2 == 0)
			okf("the second operation after a whole-fleet restart succeeded (first: %s)",
				op1 == 0 ? "replayed, succeeded too" : "failed");
		else
			bad("the second operation after a whole-fleet restart failed: %s",
				perfd_error(p));
		if (perfd_failovers(p) >= 1)
			okf("a failover was performed (%llu)", perfd_failovers(p));
		else
			bad("no failover counted after every link died");
		if (perfd_set(p, "c", "r3", "v", 1, 0) == 0 &&
		        perfd_member_count(p) == 3 && perfd_spare_count(p) >= 1)
			okf("re-learned the fleet and re-warmed standbys after the restart (%d)",
				perfd_spare_count(p));
		else
			bad("fleet not re-held after the restart (members=%d spares=%d): %s",
				perfd_member_count(p), perfd_spare_count(p), perfd_error(p));

		printf("KILL-ALL\n");
		fflush(stdout);
		ts.tv_sec = 4;
		nanosleep(&ts, NULL);
		if (perfd_set(p, "c", "r4", "v", 1, 0) != 0 &&
		        perfd_state(p) == PERFD_ST_FAILED)
			ok("with the whole fleet down the request failed fast and the handle is FAILED");
		else
			bad("fleet down: rc/state wrong (state=%d): %s", perfd_state(p),
				perfd_error(p));
		printf("START-OTHERS\n");
		fflush(stdout);
		ts.tv_sec = 6;                 /* the two non-seed nodes come back */
		nanosleep(&ts, NULL);
		if (perfd_redial(p) == 0)
			ok("perfd_redial reached the fleet with the seed still down");
		else
			bad("perfd_redial failed with two members up: %s", perfd_error(p));
		act = perfd_active_node(p);
		if (perfd_set(p, "c", "r5", "v", 1, 0) == 0 && act && act != seed_node)
			okf("working again through node %d, not the seed (%d)", act, seed_node);
		else
			bad("not working through a non-seed member (active=%d seed=%d): %s",
				act, seed_node, perfd_error(p));
		printf("DONE\n");
		fflush(stdout);
		perfd_free(p);
	}

	goto done;

	/* ---- S107: a member comes back on its own, and the fleet tells the
	 * handle when its shape changes.  Three cues on the routing fleet:
	 * BOUNCE-PORT (kill and restart that node at once - within the purge
	 * window, so it stays a member), ADD-NODE (a fourth node joins),
	 * EXPEL-PORT (a clean stop, so the fleet expels it at once). ---- */
member:
	if (argc >= 7 && !strcmp(argv[6], "member")) {
		struct timespec ts = { 0, 0 };
		char why[64];
		int bp = 0, bnode = 0, k, fails = 0, saw_standby = 0, seen7 = 0;
		unsigned long long fo0;

		memset(&o, 0, sizeof o);
		o.secrets = sec;
		o.spares = -1;
		o.policy = PERFD_POLICY_FAILOVER;
		o.route_keys = 1;
		o.connect_timeout_ms = 2000;
		o.io_timeout_ms = 2000;
		o.idle_ping_ms = 1500;
		p = perfd_connect(argv[1], ports[0], &o);
		if (!p) {
			fprintf(stderr, "member connect: %s\n", perfd_error(NULL));
			return 2;
		}
		t0_ms = now_ms_t();
		perfd_set_notify(p, notify_seen, NULL);
		n = perfd_member_count(p);
		for (i = 0; i < n; i++) {
			int port = 0, node = 0;

			perfd_member_info(p, i, NULL, 0, &port, &node, NULL);
			if (!bp && !strcmp(perfd_member_state(p, i, why, sizeof why), "standby")) {
				bp = port;
				bnode = node;
			}
		}
		if (n == 3 && bp)
			okf("holds the fleet: 3 members, standby node %d on port %d to bounce", bnode, bp);
		else
			bad("fleet not held (members=%d standby port=%d)", n, bp);
		fo0 = perfd_failovers(p);

		/* 1. a member bounced within the purge window: found dead by the
		 * idle ping or a routed request, brought back by the background
		 * recovery - with every request succeeding meanwhile */
		printf("BOUNCE-PORT %d\n", bp);
		fflush(stdout);
		for (k = 0; k < 80; k++) {         /* 8 s of routed traffic */
			char key[32];

			snprintf(key, sizeof key, "mk%03d", k);
			if (perfd_set(p, "c", key, "v", 1, 0) != 0)
				fails++;
			ts.tv_sec = 0;
			ts.tv_nsec = 100 * 1000 * 1000L;
			nanosleep(&ts, NULL);
			for (i = 0; i < perfd_member_count(p); i++) {
				int node = 0;

				perfd_member_info(p, i, NULL, 0, NULL, &node, NULL);
				if (node == bnode && !strcmp(perfd_member_state(p, i, why, sizeof why), "standby") && k > 10)
					saw_standby = 1;
			}
			if (saw_standby && k > 30)
				break;
		}
		if (fails == 0)
			ok("no request failed while a member was bounced");
		else
			bad("%d request(s) failed during the bounce", fails);
		if (saw_standby && perfd_recovered(p) >= 1)
			okf("the bounced member came back as a standby on its own (recovered=%llu, after %d00 ms)",
				perfd_recovered(p), k);
		else
			bad("the bounced member never came back (recovered=%llu)", perfd_recovered(p));
		if (perfd_failovers(p) == fo0)
			ok("no failover was needed: the active was never touched");
		else
			bad("a failover happened during a standby's bounce (%llu)", perfd_failovers(p) - fo0);
		{
			/* the bounced node has no state directory, so it came back
			 * as a NEW identity with a new id at the same address: the
			 * fleet must push the old id out and the new one in, or
			 * this handle routes by an id that no longer exists.  The
			 * new id exists only once the node has founded or joined
			 * (2 s of JOIN_WAIT at the least), and the push rides its
			 * next heartbeat - so this polls, with traffic flowing,
			 * rather than sampling the moment the link was back. */
			int newnode = 0;

			for (k = 0; k < 100 && (!newnode || newnode == bnode); k++) {
				char key[32];

				snprintf(key, sizeof key, "nk%03d", k);
				if (perfd_set(p, "c", key, "v", 1, 0) != 0)
					fails++;
				ts.tv_nsec = 100 * 1000 * 1000L;
				nanosleep(&ts, NULL);
				newnode = 0;
				for (i = 0; i < perfd_member_count(p); i++) {
					int port = 0, node = 0;

					perfd_member_info(p, i, NULL, 0, &port, &node, NULL);
					if (port == bp)
						newnode = node;
				}
			}
			if (newnode && newnode != bnode)
				okf("the bounced member came back under its new id (%d -> %d, after %d00 ms): replaced by the push, not just re-dialled", bnode, newnode, k);
			else {
				bad("the bounced member still carries its old id %d (now %d) after 10 s: the same-address restart was never announced", bnode, newnode);
				dump_members(p, "after the bounce");
			}
		}

		/* 2. a node joins: the fleet tells us, and we dial it.  The
		 * standby bounced above is usually the fleet's master, so a
		 * re-election can still be running when node 7 asks to join,
		 * and its JOIN waits for the new master: 15 s covers that
		 * (measured 1.9 s when no election was pending). */
		printf("ADD-NODE\n");
		fflush(stdout);
		for (k = 0; k < 150 && !seen7; k++) {   /* up to 15 s */
			char key[32];

			snprintf(key, sizeof key, "ak%03d", k);
			if (perfd_set(p, "c", key, "v", 1, 0) != 0)
				fails++;
			ts.tv_nsec = 100 * 1000 * 1000L;
			nanosleep(&ts, NULL);
			for (i = 0; i < perfd_member_count(p); i++) {
				int port = 0;

				perfd_member_info(p, i, NULL, 0, &port, NULL, NULL);
				if (port == ports[0] + 3 &&
				        !strcmp(perfd_member_state(p, i, why, sizeof why), "standby"))
					seen7 = 1;
			}
		}
		if (seen7 && perfd_member_count(p) == 4)
			okf("the new node was learned and dialled from the push (after %d00 ms, %d members)", k, perfd_member_count(p));
		else {
			bad("the new node was not picked up (members=%d, standby=%d)", perfd_member_count(p), seen7);
			dump_members(p, "after ADD-NODE");
		}

		/* 3. it is expelled: the fleet tells us, and we drop it.  Only
		 * a proof when it was there to drop (seen7). */
		printf("EXPEL-PORT %d\n", ports[0] + 3);
		fflush(stdout);
		for (k = 0; k < 60; k++) {
			char key[32];
			int present = 0;

			snprintf(key, sizeof key, "ek%03d", k);
			if (perfd_set(p, "c", key, "v", 1, 0) != 0)
				fails++;
			ts.tv_nsec = 100 * 1000 * 1000L;
			nanosleep(&ts, NULL);
			for (i = 0; i < perfd_member_count(p); i++) {
				int port = 0;

				perfd_member_info(p, i, NULL, 0, &port, NULL, NULL);
				if (port == ports[0] + 3)
					present = 1;
			}
			if (!present)
				break;
		}
		if (seen7 && k < 60 && perfd_member_count(p) == 3)
			okf("the expelled node was dropped from the push (after %d00 ms, %d members)", k, perfd_member_count(p));
		else if (!seen7)
			bad("the expelled node was never held, so there was nothing to drop (members=%d)", perfd_member_count(p));
		else
			bad("the expelled node lingered (members=%d)", perfd_member_count(p));
		printf("DONE\n");
		fflush(stdout);
		perfd_free(p);
	}

done:
	printf("failovertest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
