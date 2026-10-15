/* hinttest.c - S295: proxy holder hints.  A handle with route_hints on,
 * connected to node 1 of a three-node proxy fleet whose keys were written
 * through nodes 2 and 3; bare handles on each node for writes and stats.
 *   1  cold: the first read of every key comes back with its holder; the
 *      second goes there - node 1 pulls nothing for it, the holders count
 *      the requests as already-at-holder, every one a hint hit
 *   2  stale: a key moved from node 2 to node 3 behind the handle's back
 *      reads right, by one extra hop, and counts as stale; the next read
 *      lands on node 3
 *   3  ttl: a record with EX 2 is read by hint for 2 s; past its TTL the
 *      entry is dead and the read takes the default way
 *   4  membership: node 3 killed - reads of its keys answer (absent, its
 *      single copies died with it), never an error; node 2's still hit
 * Usage: hinttest host1 host2 host3 port node3-pid */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "perfd.h"

static int pass, fail;
#define OK(c, m) do { if (c) { pass++; printf("  ok   %s\n", m); } else { fail++; printf("  FAIL %s\n", m); } } while (0)

static perfd_t *bare(const char *host, int port)
{
	perfd_opts o;

	memset(&o, 0, sizeof o);
	o.spares = PERFD_SPARES_NONE;
	return perfd_connect(host, port, &o);
}

/* a counter from a node's stats JSON, by name (first occurrence) */
static long long stat(perfd_t *b, const char *name)
{
	char *js = perfd_command(b, "stats", NULL), pat[64], *at;
	long long v = -1;

	if (!js)
		return -1;
	snprintf(pat, sizeof pat, "\"%s\":", name);
	at = strstr(js, pat);
	if (at)
		v = atoll(at + strlen(pat));
	free(js);
	return v;
}

/* get @key: 1 with the value equal to @want, 0 absent, -1 error or wrong */
static int get_is(perfd_t *p, const char *key, const char *want)
{
	void *v;
	size_t vl;
	long long ttl;
	int rc = perfd_get(p, "0", key, &v, &vl, &ttl);

	if (rc != 1)
		return rc;
	rc = want && vl == strlen(want) && !memcmp(v, want, vl) ? 1 : -1;
	free(v);
	return rc;
}

