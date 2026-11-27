/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clwork.c - the cluster entry points that run on a WORKER thread, never the peer thread;
 * cluster state is read only through clstate.h's accessors, never `C` */
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cluster.h"
#include "clpeers.h"
#include "clplace.h"
#include "clstate.h"
#include "clpend.h"                  /* struct pending */
#include "clsend.h"                  /* the send path */
#include "clpull.h"                  /* clpull_req_frame */
#include "upstream.h"                /* a through key's pull waits for the owner's fetch */
#include "clfwd.h"                   /* clfwd_op_frame, clfwd_json_build */
#include "clmsg.h"                   /* M_FWD_JSON, M_REPL_MANY */
#include "clmig.h"                   /* the record and group headers */
#include "clpush.h"                  /* the per-worker push groups */
#include "pc_slot.h"                 /* pc_key_slot */
#include "daemon.h"                  /* pc_worker_id */
#include "compat/timer.h"            /* get_ticks */
#include "pc_mix.h"
#include "clrp.h"

#define SELF_BAND_PCT 25

/* per-thread slot -> owner cache (pc_clplace_owner is a rendezvous over every node), valid for
 * one map epoch (C.map is replaced whole): kept only if the epoch read the same before and after */
static __thread uint32_t slc_term, slc_seq;
static __thread uint16_t slc_owner[PC_SLOTS];
static __thread uint8_t slc_have[PC_SLOTS / 8];

/* the map is rewritten in place (cl_map_gen): a read counts only if the generation was even before
 * and the same after; a few tries, then 0 and the caller's fallback */
#define MAP_READ_TRIES 8

static int map_read_ok(unsigned int g)
{
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	return !(g & 1) && cl_map_gen() == g;
}

static uint16_t slot_map_owner(const struct pc_clmap *m, unsigned slot)
{
	int tries;

	for (tries = 0; tries < MAP_READ_TRIES; tries++) {
		unsigned int g = cl_map_gen();
		uint32_t t, s;
		uint16_t o;

		if (g & 1)
			continue;
		t = m->term;
		s = m->seq;
		if (!map_read_ok(g))
			continue;
		if (t != slc_term || s != slc_seq) {
			memset(slc_have, 0, sizeof slc_have);
			slc_term = t;
			slc_seq = s;
		}
		if (slc_have[slot >> 3] & (1u << (slot & 7)))
			return slc_owner[slot];
		o = pc_clplace_owner(m, slot);
		if (!map_read_ok(g))
			continue;
		if (o) {
			slc_owner[slot] = o;
			slc_have[slot >> 3] |= (uint8_t)(1u << (slot & 7));
		}
		return o;
	}
	return 0;
}

/* placement hashes the SLOT, not the key, so a RESP client reaches the same owner (and a key
 * shares one owner across collections); 0 = self, as the map names self like any node */
int pc_shard_owner_slot(unsigned slot)
{
	uint64_t kh = slot, best, h;
	long long now;
	int i, owner = 0;
	struct clpeers *pt;
	pc_addr self;

	if (!cl_enabled())
		return 0;
	if (cl_map_usable()) {
		uint16_t o = slot_map_owner(cl_map(), slot);

		if (o) {
			cl_place_map_hit();
			return o == (uint16_t)cl_node_id() ? 0 : (int)o;
		}
	}
	/* only the fallback needs the clock and the peers */
	now = cl_now_ms();
	pt = cl_peers();
	self = cl_self_addr();
	cl_place_hrw_hit();
	/* a node that is not READY owns nothing, here as in the map: seeding 0 lets any live peer
	 * outrank us, and with none live 0 still reads as self (there is nowhere else to go) */
	best = pc_node_state() == PC_NST_READY
		? pc_hrw_mix(kh, self.in4.sin_addr.s_addr,
			self.in4.sin_port)
		: 0;
	for (i = 0; i < pt->n_peers; i++) {
		/* a FAILED peer owns nothing; a stall, a join, a heal or a resize keeps its rank */
		if (!clpeers_usable(&pt->peers[i], now) ||
		    pt->peers[i].nstate == PC_NST_FAILED)
			continue;
		h = pc_hrw_mix(kh, pt->peers[i].addr.in4.sin_addr.s_addr,
			pt->peers[i].addr.in4.sin_port);
		if (h > best) {
			best = h;
			owner = pt->peers[i].node;
		}
	}
	return owner;
}

