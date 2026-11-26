/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clprobe.h - the lane probes' table: per silent address, when TCP was asked and when a UDP probe
 * was asked and answered.  The cluster thread's only; a header so test/clprobetest.c reaches it. */
#ifndef PC_CLPROBE_H
#define PC_CLPROBE_H

#include <string.h>

#include "pcaddr.h"

#define CLPROBE_MAX 64

struct lprobe { pc_addr a; long long ms, ask_ms, heard_ms, used_ms; };

struct clprobe_tab {
	struct lprobe e[CLPROBE_MAX];
	int n;
	unsigned long long saves;      /* lanes forced */
};

/* @a's entry, made (zeroed) when it has none and @make; NULL when it has none and @make is 0.  A full
 * table gives up the entry used longest ago: one an address still probed on every tick stays */
static inline struct lprobe *clprobe_slot(struct clprobe_tab *t, const pc_addr *a, int make,
		long long now)
{
	int i, v = 0;

	for (i = 0; i < t->n; i++)
		if (pc_addr_eq(&t->e[i].a, a)) {
			t->e[i].used_ms = now;
			return &t->e[i];
		}
	if (!make)
		return NULL;
	if (t->n < CLPROBE_MAX)
		v = t->n++;
	else
		for (i = 1; i < t->n; i++)
			if (t->e[i].used_ms < t->e[v].used_ms)
				v = i;
	memset(&t->e[v], 0, sizeof t->e[v]);
	t->e[v].a = *a;
	t->e[v].used_ms = now;
	return &t->e[v];
}

#endif
