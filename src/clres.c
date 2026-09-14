/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clres.c - id reservations for departed members.  See clres.h.
 *
 * Every body came out of cluster.c unchanged; what differs is that the
 * table is reached through a pointer, the clock and the TTL are passed
 * in, and the one log line stayed behind with the caller.
 */
#include <string.h>

#include "clres.h"

int clres_note(struct clres *r, int id, struct in_addr addr,
		const unsigned char *ident, int has_ident,
		long long now, long long ttl_ms)
{
	long long oldest = 0;
	int i, slot = -1;

	if (!id)
		return 0;
	for (i = 0; i < CLRES_MAX; i++) {
		if (r->e[i].id && r->e[i].addr.s_addr == addr.s_addr) {
			slot = i;              /* the same address: refresh */
			break;
		}
		if (!r->e[i].id || r->e[i].until_ms <= now) {
			if (slot < 0)
				slot = i;
		} else if (slot < 0 && (!oldest || r->e[i].until_ms < oldest))
			oldest = r->e[i].until_ms;
	}
	if (slot < 0)                      /* full: evict the oldest */
		for (i = 0; i < CLRES_MAX; i++)
			if (r->e[i].until_ms == oldest) {
				slot = i;
				break;
			}
	if (slot < 0)
		slot = 0;
	r->e[slot].addr = addr;
	r->e[slot].has_ident = has_ident;
	memcpy(r->e[slot].ident, ident, CLRES_IDENT_LEN);
	r->e[slot].id = id;
	r->e[slot].until_ms = now + ttl_ms;
	return 1;
}

int clres_holds(const struct clres *r, int id, long long now)
{
	int i;

	for (i = 0; i < CLRES_MAX; i++)
		if (r->e[i].id == id && r->e[i].until_ms > now)
			return 1;
	return 0;
}

int clres_take(struct clres *r, const unsigned char *ident,
		const struct in_addr *addr, long long now, const char **how)
{
	int i, id = 0;

	for (i = 0; i < CLRES_MAX && !id; i++)
		if (r->e[i].id && r->e[i].until_ms > now &&
		    r->e[i].has_ident && ident &&
		    memcmp(r->e[i].ident, ident, CLRES_IDENT_LEN) == 0) {
			id = r->e[i].id;
			*how = "identity";
			r->e[i].id = 0;
		}
	for (i = 0; i < CLRES_MAX && !id; i++)
		if (r->e[i].id && r->e[i].until_ms > now &&
		    r->e[i].addr.s_addr == addr->s_addr) {
			id = r->e[i].id;
			*how = "address";
			r->e[i].id = 0;
		}
	return id;
}

int clres_count(const struct clres *r, long long now)
{
	int i, n = 0;

	for (i = 0; i < CLRES_MAX; i++)
		if (r->e[i].id && r->e[i].until_ms > now)
			n++;
	return n;
}

int clres_home(const struct clres *r, const unsigned char *ident,
		long long now, struct in_addr *home)
{
	int i;

	for (i = 0; i < CLRES_MAX; i++)
		if (r->e[i].id && r->e[i].until_ms > now &&
		    r->e[i].has_ident &&
		    memcmp(r->e[i].ident, ident, CLRES_IDENT_LEN) == 0) {
			*home = r->e[i].addr;
			return 1;
		}
	return 0;
}
