/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* cfgcluster.c - the config sections of the cluster plane: [cluster] and [collection X]. */
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

/* [cluster] */
int cfg_kv_cluster(struct pstate *ps, char *key, char *val)
{
	struct pc_config *cfg = ps->cfg;

	if (!strcasecmp(key, "cluster_id")) {
		free(cfg->cl_cluster_id);
		cfg->cl_cluster_id = cfg_dup(ps, val);
		return cfg->cl_cluster_id ? 0 : -1;
	}
	if (!strcasecmp(key, "max_members"))
		return parse_int(ps, val, 0, 257, &cfg->cl_max_members);
	if (!strcasecmp(key, "multicast_ttl"))
		return parse_int(ps, val, 1, 255, &cfg->cl_mcast_ttl);
	if (!strcasecmp(key, "discovery")) {
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
	if (!strcasecmp(key, "seeds")) {
		free(cfg->cl_seeds);
		cfg->cl_seeds = cfg_dup(ps, val);
		return cfg->cl_seeds ? 0 : -1;
	}
	if (!strcasecmp(key, "identity_source")) {
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
	if (!strcasecmp(key, "expect"))
		return parse_int(ps, val, 0, 257, &cfg->cl_expect);
	if (!strcasecmp(key, "max_datagram"))
		return parse_int(ps, val, 0, 65000, &cfg->cl_max_dgram);
	if (!strcasecmp(key, "lane_port"))
		return parse_int(ps, val, 1, 65535, &cfg->cl_lane_port);
	if (!strcasecmp(key, "lane_retry_s"))
		return parse_int(ps, val, 1, 3600, &cfg->cl_lane_retry_s);
	if (!strcasecmp(key, "port"))
		return parse_int(ps, val, 1, 65535, &cfg->cl_port);
	if (!strcasecmp(key, "multicast")) {
		char *grp = NULL;          /* gcc 13 -flto cannot see split_hostport set it */

		if (split_hostport(ps, val, "multicast must be group:port",
		        &grp, &cfg->cl_port))
			return -1;
		free(cfg->cl_mcast);
		cfg->cl_mcast = grp;
		return 0;
	}
	if (!strcasecmp(key, "pubsub_rx_threads")) {
		if (!strcasecmp(val, "auto")) {
			cfg->cl_ps_rx_threads = -1;
			return 0;
		}
		return parse_int(ps, val, 0, 15, &cfg->cl_ps_rx_threads);   /* one a lane */
	}
	if (!strcasecmp(key, "pubsub_port"))
		return parse_int(ps, val, 1, 65535, &cfg->cl_ps_port);
	if (!strcasecmp(key, "pubsub_relay")) {
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
		cfg->cl_advertise = cfg_dup(ps, val);
		return cfg->cl_advertise ? 0 : -1;
	}
	/* mixed into a platform-derived identity so one machine in two fleets is two members; not
	 * the cluster PSK, so rotating a secret does not re-mint identities.  Unset = file/random. */
	if (!strcasecmp(key, "site_salt")) {
		free(cfg->cl_site_salt);
		cfg->cl_site_salt = cfg_dup(ps, val);
		return cfg->cl_site_salt ? 0 : -1;
	}
	/* the cluster owns mode + the clustered collection set */
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
	/* K for mode = spread, range-checked here; the rules that need
	 * the whole config are in validate() */
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
		cfg->cl_collections = cfg_dup(ps, val);
		dup = cfg_dup(ps, val);
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
			if (!pc_col_name_ok(tok, strlen(tok))) {
				char pb[160];

				PERR(ps, "[cluster] collections: '%s' is not a "
					"collection name (" PC_COL_NAME_RULE ")\n",
					pc_col_name_print(pb, sizeof pb, tok, strlen(tok)));
				free(dup);
				return -1;
			}
			if (!(cfg->cl_colname[cfg->n_cl_col] = cfg_dup(ps, tok))) {
				free(dup);
				return -1;
			}
			cfg->n_cl_col++;
		}
		free(dup);
		return 0;
	}
	/* the retired static topology: a hard error with the migration hint, never silently ignored */
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
	/* 0 turns the detector off; under a second it would
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
	if (!strcasecmp(key, "apply_threads"))
		return parse_int(ps, val, 1, 16, &cfg->cl_apply_threads);
	if (!strcasecmp(key, "max_pending"))
		return parse_int(ps, val, 64, 65536,
			&cfg->cl_max_pending);
	if (!strcasecmp(key, "negative_ms"))
		return parse_int(ps, val, 0, 60000, &cfg->cl_negative_ms);
	if (!strcasecmp(key, "tombstone_ms"))
		return parse_int(ps, val, 100, 60000, &cfg->cl_tombstone_ms);
	if (!strcasecmp(key, "sweep_digests")) {
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
	if (!strcasecmp(key, "reconcile_rate_max"))
		return parse_int(ps, val, 64, 65536,
			&cfg->cl_reconcile_rate_max);
	if (!strcasecmp(key, "reconcile_fast")) {
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
	/* how long a delete is remembered past the crossing window, and how many -
	 * a partition healing inside it cannot undo a delete */
	if (!strcasecmp(key, "tombstone_retain_s"))
		return parse_int(ps, val, 0, 86400, &cfg->cl_tomb_retain_s);
	/* a node away longer than this drops what it stored and pulls the fleet's state instead of
	 * a record-by-record reconcile (0: never by time - only when no witness can rule) */
	if (!strcasecmp(key, "resync_after_s"))
		return parse_int(ps, val, 0, 31536000, &cfg->cl_resync_after_s);
	if (!strcasecmp(key, "tombstone_retain_max"))
		return parse_int(ps, val, 1000, 1000000,
			&cfg->cl_tomb_retain_max);
	/* forget a delete once every member holds it (yes), or keep each tombstone_retain_s (no);
	 * a node with no announces no marks, keeping the whole fleet on the time rule */
	if (!strcasecmp(key, "tombstone_acks")) {
		if (!strcasecmp(val, "yes") || !strcasecmp(val, "on"))
			cfg->cl_tomb_acks = 1;
		else if (!strcasecmp(val, "no") || !strcasecmp(val, "off"))
			cfg->cl_tomb_acks = 0;
		else {
			PERR(ps, "tombstone_acks: yes|no\n");
			return -1;
		}
		return 0;
	}
	/* on a multicast fleet a delete goes to the group once (auto) or one copy a peer (off) -
	 * per node, no agreement needed: a receiver does not care which socket a frame came in on */
	if (!strcasecmp(key, "data_multicast")) {
		if (!strcasecmp(val, "auto") || !strcasecmp(val, "yes") ||
		    !strcasecmp(val, "on"))
			cfg->cl_data_mcast = 1;
		else if (!strcasecmp(val, "off") || !strcasecmp(val, "no"))
			cfg->cl_data_mcast = 0;
		else {
			PERR(ps, "data_multicast: auto|off\n");
			return -1;
		}
		return 0;
	}
	return CFG_KV_UNKNOWN;
}