/* rank = how many candidates beat @node_id, so the count stops at @k: nothing sorted, as this
 * runs once per peer per record on the write path */
int pc_spread_in_set(unsigned slot, int node_id, int k)
{
	uint64_t kh = slot, mine, h;
	long long now = cl_now_ms();
	struct clpeers *pt = cl_peers();
	pc_addr self = cl_self_addr();
	int i, ahead = 0, self_id = cl_node_id();
	int me = node_id == 0 || node_id == self_id;

	if (!cl_enabled() || k <= 0)
		return 0;
	/* the map is used only when it lists every node seen live: a false negative here makes a
	 * real holder DELETE the record, while HRW costs at most a brief disagreement */
	if (cl_map_usable()) {
		const struct pc_clmap *m = cl_map();
		int seen = 1;                  /* ourselves */

		for (i = 0; i < pt->n_peers; i++)
			if (clpeers_holds_slots(&pt->peers[i], now))
				seen++;
		if (m) {
			uint16_t want = (uint16_t)(me ? self_id : node_id);
			int tries;

			for (tries = 0; tries < MAP_READ_TRIES; tries++) {
				unsigned int g = cl_map_gen();
				int r, use;

				if (g & 1)
					continue;
				use = (int)m->nnodes >= seen;
				r = use ? pc_clplace_in_set(m, kh, want, k) : 0;
				if (!map_read_ok(g))
					continue;
				if (!use)
					break;
				cl_place_map_hit();
				return r;
			}
		}
	}
	cl_place_hrw_hit();
	if (me) {
		/* the same exclusion shard's fallback makes: a node that is
		 * not READY owns nothing, and must not hold a replica either */
		if (pc_node_state() != PC_NST_READY)
			return 0;
		mine = pc_hrw_mix(kh, self.in4.sin_addr.s_addr, self.in4.sin_port);
	} else {
		for (i = 0; i < pt->n_peers; i++)
			if (pt->peers[i].node == node_id)
				break;
		/* the GRACE window, not liveness: a peer that is merely
		 * quiet keeps its slots, or every blip reshuffles the set */
		if (i == pt->n_peers || !clpeers_holds_slots(&pt->peers[i], now))
			return 0;              /* unknown or long gone */
		mine = pc_hrw_mix(kh, pt->peers[i].addr.in4.sin_addr.s_addr,
			pt->peers[i].addr.in4.sin_port);
	}
	/* self is a candidate when asking about a peer and every live peer when asking about self:
	 * one candidate set either way keeps the answers consistent */
	if (!me && pc_node_state() == PC_NST_READY) {
		h = pc_hrw_mix(kh, self.in4.sin_addr.s_addr, self.in4.sin_port);
		if (h > mine && ++ahead >= k)
			return 0;
	}
	for (i = 0; i < pt->n_peers; i++) {
		if (pt->peers[i].node == (me ? self_id : node_id))
			continue;
		if (!clpeers_holds_slots(&pt->peers[i], now))
			continue;
		h = pc_hrw_mix(kh, pt->peers[i].addr.in4.sin_addr.s_addr,
			pt->peers[i].addr.in4.sin_port);
		if (h > mine && ++ahead >= k)
			return 0;
	}
	return 1;
}

/* ranked, not first-found: every asker picks the same top holder, so repeated misses for one
 * key converge on one peer; a miss under spread never broadcasts (only K of P hold it) */
int pc_spread_pick_peer(unsigned slot, int k)
{
	return pc_spread_pick_peer_except(slot, k, 0);
}

