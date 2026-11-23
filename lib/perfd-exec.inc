/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov
 * Part of libperfd - see lib/LICENSE.  This file must stay
 * free of src/core includes; tools/sync-libperfd.sh exports
 * exactly the MIT set to consumers. */
/* perfd-exec.inc - part of perfd.c, #included in order (one translation unit): perfd_exec, the RESP
 * door's keyed data commands mapped onto this library's verbs, each reply shaped as that door's. */
#include <limits.h>
#include <strings.h>

#define EXEC_TTL_MAX 0x7FFFFFFFLL     /* the daemon's PC_TTL_MAX */
#define EXEC_JPATH   512              /* the daemon's JP_NAME_PARAM */

struct exec_args {
	int argc;
	const char *const *argv;
	const size_t *len;
};

static perfd_reply *rep_mk(int type)
{
	perfd_reply *r = calloc(1, sizeof *r);

	if (r)
		r->type = type;
	return r;
}

static perfd_reply *rep_str(int type, const char *s, size_t n)
{
	perfd_reply *r = rep_mk(type);

	if (!r)
		return NULL;
	r->str = malloc(n + 1);
	if (!r->str) {
		free(r);
		return NULL;
	}
	memcpy(r->str, s, n);
	r->str[n] = 0;
	r->len = n;
	return r;
}

static perfd_reply *rep_int(long long v)
{
	perfd_reply *r = rep_mk(PERFD_REPLY_INTEGER);

	if (r)
		r->integer = v;
	return r;
}

static perfd_reply *rep_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static perfd_reply *rep_err(const char *fmt, ...)
{
	char b[300];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	if (n < 0)
		n = 0;
	if ((size_t)n >= sizeof b)
		n = (int)sizeof b - 1;
	return rep_str(PERFD_REPLY_ERROR, b, (size_t)n);
}

static perfd_reply *rep_arr(size_t n)
{
	perfd_reply *r = rep_mk(PERFD_REPLY_ARRAY);

	if (!r)
		return NULL;
	r->element = calloc(n ? n : 1, sizeof *r->element);
	if (!r->element) {
		free(r);
		return NULL;
	}
	r->elements = n;
	return r;
}

void perfd_reply_free(perfd_reply *r)
{
	size_t i;

	if (!r)
		return;
	for (i = 0; i < r->elements && r->element; i++)
		perfd_reply_free(r->element[i]);
	free(r->element);
	free(r->str);
	free(r);
}

/* a call that failed: the daemon's words are an ERROR reply, spelled as the RESP door spells them;
 * a transport, protocol or memory failure is NULL with perfd_error set */
static perfd_reply *exec_fail(perfd_t *p)
{
	const char *e = p->err;
	size_t i = 0;

	if (p->err_kind != PERFD_E_SERVER && p->err_kind != PERFD_E_REFUSED &&
	    p->err_kind != PERFD_E_ARG)
		return NULL;
	if (!strcmp(e, "Existing key has wrong Redis type"))
		return rep_str(PERFD_REPLY_ERROR, e, strlen(e));
	if (!strcmp(e, "stored value is not JSON"))
		return rep_err("WRONGTYPE the value at this key is not a JSON document");
	while (e[i] >= 'A' && e[i] <= 'Z')
		i++;
	if (i >= 2 && e[i] == ' ')         /* a code word of its own: WRONGTYPE, ERR, ... */
		return rep_str(PERFD_REPLY_ERROR, e, strlen(e));
	return rep_err("ERR %s", e);
}

/* Redis's integer argument: a sign or a digit first, the whole span a long long */
static int exec_ll(const char *s, size_t n, long long *out)
{
	char num[24], *end;
	long long v;

	if (!n || n >= sizeof num || (s[0] != '-' && (s[0] < '0' || s[0] > '9')))
		return -1;
	memcpy(num, s, n);
	num[n] = 0;
	errno = 0;
	v = strtoll(num, &end, 10);
	if (end != num + n || errno == ERANGE)
		return -1;
	*out = v;
	return 0;
}

static long long ms_to_s_lib(long long ms)
{
	return ms > 0 ? ms / 1000 + (ms % 1000 != 0) : ms / 1000;
}

