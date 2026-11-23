/* libtest.c - libperfd end to end against live daemons: every typed verb round-trips byte-exact,
 * errors carry messages, the JSON edge renders right, the pipeline keeps order, secrets rotate. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/wait.h>

#include "../lib/perfd.h"

static int pass, fail;

/* perfd_connect honours connect_timeout_ms against a listener whose full backlog makes the
 * kernel drop every SYN, as a member gone dark does; no daemon needed, the alarm ends a hang */
#include <fcntl.h>
#include <signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

static void s441_alarm(int sig)
{
	static const char m[] = "  FAIL S441: perfd_connect to a dropped SYN "
		"was still blocked after 20 s - connect_timeout_ms ignored\n";

	(void)sig;
	if (write(1, m, sizeof m - 1) < 0)
		_exit(2);
	_exit(1);
}

static void s441_connect_timeout(void)
{
	struct sockaddr_in sa;
	socklen_t sl = sizeof sa;
	int lf = socket(AF_INET, SOCK_STREAM, 0), f1, f2;
	struct timespec t0, t1;
	perfd_opts o = PERFD_OPTS_INIT;
	perfd_t *p;
	long ms;

	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (lf < 0 || bind(lf, (struct sockaddr *)&sa, sizeof sa) != 0 ||
	    listen(lf, 0) != 0 || getsockname(lf, (struct sockaddr *)&sa, &sl) != 0) {
		printf("  FAIL S441: no listener to stand in for a dark member\n");
		fail++;
		return;
	}
	/* fill the queue: non-blocking, never waited on */
	f1 = socket(AF_INET, SOCK_STREAM, 0);
	f2 = socket(AF_INET, SOCK_STREAM, 0);
	if (f1 >= 0) {
		fcntl(f1, F_SETFL, O_NONBLOCK);
		(void)connect(f1, (struct sockaddr *)&sa, sizeof sa);
	}
	usleep(100000);
	if (f2 >= 0) {
		fcntl(f2, F_SETFL, O_NONBLOCK);
		(void)connect(f2, (struct sockaddr *)&sa, sizeof sa);
	}
	usleep(100000);
	o.connect_timeout_ms = 300;
	o.spares = PERFD_SPARES_NONE;
	signal(SIGALRM, s441_alarm);
	alarm(20);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	p = perfd_connect("127.0.0.1", ntohs(sa.sin_port), &o);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	alarm(0);
	ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
	if (!p && ms >= 250 && ms < 3000) {
		printf("  ok   S441: a connect to a member that drops the SYN gives up "
			"after connect_timeout_ms (300 ms asked, %ld ms): %s\n", ms,
			perfd_error(NULL) ? perfd_error(NULL) : "-");
		pass++;
	} else {
		printf("  FAIL S441: connect_timeout_ms 300 against a dropped SYN: %s "
			"after %ld ms\n", p ? "CONNECTED" : "refused", ms);
		fail++;
	}
	if (p)
		perfd_free(p);
	if (f1 >= 0)
		close(f1);
	if (f2 >= 0)
		close(f2);
	close(lf);
}

/* this process's CPU time, seconds */
static double cpu_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
	return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---- the key cache, run in children forked while nothing is cached ------- */
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

/* 8 threads connect at once, read cold then warm: the derivations cost the DIFFERENCE, so the
 * handshakes' own cost, which a sanitizer multiplies, falls out */
static double s355_round(int *ok)
{
	pthread_t th[8];
	int okv[8], k;
	double t0 = cpu_s();

	for (k = 0; k < 8; k++)
		pthread_create(&th[k], NULL, s355_conn, &okv[k]);
	for (k = 0; k < 8; k++) {
		pthread_join(th[k], NULL);
		*ok = *ok && okv[k];
	}
	return cpu_s() - t0;
}

