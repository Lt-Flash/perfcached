/* routedpairtest.c - S211: the delete behind a routed jset takes the
 * same road as the jset.
 *
 * With per-key routing on, jset/jget/set/get/ttl/expire went to the
 * key's owner over its own link while jdel and jincr went over the
 * primary link.  On an eager fleet the primary is a peer of the owner:
 * the jset was applied and acknowledged at the owner, its push to the
 * primary batched for up to 3 ms, and the jdel arriving at the primary
 * meanwhile found nothing to delete - so it deleted nothing, and the
 * push then put the document on every member.  Deterministic for any
 * key whose owner is not the node the handle dialled (2 of 3 keys on a
 * 3-node fleet), and invisible to anything that puts even a few ms
 * between the two calls - which is why perfcli never saw it.
 *
 * The proof is on EVERY member through per-node handles that do not
 * route, never on the handle under test - the shape that hid S209.
 * Legs: a routed jset then jdel (deleted, absent everywhere for 2.5 s);
 * a routed jset then jincr (2, and 2 on every member); the same pair
 * with routing OFF stays correct; a routed jset alone reaches every
 * member, so "absent" above cannot mean "never arrived".
 * Usage: routedpairtest host port secret   (see routedpairtest.sh) */
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

/* the client port of member @i as the handle learned it, -1 = none */
static int member_port(const perfd_t *p, int i)
{
	int port = 0;

	if (perfd_member_info(p, i, NULL, 0, &port, NULL, NULL) != 0)
		return -1;
	return port;
}

/* the port of the member serving the handle, -1 = none */
static int active_port(const perfd_t *p)
{
	int i, n = perfd_member_count(p);

	for (i = 0; i < n; i++) {
		int port = 0, act = 0;

		if (perfd_member_info(p, i, NULL, 0, &port, NULL, &act) == 0
		        && act)
			return port;
	}
	return -1;
}

/* a key whose owner is NOT the node the handle dialled - the shape that
 * splits the two roads.  0 ok, -1 = routing is off on this handle,
 * -2 = no such key in 200 (impossible on three live members) */
static int pick_key(perfd_t *p, const char *prefix, char *out, size_t cap,
		int *owner_port)
{
	int i, aport = active_port(p);

	for (i = 0; i < 200; i++) {
		int o;

		snprintf(out, cap, "%s-%d", prefix, i);
		o = perfd_owner_of(p, "c", out);
		if (o < 0)
			return -1;
		*owner_port = member_port(p, o);
		if (*owner_port != aport)
			return 0;
	}
	return -2;
}

/* @key on member @m: 1 present, 0 absent, -1 the member did not answer */
static int on_member(perfd_t *m, const char *key)
{
	char *f = NULL;
	int rc = perfd_jget(m, "c", key, "$", &f);

	free(f);
	return rc;
}

/* every member must say absent at every sample across 2.5 s - the push
 * batch flushes at 3 ms and the apply follows, so a resurrection shows
 * by the first sample.  Returns "" or the offending samples. */
static const char *absent_everywhere(perfd_t **m, const char *key)
{
	static char buf[512];
	size_t off = 0;
	int t, i;

	buf[0] = 0;
	for (t = 1; t <= 5; t++) {
		msleep(500);
		for (i = 0; i < 3; i++) {
			int rc = on_member(m[i], key);

			if (rc != 0 && off < sizeof buf - 40)
				off += (size_t)snprintf(buf + off,
					sizeof buf - off, " m%d@%dms=%s", i + 1,
					t * 500, rc > 0 ? "present" : "err");
		}
	}
	return buf;
}

/* the key on every member within 3 s: the ms it took, or -1 */
static int reaches_all(perfd_t **m, const char *key)
{
	int t, i;

	for (t = 0; t < 30; t++) {
		int all = 1;

		for (i = 0; i < 3; i++)
			if (on_member(m[i], key) != 1)
				all = 0;
		if (all)
			return t * 100;
		msleep(100);
	}
	return -1;
}

