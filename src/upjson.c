/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* upjson.c - two JSON documents compared as values, for the upfill sweep: members by name in any
 * order, elements in order, strings unescaped, numbers by value */
#include <stdlib.h>
#include <string.h>
#include "json.h"
#include "upjson.h"

#define UPJ_DEPTH    64                /* deeper: compared as text */

/* an integer literal: digits, perhaps a minus - compared by its digits,
 * since a double holds only 2^53 of them exactly */
static int jint(const char *b, const struct pc_jtok *t)
{
	int i = t->start;

	if (i < t->end && b[i] == '-')
		i++;
	if (i >= t->end)
		return 0;
	for (; i < t->end; i++)
		if (b[i] < '0' || b[i] > '9')
			return 0;
	return 1;
}

/* a number's value, from its token's text; 0 not a finite number */
static int jnum(const char *b, const struct pc_jtok *t, double *d)
{
	char tmp[64], *e;
	int n = t->end - t->start;

	if (n <= 0 || n >= (int)sizeof tmp)
		return 0;
	memcpy(tmp, b + t->start, (size_t)n);
	tmp[n] = 0;
	*d = strtod(tmp, &e);
	return *e == 0 && *d == *d && *d - *d == 0;
}

/* the token after @i's whole value */
static int jnext(const struct pc_jtok *t, int n, int i)
{
	int j = i + 1;

	while (j < n && t[j].start < t[i].end)
		j++;
	return j;
}

/* a string token, unescaped; -1 a bad escape */
static int jstr(const char *b, const struct pc_jtok *t, char **out)
{
	size_t cap = (size_t)(t->end - t->start) + 1;
	int n;

	*out = malloc(cap);
	if (!*out)
		return -1;
	n = pc_json_unescape(b, t, *out, cap);
	if (n < 0) {
		free(*out);
		*out = NULL;
	}
	return n;
}