static int exec_is(const struct exec_args *a, int i, const char *w)
{
	size_t n = strlen(w);

	return a->len[i] == n && !strncasecmp(a->argv[i], w, n);
}

/* argument @i as a C string in @buf (@cap with its NUL): 0, -1 too long or holding a NUL */
static int exec_cstr(const struct exec_args *a, int i, char *buf, size_t cap)
{
	if (a->len[i] >= cap || memchr(a->argv[i], 0, a->len[i]))
		return -1;
	memcpy(buf, a->argv[i], a->len[i]);
	buf[a->len[i]] = 0;
	return 0;
}

/* a key the door would take; NULL = usable, else the reply that refuses it */
static perfd_reply *exec_key(const struct exec_args *a, int i, char *kb)
{
	if (a->len[i] == 0 || a->len[i] > KEY_MAX)
		return rep_err("ERR key too long");
	if (exec_cstr(a, i, kb, KEY_MAX + 1) != 0)
		return rep_err("ERR a key holding a NUL byte is not supported by perfd_exec");
	return NULL;
}

static int exec_key_ok(const struct exec_args *a, int i)
{
	return a->len[i] && a->len[i] <= KEY_MAX && !memchr(a->argv[i], 0, a->len[i]);
}

static perfd_reply *x_get(perfd_t *p, const char *col, const struct exec_args *a)
{
	char kb[KEY_MAX + 1];
	perfd_reply *r;
	void *v = NULL;
	size_t vl = 0;
	int rc;

	if (a->argc != 2)
		return rep_err("ERR wrong number of arguments for 'get' command");
	if ((r = exec_key(a, 1, kb)))
		return r;
	rc = perfd_get(p, col, kb, &v, &vl, NULL);
	if (rc < 0)
		return exec_fail(p);
	if (rc == 0)
		return rep_mk(PERFD_REPLY_NIL);
	r = rep_str(PERFD_REPLY_STRING, v, vl);
	free(v);
	return r;
}

static perfd_reply *x_set(perfd_t *p, const char *col, const struct exec_args *a)
{
	int setex = exec_is(a, 0, "SETEX"), psetex = exec_is(a, 0, "PSETEX");
	int vi = setex || psetex ? 3 : 2, i, cond = 0, ckind = 0, rc;
	const char *cmp = NULL;
	size_t cmplen = 0;
	long long ttl = 0;
	char kb[KEY_MAX + 1];
	perfd_reply *r;

	if (setex || psetex) {
		if (a->argc != 4)
			return rep_err("ERR wrong number of arguments");
		if (exec_ll(a->argv[2], a->len[2], &ttl) < 0 || ttl <= 0 ||
		    (psetex ? ms_to_s_lib(ttl) : ttl) > EXEC_TTL_MAX)
			return rep_err("ERR invalid expire time");
		if (psetex)
			ttl = ms_to_s_lib(ttl);
	} else {
		if (a->argc < 3)
			return rep_err("ERR wrong number of arguments for 'set' command");
		for (i = 3; i < a->argc; i++) {
			if (exec_is(a, i, "EX") || exec_is(a, i, "PX")) {
				int px = exec_is(a, i, "PX");

				if (i + 1 >= a->argc || exec_ll(a->argv[i + 1], a->len[i + 1], &ttl) < 0 ||
				    ttl <= 0 || (px ? ms_to_s_lib(ttl) : ttl) > EXEC_TTL_MAX)
					return rep_err("ERR invalid expire time in 'set' command");
				if (px)
					ttl = ms_to_s_lib(ttl);
				i++;
			} else if (exec_is(a, i, "NX") || exec_is(a, i, "XX")) {
				int want = exec_is(a, i, "NX") ? PERFD_NX : PERFD_XX;

				if ((cond && cond != want) || ckind)
					return rep_err("ERR syntax error");
				cond = want;
			} else if (exec_is(a, i, "IFEQ") || exec_is(a, i, "IFNE")) {
				if (cond || ckind || i + 1 >= a->argc)
					return rep_err("ERR syntax error");
				ckind = exec_is(a, i, "IFEQ") ? 1 : 2;
				cmp = a->argv[i + 1];
				cmplen = a->len[i + 1];
				i++;
			} else if (exec_is(a, i, "IFDEQ") || exec_is(a, i, "IFDNE")) {
				return rep_err("ERR IFDEQ/IFDNE are not supported: perfcached has no "
					"DIGEST - compare the value with IFEQ/IFNE");
			} else {
				return rep_err("ERR unsupported SET option");
			}
		}
	}
	if ((r = exec_key(a, 1, kb)))
		return r;
	if (ckind)
		rc = perfd_set_if(p, col, kb, a->argv[vi], a->len[vi], ttl, cmp, cmplen, ckind == 2);
	else if (cond)
		rc = perfd_set_cond(p, col, kb, a->argv[vi], a->len[vi], ttl, cond);
	else
		rc = perfd_set(p, col, kb, a->argv[vi], a->len[vi], ttl) == 0 ? 1 : -1;
	if (rc < 0)
		return exec_fail(p);
	return rc ? rep_str(PERFD_REPLY_STATUS, "OK", 2) : rep_mk(PERFD_REPLY_NIL);
}