int main(int argc, char **argv)
{
	const char *sec[2], *r;
	perfd_opts o;
	perfd_t *p = NULL, *q = NULL, *m[3] = { NULL, NULL, NULL };
	int i, n, rc, oport, ms;
	char key[64], addr[64];
	long long nv = 0;

	if (argc < 4) {
		fprintf(stderr, "usage: %s host port secret\n", argv[0]);
		return 2;
	}
	sec[0] = argv[3];
	sec[1] = NULL;

	/* the handle under test: the fleet learned, a standby to every
	 * member, routing ON - the shape libperfd's callers run */
	memset(&o, 0, sizeof o);
	o.secrets = sec;
	o.spares = -1;
	o.route_keys = 1;
	o.policy = PERFD_POLICY_FAILOVER;
	p = perfd_connect(argv[1], atoi(argv[2]), &o);
	if (!p) {
		fprintf(stderr, "routed connect: %s\n", perfd_error(NULL));
		return 2;
	}
	n = perfd_member_count(p);
	if (n != 3) {
		bad("the routed handle learned %d members, not 3", n);
		for (i = 0; i < n; i++) {
			int port = 0, node = 0;

			if (perfd_member_info(p, i, addr, sizeof addr, &port,
			        &node, NULL) == 0)
				printf("  learned: %s:%d node %d (%s)\n", addr, port,
					node, perfd_member_state(p, i, NULL, 0));
		}
		goto done;
	}

	/* the witnesses: one bare handle per member AS THE HANDLE LEARNED
	 * IT, no fleet, no routing - each one reads its node and nothing
	 * else */
	for (i = 0; i < 3; i++) {
		int port = 0;

		if (perfd_member_info(p, i, addr, sizeof addr, &port, NULL,
		        NULL) != 0) {
			bad("member %d is not readable from the handle", i);
			goto done;
		}
		memset(&o, 0, sizeof o);
		o.secrets = sec;
		o.spares = PERFD_SPARES_NONE;
		m[i] = perfd_connect(addr, port, &o);
		if (!m[i]) {
			bad("witness to member %s:%d: %s", addr, port,
				perfd_error(NULL));
			goto done;
		}
	}
	rc = pick_key(p, "s211-del", key, sizeof key, &oport);
	if (rc == -1) {
		bad("routing is off on this handle (perfd_owner_of = -1) - "
			"the fleet did not advertise a contract the library takes");
		goto done;
	}
	if (rc == -2) {
		bad("no key in 200 hashed to a member other than the active one");
		goto done;
	}
	okf("routed handle serves from port %d; %s is owned by port %d",
		active_port(p), key, oport);

	/* ---- leg 1: a routed jset, then jdel with nothing between ---- */
	if (perfd_jset(p, "c", key, "$", "{\"a\":1}", 0) != 0) {
		bad("jset %s: %s", key, perfd_error(p));
		goto done;
	}
	rc = perfd_jdel(p, "c", key, "$");
	if (rc == 1)
		okf("the jdel right behind a routed jset reports a deletion");
	else
		bad("the jdel right behind a routed jset deleted nothing "
			"(rc=%d%s%s)", rc, rc < 0 ? ": " : "",
			rc < 0 ? perfd_error(p) : "");
	r = absent_everywhere(m, key);
	if (!*r)
		okf("the key is absent on all three members, every sample "
			"for 2.5 s");
	else
		bad("the key survived the jdel somewhere:%s", r);

	/* ---- leg 2: a routed jset, then jincr with nothing between ---- */
	if (pick_key(p, "s211-incr", key, sizeof key, &oport) != 0) {
		bad("no routed key for the jincr leg");
		goto done;
	}
	if (perfd_jset(p, "c", key, "$", "{\"n\":1}", 0) != 0) {
		bad("jset %s: %s", key, perfd_error(p));
		goto done;
	}
	rc = perfd_jincr(p, "c", key, "$.n", 1, &nv);
	if (rc == 0 && nv == 2)
		okf("the jincr right behind a routed jset answers 2");
	else
		bad("the jincr right behind a routed jset: rc=%d newval=%lld%s%s",
			rc, nv, rc != 0 ? ": " : "",
			rc != 0 ? perfd_error(p) : "");
	{
		int t, all = 0;

		for (t = 0; t < 30 && !all; t++) {
			all = 1;
			for (i = 0; i < 3; i++) {
				char *f = NULL;

				if (perfd_jget(m[i], "c", key, "$.n", &f) != 1 ||
				        !f || atoll(f) != 2)
					all = 0;
				free(f);
			}
			if (!all)
				msleep(100);
		}
		if (all)
			okf("every member reads n=2 (%d ms)", (t - 1) * 100);
		else
			bad("the members do not all read n=2 after 3 s");
	}

	/* ---- leg 3: the same pair with routing OFF stays correct - both
	 * ops on the dialled node, the peers by S209's tombstone ---- */
	memset(&o, 0, sizeof o);
	o.secrets = sec;
	o.spares = -1;
	o.route_keys = 0;
	o.policy = PERFD_POLICY_FAILOVER;
	q = perfd_connect(argv[1], atoi(argv[2]), &o);
	if (!q) {
		bad("unrouted connect: %s", perfd_error(NULL));
		goto done;
	}
	snprintf(key, sizeof key, "s211-unrouted");
	if (perfd_jset(q, "c", key, "$", "{\"a\":1}", 0) != 0) {
		bad("unrouted jset: %s", perfd_error(q));
		goto done;
	}
	rc = perfd_jdel(q, "c", key, "$");
	r = absent_everywhere(m, key);
	if (rc == 1 && !*r)
		okf("with routing off the pair is deleted everywhere too");
	else
		bad("with routing off: jdel rc=%d, survivors:%s", rc, r);

	/* ---- leg 4: positive control - a routed jset alone reaches every
	 * member, so an "absent" above is not a "never arrived" ---- */
	if (pick_key(p, "s211-ctl", key, sizeof key, &oport) != 0) {
		bad("no routed key for the control leg");
		goto done;
	}
	if (perfd_jset(p, "c", key, "$", "{\"a\":1}", 0) != 0) {
		bad("control jset: %s", perfd_error(p));
		goto done;
	}
	ms = reaches_all(m, key);
	if (ms >= 0)
		okf("positive control: a routed jset alone is on every member "
			"(%d ms)", ms);
	else
		bad("positive control failed: a routed jset alone did not "
			"reach every member in 3 s");

done:
	perfd_free(q);
	perfd_free(p);
	for (i = 0; i < 3; i++)
		perfd_free(m[i]);
	printf("routedpairtest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
