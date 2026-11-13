/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clplace.h - weighted rendezvous over the map: score = w / -ln(u), compared as w_a * D_b > w_b * D_a
 * in integers - every node and client must reach the SAME answer, which float log cannot promise */
#ifndef PC_CLPLACE_H
#define PC_CLPLACE_H

#include <stddef.h>
#include <stdint.h>

#include "clmap.h"

/* The key hash and the node mix, which MUST stay bit-identical to the
 * ones the cluster already uses - changing either moves every key. */
uint64_t pc_clplace_mix(uint64_t kh, uint32_t addr, uint16_t port);

/* the placeable node with the highest score, or 0 if none; ties break on node id.  With equal
 * weights this is exactly the unweighted argmax, so uniform weights move nothing. */
uint16_t pc_clplace_owner(const struct pc_clmap *m, uint64_t kh);

/* the top @max placeable nodes by score, best first, into @out: how many were written - fewer
 * means the map holds fewer placeable nodes, and the caller decides if a short list will do */
int pc_clplace_rank(const struct pc_clmap *m, uint64_t kh,
		uint16_t *out, int max);

/* is @node_id one of the top @k placeable nodes?  Counts the nodes that BEAT it - no sort, exactly
 * pc_clplace_rank()'s first @k.  Any member may accept a write; rank 0 only owns repair. */
int pc_clplace_in_set(const struct pc_clmap *m, uint64_t kh,
		uint16_t node_id, int k);

/* does @m mention @node_id at all?  in_set() answers 0 both for "not in the top K" and "unknown to
 * this map" (a rejoined node's id) - ask this first if a 0 is going to remove data */
int pc_clplace_has(const struct pc_clmap *m, uint16_t node_id);

/* Exposed for the test: the fixed-point (64 - log2(h)) term. */
#define PC_CLPLACE_FBITS 20
uint64_t pc_clplace_dterm(uint64_t h);

#endif /* PC_CLPLACE_H */
