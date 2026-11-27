/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clres.h - id reservations for departed members, so a node back within the window
 * claims its old id instead of renumbering the fleet.  The caller owns the table. */
#ifndef PC_CLRES_H
#define PC_CLRES_H

#include <stdint.h>
#include <netinet/in.h>

#define CLRES_MAX        256       /* = PC_CL_MAXPEER, the peer table's bound */
#define CLRES_IDENT_LEN  16

/* WHY a member left, so one that went silent does not read as cleanly stopped */
enum {
	CLRES_GONE_UNKNOWN = 0,
	CLRES_GONE_GOODBYE,                /* it sent GOODBYE */
	CLRES_GONE_SILENT,                 /* purged after no beat for a while */
	CLRES_GONE_FORGOTTEN               /* dropped: refused (config mismatch) */
};

struct clres_ent {
	struct in_addr addr;
	uint16_t port;                     /* the cluster port, network order */
	unsigned char ident[CLRES_IDENT_LEN];
	int has_ident;
	int id;                            /* 0 = the slot is unused */
	long long until_ms;
	int why;                           /* CLRES_GONE_* */
	int silent_s;                      /* how long it was silent */
};

struct clres {
	struct clres_ent e[CLRES_MAX];
};

/* Remember @id for @addr:@port / @ident (CLRES_IDENT_LEN bytes even without @has_ident)
 * until now + ttl_ms; the same address and port refresh in place.  1 if recorded, 0 if @id 0.
 * The port tells co-hosted members apart. */
int clres_note(struct clres *r, int id, struct in_addr addr, uint16_t port,
		const unsigned char *ident, int has_ident,
		long long now, long long ttl_ms);

/* record why the member @id (just noted) left, and for a silent
 * one how long it had been quiet.  A no-op if @id holds no entry. */
void clres_why(struct clres *r, int id, int why, int silent_s);

/* "goodbye" | "silent" | "forgotten" | "unknown" */
const char *clres_why_name(int why);

/* Is @id held by a LIVE reservation?  The boundary is exclusive: an
 * entry is live while until_ms > now, so it lapses AT until_ms. */
int clres_holds(const struct clres *r, int id, long long now);

/* Claim and CONSUME a reservation: IDENTITY first (it survives a move), then
 * address and port; @ident NULL matches by address only.  *how says which; 0 if none. */
int clres_take(struct clres *r, const unsigned char *ident,
		const struct in_addr *addr, uint16_t port, long long now, const char **how);

/* How many reservations are live - the stats surface. */
int clres_count(const struct clres *r, long long now);
/* live reservations of members that left for @why (CLRES_GONE_*) */
int clres_count_why(const struct clres *r, int why, long long now);
/* drop every reservation of @id at once (the operator's forget): how many */
int clres_drop(struct clres *r, int id);

/* The address a live reservation for @ident was last held at, without
 * consuming it.  Returns 1 and fills *home, or 0. */
int clres_home(const struct clres *r, const unsigned char *ident,
		long long now, struct in_addr *home);

#endif /* PC_CLRES_H */
