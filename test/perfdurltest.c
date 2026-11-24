/* SPDX-License-Identifier: GPL-2.0-or-later */
/* perfdurltest.c - perfd:// URLs (perfd.c compiled in): what parses and what is refused with its
 * reason, what perfd_url_apply writes, and perfd_url_connect's seed order, log lines and backoff. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <unistd.h>
#include "perfd.c"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static int warns, notices;
static char first_warn[512];

static void count_log(int level, const char *msg, void *ctx)
{
	(void)ctx;
	if (level == L_WARN && strstr(msg, "perfd url: seed ")) {
		if (!warns++)
			snprintf(first_warn, sizeof first_warn, "%s", msg);
	}
	if (level == L_NOTICE && strstr(msg, "answers again"))
		notices++;
}

/* a loopback port nothing listens on: bound, then closed */
static int closed_port(void)
{
	struct sockaddr_in a;
	socklen_t al = sizeof a;
	int fd = socket(AF_INET, SOCK_STREAM, 0), port = 0;

	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (fd >= 0 && bind(fd, (struct sockaddr *)&a, sizeof a) == 0 &&
	    getsockname(fd, (struct sockaddr *)&a, &al) == 0)
		port = ntohs(a.sin_port);
	if (fd >= 0)
		close(fd);
	return port;
}

static int listen_on(int port)
{
	struct sockaddr_in a;
	int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;

	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons((unsigned short)port);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 8) != 0) {
		if (fd >= 0)
			close(fd);
		return -1;
	}
	return fd;
}

/* @url must be refused, its reason holding @why */
static void refused(const char *url, const char *why)
{
	char err[256] = "unset";
	perfd_url *u = perfd_url_parse(url, err, sizeof err);

	CHK(!u && strstr(err, why) && !strncmp(err, "perfd url: ", 11),
		"refused %-44s (%s)", url, err);
	perfd_url_free(u);
}

