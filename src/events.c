/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "compat/compat.h"
#include "compat/dprint.h"
#include "config.h"
#include "store.h"
#include "fnv1a.h"
#include "events.h"
#include "pubsub.h"                    /* PS8 */

unsigned int pc_ev_any;
unsigned int pc_ev_notify_any;                 /* PS8 */
static unsigned int nmask[PC_MAX_COLLECTIONS], def_nmask;

static unsigned int def_mask;
static int def_rate = 100, def_hash;

#define EV_KINDS 4
static struct evslot {
	unsigned int mask;
	int tokens[EV_KINDS];          /* refilled to def_rate each tick */
	unsigned int suppressed[EV_KINDS];
} ev[PC_MAX_COLLECTIONS];

static const char *const kind_name[EV_KINDS] = { "miss", "expired", "store", "remove" };

static __thread const char *ev_door = "-";
static __thread const char *ev_peer = "-";

static void recompute_any(void)
{
	unsigned int any = 0;
	int i;

	for (i = 0; i < PC_MAX_COLLECTIONS; i++)
		any |= ev[i].mask;
	__atomic_store_n(&pc_ev_any, any, __ATOMIC_RELEASE);
}


void pc_ev_set_default(unsigned int mask, int rate_per_s, int hash_keys)
{
	def_mask = mask;
	def_rate = rate_per_s > 0 ? rate_per_s : 100;
	def_hash = hash_keys;
}

/* PS8: the keyspace notification, both channel shapes, local-only */
static void notify(int slot, const char *event, const str *key)
{
	char chan[64 + 256 + 4096];
	const char *name = pc_store_name(slot);
	size_t nl, el = strlen(event);

	if (!name || key->len <= 0 || (size_t)key->len > 4096)
		return;
	nl = strlen(name);
	if (nl > 256)
		return;
	memcpy(chan, "__keyspace@", 11);
	memcpy(chan + 11, name, nl);
	memcpy(chan + 11 + nl, "__:", 3);
	memcpy(chan + 14 + nl, key->s, (size_t)key->len);
	pc_pubsub_publish(chan, 14 + nl + (size_t)key->len, event, el, 2);
	memcpy(chan, "__keyevent@", 11);
	memcpy(chan + 11, name, nl);
	memcpy(chan + 11 + nl, "__:", 3);
	memcpy(chan + 14 + nl, event, el);
	pc_pubsub_publish(chan, 14 + nl + el, key->s, (size_t)key->len, 2);
}

static void recompute_notify_any(void)
{
	unsigned int any = 0;
	int i;

	for (i = 0; i < PC_MAX_COLLECTIONS; i++)
		any |= nmask[i];
	__atomic_store_n(&pc_ev_notify_any, any, __ATOMIC_RELEASE);
}

void pc_ev_set_notify_default(unsigned int mask)
{
	def_nmask = mask;
}

void pc_ev_configure_notify(int slot, unsigned int mask)
{
	if (slot < 0 || slot >= PC_MAX_COLLECTIONS)
		return;
	nmask[slot] = mask == PC_EV_INHERIT ? def_nmask : mask;
	recompute_notify_any();
}

static int nslot_for(pcache_htable_t *ht, unsigned int want)
{
	int s;

	if (!__atomic_load_n(&pc_ev_notify_any, __ATOMIC_ACQUIRE))
		return -1;
	s = pc_store_slot_of(ht);
	if (s < 0 || !(nmask[s] & want))
		return -1;
	return s;
}

void pc_ev_applied(pcache_htable_t *ht, const str *key)
{
	int s = nslot_for(ht, PC_EV_STORE);

	if (s >= 0)
		notify(s, "set", key);
}

void pc_ev_configure(int slot, unsigned int mask)
{
	int k;

	if (slot < 0 || slot >= PC_MAX_COLLECTIONS)
		return;
	ev[slot].mask = mask == PC_EV_INHERIT ? def_mask : mask;
	for (k = 0; k < EV_KINDS; k++) {
		__atomic_store_n(&ev[slot].tokens[k], def_rate, __ATOMIC_RELAXED);
		__atomic_store_n(&ev[slot].suppressed[k], 0u, __ATOMIC_RELAXED);
	}
	recompute_any();
}

void pc_ev_set_origin(const char *door, const char *peer)
{
	ev_door = door ? door : "-";
	ev_peer = peer ? peer : "-";
}

/* the key as it goes on the line: bounded, printable, or a hash */
static const char *fmt_key(const str *key, char *buf, size_t cap)
{
	size_t n, i;

	if (def_hash) {
		snprintf(buf, cap, "#%016llx",
			(unsigned long long)fnv1a64(key->s, (size_t)key->len));
		return buf;
	}
	n = (size_t)key->len;
	if (n > 64)
		n = 64;
	if (n > cap - 4)
		n = cap - 4;
	for (i = 0; i < n; i++) {
		unsigned char c = (unsigned char)key->s[i];
		buf[i] = (c >= 0x20 && c < 0x7f && c != ' ') ? (char)c : '?';
	}
	if ((size_t)key->len > n) { buf[n++] = '.'; buf[n++] = '.'; buf[n++] = '.'; }
	buf[n] = 0;
	return buf;
}

