/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING
 *
 * clustersim.c — S42f: the mastership state machine under adverse
 * message schedules, with no network and no wall clock.
 *
 * WHY THIS EXISTS, given S42a-e already work.  The staged plan said to
 * build this only if the container/loopback suites proved too slow or
 * too flaky to iterate on, and they did not: a-e all passed on the real
 * rigs and every failure was a harness bug, not rig flakiness.  What
 * they CANNOT do is the rest of that paragraph - drive the same
 * schedule twice.  A partition on a live fleet happens once, at
 * whatever moment the scheduler chose; a split brain that needs two
 * promotions inside one detection interval is not something you can ask
 * a live rig for.  Here the schedule IS the input, and a seed replays
 * it exactly.
 *
 * WHAT IS REAL AND WHAT IS MODELLED, stated plainly because a
 * simulator that blurs this is worse than none:
 *
 *   REAL - the shipped decision functions, called directly:
 *     pc_term_cmp, pc_term_must_stepdown  (src/clterm.c)
 *     pc_clmap_epoch_cmp                  (src/clmap.c)
 *   MODELLED - everything else: nodes, their roles, the message queue,
 *     delivery order, partitions.  This simulates the PROTOCOL around
 *     those rules; it does not run the daemon.
 *
 * So a green run means "the rules compose correctly under this
 * schedule", never "the daemon does this".  Which brings us to:
 *
 * THE STEP-DOWN RULE IS NOT WIRED IN THE DAEMON.  clterm.h states rule
 * 3 - "a master that observes a HIGHER term steps down immediately and
 * unconditionally" - but cluster.c's map handler only LOGS it:
 *
 *     if (C.role == PC_ROLE_MASTER && ... m.term > pc_term_current()) {
 *             LM_WARN("... the step-down rule is not wired yet\n");
 *     }
 *     C.map = m;  C.map_valid = 1;      // adopts the map
 *                                       // ...and never touches C.role
 *
 * The master adopts the newer map, keeps the role, and beat_pump() goes
 * on sending M_MASTER_ALIVE.  So this runs BOTH models:
 *
 *   MODE_WIRED    rule 3 as clterm.h specifies
 *   MODE_ASIS     rule 3 as cluster.c behaves today (adopt, keep role)
 *
 * and asserts that the second one really does strand two masters.  That
 * is the fail-first half: a simulator that cannot demonstrate the bug it
 * is meant to guard against has proved nothing when it later goes green.
 *
 * usage: ./clustersim [seeds]        (default 2000)
 */
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
	/* Address ordering, drawn from the SEED rather than taken from the
	 * node index.  Index would be an artifact: node 0 always founds the
	 * cluster, so the freshly promoted node always had the higher index
	 * and a stale master could never win the address tiebreak - the
	 * as-is arm reported zero stale winners for a reason that was
	 * entirely the model's. */
	int      addr;
	uint32_t term;                 /* highest term this node has SEEN */
	uint32_t map_term, map_seq;    /* the epoch it currently holds */
	/* The term this node CLAIMED when it promoted, which is NOT the
	 * same as the epoch it holds: a stale master adopts a newer map
	 * (cluster.c: C.map = m) while keeping its role, so map_term rises
	 * and stops witnessing how stale the MASTER is.  Asking the wrong
	 * one of these made the as-is arm report zero stale winners. */
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

/* @a publishes its map to everyone it can reach, with per-link loss and
 * delay drawn from the seed.  Reorder is implicit: delays differ per
 * link, and the queue is drained by delivery tick. */
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

/*
 * THE KEEPALIVE CURE, which the first version of this file left out
 * entirely - and that omission made it report a bug the daemon does not
 * have.  handle_master_alive ranks two masters by member count and then
 * by ADDRESS, which is a total order, so it resolves an equal-term
 * collision that rule 3 (a strict inequality) cannot touch.  Modelling
 * rule 3 alone showed 1296/2000 equal-term schedules "unresolved"; the
 * daemon resolves them in about two seconds.  A simulator that raises
 * false alarms is no better than one that goes falsely green.
 *
 * Node index stands in for address ordering: higher index wins, exactly
 * as `addr_cmp(from, &C.self_addr) > 0` yields to the higher address.
 */
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
	/* the term always goes through the observation path first: a node
	 * learns from a term even when it refuses the map (cluster.c calls
	 * pc_term_observe before the epoch comparison) */
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
	/* MODE_ASIS: the map is adopted and the role is NOT touched -
	 * exactly what cluster.c does today */
}

static int masters(const struct sim *s)
{
	int i, n = 0;

	for (i = 0; i < s->nn; i++)
		if (s->n[i].up && s->n[i].is_master)
			n++;
	return n;
}

/* Which fault this run stages.  They are NOT the same scenario and
 * conflating them is how the first version of this file produced a
 * meaningless red:
 *
 *  SC_PARTITION  the fleet splits.  A master that is merely isolated
 *                STAYS master at its existing term - it does not
 *                re-promote itself - so the side without one promotes
 *                and the two sides end at DIFFERENT terms.  Rule 3 has
 *                a strict inequality to work with, and must converge.
 *  SC_DUALCLAIM  the master DIES and the fleet splits in the same
 *                window, so both sides promote from the same highest
 *                seen term and claim the SAME next one.  Rule 3 is
 *                `theirs > mine`; equal terms give it nothing to act on.
 *                Reachable, and not covered by any rule in clterm.h. */
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
	/* Node 0 founds the cluster - and the fleet must AGREE before any
	 * fault is staged.  Without this the run is not testing what it
	 * says: with 25% loss, a node that never received the founder's
	 * map still believes term 0, so when it promotes it claims term 1
	 * and collides with the founder.  That is a real hazard, but it is
	 * the equal-term one that SC_DUALCLAIM exists to stage - letting it
	 * leak in here made 460 of 2000 partition runs fail for a reason
	 * that had nothing to do with rule 3. */
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
		/* Whichever side has no master promotes ONE node - the
		 * lowest id, which is what pick_backup() settles on.  The
		 * side that still HAS the master leaves it alone: an
		 * isolated master keeps serving at the term it holds. */
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

	/* HEAL, then let everyone re-publish until the queue drains.  The
	 * keepalive runs EVERY tick and the map only when a master
	 * publishes - the 1 Hz vs on-change asymmetry that decided the real
	 * outcome, so the model has to carry it. */
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
	/* Did a STALE master win?  Counting masters is not the question -
	 * the keepalive cure converges the fleet either way, it just picks
	 * the wrong node when nothing is term-aware.  This is the same
	 * correction stepdowntest needed. */
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

	/*
	 * FAIL-FIRST, and NOT on the master count.  The keepalive cure is a
	 * total order, so it converges the fleet with or without anything
	 * term-aware - it just seats the wrong node.  Counting masters
	 * therefore cannot tell the two builds apart, which is exactly the
	 * false green the first stepdowntest walked into.  The
	 * discriminating question is WHO won.
	 */
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

	/* ---- 3. the equal-term dual claim ------------------------------
	 * Rule 3 is a strict inequality, so two nodes promoting from the
	 * same seen term hold the SAME term and it has nothing to say.  The
	 * member-count/address cure does - it is a total order - and the
	 * daemon resolves this in about two seconds
	 * (test/dualclaimtest.sh).  Modelling rule 3 ALONE made this file
	 * report 1296/2000 "unresolved": a bug the product does not have.
	 */
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

		/* the guard lives in pc_term_observe (stateful); what is
		 * asserted here is the pure half: an absurd term still
		 * compares as AHEAD, so the guard - not the comparison - is
		 * what must refuse it.  A silent clamp in cmp would hide it. */
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
