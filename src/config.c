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
#include "pcaddr.h"
#include "config.h"
#include "colname.h"
#include "events.h"
#include "relkey.h"
#include "configint.h"                 /* the parser's state (wave 1) */
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


char *trim(char *s)
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

int parse_int(struct pstate *ps, const char *v, int lo, int hi,
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

int level_of(const char *v)
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
int mail_url_ok(struct pstate *ps, const char *url)
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
int mail_addr_ok(const char *a)
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
		ps->col->up_rewarm_max = 100000;         /* S421 */
		ps->col->up_echo_ms = 2000;              /* S422 */
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
int mode_of(const char *v, int *eager)
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

const char *mode_name(enum pc_pull_mode m, int eager)
{
	switch (m) {
	case PC_MODE_PROXY: return "proxy";
	case PC_MODE_SHARD: return "shard";
	case PC_MODE_SPREAD: return "spread";
	default:            return eager ? "eager" : "store";
	}
}

/* @kind: 0 native, 1 RESP-only (S33), 2 HTTP metrics (S46) */
/* IP6-1: "host:port" or "[v6]:port" through the one parser (pcaddr.c),
 * the old words kept for what it refuses.  Without brackets the port is
 * after the last colon, as this file always split it; the port is a plain
 * decimal number now (strtol also took "+80" and " 80") */
int split_hostport(struct pstate *ps, const char *v, const char *form,
		char **host, int *port)
{
	char h[256];
	const char *c;

	if (pc_hostport_split(v, h, sizeof h, port) == 0) {
		*host = strdup(h);
		return *host ? 0 : -1;
	}
	c = strrchr(v, ':');
	if (!c || c == v || (*v == '[' && !strchr(v, ']')))
		PERR(ps, "%s\n", form);
	else
		PERR(ps, "'%s' is not a port in [1..65535] (%s)\n", c + 1, form);
	return -1;
}

int add_listener_ex(struct pstate *ps, enum pc_listen_type type,
		const char *v, int kind)
{
	struct pc_config *cfg = ps->cfg;
	struct pc_listener *l;
	char *addr = NULL;     /* gcc 13 cannot see split_hostport set it */
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
		pc_addr a6;
		const char *why;

		if (split_hostport(ps, v, "tcp listener must be host:port",
		        &addr, &port))
			return -1;
		/* 12np: an EXPLICIT IPv6 listener on a host without IPv6 is
		 * refused by name - never silently dropped, never IPv4 */
		if (pc_addr_parse(addr, 0, &a6) == 0 &&
		    pc_addr_family(&a6) == AF_INET6 && !pc_ipv6_usable(&why)) {
			PERR(ps, "listener [%s]:%d is IPv6, but IPv6 is not usable "
				"on this host - %s\n", addr, port, why);
			free(addr);
			return -1;
		}
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

int add_listener(struct pstate *ps, enum pc_listen_type type,
		const char *v)
{
	return add_listener_ex(ps, type, v, 0);
}

/* one "a.b.c.d/len" (or a bare address = /32) into an allow-list.
 * Shared by resp_allow and http_allow: both guard a listener that
 * has no handshake, so the network is its authentication.  12np IP6-2:
 * or "v6/len" ("[v6]/len" too; a bare IPv6 address = /128) */
static int add_allow(struct pstate *ps, const char *v, const char *what,
		struct pc_cidr *list, int *n)
{
	char buf[64], *slash, *host;
	struct in_addr ia;
	pc_addr a6;
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
	if (slash)
		*slash = 0;
	host = buf;
	if (*host == '[' && host[strlen(host) - 1] == ']') {
		host[strlen(host) - 1] = 0;
		host++;
	}
	memset(&list[*n], 0, sizeof list[*n]);
	if (inet_aton(host, &ia)) {
		/* IPv4 as before - inet_aton's short forms ("10/8") included */
		if (slash && parse_int(ps, slash + 1, 0, 32, &len))
			return -1;
		list[*n].family = AF_INET;
		memcpy(list[*n].net, &ia, 4);
	} else if (pc_addr_parse(host, 0, &a6) == 0 &&
	           pc_addr_family(&a6) == AF_INET6) {
		len = 128;
		if (slash && parse_int(ps, slash + 1, 0, 128, &len))
			return -1;
		list[*n].family = AF_INET6;
		memcpy(list[*n].net, a6.in6.sin6_addr.s6_addr, 16);
	} else {
		PERR(ps, "%s: '%s' is not an IPv4 or IPv6 address or CIDR\n",
			what, buf);
		return -1;
	}
	list[*n].bits = len;
	{
		/* the network, masked */
		int i, nb = list[*n].family == AF_INET ? 4 : 16;

		for (i = 0; i < nb; i++) {
			int keep = len - i * 8;

			if (keep <= 0)
				list[*n].net[i] = 0;
			else if (keep < 8)
				list[*n].net[i] &= (unsigned char)(0xFFu << (8 - keep));
		}
	}
	(*n)++;
	return 0;
}

int add_resp_allow(struct pstate *ps, const char *v)
{
	return add_allow(ps, v, "resp_allow", ps->cfg->resp_allow,
		&ps->cfg->n_resp_allow);
}

int add_http_allow(struct pstate *ps, const char *v)
{
	return add_allow(ps, v, "http_allow", ps->cfg->http_allow,
		&ps->cfg->n_http_allow);
}

/* S358: "off" or 1d..7d - how often /stats and the page start counting
 * again.  0 days = never. */
int stats_reset_parse(const char *v, int *days)
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
int hhmm_parse(const char *v, int *secs)
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
	int rc = CFG_KV_UNKNOWN;

	switch (ps->sect) {
	case S_DAEMON:
		rc = cfg_kv_daemon(ps, key, val);
		break;
	case S_MEMORY:
		rc = cfg_kv_memory(ps, key, val);
		break;
	case S_SECRETS:
		rc = cfg_kv_secrets(ps, key, val);
		break;
	case S_LISTEN:
		rc = cfg_kv_listen(ps, key, val);
		break;
	case S_WAL:
		rc = cfg_kv_wal(ps, key, val);
		break;
	case S_CLUSTER:
		rc = cfg_kv_cluster(ps, key, val);
		break;
	case S_COLLECTION:
		rc = cfg_kv_collection(ps, key, val);
		break;
	case S_MAIL:                                     /* S352 */
		rc = cfg_kv_mail(ps, key, val);
		break;
	case S_ALERTS:                                   /* S380 */
		rc = cfg_kv_alerts(ps, key, val);
		break;
	case S_UPGRADE:                                  /* UP6n */
		rc = cfg_kv_upgrade(ps, key, val);
		break;
	case S_NONE:
		PERR(ps, "key outside any section\n");
		return -1;
	}
	if (rc != CFG_KV_UNKNOWN)
		return rc;
	PERR(ps, "unknown key '%s' in this section\n", key);
	return -1;
}

/* 12nw: the keymap's digest key, minted once per process */
static unsigned char km_key[crypto_generichash_KEYBYTES];
static int km_key_set;

static void km_norm(char *dst, size_t cap, const char *src)
{
	size_t n = 0;
	int sp = 0;

	for (; *src && n + 1 < cap; src++) {
		unsigned char ch = (unsigned char)*src;

		if (ch == ' ' || ch == '\t') {
			sp = n > 0;
			continue;
		}
		if (sp && n + 2 < cap)
			dst[n++] = ' ';
		sp = 0;
		dst[n++] = (char)(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
	}
	dst[n] = 0;
}

static void km_add(struct pc_keymap *km, const char *sect, const char *key,
		const char *val)
{
	struct pc_km_ent *e;

	if (!km)
		return;
	if (km->n >= PC_KM_MAX) {
		km->overflow = 1;
		return;
	}
	if (!km_key_set) {
		randombytes_buf(km_key, sizeof km_key);
		km_key_set = 1;
	}
	e = &km->e[km->n++];
	km_norm(e->sect, sizeof e->sect, sect);
	km_norm(e->key, sizeof e->key, key);
	crypto_generichash(e->h, sizeof e->h, (const unsigned char *)val,
		strlen(val), km_key, sizeof km_key);
}

int pc_config_load(const char *path, struct pc_config *cfg)
{
	return pc_config_load_km(path, cfg, NULL);
}

int pc_config_load_km(const char *path, struct pc_config *cfg,
		struct pc_keymap *km)
{
	struct pstate ps;
	char line[1024], *s, *eq, *val;
	char cursect[96] = "";
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
	cfg->cl_tomb_acks = 1;             /* 12ny AT-5 */
	cfg->cl_data_mcast = 1;            /* 12ny AT-9: auto */
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
			snprintf(cursect, sizeof cursect, "%s", s + 1);  /* 12nw: before enter_section splits it */
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
		km_add(km, cursect, trim(s), val);             /* 12nw */
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
	if (cfg_validate(&ps, path, cfg))
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
		char t[INET6_ADDRSTRLEN];

		if (!inet_ntop(cfg->resp_allow[i].family, cfg->resp_allow[i].net,
		        t, sizeof t))
			snprintf(t, sizeof t, "?");
		printf("resp_allow = %s/%d\n", t, cfg->resp_allow[i].bits);
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
	for (i = 0; i < cfg->n_col; i++) {
		printf("\n[collection %s]\nbuckets_log2 = %d   # %u buckets\n"
			"mode = %s\n", cfg->col[i].name, cfg->col[i].buckets_log2,
			1u << cfg->col[i].buckets_log2,
			mode_name(cfg->col[i].mode, cfg->col[i].eager));
		if (cfg->col[i].nondurable)                  /* G5 */
			printf("durable = no   # a cache: off the WAL and the "
				"snapshot, empty after a restart\n");
	}
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
