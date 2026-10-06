/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov
 * Part of libperfd - see lib/LICENSE.  This file must stay
 * free of src/core includes; tools/sync-libperfd.sh exports
 * exactly the MIT set to consumers. */
/*
 * ptree.c - the native door's value codec (task S317).  See ptree.h.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ptree.h"

/* ---- writer ------------------------------------------------------------ */

void pc_tw_init(struct pc_tw *w, unsigned char *buf, size_t cap)
{
	memset(w, 0, sizeof *w);
	w->b = buf;
	w->cap = buf ? cap : 0;
	w->owned = buf == NULL;
}

void pc_tw_free(struct pc_tw *w)
{
	if (w->owned)
		free(w->b);
	w->b = NULL;
	w->n = w->cap = 0;
}

static int room(struct pc_tw *w, size_t k)
{
	size_t nc;
	unsigned char *nb;

	if (w->over)
		return 0;
	if (w->cap - w->n >= k)
		return 1;
	if (!w->owned || w->n + k > PT_HEAP_MAX) {
		w->over = 1;
		return 0;
	}
	nc = w->cap ? w->cap : 512;
	while (nc - w->n < k)
		nc *= 2;
	if (nc > PT_HEAP_MAX)
		nc = PT_HEAP_MAX;
	nb = realloc(w->b, nc);
	if (!nb) {
		w->over = 1;
		return 0;
	}
	w->b = nb;
	w->cap = nc;
	return 1;
}

static void put32(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16);
	p[3] = (unsigned char)(v >> 24);
}

static uint32_t get32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
		(uint32_t)p[3] << 24;
}

/* one more item in the open container; a map counts its keys, not the
 * values after them */
static void counted(struct pc_tw *w, int is_key)
{
	if (w->depth && w->st[w->depth - 1].map == is_key)
		w->st[w->depth - 1].cnt++;
}

static void open_c(struct pc_tw *w, char tag, int map)
{
	if (w->depth >= PT_DEPTH) {
		w->over = 1;
		return;
	}
	if (!room(w, 5))
		return;
	counted(w, 0);
	w->b[w->n] = (unsigned char)tag;
	w->st[w->depth].at = w->n + 1;
	w->st[w->depth].cnt = 0;
	w->st[w->depth].map = map;
	w->depth++;
	w->n += 5;
}

void pc_tw_map(struct pc_tw *w)
{
	open_c(w, 'm', 1);
}

void pc_tw_arr(struct pc_tw *w)
{
	open_c(w, 'a', 0);
}

void pc_tw_end(struct pc_tw *w)
{
	if (w->over || !w->depth) {
		w->over = 1;
		return;
	}
	w->depth--;
	put32(w->b + w->st[w->depth].at, w->st[w->depth].cnt);
}

static void lenitem(struct pc_tw *w, char tag, const void *p, size_t n)
{
	if (n > 0xffffffffu || !room(w, 5 + n)) {
		w->over = 1;
		return;
	}
	w->b[w->n] = (unsigned char)tag;
	put32(w->b + w->n + 1, (uint32_t)n);
	if (n)
		memcpy(w->b + w->n + 5, p, n);
	w->n += 5 + n;
}

void pc_tw_keyn(struct pc_tw *w, const void *k, size_t n)
{
	if (!w->depth || !w->st[w->depth - 1].map) {
		w->over = 1;
		return;
	}
	counted(w, 1);
	lenitem(w, 'b', k, n);
}

void pc_tw_key(struct pc_tw *w, const char *k)
{
	pc_tw_keyn(w, k, strlen(k));
}

void pc_tw_i64(struct pc_tw *w, long long v)
{
	unsigned long long u = (unsigned long long)v;
	int k;

	if (!room(w, 9))
		return;
	counted(w, 0);
	w->b[w->n] = 'i';
	for (k = 0; k < 8; k++)
		w->b[w->n + 1 + k] = (unsigned char)(u >> (8 * k));
	w->n += 9;
}