/* one token, or one more suppressed.  Tokens go negative between ticks;
 * the tick resets them, so nothing here needs a lock. */
static int allow(int slot, int kind)
{
	if (__atomic_sub_fetch(&ev[slot].tokens[kind], 1, __ATOMIC_RELAXED) >= 0)
		return 1;
	__atomic_add_fetch(&ev[slot].suppressed[kind], 1u, __ATOMIC_RELAXED);
	return 0;
}

static int slot_for(pcache_htable_t *ht, unsigned int want)
{
	int s = pc_store_slot_of(ht);

	if (s < 0 || !(ev[s].mask & want))
		return -1;
	return s;
}

void pc_ev_miss(pcache_htable_t *ht, const str *key, int expired)
{
	char kb[80];
	int s;

	if ((s = nslot_for(ht, PC_EV_MISS)) >= 0)         /* PS8 */
		notify(s, "keymiss", key);
	if (!__atomic_load_n(&pc_ev_any, __ATOMIC_ACQUIRE))
		return;
	if ((s = slot_for(ht, PC_EV_MISS)) < 0 || !allow(s, 0))
		return;
	LM_NOTICE("event=miss cause=%s col=%s key=%s door=%s peer=%s\n",
		expired ? "expired" : "absent", pc_store_name(s),
		fmt_key(key, kb, sizeof kb), ev_door, ev_peer);
}

void pc_ev_store(pcache_htable_t *ht, const str *key, unsigned int bytes,
		unsigned int ttl)
{
	char kb[80];
	int s;

	if ((s = nslot_for(ht, PC_EV_STORE)) >= 0)        /* PS8 */
		notify(s, "set", key);
	if (!__atomic_load_n(&pc_ev_any, __ATOMIC_ACQUIRE))
		return;
	if ((s = slot_for(ht, PC_EV_STORE)) < 0 || !allow(s, 2))
		return;
	LM_NOTICE("event=store col=%s key=%s bytes=%u ttl=%u door=%s peer=%s\n",
		pc_store_name(s), fmt_key(key, kb, sizeof kb), bytes, ttl,
		ev_door, ev_peer);
}

void pc_ev_remove(pcache_htable_t *ht, const str *key)
{
	char kb[80];
	int s;

	if ((s = nslot_for(ht, PC_EV_REMOVE)) >= 0)       /* PS8 */
		notify(s, "del", key);
	if (!__atomic_load_n(&pc_ev_any, __ATOMIC_ACQUIRE))
		return;
	if ((s = slot_for(ht, PC_EV_REMOVE)) < 0 || !allow(s, 3))
		return;
	LM_NOTICE("event=remove col=%s key=%s door=%s peer=%s\n",
		pc_store_name(s), fmt_key(key, kb, sizeof kb), ev_door, ev_peer);
}

void pc_ev_expired(int slot, const str *key)
{
	char kb[80];

	if (__atomic_load_n(&pc_ev_notify_any, __ATOMIC_ACQUIRE) &&
	    slot >= 0 && slot < PC_MAX_COLLECTIONS && (nmask[slot] & PC_EV_EXPIRED))
		notify(slot, "expired", key);                /* PS8 */
	if (!__atomic_load_n(&pc_ev_any, __ATOMIC_ACQUIRE))
		return;
	if (slot < 0 || slot >= PC_MAX_COLLECTIONS ||
	        !(ev[slot].mask & PC_EV_EXPIRED) || !allow(slot, 1))
		return;
	LM_NOTICE("event=expired col=%s key=%s door=sweep peer=-\n",
		pc_store_name(slot), fmt_key(key, kb, sizeof kb));
}

void pc_ev_tick(void)
{
	int s, k;

	if (!__atomic_load_n(&pc_ev_any, __ATOMIC_ACQUIRE))
		return;
	for (s = 0; s < PC_MAX_COLLECTIONS; s++) {
		if (!ev[s].mask)
			continue;
		for (k = 0; k < EV_KINDS; k++) {
			unsigned int sup = __atomic_exchange_n(&ev[s].suppressed[k],
				0u, __ATOMIC_RELAXED);

			if (sup)
				LM_NOTICE("event=%s col=%s suppressed=%u - over "
					"log_events_rate (%d/s) in the last tick\n",
					kind_name[k], pc_store_name(s), sup, def_rate);
			__atomic_store_n(&ev[s].tokens[k], def_rate, __ATOMIC_RELAXED);
		}
	}
}
