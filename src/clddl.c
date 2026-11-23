/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clddl.c - the collection DDL across the fleet: M_COL_SET announces and their apply;
 * announces run on the verb's thread, rz_again_tick on the heartbeat, apply on the peer. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat/compat.h"             /* the Lamport clock */
#include "compat/dprint.h"
#include "config.h"                    /* pc_autoscale_name */
#include "store.h"
#include "wal.h"                       /* a fleet rename is logged */
#include "colname.h"
#include "respmap.h"
#include "cluster.h"
#include "clcol.h"                     /* the M_COL_SET frame */
#include "clmemb.h"                    /* CLMEMB_FEAT_* */
#include "clmsg.h"
#include "clpeers.h"
#include "clsend.h"
#include "clstate.h"
#include "clddl.h"

/* collection names peers announced that break the rule - refused,
 * never created */
static unsigned long long cl_bad_col_names;

unsigned long long pc_cluster_bad_col_names(void)
{
	return __atomic_load_n(&cl_bad_col_names, __ATOMIC_RELAXED);
}

static int col_name_refused(const char *what, const char *s, size_t n)
{
	unsigned long long c;
	char pb[4 * PC_COL_NAME_MAX + 1];

	if (pc_col_name_held_ok(s, n))
		return 0;
	c = __atomic_add_fetch(&cl_bad_col_names, 1, __ATOMIC_RELAXED);
	if (c <= 8 || !(c & (c - 1)))          /* the first eight, then rarely */
		LM_WARN("collection '%s' %s by a peer refused: %s (%llu refused "
			"so far)\n", pc_col_name_print(pb, sizeof pb, s, n), what,
			PC_COL_NAME_RULE, c);
	return 1;
}

/* the repeats of a RESIZE announce: idempotent at the receiver (same generation, an
 * older one ignored), so a repeat costs a datagram per live peer and nothing else */
#define RZ_AGAIN 8
static struct rz_again {
	char name[PC_COL_NAME_MAX];
	size_t nlen;
	int bl, left;
	unsigned long long gen;
	long long due;
} rz_again[RZ_AGAIN];
static pthread_mutex_t rz_again_mx = PTHREAD_MUTEX_INITIALIZER;
static const int rz_again_ms[] = { 1000, 2000, 4000 };   /* +1, +3, +7 s */

static void rz_again_arm(const char *name, size_t nlen, int bl,
		unsigned long long gen, long long now)
{
	int i, pick = 0;

	pthread_mutex_lock(&rz_again_mx);
	for (i = 0; i < RZ_AGAIN; i++) {
		if (rz_again[i].left && rz_again[i].nlen == nlen &&
		        !memcmp(rz_again[i].name, name, nlen)) {
			pick = i;                  /* the newer resize replaces it */
			break;
		}
		if (!rz_again[i].left || rz_again[i].due < rz_again[pick].due)
			pick = i;
	}
	memcpy(rz_again[pick].name, name, nlen);
	rz_again[pick].nlen = nlen;
	rz_again[pick].bl = bl;
	rz_again[pick].gen = gen;
	rz_again[pick].left = 3;
	rz_again[pick].due = now + rz_again_ms[0];
	pthread_mutex_unlock(&rz_again_mx);
}

static size_t col_ann_build(unsigned char *msg, size_t cap, const char *name,
		size_t nlen, int bl, int op, unsigned long long gen)
{
	struct clcol_ann a;

	memset(&a, 0, sizeof a);
	a.op = (unsigned char)op;
	a.buckets_log2 = bl;
	a.gen = gen;
	a.name = name;
	a.nlen = (unsigned int)nlen;
	return clcol_write(msg, cap, M_COL_SET, &a);
}

