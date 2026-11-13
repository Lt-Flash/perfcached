/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clfleet.h - one collection's figures for the whole fleet, pure arithmetic (test/clfleettest.c):
 * held records follow the mode's copy rule (@basis); client events happen once and sum. */
#ifndef PC_CLFLEET_H
#define PC_CLFLEET_H

#include <stdint.h>

#include "clmemb.h"                    /* struct clmemb_col */

#define PC_FLEET_SUM      0   /* shard, proxy: one copy of a record */
#define PC_FLEET_FULLEST  1   /* eager: every member holds every record */
#define PC_FLEET_AT_LEAST 2   /* store: a pull keeps a copy - a lower bound */
#define PC_FLEET_PER_K    3   /* spread: K copies of a record */
struct pc_col_fleet {
	int basis, members, reporting;
	uint64_t entries, expired, copies, hits, misses, stores, removes;
	/* the same counters over the last five minutes, combined by the same rules; w_ok 0 while
	 * any member has no baseline yet (it just joined, or this node just started) */
	int w_ok;
	uint64_t w_expired, w_hits, w_misses, w_stores, w_removes;
};

struct clfleet {
	struct pc_col_fleet f;
	uint64_t emax, xmax, xsum;
};

/* @own is this node's block: a member, and reporting */
void clfleet_begin(struct clfleet *a, int basis,
		const struct clmemb_col *own);
/* a live peer: @c its block for the collection, or NULL (a member, not reporting) */
void clfleet_member(struct clfleet *a, const struct clmemb_col *c);
/* @k is spread's K (pc_cluster_replicas()), ignored by the other bases */
void clfleet_end(struct clfleet *a, int k, struct pc_col_fleet *out);

/* the block for @hash among a member's first @ncols (bounded by
 * CLMEMB_COLS_MAX), or NULL */
const struct clmemb_col *clfleet_find(const struct clmemb_col *cols,
		int ncols, uint64_t hash);

const char *pc_fleet_basis_name(int basis);

#endif /* PC_CLFLEET_H */
