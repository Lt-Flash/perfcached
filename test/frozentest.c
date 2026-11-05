/* frozentest.c - S476: a member that takes the connection and says nothing must not hold a caller.
 * Usage: frozentest host1 host3 port secret secret3 pid3; node 3 frozen by SIGSTOP, its kernel still accepting. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "perfd.h"

#define NH 6
#define SETTLE_S 8.0
#define HOLD_MS 1500.0         /* idle_ping's 500 ms, and room for a loaded runner; the dial's old cost was 5,000 ms */
static int pass, fail;
static pid_t frozen;
#define OK(c, ...) do { if (c) { pass++; printf("  ok   "); } else { fail++; printf("  FAIL "); } \
	printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static double now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

static void thaw(void)
{
	if (frozen > 0)
		kill(frozen, SIGCONT);
	frozen = 0;
}

static perfd_t *dial(const char *host, int port, const char *const *sec, int idle_ping, int connect_ms)
{
	perfd_opts o;

	perfd_opts_init(&o);
	o.secrets = sec;
	o.idle_ping_ms = idle_ping;
	o.connect_timeout_ms = connect_ms;
	return perfd_connect(host, port, &o);
}

/* calls for @secs: the slowest, the ones that failed, the ones that read wrong */
static double drive(perfd_t **h, int n, double secs, int *errs, int *wrong)
{
	static char buf[256];
	double end = now_ms() + secs * 1e3, worst = 0;

	while (now_ms() < end) {
		int i;

		for (i = 0; i < n; i++) {
			size_t len = 0;
			double t = now_ms();
			int rc = perfd_get_buf(h[i], "c", "k", buf, sizeof buf, &len, NULL);

			t = now_ms() - t;
			worst = t > worst ? t : worst;
			if (rc < 0)
				(*errs)++;
			else if (len != 1 || buf[0] != 'v')
				(*wrong)++;
		}
		usleep(2000);
	}
	return worst;
}

int main(int argc, char **argv)
{
	const char *sec[3] = { NULL, NULL, NULL };
	perfd_t *h[NH], *n;
	int port, i, errs = 0, wrong = 0, spares = 0, back = 0, kind;
	unsigned long long rec = 0;
	double worst, t, deadline;

	if (argc != 7)
		return 2;
	port = atoi(argv[3]);
	sec[0] = argv[4];
	sec[1] = argv[5];
	frozen = (pid_t)atoi(argv[6]);
	atexit(thaw);
	/* idle_ping 1 s: the standby to node 3 fails its ping and is re-dialled while frozen */
	for (i = 0; i < NH; i++) {
		h[i] = dial(argv[1], port, sec, 1000, 0);
		if (!h[i]) {
			printf("  FAIL connect: %s\n", perfd_error(NULL));
			return 1;
		}
	}
	if (perfd_set(h[0], "c", "k", "v", 1, 0) != 0) {
		printf("  FAIL the first set: %s\n", perfd_error(h[0]));
		return 1;
	}
	usleep(500000);
	worst = drive(h, NH, 1.0, &errs, &wrong);
	for (i = 0; i < NH; i++)
		spares += perfd_spare_count(h[i]);
	OK(errs == 0 && wrong == 0 && spares == NH * 2, "before: %d standbys on %d handles, one on the list's other secret, slowest call %.1f ms", spares, NH, worst);

	kill(frozen, SIGSTOP);
	errs = wrong = 0;
	worst = drive(h, NH, SETTLE_S, &errs, &wrong);
	OK(worst < HOLD_MS, "live handles, %.0f s with a standby's node frozen: slowest call %.0f ms", SETTLE_S, worst);
	OK(errs == 0 && wrong == 0, "and %d calls failed, %d read wrong", errs, wrong);

	worst = 0;
	for (i = 0; i < 5; i++) {
		perfd_t *f[1];
		double w;

		t = now_ms();
		f[0] = dial(argv[1], port, sec, 0, 0);
		t = now_ms() - t;
		worst = t > worst ? t : worst;
		if (!f[0]) {
			OK(0, "a new handle with a member frozen: %s", perfd_error(NULL));
			continue;
		}
		w = drive(f, 1, 0.3, &errs, &wrong);
		worst = w > worst ? w : worst;
		perfd_free(f[0]);
	}
	OK(worst < HOLD_MS, "new handles, a listed member frozen: slowest connect or call %.0f ms", worst);

	t = now_ms();
	n = dial(argv[2], port, sec, 0, 300);
	t = now_ms() - t;
	kind = perfd_error_kind(NULL);
	OK(!n && t < HOLD_MS, "dialling the frozen node, connect_timeout_ms 300: gave up in %.0f ms", t);
	OK(!n && kind == PERFD_E_TRANSPORT, "and says no answer, not a wrong secret (kind %d): %s", kind, n ? "" : perfd_error(NULL));
	if (n)
		perfd_free(n);

	thaw();
	errs = wrong = 0;
	deadline = now_ms() + 45e3;
	do {
		worst = drive(h, NH, 0.5, &errs, &wrong);
		rec = 0;
		back = 0;
		for (i = 0; i < NH; i++) {
			rec += perfd_recovered(h[i]);
			back += perfd_spare_count(h[i]) == 2;
		}
	} while (back < NH && now_ms() < deadline);
	OK(back == NH, "thawed: %d of %d handles hold both standbys again, %llu recoveries counted", back, NH, rec);
	OK(rec >= 1, "the re-dial that waited in the background counts as a recovery");
	for (i = 0; i < NH; i++)
		perfd_free(h[i]);
	printf("frozentest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