void pc_tw_num(struct pc_tw *w, const char *text, size_t n)
{
	if (n > 0xffff || !room(w, 3 + n)) {
		w->over = 1;
		return;
	}
	counted(w, 0);
	w->b[w->n] = 'd';
	w->b[w->n + 1] = (unsigned char)n;
	w->b[w->n + 2] = (unsigned char)(n >> 8);
	memcpy(w->b + w->n + 3, text, n);
	w->n += 3 + n;
}

void pc_tw_bulk(struct pc_tw *w, const void *p, size_t n)
{
	counted(w, 0);
	lenitem(w, 'b', p, n);
}

void pc_tw_str(struct pc_tw *w, const char *s)
{
	pc_tw_bulk(w, s, strlen(s));
}

static void tag1(struct pc_tw *w, char t)
{
	if (!room(w, 1))
		return;
	counted(w, 0);
	w->b[w->n++] = (unsigned char)t;
}

void pc_tw_bool(struct pc_tw *w, int v)
{
	tag1(w, v ? 't' : 'f');
}

void pc_tw_nil(struct pc_tw *w)
{
	tag1(w, 'n');
}

void pc_tw_ok(struct pc_tw *w)
{
	tag1(w, 'o');
}

void pc_tw_err(struct pc_tw *w, const char *line)
{
	counted(w, 0);
	lenitem(w, 'e', line, strlen(line));
}

void pc_tw_raw(struct pc_tw *w, const void *item, size_t n)
{
	if (!room(w, n))
		return;
	counted(w, 0);
	memcpy(w->b + w->n, item, n);
	w->n += n;
}

struct pc_tw_mark pc_tw_mark(const struct pc_tw *w)
{
	struct pc_tw_mark m;

	m.n = w->n;
	m.depth = w->depth;
	m.cnt = w->depth ? w->st[w->depth - 1].cnt : 0;
	return m;
}

void pc_tw_rollback(struct pc_tw *w, struct pc_tw_mark m)
{
	if (m.n > w->n || m.depth > w->depth)
		return;
	w->n = m.n;
	w->depth = m.depth;
	if (w->depth)
		w->st[w->depth - 1].cnt = m.cnt;
	/* an overflow past the mark is undone with what it overflowed */
	w->over = 0;
}

int pc_tw_done(const struct pc_tw *w)
{
	return w->over || w->depth ? -1 : 0;
}

/* ---- reader ------------------------------------------------------------ */

static int node_add(struct pc_tv *v)
{
	if (v->nn == v->cap) {
		int nc = v->cap ? v->cap * 2 : 32;
		struct pc_tn *nn = realloc(v->n, sizeof *nn * (size_t)nc);

		if (!nn) {
			v->err = "out of memory";
			return -1;
		}
		v->n = nn;
		v->cap = nc;
	}
	memset(&v->n[v->nn], 0, sizeof v->n[0]);
	return v->nn++;
}

