/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clwork.c - entry points that run on a WORKER thread (M9, first slice).
 *
 * The contract of this file is its thread, not its feature: everything
 * here is called by a worker, never by the peer thread, and reads the
 * cluster state only through clstate.h - no `C`, per rule 2.
 *
 * M9 (2026-09-12) moved two of its eight entry points and measured why
 * the other six stayed: they called thirteen functions static to
 * cluster.c - pend_alloc, seal_send, peer_by_node, broadcast_live,
 * build_fwd_op, build_pull_req among them.  Field coupling was solved by
 * the accessor header; function coupling gated the rest.
 *
 * Wave 4 answered it (2026-09-19).  M14 made the send path a module
 * (clsend), the frame builders moved beside their codecs
 * (clpull_req_frame, clfwd_op_frame), and clstate.h gained the "complete
 * request" primitive (cl_pend_alloc, whose rate-limited log stays in
 * cluster.c) with cl_pend_release and cl_set_fail.  M9b then moved the
 * pull and forward entry points here (pc_pull_begin_at, pc_pull_begin_kind,
 * pc_pull_begin, pc_fwd_begin, pc_probe_fwd_begin, pc_fwd_json_begin),
 * bodies unchanged but for each C field read through its accessor.  Of
 * M9's list, pc_repl_push is left: it travels with the write-path push
 * callback.
 *
 * test/clworktest.c links THIS object with the real clpend and clsend and
 * supplies the accessors itself - M9's missing test.
 */
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cluster.h"
#include "clpeers.h"
#include "clplace.h"
#include "clstate.h"
#include "clpend.h"                  /* M9b: struct pending */
#include "clsend.h"                  /* M14: the send path */
#include "clpull.h"                  /* M9b: clpull_req_frame */
#include "clfwd.h"                   /* M9b: clfwd_op_frame, clfwd_json_build */
#include "clmsg.h"                   /* M9b: M_FWD_JSON, M_REPL_MANY */
#include "clmig.h"                   /* M9b: the record and group headers */
#include "clpush.h"                  /* M9b: the per-worker push groups */
#include "pc_slot.h"                 /* M9b: pc_key_slot */
#include "daemon.h"                  /* M9b: pc_worker_id */
#include "compat/timer.h"            /* M9b: get_ticks */
#include "pc_mix.h"

#define SELF_BAND_PCT 25

/*
 * Ownership.  Returns 0 when THIS node owns the key and a node id
 * otherwise - restated here because the map's placement function returns
 * a real id for every node including self, and handing that back
 * unchanged would tell this node it owns nothing.
 *
 * The map decides only once it has caught up with what this node sees as
 * live (clmap_recheck_usable).  Until then the fleet's own liveness
 * does, which is what it did before there was a map - and the two agree
 * whenever their node sets do, because uniform weights make weighted
 * rendezvous the unweighted argmax.
 */
/* Placement hashes the SLOT, not the key.
 *
 * A RESP client computes crc16(key) % 16384 knowing nothing about our
 * collections, so the slot is the only value it and we can both derive
 * - and both reaching the same owner is the entire point of S44.  The
 * rendezvous function underneath is unchanged: it still mixes the
 * value with each node's stable address, still compares in integers,
 * still moves minimal data when the fleet changes.  Only what goes in
 * has changed.
 *
 * Two consequences, both intended:
 *   - the collection no longer affects placement, so the same key in
 *     two collections shares an owner.  Redis has no collections, so
 *     any scheme a Redis client can compute must ignore ours;
 *   - granularity is 1-in-16384 rather than per-key.  At three nodes
 *     that is ~1.4% imbalance; see the §9.1 note before running very
 *     large fleets or very uneven weights.
 *
 * This entry point takes the SLOT so that CLUSTER SLOTS can walk the
 * slot space directly; pc_shard_owner() below is this function after
 * hashing the key. */
