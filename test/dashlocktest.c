/* SPDX-License-Identifier: GPL-2.0-or-later */
/* dashlocktest.c - the dashboard's fleet lock (perfcached-dashboard.c compiled in, its main renamed,
 * its dials answered by a member that does not answer): a dial holds no other route behind it;
 * the lockout table counts every wrong secret of parallel requests. */
#include <pthread.h>
#include <time.h>

#define perfd_connect test_perfd_connect
#define main dashboard_main
#include "../cli/perfcached-dashboard.c"
#undef main
#undef perfd_connect

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

#define DIAL_MS 2200                   /* past connect_timeout_ms, as a blackholed host costs */
static int dials;

static void sleep_ms(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

/* every member is a host that drops the SYN: the dial fails after DIAL_MS */
perfd_t *test_perfd_connect(const char *host, int port, const perfd_opts *opts)
{
	(void)host;
	(void)port;
	(void)opts;
	__atomic_add_fetch(&dials, 1, __ATOMIC_SEQ_CST);
	sleep_ms(DIAL_MS);
	return NULL;
}

/* what every route does first */
static void *route(void *a)
{
	pthread_mutex_lock(&FL.mu);
	learn();
	pthread_mutex_unlock(&FL.mu);
	return a;
}

#define GUESSES 250000

static void *guesser(void *a)
{
	int i;

	for (i = 0; i < GUESSES; i++)
		note_failure("198.51.100.7");
	return a;
}

int main(void)
{
	printf("--- A. a member that does not answer: its dial holds no route behind the fleet lock\n");
	{
		pthread_t t;
		long long t0, waited;
		int i, d;

		pthread_mutex_lock(&FL.mu);
		member_add("192.0.2.1", 6479, 0, 1);
		pthread_mutex_unlock(&FL.mu);
		if (pthread_create(&t, NULL, route, NULL) != 0) {
			printf("  FAIL pthread_create\n");
			return 1;
		}
		for (i = 0; i < 2000 && !__atomic_load_n(&dials, __ATOMIC_SEQ_CST); i++)
			sleep_ms(1);
		/* past the one-a-second dial limit, the first dial still in flight */
		sleep_ms(1200);
		t0 = now_ms();
		pthread_mutex_lock(&FL.mu);
		waited = now_ms() - t0;
		member_connect(&FL.m[0]);
		pthread_mutex_unlock(&FL.mu);
		pthread_join(t, NULL);
		d = __atomic_load_n(&dials, __ATOMIC_SEQ_CST);
		CHK(waited < 500, "A1 another route takes the lock while the dial runs (waited %lld ms)",
			waited);
		CHK(d == 1, "A2 one dial for the member in all: the pass dials it once and another route "
			"does not dial it again while that dial is out (%d dials)", d);
	}

	printf("--- B. wrong secrets from one address on the two HTTP threads at once: none is lost\n");
	{
		pthread_t g[4];
		int i, n = 0;

		CF.lockout_after = 1 << 30;   /* count, never lock out */
		for (i = 0; i < 4; i++)
			if (pthread_create(&g[i], NULL, guesser, NULL) == 0)
				n++;
		for (i = 0; i < n; i++)
			pthread_join(g[i], NULL);
		CHK(n == 4 && lock_slot("198.51.100.7")->fails == 4 * GUESSES,
			"B1 4 x %d failures counted (%d)", GUESSES, lock_slot("198.51.100.7")->fails);
	}

	printf("dashlocktest: %s\n", fails ? "FAILED" : "all passed");
	return fails ? 1 : 0;
}
