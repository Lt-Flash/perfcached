/*
 * libtest.c — libperfd end to end against live daemons (booted by
 * test/libtest.sh): every typed verb round-trips, binary values ride
 * the b64 leg transparently, errors surface with messages, the
 * pipeline delivers in request order, and the secret LIST tries in
 * order (rotation).  Usage: libtest <pt-port> <nx-port> <secret>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../lib/perfd.h"

static int pass, fail;

#define OK(cond, name) do { \
	if (cond) { pass++; } \
	else { fail++; printf("FAIL: %s\n", name); } \
} while (0)

int main(int argc, char **argv)
{
	int ptport = argc > 1 ? atoi(argv[1]) : 6479;
	int nxport = argc > 2 ? atoi(argv[2]) : 0;
	const char *secret = argc > 3 ? argv[3] : NULL;
	perfd_t *p;
	void *val;
	size_t vlen;
	long long ttl, nv;
	char *r, **keys;
	int rc, i;

	/* ---- plaintext ------------------------------------------------ */
	p = perfd_connect("127.0.0.1", ptport, NULL);
	OK(p != NULL, "connect plaintext");
	if (!p) {
		printf("connect: %s\n", perfd_error(NULL));
		return 1;
	}
	OK(perfd_ping(p) == 0, "ping");

	OK(perfd_set(p, "c", "k1", "hello", 5, 500) == 0, "set");
	rc = perfd_get(p, "c", "k1", &val, &vlen, &ttl);
	OK(rc == 1 && vlen == 5 && !memcmp(val, "hello", 5), "get value");
	OK(ttl > 400 && ttl <= 500, "get ttl");
	if (rc == 1)
		free(val);

	/* binary value with embedded NULs: the b64 leg must be invisible */
	{
		const char bin[] = { 'a', 0, 1, 2, (char)0xff, 0, 'z' };

		OK(perfd_set(p, "c", "bin", bin, sizeof bin, 0) == 0,
			"set binary");
		rc = perfd_get(p, "c", "bin", &val, &vlen, &ttl);
		OK(rc == 1 && vlen == sizeof bin &&
			!memcmp(val, bin, sizeof bin), "get binary");
		OK(ttl == -1, "no-expiry ttl is -1");
		if (rc == 1)
			free(val);
	}

	/* a 50KB binary value: the wire's b64 form must have headroom
	 * (the ceiling the PHP client caught: b64 of 50KB > 64KB) */
	{
		size_t bl = 50000, bi;
		char *big = malloc(bl);

		for (bi = 0; bi < bl; bi++)
			big[bi] = (char)(bi * 131 + 7);
		OK(perfd_set(p, "c", "big", big, bl, 0) == 0, "set 50KB bin");
		rc = perfd_get(p, "c", "big", &val, &vlen, NULL);
		OK(rc == 1 && vlen == bl && !memcmp(val, big, bl),
			"get 50KB bin bit-exact");
		if (rc == 1)
			free(val);
		free(big);
	}

	OK(perfd_get(p, "c", "nokey", &val, &vlen, NULL) == 0, "get miss");
	OK(perfd_exists(p, "c", "k1") == 1, "exists");
	OK(perfd_exists(p, "c", "nokey") == 0, "not exists");
	OK(perfd_ttl(p, "c", "nokey") == -2, "ttl absent = -2");
	OK(perfd_expire(p, "c", "k1", 900) == 0, "expire");
	OK(perfd_ttl(p, "c", "k1") > 800, "expire took");
	OK(perfd_expire(p, "c", "nokey", 5) == 1, "expire absent = 1");
	OK(perfd_del(p, "c", "k1") == 1, "del");
	OK(perfd_del(p, "c", "k1") == 0, "del absent = 0");

	OK(perfd_add(p, "c", "ctr", 5, 0, &nv) == 0 && nv == 5, "add 5");
	OK(perfd_sub(p, "c", "ctr", 2, &nv) == 0 && nv == 3, "sub 2");

	/* mget: 2 present, 1 absent */
	OK(perfd_set(p, "c", "m1", "v1", 2, 0) == 0, "set m1");
	OK(perfd_set(p, "c", "m2", "v2", 2, 0) == 0, "set m2");
	{
		const char *const ks[] = { "m1", "gone", "m2" };
		void *vals[3];
		size_t lens[3];

		rc = perfd_mget(p, "c", ks, 3, vals, lens);
		OK(rc == 0, "mget");
		OK(vals[0] && lens[0] == 2 && !memcmp(vals[0], "v1", 2),
			"mget[0]");
		OK(vals[1] == NULL, "mget miss NULL");
		OK(vals[2] && !memcmp(vals[2], "v2", 2), "mget[2]");
		free(vals[0]);
		free(vals[2]);
	}

	rc = perfd_keys(p, "c", "m*", 0, &keys);
	OK(rc == 2, "keys count");
	perfd_free_keys(keys, rc > 0 ? rc : 0);

	/* JSON path verbs */
	OK(perfd_jset(p, "c", "doc", "$", "{\"n\":1,\"s\":\"x\"}", 0) == 0,
		"jset root");
	OK(perfd_jincr(p, "c", "doc", "$.n", 4, &nv) == 0 && nv == 5,
		"jincr");
	rc = perfd_jget(p, "c", "doc", "$.s", &r);
	OK(rc == 1 && !strcmp(r, "\"x\""), "jget fragment");
	if (rc == 1)
		free(r);
	OK(perfd_jdel(p, "c", "doc", "$.s") == 1, "jdel");
	OK(perfd_jdel(p, "c", "doc", "$.s") == 0, "jdel absent");

	/* the escape hatch */
	r = perfd_command(p, "stats", NULL);
	OK(r && strstr(r, "\"arena_total\""), "command stats");
	free(r);

	/* a server error surfaces with its message */
	r = perfd_command(p, "get", "{\"col\":\"nosuch\",\"key\":\"x\"}");
	OK(r == NULL && strstr(perfd_error(p), "collection"),
		"error message surfaced");

	/* ---- pipeline: order kept over 60 mixed requests --------------- */
	for (i = 0; i < 20; i++) {
		char ps[128];

		snprintf(ps, sizeof ps,
			"{\"col\":\"c\",\"key\":\"pl%02d\",\"value\":\"v%02d\"}",
			i, i);
		OK(perfd_append(p, "set", ps) == 0, "append set");
	}
	for (i = 0; i < 20; i++) {
		char ps[64];

		snprintf(ps, sizeof ps, "{\"col\":\"c\",\"key\":\"pl%02d\"}",
			i);
		OK(perfd_append(p, "get", ps) == 0, "append get");
	}
	OK(perfd_pending(p) == 40, "pending count");
	OK(perfd_flush(p) == 0, "flush");
	for (i = 0; i < 20; i++) {
		r = perfd_next_reply(p);
		OK(r && strstr(r, "\"stored\":true"), "pipe set reply");
		free(r);
	}
	for (i = 0; i < 20; i++) {
		char want[16];

		snprintf(want, sizeof want, "\"v%02d\"", i);
		r = perfd_next_reply(p);
		OK(r && strstr(r, want), "pipe get IN ORDER");
		free(r);
	}
	OK(perfd_pending(p) == 0, "pipeline drained");

	/* typed call refused mid-pipeline */
	OK(perfd_append(p, "ping", NULL) == 0, "append one more");
	OK(perfd_ping(p) == -1 &&
		strstr(perfd_error(p), "pipeline"), "typed refused mid-pipe");
	OK(perfd_flush(p) == 0, "flush last");
	r = perfd_next_reply(p);
	OK(r != NULL, "last reply");
	free(r);
	perfd_free(p);

	/* ---- the binary dialect (opts.binary=1) ------------------------ */
	{
		perfd_opts ob = { .binary = 1 };
		unsigned char blob[300];

		for (i = 0; i < (int)sizeof blob; i++)
			blob[i] = (unsigned char)(i * 7);      /* NULs included */
		p = perfd_connect("127.0.0.1", ptport, &ob);
		OK(p != NULL, "bin connect");
		if (p) {
			OK(perfd_ping(p) == 0, "bin ping");
			OK(perfd_set(p, "c", "bk", blob, sizeof blob, 0) == 0,
				"bin set (raw bytes)");
			rc = perfd_get(p, "c", "bk", &val, &vlen, &ttl);
			OK(rc == 1 && vlen == sizeof blob &&
				!memcmp(val, blob, sizeof blob), "bin get exact");
			OK(rc == 1 && ttl == -1, "bin get no-ttl");
			if (rc == 1)
				free(val);
			OK(perfd_get(p, "c", "bnope", &val, &vlen, NULL) == 0,
				"bin get miss");
			OK(perfd_exists(p, "c", "bk") == 1, "bin exists");
			OK(perfd_expire(p, "c", "bk", 500) == 0, "bin expire");
			ttl = perfd_ttl(p, "c", "bk");
			OK(ttl > 490 && ttl <= 500, "bin ttl");
			OK(perfd_ttl(p, "c", "bnope") == -2, "bin ttl absent");
			OK(perfd_add(p, "c", "bctr", 41, 0, &nv) == 0 && nv == 41,
				"bin add");
			OK(perfd_sub(p, "c", "bctr", 40, &nv) == 0 && nv == 1,
				"bin sub");
			OK(perfd_add(p, "c", "bk", 1, 0, &nv) == -1 &&
				strstr(perfd_error(p), "integer"),
				"bin add non-integer errs");
			OK(perfd_del(p, "c", "bk") == 1, "bin del");
			OK(perfd_del(p, "c", "bk") == 0, "bin del absent");
			/* text verbs interleave on the SAME connection */
			r = perfd_command(p, "stats", NULL);
			OK(r && strstr(r, "\"hits\""), "text stats on bin conn");
			/* S76: this connection was sniffed as BINARY at its first
			 * byte and has carried 13 binary frames so far (the text
			 * stats above rides the same connection); the daemon must
			 * have counted both.  The stats reply is in hand. */
			{
				const char *b = r ? strstr(r, "\"binary\":{\"conns\":") : NULL;
				const char *q = b ? strstr(b, "\"requests\":") : NULL;

				OK(b && atoi(b + 18) >= 1, "S76: a binary connection was counted");
				OK(q && atoi(q + 11) >= 13, "S76: binary requests were counted");
			}
			free(r);
			OK(perfd_set(p, "c", "bk2", "x", 1, 0) == 0,
				"bin set after text");
			OK(perfd_del(p, "c", "bk2") == 1, "bin del after text");
			perfd_free(p);
		}
	}

	/* ---- the Noise leg + secret-list rotation ---------------------- */
	if (nxport && secret) {
		const char *wrong_then_right[] = { "not-the-secret", secret,
			NULL };
		const char *wrong_only[] = { "not-the-secret", NULL };
		perfd_opts o = { .secrets = wrong_then_right };

		p = perfd_connect("127.0.0.1", nxport, &o);
		OK(p != NULL, "rotation: second secret wins");
		if (p) {
			OK(perfd_set(p, "c", "nk", "nv", 2, 0) == 0,
				"noise set");
			rc = perfd_get(p, "c", "nk", &val, &vlen, NULL);
			OK(rc == 1 && vlen == 2 && !memcmp(val, "nv", 2),
				"noise get");
			if (rc == 1)
				free(val);
			perfd_free(p);
		}
		o.secrets = wrong_only;
		p = perfd_connect("127.0.0.1", nxport, &o);
		OK(p == NULL && strstr(perfd_error(NULL), "handshake"),
			"wrong-only secrets refused");
		if (p)
			perfd_free(p);

		/* binary frames inside the Noise transport */
		o.secrets = wrong_then_right;
		o.binary = 1;
		p = perfd_connect("127.0.0.1", nxport, &o);
		OK(p != NULL, "bin+noise connect");
		if (p) {
			OK(perfd_set(p, "c", "bnk", "\x00\x9E\n", 3, 0) == 0,
				"bin+noise set");
			rc = perfd_get(p, "c", "bnk", &val, &vlen, NULL);
			OK(rc == 1 && vlen == 3 &&
				!memcmp(val, "\x00\x9E\n", 3),
				"bin+noise get exact");
			if (rc == 1)
				free(val);
			perfd_free(p);
		}
	}

	printf("libtest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