/* the best-ranked live holder other than @except (0 = none) - the next one to try when a
 * forward to the best could not even be sent */
int pc_spread_pick_peer_except(unsigned slot, int k, int except)
{
	uint64_t kh = slot, best = 0, h;
	long long now = cl_now_ms();
	struct clpeers *pt = cl_peers();
	int i, pick = 0;

	if (!cl_enabled() || k <= 0)
		return 0;
	for (i = 0; i < pt->n_peers; i++) {
		if (!clpeers_usable(&pt->peers[i], now))
			continue;
		if (except && pt->peers[i].node == except)
			continue;
		if (!pc_spread_in_set(slot, pt->peers[i].node, k))
			continue;
		h = pc_hrw_mix(kh, pt->peers[i].addr.in4.sin_addr.s_addr,
			pt->peers[i].addr.in4.sin_port);
		if (!pick || h > best) {
			best = h;
			pick = pt->peers[i].node;
		}
	}
	return pick;
}

/* keeping a surplus beats deleting the last copy we can prove exists: reclaim comes later */
int pc_spread_set_live(unsigned slot, int k)
{
	struct cand { uint64_t h; int live; } c[PC_CL_MAXPEER + 1];
	uint64_t kh = slot;
	long long now = cl_now_ms();
	struct clpeers *pt = cl_peers();
	pc_addr self = cl_self_addr();
	int i, j, n = 0, sel;

	if (!cl_enabled() || k <= 0)
		return 0;
	/* the same map-or-HRW authority as pc_spread_in_set: reclaim asks both before a delete, and
	 * two node sets would let this vouch for nodes that are not the holders */
	if (cl_map_usable()) {
		const struct pc_clmap *m = cl_map();
		int seen = 1;                  /* ourselves */

		for (i = 0; i < pt->n_peers; i++)
			if (clpeers_holds_slots(&pt->peers[i], now))
				seen++;
		if (m) {
			uint16_t top[PC_CL_MAXPEER + 1];
			int got = -2, s, tries, self_id = cl_node_id();

			for (tries = 0; tries < MAP_READ_TRIES && got == -2; tries++) {
				unsigned int g = cl_map_gen();
				int r = -1;

				if (g & 1)
					continue;
				if ((int)m->nnodes >= seen) {
					if (k > (int)(sizeof top / sizeof top[0]))
						return 0;
					r = pc_clplace_rank(m, kh, top, k);
				}
				if (map_read_ok(g))
					got = r;
			}
			if (got == -2)
				return 0;      /* the map never held still: keep the copy */
			if (got >= 0) {
				if (got < k)
					return 0;      /* the map cannot fill the set */
				for (s = 0; s < got; s++) {
					if (top[s] == (uint16_t)self_id)
						continue;   /* ourselves, by definition */
					if (!clpeers_by_id_live(pt, (int)top[s], now))
						return 0;
				}
				return 1;
			}
		}
	}
	if (pc_node_state() == PC_NST_READY) {
		c[n].h = pc_hrw_mix(kh, self.in4.sin_addr.s_addr, self.in4.sin_port);
		c[n].live = 1;                 /* ourselves, by definition */
		n++;
	}
	for (i = 0; i < pt->n_peers && n <= PC_CL_MAXPEER; i++) {
		if (!clpeers_holds_slots(&pt->peers[i], now))
			continue;
		c[n].h = pc_hrw_mix(kh, pt->peers[i].addr.in4.sin_addr.s_addr,
			pt->peers[i].addr.in4.sin_port);
		c[n].live = clpeers_live(&pt->peers[i], now);
		n++;
	}
	if (n < k)
		return 0;                      /* the set is not even complete */
	/* the top k by the same ranking placement uses; every one must be
	 * reachable, or this node's copy is not surplus in any useful sense */
	for (sel = 0; sel < k; sel++) {
		int best = sel;

		for (j = sel + 1; j < n; j++)
			if (c[j].h > c[best].h)
				best = j;
		if (best != sel) {
			struct cand t = c[sel];

			c[sel] = c[best];
			c[best] = t;
		}
		if (!c[sel].live)
			return 0;
	}
	return 1;
}

