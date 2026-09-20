/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov
 * Part of libperfd - see lib/LICENSE.  This file must stay
 * free of src/core includes; tools/sync-libperfd.sh exports
 * exactly the MIT set to consumers. */
/*
 * json.c — the in-house JSON codec (task S7).  See json.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sodium.h>

#include "json.h"

/* ---- tokenizer --------------------------------------------------------- */

struct jp {
	const char *b;
	size_t len, pos;
	struct pc_jtok *t;
	int maxt, ntok, parent;
};

static int tok_new(struct jp *p, enum pc_jtype ty, int start)
{
	struct pc_jtok *t;

	if (p->ntok >= p->maxt)
		return -1;
	t = &p->t[p->ntok];
	t->type = ty;
	t->start = start;
	t->end = -1;
	t->size = 0;
	t->parent = p->parent;
	return p->ntok++;
}

static void count_in_parent(struct jp *p)
{
	if (p->parent >= 0)
		p->t[p->parent].size++;
}

static int parse_string(struct jp *p)
{
	size_t s = ++p->pos;               /* past the opening quote */

	while (p->pos < p->len) {
		unsigned char c = (unsigned char)p->b[p->pos];

		if (c == '"') {
			int i = tok_new(p, PC_J_STR, (int)s);

			if (i < 0)
				return -1;
			p->t[i].end = (int)p->pos;
			p->pos++;
			return i;
		}
		if (c == '\\') {
			p->pos++;
			if (p->pos >= p->len)
				return -1;
			if (p->b[p->pos] == 'u') {
				if (p->pos + 4 >= p->len)
					return -1;
				p->pos += 4;
			}
		} else if (c < 0x20) {
			return -1;                 /* raw control chars are invalid */
		}
		p->pos++;
	}
	return -1;
}

static int parse_prim(struct jp *p)
{
	size_t s = p->pos;
	int i;

	while (p->pos < p->len) {
		char c = p->b[p->pos];

		if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\t' ||
		        c == '\n' || c == '\r')
			break;
		p->pos++;
	}
	if (p->pos == s)
		return -1;
	i = tok_new(p, PC_J_PRIM, (int)s);
	if (i < 0)
		return -1;
	p->t[i].end = (int)p->pos;
	return i;
}

int pc_json_parse(const char *buf, size_t len, struct pc_jtok *t, int maxt)
{
	struct jp p = { buf, len, 0, t, maxt, 0, -1 };
	int i, expect_key = 0;

	while (p.pos < p.len) {
		char c = p.b[p.pos];

		switch (c) {
		case ' ': case '\t': case '\n': case '\r':
			p.pos++;
			break;
		case '{': case '[':
			count_in_parent(&p);
			i = tok_new(&p, c == '{' ? PC_J_OBJ : PC_J_ARR, (int)p.pos);
			if (i < 0)
				return -1;
			p.parent = i;
			expect_key = c == '{';
			p.pos++;
			break;
		case '}': case ']':
			if (p.parent < 0 ||
			        p.t[p.parent].type != (c == '}' ? PC_J_OBJ : PC_J_ARR))
				return -1;
			p.t[p.parent].end = (int)p.pos + 1;
			p.parent = p.t[p.parent].parent;
			expect_key = 0;
			p.pos++;
			break;
		case ':':
			expect_key = 0;
			p.pos++;
			break;
		case ',':
			if (p.parent >= 0 && p.t[p.parent].type == PC_J_OBJ)
				expect_key = 1;
			p.pos++;
			break;
		case '"':
			if (!expect_key)
				count_in_parent(&p);
			i = parse_string(&p);
			if (i < 0)
				return -1;
			if (expect_key)
				expect_key = 0;        /* the ':' resets properly */
			break;
		default:
			count_in_parent(&p);
			if (parse_prim(&p) < 0)
				return -1;
			break;
		}
	}
	if (p.parent != -1)
		return -1;                     /* unclosed container */
	return p.ntok;
}

