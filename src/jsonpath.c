/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * jsonpath.c — the JSON path engine (task S26).  See jsonpath.h.
 *
 * Everything is span splicing over the S7 tokenizer: parse the document,
 * walk the path to a leaf (or its would-be parent), then rebuild the
 * text as prefix + fragment + suffix.  Token spans need one adjustment:
 * for strings, start/end EXCLUDE the quotes, so the syntactic extent is
 * start-1 .. end+1; containers and primitives already span their whole
 * text.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "json.h"
#include "jsonpath.h"

#define JP_MAX_STEPS 32
#define JP_NAME_MAX  256

struct jp_step {
	const char *name;          /* NULL = index step */
	int namelen;
	long idx;
};

/* split "$.a.b[2].c" into steps; returns count or PC_JP_E_PATH.
 * v1 names are plain: no quoting, no '.', '[', '"', '\' or controls. */
static int jp_split(const char *p, size_t n, struct jp_step *st)
{
	size_t i = 0;
	int ns = 0;

	if (n == 0 || p[0] != '$')
		return PC_JP_E_PATH;
	i = 1;
	while (i < n) {
		if (ns >= JP_MAX_STEPS)
			return PC_JP_E_PATH;
		if (p[i] == '.') {
			size_t s = ++i;

			while (i < n && p[i] != '.' && p[i] != '[') {
				unsigned char c = (unsigned char)p[i];

				if (c < 0x20 || c == '"' || c == '\\')
					return PC_JP_E_PATH;
				i++;
			}
			if (i == s || i - s > JP_NAME_MAX)
				return PC_JP_E_PATH;
			st[ns].name = p + s;
			st[ns].namelen = (int)(i - s);
			ns++;
		} else if (p[i] == '[') {
			long v = 0;
			size_t s = ++i;

			while (i < n && p[i] >= '0' && p[i] <= '9') {
				v = v * 10 + (p[i] - '0');
				if (v > 1000000000)
					return PC_JP_E_PATH;
				i++;
			}
			if (i == s || i >= n || p[i] != ']')
				return PC_JP_E_PATH;
			i++;
			st[ns].name = NULL;
			st[ns].namelen = 0;
			st[ns].idx = v;
			ns++;
		} else {
			return PC_JP_E_PATH;
		}
	}
	return ns;
}

/* syntactic extent of a token (strings reclaim their quotes) */
static int ext_start(const struct pc_jtok *t)
{
	return t->type == PC_J_STR ? t->start - 1 : t->start;
}
static int ext_end(const struct pc_jtok *t)
{
	return t->type == PC_J_STR ? t->end + 1 : t->end;
}

/* the walk result: leaf >= 0 when the full path resolved; otherwise
 * parent/-holds the last container reached (valid only when rc says
 * NOLEAF - the final step's parent exists but the member does not). */
struct jp_walk {
	int leaf;                  /* token idx of the value, or -1 */
	int parent;                /* the final step's container token */
	int key_start;             /* obj member: byte offset of the key's
	                            * opening quote (leaf >= 0 only) */
	int prev_end;              /* ext end of the previous sibling value,
	                            * or -1 when the leaf is the first member */
	const struct jp_step *last;
};

/* find the final-step member inside @parent; fills w->leaf/key_start/
 * prev_end.  Returns 0, PC_JP_E_NOLEAF or PC_JP_E_TYPE. */
static int jp_member(const char *doc, const struct pc_jtok *t, int ntok,
		int parent, const struct jp_step *st, struct jp_walk *w)
{
	int i, child = 0, prev = -1;

	w->leaf = -1;
	w->parent = parent;
	w->prev_end = -1;
	w->key_start = -1;
	w->last = st;
	if (st->name) {
		if (t[parent].type != PC_J_OBJ)
			return PC_JP_E_TYPE;
		for (i = parent + 1; i < ntok; i++) {
			if (t[i].parent != parent)
				continue;
			if ((child++ & 1) == 0) {
				/* a key position */
				if (t[i].type == PC_J_STR &&
				        t[i].end - t[i].start == st->namelen &&
				        !memcmp(doc + t[i].start, st->name,
				                (size_t)st->namelen) &&
				        i + 1 < ntok) {
					w->leaf = i + 1;
					w->key_start = t[i].start - 1;
					w->prev_end = prev;
					return 0;
				}
			} else {
				prev = ext_end(&t[i]);
			}
		}
		return PC_JP_E_NOLEAF;
	}
	if (t[parent].type != PC_J_ARR)
		return PC_JP_E_TYPE;
	for (i = parent + 1; i < ntok; i++) {
		if (t[i].parent != parent)
			continue;
		if (child++ == st->idx) {
			w->leaf = i;
			w->prev_end = prev;
			return 0;
		}
		prev = ext_end(&t[i]);
	}
	return PC_JP_E_NOLEAF;
}

