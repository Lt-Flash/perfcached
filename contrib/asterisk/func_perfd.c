/*
 * Asterisk -- An open source telephony toolkit.
 *
 * func_perfd - perfcached, or a standalone Redis, from the dialplan
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov
 *
 * This program is free software, distributed under the terms of the GNU
 * General Public License Version 2 or later.  The perfd/lib and perfd/src
 * files beside it are libperfd, MIT - see perfd/lib/LICENSE.
 */

/*! \file
 *
 * \brief perfcached dialplan functions: PERFD_GET, PERFD_SET, PERFD_EXISTS
 * and PERFD_DELETE, over libperfd's binary dialect or RESP2.
 *
 * \ingroup functions
 */

/*** MODULEINFO
	<depend>sodium</depend>
	<defaultenabled>no</defaultenabled>
	<support_level>extended</support_level>
 ***/

#include "asterisk.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <time.h>

#include "asterisk/app.h"
#include "asterisk/astobj2.h"
#include "asterisk/channel.h"
#include "asterisk/cli.h"
#include "asterisk/config.h"
#include "asterisk/lock.h"
#include "asterisk/logger.h"
#include "asterisk/module.h"
#include "asterisk/pbx.h"
#include "asterisk/strings.h"
#include "asterisk/time.h"
#include "asterisk/utils.h"

#include "perfd/lib/perfd.h"
#include "perfd/src/compat/dprint.h"
#include "perfd/resp.h"

/*** DOCUMENTATION
	<function name="PERFD_GET" language="en_US">
		<synopsis>
			Read a value from perfcached or Redis.
		</synopsis>
		<syntax>
			<parameter name="collection">
				<para>The collection to read. Empty uses <literal>collection</literal>
				from <filename>perfd.conf</filename>. Against Redis this is a database
				number.</para>
			</parameter>
			<parameter name="key" required="true" />
		</syntax>
		<description>
			<para>Returns the value stored under the key, or an empty string, and
			sets <variable>PERFDSTATUS</variable>.  The server, the protocol, the
			credentials and the timeouts come from <filename>perfd.conf</filename>.</para>
			<para>A value is never truncated: one containing a NUL byte, longer than
			<literal>max_value</literal>, or longer than the caller's buffer (4095
			bytes inside ordinary dialplan substitution) is an error.  Use
			<literal>BASE64_ENCODE</literal> for binary values.</para>
			<variablelist>
				<variable name="PERFDSTATUS">
					<value name="OK">
						The key was found.
					</value>
					<value name="NOTFOUND">
						The key was not there.
					</value>
					<value name="ERROR">
						The request failed; the reason is logged.
					</value>
				</variable>
			</variablelist>
		</description>
		<see-also>
			<ref type="function">PERFD_SET</ref>
			<ref type="function">PERFD_EXISTS</ref>
			<ref type="function">PERFD_DELETE</ref>
		</see-also>
	</function>
	<function name="PERFD_SET" language="en_US">
		<synopsis>
			Store a value in perfcached or Redis.
		</synopsis>
		<syntax>
			<parameter name="collection">
				<para>The collection to write. Empty uses <literal>collection</literal>
				from <filename>perfd.conf</filename>.</para>
			</parameter>
			<parameter name="key" required="true" />
			<parameter name="ttl">
				<para>Seconds until the value expires. Empty uses
				<literal>default_ttl</literal>; 0 means no expiry.</para>
			</parameter>
		</syntax>
		<description>
			<para>Write-only: <literal>Set(PERFD_SET(calls,${UNIQUEID},3600)=${CALLERID(num)})</literal>.
			Sets <variable>PERFDSTATUS</variable>.</para>
			<variablelist>
				<variable name="PERFDSTATUS">
					<value name="OK">
						The value was stored.
					</value>
					<value name="ERROR">
						The request failed; the reason is logged.
					</value>
				</variable>
			</variablelist>
		</description>
		<see-also>
			<ref type="function">PERFD_GET</ref>
		</see-also>
	</function>
	<function name="PERFD_EXISTS" language="en_US">
		<synopsis>
			Check whether a key exists in perfcached or Redis.
		</synopsis>
		<syntax>
			<parameter name="collection" />
			<parameter name="key" required="true" />
		</syntax>
		<description>
			<para>Returns <literal>1</literal> or <literal>0</literal>, and sets
			<variable>PERFDSTATUS</variable>.</para>
			<variablelist>
				<variable name="PERFDSTATUS">
					<value name="OK">
						The key exists.
					</value>
					<value name="NOTFOUND">
						The key was not there.
					</value>
					<value name="ERROR">
						The request failed; the reason is logged.
					</value>
				</variable>
			</variablelist>
		</description>
		<see-also>
			<ref type="function">PERFD_GET</ref>
		</see-also>
	</function>
	<function name="PERFD_DELETE" language="en_US">
		<synopsis>
			Delete a key from perfcached or Redis.
		</synopsis>
		<syntax>
			<parameter name="collection" />
			<parameter name="key" required="true" />
		</syntax>
		<description>
			<para>Reading the function deletes the key and returns <literal>1</literal>
			when it removed it, <literal>0</literal> when it was absent.  Writing it,
			<literal>Set(PERFD_DELETE(calls,${UNIQUEID})=)</literal>, deletes and
			ignores the value.  A delete retried after a lost reply can answer
			<literal>0</literal> for a key the first attempt removed.  Sets
			<variable>PERFDSTATUS</variable>.</para>
			<variablelist>
				<variable name="PERFDSTATUS">
					<value name="OK">
						The key was removed.
					</value>
					<value name="NOTFOUND">
						The key was not there.
					</value>
					<value name="ERROR">
						The request failed; the reason is logged.
					</value>
				</variable>
			</variablelist>
		</description>
		<see-also>
			<ref type="function">PERFD_EXISTS</ref>
		</see-also>
	</function>
 ***/

