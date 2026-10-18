/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * config.c — INI-style config parser + strict validation (task S5).
 * See config.h for the contract and the hard rules.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sodium.h>


#include "compat/dprint.h"
#include "config.h"
#include "colname.h"
#include "events.h"
#include "relkey.h"
#include <strings.h>

/* S153: the log_events list parser.  It lives here, not in events.c,
 * because wipetest links config.c on its own - a parser is config's job,
 * and events.c is the consumer of the mask, not its source. */
int pc_ev_parse(const char *list, unsigned int *mask)
{
	char *dup, *tok, *save;
	unsigned int m = 0;

	*mask = 0;
	if (!list || !*list)
		return 0;
	dup = strdup(list);
	if (!dup)
		return -1;
	for (tok = strtok_r(dup, ", \t", &save); tok;
	     tok = strtok_r(NULL, ", \t", &save)) {
		if (!strcasecmp(tok, "miss"))         m |= PC_EV_MISS;
		else if (!strcasecmp(tok, "expired")) m |= PC_EV_EXPIRED;
		else if (!strcasecmp(tok, "store"))   m |= PC_EV_STORE;
		else if (!strcasecmp(tok, "remove"))  m |= PC_EV_REMOVE;
		else if (!strcasecmp(tok, "all"))     m |= PC_EV_ALL;
		else if (!strcasecmp(tok, "none"))    m = 0;
		else { free(dup); return -1; }
	}
	free(dup);
	*mask = m;
	return 0;
}

enum sect { S_NONE, S_DAEMON, S_MEMORY, S_SECRETS, S_LISTEN, S_WAL, S_CLUSTER, S_COLLECTION, S_MAIL,
	S_ALERTS, S_UPGRADE };

struct pstate {
	const char *path;
	int line;
	enum sect sect;
	struct pc_collection *col;    /* current [collection X] */
	struct pc_config *cfg;
};

#define PERR(ps, ...) do { \
	char _pe[512]; \
	snprintf(_pe, sizeof _pe, __VA_ARGS__); \
	if ((ps)->line) \
		compat_log(L_ERR, "ERROR", "%s:%d: %s", (ps)->path, \
			(ps)->line, _pe); \
	else \
		compat_log(L_ERR, "ERROR", "%s: %s", (ps)->path, _pe); \
} while (0)

static char *trim(char *s)
{
	char *e;

	while (*s == ' ' || *s == '\t')
		s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' ||
	        e[-1] == '\r'))
		*--e = 0;
	return s;
}

/* value: rest of line; optional double quotes protect '#' and spaces */
static char *parse_value(struct pstate *ps, char *v)
{
	char *e;

	v = trim(v);
	if (*v == '"') {
		e = strrchr(v + 1, '"');
		if (!e || trim(e + 1)[0]) {
			PERR(ps, "unterminated or trailing-garbage quoted value\n");
			return NULL;
		}
		*e = 0;
		return v + 1;
	}
	e = strchr(v, '#');
	if (e)
		*e = 0;
	return trim(v);
}

static int parse_int(struct pstate *ps, const char *v, int lo, int hi,
		int *out)
{
	char *end;
	long n = strtol(v, &end, 10);

	if (!*v || *end || n < lo || n > hi) {
		PERR(ps, "'%s' is not an integer in [%d..%d]\n", v, lo, hi);
		return -1;
	}
	*out = (int)n;
	return 0;
}

static int level_of(const char *v)
{
	if (!strcasecmp(v, "crit"))   return L_CRIT;
	if (!strcasecmp(v, "err"))    return L_ERR;
	if (!strcasecmp(v, "warn"))   return L_WARN;
	if (!strcasecmp(v, "notice")) return L_NOTICE;
	if (!strcasecmp(v, "info"))   return L_INFO;
	if (!strcasecmp(v, "dbg"))    return L_DBG;
	return -100;
}

/* S343: the one rule (colname.h) - the config's own check was it, less
 * the leading '-' */
static int name_ok(const char *s)
{
	return pc_col_name_ok(s, strlen(s));
}

/* S352: smtp://host[:port][/] or smtps://host[:port][/] - no credentials
 * in the URL (they belong in user = and [secrets] mail), no path, no query */
static int mail_url_ok(struct pstate *ps, const char *url)
{
	const char *h, *e;

	if (!strncasecmp(url, "smtp://", 7))
		h = url + 7;
	else if (!strncasecmp(url, "smtps://", 8))
		h = url + 8;
	else {
		PERR(ps, "smtp = '%s': want smtp://host:port (STARTTLS) or "
			"smtps://host:port (TLS)\n", url);
		return 0;
	}
	if (strchr(h, '@')) {
		PERR(ps, "smtp: no credentials in the URL - user = in [mail], "
			"the password as [secrets] mail\n");
		return 0;
	}
	e = h + strcspn(h, "/?#");
	if (e == h || *h == ':') {
		PERR(ps, "smtp = '%s': no host\n", url);
		return 0;
	}
	if (*e && (e[0] != '/' || e[1])) {
		PERR(ps, "smtp = '%s': a host and a port only - no path or "
			"query\n", url);
		return 0;
	}
	return 1;
}

/* S352: local@domain.tld in printable ASCII, no spaces, brackets or
 * separators - enough to catch a typo or a pasted display name */
static int mail_addr_ok(const char *a)
{
	const char *at = strchr(a, '@'), *p;
	size_t n = strlen(a);

	if (!at || at == a || !at[1] || strchr(at + 1, '@') || n >= 255)
		return 0;
	if (!strchr(at + 1, '.') || at[1] == '.' || a[n - 1] == '.')
		return 0;
	for (p = a; *p; p++)
		if (*p <= ' ' || *p > '~' || *p == '<' || *p == '>' ||
		    *p == ',' || *p == ';' || *p == '"' || *p == '(' || *p == ')')
			return 0;
	return 1;
}

static int enter_section(struct pstate *ps, char *hdr)
{
	struct pc_config *cfg = ps->cfg;
	char *arg;

	hdr = trim(hdr);
	arg = strchr(hdr, ' ');
	if (arg)
		*arg++ = 0, arg = trim(arg);

	ps->col = NULL;
	if (!strcasecmp(hdr, "daemon") && !arg) {
		ps->sect = S_DAEMON;
	} else if (!strcasecmp(hdr, "memory") && !arg) {
		ps->sect = S_MEMORY;
	} else if (!strcasecmp(hdr, "secrets") && !arg) {
		ps->sect = S_SECRETS;
	} else if (!strcasecmp(hdr, "listen") && !arg) {
		ps->sect = S_LISTEN;
	} else if (!strcasecmp(hdr, "wal") && !arg) {
		ps->sect = S_WAL;
	} else if (!strcasecmp(hdr, "cluster") && !arg) {
#ifdef PC_EDITION_STANDALONE
		/* S250: refused, never ignored - a node that silently ran
		 * alone would look like a fleet member to its operator */
		PERR(ps, "[cluster]: this is the STANDALONE build of perfcached "
			"- no cluster code is compiled in.  Remove the section, "
			"or build the clustered edition (make menuconfig, then "
			"make)\n");
		return -1;
#endif
		ps->sect = S_CLUSTER;
		ps->cfg->cl_enabled = 1;
	} else if (!strcasecmp(hdr, "mail") && !arg) {
#ifndef PC_MAIL_CURL
		/* S352: refused, never ignored - an operator who wrote
		 * [mail] expects mail, and a build that cannot send must
		 * say so at the check, not stay silent at the incident */
		PERR(ps, "[mail]: %s\n", PC_MAIL_NOT_BUILT);
		return -1;
#endif
		ps->sect = S_MAIL;
		cfg->mail_on = 1;
	} else if (!strcasecmp(hdr, "alerts") && !arg) {     /* S380 */
		ps->sect = S_ALERTS;
	} else if (!strcasecmp(hdr, "upgrade") && !arg) {    /* UP6n */
		ps->sect = S_UPGRADE;
	} else if (!strcasecmp(hdr, "collection")) {
		if (!arg || !name_ok(arg)) {
			PERR(ps, "[collection <name>]: name missing or invalid "
				"(" PC_COL_NAME_RULE ")\n");
			return -1;
		}
		if (cfg->n_col >= PC_MAX_COLLECTIONS) {
			PERR(ps, "more than %d collections\n", PC_MAX_COLLECTIONS);
			return -1;
		}
		ps->sect = S_COLLECTION;
		ps->col = &cfg->col[cfg->n_col++];
		ps->col->name = strdup(arg);
		ps->col->buckets_log2 = -1;
		ps->col->mode = PC_MODE_STORE;
		ps->col->up_negative_ms = 2000;          /* S331 */
		ps->col->up_timeout_ms = 500;
		ps->col->up_conns = 2;
		ps->col->up_tracking = 1;
		ps->col->up_on_error = PC_UP_ON_ERROR_ERROR;
		ps->col->up_drain_rate = 5000;
		/* S401: what keeps an upfill sweep within a quarter of the
		 * writer thread at any value size (measured on 224, DESIGN
		 * 12nl) - 5,000 keys a second alone already hold small values
		 * there */
		ps->col->up_drain_mb_rate = 16;
		ps->col->up_pass_max_kb = 1024;        /* S385 */
		ps->col->up_write_queue = 10000;       /* AR2 */
		ps->col->up_preempt = 1;                 /* S382: the operator's */
		ps->col->up_preempt_hold_s = 30;
	} else {
		PERR(ps, "unknown section [%s]\n", hdr);
		return -1;
	}
	return 0;
}

/* "store|eager|proxy|shard" -> enum; -1 = not a mode word.
 *
 * eager is spelled as a MODE rather than as a second parameter because
 * the config never allowed it anywhere else: it was refused beside
 * proxy and beside shard, so the pair named four states and not six.
 * Carried as an attribute it went missing from every report - the
 * built-in page called a fully eager fleet "all in store mode" while
 * every node was replicating every record to every peer.
 *
 * Internally it stays PC_MODE_STORE plus a flag, which is what the
 * cluster config digest and the ALIVE wire already carry, so a node
 * running `mode = eager` produces the same digest as an older node
 * running `mode = store` + `eager = 1` and a rolling upgrade cannot
 * split the cluster on it.
 *
 * @eager receives 1 for the eager spelling; NULL means eager is not
 * accepted at this call site.
 */
static int mode_of(const char *v, int *eager)
{
	if (eager)
		*eager = 0;
	if (!strcasecmp(v, "store"))
		return PC_MODE_STORE;
	if (!strcasecmp(v, "eager")) {
		if (!eager)
			return -1;
		*eager = 1;
		return PC_MODE_STORE;
	}
	if (!strcasecmp(v, "proxy"))
		return PC_MODE_PROXY;
	if (!strcasecmp(v, "shard"))
		return PC_MODE_SHARD;
	if (!strcasecmp(v, "spread"))
		return PC_MODE_SPREAD;
	return -1;
}

static const char *mode_name(enum pc_pull_mode m, int eager)
{
	switch (m) {
	case PC_MODE_PROXY: return "proxy";
	case PC_MODE_SHARD: return "shard";
	case PC_MODE_SPREAD: return "spread";
	default:            return eager ? "eager" : "store";
	}
}

/* @kind: 0 native, 1 RESP-only (S33), 2 HTTP metrics (S46) */
static int add_listener_ex(struct pstate *ps, enum pc_listen_type type,
		const char *v, int kind)
{
	struct pc_config *cfg = ps->cfg;
	struct pc_listener *l;
	const char *colon;
	char *addr;
	int port = 0;

	if (cfg->n_listen >= PC_MAX_LISTEN) {
		PERR(ps, "more than %d listeners\n", PC_MAX_LISTEN);
		return -1;
	}
	l = &cfg->listen[cfg->n_listen];

	if (type == PC_LISTEN_UNIX) {
		if (v[0] != '/') {
			PERR(ps, "unix socket path must be absolute\n");
			return -1;
		}
		addr = strdup(v);
	} else {
		colon = strrchr(v, ':');
		if (!colon || colon == v) {
			PERR(ps, "tcp listener must be host:port\n");
			return -1;
		}
		if (parse_int(ps, colon + 1, 1, 65535, &port))
			return -1;
		addr = strndup(v, (size_t)(colon - v));
	}
	l->type = type;
	l->addr = addr;
	l->port = port;
	l->resp = kind == 1;
	l->http = kind == 2;
	/* loopback classification decides plaintext ELIGIBILITY only */
	l->loopback = type == PC_LISTEN_UNIX ||
		!strncmp(addr, "127.", 4) || !strcmp(addr, "::1") ||
		!strcmp(addr, "localhost");
	cfg->n_listen++;
	return 0;
}

static int add_listener(struct pstate *ps, enum pc_listen_type type,
		const char *v)
{
	return add_listener_ex(ps, type, v, 0);
}

/* one "a.b.c.d/len" (or a bare address = /32) into an allow-list.
 * Shared by resp_allow and http_allow: both guard a listener that
 * has no handshake, so the network is its authentication. */
static int add_allow(struct pstate *ps, const char *v, const char *what,
		struct pc_cidr *list, int *n)
{
	char buf[64], *slash;
	struct in_addr ia;
	int len = 32;

	if (*n >= PC_MAX_RESP_ALLOW) {
		PERR(ps, "more than %d %s entries\n", PC_MAX_RESP_ALLOW, what);
		return -1;
	}
	if (strlen(v) >= sizeof buf) {
		PERR(ps, "%s entry too long\n", what);
		return -1;
	}
	strcpy(buf, v);
	slash = strchr(buf, '/');
	if (slash) {
		*slash = 0;
		if (parse_int(ps, slash + 1, 0, 32, &len))
			return -1;
	}
	if (!inet_aton(buf, &ia)) {
		PERR(ps, "%s: '%s' is not an IPv4 address or CIDR\n", what, buf);
		return -1;
	}
	list[*n].mask = len ? htonl(0xFFFFFFFFu << (32 - len)) : 0;
	list[*n].net = ia.s_addr & list[*n].mask;
	(*n)++;
	return 0;
}

static int add_resp_allow(struct pstate *ps, const char *v)
{
	return add_allow(ps, v, "resp_allow", ps->cfg->resp_allow,
		&ps->cfg->n_resp_allow);
}

static int add_http_allow(struct pstate *ps, const char *v)
{
	return add_allow(ps, v, "http_allow", ps->cfg->http_allow,
		&ps->cfg->n_http_allow);
}

/* S358: "off" or 1d..7d - how often /stats and the page start counting
 * again.  0 days = never. */
static int stats_reset_parse(const char *v, int *days)
{
	char *end;
	long n;

	if (!strcasecmp(v, "off") || !strcmp(v, "0")) {
		*days = 0;
		return 0;
	}
	n = strtol(v, &end, 10);
	if (end == v || n < 1 || n > 7 || (*end && strcasecmp(end, "d")))
		return -1;
	*days = (int)n;
	return 0;
}

