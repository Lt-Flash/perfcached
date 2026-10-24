/* doorlivetest.c - the doors' live settings (doorlive.c; the 0.7.2 split,
 * wave 1): snapshots made from configs built here and installed as a
 * reload would; the log and the monotonic clock stubbed.
 *
 *   A  before any snapshot: every check fail-closed - no password, no
 *      enable secret, no token - and the allow-lists open (a door without
 *      a list was refused at validate)
 *   B  a snapshot is a deep copy: its strings are its own, a config
 *      changed afterwards does not reach it
 *   C  the RESP password: exact only - a wrong length, one byte off; the
 *      enable secrets: either of a rotation list, none, no input
 *   D  the allow-lists: IPv4 in and out of a prefix, IPv6, an IPv4 client
 *      of a dual-stack door as ::ffff:a.b.c.d, a length short of an
 *      address; each refusal logged by address, ten at most, the tenth
 *      saying so; the HTTP list its own
 *   E  a reload: the new snapshot answers at once; the one it replaced is
 *      retired, still readable and unwiped for a reader that holds it,
 *      and still so one second before its retirement ends
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <arpa/inet.h>
#include "compat/dprint.h"
#include "pcaddr.h"
#include "config.h"
#include "daemon.h"
#include "reload.h"
#include "doorlive.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* ---- the stubs: the log, the clock ------------------------------------- */
int compat_log_level = L_NOTICE;
static char logbuf[8192];
static int warns;

void compat_log(int level, const char *tag, const char *fmt, ...)
{
	size_t n = strlen(logbuf);
	va_list ap;

	(void)tag;
	va_start(ap, fmt);
	vsnprintf(logbuf + n, sizeof logbuf - n, fmt, ap);
	va_end(ap);
	if (level == L_WARN)
		warns++;
}

/* this file's clock_gettime is the one doorlive.o calls: the retirement
 * clock, in whole seconds */
static long long now_s = 1000;

int clock_gettime(clockid_t clk, struct timespec *ts)
{
	(void)clk;
	ts->tv_sec = (time_t)now_s;
	ts->tv_nsec = 0;
	return 0;
}

/* ---- the harness ------------------------------------------------------- */
static struct pc_config cfg;

static void cidr(struct pc_cidr *c, const char *net, int bits)
{
	memset(c, 0, sizeof *c);
	c->bits = bits;
	if (inet_pton(AF_INET, net, c->net) == 1)
		c->family = AF_INET;
	else if (inet_pton(AF_INET6, net, c->net) == 1)
		c->family = AF_INET6;
}

static pc_addr v4(const char *ip)
{
	pc_addr a;

	memset(&a, 0, sizeof a);
	a.in4.sin_family = AF_INET;
	a.in4.sin_port = htons(40000);
	inet_pton(AF_INET, ip, &a.in4.sin_addr);
	return a;
}

static pc_addr v6(const char *ip)
{
	pc_addr a;

	memset(&a, 0, sizeof a);
	a.in6.sin6_family = AF_INET6;
	a.in6.sin6_port = htons(40000);
	inet_pton(AF_INET6, ip, &a.in6.sin6_addr);
	return a;
}

static int resp_ok(const pc_addr *a)
{
	return pc_resp_peer_allowed(a, sizeof *a);
}

static int http_ok(const pc_addr *a)
{
	return pc_http_peer_allowed(a, sizeof *a);
}

