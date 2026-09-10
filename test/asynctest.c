/*
 * asynctest.c — libperfd's event-loop surface (S32) driven by a REAL
 * event loop, which is the only way the claim means anything.
 *
 * The consumer here is shaped like the one S32 exists for: rtpengine
 * owns a libevent loop and cannot afford a blocking call in it, because
 * one stall there is every call on the box.  So this test uses libevent,
 * never calls a blocking libperfd function, and proves the "no stall"
 * property with a WATCHDOG rather than by inspection: a 5 ms repeating
 * timer records how late it ever fires.  A blocking handshake (Argon2id
 * alone is ~100 ms) or a blocking drain would show up there as lateness;
 * inspection would not show it at all.
 *
 * Four things are asserted, matching the task's proof obligations:
 *   2. set/get/del/keys complete with zero blocking calls, watchdog clean
 *   3. the daemon is killed MID-LOAD: every in-flight request must get
 *      its callback (correlation is never silently lost), and the
 *      reconnect - handshake included - happens inside the loop
 *   4. a large multi-record KEYS reply drains across several loop
 *      iterations without a single long stall
 * (1, that the blocking suites are unchanged, is the rest of `make check`.)
 *
 * usage: asynctest <host> <port> <secret|-> <col>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <event2/event.h>

#include "../lib/perfd.h"

#define NKEYS      3000
#define TICK_MS    5
#define STALL_MS   60          /* a blocking handshake would be ~100 ms */

static struct event_base *base;
static struct event *ev_read, *ev_write, *ev_tick;
static perfd_t *P;
static const char *g_host, *g_secret, *g_col;
static int g_port;

static double t_last_tick, t_worst_late;
static int pass, fail;
static int n_set_ok, n_get_ok, n_err;
static int keys_seen = -1;
static int phase;
static int inflight_at_kill, cbs_after_kill;
static double t_reconnect_start, t_reconnect_done;

static void ok(const char *what)
{
	pass++;
	printf("  ok   %s\n", what);
}
static void bad(const char *what)
{
	fail++;
	printf("  FAIL %s\n", what);
}

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* re-arm write interest: perfd_events() is not constant - a partial
 * write turns it on, a drained buffer turns it off */
static void rearm(void)
{
	int ev = perfd_events(P);

	if (ev & PERFD_EV_WRITE)
		event_add(ev_write, NULL);
	else
		event_del(ev_write);
}

static void die(const char *why)
{
	printf("  FAIL %s: %s\n", why, perfd_error(P));
	fail++;
	event_base_loopbreak(base);
}

/* ---- phases ------------------------------------------------------------ */

static void start_phase(int n);

static void cb_set(char *res, size_t len, const char *e, void *ctx)
{
	(void)len; (void)ctx; (void)e;
	if (res)
		n_set_ok++;
	else
		n_err++;
	free(res);
	if (n_set_ok + n_err == NKEYS)
		start_phase(2);
}

static void cb_get(char *res, size_t len, const char *e, void *ctx)
{
	(void)len; (void)ctx; (void)e;
	if (res && strstr(res, "\"found\""))
		n_get_ok++;
	else
		n_err++;
	free(res);
	if (n_get_ok + n_err == NKEYS)
		start_phase(3);
}

/* obligation 4: this reply is big and arrives over several records */
static void cb_keys(char *res, size_t len, const char *e, void *ctx)
{
	(void)ctx;
	if (!res) {
		printf("  FAIL keys: %s\n", e ? e : "?");
		fail++;
		event_base_loopbreak(base);
		return;
	}
	keys_seen = 0;
	{
		const char *q = res;

		while ((q = strchr(q, '"'))) {   /* count quoted strings / 2 */
			q++;
			keys_seen++;
		}
		keys_seen /= 2;
	}
	printf("  ..   keys reply %zu bytes, ~%d keys\n", len, keys_seen);
	free(res);
	start_phase(4);
}

/* obligation 3: these were in flight when the daemon died */
static void cb_inflight(char *res, size_t len, const char *e, void *ctx)
{
	(void)len; (void)e; (void)ctx;
	cbs_after_kill++;
	free(res);
}

static void cb_after_reconnect(char *res, size_t len, const char *e,
		void *ctx)
{
	(void)len; (void)ctx;
	t_reconnect_done = now_ms();
	if (res)
		ok("a request completes after an in-loop reconnect");
	else
		printf("  FAIL post-reconnect request: %s\n", e ? e : "?"),
			fail++;
	free(res);
	event_base_loopbreak(base);
}

