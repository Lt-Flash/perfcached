/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov - see LICENSE */
/*
 * perfcached.c - the PHP client over libperfd: one class, Perfcached, on the
 * native door.  Values are bytes both ways; keys, collections and fields are
 * C strings to libperfd, so a NUL in one is a ValueError here.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "php.h"
#include "ext/standard/info.h"
#include "ext/json/php_json.h"
#include "ext/spl/spl_exceptions.h"
#include "zend_exceptions.h"
#include "zend_smart_str.h"
#include "php_perfcached.h"
#include <perfd.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PC_MAX_SECRETS 8
#define PC_MAX_SEEDS   16
#define PC_SEED_FIRST_MS 500
/* a whole record fits: PCACHE_CELL_MAX, so a read takes one round trip */
#define PC_READ_BUF    262144

/* PE-2: the handle and what it takes to dial again.  A pooled one outlives its
 * request: one per worker and (seeds, port, options), kept in pc_pool */
typedef struct {
	perfd_t *p;
	pid_t pid;
	perfd_opts o;
	char *unix_path, *name;
	char *seeds[PC_MAX_SEEDS + 1];
	char *secrets[PC_MAX_SECRETS + 1];
	int nseeds, port, pooled;
} pc_conn;

typedef struct {
	pc_conn *c;
	zend_object std;
} pc_obj;

static struct {
	zend_long dials, reused, redials, forks, dirty, prepared, last_dial_ms, seeds_failed;
} pc_st;

static HashTable pc_pool;
static pc_conn *pc_cur;                /* the call in progress: blocking, one thread */
static pc_conn *pc_failed;             /* its transport failed: redial before the next */

static zend_class_entry *pc_ce, *pc_ex, *pc_ex_transport, *pc_ex_auth,
	*pc_ex_refused, *pc_ex_server, *pc_ex_arg;
static zend_object_handlers pc_handlers;
static char pc_buf[PC_READ_BUF];

static inline pc_obj *pc_from(zend_object *o)
{
	return (pc_obj *)((char *)o - XtOffsetOf(pc_obj, std));
}

static zend_object *pc_create(zend_class_entry *ce)
{
	pc_obj *o = zend_object_alloc(sizeof(pc_obj), ce);

	zend_object_std_init(&o->std, ce);
	object_properties_init(&o->std, ce);
	o->std.handlers = &pc_handlers;
	return &o->std;
}

static void pc_conn_free(pc_conn *c)
{
	int i;

	if (pc_failed == c)
		pc_failed = NULL;
	if (pc_cur == c)
		pc_cur = NULL;
	if (c->p)
		perfd_free(c->p);      /* closes this process's fds, writes nothing */
	for (i = 0; c->secrets[i]; i++) {
		ZEND_SECURE_ZERO(c->secrets[i], strlen(c->secrets[i]));
		pefree(c->secrets[i], 1);
	}
	for (i = 0; c->seeds[i]; i++)
		pefree(c->seeds[i], 1);
	if (c->unix_path)
		pefree(c->unix_path, 1);
	if (c->name)
		pefree(c->name, 1);
	pefree(c, 1);
}

static void pc_obj_detach(pc_obj *o)
{
	if (o->c && !o->c->pooled)
		pc_conn_free(o->c);
	o->c = NULL;
}

static void pc_free(zend_object *z)
{
	pc_obj_detach(pc_from(z));
	zend_object_std_dtor(z);
}

static void pc_pool_dtor(zval *z)
{
	pc_conn_free((pc_conn *)Z_PTR_P(z));
}