/* heartbeat thread */
void rz_again_tick(long long now)
{
	unsigned char msg[16 + PC_COL_NAME_MAX];
	int i;

	pthread_mutex_lock(&rz_again_mx);
	for (i = 0; i < RZ_AGAIN; i++) {
		struct rz_again *r = &rz_again[i];

		if (!r->left || r->due > now)
			continue;
		(void)clsend_live(cl_peers(), msg, col_ann_build(msg, sizeof msg, r->name,
			r->nlen, r->bl, CLCOL_OP_RESIZE, r->gen), now);
		r->left--;
		if (r->left)
			r->due = now + rz_again_ms[3 - r->left];
	}
	pthread_mutex_unlock(&rz_again_mx);
}

/* @op is a CLCOL_OP_* value, SENT AS ITSELF: folded into a flag, a RESIZE would
 * go out as a DROP and delete the collection on every peer */
unsigned long long pc_cluster_col_announce(const char *name, size_t nlen,
		int buckets_log2, int op)
{
	unsigned char msg[16 + PC_COL_NAME_MAX];
	unsigned long long gen;
	long long now = cl_now_ms();

	if (!cl_enabled() || nlen == 0 || nlen >= PC_COL_NAME_MAX)
		return 0;
	/* refuse an op this cannot express rather than degrade it to something destructive;
	 * a rename goes through pc_cluster_col_announce2 */
	if (op != CLCOL_OP_SET && op != CLCOL_OP_DROP &&
	        op != CLCOL_OP_RESIZE) {
		LM_ERR("cluster: refusing to announce collection '%.*s' with "
			"op %d - not a one-name op\n", (int)nlen, name, op);
		return 0;
	}
	/* a resize TICKS for its generation: two resizes read off an idle clock could
	 * share one, and their order is what lets a receiver drop the older */
	gen = op == CLCOL_OP_RESIZE ? pc_lamport_tick() : pc_lamport_now();
	(void)clsend_live(cl_peers(), msg, col_ann_build(msg, sizeof msg, name, nlen,
		buckets_log2, op, gen), now);
	if (op == CLCOL_OP_RESIZE)
		rz_again_arm(name, nlen, buckets_log2, gen, now);
	return gen;
}

/* a rename, which needs both names in the one datagram */
void pc_cluster_col_announce2(const char *from, size_t flen, const char *to,
		size_t tlen)
{
	unsigned char msg[16 + 2 * PC_COL_NAME_MAX];
	unsigned long long gen = pc_lamport_now();
	long long now = cl_now_ms();
	struct clcol_ann a;

	memset(&a, 0, sizeof a);
	if (!cl_enabled() || flen == 0 || flen >= PC_COL_NAME_MAX ||
	        tlen == 0 || tlen >= PC_COL_NAME_MAX)
		return;
	a.op = CLCOL_OP_RENAME;
	a.gen = gen;
	a.name = from;
	a.nlen = (unsigned int)flen;
	a.to = to;
	a.tlen = (unsigned int)tlen;
	(void)clsend_live(cl_peers(), msg, clcol_write(msg, sizeof msg, M_COL_SET, &a), now);
}

/* one RESP map override to one peer that reads it: an older build reads
 * CLCOL_OP_MAP as a SET and would create a collection named after the index */
static void respmap_send(struct peer *p, int index, const char *col,
		uint64_t gen, int set)
{
	unsigned char msg[16 + 2 * PC_COL_NAME_MAX];
	char ix[8];
	struct clcol_ann a;

	if (!(p->features & CLMEMB_FEAT_RESPMAP))
		return;
	snprintf(ix, sizeof ix, "%d", index);
	memset(&a, 0, sizeof a);
	a.op = CLCOL_OP_MAP;
	a.buckets_log2 = set ? 1 : 0;
	a.gen = gen;
	a.name = ix;
	a.nlen = (unsigned int)strlen(ix);
	if (set && col && *col) {
		a.to = col;
		a.tlen = (unsigned int)strlen(col);
	}
	clsend_seal(&p->addr, msg, clcol_write(msg, sizeof msg, M_COL_SET, &a));
}