/* index the item at *off; -1 with v->err */
static int item(struct pc_tv *v, size_t *off, int depth)
{
	const unsigned char *b = v->src;
	size_t n = v->srclen, o = *off;
	int me, k;
	uint32_t i, cnt;

	if (depth > PT_DEPTH) {
		v->err = "too deep";
		return -1;
	}
	if (o >= n) {
		v->err = "truncated";
		return -1;
	}
	me = node_add(v);
	if (me < 0)
		return -1;
	v->n[me].type = (char)b[o++];
	switch (v->n[me].type) {
	case 'n': case 't': case 'f': case 'o':
		break;
	case 'i': {
		unsigned long long u = 0;

		if (n - o < 8)
			goto trunc;
		for (k = 7; k >= 0; k--)
			u = u << 8 | b[o + (size_t)k];
		v->n[me].i = (long long)u;
		o += 8;
		break;
	}
	case 'd':
		if (n - o < 2)
			goto trunc;
		v->n[me].len = (uint32_t)b[o] | (uint32_t)b[o + 1] << 8;
		o += 2;
		if (n - o < v->n[me].len)
			goto trunc;
		v->n[me].p = b + o;
		o += v->n[me].len;
		break;
	case 'b': case 'e':
		if (n - o < 4)
			goto trunc;
		v->n[me].len = get32(b + o);
		o += 4;
		if (n - o < v->n[me].len)
			goto trunc;
		v->n[me].p = b + o;
		o += v->n[me].len;
		break;
	case 'a': case 'm':
		if (n - o < 4)
			goto trunc;
		cnt = get32(b + o);
		o += 4;
		v->n[me].len = cnt;
		/* each item is at least one byte, a pair at least six: a
		 * count that lies runs out of bytes, never out of time */
		for (i = 0; i < cnt; i++) {
			if (v->n[me].type == 'm') {
				if (o >= n || b[o] != 'b') {
					v->err = o >= n ? "truncated" : "map key not bytes";
					return -1;
				}
				if (item(v, &o, depth + 1) < 0)
					return -1;
			}
			if (item(v, &o, depth + 1) < 0)
				return -1;
		}
		break;
	default:
		v->err = "bad tag";
		return -1;
	}
	v->n[me].next = v->nn;
	*off = o;
	return me;
trunc:
	v->err = "truncated";
	return -1;
}

int pc_tv_parse(struct pc_tv *v, const unsigned char *b, size_t n)
{
	size_t off = 0;

	v->nn = 0;
	v->src = b;
	v->srclen = n;
	v->err = NULL;
	if (item(v, &off, 1) < 0)
		return -1;
	if (off != n) {
		v->err = "trailing bytes";
		return -1;
	}
	return 0;
}

void pc_tv_free(struct pc_tv *v)
{
	free(v->n);
	v->n = NULL;
	v->nn = v->cap = 0;
}

int pc_tv_get(const struct pc_tv *v, int map, const char *key)
{
	size_t kl = strlen(key);
	uint32_t i;
	int k;

	if (map < 0 || map >= v->nn || v->n[map].type != 'm')
		return -1;
	k = map + 1;
	for (i = 0; i < v->n[map].len; i++) {
		int val = v->n[k].next;

		if (v->n[k].len == kl && !memcmp(v->n[k].p, key, kl))
			return val;
		k = v->n[val].next;
	}
	return -1;
}

int pc_tv_at(const struct pc_tv *v, int arr, unsigned int idx)
{
	uint32_t i;
	int k;

	if (arr < 0 || arr >= v->nn ||
	        (v->n[arr].type != 'a' && v->n[arr].type != 'm') ||
	        idx >= v->n[arr].len)
		return -1;
	k = arr + 1;
	for (i = 0; i <= idx; i++) {
		if (v->n[arr].type == 'm')
			k = v->n[k].next;              /* past the key */
		if (i == idx)
			return k;
		k = v->n[k].next;
	}
	return -1;
}

int pc_tv_key_at(const struct pc_tv *v, int map, unsigned int idx)
{
	uint32_t i;
	int k;

	if (map < 0 || map >= v->nn || v->n[map].type != 'm' ||
	        idx >= v->n[map].len)
		return -1;
	k = map + 1;
	for (i = 0; i < idx; i++)
		k = v->n[v->n[k].next].next;
	return k;
}

int pc_tv_get_str(const struct pc_tv *v, int map, const char *key,
		char *buf, size_t cap)
{
	int k = pc_tv_get(v, map, key);

	if (k < 0 || v->n[k].type != 'b' || v->n[k].len > cap)
		return -1;
	memcpy(buf, v->n[k].p, v->n[k].len);
	return (int)v->n[k].len;
}

int pc_tv_get_int(const struct pc_tv *v, int map, const char *key,
		long long *out)
{
	int k = pc_tv_get(v, map, key);
	char num[24];
	char *end;

	if (k < 0)
		return -1;
	if (v->n[k].type == 'i') {
		*out = v->n[k].i;
		return 0;
	}
	if (v->n[k].type != 'd' || !v->n[k].len ||
	        v->n[k].len >= sizeof num)
		return -1;
	memcpy(num, v->n[k].p, v->n[k].len);
	num[v->n[k].len] = 0;
	*out = strtoll(num, &end, 10);
	return *end ? -1 : 0;
}