/* walk all steps.  On success w->leaf is the value token.  On a missing
 * FINAL step returns PC_JP_E_NOLEAF with w->parent/w->last valid (so
 * jset can create); a missing INTERMEDIATE step is PC_JP_E_MISSING. */
static int jp_walk(const char *doc, const struct pc_jtok *t, int ntok,
		const struct jp_step *st, int ns, struct jp_walk *w)
{
	int cur = 0, s, rc;

	w->leaf = 0;               /* zero steps = the root */
	w->parent = -1;
	w->last = NULL;
	for (s = 0; s < ns; s++) {
		rc = jp_member(doc, t, ntok, cur, &st[s], w);
		if (rc == PC_JP_E_NOLEAF && s < ns - 1)
			return PC_JP_E_MISSING;
		if (rc != 0)
			return rc;
		cur = w->leaf;
	}
	w->leaf = cur;
	return 0;
}

/* tokenize with a heap array sized to the document */
static int jp_tokenize(const char *doc, size_t dlen, struct pc_jtok **tp)
{
	int maxt = (int)(dlen / 2) + 8, ntok;

	if (maxt > 32768)
		maxt = 32768;
	*tp = malloc(sizeof(struct pc_jtok) * (size_t)maxt);
	if (!*tp)
		return PC_JP_E_DOC;
	ntok = pc_json_parse(doc, dlen, *tp, maxt);
	if (ntok <= 0) {
		free(*tp);
		*tp = NULL;
		return PC_JP_E_DOC;
	}
	return ntok;
}

/* is @val exactly one JSON value (no trailing garbage)? */
static int jp_valid_value(const char *val, size_t vlen)
{
	struct pc_jtok *t;
	int ntok, e;
	size_t i;

	ntok = jp_tokenize(val, vlen, &t);
	if (ntok < 0)
		return 0;
	e = ext_end(&t[0]);
	for (i = (size_t)e; i < vlen; i++)
		if (val[i] != ' ' && val[i] != '\t' && val[i] != '\n' &&
		        val[i] != '\r') {
			free(t);
			return 0;
		}
	/* leading whitespace before a second root would confuse splices */
	for (i = 0; i < (size_t)ext_start(&t[0]); i++)
		if (val[i] != ' ' && val[i] != '\t' && val[i] != '\n' &&
		        val[i] != '\r') {
			free(t);
			return 0;
		}
	free(t);
	return 1;
}

/* out = doc[0..from) + ins + doc[to..dlen) */
static int jp_splice(const char *doc, size_t dlen, int from, int to,
		const char *ins, size_t ilen, char *out, size_t cap)
{
	size_t n = (size_t)from + ilen + (dlen - (size_t)to);

	if (n > cap || n > PC_JP_MAX)
		return PC_JP_E_SIZE;
	memcpy(out, doc, (size_t)from);
	memcpy(out + from, ins, ilen);
	memcpy(out + from + ilen, doc + to, dlen - (size_t)to);
	return (int)n;
}

int pc_jp_get(const char *doc, size_t dlen, const char *path, size_t plen,
		char *out, size_t cap)
{
	struct jp_step st[JP_MAX_STEPS];
	struct pc_jtok *t;
	struct jp_walk w;
	int ns, ntok, rc, a, b;

	ns = jp_split(path, plen, st);
	if (ns < 0)
		return ns;
	ntok = jp_tokenize(doc, dlen, &t);
	if (ntok < 0)
		return ntok;
	rc = jp_walk(doc, t, ntok, st, ns, &w);
	if (rc != 0) {
		free(t);
		return rc;
	}
	a = ext_start(&t[w.leaf]);
	b = ext_end(&t[w.leaf]);
	free(t);
	if ((size_t)(b - a) > cap)
		return PC_JP_E_SIZE;
	memcpy(out, doc + a, (size_t)(b - a));
	return b - a;
}

