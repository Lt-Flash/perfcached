/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clrecon.c - the B4 reconcile's pacing (clrecon.h): the probe budget a
 * second and its backoff (S393), the slices it is spent in and the debt a
 * slice that overshot leaves, the walk's rate and what it leaves (S395),
 * and the no-witness clock (ST8).  Moved from cluster.c with its state;
 * what it read of the cluster's - the pending table's room, the counters
 * - is passed in (the 0.7.2 split, wave 1 - doc/MODULARITY.md).  And the
 * returning node's half of the S392 fast path: the ask, the wait, the
 * numbering of the witness's replay and its count; the frames, the sends,
 * the logs and the stats stay cluster.c's.  And S223's witness: when this
 * node stopped being current, which live peers were up throughout, and
 * (S274) what a restart of this node now would find - the grace is
 * cluster.c's, passed in.  test/clrecontest.c.  Peer thread only.
 */
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "clpeers.h"
#include "clrecon.h"

/* a second's probes: a quarter of the pending table's free slots, held at
 * the floor for RECONCILE_CALM_MS after a refused slot or a timed-out pull */
int clrecon_budget(struct clrecon *r, long long now, int pend_free,
		unsigned long long ex, unsigned long long to, int rate_max)
{
	int b = pend_free / 4;

	if (r->marks_set && (ex != r->ex_mark || to != r->to_mark))
		r->calm_until = now + RECONCILE_CALM_MS;
	r->marks_set = 1;
	r->ex_mark = ex;
	r->to_mark = to;
	if (now < r->calm_until || b < RECONCILE_PER_TICK)
		return RECONCILE_PER_TICK;
	return b > rate_max ? rate_max : b;
}

int clrecon_slice_budget(struct clrecon *r, long long now, int pend_free,
		unsigned long long ex, unsigned long long to, int rate_max,
		int *renewed)
{
	*renewed = 0;
	if (!r->budget || now - r->budget_ms >= 1000) {
		r->budget_ms = now;
		r->budget = clrecon_budget(r, now, pend_free, ex, to, rate_max);
		*renewed = 1;
	}
	return (r->budget * RECONCILE_SLICE_MS + 999) / 1000;
}

int clrecon_slice_take(struct clrecon *r, int slice)
{
	int b = slice - r->debt;

	r->debt = 0;
	if (b <= 0)
		r->debt = -b;
	return b;
}

void clrecon_owe(struct clrecon *r, int left)
{
	r->debt = -left;
}

/* once a beat: keys walked a second over the last minute, and what that
 * leaves (-1 = not known: not walking yet) */
void clrecon_progress(struct clrecon *r, long long now, int open, int walking,
		unsigned long long w, unsigned long long t,
		unsigned long long *rate, long long *eta)
{
	int i = r->rate_i, o;

	if (!open) {
		*rate = 0;
		*eta = 0;
		return;
	}
	if (!walking || !t) {
		*rate = 0;
		*eta = -1;
		return;
	}
	r->rate_ms[i] = now;
	r->rate_n[i] = w;
	r->rate_i = (i + 1) % RECON_RATE_N;
	o = r->rate_ms[r->rate_i] ? r->rate_i : 0;
	*rate = now > r->rate_ms[o] && w >= r->rate_n[o] ?
		(w - r->rate_n[o]) * 1000 /
		(unsigned long long)(now - r->rate_ms[o]) : 0;
	*eta = !*rate ? -1 :
		t > w ? (long long)((t - w) / *rate) : 0;
}

void clrecon_walk_start(struct clrecon *r, long long now)
{
	r->may_walk = 1;
	r->slice_ms = now;
}

void clrecon_walk_stop(struct clrecon *r)
{
	r->may_walk = 0;
}

int clrecon_pump_due(struct clrecon *r, long long now, int on)
{
	if (!r->may_walk || !on || now - r->slice_ms < RECONCILE_SLICE_MS)
		return 0;
	r->slice_ms = now;
	return 1;
}

int clrecon_nowit(struct clrecon *r, long long now, long long *secs)
{
	if (!r->nowit_since)
		r->nowit_since = now;
	*secs = (now - r->nowit_since) / 1000;
	if (*secs >= RECON_NOWITNESS_WARN_S && !r->nowit_warned) {
		r->nowit_warned = 1;
		return 1;
	}
	return 0;
}

void clrecon_nowit_stop(struct clrecon *r)
{
	r->nowit_since = 0;
}

void clrecon_nowit_end(struct clrecon *r)
{
	r->nowit_since = 0;
	r->nowit_warned = 0;
}

/* ---- S392: the fast path, the returning node's half ------------------- */

void clrecon_fp_drop_seen(struct clrecon_fp *f)
{
	free(f->seen);
	f->seen = NULL;
	f->seen_cap = 0;
}

void clrecon_fp_fall_back(struct clrecon_fp *f)
{
	f->state = FPS_WALK;
	clrecon_fp_drop_seen(f);
}

