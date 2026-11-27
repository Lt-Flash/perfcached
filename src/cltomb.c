/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* cltomb.c - drives the delete numbering's watermarks (forgetting, gaps, floors, tails, rebuild
 * asks, holds) and the replay of retained deletes; every entry point runs on the peer thread. */
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "compat/dprint.h"
#include "fnv1a.h"
#include "cluster.h"
#include "recover.h"                 /* came_back_empty */
#include "clcodec.h"                 /* the LE wire codec */
#include "clfwd.h"                   /* the numbered tombstone frame */
#include "clretain.h"
#include "clres.h"                   /* the id reservations */
#include "ackstate.h"                /* the watermarks */
#include "clboot.h"                  /* BOOT_MAX_CAND */
#include "clpeers.h"
#include "clmsg.h"                   /* the datagram types */
#include "clsend.h"                  /* the send path */
#include "logrl.h"                   /* LRL_GAP_FLOOR */
#include "clstate.h"
#include "cltomb.h"

static int came_back_empty(void);

static struct { uint64_t oid; uint32_t incarn; uint64_t mark; } frozen[64];
static int nfrozen;

uint64_t freeze_mark(uint64_t oid, uint32_t incarn, uint64_t mark)
{
	int i;

	for (i = 0; i < nfrozen; i++)
		if (frozen[i].oid == oid && frozen[i].incarn == incarn)
			return frozen[i].mark;
	if (nfrozen < 64) {
		frozen[nfrozen].oid = oid;
		frozen[nfrozen].incarn = incarn;
		frozen[nfrozen++].mark = mark;
	}
	return mark;
}

/* a node back empty floors each space at the lowest mark its candidate sources announced (the
 * pull may fall back to any): what they forgot before it joined is never asked for */
struct bfl { uint64_t oid; uint32_t incarn; uint64_t lo; int n; };
static struct bfl bflv[CLACK_SPACES];
static int bfln;
static uint64_t bfl_members[BOOT_MAX_CAND];
static int bfl_nmem;

static int bfl_cb(uint64_t member, uint32_t incarn, uint64_t oid,
		uint32_t sincarn, uint64_t mark, void *ctx)
{
	int i, k;

	(void)incarn; (void)ctx;
	for (k = 0; k < bfl_nmem && bfl_members[k] != member; k++)
		;
	if (k == bfl_nmem)
		return 0;
	for (i = 0; i < bfln; i++)
		if (bflv[i].oid == oid && bflv[i].incarn == sincarn)
			break;
	if (i == bfln) {
		if (bfln == CLACK_SPACES)
			return 0;
		bflv[i].oid = oid;
		bflv[i].incarn = sincarn;
		bflv[i].lo = mark;
		bflv[i].n = 0;
		bfln++;
	}
	if (mark < bflv[i].lo)
		bflv[i].lo = mark;
	bflv[i].n++;
	return 0;
}

void acks_boot_floors(const int *ids, int n)
{
	int i, k;

	/* a resync's pull is a joiner's: what this node held of the
	 * spaces before it is gone, its sources' marks are the floor */
	if (!(came_back_empty() || cl_resync_floor()) || !pc_ack_enabled() ||
	    cl_self_no_acks())
		return;
	bfl_nmem = 0;
	for (i = 0; i < n && bfl_nmem < BOOT_MAX_CAND; i++) {
		struct peer *p = clpeers_by_id_live(cl_peers(), ids[i],
			cl_now_ms());

		if (p && p->has_ident && p->acks)
			bfl_members[bfl_nmem++] = fnv1a64(p->ident, sizeof p->ident);
	}
	if (bfl_nmem < n)
		return;                        /* a candidate announces no marks */
	bfln = 0;
	pc_ack_fleet_each(bfl_cb, NULL);
	for (k = 0; k < bfln; k++)
		if (bflv[k].n == bfl_nmem && bflv[k].lo &&
		    pc_ack_floor(bflv[k].oid, bflv[k].incarn, bflv[k].lo) ==
		        CLACK_NEW)
			cl_st_acks_floors();
	if (cl_st_acks_floors_read())
		LM_NOTICE("cluster: %s - %llu numbering space(s) floored at "
			"what the bootstrap's sources confirmed\n", cl_resync_floor()
			? "resynced" : "came back empty", cl_st_acks_floors_read());
	cl_resync_floor_clear();           /* once, at the pick */
}

/* a delete is broadcast once: a peer back from a silence gets every one retained after its
 * tomb_mark (when it was last heard) again, as versioned tombstones - a later write survives */
static unsigned long long tomb_catchup_total;
/* the members back after a silence the table did not cover - for
 * the alerts (cluster.peer_absent), the last one's line kept */
static pthread_mutex_t tomb_gap_mx = PTHREAD_MUTEX_INITIALIZER;
static unsigned long long tomb_gaps;
static char tomb_gap_text[256];

