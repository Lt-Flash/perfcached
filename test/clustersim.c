/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING
 * clustersim.c - the shipped term and epoch rules in a modelled mastership protocol under seeded,
 * replayable schedules: MODE_WIRED steps down on a higher term, MODE_ASIS keeps the role. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../src/clterm.h"
#include "../src/clmap.h"

#define MAX_NODES  5
#define MAX_MSGS   4096

enum { MODE_WIRED = 0, MODE_ASIS = 1 };

struct node {
	uint16_t id;
	/* address order is drawn from the seed: node 0 always founds, so index order
	 * would never let a stale master win the address tiebreak */
	int      addr;
	uint32_t term;                 /* highest term this node has SEEN */
	uint32_t map_term, map_seq;    /* the epoch it currently holds */
	/* the term claimed at promotion, not the epoch held: a stale master adopting a
	 * newer map keeps its role, so map_term no longer shows how stale it is */
	uint32_t claim_term;
	int      is_master;
	int      up;
};

/* a map publication in flight */
struct msg {
	int      from, to;
	uint32_t term, seq;
	uint16_t master_id;
	long     at;                   /* logical delivery tick */
	int      live;
};

struct sim {
	struct node n[MAX_NODES];
	int         nn;
	struct msg  q[MAX_MSGS];
	int         nq;
	long        tick;
	uint64_t    rng;
	/* the partition matrix: cut[a][b] = they cannot hear each other */
	unsigned char cut[MAX_NODES][MAX_NODES];
	int         mode;
	uint64_t    trace;             /* order-sensitive hash of every event */
	uint32_t    max_claimed;       /* highest term any node ever claimed */
};

/* xorshift64*, so a seed replays identically on every libc and arch -
 * rand() would make "deterministic" a property of the platform */
static uint64_t rnd(struct sim *s)
{
	uint64_t x = s->rng;

	x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
	s->rng = x;
	return x * 2685821657736338717ull;
}
static unsigned rnd_n(struct sim *s, unsigned n) { return (unsigned)(rnd(s) % n); }

static void trace_ev(struct sim *s, uint64_t what)
{
	s->trace = s->trace * 1099511628211ull ^ what;   /* FNV-ish */
}

/* ---- the protocol under test ------------------------------------- */

/* @a publishes its map to everyone it can reach, loss and delay per link from the seed;
 * differing delays reorder, and the queue drains by delivery tick */
static void publish(struct sim *s, int a)
{
	int b;

	for (b = 0; b < s->nn; b++) {
		unsigned delay;

		if (b == a || !s->n[b].up || s->cut[a][b])
			continue;
		if (rnd_n(s, 100) < 25)
			continue;                          /* dropped */
		if (s->nq >= MAX_MSGS)
			return;
		delay = 1 + rnd_n(s, 5);
		s->q[s->nq].from = a;
		s->q[s->nq].to = b;
		s->q[s->nq].term = s->n[a].map_term;
		s->q[s->nq].seq = s->n[a].map_seq;
		s->q[s->nq].master_id = s->n[a].id;
		s->q[s->nq].at = s->tick + (long)delay;
		s->q[s->nq].live = 1;
		s->nq++;
	}
	trace_ev(s, 0x9E37u ^ ((uint64_t)a << 32) ^ s->n[a].map_term);
}

/* promotion: claim a term ABOVE EVERYTHING SEEN (clterm.h rule 2) */
static void promote(struct sim *s, int a)
{
	s->n[a].term += 1;
	s->n[a].is_master = 1;
	s->n[a].map_term = s->n[a].term;
	s->n[a].map_seq = 1;
	s->n[a].claim_term = s->n[a].term;
	if (s->n[a].term > s->max_claimed)
		s->max_claimed = s->n[a].term;
	trace_ev(s, 0xC0FFEEu ^ ((uint64_t)a << 40) ^ s->n[a].term);
	publish(s, a);
}

/* How many members a node can currently see (itself included) - the
 * `peers_live() + 1` that handle_master_alive ranks on. */
static int seen_by(const struct sim *s, int a)
{
	int b, n = 1;

	for (b = 0; b < s->nn; b++)
		if (b != a && s->n[b].up && !s->cut[a][b])
			n++;
	return n;
}