int pc_jp_set(const char *doc, size_t dlen, const char *path, size_t plen,
		const char *val, size_t vlen, int nx, int xx, int mkpath,
		char *out, size_t cap)
{
	struct jp_step st[JP_MAX_STEPS];
	struct pc_jtok *t;
	struct jp_walk w;
	int ns, ntok, rc;

	ns = jp_split(path, plen, st);
	if (ns < 0)
		return ns;
	if (!jp_valid_value(val, vlen))
		return PC_JP_E_VAL;
	if (ns == 0) {
		/* root replace: the document becomes @val */
		if (vlen > cap || vlen > PC_JP_MAX)
			return PC_JP_E_SIZE;
		memcpy(out, val, vlen);
		return (int)vlen;
	}
	ntok = jp_tokenize(doc, dlen, &t);
	if (ntok < 0)
		return ntok;
	rc = jp_walk(doc, t, ntok, st, ns, &w);
	if (rc == 0) {
		/* leaf exists: replace its extent */
		int a = ext_start(&t[w.leaf]), b = ext_end(&t[w.leaf]);

		free(t);
		if (nx)
			return PC_JP_E_EXISTS;
		return jp_splice(doc, dlen, a, b, val, vlen, out, cap);
	}
	if (rc == PC_JP_E_MISSING && mkpath) {
		/* walk manually to the deepest EXISTING container, then
		 * synthesize "rest1":{"rest2":{...VAL}} inside it - name
		 * steps only, and the container must be an object */
		int cur = 0, si, at, nonempty, n2;
		char ins[JP_NAME_MAX * 2 + 8];
		size_t need, k;

		for (si = 0; si < ns; si++) {
			struct jp_walk w2;
			int r2 = jp_member(doc, t, ntok, cur, &st[si], &w2);

			if (r2 == 0) {
				cur = w2.leaf;
				continue;
			}
			if (r2 != PC_JP_E_NOLEAF) {
				free(t);
				return r2;
			}
			if (!st[si].name || t[cur].type != PC_J_OBJ) {
				free(t);
				/* an index step cannot be created; a name
				 * step under a non-object is a type clash */
				return st[si].name ? PC_JP_E_TYPE
					: PC_JP_E_MISSING;
			}
			break;                 /* create from step @si down */
		}
		for (k = (size_t)si; k < (size_t)ns; k++)
			if (!st[k].name) {
				free(t);
				return PC_JP_E_MISSING;   /* index: no create */
			}
		if (xx) {
			free(t);
			return PC_JP_E_NOLEAF;
		}
		at = t[cur].end - 1;
		nonempty = t[cur].size > 0;
		free(t);
		/* prefix: ,"r1":{"r2":{...  suffix: }}} */
		need = (size_t)at + vlen + (dlen - (size_t)at) +
			(size_t)(ns - si) * (JP_NAME_MAX + 4) + 2;
		if (need > cap)
			return PC_JP_E_SIZE;
		{
			char *o = out;

			memcpy(o, doc, (size_t)at);
			o += at;
			for (k = (size_t)si; k < (size_t)ns; k++) {
				n2 = snprintf(ins, sizeof ins,
					"%s\"%.*s\":%s",
					k == (size_t)si ? (nonempty ? "," : "")
						: "",
					st[k].namelen, st[k].name,
					k + 1 < (size_t)ns ? "{" : "");
				memcpy(o, ins, (size_t)n2);
				o += n2;
			}
			memcpy(o, val, vlen);
			o += vlen;
			for (k = (size_t)si; k + 1 < (size_t)ns; k++)
				*o++ = '}';
			memcpy(o, doc + at, dlen - (size_t)at);
			o += dlen - (size_t)at;
			return (int)(o - out);
		}
	}
	if (rc == PC_JP_E_NOLEAF && w.last && w.last->name) {
		/* a new member under an existing object: insert before the
		 * closing brace, comma-prefixed unless the object is empty */
		char ins[JP_NAME_MAX + 16];
		int at = t[w.parent].end - 1, n;
		int nonempty = t[w.parent].size > 0;

		free(t);
		if (xx)
			return PC_JP_E_NOLEAF;
		n = snprintf(ins, sizeof ins, "%s\"%.*s\":",
			nonempty ? "," : "", w.last->namelen, w.last->name);
		if (n < 0 || (size_t)n >= sizeof ins)
			return PC_JP_E_PATH;
		/* two splices in one: prefix + name + val at the same spot */
		if ((size_t)at + (size_t)n + vlen + (dlen - (size_t)at) > cap)
			return PC_JP_E_SIZE;
		memcpy(out, doc, (size_t)at);
		memcpy(out + at, ins, (size_t)n);
		memcpy(out + at + n, val, vlen);
		memcpy(out + at + n + vlen, doc + at, dlen - (size_t)at);
		return at + n + (int)vlen + (int)(dlen - (size_t)at);
	}
	free(t);
	/* array holes are not creatable (append is a later verb) */
	return rc == PC_JP_E_NOLEAF ? PC_JP_E_MISSING : rc;
}

