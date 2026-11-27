/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* winctr.h - the last five minutes of any counter /stats publishes, kept by the daemon so a
 * window is true without the page having been open for five minutes */
#ifndef PC_WINCTR_H
#define PC_WINCTR_H

#define PC_WIN_SPAN   300           /* seconds a window reaches back, at most */
#define PC_WIN_SLOTS  6             /* one a minute: the current + five */
#define PC_WIN_MIN    60            /* a non-zero-based key needs this much */

/* @cur less @key's oldest minute snapshot within five minutes, @secs (may be NULL) the span; -1
 * with no baseline yet unless @from_zero.  A counter gone backwards counts from its new start. */
long long pc_win_delta(const char *key, unsigned long long cur, int from_zero,
		unsigned int *secs);
/* the same at an explicit time (seconds since start) - for tests */
long long pc_win_delta_at(const char *key, unsigned long long cur,
		int from_zero, unsigned int now, unsigned int *secs);
/* keys held now (for tests and /stats) */
unsigned int pc_win_keys(void);
/* forget everything (tests) */
void pc_win_reset(void);

/* a maximum over the last five minutes, kept in minute slots where the
 * value is observed; a zeroed struct is empty */
struct pc_winmax {
	unsigned int mi[PC_WIN_SLOTS];        /* minute number of each slot */
	unsigned long long m[PC_WIN_SLOTS];
};

static inline void pc_winmax_note(struct pc_winmax *w, unsigned int now,
		unsigned long long v)
{
	unsigned int mi = now / 60, s = mi % PC_WIN_SLOTS;
	unsigned long long cur;

	if (__atomic_load_n(&w->mi[s], __ATOMIC_RELAXED) != mi) {
		__atomic_store_n(&w->m[s], v, __ATOMIC_RELAXED);
		__atomic_store_n(&w->mi[s], mi, __ATOMIC_RELEASE);
		return;
	}
	cur = __atomic_load_n(&w->m[s], __ATOMIC_RELAXED);
	while (v > cur && !__atomic_compare_exchange_n(&w->m[s], &cur, v, 0,
	        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
		;                  /* cur reloaded: try again while v is higher */
}

/* the largest value noted in the current minute and the four before it */
static inline unsigned long long pc_winmax_get(const struct pc_winmax *w,
		unsigned int now)
{
	unsigned int mi = now / 60, s;
	unsigned long long r = 0, v;

	for (s = 0; s < PC_WIN_SLOTS; s++) {
		unsigned int sm = __atomic_load_n(&w->mi[s], __ATOMIC_ACQUIRE);

		if (sm > mi || mi - sm > PC_WIN_SLOTS - 2)
			continue;          /* older than five minutes */
		v = __atomic_load_n(&w->m[s], __ATOMIC_RELAXED);
		if (v > r)
			r = v;
	}
	return r;
}

#endif /* PC_WINCTR_H */
