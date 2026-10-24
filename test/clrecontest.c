/* clrecontest.c - the B4 reconcile's pacing (clrecon.c; the 0.7.2 split,
 * wave 1): pure functions over struct clrecon, the clock passed in.
 *
 *   A  the second's budget (S393): a quarter of the pending table's free
 *      slots, never under RECONCILE_PER_TICK nor over the ceiling; a new
 *      refusal or a new pull timeout holds it at the floor for
 *      RECONCILE_CALM_MS, then the room counts again
 *   B  the slice's share: renewed once a second (and said so), the share
 *      rounded up - 64 a second is 7 a 100-ms slice, not 6
 *   C  the debt: a slice that overshot is paid by the next; one that owes
 *      more than its share is skipped and still owes the rest
 *   D  the rate and ETA (S395): 0 and 0 while the pass is not open, 0 and
 *      -1 while it is not walking; over the last minute's samples, not
 *      since the start; the ETA what the rate leaves of the total
 *   E  the no-witness clock (ST8): the seconds since the spell began, the
 *      warning once when it reaches RECON_NOWITNESS_WARN_S; stopping the
 *      clock keeps it said, a witness's return ends the spell
 *   F  the pump's gate: only while the beat lets the walk go on and the
 *      pass is on, a slice per RECONCILE_SLICE_MS
 *   G  the fast path (S392), the returning node's half: an ask's request
 *      id (never 0), the counts cleared and the try counted; the wait -
 *      hold while heard from, ask again when quiet, give up after
 *      FP_TRIES, done once every counted number is held; only the frames
 *      of the ask in flight count, and they are heard from; the replay's
 *      numbers - a repeat counted once, the bitmap grown past its first
 *      4 KB and to the last number, one past FP_SEEN_MAX refused, a new
 *      ask starting it afresh; giving up frees it
 *   H  the witness (S223): the death estimate - the later of the last write
 *      and the alive stamp, onto the monotonic clock, unknown without
 *      either, a wall clock behind both read as no time dead; the
 *      predicate at the grace's edge; the first live witness and the live
 *      peers counted to it (all of them when there is none), a dead peer
 *      and an empty slot never one; the count and the last id; the
 *      restart note (S274) - witnesses now, the wait for the next, a peer
 *      with no uptime never one, and the peers waiting on this node
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include "clpeers.h"
#include "clrecon.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

int main(void)
{
	struct clrecon r;
	unsigned long long rate;
	long long eta, secs;
	int renewed, b, k, warns;

	/* A */
	memset(&r, 0, sizeof r);
	CHK(clrecon_budget(&r, 1000, 4000, 0, 0, 1024) == 1000,
		"A: 4,000 free slots - a quarter, 1,000 a second");
	CHK(clrecon_budget(&r, 1000, 100, 0, 0, 1024) == RECONCILE_PER_TICK,
		"A: 100 free - the floor, %d", RECONCILE_PER_TICK);
	CHK(clrecon_budget(&r, 1000, 8000, 0, 0, 1024) == 1024, "A: 8,000 free - the ceiling, 1,024");
	CHK(clrecon_budget(&r, 2000, 4000, 1, 0, 1024) == RECONCILE_PER_TICK &&
		r.calm_until == 2000 + RECONCILE_CALM_MS, "A: a new refusal - the floor, calm for %d ms",
		RECONCILE_CALM_MS);
	CHK(clrecon_budget(&r, 2000 + RECONCILE_CALM_MS - 1, 4000, 1, 0, 1024) == RECONCILE_PER_TICK,
		"A: still calm a millisecond before it ends");
	CHK(clrecon_budget(&r, 2000 + RECONCILE_CALM_MS, 4000, 1, 0, 1024) == 1000,
		"A: then the room counts again");
	CHK(clrecon_budget(&r, 20000, 4000, 1, 7, 1024) == RECONCILE_PER_TICK &&
		r.calm_until == 20000 + RECONCILE_CALM_MS, "A: a new pull timeout calms it too");

	/* B */
	memset(&r, 0, sizeof r);
	b = clrecon_slice_budget(&r, 1000, 4000, 0, 0, 1024, &renewed);
	CHK(b == 100 && renewed && r.budget == 1000, "B: 1,000 a second - 100 a slice, renewed");
	b = clrecon_slice_budget(&r, 1999, 400, 0, 0, 1024, &renewed);
	CHK(b == 100 && !renewed, "B: inside the second - the same share, not renewed");
	b = clrecon_slice_budget(&r, 2000, 400, 0, 0, 1024, &renewed);
	CHK(b == 10 && renewed && r.budget == 100, "B: a second on - renewed from the room now (100: 10 a slice)");
	b = clrecon_slice_budget(&r, 3000, 0, 0, 0, 1024, &renewed);
	CHK(b == 7 && r.budget == RECONCILE_PER_TICK, "B: 64 a second - 7 a slice, rounded up");

	/* C */
	memset(&r, 0, sizeof r);
	CHK(clrecon_slice_take(&r, 100) == 100 && r.debt == 0, "C: no debt - the whole share");
	clrecon_owe(&r, -30);
	CHK(clrecon_slice_take(&r, 100) == 70 && r.debt == 0, "C: 30 overshot - the next slice 70");
	clrecon_owe(&r, -150);
	CHK(clrecon_slice_take(&r, 100) == -50 && r.debt == 50, "C: 150 owed - this slice skipped, 50 still owed");
	CHK(clrecon_slice_take(&r, 100) == 50 && r.debt == 0, "C: and the next pays it - 50");

	/* D */
	memset(&r, 0, sizeof r);
	clrecon_progress(&r, 1000, 0, 1, 10, 100, &rate, &eta);
	CHK(rate == 0 && eta == 0, "D: the pass not open - 0, 0");
	clrecon_progress(&r, 1000, 1, 0, 10, 100, &rate, &eta);
	CHK(rate == 0 && eta == -1 && r.rate_i == 0, "D: not walking yet - 0, -1, no sample taken");
	clrecon_progress(&r, 1000, 1, 1, 10, 0, &rate, &eta);
	CHK(rate == 0 && eta == -1, "D: no total yet - 0, -1");
	clrecon_progress(&r, 1000, 1, 1, 0, 10000, &rate, &eta);
	CHK(rate == 0 && eta == -1, "D: the first sample - no rate yet");
	clrecon_progress(&r, 2000, 1, 1, 500, 10000, &rate, &eta);
	CHK(rate == 500 && eta == 19, "D: 500 keys in a second - 500/s, 19 s for the 9,500 left");
	/* a minute at 10 a second, then a minute at 1,000: the rate is the
	 * last minute's, not the average since the start (~505) */
	memset(&r, 0, sizeof r);
	for (k = 0; k <= 60; k++)
		clrecon_progress(&r, 1000LL * (k + 1), 1, 1, 10ULL * (unsigned)k, 1000000, &rate, &eta);
	CHK(rate == 10, "D: a minute at 10 a second - 10/s");
	for (k = 1; k <= 60; k++)
		clrecon_progress(&r, 61000 + 1000LL * k, 1, 1, 600 + 1000ULL * (unsigned)k,
			1000000, &rate, &eta);
	CHK(rate == 1000 && eta == (1000000 - 60600) / 1000,
		"D: then a minute at 1,000 - 1,000/s over the last minute, the ETA from it");
	clrecon_progress(&r, 122000, 1, 1, 1000000, 1000000, &rate, &eta);
	CHK(eta == 0, "D: everything walked - 0 s left");

	/* E */
	memset(&r, 0, sizeof r);
	warns = 0;
	warns += clrecon_nowit(&r, 1000, &secs);
	CHK(secs == 0 && !warns, "E: the spell begins - 0 s");
	warns += clrecon_nowit(&r, 1000 + RECON_NOWITNESS_WARN_S * 1000LL - 1, &secs);
	CHK(secs == RECON_NOWITNESS_WARN_S - 1 && !warns, "E: a millisecond short - no warning");
	warns += clrecon_nowit(&r, 1000 + RECON_NOWITNESS_WARN_S * 1000LL, &secs);
	CHK(secs == RECON_NOWITNESS_WARN_S && warns == 1, "E: %d s - the warning", RECON_NOWITNESS_WARN_S);
	warns += clrecon_nowit(&r, 100000, &secs);
	CHK(secs == 99 && warns == 1, "E: later - counting, said once");
	clrecon_nowit_stop(&r);
	warns += clrecon_nowit(&r, 200000, &secs);
	warns += clrecon_nowit(&r, 200000 + RECON_NOWITNESS_WARN_S * 1000LL, &secs);
	CHK(secs == RECON_NOWITNESS_WARN_S && warns == 1, "E: the clock stopped and restarted - counts again, still said");
	clrecon_nowit_end(&r);
	warns += clrecon_nowit(&r, 300000, &secs);
	warns += clrecon_nowit(&r, 300000 + RECON_NOWITNESS_WARN_S * 1000LL, &secs);
	CHK(secs == RECON_NOWITNESS_WARN_S && warns == 2, "E: a witness came back between - the next spell warns again");

	/* F */
	memset(&r, 0, sizeof r);
	CHK(!clrecon_pump_due(&r, 5000, 1), "F: the beat has not let it walk - no slice");
	clrecon_walk_start(&r, 1000);
	CHK(!clrecon_pump_due(&r, 1000 + RECONCILE_SLICE_MS - 1, 1), "F: inside the beat's slice - none");
	CHK(clrecon_pump_due(&r, 1000 + RECONCILE_SLICE_MS, 1) && r.slice_ms == 1000 + RECONCILE_SLICE_MS,
		"F: %d ms on - a slice, its clock taken", RECONCILE_SLICE_MS);
	CHK(!clrecon_pump_due(&r, 1000 + 2 * RECONCILE_SLICE_MS, 0), "F: the pass off - none");
	clrecon_walk_stop(&r);
	CHK(!clrecon_pump_due(&r, 9000, 1), "F: the beat stopped it - none");

	/* G */
	{
		struct clrecon_fp f;
		uint32_t req;

		memset(&f, 0, sizeof f);
		req = clrecon_fp_asked(&f, 3, 1000);
		CHK(req == 0x5a5a3e80u && f.req == req && f.node == 3 && f.tries == 1 &&
			f.state == FPS_WAIT && f.n == -1 && !f.got && f.heard_ms == 1000,
			"G: an ask of node 3 at 1000 - request 0x5a5a3e80, waiting, nothing counted");
		CHK(clrecon_fp_wait(&f, 1000 + FP_QUIET_MS - 1) == CLRECON_FP_HOLD,
			"G: heard from within %d ms - hold", FP_QUIET_MS);
		CHK(clrecon_fp_wait(&f, 1000 + FP_QUIET_MS) == CLRECON_FP_ASK, "G: then quiet - ask again");
		CHK(clrecon_fp_mine(&f, req, 3, 2000) && f.heard_ms == 2000 &&
			!clrecon_fp_mine(&f, req + 1, 3, 3000) && !clrecon_fp_mine(&f, req, 4, 3000) &&
			f.heard_ms == 2000, "G: only the ask in flight's frames count, and are heard from");
		CHK(clrecon_fp_number(&f, 5) == 1 && clrecon_fp_number(&f, 5) == 0 && f.got == 1 &&
			f.seen_cap == 4096, "G: number 5 - held once, a repeat not counted again");
		CHK(clrecon_fp_number(&f, 40000) == 1 && f.seen_cap == 8192 && f.got == 2 &&
			clrecon_fp_number(&f, 5) == 0, "G: number 40,000 - the bitmap grown to 8 KB, 5 still held");
		CHK(clrecon_fp_number(&f, FP_SEEN_MAX) == -1 && f.got == 2,
			"G: one past FP_SEEN_MAX - refused");
		CHK(clrecon_fp_number(&f, FP_SEEN_MAX - 1) == 1 && f.seen_cap == FP_SEEN_MAX / 8,
			"G: the last number - the bitmap at its whole size");
		clrecon_fp_count(&f, 4);
		CHK(f.state == FPS_WAIT && f.n == 4, "G: the witness counted 4, 3 held - still waiting");
		CHK(clrecon_fp_number(&f, 7) == 1 && clrecon_fp_wait(&f, 2001) == CLRECON_FP_DONE &&
			f.state == FPS_DONE, "G: the fourth arrives - done");
		CHK(!clrecon_fp_mine(&f, req, 3, 4000), "G: done - a late frame does not count");
		req = clrecon_fp_asked(&f, 3, 7000);
		CHK(f.tries == 2 && !f.got && f.n == -1 && f.state == FPS_WAIT &&
			clrecon_fp_number(&f, 5) == 1, "G: asked again - the numbers start afresh");
		clrecon_fp_asked(&f, 3, 13000);
		CHK(f.tries == FP_TRIES && clrecon_fp_wait(&f, 13000 + FP_QUIET_MS) == CLRECON_FP_GIVE_UP,
			"G: quiet after the %d-th ask - give up", FP_TRIES);
		clrecon_fp_fall_back(&f);
		CHK(f.state == FPS_WALK && !f.seen && !f.seen_cap, "G: given up for the walk - the bitmap freed");
		memset(&f, 0, sizeof f);
		CHK(clrecon_fp_asked(&f, 1, 94740480LL) == 1,
			"G: a request id that would be 0 - 1 instead");
		clrecon_fp_number(&f, 1);
		clrecon_fp_count(&f, 1);
		CHK(f.state == FPS_DONE, "G: a count already met - done at once");
		clrecon_fp_drop_seen(&f);
	}

	/* H */
	{
		static struct clpeers pt;
		struct clrecon_restart o;
		struct peer *w;
		long long now = 200000, g = 30000;
		int live, id;

		CHK(clrecon_death_mono(0, 0, 65000, 100000) == LLONG_MIN,
			"H: no last write, no stamp - unknown");
		CHK(clrecon_death_mono(5000, 0, 65000, 100000) == 40000,
			"H: dead since 5,000 (wall), 60 s ago - 40,000 on the monotonic clock");
		CHK(clrecon_death_mono(5000, 8000, 65000, 100000) == 43000, "H: the later stamp wins");
		CHK(clrecon_death_mono(70000, 0, 65000, 100000) == 100000,
			"H: a wall clock behind the last write - no time dead");
		memset(&r, 0, sizeof r);
		r.death_mono = 100000;
		CHK(!clrecon_is_witness(&r, 0, g), "H: a peer with no uptime heard - never a witness");
		CHK(clrecon_is_witness(&r, 69999, g) && !clrecon_is_witness(&r, 70000, g),
			"H: up %lld ms before the death - a witness; a millisecond less - not", g + 1);
		r.death_mono = LLONG_MIN;
		CHK(!clrecon_is_witness(&r, 1, g), "H: death unknown - nobody");
		r.death_mono = 100000;
		/* node 2 a witness, 3 booted too late, 4 a witness but not live,
		 * an empty slot, 5 a witness */
		pt.n_peers = 5;
		pt.peers[0].node = 2; pt.peers[0].last_seen_ms = now; pt.peers[0].boot_ms = 10000;
		pt.peers[1].node = 3; pt.peers[1].last_seen_ms = now; pt.peers[1].boot_ms = 90000;
		pt.peers[2].node = 4; pt.peers[2].last_seen_ms = now - PEER_UP_MS; pt.peers[2].boot_ms = 5000;
		pt.peers[3].node = 0; pt.peers[3].last_seen_ms = now; pt.peers[3].boot_ms = 1000;
		pt.peers[4].node = 5; pt.peers[4].last_seen_ms = now; pt.peers[4].boot_ms = 20000;
		w = clrecon_first_witness(&r, &pt, now, g, &live);
		CHK(w == &pt.peers[0] && live == 1, "H: the first live witness - node 2, one live peer counted to it");
		CHK(clrecon_witnesses(&r, &pt, now, g, &id) == 2 && id == 5,
			"H: two live witnesses (4 is not live), the last node 5");
		pt.peers[0].boot_ms = 95000;
		pt.peers[4].boot_ms = 0;
		w = clrecon_first_witness(&r, &pt, now, g, &live);
		CHK(!w && live == 3 && clrecon_witnesses(&r, &pt, now, g, &id) == 0 && id == 0,
			"H: none - every live peer counted (3), nobody named");
		/* the restart note at 200,000, a witness 41,000 after its boot */
		pt.peers[0].boot_ms = 10000;
		pt.peers[1].boot_ms = 170000;
		pt.peers[0].reconciling = 1; pt.peers[0].sole_witness = 1;   /* waits on us */
		pt.peers[1].reconciling = 1; pt.peers[1].sole_witness = -1;  /* does not say */
		pt.peers[2].reconciling = 1; pt.peers[2].sole_witness = 1;   /* not live */
		pt.peers[4].reconciling = 1; pt.peers[4].sole_witness = 7;   /* has another */
		clrecon_restart(&pt, now, 41000, 1, &o);
		CHK(o.live == 3 && o.witnesses == 1 && o.wait_ms == 11000,
			"H: restart now - 3 live, 1 would witness (node 2), the next in 11 s (node 3); "
			"node 5 has no uptime");
		CHK(o.waits_n == 2 && o.waits_for[0] == 2 && o.waits_for[1] == 3,
			"H: waiting on this node - 2 (names it) and 3 (says none); 5 has another, 4 is not live");
		pt.peers[1].boot_ms = 100000;
		clrecon_restart(&pt, now, 41000, 1, &o);
		CHK(o.witnesses == 2 && o.wait_ms == -1, "H: both up long enough - 2, nothing to wait for");
	}

	printf("clrecontest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
