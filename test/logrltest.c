/* logrltest.c - the limit on network-driven log lines (logrl.c, CH-17), on
 * a clock the test moves; the log captured.
 *
 *   A  one line per reason and address a minute: the first said, the
 *      repeats held and counted, and the next said a minute on - after a
 *      line giving how many were held
 *   B  another address, another reason, no address at all: keys of
 *      their own
 *   C  at most LOGRL_CAP lines a minute across every key: the next one
 *      held, and the next said after the window carries the cap's count
 *   D  more keys than slots: still answered, the oldest slot reused
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include "compat/dprint.h"
#include "logrl.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* ---- the stub: the log, kept ------------------------------------------- */
int compat_log_level = L_NOTICE;
static char logbuf[4096];
static int lines;

void compat_log(int level, const char *tag, const char *fmt, ...)
{
	size_t n = strlen(logbuf);
	va_list ap;

	(void)level; (void)tag;
	va_start(ap, fmt);
	vsnprintf(logbuf + n, sizeof logbuf - n, fmt, ap);
	va_end(ap);
	lines++;
}

static void quiet(void)
{
	logbuf[0] = 0;
	lines = 0;
}

static pc_addr v4(unsigned a, unsigned b, unsigned c, unsigned d)
{
	pc_addr x;

	memset(&x, 0, sizeof x);
	x.in4.sin_family = AF_INET;
	x.in4.sin_addr.s_addr = htonl(a << 24 | b << 16 | c << 8 | d);
	x.in4.sin_port = htons(6480);
	return x;
}

int main(void)
{
	pc_addr a = v4(10, 0, 0, 1), b = v4(10, 0, 0, 2), a2 = a;
	long long t = 1000000;
	int i, n;

	/* A */
	quiet();
	CHK(pc_logrl_ok_at(LRL_JOIN_HELD, &a, t), "A: the first line - said");
	n = 0;
	for (i = 0; i < 5; i++)
		n += pc_logrl_ok_at(LRL_JOIN_HELD, &a, t + 1000 * i);
	CHK(n == 0 && lines == 0 && pc_logrl_held() == 5, "A: five more inside the minute - held, counted (5)");
	a2.in4.sin_port = htons(40000);
	CHK(!pc_logrl_ok_at(LRL_JOIN_HELD, &a2, t + 6000), "A: the same host from another port - the same key, held");
	CHK(pc_logrl_ok_at(LRL_JOIN_HELD, &a, t + LOGRL_WINDOW_MS) && lines == 1 &&
		strstr(logbuf, "log limit: 6 more 'join identity held' line(s) about 10.0.0.1 held back since the last one"),
		"A: a minute on - said, after the count of the six held");

	/* B */
	quiet();
	CHK(pc_logrl_ok_at(LRL_JOIN_HELD, &b, t + LOGRL_WINDOW_MS + 1) && lines == 0,
		"B: another address - its own key, said");
	CHK(pc_logrl_ok_at(LRL_EPOCH_ALIVE, &a, t + LOGRL_WINDOW_MS + 2), "B: another reason, the first address - its own key, said");
	CHK(pc_logrl_ok_at(LRL_HS_AUTH, NULL, t + LOGRL_WINDOW_MS + 3) &&
		!pc_logrl_ok_at(LRL_HS_AUTH, NULL, t + LOGRL_WINDOW_MS + 4),
		"B: no address (a unix-socket client) - one key of its own");

	/* C */
	t += 10 * LOGRL_WINDOW_MS;                  /* a fresh window */
	quiet();
	for (n = 0, i = 0; i < LOGRL_CAP; i++) {
		pc_addr x = v4(10, 1, (unsigned)i >> 8, (unsigned)i & 0xff);

		n += pc_logrl_ok_at(LRL_LANE_FORCE, &x, t + i);
	}
	{
		pc_addr x = v4(10, 2, 0, 1), y = v4(10, 2, 0, 2);

		CHK(n == LOGRL_CAP && !pc_logrl_ok_at(LRL_LANE_FORCE, &x, t + 100) &&
			!pc_logrl_ok_at(LRL_LANE_FORCE, &y, t + 101),
			"C: %d different addresses said, the next two held by the cap", LOGRL_CAP);
		quiet();
		CHK(pc_logrl_ok_at(LRL_LANE_FORCE, &x, t + LOGRL_WINDOW_MS + 100) &&
			strstr(logbuf, "log limit: 2 network-driven line(s) held back - more than 60 a minute") &&
			strstr(logbuf, "log limit: 1 more 'lane forced' line(s) about 10.2.0.1"),
			"C: the window over - said, after the cap's count and its own");
	}

	/* D */
	t += 10 * LOGRL_WINDOW_MS;
	for (n = 0, i = 0; i < LOGRL_SLOTS + 40; i++) {
		pc_addr x = v4(10, 3, (unsigned)i >> 8, (unsigned)i & 0xff);

		n += pc_logrl_ok_at(LRL_STATS_RESET, &x, t + LOGRL_WINDOW_MS * (long long)(i / LOGRL_CAP));
	}
	CHK(n == LOGRL_SLOTS + 40, "D: %d addresses over %d slots, each its first line - all said",
		LOGRL_SLOTS + 40, LOGRL_SLOTS);

	printf("logrltest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
