/*
 * libtest.c — libperfd end to end against live daemons (booted by
 * test/libtest.sh): every typed verb round-trips on the one binary
 * wire (S317: the data verbs on their fixed-layout frames, everything
 * else as CMD trees), values are byte-exact - NULs, 0x9E, newlines and
 * bytes that are not UTF-8 included - errors surface with messages,
 * the JSON edge (perfd_command, the pipeline) renders maps, bools and
 * strings as the text door used to, the pipeline delivers in request
 * order, and the secret LIST tries in order (rotation).
 * Usage: libtest <pt-port> <nx-port> <secret>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/wait.h>

#include "../lib/perfd.h"

static int pass, fail;

/* S354: this process's CPU time, seconds */
static double cpu_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
	return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---- S355 - run in children forked while nothing is cached -------------- */
static int s355_port;
static const char *s355_secret;

/* the CPU of one Argon2id derive: a connect with a secret the daemon does
 * not hold derives its key and then fails the handshake */
static double derive_cost(void)
{
	const char *wrong[] = { "s355-not-the-secret", NULL };
	perfd_opts o = { .secrets = wrong };
	double t0 = cpu_s();
	perfd_t *p = perfd_connect("127.0.0.1", s355_port, &o);

	if (p)
		perfd_free(p);
	return cpu_s() - t0;
}

static void *s355_conn(void *arg)
{
	const char *only[] = { s355_secret, NULL };
	perfd_opts o = { .secrets = only };
	perfd_t *p = perfd_connect("127.0.0.1", s355_port, &o);

	*(int *)arg = p != NULL && perfd_ping(p) == 0;
	if (p)
		perfd_free(p);
	return NULL;
}

/* 8 threads connect at once, the cache cold: one derives, the rest wait for
 * its key - the 8 together cost about one derive, not eight */
static int s355_threads(void)
{
	pthread_t th[8];
	int okv[8], k, ok = 1;
	double d = derive_cost(), t0 = cpu_s(), used;

	for (k = 0; k < 8; k++)
		pthread_create(&th[k], NULL, s355_conn, &okv[k]);
	for (k = 0; k < 8; k++) {
		pthread_join(th[k], NULL);
		ok = ok && okv[k];
	}
	used = cpu_s() - t0;
	printf("  S355: one derive %.1f ms of CPU; 8 threads connecting at once "
		"%.1f ms\n", d * 1e3, used * 1e3);
	return ok && used < 2.5 * d ? 0 : 1;
}

/* S369: libperfd on a SMALL thread stack.  musl gives a pthread 128 KB by
 * default and noise_handshake's frame alone was 131,440 bytes (two 64 KB
 * buffers for a handshake message the daemon caps at 2 KB), send_bytes and
 * fill 64 KB each: the first connect from a thread on Alpine overflowed
 * (GitLab 110084 alpine, S355's threads).  Four threads with 128 KB stacks,
 * on glibc too: connect, a 100 KB value set and read back (more than one
 * 64 KB transport record each way), ping. */
static void *s369_conn(void *arg)
{
	const char *only[] = { s355_secret, NULL };
	perfd_opts o = { .secrets = only };
	perfd_t *p = perfd_connect("127.0.0.1", s355_port, &o);
	size_t n = 100 * 1024, vlen = 0;
	char *v = malloc(n), key[32];
	void *val = NULL;
	long long ttl;
	int ok = p != NULL && v != NULL;

	if (ok) {
		memset(v, 'x', n);
		snprintf(key, sizeof key, "s369-%lu", (unsigned long)pthread_self());
		ok = perfd_set(p, "c", key, v, n, 60) == 0 &&
			perfd_get(p, "c", key, &val, &vlen, &ttl) == 1 &&
			vlen == n && !memcmp(val, v, n) && perfd_ping(p) == 0;
	}
	free(val);
	free(v);
	if (p)
		perfd_free(p);
	*(int *)arg = ok;
	return NULL;
}