int pc_jp_append(const char *doc, size_t dlen, const char *path,
		size_t plen, const char *val, size_t vlen, int *count_out,
		char *out, size_t cap)
{
	struct jp_step st[JP_MAX_STEPS];
	struct pc_jtok *t;
	struct jp_walk w;
	int ns, ntok, rc, at, nonempty, n;

	ns = jp_split(path, plen, st);
	if (ns < 0)
		return ns;
	if (!jp_valid_value(val, vlen))
		return PC_JP_E_VAL;
	ntok = jp_tokenize(doc, dlen, &t);
	if (ntok < 0)
		return ntok;
	rc = jp_walk(doc, t, ntok, st, ns, &w);
	if (rc != 0) {
		free(t);
		return rc;
	}
	if (t[w.leaf].type != PC_J_ARR) {
		free(t);
		return PC_JP_E_TYPE;
	}
	at = t[w.leaf].end - 1;            /* before the closing bracket */
	nonempty = t[w.leaf].size > 0;
	if (count_out)
		*count_out = t[w.leaf].size + 1;
	free(t);
	{
		size_t need = dlen + vlen + 1;

		if (need > cap || need > PC_JP_MAX)
			return PC_JP_E_SIZE;
		memcpy(out, doc, (size_t)at);
		n = at;
		if (nonempty)
			out[n++] = ',';
		memcpy(out + n, val, vlen);
		n += (int)vlen;
		memcpy(out + n, doc + at, dlen - (size_t)at);
		return n + (int)(dlen - (size_t)at);
	}
}

int pc_jp_del(const char *doc, size_t dlen, const char *path, size_t plen,
		char *out, size_t cap)
{
	struct jp_step st[JP_MAX_STEPS];
	struct pc_jtok *t;
	struct jp_walk w;
	int ns, ntok, rc, a, b;

	ns = jp_split(path, plen, st);
	if (ns < 0)
		return ns;
	if (ns == 0)
		return PC_JP_E_PATH;       /* deleting the root is verb del */
	ntok = jp_tokenize(doc, dlen, &t);
	if (ntok < 0)
		return ntok;
	rc = jp_walk(doc, t, ntok, st, ns, &w);
	if (rc != 0) {
		free(t);
		return rc;
	}
	b = ext_end(&t[w.leaf]);
	if (w.prev_end >= 0) {
		/* not the first member: cut from the previous sibling's end -
		 * that swallows the separating comma */
		a = w.prev_end;
	} else {
		/* first member: cut from the key (objects) or the value
		 * (arrays), then swallow one FOLLOWING comma if any */
		a = w.key_start >= 0 ? w.key_start : ext_start(&t[w.leaf]);
		while ((size_t)b < dlen && (doc[b] == ' ' || doc[b] == '\t' ||
		        doc[b] == '\n' || doc[b] == '\r'))
			b++;
		if ((size_t)b < dlen && doc[b] == ',')
			b++;
	}
	free(t);
	return jp_splice(doc, dlen, a, b, "", 0, out, cap);
}

int pc_jp_incr(const char *doc, size_t dlen, const char *path, size_t plen,
		long long by, long long *result, char *out, size_t cap)
{
	struct jp_step st[JP_MAX_STEPS];
	struct pc_jtok *t;
	struct jp_walk w;
	char num[24], *endp;
	long long v;
	int ns, ntok, rc, a, b, n;

	ns = jp_split(path, plen, st);
	if (ns < 0)
		return ns;
	ntok = jp_tokenize(doc, dlen, &t);
	if (ntok < 0)
		return ntok;
	rc = jp_walk(doc, t, ntok, st, ns, &w);
	if (rc != 0) {
		free(t);
		return rc;
	}
	a = t[w.leaf].start;
	b = t[w.leaf].end;
	if (t[w.leaf].type != PC_J_PRIM || b - a <= 0 ||
	        b - a >= (int)sizeof num ||
	        !(doc[a] == '-' || (doc[a] >= '0' && doc[a] <= '9'))) {
		free(t);
		return PC_JP_E_NUM;
	}
	memcpy(num, doc + a, (size_t)(b - a));
	num[b - a] = 0;
	v = strtoll(num, &endp, 10);
	if (*endp) {
		free(t);
		return PC_JP_E_NUM;        /* floats stay untouched in v1 */
	}
	free(t);
	v += by;
	*result = v;
	n = snprintf(num, sizeof num, "%lld", v);
	return jp_splice(doc, dlen, a, b, num, (size_t)n, out, cap);
}