int pc_cluster_tomb_alert(struct pc_tomb_alert *out)
{
	struct clretain_stats rs;
	struct timespec ts;

	memset(out, 0, sizeof *out);
	if (!clretain_enabled())
		return 0;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	clretain_get_stats(&rs, (unsigned int)ts.tv_sec);
	out->retain_s = rs.retain_s;
	out->max = rs.max;
	out->entries = rs.entries;
	out->dropped_early = rs.dropped_early;
	out->noted = rs.noted;
	out->dropped_early_expiries = rs.dropped_early_expiries;
	out->noted_expiries = rs.noted_expiries;
	pthread_mutex_lock(&tomb_gap_mx);
	out->gaps = tomb_gaps;
	memcpy(out->gap_text, tomb_gap_text, sizeof out->gap_text);
	pthread_mutex_unlock(&tomb_gap_mx);
	return 1;
}

static int catchup_by_marks;           /* set each tick */

/* tests only: PERFCACHED_TEST_TOMB_BURST=<n> - the catch-up's per-tick bound (4,000) */
static unsigned int catchup_burst(void)
{
	static unsigned int b;

	if (!b) {
		const char *e = getenv("PERFCACHED_TEST_TOMB_BURST");

		b = e && atoi(e) >= 4 ? (unsigned int)atoi(e) : 4000;
	}
	return b;
}

static void tomb_catchup_cb(const char *col, size_t cn, const char *key,
		size_t kn, uint64_t ver, int expiry, uint64_t oid, uint32_t incarn,
		uint64_t seq, void *ctx)
{
	unsigned char msg[CLFWD_TOMB_MAX];
	struct peer *p = ctx;
	struct clfwd_key fk = {
		.col = col, .collen = (unsigned int)cn,
		.key = key, .klen = (unsigned int)kn };
	/* with its number, so the peer counts what it gets back */
	struct clfwd_tomb_num num = { oid, incarn, seq };

	/* a numbered delete the member's marks cover is not sent again; the rest go by time, since
	 * a space born during a partition is one the other side never announced or asks for */
	if (seq && catchup_by_marks && p->has_ident) {
		uint64_t m = 0;

		if (pc_ack_fleet_mark(fnv1a64(p->ident, sizeof p->ident), oid,
		        incarn, &m) && m >= seq)
			return;
	}
	/* an expiry says so - the peer counted it as a delete - and a kept one says that too */
	size_t n = clfwd_tomb_build_n(msg, M_TOMBSTONE, &fk, ver,
		(expiry ? CLFWD_TOMB_F_EXPIRY : 0) |
		(expiry == 2 ? CLFWD_TOMB_F_KEEP : 0), &num);

	if (n && clsend_seal(&p->addr, msg, n) == 0) {
		p->tomb_sent++;
		tomb_catchup_total++;
	}
}

/* an expiry is forgotten after this, each member reaping its own copy by then - unless its TTL
 * was shortened (PCACHE_F_SHORTENED): an older copy can outlive it, so it keeps the time rule */
#define PC_TOMB_EXPIRY_HOLD_S 10
/* forgetting: each space goes through the LOWEST mark of this node and every member, reserved ids
 * included while the time rule would keep what they missed; never announced counts as 0 */
struct fsp { uint64_t oid; uint32_t incarn; uint64_t mark, hi; };
static struct fsp fspv[CLACK_SPACES];
static unsigned int fspn;
static uint64_t fmembers[PC_CL_MAXPEER + CLRES_MAX];

static int fsp_cb(uint64_t oid, uint32_t incarn, uint64_t mark,
		uint64_t applied, uint64_t hi, void *ctx)
{
	(void)applied; (void)ctx;
	if (fspn < CLACK_SPACES) {
		fspv[fspn].oid = oid;
		fspv[fspn].incarn = incarn;
		fspv[fspn].hi = hi;
		fspv[fspn++].mark = mark;
	}
	return 0;
}

/* what holds each space's deletes here (the lowest member, how far behind) and who is STUCK on
 * it; the stats and alerts threads read it under hold_mx */
static struct pc_ack_hold holdv[PC_ACK_HOLD_MAX];
static int holdn;
static pthread_mutex_t hold_mx = PTHREAD_MUTEX_INITIALIZER;
/* this node's own mark of each space and when it last moved: its own
 * progress is "another member's" to the stuck test (clackf_stuck @also) */
static struct { uint64_t oid; uint32_t incarn; uint64_t mark; long long ms; }
	progv[CLACK_SPACES];
static unsigned int progn;
static uint64_t lmembers[PC_CL_MAXPEER];

/* the node id of a member's identity hash: the peer table, then the ids
 * held for those gone (0: not known here) */
static int member_node(uint64_t h)
{
	int k;

	for (k = 0; k < cl_peers()->n_peers; k++) {
		struct peer *p = &cl_peers()->peers[k];
		int node = __atomic_load_n(&p->node, __ATOMIC_ACQUIRE);

		if (node && p->has_ident && fnv1a64(p->ident, sizeof p->ident) == h)
			return node;
	}
	for (k = 0; k < CLRES_MAX; k++)
		if (cl_reserved()->e[k].id && cl_reserved()->e[k].has_ident &&
		    fnv1a64(cl_reserved()->e[k].ident, sizeof cl_reserved()->e[k].ident) == h)
			return cl_reserved()->e[k].id;
	return 0;
}

