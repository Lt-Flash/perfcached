/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* cfgcheck.c - the config's cross-key rules, checked once the whole file is read
 * (cfg_validate); a refusal names the rule, not a line. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include "compat/dprint.h"
#include "clwire.h"
#include "config.h"
#include "configint.h"

/* the secret @s equals, named into @buf, or NULL: a client's, the cluster's,
 * cluster_accept and the enable secrets */
static const char *same_secret(const struct pc_config *cfg, const char *s,
		char *buf, size_t cap)
{
	int i;

	for (i = 0; i < cfg->n_client_secrets; i++)
		if (!strcmp(s, cfg->client_secret[i])) {
			snprintf(buf, cap, "client secret #%d", i + 1);
			return buf;
		}
	if (cfg->cluster_secret && !strcmp(s, cfg->cluster_secret))
		return "the cluster secret";
	if (cfg->cluster_accept_secret && !strcmp(s, cfg->cluster_accept_secret))
		return "the cluster_accept secret";
	for (i = 0; i < cfg->n_enable_secrets; i++)
		if (!strcmp(s, cfg->enable_secret[i])) {
			snprintf(buf, cap, "enable secret #%d", i + 1);
			return buf;
		}
	return NULL;
}

/* is @hp ("host:port") one of the upstreams @list names
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
		h = end - p > 9 && !strncmp(p, "rediss://", 9) ? p + 9 :
			end - p > 8 && !strncmp(p, "redis://", 8) ? p + 8 : p;
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

	/* a warning below its critical, or the critical would be raised
	 * on the way past a warning that can never come first */
	{
		const struct pc_alerts_conf *al = &cfg->alerts;
		const char *bad = al->cpu_warn >= al->cpu_crit ? "cpu"
			: al->io_warn >= al->io_crit ? "io"
			: al->mem_warn >= al->mem_crit ? "memory"
			: al->arena_warn >= al->arena_crit ? "arena"
			: al->link_warn >= al->link_crit ? "link" : NULL;

		if (bad) {
			PERR(ps, "[alerts] %s_warn_pct must be below %s_crit_pct\n",
				bad, bad);
			return -1;
		}
	}

	/* n_col may be 0 when [cluster] declares the set and this node has no local blocks:
	 * the set is expanded below, and a joiner naming no clustered collection is supported */
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
			/* one config serves a single node and, later, the fleet: fleet-shaped keys run as store
			 * here and the warning names what is inert (said by -C and at startup) */
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
	/* the workers serve one unix socket: a second would be bound and never accepted */
	for (i = 0, j = 0; i < cfg->n_listen; i++)
		if (cfg->listen[i].type == PC_LISTEN_UNIX && ++j > 1) {
			PERR(ps, "more than one 'unix =' listener - one is served\n");
			return -1;
		}

	if (!cfg->n_client_secrets) {
		PERR(ps, "no client secret configured\n");
		return -1;
	}
	/* the cluster secret derives the peer plane's PSK, so only a [cluster] node needs it;
	 * present, it must still differ from every client secret */
	if (!cfg->cluster_secret && cfg->cl_enabled) {
		PERR(ps, "no cluster secret configured\n");
		return -1;
	}
	for (i = 0; i < cfg->n_client_secrets; i++) {
		if (!cfg->client_secret[i][0]) {
			PERR(ps, "empty client secret\n");
			return -1;
		}
		/* the two-principal rule: structurally different secrets */
		if (cfg->cluster_secret &&
		        !strcmp(cfg->client_secret[i], cfg->cluster_secret)) {
			PERR(ps, "the cluster secret EQUALS client secret #%d - the "
				"two principals must not share a secret; refusing to "
				"start\n", i + 1);
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
	if (cfg->cluster_accept_secret && (!cfg->cluster_secret ||
	        !cfg->cluster_accept_secret[0] ||
	        !strcmp(cfg->cluster_accept_secret, cfg->cluster_secret))) {
		PERR(ps, "cluster_accept needs a cluster secret, and must differ from it\n");
		return -1;
	}
	/* the two-principal rule holds for every secret that opens the peer plane or grants DDL */
	for (i = 0; i < cfg->n_client_secrets; i++) {
		if (cfg->cluster_accept_secret &&
		        !strcmp(cfg->client_secret[i], cfg->cluster_accept_secret)) {
			PERR(ps, "the cluster_accept secret EQUALS client secret #%d - the "
				"two principals must not share a secret; refusing to "
				"start\n", i + 1);
			return -1;
		}
		for (j = 0; j < cfg->n_enable_secrets; j++)
			if (!strcmp(cfg->client_secret[i], cfg->enable_secret[j])) {
				PERR(ps, "enable secret #%d EQUALS client secret #%d - "
					"every client link could enable; refusing to "
					"start\n", j + 1, i + 1);
				return -1;
			}
	}
	for (i = 0; i < cfg->n_enable_secrets; i++)
		if (strlen(cfg->enable_secret[i]) < 8)
			LM_WARN("enable secret #%d is under 8 characters\n", i + 1);
	if (cfg->allow_create && cfg->n_enable_secrets == 0)
		LM_WARN("allow_create is ON and no [secrets] enable is set - "
			"DDL is refused from the client door entirely "
			"(fail-closed).  Set an enable secret to allow "
			"privileged connections, or turn allow_create off\n");

	/* ---- RESP listeners: no handshake (stock Redis clients cannot speak Noise), so the
	 * network is the authentication and the daemon refuses to start rather than serve the world */
	{
		int n_resp = 0, n_resp_lan = 0;

		for (i = 0; i < cfg->n_listen; i++)
			if (cfg->listen[i].resp) {
				n_resp++;
				if (!cfg->listen[i].loopback)
					n_resp_lan++;
			}
		if (cfg->http_token && !cfg->http_token[0]) {
			PERR(ps, "empty http secret - an empty bearer would match it\n");
			return -1;
		}
		if (cfg->http_token) {
			char nb[32];
			const char *w = same_secret(cfg, cfg->http_token, nb, sizeof nb);

			if (w) {
				PERR(ps, "the http token EQUALS %s - it crosses the wire "
					"in the clear; refusing to start\n", w);
				return -1;
			}
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
		{       /* a TLS door needs its pair and a build with OpenSSL; a secret a
			 * plaintext door carries off loopback is said, not refused (by default) */
			int ntls = 0, plr = 0, plh = 0, offr = 0, offh = 0;

			for (i = 0; i < cfg->n_listen; i++) {
				const struct pc_listener *l = &cfg->listen[i];

				ntls += l->tls;
				plr += !l->tls && !l->loopback && l->resp;
				plh += !l->tls && !l->loopback && l->http;
				offr += !l->loopback && l->resp;
				offh += !l->loopback && l->http;
			}
			/* said, not refused - a refusal would stop a live node at its upgrade */
			if (offr && !cfg->resp_password)
				LM_WARN("a RESP listener off loopback has no password ([secrets] "
					"resp): any client resp_allow admits reads and writes\n");
			if (offh && !cfg->http_token)
				LM_WARN("an HTTP listener off loopback has no token ([secrets] "
					"http): any client http_allow admits reads /stats\n");
#ifndef PC_TLS
			if (ntls) {
				PERR(ps, "a listener says tls, but this build has no TLS "
					"(make PC_TLS=yes, with OpenSSL)\n");
				return -1;
			}
#endif
			if (ntls && (!cfg->tls_cert || !cfg->tls_key)) {
				PERR(ps, "a tls listener needs [listen] tls_cert and tls_key\n");
				return -1;
			}
			if (!ntls && (cfg->tls_cert || cfg->tls_key)) {
				PERR(ps, "tls_cert/tls_key are set but no listener says tls\n");
				return -1;
			}
			if (plr && cfg->resp_password)
				LM_WARN("the RESP AUTH password travels in the clear on a "
					"plaintext listener off loopback - give that listener tls "
					"(resp = <addr:port> tls)\n");
			if (plh && cfg->http_token)
				LM_WARN("the HTTP token travels in the clear on a plaintext "
					"listener off loopback - give that listener tls (http = "
					"<addr:port> tls)\n");
		}
		for (i = 0; i < cfg->n_listen; i++)
			if (cfg->listen[i].resp)
				LM_NOTICE("RESP listener on %s:%d%s - %d allow-list "
					"entr%s, password %s, collections %s\n",
					cfg->listen[i].addr, cfg->listen[i].port,
					cfg->listen[i].tls ? " (tls)" : "",
					cfg->n_resp_allow,
					cfg->n_resp_allow == 1 ? "y" : "ies",
					cfg->resp_password ? "SET" : "none",
					cfg->resp_collections ?
						cfg->resp_collections : "all");
		{
			/* the HTTP door has no handshake and no password: the same exposure as a RESP
			 * listener, so the same rule (a reachable scrape target also discloses the fleet's shape) */
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
						"(/stats, /members, /clients, "
						"/metrics, /health)\n",
						cfg->listen[i].addr,
						cfg->listen[i].port,
						cfg->n_http_allow,
						cfg->n_http_allow == 1 ?
							"y" : "ies",
						cfg->http_token ? "SET" : "none");
		}

		if (cfg->resp_password) {
			if (!cfg->resp_password[0]) {
				PERR(ps, "empty resp secret\n");
				return -1;
			}
			char nb[32];
			const char *w = same_secret(cfg, cfg->resp_password, nb, sizeof nb);

			if (w) {
				PERR(ps, "the resp password EQUALS %s - it crosses "
					"the wire in the clear; refusing to start\n", w);
				return -1;
			}
			if (strlen(cfg->resp_password) < 8)
				LM_WARN("the resp password is under 8 characters "
					"and crosses the wire in the clear\n");
		}
	}

	/* read-through: an upstream with nothing allowed to fall through is a mistake,
	 * refused like an empty secret - the allow-list is never implicit */
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
		/* the master is the fallback FROM replicas - asked for
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
		/* the failover keys say nothing to balance */
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
		if (!cc->upstream && cc->up_allow_pc) {
			PERR(ps, "[collection %s] upstream_allow_perfcached without "
				"an upstream\n", cc->name);
			return -1;
		}
		if (!cc->upstream && cc->up_password) {
			PERR(ps, "[collection %s] upstream_password without an "
				"upstream\n", cc->name);
			return -1;
		}
		if (!cc->up_tls && (cc->up_tls_ca || cc->up_tls_name || cc->up_tls_cert ||
		    cc->up_tls_key || cc->up_tls_verify_set)) {
			PERR(ps, "[collection %s] upstream_tls_* need a rediss:// upstream\n", cc->name);
			return -1;
		}
		if (cc->up_tls) {
#ifndef PC_TLS
			PERR(ps, "[collection %s] rediss:// needs a build with TLS (PC_TLS)\n", cc->name);
			return -1;
#endif
			if (!cc->up_tls_cert != !cc->up_tls_key) {
				PERR(ps, "[collection %s] upstream_tls_cert and upstream_tls_key go "
					"together\n", cc->name);
				return -1;
			}
			if (cc->up_tls_ca && !cc->up_tls_verify) {
				PERR(ps, "[collection %s] upstream_tls_ca has no use with "
					"upstream_tls_verify = no\n", cc->name);
				return -1;
			}
			if (!cc->up_tls_verify)
				LM_WARN("[collection %s] upstream_tls_verify = no - the upstream's "
					"certificate is not checked, so anyone on the path can pose as "
					"it; a self-signed one can be named in upstream_tls_ca instead\n",
					cc->name);
		}
	}
	/* each read-through collection gets a companion, <name>.nofall: the keys this fleet wrote or
	 * deleted, never to fall through again; '.' is outside the name rule, so it cannot collide */
	{
		int n0 = cfg->n_col;

		for (i = 0; i < n0; i++) {
			struct pc_collection *pc = &cfg->col[i];
			char nm[64];               /* a name is 1-32 here, plus ".nofall" */
			struct pc_collection *nf;
			int in_cl = 0;

			/* only a read-through collection can refuse writes */
			if ((pc->up_refuse_writes || pc->up_write_through) &&
			    !pc->upstream) {
				PERR(ps, "[collection %s] upstream_writes needs "
					"upstream = - it says what a read-through "
					"collection does with a write\n", pc->name);
				return -1;
			}
			/* unknown commands go to Redis only where Redis has every write (through);
			 * elsewhere passing would split the truth */
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
			if ((pc->up_wuser || pc->up_wpassword) &&
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
			/* a collection that refuses writes never owns a key,
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
			nf->name = cfg_dup(ps, nm);
			if (!nf->name)
				return -1;
			nf->buckets_log2 = pc->buckets_log2;
			/* and the parent's autoscale: under the default an empty companion shrinks, then grows back
			 * while a sweep writes one entry per key - the pair is sized together */
			nf->autoscale = pc->autoscale;
			nf->has_autoscale = pc->has_autoscale;
			nf->mode = pc->mode;
			nf->has_mode = pc->has_mode;
			nf->eager = pc->eager;
			/* a cache's marks are a cache's too */
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
				if (!(cfg->cl_colname[cfg->n_cl_col] = cfg_dup(ps, nm)))
					return -1;
				cfg->n_cl_col++;
			}
		}
	}

	if (sizeof(void *) == 4 && cfg->arena_mb > 1536) {
		PERR(ps, "arena_mb %d exceeds the 32-bit cap (1536)\n",
			cfg->arena_mb);
		return -1;
	}
	/* under `backing = heap` the core allocator owns the pages and there is nothing to lock:
	 * refused at parse time rather than a start that looks pinned */
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
	/* a floor above the ceiling can never be satisfied - the node
	 * would refuse to release memory it is also refusing to hold */
	if (cfg->reclaim_floor_mb) {
		int cap = cfg->arena_cap_mb ? cfg->arena_cap_mb : cfg->arena_mb;

		if (cfg->reclaim_floor_mb > cap) {
			PERR(ps, "reclaim_floor_mb %d above the arena ceiling "
				"%d MB\n", cfg->reclaim_floor_mb, cap);
			return -1;
		}
	}

	if (cfg->cl_enabled && cfg->cl_unicast) {
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
		if (cfg->cl_max_dgram && !cfg->cl_lane_port) {
			PERR(ps, "[cluster] the cluster port is %d, so lane_port "
				"must be set\n", cfg->cl_port);
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
	/* said, not refused: a node running such a file today must still start */
	if (ps->wal_key[0] && !cfg->wal_dir)
		LM_WARN("[wal] %s is set but [wal] has no dir - there is no WAL and "
			"no snapshot, and nothing written survives a restart\n",
			ps->wal_key);
	if (cfg->wal_segments_max &&
	    cfg->wal_segments_max < cfg->wal_segments) {
		PERR(ps, "[wal] segments_max %d is below segments (the floor) "
			"%d\n", cfg->wal_segments_max, cfg->wal_segments);
		return -1;
	}
	if (cfg->cl_ps_rx_threads < 0) {           /* one per sending lane at most */
		cfg->cl_ps_rx_threads = (cfg->workers + 1) / 2;
		if (cfg->cl_ps_rx_threads > CLWIRE_LANES)
			cfg->cl_ps_rx_threads = CLWIRE_LANES;
		cfg->cl_ps_rx_auto = 1;
	}
	if (cfg->cl_enabled) {
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
	/* ---- `collections` in [cluster] makes the cluster authoritative: one mode, an exhaustive
	 * set, a local `mode =` only a hint that must match; the legacy form parses, with a warning */
	if (cfg->n_cl_col) {
		if (!cfg->cl_has_mode) {
			PERR(ps, "[cluster] collections = ... needs mode = "
				"store|proxy|shard|spread: one cluster, one mode\n");
			return -1;
		}
		/* mode_of() cannot produce this pair; kept as an invariant
		 * so a future edit cannot reintroduce it quietly */
		if (cfg->cl_eager && cfg->cl_mode != PC_MODE_STORE) {
			PERR(ps, "[cluster] internal: eager outside store\n");
			return -1;
		}

		/* an eager replica WALs only what it authored, so a whole-fleet restart loses the copies:
		 * only a snapshot (the RDB walk keeps passive records) makes that recoverable */
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

		/* ---- the copy factor K decides which nodes hold a key: interchange state, in the digest,
		 * and a member with a different K is refused at the join */
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
			/* eager and store are both PC_MODE_STORE: compare the eager half too,
			 * or `mode = eager` under a `mode = store` cluster passes silently */
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
			cfg->col[cfg->n_col].name = cfg_dup(ps, cfg->cl_colname[j]);
			if (!cfg->col[cfg->n_col].name)
				return -1;
			cfg->col[cfg->n_col].buckets_log2 = PC_DEFAULT_BUCKETS_LOG2;
			cfg->n_col++;
			LM_NOTICE("collection '%s' comes from the cluster and has "
				"no local block - created with default sizing "
				"(buckets_log2 = %d)\n", cfg->cl_colname[j],
				PC_DEFAULT_BUCKETS_LOG2);
		}
		/* stamp the cluster's mode onto every collection: from here the runtime has one
		 * authority, and the local file is untouched */
		for (i = 0; i < cfg->n_col; i++) {
			cfg->col[i].mode = cfg->cl_mode;
			cfg->col[i].has_mode = 1;
			cfg->col[i].eager = cfg->cl_eager;
			if (cfg->cl_mode == PC_MODE_STORE)
				cfg->col[i].pull = 1;
			/* spread is eager with a bounded target (the K holders), so it runs the eager path and
			 * pulls on a miss; without the eager flag it would replicate nothing */
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

	/* a [mail] section that can actually send */
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
