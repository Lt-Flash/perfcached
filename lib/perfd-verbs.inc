/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov
 * Part of libperfd - see lib/LICENSE.  This file must stay
 * free of src/core includes; tools/sync-libperfd.sh exports
 * exactly the MIT set to consumers. */
/*
 * perfd-verbs.inc - part of perfd.c, which #includes it in order: one translation
 * unit, so every static stays file-local and consumers still compile
 * perfd.c alone.  Not a compilation unit of its own.
 *
 * Holds the typed verbs: strings and counters, conditional sets (S279, S297),
 * hashes (S313), rate limits (S314) and the JSON path verbs.
 */
/* ---- request helpers --------------------------------------------------- */

/* open a params map holding col and key; the caller adds its own
 * members and closes it */
static void tw_ck(struct pc_tw *w, const char *col, const char *key)
{
	pc_tw_map(w);
	pc_tw_key(w, "col");
	pc_tw_str(w, col);
	pc_tw_key(w, "key");
	pc_tw_str(w, key);
}

/* ---- typed verbs ------------------------------------------------------- */

int perfd_ping(perfd_t *p)
{
	char *r = NULL;
	size_t n = 0;
	int ok;

	if (bin_roundtrip(p, BV_PING, NULL, NULL, "perfd", 5, 0, 0, &r, &n) != 0)
		return -1;
	ok = r && n == 5 && memcmp(r, "perfd", 5) == 0;
	
	return ok ? 0 : errk(p, PERFD_E_PROTOCOL, "bad pong");
}

int perfd_set(perfd_t *p, const char *col, const char *key,
		const void *val, size_t vlen, long long ttl)
{
	char *r = NULL;
	size_t n = 0;
	int ok;

	if (bin_roundtrip(p, BV_SET, col, key, val, vlen, 0, ttl, &r, &n) != 0)
		return -1;
	ok = r && n >= 1 && r[0] == 1;
	
	return ok ? 0 : errk(p, PERFD_E_REFUSED, "not stored");
}

int perfd_set_cond(perfd_t *p, const char *col, const char *key,
		const void *val, size_t vlen, long long ttl, int cond)
{
	char *r = NULL;
	size_t n = 0;
	int ok;

	if (cond != PERFD_NX && cond != PERFD_XX)
		return errk(p, PERFD_E_ARG, "cond must be PERFD_NX or PERFD_XX");
	if (bin_roundtrip(p, cond == PERFD_NX ? BV_SETNX : BV_SETXX, col, key,
	        val, vlen, 0, ttl, &r, &n) != 0)
		return -1;
	ok = r && n >= 1 && r[0] == 1;
	
	return ok;
}

/* ---- S313: hashes ---------------------------------------------------------
 * Every call sends one H command (verb 18, the fixed layout) and reads
 * the daemon's reply TREE as the payload: 'i' i64 | 'b' u32 len, bytes
 * | 'n' | 'a' u32 count, items | 'o' | 'e' u32 len, the error line -
 * the S313 subset of src/ptree.h, read here by a cursor rather than an
 * index because every reply is a flat item or one array. */
struct hrt {
	const unsigned char *b;
	size_t n, o;
};

static unsigned int hrt_u32(const unsigned char *p)
{
	return (unsigned int)p[0] | (unsigned int)p[1] << 8 |
		(unsigned int)p[2] << 16 | (unsigned int)p[3] << 24;
}

/* the next item's type, or 0 at the end / on a short tree */
static int hrt_peek(const struct hrt *t)
{
	return t->o < t->n ? t->b[t->o] : 0;
}

static int hrt_int(struct hrt *t, long long *v)
{
	unsigned long long u = 0;
	int i;

	if (hrt_peek(t) != 'i' || t->n - t->o < 9)
		return -1;
	for (i = 8; i > 0; i--)
		u = (u << 8) | t->b[t->o + (size_t)i];
	*v = (long long)u;
	t->o += 9;
	return 0;
}

/* 1 a bulk (*p, *l into the tree), 0 nil, -1 neither */
static int hrt_bulk(struct hrt *t, const unsigned char **p, size_t *l)
{
	size_t len;

	if (hrt_peek(t) == 'n') {
		t->o++;
		return 0;
	}
	if (hrt_peek(t) != 'b' || t->n - t->o < 5)
		return -1;
	len = hrt_u32(t->b + t->o + 1);
	if (t->n - t->o - 5 < len)
		return -1;
	*p = t->b + t->o + 5;
	*l = len;
	t->o += 5 + len;
	return 1;
}

static int hrt_arr(struct hrt *t, unsigned int *cnt)
{
	if (hrt_peek(t) != 'a' || t->n - t->o < 5)
		return -1;
	*cnt = hrt_u32(t->b + t->o + 1);
	t->o += 5;
	return 0;
}

/* run @cmd key args... on the key's node; *@tree (malloc'd) is the reply.
 * An error reply becomes -1 with the daemon's line in perfd_error(). */
