/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * logrl.c - a limit on log lines the network can drive (CH-17; logrl.h).
 */
#include <pthread.h>
#include <string.h>
#include <time.h>
#include "compat/dprint.h"
#include "logrl.h"

static const char *const reason_name[LRL_REASONS] = {
	"?", "join identity held", "join refused as a clone",
	"join held for its home", "no config digest", "fleet stop",
	"lane forced", "lane back to UDP", "stats reset", "join wire epoch",
	"keepalive wire epoch", "master keepalive wire epoch",
	"wire epoch", "config mismatch", "another cluster's id",
	"handshake as the cluster", "handshake principal",
	"handshake authentication", "handshake version", "id collision",
	"refused sender", "refused keepalive", "master keepalive out of range",
	"master keepalive not followed", "join identity live elsewhere",
	"stray datagram on the multicast socket",
};

struct lrl_slot {
	int used, reason, has_src;
	pc_addr src;
	long long said_ms;                 /* its last line said */
	unsigned long long held;           /* held back since */
};

static struct {
	pthread_mutex_t mx;
	struct lrl_slot s[LOGRL_SLOTS];
	int win_on;
	long long win_ms;                  /* the cap's window began */
	unsigned int win_n;                /* lines said in it */
	unsigned long long cap_held;       /* held by the cap since one was said */
	unsigned long long held_total;
} R = { .mx = PTHREAD_MUTEX_INITIALIZER };

static int same_key(const struct lrl_slot *s, int reason, const pc_addr *src)
{
	if (!s->used || s->reason != reason || s->has_src != (src != NULL))
		return 0;
	return !src || pc_addr_eq_host(&s->src, src);
}

int pc_logrl_ok_at(int reason, const pc_addr *src, long long now)
{
	struct lrl_slot *e = NULL, *spare = NULL;
	unsigned long long held = 0, cap_held = 0;
	char ab[PC_ADDR_STRLEN] = "-";
	int i, say = 0;

	if (reason <= 0 || reason >= LRL_REASONS)
		reason = 0;
	pthread_mutex_lock(&R.mx);
	for (i = 0; i < LOGRL_SLOTS; i++) {
		struct lrl_slot *s = &R.s[i];

		if (same_key(s, reason, src)) {
			e = s;
			break;
		}
		/* a free slot first, else the one said longest ago */
		if (!spare || (spare->used &&
		        (!s->used || s->said_ms < spare->said_ms)))
			spare = s;
	}
	if (!e) {
		e = spare;
		memset(e, 0, sizeof *e);
		e->used = 1;
		e->reason = reason;
		e->has_src = src != NULL;
		if (src)
			e->src = *src;
		e->said_ms = now - LOGRL_WINDOW_MS;        /* may say at once */
	}
	if (now - e->said_ms < LOGRL_WINDOW_MS) {
		e->held++;                     /* its own minute: held */
		R.held_total++;
	} else {
		if (!R.win_on || now - R.win_ms >= LOGRL_WINDOW_MS) {
			R.win_on = 1;
			R.win_ms = now;
			R.win_n = 0;
		}
		if (R.win_n >= LOGRL_CAP) {
			e->held++;             /* the cap: held, said with the next */
			R.cap_held++;
			R.held_total++;
		} else {
			R.win_n++;
			held = e->held;
			e->held = 0;
			e->said_ms = now;
			cap_held = R.cap_held;
			R.cap_held = 0;
			say = 1;
		}
	}
	pthread_mutex_unlock(&R.mx);
	if (!say)
		return 0;
	if (cap_held)
		LM_WARN("log limit: %llu network-driven line(s) held back - more "
			"than %d a minute\n", cap_held, LOGRL_CAP);
	if (held) {
		if (src)
			pc_addr_ntop(src, ab, sizeof ab);
		LM_WARN("log limit: %llu more '%s' line(s) about %s held back "
			"since the last one\n", held, reason_name[reason], ab);
	}
	return 1;
}

int pc_logrl_ok(int reason, const pc_addr *src)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return pc_logrl_ok_at(reason, src,
		(long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

unsigned long long pc_logrl_held(void)
{
	unsigned long long n;

	pthread_mutex_lock(&R.mx);
	n = R.held_total;
	pthread_mutex_unlock(&R.mx);
	return n;
}
