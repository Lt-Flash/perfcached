/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clpeers.c - the peer table (see clpeers.h): no sockets, logging or notifications
 * and the clock passed in, so its test links against clpeers.o alone */
#include <string.h>

#include "clpeers.h"

struct peer *clpeers_by_id_live(struct clpeers *pt, int node, long long now)
{
	int i;

	for (i = 0; i < pt->n_peers; i++)
		if (clpeers_live(&pt->peers[i], now))
			if (pt->peers[i].node == node)
				return &pt->peers[i];
	return NULL;
}

int clpeers_id_elsewhere(struct clpeers *pt, int node, const pc_addr *from,
		long long now)
{
	int i;

	for (i = 0; i < pt->n_peers; i++)
		if (pt->peers[i].node == node && clpeers_live(&pt->peers[i], now) &&
		    !pc_addr_eq(&pt->peers[i].addr, from))
			return 1;
	return 0;
}

struct peer *clpeers_by_id_from(struct clpeers *pt, unsigned node,
		const pc_addr *from)
{
	int i;

	/* node 0 is nobody (no id yet, or a departed peer's slot); matching it let
	 * a restarted node filter relays before we counted it live: lost updates */
	if (!node)
		return NULL;
	for (i = 0; i < pt->n_peers; i++) {
		struct peer *p = &pt->peers[i];

		if (__atomic_load_n(&p->node, __ATOMIC_ACQUIRE) == (int)node &&
		    pc_addr_eq_host(&p->addr, from))
			return p;
	}
	return NULL;
}

struct peer *clpeers_by_addr(struct clpeers *pt, const pc_addr *a)
{
	int i;

	for (i = 0; i < pt->n_peers; i++)
		if (pc_addr_eq(&pt->peers[i].addr, a))
			return &pt->peers[i];
	return NULL;
}

int clpeers_count_live(struct clpeers *pt, long long now)
{
	int i, n = 0;

	for (i = 0; i < pt->n_peers; i++)
		if (clpeers_live(&pt->peers[i], now))
			n++;
	return n;
}

struct peer *clpeers_upsert(struct clpeers *pt, const pc_addr *addr,
		int *created)
{
	struct peer *freep = NULL;
	int i;

	*created = 0;
	for (i = 0; i < pt->n_peers; i++) {
		if (pc_addr_eq(&pt->peers[i].addr, addr))
			return &pt->peers[i];
		if (!freep && !__atomic_load_n(&pt->peers[i].node,
		        __ATOMIC_ACQUIRE))
			freep = &pt->peers[i];
	}
	if (!freep) {
		if (pt->n_peers >= PC_CL_MAXPEER)
			return NULL;
		freep = &pt->peers[pt->n_peers];
		memset(freep->repl_mark, 0, sizeof freep->repl_mark);
		memset(freep->repl_cursor, 0, sizeof freep->repl_cursor);
		memset(freep->repl_bfcycle, 0, sizeof freep->repl_bfcycle);
		memset(freep->backfill, 0, sizeof freep->backfill);
		freep->tomb_mark = freep->tomb_win = 0;
		freep->tomb_gap_from = 0;
		freep->tomb_drop_mark = freep->tomb_sent = 0;
		freep->tomb_owed = 0;
		freep->alive_ms = 0;
		freep->link_since_ms = freep->link_probe_ms = freep->link_ok_ms = 0;
		memset(freep->link_tcp_ms, 0, sizeof freep->link_tcp_ms);
		/* publish the slot after addr is in place */
		freep->addr = *addr;
		__atomic_store_n(&pt->n_peers, pt->n_peers + 1,
			__ATOMIC_RELEASE);
		*created = 1;
		return freep;
	}
	/* a slot reused for a new address inherits nothing from its last occupant */
	memset(freep->repl_mark, 0, sizeof freep->repl_mark);
	memset(freep->repl_cursor, 0, sizeof freep->repl_cursor);
	memset(freep->repl_bfcycle, 0, sizeof freep->repl_bfcycle);
	memset(freep->backfill, 0, sizeof freep->backfill);
	freep->tomb_mark = freep->tomb_win = 0;
	freep->tomb_gap_from = 0;
	freep->tomb_drop_mark = freep->tomb_sent = 0;
	freep->tomb_owed = 0;
	freep->alive_ms = 0;
	freep->link_since_ms = freep->link_probe_ms = freep->link_ok_ms = 0;
	memset(freep->link_tcp_ms, 0, sizeof freep->link_tcp_ms);
	freep->addr = *addr;
	return freep;
}

int clpeers_dup_persisted(struct clpeers *pt, const pc_addr *from,
		long long now)
{
	int i, slot = -1;

	for (i = 0; i < PC_CL_MAXPEER; i++) {
		if (pt->dup_seen[i].addr.s_addr == pc_addr_v4(from)) {
			/* a joiner knocks every second: a grace with no sighting ended that episode */
			if (now - pt->dup_seen[i].last_ms >= DUP_GRACE_MS)
				pt->dup_seen[i].first_ms = now;
			pt->dup_seen[i].last_ms = now;
			return now - pt->dup_seen[i].first_ms >= DUP_GRACE_MS;
		}
		if (slot < 0 && !pt->dup_seen[i].addr.s_addr)
			slot = i;
	}
	if (slot >= 0) {
		pt->dup_seen[slot].addr.s_addr = pc_addr_v4(from);
		pt->dup_seen[slot].first_ms = pt->dup_seen[slot].last_ms = now;
	}
	return 0;
}

void clpeers_dup_forget(struct clpeers *pt, const pc_addr *from)
{
	int i;

	for (i = 0; i < PC_CL_MAXPEER; i++)
		if (pt->dup_seen[i].addr.s_addr == pc_addr_v4(from)) {
			pt->dup_seen[i].addr.s_addr = 0;
			return;
		}
}

/* is @addr a joiner held for a duplicate claim within the grace window -
 * i.e. still knocking? */
int clpeers_dup_seen_recent(struct clpeers *pt, const struct in_addr *addr,
		long long now)
{
	int i;

	for (i = 0; i < PC_CL_MAXPEER; i++)
		if (pt->dup_seen[i].addr.s_addr == addr->s_addr &&
		        now - pt->dup_seen[i].first_ms < DUP_GRACE_MS)
			return 1;
	return 0;
}
