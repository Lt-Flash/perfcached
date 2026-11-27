/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clrecv.c - replication receive/apply: M_MIGRATE and the gathered M_MIGRATE_MANY / M_REPL_MANY,
 * the one-record apply with its drop floor and ceiling drop, the group ack, the apply threads */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "compat/compat.h"           /* pc_lamport_observe */
#include "core/pcache_htable.h"
#include "quiesce.h"
#include "store.h"
#include "upstream.h"                /* an upstream copy's expiry */
#include "events.h"                  /* applied-here notifications */
#include "wal.h"
#include "cluster.h"                 /* pc_loc_clear, pc_neg_ver */
#include "clstate.h"
#include "clsend.h"                  /* the send path */
#include "clmsg.h"                   /* M_MIGRATE_ACK */
#include "clmig.h"
#include "clapply.h"
#include "clorder.h"
#include "clloc.h"                   /* clloc_hash */
#include "clpush.h"                  /* CLPUSH_ACK_TAG */
#include "clrecv.h"

/* what a group's ack counts (apply_rec: 1 stored, 2 this version or newer is here, 0 else): a
 * migration only STORED; a push, whose ack lets the sender drop a non-holder copy, a 2 too */
static int apply_counts(int rc, uint32_t req)
{
	return rc == 1 || (rc == 2 && (req & CLPUSH_ACK_TAG));
}

/* daemon.c runs one of these per thread, idx 0..n-1, each with a thread
 * slot of its own (the WAL ring a migration lands in is the slot's) */
void pc_cluster_apply_thread(volatile int *stop, int idx)
{
	struct clapply *ap = cl_ap();      /* once: the call is per record otherwise */
	struct clapply_item it;

	while (clapply_take(ap, idx, stop, &it, cl_now_ms())) {
		int rc;

		pc_qs_enter();                 /* a turn per record: parked while the take waits */
		rc = apply_rec(&it.r, NULL);
		if (it.grp && apply_counts(rc, it.grp->req))
			__atomic_fetch_add(&it.grp->stored, 1, __ATOMIC_RELAXED);
		clapply_done(ap, idx, &it);
		pc_qs_exit();
	}
}

/* migration: the receiver becomes the holder */
void handle_migrate(const unsigned char *pt, size_t n,
		const pc_addr *from)
{
	extern void pc_wal_upsert(const char *, const char *, int,
		const char *, int, unsigned int, unsigned long long);
	pcache_htable_t *ht;
	unsigned char ack[CLMIG_ACK_LEN];
	struct clmig_single sg;
	uint32_t req, ttl_left;
	unsigned int cn, kn, vlen, exp = 0;
	unsigned long long tv;
	int ok = 0;
	str k, v;
	char colz[256];

	if (!clmig_single_parse(pt, n, &sg))
		return;
	req = sg.req;
	ttl_left = sg.ttl_left;
	cn = sg.collen;
	kn = sg.klen;
	vlen = sg.vlen;
	memcpy(colz, sg.col, cn);
	colz[cn] = 0;

	ht = pc_store_find(colz, cn);
	/* the frame carries no version: never over a copy held here, nor past a delete seen */
	if (ht && kn && kn <= 4096 && !(pc_neg_ver(colz, cn, sg.key, kn, &tv) && tv)) {
		k.s = (char *)sg.key;
		k.len = (int)kn;
		v.s = (char *)sg.val;
		v.len = (int)vlen;
		if (ttl_left)                   /* as apply_rec's */
			exp = pc_upstream_copy_exp(sg.col, cn, ttl_left);
		ok = pcache_ht_store_ver(ht, &k, &v, exp, 0, 1) == 0;
		if (ok) {
			pc_wal_upsert(colz, k.s, k.len, v.s, v.len, exp,
				pcache_last_ver);
			pc_loc_clear(colz, cn, k.s, kn);   /* we hold it now */
			cl_px_migrated_in_plain();
			if (pc_store_shard_enabled(ht))
				cl_shard_note_change();
		}
	}
	clsend_seal(from, ack,
		clmig_ack_write(ack, sizeof ack, M_MIGRATE_ACK, req, ok, NULL));
}