static int s355_threads(void)
{
	int ok = 1;
	double d = derive_cost(), d2, cold, warm;

	cold = s355_round(&ok);
	warm = s355_round(&ok);
	/* the memory-hard derive's CPU time moves with the host's load, so it is measured on
	 * both sides of the rounds; 2.5 derives bounds one thread deriving, not all 8 */
	d2 = derive_cost();
	if (d2 > d)
		d = d2;
	printf("  S355: one derive %.1f ms of CPU; 8 threads connecting at once "
		"%.1f ms cold, %.1f ms warm - the derivations cost %.1f ms\n",
		d * 1e3, cold * 1e3, warm * 1e3, (cold - warm) * 1e3);
	return ok && cold - warm < 2.5 * d ? 0 : 1;
}

/* libperfd on musl's default 128 KB thread stack: four threads connect, set and read back
 * 100 KB (over one 64 KB transport record each way) and ping */
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
	{                                      /* an older library: a connect is all there is */
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
/* ---- the options carry their size: a perfd_opts cut after `size`, 0x7F garbage past it, must
 * connect plaintext and ping; a library reading past it follows a garbage secrets pointer ---- */
static int s370_port;

static int s370_short_opts(void)
{
	unsigned char buf[sizeof(perfd_opts) + 64];
	perfd_opts *o = (perfd_opts *)(void *)buf;
	perfd_t *p;
	int ok;

	memset(buf, 0x7F, sizeof buf);
	o->size = offsetof(perfd_opts, secrets);
	p = perfd_connect("127.0.0.1", s370_port, o);
	ok = p != NULL && perfd_ping(p) == 0;
	if (p)
		perfd_free(p);
	return ok ? 0 : 1;
}

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


/* perfd_jset_ex: NX and XX on the root and on a path, declined as 0 with no error left behind */
static void jset_ex_checks(perfd_t *p)
{
	char *r = NULL;
	int rc;

	OK(perfd_jset_ex(p, "c", "jx", "$", "{\"a\":1}", 0, PERFD_JSET_NX) == 1,
		"jset_ex: NX on a missing key creates it (1)");
	OK(perfd_jset_ex(p, "c", "jx", "$", "{\"a\":2}", 0, PERFD_JSET_NX) == 0 &&
		perfd_error_kind(p) == PERFD_E_NONE,
		"jset_ex: NX on a key that holds a document declines (0, no error)");
	OK(perfd_jset_ex(p, "c", "jx", "$.b", "true", 0, PERFD_JSET_NX) == 1,
		"jset_ex: NX on an absent path sets it");
	OK(perfd_jset_ex(p, "c", "jx", "$.b", "false", 0, PERFD_JSET_NX) == 0,
		"jset_ex: NX on that path again declines");
	OK(perfd_jset_ex(p, "c", "jx", "$.a", "5", 0, PERFD_JSET_XX) == 1,
		"jset_ex: XX on a path that holds a value sets it");
	OK(perfd_jset_ex(p, "c", "jx", "$.zz", "5", 0, PERFD_JSET_XX) == 0 &&
		perfd_error_kind(p) == PERFD_E_NONE, "jset_ex: XX on an absent path declines");
	rc = perfd_jget(p, "c", "jx", "$", &r);
	OK(rc == 1 && !strcmp(r, "{\"a\":5,\"b\":true}"), "jset_ex: the document as the sets left it");
	if (rc == 1)
		free(r);
	OK(perfd_jset_ex(p, "c", "jxnone", "$", "1", 0, PERFD_JSET_XX) == 0,
		"jset_ex: XX on a missing key declines");
	OK(perfd_jget(p, "c", "jxnone", "$", &r) == 0, "jset_ex: and the key stays missing");
	OK(perfd_jset_ex(p, "c", "jxnone", "$.a", "1", 0, PERFD_JSET_NX) == -1 &&
		perfd_error_kind(p) == PERFD_E_SERVER,
		"jset_ex: NX under a missing key is the daemon's error, not a decline");
	OK(perfd_jset_ex(p, "c", "jxt", "$", "[1]", 100, 0) == 1, "jset_ex: no flag sets (1)");
	{
		long long t = perfd_ttl(p, "c", "jxt");

		OK(t > 90 && t <= 100, "jset_ex: with its ttl");
	}
	OK(perfd_jset_ex(p, "c", "jxt", "$", "{bad", 0, 0) == -1 &&
		perfd_error_kind(p) == PERFD_E_SERVER, "jset_ex: a value that is not JSON is an error");
}

/* perfd_url_connect against the live daemons: plaintext, the Noise leg with the URL's secret, and a
 * wrong secret that turns into the backoff */
static void url_checks(int ptport, int nxport, const char *secret)
{
	char url[256], err[256];
	perfd_url *u;
	perfd_t *p;
	void *val;
	size_t vlen;
	int rc;

	snprintf(url, sizeof url, "perfd://127.0.0.1:%d/c?spares=0", ptport);
	u = perfd_url_parse(url, err, sizeof err);
	OK(u != NULL, "url: a plaintext URL parses");
	p = u ? perfd_url_connect(u, NULL) : NULL;
	OK(p != NULL, "url: perfd_url_connect over its one seed");
	if (p) {
		OK(perfd_set(p, perfd_url_collection(u), "uk", "uv", 2, 0) == 0,
			"url: a set in the URL's collection");
		rc = perfd_get(p, perfd_url_collection(u), "uk", &val, &vlen, NULL);
		OK(rc == 1 && vlen == 2 && !memcmp(val, "uv", 2), "url: and its get");
		if (rc == 1)
			perfd_free_value(val);
		perfd_free(p);
	}
	perfd_url_free(u);
	if (!nxport || !secret)
		return;
	snprintf(url, sizeof url, "perfd://app:%s@127.0.0.1:%d/c", secret, nxport);
	u = perfd_url_parse(url, err, sizeof err);
	p = u ? perfd_url_connect(u, NULL) : NULL;
	OK(p != NULL && perfd_ping(p) == 0, "url: the Noise leg with the URL's secret");
	if (p)
		perfd_free(p);
	perfd_url_free(u);
	snprintf(url, sizeof url, "perfd://app:not-the-secret@127.0.0.1:%d/c", nxport);
	u = perfd_url_parse(url, err, sizeof err);
	OK(u && perfd_url_connect(u, NULL) == NULL && perfd_error_kind(NULL) == PERFD_E_AUTH,
		"url: a wrong secret is PERFD_E_AUTH");
	OK(u && perfd_url_connect(u, NULL) == NULL && perfd_error_kind(NULL) == PERFD_E_BACKOFF,
		"url: and the next call within the second backs off");
	perfd_url_free(u);
}

/* perfd_hgetall's callback - counts pairs, remembers "f2"'s value */
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

/* ---- the reads that allocate nothing ----------------------------------- */
static void s353_checks(perfd_t *p)
{
	/* the largest field a hash command carries: its arguments are capped
	 * at 58,000 bytes (the field name counts), so 57,000 of value */
	static char big[57000], bb[65536];
	const char bv[] = { 'a', 0, 'b', (char)0xff };
	char b[16];
	size_t vl = 99;
	long long ttl = 0;
	int rc;

	OK(perfd_set(p, "c", "s353", "hello", 5, 500) == 0, "S353: seed");
	rc = perfd_get_buf(p, "c", "s353", b, sizeof b, &vl, &ttl);
	OK(rc == 1 && vl == 5 && !memcmp(b, "hello", 5) && ttl > 400 && ttl <= 500,
		"S353: get_buf found - value and ttl");
	vl = 99;
	OK(perfd_get_buf(p, "c", "s353-none", b, sizeof b, &vl, NULL) == 0 && vl == 0,
		"S353: get_buf absent - 0, *vlen 0");
	memset(b, 'Z', sizeof b);
	rc = perfd_get_buf(p, "c", "s353", b, 3, &vl, NULL);
	OK(rc == -2 && vl == 5 && b[0] == 'Z' && b[1] == 'Z' && b[2] == 'Z',
		"S353: get_buf too small - -2, *vlen the size needed, buffer untouched");
	memset(b, 'Z', sizeof b);
	OK(perfd_get_buf(p, "c", "s353", b, 5, &vl, NULL) == 1 && vl == 5 &&
		!memcmp(b, "hello", 5) && b[5] == 'Z',
		"S353: get_buf exact fit - no terminator written past it");
	OK(perfd_get_buf(p, "c", "s353", NULL, 0, &vl, NULL) == -2 && vl == 5,
		"S353: get_buf NULL/0 asks the size");
	OK(perfd_set(p, "c", "s353b", bv, sizeof bv, 0) == 0 &&
		perfd_get_buf(p, "c", "s353b", b, sizeof b, &vl, &ttl) == 1 &&
		vl == sizeof bv && !memcmp(b, bv, sizeof bv) && ttl == -1,
		"S353: get_buf a value with NULs, exact; no expiry is -1");
	OK(perfd_set(p, "c", "s353e", "", 0, 0) == 0 &&
		perfd_get_buf(p, "c", "s353e", NULL, 0, &vl, NULL) == 1 && vl == 0,
		"S353: get_buf an empty value fits a 0-byte buffer");

	OK(perfd_hset(p, "c", "s353h", "f", "V1", 2) == 1 &&
		perfd_hget_buf(p, "c", "s353h", "f", b, sizeof b, &vl) == 1 &&
		vl == 2 && !memcmp(b, "V1", 2), "S353: hget_buf found");
	OK(perfd_hget_buf(p, "c", "s353h", "nope", b, sizeof b, &vl) == 0 && vl == 0,
		"S353: hget_buf absent field - 0");
	memset(big, 'q', sizeof big);
	big[0] = 'A';
	big[sizeof big - 1] = 'Z';
	OK(perfd_hset(p, "c", "s353h", "big", big, sizeof big) == 1 &&
		perfd_hget_buf(p, "c", "s353h", "big", bb, sizeof bb, &vl) == 1 &&
		vl == sizeof big && !memcmp(bb, big, sizeof big),
		"S353: hget_buf a 57,000-byte field (near the hash-argument cap)");
	OK(perfd_hget_buf(p, "c", "s353h", "big", b, sizeof b, &vl) == -2 &&
		vl == sizeof big, "S353: hget_buf too small - the size of the big field");
	OK(perfd_set(p, "c", "s353s", "s", 1, 0) == 0 &&
		perfd_hget_buf(p, "c", "s353s", "f", b, sizeof b, &vl) == -1 &&
		strstr(perfd_error(p), "WRONGTYPE"), "S353: hget_buf on a string - -1, WRONGTYPE");
	OK(perfd_ping(p) == 0, "S353: the handle still works after the error");

	OK(perfd_jset(p, "c", "s353j", "$", "{\"n\":1,\"s\":\"x\"}", 0) == 0,
		"S353: jset seed");
	memset(b, 'Z', sizeof b);
	OK(perfd_jget_buf(p, "c", "s353j", "$.s", b, sizeof b, &vl) == 1 &&
		vl == 3 && !memcmp(b, "\"x\"", 3) && b[3] == 'Z',
		"S353: jget_buf the fragment's JSON text, not terminated");
	OK(perfd_jget_buf(p, "c", "s353j", "$.n", b, 0, &vl) == -2 && vl == 1,
		"S353: jget_buf too small");
	OK(perfd_jget_buf(p, "c", "s353-nodoc", "$", b, sizeof b, &vl) == 0,
		"S353: jget_buf absent - 0");
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
	const char *enable = argc > 4 ? argv[4] : NULL;
	perfd_t *p;
	void *val;
	size_t vlen;
	long long ttl, nv;
	char *r, **keys;
	int rc, i;

	s441_connect_timeout();                /* no daemon needed */

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
		perfd_free_value(val);         /* the library's release */

	/* errors carry a kind; the options carry their size */
	{
		char big[5000];
		perfd_opts io = PERFD_OPTS_INIT;

		memset(big, 'k', sizeof big - 1);
		big[sizeof big - 1] = 0;
		OK(perfd_set(p, "c", big, "v", 1, 0) != 0 &&
			perfd_error_kind(p) == PERFD_E_ARG,
			"S370: a key over the limit is PERFD_E_ARG");
		OK(perfd_ping(p) == 0, "S370: the handle still works after it");
		OK(perfd_connect("127.0.0.1", 1, NULL) == NULL &&
			perfd_error_kind(NULL) == PERFD_E_TRANSPORT,
			"S370: a refused connect is PERFD_E_TRANSPORT (perfd_error_kind(NULL))");
		OK(io.size == sizeof(perfd_opts) && io.secrets == NULL,
			"S370: PERFD_OPTS_INIT sets the size, the rest 0");
		memset(&io, 0x5A, sizeof io);
		perfd_opts_init(&io);
		OK(io.size == sizeof(perfd_opts) && io.secrets == NULL && io.spares == 0,
			"S370: perfd_opts_init() zeroes the struct and sets the size");
		s370_port = ptport;
		OK(in_child(s370_short_opts), "S370: an options struct cut after "
			"`size` (0x7F garbage beyond) connects as defaults - the "
			"library reads only the bytes the caller has");
	}

	s353_checks(p);                        /* the _buf reads */

	/* conditional set */
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

	/* compare-and-set / compare-and-delete - the token lock */
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

	hash_checks(p);

	/* rate-limit hits */
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
	{
		/* the same with each key's outcome */
		const char *const ks[] = { "m1", "gone", "m2" };
		void *vals[3];
		size_t lens[3];
		int st[3];

		rc = perfd_mget_ex(p, "c", ks, 3, vals, lens, st);
		OK(rc == 0 && st[0] == PERFD_MGET_FOUND &&
			st[1] == PERFD_MGET_ABSENT && st[2] == PERFD_MGET_FOUND,
			"mget_ex: found, absent, found");
		OK(vals[0] && lens[0] == 2 && !memcmp(vals[0], "v1", 2) &&
			vals[1] == NULL && vals[2] && lens[2] == 3,
			"mget_ex: the values as mget gives them");
		free(vals[0]);
		free(vals[2]);
		OK(perfd_mget_ex(p, "c", ks, 3, vals, lens, NULL) == -1 &&
			perfd_error_kind(p) == PERFD_E_ARG,
			"mget_ex without a status array: refused (PERFD_E_ARG)");
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
	jset_ex_checks(p);

	/* the escape hatch: the JSON edge renders the stats tree */
	r = perfd_command(p, "stats", NULL);
	OK(r && strstr(r, "\"arena_total\""), "command stats");
	OK(r && strstr(r, "\"hits\""), "stats carries hits");
	/* this connection is a binary one and has carried every request above as a frame */
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
	/* ---- the admin verbs ------------------------------------------------ */
	r = perfd_stats(p, NULL);
	OK(r && strstr(r, "\"collections\""), "stats: the tree as JSON");
	perfd_free_value(r);
	r = perfd_stats(p, "c");
	OK(r && strstr(r, "\"name\":\"c\""), "stats c: that collection");
	perfd_free_value(r);
	r = perfd_members(p);
	OK(r && strstr(r, "\"members\""), "members: the tree as JSON");
	perfd_free_value(r);
	r = perfd_clients(p, 5);
	OK(r && strstr(r, "\"clients\""), "clients: the tree the HTTP door renders, on the native door");
	perfd_free_value(r);
	r = perfd_upfill(p, "c", PERFD_SWEEP_CHECK);
	OK(r == NULL && strstr(perfd_error(p), "privilege"),
		"upfill check without privilege: refused");
	if (enable) {
		OK(perfd_enable(p, "not-the-enable") == -1 &&
			strstr(perfd_error(p), "wrong enable secret"),
			"enable with a wrong secret: refused");
		OK(perfd_enable(p, enable) == 0, "enable raises privilege");
		r = perfd_upfill(p, "c", PERFD_SWEEP_CHECK);
		OK(r == NULL && strstr(perfd_error(p), "no upstream"),
			"upfill check, privileged: the daemon's own refusal (c has no upstream)");
		r = perfd_updrain(p, "c", PERFD_SWEEP_RUN);
		OK(r == NULL && strstr(perfd_error(p), "no upstream"),
			"updrain, privileged: the same");
		/* the DDL, typed - a round trip */
		r = perfd_create(p, "lt-ddl", 8);
		OK(r && strstr(r, "\"created\":true") && strstr(r, "\"buckets\":256"),
			"create lt-ddl 8: created, 256 buckets");
		perfd_free_value(r);
		r = perfd_collections(p);
		OK(r && strstr(r, "\"name\":\"lt-ddl\""), "collections lists it");
		perfd_free_value(r);
		OK(perfd_set(p, "lt-ddl", "k", "v", 1, 0) == 0, "a record in it");
		r = perfd_rename(p, "lt-ddl", "lt-ddl2");
		OK(r && strstr(r, "\"renamed\":true"), "rename lt-ddl -> lt-ddl2");
		perfd_free_value(r);
		/* the RESP DB-index map, typed - a round trip */
		r = perfd_map(p, 7, "lt-ddl2");
		OK(r && strstr(r, "\"mapped\":true") && strstr(r, "\"col\":\"lt-ddl2\""),
			"map 7 lt-ddl2: mapped");
		perfd_free_value(r);
		r = perfd_maps(p);
		OK(r && strstr(r, "\"col\":\"lt-ddl2\"") &&
			strstr(r, "\"source\":\"runtime\"") && strstr(r, "\"overrides\":1"),
			"maps lists index 7 as a runtime map");
		perfd_free_value(r);
		r = perfd_unmap(p, 7);
		OK(r && strstr(r, "\"unmapped\":true"), "unmap 7: unmapped");
		perfd_free_value(r);
		r = perfd_unmap(p, 7);
		OK(!r && strstr(perfd_error(p), "not mapped at runtime"),
			"unmap 7 again: refused, nothing to unmap");
		perfd_free_value(r);
		r = perfd_map(p, 7, "lt-no-such");
		OK(!r && strstr(perfd_error(p), "no such collection"),
			"map 7 to a collection that does not exist: refused");
		perfd_free_value(r);
		r = perfd_map(p, 64, "lt-ddl2");
		OK(!r && strstr(perfd_error(p), "index (0..63)"),
			"map 64: out of range, refused");
		perfd_free_value(r);
		r = perfd_resize(p, "lt-ddl2", 9);
		OK(r && strstr(r, "\"resizing\":true"), "resize lt-ddl2 9: a migration starts");
		perfd_free_value(r);
		r = perfd_drop(p, "lt-ddl2", 0);
		OK(r && strstr(r, "\"dropped\":false"), "drop without force: refused, it holds a record");
		perfd_free_value(r);
		r = perfd_drop(p, "lt-ddl2", 1);
		OK(r && strstr(r, "\"dropped\":true"), "drop with force: gone");
		perfd_free_value(r);
		r = perfd_collections(p);
		OK(r && !strstr(r, "lt-ddl"), "collections no longer lists it");
		perfd_free_value(r);
		r = perfd_upgrade_status(p);
		OK(r && strstr(r, "\"install_mode\"") && strstr(r, "\"previous\"") && strstr(r, "\"switching\""),
			"upgrade_status: the install mode, the staged builds, previous and switching (UP7b)");
		perfd_free_value(r);
		r = perfd_upgrade_revert(p, 0);
		OK(!r && strstr(perfd_error(p), "revert"), "upgrade_revert on a local install: refused with the reason");
		perfd_free_value(r);
		r = perfd_reset_stats(p);
		OK(r && strstr(r, "\"reset\":true"), "reset_stats: this node's totals");
		perfd_free_value(r);
		r = perfd_reset_stats_fleet(p);       /* the fleet form */
		OK(r && strstr(r, "\"reset\":true"), "reset_stats_fleet: this node and its live peers");
		perfd_free_value(r);
		r = perfd_save(p);
		OK((r && strstr(r, "\"started\"")) || (!r && strstr(perfd_error(p), "persistence")),
			"save: started, or the daemon's 'persistence is not configured'");
		perfd_free_value(r);
		/* the staging call: the arguments are checked here, the bytes reach the node,
		 * and its refusal comes back (unsigned, no state directory, or a package install) */
		r = perfd_upgrade_stage(p, "x", 1, "bad version!", NULL, NULL, NULL);
		OK(r == NULL && perfd_error_kind(p) == PERFD_E_ARG,
			"upgrade_stage with a bad version: PERFD_E_ARG, nothing sent");
		r = perfd_upgrade_stage(p, "hello", 5, "0.0.1", NULL, NULL, NULL);
		OK(r == NULL && (strstr(perfd_error(p), "unsigned") ||
			strstr(perfd_error(p), "state directory") ||
			strstr(perfd_error(p), "package install")),
			"upgrade_stage unsigned: the node's own refusal");
		r = perfd_upgrade_stage_file(p, "/nonexistent/build.tgz", "0.0.1", NULL,
			NULL, NULL);
		OK(r == NULL && perfd_error_kind(p) == PERFD_E_ARG,
			"upgrade_stage_file on a missing file: PERFD_E_ARG");
		r = perfd_sync(p, 1000);
		OK((r && strstr(r, "\"synced\"")) || (!r && perfd_error(p)[0]),
			"sync: the barrier, or the daemon's refusal without a WAL");
		perfd_free_value(r);
	} else {
		printf("  (no enable secret given: the privileged verbs not tried)\n");
	}
	perfd_free(p);

	/* ---- the Noise leg + secret-list rotation ---------------------- */
	if (nxport && secret) {
		const char *wrong_then_right[] = { "not-the-secret", secret,
			NULL };
		const char *wrong_only[] = { "not-the-secret", NULL };
		perfd_opts o = { .secrets = wrong_then_right };

		/* a wrong secret is PERFD_E_AUTH */
		{
			perfd_opts wo = { .secrets = wrong_only };

			OK(perfd_connect("127.0.0.1", nxport, &wo) == NULL &&
				perfd_error_kind(NULL) == PERFD_E_AUTH,
				"S370: a wrong secret is PERFD_E_AUTH");
		}
		/* forked while this process has derived nothing: each child's cache is cold */
		s355_port = nxport;
		s355_secret = secret;
		OK(in_child(s355_threads), "S355: 8 threads connecting at once "
			"with a cold cache derive once (single-flight)");
		OK(in_child(s355_fork), "S355: perfd_psk_prepare in the parent - "
			"4 forked children connect without deriving");
		OK(in_child(s369_small_stacks), "S369: four threads on 128 KB stacks "
			"(musl's default) connect, set and get 100 KB, ping");

		/* FIRST in the Noise leg: only the first connect pays Argon2id (~100 ms of CPU,
		 * 64 MiB); the process-wide cache makes the next 19 together cost less */
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

	url_checks(ptport, nxport, secret);       /* after the S355 legs: the PSK cache is warm here */

	printf("libtest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
