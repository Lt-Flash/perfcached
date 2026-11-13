/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* Spread's decisions (see clspread.h).  Nothing here reads `C` or logs. */
#include <stddef.h>

#include "clspread.h"
#include "cluster.h"
#include "clmap.h"
#include "clplace.h"
#include "clpeers.h"
#include "fnv1a.h"
#include "pc_slot.h"

/* Anchored on repair completion, not a timer: every live peer's cycle walked clean and
 * none armed is what proves the real holder already has the copy. */
int clspread_settled(int k, int in_grace, const struct pc_clmap *m,
		int self_id, const struct clpeers *pt, int c, long long now)
{
	int i;

	if (!k)
		return 0;
	if (in_grace)
		return 0;              /* a membership change is still recent */
	/* Reclaim needs the SHARED map: a still-converging local view computes another
	 * set and deletes what the senders just gave.  No covering map, no reclaim. */
	{
		int seen = 1, j;

		for (j = 0; j < pt->n_peers; j++)
			if (clpeers_holds_slots(&pt->peers[j], now))
				seen++;
		if (!m || (int)m->nnodes < seen)
			return 0;
		/* The map must list us: a rejoined node's id can postdate it, and
		 * in_set's 0 would then read every record it holds as another's. */
		if (!pc_clplace_has(m, (uint16_t)self_id))
			return 0;
	}
	for (i = 0; i < pt->n_peers; i++) {
		const struct peer *pr = &pt->peers[i];

		if (__atomic_load_n(&pr->missed, __ATOMIC_RELAXED))
			return 0;      /* owed writes it was skipped for: the copies kept for it stay */
		if (!clpeers_usable(pr, now))
			continue;      /* a CONNECTING one is placed nowhere yet */
		if (pr->nstate == PC_NST_RECOVERING)
			return 0;      /* it is still pulling its own state */
		if (pr->setrepair[c] || pr->backfill[c])
			return 0;      /* a re-placement is armed, not finished */
		if (pr->repl_cursor[c] || pr->repl_dirty[c])
			return 0;      /* mid-cycle, or a cycle lost records */
	}
	return 1;
}


/* Settled must HOLD, not merely occur: the predicate is per-node and instantaneous, and
 * another holder may still be sending to a returning node. */
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

unsigned long long clspread_set_fp(const struct clpeers *pt, long long now)
{
	unsigned long long fp = 0;
	int i;

	for (i = 0; i < pt->n_peers; i++)
		if (clpeers_holds_slots(&pt->peers[i], now))
			/* the pair pc_hrw_mix() takes; never the id, which peer_gone()
			 * zeroes and would so arm the repair inside the purge's grace */
			fp ^= fnv1a64_u64(FNV1A64_BASIS,
				(unsigned long long)
				pt->peers[i].addr.in4.sin_addr.s_addr << 16 |
				(unsigned long long)
				pt->peers[i].addr.in4.sin_port);
	return fp;
}

/* Warn, never refuse: membership is automatic so -C cannot check it, and a fleet below K
 * is degraded, not wrong.  The hold keeps a flapping peer from writing the log. */
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
