/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clsel.c — client-side node selection over the map.
 */
#include <stddef.h>

#include "clsel.h"

int pc_clsel_eligible(const struct pc_clmap_node *n)
{
	/* state only - see the header on why weight does not gate this */
	return n && n->state == PC_CLMAP_ST_READY;
}

int pc_clsel_count(const struct pc_clmap *m)
{
	unsigned int i;
	int n = 0;

	if (!m)
		return 0;
	for (i = 0; i < m->nnodes; i++)
		if (pc_clsel_eligible(&m->node[i]))
			n++;
	return n;
}

/* splitmix64: a client's seed spread over the whole range, so two
 * clients started in the same millisecond do not begin adjacent */
static uint64_t mix64(uint64_t x)
{
	x += 0x9E3779B97F4A7C15ull;
	x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
	x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
	return x ^ (x >> 31);
}

uint16_t pc_clsel_pick(const struct pc_clmap *m, int policy, uint64_t seed,
		uint16_t avoid)
{
	unsigned int i;
	int n = 0, want;
	uint16_t first = 0;
	const struct pc_clmap_node *best = NULL;
	uint64_t total = 0, r;

	if (!m)
		return 0;

	/* count what is usable, having removed the node to avoid */
	for (i = 0; i < m->nnodes; i++) {
		const struct pc_clmap_node *nd = &m->node[i];

		if (!pc_clsel_eligible(nd) || nd->node_id == avoid)
			continue;
		n++;
		if (!first)
			first = nd->node_id;
	}
	if (!n) {
		/* everything eligible was avoided: better the node we were
		 * told to skip than nothing at all, since the alternative is
		 * no service whatsoever */
		if (avoid)
			return pc_clsel_pick(m, policy, seed, 0);
		return 0;
	}

	switch (policy) {
	case PC_CLSEL_ROUND_ROBIN:
		/* an independent start per client.  A fixed start here would
		 * put every client in the fleet on one node - which is the
		 * failure the fleet-level spread check exists to catch. */
		want = (int)(mix64(seed) % (uint64_t)n);
		for (i = 0; i < m->nnodes; i++) {
			const struct pc_clmap_node *nd = &m->node[i];

			if (!pc_clsel_eligible(nd) || nd->node_id == avoid)
				continue;
			if (want-- == 0)
				return nd->node_id;
		}
		return first;

	case PC_CLSEL_LEAST_CONN:
		for (i = 0; i < m->nnodes; i++) {
			const struct pc_clmap_node *nd = &m->node[i];

			if (!pc_clsel_eligible(nd) || nd->node_id == avoid)
				continue;
			/* cap_weight carries the master's view of capacity;
			 * ties break on id so two clients with the same view
			 * agree */
			if (!best || nd->cap_weight > best->cap_weight ||
			        (nd->cap_weight == best->cap_weight &&
			         nd->node_id < best->node_id))
				best = nd;
		}
		return best ? best->node_id : first;

	case PC_CLSEL_WEIGHTED:
		for (i = 0; i < m->nnodes; i++) {
			const struct pc_clmap_node *nd = &m->node[i];

			if (!pc_clsel_eligible(nd) || nd->node_id == avoid)
				continue;
			total += pc_clmap_weight(nd);
		}
		if (!total)
			return first;      /* all drained but serving: spread
			                    * evenly rather than not at all */
		r = mix64(seed) % total;
		for (i = 0; i < m->nnodes; i++) {
			const struct pc_clmap_node *nd = &m->node[i];
			uint32_t w;

			if (!pc_clsel_eligible(nd) || nd->node_id == avoid)
				continue;
			w = pc_clmap_weight(nd);
			if (r < w)
				return nd->node_id;
			r -= w;
		}
		return first;

	case PC_CLSEL_FAILOVER:
	default:
		/* stable by construction: the same map gives the same answer,
		 * so a client does not wander between nodes for no reason */
		return first;
	}
}

int pc_clsel_stale(uint32_t my_term, uint32_t my_seq,
		uint32_t their_term, uint32_t their_seq)
{
	if (their_term != my_term)
		return their_term > my_term;
	return their_seq > my_seq;
}