/* DEL and EXISTS over every key, each on its own node; one key is refused as the door refuses it */
static perfd_reply *x_count(perfd_t *p, const char *col, const struct exec_args *a, int del)
{
	char kb[KEY_MAX + 1];
	perfd_reply *r;
	long long n = 0;
	int i, rc;

	if (a->argc < 2)
		return rep_err(del ? "ERR wrong number of arguments for 'del' command" :
			"ERR wrong number of arguments for 'exists' command");
	if (del && a->argc == 2 && (r = exec_key(a, 1, kb)))
		return r;
	for (i = 1; i < a->argc; i++) {
		if (!exec_key_ok(a, i))
			continue;
		exec_cstr(a, i, kb, sizeof kb);
		rc = del ? perfd_del(p, col, kb) : perfd_exists(p, col, kb);
		if (rc < 0)
			return exec_fail(p);
		n += rc;
	}
	return rep_int(n);
}

static perfd_reply *x_expire(perfd_t *p, const char *col, const struct exec_args *a)
{
	char kb[KEY_MAX + 1];
	long long ttl;
	int rc;

	if (a->argc != 3 || !a->len[1] || a->len[1] > KEY_MAX ||
	    exec_ll(a->argv[2], a->len[2], &ttl) < 0)
		return rep_err("ERR wrong number of arguments for 'expire' command");
	if (exec_cstr(a, 1, kb, sizeof kb) != 0)
		return rep_err("ERR a key holding a NUL byte is not supported by perfd_exec");
	if (exec_is(a, 0, "PEXPIRE"))
		ttl = ms_to_s_lib(ttl);
	if (ttl > EXEC_TTL_MAX)
		return rep_err("ERR invalid expire time in 'expire' command");
	if (ttl <= 0) {                    /* Redis deletes on a non-positive expire */
		rc = perfd_del(p, col, kb);
		return rc < 0 ? exec_fail(p) : rep_int(rc);
	}
	rc = perfd_expire(p, col, kb, ttl);
	return rc < 0 ? exec_fail(p) : rep_int(rc == 0);
}

static perfd_reply *x_ttl(perfd_t *p, const char *col, const struct exec_args *a)
{
	char kb[KEY_MAX + 1];
	long long v;

	if (a->argc != 2 || !a->len[1] || a->len[1] > KEY_MAX)
		return rep_err("ERR wrong number of arguments for 'ttl' command");
	if (exec_cstr(a, 1, kb, sizeof kb) != 0)
		return rep_err("ERR a key holding a NUL byte is not supported by perfd_exec");
	v = perfd_ttl(p, col, kb);
	if (v == -1 && p->err_kind != PERFD_E_NONE)
		return exec_fail(p);
	return rep_int(v > 0 && exec_is(a, 0, "PTTL") ? v * 1000 : v);
}