static long long own_progress(uint64_t oid, uint32_t incarn, uint64_t mark,
		long long now)
{
	unsigned int i;

	for (i = 0; i < progn; i++)
		if (progv[i].oid == oid && progv[i].incarn == incarn) {
			if (progv[i].mark != mark) {
				progv[i].mark = mark;
				progv[i].ms = now;
			}
			return progv[i].ms;
		}
	if (progn < CLACK_SPACES) {
		progv[progn].oid = oid;
		progv[progn].incarn = incarn;
		progv[progn].mark = mark;
		progv[progn++].ms = now;
	}
	return now;
}

/* one space, after its forgetting: who holds it below the head, and who is
 * stuck on it - into hv (PC_ACK_HOLD_MAX of them, the rest uncounted) */
static void acks_hold_note(const struct fsp *sp, uint64_t lo, int nm, int nl,
		long long now, struct pc_ack_hold *hv, int *hn)
{
	struct clackf_stuck sk[1];
	struct pc_ack_hold *h;
	uint64_t head = sp->hi > sp->mark ? sp->hi : sp->mark, f;
	long long also = own_progress(sp->oid, sp->incarn, sp->mark, now);
	int d, k;

	if (nm && (f = pc_ack_fleet_max(sp->oid, sp->incarn, fmembers, nm)) > head)
		head = f;
	if (lo >= head || *hn >= PC_ACK_HOLD_MAX)
		return;                        /* nothing waits, or no room */
	h = &hv[(*hn)++];
	memset(h, 0, sizeof *h);
	h->oid = sp->oid;
	h->incarn = sp->incarn;
	h->through = lo;
	h->head = head;
	if (sp->mark == lo) {
		h->node = cl_node_id();        /* this node is the lowest */
		h->self = 1;
	} else
		for (d = 0; d < nm; d++) {
			uint64_t m = 0;

			(void)pc_ack_fleet_mark(fmembers[d], sp->oid, sp->incarn, &m);
			if (m == lo) {             /* 0: it never announced the space */
				h->node = member_node(fmembers[d]);
				break;
			}
		}
	k = nl ? pc_ack_fleet_stuck(sp->oid, sp->incarn, lmembers, nl, head,
		also, now, cl_acks_stuck_ms(), sk, 1) : 0;
	if (k > 0) {
		h->stuck_node = member_node(sk[0].member);
		h->stuck_mark = sk[0].mark;
		h->stuck_s = (unsigned int)((now - sk[0].moved) / 1000);
	}
}

/* the run that numbered space (oid, incarn) is over: neither this process nor a listed member */
static int space_run_over(uint64_t oid, uint32_t incarn)
{
	int k;

	if (oid == fnv1a64(cl_self_ident(), 16))
		return incarn != pc_cluster_incarnation();
	for (k = 0; k < cl_peers()->n_peers; k++) {
		struct peer *p = &cl_peers()->peers[k];

		if (__atomic_load_n(&p->node, __ATOMIC_ACQUIRE) && p->has_ident &&
		    fnv1a64(p->ident, sizeof p->ident) == oid)
			return p->incarn != incarn;
	}
	return 1;
}

/* a space of a run that is over goes once this node holds all of it, its mark has stood
 * retain_s, no member that reports it is below it and no retained entry of it is left */
static int space_drop_ok(const struct fsp *sp, int nm, long long now,
		long long keep_ms)
{
	unsigned int i;
	int d;

	if (sp->hi > sp->mark || !space_run_over(sp->oid, sp->incarn))
		return 0;
	for (i = 0; i < progn; i++)
		if (progv[i].oid == sp->oid && progv[i].incarn == sp->incarn)
			break;
	if (i == progn || now - progv[i].ms < keep_ms)
		return 0;
	for (d = 0; d < nm; d++) {
		uint64_t m;

		if (pc_ack_fleet_mark(fmembers[d], sp->oid, sp->incarn, &m) &&
		    m < sp->mark)
			return 0;
	}
	if (!clretain_space_drop(sp->oid, sp->incarn))
		return 0;
	progv[i] = progv[--progn];
	return 1;
}

int pc_cluster_acks_holds(struct pc_ack_hold *out, int max)
{
	int n;

	pthread_mutex_lock(&hold_mx);
	n = holdn < max ? holdn : max;
	memcpy(out, holdv, (size_t)n * sizeof *out);
	pthread_mutex_unlock(&hold_mx);
	return n;
}