static int s369_small_stacks(void)
{
	pthread_t th[4];
	pthread_attr_t a;
	int okv[4], k, ok = 1;

	pthread_attr_init(&a);
	pthread_attr_setstacksize(&a, 128 * 1024);
	for (k = 0; k < 4; k++)
		if (pthread_create(&th[k], &a, s369_conn, &okv[k]) != 0)
			return 1;
	for (k = 0; k < 4; k++) {
		pthread_join(th[k], NULL);
		ok = ok && okv[k];
	}
	pthread_attr_destroy(&a);
	return ok ? 0 : 1;
}

/* the parent prepares the key, then forks 4 children: each connects
 * without deriving (well under a quarter of a derive) */
static int s355_fork(void)
{
	const char *only[] = { s355_secret, NULL };
	double d = derive_cost();
	int k, bad = 0, st;

#ifdef PERFD_HAS_PSK_PREPARE
	if (perfd_psk_prepare(only) != 0)
		return 1;
#else
	{                                      /* 0.5.6.4: a connect is all there is */
		perfd_opts o = { .secrets = only };
		perfd_t *p = perfd_connect("127.0.0.1", s355_port, &o);

		if (p)
			perfd_free(p);
	}
#endif
	for (k = 0; k < 4; k++) {
		pid_t pid = fork();

		if (pid == 0) {
			perfd_opts o = { .secrets = only };
			double t0 = cpu_s(), used;
			perfd_t *p = perfd_connect("127.0.0.1", s355_port, &o);
			int ok = p != NULL && perfd_ping(p) == 0;

			used = cpu_s() - t0;
			if (p)
				perfd_free(p);
			printf("  S355: forked child %d connected with %.1f ms of CPU "
				"(one derive: %.1f ms)\n", k, used * 1e3, d * 1e3);
			fflush(stdout);
			_exit(ok && used < d / 4 ? 0 : 1);
		}
		if (pid < 0 || waitpid(pid, &st, 0) != pid ||
		    !WIFEXITED(st) || WEXITSTATUS(st) != 0)
			bad = 1;
	}
	return bad;
}

/* fork @fn into a child of its own; its exit status is the verdict */
static int in_child(int (*fn)(void))
{
	int st;
	pid_t pid;

	fflush(stdout);
	pid = fork();
	if (pid == 0) {
		int rc = fn();

		fflush(stdout);
		_exit(rc);
	}
	return pid > 0 && waitpid(pid, &st, 0) == pid && WIFEXITED(st) &&
		WEXITSTATUS(st) == 0;
}

#define OK(cond, name) do { \
	if (cond) { pass++; } \
	else { fail++; printf("FAIL: %s\n", name); } \
} while (0)


/* S313: perfd_hgetall's callback - counts pairs, remembers "f2"'s value */
struct hga { int n; char f2[16]; };
static int hga_cb(void *ctx, const char *f, size_t fl, const void *v, size_t vl)
{
	struct hga *h = ctx;

	h->n++;
	if (fl == 2 && !memcmp(f, "f2", 2) && vl < sizeof h->f2) {
		memcpy(h->f2, v, vl);
		h->f2[vl] = 0;
	}
	return 0;
}

