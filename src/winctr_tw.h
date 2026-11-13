/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* winctr_tw.h - write a counter into a /stats tree with its five-minute window beside it:
 * `name` stays cumulative (/metrics), `win_name` is null until there is a baseline. */
#ifndef PC_WINCTR_TW_H
#define PC_WINCTR_TW_H

#include <stdio.h>

#include "ptree.h"
#include "winctr.h"

/* @wk: the counter's key in the window table, unique across the tree.  Each such counter
 * started with the process (or its table), so it counts from zero in the first window. */
static inline void pc_tw_ctr(struct pc_tw *out, const char *wk,
		const char *name, unsigned long long v)
{
	char wn[64];
	long long d = pc_win_delta(wk, v, 1, NULL);

	pc_tw_key(out, name);
	pc_tw_i64(out, (long long)v);
	snprintf(wn, sizeof wn, "win_%s", name);
	pc_tw_key(out, wn);
	if (d < 0)
		pc_tw_nil(out);
	else
		pc_tw_i64(out, d);
}

/* the same for a per-collection counter: key col.<name>.<sub><field> */
static inline void pc_tw_ctr_col(struct pc_tw *out, const char *col,
		size_t collen, const char *sub, const char *name,
		unsigned long long v)
{
	char wk[160];

	snprintf(wk, sizeof wk, "col.%.*s.%s%s", (int)collen, col, sub, name);
	pc_tw_ctr(out, wk, name, v);
}

#endif /* PC_WINCTR_TW_H */