static perfd_reply *x_incr(perfd_t *p, const char *col, const struct exec_args *a)
{
	int by_given = exec_is(a, 0, "INCRBY") || exec_is(a, 0, "DECRBY");
	int neg = exec_is(a, 0, "DECR") || exec_is(a, 0, "DECRBY");
	char kb[KEY_MAX + 1];
	long long by = 1, nv = 0;
	perfd_reply *r;

	if (by_given && (a->argc != 3 || exec_ll(a->argv[2], a->len[2], &by) < 0))
		return rep_err("ERR value is not an integer or out of range");
	if (!by_given && a->argc != 2)
		return rep_err("ERR wrong number of arguments");
	if ((r = exec_key(a, 1, kb)))
		return r;
	if (neg && by == LLONG_MIN)
		return rep_err("ERR value is not an integer or out of range");
	/* ttl -1: the expiry kept, as INCR keeps it */
	if (perfd_add(p, col, kb, neg ? -by : by, -1, &nv) != 0)
		return exec_fail(p);
	return rep_int(nv);
}

static perfd_reply *x_mget(perfd_t *p, const char *col, const struct exec_args *a)
{
	int n = a->argc - 1, i, k = 0, unans = 0, *st = NULL, *map = NULL;
	const char **keys = NULL;
	void **vals = NULL;
	size_t *vls = NULL, tot = 0;
	char *kbuf = NULL, *o;
	perfd_reply *r = NULL;

	if (a->argc < 2)
		return rep_err("ERR wrong number of arguments for 'mget' command");
	for (i = 1; i < a->argc; i++)
		tot += a->len[i] + 1;
	keys = calloc((size_t)n, sizeof *keys);
	vals = calloc((size_t)n, sizeof *vals);
	vls = calloc((size_t)n, sizeof *vls);
	st = calloc((size_t)n, sizeof *st);
	map = calloc((size_t)n, sizeof *map);
	kbuf = malloc(tot);
	if (!keys || !vals || !vls || !st || !map || !kbuf) {
		errk(p, PERFD_E_NOMEM, "out of memory");
		goto out;
	}
	for (i = 1, o = kbuf; i < a->argc; i++) {
		map[i - 1] = -1;               /* a key the door would not take: nil */
		if (!exec_key_ok(a, i))
			continue;
		memcpy(o, a->argv[i], a->len[i]);
		o[a->len[i]] = 0;
		keys[k] = o;
		map[i - 1] = k++;
		o += a->len[i] + 1;
	}
	if (k && perfd_mget_ex(p, col, keys, k, vals, vls, st) != 0) {
		r = exec_fail(p);
		goto out;
	}
	for (i = 0; i < k; i++)
		unans += st[i] == PERFD_MGET_UNANSWERED;
	if (unans) {
		r = rep_err("TRYAGAIN mget: %d key(s) not answered", unans);
		goto out;
	}
	r = rep_arr((size_t)n);
	for (i = 0; r && i < n; i++) {
		int j = map[i];

		r->element[i] = j >= 0 && st[j] == PERFD_MGET_FOUND ?
			rep_str(PERFD_REPLY_STRING, vals[j], vls[j]) : rep_mk(PERFD_REPLY_NIL);
		if (!r->element[i]) {
			perfd_reply_free(r);
			r = NULL;
			errk(p, PERFD_E_NOMEM, "out of memory");
		}
	}
out:
	for (i = 0; vals && i < k; i++)
		free(vals[i]);
	free(keys);
	free(vals);
	free(vls);
	free(st);
	free(map);
	free(kbuf);
	return r;
}

/* MSET as a set per key, each on its key's node: OK once every one is stored */
static perfd_reply *x_mset(perfd_t *p, const char *col, const struct exec_args *a)
{
	char kb[KEY_MAX + 1];
	perfd_reply *r;
	int i;

	if (a->argc < 3 || a->argc % 2 == 0)
		return rep_err("ERR wrong number of arguments for 'mset' command");
	for (i = 1; i < a->argc; i += 2)
		if ((r = exec_key(a, i, kb)))
			return r;
	for (i = 1; i < a->argc; i += 2) {
		exec_cstr(a, i, kb, sizeof kb);
		if (perfd_set(p, col, kb, a->argv[i + 1], a->len[i + 1], 0) != 0)
			return exec_fail(p);
	}
	return rep_str(PERFD_REPLY_STATUS, "OK", 2);
}