/* whitespace JSON allows between tokens */
static int jws(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* the bytes the text comparison stops at: outside a string, whitespace and
 * the quote; inside one, the quote and the backslash */
static const unsigned char jsp_out[256] = { [' '] = 1, ['\t'] = 1, ['\n'] = 1,
	['\r'] = 1, ['"'] = 1 };
static const unsigned char jsp_in[256] = { ['"'] = 1, ['\\'] = 1 };

/* the same document spelt with other spacing, settled without parsing: byte for byte, whitespace
 * outside strings skipped.  1 the same text, 0 not decided here (the walk decides) */
static int json_same_text(const char *a, size_t al, const char *b, size_t bl)
{
	size_t i = 0, j = 0;
	int ins = 0;

	for (;;) {
		if (ins) {
			while (i < al && j < bl && a[i] == b[j] &&
			       !jsp_in[(unsigned char)a[i]]) {
				i++;
				j++;
			}
		} else {
			for (;;) {
				while (i < al && j < bl && a[i] == b[j] &&
				       !jsp_out[(unsigned char)a[i]]) {
					i++;
					j++;
				}
				if (i < al && jws(a[i]))
					i++;
				else if (j < bl && jws(b[j]))
					j++;
				else
					break;
			}
		}
		if (i == al || j == bl)
			return i == al && j == bl;
		if (a[i] != b[j])
			return 0;
		/* equal and special: outside, the quote; inside, the quote or
		 * an escape, whose next byte is taken as it is */
		if (!ins)
			ins = 1;
		else if (a[i] == '\\') {
			i++;
			j++;
			if (i == al || j == bl || a[i] != b[j])
				return 0;
		} else
			ins = 0;
		i++;
		j++;
	}
}

/* two string tokens: raw spellings first, unescaped only when a backslash lets two spellings be
 * one value.  1 equal, 0 not, -1 not decided (out of memory, a bad escape) */
static int jstreq(const char *ab, const struct pc_jtok *x, const char *bb,
		const struct pc_jtok *y)
{
	int lx = x->end - x->start, ly = y->end - y->start, r;
	char *sx, *sy;

	if (lx == ly && !memcmp(ab + x->start, bb + y->start, (size_t)lx))
		return 1;
	if (!memchr(ab + x->start, '\\', (size_t)lx) &&
	    !memchr(bb + y->start, '\\', (size_t)ly))
		return 0;
	if ((lx = jstr(ab, x, &sx)) < 0)
		return -1;
	if ((ly = jstr(bb, y, &sy)) < 0) {
		free(sx);
		return -1;
	}
	r = lx == ly && !memcmp(sx, sy, (size_t)lx);
	free(sx);
	free(sy);
	return r;
}

/* an object's members by raw name, for the lookup when the order differs */
struct jkey {
	const char *s;
	int n;
	int v;                             /* its value's token */
};

static int jkey_cmp(const void *p, const void *q)
{
	const struct jkey *a = p, *b = q;
	int c = memcmp(a->s, b->s, (size_t)(a->n < b->n ? a->n : b->n));

	return c ? c : a->n - b->n;
}

/* the value token of y's (b[ib]) member named as a[ka]: -1 none, -2 not decided.  *@ix is built at
 * the first call (sorted raw names; NULL with *@esc set when a name holds a backslash: a scan) */
static int jfind(const char *ab, const struct pc_jtok *a, int ka,
		const char *bb, const struct pc_jtok *b, int nb, int ib,
		struct jkey **ix, int *nix, int *esc)
{
	const struct pc_jtok *y = &b[ib];
	struct jkey key, *hit;
	int j, r;

	if (!*ix && !*esc) {
		*ix = malloc((size_t)(y->size ? y->size : 1) * sizeof **ix);
		if (!*ix)
			return -2;
		for (j = ib + 1, *nix = 0; j < nb && b[j].start < y->end &&
		     *nix < y->size; j = jnext(b, nb, j + 1)) {
			if (j + 1 >= nb || b[j + 1].start >= y->end) {
				free(*ix);             /* a key with no value: not a document */
				*ix = NULL;
				return -2;
			}
			(*ix)[*nix].s = bb + b[j].start;
			(*ix)[*nix].n = b[j].end - b[j].start;
			(*ix)[*nix].v = j + 1;
			if (memchr((*ix)[*nix].s, '\\', (size_t)(*ix)[*nix].n))
				*esc = 1;
			(*nix)++;
		}
		if (*esc) {
			free(*ix);
			*ix = NULL;
		} else
			qsort(*ix, (size_t)*nix, sizeof **ix, jkey_cmp);
	}
	key.s = ab + a[ka].start;
	key.n = a[ka].end - a[ka].start;
	if (*ix && !memchr(key.s, '\\', (size_t)key.n)) {
		hit = bsearch(&key, *ix, (size_t)*nix, sizeof **ix, jkey_cmp);
		return hit ? hit->v : -1;
	}
	for (j = ib + 1; j < nb && b[j].start < y->end; j = jnext(b, nb, j + 1))
		if ((r = jstreq(ab, &a[ka], bb, &b[j])) != 0)
			return r < 0 || j + 1 >= nb || b[j + 1].start >= y->end ? -2 : j + 1;
	return -1;
}

/* are a[ia] and b[ib] the same JSON value (members by name in any order, elements in order,
 * strings unescaped, numbers by value)?  1 yes, 0 no, -1 not decided (too deep, no memory) */
static int jeq(const char *ab, const struct pc_jtok *a, int na, int ia,
		const char *bb, const struct pc_jtok *b, int nb, int ib, int depth)
{
	const struct pc_jtok *x = &a[ia], *y = &b[ib];
	struct jkey *ix = NULL;
	int i, j, r, nix = 0, esc = 0;

	if (depth > UPJ_DEPTH)
		return -1;
	if (x->type != y->type || x->size != y->size)
		return 0;
	if (x->type == PC_J_PRIM) {
		double dx, dy;
		int lx = x->end - x->start, ly = y->end - y->start;

		if (lx == ly && !memcmp(ab + x->start, bb + y->start, (size_t)lx))
			return 1;
		if (jint(ab, x) && jint(bb, y))        /* "-0" and "0" aside */
			return lx + ly == 3 && (ab[x->start] == '-' ?
				lx == 2 && !memcmp(ab + x->start, "-0", 2) && bb[y->start] == '0' :
				ly == 2 && !memcmp(bb + y->start, "-0", 2) && ab[x->start] == '0');
		return jnum(ab, x, &dx) && jnum(bb, y, &dy) && dx == dy;
	}
	if (x->type == PC_J_STR)
		return jstreq(ab, x, bb, y);
	if (x->type == PC_J_ARR) {
		for (i = ia + 1, j = ib + 1; i < na && j < nb && a[i].start < x->end;
		     i = jnext(a, na, i), j = jnext(b, nb, j))
			if ((r = jeq(ab, a, na, i, bb, b, nb, j, depth + 1)) != 1)
				return r;
		return 1;
	}
	/* an object: each of x's members in y by name - in the same place first (RedisJSON keeps the
	 * written order), looked up when it is not there */
	r = 1;
	for (i = ia + 1, j = ib + 1; r == 1 && i < na && a[i].start < x->end;
	     i = jnext(a, na, i + 1)) {
		int v = -1;

		if (i + 1 >= na || a[i + 1].start >= x->end) {
			r = -1;                    /* a key with no value: not decided */
			break;
		}
		if (j < nb && b[j].start < y->end) {
			if ((r = jstreq(ab, &a[i], bb, &b[j])) < 0)
				break;
			if (r && (j + 1 >= nb || b[j + 1].start >= y->end)) {
				r = -1;
				break;
			}
			if (r)
				v = j + 1;
			j = jnext(b, nb, j + 1);
		}
		if (v < 0)
			v = jfind(ab, a, i, bb, b, nb, ib, &ix, &nix, &esc);
		r = v >= 0 ? jeq(ab, a, na, i + 1, bb, b, nb, v, depth + 1) :
			v == -1 ? 0 : -1;
	}
	free(ix);
	return r;
}

/* the walk's tokens: one buffer a thread, reused (n bytes hold at most n / 2 + 1 tokens); one
 * larger than UPJ_TOK_KEEP is freed after use, so a rare big document pins nothing */
#define UPJ_TOK_KEEP 65536
static __thread struct pc_jtok *jtok;
static __thread size_t jtok_cap;

void up_json_release(void)
{
	free(jtok);
	jtok = NULL;
	jtok_cap = 0;
}

/* two documents: 1 the same JSON value, 0 not, -1 not decided (too deep, refused by the tokenizer,
 * no memory) - counted uncompared, never different.  *@walked: the text alone did not settle it */
int up_json_same(const char *a, size_t al, const char *b, size_t bl,
		int *walked)
{
	size_t ca = al / 2 + 2, cb = bl / 2 + 2;
	int na = -1, nb = -1, r = -1;

	*walked = 0;
	if (json_same_text(a, al, b, bl))
		return 1;
	*walked = 1;
	if (ca + cb > jtok_cap) {
		struct pc_jtok *t = realloc(jtok, (ca + cb) * sizeof *t);

		if (t) {
			jtok = t;
			jtok_cap = ca + cb;
		}
	}
	if (ca + cb <= jtok_cap) {
		na = pc_json_parse(a, al, jtok, (int)ca);
		nb = pc_json_parse(b, bl, jtok + ca, (int)cb);
	}
	if (na > 0 && nb > 0)
		r = jeq(a, jtok, na, 0, b, jtok + ca, nb, 0, 0);
	if (jtok_cap > UPJ_TOK_KEEP)
		up_json_release();
	return r;
}