void pc_cluster_respmap_announce(int index, const char *col, uint64_t gen,
		int set)
{
	long long now = cl_now_ms();
	struct clpeers *pt = cl_peers();
	int i;

	if (!cl_enabled())
		return;
	for (i = 0; i < pt->n_peers; i++)
		if (clpeers_live(&pt->peers[i], now))
			respmap_send(&pt->peers[i], index, col, gen, set);
}

static void respmap_sync_cb(int index, const char *col, uint64_t gen, int set,
		void *ctx)
{
	respmap_send(ctx, index, col, gen, set);
}

/* one collection's autoscale mode to one peer - only one that reads
 * it (see CLCOL_OP_AUTOSCALE) */
static void autoscale_send(struct peer *p, const char *name, size_t nlen,
		int mode, unsigned long long gen)
{
	unsigned char msg[16 + PC_COL_NAME_MAX];
	struct clcol_ann a;

	if (!(p->features & CLMEMB_FEAT_AUTOSCALE) || cl_self_no_autoscale() ||
	        nlen == 0 || nlen >= PC_COL_NAME_MAX)
		return;
	memset(&a, 0, sizeof a);
	a.op = CLCOL_OP_AUTOSCALE;
	a.buckets_log2 = mode;
	a.gen = gen;
	a.name = name;
	a.nlen = (unsigned int)nlen;
	clsend_seal(&p->addr, msg, clcol_write(msg, sizeof msg, M_COL_SET, &a));
}

void pc_cluster_autoscale_announce(const char *name, size_t nlen, int mode,
		unsigned long long gen)
{
	long long now = cl_now_ms();
	struct clpeers *pt = cl_peers();
	int i;

	if (!cl_enabled())
		return;
	for (i = 0; i < pt->n_peers; i++)
		if (clpeers_live(&pt->peers[i], now))
			autoscale_send(&pt->peers[i], name, nlen, mode, gen);
}

/* the created set to one peer: every runtime name, live or tombstoned, with the
 * generation that last moved it; sent at first sight, it carries a create across downtime */
void col_sync_peer(struct peer *p)
{
	unsigned char msg[16 + PC_COL_NAME_MAX];
	struct clcol_ann a;
	const char *name;
	unsigned long long gen;
	int i, bl, live;

	for (i = 0; i < pc_store_count(); i++) {
		size_t nl;

		if (pc_store_entry(i, &name, &bl, &gen, &live) != 0)
			continue;              /* not a runtime name */
		nl = strlen(name);
		if (nl == 0 || nl >= PC_COL_NAME_MAX)
			continue;
		memset(&a, 0, sizeof a);
		a.op = live ? CLCOL_OP_SET : CLCOL_OP_DROP;
		a.buckets_log2 = bl;
		a.gen = gen;
		a.name = name;
		a.nlen = (unsigned int)nl;
		clsend_seal(&p->addr, msg,
			clcol_write(msg, sizeof msg, M_COL_SET, &a));
	}
	/* and the RESP map's overrides, tombstones too, to a peer that
	 * reads them - the same anti-entropy carries a map across a downtime */
	pc_respmap_each(respmap_sync_cb, p);
	/* and the autoscale modes set at runtime: a member that was
	 * down, restarted or simply lost the announce takes the fleet's */
	for (i = 0; i < pc_store_count(); i++) {
		int m;

		if (pc_store_autoscale_entry(i, &name, &m, &gen) == 0)
			autoscale_send(p, name, strlen(name), m, gen);
	}
}

/* applied whatever allow_create says here (it gates who may ORIGINATE); the generation
 * orders a name's create and drop, so a re-announce never undoes a later drop */