int pc_tv_get_bool(const struct pc_tv *v, int map, const char *key)
{
	int k = pc_tv_get(v, map, key);

	return k >= 0 && v->n[k].type == 't';
}

int pc_tv_streq(const struct pc_tv *v, int k, const char *s)
{
	size_t l = strlen(s);

	return k >= 0 && k < v->nn && v->n[k].type == 'b' &&
		v->n[k].len == l && !memcmp(v->n[k].p, s, l);
}

/* ---- renderers --------------------------------------------------------- */

static size_t one_json(struct pc_jw *w, const unsigned char *b, size_t n,
		int depth)
{
	size_t used;
	uint32_t len, cnt, i;
	int k;

	if (!n || depth > PT_DEPTH)
		return 0;
	switch (b[0]) {
	case 'i': {
		unsigned long long u = 0;

		if (n < 9)
			return 0;
		for (k = 8; k > 0; k--)
			u = u << 8 | b[k];
		pc_jw_i64(w, (long long)u);
		return 9;
	}
	case 'd':
		if (n < 3)
			return 0;
		len = (uint32_t)b[1] | (uint32_t)b[2] << 8;
		if (n - 3 < len)
			return 0;
		pc_jw_raw(w, (const char *)b + 3, len);
		return 3 + (size_t)len;
	case 'b':
	case 'e':
		if (n < 5)
			return 0;
		len = get32(b + 1);
		if (n - 5 < len)
			return 0;
		if (b[0] == 'e')
			pc_jw_lit(w, "{\"error\":");
		pc_jw_str_utf8(w, (const char *)b + 5, len);
		if (b[0] == 'e')
			pc_jw_lit(w, "}");
		return 5 + (size_t)len;
	case 'n':
		pc_jw_lit(w, "null");
		return 1;
	case 't':
		pc_jw_lit(w, "true");
		return 1;
	case 'f':
		pc_jw_lit(w, "false");
		return 1;
	case 'o':
		pc_jw_lit(w, "\"OK\"");
		return 1;
	case 'a':
	case 'm':
		if (n < 5)
			return 0;
		cnt = get32(b + 1);
		pc_jw_lit(w, b[0] == 'a' ? "[" : "{");
		used = 5;
		for (i = 0; i < cnt; i++) {
			size_t u;

			if (i)
				pc_jw_lit(w, ",");
			if (b[0] == 'm') {
				if (used >= n || b[used] != 'b')
					return 0;
				u = one_json(w, b + used, n - used, depth + 1);
				if (!u)
					return 0;
				used += u;
				pc_jw_lit(w, ":");
			}
			u = one_json(w, b + used, n - used, depth + 1);
			if (!u)
				return 0;
			used += u;
		}
		pc_jw_lit(w, b[0] == 'a' ? "]" : "}");
		return used;
	}
	return 0;
}

size_t pc_tree_json(struct pc_jw *w, const unsigned char *b, size_t n)
{
	return one_json(w, b, n, 1);
}

