/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* respout.c - the RESP door's reply writers and the two readers every command shares
 * (a command word compared without case, a whole decimal integer).  Pure; any thread. */
#include <errno.h>
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

/* Clients dispatch on the error CODE word: a retryable condition must answer
 * -TRYAGAIN, not -ERR, or no client retries. */
void resp_err_code(struct pc_jw *w, const char *code, const char *msg)
{
	pc_jw_lit(w, "-");
	pc_jw_lit(w, code);
	pc_jw_lit(w, " ");
	pc_jw_lit(w, msg);
	pc_jw_lit(w, "\r\n");
}

/* an error line in Redis's own words, code and all - its WRONGTYPE,
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

/* digits with an optional leading '-', all @n bytes, inside int64 - as Redis's string2ll */
int resp_ll(const char *p, size_t n, long long *out)
{
	char num[24], *end;
	long long v;

	if (!n || n >= sizeof num || (p[0] != '-' && (p[0] < '0' || p[0] > '9')))
		return -1;
	memcpy(num, p, n);
	num[n] = 0;
	errno = 0;
	v = strtoll(num, &end, 10);
	if (end != num + n || errno == ERANGE)
		return -1;
	*out = v;
	return 0;
}
