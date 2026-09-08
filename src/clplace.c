/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clplace.c — weighted rendezvous, in integers.
 */
#include <string.h>

#include "clplace.h"
#include "pc_mix.h"                 /* the ONE rendezvous mix */

/* The public name for pc_hrw_mix(): kept because clplace.h exports it
 * and the map placer is unit-tested through it.  The body moved to
 * src/pc_mix.h so this and cluster.c and libperfd cannot drift. */
uint64_t pc_clplace_mix(uint64_t kh, uint32_t addr, uint16_t port)
{
	return pc_hrw_mix(kh, addr, port);
}

/* log2(1 + i/32) in PC_CLPLACE_FBITS fixed point, for the mantissa.
 * The last entry MUST be exactly 1<<FBITS - log2(2) is 1, and a table
 * that overshoots it makes the D term underflow for large h, which
 * silently reverses the ordering at the top of the range. */
static const uint32_t LOG2_TAB[33] = {
	      0,   46551,   91711,  135563,  178179,  219628,  259971,  299266,
	 337566,  374920,  411375,  446971,  481749,  515745,  548995,  581529,
	 613378,  644570,  675132,  705089,  734464,  763280,  791557,  819315,
	 846573,  873348,  899658,  925518,  950944,  975949, 1000547, 1024752,
	1048576
};

/*
 * (64 - log2(h)) in fixed point.  Never zero: h < 2^64 always, so
 * log2(h) < 64 - and a zero here would divide the comparison by nothing.
 * h == 0 gives the maximum term, i.e. the lowest possible score, which
 * is the right answer for a node whose mix landed on zero.
 */
uint64_t pc_clplace_dterm(uint64_t h)
{
	unsigned int msb, idx;
	uint64_t norm, frac, lo, hi, rem;

	if (!h)
		return (uint64_t)64 << PC_CLPLACE_FBITS;

	msb = 63;
	while (!(h & (1ull << msb)))
		msb--;

	/* mantissa: the 5 bits below the leading 1 index the table, the
	 * next bits interpolate between entries */
	norm = h << (63 - msb);            /* leading 1 now at bit 63 */
	idx = (unsigned int)((norm >> 58) & 0x1F);
	rem = (norm >> 38) & 0xFFFFF;      /* 20 bits below the index */
	lo = LOG2_TAB[idx];
	hi = LOG2_TAB[idx + 1];
	frac = lo + ((hi - lo) * rem >> 20);

	/* log2(h) = msb + frac ; the term is 64 - that */
	return ((uint64_t)(64 - msb) << PC_CLPLACE_FBITS) - frac;
}

/* a beats b?  w_a/D_a > w_b/D_b  <=>  w_a*D_b > w_b*D_a.  Ties break on
 * node id, so two nodes cannot disagree about a colliding key. */
static int beats(uint32_t wa, uint64_t da, uint16_t ida,
		uint32_t wb, uint64_t db, uint16_t idb)
{
	/* w <= 65535 and D <= 64<<20, so the products stay well inside 64
	 * bits - no widening needed, and none of the rounding a float
	 * version would bring */
	uint64_t la = (uint64_t)wa * db;
	uint64_t lb = (uint64_t)wb * da;

	if (la != lb)
		return la > lb;
	return ida > idb;
}

uint16_t pc_clplace_owner(const struct pc_clmap *m, uint64_t kh)
{
	unsigned int i;
	uint16_t best_id = 0;
	uint32_t best_w = 0;
	uint64_t best_d = 0;
	int have = 0;

	if (!m)
		return 0;
	for (i = 0; i < m->nnodes; i++) {
		const struct pc_clmap_node *n = &m->node[i];
		uint32_t w;
		uint64_t d;

		if (!pc_clmap_placeable(n))
			continue;
		w = pc_clmap_weight(n);
		d = pc_clplace_dterm(pc_clplace_mix(kh, n->addr,
			n->cluster_port));
		if (!have || beats(w, d, n->node_id, best_w, best_d, best_id)) {
			best_w = w;
			best_d = d;
			best_id = n->node_id;
			have = 1;
		}
	}
	return have ? best_id : 0;
}

int pc_clplace_rank(const struct pc_clmap *m, uint64_t kh,
		uint16_t *out, int max)
{
	struct cand { uint32_t w; uint64_t d; uint16_t id; } c[PC_CLMAP_MAXNODE];
	unsigned int i;
	int n = 0, k, j;

	if (!m || !out || max <= 0)
		return 0;
	for (i = 0; i < m->nnodes; i++) {
		const struct pc_clmap_node *nd = &m->node[i];

		if (!pc_clmap_placeable(nd))
			continue;
		c[n].w = pc_clmap_weight(nd);
		c[n].d = pc_clplace_dterm(pc_clplace_mix(kh, nd->addr,
			nd->cluster_port));
		c[n].id = nd->node_id;
		n++;
	}
	/* selection sort of the top @max - n is at most 257, and callers
	 * want a ranked list per placement decision, not per get */
	if (max > n)
		max = n;
	for (k = 0; k < max; k++) {
		int best = k;

		for (j = k + 1; j < n; j++)
			if (beats(c[j].w, c[j].d, c[j].id,
			          c[best].w, c[best].d, c[best].id))
				best = j;
		if (best != k) {
			struct cand t = c[k];

			c[k] = c[best];
			c[best] = t;
		}
		out[k] = c[k].id;
	}
	return max;
}