static size_t one_resp(struct pc_jw *w, const unsigned char *b, size_t n,
		int depth)
{
	char num[32];
	size_t used;
	uint32_t len, cnt, i;
	int k;

	if (!n || depth > PT_DEPTH)
		return 0;
	switch (b[0]) {
	case 'i': {
		unsigned long long u = 0;

		if (n < 9)
			return 0;
		for (k = 8; k > 0; k--)
			u = u << 8 | b[k];
		snprintf(num, sizeof num, ":%lld\r\n", (long long)u);
		pc_jw_raw(w, num, strlen(num));
		return 9;
	}
	case 'd':
		if (n < 3)
			return 0;
		len = (uint32_t)b[1] | (uint32_t)b[2] << 8;
		if (n - 3 < len)
			return 0;
		snprintf(num, sizeof num, "$%u\r\n", len);
		pc_jw_raw(w, num, strlen(num));
		pc_jw_raw(w, (const char *)b + 3, len);
		pc_jw_lit(w, "\r\n");
		return 3 + (size_t)len;
	case 'b':
	case 'e':
		if (n < 5)
			return 0;
		len = get32(b + 1);
		if (n - 5 < len)
			return 0;
		if (b[0] == 'e') {
			pc_jw_lit(w, "-");
		} else {
			snprintf(num, sizeof num, "$%u\r\n", len);
			pc_jw_raw(w, num, strlen(num));
		}
		pc_jw_raw(w, (const char *)b + 5, len);
		pc_jw_lit(w, "\r\n");
		return 5 + (size_t)len;
	case 'n':
		pc_jw_lit(w, "$-1\r\n");
		return 1;
	case 't':
		pc_jw_lit(w, ":1\r\n");
		return 1;
	case 'f':
		pc_jw_lit(w, ":0\r\n");
		return 1;
	case 'o':
		pc_jw_lit(w, "+OK\r\n");
		return 1;
	case 'a':
	case 'm':
		if (n < 5)
			return 0;
		cnt = get32(b + 1);
		snprintf(num, sizeof num, "*%u\r\n", b[0] == 'm' ? cnt * 2 : cnt);
		pc_jw_raw(w, num, strlen(num));
		used = 5;
		for (i = 0; i < (b[0] == 'm' ? cnt * 2 : cnt); i++) {
			size_t u = one_resp(w, b + used, n - used, depth + 1);

			if (!u)
				return 0;
			used += u;
		}
		return used;
	}
	return 0;
}

size_t pc_tree_resp(struct pc_jw *w, const unsigned char *b, size_t n)
{
	return one_resp(w, b, n, 1);
}

/* ---- heap switch ------------------------------------------------------- */

int pc_tw_to_heap(struct pc_tw *w, size_t cap)
{
	unsigned char *b;

	if (w->n || w->owned || w->depth)
		return -1;
	if (cap < 512)
		cap = 512;
	if (cap > PT_HEAP_MAX)
		cap = PT_HEAP_MAX;
	b = malloc(cap);
	if (!b)
		return -1;
	w->b = b;
	w->cap = cap;
	w->owned = 1;
	return 0;
}

/* ---- JSON text to a tree ----------------------------------------------- */

/* the first token after @i's subtree */
static int skip_tok(const struct pc_jtok *t, int ntok, int i)
{
	int j = i + 1;

	while (j < ntok) {
		int p = t[j].parent;

		while (p > i)
			p = t[p].parent;
		if (p != i)
			break;
		j++;
	}
	return j;
}

/* a string token as a 'b' item, unescaped straight into the writer's
 * buffer (the escaped span is never shorter than the bytes); @is_key
 * counts it as a map key */
static int put_str(struct pc_tw *w, const char *json, const struct pc_jtok *k,
		int is_key, const char **err)
{
	size_t span = (size_t)(k->end - k->start);
	int n;

	if (!room(w, 5 + span)) {
		*err = "out of room";
		return -1;
	}
	n = pc_json_unescape(json, k, (char *)w->b + w->n + 5, span);
	if (n < 0) {
		*err = "bad escape";
		return -1;
	}
	counted(w, is_key);
	w->b[w->n] = 'b';
	put32(w->b + w->n + 1, (uint32_t)n);
	w->n += 5 + (size_t)n;
	return 0;
}

static int is_doc_key(const char *json, const struct pc_jtok *k,
		const char *const *doc_keys)
{
	size_t kl = (size_t)(k->end - k->start);

	if (!doc_keys)
		return 0;
	for (; *doc_keys; doc_keys++)
		if (strlen(*doc_keys) == kl && !memcmp(json + k->start, *doc_keys, kl))
			return 1;
	return 0;
}