/* the H commands the daemon runs (its hcmds table): argc bounds with name and key, -1 = any */
static const struct {
	const char *name, *lower;
	int min, max;
} exec_hcmds[] = {
	{ "HSET", "hset", 4, -1 }, { "HSETNX", "hsetnx", 4, 4 },
	{ "HMSET", "hmset", 4, -1 }, { "HGET", "hget", 3, 3 },
	{ "HMGET", "hmget", 3, -1 }, { "HDEL", "hdel", 3, -1 },
	{ "HEXISTS", "hexists", 3, 3 }, { "HLEN", "hlen", 2, 2 },
	{ "HSTRLEN", "hstrlen", 3, 3 }, { "HKEYS", "hkeys", 2, 2 },
	{ "HVALS", "hvals", 2, 2 }, { "HGETALL", "hgetall", 2, 2 },
	{ "HINCRBY", "hincrby", 4, 4 }, { "HINCRBYFLOAT", "hincrbyfloat", 4, 4 },
	{ "HRANDFIELD", "hrandfield", 2, 4 }, { "HSCAN", "hscan", 3, -1 },
};

/* one reply tree item as the RESP door renders it (ptree.c one_resp); NULL with *@used 0 on a
 * malformed tree, or NULL with @p's error on no memory */
static perfd_reply *exec_tree(perfd_t *p, const unsigned char *b, size_t n, size_t *used, int depth)
{
	perfd_reply *r;
	size_t len, cnt, i, o;

	*used = 0;
	if (!n || depth > 32)
		return NULL;
	switch (b[0]) {
	case 'i':
		if (n < 9)
			return NULL;
		*used = 9;
		return rep_int((long long)rle64((const char *)b + 1));
	case 'd':
		if (n < 3 || n - 3 < (len = (size_t)b[1] | (size_t)b[2] << 8))
			return NULL;
		*used = 3 + len;
		return rep_str(PERFD_REPLY_STRING, (const char *)b + 3, len);
	case 'b':
	case 'e':
		if (n < 5 || n - 5 < (len = hrt_u32(b + 1)))
			return NULL;
		*used = 5 + len;
		return rep_str(b[0] == 'e' ? PERFD_REPLY_ERROR : PERFD_REPLY_STRING,
			(const char *)b + 5, len);
	case 'n':
		*used = 1;
		return rep_mk(PERFD_REPLY_NIL);
	case 't':
	case 'f':
		*used = 1;
		return rep_int(b[0] == 't');
	case 'o':
		*used = 1;
		return rep_str(PERFD_REPLY_STATUS, "OK", 2);
	case 'a':
	case 'm':
		if (n < 5)
			return NULL;
		cnt = hrt_u32(b + 1) * (b[0] == 'm' ? 2u : 1u);
		if (cnt > n)
			return NULL;
		r = rep_arr(cnt);
		if (!r)
			return NULL;
		for (i = 0, o = 5; i < cnt; i++) {
			size_t u;

			r->element[i] = exec_tree(p, b + o, n - o, &u, depth + 1);
			if (!r->element[i]) {
				perfd_reply_free(r);
				return NULL;
			}
			o += u;
		}
		*used = o;
		return r;
	default:
		return NULL;
	}
}

static perfd_reply *x_hash(perfd_t *p, const char *col, const struct exec_args *a, int ci)
{
	char kb[KEY_MAX + 1];
	unsigned char *tree = NULL;
	size_t tn = 0, used;
	perfd_reply *r;

	if (a->argc < exec_hcmds[ci].min ||
	    (exec_hcmds[ci].max > 0 && a->argc > exec_hcmds[ci].max) ||
	    ((ci == 0 || ci == 2) && a->argc % 2))
		return rep_err("ERR wrong number of arguments for '%s' command", exec_hcmds[ci].lower);
	if ((r = exec_key(a, 1, kb)))
		return r;
	if (hcall(p, col, kb, exec_hcmds[ci].name, a->argc - 2, a->argv + 2, a->len + 2,
	        &tree, &tn) != 0)
		return exec_fail(p);
	r = exec_tree(p, tree, tn, &used, 0);
	if (!r && !used)
		errk(p, PERFD_E_PROTOCOL, "bad %s reply", exec_hcmds[ci].name);
	return r;
}