static int hcall(perfd_t *p, const char *col, const char *key,
		const char *cmd, int nargs, const char *const *args,
		const size_t *argl, unsigned char **tree, size_t *tn)
{
	char *r = NULL;
	size_t n = 0;
	int i;

	*tree = NULL;
	if (nargs > 500)
		return errk(p, PERFD_E_ARG, "too many arguments");
	{
		size_t need = 2 + 4 + strlen(cmd), o;
		unsigned char *enc;

		for (i = 0; i < nargs; i++)
			need += 4 + argl[i];
		/* S284: the handle's request scratch, grown once */
		if (grow_buf(p, &p->wsc, &p->wscap, need) != 0)
			return -1;
		enc = p->wsc;
		enc[0] = (unsigned char)(nargs + 1);
		enc[1] = (unsigned char)((nargs + 1) >> 8);
		o = 2;
		for (i = -1; i < nargs; i++) {
			const char *a = i < 0 ? cmd : args[i];
			size_t l = i < 0 ? strlen(cmd) : argl[i];

			enc[o] = (unsigned char)l;
			enc[o + 1] = (unsigned char)(l >> 8);
			enc[o + 2] = (unsigned char)(l >> 16);
			enc[o + 3] = (unsigned char)(l >> 24);
			memcpy(enc + o + 4, a, l);
			o += 4 + l;
		}
		i = bin_roundtrip(p, BV_HCMD, col, key, enc, need, 0, 0, &r, &n);
		if (i != 0)
			return -1;
		*tree = (unsigned char *)r;
		*tn = n;
	}
	if (*tn && (*tree)[0] == 'e' && *tn >= 5) {
		size_t l = hrt_u32(*tree + 1);
		char msg[256];

		if (l > *tn - 5)
			l = *tn - 5;
		if (l >= sizeof msg)
			l = sizeof msg - 1;
		memcpy(msg, *tree + 5, l);
		msg[l] = 0;
		*tree = NULL;                  /* borrowed (S284) */
		/* a server's own words, never a format (it used to be one) */
		return errk(p, server_kind(msg), "%s", msg);
	}
	return 0;
}

/* an H command whose reply is one integer */
static int hcall_int(perfd_t *p, const char *col, const char *key,
		const char *cmd, int nargs, const char *const *args,
		const size_t *argl, long long *v)
{
	unsigned char *tr;
	size_t tn;
	struct hrt t;

	if (hcall(p, col, key, cmd, nargs, args, argl, &tr, &tn) != 0)
		return -1;
	t.b = tr;
	t.n = tn;
	t.o = 0;
	if (hrt_int(&t, v) != 0) {
		return errk(p, PERFD_E_PROTOCOL, "bad reply");
	}
	return 0;
}

int perfd_hset(perfd_t *p, const char *col, const char *key,
		const char *field, const void *val, size_t vlen)
{
	const char *a[2] = { field, (const char *)val };
	size_t l[2] = { strlen(field), vlen };
	long long v;

	return hcall_int(p, col, key, "HSET", 2, a, l, &v) ? -1 : (int)v;
}

int perfd_hsetnx(perfd_t *p, const char *col, const char *key,
		const char *field, const void *val, size_t vlen)
{
	const char *a[2] = { field, (const char *)val };
	size_t l[2] = { strlen(field), vlen };
	long long v;

	return hcall_int(p, col, key, "HSETNX", 2, a, l, &v) ? -1 : (int)v;
}

int perfd_hdel(perfd_t *p, const char *col, const char *key,
		const char *field)
{
	const char *a[1] = { field };
	size_t l[1] = { strlen(field) };
	long long v;

	return hcall_int(p, col, key, "HDEL", 1, a, l, &v) ? -1 : (int)v;
}

int perfd_hexists(perfd_t *p, const char *col, const char *key,
		const char *field)
{
	const char *a[1] = { field };
	size_t l[1] = { strlen(field) };
	long long v;

	return hcall_int(p, col, key, "HEXISTS", 1, a, l, &v) ? -1 : (int)v;
}

int perfd_hlen(perfd_t *p, const char *col, const char *key, long long *n)
{
	return hcall_int(p, col, key, "HLEN", 0, NULL, NULL, n);
}

int perfd_hincrby(perfd_t *p, const char *col, const char *key,
		const char *field, long long by, long long *result)
{
	char num[24];
	const char *a[2];
	size_t l[2];
	long long v;

	snprintf(num, sizeof num, "%lld", by);
	a[0] = field;
	a[1] = num;
	l[0] = strlen(field);
	l[1] = strlen(num);
	if (hcall_int(p, col, key, "HINCRBY", 2, a, l, &v) != 0)
		return -1;
	if (result)
		*result = v;
	return 0;
}

int perfd_hget(perfd_t *p, const char *col, const char *key,
		const char *field, void **val, size_t *vlen)
{
	const char *a[1] = { field };
	size_t l[1] = { strlen(field) }, bl;
	unsigned char *tr;
	const unsigned char *bp;
	size_t tn;
	struct hrt t;
	int rc;

	*val = NULL;
	*vlen = 0;
	if (hcall(p, col, key, "HGET", 1, a, l, &tr, &tn) != 0)
		return -1;
	t.b = tr;
	t.n = tn;
	t.o = 0;
	rc = hrt_bulk(&t, &bp, &bl);
	if (rc < 0) {
		return errk(p, PERFD_E_PROTOCOL, "bad reply");
	}
	if (rc == 1) {
		*val = malloc(bl ? bl : 1);
		if (!*val) {
			return errk(p, PERFD_E_NOMEM, "out of memory");
		}
		memcpy(*val, bp, bl);
		*vlen = bl;
	}
	return rc;
}

/* S353: a _buf call's copy - straight from the reply the handle lends into
 * the caller's buffer.  Too small: the buffer is left untouched and *vlen
 * says how much is needed (-2); the caller retries with a bigger one. */
static int buf_out(const void *src, size_t n, void *buf, size_t cap,
		size_t *vlen)
{
	if (vlen)
		*vlen = n;
	if (n > cap)
		return -2;
	if (n)
		memcpy(buf, src, n);
	return 1;
}

