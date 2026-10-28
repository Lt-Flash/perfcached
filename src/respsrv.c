/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * respsrv.c - the RESP door's connection and server commands: PING (PS2's
 * array form in subscribed mode), AUTH (S33), HELLO (RESP2 only), INFO
 * (the sections the Grafana Redis datasource and rtpengine read) and TIME
 * (S48).  Moved from verbs.c unchanged (the 0.7.2 split, wave 1 -
 * doc/MODULARITY.md).
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "json.h"
#include "store.h"
#include "core/pcache_htable.h"
#include "core/pcache_arena.h"          /* pcache_arena_held_bytes */
#include "obs.h"                        /* the INFO figures */
#include "pubsub.h"                     /* pc_pubsub_count */
#include "daemon.h"                     /* the RESP password, PC_RESP_BUMP */
#include "version.h"
#include "verbs.h"
#include "respout.h"
#include "verbsint.h"

int rh_ping(struct resp_ctx *c)
{
	if (pc_pubsub_count(c->conn) > 0) {
		/* PS2: in subscribed mode a PING answers as an array, the
		 * shape predis's pubSubLoop reads */
		resp_arr(c->out, 2);
		resp_bulk(c->out, "pong", 4);
		if (c->nargs > 1)
			resp_bulk(c->out, c->argv[1], c->argl[1]);
		else
			resp_bulk(c->out, "", 0);
		return 0;
	}
	if (c->nargs > 1)
		resp_bulk(c->out, c->argv[1], c->argl[1]);
	else
		resp_simple(c->out, "PONG");
	return 0;
}

int rh_auth(struct resp_ctx *c)
{
	/* On the native plaintext listener the handshake IS the
	 * authentication (5.1) and there is no password plane - same
	 * answer a requirepass-less Redis gives.  On a RESP listener
	 * (S33) there is no handshake, so an optional password is the
	 * strongest in-band control the dialect allows.  Accept both
	 * RESP2 forms: AUTH <pw> and the ACL-style AUTH <user> <pw>
	 * (any username - we have one principal). */
	const char *pw;
	size_t pwn;

	if (!pc_resp_password_set()) {
		resp_err(c->out, "Client sent AUTH, but no password is set");
		return 0;
	}
	if (c->nargs == 2) {
		pw = c->argv[1];
		pwn = c->argl[1];
	} else if (c->nargs == 3) {
		pw = c->argv[2];
		pwn = c->argl[2];
	} else {
		resp_err(c->out, "wrong number of arguments for 'auth' "
			"command");
		return 0;
	}
	if (pc_resp_password_ok(pw, pwn)) {
		*c->authed = 1;
		resp_simple(c->out, "OK");
	} else {
		PC_RESP_BUMP(pc_resp_authfail);
		pc_jw_lit(c->out, "-WRONGPASS invalid username-password "
			"pair or user is disabled.\r\n");
	}
	return 0;
}

int rh_hello(struct resp_ctx *c)
{
	long long ver = 2;

	if (c->nargs > 1 && resp_ll(c->argv[1], c->argl[1], &ver) < 0) {
		resp_err(c->out, "Protocol version is not an integer or "
			"out of range");
		return 0;
	}
	if (ver != 2) {
		pc_jw_lit(c->out, "-NOPROTO unsupported protocol version\r\n");
		return 0;
	}
	resp_arr(c->out, 14);
	resp_bulk_lit(c->out, "server");
	resp_bulk_lit(c->out, "perfcached");
	resp_bulk_lit(c->out, "version");
	resp_bulk(c->out, PC_VERSION, strlen(PC_VERSION));
	resp_bulk_lit(c->out, "proto");
	resp_int(c->out, 2);
	resp_bulk_lit(c->out, "id");
	resp_int(c->out, 0);
	resp_bulk_lit(c->out, "mode");
	resp_bulk_lit(c->out, "standalone");
	resp_bulk_lit(c->out, "role");
	resp_bulk_lit(c->out, "master");
	resp_bulk_lit(c->out, "modules");
	resp_arr(c->out, 0);
	return 0;
}