static void hash_checks(perfd_t *p)
{
	char *val;
	size_t vlen;
	long long n = 0;
	struct hga h = { 0, "" };
	int rc;

	OK(perfd_hset(p, "c", "hh", "f1", "v1", 2) == 1, "hset new: 1");
	OK(perfd_hset(p, "c", "hh", "f1", "V1", 2) == 0, "hset update: 0");
	OK(perfd_hset(p, "c", "hh", "f2", "v2", 2) == 1, "hset second field");
	OK(perfd_hsetnx(p, "c", "hh", "f1", "x", 1) == 0, "hsetnx present: 0");
	rc = perfd_hget(p, "c", "hh", "f1", (void **)&val, &vlen);
	OK(rc == 1 && vlen == 2 && !memcmp(val, "V1", 2), "hget");
	if (rc == 1)
		free(val);
	OK(perfd_hget(p, "c", "hh", "nope", (void **)&val, &vlen) == 0, "hget absent: 0");
	OK(perfd_hexists(p, "c", "hh", "f2") == 1 &&
		perfd_hexists(p, "c", "hh", "nope") == 0, "hexists");
	OK(perfd_hlen(p, "c", "hh", &n) == 0 && n == 2, "hlen");
	OK(perfd_hincrby(p, "c", "hh", "ctr", 5, &n) == 0 && n == 5, "hincrby");
	OK(perfd_hgetall(p, "c", "hh", hga_cb, &h) == 3 && h.n == 3 &&
		!strcmp(h.f2, "v2"), "hgetall: three pairs through the callback");
	OK(perfd_hdel(p, "c", "hh", "f1") == 1 && perfd_hdel(p, "c", "hh", "f2") == 1 &&
		perfd_hdel(p, "c", "hh", "ctr") == 1 && perfd_exists(p, "c", "hh") == 0,
		"hdel to empty deletes the key");
	OK(perfd_set(p, "c", "hstr", "s", 1, 0) == 0 &&
		perfd_hset(p, "c", "hstr", "f", "v", 1) == -1 &&
		strstr(perfd_error(p), "WRONGTYPE"), "hset on a string: WRONGTYPE");
	{                                      /* a value with NULs, exact */
		const char bv[] = { 'a', 0, 'b', (char)0xff };
		void *gv = NULL;
		size_t gl = 0;

		OK(perfd_hset(p, "c", "hb", "f", bv, sizeof bv) == 1 &&
			perfd_hget(p, "c", "hb", "f", &gv, &gl) == 1 &&
			gl == sizeof bv && !memcmp(gv, bv, sizeof bv),
			"hset/hget: a value with NULs comes back exact");
		free(gv);
	}
}

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

	/* S279b: conditional set */
	OK(perfd_set_cond(p, "c", "snx1", "a", 1, 60, PERFD_NX) == 1,
		"set_cond NX on absent: stored");
	OK(perfd_set_cond(p, "c", "snx1", "b", 1, 0, PERFD_NX) == 0,
		"set_cond NX on present: declined");
	rc = perfd_get(p, "c", "snx1", &val, &vlen, &ttl);
	OK(rc == 1 && vlen == 1 && !memcmp(val, "a", 1) && ttl > 50 &&
		ttl <= 60, "set_cond NX kept the first value and its ttl");
	if (rc == 1)
		free(val);
	OK(perfd_set_cond(p, "c", "snx1", "c", 1, 0, PERFD_XX) == 1,
		"set_cond XX on present: stored");
	OK(perfd_set_cond(p, "c", "snx2", "c", 1, 0, PERFD_XX) == 0 &&
		perfd_exists(p, "c", "snx2") == 0,
		"set_cond XX on absent: declined, nothing stored");
	OK(perfd_set_cond(p, "c", "snx1", "c", 1, 0, 3) == -1,
		"set_cond with a bad cond errs");

	/* S297: compare-and-set / compare-and-delete - the token lock */
	OK(perfd_set_cond(p, "c", "lk", "tokA", 4, 60, PERFD_NX) == 1,
		"lock: acquired with NX");
	OK(perfd_set_if(p, "c", "lk", "tokB", 4, 60, "tokX", 4, 0) == 0,
		"set_if IFEQ with another token: declined");
	OK(perfd_set_if(p, "c", "lk", "tokA", 4, 120, "tokA", 4, 0) == 1,
		"set_if IFEQ with the holder's token: refreshed");
	rc = perfd_get(p, "c", "lk", &val, &vlen, &ttl);
	OK(rc == 1 && vlen == 4 && !memcmp(val, "tokA", 4) && ttl > 100 &&
		ttl <= 120, "the refresh kept the token and set the new ttl");
	if (rc == 1)
		free(val);
	OK(perfd_del_if(p, "c", "lk", "tokX", 4, 0) == 0 &&
		perfd_exists(p, "c", "lk") == 1,
		"del_if IFEQ with another token: nothing deleted");
	OK(perfd_del_if(p, "c", "lk", "tokA", 4, 0) == 1 &&
		perfd_exists(p, "c", "lk") == 0,
		"del_if IFEQ with the holder's token: released");
	OK(perfd_set_if(p, "c", "lk2", "v", 1, 0, "x", 1, 1) == 1,
		"set_if IFNE on an absent key: stored");
	OK(perfd_set_if(p, "c", "lk2", "w", 1, 0, "v", 1, 1) == 0,
		"set_if IFNE on an equal value: declined");
	OK(perfd_del_if(p, "c", "lk2", "v", 1, 1) == 0 &&
		perfd_del_if(p, "c", "lk2", "z", 1, 1) == 1,
		"del_if IFNE: kept on equal, deleted on different");
	OK(perfd_del_if(p, "c", "lk3", "v", 1, 1) == 0,
		"del_if IFNE on an absent key: nothing to delete");

	hash_checks(p);                                /* S313 */

	/* S314: rate-limit hits */
	{
		long long cnt = 0;

		OK(perfd_rl_hit(p, "c", "rl1", 60000, 2, &cnt) == 1 && cnt == 1,
			"rl_hit 1 of 2: allowed");
		OK(perfd_rl_hit(p, "c", "rl1", 60000, 2, &cnt) == 1 && cnt == 2,
			"rl_hit 2 of 2: allowed");
		OK(perfd_rl_hit(p, "c", "rl1", 60000, 2, &cnt) == 0 && cnt == 3,
			"rl_hit 3 of 2: refused, and counted");
		OK(perfd_rl_hit(p, "c", "rl1", 0, 2, &cnt) == -1,
			"rl_hit with a bad window errs");
		OK(perfd_rl_hit(p, "c", "rl1", 60000, 0, &cnt) == 1 && cnt == 4,
			"rl_hit with no limit: 4, allowed");
	}

	/* a value with embedded NULs and a byte that is not UTF-8: bytes
	 * are bytes on the wire, nothing to escape or encode */
	{
		const char bin[] = { 'a', 0, 1, 2, (char)0xff, 0, 'z' };

		OK(perfd_set(p, "c", "bin", bin, sizeof bin, 0) == 0,
			"set binary");
		rc = perfd_get(p, "c", "bin", &val, &vlen, &ttl);
		OK(rc == 1 && vlen == sizeof bin &&
			!memcmp(val, bin, sizeof bin), "get binary exact");
		OK(ttl == -1, "no-expiry ttl is -1");
		if (rc == 1)
			free(val);
	}

	/* a 300-byte blob with every byte value, and a 50KB one: the frame
	 * carries them whole, whatever the bytes */
	{
		unsigned char blob[300];
		size_t bl = 50000, bi;
		char *big = malloc(bl);

		for (i = 0; i < (int)sizeof blob; i++)
			blob[i] = (unsigned char)(i * 7);      /* NULs included */
		OK(perfd_set(p, "c", "bk", blob, sizeof blob, 0) == 0,
			"set a 300-byte blob");
		rc = perfd_get(p, "c", "bk", &val, &vlen, &ttl);
		OK(rc == 1 && vlen == sizeof blob &&
			!memcmp(val, blob, sizeof blob), "get the blob exact");
		OK(rc == 1 && ttl == -1, "the blob has no ttl");
		if (rc == 1)
			free(val);
		OK(perfd_expire(p, "c", "bk", 500) == 0, "expire the blob");
		ttl = perfd_ttl(p, "c", "bk");
		OK(ttl > 490 && ttl <= 500, "its ttl took");
		OK(perfd_add(p, "c", "bk", 1, 0, &nv) == -1 &&
			strstr(perfd_error(p), "integer"),
			"add on a non-integer errs");
		OK(perfd_del(p, "c", "bk") == 1, "del the blob");
		OK(perfd_del(p, "c", "bk") == 0, "del it again: absent");
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

	/* mget: 2 present, 1 absent - a CMD tree in, bulks out */
	OK(perfd_set(p, "c", "m1", "v1", 2, 0) == 0, "set m1");
	OK(perfd_set(p, "c", "m2", "v\0\xff", 3, 0) == 0, "set m2 (NUL, 0xff)");
	{
		const char *const ks[] = { "m1", "gone", "m2" };
		void *vals[3];
		size_t lens[3];

		rc = perfd_mget(p, "c", ks, 3, vals, lens);
		OK(rc == 0, "mget");
		OK(vals[0] && lens[0] == 2 && !memcmp(vals[0], "v1", 2),
			"mget[0]");
		OK(vals[1] == NULL, "mget miss NULL");
		OK(vals[2] && lens[2] == 3 && !memcmp(vals[2], "v\0\xff", 3),
			"mget[2] exact, NUL and 0xff included");
		free(vals[0]);
		free(vals[2]);
	}

	rc = perfd_keys(p, "c", "m*", 0, &keys);
	OK(rc == 2, "keys count");
	perfd_free_keys(keys, rc > 0 ? rc : 0);

	/* JSON path verbs: the document travels as one bulk of its text */
	OK(perfd_jset(p, "c", "doc", "$", "{\"n\":1,\"s\":\"x\"}", 0) == 0,
		"jset root");
	OK(perfd_jincr(p, "c", "doc", "$.n", 4, &nv) == 0 && nv == 5,
		"jincr");
	rc = perfd_jget(p, "c", "doc", "$.s", &r);
	OK(rc == 1 && !strcmp(r, "\"x\""), "jget fragment");
	if (rc == 1)
		free(r);
	rc = perfd_jget(p, "c", "doc", "$", &r);
	OK(rc == 1 && !strcmp(r, "{\"n\":5,\"s\":\"x\"}"), "jget the whole document");
	if (rc == 1)
		free(r);
	OK(perfd_jdel(p, "c", "doc", "$.s") == 1, "jdel");
	OK(perfd_jdel(p, "c", "doc", "$.s") == 0, "jdel absent");
	OK(perfd_jget(p, "c", "nodoc", "$", &r) == 0, "jget absent: 0");

	/* the escape hatch: the JSON edge renders the stats tree */
	r = perfd_command(p, "stats", NULL);
	OK(r && strstr(r, "\"arena_total\""), "command stats");
	OK(r && strstr(r, "\"hits\""), "stats carries hits");
	/* S76 as 0.4.5 counts it (D10): this connection is a binary one
	 * and has carried every request above as a frame */
	{
		const char *b = r ? strstr(r, "\"binary\":{\"conns\":") : NULL;
		const char *q = b ? strstr(b, "\"requests\":") : NULL;

		OK(b && atoi(b + 18) >= 1, "stats: a binary connection was counted");
		OK(q && atoi(q + 11) >= 13, "stats: its requests were counted");
	}
	free(r);
	/* a jset through the edge: a document under "val" goes as its text */
	r = perfd_command(p, "jset",
		"{\"col\":\"c\",\"key\":\"edoc\",\"path\":\"$\",\"val\":{\"a\":[1,2]}}");
	OK(r && strstr(r, "\"set\":true"), "command jset: a nested val is the document");
	free(r);
	rc = perfd_jget(p, "c", "edoc", "$.a", &r);
	OK(rc == 1 && !strcmp(r, "[1,2]"), "and it was stored as written");
	if (rc == 1)
		free(r);
	r = perfd_command(p, "get", "{\"col\":\"c\",\"key\":\"m1\"}");
	OK(r && strstr(r, "\"found\":true") && strstr(r, "\"value\":\"v1\""),
		"command get: bools and strings render as before");
	free(r);

	/* a server error surfaces with its message */
	r = perfd_command(p, "get", "{\"col\":\"nosuch\",\"key\":\"x\"}");
	OK(r == NULL && strstr(perfd_error(p), "collection"),
		"error message surfaced");
	r = perfd_command(p, "get", "{\"col\":");
	OK(r == NULL && strstr(perfd_error(p), "params"),
		"bad params JSON refused before anything is sent");

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

	/* the tree forms of the pipeline, mixed with the JSON ones */
	{
		unsigned char *t;
		size_t tn;

		OK(perfd_append_tree(p, "ping", NULL, 0) == 0, "append_tree ping");
		OK(perfd_append(p, "ping", NULL) == 0, "append ping");
		OK(perfd_flush(p) == 0, "flush both");
		t = perfd_next_reply_tree(p, &tn);
		OK(t && tn > 0 && t[0] == 'm', "next_reply_tree: the reply tree, a map");
		free(t);
		r = perfd_next_reply(p);
		OK(r && strstr(r, "\"pong\":true"), "next_reply: rendered");
		free(r);
	}

	/* typed call refused mid-pipeline */
	OK(perfd_append(p, "ping", NULL) == 0, "append one more");
	OK(perfd_ping(p) == -1 &&
		strstr(perfd_error(p), "pipeline"), "typed refused mid-pipe");
	OK(perfd_flush(p) == 0, "flush last");
	r = perfd_next_reply(p);
	OK(r != NULL, "last reply");
	free(r);
	OK(perfd_set(p, "c", "bk2", "x", 1, 0) == 0, "set after the pipeline");
	OK(perfd_del(p, "c", "bk2") == 1, "del after the pipeline");
	perfd_free(p);

	/* ---- the Noise leg + secret-list rotation ---------------------- */
	if (nxport && secret) {
		const char *wrong_then_right[] = { "not-the-secret", secret,
			NULL };
		const char *wrong_only[] = { "not-the-secret", NULL };
		perfd_opts o = { .secrets = wrong_then_right };

		/* S355 - forked while this process has derived nothing, so each
		 * child starts with a cold cache */
		s355_port = nxport;
		s355_secret = secret;
		OK(in_child(s355_threads), "S355: 8 threads connecting at once "
			"with a cold cache derive once (single-flight)");
		OK(in_child(s355_fork), "S355: perfd_psk_prepare in the parent - "
			"4 forked children connect without deriving");
		OK(in_child(s369_small_stacks), "S369: four threads on 128 KB stacks "
			"(musl's default) connect, set and get 100 KB, ping");

		/* S354 - FIRST in the Noise leg, while the process has derived
		 * nothing yet.  A blocking connect takes the PSK from the
		 * process-wide cache, so only the first connect with a secret
		 * pays Argon2id (~100 ms of CPU and a 64 MiB mapping): the next
		 * 19 together cost less than the first.  Before, every blocking
		 * connect derived - about 19 times the first. */
		{
			const char *only[] = { secret, NULL };
			perfd_opts so = { .secrets = only };
			double first, rest = 0, t0;
			int k, ok;

			t0 = cpu_s();
			p = perfd_connect("127.0.0.1", nxport, &so);
			first = cpu_s() - t0;
			ok = p != NULL && perfd_ping(p) == 0;
			if (p)
				perfd_free(p);
			for (k = 0; k < 19 && ok; k++) {
				t0 = cpu_s();
				p = perfd_connect("127.0.0.1", nxport, &so);
				rest += cpu_s() - t0;
				ok = p != NULL && perfd_ping(p) == 0;
				if (p)
					perfd_free(p);
			}
			printf("  S354: the first connect %.1f ms of CPU, the next "
				"19 together %.1f ms\n", first * 1e3, rest * 1e3);
			OK(ok, "S354: 20 blocking connects with one secret, each answers");
			OK(ok && rest < first, "S354: the PSK is derived once - "
				"19 more connects cost less than the first");
		}

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
			/* a value holding a NUL, the frame magic and a newline:
			 * no byte of a value is ever read as a frame boundary */
			OK(perfd_set(p, "c", "bnk", "\x00\x9E\n", 3, 0) == 0,
				"noise set NUL/0x9E/newline");
			rc = perfd_get(p, "c", "bnk", &val, &vlen, NULL);
			OK(rc == 1 && vlen == 3 &&
				!memcmp(val, "\x00\x9E\n", 3),
				"noise get exact");
			if (rc == 1)
				free(val);
			r = perfd_command(p, "stats", NULL);
			OK(r && strstr(r, "\"hits\""), "a CMD tree inside the Noise transport");
			free(r);
			perfd_free(p);
		}
		o.secrets = wrong_only;
		p = perfd_connect("127.0.0.1", nxport, &o);
		OK(p == NULL && strstr(perfd_error(NULL), "handshake"),
			"wrong-only secrets refused");
		if (p)
			perfd_free(p);
	}

	printf("libtest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
