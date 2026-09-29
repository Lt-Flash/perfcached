/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clspread.c - spread's decisions (M17, wave 4).  See clspread.h.
 *
 * Moved from cluster.c at e6c1232: the settled predicate, the settle
 * hold, the reclaim verdict, the set fingerprint and the replicas-short
 * reading.  The walks, the probes, the deletes and the words stay with
 * the orchestrator.  Nothing here reads `C` - what cluster.c read from
 * it arrives as arguments - and nothing logs (rule 4).
 */
#include <stddef.h>

#include "clspread.h"
#include "cluster.h"
#include "clmap.h"
#include "clplace.h"
#include "clpeers.h"
#include "fnv1a.h"
#include "pc_slot.h"

/*
 * Settled, and anchored on REPAIR COMPLETION rather than a timer.
 *
 * The failure this must exclude is dropping a copy before the real
 * holder has it.  "Membership has been quiet for T seconds" only
 * correlates with that; "every live peer's repair cycle has walked
 * clean, and none is armed" establishes it, and the sweep already
 * maintains exactly those flags.  The grace being quiet is kept as a
 * second gate, not the primary one.
 */
int clspread_settled(int k, int in_grace, const struct pc_clmap *m,
		int self_id, const struct clpeers *pt, int c, long long now)
{
	int i;

	if (!k)
		return 0;
	if (in_grace)
		return 0;              /* a membership change is still recent */
	/*
	 * RECLAIM REQUIRES THE SHARED MAP, and this is the rule the whole
	 * pass turns on: ADDING a copy may act on a local opinion, because
	 * being wrong costs a surplus; REMOVING one may not, because being
	 * wrong costs the record.
	 *
	 * pc_spread_in_set falls back to HRW over what THIS node can see
	 * when the map is short or absent.  That is right for repair - two
	 * nodes with different views simply both send.  It is wrong here:
	 * a node whose view is still converging computes a different set
	 * from the senders' and deletes what they just gave it.  Measured
	 * on the CI runner, which is slower than this machine and so
	 * converges later: a RETURNING node went 64 -> 25 records, and the
	 * fleet to 207 where it owed 240.  Three local runs had passed.
	 *
	 * The master's map is the one view every node adopts, so sets
	 * derived from it agree by construction.  No map that covers the
	 * fleet, no reclaim - the surplus simply waits, which is the safe
	 * direction.
	 */
	{
		int seen = 1, j;

		for (j = 0; j < pt->n_peers; j++)
			if (clpeers_holds_slots(&pt->peers[j], now))
				seen++;
		if (!m || (int)m->nnodes < seen)
			return 0;
		/*
		 * AND THE MAP MUST LIST US.  pc_clplace_in_set() answers 0
		 * both for "not in the top K" and for "not in this map at
		 * all", and for a DELETE those are opposite meanings: the
		 * first says the record is not ours, the second says the map
		 * cannot tell us anything about ourselves.  A node that has
		 * just REJOINED can hold an id the published map predates -
		 * placement mixes addresses, but this lookup is by id - and
		 * it then reads every record it holds as somebody else's.
		 * Measured: a returning node went 64 -> 25 and the fleet to
		 * 207 of 240 owed.
		 */
		if (!pc_clplace_has(m, (uint16_t)self_id))
			return 0;
	}
	for (i = 0; i < pt->n_peers; i++) {
		const struct peer *pr = &pt->peers[i];

		if (!clpeers_live(pr, now))
			continue;
		if (pr->nstate == PC_NST_RECOVERING)
			return 0;      /* it is still pulling its own state */
		if (pr->setrepair[c] || pr->backfill[c])
			return 0;      /* a re-placement is armed, not finished */
		if (pr->repl_cursor[c] || pr->repl_dirty[c])
			return 0;      /* mid-cycle, or a cycle lost records */
	}
	return 1;
}


/*
 * SETTLED IS NOT ENOUGH ON ITS OWN, and the measurement that proved it:
 * a returning node ended with 25 of the ~58 records placement gives it
 * and the fleet sat at 207 where it owed 240 - UNDER-replicated, which
 * is the direction that loses data.
 *
 * The predicate is per-node and instantaneous.  The moment it first
 * goes true, this node's own repair cycles are clean, but another
 * holder may not have finished sending to the returning node, and two
 * nodes whose views of the set differ by one beat can each drop a copy
 * believing the other holds it.  Liveness of the set is not POSSESSION
 * by the set, and nothing cheap can prove possession per record.
 *
 * So settle must HOLD, not merely occur: the predicate has to be true
 * on this many consecutive ticks before anything is dropped, and any
 * false reading resets the count to zero.  Reclaiming late costs a
 * little memory and a stale-read window that was already there;
 * reclaiming early costs records.
 */
