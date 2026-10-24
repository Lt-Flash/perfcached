/* httpdoortest.c - the metrics door (httpdoor.c; the 0.7.2 split, wave 1):
 * proto.c's reply staging captured, the clock faked, the trees and the
 * exposition stubbed; the JSON renderer and the address helpers are the
 * real ones.
 *
 *   A  the head: incomplete is "read more", no head in 8 KB drops the
 *      connection, a bare-LF head is read, the bytes consumed are the
 *      head's (from an offset too)
 *   B  methods: anything but GET is 405 - POST since 0.7.0 with its Allow
 *      line; a path of 256 bytes is 414; a query string is ignored
 *   C  the token: none sent is 401 with the browser's Basic challenge;
 *      Bearer right, wrong and the wrong length; the header name in any
 *      case; Basic with the token as the password (any user), and wrong
 *   D  the routes: / is 410 with where the page went, /health, /members,
 *      /clients, /stats (the tree as JSON), /metrics (the exposition's
 *      content type), anything else 404; every answer has a correct
 *      Content-Length, the three hardening headers and Connection: close,
 *      and marks the connection to close
 *   E  the cache (0.6.2): /stats and /metrics built once per http_cache_ms
 *      for a remote peer, the copy served between; a stats reset drops
 *      the copies; a loopback peer and a cache of 0 always get a fresh
 *      build
 *   F  the rate (0.6.2): a bucket of twice http_rate per IPv4 address or
 *      IPv6 /64, refilled by the clock; another address has its own; an
 *      IPv4 client and its ::ffff: form share one; loopback and rate 0
 *      are never limited; the refusals counted, logged once in 10 s
 */
#include <arpa/inet.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "compat/dprint.h"
#include "json.h"
#include "ptree.h"
#include "proto.h"
#include "protoint.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* ---- the stubs ---------------------------------------------------------- */
static char cap_buf[65536];
static size_t cap_len;
static long long fake_ms = 100000;
static const char *token;
static int rate, cache_ms, stats_builds, metrics_builds, notices;

int out_append(struct pc_conn *c, const char *p, size_t n)
{
	(void)c;
	if (cap_len + n > sizeof cap_buf)
		return -1;
	memcpy(cap_buf + cap_len, p, n);
	cap_len += n;
	return 0;
}

long long deny_now(void) { return fake_ms; }
unsigned int compat_ticks(void) { return 1; }
const char *pc_http_token(void) { return token; }
int pc_http_rate(void) { return rate; }
int pc_http_cache_ms(void) { return cache_ms; }
int compat_log_level = L_NOTICE;

void compat_log(int level, const char *tag, const char *fmt, ...)
{
	(void)level; (void)tag; (void)fmt;
	notices++;
}

size_t pc_metrics_render(char *buf, size_t cap)
{
	int n = snprintf(buf, cap, "pc_up 1\npc_build %d\n", ++metrics_builds);

	return (size_t)n;
}

void pc_stats_tree(struct pc_tw *out, const char *only_col)
{
	(void)only_col;
	pc_tw_map(out);
	pc_tw_key(out, "build");
	pc_tw_i64(out, ++stats_builds);
	pc_tw_end(out);
}

void pc_members_tree(struct pc_tw *out)
{
	pc_tw_map(out);
	pc_tw_key(out, "members");
	pc_tw_arr(out);
	pc_tw_end(out);
	pc_tw_end(out);
}

void pc_obs_clients_tree(struct pc_tw *w, unsigned int now_ticks, int cap)
{
	(void)now_ticks; (void)cap;
	pc_tw_map(w);
	pc_tw_key(w, "clients");
	pc_tw_i64(w, 0);
	pc_tw_end(w);
}

/* ---- the harness ------------------------------------------------------- */
static struct pc_conn c;
static char inbuf[16384];

/* @req at @off of the input, as a remote (@loop 0) or loopback peer */
static int send_at(const char *req, size_t off, int loop)
{
	size_t n = strlen(req);

	memset(inbuf, 'x', off);
	memcpy(inbuf + off, req, n);
	c.in = inbuf;
	c.in_len = off + n;
	c.http_loop = loop;
	c.close_after = 0;
	cap_len = 0;
	return http_one(&c, off);
}
#define GET(path) send_at("GET " path " HTTP/1.0\r\n\r\n", 0, 0)

static int starts(const char *s)
{
	return cap_len >= strlen(s) && !memcmp(cap_buf, s, strlen(s));
}

static int has(const char *s)
{
	size_t n = strlen(s), i;

	for (i = 0; i + n <= cap_len; i++)
		if (!memcmp(cap_buf + i, s, n))
			return 1;
	return 0;
}

/* the body, and whether Content-Length, the hardening headers and
 * Connection: close are all as they should be */
static const char *body(size_t *bl)
{
	size_t i;

	for (i = 0; i + 4 <= cap_len; i++)
		if (!memcmp(cap_buf + i, "\r\n\r\n", 4)) {
			*bl = cap_len - i - 4;
			return cap_buf + i + 4;
		}
	*bl = 0;
	return NULL;
}