#define PD_CONFIG "perfd.conf"

enum pd_protocol {
	PD_BINARY = 0,
	PD_RESP
};

enum pd_verb {
	PD_GET = 0,
	PD_SET,
	PD_EXISTS,
	PD_DELETE
};

enum pd_status {
	PD_OK = 0,
	PD_NOTFOUND,
	PD_ERROR
};

static const char *const pd_status_names[] = { "OK", "NOTFOUND", "ERROR" };

struct pd_conf {
	int protocol;
	struct resp_server *servers;
	int nservers;
	char **secrets;                /* NULL-terminated; NULL when none */
	int nsecrets;
	char *username;
	char *password;
	char *collection;
	int pool;
	int connect_timeout_ms;
	int io_timeout_ms;
	int wait_timeout_ms;
	long long default_ttl;
	size_t max_value;
	int policy;
	int spares;
	int route_keys;
	int keepalive_s;
	struct resp_sess_conf resp;    /* points into this struct */
};

struct pd_conn {
	int in_use;                    /* pool lock */
	/* owned by the borrower while in_use */
	perfd_t *h;
	struct resp_sess *rs;
	int next_server;
	int opened_before;
	unsigned long long failovers_done;
	unsigned long long failovers_live;
	unsigned long long pending_reconnects;
	unsigned long long resp_reconnects_seen;
	char seed[128];
	/* snapshots for the CLI, written under the pool lock */
	int snap_open;
	int snap_node;
	unsigned long long snap_failovers;
	char snap_where[128];
};

struct pd_stats {
	unsigned long long get_hits;
	unsigned long long get_misses;
	unsigned long long sets;
	unsigned long long exists_yes;
	unsigned long long exists_no;
	unsigned long long del_removed;
	unsigned long long del_absent;
	unsigned long long errors;
	unsigned long long wait_timeouts;
	unsigned long long reconnects;
};

struct pd_pool {
	struct pd_conf *cf;
	ast_mutex_t lock;
	ast_cond_t cond;
	int in_use;
	int waiters;
	struct pd_stats st;
	struct pd_conn conns[];
};

static AO2_GLOBAL_OBJ_STATIC(pd_pool_global);

/* libperfd's connect error lives in one process-wide buffer: dials take turns */
AST_MUTEX_DEFINE_STATIC(pd_dial_lock);

AST_MUTEX_DEFINE_STATIC(pd_log_lock);
static time_t pd_log_second;
static int pd_log_count;
static unsigned int pd_log_dropped;

#define PD_LOG_PER_SECOND 10

void func_perfd_log(int level, const char *file, int line, const char *func,
	const char *fmt, ...)
{
	va_list ap;
	int lev;

	switch (level) {
	case FUNC_PERFD_LOG_ERROR:
		lev = __LOG_ERROR;
		break;
	case FUNC_PERFD_LOG_WARNING:
		lev = __LOG_WARNING;
		break;
	case FUNC_PERFD_LOG_NOTICE:
		lev = __LOG_NOTICE;
		break;
	default:
		if (!DEBUG_ATLEAST(1)) {
			return;
		}
		lev = __LOG_DEBUG;
		break;
	}
	va_start(ap, fmt);
	ast_log_ap(lev, file, line, func, fmt, ap);
	va_end(ap);
}

static void pd_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* per-call failures: at most PD_LOG_PER_SECOND a second, the rest counted */
static void pd_warn(const char *fmt, ...)
{
	va_list ap;
	time_t now = time(NULL);
	unsigned int dropped = 0;
	int allow;

	ast_mutex_lock(&pd_log_lock);
	if (now != pd_log_second) {
		dropped = pd_log_dropped;
		pd_log_dropped = 0;
		pd_log_second = now;
		pd_log_count = 0;
	}
	allow = pd_log_count < PD_LOG_PER_SECOND;
	if (allow) {
		pd_log_count++;
	} else {
		pd_log_dropped++;
	}
	ast_mutex_unlock(&pd_log_lock);

	if (dropped) {
		ast_log(LOG_WARNING, "func_perfd: %u more failure messages suppressed\n", dropped);
	}
	if (!allow) {
		return;
	}
	va_start(ap, fmt);
	ast_log_ap(__LOG_WARNING, __FILE__, __LINE__, __func__, fmt, ap);
	va_end(ap);
}

static void server_name(const struct resp_server *sv, char *buf, size_t cap)
{
	if (sv->path) {
		snprintf(buf, cap, "unix:%s", sv->path);
	} else if (strchr(sv->host, ':')) {
		snprintf(buf, cap, "[%s]:%d", sv->host, sv->port);
	} else {
		snprintf(buf, cap, "%s:%d", sv->host, sv->port);
	}
}

static void conf_free(struct pd_conf *cf)
{
	int i;

	if (!cf) {
		return;
	}
	for (i = 0; i < cf->nservers; i++) {
		ast_free(cf->servers[i].host);
		ast_free(cf->servers[i].path);
	}
	ast_free(cf->servers);
	for (i = 0; i < cf->nsecrets; i++) {
		ast_free(cf->secrets[i]);
	}
	ast_free(cf->secrets);
	ast_free(cf->username);
	ast_free(cf->password);
	ast_free(cf->collection);
	ast_free(cf);
}

static int parse_ll(const char *v, long long lo, long long hi, long long *out)
{
	char *end;
	long long x;

	errno = 0;
	x = strtoll(v, &end, 10);
	if (errno || end == v || *end || x < lo || x > hi) {
		return -1;
	}
	*out = x;
	return 0;
}