int main(int argc, char **argv)
{
	enum { K = 100 };
	perfd_t *b[3], *c;
	perfd_opts o;
	char key[32], val[32], m[160];
	long long pull0, loc0, hits0, n;
	int i, bad, port;

	if (argc < 6)
		return 2;
	port = atoi(argv[4]);
	for (i = 0; i < 3; i++)
		if (!(b[i] = bare(argv[1 + i], port))) {
			printf("  FAIL cannot reach node %d: %s\n", i + 1, perfd_error(NULL));
			return 1;
		}
	memset(&o, 0, sizeof o);
	o.route_hints = 1;                     /* spares: the default, every member */
	c = perfd_connect(argv[1], port, &o);
	if (!c) {
		printf("  FAIL connect: %s\n", perfd_error(NULL));
		return 1;
	}
	for (i = 0; i < 100 && perfd_spare_count(c) < 2; i++) {
		(void)perfd_ping(c);
		(void)perfd_maintain(c);
		usleep(100000);
	}
	OK(perfd_spare_count(c) == 2, "the hinted handle holds standbys to nodes 2 and 3");

	/* keys written through nodes 2 and 3: proxy places them there */
	for (i = 0, bad = 0; i < K; i++) {
		snprintf(key, sizeof key, "h2_%d", i);
		snprintf(val, sizeof val, "v2_%d", i);
		bad += perfd_set(b[1], "0", key, val, strlen(val), 0) != 0;
		snprintf(key, sizeof key, "h3_%d", i);
		snprintf(val, sizeof val, "v3_%d", i);
		bad += perfd_set(b[2], "0", key, val, strlen(val), 0) != 0;
	}
	OK(!bad, "200 keys written through nodes 2 and 3");

	/* 1. cold, then by hint */
	for (i = 0, bad = 0; i < K; i++) {
		snprintf(key, sizeof key, "h2_%d", i); snprintf(val, sizeof val, "v2_%d", i);
		bad += get_is(c, key, val) != 1;
		snprintf(key, sizeof key, "h3_%d", i); snprintf(val, sizeof val, "v3_%d", i);
		bad += get_is(c, key, val) != 1;
	}
	OK(!bad, "1. first pass through node 1: every value right");
	OK(perfd_hint_hits(c) == 0, "1. no hint hits yet (nothing was known)");
	pull0 = stat(b[0], "pull_sent");
	loc0 = stat(b[1], "holder_hint_local") + stat(b[2], "holder_hint_local");
	for (i = 0, bad = 0; i < K; i++) {
		snprintf(key, sizeof key, "h2_%d", i); snprintf(val, sizeof val, "v2_%d", i);
		bad += get_is(c, key, val) != 1;
		snprintf(key, sizeof key, "h3_%d", i); snprintf(val, sizeof val, "v3_%d", i);
		bad += get_is(c, key, val) != 1;
	}
	OK(!bad, "1. second pass: every value right");
	snprintf(m, sizeof m, "1. second pass: %llu of 200 reads went where the hint said",
		perfd_hint_hits(c));
	OK(perfd_hint_hits(c) == 2 * K, m);
	n = stat(b[0], "pull_sent") - pull0;
	snprintf(m, sizeof m, "1. node 1 pulled nothing for them (pull_sent +%lld)", n);
	OK(n == 0, m);
	n = stat(b[1], "holder_hint_local") + stat(b[2], "holder_hint_local") - loc0;
	snprintf(m, sizeof m, "1. the holders answered them from their own tables (+%lld)", n);
	OK(n == 2 * K, m);
	n = stat(b[0], "holder_hints_sent");
	snprintf(m, sizeof m, "1. node 1 sent a hint with every first-pass answer (%lld)", n);
	OK(n == 2 * K, m);

	/* 2. stale: h2_0 moves to node 3 */
	OK(perfd_del(b[1], "0", "h2_0") == 1 &&
		perfd_set(b[2], "0", "h2_0", "moved", 5, 0) == 0, "2. h2_0 moved from node 2 to node 3");
	/* the move is a delete and a re-create, and a delete leaves node 2 a
	 * tombstone that answers "absent" for tombstone_ms (2 s) without
	 * asking; a rebalancer move leaves none - outlast it */
	sleep(3);
	hits0 = (long long)perfd_hint_hits(c);
	OK(get_is(c, "h2_0", "moved") == 1, "2. the read by the old hint is right");
	OK(perfd_hint_stale(c) == 1, "2. and counted stale (one extra hop)");
	loc0 = stat(b[2], "holder_hint_local");
	OK(get_is(c, "h2_0", "moved") == 1 && (long long)perfd_hint_hits(c) == hits0 + 1 &&
		stat(b[2], "holder_hint_local") == loc0 + 1, "2. the next read lands on node 3");

	/* 3. ttl */
	OK(perfd_set(b[1], "0", "t2", "x", 1, 2) == 0, "3. t2 written through node 2 with EX 2");
	OK(get_is(c, "t2", "x") == 1, "3. first read (cold)");
	hits0 = (long long)perfd_hint_hits(c);
	OK(get_is(c, "t2", "x") == 1 && (long long)perfd_hint_hits(c) == hits0 + 1,
		"3. inside the TTL: by hint");
	sleep(3);
	OK(get_is(c, "t2", "x") == 0 && (long long)perfd_hint_hits(c) == hits0 + 1,
		"3. past the TTL: absent, and not sent by the dead entry");

	/* 4. membership */
	kill((pid_t)atoi(argv[5]), SIGKILL);
	sleep(1);
	for (i = 0, bad = 0; i < 20; i++) {
		snprintf(key, sizeof key, "h3_%d", i);
		bad += get_is(c, key, NULL) < 0;
	}
	OK(!bad, "4. node 3 killed: reads of its keys answer (absent), never an error");
	for (i = 1, bad = 0; i < 21; i++) {
		snprintf(key, sizeof key, "h2_%d", i); snprintf(val, sizeof val, "v2_%d", i);
		bad += get_is(c, key, val) != 1;
	}
	OK(!bad, "4. node 2's keys still read right");
	printf("hinttest: %d passed, %d failed\n", pass, fail);
	perfd_free(c);
	for (i = 0; i < 3; i++)
		perfd_free(b[i]);
	return fail != 0;
}
