/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* obstest.c - what the command rows and the query log keep of a client's bytes: a row's name is
 * printable whatever the dialect sent (INFO prints it raw); AUTH's password is never the query
 * log's key, hashed or in full; the five-minute window's max includes the minute in progress; the
 * slow log's newest entry is shown whatever worker logged it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "compat/compat.h"
#include "json.h"
#include "obs.h"

int pc_obs_init(int nworkers, long long slowlog_usec);

/* the query log's lines for one call, read back from stderr (compat_log writes there) */
static char qbuf[4096];

static const char *qlog_line(int keys, const char *verb, const char *key)
{
	static int fd = -1;
	FILE *f;
	size_t n;

	if (fd < 0) {
		char tmp[] = "/var/tmp/obstest.XXXXXX";

		fd = mkstemp(tmp);
		if (fd < 0 || !freopen(tmp, "w+", stderr))
			return "";
		unlink(tmp);
	}
	f = stderr;
	fflush(f);
	rewind(f);
	if (ftruncate(fileno(f), 0) != 0)
		return "";
	pc_obs_qlog_config(1, keys);
	pc_obs_qlog("resp", NULL, verb, strlen(verb), NULL, 0, key, strlen(key), "ok", 5);
	fflush(f);
	rewind(f);
	n = fread(qbuf, 1, sizeof qbuf - 1, f);
	qbuf[n] = 0;
	return qbuf;
}

/* obs.c reaches for the worker id: the row a call is counted in */
static int wid;
int pc_worker_id(void) { return wid; }

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

int main(void)
{
	static struct pc_obs_cmdsum out[128];
	int n, i, raw = 0;
	unsigned int k;

	if (pc_obs_init(1, -1) != 0) {
		printf("obs init failed\n");
		return 2;
	}

	/* A: a name holding CR LF, a control byte and bytes that are not UTF-8 - its row prints clean */
	pc_obs_cmd_at(pc_obs_cmd_slot("x\r\ninjected:1\x01\xff", 15), 5);
	pc_obs_cmd("y\r\nz", 4, 5);
	n = pc_obs_cmd_merge(out, 128);
	for (i = 0; i < n; i++)
		for (k = 0; k < out[i].nlen; k++)
			if ((unsigned char)out[i].name[k] < 0x21 || (unsigned char)out[i].name[k] > 0x7e)
				raw++;
	CHK(n == 2 && raw == 0, "A: two rows from names with CR LF and raw bytes - %d byte(s) "
		"outside printable ASCII in %d row(s)", raw, n);

	/* B: the query log - a GET's key logged (in full, then hashed), AUTH's password never */
	CHK(strstr(qlog_line(2, "GET", "k1"), " key=k1"), "B: GET k1, keys in full - key=k1");
	CHK(strstr(qlog_line(1, "GET", "k1"), " key=#"), "B: GET k1, keys hashed - key=#...");
	qlog_line(2, "AUTH", "hunter2-secret");
	CHK(strstr(qbuf, "query:") && !strstr(qbuf, "hunter2") && !strstr(qbuf, "key="),
		"B: AUTH, keys in full - the line without the password (%.*s)",
		(int)strcspn(qbuf, "\n"), qbuf);
	qlog_line(1, "auth", "hunter2-secret");
	CHK(strstr(qbuf, "query:") && !strstr(qbuf, "key="),
		"B: auth, keys hashed - no hash of the password (%.*s)", (int)strcspn(qbuf, "\n"), qbuf);

	/* C: past five minute rolls the window's max still covers the minute in progress */
	compat_ticks_offset += 1000;            /* a tick of 0 reads as "no minute yet" */
	pc_obs_tick_1hz();                      /* the first tick starts the minute */
	pc_obs_cmd("winmax", 6, 200);
	for (i = 0; i < 6; i++) {
		compat_ticks_offset += 60;
		pc_obs_tick_1hz();
		pc_obs_cmd("winmax", 6, 200);
	}
	pc_obs_cmd("winmax", 6, 50000);         /* a stall, this minute */
	n = pc_obs_cmd_merge(out, 128);
	for (i = 0; i < n && !(out[i].nlen == 6 && !memcmp(out[i].name, "winmax", 6)); i++)
		;
	CHK(i < n && out[i].w_max == 50000, "C: six rolls in, a 50 ms call this minute is the "
		"window's max - %llu us", i < n ? out[i].w_max : 0ULL);

	/* D: eight workers, the first four rings full - the newest entry, on worker 6, comes first */
	if (pc_obs_init(8, 0) != 0) {
		printf("obs init failed\n");
		return 2;
	}
	{
		char *av[2] = { "GET", "old" }, jb[8192];
		size_t al[2] = { 3, 3 };
		struct pc_jw jw;

		for (wid = 0; wid < 4; wid++)
			for (i = 0; i < 128; i++)
				pc_obs_slow(av, al, 2, 10, NULL);
		wid = 6;
		av[1] = "newest";
		al[1] = 6;
		pc_obs_slow(av, al, 2, 10, NULL);
		wid = 0;
		pc_jw_init(&jw, jb, sizeof jb);
		pc_obs_slowlog_get(&jw, 1);
		jb[jw.len < sizeof jb ? jw.len : sizeof jb - 1] = 0;
		CHK(strstr(jb, "newest") != NULL, "D: SLOWLOG GET 1 is the newest entry, worker 6's "
			"behind four full rings - %.60s", jb);
	}

	printf("obstest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
