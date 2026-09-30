/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clres.h - id reservations for departed members (M12, slice 1).  S90.
 *
 * When a member leaves this node's view, the master remembers the id it
 * had and the address and identity that held it.  A node coming back
 * within the window claims its old id instead of being given a fresh
 * one, which is what keeps a restart from renumbering the fleet.
 *
 * The table is a fixed array: bounded so a churning fleet cannot grow
 * it, and expiring so ids return to the pool.  Full means evict the
 * entry that expires soonest - reservations are a courtesy, and the one
 * closest to lapsing is the one whose owner is least likely to return.
 *
 * A claim matches IDENTITY FIRST, then address, and that order is the
 * whole point: a node's identity file survives a move, its address does
 * not.  `how` names which matched so the caller can say so in its log.
 *
 * Pure, in the M-series sense: the caller owns the table, passes the
 * clock, and does the logging.  Every rule here - the expiry boundary,
 * the refresh-in-place, the eviction choice, identity beating address -
 * is then a case in test/clrestest.c rather than something only a fleet
 * can show.
 */
#ifndef PC_CLRES_H
#define PC_CLRES_H

#include <netinet/in.h>

#define CLRES_MAX        64        /* the table's bound, not a policy */
#define CLRES_IDENT_LEN  16

struct clres_ent {
	struct in_addr addr;
	unsigned char ident[CLRES_IDENT_LEN];
	int has_ident;
	int id;                            /* 0 = the slot is unused */
	long long until_ms;
};

struct clres {
	struct clres_ent e[CLRES_MAX];
};

/* Remember @id, held by @addr / @ident, until now + ttl_ms.
 *
 * @ident must point at CLRES_IDENT_LEN bytes; @has_ident says whether
 * they mean anything (they are stored either way, as they were).
 * An entry for the SAME ADDRESS is refreshed in place, live or lapsed.
 *
 * Returns 1 if it recorded, 0 if @id was 0 and there was nothing to
 * remember - the caller logs on 1. */
int clres_note(struct clres *r, int id, struct in_addr addr,
		const unsigned char *ident, int has_ident,
		long long now, long long ttl_ms);

/* Is @id held by a LIVE reservation?  The boundary is exclusive: an
 * entry is live while until_ms > now, so it lapses AT until_ms. */
int clres_holds(const struct clres *r, int id, long long now);

/* Claim a reservation and CONSUME it.  @ident may be NULL, in which
 * case only the address can match.  On a match *how is set to
 * "identity" or "address" and the id returned; 0 if nothing matched. */
int clres_take(struct clres *r, const unsigned char *ident,
		const struct in_addr *addr, long long now, const char **how);

/* How many reservations are live - the stats surface. */
int clres_count(const struct clres *r, long long now);

/* The address a live reservation for @ident was last held at, without
 * consuming it.  Returns 1 and fills *home, or 0. */
int clres_home(const struct clres *r, const unsigned char *ident,
		long long now, struct in_addr *home);

#endif /* PC_CLRES_H */
