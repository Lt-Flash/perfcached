/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * respout.c - the RESP door's reply writers: an error (its code word), a
 * simple string, an integer, a bulk, the null bulk, an array's header, and
 * the two readers every command shares (a command word compared without
 * case, a whole decimal integer).  Pure, into a pc_jw; any thread.  Moved
 * from verbs.c unchanged (the 0.7.2 split, wave 1 - doc/MODULARITY.md);
 * test/respouttest.c.
 */
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "respout.h"

void resp_err(struct pc_jw *w, const char *msg)
{
	pc_jw_lit(w, "-ERR ");
	pc_jw_lit(w, msg);
	pc_jw_lit(w, "\r\n");
}

/* RESP carries the error CODE in the first word and clients dispatch on
 * it: a retryable condition must answer -TRYAGAIN, not -ERR.  resp_err()
 * hardcodes ERR, so backpressure needs its own emitter - otherwise every
 * client sees a generic error and none of them retries, which is the whole
 * point of saying "busy" instead of "forward failed".  S38. */
void resp_err_code(struct pc_jw *w, const char *code, const char *msg)
{
	pc_jw_lit(w, "-");
	pc_jw_lit(w, code);
	pc_jw_lit(w, " ");
	pc_jw_lit(w, msg);
	pc_jw_lit(w, "\r\n");
}

/* S280: an error line in Redis's own words, code and all - its WRONGTYPE,
 * and RedisJSON's wrong-type error, which has no code word */
void resp_err_line(struct pc_jw *w, const char *line)
{
	pc_jw_lit(w, "-");
	pc_jw_lit(w, line);
	pc_jw_lit(w, "\r\n");
}

void resp_simple(struct pc_jw *w, const char *s)
{
	pc_jw_lit(w, "+");
	pc_jw_lit(w, s);
	pc_jw_lit(w, "\r\n");
}

void resp_int(struct pc_jw *w, long long v)
{
	pc_jw_lit(w, ":");
	pc_jw_i64(w, v);
	pc_jw_lit(w, "\r\n");
}

void resp_nil(struct pc_jw *w)
{
	pc_jw_lit(w, "$-1\r\n");
}

void resp_bulk(struct pc_jw *w, const char *p, size_t n)
{
	pc_jw_lit(w, "$");
	pc_jw_i64(w, (long long)n);
	pc_jw_lit(w, "\r\n");
	pc_jw_raw(w, p, n);
	pc_jw_lit(w, "\r\n");
}

void resp_arr(struct pc_jw *w, int n)
{
	pc_jw_lit(w, "*");
	pc_jw_i64(w, n);
	pc_jw_lit(w, "\r\n");
}

/* case-insensitive command word compare (Redis commands are ci) */
int resp_is(const char *a, size_t al, const char *b)
{
	size_t i;

	for (i = 0; i < al; i++) {
		char ca = a[i], cb = b[i];

		if (!cb)
			return 0;
		if (ca >= 'a' && ca <= 'z')
			ca -= 32;
		if (cb >= 'a' && cb <= 'z')
			cb -= 32;
		if (ca != cb)
			return 0;
	}
	return b[al] == 0;
}

int resp_ll(const char *p, size_t n, long long *out)
{
	char num[24];

	if (!n || n >= sizeof num)
		return -1;
	memcpy(num, p, n);
	num[n] = 0;
	{
		char *end;
		long long v = strtoll(num, &end, 10);

		if (*end)
			return -1;
		*out = v;
	}
	return 0;
}