void acks_forget_tick(unsigned int now_s)
{
	unsigned int i;
	int nm = 0, k;

	long long now = cl_now_ms();
	long long keep_ms = (long long)clretain_retain_s() * 1000;
	struct pc_ack_hold hv[PC_ACK_HOLD_MAX];
	int hn = 0, nl = 0;

	if (!pc_cluster_acks_gate(NULL, 0, NULL)) {
		pthread_mutex_lock(&hold_mx);
		holdn = 0;                     /* the time rule: nothing held by marks */
		pthread_mutex_unlock(&hold_mx);
		return;
	}
	for (k = 0; k < cl_peers()->n_peers && nm < PC_CL_MAXPEER; k++) {
		struct peer *p = &cl_peers()->peers[k];

		if (__atomic_load_n(&p->node, __ATOMIC_ACQUIRE) && p->has_ident) {
			fmembers[nm++] = fnv1a64(p->ident, sizeof p->ident);
			if (clpeers_live(p, now))      /* stuck is a live one's */
				lmembers[nl++] = fmembers[nm - 1];
		}
	}
	for (k = 0; k < CLRES_MAX; k++) {
		const struct clres_ent *e = &cl_reserved()->e[k];
		uint64_t h;
		int d;

		if (!e->id || !e->has_ident || e->why == CLRES_GONE_FORGOTTEN ||
		    now - (e->until_ms - cl_reserve_ms()) >= keep_ms)
			continue;
		h = fnv1a64(e->ident, sizeof e->ident);
		for (d = 0; d < nm && fmembers[d] != h; d++)
			;
		if (d == nm)
			fmembers[nm++] = h;
	}
	fspn = 0;
	pc_ack_each(fsp_cb, NULL);         /* collected: no lock held below */
	for (i = 0; i < fspn; i++) {
		uint64_t lo = fspv[i].mark;    /* this node's own confirmed */

		if (nm) {
			uint64_t f = pc_ack_fleet_min(fspv[i].oid, fspv[i].incarn,
				fmembers, nm);

			if (f < lo)
				lo = f;
		}
		if (lo)
			cl_st_tomb_forgotten(clretain_forget(fspv[i].oid,
				fspv[i].incarn, lo, now_s));
		acks_hold_note(&fspv[i], lo, nm, nl, now, hv, &hn);
	}
	if (now_s % 10 == 0) {
		unsigned int nd = 0;

		for (i = 0; i < fspn; i++)
			if (space_drop_ok(&fspv[i], nm, now, keep_ms)) {
				pc_ack_drop(fspv[i].oid, fspv[i].incarn);
				nd++;
			}
		if (nd)
			LM_NOTICE("cluster: %u numbering space(s) of runs that are over "
				"forgotten - held whole, confirmed by every member that "
				"reports them, no delete of them retained\n", nd);
	}
	clretain_expire_expiries(now_s, PC_TOMB_EXPIRY_HOLD_S);
	pthread_mutex_lock(&hold_mx);
	memcpy(holdv, hv, (size_t)hn * sizeof hv[0]);
	holdn = hn;
	pthread_mutex_unlock(&hold_mx);
}

/* a hole in an origin's numbering holds this node's mark below it: asked for once it stood
 * GAP_WAIT_MS (datagrams overtake), of the origin if live in that incarnation, else of all */
#define GAP_WAIT_MS   500
#define GAP_RETRY_MS  1000
#define GAP_SPAN_MAX  4096u                /* seqs a request asks for */
#define GAP_SEND_MAX  4096u                /* deletes an answer re-sends */
#define GAP_DEAD_MS   30000                /* asked of the peers this long */
struct gapst { uint64_t oid; uint32_t incarn; uint64_t from;
	long long seen_ms, asked_ms, over_ms; int tries; };
static struct gapst gapv[64];
static struct pc_ack_hole holev[64];
/* an origin's own announced mark of its own space - what it has
 * confirmed numbering, so a member below it is missing the tail */
struct ownmark { uint64_t oid; uint32_t incarn; uint64_t mark; };
static struct ownmark ownv[64];
static int ownn;

static int own_cb(uint64_t member, uint32_t incarn, uint64_t oid,
		uint32_t sincarn, uint64_t mark, void *ctx)
{
	(void)incarn; (void)ctx;
	if (member == oid && ownn < 64) {      /* the origin about itself */
		ownv[ownn].oid = oid;
		ownv[ownn].incarn = sincarn;
		ownv[ownn++].mark = mark;
	}
	return 0;
}

/* did this process start with nothing of its own - no snapshot, no
 * record replayed - so that what it holds came from the fleet */
static int came_back_empty(void)
{
	return !pc_recovered_records() && !pc_recovered_snapshot_wall();
}

/* a run of an origin that is over - it restarted, or it is this node's
 * own last.  Only that run could floor a mark, so only this node can now */
static int gap_run_over(const struct pc_ack_hole *h, long long now,
		long long *death)
{
	int k;

	*death = LLONG_MIN;
	if (h->oid == fnv1a64(cl_self_ident(), 16))
		return h->incarn != pc_cluster_incarnation();
	for (k = 0; k < cl_peers()->n_peers; k++) {
		struct peer *p = &cl_peers()->peers[k];

		if (__atomic_load_n(&p->node, __ATOMIC_ACQUIRE) && p->has_ident &&
		    clpeers_live(p, now) &&
		    fnv1a64(p->ident, sizeof p->ident) == h->oid) {
			if (p->boot_ms)
				*death = p->boot_ms + cl_witness_grace_ms() + 1;
			return p->incarn != h->incarn;
		}
	}
	return 0;
}

