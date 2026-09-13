/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clpeers.c - the peer table (M1).  See clpeers.h.
 *
 * Every function here is the body that was in cluster.c, moved unchanged:
 * C.peers / C.n_peers / C.dup_seen are reached through the caller's
 * pointer, and the clock is passed in rather than read.  The two things
 * the originals did BESIDES touching the table - shard_note_change() on
 * an appended slot, and peer_gone() on a forgotten address - belong to
 * the caller again.  That is what keeps this file clear of the cluster's
 * sockets, logging and notifications, and is why its test links against
 * clpeers.o alone.
 */
#include <string.h>

#include "clpeers.h"

/* cluster.c keeps its own addr_eq for the write-path groups; this is the
 * same comparison, private here so the module links alone. */
static int peer_addr_eq(const struct sockaddr_in *a,
		const struct sockaddr_in *b)
{
	return a->sin_addr.s_addr == b->sin_addr.s_addr &&
		a->sin_port == b->sin_port;
}

struct peer *clpeers_by_id_live(struct clpeers *pt, int node, long long now)
{
	int i;

	for (i = 0; i < pt->n_peers; i++)
		if (clpeers_live(&pt->peers[i], now))
			if (pt->peers[i].node == node)
				return &pt->peers[i];
	return NULL;
}

struct peer *clpeers_by_addr(struct clpeers *pt, const struct sockaddr_in *a)
{
	int i;

	for (i = 0; i < pt->n_peers; i++)
		if (peer_addr_eq(&pt->peers[i].addr, a))
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

struct peer *clpeers_upsert(struct clpeers *pt, const struct sockaddr_in *addr,
		int *created)
{
	struct peer *freep = NULL;
	int i;

	*created = 0;
	for (i = 0; i < pt->n_peers; i++) {
		if (peer_addr_eq(&pt->peers[i].addr, addr))
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
		memset(freep->backfill, 0, sizeof freep->backfill);   /* S102 */
		/* publish the slot after addr is in place */
		freep->addr = *addr;
		__atomic_store_n(&pt->n_peers, pt->n_peers + 1,
			__ATOMIC_RELEASE);
		*created = 1;
		return freep;
	}
	memset(freep->repl_mark, 0, sizeof freep->repl_mark);
	memset(freep->repl_cursor, 0, sizeof freep->repl_cursor);
	memset(freep->repl_bfcycle, 0, sizeof freep->repl_bfcycle);
	memset(freep->backfill, 0, sizeof freep->backfill);   /* S102: a slot
	                                     * reused for a new address must not
	                                     * inherit its last occupant's flag */
	freep->addr = *addr;
	return freep;
}

int clpeers_dup_persisted(struct clpeers *pt, const struct sockaddr_in *from,
		long long now)
{
	int i, slot = -1;

	for (i = 0; i < PC_CL_MAXPEER; i++) {
		if (pt->dup_seen[i].addr.s_addr == from->sin_addr.s_addr) {
			if (now - pt->dup_seen[i].first_ms >= DUP_GRACE_MS)
				return 1;
			return 0;
		}
		if (slot < 0 && !pt->dup_seen[i].addr.s_addr)
			slot = i;
	}
	if (slot >= 0) {
		pt->dup_seen[slot].addr = from->sin_addr;
		pt->dup_seen[slot].first_ms = now;
	}
	return 0;
}

void clpeers_dup_forget(struct clpeers *pt, const struct sockaddr_in *from)
{
	int i;

	for (i = 0; i < PC_CL_MAXPEER; i++)
		if (pt->dup_seen[i].addr.s_addr == from->sin_addr.s_addr) {
			pt->dup_seen[i].addr.s_addr = 0;
			return;
		}
}

/* S108: is @addr a joiner that was held for a duplicate claim within
 * the grace window - i.e. still knocking? */
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