int main(void)
{
	char pw[] = "resp-pass-1", tk[] = "token-1", e1[] = "enable-one", e2[] = "enable-two";
	pc_addr a;
	struct pc_live *l1, *l2;
	int i, n;

	/* A */
	a = v4("11.0.0.1");
	CHK(!pc_resp_password_set() && !pc_resp_password_ok("", 0) && !pc_enable_configured() &&
		!pc_enable_secret_ok("x", 1) && !pc_http_token(),
		"A: no snapshot - no password, no enable secret, no token: every check fails closed");
	CHK(resp_ok(&a) && http_ok(&a), "A: no snapshot - the allow-lists open");

	/* B */
	cfg.resp_password = pw;
	cfg.http_token = tk;
	cfg.enable_secret[0] = e1;
	cfg.enable_secret[1] = e2;
	cfg.n_enable_secrets = 2;
	cidr(&cfg.resp_allow[0], "10.0.0.0", 8);
	cidr(&cfg.resp_allow[1], "2001:db8::", 32);
	cfg.n_resp_allow = 2;
	cidr(&cfg.http_allow[0], "192.168.1.0", 24);
	cfg.n_http_allow = 1;
	l1 = live_from(&cfg);
	CHK(l1 && l1->resp_password != pw && !strcmp(l1->resp_password, pw) &&
		l1->enable_secret[1] != e2 && !strcmp(l1->enable_secret[1], e2) &&
		l1->n_resp_allow == 2 && l1->n_http_allow == 1 && l1->http_allow[0].bits == 24,
		"B: a snapshot - the strings its own copies, the lists copied");
	live_install(l1);
	pw[0] = 'X';
	CHK(live() == l1 && pc_resp_password_ok("resp-pass-1", 11), "B: the config changed afterwards - the snapshot is not");
	pw[0] = 'r';

	/* C */
	CHK(pc_resp_password_set() && pc_resp_password_ok("resp-pass-1", 11), "C: the RESP password - exact, taken");
	CHK(!pc_resp_password_ok("resp-pass-12", 12) && !pc_resp_password_ok("resp-pass-1", 10) &&
		!pc_resp_password_ok("resp-pass-2", 11), "C: a byte more, a byte less, the last byte off - refused");
	CHK(pc_enable_configured() && pc_enable_secret_ok("enable-two", 10) && pc_enable_secret_ok("enable-one", 10),
		"C: the enable secrets - either of the rotation list");
	CHK(!pc_enable_secret_ok("enable-thr", 10) && !pc_enable_secret_ok("enable-on", 9) &&
		!pc_enable_secret_ok(NULL, 10), "C: neither, a prefix of one, no input - refused");
	CHK(pc_http_token() && !strcmp(pc_http_token(), "token-1"), "C: the HTTP token");

	/* D */
	a = v4("10.200.0.7");
	CHK(resp_ok(&a), "D: 10.200.0.7 in 10.0.0.0/8 - allowed");
	a = v6("2001:db8:5::1");
	CHK(resp_ok(&a), "D: 2001:db8:5::1 in 2001:db8::/32 - allowed");
	a = v6("::ffff:10.1.2.3");
	CHK(resp_ok(&a), "D: ::ffff:10.1.2.3 on a dual-stack door - matched as 10.1.2.3, allowed");
	a = v4("10.1.2.3");
	CHK(!pc_resp_peer_allowed(&a, sizeof a.in4 - 1), "D: a length short of an address - refused");
	logbuf[0] = 0;
	warns = 0;
	a = v4("11.0.0.1");
	CHK(!resp_ok(&a) && warns == 1 && strstr(logbuf, "RESP connection from 11.0.0.1 refused: not in resp_allow"),
		"D: 11.0.0.1 - refused, logged by address");
	a = v6("2001:db9::1");
	CHK(!resp_ok(&a), "D: 2001:db9::1 - refused");
	for (n = 0, i = 0; i < 20; i++)
		n += !resp_ok(&a);
	CHK(n == 20 && warns == 10 && strstr(logbuf, "not in resp_allow (further ones silent)"),
		"D: twenty more refused - ten logged in all, the tenth saying the rest are silent");
	logbuf[0] = 0;
	warns = 0;
	a = v4("192.168.1.77");
	CHK(http_ok(&a), "D: the HTTP list - 192.168.1.77 in 192.168.1.0/24");
	a = v4("10.200.0.7");
	CHK(!http_ok(&a) && warns == 1 && strstr(logbuf, "http connection from 10.200.0.7 refused: not in http_allow"),
		"D: the RESP list's 10.200.0.7 - refused at the HTTP door, logged on its own count");

	/* E */
	cfg.resp_password = "resp-pass-2";
	cfg.n_enable_secrets = 1;
	cfg.n_http_allow = 0;
	l2 = live_from(&cfg);
	live_install(l2);
	CHK(live() == l2 && pc_resp_password_ok("resp-pass-2", 11) && !pc_resp_password_ok("resp-pass-1", 11) &&
		!pc_enable_secret_ok("enable-two", 10) && http_ok(&a),
		"E: a reload - the new snapshot answers at once");
	CHK(!strcmp(l1->resp_password, "resp-pass-1") && !strcmp(l1->enable_secret[1], "enable-two"),
		"E: the one it replaced - retired, still readable, not wiped");
	now_s += 9;
	pc_live_retire_tick();
	CHK(!strcmp(l1->resp_password, "resp-pass-1"), "E: nine seconds on - still held");
	now_s += 1;
	pc_live_retire_tick();                 /* ten: freed - not read again */
	CHK(live() == l2, "E: ten seconds on - retired, the current one untouched");

	printf("doorlivetest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
