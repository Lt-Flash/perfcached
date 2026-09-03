/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * asyncbintest.c — the BINARY async path: warm-up and reply decoding.
 *
 * asynctest.c drives perfd_submit(), which is JSON whatever opts.binary
 * says, so perfd_submit_kv() - the binary async call - had no coverage
 * at all.  The OpenSIPS driver's async fetch (cachedb_perfd, S28) is
 * built on exactly these two things, and both are easy to get wrong:
 *
 *  1. WARM-UP.  perfd_connect_async() returns in PERFD_ST_CONNECTING;
 *     TCP and the Noise handshake are driven by readiness events.  A
 *     consumer that submits before PERFD_ST_READY gets "connection not
 *     ready", so the poll loop below is the shape a caller needs.
 *  2. DECODING.  A binary GET answers [u8 found][u32 ttl][value], and a
 *     MISS is a SUCCESSFUL frame carrying a single zero byte - not an
 *     error frame, and not a NULL result.  Reading the payload as the
 *     value yields a one-byte NUL string on every miss, which looks
 *     like a stored empty value to whatever asked.
 *
 * Usage: asyncbintest <host> <port> <secret|-> <col>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>

#include "../lib/perfd.h"

static int pass, fail;

static void ok(const char *what)  { pass++; printf("  ok   %s\n", what); }
static void bad(const char *what) { fail++; printf("  FAIL %s\n", what); }

struct reply {
	char *res;
	size_t len;
	char err[256];
	int fired, failed;
};

static void cb(char *res, size_t len, const char *e, void *ctx)
{
	struct reply *r = ctx;

	r->fired = 1;
	if (!res) {
		r->failed = 1;
		snprintf(r->err, sizeof r->err, "%s", e ? e : "?");
		return;
	}
	r->res = res;
	r->len = len;
}

/* drive the handle until @done() says stop, or the budget runs out */
static int pump(perfd_t *p, const int *done, int budget_ms)
{
	while (!*done && budget_ms > 0) {
		struct pollfd pfd;
		int ev = perfd_events(p), rc;

		pfd.fd = perfd_fd(p);
		if (pfd.fd < 0)
			return -1;
		pfd.events = 0;
		if (ev & PERFD_EV_READ)
			pfd.events |= POLLIN;
		if (ev & PERFD_EV_WRITE)
			pfd.events |= POLLOUT;
		if (!pfd.events)
			pfd.events = POLLIN;
		rc = poll(&pfd, 1, 100);
		if (rc < 0)
			return -1;
		if (rc == 0) {
			budget_ms -= 100;
			continue;
		}
		if ((pfd.revents & POLLIN) && perfd_read_ready(p) < 0)
			return -1;
		if ((pfd.revents & POLLOUT) && perfd_write_ready(p) < 0)
			return -1;
	}
	return *done ? 0 : -1;
}

int main(int argc, char **argv)
{
	const char *host, *secret, *col;
	const char *sec[2];
	perfd_opts o;
	perfd_t *p;
	struct reply rs, rg, rm;
	int port;

	if (argc < 5) {
		fprintf(stderr, "usage: %s host port secret|- col\n", argv[0]);
		return 2;
	}
	host = argv[1];
	port = atoi(argv[2]);
	secret = argv[3];
	col = argv[4];

	memset(&o, 0, sizeof o);
	sec[0] = secret;
	sec[1] = NULL;
	o.secrets = strcmp(secret, "-") ? sec : NULL;
	o.binary = 1;

	p = perfd_connect_async(host, port, &o);
	if (!p) {
		printf("  FAIL async connect: %s\n", perfd_error(NULL));
		return 1;
	}

	/* 1. warm-up: CONNECTING -> READY under a poll loop */
	while (perfd_state(p) == PERFD_ST_CONNECTING) {
		int dummy = 0;

		if (pump(p, &dummy, 100) < 0 &&
		    perfd_state(p) == PERFD_ST_FAILED)
			break;
		if (perfd_state(p) != PERFD_ST_CONNECTING)
			break;
	}
	if (perfd_state(p) == PERFD_ST_READY)
		ok("a fresh async handle reaches READY under a poll loop");
	else
		bad("the async handshake never completed");

	if (perfd_state(p) != PERFD_ST_READY) {
		printf("asyncbintest: %d passed, %d failed\n", pass, fail + 1);
		return 1;
	}

	/* 2. a binary SET, so there is something to hit */
	memset(&rs, 0, sizeof rs);
	if (perfd_submit_kv(p, PERFD_V_SET, col, "abk", "hello", 5, 0, 0,
	        cb, &rs) != 0 || perfd_push(p) < 0) {
		bad("binary async SET could not be submitted");
	} else if (pump(p, &rs.fired, 5000) < 0 || rs.failed) {
		bad("binary async SET did not complete");
	} else {
		ok("binary async SET completed");
	}
	free(rs.res);

	/* 3. a HIT decodes as [1][u32 ttl][value] */
	memset(&rg, 0, sizeof rg);
	if (perfd_submit_kv(p, PERFD_V_GET, col, "abk", NULL, 0, 0, 0,
	        cb, &rg) != 0 || perfd_push(p) < 0) {
		bad("binary async GET could not be submitted");
	} else if (pump(p, &rg.fired, 5000) < 0 || rg.failed) {
		bad("binary async GET did not complete");
	} else if (rg.len < 5 || rg.res[0] != 1) {
		bad("a hit did not answer [found=1][ttl][value]");
		printf("       (len=%zu first=%d)\n", rg.len,
			rg.len ? rg.res[0] : -1);
	} else if (rg.len - 5 != 5 || memcmp(rg.res + 5, "hello", 5)) {
		bad("the value did not start at offset 5");
		printf("       (payload len=%zu)\n", rg.len - 5);
	} else {
		ok("a hit carries found=1 and its value at offset 5");
	}
	free(rg.res);

	/* 4. THE ONE THAT BITES: a miss is a SUCCESSFUL one-byte frame */
	memset(&rm, 0, sizeof rm);
	if (perfd_submit_kv(p, PERFD_V_GET, col, "abk-absent", NULL, 0, 0, 0,
	        cb, &rm) != 0 || perfd_push(p) < 0) {
		bad("binary async GET (miss) could not be submitted");
	} else if (pump(p, &rm.fired, 5000) < 0) {
		bad("binary async GET (miss) did not complete");
	} else if (rm.failed || !rm.res) {
		bad("a miss arrived as an ERROR - indistinguishable from "
			"a broken connection");
		printf("       (err=%s)\n", rm.err);
	} else if (rm.len != 1 || rm.res[0] != 0) {
		bad("a miss was not a single found=0 byte");
		printf("       (len=%zu first=%d)\n", rm.len,
			rm.len ? rm.res[0] : -1);
	} else {
		ok("a miss is a successful frame carrying found=0");
	}
	free(rm.res);

	perfd_free(p);
	printf("asyncbintest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