static int parse_server(const char *v, struct resp_server *sv)
{
	const char *colon;
	long long port;

	memset(sv, 0, sizeof(*sv));
	if (!strncmp(v, "unix:", 5)) {
		if (v[5] != '/') {
			return -1;
		}
		sv->path = ast_strdup(v + 5);
		return sv->path ? 0 : -1;
	}
	if (v[0] == '[') {
		const char *close = strchr(v, ']');

		if (!close || close == v + 1 || close[1] != ':') {
			return -1;
		}
		colon = close + 1;
		sv->host = ast_strndup(v + 1, close - v - 1);
	} else {
		colon = strrchr(v, ':');
		if (!colon || colon == v || strchr(v, ':') != colon) {
			return -1;
		}
		sv->host = ast_strndup(v, colon - v);
	}
	if (!sv->host || parse_ll(colon + 1, 1, 65535, &port)) {
		ast_free(sv->host);
		sv->host = NULL;
		return -1;
	}
	sv->port = (int)port;
	return 0;
}

static int server_is_loopback(const struct resp_server *sv)
{
	struct in_addr a4;
	struct in6_addr a6;

	if (sv->path || !strcasecmp(sv->host, "localhost")) {
		return 1;
	}
	if (inet_pton(AF_INET, sv->host, &a4) == 1) {
		return (ntohl(a4.s_addr) >> 24) == 127;
	}
	if (inet_pton(AF_INET6, sv->host, &a6) == 1) {
		return IN6_IS_ADDR_LOOPBACK(&a6);
	}
	return 0;
}

static int set_string(char **dst, const char *v)
{
	ast_free(*dst);
	*dst = ast_strdup(v);
	return *dst ? 0 : -1;
}

/* @line 0: the refusal is about the file as a whole */
#define CONF_FAIL(line, ...) do { \
	ast_log(LOG_ERROR, PD_CONFIG ": " __VA_ARGS__); \
	if (line) { \
		ast_log(LOG_ERROR, PD_CONFIG ": ... at line %d\n", (line)); \
	} \
	conf_free(cf); \
	return NULL; \
} while (0)

static struct pd_conf *conf_parse(struct ast_config *cfg)
{
	struct pd_conf *cf;
	struct ast_variable *v;
	const char *cat = NULL;
	int nserv = 0, nsec = 0, i;
	long long x;

	while ((cat = ast_category_browse(cfg, cat))) {
		if (strcasecmp(cat, "general")) {
			ast_log(LOG_WARNING, PD_CONFIG ": section [%s] is not used; settings belong in [general]\n", cat);
		}
	}

	cf = ast_calloc(1, sizeof(*cf));
	if (!cf) {
		return NULL;
	}
	cf->protocol = PD_BINARY;
	cf->pool = 4;
	cf->connect_timeout_ms = 1000;
	cf->io_timeout_ms = 300;
	cf->wait_timeout_ms = 300;
	cf->default_ttl = 0;
	cf->max_value = 65536;
	cf->policy = PERFD_POLICY_FAILOVER;
	cf->spares = 0;
	cf->route_keys = 0;
	cf->keepalive_s = 30;

	for (v = ast_variable_browse(cfg, "general"); v; v = v->next) {
		if (!strcasecmp(v->name, "server")) {
			nserv++;
		} else if (!strcasecmp(v->name, "secret")) {
			nsec++;
		}
	}
	cf->servers = ast_calloc(nserv ? nserv : 1, sizeof(*cf->servers));
	cf->secrets = nsec ? ast_calloc(nsec + 1, sizeof(*cf->secrets)) : NULL;
	if (!cf->servers || (nsec && !cf->secrets)) {
		conf_free(cf);
		return NULL;
	}