int perfd_hget_buf(perfd_t *p, const char *col, const char *key,
		const char *field, void *buf, size_t cap, size_t *vlen)
{
	const char *a[1] = { field };
	size_t l[1] = { strlen(field) }, bl;
	unsigned char *tr;
	const unsigned char *bp;
	size_t tn;
	struct hrt t;
	int rc;

	if (vlen)
		*vlen = 0;
	if (hcall(p, col, key, "HGET", 1, a, l, &tr, &tn) != 0)
		return -1;
	t.b = tr;
	t.n = tn;
	t.o = 0;
	rc = hrt_bulk(&t, &bp, &bl);
	if (rc < 0)
		return errk(p, PERFD_E_PROTOCOL, "bad reply");
	if (rc == 0)
		return 0;
	return buf_out(bp, bl, buf, cap, vlen);
}

int perfd_hgetall(perfd_t *p, const char *col, const char *key,
		perfd_hfield_cb cb, void *ctx)
{
	unsigned char *tr;
	size_t tn;
	struct hrt t;
	unsigned int cnt, i;

	if (hcall(p, col, key, "HGETALL", 0, NULL, NULL, &tr, &tn) != 0)
		return -1;
	t.b = tr;
	t.n = tn;
	t.o = 0;
	if (hrt_arr(&t, &cnt) != 0 || cnt % 2) {
		return errk(p, PERFD_E_PROTOCOL, "bad reply");
	}
	for (i = 0; i < cnt; i += 2) {
		const unsigned char *f, *v;
		size_t fl, vl;

		if (hrt_bulk(&t, &f, &fl) != 1 || hrt_bulk(&t, &v, &vl) != 1) {
			return errk(p, PERFD_E_PROTOCOL, "bad reply");
		}
		if (cb && cb(ctx, (const char *)f, fl, v, vl))
			break;
	}
	return (int)(cnt / 2);
}

int perfd_rl_hit(perfd_t *p, const char *col, const char *key,
		long long window_ms, long long limit, long long *count)
{
	char *r = NULL;
	size_t n = 0;
	long long cnt;
	int al;

	if (window_ms < 1 || window_ms > 86400000LL)
		return errk(p, PERFD_E_ARG, "window_ms: 1..86400000");
	if (limit < 0 || limit > 5999)
		return errk(p, PERFD_E_ARG, "limit: 0 (none) .. 5999");
	/* ADD's frame: window in by, limit in ttl */
	if (bin_roundtrip(p, BV_RLHIT, col, key, NULL, 0, window_ms, limit,
	        &r, &n) != 0)
		return -1;
	if (!r || n < 9) {
		
		return errk(p, PERFD_E_PROTOCOL, "bad reply");
	}
	cnt = (long long)rle64(r);
	al = r[8] == 1;
	
	if (count)
		*count = cnt;
	return al;
}

int perfd_get(perfd_t *p, const char *col, const char *key,
		void **val, size_t *vlen, long long *ttl_out)
{
	char *r = NULL;
	size_t n = 0;

	if (bin_roundtrip(p, BV_GET, col, key, NULL, 0, 0, 0, &r, &n) != 0)
		return -1;
	if (!r || n < 1 || (r[0] == 1 && n < 5)) {
		
		return errk(p, PERFD_E_PROTOCOL, "bad get reply");
	}
	if (r[0] != 1) {
		
		return 0;
	}
	if (val) {
		*val = malloc(n - 5 ? n - 5 : 1);
		if (!*val) {
			
			return errk(p, PERFD_E_NOMEM, "out of memory");
		}
		memcpy(*val, r + 5, n - 5);
	}
	if (vlen)
		*vlen = n - 5;
	if (ttl_out) {
		unsigned int tl = rle32(r + 1);

		*ttl_out = tl ? (long long)tl : -1;
	}
	
	return 1;
}

int perfd_get_buf(perfd_t *p, const char *col, const char *key,
		void *buf, size_t cap, size_t *vlen, long long *ttl_out)
{
	char *r = NULL;
	size_t n = 0;
	int rc;

	if (vlen)
		*vlen = 0;
	if (bin_roundtrip(p, BV_GET, col, key, NULL, 0, 0, 0, &r, &n) != 0)
		return -1;
	if (!r || n < 1 || (r[0] == 1 && n < 5))
		return errk(p, PERFD_E_PROTOCOL, "bad get reply");
	if (r[0] != 1)
		return 0;
	rc = buf_out(r + 5, n - 5, buf, cap, vlen);
	if (rc == 1 && ttl_out) {
		unsigned int tl = rle32(r + 1);

		*ttl_out = tl ? (long long)tl : -1;
	}
	return rc;
}

/* one byte back: 1/0 (del, exists, expire) */
static int bin_byte_verb(perfd_t *p, int verb, const char *col,
		const char *key, long long ttl)
{
	char *r = NULL;
	size_t n = 0;
	int v;

	if (bin_roundtrip(p, verb, col, key, NULL, 0, 0, ttl, &r, &n) != 0)
		return -1;
	if (!r || n < 1) {
		
		return errk(p, PERFD_E_PROTOCOL, "short reply");
	}
	v = (unsigned char)r[0];
	
	return v;
}

int perfd_del(perfd_t *p, const char *col, const char *key)
{
	return bin_byte_verb(p, BV_DEL, col, key, 0);
}

int perfd_exists(perfd_t *p, const char *col, const char *key)
{
	return bin_byte_verb(p, BV_EXISTS, col, key, 0);
}

long long perfd_ttl(perfd_t *p, const char *col, const char *key)
{
	char *r = NULL;
	size_t n = 0;
	long long v;

	if (bin_roundtrip(p, BV_TTL, col, key, NULL, 0, 0, 0, &r, &n) != 0)
		return -1;
	if (!r || n < 8) {
		
		return errk(p, PERFD_E_PROTOCOL, "short reply");
	}
	v = (long long)rle64(r);
	
	return v;
}

int perfd_expire(perfd_t *p, const char *col, const char *key,
		long long ttl)
{
	int v = bin_byte_verb(p, BV_EXPIRE, col, key, ttl);

	return v < 0 ? -1 : (v ? 0 : 1);
}