static zend_long pc_now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (zend_long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* the class says what to do about it, the code is perfd_error_kind's */
static void pc_throw(const perfd_t *p)
{
	int kind = perfd_error_kind(p);
	const char *msg = perfd_error(p);
	zend_class_entry *ce;

	switch (kind) {
	case PERFD_E_TRANSPORT:
		ce = pc_ex_transport;
		pc_failed = pc_cur;
		break;
	case PERFD_E_AUTH:      ce = pc_ex_auth; break;
	case PERFD_E_REFUSED:   ce = pc_ex_refused; break;
	case PERFD_E_SERVER:    ce = pc_ex_server; break;
	case PERFD_E_ARG:       ce = pc_ex_arg; break;
	default:                ce = pc_ex; break;
	}
	zend_throw_exception(ce, msg && *msg ? msg : "perfcached: unknown error", kind);
}

/* the first seed by pid, so a pool's workers start apart; the next when one does
 * not answer.  A wrong secret or option is wrong on all of them: it stops the walk */
static int pc_walk_seeds(pc_conn *c)
{
	int i, first = (int)(c->pid % c->nseeds), kind;

	for (i = 0; i < c->nseeds; i++) {
		c->p = perfd_connect(c->seeds[(first + i) % c->nseeds], c->port, &c->o);
		if (c->p)
			return 1;
		pc_st.seeds_failed++;
		kind = perfd_error_kind(NULL);
		if (kind == PERFD_E_AUTH || kind == PERFD_E_ARG)
			return 1;
	}
	return 0;
}

/* a seed that takes the connection and says nothing costs a short wait while another
 * may answer; only when none did is the whole timeout worth waiting */
static void pc_dial_seeds(pc_conn *c)
{
	int ct = c->o.connect_timeout_ms;

	if (c->nseeds > 1 && (ct <= 0 || ct > PC_SEED_FIRST_MS)) {
		c->o.connect_timeout_ms = PC_SEED_FIRST_MS;
		if (pc_walk_seeds(c)) {
			c->o.connect_timeout_ms = ct;
			return;
		}
		c->o.connect_timeout_ms = ct;
	}
	(void)pc_walk_seeds(c);
}

static int pc_dial(pc_conn *c)
{
	zend_long t0 = pc_now_ms();

	c->o.secrets = c->secrets[0] ? (const char *const *)c->secrets : NULL;
	c->o.name = c->name;
	c->pid = getpid();
	if (c->unix_path)
		c->p = perfd_connect_unix(c->unix_path, &c->o);
	else
		pc_dial_seeds(c);
	pc_st.dials++;
	pc_st.last_dial_ms = pc_now_ms() - t0;
	return c->p ? 0 : -1;
}

/* a forked child must not share the parent's Noise state: its nonces would
 * repeat, so it dials its own, and closing the inherited fds leaves the parent's */
static int pc_after_fork(pc_conn *c)
{
	if (c->pid == getpid())
		return 0;
	pc_st.forks++;
	if (c->p)
		perfd_free(c->p);
	c->p = NULL;
	return pc_dial(c);
}

/* a pooled handle at the start of a request: anything left unread is not this
 * request's, and the first look may be what finds a dead socket, so look twice */
static int pc_checkout(pc_conn *c)
{
	if (c->pid != getpid())
		return pc_after_fork(c);
	if (c->p && perfd_pending(c->p) > 0) {
		pc_st.dirty++;
		perfd_free(c->p);
		c->p = NULL;
	}
	if (!c->p)
		return pc_dial(c);
	pc_st.reused++;
	if (perfd_maintain(c->p) == 0 && perfd_maintain(c->p) == 0)
		return 0;
	pc_st.redials++;
	if (perfd_redial(c->p) == 0)
		return 0;
	perfd_free(c->p);
	c->p = NULL;
	return pc_dial(c);
}

static perfd_t *pc_handle(zval *self)
{
	pc_obj *o = pc_from(Z_OBJ_P(self));
	pc_conn *c = o->c;

	if (!c) {
		zend_throw_exception(pc_ex, "perfcached: not connected", PERFD_E_STATE);
		return NULL;
	}
	pc_cur = c;
	if (c->pid != getpid() && pc_after_fork(c) != 0) {
		pc_throw(NULL);
		return NULL;
	}
	if (pc_failed == c) {
		pc_failed = NULL;
		pc_st.redials++;
		/* a bare handle learned no members to re-dial through */
		if (perfd_redial(c->p) != 0) {
			perfd_free(c->p);
			c->p = NULL;
		}
	}
	if (!c->p && pc_dial(c) != 0) {
		pc_throw(NULL);
		return NULL;
	}
	return c->p;
}

/* opts' secrets: a string, or a list tried in order */
static int pc_secrets(zval *z, pc_conn *c)
{
	zval *v;
	int n = 0;

	if (Z_TYPE_P(z) == IS_STRING) {
		c->secrets[n++] = pestrdup(Z_STRVAL_P(z), 1);
	} else if (Z_TYPE_P(z) == IS_ARRAY) {
		ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(z), v) {
			if (Z_TYPE_P(v) != IS_STRING || n >= PC_MAX_SECRETS) {
				zend_value_error("secrets must be at most %d strings", PC_MAX_SECRETS);
				return -1;
			}
			c->secrets[n++] = pestrdup(Z_STRVAL_P(v), 1);
		} ZEND_HASH_FOREACH_END();
	} else {
		zend_value_error("secrets must be a string or a list of strings");
		return -1;
	}
	return 0;
}

static int pc_seed_add(pc_conn *c, zend_string *s)
{
	if (strlen(ZSTR_VAL(s)) != ZSTR_LEN(s) || c->nseeds >= PC_MAX_SEEDS) {
		zend_value_error("host must be at most %d names, with no NUL", PC_MAX_SEEDS);
		return -1;
	}
	c->seeds[c->nseeds++] = pestrdup(ZSTR_VAL(s), 1);
	return 0;
}

/* the constructor's host: a name, or a list of them on one port */
static int pc_seeds(HashTable *list, zend_string *one, pc_conn *c)
{
	zval *v;

	if (one)
		return pc_seed_add(c, one);
	ZEND_HASH_FOREACH_VAL(list, v) {
		if (Z_TYPE_P(v) != IS_STRING) {
			zend_value_error("host must be a string or a list of strings");
			return -1;
		}
		if (pc_seed_add(c, Z_STR_P(v)) != 0)
			return -1;
	} ZEND_HASH_FOREACH_END();
	if (!c->nseeds) {
		zend_value_error("host must name a node");
		return -1;
	}
	return 0;
}