int rh_info(struct resp_ctx *c)
{
	struct pc_jw b;
	const char *sec = c->nargs >= 2 ? c->argv[1] : NULL;
	size_t secl = c->nargs >= 2 ? c->argl[1] : 0;
	int all;

	/* Redis semantics, which the Grafana Redis datasource RELIES
	 * on: INFO <section> returns ONLY that section (the plugin's
	 * commandstats panel got every section appended and rendered
	 * nothing); bare INFO returns the default set, which
	 * excludes commandstats; "all"/"everything" include it. */
	if (sec && (resp_is(sec, secl, "all") ||
	        resp_is(sec, secl, "everything") ||
	        resp_is(sec, secl, "default")))
		sec = NULL, all = 1;
	else
		all = 0;
#define INFO_WANT(_n) (!sec || resp_is(sec, secl, _n))
	pc_jw_init(&b, c->scratch, c->scratch_cap);
	if (INFO_WANT("server")) {
		/* S74: redis_mode is where a cluster-aware client
		 * decides whether to fetch the slot map at all */
		pc_jw_lit(&b, "# Server\r\nredis_version:7.0.0\r\n"
			"redis_mode:");
		pc_jw_lit(&b, resp_cluster_on() ? "cluster" : "standalone");
		pc_jw_lit(&b, "\r\nperfcached_version:" PC_VERSION "\r\n"
			"perfcached_dialect:resp2-compat\r\n");
	}
	/* The Replication section is NOT decoration: rtpengine asks
	 * INFO at startup purely to learn whether the server is a
	 * master, and REFUSES TO START without a role line - found by
	 * pointing a real rtpengine at us.  A perfcached node is
	 * always writable, so the honest answer is master with no
	 * replicas: redundancy lives in the collection modes. */
	if (INFO_WANT("replication"))
		pc_jw_lit(&b, "# Replication\r\nrole:master\r\n"
			"connected_slaves:0\r\n"
			"master_failover_state:no-failover\r\n"
			"master_repl_offset:0\r\n");
	/* S74: the other field clients decide from.  A section that
	 * is absent or empty reads as "no cluster" to anything that
	 * parses it - which it was. */
	if (INFO_WANT("cluster")) {
		pc_jw_lit(&b, "# Cluster\r\ncluster_enabled:");
		pc_jw_i64(&b, resp_cluster_on());
		pc_jw_lit(&b, "\r\n");
	}
	/* S53: the fields the Grafana summary panels read.
	 * used_memory is the arena's HELD bytes - the figure sized
	 * against arena_mb, not a malloc guess. */
	if (INFO_WANT("clients")) {
		pc_jw_lit(&b, "# Clients\r\nconnected_clients:");
		pc_jw_i64(&b, pc_obs_conn_count());
		pc_jw_lit(&b, "\r\n");
	}
	if (INFO_WANT("memory")) {
		pc_jw_lit(&b, "# Memory\r\nused_memory:");
		pc_jw_i64(&b, (long long)pcache_arena_held_bytes());
		pc_jw_lit(&b, "\r\nused_memory_human:");
		pc_jw_i64(&b, (long long)(pcache_arena_held_bytes()
			>> 20));
		pc_jw_lit(&b, "M\r\n");
	}
	if (INFO_WANT("stats")) {
		unsigned long long kh = 0, km = 0;
		int ci;

		for (ci = 0; ci < pc_store_count(); ci++) {
			if (!pc_store_live(ci))
				continue;   /* S69: a dropped collection */
			pcache_ht_totals_t ct;

			pcache_ht_totals(pc_store_ht(ci), &ct);
			kh += ct.hits;
			km += ct.misses;
		}
		pc_jw_lit(&b, "# Stats\r\n"
			"total_commands_processed:");
		pc_jw_i64(&b, (long long)pc_obs_total_calls());
		pc_jw_lit(&b, "\r\ninstantaneous_ops_per_sec:");
		pc_jw_i64(&b, (long long)pc_obs_inst_ops());
		pc_jw_lit(&b, "\r\nkeyspace_hits:");
		pc_jw_i64(&b, (long long)kh);
		pc_jw_lit(&b, "\r\nkeyspace_misses:");
		pc_jw_i64(&b, (long long)km);
		pc_jw_lit(&b, "\r\n");
	}
	if (all || (sec && resp_is(sec, secl, "commandstats"))) {
		pc_jw_lit(&b, "# Commandstats\r\n");
		pc_obs_cmdstats(&b);
	}
	if (all || (sec && resp_is(sec, secl, "latencystats"))) {
		pc_jw_lit(&b, "# Latencystats\r\n");   /* S159 */
		pc_obs_latencystats(&b);
	}
	if (INFO_WANT("keyspace")) {
		int ci;

		/* S69: EVERY collection this door can reach, each under
		 * the index that reaches it.  Reporting only the
		 * selected one made a node holding live data read as
		 * one empty database to every Redis-native monitor - a
		 * flat zero line through an outage and a normal day
		 * alike. */
		pc_jw_lit(&b, "# Keyspace\r\n");
		for (ci = 0; ci < pc_store_count(); ci++) {
			pcache_ht_totals_t tot;
			const char *nm;
			char idx[64];
			size_t il;

			if (!pc_store_live(ci))
				continue;
			nm = pc_store_name(ci);
			if (!resp_col_allowed(nm, strlen(nm)))
				continue;
			pcache_ht_totals(pc_store_ht(ci), &tot);
			if (!tot.entries)
				continue;   /* Redis omits empty dbs */
			il = resp_index_for(nm, strlen(nm), idx,
				sizeof idx);
			pc_jw_lit(&b, "db");
			pc_jw_raw(&b, idx, il);
			pc_jw_lit(&b, ":keys=");
			pc_jw_i64(&b, (long long)tot.entries);
			pc_jw_lit(&b, ",expires=0,avg_ttl=0\r\n");
		}
	}
#undef INFO_WANT
	if (b.overflow)
		resp_err(c->out, "info too large");
	else
		resp_bulk(c->out, b.buf, b.len);
	return 0;
}

/* S48: TIME is collection-independent (17.2M calls/day measured on
 * the realtime instance) - answer before the collection resolves */
int rh_time(struct resp_ctx *c)
{
	struct timespec tw;
	char sb[24], ub[24];
	int sn, un;

	clock_gettime(CLOCK_REALTIME, &tw);
	sn = snprintf(sb, sizeof sb, "%lld", (long long)tw.tv_sec);
	un = snprintf(ub, sizeof ub, "%ld", tw.tv_nsec / 1000);
	resp_arr(c->out, 2);
	resp_bulk(c->out, sb, (size_t)sn);
	resp_bulk(c->out, ub, (size_t)un);
	return 0;
}