static void start_phase(int n)
{
	char k[64], params[256];
	int i;

	phase = n;
	if (n == 1) {
		printf("  ..   phase 1: %d async SETs\n", NKEYS);
		for (i = 0; i < NKEYS; i++) {
			snprintf(k, sizeof k, "as%06d", i);
			snprintf(params, sizeof params,
				"{\"col\":\"%s\",\"key\":\"%s\",\"value\":"
				"\"payload-%06d\"}", g_col, k, i);
			if (perfd_submit(P, "set", params, cb_set, NULL) != 0) {
				die("submit set");
				return;
			}
		}
		rearm();
	} else if (n == 2) {
		if (n_set_ok == NKEYS)
			ok("every async SET completed");
		else
			bad("async SETs completed");
		printf("  ..   phase 2: %d async GETs\n", NKEYS);
		n_err = 0;
		for (i = 0; i < NKEYS; i++) {
			snprintf(k, sizeof k, "as%06d", i);
			snprintf(params, sizeof params,
				"{\"col\":\"%s\",\"key\":\"%s\"}", g_col, k);
			if (perfd_submit(P, "get", params, cb_get, NULL) != 0) {
				die("submit get");
				return;
			}
		}
		rearm();
	} else if (n == 3) {
		if (n_get_ok == NKEYS)
			ok("every async GET completed");
		else
			bad("async GETs completed");
		printf("  ..   phase 3: one large KEYS reply\n");
		snprintf(params, sizeof params,
			"{\"col\":\"%s\",\"limit\":100000}", g_col);
		if (perfd_submit(P, "keys", params, cb_keys, NULL) != 0) {
			die("submit keys");
			return;
		}
		rearm();
	} else if (n == 4) {
		if (keys_seen >= NKEYS)
			ok("large KEYS reply drained whole");
		else
			bad("large KEYS reply drained whole");
		/* obligation 3: put requests in flight, then pull the rug */
		printf("  ..   phase 4: %d requests in flight, then kill\n",
			200);
		for (i = 0; i < 200; i++) {
			snprintf(k, sizeof k, "as%06d", i);
			snprintf(params, sizeof params,
				"{\"col\":\"%s\",\"key\":\"%s\"}", g_col, k);
			perfd_submit(P, "get", params, cb_inflight, NULL);
		}
		inflight_at_kill = perfd_inflight(P);
		rearm();
		/* the bracket keeps the pattern from matching the shell
		 * system() spawns to run it - pkill -f reads its own
		 * command line too, and would kill the killer */
		if (system("pkill -9 -f '[p]erfcached -f /var/tmp/pcasync' "
		        ">/dev/null 2>&1") == -1)
			printf("  ..   (kill returned -1)\n");
	}
}

/* ---- loop plumbing ----------------------------------------------------- */

static void on_read(evutil_socket_t fd, short what, void *arg)
{
	(void)fd; (void)what; (void)arg;
	if (perfd_read_ready(P) != 0) {
		if (phase == 4) {
			/* the kill: every in-flight callback must have run */
			if (cbs_after_kill == inflight_at_kill && inflight_at_kill)
				ok("every in-flight request got its callback");
			else
				bad("every in-flight request got its callback");
			if (perfd_inflight(P) == 0)
				ok("no request left uncorrelated");
			else
				bad("no request left uncorrelated");
			phase = 5;
		}
		if (phase >= 5) {
			/* expected from here on: the node is gone and the
			 * dead fd would otherwise spin the loop.  Disarm and
			 * let the watchdog tick drive the reconnect. */
			event_del(ev_read);
			event_del(ev_write);
			return;
		}
		die("read_ready");
		return;
	}
	/* the Noise handshake completes on a READ (msg2 arriving), and by
	 * then the write event is disarmed - so the first phase has to be
	 * able to start from here as well as from on_write, or a run over
	 * an encrypted channel simply sits there */
	if (perfd_state(P) == PERFD_ST_READY && phase == 0)
		start_phase(1);
	rearm();
}

static void on_write(evutil_socket_t fd, short what, void *arg)
{
	(void)fd; (void)what; (void)arg;
	if (perfd_write_ready(P) != 0) {
		if (phase < 4)
			die("write_ready");
		else {
			event_del(ev_read);
			event_del(ev_write);
		}
		return;
	}
	if (perfd_state(P) == PERFD_ST_READY && phase == 0)
		start_phase(1);
	rearm();
}