static int pc_opts(HashTable *h, pc_conn *c, int *persistent)
{
	perfd_opts *o = &c->o;
	zend_string *k;
	zval *v;

	o->encrypt = 1;                /* stays encrypted past the handshake */
	if (!h)
		return 0;
	ZEND_HASH_FOREACH_STR_KEY_VAL(h, k, v) {
		if (!k) {
			zend_value_error("options must be named");
			return -1;
		}
		if (zend_string_equals_literal(k, "secrets")) {
			if (pc_secrets(v, c) != 0)
				return -1;
		} else if (zend_string_equals_literal(k, "unix")) {
			if (Z_TYPE_P(v) != IS_STRING) {
				zend_value_error("unix must be a string");
				return -1;
			}
			c->unix_path = pestrdup(Z_STRVAL_P(v), 1);
		} else if (zend_string_equals_literal(k, "name")) {
			if (Z_TYPE_P(v) != IS_STRING) {
				zend_value_error("name must be a string");
				return -1;
			}
			c->name = pestrdup(Z_STRVAL_P(v), 1);
		} else if (zend_string_equals_literal(k, "persistent")) {
			*persistent = zend_is_true(v);
		} else if (zend_string_equals_literal(k, "encrypt")) {
			o->encrypt = zend_is_true(v) ? 1 : 0;
		} else if (zend_string_equals_literal(k, "route_keys")) {
			o->route_keys = zend_is_true(v);
		} else if (zend_string_equals_literal(k, "route_hints")) {
			o->route_hints = zend_is_true(v);
		} else if (zend_string_equals_literal(k, "connect_timeout_ms")) {
			o->connect_timeout_ms = (int)zval_get_long(v);
		} else if (zend_string_equals_literal(k, "io_timeout_ms")) {
			o->io_timeout_ms = (int)zval_get_long(v);
		} else if (zend_string_equals_literal(k, "policy")) {
			o->policy = (int)zval_get_long(v);
		} else if (zend_string_equals_literal(k, "spares")) {
			o->spares = (int)zval_get_long(v);
		} else if (zend_string_equals_literal(k, "refresh_ms")) {
			o->refresh_ms = (int)zval_get_long(v);
		} else if (zend_string_equals_literal(k, "keepalive_s")) {
			o->keepalive_s = (int)zval_get_long(v);
		} else if (zend_string_equals_literal(k, "idle_ping_ms")) {
			o->idle_ping_ms = (int)zval_get_long(v);
		} else if (zend_string_equals_literal(k, "worker_balance")) {
			o->worker_balance = (int)zval_get_long(v);
		} else {
			zend_value_error("unknown option \"%s\"", ZSTR_VAL(k));
			return -1;
		}
	} ZEND_HASH_FOREACH_END();
	return 0;
}

/* the pool's key: every field that decides what a dial gives */
static zend_string *pc_pool_key(const pc_conn *c)
{
	smart_str k = {0};
	const perfd_opts *o = &c->o;
	int i;

	for (i = 0; c->seeds[i]; i++) {
		smart_str_appends(&k, c->seeds[i]);
		smart_str_appendc(&k, '\2');
	}
	smart_str_append_printf(&k, "\1%d\1%s\1%s\1%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
		c->port, c->unix_path ? c->unix_path : "", c->name ? c->name : "",
		o->connect_timeout_ms, o->io_timeout_ms, o->policy, o->spares,
		o->refresh_ms, o->route_keys, o->keepalive_s, o->idle_ping_ms,
		o->route_hints, o->worker_balance, o->encrypt);
	for (i = 0; c->secrets[i]; i++) {
		smart_str_appendc(&k, '\1');
		smart_str_appends(&k, c->secrets[i]);
	}
	smart_str_0(&k);
	return k.s;
}

#define PC_KEY_ARGS() \
	char *col, *key; \
	size_t col_len, key_len
#define PC_PARSE_COL_KEY() \
	Z_PARAM_PATH(col, col_len) \
	Z_PARAM_PATH(key, key_len)
#define PC_SELF(p) \
	perfd_t *p = pc_handle(ZEND_THIS); \
	if (!p) \
		RETURN_THROWS()

PHP_METHOD(Perfcached, __construct)
{
	zend_string *host = NULL;
	HashTable *hosts = NULL;
	zend_long port = 6479;
	HashTable *opts = NULL;
	int persistent = 1;
	pc_conn *c, *have;
	zend_string *key;
	pc_obj *self = pc_from(Z_OBJ_P(ZEND_THIS));

	ZEND_PARSE_PARAMETERS_START(1, 3)
		Z_PARAM_ARRAY_HT_OR_STR(hosts, host)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(port)
		Z_PARAM_ARRAY_HT(opts)
	ZEND_PARSE_PARAMETERS_END();
	pc_cur = NULL;                 /* a failed dial here is no handle's */
	c = pecalloc(1, sizeof(*c), 1);
	perfd_opts_init(&c->o);
	c->port = (int)port;
	if (pc_seeds(hosts, host, c) != 0 || pc_opts(opts, c, &persistent) != 0) {
		pc_conn_free(c);
		RETURN_THROWS();
	}
	pc_obj_detach(self);
	if (!persistent) {
		if (pc_dial(c) != 0) {
			pc_throw(NULL);
			pc_conn_free(c);
			RETURN_THROWS();
		}
		self->c = c;
		return;
	}
	key = pc_pool_key(c);
	have = zend_hash_find_ptr(&pc_pool, key);
	if (have) {
		pc_conn_free(c);
		zend_string_release(key);
		if (pc_checkout(have) != 0) {
			pc_throw(NULL);
			RETURN_THROWS();
		}
		self->c = have;
		return;
	}
	if (pc_dial(c) != 0) {
		pc_throw(NULL);
		pc_conn_free(c);
		zend_string_release(key);
		RETURN_THROWS();
	}
	c->pooled = 1;
	/* _str_: the table takes a persistent copy; this key dies with the request */
	zend_hash_str_add_new_ptr(&pc_pool, ZSTR_VAL(key), ZSTR_LEN(key), c);
	zend_string_release(key);
	self->c = c;
}