static void gap_send(const pc_addr *to, const struct pc_ack_hole *h)
{
	unsigned char m[M_GAP_REQ_LEN];
	struct pc_wcur c;
	uint64_t to_seq = h->to - h->from >= GAP_SPAN_MAX ?
		h->from + GAP_SPAN_MAX - 1 : h->to;

	pc_wcur_init(&c, m, sizeof m);
	pc_w8(&c, M_GAP_REQ);
	pc_w64(&c, h->oid);
	pc_w32(&c, h->incarn);
	pc_w64(&c, h->from);
	pc_w64(&c, to_seq);
	if (c.ok && clsend_seal(to, m, sizeof m) == 0)
		cl_st_gap_asked();
}

void acks_gap_tick(long long now)
{
	int nh, i, j, k, sent, timed;

	clretain_own_space(fnv1a64(cl_self_ident(), 16), pc_cluster_incarnation());
	if (!cl_enabled() || cl_self_no_acks() || !pc_ack_enabled())
		return;
	timed = !pc_cluster_acks_gate(NULL, 0, NULL);
	nh = pc_ack_holes(holev, 64);
	/* the tails: a space whose origin announces more than is held here, or one never heard of, is
	 * asked for from just past what is held; the origin's mark is its own, so it holds them */
	ownn = 0;
	pc_ack_fleet_each(own_cb, NULL);
	for (j = 0; j < ownn && nh < 64; j++) {
		uint64_t applied = 0, hi = 0;
		int known = pc_ack_known(ownv[j].oid, ownv[j].incarn, &applied, &hi);

		if (!known && came_back_empty())
			continue;              /* its floors are its sources' (boot) */
		if (known && hi > applied)
			continue;              /* an inner hole first (above) */
		if (ownv[j].mark <= applied)
			continue;
		holev[nh].oid = ownv[j].oid;
		holev[nh].incarn = ownv[j].incarn;
		holev[nh].from = applied + 1;
		holev[nh++].to = ownv[j].mark;
	}
	for (i = 0; i < nh; i++) {
		struct pc_ack_hole *h = &holev[i];
		struct gapst *g = NULL;

		for (j = 0; j < 64; j++)
			if (gapv[j].seen_ms && gapv[j].oid == h->oid &&
			    gapv[j].incarn == h->incarn) {
				g = &gapv[j];
				break;
			}
		if (!g || g->from != h->from) {    /* a new hole, or it moved on */
			if (!g)
				for (j = 0; j < 64 && !g; j++)
					if (!gapv[j].seen_ms ||
					    now - gapv[j].seen_ms > 60000)
						g = &gapv[j];
			if (!g)
				continue;
			g->oid = h->oid;
			g->incarn = h->incarn;
			g->from = h->from;
			g->seen_ms = now;
			g->asked_ms = 0;
			g->over_ms = 0;
			g->tries = 0;
			continue;
		}
		/* a member not on yes: the fleet keeps the time rule, where nobody asks for a lost
		 * delete or rebuilds for it, so the mark goes past the hole */
		if (timed) {
			if (now - g->seen_ms >= GAP_WAIT_MS &&
			    pc_ack_floor(h->oid, h->incarn, h->to) == CLACK_NEW)
				cl_st_acks_floors();
			continue;
		}
		{
			long long death;

			if (!gap_run_over(h, now, &death))
				g->over_ms = 0;
			else if (!g->over_ms)
				g->over_ms = now;
			else if (now - g->over_ms >= GAP_DEAD_MS) {
				gap_floor_over(h, death);
				continue;
			}
		}
		/* a hole that does not move waits longer each time - 1, 2,
		 * 4 ... 32 s: what nobody holds any more is asked for rarely */
		if (now - g->seen_ms < GAP_WAIT_MS ||
		    now - g->asked_ms < (long long)GAP_RETRY_MS << (g->tries < 5 ? g->tries : 5))
			continue;
		g->asked_ms = now;
		g->tries++;
		if (h->from > 1 && h->to >= h->from) {
			uint64_t applied = 0, hi = 0;

			if (pc_ack_known(h->oid, h->incarn, &applied, &hi) &&
			    h->from > hi)
				cl_st_gap_tail();
		}
		/* the origin, live in that incarnation */
		for (k = 0, sent = 0; k < cl_peers()->n_peers && !sent; k++) {
			struct peer *p = &cl_peers()->peers[k];

			if (__atomic_load_n(&p->node, __ATOMIC_ACQUIRE) &&
			    p->has_ident && p->incarn == h->incarn &&
			    clpeers_live(p, now) &&
			    fnv1a64(p->ident, sizeof p->ident) == h->oid) {
				gap_send(&p->addr, h);
				sent = 1;
			}
		}
		if (sent)
			continue;
		/* gone, restarted, or this node's own old space: whoever
		 * still holds it */
		for (k = 0; k < cl_peers()->n_peers; k++) {
			struct peer *p = &cl_peers()->peers[k];

			if (__atomic_load_n(&p->node, __ATOMIC_ACQUIRE) &&
			    p->acks && clpeers_live(p, now))
				gap_send(&p->addr, h);
		}
	}
}