/* write token @i; returns the token after its subtree, -1 with *err */
static int from_tok(const char *json, const struct pc_jtok *t, int ntok,
		int i, struct pc_tw *w, const char *const *doc_keys, int depth,
		const char **err)
{
	int k, n, cnt;

	if (i >= ntok) {
		*err = "malformed";
		return -1;
	}
	if (depth > PT_DEPTH) {
		*err = "too deep";
		return -1;
	}
	switch (t[i].type) {
	case PC_J_STR:
		return put_str(w, json, &t[i], 0, err) < 0 ? -1 : i + 1;
	case PC_J_PRIM: {
		const char *s = json + t[i].start;
		int l = t[i].end - t[i].start;

		if (l == 4 && !memcmp(s, "true", 4))
			pc_tw_bool(w, 1);
		else if (l == 5 && !memcmp(s, "false", 5))
			pc_tw_bool(w, 0);
		else if (l == 4 && !memcmp(s, "null", 4))
			pc_tw_nil(w);
		else {
			char num[24], *end;
			long long v = 0;
			int plain = l > 0 && l < (int)sizeof num;

			for (k = 0; plain && k < l; k++)
				if (!((s[k] >= '0' && s[k] <= '9') ||
				        (k == 0 && s[k] == '-' && l > 1)))
					plain = 0;
			if (plain) {
				memcpy(num, s, (size_t)l);
				num[l] = 0;
				errno = 0;
				v = strtoll(num, &end, 10);
				if (*end || errno == ERANGE)
					plain = 0;
			}
			if (plain)
				pc_tw_i64(w, v);
			else
				pc_tw_num(w, s, (size_t)l);
		}
		return i + 1;
	}
	case PC_J_ARR:
		cnt = t[i].size;
		pc_tw_arr(w);
		k = i + 1;
		for (n = 0; n < cnt; n++) {
			k = from_tok(json, t, ntok, k, w, doc_keys, depth + 1, err);
			if (k < 0)
				return -1;
		}
		pc_tw_end(w);
		return k;
	case PC_J_OBJ:
		cnt = t[i].size;
		pc_tw_map(w);
		k = i + 1;
		for (n = 0; n < cnt; n++) {
			const struct pc_jtok *key;

			if (k + 1 >= ntok || t[k].type != PC_J_STR) {
				*err = "malformed";
				return -1;
			}
			key = &t[k];
			if (put_str(w, json, key, 1, err) < 0)
				return -1;
			k++;
			if (is_doc_key(json, key, doc_keys)) {
				/* the document: its JSON text as one bulk */
				int a = t[k].type == PC_J_STR ? t[k].start - 1 : t[k].start;
				int b = t[k].type == PC_J_STR ? t[k].end + 1 : t[k].end;

				pc_tw_bulk(w, json + a, (size_t)(b - a));
				k = skip_tok(t, ntok, k);
			} else {
				k = from_tok(json, t, ntok, k, w, doc_keys, depth + 1, err);
				if (k < 0)
					return -1;
			}
		}
		pc_tw_end(w);
		return k;
	default:
		*err = "malformed";
		return -1;
	}
}

int pc_tree_from_json(const char *json, size_t n, struct pc_tw *w,
		const char *const *doc_keys, const char **err)
{
	struct pc_jtok *t;
	int maxt = (int)(n / 2) + 8, ntok, last;

	if (maxt > 262144)
		maxt = 262144;
	t = malloc(sizeof *t * (size_t)maxt);
	if (!t) {
		*err = "out of memory";
		return -1;
	}
	ntok = pc_json_parse(json, n, t, maxt);
	if (ntok < 1) {
		free(t);
		*err = "malformed";
		return -1;
	}
	last = from_tok(json, t, ntok, 0, w, doc_keys, 1, err);
	free(t);
	if (last < 0)
		return -1;
	if (last != ntok) {
		*err = "trailing text";
		return -1;
	}
	if (w->over) {
		*err = "out of room";
		return -1;
	}
	return 0;
}
