/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clfleet.c - one collection's figures for the whole fleet (M15).  See
 * clfleet.h.  Moved from pc_cluster_col_fleet() (S164) unchanged in what
 * it computes; the peer walk stays in cluster.c.
 */
#include <string.h>

#include "clfleet.h"

void clfleet_begin(struct clfleet *a, int basis,
		const struct clmemb_col *own)
{
	memset(a, 0, sizeof *a);
	a->f.basis = basis;
	a->f.members = a->f.reporting = 1;
	a->f.copies = a->emax = own->entries;
	a->xsum = a->xmax = own->expired;
	a->f.hits = own->hits;
	a->f.misses = own->misses;
	a->f.stores = own->stores;
	a->f.removes = own->removes;
}

void clfleet_member(struct clfleet *a, const struct clmemb_col *c)
{
	a->f.members++;
	if (!c)
		return;
	a->f.reporting++;
	a->f.copies += c->entries;
	if (c->entries > a->emax)
		a->emax = c->entries;
	a->xsum += c->expired;
	if (c->expired > a->xmax)
		a->xmax = c->expired;
	a->f.hits += c->hits;
	a->f.misses += c->misses;
	a->f.stores += c->stores;
	a->f.removes += c->removes;
}

void clfleet_end(struct clfleet *a, int k, struct pc_col_fleet *out)
{
	int d;

	*out = a->f;
	switch (a->f.basis) {
	case PC_FLEET_FULLEST:
	case PC_FLEET_AT_LEAST:
		out->entries = a->emax;
		out->expired = a->xmax;
		break;
	case PC_FLEET_PER_K:
		/* K copies, or one on every member while there are fewer */
		d = k;
		if (d > out->reporting)
			d = out->reporting;
		if (d < 1)
			d = 1;
		out->entries = (out->copies + (uint64_t)d / 2) / (uint64_t)d;
		out->expired = (a->xsum + (uint64_t)d / 2) / (uint64_t)d;
		break;
	default:
		out->entries = out->copies;
		out->expired = a->xsum;
	}
}

const struct clmemb_col *clfleet_find(const struct clmemb_col *cols,
		int ncols, uint64_t hash)
{
	int k;

	for (k = 0; k < ncols && k < CLMEMB_COLS_MAX; k++)
		if (cols[k].hash == hash)
			return &cols[k];
	return NULL;
}

const char *pc_fleet_basis_name(int basis)
{
	switch (basis) {
	case PC_FLEET_FULLEST:  return "fullest";
	case PC_FLEET_AT_LEAST: return "at_least";
	case PC_FLEET_PER_K:    return "per_k";
	default:                return "sum";
	}
}