/* S358: HH:MM or HH:MM:SS, local time - seconds past midnight */
static int hhmm_parse(const char *v, int *secs)
{
	int h, m, s = 0, n = 0;

	if ((sscanf(v, "%d:%d:%d%n", &h, &m, &s, &n) != 3 || v[n]) &&
	    (s = 0, n = 0, sscanf(v, "%d:%d%n", &h, &m, &n) != 2 || v[n]))
		return -1;
	if (h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59)
		return -1;
	*secs = h * 3600 + m * 60 + s;
	return 0;
}

static int handle_kv(struct pstate *ps, char *key, char *val)
{
	struct pc_config *cfg = ps->cfg;
	int lv;

	switch (ps->sect) {
	case S_DAEMON:
		if (!strcasecmp(key, "allow_create")) {
			/* S69: whether a CLIENT may create and drop
			 * collections here.  It gates ORIGINATION only, not
			 * what this node accepts from the fleet: a create is
			 * propagated to every member whatever its own
			 * setting, so the collection set stays uniform and
			 * turning this on does not need a fleet restart. */
			if (!strcasecmp(val, "yes") || !strcasecmp(val, "on"))
				cfg->allow_create = 1;
			else if (!strcasecmp(val, "no") || !strcasecmp(val, "off"))
				cfg->allow_create = 0;
			else {
				PERR(ps, "allow_create: yes|no\n");
				return -1;
			}
			return 0;
		}
		if (!strcasecmp(key, "log_events")) {
			if (pc_ev_parse(val, &cfg->log_events) != 0) {
				PERR(ps, "log_events: miss|expired|store|remove|all|none, comma-separated\n");
				return -1;
			}
			return 0;
		}
		if (!strcasecmp(key, "notify_events")) {        /* PS8 */
			if (pc_ev_parse(val, &cfg->notify_events) != 0) {
				PERR(ps, "notify_events: miss|expired|store|remove|all|none, comma-separated\n");
				return -1;
			}
			return 0;
		}
		if (!strcasecmp(key, "log_events_rate"))
			return parse_int(ps, val, 1, 100000, &cfg->log_events_rate);
		if (!strcasecmp(key, "log_events_hash")) {
			if (!strcasecmp(val, "yes") || !strcasecmp(val, "on") ||
			    !strcasecmp(val, "true") || !strcmp(val, "1"))
				cfg->log_events_hash = 1;
			else if (!strcasecmp(val, "no") || !strcasecmp(val, "off") ||
			    !strcasecmp(val, "false") || !strcmp(val, "0"))
				cfg->log_events_hash = 0;
			else { PERR(ps, "log_events_hash: yes|no\n"); return -1; }
			return 0;
		}
		if (!strcasecmp(key, "grow_at_pct"))                /* S150 */
			return parse_int(ps, val, 10, 100, &cfg->grow_at_pct);
		if (!strcasecmp(key, "autoscale")) {                /* S239 */
			if ((cfg->autoscale = pc_autoscale_parse(val)) < 0) {
				PERR(ps, "autoscale: auto|floor|warn|off\n");
				return -1;
			}
			return 0;
		}
		if (!strcasecmp(key, "redis_types")) {              /* S280 */
			if ((cfg->redis_types = pc_types_parse(val)) < 0) {
				PERR(ps, "redis_types: strict|loose\n");
				return -1;
			}
			return 0;
		}
		if (!strcasecmp(key, "shrink_at_pct"))              /* S150 D */
			return parse_int(ps, val, 0, 99, &cfg->shrink_at_pct);
		if (!strcasecmp(key, "grow_floor_pct"))            /* S175 */
			return parse_int(ps, val, 0, 100, &cfg->grow_floor_pct);
		if (!strcasecmp(key, "leg_stuck_pct"))             /* S175 */
			return parse_int(ps, val, 0, 100, &cfg->leg_stuck_pct);
		if (!strcasecmp(key, "prefetch_min_keys"))          /* S232 */
			return parse_int(ps, val, 0, 100000000, &cfg->prefetch_min_keys);
		if (!strcasecmp(key, "shrink_cooloff_s"))           /* S150 D */
			return parse_int(ps, val, 0, 86400, &cfg->shrink_cooloff_s);
		if (!strcasecmp(key, "workers"))
			return parse_int(ps, val, 1, 512, &cfg->workers);
		if (!strcasecmp(key, "busy_poll_us"))           /* S363 */
			return parse_int(ps, val, 0, 100000, &cfg->busy_poll_us);
		if (!strcasecmp(key, "busy_poll_prefer")) {     /* S363 */
			if (!strcasecmp(val, "yes") || !strcasecmp(val, "on"))
				cfg->busy_poll_prefer = 1;
			else if (!strcasecmp(val, "no") || !strcasecmp(val, "off"))
				cfg->busy_poll_prefer = 0;
			else {
				PERR(ps, "busy_poll_prefer: yes | no\n");
				return -1;
			}
			return 0;
		}
		if (!strcasecmp(key, "output_stall_s"))         /* S372 */
			return parse_int(ps, val, 1, 3600, &cfg->output_stall_s);
		if (!strcasecmp(key, "io")) {                    /* S361 */
			if (!strcasecmp(val, "io_uring"))
				cfg->io_backend = PC_IO_URING;
			else if (!strcasecmp(val, "epoll"))
				cfg->io_backend = PC_IO_EPOLL;
			else {
				PERR(ps, "io: io_uring | epoll, not '%s'\n", val);
				return -1;
			}
			cfg->io_explicit = 1;
			return 0;
		}
		if (!strcasecmp(key, "address_wait_s"))           /* S294 */
			return parse_int(ps, val, 0, 600, &cfg->address_wait_s);
		if (!strcasecmp(key, "keys_turn_us"))             /* S292 */
			return parse_int(ps, val, 0, 100000, &cfg->keys_turn_us);
		if (!strcasecmp(key, "slowlog_usec"))
			return parse_int(ps, val, 0, 60000000,
				&cfg->slowlog_usec);
		if (!strcasecmp(key, "stats_reset")) {           /* S358 */
			if (stats_reset_parse(val, &cfg->stats_reset_days)) {
				PERR(ps, "stats_reset must be off or 1d .. 7d, "
					"not '%s'\n", val);
				return -1;
			}
			return 0;
		}
		if (!strcasecmp(key, "stats_reset_at")) {        /* S358 */
			if (hhmm_parse(val, &cfg->stats_reset_at_s)) {
				PERR(ps, "stats_reset_at must be HH:MM (local "
					"time), not '%s'\n", val);
				return -1;
			}
			return 0;
		}
		if (!strcasecmp(key, "pubsub_queue_mb"))
			return parse_int(ps, val, 0, 65536, &cfg->pubsub_queue_mb);
		if (!strcasecmp(key, "max_clients"))      /* S161 */
			return parse_int(ps, val, 0, 1000000, &cfg->max_clients);
		if (!strcasecmp(key, "enable_lockout_after"))   /* S417 */
			return parse_int(ps, val, 0, 1000000, &cfg->enable_lockout_after);
		if (!strcasecmp(key, "enable_lockout_s"))
			return parse_int(ps, val, 1, 86400, &cfg->enable_lockout_s);
		if (!strcasecmp(key, "install_mode")) {       /* PD-9 */
			if (!strcasecmp(val, "auto")) cfg->install_mode = PC_INSTALL_AUTO;
			else if (!strcasecmp(val, "package")) cfg->install_mode = PC_INSTALL_PACKAGE;
			else if (!strcasecmp(val, "prefix")) cfg->install_mode = PC_INSTALL_PREFIX;
			else if (!strcasecmp(val, "local")) cfg->install_mode = PC_INSTALL_LOCAL;
			else {
				PERR(ps, "install_mode: auto|package|prefix|local\n");
				return -1;
			}
			return 0;
		}
		if (!strcasecmp(key, "memory_floor")) {   /* S366 */
			char *e;
			long n;

			if (!strcasecmp(val, "off") || !strcmp(val, "0")) {
				cfg->memory_floor_pct = cfg->memory_floor_mb = 0;
				return 0;
			}
			n = strtol(val, &e, 10);
			while (*e == ' ')
				e++;
			if (e != val && *e == '%' && !e[1] && n >= 1 && n <= 50) {
				cfg->memory_floor_pct = (int)n;
				cfg->memory_floor_mb = 0;
				return 0;
			}
			if (e != val && n >= 1 && n <= 1048576 &&
			    (!strcasecmp(e, "mb") || !strcasecmp(e, "m") ||
			     !strcasecmp(e, "gb") || !strcasecmp(e, "g"))) {
				if ((*e == 'g' || *e == 'G') && n > 1024) {
					PERR(ps, "memory_floor: at most 1024 GB\n");
					return -1;
				}
				cfg->memory_floor_mb = (int)((*e == 'g' || *e == 'G') ? n * 1024 : n);
				cfg->memory_floor_pct = 0;
				return 0;
			}
			PERR(ps, "memory_floor: N%% of the memory limit (1-50), N MB, N GB or off\n");
			return -1;
		}
		if (!strcasecmp(key, "log_level")) {
			lv = level_of(val);
			if (lv == -100) {
				PERR(ps, "log_level: crit|err|warn|notice|info|dbg\n");
				return -1;
			}
			cfg->log_level = lv;
			return 0;
		}
		if (!strcasecmp(key, "query_log")) {
			if (!strcasecmp(val, "off"))
				cfg->query_log = 0;
			else if (!strcasecmp(val, "all"))
				cfg->query_log = 1;
			else if (!strncasecmp(val, "sampled:", 8)) {
				int n = atoi(val + 8);

				if (n < 2 || n > 1000000) {
					PERR(ps, "query_log: sampled:N needs "
						"2 <= N <= 1000000\n");
					return -1;
				}
				cfg->query_log = n;
			} else {
				PERR(ps, "query_log: off | all | sampled:N\n");
				return -1;
			}
			return 0;
		}
		if (!strcasecmp(key, "query_log_keys")) {
			if (!strcasecmp(val, "no"))
				cfg->query_log_keys = 0;
			else if (!strcasecmp(val, "hashed"))
				cfg->query_log_keys = 1;
			else if (!strcasecmp(val, "full"))
				cfg->query_log_keys = 2;
			else {
				PERR(ps, "query_log_keys: no | hashed | full\n");
				return -1;
			}
			return 0;
		}
		if (!strcasecmp(key, "state_dir")) {
			if (val[0] != '/') {
				PERR(ps, "state_dir must be absolute\n");
				return -1;
			}
			free(cfg->state_dir);
			cfg->state_dir = strdup(val);
			return 0;
		}
		break;
	case S_MEMORY:
		if (!strcasecmp(key, "arena_mb"))
			return parse_int(ps, val, 16, 1048576, &cfg->arena_mb);
		if (!strcasecmp(key, "arena_cap_mb"))
			return parse_int(ps, val, 0, 1048576, &cfg->arena_cap_mb);
		if (!strcasecmp(key, "backing")) {
			if (!strcasecmp(val, "own"))
				cfg->backing_heap = 0;
			else if (!strcasecmp(val, "heap"))
				cfg->backing_heap = 1;
			else {
				PERR(ps, "backing: own|heap\n");
				return -1;
			}
			return 0;
		}
		if (!strcasecmp(key, "reclaim_keep"))
			return parse_int(ps, val, 0, 1024, &cfg->reclaim_keep);
		if (!strcasecmp(key, "reclaim_quiet_s"))
			return parse_int(ps, val, 0, 86400, &cfg->reclaim_quiet_s);
		if (!strcasecmp(key, "reclaim_cooloff_s"))
			return parse_int(ps, val, 0, 86400, &cfg->reclaim_cooloff_s);
		if (!strcasecmp(key, "pin")) {              /* S167 */
			if (!strcasecmp(val, "auto"))
				cfg->pin_require = 0;
			else if (!strcasecmp(val, "require"))
				cfg->pin_require = 1;
			else {
				PERR(ps, "pin: auto|require\n");
				return -1;
			}
			return 0;
		}
		if (!strcasecmp(key, "huge_pages")) {       /* S167 */
			if (!strcasecmp(val, "auto"))
				cfg->huge_pages_off = 0;
			else if (!strcasecmp(val, "off"))
				cfg->huge_pages_off = 1;
			else {
				PERR(ps, "huge_pages: auto|off\n");
				return -1;
			}
			return 0;
		}
		if (!strcasecmp(key, "reclaim_giveback"))
			return parse_int(ps, val, 0, 1, &cfg->reclaim_giveback);
		if (!strcasecmp(key, "reclaim_floor_mb"))
			return parse_int(ps, val, 0, 1048576,
				&cfg->reclaim_floor_mb);
		if (!strcasecmp(key, "shrink_step_mb"))
			return parse_int(ps, val, 0, 1048576, &cfg->shrink_step_mb);
		break;
	case S_SECRETS:
		if (!strcasecmp(key, "client")) {
			if (cfg->n_client_secrets >= PC_MAX_CLIENT_SECRETS) {
				PERR(ps, "more than %d client secrets\n",
					PC_MAX_CLIENT_SECRETS);
				return -1;
			}
			cfg->client_secret[cfg->n_client_secrets++] = strdup(val);
			return 0;
		}
		if (!strcasecmp(key, "enable")) {
			/* S129: refused empty - an empty privilege secret
			 * would authorise every caller that sent nothing */
			if (!*val) {
				PERR(ps, "the enable secret is empty\n");
				return -1;
			}
			if (cfg->n_enable_secrets >= PC_MAX_CLIENT_SECRETS) {
				PERR(ps, "more than %d enable secrets\n",
					PC_MAX_CLIENT_SECRETS);
				return -1;
			}
			cfg->enable_secret[cfg->n_enable_secrets++] = strdup(val);
			return 0;
		}
		if (!strcasecmp(key, "cluster")) {
			if (cfg->cluster_secret) {
				PERR(ps, "duplicate cluster secret\n");
				return -1;
			}
			cfg->cluster_secret = strdup(val);
			return 0;
		}
		if (!strcasecmp(key, "http")) {
			/* S37: a shared token for the HTTP door.  A SECOND
			 * layer, never the first: it rides plaintext, so
			 * anyone who can see the traffic can replay it.  It
			 * exists so one mis-scoped firewall rule does not
			 * immediately hand over the fleet's topology. */
			if (cfg->http_token) {
				PERR(ps, "duplicate http token\n");
				return -1;
			}
			cfg->http_token = strdup(val);
			return 0;
		}
		if (!strcasecmp(key, "resp")) {
			/* the Redis AUTH password for RESP listeners - a plain
			 * shared secret compared in constant time, NOT an
			 * Argon2id PSK: stock Redis clients can only send a
			 * password, so this is the strongest in-band control
			 * the dialect allows */
			if (cfg->resp_password) {
				PERR(ps, "duplicate resp secret\n");
				return -1;
			}
			cfg->resp_password = strdup(val);
			return 0;
		}
		if (!strcasecmp(key, "upstream")) {             /* S383 */
			PERR(ps, "[secrets] upstream moved in 0.6.1: write "
				"upstream_password = <it> in each read-through "
				"[collection] - each may front its own Redis\n");
			return -1;
		}
		if (!strcasecmp(key, "mail")) {                 /* S352 */
			if (cfg->mail.pass) {
				PERR(ps, "duplicate mail secret\n");
				return -1;
			}
			if (!val[0]) {
				PERR(ps, "empty mail secret\n");
				return -1;
			}
			cfg->mail.pass = strdup(val);
			return 0;
		}
		break;
	case S_LISTEN:
		if (!strcasecmp(key, "tcp"))
			return add_listener(ps, PC_LISTEN_TCP, val);
		if (!strcasecmp(key, "unix"))
			return add_listener(ps, PC_LISTEN_UNIX, val);
		if (!strcasecmp(key, "resp"))
			return add_listener_ex(ps, PC_LISTEN_TCP, val, 1);
		if (!strcasecmp(key, "http"))
			return add_listener_ex(ps, PC_LISTEN_TCP, val, 2);
		if (!strcasecmp(key, "http_timeout"))
			return parse_int(ps, val, 0, 3600,
				&cfg->http_timeout_s);
		if (!strcasecmp(key, "http_rate"))         /* 0.6.2 */
			return parse_int(ps, val, 0, 100000, &cfg->http_rate);
		if (!strcasecmp(key, "http_cache_ms"))     /* 0.6.2 */
			return parse_int(ps, val, 0, 60000, &cfg->http_cache_ms);
		if (!strcasecmp(key, "http_allow")) {
			char *tok, *save = NULL, *dup = strdup(val);
			int rc = 0;

			if (!dup)
				return -1;
			for (tok = strtok_r(dup, ",", &save); tok && !rc;
			        tok = strtok_r(NULL, ",", &save)) {
				while (*tok == ' ' || *tok == '\t')
					tok++;
				rc = add_http_allow(ps, tok);
			}
			free(dup);
			return rc;
		}
		if (!strcasecmp(key, "resp_allow")) {
			char *tok, *save = NULL, *dup = strdup(val);
			int rc = 0;

			if (!dup)
				return -1;
			for (tok = strtok_r(dup, ",", &save); tok && !rc;
			        tok = strtok_r(NULL, ",", &save)) {
				while (*tok == ' ' || *tok == '\t')
					tok++;
				rc = add_resp_allow(ps, tok);
			}
			free(dup);
			return rc;
		}
		if (!strcasecmp(key, "resp_collections")) {
			free(cfg->resp_collections);
			cfg->resp_collections = strdup(val);
			return 0;
		}
		if (!strcasecmp(key, "keepalive_s"))       /* PS2 */
			return parse_int(ps, val, 0, 86400, &cfg->keepalive_s);
		if (!strcasecmp(key, "pubsub_udp")) {      /* PS5 */
			if (!strcasecmp(val, "yes") || !strcasecmp(val, "on"))
				cfg->pubsub_udp = 1;
			else if (!strcasecmp(val, "no") || !strcasecmp(val, "off"))
				cfg->pubsub_udp = 0;
			else {
				PERR(ps, "pubsub_udp: yes|no\n");
				return -1;
			}
			return 0;
		}
		if (!strcasecmp(key, "resp_redirect")) {
			/* RV-10: what a RESP door does with a key this node does
			 * not own under shard or spread.  forward - answer it,
			 * paying the hop (today; right for a plain client).
			 * moved - a genuine -MOVED and no forward, for a
			 * cluster-aware client, which refreshes its slot table
			 * from exactly that and from nothing else.  Per daemon,
			 * not per request: the daemon cannot tell the two
			 * kinds of client apart. */
			if (!strcasecmp(val, "forward"))
				cfg->resp_redirect = 0;
			else if (!strcasecmp(val, "moved"))
				cfg->resp_redirect = 1;
			else {
				PERR(ps, "resp_redirect: forward|moved\n");
				return -1;
			}
			return 0;
		}
		if (!strcasecmp(key, "plaintext")) {
			if (!strcasecmp(val, "never"))
				cfg->plaintext = PC_PLAINTEXT_NEVER;
			else if (!strcasecmp(val, "loopback"))
				cfg->plaintext = PC_PLAINTEXT_LOOPBACK;
			else {
				PERR(ps, "plaintext: never|loopback\n");
				return -1;
			}
			return 0;
		}
		break;
	case S_WAL:
		if (!strcasecmp(key, "dir")) {
			if (val[0] != '/') {
				PERR(ps, "wal dir must be absolute\n");
				return -1;
			}
			free(cfg->wal_dir);
			cfg->wal_dir = strdup(val);
			return 0;
		}
		if (!strcasecmp(key, "probe")) {
			if (!strcasecmp(val, "auto"))
				cfg->wal_probe = PC_WPROBE_AUTO;
			else if (!strcasecmp(val, "always"))
				cfg->wal_probe = PC_WPROBE_ALWAYS;
			else if (!strcasecmp(val, "no"))
				cfg->wal_probe = PC_WPROBE_NO;
			else {
				PERR(ps, "probe: auto|always|no\n");
				return -1;
			}
			return 0;
		}
		if (!strcasecmp(key, "probe_secs"))
			return parse_int(ps, val, 0, 600, &cfg->wal_probe_secs);
		if (!strcasecmp(key, "fsync")) {
			if (!strcasecmp(val, "everysec"))
				cfg->wal_fsync = 0;
			else if (!strcasecmp(val, "always"))
				cfg->wal_fsync = 1;
			else if (!strcasecmp(val, "no"))
				cfg->wal_fsync = 2;
			else {
				PERR(ps, "fsync: always|everysec|no\n");
				return -1;
			}
			return 0;
		}
		if (!strcasecmp(key, "segment_mb"))
			return parse_int(ps, val, 0, 1024, &cfg->wal_segment_mb);
		if (!strcasecmp(key, "segments"))
			return parse_int(ps, val, 2, 64, &cfg->wal_segments);
		if (!strcasecmp(key, "ring_kb")) {
			cfg->wal_ring_kb_set = 1;
			return parse_int(ps, val, 4, 16384, &cfg->wal_ring_kb);
		}
		if (!strcasecmp(key, "unknown_records")) {    /* UP3 */
			if (!strcasecmp(val, "refuse"))
				cfg->wal_unknown_skip = 0;
			else if (!strcasecmp(val, "skip"))
				cfg->wal_unknown_skip = 1;
			else {
				PERR(ps, "unknown_records: refuse|skip\n");
				return -1;
			}
			return 0;
		}
		if (!strcasecmp(key, "save")) {
			/* Redis-style OR'd rules: "900 1, 300 10" - or "off" */
			char *tok, *sp, *dup;

			cfg->n_rdb_rules = 0;
			if (!strcasecmp(val, "off"))
				return 0;
			dup = strdup(val);
			for (tok = strtok_r(dup, ",", &sp); tok;
			        tok = strtok_r(NULL, ",", &sp)) {
				int secs;
				long long ch;

				if (sscanf(tok, "%d %lld", &secs, &ch) == 2 &&
				        secs == 0 && ch >= 1) {
					/* S224: Redis's manual invites this, so say
					 * why it is refused rather than just "bad" */
					free(dup);
					PERR(ps, "save: seconds must be at least 1 - a "
						"rule of changes alone would, under a heavy "
						"write load, run full snapshots back to back.  "
						"Use \"1 %lld\" for the tightest rule.  Write "
						"pressure is covered anyway: the WAL requests a "
						"snapshot itself when its free segments would "
						"run out before one could finish\n", ch);
					return -1;
				}
				if (sscanf(tok, "%d %lld", &secs, &ch) != 2 ||
				        secs < 1 || ch < 1 ||
				        cfg->n_rdb_rules >= PC_RDB_MAX_RULES) {
					free(dup);
					PERR(ps, "save: \"SECS CHANGES, ...\" or off\n");
					return -1;
				}
				cfg->rdb_rules[cfg->n_rdb_rules].secs = secs;
				cfg->rdb_rules[cfg->n_rdb_rules].changes = ch;
				cfg->n_rdb_rules++;
			}
			free(dup);
			return 0;
		}
		if (!strcasecmp(key, "rdb_mb_s"))
			return parse_int(ps, val, 0, 4000, &cfg->rdb_mb_s);
		break;
	case S_CLUSTER:
		if (!strcasecmp(key, "cluster_id")) {             /* S302 */
			free(cfg->cl_cluster_id);
			cfg->cl_cluster_id = strdup(val);
			return 0;
		}
		if (!strcasecmp(key, "max_members"))              /* S299 */
			return parse_int(ps, val, 0, 257, &cfg->cl_max_members);
		if (!strcasecmp(key, "multicast_ttl"))            /* S291 */
			return parse_int(ps, val, 1, 255, &cfg->cl_mcast_ttl);
		if (!strcasecmp(key, "discovery")) {              /* S36 */
			if (!strcasecmp(val, "multicast"))
				cfg->cl_unicast = 0;
			else if (!strcasecmp(val, "unicast"))
				cfg->cl_unicast = 1;
			else {
				PERR(ps, "discovery must be multicast or unicast\n");
				return -1;
			}
			return 0;
		}
		if (!strcasecmp(key, "seeds")) {                  /* S36 */
			free(cfg->cl_seeds);
			cfg->cl_seeds = strdup(val);
			return 0;
		}
		if (!strcasecmp(key, "identity_source")) {        /* S143 */
			static const char *n[] = { "auto", "dmi", "mac", "env", "file" };
			int k;

			for (k = 0; k < 5; k++)
				if (!strcasecmp(val, n[k])) {
					cfg->cl_id_source = k;
					return 0;
				}
			PERR(ps, "identity_source must be auto, dmi, mac, env or file\n");
			return -1;
		}
		if (!strcasecmp(key, "expect"))                   /* S36c */
			return parse_int(ps, val, 0, 257, &cfg->cl_expect);
		if (!strcasecmp(key, "max_datagram"))             /* S36b */
			return parse_int(ps, val, 0, 65000, &cfg->cl_max_dgram);
		if (!strcasecmp(key, "lane_port"))                /* S36b */
			return parse_int(ps, val, 1, 65535, &cfg->cl_lane_port);
		if (!strcasecmp(key, "lane_retry_s"))             /* S329 */
			return parse_int(ps, val, 1, 3600, &cfg->cl_lane_retry_s);
		if (!strcasecmp(key, "port"))                     /* S36 */
			return parse_int(ps, val, 1, 65535, &cfg->cl_port);
		if (!strcasecmp(key, "multicast")) {
			const char *colon = strrchr(val, ':');

			if (!colon || colon == val) {
				PERR(ps, "multicast must be group:port\n");
				return -1;
			}
			if (parse_int(ps, colon + 1, 1, 65535, &cfg->cl_port))
				return -1;
			free(cfg->cl_mcast);
			cfg->cl_mcast = strndup(val, (size_t)(colon - val));
			return 0;
		}
		if (!strcasecmp(key, "pubsub_rx_threads"))       /* PS11 */
			return parse_int(ps, val, 0, 16, &cfg->cl_ps_rx_threads);
		if (!strcasecmp(key, "pubsub_port"))             /* PS11 */
			return parse_int(ps, val, 1, 65535, &cfg->cl_ps_port);
		if (!strcasecmp(key, "pubsub_relay")) {          /* PS12 */
			if (!strcasecmp(val, "all"))
				cfg->cl_ps_interested = 0;
			else if (!strcasecmp(val, "interested"))
				cfg->cl_ps_interested = 1;
			else {
				PERR(ps, "[cluster] pubsub_relay must be all or "
					"interested, not '%s'\n", val);
				return -1;
			}
			return 0;
		}
		if (!strcasecmp(key, "advertise")) {
			free(cfg->cl_advertise);
			cfg->cl_advertise = strdup(val);
			return 0;
		}
		/* The salt a platform-derived identity is mixed with, so the
		 * same machine in two fleets is two members.  Deliberately NOT
		 * the cluster PSK: rotating a secret must not re-mint every
		 * identity.  Unset means identities stay file/random. */
		if (!strcasecmp(key, "site_salt")) {
			free(cfg->cl_site_salt);
			cfg->cl_site_salt = strdup(val);
			return 0;
		}
		/* S30: the cluster owns mode + the clustered collection set */
		if (!strcasecmp(key, "mode")) {
			int m = mode_of(val, &cfg->cl_eager);

			if (m < 0) {
				PERR(ps, "[cluster] mode: "
					"store|eager|proxy|shard|spread\n");
				return -1;
			}
			cfg->cl_mode = (enum pc_pull_mode)m;
			cfg->cl_has_mode = 1;
			return 0;
		}
		/* S127: K for mode = spread.  Range-checked here; the
		 * "only with spread" and "not more than the fleet" rules
		 * are in validate(), which can see the whole config. */
		if (!strcasecmp(key, "replicas")) {
			if (cfg->cl_replicas) {
				PERR(ps, "duplicate [cluster] replicas\n");
				return -1;
			}
			return parse_int(ps, val, 1, PC_REPLICAS_MAX,
				&cfg->cl_replicas);
		}
		if (!strcasecmp(key, "eager")) {
			PERR(ps, "[cluster] eager is now a mode: write "
				"`mode = eager`.  It was only ever legal "
				"beside mode = store, so it IS a store mode "
				"rather than a flag on one.\n");
			return -1;
		}
		if (!strcasecmp(key, "collections")) {
			char *tok, *save = NULL, *dup;

			if (cfg->cl_collections) {
				PERR(ps, "duplicate [cluster] collections\n");
				return -1;
			}
			cfg->cl_collections = strdup(val);
			dup = strdup(val);
			if (!dup || !cfg->cl_collections) {
				free(dup);
				free(cfg->cl_collections);
				cfg->cl_collections = NULL;
				return -1;
			}
			for (tok = strtok_r(dup, ",", &save); tok;
			        tok = strtok_r(NULL, ",", &save)) {
				while (*tok == ' ' || *tok == '\t')
					tok++;
				{
					size_t l = strlen(tok);

					while (l && (tok[l - 1] == ' ' ||
					        tok[l - 1] == '\t'))
						tok[--l] = 0;
					if (!l)
						continue;
				}
				if (cfg->n_cl_col >= PC_MAX_COLLECTIONS) {
					PERR(ps, "more than %d clustered "
						"collections\n",
						PC_MAX_COLLECTIONS);
					free(dup);
					return -1;
				}
				cfg->cl_colname[cfg->n_cl_col++] = strdup(tok);
			}
			free(dup);
			return 0;
		}
		/* the retired static topology - hard error with the migration
		 * hint (the cluster_options idiom: never silently ignore) */
		if (!strcasecmp(key, "node_id") || !strcasecmp(key, "bind") ||
		        !strcasecmp(key, "peer")) {
			PERR(ps, "'%s' is gone: membership is automatic - "
				"declare only 'multicast = group:port' "
				"(+ optional 'advertise = ip'); node ids are "
				"assigned by the elected master\n", key);
			return -1;
		}
		if (!strcasecmp(key, "pull_timeout_ms"))
			return parse_int(ps, val, 10, 5000, &cfg->cl_pull_timeout_ms);
		/* S216: 0 turns the detector off; under a second it would
		 * read an ordinary long duty as a stall */
		if (!strcasecmp(key, "apply_stall_ms")) {
			if (parse_int(ps, val, 0, 600000,
			        &cfg->cl_apply_stall_ms) != 0)
				return -1;
			if (cfg->cl_apply_stall_ms > 0 &&
			        cfg->cl_apply_stall_ms < 1000) {
				LM_ERR("config: apply_stall_ms = %d - use 0 "
					"(off) or 1000 and up\n",
					cfg->cl_apply_stall_ms);
				return -1;
			}
			return 0;
		}
		if (!strcasecmp(key, "apply_threads"))        /* S217 */
			return parse_int(ps, val, 1, 16, &cfg->cl_apply_threads);
		if (!strcasecmp(key, "max_pending"))
			return parse_int(ps, val, 64, 65536,
				&cfg->cl_max_pending);
		if (!strcasecmp(key, "negative_ms"))
			return parse_int(ps, val, 0, 60000, &cfg->cl_negative_ms);
		if (!strcasecmp(key, "tombstone_ms"))
			return parse_int(ps, val, 100, 60000, &cfg->cl_tombstone_ms);
		if (!strcasecmp(key, "sweep_digests")) {     /* S260 */
			if (!strcasecmp(val, "yes") || !strcasecmp(val, "on"))
				cfg->cl_sweep_digests = 1;
			else if (!strcasecmp(val, "no") || !strcasecmp(val, "off"))
				cfg->cl_sweep_digests = 0;
			else {
				PERR(ps, "sweep_digests: yes|no\n");
				return -1;
			}
			return 0;
		}
		if (!strcasecmp(key, "reconcile_rate_max"))   /* S393 */
			return parse_int(ps, val, 64, 65536,
				&cfg->cl_reconcile_rate_max);
		if (!strcasecmp(key, "reconcile_fast")) {    /* S392 */
			if (!strcasecmp(val, "yes") || !strcasecmp(val, "on"))
				cfg->cl_reconcile_fast = 1;
			else if (!strcasecmp(val, "no") || !strcasecmp(val, "off"))
				cfg->cl_reconcile_fast = 0;
			else {
				PERR(ps, "reconcile_fast: yes|no\n");
				return -1;
			}
			return 0;
		}
		/* S238: how long a delete is remembered past the crossing
		 * window, and how many - a partition healing inside it cannot
		 * undo a delete */
		if (!strcasecmp(key, "tombstone_retain_s"))
			return parse_int(ps, val, 0, 86400, &cfg->cl_tomb_retain_s);
		if (!strcasecmp(key, "tombstone_retain_max"))
			return parse_int(ps, val, 1000, 1000000,
				&cfg->cl_tomb_retain_max);
		break;
	case S_COLLECTION:
		/* S128: one range for one knob.  This parsed 1..24 while the
		 * create and resize verbs took 4..24, so the same value was
		 * legal in a config file and refused on the wire.  4 is the
		 * floor at both now; nothing in the tree configures below it,
		 * and below 2^12 the number buys nothing anyway - segments
		 * are fixed at 4,096 buckets, so 2^4 and 2^12 carve the same
		 * 1.5 MB of index. */
		if (!strcasecmp(key, "buckets_log2"))
			return parse_int(ps, val, 4, 24, &ps->col->buckets_log2);
		if (!strcasecmp(key, "log_events")) {           /* S153 */
			if (pc_ev_parse(val, &ps->col->log_events) != 0) {
				PERR(ps, "log_events: miss|expired|store|remove|all|none\n");
				return -1;
			}
			ps->col->has_log_events = 1;
			return 0;
		}
		if (!strcasecmp(key, "notify_events")) {        /* PS8 */
			if (pc_ev_parse(val, &ps->col->notify_events) != 0) {
				PERR(ps, "notify_events: miss|expired|store|remove|all|none\n");
				return -1;
			}
			ps->col->has_notify_events = 1;
			return 0;
		}
		if (!strcasecmp(key, "pull"))
			return parse_int(ps, val, 0, 1, &ps->col->pull);
		if (!strcasecmp(key, "autoscale")) {            /* S239 */
			if ((ps->col->autoscale = pc_autoscale_parse(val)) < 0) {
				PERR(ps, "autoscale: auto|floor|warn|off\n");
				return -1;
			}
			ps->col->has_autoscale = 1;
			return 0;
		}
		if (!strcasecmp(key, "redis_types")) {          /* S280 */
			if ((ps->col->redis_types = pc_types_parse(val)) < 0) {
				PERR(ps, "redis_types: strict|loose\n");
				return -1;
			}
			ps->col->has_redis_types = 1;
			return 0;
		}
		if (!strcasecmp(key, "upstream")) {             /* S331 */
			/* S382: one or more, comma-separated */
			const char *p = val;
			int n = 0;

			for (;;) {
				const char *e = strchr(p, ',');
				size_t l = e ? (size_t)(e - p) : strlen(p);

				while (l && (*p == ' ' || *p == '\t')) {
					p++;
					l--;
				}
				while (l && (p[l - 1] == ' ' || p[l - 1] == '\t'))
					l--;
				if (l <= 8 || strncmp(p, "redis://", 8)) {
					PERR(ps, "upstream: redis://[user@]host:port[/db], "
						"or several, comma-separated\n");
					return -1;
				}
				if (++n > PC_UP_MAX_SRV) {
					PERR(ps, "upstream: at most %d\n", PC_UP_MAX_SRV);
					return -1;
				}
				if (!e)
					break;
				p = e + 1;
			}
			free(ps->col->upstream);
			ps->col->upstream = strdup(val);
			return 0;
		}
		if (!strcasecmp(key, "upstream_role")) {        /* S382 */
			if (!strcasecmp(val, "any"))
				ps->col->up_role = PC_UP_ROLE_ANY;
			else if (!strcasecmp(val, "replica"))
				ps->col->up_role = PC_UP_ROLE_REPLICA;
			else {
				PERR(ps, "upstream_role: any|replica\n");
				return -1;
			}
			return 0;
		}
		if (!strcasecmp(key, "upstream_master_fallback")) {   /* S382 */
			if (!strcasecmp(val, "yes"))
				ps->col->up_master_fallback = 1;
			else if (!strcasecmp(val, "no"))
				ps->col->up_master_fallback = 0;
			else {
				PERR(ps, "upstream_master_fallback: no|yes\n");
				return -1;
			}
			return 0;
		}
		if (!strcasecmp(key, "upstream_max_lag_ms"))          /* S382 */
			return parse_int(ps, val, 0, 3600000,
				&ps->col->up_max_lag_ms);
		if (!strcasecmp(key, "upstream_mode")) {              /* S382 */
			if (!strcasecmp(val, "balance"))
				ps->col->up_mode = PC_UP_MODE_BALANCE;
			else if (!strcasecmp(val, "failover"))
				ps->col->up_mode = PC_UP_MODE_FAILOVER;
			else {
				PERR(ps, "upstream_mode: balance|failover\n");
				return -1;
			}
			ps->col->has_up_mode = 1;
			return 0;
		}
		if (!strcasecmp(key, "upstream_primary")) {           /* S382 */
			free(ps->col->up_primary);
			ps->col->up_primary = strdup(val);
			return 0;
		}
		if (!strcasecmp(key, "upstream_preempt")) {           /* S382 */
			if (!strcasecmp(val, "yes"))
				ps->col->up_preempt = 1;
			else if (!strcasecmp(val, "no"))
				ps->col->up_preempt = 0;
			else {
				PERR(ps, "upstream_preempt: yes|no\n");
				return -1;
			}
			ps->col->has_up_preempt = 1;
			return 0;
		}
		if (!strcasecmp(key, "upstream_preempt_hold_s")) {    /* S382 */
			ps->col->has_up_preempt = 1;
			return parse_int(ps, val, 15, 300,
				&ps->col->up_preempt_hold_s);
		}
		if (!strcasecmp(key, "upstream_allow_perfcached")) {  /* S384 */
			if (!strcasecmp(val, "yes"))
				ps->col->up_allow_pc = 1;
			else if (!strcasecmp(val, "no"))
				ps->col->up_allow_pc = 0;
			else {
				PERR(ps, "upstream_allow_perfcached: no|yes\n");
				return -1;
			}
			return 0;
		}
		if (!strcasecmp(key, "upstream_password")) {          /* S383 */
			if (ps->col->up_password) {
				PERR(ps, "duplicate upstream_password\n");
				return -1;
			}
			if (!*val) {
				PERR(ps, "empty upstream_password\n");
				return -1;
			}
			ps->col->up_password = strdup(val);
			return 0;
		}
		if (!strcasecmp(key, "upstream_write_user") ||         /* AR7 */
		    !strcasecmp(key, "upstream_write_password")) {
			int pw = !strcasecmp(key, "upstream_write_password");
			char **dst = pw ? &ps->col->up_wpassword : &ps->col->up_wuser;

			if (*dst) {
				PERR(ps, "duplicate %s\n", key);
				return -1;
			}
			if (!*val) {
				PERR(ps, "empty %s\n", key);
				return -1;
			}
			*dst = strdup(val);
			return 0;
		}
		if (!strcasecmp(key, "upstream_prefixes")) {
			free(ps->col->up_prefixes);
			ps->col->up_prefixes = strdup(val);
			return 0;
		}
		if (!strcasecmp(key, "upstream_negative_ms"))
			return parse_int(ps, val, 0, 3600000,
				&ps->col->up_negative_ms);
		if (!strcasecmp(key, "upstream_timeout_ms"))
			return parse_int(ps, val, 10, 60000,
				&ps->col->up_timeout_ms);
		if (!strcasecmp(key, "upstream_conns"))
			return parse_int(ps, val, 1, 16, &ps->col->up_conns);
		if (!strcasecmp(key, "upstream_drain_rate"))
			return parse_int(ps, val, 0, 10000000,
				&ps->col->up_drain_rate);
		/* S401: the sweeps' bytes a second, beside their keys */
		if (!strcasecmp(key, "upstream_drain_mb_rate"))
			return parse_int(ps, val, 0, 100000,
				&ps->col->up_drain_mb_rate);
		/* S385: the largest reply relayed whole - under the 8 MB a
		 * client connection may have waiting */
		if (!strcasecmp(key, "upstream_pass_max_kb"))
			return parse_int(ps, val, 64, 4096,
				&ps->col->up_pass_max_kb);
		/* AR2: writes waiting for the master, at most; past it a write
		 * is refused at once - "not applied", never parked unbounded */
		if (!strcasecmp(key, "upstream_write_queue")) {
			ps->col->has_up_write_queue = 1;
			return parse_int(ps, val, 10, 1000000,
				&ps->col->up_write_queue);
		}
		if (!strcasecmp(key, "upstream_tracking")) {
			if (!strcasecmp(val, "on") || !strcmp(val, "1"))
				ps->col->up_tracking = 1;
			else if (!strcasecmp(val, "off") || !strcmp(val, "0"))
				ps->col->up_tracking = 0;
			else {
				PERR(ps, "upstream_tracking: on|off\n");
				return -1;
			}
			return 0;
		}
		if (!strcasecmp(key, "upstream_writes")) {      /* S348, S385 */
			ps->col->up_refuse_writes = 0;
			ps->col->up_write_through = 0;
			if (!strcasecmp(val, "refuse"))
				ps->col->up_refuse_writes = 1;
			else if (!strcasecmp(val, "through"))
				ps->col->up_write_through = 1;
			else if (strcasecmp(val, "allow")) {
				PERR(ps, "upstream_writes: allow|refuse|through\n");
				return -1;
			}
			return 0;
		}
		if (!strcasecmp(key, "upstream_unknown")) {     /* S385 */
			if (!strcasecmp(val, "pass"))
				ps->col->up_unknown = PC_UP_UNKNOWN_PASS;
			else if (!strcasecmp(val, "refuse"))
				ps->col->up_unknown = PC_UP_UNKNOWN_REFUSE;
			else {
				PERR(ps, "upstream_unknown: pass|refuse\n");
				return -1;
			}
			ps->col->has_up_unknown = 1;
			return 0;
		}
		if (!strcasecmp(key, "upstream_on_error")) {
			if (!strcasecmp(val, "error"))
				ps->col->up_on_error = PC_UP_ON_ERROR_ERROR;
			else if (!strcasecmp(val, "miss"))
				ps->col->up_on_error = PC_UP_ON_ERROR_MISS;
			else {
				PERR(ps, "upstream_on_error: error|miss\n");
				return -1;
			}
			return 0;
		}
		if (!strcasecmp(key, "eager")) {
			PERR(ps, "collection eager is now a mode: write "
				"`mode = eager`\n");
			return -1;
		}
		if (!strcasecmp(key, "mode")) {
			int m = mode_of(val, &ps->col->eager);

			if (m < 0) {
				PERR(ps, "mode: store|eager|proxy|shard|spread\n");
				return -1;
			}
			/* S127: K lives in [cluster] only, so a per-collection
			 * spread has no copy factor - refused at validation,
			 * where it is known whether a cluster exists at all
			 * (S158: standalone, it is inert like the rest) */
			ps->col->mode = (enum pc_pull_mode)m;
			ps->col->has_mode = 1;
			return 0;
		}
		break;
	case S_MAIL:                                            /* S352 */
		if (!strcasecmp(key, "smtp")) {
			if (!mail_url_ok(ps, val))
				return -1;
			free(cfg->mail.url);
			cfg->mail.url = strdup(val);
			return 0;
		}
		if (!strcasecmp(key, "user")) {
			free(cfg->mail.user);
			cfg->mail.user = strdup(val);
			return 0;
		}
		if (!strcasecmp(key, "from")) {
			if (!mail_addr_ok(val)) {
				PERR(ps, "from = '%s': not an address "
					"(local@domain, no display name)\n", val);
				return -1;
			}
			free(cfg->mail.from);
			cfg->mail.from = strdup(val);
			return 0;
		}
		if (!strcasecmp(key, "to")) {
			char *dup = strdup(val), *save = NULL, *t;

			if (!dup)
				return -1;
			for (t = strtok_r(dup, ",", &save); t;
			     t = strtok_r(NULL, ",", &save)) {
				t = trim(t);
				if (!*t)
					continue;
				if (!mail_addr_ok(t)) {
					PERR(ps, "to: '%s' is not an address "
						"(local@domain, no display name)\n", t);
					free(dup);
					return -1;
				}
				if (cfg->mail.nto >= PC_MAIL_TO_MAX) {
					PERR(ps, "to: more than %d recipients\n",
						PC_MAIL_TO_MAX);
					free(dup);
					return -1;
				}
				cfg->mail.to[cfg->mail.nto++] = strdup(t);
			}
			free(dup);
			return 0;
		}
		if (!strcasecmp(key, "ca_file")) {
			if (val[0] != '/') {
				PERR(ps, "ca_file must be absolute\n");
				return -1;
			}
			free(cfg->mail.ca_file);
			cfg->mail.ca_file = strdup(val);
			return 0;
		}
		if (!strcasecmp(key, "tls_verify")) {
			if (!strcasecmp(val, "yes") || !strcasecmp(val, "on"))
				cfg->mail.tls_verify = 1;
			else if (!strcasecmp(val, "no") || !strcasecmp(val, "off"))
				cfg->mail.tls_verify = 0;
			else {
				PERR(ps, "tls_verify: yes|no\n");
				return -1;
			}
			return 0;
		}
		if (!strcasecmp(key, "subject_prefix")) {
			free(cfg->mail.subject_prefix);
			cfg->mail.subject_prefix = strdup(val);
			return 0;
		}
		if (!strcasecmp(key, "timeout_s"))
			return parse_int(ps, val, 1, 300, &cfg->mail.timeout_s);
		if (!strcasecmp(key, "min_interval_s"))
			return parse_int(ps, val, 0, 86400, &cfg->mail.min_interval_s);
		if (!strcasecmp(key, "digest_s"))
			return parse_int(ps, val, 0, 3600, &cfg->mail.digest_s);
		if (!strcasecmp(key, "level")) {
			if (!strcasecmp(val, "warning"))
				cfg->mail.level = PC_MAIL_LVL_WARNING;
			else if (!strcasecmp(val, "critical"))
				cfg->mail.level = PC_MAIL_LVL_CRITICAL;
			else if (!strcasecmp(val, "info"))           /* S377 */
				cfg->mail.level = PC_MAIL_LVL_INFO;
			else {
				PERR(ps, "level: info|warning|critical\n");
				return -1;
			}
			return 0;
		}
		break;
	case S_ALERTS: {                                       /* S380 */
		static const struct { const char *k; size_t off; } ak[] = {
			{ "cpu_warn_pct", offsetof(struct pc_alerts_conf, cpu_warn) },
			{ "cpu_crit_pct", offsetof(struct pc_alerts_conf, cpu_crit) },
			{ "io_warn_pct", offsetof(struct pc_alerts_conf, io_warn) },
			{ "io_crit_pct", offsetof(struct pc_alerts_conf, io_crit) },
			{ "memory_warn_pct", offsetof(struct pc_alerts_conf, mem_warn) },
			{ "memory_crit_pct", offsetof(struct pc_alerts_conf, mem_crit) },
			{ "arena_warn_pct", offsetof(struct pc_alerts_conf, arena_warn) },
			{ "arena_crit_pct", offsetof(struct pc_alerts_conf, arena_crit) },
			{ "epoll_busy_pct", offsetof(struct pc_alerts_conf, epoll_busy) },
		};
		size_t i;

		for (i = 0; i < sizeof ak / sizeof ak[0]; i++)
			if (!strcasecmp(key, ak[i].k))
				return parse_int(ps, val, 1, 100, (int *)((char *)&cfg->alerts
					+ ak[i].off));
		break;
	}
	case S_UPGRADE:                                        /* UP6n */
		if (!strcasecmp(key, "trusted_keys")) {
			/* ed25519 public keys, 64 hex characters each, comma
			 * separated; the release key is built in, not listed */
			char *dup = strdup(val), *tok, *sp = NULL;

			if (!dup)
				return -1;
			cfg->upgrade.n_keys = 0;
			for (tok = strtok_r(dup, ", \t", &sp); tok;
			        tok = strtok_r(NULL, ", \t", &sp)) {
				size_t bl = 0;
				const char *end = NULL;

				if (cfg->upgrade.n_keys >= PC_UPG_MAX_KEYS) {
					PERR(ps, "trusted_keys: at most %d\n",
						PC_UPG_MAX_KEYS);
					free(dup);
					return -1;
				}
				if (strlen(tok) != 64 || sodium_hex2bin(
				        cfg->upgrade.keys[cfg->upgrade.n_keys], 32, tok,
				        64, NULL, &bl, &end) != 0 || bl != 32 || *end) {
					PERR(ps, "trusted_keys: '%s' is not an ed25519 "
						"public key (64 hex characters)\n", tok);
					free(dup);
					return -1;
				}
				cfg->upgrade.n_keys++;
			}
			free(dup);
			return 0;
		}
		if (!strcasecmp(key, "allow_unsigned")) {
			if (!strcasecmp(val, "yes") || !strcasecmp(val, "on"))
				cfg->upgrade.allow_unsigned = 1;
			else if (!strcasecmp(val, "no") || !strcasecmp(val, "off"))
				cfg->upgrade.allow_unsigned = 0;
			else {
				PERR(ps, "allow_unsigned: yes|no\n");
				return -1;
			}
			return 0;
		}
		if (!strcasecmp(key, "stage_mb"))
			return parse_int(ps, val, 16, 4096, &cfg->upgrade.stage_mb);
		if (!strcasecmp(key, "results_dir")) {         /* UPS */
			if (val[0] != '/' || strlen(val) >= sizeof cfg->upgrade.results_dir) {
				PERR(ps, "results_dir must be an absolute path, under 256\n");
				return -1;
			}
			snprintf(cfg->upgrade.results_dir,
				sizeof cfg->upgrade.results_dir, "%s", val);
			return 0;
		}
		break;
	case S_NONE:
		PERR(ps, "key outside any section\n");
		return -1;
	}
	PERR(ps, "unknown key '%s' in this section\n", key);
	return -1;
}