/* the JSON path as the door reads it: `$...` v2 (an array-shaped reply), `.`, `.x`, `[..` and
 * `x` legacy (bare); absent = the root.  NULL = ok, else the refusal */
static perfd_reply *exec_jpath(const struct exec_args *a, int has, char *jp, int *v2, int *root)
{
	const char *pp = has ? a->argv[2] : "$";
	size_t pl = has ? a->len[2] : 1, n;

	*v2 = 0;
	if (pl == 0 || pl + 2 >= EXEC_JPATH || memchr(pp, 0, pl))
		return rep_err("ERR bad path");
	if (has && pp[0] == '$') {
		*v2 = 1;
		memcpy(jp, pp, pl);
		n = pl;
	} else if (!has || (pl == 1 && pp[0] == '.')) {
		jp[0] = '$';
		n = 1;
	} else if (pp[0] == '.' || pp[0] == '[') {
		jp[0] = '$';
		memcpy(jp + 1, pp, pl);
		n = pl + 1;
	} else {
		jp[0] = '$';
		jp[1] = '.';
		memcpy(jp + 2, pp, pl);
		n = pl + 2;
	}
	jp[n] = 0;
	*root = n == 1;
	return NULL;
}

static perfd_reply *x_json(perfd_t *p, const char *col, const struct exec_args *a)
{
	int get = exec_is(a, 0, "JSON.GET"), set = exec_is(a, 0, "JSON.SET");
	int v2 = 0, root = 0, i, flags = 0, rc;
	char kb[KEY_MAX + 1], jp[EXEC_JPATH + 2];
	long long ttl = 0;
	perfd_reply *r;

	if (a->argc < 2 || (set && a->argc < 4))
		return rep_err("ERR wrong number of arguments for this JSON command");
	if ((r = exec_key(a, 1, kb)))
		return r;
	if ((r = exec_jpath(a, a->argc >= 3, jp, &v2, &root)))
		return r;
	if (set) {
		char *jv;

		for (i = 4; i < a->argc; i++) {
			if (exec_is(a, i, "NX") || exec_is(a, i, "XX")) {
				int f = exec_is(a, i, "NX") ? PERFD_JSET_NX : PERFD_JSET_XX;

				if (flags && flags != f)
					return rep_err("ERR syntax error");
				flags = f;
			} else if (exec_is(a, i, "EX") && i + 1 < a->argc) {
				if (exec_ll(a->argv[i + 1], a->len[i + 1], &ttl) < 0 ||
				    ttl <= 0 || ttl > EXEC_TTL_MAX)
					return rep_err("ERR invalid expire time in 'json.set' command");
				i++;
			} else {
				return rep_err("ERR unsupported JSON.SET option");
			}
		}
		if (memchr(a->argv[3], 0, a->len[3]))
			return rep_err("ERR new value is not valid JSON");
		jv = malloc(a->len[3] + 1);
		if (!jv) {
			errk(p, PERFD_E_NOMEM, "out of memory");
			return NULL;
		}
		memcpy(jv, a->argv[3], a->len[3]);
		jv[a->len[3]] = 0;
		rc = perfd_jset_ex(p, col, kb, jp, jv, ttl, flags);
		free(jv);
		if (rc < 0)
			return exec_fail(p);
		return rc ? rep_str(PERFD_REPLY_STATUS, "OK", 2) : rep_mk(PERFD_REPLY_NIL);
	}
	if (get) {
		char *frag = NULL;

		rc = perfd_jget(p, col, kb, jp, &frag);
		if (rc < 0)
			return exec_fail(p);
		if (rc == 0)                   /* a missing key or path, as RedisJSON answers it */
			return root ? rep_mk(PERFD_REPLY_NIL) : v2 ? rep_str(PERFD_REPLY_STRING, "[]", 2) :
				rep_err("ERR path does not exist");
		if (v2) {
			size_t fl = strlen(frag);

			r = rep_str(PERFD_REPLY_STRING, "[", 1);
			if (r) {
				char *s = realloc(r->str, fl + 3);

				if (!s) {
					perfd_reply_free(r);
					r = NULL;
				} else {
					memcpy(s + 1, frag, fl);
					s[fl + 1] = ']';
					s[fl + 2] = 0;
					r->str = s;
					r->len = fl + 2;
				}
			}
		} else {
			r = rep_str(PERFD_REPLY_STRING, frag, strlen(frag));
		}
		free(frag);
		return r;
	}
	rc = perfd_jdel(p, col, kb, jp);       /* JSON.DEL; at the root it removes the key */
	return rc < 0 ? exec_fail(p) : rep_int(rc);
}

