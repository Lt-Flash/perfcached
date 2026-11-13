/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* presence.c - EXISTS, TYPE, TTL and PTTL across the fleet, and the native
 * door's exists and ttl.  See presence.h. */
#include <stddef.h>

#include "compat/timer.h"                /* get_ticks */
#include "cluster.h"
#include "json.h"
#include "ptree.h"
#include "verbs.h"
#include "verbsint.h"
#include "presence.h"

static __thread unsigned int pres_req_v;   /* the pull op_pres_miss parked */
static __thread int pres_kind_v;

int op_pres_miss(pcache_htable_t *ht, str *k, int kind, unsigned int *park)
{
	unsigned int req = 0;

	if (!ht || !pc_cluster_enabled() ||
	    op_get_miss(ht, k, &req) != PC_OP_PARKED)
		return PC_OP_ABSENT;
	pres_req_v = req;
	pres_kind_v = kind;
	*park = req;
	return PC_OP_PARKED;
}

int pc_pres_park_take(unsigned int req)
{
	int kind = req && req == pres_req_v ? pres_kind_v : 0;

	pres_req_v = 0;
	pres_kind_v = 0;
	return kind;
}

/* @ttl: -1 none, else the seconds left */
static void tree_put(struct pc_tw *w, int kind, int found,
		unsigned long long ver, long long ttl)
{
	pc_tw_map(w);
	if (kind == PC_PRES_EXISTS) {
		pc_tw_key(w, "exists");
		pc_tw_bool(w, found);
		if (found) {
			pc_tw_key(w, "ver");
			pc_tw_i64(w, (long long)ver);
		}
	} else {
		pc_tw_key(w, "ttl");
		pc_tw_i64(w, found ? ttl : -2);
	}
	pc_tw_end(w);
}

/* exists [u8], ttl [i64 LE] */
static size_t bin_put(char *pay, int kind, int found, long long ttl)
{
	unsigned long long v = (unsigned long long)(found ? ttl : -2);
	int i;

	if (kind == PC_PRES_EXISTS) {
		pay[0] = found ? 1 : 0;
		return 1;
	}
	for (i = 0; i < 8; i++)
		pay[i] = (char)(v >> (8 * i));
	return 8;
}

/* 1 when this node holds @k; exists by version first when @want_ver (a
 * fetched copy or a PRESENT marker exists too, with version 0) */
static int here(pcache_htable_t *ht, str *k, int kind, int want_ver,
		unsigned long long *ver, long long *ttl)
{
	unsigned int exp = 0;
	int rc;

	*ver = 0;
	*ttl = -1;
	if (kind == PC_PRES_EXISTS) {
		if (want_ver && pcache_ht_getver(ht, k, ver) == 0)
			return 1;
		*ver = 0;
		return probe_or_present(ht, k, NULL, NULL, NULL, NULL) == 0;
	}
	rc = probe_or_present(ht, k, NULL, &exp, NULL, NULL);
	if (exp)
		*ttl = (long long)exp - (long long)get_ticks();
	return rc != -2;
}

void pc_pres_cmd(pcache_htable_t *ht, str *k, int kind, struct pc_tw *out,
		unsigned int *park)
{
	unsigned long long ver;
	long long ttl;
	int found = here(ht, k, kind, 1, &ver, &ttl);

	if (!found)
		(void)op_pres_miss(ht, k, kind, park);
	tree_put(out, kind, found, ver, ttl);
}

void pc_pres_verb(pcache_htable_t *ht, str *k, int kind, struct pc_jw *out,
		unsigned int *park)
{
	unsigned long long ver;
	long long ttl;
	char pay[8];
	int found = here(ht, k, kind, 0, &ver, &ttl);

	if (!found)
		(void)op_pres_miss(ht, k, kind, park);
	pc_jw_raw(out, pay, bin_put(pay, kind, found, ttl));
}

void pc_pres_tree(struct pc_tw *w, int kind, int found,
		unsigned long long ver, unsigned int ttl_left)
{
	tree_put(w, kind, found, ver, ttl_left ? (long long)ttl_left : -1);
}

size_t pc_pres_bin(char *pay, int kind, int found, unsigned int ttl_left)
{
	return bin_put(pay, kind, found, ttl_left ? (long long)ttl_left : -1);
}
