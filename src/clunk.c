/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clunk.c - unknown commands: the bounded table, its frame and the fleet
 * fold (S165).  See clunk.h.  No lock, no clock, no log: the caller owns
 * the table and says what time it is.
 */
#include <stdlib.h>
#include <string.h>

#include "clunk.h"
#include "clcodec.h"

static size_t put_part(char *out, size_t at, int upper, const char *s,
		size_t n)
{
	size_t i;

	for (i = 0; i < n && at < CLUNK_NAME - 1; i++, at++) {
		unsigned char ch = (unsigned char)s[i];

		if (upper && ch >= 'a' && ch <= 'z')
			ch = (unsigned char)(ch - 'a' + 'A');
		out[at] = (ch > 0x20 && ch < 0x7f) ? (char)ch : '?';
	}
	return at;
}

size_t clunk_name(char *out, int upper, const char *cmd, size_t clen,
		const char *sub, size_t slen)
{
	size_t at = put_part(out, 0, upper, cmd, clen);

	if (sub && at < CLUNK_NAME - 1) {
		out[at++] = ' ';
		at = put_part(out, at, upper, sub, slen);
	}
	out[at] = 0;
	return at;
}

/* copy a NUL-terminated string that is already ours (an address, a
 * client name) with the same printable rule, so nothing unprintable
 * reaches a log line or the page through the side fields either */
static void put_str(char *out, size_t cap, const char *s)
{
	size_t i = 0;

	if (s)
		for (; s[i] && i < cap - 1; i++) {
			unsigned char ch = (unsigned char)s[i];

			out[i] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '?';
		}
	out[i] = 0;
}

void clunk_clean(char *s)
{
	put_str(s, strlen(s) + 1, s);
}

static struct clunk_row *find(struct clunk_table *t, int dialect,
		const char *name, size_t nlen)
{
	int i;

	for (i = 0; i < t->n; i++) {
		struct clunk_row *r = &t->rows[i];

		if (r->dialect == dialect && r->nlen == nlen &&
		    memcmp(r->name, name, nlen) == 0)
			return r;
	}
	return NULL;
}

int clunk_note(struct clunk_table *t, int dialect, const char *name,
		size_t nlen, uint64_t now_s, const char *addr,
		const char *client)
{
	struct clunk_row *r;

	if (nlen > CLUNK_NAME - 1)
		nlen = CLUNK_NAME - 1;
	r = find(t, dialect, name, nlen);
	if (!r) {
		if (t->n >= CLUNK_ROWS) {
			t->other++;
			return -1;
		}
		/* filled BEFORE it is counted in: a worker's table is folded
		 * by other threads while its owner writes it (obs.h's racy-read
		 * rule), and a reader must never meet a half-built row */
		r = &t->rows[t->n];
		memset(r, 0, sizeof *r);
		r->dialect = (unsigned char)dialect;
		r->nlen = (unsigned char)nlen;
		memcpy(r->name, name, nlen);
		r->name[nlen] = 0;
		r->first_s = now_s;
		r->last_s = now_s;
		r->count = 1;
		put_str(r->addr, sizeof r->addr, addr);
		put_str(r->client, sizeof r->client, client);
		__atomic_store_n(&t->n, t->n + 1, __ATOMIC_RELEASE);
		return 1;
	}
	r->count++;
	r->last_s = now_s;
	put_str(r->addr, sizeof r->addr, addr);
	put_str(r->client, sizeof r->client, client);
	return 0;
}

void clunk_fold(struct clunk_table *dst, const struct clunk_table *src,
		int node)
{
	int i;

	int n = __atomic_load_n(&src->n, __ATOMIC_ACQUIRE);

	dst->other += src->other;
	dst->preauth += src->preauth;
	for (i = 0; i < n && i < CLUNK_ROWS; i++) {
		const struct clunk_row *s = &src->rows[i];
		struct clunk_row *d;

		if (!s->nlen || s->nlen > CLUNK_NAME - 1)
			continue;
		d = find(dst, s->dialect, s->name, s->nlen);

		if (!d) {
			if (dst->n >= CLUNK_ROWS) {
				dst->other += s->count;
				continue;
			}
			d = &dst->rows[dst->n++];
			*d = *s;
			d->here = node ? 0 : s->count;
			d->node = node;
			continue;
		}
		d->count += s->count;
		if (!node)
			d->here += s->count;
		if (s->first_s < d->first_s)
			d->first_s = s->first_s;
		if (s->last_s > d->last_s) {
			d->last_s = s->last_s;
			memcpy(d->addr, s->addr, sizeof d->addr);
			memcpy(d->client, s->client, sizeof d->client);
			d->node = node;
		}
	}
}

