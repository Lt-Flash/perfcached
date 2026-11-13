/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* keeps.c - what a collection keeps.  See keeps.h. */
#include <stdio.h>

#include "compat/dprint.h"
#include "keeps.h"
#include "config.h"
#include "store.h"
#include "wal.h"
#include "cluster.h"

const char *pc_keeps_name(int col)
{
	if (!pc_wal_enabled() || !pc_store_durable_idx(col))
		return "ephemeral";
	if (pc_cluster_enabled() && pc_cluster_acks_gate(NULL, 0, NULL))
		return "fleet-delete-safe";
	return "recoverable";
}

const char *pc_keeps_copies(int col, char *buf, size_t n)
{
	pcache_htable_t *ht = pc_store_ht(col);
	int k = pc_cluster_replicas();

	if (!pc_cluster_enabled() || !ht || pc_store_shard_enabled(ht) ||
	    pc_store_proxy_enabled(ht))
		return "1";
	if (k) {
		snprintf(buf, n, "%d", k);
		return buf;
	}
	return pc_store_eager_enabled(ht) ? "all" : "pulled";
}

void pc_keeps_log(const struct pc_config *cfg)
{
	static const char *const fs[] = { "everysec", "always", "no" };
	char kb[48];
	int i;

	for (i = 0; i < cfg->n_col; i++) {
		const struct pc_collection *c = &cfg->col[i];
		const char *copies, *why;

		if (!cfg->cl_enabled || c->mode == PC_MODE_SHARD ||
		    c->mode == PC_MODE_PROXY)
			copies = "one";
		else if (cfg->cl_replicas) {
			snprintf(kb, sizeof kb, "%d of the fleet's (spread)",
				cfg->cl_replicas);
			copies = kb;
		} else
			copies = c->eager ? "every node's (eager)"
				: "the writer's and each reader's (store)";
		if (!cfg->wal_dir || c->nondurable) {
			LM_NOTICE("collection '%s' keeps: ephemeral (%s) - a restart "
				"starts it empty here; copies: %s\n", c->name,
				cfg->wal_dir ? "durable = no" : "no [wal]", copies);
			continue;
		}
		why = cfg->cl_enabled && cfg->cl_tomb_acks
			? "fleet-delete-safe once every member acknowledges deletes"
			: "recoverable";
		LM_NOTICE("collection '%s' keeps: %s - a restart replays its WAL "
			"(fsync %s); copies: %s\n", c->name, why,
			fs[cfg->wal_fsync >= 0 && cfg->wal_fsync <= 2 ?
			   cfg->wal_fsync : 0], copies);
	}
}