perfd_reply *perfd_exec(perfd_t *p, const char *col, int argc,
		const char *const *argv, const size_t *argvlen)
{
	size_t lens[64], *own = NULL;
	struct exec_args a;
	perfd_reply *r;
	int i;

	if (!p)
		return NULL;
	if (!col || argc < 1 || !argv) {
		errk(p, PERFD_E_ARG, "perfd_exec: a collection and a command are needed");
		return NULL;
	}
	if (!argvlen) {
		own = argc <= 64 ? lens : malloc((size_t)argc * sizeof *own);
		if (!own) {
			errk(p, PERFD_E_NOMEM, "out of memory");
			return NULL;
		}
		for (i = 0; i < argc; i++)
			own[i] = argv[i] ? strlen(argv[i]) : 0;
	}
	for (i = 0; i < argc; i++)
		if (!argv[i]) {
			if (own != lens)
				free(own);
			errk(p, PERFD_E_ARG, "perfd_exec: argv[%d] is NULL", i);
			return NULL;
		}
	a.argc = argc;
	a.argv = argv;
	a.len = argvlen ? argvlen : own;
	p->err[0] = 0;
	p->err_kind = PERFD_E_NONE;
	if (exec_is(&a, 0, "GET"))
		r = x_get(p, col, &a);
	else if (exec_is(&a, 0, "SET") || exec_is(&a, 0, "SETEX") || exec_is(&a, 0, "PSETEX"))
		r = x_set(p, col, &a);
	else if (exec_is(&a, 0, "DEL") || exec_is(&a, 0, "UNLINK"))
		r = x_count(p, col, &a, 1);
	else if (exec_is(&a, 0, "EXISTS"))
		r = x_count(p, col, &a, 0);
	else if (exec_is(&a, 0, "EXPIRE") || exec_is(&a, 0, "PEXPIRE"))
		r = x_expire(p, col, &a);
	else if (exec_is(&a, 0, "TTL") || exec_is(&a, 0, "PTTL"))
		r = x_ttl(p, col, &a);
	else if (exec_is(&a, 0, "INCR") || exec_is(&a, 0, "INCRBY") ||
	         exec_is(&a, 0, "DECR") || exec_is(&a, 0, "DECRBY"))
		r = x_incr(p, col, &a);
	else if (exec_is(&a, 0, "MGET"))
		r = x_mget(p, col, &a);
	else if (exec_is(&a, 0, "MSET"))
		r = x_mset(p, col, &a);
	else if (exec_is(&a, 0, "JSON.GET") || exec_is(&a, 0, "JSON.SET") || exec_is(&a, 0, "JSON.DEL"))
		r = x_json(p, col, &a);
	else {
		int ci;

		r = NULL;
		for (ci = 0; ci < (int)(sizeof exec_hcmds / sizeof exec_hcmds[0]); ci++)
			if (exec_is(&a, 0, exec_hcmds[ci].name))
				break;
		if (ci < (int)(sizeof exec_hcmds / sizeof exec_hcmds[0]))
			r = x_hash(p, col, &a, ci);
		else
			r = rep_err("ERR unsupported command '%.*s' (perfd_exec maps the keyed data "
				"commands only)", a.len[0] > 64 ? 64 : (int)a.len[0], argv[0]);
	}
	if (!r && p->err_kind == PERFD_E_NONE)
		errk(p, PERFD_E_NOMEM, "out of memory");
	if (own != lens)
		free(own);
	return r;
}