	for (v = ast_variable_browse(cfg, "general"); v; v = v->next) {
		if (!strcasecmp(v->name, "protocol")) {
			if (!strcasecmp(v->value, "binary")) {
				cf->protocol = PD_BINARY;
			} else if (!strcasecmp(v->value, "resp")) {
				cf->protocol = PD_RESP;
			} else {
				CONF_FAIL(v->lineno, "protocol '%s' is neither binary nor resp\n", v->value);
			}
		} else if (!strcasecmp(v->name, "server")) {
			if (parse_server(v->value, &cf->servers[cf->nservers])) {
				CONF_FAIL(v->lineno, "server '%s' is not host:port, [v6]:port or unix:/path\n", v->value);
			}
			cf->nservers++;
		} else if (!strcasecmp(v->name, "secret")) {
			if (ast_strlen_zero(v->value)) {
				CONF_FAIL(v->lineno, "secret is empty\n");
			}
			cf->secrets[cf->nsecrets] = ast_strdup(v->value);
			if (!cf->secrets[cf->nsecrets]) {
				CONF_FAIL(0, "out of memory\n");
			}
			cf->nsecrets++;
		} else if (!strcasecmp(v->name, "password")) {
			if (set_string(&cf->password, v->value)) {
				CONF_FAIL(0, "out of memory\n");
			}
		} else if (!strcasecmp(v->name, "username")) {
			if (set_string(&cf->username, v->value)) {
				CONF_FAIL(0, "out of memory\n");
			}
		} else if (!strcasecmp(v->name, "collection")) {
			if (set_string(&cf->collection, v->value)) {
				CONF_FAIL(0, "out of memory\n");
			}
		} else if (!strcasecmp(v->name, "pool")) {
			if (parse_ll(v->value, 1, 64, &x)) {
				CONF_FAIL(v->lineno, "pool '%s' is not 1..64\n", v->value);
			}
			cf->pool = (int)x;
		} else if (!strcasecmp(v->name, "connect_timeout_ms")) {
			if (parse_ll(v->value, 1, 60000, &x)) {
				CONF_FAIL(v->lineno, "connect_timeout_ms '%s' is not 1..60000\n", v->value);
			}
			cf->connect_timeout_ms = (int)x;
		} else if (!strcasecmp(v->name, "io_timeout_ms")) {
			if (parse_ll(v->value, 1, 60000, &x)) {
				CONF_FAIL(v->lineno, "io_timeout_ms '%s' is not 1..60000\n", v->value);
			}
			cf->io_timeout_ms = (int)x;
		} else if (!strcasecmp(v->name, "wait_timeout_ms")) {
			if (parse_ll(v->value, 1, 60000, &x)) {
				CONF_FAIL(v->lineno, "wait_timeout_ms '%s' is not 1..60000\n", v->value);
			}
			cf->wait_timeout_ms = (int)x;
		} else if (!strcasecmp(v->name, "default_ttl")) {
			if (parse_ll(v->value, 0, INT_MAX, &x)) {
				CONF_FAIL(v->lineno, "default_ttl '%s' is not 0..%d seconds\n", v->value, INT_MAX);
			}
			cf->default_ttl = x;
		} else if (!strcasecmp(v->name, "max_value")) {
			if (parse_ll(v->value, 1, 64 * 1024 * 1024, &x)) {
				CONF_FAIL(v->lineno, "max_value '%s' is not 1..67108864 bytes\n", v->value);
			}
			cf->max_value = (size_t)x;
		} else if (!strcasecmp(v->name, "policy")) {
			if (!strcasecmp(v->value, "failover")) {
				cf->policy = PERFD_POLICY_FAILOVER;
			} else if (!strcasecmp(v->value, "round_robin")) {
				cf->policy = PERFD_POLICY_ROUND_ROBIN;
			} else if (!strcasecmp(v->value, "least_conn")) {
				cf->policy = PERFD_POLICY_LEAST_CONN;
			} else if (!strcasecmp(v->value, "weighted")) {
				cf->policy = PERFD_POLICY_WEIGHTED;
			} else {
				CONF_FAIL(v->lineno, "policy '%s' is not failover, round_robin, least_conn or weighted\n", v->value);
			}
		} else if (!strcasecmp(v->name, "spares")) {
			if (!strcasecmp(v->value, "none")) {
				cf->spares = PERFD_SPARES_NONE;
			} else if (parse_ll(v->value, -1, 64, &x)) {
				CONF_FAIL(v->lineno, "spares '%s' is not none or -1..64\n", v->value);
			} else {
				cf->spares = (int)x;
			}
		} else if (!strcasecmp(v->name, "route_keys")) {
			if (ast_true(v->value)) {
				cf->route_keys = 1;
			} else if (ast_false(v->value)) {
				cf->route_keys = 0;
			} else {
				CONF_FAIL(v->lineno, "route_keys '%s' is not yes or no\n", v->value);
			}
		} else if (!strcasecmp(v->name, "keepalive_s")) {
			if (parse_ll(v->value, -1, 86400, &x)) {
				CONF_FAIL(v->lineno, "keepalive_s '%s' is not -1..86400\n", v->value);
			}
			cf->keepalive_s = (int)x;
		} else {
			CONF_FAIL(v->lineno, "unknown setting '%s'\n", v->name);
		}
	}

	if (!cf->nservers) {
		CONF_FAIL(0, "no server is configured\n");
	}
	if (cf->protocol == PD_RESP && cf->nsecrets) {
		CONF_FAIL(0, "secret is for protocol = binary; RESP authenticates with password\n");
	}
	if (cf->protocol == PD_BINARY && (cf->password || cf->username)) {
		CONF_FAIL(0, "password and username are for protocol = resp; binary authenticates with secret\n");
	}
	if (cf->username && !cf->password) {
		CONF_FAIL(0, "username is set without password\n");
	}
	if (cf->protocol == PD_BINARY && !cf->nsecrets) {
		for (i = 0; i < cf->nservers; i++) {
			if (!server_is_loopback(&cf->servers[i])) {
				char name[160];

				server_name(&cf->servers[i], name, sizeof(name));
				CONF_FAIL(0, "server %s is not loopback and no secret is set; the binary door is encrypted off-loopback\n", name);
			}
		}
	}

	cf->resp.servers = cf->servers;
	cf->resp.nservers = cf->nservers;
	cf->resp.username = cf->username;
	cf->resp.password = cf->password;
	cf->resp.connect_timeout_ms = cf->connect_timeout_ms;
	cf->resp.io_timeout_ms = cf->io_timeout_ms;
	cf->resp.max_bulk = cf->max_value;
	return cf;
}

static void pool_destroy(void *obj)
{
	struct pd_pool *p = obj;
	int i;

	if (p->cf) {
		for (i = 0; i < p->cf->pool; i++) {
			if (p->conns[i].h) {
				perfd_free(p->conns[i].h);
			}
			resp_sess_free(p->conns[i].rs);
		}
	}
	ast_cond_destroy(&p->cond);
	ast_mutex_destroy(&p->lock);
	conf_free(p->cf);
}

/* takes @cf */
static struct pd_pool *pool_new(struct pd_conf *cf)
{
	struct pd_pool *p;
	int i;

	p = ao2_alloc_options(sizeof(*p) + cf->pool * sizeof(p->conns[0]),
		pool_destroy, AO2_ALLOC_OPT_LOCK_NOLOCK);
	if (!p) {
		conf_free(cf);
		return NULL;
	}
	ast_mutex_init(&p->lock);
	ast_cond_init(&p->cond, NULL);
	p->cf = cf;
	if (cf->protocol == PD_RESP) {
		for (i = 0; i < cf->pool; i++) {
			p->conns[i].rs = resp_sess_new(&cf->resp);
			if (!p->conns[i].rs) {
				ao2_ref(p, -1);
				return NULL;
			}
		}
	}
	return p;
}