/* S382: is @hp ("host:port") one of the upstreams @list names
 * ("redis://[user@]host:port[/db], ...")? */
static int up_listed(const char *list, const char *hp)
{
	size_t hl = strlen(hp);
	const char *p = list;

	while (p && *p) {
		const char *e = strchr(p, ','), *h, *at, *end, *sl, *q;

		end = e ? e : p + strlen(p);
		while (p < end && (*p == ' ' || *p == '\t'))
			p++;
		h = end - p > 8 && !strncmp(p, "redis://", 8) ? p + 8 : p;
		at = memchr(h, '@', (size_t)(end - h));
		if (at)
			h = at + 1;
		/* host:port runs to the '/' of the db, or the end */
		sl = memchr(h, '/', (size_t)(end - h));
		q = sl ? sl : end;
		while (q > h && (q[-1] == ' ' || q[-1] == '\t'))
			q--;
		if ((size_t)(q - h) == hl && !strncmp(h, hp, hl))
			return 1;
		p = e ? e + 1 : NULL;
	}
	return 0;
}

static int validate(struct pstate *ps, const char *path,
		struct pc_config *cfg)
{
	struct stat st;
	int i, j;

	ps->line = 0;                      /* rule errors are not line errors */

	/* S380: a warning below its critical, or the critical would be raised
	 * on the way past a warning that can never come first */
	{
		const struct pc_alerts_conf *al = &cfg->alerts;
		const char *bad = al->cpu_warn >= al->cpu_crit ? "cpu"
			: al->io_warn >= al->io_crit ? "io"
			: al->mem_warn >= al->mem_crit ? "memory"
			: al->arena_warn >= al->arena_crit ? "arena" : NULL;

		if (bad) {
			PERR(ps, "[alerts] %s_warn_pct must be below %s_crit_pct\n",
				bad, bad);
			return -1;
		}
	}

