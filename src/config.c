/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * config.c — INI-style config parser + strict validation (task S5).
 * See config.h for the contract and the hard rules.
 */
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

enum sect { S_NONE, S_DAEMON, S_MEMORY, S_SECRETS, S_LISTEN, S_WAL, S_CLUSTER, S_COLLECTION };

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

static int name_ok(const char *s)
{
	size_t i, n = strlen(s);

	if (!n || n > 32)
		return 0;
	for (i = 0; i < n; i++)
		if (!((s[i] >= 'a' && s[i] <= 'z') ||
		      (s[i] >= 'A' && s[i] <= 'Z') ||
		      (s[i] >= '0' && s[i] <= '9') ||
		      s[i] == '_' || s[i] == '-'))
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
		ps->sect = S_CLUSTER;
		ps->cfg->cl_enabled = 1;
	} else if (!strcasecmp(hdr, "collection")) {
		if (!arg || !name_ok(arg)) {
			PERR(ps, "[collection <name>]: name missing or invalid "
				"(1-32 of [a-zA-Z0-9_-])\n");
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
	} else {
		PERR(ps, "unknown section [%s]\n", hdr);
		return -1;
	}
	return 0;
}

/* "store|proxy|shard" -> enum; -1 = not a mode word */
static int mode_of(const char *v)
{
	if (!strcasecmp(v, "store"))
		return PC_MODE_STORE;
	if (!strcasecmp(v, "proxy"))
		return PC_MODE_PROXY;
	if (!strcasecmp(v, "shard"))
		return PC_MODE_SHARD;
	return -1;
}

