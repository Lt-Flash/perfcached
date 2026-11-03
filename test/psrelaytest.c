/*
 * psrelaytest.c - the relay's sequence accounting (src/psrelay.h), driven
 * deterministically: reordering is not loss, a real gap is counted once and
 * exactly, a duplicate is dropped, a restart is not a gap.  The first block
 * runs the SHIPPED 0.4.0-rc1 rule on the same arrivals to show what it got
 * wrong - that rule is written out here as the reference it was.
 */
#include <stdio.h>
#include <string.h>
#include "psrelay.h"

static int pass, fail;
#define CHK(c, m) do { if (c) pass++; else { fail++; printf("  FAIL %s\n", m); } } while (0)

/* 0.4.0-rc1's receiver rule, as shipped: one high-water mark per node */
static unsigned long long rc1_seen;
static int rc1_used;
static unsigned long long rc1_accept(unsigned long long seq)
{
	unsigned long long lost = 0;

	if (!rc1_used) { rc1_used = 1; rc1_seen = seq; return 0; }
	if (seq > rc1_seen + 1)
		lost = seq - rc1_seen - 1;
	if (seq > rc1_seen)
		rc1_seen = seq;
	return lost;
}

static void feed(struct psr_lane *l, const uint64_t *c, int n, uint16_t ep,
		uint64_t *lost, int *delivered, int *dups)
{
	int i;

	for (i = 0; i < n; i++) {
		if (psr_accept(l, psr_seq(ep, 3, c[i]), lost) == PSR_DELIVER)
			(*delivered)++;
		else
			(*dups)++;
	}
}