static int bin_open(const struct pd_conf *cf, struct pd_conn *c, char *err, size_t cap)
{
	perfd_opts o;
	char name[160];
	int k;

	memset(&o, 0, sizeof(o));
	o.secrets = cf->nsecrets ? (const char *const *)cf->secrets : NULL;
	o.connect_timeout_ms = cf->connect_timeout_ms;
	o.io_timeout_ms = cf->io_timeout_ms;
	o.binary = 1;
	o.policy = cf->policy;
	o.spares = cf->spares;
	o.route_keys = cf->route_keys;
	o.keepalive_s = cf->keepalive_s;

	ast_mutex_lock(&pd_dial_lock);
	for (k = 0; k < cf->nservers; k++) {
		int i = (c->next_server + k) % cf->nservers;
		const struct resp_server *sv = &cf->servers[i];
		perfd_t *h;

		server_name(sv, name, sizeof(name));
		h = sv->path ? perfd_connect_unix(sv->path, &o)
			: perfd_connect(sv->host, sv->port, &o);
		if (h) {
			ast_mutex_unlock(&pd_dial_lock);
			c->h = h;
			c->next_server = (i + 1) % cf->nservers;
			ast_copy_string(c->seed, name, sizeof(c->seed));
			return 0;
		}
		snprintf(err, cap, "%s: %s", name, perfd_error(NULL));
	}
	ast_mutex_unlock(&pd_dial_lock);
	return -1;
}

static void bin_close(struct pd_conn *c)
{
	if (!c->h) {
		return;
	}
	c->failovers_done += perfd_failovers(c->h);
	c->failovers_live = 0;
	perfd_free(c->h);
	c->h = NULL;
}

/* a transport failure poisons the handle: re-dial it, or drop it for the next borrow */
static void bin_after_error(struct pd_conn *c)
{
	int rc;

	if (perfd_state(c->h) != PERFD_ST_FAILED) {
		return;
	}
	ast_mutex_lock(&pd_dial_lock);
	rc = perfd_redial(c->h);
	ast_mutex_unlock(&pd_dial_lock);
	if (rc == 0) {
		c->pending_reconnects++;
	} else {
		bin_close(c);
	}
}

/* 1 hit / stored / exists / removed; 0 miss / absent; -1 failure; -2 value refused */
static int op_binary(const struct pd_conf *cf, struct pd_conn *c, int verb,
	const char *col, const char *key, const char *val, size_t vlen, long long ttl,
	char **vout, size_t *vlout, char *err, size_t cap)
{
	void *v = NULL;
	size_t n = 0;
	int rc;

	if (!c->h) {
		if (bin_open(cf, c, err, cap)) {
			return -1;
		}
		if (c->opened_before) {
			c->pending_reconnects++;
		}
		c->opened_before = 1;
	}
	switch (verb) {
	case PD_GET:
		rc = perfd_get(c->h, col, key, &v, &n, NULL);
		if (rc == 1) {
			*vout = v;
			*vlout = n;
		}
		break;
	case PD_SET:
		rc = perfd_set(c->h, col, key, val, vlen, ttl) == 0 ? 1 : -1;
		break;
	case PD_EXISTS:
		rc = perfd_exists(c->h, col, key);
		break;
	case PD_DELETE:
		rc = perfd_del(c->h, col, key);
		break;
	default:
		rc = -1;
		break;
	}
	if (rc < 0) {
		snprintf(err, cap, "%s", perfd_error(c->h));
		bin_after_error(c);
		return -1;
	}
	c->failovers_live = perfd_failovers(c->h);
	return rc;
}

static int op_resp(const struct pd_conf *cf, struct pd_conn *c, int verb,
	const char *col, const char *key, const char *val, size_t vlen, long long ttl,
	char **vout, size_t *vlout, char *err, size_t cap)
{
	const char *argv[5];
	size_t lens[5];
	char ttlbuf[24];
	struct resp_reply r;
	int argc = 0, rc;

	switch (verb) {
	case PD_GET:
		argv[argc] = "GET";
		break;
	case PD_SET:
		argv[argc] = "SET";
		break;
	case PD_EXISTS:
		argv[argc] = "EXISTS";
		break;
	default:
		argv[argc] = "DEL";
		break;
	}
	lens[argc] = strlen(argv[argc]);
	argc++;
	argv[argc] = key;
	lens[argc++] = strlen(key);
	if (verb == PD_SET) {
		argv[argc] = val;
		lens[argc++] = vlen;
		if (ttl > 0) {
			snprintf(ttlbuf, sizeof(ttlbuf), "%lld", ttl);
			argv[argc] = "EX";
			lens[argc++] = 2;
			argv[argc] = ttlbuf;
			lens[argc++] = strlen(ttlbuf);
		}
	}

	if (resp_sess_call(c->rs, col, argc, argv, lens, &r, err, cap)) {
		return -1;
	}
	rc = -1;
	if (r.type == RESP_T_ERROR) {
		snprintf(err, cap, "%s: %s", resp_sess_server(c->rs), r.str);
	} else if (verb == PD_GET && r.type == RESP_T_NIL) {
		rc = 0;
	} else if (verb == PD_GET && r.type == RESP_T_BULK && r.oversize) {
		snprintf(err, cap, "value of %zu bytes is longer than max_value (%zu)", r.len, cf->max_value);
		rc = -2;
	} else if (verb == PD_GET && r.type == RESP_T_BULK) {
		*vout = r.str;
		*vlout = r.len;
		r.str = NULL;
		rc = 1;
	} else if (verb == PD_SET && r.type == RESP_T_STATUS) {
		rc = 1;
	} else if ((verb == PD_EXISTS || verb == PD_DELETE) && r.type == RESP_T_INTEGER) {
		rc = r.integer > 0 ? 1 : 0;
	} else {
		snprintf(err, cap, "%s: unexpected reply type %d", resp_sess_server(c->rs), r.type);
	}
	resp_reply_free(&r);
	return rc;
}

static struct pd_conn *pool_borrow(struct pd_pool *p)
{
	struct timeval tv;
	struct timespec ts;
	struct pd_conn *c = NULL;
	int i, rc = 0;