/* this object lets go; a pooled handle stays for the worker's next request */
PHP_METHOD(Perfcached, close)
{
	ZEND_PARSE_PARAMETERS_NONE();
	pc_obj_detach(pc_from(Z_OBJ_P(ZEND_THIS)));
}

/* what this process's pool has done: for a test, or a status page */
PHP_METHOD(Perfcached, pool)
{
	ZEND_PARSE_PARAMETERS_NONE();
	array_init(return_value);
	add_assoc_long(return_value, "pid", (zend_long)getpid());
	add_assoc_long(return_value, "handles", zend_hash_num_elements(&pc_pool));
	add_assoc_long(return_value, "dials", pc_st.dials);
	add_assoc_long(return_value, "reused", pc_st.reused);
	add_assoc_long(return_value, "redials", pc_st.redials);
	add_assoc_long(return_value, "forks", pc_st.forks);
	add_assoc_long(return_value, "dirty", pc_st.dirty);
	add_assoc_long(return_value, "prepared", pc_st.prepared);
	add_assoc_long(return_value, "last_dial_ms", pc_st.last_dial_ms);
	add_assoc_long(return_value, "seeds_failed", pc_st.seeds_failed);
}

PHP_METHOD(Perfcached, ping)
{
	ZEND_PARSE_PARAMETERS_NONE();
	PC_SELF(p);
	if (perfd_ping(p) != 0)
		pc_throw(p);
}

PHP_METHOD(Perfcached, set)
{
	PC_KEY_ARGS();
	zend_string *val;
	zend_long ttl = 0;

	ZEND_PARSE_PARAMETERS_START(3, 4)
		PC_PARSE_COL_KEY()
		Z_PARAM_STR(val)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(ttl)
	ZEND_PARSE_PARAMETERS_END();
	PC_SELF(p);
	if (perfd_set(p, col, key, ZSTR_VAL(val), ZSTR_LEN(val), ttl) != 0)
		pc_throw(p);
}

/* -2 from a _buf read: only past a record, which the daemon would not hold */
PHP_METHOD(Perfcached, get)
{
	PC_KEY_ARGS();
	size_t n = 0;
	int rc;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		PC_PARSE_COL_KEY()
	ZEND_PARSE_PARAMETERS_END();
	PC_SELF(p);
	rc = perfd_get_buf(p, col, key, pc_buf, sizeof pc_buf, &n, NULL);
	if (rc == 1)
		RETURN_STRINGL(pc_buf, n);
	if (rc == 0)
		RETURN_NULL();
	if (rc == -2) {
		void *big = NULL;
		long long ttl;

		rc = perfd_get(p, col, key, &big, &n, &ttl);
		if (rc == 1) {
			RETVAL_STRINGL(big, n);
			perfd_free_value(big);
			return;
		}
		if (rc == 0)
			RETURN_NULL();
	}
	pc_throw(p);
}

PHP_METHOD(Perfcached, del)
{
	PC_KEY_ARGS();
	int rc;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		PC_PARSE_COL_KEY()
	ZEND_PARSE_PARAMETERS_END();
	PC_SELF(p);
	rc = perfd_del(p, col, key);
	if (rc < 0) {
		pc_throw(p);
		RETURN_THROWS();
	}
	RETURN_BOOL(rc == 1);
}

PHP_METHOD(Perfcached, exists)
{
	PC_KEY_ARGS();
	int rc;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		PC_PARSE_COL_KEY()
	ZEND_PARSE_PARAMETERS_END();
	PC_SELF(p);
	rc = perfd_exists(p, col, key);
	if (rc < 0) {
		pc_throw(p);
		RETURN_THROWS();
	}
	RETURN_BOOL(rc == 1);
}

/* seconds left, -1 no expiry, -2 no such key: perfd_ttl's -1 is also its error */
PHP_METHOD(Perfcached, ttl)
{
	PC_KEY_ARGS();
	long long t;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		PC_PARSE_COL_KEY()
	ZEND_PARSE_PARAMETERS_END();
	PC_SELF(p);
	t = perfd_ttl(p, col, key);
	if (t == -1 && perfd_error_kind(p) != PERFD_E_NONE) {
		pc_throw(p);
		RETURN_THROWS();
	}
	RETURN_LONG((zend_long)t);
}

PHP_METHOD(Perfcached, expire)
{
	PC_KEY_ARGS();
	zend_long ttl;
	int rc;

	ZEND_PARSE_PARAMETERS_START(3, 3)
		PC_PARSE_COL_KEY()
		Z_PARAM_LONG(ttl)
	ZEND_PARSE_PARAMETERS_END();
	PC_SELF(p);
	rc = perfd_expire(p, col, key, ttl);
	if (rc < 0) {
		pc_throw(p);
		RETURN_THROWS();
	}
	RETURN_BOOL(rc == 0);
}

PHP_METHOD(Perfcached, add)
{
	PC_KEY_ARGS();
	zend_long by = 1, ttl = 0;
	long long v = 0;

	ZEND_PARSE_PARAMETERS_START(2, 4)
		PC_PARSE_COL_KEY()
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(by)
		Z_PARAM_LONG(ttl)
	ZEND_PARSE_PARAMETERS_END();
	PC_SELF(p);
	if (perfd_add(p, col, key, by, ttl, &v) != 0) {
		pc_throw(p);
		RETURN_THROWS();
	}
	RETURN_LONG((zend_long)v);
}