/* the keepalive cure: handle_master_alive ranks two masters by member count, then address (a
 * total order, here node index), so it resolves the equal-term collision rule 3 cannot */
static void keepalive(struct sim *s, int from, int to)
{
	struct node *d = &s->n[to], *f = &s->n[from];

	if (!d->up || !f->up || !d->is_master || !f->is_master)
		return;
	/* RULE 3 FIRST, when the build under test carries it there */
	if (s->mode == MODE_WIRED &&
	    pc_term_must_stepdown(d->is_master, d->claim_term,
	        f->claim_term)) {
		d->is_master = 0;
		trace_ev(s, 0xBEEFu ^ ((uint64_t)to << 24));
		return;
	}
	/* a LOWER term never outranks us - the guard that makes the whole
	 * thing an ordering rather than two rules pulling opposite ways */
	if (s->mode == MODE_WIRED &&
	    pc_term_cmp(d->claim_term, f->claim_term) == PC_TERM_STALE)
		return;
	{
		int mine = seen_by(s, to), theirs = seen_by(s, from);

		if (theirs > mine ||
		    (theirs == mine && f->addr > d->addr)) {
			d->is_master = 0;
			trace_ev(s, 0xFEEDu ^ ((uint64_t)to << 32));
		}
	}
}

/* deliver one map to one node - the heart of what is being tested */
static void deliver(struct sim *s, const struct msg *m)
{
	struct node *d = &s->n[m->to];

	if (!d->up)
		return;
	/* a node learns a term even when it refuses the map: pc_term_observe
	 * runs before the epoch comparison */
	if (m->term > d->term && m->term - d->term < PC_TERM_MAX_JUMP)
		d->term = m->term;

	/* RULE 3, in whichever model this run is exercising */
	if (s->mode == MODE_WIRED &&
	    pc_term_must_stepdown(d->is_master, d->map_term, m->term)) {
		d->is_master = 0;
		trace_ev(s, 0xDEADu ^ ((uint64_t)m->to << 8));
	}

	/* not newer: nothing to learn (cluster.c's map_stale_n path) */
	if (pc_clmap_epoch_cmp(m->term, m->seq, d->map_term, d->map_seq) <= 0)
		return;
	d->map_term = m->term;
	d->map_seq = m->seq;
	trace_ev(s, 0xABCDu ^ ((uint64_t)m->to << 16) ^ m->term);
	/* MODE_ASIS: the map is adopted and the role is NOT touched */
}

static int masters(const struct sim *s)
{
	int i, n = 0;

	for (i = 0; i < s->nn; i++)
		if (s->n[i].up && s->n[i].is_master)
			n++;
	return n;
}

/* SC_PARTITION: an isolated master keeps its term, the other side promotes, rule 3 must converge;
 * SC_DUALCLAIM: the master dies as the fleet splits, so both sides claim the SAME next term */
enum { SC_PARTITION = 0, SC_DUALCLAIM = 1 };

/* One run: stage the fault, heal, then quiesce.
 * Returns the number of masters once everything has settled. */
