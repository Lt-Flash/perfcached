/*
 * clpendtest.c - parked requests and the completion queues (M2).
 *
 * The parked-request id is a HANDLE, not a ticket, and almost every case
 * here is a way that distinction could be lost:
 *   - a reply arriving after its slot was recycled, completing somebody
 *     else's request (the ABA guard, A4)
 *   - occupancy drifting, so a peak sized from measurement can never be
 *     reached and "full" has to be special-cased (B2)
 *   - a completion dropped into a full queue, leaving a request to
 *     resolve only by timing out - which the CQ/table sizing is what
 *     makes unreachable (D2)
 *   - a deadline that fires against a real clock, which is the same
 *     class of flake as a device-dependent fsync count (E, all injected)
 *
 * Case group H (the answers state machine) is NOT here: deciding what a
 * peer's reply means still reaches the locator, the peer table, the wire
 * and the store inside cluster.c, so clpend_answer() waits on M3 and M6.
 * doc/MODULARITY.md's appendix carries the full catalogue.
 *
 * Build: cc -o clpendtest test/clpendtest.c src/clpend.o -lpthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/clpend.h"

static int pass, fail;
static void chk(int cond, const char *what)
{
	if (cond) { pass++; return; }
	fail++;
	printf("  FAIL %s\n", what);
}

static struct clpend P;
#define CAP 64
#define TMO 50

static uint32_t park(int expect, int extra, int worker, long long now,
		struct pending **out)
{
	int reason = -1;

	return clpend_alloc(&P, PC_DONE_PULL, expect, extra, worker, now,
		TMO, out, &reason);
}

struct racer { int n; };
static void *race_alloc(void *arg)
{
	struct racer *r = arg;
	struct pending *sl;
	int reason;

	for (;;) {
		if (!clpend_alloc(&P, PC_DONE_PULL, 1, 0, 0, 1000, TMO, &sl,
		        &reason))
			break;
		r->n++;
	}
	return NULL;
}
static void *race_churn(void *arg)
{
	struct pending *sl;
	int reason, i;

	(void)arg;
	for (i = 0; i < 5000; i++)
		if (clpend_alloc(&P, PC_DONE_PULL, 1, 0, 0, 1000, TMO, &sl,
		        &reason))
			clpend_release(&P, sl);
	return NULL;
}

int main(void)
{
	struct pending *sl, *sl2;
	struct pc_pull_done d[4];
	uint32_t req, old;
	int i, n;
	long long now = 1000;

	printf("=== clpendtest ===\n");
	chk(clpend_init(&P, CAP) == CAP, "init returns the cap it used");

	/* ---- A. handle encoding ---------------------------------------- */
	req = park(1, 0, 3, now, &sl);
	chk(req && (req & PEND_TAG), "A1 a handle carries the tag");
	chk((req & PEND_SLOT_MASK) < (uint32_t)CAP, "A1 its slot is in range");
	chk(clpend_find(&P, req) == sl && sl->req == req, "A2 find returns the slot");
	chk(!clpend_find(&P, req & ~PEND_TAG), "A3 an untagged id never finds");
	old = req;
	clpend_release(&P, sl);
	req = park(1, 0, 3, now, &sl2);
	chk(sl2 == sl, "A4 the freed slot is the next one taken");
	chk(req != old, "A4 but its handle differs");
	chk(!clpend_find(&P, old), "A4 the OLD handle no longer finds - the ABA guard");
	chk(clpend_find(&P, req) == sl2, "A4 the new one does");
	clpend_release(&P, sl2);
	for (i = 0; i < 300; i++) {           /* A5: gen never lands on 0 */
		req = park(1, 0, 0, now, &sl);
		chk((req >> PEND_GEN_SHIFT) & (PEND_GEN_MASK >> PEND_GEN_SHIFT) ?
			1 : 0, "A5 every recycled handle has a non-zero generation");
		clpend_release(&P, sl);
	}
	chk(!clpend_find(&P, PEND_TAG | (1u << PEND_GEN_SHIFT) | (CAP + 5)),
		"A6 a slot index beyond cap finds nothing");

	/* ---- B. occupancy and peak -------------------------------------- */
	chk(clpend_used(&P) == 0, "B1 used is zero when nothing is parked");
	for (i = 0; i < CAP; i++)
		chk(park(1, 0, 0, now, &sl) != 0, "B fill parks");
	chk(clpend_used(&P) == CAP, "B1 used counts every parked slot");
	chk(clpend_peak(&P) == CAP, "B2 peak CAN reach cap");
	req = park(1, 0, 0, now, &sl);
	chk(req == 0, "B4 a full table refuses");
	{
		int r2 = -1;
		unsigned long long ex = clpend_exhausted(&P);

		clpend_alloc(&P, PC_DONE_PULL, 1, 0, 0, now, TMO, &sl, &r2);
		chk(r2 == PC_CLFAIL_BUSY, "B4 and reports BUSY as the reason");
		chk(clpend_exhausted(&P) > ex, "B4 exhausted counts the refusal");
	}
	sl = clpend_find(&P, PEND_TAG | (1u << PEND_GEN_SHIFT) | 0u);
	if (!sl)
		for (i = 0; i < CAP; i++)
			if (P.pend[i].req) { sl = &P.pend[i]; break; }
	n = clpend_used(&P);
	clpend_release(&P, sl);
	chk(clpend_used(&P) == n - 1, "B3 release decrements once");
	clpend_release(&P, sl);
	chk(clpend_used(&P) == n - 1, "B3 a second release is a no-op");
	chk(park(1, 0, 0, now, &sl) != 0, "B5 after a release the next alloc succeeds");
	chk(clpend_peak(&P) == CAP, "B5 and peak is unchanged");
	clpend_fini(&P);

	/* ---- C. alloc fields -------------------------------------------- */
	clpend_init(&P, CAP);
	req = park(3, 700, 7, 1000, &sl);
	chk(sl->kind == PC_DONE_PULL && sl->expect == 3 && sl->worker == 7,
		"C1 kind, expect and worker are stored as given");
	chk(sl->deadline_ms == 1000 + TMO + 700,
		"C2 deadline is now + pull_timeout + extra");
	chk(!sl->answered && !sl->first_node && !sl->probe_op && !sl->stash &&
		!sl->collen && !sl->klen,
		"C3 every other field starts zeroed");

	/* ---- D. completion queue ---------------------------------------- */
	chk(clpend_cq_drain(&P, 0, d, 4) == 0, "D4 before register, drain is empty");
	clpend_cq_post(&P, 0, 1, PC_DONE_PULL, 1, 1, 0, 2, 0, NULL, 0, 0, 0, 0, 0);
	chk(clpend_cq_drain(&P, 0, d, 4) == 0, "D4 and a post before register is a safe no-op");
	clpend_cq_register(&P, 0, -1);
	clpend_cq_post(&P, 0, 42, PC_DONE_PULL, 1, 1, 99, 5, 7, NULL, 0, 0, 0, 0, 0);
	n = clpend_cq_drain(&P, 0, d, 4);
	chk(n == 1 && d[0].req == 42 && d[0].found == 1 && d[0].newval == 99 &&
		d[0].from_node == 5 && d[0].ttl_left == 7, "D1 a posted completion drains intact");
	chk(clpend_cq_drain(&P, 0, d, 4) == 0, "D1 and the queue is then empty");
	clpend_cq_register(&P, 1, -1);
	clpend_cq_post(&P, 0, 7, PC_DONE_PULL, 1, 1, 0, 0, 0, NULL, 0, 0, 0, 0, 0);
	chk(clpend_cq_drain(&P, 1, d, 4) == 0, "D3 a post to worker 0 does not reach worker 1");
	(void)clpend_cq_drain(&P, 0, d, 4);
	for (i = 0; i < CAP; i++)
		clpend_cq_post(&P, 0, (uint32_t)i, PC_DONE_PULL, 1, 1, 0, 0, 0,
			NULL, 0, 0, 0, 0, 0);
	clpend_cq_post(&P, 0, 9999, PC_DONE_PULL, 1, 1, 0, 0, 0, NULL, 0, 0, 0, 0, 0);
	{
		int total = 0, saw_over = 0, k;

		while ((n = clpend_cq_drain(&P, 0, d, 4)) > 0) {
			for (k = 0; k < n; k++)
				if (d[k].req == 9999)
					saw_over = 1;
			total += n;
		}
		chk(total == CAP, "D2a the ring holds exactly cap completions");
		chk(!saw_over, "D2a the one past cap is DROPPED, never written over "
			"a live entry - this is what makes the silent drop unreachable");
	}
	{
		const unsigned char v[] = "hello";

		clpend_cq_post(&P, 0, 5, PC_DONE_PULL, 1, 1, 0, 0, 0, v, 5, 0, 0, 0, 0);
		chk(clpend_cq_drain(&P, 0, d, 1) == 1 && d[0].vlen == 5 &&
			d[0].val && d[0].val != (char *)v &&
			!memcmp(d[0].val, v, 5), "D5 the value is copied, not aliased");
		free(d[0].val);
	}
	clpend_fini(&P);

	/* ---- E. expiry, clock injected ---------------------------------- */
	clpend_init(&P, CAP);
	clpend_cq_register(&P, 0, -1);
	req = park(1, 0, 0, 1000, &sl);
	chk(clpend_expire(&P, 1000 + TMO - 1) == 0, "E1 before the deadline nothing expires");
	chk(clpend_find(&P, req) == sl, "E1 and the slot is still live");
	chk(clpend_expire(&P, 1000 + TMO) == 1, "E2 at the deadline it times out");
	chk(!clpend_find(&P, req) && clpend_used(&P) == 0, "E2 the slot is freed");
	n = clpend_cq_drain(&P, 0, d, 4);
	chk(n == 1 && d[0].req == req && d[0].timedout == 1,
		"E2 a timeout completion is posted");
	req = park(1, 0, 0, 1000, &sl);
	sl->answered = 1;
	chk(clpend_expire(&P, 9999) == 0, "E3 an answered slot times out silently");
	chk(clpend_cq_drain(&P, 0, d, 4) == 0, "E3 with no second completion");
	req = park(1, 0, 0, 1000, &sl);
	sl->probe_op = 1;
	sl->stash = malloc(16);              /* E4: ASan sees the leak if kept */
	sl->stash_len = 16;
	chk(clpend_expire(&P, 9999) == 1, "E4 a probe times out");
	n = clpend_cq_drain(&P, 0, d, 4);
	chk(n == 1 && d[0].kind == PC_DONE_SET_RESUME && d[0].timedout == 1,
		"E4 as an honest refusal, not a fork");
	clpend_fini(&P);

	/* ---- F. concurrency --------------------------------------------- */
	clpend_init(&P, CAP);
	{
		pthread_t t1, t2;
		struct racer r1 = { 0 }, r2 = { 0 };

		pthread_create(&t1, NULL, race_alloc, &r1);
		pthread_create(&t2, NULL, race_alloc, &r2);
		pthread_join(t1, NULL);
		pthread_join(t2, NULL);
		chk(r1.n + r2.n == CAP, "F1 two threads allocating share exactly cap slots");
		chk(clpend_used(&P) == CAP, "F1 and used agrees");
	}
	clpend_fini(&P);
	clpend_init(&P, CAP);
	{
		pthread_t t1, t2;

		pthread_create(&t1, NULL, race_churn, NULL);
		pthread_create(&t2, NULL, race_churn, NULL);
		pthread_join(t1, NULL);
		pthread_join(t2, NULL);
		chk(clpend_used(&P) == 0, "F2 alloc/release churn leaks no slot");
	}
	clpend_fini(&P);

	/* ---- G. init clamps ---------------------------------------------- */
	chk(clpend_init(&P, 1) == PEND_MIN, "G1 a tiny cap is raised to PEND_MIN");
	clpend_fini(&P);
	chk(clpend_init(&P, PEND_LIMIT + 1000) == PEND_LIMIT,
		"G2 a cap above the id's reach is clamped to PEND_LIMIT");
	clpend_fini(&P);
	chk(clpend_init(&P, 0) == PEND_DEFAULT, "G1 zero means the default");
	clpend_fini(&P);

	/* ---- H. the answer path ------------------------------------------- */
	clpend_init(&P, CAP);
	{
		struct clpend_ans_out a;
		struct pending *sl, *sl2;
		uint32_t req, req2;
		int r;

		/* H1  expect 3: the first positive completes, once */
		req = park(3, 0, 7, 1000, &sl);
		r = clpend_answer(&P, req, 1, 42, 9999, &a);
		chk(r == CLPEND_ANS_HIT, "H1 the first positive is a hit");
		chk(a.worker == 7, "H1 it names the worker to wake");
		chk(a.from_node == 42, "H1 and the node that answered");
		chk(sl->answered == 1 && sl->first_node == 42,
			"H1 the slot records that it was answered, and by whom");
		chk(clpend_find(&P, req) == sl,
			"H1 the slot LINGERS so a second positive can be seen");

		/* H1b  a later positive posts NO second completion.  The
		 * appendix said IGNORE; the tree demotes instead, which is
		 * the behaviour asserted here - see doc/MODULARITY.md. */
		r = clpend_answer(&P, req, 1, 9, 9999, &a);
		chk(r == CLPEND_ANS_RACE,
			"H1 a second positive is a birth race, not a completion");
		chk(a.winner == 9 && a.loser == 42,
			"H1 the lower node id wins and the other is demoted");

		/* H2  expect 3, three negatives: WAIT, WAIT, miss */
		req = park(3, 0, 3, 1000, &sl);
		chk(clpend_answer(&P, req, 0, 1, 0, &a) == CLPEND_ANS_WAIT,
			"H2 a negative with answers still due waits");
		chk(clpend_answer(&P, req, 0, 2, 0, &a) == CLPEND_ANS_WAIT,
			"H2 and so does the second");
		r = clpend_answer(&P, req, 0, 3, 0, &a);
		chk(r == CLPEND_ANS_MISS, "H2 the last negative is a miss");
		chk(a.worker == 3, "H2 the miss names the worker to wake");
		chk(!clpend_find(&P, req), "H2 and the slot is freed");

		/* H3  a late positive after the miss */
		chk(clpend_answer(&P, req, 1, 5, 0, &a) == CLPEND_ANS_IGNORE,
			"H3 a positive arriving after the miss is ignored");

		/* H4  an answer for a stale generation (A4 through answer) */
		req = park(1, 0, 0, 1000, &sl);
		clpend_release(&P, sl);
		req2 = park(1, 0, 0, 1000, &sl2);
		chk(sl2 == sl && req2 != req,
			"H4 the next park reuses the slot with a new generation");
		chk(clpend_answer(&P, req, 1, 1, 0, &a) == CLPEND_ANS_IGNORE,
			"H4 an answer for the stale handle is ignored");
		chk(clpend_find(&P, req2) == sl2,
			"H4 and the live request is untouched by it");
		clpend_release(&P, sl2);

		/* H5  THE REGRESSION.  Everything the answer hands back is
		 * copied out of the slot.  cluster.c used to keep
		 * `rec_col = p->col` - a bare pointer - and read it after
		 * the release and after the lock was dropped, so a worker
		 * reusing the slot changed which collection was deleted
		 * from.  Reuse the slot here and the copy must not move. */
		req = park(1, 0, 0, 1000, &sl);
		sl->kind = PC_DONE_RECONCILE;
		memcpy(sl->col, "orders", 6);
		sl->collen = 6;
		memcpy(sl->key, "k1", 2);
		sl->klen = 2;
		r = clpend_answer(&P, req, 0, 1, 0, &a);
		chk(r == CLPEND_ANS_RECONCILE,
			"H5 a lone negative on a reconcile drops the record");
		chk(a.collen == 6 && !memcmp(a.col, "orders", 6),
			"H5 the collection comes back copied");
		chk(a.klen == 2 && !memcmp(a.key, "k1", 2),
			"H5 and so does the key");
		req2 = park(1, 0, 0, 1000, &sl2);
		chk(sl2 == sl, "H5 the next park reuses that very slot");
		memcpy(sl2->col, "XXXXXX", 6);
		sl2->collen = 6;
		chk(!memcmp(a.col, "orders", 6),
			"H5 and reusing the slot does NOT change the answer");
		clpend_release(&P, sl2);

		/* H6  a probe that missed fleet-wide hands the stash over */
		req = park(1, 0, 4, 1000, &sl);
		sl->probe_op = 1;
		sl->stash = malloc(4);
		memcpy(sl->stash, "abc", 4);
		sl->stash_len = 4;
		r = clpend_answer(&P, req, 0, 1, 0, &a);
		chk(r == CLPEND_ANS_RESUME,
			"H6 a probe nobody holds resumes the write");
		chk(a.stash && a.stash_len == 4 && !memcmp(a.stash, "abc", 4),
			"H6 the stash travels with the answer");
		chk(!sl->stash, "H6 and the slot no longer owns it");
		free(a.stash);

		/* H7  a probe HIT transforms the park into a forward */
		req = park(1, 0, 4, 1000, &sl);
		sl->probe_op = 2;
		memcpy(sl->col, "c", 1);
		sl->collen = 1;
		memcpy(sl->key, "k", 1);
		sl->klen = 1;
		r = clpend_answer(&P, req, 1, 11, 555555, &a);
		chk(r == CLPEND_ANS_PROBE_FWD,
			"H7 a probe hit becomes a forward to the holder");
		chk(sl->probe_fwd == 1 && sl->probe_op == 0,
			"H7 the slot is a forward now, not a probe");
		chk(sl->answered == 0,
			"H7 answered stays 0 - the holder's ack completes it");
		chk(sl->deadline_ms == 555555,
			"H7 the deadline is the caller's, not a stored clock");
		chk(sl->expect > (1 << 20),
			"H7 a stray negative cannot free it under the ack");
		chk(a.kind == PC_DONE_FWD_ADD, "H7 probe_op 2 becomes FWD_ADD");
		chk(clpend_find(&P, req) == sl,
			"H7 the same req still finds it, for the ack");
	}
	clpend_fini(&P);

	/* ---- I. clpend_retire: the forward-ack completion (M11 slice 2) ---
	 * The FWD ack and the JSON ack retire a parked request the same way
	 * except for one thing: the FWD ack refuses one already answered.
	 * That difference used to be split across two handlers three
	 * thousand lines apart; here it is a parameter, and testable. */
	clpend_init(&P, CAP);
	{
		struct pending *sl;
		uint32_t req;
		int w = -1, kd = -1;

		/* I1. an unanswered request retires and names its worker */
		req = park(1, 0, 9, 1000, &sl);
		sl->kind = PC_DONE_FWD_SET;
		chk(clpend_retire(&P, req, 1, &w, &kd) == 1,
			"I1 an unanswered request retires");
		chk(w == 9, "I1 and names the worker to wake");
		chk(kd == PC_DONE_FWD_SET, "I1 and the kind it was parked as");
		chk(!clpend_find(&P, req), "I1 the slot is freed");

		/* I2. a DUPLICATE ack retires nothing */
		w = -1;
		chk(clpend_retire(&P, req, 1, &w, &kd) == 0,
			"I2 a duplicate ack retires nothing");
		chk(w == -1, "I2 and leaves the caller's worker untouched");

		/* I3. only_unanswered REFUSES an answered request - the case
		 * that stops a stray second ack completing a probe-forward
		 * twice */
		req = park(1, 0, 3, 1000, &sl);
		sl->answered = 1;
		chk(clpend_retire(&P, req, 1, &w, &kd) == 0,
			"I3 only_unanswered refuses an ANSWERED request");
		chk(clpend_find(&P, req) == sl, "I3 and leaves the slot alone");

		/* I4. ... and the JSON path, which does not check, retires it */
		chk(clpend_retire(&P, req, 0, &w, &kd) == 1,
			"I4 without only_unanswered it retires anyway");
		chk(w == 3, "I4 naming the worker");
		chk(!clpend_find(&P, req), "I4 and frees the slot");

		/* I5. an ack for something never parked */
		chk(clpend_retire(&P, 0x7fffffff, 1, &w, &kd) == 0,
			"I5 an ack for an unknown request retires nothing");

		/* I6. a stale generation - the slot was reused */
		req = park(1, 0, 5, 1000, &sl);
		clpend_release(&P, sl);
		{
			struct pending *sl2;
			uint32_t req2 = park(1, 0, 6, 1000, &sl2);

			chk(sl2 == sl && req2 != req, "I6 the slot is reused");
			chk(clpend_retire(&P, req, 1, &w, &kd) == 0,
				"I6 an ack for the STALE handle retires nothing");
			chk(clpend_find(&P, req2) == sl2,
				"I6 and the live request is untouched");
		}
	}
	clpend_fini(&P);

	printf("clpendtest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