int pc_json_get(const char *buf, const struct pc_jtok *t, int ntok,
		int obj, const char *key)
{
	size_t klen = strlen(key);
	int i, child = 0;

	if (obj < 0 || obj >= ntok || t[obj].type != PC_J_OBJ)
		return -1;
	for (i = obj + 1; i < ntok; i++) {
		if (t[i].parent != obj)
			continue;                  /* nested subtrees skipped by parent */
		/* direct children alternate key, value, key, value... - only
		 * even positions are keys (a VALUE whose text equals the key
		 * name must not match) */
		if ((child++ & 1) == 0 && t[i].type == PC_J_STR &&
		        (size_t)(t[i].end - t[i].start) == klen &&
		        !memcmp(buf + t[i].start, key, klen) && i + 1 < ntok)
			return i + 1;
	}
	return -1;
}

static int hex4(const char *s)
{
	int i, v = 0;

	for (i = 0; i < 4; i++) {
		char c = s[i];

		v <<= 4;
		if (c >= '0' && c <= '9')
			v |= c - '0';
		else if (c >= 'a' && c <= 'f')
			v |= c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')
			v |= c - 'A' + 10;
		else
			return -1;
	}
	return v;
}

int pc_json_unescape(const char *buf, const struct pc_jtok *tok,
		char *out, size_t outcap)
{
	const char *s = buf + tok->start, *e = buf + tok->end;
	size_t n = 0;
	int u;

	/* the decoded length never exceeds the source span (every escape
	 * shrinks or stays: \uXXXX is 6 source bytes -> at most 3), so the
	 * bound is checked per write against the ACTUAL bytes about to land,
	 * not a fixed worst-case headroom. */
#define PUT1(ch) do { if (n + 1 > outcap) return -1; out[n++] = (ch); } while (0)
	while (s < e) {
		if (*s != '\\') {
			PUT1(*s++);
			continue;
		}
		if (++s >= e)
			return -1;
		switch (*s) {
		case '"':  PUT1('"');  break;
		case '\\': PUT1('\\'); break;
		case '/':  PUT1('/');  break;
		case 'b':  PUT1('\b'); break;
		case 'f':  PUT1('\f'); break;
		case 'n':  PUT1('\n'); break;
		case 'r':  PUT1('\r'); break;
		case 't':  PUT1('\t'); break;
		case 'u':
			if (e - s < 5)
				return -1;
			u = hex4(s + 1);
			if (u < 0)
				return -1;
			s += 4;
			/* BMP only; a NUL here is legitimate (the whole point) */
			if (u < 0x80) {
				PUT1((char)u);
			} else if (u < 0x800) {
				if (n + 2 > outcap)
					return -1;
				out[n++] = (char)(0xC0 | (u >> 6));
				out[n++] = (char)(0x80 | (u & 0x3F));
			} else {
				if (n + 3 > outcap)
					return -1;
				out[n++] = (char)(0xE0 | (u >> 12));
				out[n++] = (char)(0x80 | ((u >> 6) & 0x3F));
				out[n++] = (char)(0x80 | (u & 0x3F));
			}
			break;
		default:
			return -1;
		}
		s++;
	}
#undef PUT1
	return (int)n;
}

int pc_json_streq(const char *buf, const struct pc_jtok *tok, const char *s)
{
	size_t n = strlen(s);

	return tok->type == PC_J_STR &&
		(size_t)(tok->end - tok->start) == n &&
		!memcmp(buf + tok->start, s, n);
}

/* ---- writer ------------------------------------------------------------ */

void pc_jw_init(struct pc_jw *w, char *buf, size_t cap)
{
	w->buf = buf;
	w->cap = cap;
	w->len = 0;
	w->overflow = 0;
	w->owned = 0;
}

int pc_jw_init_heap(struct pc_jw *w, size_t cap)
{
	char *b;

	if (w->len)                            /* would strand written bytes */
		return -1;
	if (cap < 4096)
		cap = 4096;
	if (cap > JW_HEAP_MAX)
		return -1;
	b = malloc(cap);
	if (!b)
		return -1;
	w->buf = b;
	w->cap = cap;
	w->len = 0;
	w->overflow = 0;
	w->owned = 1;
	return 0;
}

void pc_jw_free(struct pc_jw *w)
{
	if (w->owned)
		free(w->buf);
	w->buf = NULL;
	w->cap = w->len = 0;
	w->owned = 0;
}