	tv = ast_tvadd(ast_tvnow(), ast_tv(p->cf->wait_timeout_ms / 1000,
		(p->cf->wait_timeout_ms % 1000) * 1000));
	ts.tv_sec = tv.tv_sec;
	ts.tv_nsec = tv.tv_usec * 1000;

	ast_mutex_lock(&p->lock);
	for (;;) {
		for (i = 0; i < p->cf->pool && !c; i++) {
			if (!p->conns[i].in_use && p->conns[i].snap_open) {
				c = &p->conns[i];
			}
		}
		for (i = 0; i < p->cf->pool && !c; i++) {
			if (!p->conns[i].in_use) {
				c = &p->conns[i];
			}
		}
		if (c || rc) {
			break;
		}
		p->waiters++;
		rc = ast_cond_timedwait(&p->cond, &p->lock, &ts);
		p->waiters--;
	}
	if (c) {
		c->in_use = 1;
		p->in_use++;
	} else {
		p->st.wait_timeouts++;
	}
	ast_mutex_unlock(&p->lock);
	return c;
}

static void conn_snapshot(const struct pd_conf *cf, struct pd_conn *c,
	int *open, int *node, char *where, size_t cap)
{
	if (cf->protocol == PD_BINARY) {
		*open = c->h != NULL;
		*node = c->h ? perfd_active_node(c->h) : 0;
		ast_copy_string(where, c->h ? c->seed : "", cap);
	} else {
		*open = resp_sess_is_open(c->rs);
		*node = 0;
		ast_copy_string(where, resp_sess_server(c->rs), cap);
	}
}

static void pool_release(struct pd_pool *p, struct pd_conn *c, int verb, int rc)
{
	char where[128];
	int open, node;
	unsigned long long rr = 0;

	conn_snapshot(p->cf, c, &open, &node, where, sizeof(where));
	if (c->rs) {
		rr = resp_sess_reconnects(c->rs);
	}

	ast_mutex_lock(&p->lock);
	if (rc < 0) {
		p->st.errors++;
	} else if (verb == PD_GET) {
		if (rc) {
			p->st.get_hits++;
		} else {
			p->st.get_misses++;
		}
	} else if (verb == PD_SET) {
		p->st.sets++;
	} else if (verb == PD_EXISTS) {
		if (rc) {
			p->st.exists_yes++;
		} else {
			p->st.exists_no++;
		}
	} else if (verb == PD_DELETE) {
		if (rc) {
			p->st.del_removed++;
		} else {
			p->st.del_absent++;
		}
	}
	p->st.reconnects += c->pending_reconnects + (rr - c->resp_reconnects_seen);
	c->pending_reconnects = 0;
	c->resp_reconnects_seen = rr;
	c->snap_open = open;
	c->snap_node = node;
	c->snap_failovers = c->failovers_done + c->failovers_live;
	ast_copy_string(c->snap_where, where, sizeof(c->snap_where));
	c->in_use = 0;
	p->in_use--;
	if (p->waiters) {
		ast_cond_signal(&p->cond);
	}
	ast_mutex_unlock(&p->lock);
}

/* open the connections one at a time before the pool is published */
static void pool_warm(struct pd_pool *p)
{
	char err[512];
	int i, rc;

	for (i = 0; i < p->cf->pool; i++) {
		struct pd_conn *c = &p->conns[i];

		err[0] = '\0';
		if (p->cf->protocol == PD_BINARY) {
			rc = bin_open(p->cf, c, err, sizeof(err));
			if (!rc) {
				c->opened_before = 1;
			}
		} else {
			rc = resp_sess_open(c->rs, p->cf->collection, err, sizeof(err));
			if (rc > 0) {
				ast_log(LOG_WARNING, "func_perfd: %s\n", err);
				rc = 0;
			}
		}
		if (rc) {
			ast_log(LOG_WARNING, "func_perfd: no server answered at load (%s); connections open on first use\n", err);
			break;
		}
		conn_snapshot(p->cf, c, &c->snap_open, &c->snap_node, c->snap_where, sizeof(c->snap_where));
	}
}

static const char *verb_name(int verb)
{
	switch (verb) {
	case PD_GET:
		return "PERFD_GET";
	case PD_SET:
		return "PERFD_SET";
	case PD_EXISTS:
		return "PERFD_EXISTS";
	default:
		return "PERFD_DELETE";
	}
}

/*
 * The one path every function takes.  GET: OK hands back a malloc'd value in
 * @vout (ast_std_free it).  EXISTS / DELETE: OK means 1, NOTFOUND means 0.
 */
