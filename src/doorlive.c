/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * doorlive.c - the doors' live settings (12nw): the snapshot a reload
 * swaps whole - both allow-lists, the HTTP token, the RESP password, the
 * enable secrets - its retirement, and the checks the doors make against
 * it.  Moved from daemon.c unchanged (the 0.7.2 split, wave 1 -
 * doc/MODULARITY.md); the PSK table and the reload itself stay there.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sodium.h>
#include "compat/dprint.h"
#include "pcaddr.h"
#include "config.h"
#include "daemon.h"
#include "reload.h"
#include "doorlive.h"

/* ---- 12nw: what a config reload may change, swapped whole ---------------
 * The doors read the allow-lists and the secrets through the accessors
 * below, and those read LIVE: an immutable snapshot that a reload replaces
 * with one store.  A reader holds the pointer for one check, never past
 * it, so the snapshot a reload replaced is wiped and freed RETIRE_S later
 * (pc_live_retire_tick, the maintenance thread). */
#define LIVE_RETIRE_S 10
#define LIVE_RETIRED  8
struct pc_live *LIVE;
static struct {
	struct pc_live *l;
	long long at;
} live_retired[LIVE_RETIRED];

static char *live_dup(const char *s)
{
	return s ? strdup(s) : NULL;
}

static void live_free(struct pc_live *l)
{
	int i;

	if (!l)
		return;
	if (l->http_token)
		sodium_memzero(l->http_token, strlen(l->http_token));
	if (l->resp_password)
		sodium_memzero(l->resp_password, strlen(l->resp_password));
	for (i = 0; i < l->n_enable_secrets; i++)
		if (l->enable_secret[i])
			sodium_memzero(l->enable_secret[i], strlen(l->enable_secret[i]));
	free(l->http_token);
	free(l->resp_password);
	for (i = 0; i < l->n_enable_secrets; i++)
		free(l->enable_secret[i]);
	free(l);
}

struct pc_live *live_from(const struct pc_config *c)
{
	struct pc_live *l = calloc(1, sizeof *l);
	int i;

	if (!l)
		return NULL;
	l->n_resp_allow = c->n_resp_allow;
	memcpy(l->resp_allow, c->resp_allow, sizeof l->resp_allow);
	l->n_http_allow = c->n_http_allow;
	memcpy(l->http_allow, c->http_allow, sizeof l->http_allow);
	l->http_token = live_dup(c->http_token);
	l->resp_password = live_dup(c->resp_password);
	l->n_enable_secrets = c->n_enable_secrets;
	for (i = 0; i < c->n_enable_secrets; i++)
		l->enable_secret[i] = live_dup(c->enable_secret[i]);
	return l;
}

static long long live_now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec;
}

const struct pc_live *live(void)
{
	return __atomic_load_n(&LIVE, __ATOMIC_ACQUIRE);
}

/* the maintenance thread, once a second: free what no reader can hold */
void pc_live_retire_tick(void)
{
	long long now = live_now_s();
	int i;

	for (i = 0; i < LIVE_RETIRED; i++)
		if (live_retired[i].l && now - live_retired[i].at >= LIVE_RETIRE_S) {
			live_free(live_retired[i].l);
			live_retired[i].l = NULL;
		}
}

/* replace the snapshot; the old one is retired, not freed.  Called under
 * reload.c's lock, so one install at a time. */
void live_install(struct pc_live *nl)
{
	struct pc_live *old = __atomic_exchange_n(&LIVE, nl, __ATOMIC_ACQ_REL);
	int i, slot = -1;

	if (!old)
		return;
	for (i = 0; i < LIVE_RETIRED; i++)
		if (!live_retired[i].l) {
			slot = i;
			break;
		}
	if (slot < 0) {                /* eight reloads inside ten seconds:
	                                * the oldest has had the most time */
		long long oldest = 0;

		slot = 0;
		for (i = 0; i < LIVE_RETIRED; i++)
			if (!oldest || live_retired[i].at < oldest)
				oldest = live_retired[i].at, slot = i;
		live_free(live_retired[slot].l);
	}
	live_retired[slot].l = old;
	live_retired[slot].at = live_now_s();
}

/* ---- RESP listener guards (task S33) ------------------------------------
 * A RESP listener has no handshake - stock Redis clients cannot speak
 * Noise - so its access control is entirely config: an allow-list
 * checked at accept, and an optional Redis AUTH password checked before
 * any data command.  Both live here because CFG does. */

