/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * cfgcluster.c - the config sections of the cluster plane: [cluster] and
 * [collection X].  Moved from config.c's handle_kv, each case body
 * unchanged but for its indent and its last break (the 0.7.2 split, wave
 * 1 - doc/MODULARITY.md).
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
#include "configint.h"

/* [cluster] */
int cfg_kv_cluster(struct pstate *ps, char *key, char *val)
{
	struct pc_config *cfg = ps->cfg;

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
		char *grp;

		if (split_hostport(ps, val, "multicast must be group:port",
		        &grp, &cfg->cl_port))
			return -1;
		free(cfg->cl_mcast);
		cfg->cl_mcast = grp;
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
	return CFG_KV_UNKNOWN;
}

/* [collection X] */
int cfg_kv_collection(struct pstate *ps, char *key, char *val)
{
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
	if (!strcasecmp(key, "durable")) {                    /* G5 */
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
	if (!strcasecmp(key, "upstream_rewarm_max"))       /* S421 */
		return parse_int(ps, val, 0, 10000000,
			&ps->col->up_rewarm_max);
	if (!strcasecmp(key, "upstream_echo_ms"))          /* S422 */
		return parse_int(ps, val, 0, 600000,
			&ps->col->up_echo_ms);
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
	return CFG_KV_UNKNOWN;
}