/* the watchdog: if the loop is ever blocked, this timer fires late */
static void on_tick(evutil_socket_t fd, short what, void *arg)
{
	double t = now_ms(), late;

	(void)fd; (void)what; (void)arg;
	if (t_last_tick > 0) {
		late = (t - t_last_tick) - TICK_MS;
		if (late > t_worst_late)
			t_worst_late = late;
	}
	t_last_tick = t;

	/* phase 5: the daemon is being restarted by the harness; keep
	 * trying to reconnect from INSIDE the loop, never blocking */
	if (phase == 5 && perfd_state(P) == PERFD_ST_FAILED) {
		perfd_t *n;
		perfd_opts o;
		const char *sec[2];
		static int tries;

		if (++tries % 200 == 1)
			printf("  ..   reconnect attempt %d\n", tries);
		if (tries > 3000) {
			bad("reconnect never succeeded");
			event_base_loopbreak(base);
			return;
		}

		memset(&o, 0, sizeof o);
		sec[0] = g_secret;
		sec[1] = NULL;
		o.secrets = strcmp(g_secret, "-") ? sec : NULL;
		n = perfd_connect_async(g_host, g_port, &o);
		if (!n)
			return;                /* daemon not back yet */
		/* ORDER MATTERS: drop the events BEFORE freeing the handle.
		 * perfd_free() closes the fd, and libevent then cannot
		 * deregister it - "Epoll MOD ... Bad file descriptor" and a
		 * loop that never fires again. */
		event_del(ev_read);
		event_del(ev_write);
		event_free(ev_read);
		event_free(ev_write);
		perfd_free(P);
		P = n;
		if (t_reconnect_start == 0)
			t_reconnect_start = now_ms();
		ev_read = event_new(base, perfd_fd(P), EV_READ | EV_PERSIST,
			on_read, NULL);
		ev_write = event_new(base, perfd_fd(P), EV_WRITE | EV_PERSIST,
			on_write, NULL);
		event_add(ev_read, NULL);
		event_add(ev_write, NULL);
		phase = 6;
		return;
	}
	/* A reconnect attempted while the node is still down fails
	 * ASYNCHRONOUSLY - connect() returns EINPROGRESS and the refusal
	 * arrives as writability with SO_ERROR set.  So "failed" here is
	 * an ordinary retry, not a terminal state; treating it as terminal
	 * is what made this test hang. */
	if (phase == 6 && perfd_state(P) == PERFD_ST_FAILED) {
		static int refused;

		if (++refused % 200 == 1)
			printf("  ..   reconnect refused (%s), retrying\n",
				perfd_error(P));
		phase = 5;
		return;
	}
	if (phase == 6 && perfd_state(P) == PERFD_ST_READY) {
		char params[128];

		phase = 7;
		snprintf(params, sizeof params,
			"{\"col\":\"%s\",\"key\":\"as000001\"}", g_col);
		if (perfd_submit(P, "get", params, cb_after_reconnect,
		        NULL) != 0)
			die("submit after reconnect");
		rearm();
	}
}

int main(int argc, char **argv)
{
	perfd_opts o;
	const char *sec[2];
	struct timeval tick = { 0, TICK_MS * 1000 };

	if (argc < 5) {
		fprintf(stderr, "usage: %s host port secret|- col\n", argv[0]);
		return 2;
	}
	g_host = argv[1];
	g_port = atoi(argv[2]);
	g_secret = argv[3];
	g_col = argv[4];

	memset(&o, 0, sizeof o);
	sec[0] = g_secret;
	sec[1] = NULL;
	o.secrets = strcmp(g_secret, "-") ? sec : NULL;

	base = event_base_new();
	if (!base) {
		fprintf(stderr, "event_base_new failed\n");
		return 2;
	}
	P = perfd_connect_async(g_host, g_port, &o);
	if (!P) {
		fprintf(stderr, "connect_async: %s\n", perfd_error(NULL));
		return 2;
	}
	printf("  ..   connect_async returned immediately, state=%d\n",
		perfd_state(P));
	if (perfd_state(P) == PERFD_ST_CONNECTING)
		ok("connect_async did not block");
	else
		bad("connect_async did not block");

	ev_read = event_new(base, perfd_fd(P), EV_READ | EV_PERSIST,
		on_read, NULL);
	ev_write = event_new(base, perfd_fd(P), EV_WRITE | EV_PERSIST,
		on_write, NULL);
	ev_tick = event_new(base, -1, EV_PERSIST, on_tick, NULL);
	event_add(ev_read, NULL);
	event_add(ev_write, NULL);
	event_add(ev_tick, &tick);

	event_base_dispatch(base);

	printf("  ..   worst loop lateness %.1f ms (threshold %d)\n",
		t_worst_late, STALL_MS);
	if (t_worst_late < STALL_MS)
		ok("the event loop was never stalled");
	else
		bad("the event loop was never stalled");
	if (t_reconnect_done > t_reconnect_start && t_reconnect_start > 0)
		printf("  ..   in-loop reconnect + handshake took %.1f ms\n",
			t_reconnect_done - t_reconnect_start);

	printf("asynctest: %d passed, %d failed\n", pass, fail);
	perfd_free(P);
	event_free(ev_read);
	event_free(ev_write);
	event_free(ev_tick);
	event_base_free(base);
	return fail ? 1 : 0;
}