int pc_resp_password_set(void)
{
	const struct pc_live *L = live();

	return L && L->resp_password != NULL;
}

/* constant-time: the password crosses the wire in the clear, which is
 * no reason to leak its prefix through timing as well */
int pc_resp_password_ok(const char *p, size_t n)
{
	const struct pc_live *L = live();
	const char *want = L ? L->resp_password : NULL;
	size_t wn, i;
	unsigned char diff = 0;

	if (!want)
		return 0;
	wn = strlen(want);
	if (n != wn)
		return 0;
	for (i = 0; i < n; i++)
		diff |= (unsigned char)(p[i] ^ want[i]);
	return diff == 0;
}

/* S129: does @p match ANY configured enable secret?  Constant time per
 * candidate, for the same reason the RESP password is: a privilege
 * secret must not leak its prefix through timing.  Every candidate is
 * compared even after a match, so the answer costs the same whichever
 * entry of a rotation list matched - or none did.
 *
 * FAIL-CLOSED: no secret configured returns 0, so `enable` cannot
 * succeed and the privileged verbs stay unreachable from the client
 * door however `allow_create` is set. */
int pc_enable_secret_ok(const char *p, size_t n)
{
	const struct pc_live *L = live();
	int i, ok = 0;

	if (!L || !p)
		return 0;
	for (i = 0; i < L->n_enable_secrets; i++) {
		const char *want = L->enable_secret[i];
		size_t wn, j;
		unsigned char diff = 0;

		if (!want)
			continue;
		wn = strlen(want);
		if (n != wn) {
			diff = 1;
		} else {
			for (j = 0; j < n; j++)
				diff |= (unsigned char)(p[j] ^ want[j]);
		}
		ok |= (diff == 0);
	}
	return ok;
}

int pc_enable_configured(void)
{
	const struct pc_live *L = live();

	return L && L->n_enable_secrets > 0;
}

int pc_resp_peer_allowed(const pc_addr *sa, unsigned int len)
{
	const struct pc_live *L = live();
	int i;

	if (!L || !L->n_resp_allow)
		return 1;              /* a LAN listener without a list was
		                        * already refused at config validate */
	if (len < sizeof sa->in4)
		return 0;              /* not an address: not on the list */
	for (i = 0; i < L->n_resp_allow; i++)  /* IP6-2: either family */
		if (pc_addr_in_prefix(sa, L->resp_allow[i].family,
		        L->resp_allow[i].net, L->resp_allow[i].bits))
			return 1;
	/* A turned-away client is invisible to it (we close before a byte
	 * is read), so it has to be visible HERE or an operator debugging
	 * "my Redis client just hangs up" has nothing to go on.  Rate-
	 * limited: a port scan must not become a log flood. */
	{
		static __thread unsigned int shouted;

		if (shouted < 10) {
			shouted++;
			LM_WARN("RESP connection from %s refused: not in "
				"resp_allow%s\n", pc_addr_str(sa),
				shouted == 10 ? " (further ones silent)" : "");
		}
	}
	return 0;
}

/* S46: the same check for the metrics listener.  Separate list, same
 * reasoning - and separate so an operator can let a scraper in without
 * widening the RESP door, which reads and WRITES data. */
int pc_http_peer_allowed(const pc_addr *sa, unsigned int len)
{
	const struct pc_live *L = live();
	int i;

	if (!L || !L->n_http_allow)
		return 1;              /* off-box without a list was refused
		                        * at config validate */
	if (len < sizeof sa->in4)
		return 0;
	for (i = 0; i < L->n_http_allow; i++)  /* IP6-2: either family */
		if (pc_addr_in_prefix(sa, L->http_allow[i].family,
		        L->http_allow[i].net, L->http_allow[i].bits))
			return 1;
	{
		static __thread unsigned int shouted;

		if (shouted < 10) {
			shouted++;
			LM_WARN("http connection from %s refused: not in "
				"http_allow%s\n", pc_addr_str(sa),
				shouted == 10 ? " (further ones silent)" : "");
		}
	}
	return 0;
}

/* S37: the HTTP door's token.  Compared in constant time by the
 * caller for the same reason the RESP password is - it crosses the
 * wire in the clear, which is no reason to leak its prefix through
 * timing as well. */
const char *pc_http_token(void)
{
	const struct pc_live *L = live();

	return L ? L->http_token : NULL;
}
