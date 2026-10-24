/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * cfgcheck.c - the config's rules, checked once the file is read: the
 * alert thresholds, the collections, the secrets and the two-principal
 * rule, the plaintext doors' allow-lists, the read-through keys and each
 * read-through collection's no-fall companion, the memory bounds, the
 * cluster's discovery and its collection set, [mail].  Moved from
 * config.c's validate unchanged (the 0.7.2 split, wave 1 -
 * doc/MODULARITY.md).
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include "compat/dprint.h"
#include "config.h"
#include "configint.h"

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

int cfg_validate(struct pstate *ps, const char *path,
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
			/* G5: a cache's marks are a cache's too */
			nf->nondurable = pc->nondurable;
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