void pc_jw_raw(struct pc_jw *w, const char *s, size_t n)
{
	if (w->len + n > w->cap) {
		size_t c;
		char *nb;

		if (!w->owned) {
			w->overflow = 1;
			return;
		}
		for (c = w->cap; c < w->len + n; c *= 2)
			;
		if (c > JW_HEAP_MAX) {         /* still bounded */
			w->overflow = 1;
			return;
		}
		nb = realloc(w->buf, c);
		if (!nb) {
			w->overflow = 1;
			return;
		}
		w->buf = nb;
		w->cap = c;
	}
	memcpy(w->buf + w->len, s, n);
	w->len += n;
}

void pc_jw_lit(struct pc_jw *w, const char *s)
{
	pc_jw_raw(w, s, strlen(s));
}

void pc_jw_str(struct pc_jw *w, const char *s, size_t n)
{
	static const char hx[] = "0123456789abcdef";
	size_t i;

	pc_jw_lit(w, "\"");
	for (i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];

		if (c == '"' || c == '\\') {
			char esc[2] = { '\\', (char)c };

			pc_jw_raw(w, esc, 2);
		} else if (c < 0x20) {
			char u[6] = { '\\', 'u', '0', '0', hx[c >> 4], hx[c & 15] };

			pc_jw_raw(w, u, 6);
		} else {
			pc_jw_raw(w, (const char *)&s[i], 1);
		}
	}
	pc_jw_lit(w, "\"");
}

void pc_jw_i64(struct pc_jw *w, long long v)
{
	char num[24];

	pc_jw_raw(w, num, (size_t)snprintf(num, sizeof num, "%lld", v));
}

int pc_utf8_clean(const char *s, size_t n)
{
	size_t i = 0;

	while (i < n) {
		unsigned char c = (unsigned char)s[i];

		if (c == 0)
			return 0;
		if (c < 0x80) {
			i++;
		} else if ((c & 0xE0) == 0xC0 && i + 1 < n &&
		        ((unsigned char)s[i + 1] & 0xC0) == 0x80 && c >= 0xC2) {
			i += 2;
		} else if ((c & 0xF0) == 0xE0 && i + 2 < n &&
		        ((unsigned char)s[i + 1] & 0xC0) == 0x80 &&
		        ((unsigned char)s[i + 2] & 0xC0) == 0x80) {
			i += 3;
		} else if ((c & 0xF8) == 0xF0 && i + 3 < n &&
		        ((unsigned char)s[i + 1] & 0xC0) == 0x80 &&
		        ((unsigned char)s[i + 2] & 0xC0) == 0x80 &&
		        ((unsigned char)s[i + 3] & 0xC0) == 0x80) {
			i += 4;
		} else {
			return 0;
		}
	}
	return 1;
}

void pc_jw_value(struct pc_jw *w, const char *key, const char *enc_key,
		const char *v, size_t n)
{
	pc_jw_lit(w, "\"");
	pc_jw_lit(w, key);
	pc_jw_lit(w, "\":");
	if (pc_utf8_clean(v, n)) {
		pc_jw_str(w, v, n);
		return;
	}
	/* binary payload: base64 + the sibling enc marker (decision #1) */
	{
		size_t need = sodium_base64_ENCODED_LEN(n,
			sodium_base64_VARIANT_ORIGINAL);

		if (w->len + need + 2 > w->cap) {
			w->overflow = 1;
			return;
		}
		w->buf[w->len++] = '"';
		sodium_bin2base64(w->buf + w->len, w->cap - w->len,
			(const unsigned char *)v, n, sodium_base64_VARIANT_ORIGINAL);
		w->len += strlen(w->buf + w->len);
		w->buf[w->len++] = '"';
	}
	pc_jw_lit(w, ",\"");
	pc_jw_lit(w, enc_key);
	pc_jw_lit(w, "\":\"b64\"");
}

int pc_b64_enc(const char *in, size_t n, char *out, size_t cap)
{
	if (sodium_base64_ENCODED_LEN(n, sodium_base64_VARIANT_ORIGINAL) > cap)
		return -1;
	sodium_bin2base64(out, cap, (const unsigned char *)in, n,
		sodium_base64_VARIANT_ORIGINAL);
	return (int)strlen(out);
}

int pc_b64_dec(const char *in, size_t n, char *out, size_t cap)
{
	size_t olen = 0;

	if (sodium_base642bin((unsigned char *)out, cap, in, n, NULL, &olen,
	        NULL, sodium_base64_VARIANT_ORIGINAL) != 0)
		return -1;
	return (int)olen;
}