PHP_METHOD(Perfcached, sub)
{
	PC_KEY_ARGS();
	zend_long by = 1;
	long long v = 0;

	ZEND_PARSE_PARAMETERS_START(2, 3)
		PC_PARSE_COL_KEY()
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(by)
	ZEND_PARSE_PARAMETERS_END();
	PC_SELF(p);
	if (perfd_sub(p, col, key, by, &v) != 0) {
		pc_throw(p);
		RETURN_THROWS();
	}
	RETURN_LONG((zend_long)v);
}

/* key => value or null.  A key whose node did not answer fails the whole
 * call (PerfcachedRefusedException: ask again), never reads as a miss */
PHP_METHOD(Perfcached, mget)
{
	char *col;
	size_t col_len;
	HashTable *keys;
	const char **kv;
	void **vals;
	size_t *lens;
	zend_string **strs;
	zval *z;
	int n = 0, i;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_PATH(col, col_len)
		Z_PARAM_ARRAY_HT(keys)
	ZEND_PARSE_PARAMETERS_END();
	PC_SELF(p);
	array_init(return_value);
	i = zend_hash_num_elements(keys);
	if (!i)
		return;
	kv = safe_emalloc(i, sizeof(*kv), 0);
	strs = safe_emalloc(i, sizeof(*strs), 0);
	vals = safe_emalloc(i, sizeof(*vals), 0);
	lens = safe_emalloc(i, sizeof(*lens), 0);
	ZEND_HASH_FOREACH_VAL(keys, z) {
		zend_string *s = zval_get_string(z);

		strs[n] = s;
		kv[n++] = ZSTR_VAL(s);
		if (zend_str_has_nul_byte(s)) {
			zend_value_error("mget: a key must not contain a NUL byte");
			goto out;
		}
	} ZEND_HASH_FOREACH_END();
	memset(vals, 0, (size_t)n * sizeof(*vals));
	if (perfd_mget(p, col, kv, n, vals, lens) != 0) {
		pc_throw(p);
		goto out;
	}
	for (i = 0; i < n; i++) {
		zval v;

		if (vals[i]) {
			ZVAL_STRINGL(&v, vals[i], lens[i]);
			perfd_free_value(vals[i]);
		} else {
			ZVAL_NULL(&v);
		}
		zend_symtable_update(Z_ARRVAL_P(return_value), strs[i], &v);
	}
out:
	for (i = 0; i < n; i++)
		zend_string_release(strs[i]);
	efree(kv);
	efree(strs);
	efree(vals);
	efree(lens);
}

PHP_METHOD(Perfcached, keys)
{
	char *col, *match = NULL;
	size_t col_len, match_len = 0;
	zend_long limit = 0;
	char **out = NULL;
	int n, i;

	ZEND_PARSE_PARAMETERS_START(1, 3)
		Z_PARAM_PATH(col, col_len)
		Z_PARAM_OPTIONAL
		Z_PARAM_PATH_OR_NULL(match, match_len)
		Z_PARAM_LONG(limit)
	ZEND_PARSE_PARAMETERS_END();
	PC_SELF(p);
	n = perfd_keys(p, col, match, (int)limit, &out);
	if (n < 0) {
		pc_throw(p);
		RETURN_THROWS();
	}
	array_init_size(return_value, (uint32_t)n);
	for (i = 0; i < n; i++)
		add_next_index_string(return_value, out[i]);
	perfd_free_keys(out, n);
}

PHP_METHOD(Perfcached, hset)
{
	PC_KEY_ARGS();
	char *field;
	size_t field_len;
	zend_string *val;
	int rc;

	ZEND_PARSE_PARAMETERS_START(4, 4)
		PC_PARSE_COL_KEY()
		Z_PARAM_PATH(field, field_len)
		Z_PARAM_STR(val)
	ZEND_PARSE_PARAMETERS_END();
	PC_SELF(p);
	rc = perfd_hset(p, col, key, field, ZSTR_VAL(val), ZSTR_LEN(val));
	if (rc < 0) {
		pc_throw(p);
		RETURN_THROWS();
	}
	RETURN_BOOL(rc == 1);
}

PHP_METHOD(Perfcached, hsetnx)
{
	PC_KEY_ARGS();
	char *field;
	size_t field_len;
	zend_string *val;
	int rc;

	ZEND_PARSE_PARAMETERS_START(4, 4)
		PC_PARSE_COL_KEY()
		Z_PARAM_PATH(field, field_len)
		Z_PARAM_STR(val)
	ZEND_PARSE_PARAMETERS_END();
	PC_SELF(p);
	rc = perfd_hsetnx(p, col, key, field, ZSTR_VAL(val), ZSTR_LEN(val));
	if (rc < 0) {
		pc_throw(p);
		RETURN_THROWS();
	}
	RETURN_BOOL(rc == 1);
}

PHP_METHOD(Perfcached, hget)
{
	PC_KEY_ARGS();
	char *field;
	size_t field_len, n = 0;
	int rc;

	ZEND_PARSE_PARAMETERS_START(3, 3)
		PC_PARSE_COL_KEY()
		Z_PARAM_PATH(field, field_len)
	ZEND_PARSE_PARAMETERS_END();
	PC_SELF(p);
	rc = perfd_hget_buf(p, col, key, field, pc_buf, sizeof pc_buf, &n);
	if (rc == 1)
		RETURN_STRINGL(pc_buf, n);
	if (rc == 0)
		RETURN_NULL();
	pc_throw(p);
}