/* [collection X] */
int cfg_kv_collection(struct pstate *ps, char *key, char *val)
{
	/* 4..24, as the create and resize verbs take: segments are fixed at 4,096 buckets,
	 * so anything below 2^12 carves the same index anyway */
	if (!strcasecmp(key, "buckets_log2"))
		return parse_int(ps, val, 4, 24, &ps->col->buckets_log2);
	if (!strcasecmp(key, "log_events")) {
		if (pc_ev_parse(val, &ps->col->log_events) != 0) {
			PERR(ps, "log_events: miss|expired|store|remove|all|none\n");
			return -1;
		}
		ps->col->has_log_events = 1;
		return 0;
	}
	if (!strcasecmp(key, "notify_events")) {
		if (pc_ev_parse(val, &ps->col->notify_events) != 0) {
			PERR(ps, "notify_events: miss|expired|store|remove|all|none\n");
			return -1;
		}
		ps->col->has_notify_events = 1;
		return 0;
	}
	if (!strcasecmp(key, "pull"))
		return parse_int(ps, val, 0, 1, &ps->col->pull);
	if (!strcasecmp(key, "autoscale")) {
		if ((ps->col->autoscale = pc_autoscale_parse(val)) < 0) {
			PERR(ps, "autoscale: auto|floor|warn|off\n");
			return -1;
		}
		ps->col->has_autoscale = 1;
		return 0;
	}
	if (!strcasecmp(key, "redis_types")) {
		if ((ps->col->redis_types = pc_types_parse(val)) < 0) {
			PERR(ps, "redis_types: strict|loose\n");
			return -1;
		}
		ps->col->has_redis_types = 1;
		return 0;
	}
	if (!strcasecmp(key, "upstream")) {
		/* one or more, comma-separated */
		const char *p = val;
		int n = 0, tls = -1;

		for (;;) {
			const char *e = strchr(p, ',');
			size_t l = e ? (size_t)(e - p) : strlen(p);
			int s;

			while (l && (*p == ' ' || *p == '\t')) {
				p++;
				l--;
			}
			while (l && (p[l - 1] == ' ' || p[l - 1] == '\t'))
				l--;
			s = l > 9 && !strncmp(p, "rediss://", 9);
			if (!s && (l <= 8 || strncmp(p, "redis://", 8))) {
				PERR(ps, "upstream: redis://[user@]host:port[/db] or rediss://..., "
					"or several, comma-separated\n");
				return -1;
			}
			if (tls >= 0 && tls != s) {
				PERR(ps, "upstream: all redis:// or all rediss:// - what is found "
					"from one (replicas, a sentinel's servers) is dialled the same way\n");
				return -1;
			}
			tls = s;
			if (++n > PC_UP_MAX_SRV) {
				PERR(ps, "upstream: at most %d\n", PC_UP_MAX_SRV);
				return -1;
			}
			if (!e)
				break;
			p = e + 1;
		}
		free(ps->col->upstream);
		ps->col->upstream = cfg_dup(ps, val);
		ps->col->up_tls = tls;
		return ps->col->upstream ? 0 : -1;
	}
	if (!strncasecmp(key, "upstream_tls_", 13) && strcasecmp(key, "upstream_tls_verify")) {
		char **dst = !strcasecmp(key + 13, "ca") ? &ps->col->up_tls_ca :
			!strcasecmp(key + 13, "name") ? &ps->col->up_tls_name :
			!strcasecmp(key + 13, "cert") ? &ps->col->up_tls_cert :
			!strcasecmp(key + 13, "key") ? &ps->col->up_tls_key : NULL;

		if (!dst) {
			PERR(ps, "unknown key '%s' (upstream_tls_ca, _name, _cert, _key, _verify)\n", key);
			return -1;
		}
		if (*dst || !*val) {
			PERR(ps, "%s: %s\n", key, *dst ? "given twice" : "empty");
			return -1;
		}
		*dst = cfg_dup(ps, val);
		return *dst ? 0 : -1;
	}
	if (!strcasecmp(key, "upstream_tls_verify")) {
		if (!strcasecmp(val, "yes") || !strcasecmp(val, "no")) {
			ps->col->up_tls_verify = !strcasecmp(val, "yes");
			ps->col->up_tls_verify_set = 1;
			return 0;
		}
		PERR(ps, "upstream_tls_verify: yes|no\n");
		return -1;
	}
	if (!strcasecmp(key, "upstream_role")) {
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
	if (!strcasecmp(key, "upstream_master_fallback")) {
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
	if (!strcasecmp(key, "upstream_max_lag_ms"))
		return parse_int(ps, val, 0, 3600000,
			&ps->col->up_max_lag_ms);
	if (!strcasecmp(key, "upstream_mode")) {
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
	if (!strcasecmp(key, "upstream_primary")) {
		free(ps->col->up_primary);
		ps->col->up_primary = cfg_dup(ps, val);
		return ps->col->up_primary ? 0 : -1;
	}
	if (!strcasecmp(key, "durable")) {
		if (!strcasecmp(val, "yes"))
			ps->col->nondurable = 0;
		else if (!strcasecmp(val, "no"))
			ps->col->nondurable = 1;
		else {
			PERR(ps, "durable: yes|no\n");
			return -1;
		}
		return 0;
	}
	if (!strcasecmp(key, "upstream_preempt")) {
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
	if (!strcasecmp(key, "upstream_preempt_hold_s")) {
		ps->col->has_up_preempt = 1;
		return parse_int(ps, val, 15, 300,
			&ps->col->up_preempt_hold_s);
	}
	if (!strcasecmp(key, "upstream_allow_perfcached")) {
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
	if (!strcasecmp(key, "upstream_password")) {
		if (ps->col->up_password) {
			PERR(ps, "duplicate upstream_password\n");
			return -1;
		}
		if (!*val) {
			PERR(ps, "empty upstream_password\n");
			return -1;
		}
		ps->col->up_password = cfg_dup(ps, val);
		return ps->col->up_password ? 0 : -1;
	}
	if (!strcasecmp(key, "upstream_write_user") ||
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
		*dst = cfg_dup(ps, val);
		return *dst ? 0 : -1;
	}
	if (!strcasecmp(key, "upstream_prefixes")) {
		free(ps->col->up_prefixes);
		ps->col->up_prefixes = cfg_dup(ps, val);
		return ps->col->up_prefixes ? 0 : -1;
	}
	if (!strcasecmp(key, "upstream_negative_ms"))
		return parse_int(ps, val, 0, 3600000,
			&ps->col->up_negative_ms);
	if (!strcasecmp(key, "upstream_rewarm_max"))
		return parse_int(ps, val, 0, 10000000,
			&ps->col->up_rewarm_max);
	if (!strcasecmp(key, "upstream_echo_ms"))
		return parse_int(ps, val, 0, 600000,
			&ps->col->up_echo_ms);
	if (!strcasecmp(key, "upstream_sentinel_master") ||
	    !strcasecmp(key, "upstream_sentinel_password")) {
		char **dst = !strcasecmp(key, "upstream_sentinel_master") ?
			&ps->col->up_smaster : &ps->col->up_spassword;

		if (*dst || !*val) {
			PERR(ps, "%s: %s\n", key, *dst ? "given twice" : "empty");
			return -1;
		}
		*dst = cfg_dup(ps, val);
		return *dst ? 0 : -1;
	}
	if (!strcasecmp(key, "upstream_discover")) {
		if (!strcasecmp(val, "auto"))
			ps->col->up_discover = PC_UP_DISC_AUTO;
		else if (!strcasecmp(val, "yes"))
			ps->col->up_discover = PC_UP_DISC_YES;
		else if (!strcasecmp(val, "no"))
			ps->col->up_discover = PC_UP_DISC_NO;
		else {
			PERR(ps, "upstream_discover: auto, yes or no\n");
			return -1;
		}
		return 0;
	}
	if (!strcasecmp(key, "upstream_timeout_ms"))
		return parse_int(ps, val, 10, 60000,
			&ps->col->up_timeout_ms);
	if (!strcasecmp(key, "upstream_conns"))
		return parse_int(ps, val, 1, 16, &ps->col->up_conns);
	if (!strcasecmp(key, "upstream_drain_rate"))
		return parse_int(ps, val, 0, 10000000,
			&ps->col->up_drain_rate);
	/* the sweeps' bytes a second, beside their keys */
	if (!strcasecmp(key, "upstream_drain_mb_rate"))
		return parse_int(ps, val, 0, 100000,
			&ps->col->up_drain_mb_rate);
	/* the largest reply relayed whole - under the 8 MB a
	 * client connection may have waiting */
	if (!strcasecmp(key, "upstream_pass_max_kb"))
		return parse_int(ps, val, 64, 4096,
			&ps->col->up_pass_max_kb);
	/* writes waiting for the master, at most; past it a write
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
	if (!strcasecmp(key, "upstream_writes")) {
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
	if (!strcasecmp(key, "upstream_unknown")) {
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
		/* K lives in [cluster] only, so a per-collection spread has no copy factor -
		 * refused at validation, which knows whether a cluster exists */
		ps->col->mode = (enum pc_pull_mode)m;
		ps->col->has_mode = 1;
		return 0;
	}
	return CFG_KV_UNKNOWN;
}
