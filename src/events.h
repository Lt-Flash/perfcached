/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* events.h - per-key event logging, chosen per collection by a mask and rate-limited per kind;
 * keys are truncated, or hashed on request (collection 0 holds credentials). */
#ifndef PC_EVENTS_H
#define PC_EVENTS_H

#include "compat/str.h"
#include "core/pcache_htable.h"

#define PC_EV_MISS     1u
#define PC_EV_EXPIRED  2u
#define PC_EV_STORE    4u
#define PC_EV_REMOVE   8u
#define PC_EV_ALL      15u
#define PC_EV_INHERIT  0xFFFFFFFFu     /* use the [daemon] default */

/* "miss, store" -> mask; "all"; "none" or "" -> 0.  -1 on an unknown word.
 * Defined in config.c, which links standalone in wipetest. */
int  pc_ev_parse(const char *list, unsigned int *mask);

void pc_ev_set_default(unsigned int mask, int rate_per_s, int hash_keys);
void pc_ev_configure(int slot, unsigned int mask);   /* PC_EV_INHERIT = default */
/* the same kinds as Redis keyspace notifications, to this node's subscribers only (never
 * relayed: each node emits for what it applies); off unless a mask names them */
void pc_ev_set_notify_default(unsigned int mask);
void pc_ev_configure_notify(int slot, unsigned int mask);   /* PC_EV_INHERIT = default */
/* a record applied on this node from a peer - a forwarded write at its
 * holder, an eager replica - is a store here, and notifies as one */
void pc_ev_applied(pcache_htable_t *ht, const str *key);

/* thread-local: which door this request came in by, and from where */
void pc_ev_set_origin(const char *door, const char *peer);

void pc_ev_miss(pcache_htable_t *ht, const str *key, int expired);
void pc_ev_store(pcache_htable_t *ht, const str *key, unsigned int bytes,
		unsigned int ttl);
void pc_ev_remove(pcache_htable_t *ht, const str *key);
void pc_ev_expired(int slot, const str *key);      /* from the sweep */

void pc_ev_tick(void);                             /* maintenance, ~1/s */

/* one load: is ANY collection logging anything?  Hooks test this first. */
extern unsigned int pc_ev_any;
extern unsigned int pc_ev_notify_any;   /* any collection notifying */

#endif
