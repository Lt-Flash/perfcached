/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clrecon.h - the B4 reconcile's pacing (the 0.7.2 split, wave 1): how
 * many probes a second the walk may send (S393), how far it has got
 * (S395), and how long it has gone without a witness (ST8).  The walk, its
 * witness and its fast path stay in cluster.c, which keeps one struct
 * clrecon in its state.
 */
#ifndef PC_CLRECON_H
#define PC_CLRECON_H

#include <stdint.h>

struct peer;
struct clpeers;

/* Probes a second, the floor.  Deliberately modest: this competes with
 * live traffic, and the pass only has to finish before anyone notices a
 * resurrected key, not immediately. */
#define RECONCILE_PER_TICK 64
/* S393: the budget follows the room.  A second's budget is a quarter of
 * the pending table's free slots (client pulls share it), between the
 * floor and RECONCILE_PER_TICK_MAX; a refused slot or a timed-out pull
 * holds it at the floor for RECONCILE_CALM_MS.  It is spent in slices of
 * RECONCILE_SLICE_MS, because one burst of a thousand probes is a burst
 * of two thousand answers, and an answer a receive buffer drops leaves
 * its key un-asked - kept, the safe way, but kept. */
#define RECONCILE_PER_TICK_MAX 1024   /* [cluster] reconcile_rate_max's default */
#define RECONCILE_SLICE_MS     100
#define RECONCILE_CALM_MS      10000
/* S395: the walk's rate is read over this many one-second samples */
#define RECON_RATE_N           61
/* ST8: a pass with no live witness this long says so (once a spell) */
#define RECON_NOWITNESS_WARN_S 30

/* S392: the fast path's states, how long it waits on a silent witness,
 * how often it asks, and how many replayed deletes it can number */
enum { FPS_IDLE = 0, FPS_WAIT, FPS_DONE, FPS_WALK };
#define FP_QUIET_MS   5000
#define FP_TRIES      3
#define FP_SEEN_MAX   (1u << 22)

/* S392: the fast path (FPS_*) - the request, the witness asked,
 * when asked and when last heard, tries, the replay's numbers held
 * (a bitmap), how many held, and how many the witness sent (-1 =
 * no DONE yet) */
struct clrecon_fp {
	int state;
	uint32_t req;
	int node;
	long long asked_ms, heard_ms;
	int tries;
	unsigned char *seen;
	unsigned int seen_cap;
	unsigned long long got;
	long long n;
};

struct clrecon {
	/* S393: the walk may go on between beats (the beat found everything
	 * it checks in place), the slice clock, the second's budget and its
	 * clock, and the backoff with its marks */
	int may_walk, budget, marks_set, debt;
	long long slice_ms, budget_ms, calm_until;
	unsigned long long ex_mark, to_mark;
	/* S395: one sample a beat of the keys walked, for the rate */
	long long rate_ms[RECON_RATE_N];
	unsigned long long rate_n[RECON_RATE_N];
	int rate_i;
	long long nowit_since;          /* ST8: no live witness since (ms) */
	int nowit_warned;
	/* S223: when this node's data stopped being current, translated
	 * into the monotonic clock every peer's boot_ms is kept in, and the
	 * peer that was up throughout (0 until one is found) */
	long long death_mono;
	int witness;
};

/* S393: a second's probes - a quarter of the pending table's free slots
 * (@pend_free), between RECONCILE_PER_TICK and @rate_max; held at the
 * floor for RECONCILE_CALM_MS after its refusals (@ex) or the pulls'
 * timeouts (@to) moved since the last call */
int clrecon_budget(struct clrecon *r, long long now, int pend_free,
		unsigned long long ex, unsigned long long to, int rate_max);
/* a slice's share of the second's budget, renewing that once a second
 * (*renewed = 1 then: r->budget is the new figure) */
int clrecon_slice_budget(struct clrecon *r, long long now, int pend_free,
		unsigned long long ex, unsigned long long to, int rate_max,
		int *renewed);
/* the slice's @slice less what the last slice overshot; <= 0: this slice
 * is skipped and still owes the rest.  A slice that ends at @left <= 0
 * owes -@left (clrecon_owe) */
int clrecon_slice_take(struct clrecon *r, int slice);
void clrecon_owe(struct clrecon *r, int left);
/* S395, once a beat: the keys walked a second over the last minute (from
 * @w walked of @t) and the seconds that leaves; 0 and 0 when the pass is
 * not @open, 0 and -1 when it is not walking yet */