struct gap_rpl { const struct peer *p; uint64_t oid; uint32_t incarn; };

static void gap_rpl_cb(const char *col, size_t cn, const char *key,
		size_t kn, uint64_t ver, uint64_t seq, void *ctx)
{
	unsigned char msg[CLFWD_TOMB_MAX];
	struct gap_rpl *g = ctx;
	struct clfwd_key fk = {
		.col = col, .collen = (unsigned int)cn,
		.key = key, .klen = (unsigned int)kn };
	struct clfwd_tomb_num num = { g->oid, g->incarn, seq };
	size_t n = clfwd_tomb_build_n(msg, M_TOMBSTONE, &fk, ver, 0, &num);

	if (n && clsend_seal(&g->p->addr, msg, n) == 0)
		cl_st_gap_resent();
}

static void held_min_cb(const char *col, size_t cn, const char *key,
		size_t kn, uint64_t ver, uint64_t seq, void *ctx)
{
	uint64_t *m = ctx;

	(void)col; (void)cn; (void)key; (void)kn; (void)ver;
	if (!*m || seq < *m)
		*m = seq;
}

/* the origin's own space: a seq it numbered and no longer holds is gone for good, so the floor is
 * just below the first held in [lo, hi], or @hi if none; 0 = nothing to floor */
static uint64_t gap_floor_at(uint64_t oid, uint32_t incarn, uint64_t lo,
		uint64_t hi)
{
	uint64_t first = 0;

	(void)clretain_space_each(oid, incarn, lo, hi, GAP_SEND_MAX, held_min_cb,
		&first);
	if (first == lo)
		return 0;
	return first ? first - 1 : hi;
}

/* @lost: a seq floored was lost, not taken over by a newer numbered delete still held - rebuild */
static void gap_floor_send(const pc_addr *to, uint64_t oid, uint32_t incarn,
		uint64_t floor, int node, int lost)
{
	unsigned char m[M_GAP_FLOOR_LEN];
	struct pc_wcur c;

	pc_wcur_init(&c, m, sizeof m);
	pc_w8(&c, M_GAP_FLOOR);
	pc_w64(&c, oid);
	pc_w32(&c, incarn);
	pc_w64(&c, floor);
	if (!c.ok || clsend_seal(to, m, sizeof m) != 0)
		return;
	cl_st_gap_floored();
	if (pc_logrl_ok(LRL_GAP_FLOOR, to))
		LM_NOTICE("cluster: node %d asked for deletes this node no longer "
			"holds (up to %llu) - told to floor there%s\n", node,
			(unsigned long long)floor, lost ? ", and asked to rebuild" :
			"; newer deletes of the same keys took them over, so no "
			"rebuild");
	if (lost)
		rebuild_send(to, M_REBUILD_ASK, M_REBUILD_FLOOR);
}

void handle_gap_req(const unsigned char *pt, size_t n,
		const pc_addr *from)
{
	struct peer *p = clpeers_by_addr(cl_peers(), from);
	struct pc_rcur c;
	struct gap_rpl g;
	uint64_t lo, hi;

	if (!p || !__atomic_load_n(&p->node, __ATOMIC_ACQUIRE) ||
	    n < M_GAP_REQ_LEN)
		return;
	pc_rcur_init(&c, pt, n);
	(void)pc_ru8(&c);
	g.p = p;
	g.oid = pc_ru64(&c);
	g.incarn = pc_ru32(&c);
	lo = pc_ru64(&c);
	hi = pc_ru64(&c);
	if (!lo || hi < lo || hi - lo >= GAP_SPAN_MAX)
		return;
	if (g.oid == fnv1a64(cl_self_ident(), 16) &&
	    g.incarn == pc_cluster_incarnation()) {
		uint64_t floor = gap_floor_at(g.oid, g.incarn, lo, hi);

		if (floor >= lo) {             /* it starts below what is held */
			/* a loss the table cannot rule out asks, unless every seq floored was superseded */
			gap_floor_send(from, g.oid, g.incarn, floor, p->node,
				lo <= clretain_space_gone(g.oid, g.incarn) &&
				!clretain_superseded(g.oid, g.incarn, lo, floor));
			if (floor >= hi)
				return;
			lo = floor + 1;
		}
	} else if (cl_self_gap_no_copies()) {
		return;
	}
	cl_st_gap_served();
	(void)clretain_space_each(g.oid, g.incarn, lo, hi, GAP_SEND_MAX,
		gap_rpl_cb, &g);
}