int clspread_hold(unsigned int *settled_for, unsigned int *cursor,
		int settled)
{
	if (!settled) {
		*settled_for = 0;
		*cursor = 0;           /* restart the sweep when it is */
		return 0;
	}
	if (++*settled_for < CLSPREAD_SETTLE_TICKS)
		return 0;              /* settled, but not for long enough */
	return 1;
}

/*
 * The reclaim walk's verdict on one record this node holds: 1 = ask a
 * holder whether our copy may go (S146), 0 = keep it.  Kept: a key the
 * walk's buffer cannot carry, a key this node is a holder of, and a key
 * whose K-set is not wholly live (a degraded fleet keeps its surplus).
 */
int clspread_reclaim_candidate(const char *key, int klen, int k)
{
	unsigned slot;

	if (klen <= 0 || klen > CLSPREAD_RECLAIM_KEYMAX)
		return 0;
	slot = pc_key_slot(key, (size_t)klen);
	if (pc_spread_in_set(slot, 0, k))
		return 0;                      /* we ARE a holder: keep it */
	if (!pc_spread_set_live(slot, k))
		return 0;                      /* degraded: keep the surplus */
	return 1;
}

/*
 * S127: a fingerprint of WHO IS IN THE SET - the slot-holding peers - so
 * a set change arms the repair (spread_note_set_change) and nothing else
 * does.
 */
unsigned long long clspread_set_fp(const struct clpeers *pt, long long now)
{
	unsigned long long fp = 0;
	int i;

	for (i = 0; i < pt->n_peers; i++)
		if (clpeers_holds_slots(&pt->peers[i], now))
			/* HASH EXACTLY WHAT PLACEMENT USES: the advertise
			 * address and the cluster port, which is the pair
			 * pc_hrw_mix() takes - never the runtime node id.
			 * The id is precisely what peer_gone() zeroes, so
			 * hashing it changed the fingerprint the moment a
			 * peer was purged and armed the repair INSIDE the
			 * grace that the purge was supposed to be covered
			 * by.  A fingerprint for "who is in the set" must
			 * move only when the SET moves. */
			fp ^= fnv1a64_u64(FNV1A64_BASIS,
				(unsigned long long)
				pt->peers[i].addr.sin_addr.s_addr << 16 |
				(unsigned long long)
				pt->peers[i].addr.sin_port);
	return fp;
}

/*
 * S157: WARN WHEN `replicas` EXCEEDS THE LIVE MEMBERS.
 *
 * Validation cannot check it - membership is automatic and P is unknown
 * at -C time - so it is a runtime condition on the 1 Hz tick: K against
 * self plus the live peers, once READY and outside the shard grace (a
 * membership change is "not yet", never a shortfall).  A reading has to
 * hold for a few ticks before it is believed, so a flapping peer does
 * not turn this into a log writer; one WARNING on the way in, one
 * NOTICE on the way out, and the figure on /stats, /metrics and the
 * page.  The founding case is the loud one: a single node with
 * replicas = 2 warns a few seconds after it founds the fleet, which is
 * what a reader of the configurations page needs to hear.  Not a
 * refusal - a fleet below K is degraded, not wrong.
 */
int clspread_short_step(struct clspread_short *s, int k, int ready,
		int in_grace, int live)
{
	int sh;

	if (k <= 0)
		return CLSPREAD_QUIET;
	if (!ready || in_grace)
		return CLSPREAD_QUIET;         /* not yet: hold the reading */
	sh = k > live ? k - live : 0;
	if (sh != s->last) {
		s->last = sh;
		s->held = 0;
		return CLSPREAD_QUIET;
	}
	if (++s->held < CLSPREAD_SHORT_HOLD || sh == s->now)
		return CLSPREAD_QUIET;
	__atomic_store_n(&s->now, sh, __ATOMIC_RELAXED);
	return sh ? CLSPREAD_SHORT : CLSPREAD_MET;
}

int clspread_short_now(const struct clspread_short *s)
{
	return __atomic_load_n(&s->now, __ATOMIC_RELAXED);
}