static int counter(perfd_t *p, int verb, const char *col, const char *key,
		long long by, long long ttl, long long *newval)
{
	char *r = NULL;
	size_t n = 0;

	if (bin_roundtrip(p, verb, col, key, NULL, 0, by, ttl, &r, &n) != 0)
		return -1;
	if (!r || n < 8) {
		
		return errk(p, PERFD_E_PROTOCOL, "short reply");
	}
	if (newval)
		*newval = (long long)rle64(r);
	
	return 0;
}

int perfd_add(perfd_t *p, const char *col, const char *key, long long by,
		long long ttl, long long *newval)
{
	return counter(p, BV_ADD, col, key, by, ttl, newval);
}

int perfd_sub(perfd_t *p, const char *col, const char *key, long long by,
		long long *newval)
{
	return counter(p, BV_SUB, col, key, by, 0, newval);
}

/* mget {col, keys: [..]} -> {values: [{found, value?, ttl?}..]}: the
 * values are bulks, copied exact */
int perfd_mget(perfd_t *p, const char *col, const char *const *keys,
		int nkeys, void **values, size_t *vlens)
{
	struct pc_tw w;
	struct pc_tv v;
	char *r = NULL;
	size_t rn;
	int i, ta, got = 0, rc;

	for (i = 0; i < nkeys; i++) {
		values[i] = NULL;
		vlens[i] = 0;
	}
	pc_tw_init(&w, NULL, 0);
	pc_tw_map(&w);
	pc_tw_key(&w, "col");
	pc_tw_str(&w, col);
	pc_tw_key(&w, "keys");
	pc_tw_arr(&w);
	for (i = 0; i < nkeys; i++)
		pc_tw_str(&w, keys[i]);
	pc_tw_end(&w);
	pc_tw_end(&w);
	if (pc_tw_done(&w) != 0) {
		pc_tw_free(&w);
		return errk(p, PERFD_E_ARG, "request too large");
	}
	rc = roundtrip(p, "mget", w.b, w.n, &r, &rn);
	pc_tw_free(&w);
	if (rc != 0)
		return -1;
	if (tv_root(p, &v, (const unsigned char *)r, rn, "mget") != 0) {
		
		return -1;
	}
	ta = pc_tv_get(&v, 0, "values");
	if (ta < 0 || v.n[ta].type != 'a') {
		tv_put(p, &v);
		
		return errk(p, PERFD_E_PROTOCOL, "bad mget reply");
	}
	for (i = 0; i < nkeys && (unsigned int)i < v.n[ta].len; i++) {
		int e = pc_tv_at(&v, ta, (unsigned int)i);

		if (e < 0 || v.n[e].type != 'm')
			continue;
		if (pc_tv_get_bool(&v, e, "found"))
			(void)tv_bulk_dup(&v, pc_tv_get(&v, e, "value"),
				&values[i], &vlens[i]);
		got++;
	}
	tv_put(p, &v);
	
	if (got != nkeys)
		return errk(p, PERFD_E_PROTOCOL, "mget answered %d of %d", got, nkeys);
	return 0;
}

/* keys {col, match?, limit?} -> {keys: [b..], ..}; the keys come back as
 * bytes, and a C string is what this API promises, so a NUL inside a key
 * ends it there (as it always did) */
int perfd_keys(perfd_t *p, const char *col, const char *match, int limit,
		char ***keys_out)
{
	struct pc_tw w;
	struct pc_tv v;
	char *r = NULL, **out;
	size_t rn;
	unsigned int i;
	int ta, rc, n = 0;

	pc_tw_init(&w, NULL, 0);
	pc_tw_map(&w);
	pc_tw_key(&w, "col");
	pc_tw_str(&w, col);
	if (match) {
		pc_tw_key(&w, "match");
		pc_tw_str(&w, match);
	}
	if (limit > 0) {
		pc_tw_key(&w, "limit");
		pc_tw_i64(&w, limit);
	}
	pc_tw_end(&w);
	if (pc_tw_done(&w) != 0) {
		pc_tw_free(&w);
		return errk(p, PERFD_E_ARG, "request too large");
	}
	rc = roundtrip(p, "keys", w.b, w.n, &r, &rn);
	pc_tw_free(&w);
	if (rc != 0)
		return -1;
	if (tv_root(p, &v, (const unsigned char *)r, rn, "keys") != 0) {
		
		return -1;
	}
	ta = pc_tv_get(&v, 0, "keys");
	if (ta < 0 || v.n[ta].type != 'a') {
		tv_put(p, &v);
		
		return errk(p, PERFD_E_PROTOCOL, "bad keys reply");
	}
	out = calloc(v.n[ta].len ? v.n[ta].len : 1, sizeof *out);
	if (!out) {
		tv_put(p, &v);
		
		return errk(p, PERFD_E_NOMEM, "out of memory");
	}
	for (i = 0; i < v.n[ta].len; i++) {
		int e = pc_tv_at(&v, ta, i);
		void *k;
		size_t kl;

		if (e >= 0 && tv_bulk_dup(&v, e, &k, &kl) == 0)
			out[n++] = k;
	}
	tv_put(p, &v);
	
	*keys_out = out;
	return n;
}

void perfd_free_keys(char **keys, int n)
{
	int i;

	for (i = 0; i < n; i++)
		free(keys[i]);
	free(keys);
}

/* ---- JSON path verbs --------------------------------------------------- */

/* {col, key, path, ...: the caller adds the rest and closes the map */
static void tw_ckp(struct pc_tw *w, const char *col, const char *key,
		const char *path)
{
	tw_ck(w, col, key);
	pc_tw_key(w, "path");
	pc_tw_str(w, path ? path : "$");
}