static int run_one(struct sim *s, uint64_t seed, int mode, int scenario,
		uint64_t *trace, int *wrong)
{
	int i, a, b, step;

	memset(s, 0, sizeof *s);
	s->rng = seed ? seed : 1;
	s->mode = mode;
	s->nn = 3;
	for (i = 0; i < s->nn; i++) {
		s->n[i].id = (uint16_t)(i + 1);
		s->n[i].addr = i;
		s->n[i].up = 1;
	}
	for (i = s->nn - 1; i > 0; i--) {   /* seeded Fisher-Yates */
		int j = (int)rnd_n(s, (unsigned)(i + 1)), t = s->n[i].addr;

		s->n[i].addr = s->n[j].addr;
		s->n[j].addr = t;
	}
	/* the fleet agrees on the founder's map before any fault, or a node that missed it
	 * claims term 1 and stages the equal-term collision SC_DUALCLAIM is for */
	promote(s, 0);
	for (step = 0; step < 400; step++) {
		int agreed = 1;

		s->tick++;
		publish(s, 0);
		for (i = 0; i < s->nq; i++)
			if (s->q[i].live && s->q[i].at <= s->tick) {
				s->q[i].live = 0;
				deliver(s, &s->q[i]);
			}
		for (i = 0; i < s->nn; i++)
			if (s->n[i].map_term != s->n[0].map_term)
				agreed = 0;
		if (agreed && step > 8)
			break;
	}
	for (i = 0; i < s->nn; i++)
		if (s->n[i].map_term != s->n[0].map_term)
			return -1;         /* never converged: not a valid run */

	/* isolate one node from the rest, chosen by the seed */
	a = (int)rnd_n(s, (unsigned)s->nn);
	for (b = 0; b < s->nn; b++)
		if (b != a) { s->cut[a][b] = 1; s->cut[b][a] = 1; }

	if (scenario == SC_PARTITION) {
		/* the side without a master promotes ONE node, the lowest id (as
		 * pick_backup() does); an isolated master keeps serving at its term */
		if (s->n[a].is_master) {
			for (b = 0; b < s->nn; b++)
				if (b != a) { promote(s, b); break; }
		} else {
			promote(s, a);
		}
	} else {
		/* the master is gone, and both sides promote from the same
		 * highest term they had seen */
		for (i = 0; i < s->nn; i++)
			if (s->n[i].is_master) { s->n[i].up = 0;
			                         s->n[i].is_master = 0; }
		promote(s, a);
		for (b = 0; b < s->nn; b++)
			if (b != a && s->n[b].up) { promote(s, b); break; }
	}

	for (step = 0; step < 20; step++) {
		s->tick++;
		for (i = 0; i < s->nq; i++)
			if (s->q[i].live && s->q[i].at <= s->tick) {
				s->q[i].live = 0;
				deliver(s, &s->q[i]);
			}
	}

	/* heal, then re-publish until the queue drains: the keepalive runs every tick, the
	 * map only on publish - the asymmetry that decides the outcome */
	memset(s->cut, 0, sizeof s->cut);
	for (step = 0; step < 60; step++) {
		int x, y;

		s->tick++;
		for (i = 0; i < s->nn; i++)
			if (s->n[i].up && s->n[i].is_master)
				publish(s, i);
		for (i = 0; i < s->nq; i++)
			if (s->q[i].live && s->q[i].at <= s->tick) {
				s->q[i].live = 0;
				deliver(s, &s->q[i]);
			}
		for (x = 0; x < s->nn; x++)
			for (y = 0; y < s->nn; y++)
				if (x != y && !s->cut[x][y])
					keepalive(s, x, y);
	}
	if (trace)
		*trace = s->trace;
	/* did a STALE master win?  The keepalive cure converges either way; without
	 * term awareness it just seats the wrong node */
	if (wrong) {
		int i2;

		*wrong = 0;
		for (i2 = 0; i2 < s->nn; i2++)
			if (s->n[i2].up && s->n[i2].is_master &&
			    s->n[i2].claim_term < s->max_claimed)
				*wrong = 1;
	}
	return masters(s);
}

