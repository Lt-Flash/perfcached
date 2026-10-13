/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * winctr.c - S351: five-minute windows of cumulative counters.  See
 * winctr.h.  One table of keys, each with a snapshot a minute in
 * PC_WIN_SLOTS slots; a mutex around it - /stats is built a few times a
 * second at most, never on a hot path.
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "compat/timer.h"
#include "winctr.h"

#define WIN_KEYLEN 96
#define WIN_CAP    2048            /* power of two: open addressing */
#define WIN_STALE  900             /* a key unseen this long is reusable */

struct winent {
	char key[WIN_KEYLEN];
	unsigned int seen;                    /* last asked for, ticks */
	unsigned char has[PC_WIN_SLOTS];
	unsigned char zero[PC_WIN_SLOTS];      /* a restart's baseline: 0, exact */
	unsigned int mi[PC_WIN_SLOTS];        /* minute of the snapshot */
	unsigned int at[PC_WIN_SLOTS];        /* tick it was taken */
	unsigned long long v[PC_WIN_SLOTS];
};

static pthread_mutex_t win_mx = PTHREAD_MUTEX_INITIALIZER;
static struct winent *win_tab;
static unsigned int win_n;

static unsigned int win_hash(const char *k)
{
	unsigned int h = 2166136261u;

	while (*k)
		h = (h ^ (unsigned char)*k++) * 16777619u;
	return h;
}

/* the key's entry, claimed if it is new; NULL when the table is full of
 * live keys.  Under win_mx. */
static struct winent *win_find(const char *key, unsigned int now)
{
	unsigned int h, i, probe, reuse = WIN_CAP;

	if (!win_tab) {
		win_tab = calloc(WIN_CAP, sizeof *win_tab);
		if (!win_tab)
			return NULL;
	}
	h = win_hash(key) & (WIN_CAP - 1);
	for (probe = 0; probe < WIN_CAP; probe++) {
		i = (h + probe) & (WIN_CAP - 1);
		if (!win_tab[i].key[0]) {
			if (reuse == WIN_CAP)
				reuse = i;
			break;             /* the chain ends here: a new key */
		}
		if (!strncmp(win_tab[i].key, key, WIN_KEYLEN - 1))
			return &win_tab[i];
		if (reuse == WIN_CAP && now - win_tab[i].seen > WIN_STALE)
			reuse = i;         /* stale: a dropped collection, a gone peer */
	}
	if (reuse == WIN_CAP)
		return NULL;
	if (!win_tab[reuse].key[0])
		win_n++;
	memset(&win_tab[reuse], 0, sizeof win_tab[reuse]);
	strncpy(win_tab[reuse].key, key, WIN_KEYLEN - 1);
	return &win_tab[reuse];
}

long long pc_win_delta_at(const char *key, unsigned long long cur,
		int from_zero, unsigned int now, unsigned int *secs)
{
	struct winent *e;
	unsigned int mi = now / 60, s, best = PC_WIN_SLOTS, span;
	long long d = -1;

	if (secs)
		*secs = 0;
	pthread_mutex_lock(&win_mx);
	e = win_find(key, now);
	if (!e) {
		pthread_mutex_unlock(&win_mx);
		return -1;
	}
	e->seen = now;
	/* a counter that went backwards was restarted (reset stats, a peer
	 * that came back): its snapshots belong to the run before.  It
	 * started again from 0 somewhere in the last window, so its baseline
	 * is EXACTLY 0 - kept as this minute's snapshot and flagged, so the
	 * windows that follow count everything since the restart (not just
	 * what came after the first read of it) and need no minute of
	 * history first */
	for (s = 0; s < PC_WIN_SLOTS; s++)
		if (e->has[s] && cur < e->v[s]) {
			memset(e->has, 0, sizeof e->has);
			memset(e->zero, 0, sizeof e->zero);
			s = mi % PC_WIN_SLOTS;
			e->has[s] = 1;
			e->zero[s] = 1;
			e->mi[s] = mi;
			e->at[s] = now;
			e->v[s] = 0;
			break;
		}
	/* the baseline: the oldest snapshot no more than five minutes old */
	for (s = 0; s < PC_WIN_SLOTS; s++) {
		if (!e->has[s] || now - e->at[s] > PC_WIN_SPAN)
			continue;
		if (best == PC_WIN_SLOTS || e->at[s] < e->at[best])
			best = s;
	}
	if (from_zero && now <= PC_WIN_SPAN) {
		d = (long long)cur;            /* everything since start */
		span = now;
	} else if (best < PC_WIN_SLOTS && (e->zero[best] ||
	           now - e->at[best] >= PC_WIN_MIN)) {
		d = (long long)(cur - e->v[best]);
		span = now - e->at[best];
	} else {
		span = 0;                      /* no minute of it yet */
	}
	/* this minute's snapshot, the first time the minute is seen */
	s = mi % PC_WIN_SLOTS;
	if (!e->has[s] || e->mi[s] != mi) {
		e->has[s] = 1;
		e->zero[s] = 0;
		e->mi[s] = mi;
		e->at[s] = now;
		e->v[s] = cur;
	}
	pthread_mutex_unlock(&win_mx);
	if (secs)
		*secs = d >= 0 ? span : 0;
	return d;
}

long long pc_win_delta(const char *key, unsigned long long cur, int from_zero,
		unsigned int *secs)
{
	return pc_win_delta_at(key, cur, from_zero, get_ticks(), secs);
}

unsigned int pc_win_keys(void)
{
	unsigned int n;

	pthread_mutex_lock(&win_mx);
	n = win_n;
	pthread_mutex_unlock(&win_mx);
	return n;
}

void pc_win_reset(void)
{
	pthread_mutex_lock(&win_mx);
	if (win_tab)
		memset(win_tab, 0, (size_t)WIN_CAP * sizeof *win_tab);
	win_n = 0;
	pthread_mutex_unlock(&win_mx);
}