int pc_shard_owner_slot(unsigned slot)
{
	uint64_t kh = slot, best, h;
	long long now = cl_now_ms();
	int i, owner = 0;
	struct clpeers *pt = cl_peers();
	struct sockaddr_in self = cl_self_addr();

	if (!cl_enabled())
		return 0;
	if (cl_map_usable()) {
		uint16_t o = pc_clplace_owner(cl_map(), kh);

		if (o) {
			cl_place_map_hit();
			return o == (uint16_t)cl_node_id() ? 0 : (int)o;
		}
	}
	cl_place_hrw_hit();
	/*
	 * A node that is not READY must not own anything, and the fallback
	 * has to say so as loudly as the map does.  Map placement already
	 * excludes it (pc_clmap_placeable), but this path seeded itself
	 * unconditionally - so a FAILED node fell back to HRW, kept
	 * winning about 1/N of the slots, and never handed those keys to
	 * anyone.  Its reshard tick moves exactly the keys it no longer
	 * owns, so "still owns them" and "never migrates them" are the
	 * same bug.
	 *
	 * Seeding 0 lets any live peer outrank us.  With no live peer at
	 * all it stays 0, which this function reads as self - correct in
	 * the degenerate case, since there is nowhere else for the key to
	 * go.
	 */
	best = pc_node_state() == PC_NST_READY
		? pc_hrw_mix(kh, self.sin_addr.s_addr,
			self.sin_port)
		: 0;
	for (i = 0; i < pt->n_peers; i++) {
		if (!clpeers_live(&pt->peers[i], now))
			continue;
		h = pc_hrw_mix(kh, pt->peers[i].addr.sin_addr.s_addr,
			pt->peers[i].addr.sin_port);
		if (h > best) {
			best = h;
			owner = pt->peers[i].node;
		}
	}
	return owner;
}

/*
 * S127: is @node_id one of the K holders of @slot?
 *
 * This MIRRORS pc_shard_owner_slot above, path for path, and that is
 * the point rather than an accident: map first, HRW over live peers as
 * the fallback, a node that is not READY excluded from the fallback.
 * Two placement functions that agree "usually" put a record on one node
 * and look for it on another, so spread reuses shard's structure rather
 * than growing a second opinion beside it.
 *
 * @node_id 0 means this node.  Rank is "how many peers beat me", so the
 * count stops at @k - no ranked array, nothing sorted, and it is asked
 * once per peer per record on the write path.
 */
int pc_spread_in_set(unsigned slot, int node_id, int k)
{
	uint64_t kh = slot, mine, h;
	long long now = cl_now_ms();
	struct clpeers *pt = cl_peers();
	struct sockaddr_in self = cl_self_addr();
	int i, ahead = 0, self_id = cl_node_id();
	int me = node_id == 0 || node_id == self_id;

	if (!cl_enabled() || k <= 0)
		return 0;
	/*
	 * THE MAP IS ONLY USED WHEN IT PLAUSIBLY COVERS THE FLEET.
	 *
	 * For shard, a map that is missing members yields a wrong OWNER and
	 * the request is forwarded - a hop, not a loss.  Here a false
	 * negative makes a genuine holder conclude it is not one, and the
	 * write path DELETES the record.  Measured on a three-node fleet
	 * whose master had published a one-node map: every write returned
	 * +OK, stores incremented, entries stayed at zero, and the data was
	 * simply gone.
	 *
	 * So the map must list at least as many nodes as this node can see
	 * live, or the HRW fallback - which is built from what we can see -
	 * decides.  Erring toward HRW costs at most a placement that
	 * disagrees with the master's for a beat; erring toward a short map
	 * costs records.
	 */
	if (cl_map_usable()) {
		const struct pc_clmap *m = cl_map();
		int seen = 1;                  /* ourselves */

		for (i = 0; i < pt->n_peers; i++)
			if (clpeers_holds_slots(&pt->peers[i], now))
				seen++;
		if (m && (int)m->nnodes >= seen) {
			uint16_t want = (uint16_t)(me ? self_id : node_id);

			cl_place_map_hit();
			return pc_clplace_in_set(m, kh, want, k);
		}
	}
	cl_place_hrw_hit();
	if (me) {
		/* the same exclusion shard's fallback makes: a node that is
		 * not READY owns nothing, and must not hold a replica either */
		if (pc_node_state() != PC_NST_READY)
			return 0;
		mine = pc_hrw_mix(kh, self.sin_addr.s_addr, self.sin_port);
	} else {
		for (i = 0; i < pt->n_peers; i++)
			if (pt->peers[i].node == node_id)
				break;
		/* the GRACE window, not liveness: a peer that is merely
		 * quiet keeps its slots, or every blip reshuffles the set */
		if (i == pt->n_peers || !clpeers_holds_slots(&pt->peers[i], now))
			return 0;              /* unknown or long gone */
		mine = pc_hrw_mix(kh, pt->peers[i].addr.sin_addr.s_addr,
			pt->peers[i].addr.sin_port);
	}
	/* self counts as a candidate when asking about a PEER, and every
	 * live peer counts when asking about self - the candidate set is
	 * the same either way, which is what makes the answers consistent */
	if (!me && pc_node_state() == PC_NST_READY) {
		h = pc_hrw_mix(kh, self.sin_addr.s_addr, self.sin_port);
		if (h > mine && ++ahead >= k)
			return 0;
	}
	for (i = 0; i < pt->n_peers; i++) {
		if (pt->peers[i].node == (me ? self_id : node_id))
			continue;
		if (!clpeers_holds_slots(&pt->peers[i], now))
			continue;
		h = pc_hrw_mix(kh, pt->peers[i].addr.sin_addr.s_addr,
			pt->peers[i].addr.sin_port);
		if (h > mine && ++ahead >= k)
			return 0;
	}
	return 1;
}

