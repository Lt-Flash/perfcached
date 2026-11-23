/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* metrics.h - the OpenMetrics rendering of the node's state.  The names are a
 * contract pinned by test/metricstest.sh: add freely, never rename or remove. */
#ifndef PC_METRICS_H
#define PC_METRICS_H

#include <stddef.h>

/* the exposition in a heap buffer grown to fit, NUL-terminated, its length in @len; the caller
 * frees it.  NULL when out of memory - never a cut copy */
char *pc_metrics_render(size_t *len);

/* seconds since the daemon finished starting; the exposition's
 * perfcached_uptime_seconds, kept here so main() can stamp it */
void pc_metrics_mark_start(void);

/* the same figure for callers rendering their own document; 0 if never stamped,
 * so "not up yet" differs from "up for no time at all" */
long pc_metrics_uptime(void);

#endif /* PC_METRICS_H */
