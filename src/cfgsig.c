/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* cfgsig.c - the settings the fleet's members must agree on, built from the config at start and
 * handed to the cluster for its keepalive */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "compat/dprint.h"
#include "config.h"
#include "cluster.h"
#include "fnv1a.h"
#include "cfgsig.h"

/* only settings whose difference changes what the FLEET does - where writes go, what a crash loses,
 * who reads the page, what alerts, how long deletes are remembered - not per-node ones */
static int cfgsig_add(struct pc_cfgsig *e, int n, const char *name,
		const char *what, const char *val)
{
	if (n >= PC_CFGSIG_MAX - PC_CL_NFACTS) {        /* room for the facts */
		static int said;

		if (!said++)
			LM_WARN("cluster: more settings than the keepalive carries "
				"(%d) - '%s' and any after it are not compared "
				"across the fleet\n", PC_CFGSIG_MAX - PC_CL_NFACTS,
				name);
		return n;
	}
	snprintf(e[n].name, sizeof e[n].name, "%s", name);
	snprintf(e[n].what, sizeof e[n].what, "%s", what);
	snprintf(e[n].val, sizeof e[n].val, "%s", val);
	/* bit 31 clear: a set bit names a FACT, never compared */
	e[n].name_h = (uint32_t)fnv1a64(e[n].name, strlen(e[n].name)) &
		~PC_FACT_BIT;
	e[n].val_h = (uint32_t)fnv1a64(val, strlen(val));    /* all of it */
	return n + 1;
}

void cfgsig_build(const struct pc_config *cfg)
{
	static const char *const fs[] = { "everysec", "always", "no" };
	struct pc_cfgsig e[PC_CFGSIG_MAX];
	char nm[48], wt[64], v[256];
	int n = 0, i;

	n = cfgsig_add(e, n, "wal.fsync", "[wal] fsync", !cfg->wal_dir ? "off"
		: cfg->wal_fsync >= 0 && cfg->wal_fsync <= 2 ? fs[cfg->wal_fsync]
		: "?");
	n = cfgsig_add(e, n, "http.token", "the HTTP token ([secrets] http)",
		cfg->http_token ? "set" : "none");
	snprintf(v, sizeof v, "cpu %d/%d io %d/%d memory %d/%d arena %d/%d "
		"epoll %d", cfg->alerts.cpu_warn, cfg->alerts.cpu_crit,
		cfg->alerts.io_warn, cfg->alerts.io_crit, cfg->alerts.mem_warn,
		cfg->alerts.mem_crit, cfg->alerts.arena_warn,
		cfg->alerts.arena_crit, cfg->alerts.epoll_busy);
	n = cfgsig_add(e, n, "alerts", "the [alerts] thresholds", v);
	snprintf(v, sizeof v, "%d s, %d at most", cfg->cl_tomb_retain_s,
		cfg->cl_tomb_retain_max);
	n = cfgsig_add(e, n, "tombstones", "tombstone_retain_s / _max", v);
	snprintf(v, sizeof v, "%d", cfg->cl_max_dgram);
	n = cfgsig_add(e, n, "max_datagram", "[cluster] max_datagram", v);
	for (i = 0; i < cfg->n_col; i++) {
		const struct pc_collection *c = &cfg->col[i];
		size_t k, m = 0;

		if (!c->upstream)
			continue;
		snprintf(nm, sizeof nm, "%.32s.writes", c->name);
		snprintf(wt, sizeof wt, "[collection %.32s] upstream_writes", c->name);
		n = cfgsig_add(e, n, nm, wt, c->up_refuse_writes ? "refuse" :
			c->up_write_through ? "through" : "allow");
		/* the patterns as written, spaces out */
		for (k = 0; c->up_prefixes && c->up_prefixes[k] &&
		     m < sizeof v - 1; k++)
			if (c->up_prefixes[k] != ' ' && c->up_prefixes[k] != '\t')
				v[m++] = c->up_prefixes[k];
		v[m] = 0;
		snprintf(nm, sizeof nm, "%.32s.prefixes", c->name);
		snprintf(wt, sizeof wt, "[collection %.32s] upstream_prefixes", c->name);
		n = cfgsig_add(e, n, nm, wt, v);
		snprintf(nm, sizeof nm, "%.32s.role", c->name);
		snprintf(wt, sizeof wt, "[collection %.32s] upstream_role", c->name);
		n = cfgsig_add(e, n, nm, wt, c->up_role == PC_UP_ROLE_REPLICA ?
			"replica" : "any");
		snprintf(nm, sizeof nm, "%.32s.unknown", c->name);
		snprintf(wt, sizeof wt, "[collection %.32s] upstream_unknown", c->name);
		n = cfgsig_add(e, n, nm, wt, !c->up_write_through ? "-" :
			c->up_unknown == PC_UP_UNKNOWN_PASS ? "pass" : "refuse");
	}
	/* which collections are caches: a member logging one the others do not comes back from a restart
	 * with what they start without; last, so it is what gets left out (an older build sends none) */
	{
		size_t m = 0;

		v[0] = 0;
		for (i = 0; i < cfg->n_col && m < sizeof v - 1; i++)
			if (cfg->col[i].nondurable) {
				int w = snprintf(v + m, sizeof v - m, "%s%s",
					m ? "," : "", cfg->col[i].name);

				if (w < 0)
					break;
				m += (size_t)w < sizeof v - m ? (size_t)w : sizeof v - 1 - m;
			}
		n = cfgsig_add(e, n, "caches", "[collection] durable = no",
			m ? v : "none");
	}
	pc_cluster_set_cfgsig(e, n);
}
