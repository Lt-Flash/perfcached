/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * wedgetest.c — a failed request must not disable the handle (task S72).
 *
 * The defect this exists to catch: the pipeline counters (nids/idhead,
 * qlen) were cleared ONLY on the successful collect path, so a request
 * that was sent but whose reply never arrived left the handle
 * permanently "mid-pipeline".  Every later call was then refused
 * locally with "pipeline in progress" - without touching the socket,
 * without setting `poisoned`, and therefore without ever reaching the
 * failover that would have rescued it.  The guard blocked its own
 * recovery.
 *
 * Reproduced in production on 2026-09-04 by restarting a perfcached:
 * one EOF, then "pipeline in progress" for the life of the process.
 *
 * The test drives it with NO standbys (spares=0) on purpose.  With
 * standbys the failover path adopts a fresh connection and hides the
 * bug; the bare handle is where the unwind has to be correct.
 *
 * usage: wedgetest <host> <port> <secret> <col>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../lib/perfd.h"

static int fails;

static void bad(const char *what, const char *detail)
{
	printf("FAIL: %s (%s)\n", what, detail ? detail : "-");
	fails++;
}

int main(int argc, char **argv)
{
	const char *host, *sec[2] = { NULL, NULL };
	const char *col;
	perfd_opts o;
	perfd_t *p;
	void *v = NULL;
	size_t vl = 0;
	long long gttl = 0;
	int port, i;

	if (argc < 5) {
		fprintf(stderr, "usage: %s host port secret col\n", argv[0]);
		return 2;
	}
	host = argv[1]; port = atoi(argv[2]); sec[0] = argv[3]; col = argv[4];

	memset(&o, 0, sizeof o);
	o.secrets = sec;
	o.connect_timeout_ms = 3000;
	o.io_timeout_ms = 3000;
	o.spares = 0;                  /* no standby: test the bare handle */
	o.route_keys = 0;

	p = perfd_connect(host, port, &o);
	if (!p) {
		printf("FAIL: connect: %s\n", perfd_error(NULL));
		return 1;
	}
	if (perfd_set(p, col, "wedge_probe", "before", 6, 60) != 0)
		bad("the first set should succeed", perfd_error(p));

	/* The daemon is restarted by the harness while we sit here.  Any
	 * request across that gap must fail - that is not the bug. */
	printf("  (waiting for the harness to restart the daemon)\n");
	fflush(stdout);
	sleep(8);

	/* First request after the restart: expected to fail, and expected
	 * to say something about the CONNECTION, not about a pipeline the
	 * caller never opened. */
	if (perfd_get(p, col, "wedge_probe", &v, &vl, &gttl) >= 0) {
		free(v); v = NULL;
		printf("  (the first post-restart request succeeded - "
			"the handle reconnected already)\n");
	} else {
		printf("  first post-restart error: %s\n", perfd_error(p));
		if (strstr(perfd_error(p), "pipeline in progress"))
			bad("the FIRST failure blamed a phantom pipeline",
				perfd_error(p));
	}

	/*
	 * THE ASSERTIONS.  libperfd deliberately does NOT reconnect by
	 * itself - that is the caller's decision, made through
	 * perfd_state() - so the contract after a failure is:
	 *
	 *   1. the pipeline is unwound (nothing left outstanding),
	 *   2. the handle says it is FAILED rather than inventing a
	 *      pipeline the caller never opened,
	 *   3. a caller that acts on that can reconnect and carry on.
	 *
	 * Before the fix, (1) was false, which made (2) impossible: the
	 * stale counters made every later call return "pipeline in
	 * progress" without touching the socket, so `poisoned` was never
	 * reached and the handle was dead for good.
	 */
	if (perfd_pending(p) != 0)
		bad("a failed one-shot left replies outstanding",
			"perfd_pending() != 0");

	for (i = 0; i < 3; i++) {
		if (perfd_get(p, col, "wedge_probe", &v, &vl, &gttl) >= 0) {
			free(v); v = NULL;
			break;                 /* it recovered on its own */
		}
		if (strstr(perfd_error(p), "pipeline in progress"))
			bad("handle wedged: 'pipeline in progress' on a "
				"handle the caller never pipelined",
				perfd_error(p));
		if (perfd_pending(p) != 0)
			bad("still outstanding after a later failure",
				"perfd_pending() != 0");
	}

	if (perfd_state(p) != PERFD_ST_READY &&
	    perfd_state(p) != PERFD_ST_FAILED)
		bad("state is neither READY nor FAILED after a broken "
			"connection", "perfd_state()");

	/* and the caller's recovery must actually work */
	perfd_free(p);
	p = perfd_connect(host, port, &o);
	if (!p) {
		printf("FAIL: reconnect after the restart: %s\n",
			perfd_error(NULL));
		fails++;
		printf(fails ? "wedgetest: %d FAILED\n" : "wedgetest: ok\n",
			fails);
		return 1;
	}
	if (perfd_get(p, col, "wedge_probe", &v, &vl, &gttl) < 0)
		bad("a freshly reconnected handle still fails",
			perfd_error(p));
	else
		free(v);

	perfd_free(p);
	printf(fails ? "wedgetest: %d FAILED\n" : "wedgetest: ok\n", fails);
	return fails ? 1 : 0;
}