int main(int argc, char **argv)
{
	long seeds = argc > 1 ? atol(argv[1]) : 2000;
	struct sim s;
	long i;
	int pass = 0, fail = 0;
	long wired_split = 0, asis_split = 0, dual_split = 0, unconverged = 0;
	long wired_stale = 0, asis_stale = 0;
	uint64_t t1 = 0, t2 = 0;

	printf("=== clustersim (%ld seeds, 3 nodes) ===\n", seeds);

	/* ---- 1. determinism: the same seed must replay exactly -------- */
	run_one(&s, 424242, MODE_WIRED, SC_PARTITION, &t1, NULL);
	run_one(&s, 424242, MODE_WIRED, SC_PARTITION, &t2, NULL);
	if (t1 == t2 && t1 != 0) {
		printf("  ok   the same seed replays the same trace (%016llx)\n",
			(unsigned long long)t1);
		pass++;
	} else {
		printf("  FAIL seed 424242 produced two different traces "
			"(%016llx vs %016llx) - without replay this is not a\n"
			"       simulator, it is a slower flaky test\n",
			(unsigned long long)t1, (unsigned long long)t2);
		fail++;
	}
	/* ...and different seeds must NOT all collapse to one schedule */
	run_one(&s, 999983, MODE_WIRED, SC_PARTITION, &t2, NULL);
	if (t2 != t1) {
		printf("  ok   a different seed produces a different schedule\n");
		pass++;
	} else {
		printf("  FAIL two seeds produced an identical trace - the seed "
			"is not reaching the schedule\n");
		fail++;
	}

	/* ---- 2. SC_PARTITION: the two models --------------------------- */
	for (i = 1; i <= seeds; i++) {
		uint64_t sd = (uint64_t)i * 2654435761ull;
		int r, w;

		r = run_one(&s, sd, MODE_WIRED, SC_PARTITION, NULL, &w);
		if (r < 0) unconverged++;
		else { if (r > 1) wired_split++; if (w) wired_stale++; }
		r = run_one(&s, sd, MODE_ASIS, SC_PARTITION, NULL, &w);
		if (r >= 0) { if (r > 1) asis_split++; if (w) asis_stale++; }
		r = run_one(&s, sd, MODE_WIRED, SC_DUALCLAIM, NULL, &w);
		if (r >= 0 && r != 1) dual_split++;
	}
	if (unconverged)
		printf("       %ld seed(s) never formed a fleet to begin with "
			"and were not counted\n", unconverged);
	printf("       partition:  2+ masters  wired %ld/%ld  as-is %ld/%ld"
		"   |   a STALE master won  wired %ld/%ld  as-is %ld/%ld\n",
		wired_split, seeds, asis_split, seeds,
		wired_stale, seeds, asis_stale, seeds);

	/* fail-first on WHO won, not the master count: the keepalive cure converges
	 * with or without the step-down rule, it just seats the wrong node */
	if (asis_stale > 0) {
		printf("  ok   the simulator DETECTS the gap: with nothing "
			"term-aware, a STALE master won\n"
			"       %ld of %ld schedules\n", asis_stale, seeds);
		pass++;
	} else {
		printf("  FAIL nothing term-aware, yet a stale master never "
			"won - this model cannot\n       distinguish the "
			"builds, so the result below means nothing\n");
		fail++;
	}

	if (wired_stale == 0 && wired_split == 0) {
		printf("  ok   term-aware, all %ld schedules converge to ONE "
			"master and it is the one\n"
			"       holding the highest term\n", seeds);
		pass++;
	} else {
		printf("  FAIL term-aware and still wrong: %ld/%ld left 2+ "
			"masters, %ld/%ld seated a STALE one\n",
			wired_split, seeds, wired_stale, seeds);
		fail++;
	}

	/* ---- 3. the equal-term dual claim: rule 3 is a strict inequality and says nothing;
	 * the member-count/address cure resolves it (test/dualclaimtest.sh) */
	printf("       master death + split (equal terms), NOT resolved: "
		"%ld/%ld\n", dual_split, seeds);
	if (dual_split == 0) {
		printf("  ok   an equal-term double claim resolves anyway - the "
			"member/address cure\n"
			"       is a total order where the term is a tie\n");
		pass++;
	} else {
		printf("  FAIL %ld of %ld equal-term collisions left the fleet "
			"without exactly one\n       master\n",
			dual_split, seeds);
		fail++;
	}

	/* ---- 4. the jump guard cannot be poisoned --------------------- */
	{
		uint32_t mine = 7;
		int rc = pc_term_cmp(mine, mine + PC_TERM_MAX_JUMP + 1000);

		/* the guard lives in pc_term_observe: an absurd term still compares
		 * AHEAD, so the guard, not a silent clamp in cmp, must refuse it */
		if (rc == PC_TERM_AHEAD) {
			printf("  ok   an absurd term compares AHEAD - refusing "
				"it is the guard's job, not cmp's\n");
			pass++;
		} else {
			printf("  FAIL pc_term_cmp quietly absorbed an absurd "
				"term (rc=%d)\n", rc);
			fail++;
		}
	}

	printf("clustersim: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