static int row_cmp(const void *a, const void *b)
{
	const struct clunk_row *x = a, *y = b;
	int c;

	if (x->last_s != y->last_s)
		return x->last_s < y->last_s ? 1 : -1;
	if (x->count != y->count)
		return x->count < y->count ? 1 : -1;
	c = strcmp(x->name, y->name);
	return c ? c : (int)x->dialect - (int)y->dialect;
}

void clunk_sort(struct clunk_table *t)
{
	qsort(t->rows, (size_t)t->n, sizeof t->rows[0], row_cmp);
}

size_t clunk_write(const struct clunk_table *t, unsigned node,
		unsigned char *buf, size_t cap)
{
	struct pc_wcur w;
	int i, n = t->n < CLUNK_ROWS ? t->n : CLUNK_ROWS;

	pc_wcur_init(&w, buf, cap);
	pc_w16(&w, (uint16_t)node);
	pc_w64(&w, t->other);
	pc_w64(&w, t->preauth);
	pc_w8(&w, (unsigned)n);
	for (i = 0; i < n; i++) {
		const struct clunk_row *r = &t->rows[i];
		size_t al = strnlen(r->addr, CLUNK_ADDR - 1);
		size_t cl = strnlen(r->client, CLUNK_CLIENT - 1);
		size_t nl = r->nlen < CLUNK_NAME ? r->nlen : CLUNK_NAME - 1;

		pc_w8(&w, r->dialect);
		pc_w8(&w, (unsigned)nl);
		pc_wraw(&w, r->name, nl);
		pc_w64(&w, r->count);
		pc_w64(&w, r->first_s);
		pc_w64(&w, r->last_s);
		pc_w8(&w, (unsigned)al);
		pc_wraw(&w, r->addr, al);
		pc_w8(&w, (unsigned)cl);
		pc_wraw(&w, r->client, cl);
	}
	return w.ok ? pc_wcur_len(&w, buf) : 0;
}

int clunk_parse(const unsigned char *b, size_t n, unsigned *node,
		struct clunk_table *t)
{
	struct pc_rcur r;
	struct clunk_table tmp;
	int i, rows;

	pc_rcur_init(&r, b, n);
	if (!pc_rfits(&r, 19))
		return -1;
	memset(&tmp, 0, sizeof tmp);
	*node = (unsigned)pc_ru16(&r);
	tmp.other = pc_ru64(&r);
	tmp.preauth = pc_ru64(&r);
	rows = pc_ru8(&r);
	if (rows > CLUNK_ROWS)
		return -1;
	for (i = 0; i < rows; i++) {
		struct clunk_row *x = &tmp.rows[i];
		int d, nl, al, cl;

		if (!pc_r8(&r, &d) || !pc_r8(&r, &nl) ||
		    d >= CLUNK_DIALECTS || nl == 0 || nl > CLUNK_NAME - 1 ||
		    !pc_rraw(&r, x->name, (size_t)nl) || !pc_rfits(&r, 24))
			return -1;
		x->dialect = (unsigned char)d;
		x->nlen = (unsigned char)nl;
		x->count = pc_ru64(&r);
		x->first_s = pc_ru64(&r);
		x->last_s = pc_ru64(&r);
		if (!pc_r8(&r, &al) || al > CLUNK_ADDR - 1 ||
		    !pc_rraw(&r, x->addr, (size_t)al) ||
		    !pc_r8(&r, &cl) || cl > CLUNK_CLIENT - 1 ||
		    !pc_rraw(&r, x->client, (size_t)cl))
			return -1;
		/* a peer is not trusted to have sanitised: the page and the
		 * journal get printable ASCII whatever arrived (the name's one
		 * space, between command and subcommand, is printable) */
		x->name[nl] = 0;
		put_str(x->name, sizeof x->name, x->name);
		x->nlen = (unsigned char)strlen(x->name);   /* an embedded NUL ends it */
		if (!x->nlen)
			return -1;
		put_str(x->addr, sizeof x->addr, x->addr);
		put_str(x->client, sizeof x->client, x->client);
	}
	tmp.n = rows;
	*t = tmp;
	return 0;
}

const char *clunk_dialect_name(int dialect)
{
	switch (dialect) {
	case CLUNK_RESP: return "resp";
	case CLUNK_JSON: return "json";
	case CLUNK_BIN:  return "binary";
	default:         return "?";
	}
}
