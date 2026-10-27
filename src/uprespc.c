/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * uprespc.c - the upstream's RESP reply parser: one RESP2 or RESP3 reply
 * from a buffer.  No connection, no thread state but the per-thread size
 * ceiling, no logging - upstream.c reads the bytes from its connections and
 * acts on what this parses.  Moved from upstream.c unchanged (the 0.7.2
 * split, wave 1 - doc/MODULARITY.md); test/uprespctest.c.
 */
#include <stdlib.h>
#include <string.h>

#include "uprespc.h"

/* S385: the largest bulk - and, in read_reply, the largest reply - this
 * thread reads: the fetch threads' record ceiling, the writer's and the
 * pass reader's upstream_pass_max_kb */
__thread size_t rv_max = UP_RBUF_MAX;

void rv_free(struct rv *v)
{
	int i;

	if (!v)
		return;
	for (i = 0; i < v->nel; i++)
		rv_free(&v->el[i]);
	free(v->el);
	v->el = NULL;
	v->nel = 0;
}

/* parse one reply at buf[*pos..len): 1 complete (*pos advanced), 0 need
 * more bytes, -1 malformed.  Strings point into buf (valid until the
 * next read compacts it). */
int rv_parse(char *buf, size_t len, size_t *pos, struct rv *v, int depth)
{
	size_t p = *pos, e;
	char *nl;
	long long n;
	char *end;
	int i, cnt;

	memset(v, 0, sizeof *v);
	if (depth > 8)
		return -1;
	if (p >= len)
		return 0;
	nl = memchr(buf + p, '\n', len - p);
	if (!nl)
		return 0;
	e = (size_t)(nl - buf);            /* index of '\n' */
	if (e == p || buf[e - 1] != '\r')
		return -1;
	v->t = buf[p];
	switch (v->t) {
	case '+': case '-': case '(': case ',': case '#':
		v->s = buf + p + 1;
		v->len = e - 1 - (p + 1);
		*pos = e + 1;
		return 1;
	case ':':
		buf[e - 1] = 0;
		v->n = strtoll(buf + p + 1, &end, 10);
		buf[e - 1] = '\r';
		*pos = e + 1;
		return 1;
	case '_':
		*pos = e + 1;
		return 1;
	case '$': case '=': case '!':
		buf[e - 1] = 0;
		n = strtoll(buf + p + 1, &end, 10);
		buf[e - 1] = '\r';
		if (n < 0) {                   /* RESP2 null bulk */
			v->t = '_';
			*pos = e + 1;
			return 1;
		}
		if ((size_t)n > rv_max)
			return -3;             /* too large to hold: the caller
			                        * marks the key and drops the
			                        * connection (the stream is mid
			                        * value) */
		if (len - (e + 1) < (size_t)n + 2)
			return 0;
		v->s = buf + e + 1;
		v->len = (size_t)n;
		if (v->t == '=' && v->len >= 4) {   /* verbatim: "txt:" prefix */
			v->s += 4;
			v->len -= 4;
		}
		*pos = e + 1 + (size_t)n + 2;
		return 1;
	case '*': case '%': case '~': case '>': case '|':
		buf[e - 1] = 0;
		n = strtoll(buf + p + 1, &end, 10);
		buf[e - 1] = '\r';
		p = e + 1;
		if (n < 0) {
			v->t = '_';
			*pos = p;
			return 1;
		}
		if (n > 1000000)
			return -1;
		cnt = (v->t == '%' || v->t == '|') ? (int)n * 2 : (int)n;
		v->n = n;
		if (cnt) {
			v->el = calloc((size_t)cnt, sizeof *v->el);
			if (!v->el)
				return -1;
		}
		v->nel = cnt;
		for (i = 0; i < cnt; i++) {
			int rc = rv_parse(buf, len, &p, &v->el[i], depth + 1);

			if (rc <= 0 || rc == -3) {
				v->nel = i;
				rv_free(v);
				return rc;
			}
		}
		if (v->t == '|') {             /* an attribute: skip, read the
		                                * value it annotates */
			rv_free(v);
			*pos = p;
			return rv_parse(buf, len, pos, v, depth + 1);
		}
		*pos = p;
		return 1;
	default:
		return -1;
	}
}

int is_ok(const struct rv *v)
{
	return v->t == '+' && v->len == 2 && !memcmp(v->s, "OK", 2);
}