int pc_place(void)
{
	struct peer *cand[PC_CL_MAXPEER];
	unsigned int myfree = cl_self_free_mb(), best_free;
	int i, n = 0, a, b, best;
	static unsigned int rr;
	struct clpeers *pt = cl_peers();

	for (i = 0; i < pt->n_peers; i++)
		if (clpeers_usable(&pt->peers[i], cl_now_ms()))
			cand[n++] = &pt->peers[i];
	if (!n) {
		cl_px_placed_local();
		return 0;
	}
	/* two samples (round-robin walk keeps it cheap and fair) */
	a = (int)(rr++ % (unsigned int)n);
	b = (int)(rr++ % (unsigned int)n);
	best = cand[a]->free_mb >= cand[b]->free_mb ? a : b;
	best_free = cand[best]->free_mb;
	/* the self-preference band: keep local unless a peer is
	 * MEANINGFULLY freer - symmetric load stays put */
	if (myfree * 100 >= best_free * (100 - SELF_BAND_PCT) ||
	        myfree >= best_free) {
		cl_px_placed_local();
		return 0;
	}
	cl_px_placed_remote();
	return cand[best]->node;
}

/* ---- pull and forward: worker thread, but for pc_pull_begin_kind's reconcile caller ---- */

static __thread uint32_t fwd_target_req;   /* read by pc_fwd_target_of */
static __thread int fwd_target_node;
static __thread unsigned int fwd_target_ttl;

uint32_t pc_pull_begin_at(const char *col, size_t collen, const char *key,
		size_t klen, int holder_node)
{
	unsigned char msg[16 + 256 + 4096];
	struct pending *slot;
	struct peer *pr = clpeers_by_id_live(cl_peers(), holder_node, cl_now_ms());
	uint32_t req;
	size_t n;

	cl_set_fail(PC_CLFAIL_NONE);
	if (!cl_enabled() || !pr || collen > 255 || klen > 4096) {
		cl_set_fail(PC_CLFAIL_ROUTE);
		cl_st_fwd_no_route();
		return 0;
	}
	req = cl_pend_alloc(PC_DONE_PULL, 1, pc_upstream_pull_extra_ms(col, collen),
		&slot);                    /* the owner may be fetching it */
	if (!req)
		return 0;
	if (collen < sizeof slot->col && klen < sizeof slot->key) {
		memcpy(slot->col, col, collen);
		slot->collen = (unsigned char)collen;
		memcpy(slot->key, key, klen);
		slot->klen = (unsigned short)klen;
	}
	n = clpull_req_frame(msg, req, cl_node_id(), col, collen, key, klen);
	clsend_seal(&pr->addr, msg, n);
	cl_st_pull_sent(1);
	fwd_target_req = req;          /* a unicast pull's target */
	fwd_target_node = holder_node;
	fwd_target_ttl = 0;
	return req;
}

/* @kind PC_DONE_PULL for a client's miss, PC_DONE_RECONCILE for the reconcile probe: the
 * request is the same, only the answer's handling differs */
uint32_t pc_pull_begin_kind(const char *col, size_t collen,
		const char *key, size_t klen, int kind)
{
	unsigned char msg[1 + 4 + 2 + 1 + 2 + 256 + 4096];
	struct pending *slot = NULL;
	uint32_t req;
	int up = 0;
	size_t n;

	cl_set_fail(PC_CLFAIL_NONE);
	if (!cl_enabled() || collen > 255 || klen > 4096) {
		cl_set_fail(PC_CLFAIL_ROUTE);
		cl_st_fwd_no_route();
		return 0;
	}
	up = clpeers_count_live(cl_peers(), cl_now_ms());
	if (!up)
		return 0;

	req = cl_pend_alloc(kind, up, 0, &slot);
	if (!req)
		return 0;                      /* saturated: answer local miss */
	if (collen < sizeof slot->col && klen < sizeof slot->key) {
		memcpy(slot->col, col, collen);
		slot->collen = (unsigned char)collen;
		memcpy(slot->key, key, klen);
		slot->klen = (unsigned short)klen;
	}

	n = clpull_req_frame(msg, req, cl_node_id(), col, collen, key, klen);
	(void)clsend_live(cl_peers(), msg, n, cl_now_ms());
	cl_st_pull_sent(1);
	return req;
}