/* the request tree is whole, or the error is set and the writer freed */
static int tw_whole(perfd_t *p, struct pc_tw *w)
{
	if (pc_tw_done(w) == 0)
		return 0;
	pc_tw_free(w);
	return errk(p, PERFD_E_ARG, "request too large");
}

/* S297: compare-and-set / compare-and-delete (setifeq, setifne, delifeq,
 * delifne).  1 applied, 0 not, -1 error. */
static int cmp_call(perfd_t *p, const char *method, const char *flag,
		const char *col, const char *key, const void *val, size_t vlen,
		long long ttl, const void *cmp, size_t cmplen)
{
	struct pc_tw w;
	char *r = NULL;
	size_t rn;
	int rc, ok = 0;

	if (!col || !key || (!cmp && cmplen) || (!val && vlen))
		return errk(p, PERFD_E_ARG, "bad arguments");
	if (tw_scratch(p, &w, 256 + strlen(col) + strlen(key) + cmplen +
	        vlen) != 0)
		return -1;
	tw_ck(&w, col, key);
	pc_tw_key(&w, "cmp");
	pc_tw_bulk(&w, cmplen ? cmp : "", cmplen);
	if (val) {
		pc_tw_key(&w, "value");
		pc_tw_bulk(&w, vlen ? val : "", vlen);
		if (ttl > 0) {
			pc_tw_key(&w, "ttl");
			pc_tw_i64(&w, ttl);
		}
	}
	pc_tw_end(&w);
	if (tw_whole(p, &w) != 0)
		return -1;
	rc = roundtrip_key(p, method, w.b, w.n, col, key, &r, &rn);
	if (rc != 0)
		return -1;
	if (reply_bool(p, method, r, rn, flag, &ok) != 0)
		return -1;
	return ok ? 1 : 0;
}

int perfd_set_if(perfd_t *p, const char *col, const char *key,
		const void *val, size_t vlen, long long ttl, const void *cmp,
		size_t cmplen, int ne)
{
	return cmp_call(p, ne ? "setifne" : "setifeq", "stored", col, key,
		val ? val : "", vlen, ttl, cmp, cmplen);
}

int perfd_del_if(perfd_t *p, const char *col, const char *key,
		const void *cmp, size_t cmplen, int ne)
{
	return cmp_call(p, ne ? "delifne" : "delifeq", "deleted", col, key,
		NULL, 0, 0, cmp, cmplen);
}

int perfd_jget(perfd_t *p, const char *col, const char *key,
		const char *path, char **frag_out)
{
	struct pc_tw w;
	struct pc_tv v;
	char *r = NULL;
	size_t rn, fl;
	int rc;

	*frag_out = NULL;
	if (tw_scratch(p, &w, 256 + strlen(col) + strlen(key) + (path ? strlen(path) : 0)) != 0)
		return -1;
	tw_ckp(&w, col, key, path);
	pc_tw_end(&w);
	if (tw_whole(p, &w) != 0)
		return -1;
	rc = roundtrip_key(p, "jget", w.b, w.n, col, key, &r, &rn);
	pc_tw_free(&w);
	if (rc != 0)
		return -1;
	if (tv_root(p, &v, (const unsigned char *)r, rn, "jget") != 0) {
		
		return -1;
	}
	if (!pc_tv_get_bool(&v, 0, "found")) {
		tv_put(p, &v);
		
		return 0;
	}
	/* the fragment is one bulk of its JSON text (D3); tv_bulk_dup
	 * terminates it, which is the C string this API promises */
	rc = tv_bulk_dup(&v, pc_tv_get(&v, 0, "value"), (void **)frag_out, &fl);
	tv_put(p, &v);
	
	return rc == 0 ? 1 : errk(p, PERFD_E_PROTOCOL, "bad jget reply");
}

int perfd_jget_buf(perfd_t *p, const char *col, const char *key,
		const char *path, void *buf, size_t cap, size_t *vlen)
{
	struct pc_tw w;
	struct pc_tv v;
	char *r = NULL;
	size_t rn;
	int rc, k;

	if (vlen)
		*vlen = 0;
	if (tw_scratch(p, &w, 256 + strlen(col) + strlen(key) + (path ? strlen(path) : 0)) != 0)
		return -1;
	tw_ckp(&w, col, key, path);
	pc_tw_end(&w);
	if (tw_whole(p, &w) != 0)
		return -1;
	rc = roundtrip_key(p, "jget", w.b, w.n, col, key, &r, &rn);
	pc_tw_free(&w);
	if (rc != 0)
		return -1;
	if (tv_root(p, &v, (const unsigned char *)r, rn, "jget") != 0)
		return -1;
	if (!pc_tv_get_bool(&v, 0, "found")) {
		tv_put(p, &v);
		return 0;
	}
	/* the fragment's JSON text, one bulk (D3) - copied as it stands,
	 * without the terminator perfd_jget adds */
	k = pc_tv_get(&v, 0, "value");
	if (k < 0 || k >= v.nn || v.n[k].type != 'b') {
		tv_put(p, &v);
		return errk(p, PERFD_E_PROTOCOL, "bad jget reply");
	}
	rc = buf_out(v.n[k].p, (size_t)v.n[k].len, buf, cap, vlen);
	tv_put(p, &v);
	return rc;
}