static enum pd_status pd_do(struct ast_channel *chan, int verb, char *data,
	const char *value, char **vout, size_t *vlout)
{
	struct pd_pool *p;
	struct pd_conn *c;
	const char *col;
	char *val = NULL;
	size_t vlen = 0, setlen = 0;
	long long ttl = 0;
	char err[512], empty[1] = "";
	int rc;
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(collection);
		AST_APP_ARG(key);
		AST_APP_ARG(ttl);
	);

	p = ao2_global_obj_ref(pd_pool_global);
	if (!p) {
		pd_warn("%s: func_perfd has no usable " PD_CONFIG "\n", verb_name(verb));
		return PD_ERROR;
	}
	if (!data) {
		data = empty;
	}
	AST_STANDARD_APP_ARGS(args, data);
	col = !ast_strlen_zero(args.collection) ? args.collection : p->cf->collection;
	if (ast_strlen_zero(col)) {
		pd_warn("%s: no collection given, and " PD_CONFIG " sets none\n", verb_name(verb));
		ao2_ref(p, -1);
		return PD_ERROR;
	}
	if (ast_strlen_zero(args.key)) {
		pd_warn("%s(%s): a key is required\n", verb_name(verb), col);
		ao2_ref(p, -1);
		return PD_ERROR;
	}
	if (verb == PD_SET) {
		ttl = p->cf->default_ttl;
		if (!ast_strlen_zero(args.ttl) && parse_ll(args.ttl, 0, INT_MAX, &ttl)) {
			pd_warn("PERFD_SET(%s,%s): ttl '%s' is not 0..%d seconds\n", col, args.key, args.ttl, INT_MAX);
			ao2_ref(p, -1);
			return PD_ERROR;
		}
		setlen = strlen(S_OR(value, ""));
		if (setlen > p->cf->max_value) {
			pd_warn("PERFD_SET(%s,%s): value of %zu bytes is longer than max_value (%zu)\n",
				col, args.key, setlen, p->cf->max_value);
			ao2_ref(p, -1);
			return PD_ERROR;
		}
	} else if (args.argc > 2) {
		pd_warn("%s(%s,%s): takes (collection,key) only\n", verb_name(verb), col, args.key);
		ao2_ref(p, -1);
		return PD_ERROR;
	}

	if (chan) {
		ast_autoservice_start(chan);
	}
	c = pool_borrow(p);
	if (!c) {
		snprintf(err, sizeof(err), "no free connection within wait_timeout_ms (%d)", p->cf->wait_timeout_ms);
		rc = -1;
	} else {
		err[0] = '\0';
		if (p->cf->protocol == PD_BINARY) {
			rc = op_binary(p->cf, c, verb, col, args.key, S_OR(value, ""), setlen, ttl,
				&val, &vlen, err, sizeof(err));
		} else {
			rc = op_resp(p->cf, c, verb, col, args.key, S_OR(value, ""), setlen, ttl,
				&val, &vlen, err, sizeof(err));
		}
		if (rc == 1 && verb == PD_GET) {
			if (vlen > p->cf->max_value) {
				snprintf(err, sizeof(err), "value of %zu bytes is longer than max_value (%zu)",
					vlen, p->cf->max_value);
				rc = -2;
			} else if (memchr(val, '\0', vlen)) {
				snprintf(err, sizeof(err), "value of %zu bytes contains a NUL byte", vlen);
				rc = -2;
			}
		}
		pool_release(p, c, verb, rc);
	}
	if (chan) {
		ast_autoservice_stop(chan);
	}

	if (rc < 0) {
		pd_warn("%s(%s,%s): %s\n", verb_name(verb), col, args.key, err);
		ast_std_free(val);
		ao2_ref(p, -1);
		return PD_ERROR;
	}
	ao2_ref(p, -1);
	if (verb == PD_GET && rc == 1) {
		*vout = val;
		*vlout = vlen;
	}
	return rc ? PD_OK : PD_NOTFOUND;
}

static void set_status(struct ast_channel *chan, enum pd_status st)
{
	if (chan) {
		pbx_builtin_setvar_helper(chan, "PERFDSTATUS", pd_status_names[st]);
	}
}

static int done(struct ast_channel *chan, enum pd_status st)
{
	set_status(chan, st);
	return st == PD_ERROR && !chan ? -1 : 0;
}

static int get_read(struct ast_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	char *val = NULL;
	size_t vlen = 0;
	enum pd_status st;

	st = pd_do(chan, PD_GET, data, NULL, &val, &vlen);
	buf[0] = '\0';
	if (st == PD_OK) {
		if (vlen >= len) {
			pd_warn("PERFD_GET: value of %zu bytes does not fit this caller's %zu-byte buffer\n",
				vlen, len);
			st = PD_ERROR;
		} else {
			memcpy(buf, val, vlen);
			buf[vlen] = '\0';
		}
	}
	ast_std_free(val);
	return done(chan, st);
}

static int set_write(struct ast_channel *chan, const char *cmd, char *data, const char *value)
{
	return done(chan, pd_do(chan, PD_SET, data, value, NULL, NULL));
}

static int answer_read(struct ast_channel *chan, int verb, char *data, char *buf, size_t len)
{
	enum pd_status st = pd_do(chan, verb, data, NULL, NULL, NULL);

	ast_copy_string(buf, st == PD_OK ? "1" : st == PD_NOTFOUND ? "0" : "", len);
	return done(chan, st);
}

static int exists_read(struct ast_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	return answer_read(chan, PD_EXISTS, data, buf, len);
}

static int delete_read(struct ast_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	return answer_read(chan, PD_DELETE, data, buf, len);
}

static int delete_write(struct ast_channel *chan, const char *cmd, char *data, const char *value)
{
	char buf[4];

	return answer_read(chan, PD_DELETE, data, buf, sizeof(buf));
}

static struct ast_custom_function get_function = {
	.name = "PERFD_GET",
	.read = get_read,
};

static struct ast_custom_function set_function = {
	.name = "PERFD_SET",
	.write = set_write,
};

static struct ast_custom_function exists_function = {
	.name = "PERFD_EXISTS",
	.read = exists_read,
	.read_max = 2,
};

static struct ast_custom_function delete_function = {
	.name = "PERFD_DELETE",
	.read = delete_read,
	.write = delete_write,
	.read_max = 2,
};