static const char *mode_name(enum pc_pull_mode m)
{
	switch (m) {
	case PC_MODE_PROXY: return "proxy";
	case PC_MODE_SHARD: return "shard";
	default:            return "store";
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

static int handle_kv(struct pstate *ps, char *key, char *val)
{
	struct pc_config *cfg = ps->cfg;
	int lv;

	switch (ps->sect) {
	case S_DAEMON:
		if (!strcasecmp(key, "workers"))
			return parse_int(ps, val, 1, 512, &cfg->workers);
		if (!strcasecmp(key, "slowlog_usec"))
			return parse_int(ps, val, 0, 60000000,
				&cfg->slowlog_usec);
		if (!strcasecmp(key, "log_level")) {
			lv = level_of(val);
			if (lv == -100) {
				PERR(ps, "log_level: crit|err|warn|notice|info|dbg\n");
				return -1;
			}
			cfg->log_level = lv;
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
		if (!strcasecmp(key, "reclaim_giveback"))
			return parse_int(ps, val, 0, 1, &cfg->reclaim_giveback);
		if (!strcasecmp(key, "reclaim_floor_mb"))
			return parse_int(ps, val, 0, 1048576,
				&cfg->reclaim_floor_mb);
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
		if (!strcasecmp(key, "advertise")) {
			free(cfg->cl_advertise);
			cfg->cl_advertise = strdup(val);
			return 0;
		}
		/* S30: the cluster owns mode + the clustered collection set */
		if (!strcasecmp(key, "mode")) {
			int m = mode_of(val);

			if (m < 0) {
				PERR(ps, "[cluster] mode: store|proxy|shard\n");
				return -1;
			}
			cfg->cl_mode = (enum pc_pull_mode)m;
			cfg->cl_has_mode = 1;
			return 0;
		}
		if (!strcasecmp(key, "eager"))
			return parse_int(ps, val, 0, 1, &cfg->cl_eager);
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
		if (!strcasecmp(key, "max_pending"))
			return parse_int(ps, val, 64, 65536,
				&cfg->cl_max_pending);
		if (!strcasecmp(key, "negative_ms"))
			return parse_int(ps, val, 0, 60000, &cfg->cl_negative_ms);
		if (!strcasecmp(key, "tombstone_ms"))
			return parse_int(ps, val, 100, 60000, &cfg->cl_tombstone_ms);
		break;
	case S_COLLECTION:
		if (!strcasecmp(key, "buckets_log2"))
			return parse_int(ps, val, 1, 24, &ps->col->buckets_log2);
		if (!strcasecmp(key, "pull"))
			return parse_int(ps, val, 0, 1, &ps->col->pull);
		if (!strcasecmp(key, "eager"))
			return parse_int(ps, val, 0, 1, &ps->col->eager);
		if (!strcasecmp(key, "mode")) {
			int m = mode_of(val);

			if (m < 0) {
				PERR(ps, "mode: store|proxy|shard\n");
				return -1;
			}
			ps->col->mode = (enum pc_pull_mode)m;
			ps->col->has_mode = 1;
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

static int validate(struct pstate *ps, const char *path,
		struct pc_config *cfg)
{
	struct stat st;
	int i, j;

	ps->line = 0;                      /* rule errors are not line errors */

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
		if (cfg->col[i].eager &&
		        (cfg->col[i].mode != PC_MODE_STORE ||
		         !cfg->cl_enabled)) {
			PERR(ps, "collection '%s': eager needs mode=store "
				"and [cluster]\n", cfg->col[i].name);
			return -1;
		}
		if (cfg->col[i].mode != PC_MODE_STORE && !cfg->cl_enabled) {
			PERR(ps, "collection '%s': mode=%s needs [cluster]\n",
				cfg->col[i].name,
				cfg->col[i].mode == PC_MODE_PROXY ? "proxy"
					: "shard");
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
	if (!cfg->cluster_secret) {
		PERR(ps, "no cluster secret configured\n");
		return -1;
	}
	for (i = 0; i < cfg->n_client_secrets; i++) {
		if (!cfg->client_secret[i][0]) {
			PERR(ps, "empty client secret\n");
			return -1;
		}
		/* THE two-principal rule: structurally different secrets */
		if (!strcmp(cfg->client_secret[i], cfg->cluster_secret)) {
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
	if (!cfg->cluster_secret[0]) {
		PERR(ps, "empty cluster secret\n");
		return -1;
	}
	if (strlen(cfg->cluster_secret) < 8)
		LM_WARN("the cluster secret is under 8 characters\n");

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
			if (!strcmp(cfg->resp_password, cfg->cluster_secret)) {
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

	if (sizeof(void *) == 4 && cfg->arena_mb > 1536) {
		PERR(ps, "arena_mb %d exceeds the 32-bit cap (1536)\n",
			cfg->arena_mb);
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

	if (cfg->cl_enabled && !cfg->cl_mcast) {
		PERR(ps, "[cluster] needs multicast = group:port\n");
		return -1;
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
				"store|proxy|shard: one cluster, one mode\n");
			return -1;
		}
		if (cfg->cl_eager && cfg->cl_mode != PC_MODE_STORE) {
			PERR(ps, "[cluster] eager needs mode = store\n");
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
			if (cfg->col[i].has_mode &&
			        cfg->col[i].mode != cfg->cl_mode) {
				PERR(ps, "[collection %s] mode = %s contradicts "
					"[cluster] mode = %s - the cluster owns the "
					"mode; remove the local one\n",
					cfg->col[i].name,
					mode_name(cfg->col[i].mode),
					mode_name(cfg->cl_mode));
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
	} else if (cfg->cl_has_mode || cfg->n_cl_col || cfg->cl_eager) {
		PERR(ps, "[cluster] mode/collections/eager without a "
			"[cluster] section\n");
		return -1;
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
	cfg->slowlog_usec = 10000;
	cfg->http_timeout_s = 5;    /* S46: a scrape takes milliseconds */
	if (cfg->workers < 1)
		cfg->workers = 1;
	if (cfg->workers > 512)
		cfg->workers = 512;
	cfg->log_level = L_INFO;
	cfg->arena_mb = 64;
	cfg->plaintext = PC_PLAINTEXT_NEVER;
	cfg->reclaim_keep = 1;
	cfg->reclaim_quiet_s = 5;
	cfg->reclaim_cooloff_s = 10;
	cfg->reclaim_giveback = 1;
	cfg->wal_segments = 8;
	cfg->wal_ring_kb = 1024;
	cfg->n_rdb_rules = -1;             /* -1 = the default rule set */
	cfg->cl_pull_timeout_ms = 80;
	cfg->cl_max_pending = 0;       /* 0 = the daemon's PEND_DEFAULT */
	cfg->cl_negative_ms = 300;
	cfg->cl_tombstone_ms = 2000;

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

	printf("[daemon]\nworkers = %d\nlog_level = %s\n\n", cfg->workers,
		cfg->log_level < 0 ? (cfg->log_level == L_ERR ? "err" : "crit")
			: lvl[cfg->log_level]);
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
	printf("cluster = ******** (len %zu)\n\n",
		strlen(cfg->cluster_secret));
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
	for (i = 0; i < cfg->n_col; i++)
		printf("\n[collection %s]\nbuckets_log2 = %d   # %u buckets\n"
			"mode = %s\n", cfg->col[i].name, cfg->col[i].buckets_log2,
			1u << cfg->col[i].buckets_log2,
			cfg->col[i].mode == PC_MODE_PROXY ? "proxy" : "store");
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
	free(cfg->cl_mcast);
	free(cfg->cl_advertise);
	for (i = 0; i < cfg->n_col; i++)
		free(cfg->col[i].name);
	memset(cfg, 0, sizeof *cfg);
}