int perfd_jset(perfd_t *p, const char *col, const char *key,
		const char *path, const char *json_val, long long ttl)
{
	struct pc_tw w;
	char *r = NULL;
	size_t rn;
	int rc, ok = 0;

	if (tw_scratch(p, &w, 256 + strlen(col) + strlen(key) + (path ? strlen(path) : 0) + strlen(json_val)) != 0)
		return -1;
	tw_ckp(&w, col, key, path);
	pc_tw_key(&w, "val");
	pc_tw_str(&w, json_val);           /* the document: its text, one bulk (D3) */
	if (ttl > 0) {
		pc_tw_key(&w, "ttl");
		pc_tw_i64(&w, ttl);
	}
	pc_tw_end(&w);
	if (tw_whole(p, &w) != 0)
		return -1;
	rc = roundtrip_key(p, "jset", w.b, w.n, col, key, &r, &rn);
	pc_tw_free(&w);
	if (rc != 0)
		return -1;
	if (reply_bool(p, "jset", r, rn, "set", &ok) != 0)
		return -1;
	return ok ? 0 : errk(p, PERFD_E_REFUSED, "not set");
}

int perfd_jdel(perfd_t *p, const char *col, const char *key,
		const char *path)
{
	struct pc_tw w;
	char *r = NULL;
	size_t rn;
	int rc, v = 0;

	if (tw_scratch(p, &w, 256 + strlen(col) + strlen(key) + (path ? strlen(path) : 0)) != 0)
		return -1;
	tw_ckp(&w, col, key, path);
	pc_tw_end(&w);
	if (tw_whole(p, &w) != 0)
		return -1;
	rc = roundtrip_key(p, "jdel", w.b, w.n, col, key, &r, &rn);
	pc_tw_free(&w);
	if (rc != 0)
		return -1;
	if (reply_bool(p, "jdel", r, rn, "deleted", &v) != 0)
		return -1;
	return v;
}

int perfd_jincr(perfd_t *p, const char *col, const char *key,
		const char *path, long long by, long long *newval)
{
	struct pc_tw w;
	char *r = NULL;
	size_t rn;
	long long v;
	int rc;

	if (tw_scratch(p, &w, 256 + strlen(col) + strlen(key) + (path ? strlen(path) : 0)) != 0)
		return -1;
	tw_ckp(&w, col, key, path);
	pc_tw_key(&w, "by");
	pc_tw_i64(&w, by);
	pc_tw_end(&w);
	if (tw_whole(p, &w) != 0)
		return -1;
	rc = roundtrip_key(p, "jincr", w.b, w.n, col, key, &r, &rn);
	pc_tw_free(&w);
	if (rc != 0)
		return -1;
	if (reply_int(p, "jincr", r, rn, "value", &v) != 0)
		return -1;
	if (newval)
		*newval = v;
	return 0;
}

/* ---- the admin verbs (0.6.6, DESIGN 12no) ---------------------------------
 * Thin: the params as JSON text, the reply as JSON text - perfd_command's
 * edge.  The collection name and the secret are escaped (a name is
 * limited by the daemon, but a secret is any bytes). */
static char *admin_call(perfd_t *p, const char *method, const char *col,
		const char *flag, const char *secret)
{
	char buf[1024];
	struct pc_jw w;
	int any = 0;

	pc_jw_init(&w, buf, sizeof buf);
	pc_jw_lit(&w, "{");
	if (col) {
		pc_jw_lit(&w, "\"col\":");
		pc_jw_str(&w, col, strlen(col));
		any = 1;
	}
	if (flag) {
		pc_jw_lit(&w, any ? ",\"" : "\"");
		pc_jw_lit(&w, flag);
		pc_jw_lit(&w, "\":true");
		any = 1;
	}
	if (secret) {
		pc_jw_lit(&w, any ? ",\"secret\":" : "\"secret\":");
		pc_jw_str(&w, secret, strlen(secret));
		any = 1;
	}
	pc_jw_lit(&w, "}");
	if (w.overflow || w.len >= sizeof buf) {
		errk(p, PERFD_E_PROTOCOL, "%s: the request does not fit", method);
		return NULL;
	}
	buf[w.len] = 0;
	return perfd_command(p, method, any ? buf : NULL);
}

/* 0.7.0 (S420): the handle's name, sent once per connection it uses - a
 * refusal (an older daemon without the verb) is not reported: the name is
 * a courtesy, not a capability */
static void name_replay(perfd_t *p)
{
	char buf[256];
	struct pc_jw w;
	char *r;

	if (!p->name_due || p->in_replay || !p->conn_name)
		return;
	p->name_due = 0;
	pc_jw_init(&w, buf, sizeof buf);
	pc_jw_lit(&w, "{\"name\":");
	pc_jw_str(&w, p->conn_name, strlen(p->conn_name));
	pc_jw_lit(&w, "}");
	if (w.overflow || w.len >= sizeof buf)
		return;
	buf[w.len] = 0;
	p->in_replay = 1;
	r = perfd_command(p, "name", buf);
	free(r);
	p->in_replay = 0;
}

char *perfd_stats(perfd_t *p, const char *col)
{
	return admin_call(p, "stats", col, NULL, NULL);
}

char *perfd_members(perfd_t *p)
{
	return admin_call(p, "members", NULL, NULL, NULL);
}

char *perfd_clients(perfd_t *p, int cap)
{
	char ps[40];

	if (cap <= 0)
		return perfd_command(p, "clients", NULL);
	snprintf(ps, sizeof ps, "{\"cap\":%d}", cap);
	return perfd_command(p, "clients", ps);
}

int perfd_enable(perfd_t *p, const char *secret)
{
	char *r, *keep;

	if (!p || !secret)
		return errk(p, PERFD_E_PROTOCOL, "enable: no secret");
	r = admin_call(p, "enable", NULL, NULL, secret);
	if (!r)
		return -1;
	free(r);
	keep = strdup(secret);
	if (keep) {
		free(p->enable_sec);
		p->enable_sec = keep;
	}
	p->enable_due = 0;
	return 0;
}

