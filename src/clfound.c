/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clfound.c - who founds a blank fleet: the joiner table, the unicast cold-start
 * rule and the settle window (the lowest identity founds).  Peer thread only. */
#include <string.h>

#include "compat/dprint.h"
#include "cluster.h"
#include "clcid.h"                   /* pc_cid_get */
#include "clmemb.h"                  /* the JOIN_WAIT reasons */
#include "clwire.h"                  /* CLWIRE_CID_NONE */
#include "clstate.h"
#include "clfound.h"

/* a joiner founds when its settle window ends - 3 s, longer while joiners keep appearing
 * (until 1 s quiet), 10 s at most - plus SETTLE_MS per live joiner of lower identity */
#define SETTLE_MS        3000
#define SETTLE_QUIET_MS  1000
#define SETTLE_CAP_MS   10000
#define WAIT_HOLD_MS     3000

static long long join_start_ms;        /* the settle window opens here */
static long long joiner_new_ms;        /* a joiner not heard before, last seen at */
static int join_said_wait;             /* the JOIN_WAIT reason said last, +1 */

/* a BLANK unicast cold start founds only once the joiners it hears were stable for
 * FOUND_STABLE_MS and it sees `expect` blank nodes, itself included */
#define FOUND_STABLE_MS  5000
#define JOINER_LIVE_MS   3000
#define JOINER_MAX       PC_CL_MAXPEER   /* every joiner heard is ranked */
static struct { pc_addr a; long long ms; unsigned char ident[16]; int has_ident; } joiner[JOINER_MAX];
static int joiner_n;
static long long joiner_change_ms;
static long long found_said_ms;
static const char *found_said_why;

/* @ident NULL from a joiner that sends none (ranked above every identity) */
void clfound_note(const pc_addr *a, const unsigned char *ident, long long now)
{
	int i;

	for (i = 0; i < joiner_n && !pc_addr_eq(&joiner[i].a, a); i++)
		;
	if (i == joiner_n) {
		if (joiner_n == JOINER_MAX)
			return;
		joiner[joiner_n++].a = *a;
		joiner_change_ms = now;
		joiner_new_ms = now;
	}
	joiner[i].ms = now;
	joiner[i].has_ident = ident != NULL;
	if (ident)
		memcpy(joiner[i].ident, ident, 16);
}

/* live joiners (expired ones leave, which is a change too); *lower to how many have a lower
 * identity than this node */
static int joiner_live(long long now, int *lower)
{
	int i, n = 0;

	*lower = 0;
	for (i = 0; i < joiner_n; ) {
		if (now - joiner[i].ms >= JOINER_LIVE_MS) {
			joiner[i] = joiner[--joiner_n];
			joiner_change_ms = now;
			continue;
		}
		n++;
		if (joiner[i].has_ident && memcmp(joiner[i].ident, cl_self_ident(), 16) < 0)
			(*lower)++;
		i++;
	}
	return n;
}

/* when this joiner may found: its settle window's end, then SETTLE_MS more for each
 * live joiner with a lower identity (the lowest founds; the next only if it does not) */
long long clfound_settle_at(long long now)
{
	int lower;
	long long end = join_start_ms + SETTLE_MS;

	joiner_live(now, &lower);
	if (joiner_new_ms + SETTLE_QUIET_MS > end)
		end = joiner_new_ms + SETTLE_QUIET_MS;
	if (end > join_start_ms + SETTLE_CAP_MS)
		end = join_start_ms + SETTLE_CAP_MS;
	return end + (long long)lower * SETTLE_MS;
}

/* 1 = this blank node must not found yet (and says why, now and then) */
int clfound_gate_blocks(long long now)
{
	unsigned char c[16];
	int live, lower, expect = pc_cluster_expect();
	const char *why = NULL;

	if (!cl_unicast_on() || pc_cid_get(c) != CLWIRE_CID_NONE)
		return 0;                      /* not blank: it has its cluster */
	if (!joiner_change_ms)
		joiner_change_ms = now;
	live = joiner_live(now, &lower);
	if (now - joiner_change_ms < FOUND_STABLE_MS)
		why = "the joiners it hears are still changing";
	else if (expect && live + 1 < expect)
		why = "fewer blank nodes than [cluster] expect";
	if (!why)
		return 0;
	/* said when the reason changes, and every 30 s while it holds */
	if (why != found_said_why || now - found_said_ms >= 30000) {
		LM_NOTICE("cluster: not founding yet - %s (%d other joiner(s) "
			"heard, expect %d)\n", why, live, expect);
		found_said_ms = now;
		found_said_why = why;
	}
	return 1;
}

void clfound_window_open(long long now)
{
	join_start_ms = now;
	join_said_wait = 0;
}

/* held 3 beats past the last one, and the settle window restarts behind it */
long long clfound_wait(const struct clmemb_wait *w, const pc_addr *from, long long now)
{
	join_start_ms = now;
	if (join_said_wait != w->reason + 1) {
		join_said_wait = w->reason + 1;
		if (w->reason == CLMEMB_WAIT_QUEUED)
			LM_NOTICE("cluster: queued for admission (place %d) by the master, "
				"node %d at %s, term %u - waiting, not founding\n",
				w->count, w->node, pc_addr_str(from), w->term);
		else
			LM_NOTICE("cluster: the fleet at %s has no master - node %d of its %d "
				"member(s), term %u, says it is electing; waiting for the new "
				"master, not founding\n", pc_addr_str(from), w->node, w->count, w->term);
	}
	return now + WAIT_HOLD_MS;
}
