/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * statsreset.h - the stats reset (S123) and its schedule (S358): the
 * baselines a reset snapshots for the planes whose counters it does not
 * own, and when the running totals count from.  The resets themselves -
 * pc_stats_reset, _at, _fleet, _schedule, _tick - are declared in verbs.h,
 * where their callers have always found them.
 */
#ifndef PC_STATSRESET_H
#define PC_STATSRESET_H

struct pc_cl_stats;
struct pc_proxy_stats;
struct pc_wal_stats;

/* the running totals since the last reset: the baseline the reset took
 * subtracted from the figures in @s (the gauges left alone) */
void since_cl(struct pc_cl_stats *s);
void since_px(struct pc_proxy_stats *s);
void since_wal(struct pc_wal_stats *s);

/* when the totals were last reset (0 = never - they count from the start)
 * and when the schedule resets them next (0 = no schedule) */
void statsreset_times(long long *at, long long *next);

#endif