int main(void)
{
	struct psr_lane l;
	uint64_t lost, c[4096];
	unsigned long long rc1_lost;
	int i, n, dl, dup;

	/* ---- the bug: adjacent swaps, nothing missing ---- */
	for (i = 0, n = 0; i < 1000; i += 2) {
		c[n++] = (uint64_t)i + 2;
		c[n++] = (uint64_t)i + 1;
	}
	rc1_lost = 0; rc1_used = 0;
	for (i = 0; i < n; i++)
		rc1_lost += rc1_accept(c[i]);
	CHK(rc1_lost > 0, "rc1's rule counts losses on pure reordering (the bug this replaces)");
	printf("  rc1's rule on 1000 swapped arrivals, none missing: lost %llu\n", rc1_lost);
	memset(&l, 0, sizeof l); lost = 0; dl = dup = 0;
	feed(&l, c, n, 7, &lost, &dl, &dup);
	CHK(lost == 0 && dl == 1000 && dup == 0, "swaps: nothing lost, all 1000 delivered, no duplicate");

	/* ---- in order ---- */
	for (i = 0; i < 1000; i++) c[i] = (uint64_t)i + 1;
	memset(&l, 0, sizeof l); lost = 0; dl = dup = 0;
	feed(&l, c, 1000, 7, &lost, &dl, &dup);
	CHK(lost == 0 && dl == 1000 && dup == 0, "in order: nothing lost");

	/* ---- reordering by up to 63 inside the window ---- */
	for (i = 0, n = 0; i < 640; i += 64) {
		int k;
		for (k = 63; k >= 0; k--) c[n++] = (uint64_t)(i + k + 1);
	}
	memset(&l, 0, sizeof l); lost = 0; dl = dup = 0;
	feed(&l, c, n, 7, &lost, &dl, &dup);
	CHK(lost == 0 && dl == 640 && dup == 0, "blocks of 64 sent backwards: nothing lost");

	/* ---- one real gap: counted once, and only after it leaves the window ---- */
	for (i = 0, n = 0; i < 300; i++) if (i + 1 != 100) c[n++] = (uint64_t)i + 1;
	memset(&l, 0, sizeof l); lost = 0; dl = dup = 0;
	feed(&l, c, 150, 7, &lost, &dl, &dup);       /* up to 151: 100 still inside */
	CHK(lost == 0, "a gap still inside the window is not yet counted");
	feed(&l, c + 150, n - 150, 7, &lost, &dl, &dup);
	CHK(lost == 1 && dl == 299, "the gap is counted exactly once when it leaves the window");

	/* ---- a burst loss bigger than the window ---- */
	for (i = 0, n = 0; i < 2000; i++) if (i < 500 || i >= 1500) c[n++] = (uint64_t)i + 1;
	memset(&l, 0, sizeof l); lost = 0; dl = dup = 0;
	feed(&l, c, n, 7, &lost, &dl, &dup);
	/* 501..1437 never entered the window after the jump to 1501 (937);
	 * 1438..1500 did, and left it one by one as 1502..1564 arrived (63) */
	CHK(lost == 1000 && dl == 1000, "a 1000-message hole bigger than the window: exactly 1000 counted");

	/* ---- duplicates ---- */
	memset(&l, 0, sizeof l); lost = 0; dl = dup = 0;
	for (i = 0; i < 100; i++) c[i] = (uint64_t)i + 1;
	feed(&l, c, 100, 7, &lost, &dl, &dup);
	feed(&l, (uint64_t[]){ 100, 99, 40 }, 3, 7, &lost, &dl, &dup);
	CHK(dup == 3 && dl == 100 && lost == 0, "a repeat inside the window is dropped as a duplicate");

	/* ---- a restart is a new epoch, not a gap ---- */
	memset(&l, 0, sizeof l); lost = 0; dl = dup = 0;
	for (i = 0; i < 4096; i++) c[i] = (uint64_t)i + 1;
	feed(&l, c, 4096, 7, &lost, &dl, &dup);
	feed(&l, c, 200, 8, &lost, &dl, &dup);          /* restarted: 1..200 again */
	CHK(lost == 0 && dup == 0 && dl == 4296, "a restarted sender (new epoch) counting from 1 again: delivered, not duplicates, not gaps");
	feed(&l, (uint64_t[]){ 202, 300, 400 }, 3, 8, &lost, &dl, &dup);
	/* missing: 201, 203..299, 301..399 (197); 337..399 (63) are still
	 * inside the window below 400, so 134 have been counted */
	CHK(lost == 134, "and after the restart a real gap is counted again, exactly (134 of 197 have left the window)");

	/* ---- the first sighting is not a loss of what came before ---- */
	memset(&l, 0, sizeof l); lost = 0; dl = dup = 0;
	feed(&l, (uint64_t[]){ 1000, 999, 1001, 1100 }, 4, 7, &lost, &dl, &dup);
	CHK(dl == 4 && dup == 0, "a late arrival just below the first sighting is delivered, not taken for a duplicate");
	/* the gap 1002..1099: 1002..1036 left the window when 1100 arrived (35),
	 * 1037..1099 are still inside it */
	CHK(lost == 35, "and only the part of the true gap that left the window is counted (35 of 98)");

	/* ---- two senders' lanes, interleaved, each in its own state ---- */
	{
		struct psr_lane lane[256];
		memset(lane, 0, sizeof lane); lost = 0;
		for (i = 1; i <= 200; i++) {
			uint64_t s1 = psr_seq(7, 1, (uint64_t)i), s2 = psr_seq(7, 2, (uint64_t)i);
			psr_accept(&lane[psr_lane(s1)], s1, &lost);
			psr_accept(&lane[psr_lane(s2)], s2, &lost);
		}
		CHK(lost == 0, "two lanes interleaved with the same counters: nothing lost");
		CHK(psr_lane(psr_seq(9, 200, 5)) == 200 && psr_epoch(psr_seq(9, 200, 5)) == 9 &&
			psr_counter(psr_seq(9, 200, PSR_COUNTER_MASK + 5)) == 4,
			"epoch, lane and counter round-trip; the counter wraps at 40 bits");
	}

	/* ---- PS11: the path to one peer ---- */
	{
		struct psr_path pa;
		long long now = 1000000;
		int ev, logs = 0;

		memset(&pa, 0, sizeof pa);
		CHK(!psr_path_direct(&pa, now) && !psr_path_probe_due(&pa, now, 42),
			"path: no advertised port, no probe, not direct");
		psr_path_port(&pa, 17600);
		CHK(psr_path_probe_due(&pa, now, 42) && !psr_path_direct(&pa, now),
			"path: an advertised port is probed at once and is not yet direct");
		CHK(!psr_path_probe_due(&pa, now + 500, 43), "path: not probed again within a second");
		CHK(!psr_path_answer(&pa, 999, now + 10) && !psr_path_direct(&pa, now + 10),
			"path: an answer with the wrong nonce proves nothing");
		/* a port that never answers: probed every second, never direct */
		for (i = 1; i <= 120; i++) {
			long long t0 = now + (long long)i * 1000;

			psr_path_probe_due(&pa, t0, (uint64_t)(100 + i));
			ev = psr_path_transition(&pa, t0);
			logs += ev != 0;
		}
		CHK(!psr_path_direct(&pa, now + 120000) && logs == 0,
			"path: a port that never answers stays on the cluster socket for two minutes, silently");
		now += 121000;
		CHK(psr_path_probe_due(&pa, now, 777) && psr_path_answer(&pa, 777, now + 3) &&
			psr_path_direct(&pa, now + 3), "path: the first right answer makes it direct");
		CHK(psr_path_transition(&pa, now + 3) == 1 && psr_path_transition(&pa, now + 4) == 0,
			"path: becoming direct is reported once");
		CHK(!psr_path_probe_due(&pa, now + 500, 778) && psr_path_probe_due(&pa, now + 1010, 779),
			"path: while direct it is re-confirmed every second");
		/* the answers stop.  Ticks run a little over a second apart and
		 * each probes before it decides: the tick that sends the third
		 * unanswered probe must still be direct */
		{
			int k, flips = 0, fell = 0, direct3 = 0;

			for (k = 2; k <= 4; k++) {
				long long t = now + 1010LL * k;

				psr_path_probe_due(&pa, t, (uint64_t)(780 + k));
				ev = psr_path_transition(&pa, t);
				flips += ev != 0;
				fell += ev == -1;
				if (k == 3)
					direct3 = psr_path_direct(&pa, t);
			}
			CHK(direct3 && flips == 1 && fell == 1,
				"path: two unanswered probes are tolerated, the third falls back, reported once");
		}
		CHK(psr_path_direct(&pa, now + 3 + 3499) && !psr_path_direct(&pa, now + 3 + 3500),
			"path: senders leave it 3.5 s after the last answer");
		CHK(psr_path_transition(&pa, now + 5000) == 0, "path: the fallback is not reported again");
		psr_path_probe_due(&pa, now + 6000, 900);
		psr_path_answer(&pa, 900, now + 6001);
		CHK(psr_path_direct(&pa, now + 6001), "path: an answer after a fallback restores it");
		psr_path_port(&pa, 17601);
		CHK(!psr_path_direct(&pa, now + 6002) && !psr_path_answer(&pa, 900, now + 6002),
			"path: a changed port is unproven again, and an old nonce does not prove it");
		psr_path_port(&pa, 0);
		CHK(!psr_path_direct(&pa, now + 6003) && !psr_path_probe_due(&pa, now + 15000, 5),
			"path: a withdrawn port stops the probes");
	}

	printf("psrelaytest: %d passed, %d failed\n", pass, fail);
	return fail != 0;
}