void handle_col_set(const unsigned char *pt, size_t n)
{
	char name[PC_COL_NAME_MAX];
	struct clcol_ann a;
	unsigned long long gen;
	size_t nl;
	int bl, drop;

	if (!clcol_parse(pt, n, &a))
		return;
	drop = a.op == CLCOL_OP_DROP;
	bl = a.buckets_log2;
	gen = a.gen;
	nl = a.nlen;
	/* the NAME BOUND is this file's: clcol refuses what will not fit the
	 * datagram, PC_COL_NAME_MAX is what the store will accept */
	if (nl >= PC_COL_NAME_MAX)
		return;
	memcpy(name, a.name, nl);
	name[nl] = 0;
	pc_lamport_observe(gen);
	if (a.op == CLCOL_OP_MAP) {        /* the RESP map, by generation */
		char col[PC_COL_NAME_MAX] = "", was[PC_COL_NAME_MAX] = "";
		char *end;
		long idx = strtol(name, &end, 10);
		int set = a.buckets_log2 ? 1 : 0;

		if (*end || idx < 0 || idx >= PC_RESPMAP_MAX)
			return;
		if (set) {
			if (!a.to || !a.tlen || a.tlen >= PC_COL_NAME_MAX)
				return;
			memcpy(col, a.to, a.tlen);
			col[a.tlen] = 0;
		}
		if (pc_respmap_apply((int)idx, col, strlen(col), gen, set, was,
		        sizeof was) == 1) {
			pc_respmap_persist_later();
			if (set)
				LM_NOTICE("resp: DB index %ld mapped to collection "
					"'%s' by the fleet (was '%s')\n", idx, col,
					was[0] ? was : "-");
			else
				LM_NOTICE("resp: DB index %ld unmapped by the fleet "
					"(was '%s'): it answers as resp_collections "
					"maps it\n", idx, was[0] ? was : "-");
		}
		return;
	}
	if (a.op == CLCOL_OP_AUTOSCALE) {  /* a mode, by generation */
		int was = -1;

		/* the test flags: an older build, or deaf to the fleet's */
		if (cl_self_no_autoscale() || cl_self_as_deaf())
			return;
		/* a collection not here yet is skipped: the resync sends the
		 * mode again once the create has reached this node */
		if (pc_store_autoscale_apply(name, nl, a.buckets_log2, gen,
		        &was) == 1) {
			pc_store_persist_later();
			LM_NOTICE("collection '%s': autoscale = %s, set by the "
				"fleet (was %s)\n", name,
				pc_autoscale_name(a.buckets_log2),
				pc_autoscale_name(was));
		}
		return;
	}
	if (a.op == CLCOL_OP_RENAME) {     /* a rename, two names */
		char to[PC_COL_NAME_MAX];
		size_t tl = a.tlen;

		if (tl >= PC_COL_NAME_MAX)
			return;
		memcpy(to, a.to, tl);
		to[tl] = 0;
		if (col_name_refused("as a new name", to, tl))
			return;
		if (pc_store_find(name, nl) && !pc_store_find(to, tl) &&
		        pc_store_rename(name, nl, to, tl, gen) == 0) {
			pc_wal_col_rename(name, to);   /* as the verb does */
			pc_store_persist_later();
			LM_NOTICE("collection '%s' renamed to '%s' by the "
				"fleet\n", name, to);
		}
		return;
	}
	if (drop) {
		if (pc_store_find(name, nl))
			pc_store_drop_gen(name, nl, gen);
		else
			pc_store_note_drop(name, nl, gen);
	} else if (a.op == CLCOL_OP_RESIZE) {   /* resize to bl */
		/* ordered by the announce's generation, and HELD while a resize runs here */
		if (pc_store_find(name, nl) &&
		        pc_store_resize_request(name, nl, bl, gen) == 2)
			LM_NOTICE("collection '%s': the fleet's resize to 2^%d "
				"is held until the one running here finishes\n",
				name, bl);
	} else if (!pc_store_find(name, nl)) {
		int rc;

		if (col_name_refused("announced", name, nl))
			return;
		rc = pc_store_create_gen(name, nl, bl, gen);

		if (rc == 0)
			LM_NOTICE("collection '%s' created by the fleet: "
				"2^%d buckets\n", name, bl);
		else if (rc != -4)
			LM_ERR("collection '%s' announced by a peer could not "
				"be created here (%d)\n", name, rc);
	}
	/* not here: this is the cluster thread, and the fsync would hold every pull and forward */
	pc_store_persist_later();
}
