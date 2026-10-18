/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * wintest.c - S351: the daemon's five-minute counter windows (winctr.c),
 * on an injected clock so minutes pass in microseconds.
 *
 * What the status page now reads as "last 5 min" comes from here, so each
 * promise winctr.h makes is asserted:
 *   - a counter that started with the process counts from 0 while the
 *     process is younger than five minutes;
 *   - warm, the window spans between four and five minutes, whether the
 *     tree is built every 3 s (a page polling) or once a minute (the
 *     maintenance thread alone), and its value is the counter's rise over
 *     exactly that span;
 *   - a key seen for the first time later reports NO window (-1) until a
 *     minute of it has been seen - never a few seconds as five minutes;
 *   - a counter that went backwards counts from its new start;
 *   - pc_winmax: the largest value of the last five minutes, not of all
 *     time - a spike six minutes ago is gone.
 */
#include <stdio.h>
#include <string.h>

#include "../src/winctr.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

int main(void)
{
	unsigned int t, secs, worst_lo = 1000, worst_hi = 0;
	long long d;
	int bad = 0;

	/* 1. from zero, young process */
	pc_win_reset();
	d = pc_win_delta_at("p.a", 5, 1, 10, &secs);
	CHK(d == 5 && secs == 10, "from_zero at 10 s: the whole 5 is the window, "
		"over 10 s (%lld over %u s)", d, secs);

	/* 2. warm, polled every 3 s: the counter rises 7 a second */
	pc_win_reset();
	for (t = 0; t <= 1800; t += 3) {
		d = pc_win_delta_at("p.b", (unsigned long long)t * 7, 1, t, &secs);
		if (t > PC_WIN_SPAN) {
			if (secs < worst_lo)
				worst_lo = secs;
			if (secs > worst_hi)
				worst_hi = secs;
			if (d != (long long)secs * 7)
				bad++;
		}
	}
	CHK(worst_lo >= 240 && worst_hi <= 300 && !bad, "polled every 3 s for 30 min: "
		"every window spans %u..%u s (want 240..300) and equals the rise over it "
		"(%d wrong)", worst_lo, worst_hi, bad);

	/* 3. warm, built only once a minute (no page open) */
	pc_win_reset();
	worst_lo = 1000; worst_hi = 0; bad = 0;
	for (t = 0; t <= 1800; t += 60) {
		d = pc_win_delta_at("p.c", (unsigned long long)t * 3, 1, t, &secs);
		if (t > PC_WIN_SPAN) {
			if (secs < worst_lo)
				worst_lo = secs;
			if (secs > worst_hi)
				worst_hi = secs;
			if (d != (long long)secs * 3)
				bad++;
		}
	}
	CHK(worst_lo >= 240 && worst_hi <= 300 && !bad, "built once a minute: windows "
		"span %u..%u s and are exact (%d wrong)", worst_lo, worst_hi, bad);
	/* then a page opens mid-minute: its first read is already a window */
	d = pc_win_delta_at("p.c", 1830 * 3, 1, 1830, &secs);
	CHK(d == (long long)secs * 3 && secs >= 240 && secs <= 300,
		"a page opened later reads a full window at once (%lld over %u s)", d, secs);

	/* 4. a key first seen late has no window for a minute */
	d = pc_win_delta_at("late.k", 1000, 0, 2000, &secs);
	CHK(d == -1, "a key first seen at 2000 s: no window (%lld)", d);
	d = pc_win_delta_at("late.k", 1100, 0, 2030, &secs);
	CHK(d == -1, "30 s later: still none - 30 s is not five minutes (%lld)", d);
	d = pc_win_delta_at("late.k", 1300, 0, 2061, &secs);
	CHK(d == 300 && secs == 61, "61 s later: the rise since first seen, over 61 s "
		"(%lld over %u s)", d, secs);

	/* 5. a counter that went backwards (reset stats, a peer restarted) */
	pc_win_reset();
	for (t = 0; t <= 900; t += 60)
		(void)pc_win_delta_at("r.k", 100000 + t, 0, t, &secs);
	d = pc_win_delta_at("r.k", 40, 0, 930, &secs);
	CHK(d == 40, "the counter fell to 40: the window counts from its new start "
		"(%lld)", d);
	d = pc_win_delta_at("r.k", 100, 0, 990, &secs);
	CHK(d == 100, "and goes on counting from the restart - all 100 since it, not "
		"only what came after the first read (%lld over %u s)", d, secs);
	for (t = 1020; t <= 1600; t += 60)
		(void)pc_win_delta_at("r.k", 100 + (t - 990), 0, t, &secs);
	d = pc_win_delta_at("r.k", 100 + (1610 - 990), 0, 1610, &secs);
	CHK(d == (long long)secs && secs >= 240 && secs <= 300, "ten minutes on it is an ordinary "
		"window again (%lld over %u s)", d, secs);

	/* 6. the five-minute maximum */
	{
		struct pc_winmax w;
		unsigned long long m;

		memset(&w, 0, sizeof w);
		pc_winmax_note(&w, 100, 900);          /* a spike in minute 1 */
		pc_winmax_note(&w, 200, 50);
		pc_winmax_note(&w, 250, 70);
		m = pc_winmax_get(&w, 300);
		CHK(m == 900, "the spike is the maximum while it is in the window (%llu)", m);
		pc_winmax_note(&w, 340, 20);
		m = pc_winmax_get(&w, 340);
		CHK(m == 900, "four minutes on, still (%llu)", m);
		m = pc_winmax_get(&w, 480);
		CHK(m == 70, "six minutes on it is gone: the max of the last five is 70 "
			"(%llu)", m);
		pc_winmax_note(&w, 500, 30);
		pc_winmax_note(&w, 501, 80);
		m = pc_winmax_get(&w, 510);
		CHK(m == 80, "a slot reused for a new minute starts afresh (%llu)", m);
	}

	/* 7. many keys: a table full of live keys says so, stale ones are reused */
	pc_win_reset();
	{
		char k[32];
		int i, none = 0;

		for (i = 0; i < 3000; i++) {
			snprintf(k, sizeof k, "many.%d", i);
			if (pc_win_delta_at(k, 1, 1, 10, &secs) < 0)
				none++;
		}
		CHK(pc_win_keys() == 2048 && none == 3000 - 2048,
			"3000 live keys: 2048 held, the rest report no window (%u held, %d none)",
			pc_win_keys(), none);
		snprintf(k, sizeof k, "fresh.0");
		d = pc_win_delta_at(k, 7, 1, 2000, &secs);
		CHK(pc_win_keys() == 2048 && d == -1,
			"after 15 min unseen the old keys are reusable (a new one fits: held %u)",
			pc_win_keys());
	}

	printf("%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
	return fails ? 1 : 0;
}