char *perfd_upfill(perfd_t *p, const char *col, int what)
{
	return admin_call(p, "upfill", col, what == PERFD_SWEEP_CHECK ? "check" :
		what == PERFD_SWEEP_STOP ? "stop" : NULL, NULL);
}

char *perfd_updrain(perfd_t *p, const char *col, int what)
{
	return admin_call(p, "updrain", col,
		what == PERFD_SWEEP_STOP ? "stop" : NULL, NULL);
}

/* 0.7.0 (S408 part 2): the operations verbs.  ops_call builds {"col":…,
 * <ikey>:N, <skey>:"…", <bkey>:true} from what is given - the four shapes
 * these verbs take - and sends it; a verb with nothing to say sends no
 * params. */
static char *ops_call(perfd_t *p, const char *method, const char *col,
		const char *ikey, long long ival, const char *skey, const char *sval,
		const char *bkey)
{
	char buf[1024];
	struct pc_jw w;
	int any = 0;

	pc_jw_init(&w, buf, sizeof buf);
	pc_jw_lit(&w, "{");
	if (col) {
		pc_jw_lit(&w, "\"col\":");
		pc_jw_str(&w, col, strlen(col));
		any = 1;
	}
	if (ikey) {
		pc_jw_lit(&w, any ? ",\"" : "\"");
		pc_jw_lit(&w, ikey);
		pc_jw_lit(&w, "\":");
		pc_jw_i64(&w, ival);
		any = 1;
	}
	if (skey) {
		pc_jw_lit(&w, any ? ",\"" : "\"");
		pc_jw_lit(&w, skey);
		pc_jw_lit(&w, "\":");
		pc_jw_str(&w, sval, strlen(sval));
		any = 1;
	}
	if (bkey) {
		pc_jw_lit(&w, any ? ",\"" : "\"");
		pc_jw_lit(&w, bkey);
		pc_jw_lit(&w, "\":true");
		any = 1;
	}
	pc_jw_lit(&w, "}");
	if (w.overflow || w.len >= sizeof buf) {
		errk(p, PERFD_E_PROTOCOL, "%s: the request does not fit", method);
		return NULL;
	}
	buf[w.len] = 0;
	return perfd_command(p, method, any ? buf : NULL);
}

char *perfd_collections(perfd_t *p)
{
	return ops_call(p, "collections", NULL, NULL, 0, NULL, NULL, NULL);
}

char *perfd_create(perfd_t *p, const char *col, int buckets_log2)
{
	return ops_call(p, "create", col, buckets_log2 > 0 ? "buckets_log2" : NULL,
		buckets_log2, NULL, NULL, NULL);
}

char *perfd_drop(perfd_t *p, const char *col, int force)
{
	return ops_call(p, "drop", col, NULL, 0, NULL, NULL, force ? "force" : NULL);
}

char *perfd_resize(perfd_t *p, const char *col, int buckets_log2)
{
	return ops_call(p, "resize", col, "buckets_log2", buckets_log2, NULL, NULL,
		NULL);
}

char *perfd_rename(perfd_t *p, const char *col, const char *to)
{
	return ops_call(p, "rename", col, NULL, 0, "to", to ? to : "", NULL);
}

char *perfd_reset_stats(perfd_t *p)
{
	return ops_call(p, "reset_stats", NULL, NULL, 0, NULL, NULL, NULL);
}

char *perfd_reset_stats_fleet(perfd_t *p)
{
	return ops_call(p, "reset_stats", NULL, NULL, 0, NULL, NULL, "fleet");
}

char *perfd_save(perfd_t *p)
{
	return ops_call(p, "save", NULL, NULL, 0, NULL, NULL, NULL);
}

char *perfd_sync(perfd_t *p, int timeout_ms)
{
	return ops_call(p, "sync", NULL, timeout_ms > 0 ? "timeout_ms" : NULL,
		timeout_ms, NULL, NULL, NULL);
}

char *perfd_load(perfd_t *p)
{
	return ops_call(p, "load", NULL, NULL, 0, NULL, NULL, NULL);
}

char *perfd_mailtest(perfd_t *p)
{
	return ops_call(p, "mailtest", NULL, NULL, 0, NULL, NULL, NULL);
}

char *perfd_upgrade_status(perfd_t *p)
{
	return ops_call(p, "upgrade_status", NULL, NULL, 0, NULL, NULL, NULL);
}

char *perfd_upgrade_revert(perfd_t *p, int force)
{
	return admin_call(p, "upgrade_revert", NULL, force ? "force" : NULL, NULL);
}

char *perfd_upgrade_switch(perfd_t *p, const char *sha256, int force)
{
	return ops_call(p, "upgrade_switch", NULL, NULL, 0, "sha256", sha256,
		force ? "force" : NULL);
}

char *perfd_cluster_epoch_commit(perfd_t *p, int epoch)       /* 12nu */
{
	return ops_call(p, "cluster_epoch_commit", NULL, "epoch", epoch, NULL, NULL,
		NULL);
}

char *perfd_fleetstop(perfd_t *p, int timeout_ms)
{
	return ops_call(p, "fleetstop", NULL, timeout_ms > 0 ? "timeout_ms" : NULL,
		timeout_ms, NULL, NULL, NULL);
}

/* 0.7.0 (S411 part 1): the staging of a release artifact - perfcli's
 * upgrade-stage loop, in the library so the upgrade leader and the
 * dashboard stage builds the same way. */
static int json_str_field(const char *res, const char *key, char *out,
		size_t cap)
{
	char pat[64];
	const char *s, *e;
	size_t n;

	snprintf(pat, sizeof pat, "\"%s\":\"", key);
	s = res ? strstr(res, pat) : NULL;
	if (!s)
		return -1;
	s += strlen(pat);
	e = strchr(s, '"');
	if (!e)
		return -1;
	n = (size_t)(e - s);
	if (n >= cap)
		return -1;
	memcpy(out, s, n);
	out[n] = 0;
	return 0;
}