	/* n_col may legitimately be 0 here when [cluster] declares the set
	 * and this node has no local sizing blocks - the set is expanded
	 * below, and the emptiness check moves after it (S30/S34: a joiner
	 * whose config never names a clustered collection is exactly the
	 * supported case) */
	if (!cfg->n_col && !cfg->n_cl_col) {
		PERR(ps, "no collections configured\n");
		return -1;
	}
	for (i = 0; i < cfg->n_col; i++) {
		if (cfg->col[i].buckets_log2 < 0) {
			PERR(ps, "collection '%s': buckets_log2 missing\n",
				cfg->col[i].name);
			return -1;
		}
		if (!cfg->cl_enabled) {
			/* S158: STANDALONE IGNORES THE FLEET-SHAPED KEYS AND
			 * SAYS SO.  One config file serves both shapes - a
			 * single node first, the fleet later - so a collection
			 * stanza written for the fleet runs unchanged here as
			 * store, and the warning names what is inert.  Printed
			 * by -C and again at startup, since both validate. */
			const char *mn = cfg->col[i].eager ? "eager"
				: cfg->col[i].mode == PC_MODE_PROXY ? "proxy"
				: cfg->col[i].mode == PC_MODE_SHARD ? "shard"
				: cfg->col[i].mode == PC_MODE_SPREAD ? "spread"
				: NULL;

			if (mn) {
				LM_WARN("collection '%s': mode = %s ignored - no "
					"[cluster] section, running as store\n",
					cfg->col[i].name, mn);
				cfg->col[i].eager = 0;
				cfg->col[i].mode = PC_MODE_STORE;
			}
			if (cfg->col[i].pull) {
				LM_WARN("collection '%s': pull = 1 ignored - no "
					"[cluster] section, no peers to pull from\n",
					cfg->col[i].name);
				cfg->col[i].pull = 0;
			}
		} else if (cfg->col[i].mode == PC_MODE_SPREAD) {
			PERR(ps, "collection '%s': mode = spread is not a "
				"per-collection setting: the copy factor is cluster "
				"state.  Declare [cluster] mode = spread with "
				"replicas = N\n", cfg->col[i].name);
			return -1;
		} else if (cfg->col[i].eager &&
		        cfg->col[i].mode != PC_MODE_STORE) {
			PERR(ps, "collection '%s': mode = eager conflicts with "
				"its placement mode\n", cfg->col[i].name);
			return -1;
		}
		for (j = 0; j < i; j++)
			if (!strcmp(cfg->col[i].name, cfg->col[j].name)) {
				PERR(ps, "duplicate collection '%s'\n", cfg->col[i].name);
				return -1;
			}
	}

