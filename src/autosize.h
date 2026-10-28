/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * autosize.h - the tables' sizing (autosize.c), for the maintenance
 * thread in daemon.c.
 */
#ifndef PC_AUTOSIZE_H
#define PC_AUTOSIZE_H

#include "core/pcache_htable.h"

/* [daemon] grow_at_pct, shrink_at_pct (resolved), shrink_cooloff_s -
 * daemon.c's */
extern int pc_grow_at_pct, pc_shrink_at_pct, pc_shrink_cooloff_s;

/* S328: collection @i's table is sized by its keys (AUTO, FLOOR) */
int as_sizes(int i);
/* S150 D: shrink table @i when it has sat below shrink_at_pct for the
 * cool-off (@grew: it split this tick) */
void shrink_consider(int i, pcache_htable_t *cht, unsigned int now,
		int grew);
/* S239: publish what AUTO would do with table @i; under WARN, say it */
void autoscale_advise(int i, pcache_htable_t *cht, unsigned int now);

#endif