/*
 * S127: the best-ranked live PEER that holds @slot, or 0 if none does.
 *
 * A miss under spread must not broadcast.  Only K of P nodes hold the
 * record, so a broadcast asks P-1 nodes to answer a question K of them
 * can answer - the read amplification the mode exists to avoid.
 *
 * SELF IS EXCLUDED DELIBERATELY.  This is only called after the local
 * table has said "absent", so if this node is a holder it is a holder
 * that does not have the record - mid-convergence, or it dropped a copy
 * it should not have kept.  Asking ourselves again cannot help; the
 * other holders are the ones who might.
 *
 * Ranked rather than first-found: the top holder for a slot is the same
 * node on every asker, so repeated misses for one key converge on one
 * peer instead of scattering across the set.
 */
int pc_spread_pick_peer(unsigned slot, int k)
{
	uint64_t kh = slot, best = 0, h;
	long long now = cl_now_ms();
	struct clpeers *pt = cl_peers();
	int i, pick = 0;

	if (!cl_enabled() || k <= 0)
		return 0;
	for (i = 0; i < pt->n_peers; i++) {
		if (!clpeers_live(&pt->peers[i], now))
			continue;
		if (!pc_spread_in_set(slot, pt->peers[i].node, k))
			continue;
		h = pc_hrw_mix(kh, pt->peers[i].addr.sin_addr.s_addr,
			pt->peers[i].addr.sin_port);
		if (!pick || h > best) {
			best = h;
			pick = pt->peers[i].node;
		}
	}
	return pick;
}

/*
 * S127: is EVERY member of @slot's K-set live right now?
 *
 * The reclaim pass asks this before dropping a surplus copy.  "In the
 * set" is computed over slot-HOLDING candidates, which deliberately
 * includes a peer inside its departure grace - so a set can be complete
 * on paper while one of its members is unreachable.  Dropping our
 * surplus then removes a copy we can see in favour of ones we cannot.
 *
 * Prefer keeping a surplus to deleting the last copy we can prove
 * exists: a degraded fleet keeps its surplus and reclaims later.
 */