void clrecon_progress(struct clrecon *r, long long now, int open, int walking,
		unsigned long long w, unsigned long long t,
		unsigned long long *rate, long long *eta);
/* the beat lets the walk go on between beats, or stops it */
void clrecon_walk_start(struct clrecon *r, long long now);
void clrecon_walk_stop(struct clrecon *r);
/* between beats: 1 when a slice is due (and its clock taken) */
int clrecon_pump_due(struct clrecon *r, long long now, int on);
/* ST8: no live witness at @now - *secs since the spell began; 1 the first
 * time it reaches RECON_NOWITNESS_WARN_S (say so then, once) */
int clrecon_nowit(struct clrecon *r, long long now, long long *secs);
/* the pass is not running: the clock stops (what was said stays said) */
void clrecon_nowit_stop(struct clrecon *r);
/* a witness is live again: the spell is over */
void clrecon_nowit_end(struct clrecon *r);

/* ---- S392: the fast path, the returning node's half ------------------- */

/* the replay's number bitmap freed; and the fast path given up for the
 * walk (FPS_WALK) - the caller says why */
void clrecon_fp_drop_seen(struct clrecon_fp *f);
void clrecon_fp_fall_back(struct clrecon_fp *f);
/* a witness @node asked at @now: the request id to send (never 0), the
 * counts and the bitmap cleared, the try counted, FPS_WAIT */
uint32_t clrecon_fp_asked(struct clrecon_fp *f, int node, long long now);
/* FPS_WAIT at @now: DONE (every number the witness counted is held - the
 * state is FPS_DONE), HOLD (heard from within FP_QUIET_MS), GIVE_UP
 * (silent, FP_TRIES asked), or ASK again */
#define CLRECON_FP_DONE    0
#define CLRECON_FP_HOLD    1
#define CLRECON_FP_GIVE_UP 2
#define CLRECON_FP_ASK     3
int clrecon_fp_wait(struct clrecon_fp *f, long long now);
/* a frame of the replay: 1 when it answers the ask in flight (request
 * @req from @node, while waiting) - heard from at @now; 0 to ignore */
int clrecon_fp_mine(struct clrecon_fp *f, uint32_t req, int node, long long now);
/* replayed delete @seq: 1 numbered, 0 a repeat, -1 past FP_SEEN_MAX, -2
 * no memory for the bitmap */
int clrecon_fp_number(struct clrecon_fp *f, uint32_t seq);
/* the witness vouched and sent @n: FPS_DONE once all @n are held */
void clrecon_fp_count(struct clrecon_fp *f, long long n);

/* ---- S223: the witness ------------------------------------------------- */

/* when this node stopped being current, on the monotonic clock: the later
 * of the last recovered write (@last) and the alive stamp (@stamp), wall
 * ms, taken back from @wall_now onto @mono_now; LLONG_MIN when neither is
 * known (then no peer can be shown to have watched) */
long long clrecon_death_mono(long long last, long long stamp, long long wall_now,
		long long mono_now);
/* the peer that booted at @boot_ms (0 = not heard) was up, and complete,
 * @grace_ms before this node went down */
int clrecon_is_witness(const struct clrecon *r, long long boot_ms,
		long long grace_ms);
/* the first live witness in @pt at @now, or NULL; *live (when asked) the
 * live peers counted up to it - all of them when there is none */
struct peer *clrecon_first_witness(const struct clrecon *r, struct clpeers *pt,
		long long now, long long grace_ms, int *live);
/* how many live witnesses, and the last one's node id (*id, 0 none) */
int clrecon_witnesses(const struct clrecon *r, const struct clpeers *pt,
		long long now, long long grace_ms, int *id);
/* S274: what a restart of this node (@self) now would find - live peers,
 * those up @ready_ms since their boot (they would witness it), the wait
 * for the next (-1 none); ST8: the live peers with a reconcile open whose
 * last live witness is this node or not said (the first 8) */
struct clrecon_restart {
	int live, witnesses;
	long long wait_ms;
	int waits_n, waits_for[8];
};
void clrecon_restart(const struct clpeers *pt, long long now, long long ready_ms,
		int self, struct clrecon_restart *o);

#endif