static char *handle_show_status(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	struct pd_pool *p;
	struct pd_stats st;
	struct pd_conn *snaps;
	unsigned long long failovers = 0;
	int i, n, in_use, waiters;

	switch (cmd) {
	case CLI_INIT:
		e->command = "perfd show status";
		e->usage =
			"Usage: perfd show status\n"
			"       Shows func_perfd's protocol, servers, connection pool and counters.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	if (a->argc != 3) {
		return CLI_SHOWUSAGE;
	}
	p = ao2_global_obj_ref(pd_pool_global);
	if (!p) {
		ast_cli(a->fd, "func_perfd has no usable " PD_CONFIG "\n");
		return CLI_SUCCESS;
	}
	n = p->cf->pool;
	snaps = ast_calloc(n, sizeof(*snaps));
	if (!snaps) {
		ao2_ref(p, -1);
		return CLI_FAILURE;
	}
	ast_mutex_lock(&p->lock);
	st = p->st;
	in_use = p->in_use;
	waiters = p->waiters;
	for (i = 0; i < n; i++) {
		snaps[i].in_use = p->conns[i].in_use;
		snaps[i].snap_open = p->conns[i].snap_open;
		snaps[i].snap_node = p->conns[i].snap_node;
		snaps[i].snap_failovers = p->conns[i].snap_failovers;
		ast_copy_string(snaps[i].snap_where, p->conns[i].snap_where, sizeof(snaps[i].snap_where));
		failovers += p->conns[i].snap_failovers;
	}
	ast_mutex_unlock(&p->lock);

	ast_cli(a->fd, "Protocol:        %s\n", p->cf->protocol == PD_BINARY ? "binary" : "resp");
	for (i = 0; i < p->cf->nservers; i++) {
		char name[160];

		server_name(&p->cf->servers[i], name, sizeof(name));
		ast_cli(a->fd, "Server %d:        %s\n", i, name);
	}
	ast_cli(a->fd, "Pool:            %d connections, %d in use, %d waiting\n", n, in_use, waiters);
	for (i = 0; i < n; i++) {
		const struct pd_conn *s = &snaps[i];

		if (s->in_use) {
			ast_cli(a->fd, "Connection %d:    in use\n", i);
		} else if (!s->snap_open) {
			ast_cli(a->fd, "Connection %d:    closed\n", i);
		} else if (p->cf->protocol == PD_BINARY) {
			ast_cli(a->fd, "Connection %d:    open via %s, node %d, failovers %llu\n",
				i, s->snap_where, s->snap_node, s->snap_failovers);
		} else {
			ast_cli(a->fd, "Connection %d:    open to %s\n", i, s->snap_where);
		}
	}
	ast_cli(a->fd, "get_hits         %llu\n", st.get_hits);
	ast_cli(a->fd, "get_misses       %llu\n", st.get_misses);
	ast_cli(a->fd, "sets             %llu\n", st.sets);
	ast_cli(a->fd, "exists_yes       %llu\n", st.exists_yes);
	ast_cli(a->fd, "exists_no        %llu\n", st.exists_no);
	ast_cli(a->fd, "deletes_removed  %llu\n", st.del_removed);
	ast_cli(a->fd, "deletes_absent   %llu\n", st.del_absent);
	ast_cli(a->fd, "errors           %llu\n", st.errors);
	ast_cli(a->fd, "wait_timeouts    %llu\n", st.wait_timeouts);
	ast_cli(a->fd, "reconnects       %llu\n", st.reconnects);
	ast_cli(a->fd, "failovers        %llu\n", failovers);
	ast_free(snaps);
	ao2_ref(p, -1);
	return CLI_SUCCESS;
}

static struct ast_cli_entry pd_cli[] = {
	AST_CLI_DEFINE(handle_show_status, "Show func_perfd's pool and counters"),
};

static int load_config(int reload)
{
	struct ast_flags flags = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };
	struct ast_config *cfg;
	struct pd_conf *cf;
	struct pd_pool *p;

	cfg = ast_config_load(PD_CONFIG, flags);
	if (cfg == CONFIG_STATUS_FILEUNCHANGED) {
		return 0;
	}
	if (cfg == CONFIG_STATUS_FILEMISSING) {
		ast_log(LOG_ERROR, PD_CONFIG " is missing; func_perfd needs it\n");
		return -1;
	}
	if (cfg == CONFIG_STATUS_FILEINVALID) {
		ast_log(LOG_ERROR, PD_CONFIG " could not be parsed\n");
		return -1;
	}
	cf = conf_parse(cfg);
	ast_config_destroy(cfg);
	if (!cf) {
		ast_log(LOG_ERROR, "func_perfd: " PD_CONFIG " refused%s\n",
			reload ? "; the running configuration stays" : "");
		return -1;
	}
	p = pool_new(cf);
	if (!p) {
		return -1;
	}
	pool_warm(p);
	ao2_global_obj_replace_unref(pd_pool_global, p);
	ast_verb(2, "func_perfd: protocol %s, %d server(s), pool of %d\n",
		p->cf->protocol == PD_BINARY ? "binary" : "resp", p->cf->nservers, p->cf->pool);
	ao2_ref(p, -1);
	return 0;
}

static int unload_module(void)
{
	int res = 0;

	res |= ast_custom_function_unregister(&get_function);
	res |= ast_custom_function_unregister(&set_function);
	res |= ast_custom_function_unregister(&exists_function);
	res |= ast_custom_function_unregister(&delete_function);
	ast_cli_unregister_multiple(pd_cli, ARRAY_LEN(pd_cli));
	ao2_global_obj_release(pd_pool_global);
	return res;
}

static int load_module(void)
{
	int res = 0;

	if (load_config(0)) {
		return AST_MODULE_LOAD_DECLINE;
	}
	res |= ast_custom_function_register(&get_function);
	res |= ast_custom_function_register_escalating(&set_function, AST_CFE_WRITE);
	res |= ast_custom_function_register(&exists_function);
	res |= ast_custom_function_register_escalating(&delete_function, AST_CFE_BOTH);
	res |= ast_cli_register_multiple(pd_cli, ARRAY_LEN(pd_cli));
	if (res) {
		unload_module();
		return AST_MODULE_LOAD_DECLINE;
	}
	return AST_MODULE_LOAD_SUCCESS;
}

static int reload_module(void)
{
	return load_config(1) ? AST_MODULE_LOAD_DECLINE : AST_MODULE_LOAD_SUCCESS;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "perfcached dialplan functions",
	.support_level = AST_MODULE_SUPPORT_EXTENDED,
	.load = load_module,
	.unload = unload_module,
	.reload = reload_module,
);