static int well_formed(void)
{
	char want[64];
	size_t bl;

	if (!body(&bl))
		return 0;
	snprintf(want, sizeof want, "\r\nContent-Length: %zu\r\n", bl);
	return has(want) && has("\r\nX-Content-Type-Options: nosniff\r\n") &&
		has("\r\nReferrer-Policy: no-referrer\r\n") &&
		has("\r\nX-Frame-Options: DENY\r\n") &&
		has("\r\nConnection: close\r\n") && c.close_after == 1;
}

static pc_addr v4(const char *ip)
{
	pc_addr a;

	memset(&a, 0, sizeof a);
	a.in4.sin_family = AF_INET;
	inet_pton(AF_INET, ip, &a.in4.sin_addr);
	return a;
}

static pc_addr v6(const char *ip)
{
	pc_addr a;

	memset(&a, 0, sizeof a);
	a.in6.sin6_family = AF_INET6;
	inet_pton(AF_INET6, ip, &a.in6.sin6_addr);
	return a;
}

int main(void)
{
	unsigned long long limited, hits, builds;
	char big[300];
	size_t bl;
	const char *b;
	int rc, i, n;

	/* A */
	rc = send_at("GET /health HTTP/1.0\r\nHost: x\r\n", 0, 0);
	CHK(rc == 0 && cap_len == 0, "A: a head without its blank line - read more");
	{
		static char junk[8300];

		memset(junk, 'a', sizeof junk - 1);
		rc = send_at(junk, 0, 0);
		CHK(rc == -1, "A: 8 KB and no head - drop the connection");
	}
	rc = send_at("GET /health HTTP/1.0\n\n", 0, 0);
	CHK(rc == 22 && starts("HTTP/1.0 200 OK\r\n"), "A: a bare-LF head is read (22 bytes consumed)");
	rc = send_at("GET /health HTTP/1.0\r\n\r\nGET /x", 7, 0);
	CHK(rc == 24 && starts("HTTP/1.0 200 OK\r\n"), "A: from an offset - the head's 24 bytes, not what follows");

	/* B */
	send_at("PUT /stats HTTP/1.0\r\n\r\n", 0, 0);
	b = body(&bl);
	CHK(starts("HTTP/1.0 405 Method Not Allowed\r\n") && well_formed() && b &&
		bl == strlen("only GET is served here - this door is read-only since 0.7.0\n") &&
		!memcmp(b, "only GET is served here - this door is read-only since 0.7.0\n", bl),
		"B: PUT - 405, saying only GET is served, its last byte sent");
	send_at("POST /reset-stats HTTP/1.0\r\n\r\n", 0, 0);
	CHK(starts("HTTP/1.0 405 Method Not Allowed\r\n") && has("\r\nAllow: GET, HEAD\r\n") &&
		has("read-only since 0.7.0"), "B: POST - 405, read-only since 0.7.0, with its Allow line");
	memcpy(big, "GET /", 5);
	memset(big + 5, 'p', 255);
	memcpy(big + 260, " HTTP/1.0\r\n\r\n", 14);
	send_at(big, 0, 0);
	CHK(starts("HTTP/1.0 414 URI Too Long\r\n"), "B: a path of 256 bytes - 414");
	GET("/health?probe=1&x=2");
	CHK(starts("HTTP/1.0 200 OK\r\n"), "B: a query string is ignored");

	/* C */
	token = "s3cret";
	GET("/health");
	CHK(starts("HTTP/1.0 401 Unauthorized\r\n") &&
		has("\r\nWWW-Authenticate: Basic realm=\"perfcached\", charset=\"UTF-8\"\r\n") &&
		well_formed(), "C: no token sent - 401, with the browser's Basic challenge");
	send_at("GET /health HTTP/1.0\r\nAuthorization: Bearer s3cret\r\n\r\n", 0, 0);
	CHK(starts("HTTP/1.0 200 OK\r\n"), "C: Bearer s3cret - 200");
	send_at("GET /health HTTP/1.0\r\nAuthorization: Bearer s3creT\r\n\r\n", 0, 0);
	CHK(starts("HTTP/1.0 401 "), "C: a wrong token of the same length - 401");
	send_at("GET /health HTTP/1.0\r\nAuthorization: Bearer s3cre\r\n\r\n", 0, 0);
	CHK(starts("HTTP/1.0 401 "), "C: a prefix of it - 401");
	send_at("GET /health HTTP/1.0\r\nauthorization:   bearer s3cret \r\n\r\n", 0, 0);
	CHK(starts("HTTP/1.0 200 OK\r\n"), "C: the header and scheme in any case, spaces around - 200");
	/* base64("anyone:s3cret") = YW55b25lOnMzY3JldA== */
	send_at("GET /health HTTP/1.0\r\nAuthorization: Basic YW55b25lOnMzY3JldA==\r\n\r\n", 0, 0);
	CHK(starts("HTTP/1.0 200 OK\r\n"), "C: Basic, any user, the token as the password - 200");
	/* base64("anyone:s3creX") = YW55b25lOnMzY3JlWA== */
	send_at("GET /health HTTP/1.0\r\nAuthorization: Basic YW55b25lOnMzY3JlWA==\r\n\r\n", 0, 0);
	CHK(starts("HTTP/1.0 401 "), "C: Basic with the wrong password - 401");
	token = NULL;

	/* D */
	GET("/");
	CHK(starts("HTTP/1.0 410 Gone\r\n") && has("perfcached-dashboard") && well_formed(),
		"D: / - 410, the page is the dashboard's");
	GET("/health");
	b = body(&bl);
	CHK(starts("HTTP/1.0 200 OK\r\n") && bl == 3 && !memcmp(b, "ok\n", 3) && well_formed(),
		"D: /health - ok");
	GET("/members");
	b = body(&bl);
	CHK(has("Content-Type: application/json\r\n") && bl == 14 && !memcmp(b, "{\"members\":[]}", 14) &&
		well_formed(), "D: /members - the tree as JSON");
	GET("/clients");
	b = body(&bl);
	CHK(bl == 13 && !memcmp(b, "{\"clients\":0}", 13) && well_formed(), "D: /clients");
	stats_builds = 0;
	GET("/stats");
	b = body(&bl);
	CHK(has("Content-Type: application/json\r\n") && bl == 11 && !memcmp(b, "{\"build\":1}", 11) &&
		well_formed(), "D: /stats - the stats tree as JSON");
	metrics_builds = 0;
	GET("/metrics");
	b = body(&bl);
	CHK(has("Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n") && bl == 19 &&
		!memcmp(b, "pc_up 1\npc_build 1\n", 19) && well_formed(),
		"D: /metrics - the exposition, its own content type");
	GET("/nope");
	CHK(starts("HTTP/1.0 404 Not Found\r\n") && well_formed() &&
		has("try /stats, /members, /clients, /metrics or /health\n"),
		"D: anything else - 404, naming the five routes");

	/* E */
	cache_ms = 60000;
	stats_builds = 0;
	GET("/stats");
	GET("/stats");
	b = body(&bl);
	pc_http_counters(&limited, &hits, &builds);
	CHK(stats_builds == 1 && bl == 11 && !memcmp(b, "{\"build\":1}", 11) && hits == 1,
		"E: /stats twice from a remote peer - built once, the copy served");
	pc_http_cache_drop();
	GET("/stats");
	b = body(&bl);
	CHK(stats_builds == 2 && !memcmp(b, "{\"build\":2}", 11), "E: a stats reset drops the copy - built afresh");
	send_at("GET /stats HTTP/1.0\r\n\r\n", 0, 1);
	CHK(stats_builds == 3, "E: a loopback peer - always a fresh build");
	metrics_builds = 0;
	GET("/metrics");
	GET("/metrics");
	CHK(metrics_builds == 1, "E: /metrics likewise - one build");
	cache_ms = 0;
	GET("/stats");
	GET("/stats");
	CHK(stats_builds == 5, "E: http_cache_ms = 0 - every request built");

	/* F */
	{
		pc_addr a = v4("10.0.0.1"), m = v6("::ffff:10.0.0.1"), o = v4("10.0.0.2"),
			l = v4("127.0.0.1"), s1 = v6("2001:db8:1:2::10"), s2 = v6("2001:db8:1:2::20"),
			s3 = v6("2001:db8:1:3::10");

		rate = 2;
		notices = 0;
		for (n = 0, i = 0; i < 6; i++)
			n += http_rate_ok(&a);
		CHK(n == 4, "F: rate 2 - a bucket of 4 at once, then refused (%d of 6)", n);
		CHK(http_rate_ok(&o), "F: another address has its own bucket");
		CHK(!http_rate_ok(&m), "F: the same address as ::ffff:10.0.0.1 - the same, empty bucket");
		fake_ms += 500;
		CHK(http_rate_ok(&a) && !http_rate_ok(&a), "F: 500 ms later - one more (2 a second)");
		for (n = 0, i = 0; i < 20; i++)
			n += http_rate_ok(&l);
		CHK(n == 20, "F: loopback - never limited");
		for (n = 0, i = 0; i < 4; i++)
			n += http_rate_ok(&s1);
		CHK(n == 4 && !http_rate_ok(&s2) && http_rate_ok(&s3),
			"F: IPv6 - one bucket per /64 (::20 shares ::10's), another /64 its own");
		pc_http_counters(&limited, &hits, &builds);
		CHK(limited == 5 && notices == 1, "F: 5 refusals counted, one log line in 10 s (%llu, %d)",
			limited, notices);
		fake_ms += 10000;
		for (i = 0; i < 5; i++)
			(void)http_rate_ok(&o);
		CHK(notices == 2, "F: 10 s on - the next refusal is logged again");
		rate = 0;
		for (n = 0, i = 0; i < 50; i++)
			n += http_rate_ok(&a);
		CHK(n == 50, "F: rate 0 - no limit");
	}

	printf("httpdoortest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