/* the version each dropped copy was dropped FOR, so an OLDER copy taking the freed cell is refused;
 * lossy and never expired, as a copy older than a written version is stale whatever follows */
#define DROPFLOOR_SLOTS  65536
/* (h, ver) change as one under the slot's sequence: odd while a writer holds it, and the
 * writers are many (apply threads, relay receive, the peer and bulk threads) */
static struct { uint32_t seq; uint64_t h, ver; } dropfloor[DROPFLOOR_SLOTS];
static int dropfloor_any;

static void dropfloor_note(const char *col, size_t cn, const str *k,
		unsigned long long ver)
{
	uint64_t h = clloc_hash(col, cn, k->s, (size_t)k->len) | 1;
	size_t i = (size_t)(h & (DROPFLOOR_SLOTS - 1));
	uint32_t s = __atomic_load_n(&dropfloor[i].seq, __ATOMIC_RELAXED);

	for (;;) {
		if (s & 1)
			s = __atomic_load_n(&dropfloor[i].seq, __ATOMIC_RELAXED);
		else if (__atomic_compare_exchange_n(&dropfloor[i].seq, &s, s + 1, 0,
		        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
			break;
	}
	if (dropfloor[i].h != h || dropfloor[i].ver < ver) {
		__atomic_store_n(&dropfloor[i].h, h, __ATOMIC_RELAXED);
		__atomic_store_n(&dropfloor[i].ver, (uint64_t)ver, __ATOMIC_RELAXED);
	}
	__atomic_store_n(&dropfloor[i].seq, s + 2, __ATOMIC_RELEASE);
	__atomic_store_n(&dropfloor_any, 1, __ATOMIC_RELEASE);
}

/* 1 = @ver is older than a copy this node dropped for a newer one */
int dropfloor_refuses(const char *col, size_t cn, const char *key,
		size_t kn, unsigned long long ver)
{
	uint64_t h, v;
	uint32_t s;
	size_t i;

	if (!__atomic_load_n(&dropfloor_any, __ATOMIC_RELAXED))
		return 0;
	h = clloc_hash(col, cn, key, kn) | 1;
	i = (size_t)(h & (DROPFLOOR_SLOTS - 1));
	/* a slot mid-write reads as no floor, which refuses nothing */
	s = __atomic_load_n(&dropfloor[i].seq, __ATOMIC_ACQUIRE);
	if (s & 1 || __atomic_load_n(&dropfloor[i].h, __ATOMIC_RELAXED) != h)
		return 0;
	v = __atomic_load_n(&dropfloor[i].ver, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	if (__atomic_load_n(&dropfloor[i].seq, __ATOMIC_RELAXED) != s)
		return 0;
	if (!v || ver >= v)
		return 0;
	cl_px_recv_below_dropped();
	return 1;
}

/* a peer's copy refused at the arena ceiling: drop the OLDER copy held here, or it is served for
 * ever (a read then pulls the current one).  Never a delete: no tombstone, nothing sent */
void nomem_drop(pcache_htable_t *ht, const char *col, size_t cn,
		const str *k, unsigned long long ver)
{
	cl_px_recv_nomem();
	if (pcache_ht_drop_older(ht, k, ver)) {
		cl_px_recv_stale_dropped();
		dropfloor_note(col, cn, k, ver);
	}
}

/* a tombstone NEWER than the copy means the copy is the set the
 * delete superseded, arriving after it - counted apart from "older" */
static int apply_refuse(void *arg, unsigned long long ver)
{
	const struct clapply_rec *r = arg;
	unsigned long long tv;

	if (pc_neg_ver((const char *)r->col, r->collen, (const char *)r->key,
	        r->klen, &tv) && tv && ver <= tv) {
		cl_px_recv_tombstoned();
		return 1;
	}
	/* older than a copy dropped at the ceiling - resolved */
	return dropfloor_refuses((const char *)r->col, r->collen,
		(const char *)r->key, r->klen, ver);
}

static unsigned long long apply_tomb(void *arg)
{
	const struct clapply_rec *r = arg;
	unsigned long long tv;

	return pc_neg_ver((const char *)r->col, r->collen, (const char *)r->key,
		r->klen, &tv) ? tv : 0;
}

/* ONE record of a migration or eager-replication group, on the receiver (apply_threads 1) or the
 * thread its key hashes to; all it touches is already shared with the workers.  1 = stored. */
int apply_rec(const struct clapply_rec *r, void *ht_or_null)
{
	extern void pc_wal_upsert(const char *, const char *, int,
		const char *, int, unsigned int, unsigned long long);
	pcache_htable_t *ht = ht_or_null;
	const char *col = (const char *)r->col;
	/* a non-holder's handoff is this holder's own write from here on */
	int passive = r->passive && !(r->flags & CLMIG_F_HANDOFF);
	str k, v;
	unsigned int exp;
	int rc;

	/* the collection is RESOLVED ONCE: the inline caller passes the handle its prefetch peek
	 * found, an apply thread gets NULL */
	/* the sender's clock is at least this far along; the store folds it in too, but only for
	 * records that reach a collection */
	pc_lamport_observe(r->ver);
	if (!ht)
		ht = pc_store_find(col, r->collen);
	/* DO NOT refuse a replica we do not hold here: a receiver whose view has not converged
	 * refuses copies it SHOULD hold; surplus is for a settled-only local pass */
	if (!ht)
		return 0;
	k.s = (char *)r->key;
	k.len = (int)r->klen;
	/* a replicated write is work on this node too: it is the apply thread a hot key pins */
	pc_store_hot_note(ht, k.s, (size_t)k.len);
	v.s = (char *)r->val;
	v.len = (int)r->vlen;
	/* a copy of an upstream-bound collection's record never
	 * outlives its key - one tick under what it arrived with */
	exp = pc_upstream_copy_exp(col, r->collen, r->ttl_left);
	/* PASSIVE for an eager replica (it never re-propagates), off the WAL (the author persists
	 * it); both carry the SENDER's version and refuse one older than held */
	rc = clorder_set(ht, &k, &v, exp,
		(unsigned char)((passive ? PCACHE_F_PASSIVE : 0) |
		(r->flags & PCACHE_F_CARRY)), r->ver,           /* the type */
		&(struct clorder_tomb){ apply_refuse, apply_tomb, (void *)r });
	if (rc == CLORDER_REFUSED)
		return 2;              /* a newer delete supersedes it */
	if (rc == CLORDER_TAKEN) {
		cl_px_recv_tombstoned();
		return 2;
	}
	if (rc == PCACHE_E_OLDER) {
		cl_px_recv_older();
		return 2;              /* this version or newer is here */
	}
	if (rc == -2)
		nomem_drop(ht, col, r->collen, &k, r->ver);
	if (rc != 0)
		return 0;
	pc_upstream_note_copy(col, r->collen, k.s, (size_t)k.len, exp, r->ver);
	/* an eager replica applied here notifies here; a migrated
	 * record (a placement move) does not */
	if (r->passive)
		pc_ev_applied(ht, &k);
	if (!passive) {
		char colz[PC_COL_NAME_MAX + 1];

		/* the one NUL-terminated copy, for the WAL on the authored path; a found @ht means
		 * a bounded registered name, checked anyway */
		if (r->collen <= PC_COL_NAME_MAX) {
			memcpy(colz, col, r->collen);
			colz[r->collen] = 0;
			pc_wal_upsert_fl(colz, k.s, k.len, v.s, v.len, exp,
				pcache_last_ver, r->flags & PCACHE_F_CARRY);
		}
		pc_loc_clear(col, r->collen, k.s, r->klen);
		if (pc_store_shard_enabled(ht))
			cl_shard_note_change();
	}
	cl_px_migrated_in();
	return 1;
}

/* a group's ack, sent by whichever thread applied its last record; clsend_seal is safe from any
 * thread (a shared socket, atomic sendto, an immutable key, a fresh nonce per seal) */
void ack_group(const struct clapply_grp *g, void *ctx)
{
	unsigned char ack[8];
	unsigned int st = __atomic_load_n(&g->stored, __ATOMIC_RELAXED);

	(void)ctx;
	clsend_seal(&g->from, ack, clmig_ack_write(ack, sizeof ack,
		M_MIGRATE_ACK, g->req, st > 0, &st));
}

/* the gathered variant, one ack carrying the stored count; @here: apply every record on this
 * thread - a relay receive thread is not the apply rings' one producer */
void handle_migrate_many(const unsigned char *pt, size_t n,
		const pc_addr *from, int passive, int here)
{
	struct clapply *ap = cl_ap();      /* once a datagram, not a record */
	struct clapply_grp *g = NULL;
	uint32_t req;
	unsigned int cnt, i, stored = 0;
	size_t off = CLMIG_GHDR;
	pcache_htable_t *ht;

	if (n < CLMIG_GHDR)
		return;
	req = clmig_group_req(pt);
	cnt = clmig_group_count(pt);
	/* req 0 is a write-path push, fire-and-forget: nobody to ack; a sweep batch's nonzero req
	 * is acked after its LAST record is applied, by whichever thread that is */
	if (req && ap->n)
		g = clapply_open(ap, from, req);
	for (i = 0; i < cnt; i++) {
		struct clmig_rec r;
		struct clapply_rec ar;
		unsigned int cn;

		/* a truncated record stops the walk rather than reading past
		 * the datagram; what was stored is still acked */
		if (!clmig_rec_parse(pt, n, &off, &r))
			break;
		/* the key limit every other plane assumes, as the bulk receive refuses it */
		if (!r.klen || r.klen > 4096)
			continue;
		cn = r.collen;
		ar.col = (const unsigned char *)r.col;
		ar.collen = cn;
		ar.key = (const unsigned char *)r.key;
		ar.klen = r.klen;
		ar.val = (const unsigned char *)r.val;
		ar.vlen = r.vlen;
		ar.ttl_left = r.ttl_left;
		ar.ver = r.ver;
		ar.passive = passive;
		ar.flags = (unsigned char)(r.flags & (PCACHE_F_CARRY | CLMIG_F_HANDOFF));
		/* hand it to the thread its key hashes to; a refusal (a record the ring can never
		 * hold, or shutdown) is applied here, in order, as only this thread has seen it */
		if (ap->n && !here && clapply_push(ap, &ar, g) >= 0)
			continue;
		/* resolved ONCE - for the peek below and for the apply */
		ht = pc_store_find(r.col, cn);
		/* a cold bucket miss per record: peek, not consuming, at the next record and
		 * prefetch its bucket while this one stores (SAME collection only) */
		if (ht && i + 1 < cnt) {
			size_t poff = off;
			struct clmig_rec nx;

			if (clmig_rec_parse(pt, n, &poff, &nx) &&
			        nx.collen == cn &&
			        memcmp(nx.col, r.col, cn) == 0) {
				str nk;

				nk.s = (char *)nx.key;
				nk.len = (int)nx.klen;
				pcache_ht_prefetch(ht, &nk);
			}
		}
		if (apply_counts(apply_rec(&ar, ht), req)) {
			stored++;
			if (g)
				__atomic_fetch_add(&g->stored, 1, __ATOMIC_RELAXED);
		}
	}
	if (g) {
		clapply_close(ap, g);   /* the last record out sends the ack */
		return;
	}
	if (!req)
		return;
	{
		unsigned char ack[8];
		unsigned int st = (unsigned int)stored;

		clsend_seal(from, ack, clmig_ack_write(ack, sizeof ack,
			M_MIGRATE_ACK, req, stored > 0, &st));
	}
}

/* a write-path group from the relay port, gated there (clrp_rx) */
void cl_repl_apply_group(const unsigned char *pt, size_t n,
		const pc_addr *from)
{
	handle_migrate_many(pt, n, from, 1, 1);
}