	if (!cfg->n_listen) {
		PERR(ps, "no listeners configured\n");
		return -1;
	}

	if (!cfg->n_client_secrets) {
		PERR(ps, "no client secret configured\n");
		return -1;
	}
	/* S158: the cluster secret is what the peer plane's PSK is derived
	 * from, and nothing on a standalone daemon derives it - so it is
	 * required only where a [cluster] section is.  Present, it must
	 * still differ from every client secret (the two-principal rule). */
	if (!cfg->cluster_secret && cfg->cl_enabled) {
		PERR(ps, "no cluster secret configured\n");
		return -1;
	}
	for (i = 0; i < cfg->n_client_secrets; i++) {
		if (!cfg->client_secret[i][0]) {
			PERR(ps, "empty client secret\n");
			return -1;
		}
		/* THE two-principal rule: structurally different secrets */
		if (cfg->cluster_secret &&
		        !strcmp(cfg->client_secret[i], cfg->cluster_secret)) {
			PERR(ps, "the cluster secret EQUALS client secret #%d - the "
				"two principals must not share a secret (DESIGN.md "
				"par 5.1); refusing to start\n", i + 1);
			return -1;
		}
		for (j = 0; j < i; j++)
			if (!strcmp(cfg->client_secret[i], cfg->client_secret[j])) {
				PERR(ps, "duplicate client secret (#%d = #%d)\n",
					i + 1, j + 1);
				return -1;
			}
		if (strlen(cfg->client_secret[i]) < 8)
			LM_WARN("client secret #%d is under 8 characters\n", i + 1);
	}
	if (cfg->cluster_secret && !cfg->cluster_secret[0]) {
		PERR(ps, "empty cluster secret\n");
		return -1;
	}
	if (cfg->cluster_secret && strlen(cfg->cluster_secret) < 8)
		LM_WARN("the cluster secret is under 8 characters\n");
	for (i = 0; i < cfg->n_enable_secrets; i++)
		if (strlen(cfg->enable_secret[i]) < 8)
			LM_WARN("enable secret #%d is under 8 characters\n", i + 1);
	/* S129 item 4: THE misconfiguration this task exists to prevent.
	 * allow_create on with no enable secret means the node will
	 * originate DDL and the only thing standing in front of drop and
	 * resize is the CLIENT secret, which every application link
	 * presents.  Said in the same register as the world-readable-config
	 * warning below, because it is the same class of mistake. */
	if (cfg->allow_create && cfg->n_enable_secrets == 0)
		LM_WARN("allow_create is ON and no [secrets] enable is set - "
			"DDL is refused from the client door entirely "
			"(fail-closed).  Set an enable secret to allow "
			"privileged connections, or turn allow_create off\n");