char *perfd_upgrade_stage(perfd_t *p, const void *data, size_t size,
		const char *version, const unsigned char *sig,
		perfd_progress_cb progress, void *ctx)
{
	const size_t CH = 192 * 1024;
	const size_t b64l = sodium_base64_ENCODED_LEN(CH,
		sodium_base64_VARIANT_ORIGINAL);
	unsigned char h[crypto_hash_sha256_BYTES];
	char sha[2 * crypto_hash_sha256_BYTES + 1], sx[129], id[64];
	char begin[512], *req = NULL, *res;
	size_t off = 0;

	if (!version || !*version || strlen(version) > 47 ||
	        strspn(version, "0123456789abcdefghijklmnopqrstuvwxyz"
	        "ABCDEFGHIJKLMNOPQRSTUVWXYZ.+_-") != strlen(version)) {
		errk(p, PERFD_E_ARG, "upgrade_stage: the version is [0-9A-Za-z.+_-], "
			"at most 47");
		return NULL;
	}
	if (!data || size == 0 || size > ((size_t)1 << 30)) {
		errk(p, PERFD_E_ARG, "upgrade_stage: 1 byte to 1 GB");
		return NULL;
	}
	crypto_hash_sha256(h, data, size);
	sodium_bin2hex(sha, sizeof sha, h, sizeof h);
	if (sig)
		sodium_bin2hex(sx, sizeof sx, sig, 64);
	snprintf(begin, sizeof begin, "{\"op\":\"begin\",\"size\":%zu,\"sha256\":"
		"\"%s\",\"version\":\"%s\"%s%s%s}", size, sha, version,
		sig ? ",\"sig\":\"" : "", sig ? sx : "", sig ? "\"" : "");
	res = perfd_command(p, "upgrade_stage", begin);
	if (!res)
		return NULL;
	if (json_str_field(res, "id", id, sizeof id) != 0) {
		perfd_free_value(res);
		errk(p, PERFD_E_PROTOCOL, "upgrade_stage: begin answered without an id");
		return NULL;
	}
	perfd_free_value(res);
	req = malloc(b64l + 160);
	if (!req) {
		errk(p, PERFD_E_NOMEM, "upgrade_stage: out of memory");
		return NULL;
	}
	while (off < size) {
		size_t n = size - off < CH ? size - off : CH;
		int w = snprintf(req, 160, "{\"op\":\"chunk\",\"id\":\"%s\","
			"\"off\":%zu,\"data\":\"", id, off);

		sodium_bin2base64(req + w, b64l, (const unsigned char *)data + off, n,
			sodium_base64_VARIANT_ORIGINAL);
		strcat(req, "\"}");
		res = perfd_command(p, "upgrade_stage", req);
		if (!res) {
			char keep[256];

			snprintf(keep, sizeof keep, "%s", perfd_error(p));
			snprintf(req, 160, "{\"op\":\"abort\",\"id\":\"%s\"}", id);
			perfd_free_value(perfd_command(p, "upgrade_stage", req));
			errk(p, PERFD_E_SERVER, "upgrade_stage: chunk at %zu: %s", off,
				keep);
			free(req);
			return NULL;
		}
		perfd_free_value(res);
		off += n;
		if (progress)
			progress(ctx, off, size);
	}
	snprintf(req, 160, "{\"op\":\"end\",\"id\":\"%s\"}", id);
	res = perfd_command(p, "upgrade_stage", req);
	free(req);
	return res;
}

char *perfd_upgrade_stage_file(perfd_t *p, const char *path,
		const char *version, perfd_progress_cb progress, void *ctx,
		int *signed_out)
{
	FILE *f;
	long sz;
	unsigned char *data, sig[64];
	char sp[1100], b[140];
	int have_sig = 0;
	size_t n;
	char *res;

	if (signed_out)
		*signed_out = 0;
	f = path ? fopen(path, "rb") : NULL;
	if (!f || fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) <= 0 ||
	        sz > (1L << 30) || fseek(f, 0, SEEK_SET) != 0) {
		if (f)
			fclose(f);
		errk(p, PERFD_E_ARG, "upgrade_stage: %s: not a readable file of 1 "
			"byte to 1 GB", path ? path : "(null)");
		return NULL;
	}
	data = malloc((size_t)sz);
	if (!data || fread(data, 1, (size_t)sz, f) != (size_t)sz) {
		fclose(f);
		free(data);
		errk(p, PERFD_E_ARG, "upgrade_stage: cannot read %s", path);
		return NULL;
	}
	fclose(f);
	snprintf(sp, sizeof sp, "%s.sig", path);
	if ((f = fopen(sp, "rb"))) {
		n = fread(b, 1, sizeof b, f);
		fclose(f);
		if (n == 64) {
			memcpy(sig, b, 64);
			have_sig = 1;
		} else {
			size_t bl = 0;

			while (n && (b[n - 1] == '\n' || b[n - 1] == '\r'))
				n--;
			if (n == 128 && sodium_hex2bin(sig, sizeof sig, b, 128, NULL,
			        &bl, NULL) == 0 && bl == 64)
				have_sig = 1;
		}
		if (!have_sig) {
			free(data);
			errk(p, PERFD_E_ARG, "upgrade_stage: %s is neither 64 bytes nor "
				"128 hex characters", sp);
			return NULL;
		}
	}
	if (signed_out)
		*signed_out = have_sig;
	res = perfd_upgrade_stage(p, data, (size_t)sz, version,
		have_sig ? sig : NULL, progress, ctx);
	free(data);
	return res;
}