int main(void)
{
	char err[256];
	perfd_url *u;
	const char *h;
	int port;

	printf("--- A. what parses\n");
	u = perfd_url_parse("perfd://127.0.0.1:6379/c", err, sizeof err);
	CHK(u && perfd_url_seed_count(u) == 1 && perfd_url_seed(u, 0, &h, &port) == 0 &&
		!strcmp(h, "127.0.0.1") && port == 6379, "A1 one seed (%s)", u ? "" : err);
	CHK(u && !strcmp(perfd_url_collection(u), "c") && !perfd_url_secret(u) &&
		!perfd_url_group(u), "A2 the collection; no secret, no group");
	CHK(u && perfd_url_seed(u, 1, &h, &port) == -1, "A3 no seed past the count");
	{
		perfd_opts o = PERFD_OPTS_INIT;

		o.connect_timeout_ms = 1234;
		o.secrets = NULL;
		perfd_url_apply(u, &o);
		CHK(o.route_keys == 1 && o.encrypt == 1 && o.spares == -1 &&
			o.policy == PERFD_POLICY_FAILOVER && o.secrets == NULL &&
			o.connect_timeout_ms == 1234,
			"A4 the defaults applied: route, encrypt, a standby per member, failover; "
			"the timeout stays the caller's");
	}
	perfd_url_free(u);

	u = perfd_url_parse("perfd:grp_1://us%3Aer:s%40cr%2Ft@h1:1,[::1]:2,h-3.example.org:65535/"
		"col_1.x?route=0,encrypt=0&spares=3,policy=least", err, sizeof err);
	CHK(u != NULL, "A5 a URL with every part (%s)", u ? "" : err);
	if (u) {
		perfd_opts o = PERFD_OPTS_INIT;

		CHK(!strcmp(perfd_url_group(u), "grp_1"), "A6 the group");
		CHK(!strcmp(perfd_url_secret(u), "s@cr/t"), "A7 the password is the secret, %%XX decoded");
		CHK(perfd_url_seed_count(u) == 3 && perfd_url_seed(u, 1, &h, &port) == 0 &&
			!strcmp(h, "::1") && port == 2, "A8 an IPv6 seed, its brackets dropped");
		CHK(perfd_url_seed(u, 2, &h, &port) == 0 && !strcmp(h, "h-3.example.org") &&
			port == 65535, "A9 a name, the top port");
		CHK(!strcmp(perfd_url_collection(u), "col_1.x"), "A10 the collection");
		perfd_url_apply(u, &o);
		CHK(o.route_keys == 0 && o.encrypt == 0 && o.spares == 3 &&
			o.policy == PERFD_POLICY_LEAST_CONN && o.secrets &&
			!strcmp(o.secrets[0], "s@cr/t") && !o.secrets[1],
			"A11 the options applied, and the secret as a one-entry list");
		perfd_url_free(u);
	}
	u = perfd_url_parse("perfd://onlyuser@h:1/c?spares=0,policy=weighted", err, sizeof err);
	CHK(u && !strcmp(perfd_url_secret(u), "onlyuser"), "A12 no password: the user is the secret");
	if (u) {
		perfd_opts o = PERFD_OPTS_INIT;

		perfd_url_apply(u, &o);
		CHK(o.spares == PERFD_SPARES_NONE && o.policy == PERFD_POLICY_WEIGHTED,
			"A13 spares=0 is no standby at all (PERFD_SPARES_NONE)");
		perfd_url_free(u);
	}
	u = perfd_url_parse("perfd://:pw@h:1/c?spares=-1&policy=rr", err, sizeof err);
	CHK(u && !strcmp(perfd_url_secret(u), "pw"), "A14 an empty user, a password");
	if (u) {
		perfd_opts o = PERFD_OPTS_INIT;

		perfd_url_apply(u, &o);
		CHK(o.spares == -1 && o.policy == PERFD_POLICY_ROUND_ROBIN, "A15 spares=-1, rr");
		perfd_url_free(u);
	}
	u = perfd_url_parse("perfd://h:1/c?dial_wait_ms=250", err, sizeof err);
	if (u) {
		perfd_opts o = PERFD_OPTS_INIT;

		o.dial_wait_ms = 900;
		perfd_url_apply(u, &o);
		CHK(o.dial_wait_ms == 250, "A15b dial_wait_ms=250 applied (%d)", o.dial_wait_ms);
		perfd_url_free(u);
	} else
		CHK(0, "A15b dial_wait_ms=250 parses ('%s')", err);
	u = perfd_url_parse("perfd://h:1/c?dial_wait_ms=-1", err, sizeof err);
	if (u) {
		perfd_opts o = PERFD_OPTS_INIT;

		perfd_url_apply(u, &o);
		CHK(o.dial_wait_ms == -1, "A15c dial_wait_ms=-1 applied (%d)", o.dial_wait_ms);
		perfd_url_free(u);
	} else
		CHK(0, "A15c dial_wait_ms=-1 parses ('%s')", err);
	u = perfd_url_parse("perfd://h:1/c", err, sizeof err);
	if (u) {
		perfd_opts o = PERFD_OPTS_INIT;

		o.dial_wait_ms = 900;
		perfd_url_apply(u, &o);
		CHK(o.dial_wait_ms == 900, "A15d a URL without it leaves the caller's dial_wait_ms (%d)",
			o.dial_wait_ms);
		perfd_url_free(u);
	}
	u = perfd_url_parse("perfd://a:1,b:2,c:3,d:4,e:5,f:6,g:7,h:8/c", err, sizeof err);
	CHK(u && perfd_url_seed_count(u) == 8, "A16 eight seeds");
	perfd_url_free(u);
	{
		/* a struct as an older program holds it: nothing written past its size */
		unsigned char raw[sizeof(perfd_opts) + 16];
		perfd_opts *o = (perfd_opts *)(void *)raw;
		size_t at = offsetof(perfd_opts, name), i;
		int clean = 1;

		memset(raw, 0xA5, sizeof raw);
		o->size = at;
		u = perfd_url_parse("perfd://h:1/c?encrypt=0", err, sizeof err);
		perfd_url_apply(u, o);
		for (i = at; i < sizeof raw; i++)
			clean &= raw[i] == 0xA5;
		CHK(u && o->route_keys == 1 && clean,
			"A17 apply writes only the fields a short struct holds");
		perfd_url_free(u);
	}

	printf("--- B. what is refused, and why\n");
	refused("redis://h:1/c", "scheme");
	refused("perfd:://h:1/c", "group");
	refused("perfd:a.b://h:1/c", "group");
	refused("perfd://h/c", ":port");
	refused("perfd://h:/c", "1 to 65535");
	refused("perfd://h:0/c", "1 to 65535");
	refused("perfd://h:65536/c", "1 to 65535");
	refused("perfd://h:080/c", "1 to 65535");
	refused("perfd://h:+80/c", "1 to 65535");
	refused("perfd://h:80x/c", "1 to 65535");
	refused("perfd://h:1", "/collection");
	refused("perfd://h:1/", "collection name");
	refused("perfd://h:1/a/b", "collection name");
	refused("perfd://h:1/c?", "empty option");
	refused("perfd://h:1/c?route=1,", "empty option");
	refused("perfd://h:1/c?route", "name=value");
	refused("perfd://h:1/c?route=2", "route is 0 or 1");
	refused("perfd://h:1/c?route=", "route is 0 or 1");
	refused("perfd://h:1/c?encrypt=yes", "encrypt is 0 or 1");
	refused("perfd://h:1/c?spares=9", "spares");
	refused("perfd://h:1/c?spares=-2", "spares");
	refused("perfd://h:1/c?policy=random", "policy");
	refused("perfd://h:1/c?dial_wait_ms=0", "dial_wait_ms");
	refused("perfd://h:1/c?dial_wait_ms=60001", "dial_wait_ms");
	refused("perfd://h:1/c?dial_wait_ms=-2", "dial_wait_ms");
	refused("perfd://h:1/c?bogus=1", "unknown option");
	refused("perfd://h:1/c?Route=1", "unknown option");
	refused("perfd://h:1/c?route=1&route=0", "twice");
	refused("perfd://[::1/c", "closing ]");
	refused("perfd://[zz::q]:1/c", "IPv6 address");
	refused("perfd://[::1]/c", ":port");
	refused("perfd://::1:5/c", "without a host");
	refused("perfd://fe80::1:5/c", "IPv6 seed goes in [ ]");
	refused("perfd://h_x:1/c", "host may hold");
	refused("perfd://:1/c", "without a host");
	refused("perfd://h:1,,h:2/c", "empty seed");
	refused("perfd://h:1,/c", "empty seed");
	refused("perfd:///c", "empty seed");
	refused("perfd://a:1,b:2,c:3,d:4,e:5,f:6,g:7,h:8,i:9/c", "at most 8 seeds");
	refused("perfd://u%4@h:1/c", "%XX");
	refused("perfd://u%zz@h:1/c", "%XX");
	refused("perfd://u%00x@h:1/c", "%XX");
	refused("perfd://@h:1/c", "empty secret");
	refused("perfd://:@h:1/c", "empty secret");
	CHK(perfd_url_parse(NULL, err, sizeof err) == NULL && strstr(err, "no URL"),
		"B1 NULL is refused");
	CHK(perfd_url_parse("perfd://h:0/c", NULL, 0) == NULL, "B2 no error buffer: still refused");
	{
		char tiny[8];

		CHK(perfd_url_parse("perfd://h:0/c", tiny, sizeof tiny) == NULL &&
			strlen(tiny) == sizeof tiny - 1, "B3 a short error buffer is cut, terminated");
	}
	CHK(perfd_url_collection(NULL) == NULL && perfd_url_secret(NULL) == NULL &&
		perfd_url_seed_count(NULL) == 0, "B4 the getters take NULL");
	perfd_url_free(NULL);

	printf("--- C. perfd_url_connect: seed order, one log line per outage, the backoff\n");
	{
		int pa = closed_port(), pb = closed_port(), lfd, i, ok;
		char url[160], want[32];
		perfd_opts o = PERFD_OPTS_INIT;
		perfd_t *p;
		pid_t pid;

		o.worker_balance = -1;
		o.connect_timeout_ms = 1000;
		perfd_set_log(count_log, NULL);
		snprintf(url, sizeof url, "perfd://127.0.0.1:%d,127.0.0.1:%d/c?spares=0", pa, pb);
		u = perfd_url_parse(url, err, sizeof err);
		CHK(u && pa && pb && pa != pb, "C1 two seeds nothing listens on (%d, %d)", pa, pb);
		if (!u)
			return 1;
		p = perfd_url_connect(u, &o);
		CHK(!p && perfd_error_kind(NULL) == PERFD_E_TRANSPORT &&
			strstr(perfd_error(NULL), "no seed answered"),
			"C2 neither answers: NULL, a transport error (%s)", perfd_error(NULL));
		CHK(warns == 2, "C3 each seed's failure logged (%d lines)", warns);
		snprintf(want, sizeof want, ":%d failed", getpid() % 2 ? pb : pa);
		CHK(strstr(first_warn, want) != NULL,
			"C4 the first seed tried is the pid's (%d %% 2): %s", (int)getpid(), first_warn);
		CHK(u->backoff_ms == 1000, "C5 a 1 s backoff (%d)", u->backoff_ms);
		p = perfd_url_connect(u, &o);
		CHK(!p && perfd_error_kind(NULL) == PERFD_E_BACKOFF &&
			strstr(perfd_error(NULL), "backing off"),
			"C6 the next call fails at once, PERFD_E_BACKOFF (%s)", perfd_error(NULL));
		CHK(warns == 2 && u->backoff_ms == 1000, "C7 and dials nothing: no log line, no doubling");
		ok = 1;
		for (i = 0; i < 6; i++) {
			static const int seq[] = { 2000, 4000, 8000, 16000, 30000, 30000 };

			u->hold_until_ms = 0;      /* as if the hold ran out */
			p = perfd_url_connect(u, &o);
			ok &= !p && u->backoff_ms == seq[i] &&
				u->hold_until_ms > now_ms_lib() + seq[i] - 1000;
		}
		CHK(ok, "C8 each dial none answers doubles the hold, to 30 s (%d)", u->backoff_ms);
		CHK(warns == 2, "C9 a seed still down is not logged again (%d lines)", warns);

		lfd = listen_on(pb);
		CHK(lfd >= 0, "C10 the second seed comes up");
		u->hold_until_ms = 0;
		p = perfd_url_connect(u, &o);
		CHK(p != NULL, "C11 a handle from the seed that answers (%s)", p ? "" : perfd_error(NULL));
		CHK(notices == 1 && warns == 2, "C12 its return logged once (%d), nothing else (%d)",
			notices, warns);
		CHK(u->backoff_ms == 0 && u->hold_until_ms == 0, "C13 the backoff reset");
		perfd_free(p);
		if (lfd >= 0)
			close(lfd);
		p = perfd_url_connect(u, &o);
		CHK(!p && warns == 3 && u->backoff_ms == 1000,
			"C14 down again: a new outage, logged again (%d), the hold from 1 s", warns);

		fflush(stdout);
		pid = fork();
		if (pid == 0) {
			/* the parent's hold is not the child's: it dials */
			p = perfd_url_connect(u, &o);
			_exit(!p && perfd_error_kind(NULL) == PERFD_E_TRANSPORT ? 0 : 1);
		}
		if (pid > 0) {
			int st = 0;

			waitpid(pid, &st, 0);
			CHK(WIFEXITED(st) && WEXITSTATUS(st) == 0,
				"C15 a forked child starts without the parent's backoff");
		}
		CHK(perfd_url_connect(NULL, &o) == NULL && perfd_error_kind(NULL) == PERFD_E_ARG,
			"C16 no URL: PERFD_E_ARG");
		perfd_set_log(NULL, NULL);
		perfd_url_free(u);
	}

	printf("perfdurltest: %s\n", fails ? "FAILED" : "all passed");
	return fails ? 1 : 0;
}
