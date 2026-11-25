/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* cfgcore.c - the config sections of the daemon itself: [daemon], [memory], [secrets], [listen]
 * and [wal]; one function a section, answering a key it knows or CFG_KV_UNKNOWN. */
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
#include "configint.h"

/* [daemon] */
int cfg_kv_daemon(struct pstate *ps, char *key, char *val)
{
	struct pc_config *cfg = ps->cfg;
	int lv;

	if (!strcasecmp(key, "allow_create")) {
		/* gates a client's ORIGINATION only: a create from the fleet reaches every member whatever
		 * its setting, so the set stays uniform and turning this on needs no fleet restart */
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
	if (!strcasecmp(key, "notify_events")) {
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
	if (!strcasecmp(key, "grow_at_pct"))
		return parse_int(ps, val, 10, 100, &cfg->grow_at_pct);
	if (!strcasecmp(key, "autoscale")) {
		if ((cfg->autoscale = pc_autoscale_parse(val)) < 0) {
			PERR(ps, "autoscale: auto|floor|warn|off\n");
			return -1;
		}
		return 0;
	}
	if (!strcasecmp(key, "redis_types")) {
		if ((cfg->redis_types = pc_types_parse(val)) < 0) {
			PERR(ps, "redis_types: strict|loose\n");
			return -1;
		}
		return 0;
	}
	if (!strcasecmp(key, "shrink_at_pct"))
		return parse_int(ps, val, 0, 99, &cfg->shrink_at_pct);
	if (!strcasecmp(key, "grow_floor_pct"))
		return parse_int(ps, val, 0, 100, &cfg->grow_floor_pct);
	if (!strcasecmp(key, "leg_stuck_pct"))
		return parse_int(ps, val, 0, 100, &cfg->leg_stuck_pct);
	if (!strcasecmp(key, "prefetch_min_keys"))
		return parse_int(ps, val, 0, 100000000, &cfg->prefetch_min_keys);
	if (!strcasecmp(key, "shrink_cooloff_s"))
		return parse_int(ps, val, 0, 86400, &cfg->shrink_cooloff_s);
	if (!strcasecmp(key, "workers"))
		return parse_int(ps, val, 1, 512, &cfg->workers);
	if (!strcasecmp(key, "busy_poll_us"))
		return parse_int(ps, val, 0, 100000, &cfg->busy_poll_us);
	if (!strcasecmp(key, "link_monitor")) {
		if (!*val || strlen(val) > 15 || strpbrk(val, "/ \t")) {
			PERR(ps, "link_monitor: auto | off | <device>, not '%s'\n", val);
			return -1;
		}
		snprintf(cfg->link_monitor, sizeof cfg->link_monitor, "%s",
			strcasecmp(val, "auto") ? val : "");
		return 0;
	}
	if (!strcasecmp(key, "link_speed_mbps"))
		return parse_int(ps, val, 0, 10000000, &cfg->link_speed_mbps);
	if (!strcasecmp(key, "busy_poll_prefer")) {
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
	if (!strcasecmp(key, "output_stall_s"))
		return parse_int(ps, val, 1, 3600, &cfg->output_stall_s);
	if (!strcasecmp(key, "io")) {
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
	if (!strcasecmp(key, "address_wait_s"))
		return parse_int(ps, val, 0, 600, &cfg->address_wait_s);
	if (!strcasecmp(key, "keys_turn_us"))
		return parse_int(ps, val, 0, 100000, &cfg->keys_turn_us);
	if (!strcasecmp(key, "slowlog_usec"))
		return parse_int(ps, val, 0, 60000000,
			&cfg->slowlog_usec);
	if (!strcasecmp(key, "stats_reset")) {
		if (stats_reset_parse(val, &cfg->stats_reset_days)) {
			PERR(ps, "stats_reset must be off or 1d .. 7d, "
				"not '%s'\n", val);
			return -1;
		}
		return 0;
	}
	if (!strcasecmp(key, "stats_reset_at")) {
		if (hhmm_parse(val, &cfg->stats_reset_at_s)) {
			PERR(ps, "stats_reset_at must be HH:MM (local "
				"time), not '%s'\n", val);
			return -1;
		}
		return 0;
	}
	if (!strcasecmp(key, "pubsub_queue_mb"))
		return parse_int(ps, val, 0, 65536, &cfg->pubsub_queue_mb);
	if (!strcasecmp(key, "max_clients"))
		return parse_int(ps, val, 0, 1000000, &cfg->max_clients);
	if (!strcasecmp(key, "enable_lockout_after"))
		return parse_int(ps, val, 0, 1000000, &cfg->enable_lockout_after);
	if (!strcasecmp(key, "enable_lockout_s"))
		return parse_int(ps, val, 1, 86400, &cfg->enable_lockout_s);
	if (!strcasecmp(key, "install_mode")) {
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
	if (!strcasecmp(key, "memory_floor")) {
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
	return CFG_KV_UNKNOWN;
}

/* [memory] */
int cfg_kv_memory(struct pstate *ps, char *key, char *val)
{
	struct pc_config *cfg = ps->cfg;

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
	if (!strcasecmp(key, "pin")) {
		cfg->pin_require = cfg->pin_off = 0;
		if (!strcasecmp(val, "require"))
			cfg->pin_require = 1;
		else if (!strcasecmp(val, "no"))
			cfg->pin_off = 1;
		else if (strcasecmp(val, "auto")) {
			PERR(ps, "pin: auto|require|no\n");
			return -1;
		}
		return 0;
	}
	if (!strcasecmp(key, "huge_pages")) {
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
	return CFG_KV_UNKNOWN;
}

/* [secrets] */
int cfg_kv_secrets(struct pstate *ps, char *key, char *val)
{
	struct pc_config *cfg = ps->cfg;

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
		/* refused empty - an empty privilege secret
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
	if (!strcasecmp(key, "cluster_accept")) {         /* a rotation's other secret */
		if (cfg->cluster_accept_secret) {
			PERR(ps, "duplicate cluster_accept secret\n");
			return -1;
		}
		cfg->cluster_accept_secret = strdup(val);
		return 0;
	}
	if (!strcasecmp(key, "http")) {
		/* a second layer, never the first: it rides plaintext and can be replayed - it only
		 * keeps one mis-scoped firewall rule from handing over the fleet's topology */
		if (cfg->http_token) {
			PERR(ps, "duplicate http token\n");
			return -1;
		}
		cfg->http_token = strdup(val);
		return 0;
	}
	if (!strcasecmp(key, "resp")) {
		/* a plain shared secret compared in constant time, not an Argon2id PSK: stock Redis
		 * clients can only send a password */
		if (cfg->resp_password) {
			PERR(ps, "duplicate resp secret\n");
			return -1;
		}
		cfg->resp_password = strdup(val);
		return 0;
	}
	if (!strcasecmp(key, "upstream")) {
		PERR(ps, "[secrets] upstream moved in 0.6.1: write "
			"upstream_password = <it> in each read-through "
			"[collection] - each may front its own Redis\n");
		return -1;
	}
	if (!strcasecmp(key, "mail")) {
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
	return CFG_KV_UNKNOWN;
}

static int yesno(struct pstate *ps, const char *key, const char *val, int *out)
{
	if (!strcasecmp(val, "yes") || !strcasecmp(val, "on"))
		*out = 1;
	else if (!strcasecmp(val, "no") || !strcasecmp(val, "off"))
		*out = 0;
	else {
		PERR(ps, "%s: yes|no\n", key);
		return -1;
	}
	return 0;
}

/* "<addr> [encrypt=yes|no]" - the suffix overrides [listen] encrypt */
static int add_native_listener(struct pstate *ps, enum pc_listen_type type,
		char *val)
{
	char *sp = strpbrk(val, " \t"), *opt;
	int enc = -1;

	if (sp) {
		*sp = 0;
		for (opt = sp + 1; *opt == ' ' || *opt == '\t'; opt++)
			;
		if (strncasecmp(opt, "encrypt=", 8)) {
			PERR(ps, "a native listener takes one option, "
				"encrypt=yes|no (got '%s')\n", opt);
			return -1;
		}
		if (yesno(ps, "encrypt", opt + 8, &enc))
			return -1;
	}
	if (add_listener(ps, type, val))
		return -1;
	ps->cfg->listen[ps->cfg->n_listen - 1].encrypt = enc;
	return 0;
}

/* "<addr:port> [tls]" - a RESP or HTTP door, over TLS when it says so */
static int add_door_listener(struct pstate *ps, char *val, int kind)
{
	char *sp = strpbrk(val, " \t"), *opt;
	int tls = 0;

	if (sp) {
		*sp = 0;
		for (opt = sp + 1; *opt == ' ' || *opt == '\t'; opt++)
			;
		if (strcasecmp(opt, "tls")) {
			PERR(ps, "a %s listener takes one option, tls (got '%s')\n",
				kind == 1 ? "resp" : "http", opt);
			return -1;
		}
		tls = 1;
	}
	if (add_listener_ex(ps, PC_LISTEN_TCP, val, kind))
		return -1;
	ps->cfg->listen[ps->cfg->n_listen - 1].tls = tls;
	return 0;
}

/* [listen] */
int cfg_kv_listen(struct pstate *ps, char *key, char *val)
{
	struct pc_config *cfg = ps->cfg;

	if (!strcasecmp(key, "tcp") || !strcasecmp(key, "unix"))
		return add_native_listener(ps, !strcasecmp(key, "tcp") ?
			PC_LISTEN_TCP : PC_LISTEN_UNIX, val);
	if (!strcasecmp(key, "encrypt"))
		return yesno(ps, key, val, &cfg->listen_encrypt);
	if (!strcasecmp(key, "resp"))
		return add_door_listener(ps, val, 1);
	if (!strcasecmp(key, "http"))
		return add_door_listener(ps, val, 2);
	if (!strcasecmp(key, "tls_cert") || !strcasecmp(key, "tls_key")) {
		char **dst = key[4] == 'c' ? &cfg->tls_cert : &cfg->tls_key;

		if (*dst || !*val) {
			PERR(ps, "%s: %s\n", key, *dst ? "given twice" : "empty");
			return -1;
		}
		*dst = strdup(val);
		return 0;
	}
	if (!strcasecmp(key, "auth_needs_tls"))
		return yesno(ps, key, val, &cfg->auth_needs_tls);
	if (!strcasecmp(key, "tls_min")) {
		if (strcmp(val, "1.2") && strcmp(val, "1.3")) {
			PERR(ps, "tls_min: 1.2 or 1.3\n");
			return -1;
		}
		cfg->tls_min13 = !strcmp(val, "1.3");
		return 0;
	}
	if (!strcasecmp(key, "http_timeout"))
		return parse_int(ps, val, 0, 3600,
			&cfg->http_timeout_s);
	if (!strcasecmp(key, "http_rate"))
		return parse_int(ps, val, 0, 100000, &cfg->http_rate);
	if (!strcasecmp(key, "http_cache_ms"))
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
	if (!strcasecmp(key, "keepalive_s"))
		return parse_int(ps, val, 0, 86400, &cfg->keepalive_s);
	if (!strcasecmp(key, "pubsub_udp")) {
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
		/* a key this node does not own: forward answers it (for a plain client), moved sends -MOVED
		 * (a cluster-aware client refreshes from it); per daemon, as clients cannot be told apart */
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
	return CFG_KV_UNKNOWN;
}

/* [wal] */
int cfg_kv_wal(struct pstate *ps, char *key, char *val)
{
	struct pc_config *cfg = ps->cfg;

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
	if (!strcasecmp(key, "segments") ||
	    !strcasecmp(key, "segments_min"))
		return parse_int(ps, val, 2, 64, &cfg->wal_segments);
	if (!strcasecmp(key, "segments_max"))
		return parse_int(ps, val, 2, 256, &cfg->wal_segments_max);
	if (!strcasecmp(key, "ring_kb")) {
		cfg->wal_ring_kb_set = 1;
		return parse_int(ps, val, 4, 16384, &cfg->wal_ring_kb);
	}
	if (!strcasecmp(key, "on_full")) {
		if (!strcasecmp(val, "wait"))
			cfg->wal_on_full = 0;
		else if (!strcasecmp(val, "heal"))
			cfg->wal_on_full = 1;
		else {
			PERR(ps, "on_full: wait|heal\n");
			return -1;
		}
		return 0;
	}
	if (!strcasecmp(key, "unknown_records")) {
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
				/* Redis's manual invites this, so say
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
	return CFG_KV_UNKNOWN;
}