uint32_t pc_pull_begin(const char *col, size_t collen, const char *key,
		size_t klen)
{
	return pc_pull_begin_kind(col, collen, key, klen, PC_DONE_PULL);
}

uint32_t pc_fwd_begin(int node, int op, const char *col, size_t collen,
		const char *key, size_t klen, const char *val, size_t vlen,
		unsigned int ttl_rel, long long delta)
{
	unsigned char msg[32 + 256 + 4096 + PC_MAX_FWD_VAL];
	struct pending *slot;
	struct peer *pr = clpeers_by_id_live(cl_peers(), node, cl_now_ms());
	uint32_t req;
	size_t n;
	int kind = op == 1 ? PC_DONE_FWD_DEL : op == 2 ? PC_DONE_FWD_ADD
		: op == 3 ? PC_DONE_FWD_TOUCH
		: op == 4 || op == 5 || op == 6 || op == 7 ? PC_DONE_FWD_SETCOND
		: op == 8 || op == 9 ? PC_DONE_FWD_DELCOND
		: PC_DONE_FWD_SET;

	cl_set_fail(PC_CLFAIL_NONE);
	if (!cl_enabled() || !pr || collen > 255 || klen > 4096 ||
	        vlen > PC_MAX_FWD_VAL) {
		cl_set_fail(PC_CLFAIL_ROUTE);
		cl_st_fwd_no_route();
		return 0;
	}
	/* forwards get deadline headroom over pulls (a late answer beats a false failure while the
	 * holder stores anyway), plus the master fetch of a through collection's owner */
	req = cl_pend_alloc(kind, 1, 700 + pc_upstream_pull_extra_ms(col, collen),
		&slot);
	if (!req)
		return 0;
	n = clfwd_op_frame(msg, req, cl_node_id(), op, ttl_rel, delta, col,
		collen, key, klen, val, vlen);
	if (clsend_seal(&pr->addr, msg, n) != 0) {
		cl_pend_release(slot);
		cl_px_fwd_fails();
		cl_set_fail(PC_CLFAIL_SEND);
		cl_st_fwd_send_fail();
		cl_st_fwd_send_errno(errno);
		return 0;
	}
	cl_px_fwd_sent();
	fwd_target_req = req;
	fwd_target_node = node;
	fwd_target_ttl = ttl_rel;
	return req;
}

int pc_fwd_target_of(uint32_t req, unsigned int *ttl)
{
	if (!req || req != fwd_target_req)
		return 0;
	if (ttl)
		*ttl = fwd_target_ttl;
	return fwd_target_node;
}

/* a broadcast pull whose pending slot carries the deferred write; handle_pull_rsp turns a
 * positive into a forward under the same req, so its ack completes the client's park */