uint32_t clrecon_fp_asked(struct clrecon_fp *f, int node, long long now)
{
	f->req = ((uint32_t)now << 4) ^ (uint32_t)f->tries ^ 0x5a5a0000u;
	if (!f->req)
		f->req = 1;
	f->node = node;
	f->got = 0;
	f->n = -1;
	if (f->seen)
		memset(f->seen, 0, f->seen_cap);
	f->asked_ms = f->heard_ms = now;
	f->tries++;
	f->state = FPS_WAIT;
	return f->req;
}

int clrecon_fp_wait(struct clrecon_fp *f, long long now)
{
	if (f->n >= 0 && f->got >= (unsigned long long)f->n) {
		f->state = FPS_DONE;
		return CLRECON_FP_DONE;
	}
	if (now - f->heard_ms < FP_QUIET_MS)
		return CLRECON_FP_HOLD;
	if (f->tries >= FP_TRIES)
		return CLRECON_FP_GIVE_UP;
	return CLRECON_FP_ASK;
}

int clrecon_fp_mine(struct clrecon_fp *f, uint32_t req, int node, long long now)
{
	if (f->state != FPS_WAIT || req != f->req || node != f->node)
		return 0;
	f->heard_ms = now;
	return 1;
}

int clrecon_fp_number(struct clrecon_fp *f, uint32_t seq)
{
	unsigned int need;

	if (seq >= FP_SEEN_MAX)
		return -1;
	need = seq / 8 + 1;
	if (need > f->seen_cap) {
		unsigned int cap = f->seen_cap ? f->seen_cap : 4096;
		unsigned char *nb;

		while (cap < need)
			cap *= 2;
		nb = realloc(f->seen, cap);
		if (!nb)
			return -2;
		memset(nb + f->seen_cap, 0, cap - f->seen_cap);
		f->seen = nb;
		f->seen_cap = cap;
	}
	if (!(f->seen[seq / 8] & (1u << (seq % 8)))) {
		f->seen[seq / 8] |= (unsigned char)(1u << (seq % 8));
		f->got++;
		return 1;
	}
	return 0;
}

void clrecon_fp_count(struct clrecon_fp *f, long long n)
{
	f->n = n;
	if (f->got >= (unsigned long long)f->n)
		f->state = FPS_DONE;
}

/* ---- S223: the witness ------------------------------------------------- */

long long clrecon_death_mono(long long last, long long stamp, long long wall_now,
		long long mono_now)
{
	long long dead_for;

	if (stamp > last)
		last = stamp;
	if (last <= 0)
		return LLONG_MIN;          /* unknown: nobody can be shown to have watched */
	dead_for = wall_now > last ? wall_now - last : 0;
	return mono_now - dead_for;
}

/* THE predicate: this peer was up, and complete, before we went down. */
int clrecon_is_witness(const struct clrecon *r, long long boot_ms,
		long long grace_ms)
{
	return boot_ms && r->death_mono != LLONG_MIN &&
		boot_ms + grace_ms < r->death_mono;
}

struct peer *clrecon_first_witness(const struct clrecon *r, struct clpeers *pt,
		long long now, long long grace_ms, int *live)
{
	int i, n = 0;

	for (i = 0; i < pt->n_peers; i++) {
		struct peer *p = &pt->peers[i];

		if (!__atomic_load_n(&p->node, __ATOMIC_ACQUIRE) ||
		        !clpeers_live(p, now))
			continue;
		n++;
		if (clrecon_is_witness(r, p->boot_ms, grace_ms)) {
			if (live)
				*live = n;
			return p;
		}
	}
	if (live)
		*live = n;
	return NULL;
}

int clrecon_witnesses(const struct clrecon *r, const struct clpeers *pt,
		long long now, long long grace_ms, int *id)
{
	int i, n = 0;

	*id = 0;
	for (i = 0; i < pt->n_peers; i++) {
		const struct peer *p = &pt->peers[i];

		if (__atomic_load_n(&p->node, __ATOMIC_ACQUIRE) &&
		        clpeers_live(p, now) &&
		        clrecon_is_witness(r, p->boot_ms, grace_ms)) {
			n++;
			*id = p->node;
		}
	}
	return n;
}

void clrecon_restart(const struct clpeers *pt, long long now, long long ready_ms,
		int self, struct clrecon_restart *o)
{
	int i;

	memset(o, 0, sizeof *o);
	o->wait_ms = -1;
	for (i = 0; i < pt->n_peers; i++) {
		const struct peer *p = &pt->peers[i];
		long long ready;

		if (!__atomic_load_n(&p->node, __ATOMIC_ACQUIRE) ||
		        !clpeers_live(p, now))
			continue;
		o->live++;
		/* ST8, narrowed in 0.5.7.1: only a peer whose LAST live
		 * witness is this node (or that does not say) - one with
		 * another witness loses nothing, one with none already waits
		 * for its own restart */
		if (p->reconciling && o->waits_n < 8 &&
		    (p->sole_witness < 0 || p->sole_witness == self))
			o->waits_for[o->waits_n++] = p->node;
		if (!p->boot_ms)
			continue;              /* no uptime heard: never a witness */
		ready = p->boot_ms + ready_ms;
		if (ready <= now)
			o->witnesses++;
		else if (o->wait_ms < 0 || ready - now < o->wait_ms)
			o->wait_ms = ready - now;
	}
}