	/* ---- RESP listeners (S33): the network IS the authentication ----
	 * A RESP listener has no handshake - stock Redis clients cannot
	 * speak Noise - so everything that would normally be established
	 * by the handshake has to be established by config, and the daemon
	 * refuses to start rather than quietly serving the world. */
	{
		int n_resp = 0, n_resp_lan = 0;

		for (i = 0; i < cfg->n_listen; i++)
			if (cfg->listen[i].resp) {
				n_resp++;
				if (!cfg->listen[i].loopback)
					n_resp_lan++;
			}
		if (!n_resp && (cfg->n_resp_allow || cfg->resp_password ||
		        cfg->resp_collections)) {
			PERR(ps, "resp_allow/resp_collections/[secrets] resp are "
				"set but no 'resp = addr:port' listener exists\n");
			return -1;
		}
		if (n_resp_lan && !cfg->n_resp_allow) {
			PERR(ps, "a non-loopback RESP listener needs "
				"resp_allow = <cidr>[,<cidr>...]: the dialect has "
				"no handshake, so without an allow-list anything "
				"that can reach the port owns the data.  Refusing "
				"to start\n");
			return -1;
		}
		for (i = 0; i < cfg->n_listen; i++)
			if (cfg->listen[i].resp)
				LM_NOTICE("RESP listener on %s:%d - %d allow-list "
					"entr%s, password %s, collections %s\n",
					cfg->listen[i].addr, cfg->listen[i].port,
					cfg->n_resp_allow,
					cfg->n_resp_allow == 1 ? "y" : "ies",
					cfg->resp_password ? "SET" : "none",
					cfg->resp_collections ?
						cfg->resp_collections : "all");
		{
			/* S46: the metrics listener is plaintext HTTP with no
			 * handshake and no password - identical exposure to a
			 * RESP listener, so identical rule.  A scrape target
			 * that anyone can reach is also a disclosure of the
			 * fleet's shape. */
			int n_met = 0, n_met_lan = 0;

			for (i = 0; i < cfg->n_listen; i++)
				if (cfg->listen[i].http) {
					n_met++;
					if (!cfg->listen[i].loopback)
						n_met_lan++;
				}
			if (!n_met && cfg->n_http_allow) {
				PERR(ps, "http_allow is set but no 'http = "
					"addr:port' listener exists\n");
				return -1;
			}
			if (n_met_lan && !cfg->n_http_allow) {
				PERR(ps, "a non-loopback http listener needs "
					"http_allow = <cidr>[,<cidr>...]: the "
					"endpoint is plaintext HTTP with no "
					"handshake and no password, so without an "
					"allow-list anything that can reach the "
					"port reads the node's whole state.  "
					"Refusing to start\n");
				return -1;
			}
			for (i = 0; i < cfg->n_listen; i++)
				if (cfg->listen[i].http)
					LM_NOTICE("http listener on %s:%d - "
						"%d allow-list entr%s, token %s "
						"(GET /, /members, /metrics, "
						"/health)\n",
						cfg->listen[i].addr,
						cfg->listen[i].port,
						cfg->n_http_allow,
						cfg->n_http_allow == 1 ?
							"y" : "ies",
						cfg->http_token ? "SET" : "none");
			if (n_met_lan && !cfg->http_token)
				LM_WARN("the http listener is reachable "
					"off-box with no [secrets] http token: "
					"the allow-list is the only control, "
					"and the traffic is UNENCRYPTED - put a "
					"TLS-terminating proxy in front of it "
					"rather than exposing it directly\n");
		}

		if (n_resp_lan && !cfg->resp_password)
			LM_WARN("RESP listener is reachable off-box with NO "
				"password ([secrets] resp): the allow-list is the "
				"only control, and the traffic is UNENCRYPTED - "
				"Redis clients cannot speak the Noise channel\n");
		if (cfg->resp_password) {
			if (!cfg->resp_password[0]) {
				PERR(ps, "empty resp secret\n");
				return -1;
			}
			if (cfg->cluster_secret &&
			        !strcmp(cfg->resp_password, cfg->cluster_secret)) {
				PERR(ps, "the resp password EQUALS the cluster "
					"secret - a plaintext-transmitted password "
					"must never be a principal's secret; "
					"refusing to start\n");
				return -1;
			}
			for (i = 0; i < cfg->n_client_secrets; i++)
				if (!strcmp(cfg->resp_password,
				        cfg->client_secret[i])) {
					PERR(ps, "the resp password EQUALS client "
						"secret #%d - it crosses the wire "
						"in the clear; refusing to "
						"start\n", i + 1);
					return -1;
				}
			if (strlen(cfg->resp_password) < 8)
				LM_WARN("the resp password is under 8 characters "
					"and crosses the wire in the clear\n");
		}
	}

	/* S331: read-through - an upstream with nothing allowed to fall
	 * through is a mistake, not a configuration: refused, like an empty
	 * secret.  The allow-list is never implicit. */
	for (i = 0; i < cfg->n_col; i++) {
		const struct pc_collection *cc = &cfg->col[i];

		if (!cc->upstream && cc->up_prefixes) {
			PERR(ps, "[collection %s] upstream_prefixes without an "
				"upstream\n", cc->name);
			return -1;
		}
		if (cc->upstream && (!cc->up_prefixes || !*cc->up_prefixes)) {
			PERR(ps, "[collection %s] upstream needs upstream_prefixes "
				"- the keys allowed to fall through (\"*\" for all, "
				"said explicitly)\n", cc->name);
			return -1;
		}
		/* S382: the master is the fallback FROM replicas - asked for
		 * without them, it would say nothing */
		if (cc->up_master_fallback && cc->up_role != PC_UP_ROLE_REPLICA) {
			PERR(ps, "[collection %s] upstream_master_fallback = yes "
				"needs upstream_role = replica\n", cc->name);
			return -1;
		}
		if (!cc->upstream && (cc->up_role || cc->up_max_lag_ms)) {
			PERR(ps, "[collection %s] upstream_role / "
				"upstream_max_lag_ms without an upstream\n", cc->name);
			return -1;
		}
		/* S382: the failover keys say nothing to balance */
		if (cc->up_mode != PC_UP_MODE_FAILOVER &&
		    (cc->up_primary || cc->has_up_preempt)) {
			PERR(ps, "[collection %s] upstream_primary / "
				"upstream_preempt / upstream_preempt_hold_s need "
				"upstream_mode = failover\n", cc->name);
			return -1;
		}
		if (cc->has_up_mode && !cc->upstream) {
			PERR(ps, "[collection %s] upstream_mode without an "
				"upstream\n", cc->name);
			return -1;
		}
		if (cc->up_primary && !up_listed(cc->upstream, cc->up_primary)) {
			PERR(ps, "[collection %s] upstream_primary = %s is not one "
				"of its upstreams (host:port, as upstream lists it)\n",
				cc->name, cc->up_primary);
			return -1;
		}
		if (!cc->upstream && cc->up_allow_pc) {           /* S384 */
			PERR(ps, "[collection %s] upstream_allow_perfcached without "
				"an upstream\n", cc->name);
			return -1;
		}
		if (!cc->upstream && cc->up_password) {           /* S383 */
			PERR(ps, "[collection %s] upstream_password without an "
				"upstream\n", cc->name);
			return -1;
		}
	}
	/* S331: each read-through collection gets its COMPANION, <name>.nofall
	 * - the keys this fleet has written or deleted, which must never fall
	 * through again while the upstream's copy can exist.  A collection
	 * like any other: created at start (before recovery, so its WAL and
	 * snapshot come back), replicated with its parent - it joins the
	 * cluster's set when the parent is in it.  '.' is outside the name
	 * rule, so no configured collection can collide with it. */
	{
		int n0 = cfg->n_col;

		for (i = 0; i < n0; i++) {
			struct pc_collection *pc = &cfg->col[i];
			char nm[64];               /* a name is 1-32 here, plus ".nofall" */
			struct pc_collection *nf;
			int in_cl = 0;

			/* S348: only a read-through collection can refuse writes */
			if ((pc->up_refuse_writes || pc->up_write_through) &&
			    !pc->upstream) {
				PERR(ps, "[collection %s] upstream_writes needs "
					"upstream = - it says what a read-through "
					"collection does with a write\n", pc->name);
				return -1;
			}
			/* S385: unknown commands go to Redis only where Redis has
			 * every write (through); elsewhere Redis is not complete
			 * and passing would split the truth */
			if (pc->has_up_unknown && !pc->up_write_through) {
				PERR(ps, "[collection %s] upstream_unknown needs "
					"upstream_writes = through - only then does the "
					"upstream hold every write\n", pc->name);
				return -1;
			}
			if (pc->up_write_through && !pc->has_up_unknown)
				pc->up_unknown = PC_UP_UNKNOWN_PASS;
			if (pc->has_up_write_queue && !pc->up_write_through) {
				PERR(ps, "[collection %s] upstream_write_queue needs "
					"upstream_writes = through - only then are writes "
					"queued for the master\n", pc->name);
				return -1;
			}
			if ((pc->up_wuser || pc->up_wpassword) &&      /* AR7 */
			    !pc->up_write_through) {
				PERR(ps, "[collection %s] upstream_write_user and "
					"upstream_write_password need upstream_writes = "
					"through - only then does a writer connect\n",
					pc->name);
				return -1;
			}
			if (pc->up_wuser && !pc->up_wpassword) {
				PERR(ps, "[collection %s] upstream_write_user without "
					"upstream_write_password\n", pc->name);
				return -1;
			}
			if (!pc->upstream)
				continue;
			/* S348: a collection that refuses writes never owns a key,
			 * so it has no no-fall companion to keep */
			if (pc->up_refuse_writes)
				continue;
			snprintf(nm, sizeof nm, "%s.nofall", pc->name);
			if (cfg->n_col >= PC_MAX_COLLECTIONS) {
				PERR(ps, "[collection %s] upstream needs a companion "
					"collection and there are already %d\n",
					pc->name, PC_MAX_COLLECTIONS);
				return -1;
			}
			nf = &cfg->col[cfg->n_col++];
			memset(nf, 0, sizeof *nf);
			nf->name = strdup(nm);
			nf->buckets_log2 = pc->buckets_log2;
			/* and the parent's autoscale: an empty companion under
			 * the default (auto) shrank to 2^12 a minute after the
			 * start, then had to grow back while a sweep wrote one
			 * entry per key - the operator sizes the pair with one
			 * buckets_log2 and one floor */
			nf->autoscale = pc->autoscale;
			nf->has_autoscale = pc->has_autoscale;
			nf->mode = pc->mode;
			nf->has_mode = pc->has_mode;
			nf->eager = pc->eager;
			nf->up_negative_ms = 0;
			for (j = 0; j < cfg->n_cl_col; j++)
				if (!strcmp(cfg->cl_colname[j], pc->name))
					in_cl = 1;
			if (in_cl) {
				if (cfg->n_cl_col >= PC_MAX_COLLECTIONS) {
					PERR(ps, "[cluster] collections: no room for "
						"%s\n", nm);
					return -1;
				}
				cfg->cl_colname[cfg->n_cl_col++] = strdup(nm);
			}
		}
	}

	if (sizeof(void *) == 4 && cfg->arena_mb > 1536) {
		PERR(ps, "arena_mb %d exceeds the 32-bit cap (1536)\n",
			cfg->arena_mb);
		return -1;
	}
	/* S167: there is no reservation to lock under `backing = heap` - the
	 * core allocator owns the pages - so the pair can never be honoured
	 * and saying so at parse time beats a start that looks pinned */
	if (cfg->pin_require && cfg->backing_heap) {
		PERR(ps, "pin = require needs backing = own: a heap-backed "
			"daemon has no arena to lock\n");
		return -1;
	}
	if (cfg->arena_cap_mb && cfg->arena_cap_mb < cfg->arena_mb) {
		PERR(ps, "arena_cap_mb %d below arena_mb %d\n",
			cfg->arena_cap_mb, cfg->arena_mb);
		return -1;
	}
	/* S47: a floor above the ceiling can never be satisfied - the node
	 * would refuse to release memory it is also refusing to hold */
	if (cfg->reclaim_floor_mb) {
		int cap = cfg->arena_cap_mb ? cfg->arena_cap_mb : cfg->arena_mb;

		if (cfg->reclaim_floor_mb > cap) {
			PERR(ps, "reclaim_floor_mb %d above the arena ceiling "
				"%d MB\n", cfg->reclaim_floor_mb, cap);
			return -1;
		}
	}

