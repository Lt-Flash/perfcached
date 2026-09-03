/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * metrics.h — S46: the OpenMetrics rendering of the node's state.
 *
 * The stats verb is JSON and its field names have moved more than once
 * (arena_held/arena_max were added when the old names proved
 * misleading).  A metrics endpoint cannot behave that way: a dashboard
 * and an alert rule outlive the release that introduced them, so the
 * names here are a CONTRACT, pinned by test/metricstest.sh.  Adding a
 * metric is free; renaming or removing one breaks that test on
 * purpose.
 */
#ifndef PC_METRICS_H
#define PC_METRICS_H

#include <stddef.h>

/* Render the whole exposition into @buf.  Returns the number of bytes
 * written (never more than @cap - 1, always NUL-terminated), or 0 if
 * @cap is too small to be useful. */
size_t pc_metrics_render(char *buf, size_t cap);

/* seconds since the daemon finished starting; the exposition's
 * perfcached_uptime_seconds, kept here so main() can stamp it */
void pc_metrics_mark_start(void);

#endif /* PC_METRICS_H */