PHP_METHOD(Perfcached, hdel)
{
	PC_KEY_ARGS();
	char *field;
	size_t field_len;
	int rc;

	ZEND_PARSE_PARAMETERS_START(3, 3)
		PC_PARSE_COL_KEY()
		Z_PARAM_PATH(field, field_len)
	ZEND_PARSE_PARAMETERS_END();
	PC_SELF(p);
	rc = perfd_hdel(p, col, key, field);
	if (rc < 0) {
		pc_throw(p);
		RETURN_THROWS();
	}
	RETURN_BOOL(rc == 1);
}

PHP_METHOD(Perfcached, hexists)
{
	PC_KEY_ARGS();
	char *field;
	size_t field_len;
	int rc;

	ZEND_PARSE_PARAMETERS_START(3, 3)
		PC_PARSE_COL_KEY()
		Z_PARAM_PATH(field, field_len)
	ZEND_PARSE_PARAMETERS_END();
	PC_SELF(p);
	rc = perfd_hexists(p, col, key, field);
	if (rc < 0) {
		pc_throw(p);
		RETURN_THROWS();
	}
	RETURN_BOOL(rc == 1);
}

PHP_METHOD(Perfcached, hlen)
{
	PC_KEY_ARGS();
	long long n = 0;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		PC_PARSE_COL_KEY()
	ZEND_PARSE_PARAMETERS_END();
	PC_SELF(p);
	if (perfd_hlen(p, col, key, &n) != 0) {
		pc_throw(p);
		RETURN_THROWS();
	}
	RETURN_LONG((zend_long)n);
}

PHP_METHOD(Perfcached, hincrby)
{
	PC_KEY_ARGS();
	char *field;
	size_t field_len;
	zend_long by = 1;
	long long v = 0;

	ZEND_PARSE_PARAMETERS_START(3, 4)
		PC_PARSE_COL_KEY()
		Z_PARAM_PATH(field, field_len)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(by)
	ZEND_PARSE_PARAMETERS_END();
	PC_SELF(p);
	if (perfd_hincrby(p, col, key, field, by, &v) != 0) {
		pc_throw(p);
		RETURN_THROWS();
	}
	RETURN_LONG((zend_long)v);
}

static int pc_hfield(void *ctx, const char *field, size_t flen, const void *val,
		size_t vlen)
{
	add_assoc_stringl_ex((zval *)ctx, field, flen, (const char *)val, vlen);
	return 0;
}

PHP_METHOD(Perfcached, hgetall)
{
	PC_KEY_ARGS();

	ZEND_PARSE_PARAMETERS_START(2, 2)
		PC_PARSE_COL_KEY()
	ZEND_PARSE_PARAMETERS_END();
	PC_SELF(p);
	array_init(return_value);
	if (perfd_hgetall(p, col, key, pc_hfield, return_value) < 0)
		pc_throw(p);
}

/* one row a member, as this handle's own dials found it */
PHP_METHOD(Perfcached, members)
{
	int i, n;

	ZEND_PARSE_PARAMETERS_NONE();
	PC_SELF(p);
	array_init(return_value);
	n = perfd_member_count(p);
	for (i = 0; i < n; i++) {
		char addr[128] = "";
		int port = 0, node = 0, active = 0;
		zval row;

		if (perfd_member_info(p, i, addr, sizeof addr, &port, &node, &active) != 0)
			continue;
		array_init(&row);
		add_assoc_string(&row, "addr", addr);
		add_assoc_long(&row, "port", port);
		add_assoc_long(&row, "node", node);
		add_assoc_bool(&row, "active", active);
		add_assoc_string(&row, "state", perfd_member_state(p, i, NULL, 0));
		add_next_index_zval(return_value, &row);
	}
}

/* the node that should hold the key, or null when routing does not apply */
PHP_METHOD(Perfcached, ownerOf)
{
	PC_KEY_ARGS();
	int i, node = 0;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		PC_PARSE_COL_KEY()
	ZEND_PARSE_PARAMETERS_END();
	PC_SELF(p);
	i = perfd_owner_of(p, col, key);
	if (i < 0 || perfd_member_info(p, i, NULL, 0, NULL, &node, NULL) != 0)
		RETURN_NULL();
	RETURN_LONG(node);
}

PHP_METHOD(Perfcached, activeNode)
{
	ZEND_PARSE_PARAMETERS_NONE();
	PC_SELF(p);
	RETURN_LONG(perfd_active_node(p));
}

PHP_METHOD(Perfcached, failovers)
{
	ZEND_PARSE_PARAMETERS_NONE();
	PC_SELF(p);
	RETURN_LONG((zend_long)perfd_failovers(p));
}

PHP_METHOD(Perfcached, routing)
{
	ZEND_PARSE_PARAMETERS_NONE();
	PC_SELF(p);
	RETURN_BOOL(perfd_routing(p) == 1);
}

PHP_METHOD(Perfcached, serverVersion)
{
	ZEND_PARSE_PARAMETERS_NONE();
	PC_SELF(p);
	RETURN_STRING(perfd_server_version(p));
}

/* the daemon's /stats document, for one collection or all */
PHP_METHOD(Perfcached, stats)
{
	char *col = NULL, *js;
	size_t col_len = 0;

	ZEND_PARSE_PARAMETERS_START(0, 1)
		Z_PARAM_OPTIONAL
		Z_PARAM_PATH_OR_NULL(col, col_len)
	ZEND_PARSE_PARAMETERS_END();
	PC_SELF(p);
	js = perfd_stats(p, col);
	if (!js) {
		pc_throw(p);
		RETURN_THROWS();
	}
	php_json_decode_ex(return_value, js, strlen(js), PHP_JSON_OBJECT_AS_ARRAY, PHP_JSON_PARSER_DEFAULT_DEPTH);
	perfd_free_value(js);
}