uint32_t pc_probe_fwd_begin(int op, const char *col, size_t collen,
		const char *key, size_t klen, const char *val, size_t vlen,
		unsigned int ttl_rel, long long by)
{
	unsigned char msg[1 + 4 + 2 + 1 + 2 + 256 + 4096];
	struct pending *slot = NULL;
	char *stash = NULL, *klong = NULL;
	uint32_t req;
	int up;
	size_t n;

	cl_set_fail(PC_CLFAIL_NONE);           /* BUSY only when the table refuses the slot */
	/* col/key must fit the slot: the transform re-sends them */
	if (!cl_enabled() || collen >= sizeof slot->col || klen > CLPEND_KEY_MAX ||
	        vlen > PC_MAX_FWD_VAL)
		return 0;
	up = clpeers_count_live(cl_peers(), cl_now_ms());
	if (!up)
		return 0;
	if (vlen) {
		stash = malloc(vlen);
		if (!stash)
			return 0;
		memcpy(stash, val, vlen);
	}
	if (klen >= sizeof slot->key) {
		klong = malloc(klen);
		if (!klong) {
			free(stash);
			return 0;
		}
		memcpy(klong, key, klen);
	}
	req = cl_pend_alloc(PC_DONE_PULL, up, 700, &slot);
	if (!req) {
		free(stash);
		free(klong);
		return 0;
	}
	/* fields land BEFORE the datagrams leave: no answer can race them */
	memcpy(slot->col, col, collen);
	slot->collen = (unsigned char)collen;
	if (klong)
		slot->key_long = klong;    /* the slot's release frees it */
	else
		memcpy(slot->key, key, klen);
	slot->klen = (unsigned short)klen;
	slot->probe_op = op == 2 ? 2 : op == 3 ? 3 : op == 1 ? 4 : 1;
	slot->stash = stash;
	slot->stash_len = (int)vlen;
	slot->by = by;
	slot->ttl_rel = ttl_rel;

	n = clpull_req_frame(msg, req, cl_node_id(), col, collen, key, klen);
	(void)clsend_live(cl_peers(), msg, n, cl_now_ms());
	cl_st_pull_sent(1);
	return req;
}

uint32_t pc_fwd_json_begin(int node, int jop, const char *col,
		size_t collen, const char *key, size_t klen,
		const char *path, size_t plen, const char *val, size_t vlen,
		long long by, int have_ttl, long long ttl, int nx, int xx,
		int mkpath)
{
	unsigned char msg[64 + 256 + 4096 + 512 + PC_MAX_FWD_VAL];
	struct pending *slot;
	struct peer *pr = clpeers_by_id_live(cl_peers(), node, cl_now_ms());
	uint32_t req;
	size_t n;

	if (!cl_enabled() || !pr || collen > 255 || klen > 4096 || plen > 511 ||
	        vlen > PC_MAX_FWD_VAL)
		return 0;
	req = cl_pend_alloc(PC_DONE_JSON, 1, 700, &slot);
	if (!req)
		return 0;
	slot->jop = (unsigned char)jop;
	{
		struct clfwd_json j = {
			.req = req, .node = (unsigned int)cl_node_id(),
			.jop = (unsigned char)jop,
			.flags = (unsigned char)(
				(nx ? CLFWD_J_NX : 0) | (xx ? CLFWD_J_XX : 0) |
				(mkpath ? CLFWD_J_MKPATH : 0) |
				(have_ttl ? CLFWD_J_HAVETTL : 0)),
			.ttl = (unsigned int)(ttl > 0 ? ttl : 0),
			.by = by,
			.col = col, .collen = (unsigned int)collen,
			.key = key, .klen = (unsigned int)klen,
			.path = path, .plen = (unsigned int)plen,
			.val = val, .vlen = (unsigned int)vlen,
		};

		n = clfwd_json_build(msg, M_FWD_JSON, &j);
	}
	clsend_seal(&pr->addr, msg, n);
	return req;
}

/* ---- the write-path push: worker thread, each worker owning its own push groups ---- */

/* clpush's send callback: clpush decides WHEN, this seals and sends, as the datagram is
 * clwire's and the counters the cluster's */
void cl_wgroup_send_cb(struct wgroup *g, void *ctx)
{
	uint32_t req = 0;

	(void)ctx;
	if (!g->qcount)
		return;
	/* a group carrying writes taken as a spread NON-holder goes WITH a request id, so the
	 * holder's ack lets those copies go; no free slot: sent unacked, kept for reclaim */
	if (g->nhcount) {
		unsigned int nhc = g->nhcount;

		req = clpush_ack_park(cl_push(), g, cl_now_ms());
		cl_px_spread_ack(req ? nhc : 0, req ? 0 : nhc);
	}
	clmig_group_hdr(g->q, CLMIG_GHDR, M_REPL_MANY, req, cl_node_id(),
		g->qcount);
	if (req)
		clsend_seal(&g->to, g->q, g->qlen);
	else
		(void)clrp_send(&g->to, g->q, g->qlen);
	cl_px_repl_pushed(g->qcount);
	cl_px_repl_groups();
	clpush_sent(cl_push(), g);
}

