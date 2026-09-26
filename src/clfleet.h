/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clfleet.h - one collection's figures for the whole fleet (S164), the
 * arithmetic without the peer table (M15, wave 4).
 *
 * pc_cluster_col_fleet() walks this node and every live peer; what it
 * does with each member's block is here, pure, so the rules are cases in
 * test/clfleettest.c instead of something only three daemons can show.
 *
 * A HELD record counts once per member that holds it, so entries and
 * expired follow the mode's copy rule (@basis): eager holds every record
 * on every member - the fullest member's count; spread holds K - the
 * copies over min(K, reporting members); store keeps a pulled copy where
 * it was read - the fullest member, a lower bound; shard and proxy hold
 * one - the sum.  Client events (hits, misses, stores, removes) happen
 * once, on the node the client reached, and sum.
 *
 * Streaming, so the caller keeps its own loop and its own snapshot of
 * each peer's block: begin with this node's, one member() per live peer
 * (NULL for a live peer that gossips no block for the collection - a
 * member, but not reporting), then end().
 */
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
};

struct clfleet {
	struct pc_col_fleet f;
	uint64_t emax, xmax, xsum;
};

/* @own is this node's block: a member, and reporting */
void clfleet_begin(struct clfleet *a, int basis,
		const struct clmemb_col *own);
/* a live peer: @c its block for the collection, or NULL */
void clfleet_member(struct clfleet *a, const struct clmemb_col *c);
/* @k is spread's K (pc_cluster_replicas()), ignored by the other bases */
void clfleet_end(struct clfleet *a, int k, struct pc_col_fleet *out);

/* the block for @hash among a member's first @ncols (bounded by
 * CLMEMB_COLS_MAX), or NULL */
const struct clmemb_col *clfleet_find(const struct clmemb_col *cols,
		int ncols, uint64_t hash);

const char *pc_fleet_basis_name(int basis);

#endif /* PC_CLFLEET_H */