PHP_METHOD(Perfcached, maintain)
{
	ZEND_PARSE_PARAMETERS_NONE();
	PC_SELF(p);
	if (perfd_maintain(p) != 0)
		pc_throw(p);
}

ZEND_BEGIN_ARG_INFO_EX(ai_ctor, 0, 0, 1)
	ZEND_ARG_TYPE_MASK(0, host, MAY_BE_STRING | MAY_BE_ARRAY, NULL)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, port, IS_LONG, 0, "6479")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, opts, IS_ARRAY, 0, "[]")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_void, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_int0, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_bool0, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_str0, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_arr0, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_stats, 0, 0, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, col, IS_STRING, 1, "null")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_set, 0, 3, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, col, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, value, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, ttl, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_get, 0, 2, IS_STRING, 1)
	ZEND_ARG_TYPE_INFO(0, col, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_key_bool, 0, 2, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, col, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_key_int, 0, 2, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, col, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_expire, 0, 3, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, col, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, ttl, IS_LONG, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_add, 0, 2, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, col, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, by, IS_LONG, 0, "1")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, ttl, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_sub, 0, 2, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, col, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, by, IS_LONG, 0, "1")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_mget, 0, 2, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO(0, col, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, keys, IS_ARRAY, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_keys, 0, 1, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO(0, col, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, match, IS_STRING, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, limit, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_hset, 0, 4, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, col, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, field, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, value, IS_STRING, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_hget, 0, 3, IS_STRING, 1)
	ZEND_ARG_TYPE_INFO(0, col, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, field, IS_STRING, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_field_bool, 0, 3, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, col, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, field, IS_STRING, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_hincrby, 0, 3, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, col, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, field, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, by, IS_LONG, 0, "1")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_hgetall, 0, 2, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO(0, col, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_owner, 0, 2, IS_LONG, 1)
	ZEND_ARG_TYPE_INFO(0, col, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry pc_methods[] = {
	PHP_ME(Perfcached, __construct, ai_ctor, ZEND_ACC_PUBLIC)
	PHP_ME(Perfcached, close, ai_void, ZEND_ACC_PUBLIC)
	PHP_ME(Perfcached, ping, ai_void, ZEND_ACC_PUBLIC)
	PHP_ME(Perfcached, set, ai_set, ZEND_ACC_PUBLIC)
	PHP_ME(Perfcached, get, ai_get, ZEND_ACC_PUBLIC)
	PHP_ME(Perfcached, del, ai_key_bool, ZEND_ACC_PUBLIC)
	PHP_ME(Perfcached, exists, ai_key_bool, ZEND_ACC_PUBLIC)
	PHP_ME(Perfcached, ttl, ai_key_int, ZEND_ACC_PUBLIC)
	PHP_ME(Perfcached, expire, ai_expire, ZEND_ACC_PUBLIC)
	PHP_ME(Perfcached, add, ai_add, ZEND_ACC_PUBLIC)
	PHP_ME(Perfcached, sub, ai_sub, ZEND_ACC_PUBLIC)
	PHP_ME(Perfcached, mget, ai_mget, ZEND_ACC_PUBLIC)
	PHP_ME(Perfcached, keys, ai_keys, ZEND_ACC_PUBLIC)
	PHP_ME(Perfcached, hset, ai_hset, ZEND_ACC_PUBLIC)
	PHP_ME(Perfcached, hsetnx, ai_hset, ZEND_ACC_PUBLIC)
	PHP_ME(Perfcached, hget, ai_hget, ZEND_ACC_PUBLIC)
	PHP_ME(Perfcached, hdel, ai_field_bool, ZEND_ACC_PUBLIC)
	PHP_ME(Perfcached, hexists, ai_field_bool, ZEND_ACC_PUBLIC)
	PHP_ME(Perfcached, hlen, ai_key_int, ZEND_ACC_PUBLIC)
	PHP_ME(Perfcached, hincrby, ai_hincrby, ZEND_ACC_PUBLIC)
	PHP_ME(Perfcached, hgetall, ai_hgetall, ZEND_ACC_PUBLIC)
	PHP_ME(Perfcached, members, ai_arr0, ZEND_ACC_PUBLIC)
	PHP_ME(Perfcached, ownerOf, ai_owner, ZEND_ACC_PUBLIC)
	PHP_ME(Perfcached, activeNode, ai_int0, ZEND_ACC_PUBLIC)
	PHP_ME(Perfcached, failovers, ai_int0, ZEND_ACC_PUBLIC)
	PHP_ME(Perfcached, routing, ai_bool0, ZEND_ACC_PUBLIC)
	PHP_ME(Perfcached, serverVersion, ai_str0, ZEND_ACC_PUBLIC)
	PHP_ME(Perfcached, stats, ai_stats, ZEND_ACC_PUBLIC)
	PHP_ME(Perfcached, maintain, ai_void, ZEND_ACC_PUBLIC)
	PHP_ME(Perfcached, pool, ai_arr0, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	PHP_FE_END
};

PHP_INI_BEGIN()
	PHP_INI_ENTRY("perfcached.prepare_secrets_file", "", PHP_INI_SYSTEM, NULL)
PHP_INI_END()

/* the file's secrets, one a line, derived once in this process: FPM's master
 * at startup, so its workers inherit the keys and none pays Argon2id for them */
static void pc_prepare(void)
{
	const char *path = INI_STR("perfcached.prepare_secrets_file");
	char line[192], *secs[PC_MAX_SECRETS + 1];
	struct stat st;
	FILE *f;
	int n = 0, i;

	if (!path || !*path)
		return;
	f = fopen(path, "r");
	if (!f) {
		zend_error(E_WARNING, "perfcached: cannot read %s: %s", path, strerror(errno));
		return;
	}
	if (fstat(fileno(f), &st) == 0 && (st.st_mode & 007))
		zend_error(E_WARNING, "perfcached: %s can be read by anyone", path);
	while (n < PC_MAX_SECRETS && fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (*line)
			secs[n++] = pestrdup(line, 1);
	}
	fclose(f);
	ZEND_SECURE_ZERO(line, sizeof line);
	secs[n] = NULL;
	if (n && perfd_psk_prepare((const char *const *)secs) == 0)
		pc_st.prepared = n;
	else if (n)
		zend_error(E_WARNING, "perfcached: deriving the keys in %s failed", path);
	for (i = 0; i < n; i++) {
		ZEND_SECURE_ZERO(secs[i], strlen(secs[i]));
		pefree(secs[i], 1);
	}
}

static zend_class_entry *pc_register_ex(const char *name, zend_class_entry *parent)
{
	zend_class_entry ce;

	INIT_CLASS_ENTRY_EX(ce, name, strlen(name), NULL);
	return zend_register_internal_class_ex(&ce, parent);
}

PHP_MINIT_FUNCTION(perfcached)
{
	zend_class_entry ce;

	REGISTER_INI_ENTRIES();
	zend_hash_init(&pc_pool, 8, NULL, pc_pool_dtor, 1);
	pc_prepare();
	pc_ex = pc_register_ex("PerfcachedException", spl_ce_RuntimeException);
	pc_ex_transport = pc_register_ex("PerfcachedTransportException", pc_ex);
	pc_ex_auth = pc_register_ex("PerfcachedAuthException", pc_ex);
	pc_ex_refused = pc_register_ex("PerfcachedRefusedException", pc_ex);
	pc_ex_server = pc_register_ex("PerfcachedServerException", pc_ex);
	pc_ex_arg = pc_register_ex("PerfcachedArgumentException", pc_ex);

	INIT_CLASS_ENTRY(ce, "Perfcached", pc_methods);
	pc_ce = zend_register_internal_class(&ce);
	pc_ce->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NOT_SERIALIZABLE;
	pc_ce->create_object = pc_create;
	memcpy(&pc_handlers, zend_get_std_object_handlers(), sizeof pc_handlers);
	pc_handlers.offset = XtOffsetOf(pc_obj, std);
	pc_handlers.free_obj = pc_free;
	pc_handlers.clone_obj = NULL;
	zend_declare_class_constant_string(pc_ce, "VERSION", sizeof("VERSION") - 1,
		PHP_PERFCACHED_VERSION);
	zend_declare_class_constant_string(pc_ce, "LIBPERFD", sizeof("LIBPERFD") - 1,
		PERFD_VERSION);
	zend_declare_class_constant_long(pc_ce, "POLICY_FAILOVER",
		sizeof("POLICY_FAILOVER") - 1, PERFD_POLICY_FAILOVER);
	zend_declare_class_constant_long(pc_ce, "POLICY_ROUND_ROBIN",
		sizeof("POLICY_ROUND_ROBIN") - 1, PERFD_POLICY_ROUND_ROBIN);
	zend_declare_class_constant_long(pc_ce, "POLICY_LEAST_CONN",
		sizeof("POLICY_LEAST_CONN") - 1, PERFD_POLICY_LEAST_CONN);
	zend_declare_class_constant_long(pc_ce, "POLICY_WEIGHTED",
		sizeof("POLICY_WEIGHTED") - 1, PERFD_POLICY_WEIGHTED);
	zend_declare_class_constant_long(pc_ce, "SPARES_NONE",
		sizeof("SPARES_NONE") - 1, PERFD_SPARES_NONE);
	return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(perfcached)
{
	zend_hash_destroy(&pc_pool);
	UNREGISTER_INI_ENTRIES();
	return SUCCESS;
}

PHP_MINFO_FUNCTION(perfcached)
{
	php_info_print_table_start();
	php_info_print_table_row(2, "perfcached", "enabled");
	php_info_print_table_row(2, "Version", PHP_PERFCACHED_VERSION);
	php_info_print_table_row(2, "libperfd, built against", PERFD_VERSION);
	php_info_print_table_row(2, "libperfd, linked", perfd_version());
	php_info_print_table_end();
	DISPLAY_INI_ENTRIES();
}

static const zend_module_dep pc_deps[] = {
	ZEND_MOD_REQUIRED("json")
	ZEND_MOD_REQUIRED("spl")
	ZEND_MOD_END
};

zend_module_entry perfcached_module_entry = {
	STANDARD_MODULE_HEADER_EX, NULL, pc_deps,
	"perfcached", NULL,
	PHP_MINIT(perfcached), PHP_MSHUTDOWN(perfcached), NULL, NULL,
	PHP_MINFO(perfcached),
	PHP_PERFCACHED_VERSION,
	STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_PERFCACHED
ZEND_GET_MODULE(perfcached)
#endif
