/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * S153: per-key EVENT logging, selected per collection.
 *
 * The counters say a collection misses N lookups a second and cannot
 * say WHICH keys; only the names settle whether that is correct (an
 * IP-auth style question answered "no") or a gap.  Four events, each
 * one line at NOTICE, chosen by a per-collection mask rather than by
 * the global level - `dbg` would flood every subsystem to watch one
 * collection - with the global level still a ceiling.
 *
 * Cost: OFF is one load of pc_ev_any before anything else; ON is a
 * per-collection, per-kind token bucket refilled once a second by the
 * maintenance thread, which also logs how many lines it suppressed, so
 * a hot miss loop cannot turn the daemon into a log writer and the
 * suppression is itself visible.
 *
 * The hook sits at the DOOR (verbs.c), not in the table: the door
 * knows the origin, it is inherently client-only, and the core library
 * stays free of logging.  Keys are truncated, or hashed on request -
 * collection 0 holds credentials.
 */
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

/* "miss, store" -> mask; "all"; "none" or "" -> 0.  -1 on an unknown word. */
int  pc_ev_parse(const char *list, unsigned int *mask);

void pc_ev_set_default(unsigned int mask, int rate_per_s, int hash_keys);
void pc_ev_configure(int slot, unsigned int mask);   /* PC_EV_INHERIT = default */

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

#endif