	if (cfg->cl_enabled && cfg->cl_unicast) {           /* S36 */
		if (cfg->cl_max_dgram && cfg->cl_max_dgram < 576) {
			PERR(ps, "[cluster] max_datagram is 0 (no lane) or "
				"576-65000\n");
			return -1;
		}
		if (!cfg->cl_lane_port && cfg->cl_port && cfg->cl_port <= 65533)
			cfg->cl_lane_port = cfg->cl_port + 2;
		if (cfg->cl_max_dgram && cfg->cl_lane_port == cfg->cl_port) {
			PERR(ps, "[cluster] lane_port must differ from port\n");
			return -1;
		}
		if (!cfg->cl_port) {
			PERR(ps, "[cluster] discovery = unicast needs port = "
				"<cluster port>\n");
			return -1;
		}
		if (!cfg->cl_seeds || !cfg->cl_seeds[strspn(cfg->cl_seeds, ", \t")])
			LM_WARN("[cluster] discovery = unicast with no seeds - "
				"this node finds peers only when they find it\n");
		if (cfg->cl_mcast)
			LM_WARN("[cluster] discovery = unicast: multicast = %s is "
				"ignored (its port is the cluster port unless port "
				"= is set)\n", cfg->cl_mcast);
	} else if (cfg->cl_enabled && !cfg->cl_mcast) {
		PERR(ps, "[cluster] needs multicast = group:port (or discovery "
			"= unicast with port and seeds)\n");
		return -1;
	}
	if (cfg->cl_enabled) {                     /* PS11 */
		if (!cfg->cl_ps_port)
			cfg->cl_ps_port = cfg->cl_port < 65535 ? cfg->cl_port + 1 : 0;
		if (cfg->cl_ps_rx_threads && cfg->cl_ps_port == cfg->cl_port) {
			PERR(ps, "[cluster] pubsub_port %d is the cluster port: the "
				"relay needs a port of its own (or pubsub_rx_threads = 0)\n",
				cfg->cl_ps_port);
			return -1;
		}
		if (cfg->cl_ps_rx_threads && !cfg->cl_ps_port) {
			PERR(ps, "[cluster] the cluster port is 65535, so pubsub_port "
				"must be set\n");
			return -1;
		}
	}
	/* ---- S30: the cluster owns the clustered collection set --------
	 * Declaring `collections` in [cluster] makes the cluster
	 * description AUTHORITATIVE: one mode for every clustered
	 * collection, an EXHAUSTIVE set (no private extras on a member),
	 * and per-collection `mode =` demoted to a hint that must match.
	 * Without `collections` the legacy per-collection form still
	 * parses, so existing single-mode deployments keep working - but
	 * they get the split-brain this task exists to close, so say so. */
	if (cfg->n_cl_col) {
		if (!cfg->cl_has_mode) {
			PERR(ps, "[cluster] collections = ... needs mode = "
				"store|proxy|shard|spread: one cluster, one mode\n");
			return -1;
		}
		/* mode_of() cannot produce this pair; kept as an
		 * invariant so a future edit cannot reintroduce it
		 * quietly */
		if (cfg->cl_eager && cfg->cl_mode != PC_MODE_STORE) {
			PERR(ps, "[cluster] internal: eager outside store\n");
			return -1;
		}

		/*
		 * S223: an eager replica does NOT persist the copies it
		 * receives - apply_rec() WALs only what this node authored,
		 * because the author persists it and a restarted replica
		 * resyncs.  That holds while the FLEET survives.  When every
		 * node goes down together, each one replays only its own
		 * third of the keyspace, every node then asks its peers
		 * "does anyone still have this?", the peers answer no
		 * truthfully, and the keys are dropped and tombstoned.
		 * Measured 2026-09-22: 600 of 900 keys lost on a simultaneous
		 * restart, and a node joining one-by-one deleted ALL of its
		 * own authored records within 15 seconds.
		 *
		 * The snapshot is what makes it survivable: the RDB walk does
		 * not filter passive records, so a node recovers the whole
		 * table rather than its own share.  With no snapshot rules at
		 * all there is nothing to recover from and the loss is total,
		 * so this combination is called out at startup rather than
		 * discovered after an outage.
		 */
		if (cfg->cl_eager && cfg->n_rdb_rules == 0) {
			LM_WARN("[wal] save = off with [cluster] mode = eager "
				"is a DATA-LOSS configuration: an eager replica "
				"does not persist the copies it receives, so if "
				"every node goes down together each one replays "
				"only what it authored, the fleet answers "
				"truthfully that nobody else holds those keys, "
				"and they are deleted (measured: 600 of 900 "
				"lost).  A snapshot carries the copies and is "
				"the only thing that makes a whole-fleet outage "
				"recoverable - set [wal] save, e.g. "
				"save = 900 1 / 300 10 / 60 10000.\n");
		}

		/* ---- S127: the copy factor -------------------------------
		 * K decides WHICH nodes hold a key, so it is interchange
		 * state, not local tuning: it is in the digest and a member
		 * running a different K is refused at the join.  All three
		 * rules below exist to stop a config that would look like it
		 * works and quietly place records somewhere else. */
		if (cfg->cl_mode == PC_MODE_SPREAD && !cfg->cl_replicas) {
			PERR(ps, "[cluster] mode = spread needs replicas = N "
				"(how many nodes hold each record).  There is "
				"no default: the right K depends on the fleet "
				"size and on how much loss you will accept\n");
			return -1;
		}
		if (cfg->cl_replicas && cfg->cl_mode != PC_MODE_SPREAD) {
			PERR(ps, "[cluster] replicas is only meaningful with "
				"mode = spread (this config says mode = %s)\n",
				mode_name(cfg->cl_mode, cfg->cl_eager));
			return -1;
		}
		if (cfg->cl_mode == PC_MODE_SPREAD &&
		        cfg->cl_replicas < PC_REPLICAS_MIN) {
			PERR(ps, "[cluster] replicas = 1 is shard mode, and is "
				"REFUSED rather than aliased to it: two names "
				"for one behaviour is how config drift starts. "
				"Write mode = shard\n");
			return -1;
		}
		/* every [collection X] must name a clustered collection, and
		 * may not contradict the cluster's mode */
		for (i = 0; i < cfg->n_col; i++) {
			int found = 0;

			for (j = 0; j < cfg->n_cl_col; j++)
				if (!strcmp(cfg->col[i].name, cfg->cl_colname[j])) {
					found = 1;
					break;
				}
			if (!found) {
				PERR(ps, "[collection %s] is not in the cluster's "
					"collections list - a cluster member serves "
					"NO private collections; add it to "
					"[cluster] collections or remove the "
					"block\n", cfg->col[i].name);
				return -1;
			}
			/* eager and store are both PC_MODE_STORE, so the
			 * enum alone cannot tell them apart - compare the
			 * eager half too or `mode = eager` under a
			 * `mode = store` cluster passes silently */
			if (cfg->col[i].has_mode &&
			        (cfg->col[i].mode != cfg->cl_mode ||
			         cfg->col[i].eager != cfg->cl_eager)) {
				PERR(ps, "[collection %s] mode = %s contradicts "
					"[cluster] mode = %s - the cluster owns the "
					"mode; remove the local one\n",
					cfg->col[i].name,
					mode_name(cfg->col[i].mode,
						cfg->col[i].eager),
					mode_name(cfg->cl_mode, cfg->cl_eager));
				return -1;
			}
		}
		/* a clustered collection with no local block is CREATED with
		 * default sizing - the set travels with the cluster */
		for (j = 0; j < cfg->n_cl_col; j++) {
			int have = 0;

			for (i = 0; i < cfg->n_col; i++)
				if (!strcmp(cfg->col[i].name, cfg->cl_colname[j])) {
					have = 1;
					break;
				}
			if (have)
				continue;
			if (cfg->n_col >= PC_MAX_COLLECTIONS) {
				PERR(ps, "cluster set exceeds %d collections\n",
					PC_MAX_COLLECTIONS);
				return -1;
			}
			cfg->col[cfg->n_col].name = strdup(cfg->cl_colname[j]);
			cfg->col[cfg->n_col].buckets_log2 = PC_DEFAULT_BUCKETS_LOG2;
			cfg->n_col++;
			LM_NOTICE("collection '%s' comes from the cluster and has "
				"no local block - created with default sizing "
				"(buckets_log2 = %d)\n", cfg->cl_colname[j],
				PC_DEFAULT_BUCKETS_LOG2);
		}
		/* stamp the cluster's mode onto every collection: from here
		 * the runtime has one authority, and the local file is
		 * untouched (a joiner adopting a DIFFERENT mode at join time
		 * follows the same rule - cluster/adopt path) */
		for (i = 0; i < cfg->n_col; i++) {
			cfg->col[i].mode = cfg->cl_mode;
			cfg->col[i].has_mode = 1;
			cfg->col[i].eager = cfg->cl_eager;
			if (cfg->cl_mode == PC_MODE_STORE)
				cfg->col[i].pull = 1;
			/* S127: spread IS eager with a bounded target - the
			 * same push machinery, aimed at the K holders instead
			 * of at every peer - so it runs the eager path and
			 * pulls on a miss like store does.  Without the eager
			 * flag the push is never reached at all and spread
			 * silently replicates NOTHING, which is what
			 * spreadtest caught on its first run. */
			if (cfg->cl_mode == PC_MODE_SPREAD) {
				cfg->col[i].eager = 1;
				cfg->col[i].pull = 1;
			}
		}
	} else if (cfg->cl_enabled) {
		int mixed = 0;

		for (i = 1; i < cfg->n_col; i++)
			if (cfg->col[i].mode != cfg->col[0].mode)
				mixed = 1;
		LM_WARN("[cluster] without `collections`: modes are per-node "
			"and NOTHING checks that peers agree - two nodes can "
			"run the same collection in different modes and lose "
			"data silently.  Declare `mode` + `collections` in "
			"[cluster]%s\n", mixed ? " (this config already mixes "
			"modes, which one cluster cannot represent)" : "");
	} else if (cfg->cl_has_mode || cfg->n_cl_col) {
		PERR(ps, "[cluster] mode/collections without a "
			"[cluster] section\n");
		return -1;
	}

	/* S352: a [mail] section that can actually send */
	if (cfg->mail_on) {
		if (!cfg->mail.url) {
			PERR(ps, "[mail] needs smtp = smtp://host:port or "
				"smtps://host:port\n");
			return -1;
		}
		if (!cfg->mail.from) {
			PERR(ps, "[mail] needs from = an address\n");
			return -1;
		}
		if (!cfg->mail.nto) {
			PERR(ps, "[mail] needs to = at least one address\n");
			return -1;
		}
		if (cfg->mail.user && !cfg->mail.pass) {
			PERR(ps, "[mail] user = %s, but no [secrets] mail "
				"(the password)\n", cfg->mail.user);
			return -1;
		}
		if (!cfg->mail.user && cfg->mail.pass)
			LM_WARN("[secrets] mail is set but [mail] has no user = - "
				"mail goes without AUTH and the password is unused\n");
		if (cfg->mail.ca_file && access(cfg->mail.ca_file, R_OK) != 0) {
			PERR(ps, "[mail] ca_file %s cannot be read\n",
				cfg->mail.ca_file);
			return -1;
		}
		/* still TLS, but to whoever answers: said at every check, so
		 * the choice stays visible */
		if (!cfg->mail.tls_verify)
			LM_WARN("[mail] tls_verify = no: mail is encrypted, but the "
				"server's certificate and name are NOT checked - "
				"anyone on the path to %s can pose as it and read "
				"the alerts (and, with user =, the password)%s\n",
				cfg->mail.url, cfg->mail.ca_file ? "; ca_file is "
				"unused" : "");
	} else if (cfg->mail.pass) {
		LM_WARN("[secrets] mail is set but there is no [mail] section - "
			"it is unused\n");
	}

	if (stat(path, &st) == 0 && (st.st_mode & S_IROTH))
		LM_WARN("%s is world-readable and holds secrets - "
			"chmod 640 it\n", path);

	return 0;
}

int pc_config_load(const char *path, struct pc_config *cfg)
{
	struct pstate ps;
	char line[1024], *s, *eq, *val;
	FILE *f;

	memset(cfg, 0, sizeof *cfg);
	cfg->workers = (int)sysconf(_SC_NPROCESSORS_ONLN);
	cfg->io_backend = PC_IO_URING;   /* S361: epoll where refused */
	cfg->output_stall_s = 30;        /* S372 */
	cfg->address_wait_s = 30;        /* S294 */
	cfg->mail.timeout_s = 30;        /* S352 */
	cfg->mail.tls_verify = 1;
	cfg->mail.min_interval_s = 900;
	cfg->mail.digest_s = 60;
	cfg->mail.level = PC_MAIL_LVL_WARNING;
	cfg->upgrade.stage_mb = 256;                       /* UP6n */
	snprintf(cfg->upgrade.results_dir, sizeof cfg->upgrade.results_dir,
		"/var/lib/perfcached-upgrade");             /* UPS: the helper's default */
	cfg->alerts.cpu_warn = cfg->alerts.io_warn = 65;   /* S380: the operator's */
	cfg->alerts.mem_warn = cfg->alerts.arena_warn = 65;
	cfg->alerts.cpu_crit = cfg->alerts.io_crit = 75;
	cfg->alerts.mem_crit = cfg->alerts.arena_crit = 75;
	cfg->alerts.epoll_busy = 80;     /* S379 */
	cfg->keys_turn_us = PC_KEYS_TURN_US_DEFAULT;  /* S292 */
	cfg->grow_at_pct = 75;           /* S150: the operator's figure; 4/6 = 67 was the hardcode */
	cfg->autoscale = PC_AUTOSCALE_AUTO; /* S239: today's behaviour */
	cfg->redis_types = PC_TYPES_STRICT; /* S280: Redis 8's, by default */
	cfg->shrink_at_pct = 0;          /* S150 D: 0 = a quarter of grow_at_pct */
	/* S175: at six slots a bucket and a 75% target, Poisson(4.5) leaves
	 * ~7% of records in the leg whatever the drain does; 50% leaves
	 * ~1.7% for about half a megabyte more index per 62,000 records,
	 * which is the trade the operator asked for.  1% of entries stuck
	 * is the line at which the leg counts as a tax worth paying it. */
	cfg->grow_floor_pct = 50;
	cfg->leg_stuck_pct = 1;
	cfg->shrink_cooloff_s = 60;      /* S150 D: the operator's starting figure */
	cfg->prefetch_min_keys = 65536;  /* S232: ~16 MB LLC / ~200 B a record */
	cfg->slowlog_usec = 10000;
	cfg->stats_reset_days = 1;  /* S358: daily, so no count reaches billions */
	cfg->stats_reset_at_s = 0;  /* ... at local midnight */
	cfg->max_clients = 0;       /* S161: derived from the descriptor limit */
	cfg->memory_floor_pct = 15; /* S366: refuse clients under 15% free */
	cfg->memory_floor_mb = 0;
	cfg->pubsub_queue_mb = 16;  /* a burst's worth for one worker, not a flood's */
	cfg->query_log = 0;
	cfg->query_log_keys = 1;          /* hashed unless asked otherwise */
	cfg->http_timeout_s = 5;    /* S46: a scrape takes milliseconds */
	cfg->http_rate = 20;        /* 0.6.2: a page tab asks about once a second */
	cfg->http_cache_ms = 1000;  /* 0.6.2: the page polls every 3 s */
	cfg->enable_lockout_after = 5;   /* S417: a guessed privilege secret */
	cfg->enable_lockout_s = 60;
	cfg->install_mode = PC_INSTALL_AUTO;   /* PD-9 */
	cfg->pubsub_udp = 1;        /* PS5: only a client that asks gets UDP */
	cfg->keepalive_s = 300;     /* PS2: redis's tcp-keepalive default; a
	                             * dead subscriber is found by the kernel */
	if (cfg->workers < 1)
		cfg->workers = 1;
	if (cfg->workers > 512)
		cfg->workers = 512;
	cfg->log_level = L_INFO;
	cfg->arena_mb = 64;
	cfg->plaintext = PC_PLAINTEXT_NEVER;
	cfg->reclaim_keep = 1;
	cfg->pin_require = 0;                       /* S167: warn, as before */
	cfg->huge_pages_off = 0;                    /* S167: probe the ladder */
	cfg->reclaim_quiet_s = 5;
	cfg->reclaim_cooloff_s = 10;
	cfg->reclaim_giveback = 1;
	cfg->wal_segments = 8;
	cfg->wal_ring_kb = 1024;
	cfg->n_rdb_rules = -1;             /* -1 = the default rule set */
	cfg->cl_pull_timeout_ms = 80;
	cfg->cl_apply_stall_ms = 10000;         /* S216 */
	cfg->cl_apply_threads = 1;              /* S217 */
	cfg->cl_max_pending = 0;       /* 0 = the daemon's PEND_DEFAULT */
	cfg->cl_ps_rx_threads = 2;     /* PS11: 0 = relays on the cluster socket */
	cfg->cl_ps_port = 0;           /* PS11: 0 = the cluster port + 1 */
	cfg->cl_negative_ms = 300;
	cfg->cl_tombstone_ms = 2000;
	cfg->cl_sweep_digests = 1;         /* S260 */
	cfg->cl_reconcile_fast = 1;        /* S392 */
	cfg->cl_reconcile_rate_max = 1024; /* S393 */
	cfg->cl_mcast_ttl = 1;             /* S291: one L2 segment */
	cfg->cl_max_dgram = 1400;          /* S36b: unicast mode's lane threshold */
	cfg->cl_tomb_retain_s = 900;      /* S238 */
	cfg->cl_tomb_retain_max = 250000; /* S238: ~25 MB; 1,000,000 at most */

	memset(&ps, 0, sizeof ps);
	ps.path = path;
	ps.cfg = cfg;

	f = fopen(path, "r");
	if (!f) {
		LM_ERR("cannot open config %s\n", path);
		return -1;
	}
	while (fgets(line, sizeof line, f)) {
		ps.line++;
		if (strlen(line) == sizeof line - 1 && line[sizeof line - 2] != '\n') {
			PERR(&ps, "line too long\n");
			goto err;
		}
		s = trim(line);
		if (!*s || *s == '#')
			continue;
		if (*s == '[') {
			char *e = strchr(s, ']');

			if (!e || trim(e + 1)[0]) {
				PERR(&ps, "malformed section header\n");
				goto err;
			}
			*e = 0;
			if (enter_section(&ps, s + 1))
				goto err;
			continue;
		}
		eq = strchr(s, '=');
		if (!eq) {
			PERR(&ps, "expected key = value\n");
			goto err;
		}
		*eq = 0;
		val = parse_value(&ps, eq + 1);
		if (!val)
			goto err;
		if (handle_kv(&ps, trim(s), val))
			goto err;
	}
	fclose(f);

	/* S358: the environment beats the file, so `make check` can switch
	 * the schedule off for every suite - one that spans midnight would
	 * otherwise see its counters start again under it */
	{
		const char *e = getenv("PERFCACHED_STATS_RESET");

		if (e && *e && stats_reset_parse(e, &cfg->stats_reset_days)) {
			ps.line = 0;
			PERR(&ps, "PERFCACHED_STATS_RESET must be off or 1d .. 7d, "
				"not '%s'\n", e);
			goto err_noclose;
		}
	}
	if (validate(&ps, path, cfg))
		goto err_noclose;
	return 0;

err:
	fclose(f);
err_noclose:
	pc_config_free(cfg);
	return -1;
}