/* the member floors its mark where the origin's retained deletes now
 * begin - only the origin may say so of its own space */
void handle_gap_floor(const unsigned char *pt, size_t n,
		const pc_addr *from)
{
	struct peer *p = clpeers_by_addr(cl_peers(), from);
	struct pc_rcur c;
	uint64_t oid, floor;
	uint32_t incarn;

	if (!p || !__atomic_load_n(&p->node, __ATOMIC_ACQUIRE) ||
	    n < M_GAP_FLOOR_LEN || !p->has_ident || !pc_ack_enabled() ||
	    cl_self_no_acks())
		return;
	pc_rcur_init(&c, pt, n);
	(void)pc_ru8(&c);
	oid = pc_ru64(&c);
	incarn = pc_ru32(&c);
	floor = pc_ru64(&c);
	if (oid != fnv1a64(p->ident, sizeof p->ident) || incarn != p->incarn)
		return;
	if (pc_ack_floor(oid, incarn, floor) == CLACK_NEW) {
		cl_st_acks_floors();
		LM_NOTICE("cluster: node %d no longer holds its deletes up to %llu "
			"- this node's mark floored there (a rebuild follows if "
			"any was lost rather than taken over)\n", p->node,
			(unsigned long long)floor);
	}
}

/* the backstop: a member missing deletes no longer held here (time rule, cap) must be rebuilt and
 * TOLD - a timer never forgets silently for a member that has not confirmed */
struct gone_ctx { uint64_t member; int owed; };

static int gone_cb(uint64_t oid, uint32_t incarn, uint64_t gone_hi, void *ctx)
{
	struct gone_ctx *g = ctx;
	uint64_t m = 0;

	if (!gone_hi)
		return 0;
	if (!pc_ack_fleet_mark(g->member, oid, incarn, &m) || m < gone_hi) {
		g->owed = 1;
		return 1;
	}
	return 0;
}

void rebuild_send(const pc_addr *to, int kind, long long age_ms)
{
	unsigned char m[M_REBUILD_LEN];
	struct pc_wcur c;

	pc_wcur_init(&c, m, sizeof m);
	pc_w8(&c, M_REBUILD);
	pc_w8(&c, (unsigned char)kind);
	pc_w32(&c, age_ms > 0xffffffffLL ? 0xffffffffu : (uint32_t)age_ms);
	if (c.ok && clsend_seal(to, m, sizeof m) == 0 && kind == M_REBUILD_ASK)
		cl_st_rebuild_sent();
}

/* the member's first keepalive after @now - @was ms of silence, its marks still those from
 * before: asked to rebuild only when something it missed is no longer held here */
void acks_back_check(struct peer *p, uint64_t member,
		long long was, long long now)
{
	long long away_s = (now - was) / 1000;
	struct gone_ctx g = { member, 0 };

	clretain_gone_each(gone_cb, &g);
	if (!g.owed && !clretain_lost_since((unsigned int)(was / 1000)))
		return;                        /* everything it missed is held */
	p->rb_state = 1;
	p->rb_from_ms = was;
	LM_WARN("cluster: node %d is back after %lld s - this node no longer "
		"holds every delete made meanwhile (%s); asking it to rebuild: "
		"reconcile what it stored before then with the fleet\n",
		p->node, away_s, g.owed ? "some past its marks were forgotten or "
		"dropped" : "some noted since were dropped by the time rule or "
		"the cap");
	/* and to the alerts, with what happens now */
	pthread_mutex_lock(&tomb_gap_mx);
	snprintf(tomb_gap_text, sizeof tomb_gap_text, "node %d back after "
		"%lld s - deletes made meanwhile are no longer all held here, "
		"so it was asked to rebuild: it reconciles what it stored "
		"before the absence with the fleet", p->node, away_s);
	tomb_gaps++;
	pthread_mutex_unlock(&tomb_gap_mx);
	/* now, not at the next beat: its sweeps and pull answers count from this keepalive on, and
	 * can carry a key whose delete nobody kept */
	rebuild_send(&p->addr, M_REBUILD_ASK, now - was);
	p->rb_sent_ms = now;
	p->rb_tries = 1;
}

void acks_rebuild_tick(long long now)
{
	int i;

	for (i = 0; i < cl_peers()->n_peers; i++) {
		struct peer *p = &cl_peers()->peers[i];

		if (p->rb_state != 1 || !__atomic_load_n(&p->node, __ATOMIC_ACQUIRE) ||
		    !clpeers_live(p, now) || now - p->rb_sent_ms < 2000)
			continue;
		if (p->rb_tries >= 5) {
			p->rb_state = 0;
			LM_WARN("cluster: node %d did not take the rebuild it was "
				"asked for 5 times - it may keep deletes it missed; "
				"restart it to make it reconcile\n", p->node);
			continue;
		}
		/* the age as of this send: the asked node takes its boundary
		 * from its own clock, now minus this */
		rebuild_send(&p->addr, M_REBUILD_ASK, now - p->rb_from_ms);
		p->rb_sent_ms = now;
		p->rb_tries++;
	}
}

