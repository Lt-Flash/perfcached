/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clwork.c - entry points that run on a WORKER thread (M9, first slice).
 *
 * The contract of this file is its thread, not its feature: everything
 * here is called by a worker, never by the peer thread, and reads the
 * cluster state only through clstate.h - no `C`, per rule 2.
 *
 * ONLY TWO of M9's eight entry points are here, and the reason is
 * measured rather than assumed.  The other six call thirteen functions
 * that are static to cluster.c - pend_alloc, seal_send, peer_by_node,
 * broadcast_live, build_fwd_op, build_pull_req among them - and those
 * are precisely the orchestration wrappers M1, M2 and M6 left behind on
 * purpose.  Moving them would drag C.psk, C.fd, cl_fail and the
 * rate-limited logging in here with them.  Field coupling was solved by
 * the accessor header; function coupling is what gates the rest.
 *
 * These two need only the clock and the free-space read, which cluster.c
 * lends through cl_now_ms() and cl_self_free_mb().
 *
 * No unit test: M9 is the one task in the series whose specification
 * names none, and both of these need a populated peer table and live
 * arena state.  They are covered by the cluster suites, as before.
 */
#include <netinet/in.h>
#include <stdint.h>

#include "cluster.h"
#include "clpeers.h"
#include "clplace.h"
#include "clstate.h"
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