void pc_config_dump(const struct pc_config *cfg)
{
	static const char *lvl[] = { "?", "warn", "notice", "info", "dbg" };
	int i;

	printf("[daemon]\nworkers = %d\nio = %s\nbusy_poll_us = %d\nbusy_poll_prefer = %s\noutput_stall_s = %d\naddress_wait_s = %d\nkeys_turn_us = %d\nlog_level = %s\nstate_dir = %s\n\n",
		cfg->workers,
		cfg->io_backend == PC_IO_EPOLL ? "epoll" : "io_uring",
		cfg->busy_poll_us, cfg->busy_poll_prefer ? "yes" : "no",
		cfg->output_stall_s,
		cfg->address_wait_s, cfg->keys_turn_us,
		cfg->log_level < 0 ? (cfg->log_level == L_ERR ? "err" : "crit")
			: lvl[cfg->log_level],
		cfg->state_dir ? cfg->state_dir : "");
	printf("[memory]\narena_mb = %d\narena_cap_mb = %d\nbacking = %s\n"
		"reclaim_keep = %d\nreclaim_quiet_s = %d\nreclaim_cooloff_s = %d\n"
		"reclaim_giveback = %d\n\n",
		cfg->arena_mb, cfg->arena_cap_mb,
		cfg->backing_heap ? "heap" : "own",
		cfg->reclaim_keep, cfg->reclaim_quiet_s, cfg->reclaim_cooloff_s,
		cfg->reclaim_giveback);
	printf("[secrets]\n");
	for (i = 0; i < cfg->n_client_secrets; i++)
		printf("client = ******** (len %zu)\n",
			strlen(cfg->client_secret[i]));
	if (cfg->mail.pass)                                     /* S352 */
		printf("mail = ******** (len %zu)\n", strlen(cfg->mail.pass));
	if (cfg->cluster_secret)
		printf("cluster = ******** (len %zu)\n\n",
			strlen(cfg->cluster_secret));
	else
		printf("cluster = (not set)\n\n");
	printf("[alerts]\ncpu_warn_pct = %d\ncpu_crit_pct = %d\n"     /* S380 */
		"io_warn_pct = %d\nio_crit_pct = %d\nmemory_warn_pct = %d\n"
		"memory_crit_pct = %d\narena_warn_pct = %d\narena_crit_pct = %d\n"
		"epoll_busy_pct = %d\n\n",
		cfg->alerts.cpu_warn, cfg->alerts.cpu_crit, cfg->alerts.io_warn,
		cfg->alerts.io_crit, cfg->alerts.mem_warn, cfg->alerts.mem_crit,
		cfg->alerts.arena_warn, cfg->alerts.arena_crit,
		cfg->alerts.epoll_busy);
	{                                                  /* UP6n */
		int k, j;

		printf("[upgrade]\n");
		for (k = 0; k < pc_upg_n_release_keys; k++) {
			printf("# built in: release key ");
			for (j = 0; j < 32; j++)
				printf("%02x", pc_upg_release_keys[k][j]);
			printf("\n");
		}
		printf("trusted_keys = ");
		for (k = 0; k < cfg->upgrade.n_keys; k++) {
			for (j = 0; j < 32; j++)
				printf("%02x", cfg->upgrade.keys[k][j]);
			if (k + 1 < cfg->upgrade.n_keys)
				printf(", ");
		}
		printf("%s\nallow_unsigned = %s\nstage_mb = %d\nresults_dir = %s\n\n",
			cfg->upgrade.n_keys ? "" : "(none - the release key only)",
			cfg->upgrade.allow_unsigned ? "yes" : "no",
			cfg->upgrade.stage_mb, cfg->upgrade.results_dir);
	}
	if (cfg->mail_on) {                                     /* S352 */
		printf("[mail]\nsmtp = %s\n", cfg->mail.url);
		if (cfg->mail.user)
			printf("user = %s\n", cfg->mail.user);
		printf("from = %s\nto = ", cfg->mail.from);
		for (i = 0; i < cfg->mail.nto; i++)
			printf("%s%s", i ? ", " : "", cfg->mail.to[i]);
		printf("\n");
		if (cfg->mail.ca_file)
			printf("ca_file = %s\n", cfg->mail.ca_file);
		printf("tls_verify = %s\n", cfg->mail.tls_verify ? "yes" : "no");
		if (cfg->mail.subject_prefix)
			printf("subject_prefix = %s\n", cfg->mail.subject_prefix);
		printf("timeout_s = %d\nmin_interval_s = %d\ndigest_s = %d\n"
			"level = %s\n\n", cfg->mail.timeout_s,
			cfg->mail.min_interval_s, cfg->mail.digest_s,
			cfg->mail.level == PC_MAIL_LVL_CRITICAL ? "critical"
			: cfg->mail.level == PC_MAIL_LVL_INFO ? "info" : "warning");
	}
	printf("[listen]\nplaintext = %s\n",
		cfg->plaintext == PC_PLAINTEXT_LOOPBACK ? "loopback" : "never");
	for (i = 0; i < cfg->n_listen; i++) {
		const struct pc_listener *l = &cfg->listen[i];

		if (l->type == PC_LISTEN_UNIX)
			printf("unix = %s%s\n", l->addr,
				cfg->plaintext == PC_PLAINTEXT_LOOPBACK ?
				"   # plaintext-eligible" : "");
		else if (l->resp)
			printf("resp = %s:%d   # RESP2 only, plaintext\n",
				l->addr, l->port);
		else
			printf("tcp = %s:%d%s\n", l->addr, l->port,
				l->loopback && cfg->plaintext == PC_PLAINTEXT_LOOPBACK ?
				"   # plaintext-eligible" : "");
	}
	for (i = 0; i < cfg->n_resp_allow; i++) {
		struct in_addr a;
		unsigned int m = ntohl(cfg->resp_allow[i].mask);
		int bits = 0;

		while (m & 0x80000000u) { bits++; m <<= 1; }
		a.s_addr = cfg->resp_allow[i].net;
		printf("resp_allow = %s/%d\n", inet_ntoa(a), bits);
	}
	if (cfg->resp_collections)
		printf("resp_collections = %s\n", cfg->resp_collections);
	if (cfg->cl_enabled) {
		/* PS11: the two ports a firewall must pass, side by side */
		if (cfg->cl_unicast)                    /* S36 */
			printf("\n[cluster]\ndiscovery = unicast\nport = %d   # "
				"the cluster port, UDP and TCP\nseeds = %s\n"
				"max_datagram = %d\nlane_port = %d   # TCP, frames over "
				"max_datagram\nexpect = %d\n",
				cfg->cl_port, cfg->cl_seeds ? cfg->cl_seeds : "",
				cfg->cl_max_dgram, cfg->cl_lane_port, cfg->cl_expect);
		else
			printf("\n[cluster]\nmulticast = %s:%d   # the cluster port, unicast too\n",
				cfg->cl_mcast ? cfg->cl_mcast : "", cfg->cl_port);
		if (cfg->cl_cluster_id)
			printf("cluster_id = %s\n", cfg->cl_cluster_id);
		if (cfg->cl_max_members)
			printf("max_members = %d\n", cfg->cl_max_members);
		if (!cfg->cl_unicast)                   /* S36: no group to route */
			printf("multicast_ttl = %d%s\n", cfg->cl_mcast_ttl,
				cfg->cl_mcast_ttl > 1 ? "   # beyond one L2 segment: "
				"needs multicast routing" : "   # one L2 segment");
		{                                       /* S143 */
			static const char *ids[] = { "auto", "dmi", "mac", "env", "file" };

			printf("identity_source = %s\n", ids[cfg->cl_id_source]);
		}
		if (cfg->cl_advertise)
			printf("advertise = %s\n", cfg->cl_advertise);
		if (cfg->cl_ps_rx_threads)
			printf("pubsub_port = %d   # pub/sub relays, UDP\n"
				"pubsub_rx_threads = %d\n", cfg->cl_ps_port,
				cfg->cl_ps_rx_threads);
		else
			printf("pubsub_rx_threads = 0   # relays on the cluster port\n");
		printf("pubsub_relay = %s\n", cfg->cl_ps_interested
			? "interested   # peers relay only what may match here"
			: "all");
	}
	for (i = 0; i < cfg->n_col; i++)
		printf("\n[collection %s]\nbuckets_log2 = %d   # %u buckets\n"
			"mode = %s\n", cfg->col[i].name, cfg->col[i].buckets_log2,
			1u << cfg->col[i].buckets_log2,
			mode_name(cfg->col[i].mode, cfg->col[i].eager));
}

/* S59/CWE-14: a secret's heap copy must not outlive its use.  The
 * PSKs are derived once at startup and cached; nothing re-reads the
 * raw strings after that, so their bytes are zeroed through
 * sodium_memzero - which the optimizer cannot elide - before the
 * buffers are freed or abandoned.  NULL-tolerant: a failed mid-parse
 * leaves gaps where free(NULL) was legal but strlen(NULL) is not.
 * (resp_password and http_token are NOT wiped here: both doors
 * compare them for the process lifetime, so zeroing them at
 * derivation time would break every subsequent request rather than
 * protect anything - they are wiped at teardown instead.) */
#ifdef PC_TESTHOOKS
int pc_test_secret_wipes;
#endif
void pc_config_wipe_secrets(struct pc_config *cfg)
{
	int i;

	for (i = 0; i < cfg->n_client_secrets; i++) {
		if (!cfg->client_secret[i])
			continue;
		sodium_memzero(cfg->client_secret[i],
			strlen(cfg->client_secret[i]));
#ifdef PC_TESTHOOKS
		pc_test_secret_wipes++;
#endif
	}
	if (cfg->cluster_secret) {
		sodium_memzero(cfg->cluster_secret,
			strlen(cfg->cluster_secret));
#ifdef PC_TESTHOOKS
		pc_test_secret_wipes++;
#endif
	}
}

void pc_config_free(struct pc_config *cfg)
{
	int i;

	pc_config_wipe_secrets(cfg);
	for (i = 0; i < cfg->n_client_secrets; i++)
		free(cfg->client_secret[i]);
	free(cfg->cluster_secret);
	/* S37/S59: the http token is NOT wiped by pc_config_wipe_secrets.
	 * That runs as soon as the PSKs are derived, and the client and
	 * cluster secrets are genuinely finished at that point - this one
	 * is compared on EVERY request for the process lifetime, exactly
	 * like resp_password.  Wiping it there zeroed the token in place
	 * and every correct request was answered 401. */
	if (cfg->http_token)
		sodium_memzero(cfg->http_token, strlen(cfg->http_token));
	free(cfg->http_token);
	for (i = 0; i < cfg->n_listen; i++)
		free(cfg->listen[i].addr);
	free(cfg->wal_dir);
	free(cfg->state_dir);
	free(cfg->cl_mcast);
	free(cfg->cl_seeds);
	free(cfg->cl_advertise);
	free(cfg->cl_cluster_id);                /* S302 */
	for (i = 0; i < cfg->n_col; i++) {
		free(cfg->col[i].name);
		free(cfg->col[i].upstream);              /* S331 */
		free(cfg->col[i].up_prefixes);
		free(cfg->col[i].up_primary);            /* S382 */
		if (cfg->col[i].up_password)             /* S383 */
			sodium_memzero(cfg->col[i].up_password,
				strlen(cfg->col[i].up_password));
		free(cfg->col[i].up_password);
		if (cfg->col[i].up_wpassword)            /* AR7 */
			sodium_memzero(cfg->col[i].up_wpassword,
				strlen(cfg->col[i].up_wpassword));
		free(cfg->col[i].up_wpassword);
		free(cfg->col[i].up_wuser);
	}
	if (cfg->mail.pass)                                     /* S352 */
		sodium_memzero(cfg->mail.pass, strlen(cfg->mail.pass));
	free(cfg->mail.pass);
	free(cfg->mail.url);
	free(cfg->mail.user);
	free(cfg->mail.from);
	free(cfg->mail.ca_file);
	free(cfg->mail.subject_prefix);
	for (i = 0; i < cfg->mail.nto; i++)
		free(cfg->mail.to[i]);
	memset(cfg, 0, sizeof *cfg);
}

/* S239 */
int pc_types_parse(const char *s)
{
	if (!strcasecmp(s, "strict"))
		return PC_TYPES_STRICT;
	if (!strcasecmp(s, "loose"))
		return PC_TYPES_LOOSE;
	return -1;
}

const char *pc_types_name(int m)
{
	return m == PC_TYPES_LOOSE ? "loose" : "strict";
}

int pc_autoscale_parse(const char *s)
{
	if (!s)
		return -1;
	if (!strcmp(s, "auto"))
		return PC_AUTOSCALE_AUTO;
	if (!strcmp(s, "warn"))
		return PC_AUTOSCALE_WARN;
	if (!strcmp(s, "off"))
		return PC_AUTOSCALE_OFF;
	if (!strcmp(s, "floor"))                           /* S328 */
		return PC_AUTOSCALE_FLOOR;
	return -1;
}

const char *pc_autoscale_name(int m)
{
	return m == PC_AUTOSCALE_WARN ? "warn" : m == PC_AUTOSCALE_OFF ? "off"
		: m == PC_AUTOSCALE_FLOOR ? "floor" : "auto";
}