void tomb_catchup_tick(void)
{
	static unsigned int catchup_last_s;
	long long now = cl_now_ms();
	unsigned int now_s = (unsigned int)(now / 1000), wmin;
	struct clretain_stats rs;
	int i;

	if (!clretain_enabled())
		return;
	clretain_expire(now_s);
	if (cl_self_gap_no_copies())
		return;                        /* no copies by this road either */
	catchup_by_marks = pc_cluster_acks_gate(NULL, 0, NULL);
	clretain_get_stats(&rs, now_s);
	/* a window never under twice the time between ticks: the cursor gains on the clock, or one
	 * behind a faster planting rate trails at the horizon, replaying each entry as it ages out */
	wmin = catchup_last_s && now_s > catchup_last_s ?
		2 * (now_s - catchup_last_s) : 20;
	catchup_last_s = now_s;
	for (i = 0; i < cl_peers()->n_peers; i++) {
		struct peer *p = &cl_peers()->peers[i];
		unsigned int horizon, upto, sent, win;

		if (!__atomic_load_n(&p->node, __ATOMIC_ACQUIRE) ||
		    !clpeers_live(p, now))
			continue;              /* silent: the gap is noted on return */
		horizon = now_s > rs.retain_s ? now_s - rs.retain_s : 0;
		if (!p->tomb_owed || p->tomb_gap_from) {
			if (p->tomb_gap_from) {
				/* it went quiet at gap_from: what was deleted
				 * since may never have reached it */
				unsigned int away = now_s - p->tomb_gap_from;

				/* the table vouches for the gap when it holds every delete
				 * made since the peer went quiet (the fast reconcile's test) */
				unsigned int whole = clretain_complete_since(now_s);

				/* a confirming member of an eager fleet is asked to
				 * rebuild instead (acks_back_check raises the alert) */
				if (whole > p->tomb_gap_from &&
				    !(p->acks && pc_cluster_eager() && pc_ack_enabled())) {
					int past = away > rs.retain_s;
					char why[160];

					if (past)
						snprintf(why, sizeof why, "past "
							"tombstone_retain_s %u s", rs.retain_s);
					else
						snprintf(why, sizeof why, "and the cap "
							"(tombstone_retain_max %u) dropped "
							"deletes made while it was away", rs.max);
					LM_WARN("cluster: node %d is back after %u s, %s: "
						"a delete made while it was away may come "
						"back; re-issue deletes that matter\n",
						p->node, away, why);
					/* and to the alerts */
					pthread_mutex_lock(&tomb_gap_mx);
					snprintf(tomb_gap_text, sizeof tomb_gap_text,
						"node %d back after %u s, %s - deletes made "
						"meanwhile may come back; re-issue the ones "
						"that matter", p->node, away, why);
					tomb_gaps++;
					pthread_mutex_unlock(&tomb_gap_mx);
				}
				LM_NOTICE("cluster: node %d is back after %u s of "
					"silence - replaying the deletes it may have "
					"missed (%llu retained)\n", p->node, away,
					rs.entries);
				/* a silence while a replay is owed joins it: that replay covers
				 * what is past its cursor, so the cursor only ever moves back */
				if (!p->tomb_owed || p->tomb_gap_from < p->tomb_mark)
					p->tomb_mark = p->tomb_gap_from;
				p->tomb_gap_from = 0;
			} else if (!p->tomb_mark) {
				/* never caught up before: everything retained */
				p->tomb_mark = horizon;
			} else {
				/* heard all along: the broadcasts reached it */
				p->tomb_mark = now_s;
				p->tomb_drop_mark = clretain_dropped_early();
				continue;
			}
			if (!p->tomb_owed) {
				p->tomb_sent = 0;
				p->tomb_win = 60;
			}
			p->tomb_owed = 1;
		}
		if (p->tomb_mark < horizon)
			p->tomb_mark = horizon;    /* nothing older is held */
		win = p->tomb_win ? p->tomb_win : 60;
		upto = p->tomb_mark + win < now_s ? p->tomb_mark + win : now_s;
		sent = clretain_each(p->tomb_mark, upto, tomb_catchup_cb, p);
		p->tomb_mark = upto;
		/* a few thousand a tick: halve the window past 4,000, double it
		 * under 1,000 */
		if (sent > catchup_burst() && win / 2 >= wmin)
			p->tomb_win = win / 2;
		else if (sent < catchup_burst() / 4 && win < 3600)
			p->tomb_win = win * 2;
		if (p->tomb_win < wmin)
			p->tomb_win = wmin;
		if (upto >= now_s) {
			p->tomb_owed = 0;
			p->tomb_drop_mark = clretain_dropped_early();
			if (p->tomb_sent)
				LM_NOTICE("cluster: node %d caught up - %llu delete(s) "
					"replayed\n", p->node, p->tomb_sent);
		}
	}
}

unsigned long long pc_cluster_tomb_catchup_sent(void)
{
	return tomb_catchup_total;
}