int pc_spread_set_live(unsigned slot, int k)
{
	struct cand { uint64_t h; int live; } c[PC_CL_MAXPEER + 1];
	uint64_t kh = slot;
	long long now = cl_now_ms();
	struct clpeers *pt = cl_peers();
	struct sockaddr_in self = cl_self_addr();
	int i, j, n = 0, sel;

	if (!cl_enabled() || k <= 0)
		return 0;
	/*
	 * THE SAME AUTHORITY pc_spread_in_set USES, AND THAT IS THE POINT.
	 *
	 * reclaim_cb asks two questions before deleting a copy: "am I a
	 * holder" (pc_spread_in_set) and "is the K-set live" (this).  The
	 * first consults the master's MAP whenever the map covers the
	 * fleet.  This one used to be pure HRW over locally-visible peers,
	 * so the two could describe DIFFERENT SETS OF NODES - and a delete
	 * was then authorised by "the HRW top-K are alive" while the
	 * exclusion had been decided by "the map's top-K excludes me".  The
	 * liveness guard was vouching for nodes that were not the map's
	 * holders at all.
	 *
	 * In steady state the two agree and nothing shows.  They diverge
	 * exactly when a node RETURNS - the map is being republished while
	 * HRW has just gained a live peer - which is when reclaim runs on
	 * the most records.  Measured on the CI runner, v0.3.7-rc7: surplus
	 * was 52, reclaim removed 92, the returning node went 67 -> 18
	 * against a share of ~58, and the fleet ended at 200 where it owed
	 * 240.  UNDER-replicated, the direction that loses data.
	 *
	 * Note this still proves LIVENESS, not POSSESSION - see the settle
	 * comment in cluster.c.  It removes the wrong-set error only.
	 */
	if (cl_map_usable()) {
		const struct pc_clmap *m = cl_map();
		int seen = 1;                  /* ourselves */

		for (i = 0; i < pt->n_peers; i++)
			if (clpeers_holds_slots(&pt->peers[i], now))
				seen++;
		if (m && (int)m->nnodes >= seen) {
			uint16_t top[PC_CL_MAXPEER + 1];
			int got, s, self_id = cl_node_id();

			if (k > (int)(sizeof top / sizeof top[0]))
				return 0;
			got = pc_clplace_rank(m, kh, top, k);
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
	if (pc_node_state() == PC_NST_READY) {
		c[n].h = pc_hrw_mix(kh, self.sin_addr.s_addr, self.sin_port);
		c[n].live = 1;                 /* ourselves, by definition */
		n++;
	}
	for (i = 0; i < pt->n_peers && n <= PC_CL_MAXPEER; i++) {
		if (!clpeers_holds_slots(&pt->peers[i], now))
			continue;
		c[n].h = pc_hrw_mix(kh, pt->peers[i].addr.sin_addr.s_addr,
			pt->peers[i].addr.sin_port);
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
		if (clpeers_live(&pt->peers[i], cl_now_ms()))
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

/* ---- M9b (wave 4): the worker entry points M9 left ------------------------
 * pc_pull_begin_at, pc_pull_begin_kind (was cluster.c's static
 * pull_begin_kind, also called by the reconcile probe), pc_pull_begin,
 * pc_fwd_begin, pc_probe_fwd_begin and pc_fwd_json_begin.  M9 measured
 * what held them: thirteen cluster.c statics.  With M14's clsend and the
 * three primitives clstate.h gained (cl_pend_alloc, cl_pend_release,
 * cl_set_fail) they call no cluster.c static.  Moved otherwise as they
 * were - the bodies are cluster.c's as of 911c61d, each C field read
 * through its accessor, each counter bumped through its function with
 * the ordering (atomic or plain) its call site had.  Worker thread,
 * except pc_pull_begin_kind's reconcile caller, as before. */

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
	req = cl_pend_alloc(PC_DONE_PULL, 1, 0, &slot);
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
	return req;
}

/* @kind is PC_DONE_PULL for a client's miss, or PC_DONE_RECONCILE for
 * B4's probe - the wire request is identical, only what happens to the
 * answer differs. */
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
		: PC_DONE_FWD_SET;

	cl_set_fail(PC_CLFAIL_NONE);
	if (!cl_enabled() || !pr || collen > 255 || klen > 4096 ||
	        vlen > PC_MAX_FWD_VAL) {
		cl_set_fail(PC_CLFAIL_ROUTE);
		cl_st_fwd_no_route();
		return 0;
	}
	/* forwards get real deadline headroom over pulls: a write's
	 * honest-late answer beats a false failure while the holder
	 * stores anyway (the deep-pipeline lesson) */
	req = cl_pend_alloc(kind, 1, 700, &slot);
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
	return req;
}

/* probe-before-place: see cluster.h.  Broadcasts a pull whose pending
 * slot carries the deferred write; handle_pull_rsp transforms a
 * positive into a forward (same req - the ack completes the client's
 * park), an all-negative replays on the worker, a timeout refuses. */
uint32_t pc_probe_fwd_begin(int op, const char *col, size_t collen,
		const char *key, size_t klen, const char *val, size_t vlen,
		unsigned int ttl_rel, long long by)
{
	unsigned char msg[1 + 4 + 2 + 1 + 2 + 256 + 4096];
	struct pending *slot = NULL;
	char *stash = NULL;
	uint32_t req;
	int up;
	size_t n;

	/* col/key must fit the slot: the transform re-sends them */
	if (!cl_enabled() || collen >= 40 || klen >= 256 || vlen > PC_MAX_FWD_VAL)
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
	req = cl_pend_alloc(PC_DONE_PULL, up, 700, &slot);
	if (!req) {
		free(stash);
		return 0;
	}
	/* fields land BEFORE the datagrams leave: no answer can race them */
	memcpy(slot->col, col, collen);
	slot->collen = (unsigned char)collen;
	memcpy(slot->key, key, klen);
	slot->klen = (unsigned short)klen;
	slot->probe_op = op == 2 ? 2 : 1;
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

/* ---- M9b (wave 4): the write-path push, the last of M9's list ------------
 * pc_repl_push appends a record to this worker's push group per holder
 * (S105/S110 - each worker owns its own groups), and cl_wgroup_send_cb is
 * the callback clpush calls when a group is ready: it stamps clmig's
 * group header and seals.  It was cluster.c's static wgroup_send_cb; the
 * worker flush that stays in cluster.c (repl_flush_mine) passes it by its
 * new name, which is the only rename.  Bodies otherwise as at cbe2529,
 * each C field through its accessor, MIG_RHDR spelled CLMIG_RHDR (the
 * same 19).  Worker thread. */

/* A ready group leaves here: clpush decides WHEN, this seals and sends,
 * because the datagram is clwire's (M6) and the counters are the
 * cluster's.  Handed to clpush as its send callback. */
void cl_wgroup_send_cb(struct wgroup *g, void *ctx)
{
	(void)ctx;
	if (!g->qcount)
		return;
	/* req 0: no ack, see above.  The header layout is clmig's - this
	 * was the last place in orchestration still spelling it by hand. */
	clmig_group_hdr(g->q, CLMIG_GHDR, M_REPL_MANY, 0, cl_node_id(),
		g->qcount);
	clsend_seal(&g->to, g->q, g->qlen);
	cl_px_repl_pushed(g->qcount);
	cl_px_repl_groups();
	clpush_sent(cl_push(), g);
}

int pc_repl_push(const char *col, size_t collen, const char *key,
		size_t klen, const char *val, size_t vlen, unsigned int exp,
		unsigned long long ver)
{
	unsigned char rec[CLMIG_RHDR + 255 + 4096 + PC_MAX_FWD_VAL + 8];
	unsigned int now = get_ticks(), ttl_left = 0;
	long long nowms;
	size_t n;
	int i, kk;
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
			.ver = ver
		};

		clmig_rec_write(rec, &rr);
	}
	n = clmig_rec_size((unsigned int)collen, (unsigned int)klen,
		(unsigned int)vlen);
	nowms = cl_now_ms();
	/* S127: under spread the record goes to its K HOLDERS, not to every
	 * peer.  That is the whole point of the mode - eager makes per-node
	 * apply equal to the fleet's total write rate, so one node's apply
	 * path caps the fleet however many nodes are added, while K copies
	 * spread (K-1) of the work over P nodes and per-node apply FALLS as
	 * the fleet grows.  K = 0 here means any other mode, and the loop
	 * below is then exactly what it always was. */
	kk = pc_cluster_replicas();
	slot = kk ? pc_key_slot(key, klen) : 0;
	for (i = 0; i < cl_peers()->n_peers; i++) {
		struct peer *p = &cl_peers()->peers[i];

		if (!clpeers_live(p, nowms))
			continue;
		if (kk && !pc_spread_in_set(slot, p->node, kk))
			continue;
		clpush_append(cl_push(), pc_worker_id(), i, &p->addr, rec, n,
			nowms, cl_wgroup_send_cb, NULL);
	}
	/* the caller keeps the record only if placement says this node is
	 * one of its K holders.  Any other mode keeps everything, as before. */
	if (!kk)
		return 1;
	if (pc_spread_in_set(slot, 0, kk))
		return 1;
	cl_px_spread_not_held();
	return 0;
}