int pc_repl_push(const char *col, size_t collen, const char *key,
		size_t klen, const char *val, size_t vlen, unsigned int exp,
		unsigned long long ver, unsigned char type)
{
	unsigned char rec[CLMIG_RHDR + 255 + 4096 + PC_MAX_FWD_VAL + 8];
	unsigned int now = get_ticks(), ttl_left = 0;
	long long nowms;
	size_t n;
	int i, kk, held, noted = 0, skipped = 0;
	unsigned slot;

	if (!cl_enabled() || collen == 0 || collen > 255 || klen == 0 ||
	        klen > 4096)
		return 1;          /* no plane: keeping it is all we have */
	if (vlen > PC_MAX_FWD_VAL) {
		cl_px_migrate_skipped_big();    /* the same ceiling the sweep has */
		return 1;          /* unreplicable: dropping it would LOSE it */
	}
	if (exp) {
		if (exp <= now)
			return 1;                  /* already gone */
		ttl_left = exp - now;
	}
	{
		struct clmig_rec rr = {
			.ttl_left = ttl_left,
			.col = col, .collen = (unsigned int)collen,
			.key = key, .klen = (unsigned int)klen,
			.val = val, .vlen = (unsigned int)vlen,
			.ver = ver,
			.flags = type
		};

		clmig_rec_write(rec, &rr);
	}
	n = clmig_rec_size((unsigned int)collen, (unsigned int)klen,
		(unsigned int)vlen);
	nowms = cl_now_ms();
	/* under spread a record goes to its K holders only, so per-node apply falls as the fleet
	 * grows (eager sends to every peer); kk = 0 is any other mode */
	kk = pc_cluster_replicas();
	slot = kk ? pc_key_slot(key, klen) : 0;
	held = !kk || pc_spread_in_set(slot, 0, kk);
	for (i = 0; i < cl_peers()->n_peers; i++) {
		struct peer *p = &cl_peers()->peers[i];

		if (!clpeers_usable(p, nowms)) {   /* no replication to a CONNECTING one */
			/* placed there all the same: it is owed the copy once it is placeable */
			if (kk && clpeers_live(p, nowms) && pc_spread_in_set(slot, p->node, kk)) {
				__atomic_store_n(&p->missed, 1, __ATOMIC_RELAXED);
				skipped = 1;
			}
			continue;
		}
		if (kk && !pc_spread_in_set(slot, p->node, kk))
			continue;
		/* a non-holder's write is noted in, and handed to, the FIRST holder it goes to:
		 * one ack is one holder that has it as its own */
		if (!held && !noted) {
			rec[CLMIG_RFLAGS] |= CLMIG_F_HANDOFF;
			clpush_append_nh(cl_push(), pc_worker_id(), i, &p->addr,
				rec, n, nowms, cl_wgroup_send_cb, NULL, col, collen,
				key, klen, ver);
			rec[CLMIG_RFLAGS] &= (unsigned char)~CLMIG_F_HANDOFF;
			noted = 1;
			continue;
		}
		clpush_append(cl_push(), pc_worker_id(), i, &p->addr, rec, n,
			nowms, cl_wgroup_send_cb, NULL);
	}
	/* the caller keeps the record only if this node is one of its K holders, or a holder it
	 * skipped is owed it: the walk that sends it needs a copy here, reclaim drops it after */
	if (held || skipped)
		return 1;
	cl_px_spread_not_held();
	if (!noted)
		cl_px_spread_ack(0, 1);        /* no live holder: kept */
	return 0;
}
