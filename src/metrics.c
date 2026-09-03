/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * metrics.c — S46: the OpenMetrics exposition.  See metrics.h for why
 * the names in here are a contract rather than a convenience.
 *
 * Everything is read from the same accessors the stats verb uses, with
 * the same tolerance for torn counters: an exposition is a trend, not
 * a transaction, and taking locks to serialise a scrape would put a
 * scraper in the path of the data plane - the exact failure this
 * project exists to avoid.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "metrics.h"
#include "version.h"
#include "store.h"
#include "wal.h"
#include "cluster.h"
#include "core/pcache_arena.h"
#include "core/pcache_htable.h"

static time_t started;

void pc_metrics_mark_start(void)
{
	started = time(NULL);
}

/* a tiny append helper: every writer below is bounded by @cap */
struct out {
	char *b;
	size_t cap, len;
};

static void emit(struct out *o, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void emit(struct out *o, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (o->len >= o->cap)
		return;
	va_start(ap, fmt);
	/*
	 * The newer clang-analyzer on the Ubuntu CI reports this as
	 * "vsnprintf is called with an uninitialized va_list argument".
	 * It is not: its own path trace goes line 47 -> this line and
	 * never visits the va_start directly above, so the checker simply
	 * does not model va_start as initialising `ap` - there is no note
	 * claiming the va_start was conditionally skipped, because there
	 * is no such path.  The early return above happens BEFORE the
	 * va_start, so every path that reaches here has run it.
	 *
	 * Suppressed at the site rather than by disabling the check, so a
	 * future va_list that really is uninitialised still gets caught -
	 * this is the only va_list in the tree.
	 */
	/* NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized) */
	n = vsnprintf(o->b + o->len, o->cap - o->len, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if ((size_t)n >= o->cap - o->len)
		o->len = o->cap;               /* truncated; stop writing */
	else
		o->len += (size_t)n;
}

/* a label value must not carry a quote or a backslash into the
 * exposition - a collection name is operator-chosen, not ours */
static void label_escape(const char *in, char *out, size_t cap)
{
	size_t i = 0;

	while (*in && i + 2 < cap) {
		if (*in == '"' || *in == '\\')
			out[i++] = '\\';
		out[i++] = *in++;
	}
	out[i] = 0;
}

size_t pc_metrics_render(char *buf, size_t cap)
{
	struct out o = { buf, cap, 0 };
	struct pcache_arena_pressure pr;
	struct pc_wal_stats ws;
	unsigned long held, mx;
	int i, n;

	if (cap < 64)
		return 0;

	emit(&o, "# HELP perfcached_build_info the running build\n"
		"# TYPE perfcached_build_info gauge\n"
		"perfcached_build_info{version=\"%s\",rev=\"%s\"} 1\n",
		PC_VERSION, PC_BUILD_REV);

	emit(&o, "# HELP perfcached_uptime_seconds seconds since startup\n"
		"# TYPE perfcached_uptime_seconds gauge\n"
		"perfcached_uptime_seconds %ld\n",
		started ? (long)(time(NULL) - started) : 0L);

	/* ---- memory: the S47 pressure surface ---------------------- */
	pcache_arena_pressure(&pr);
	held = pcache_arena_held_bytes();
	mx = pcache_arena_max_bytes;

	emit(&o, "# HELP perfcached_arena_held_bytes memory held from the host\n"
		"# TYPE perfcached_arena_held_bytes gauge\n"
		"perfcached_arena_held_bytes %lu\n", held);
	emit(&o, "# HELP perfcached_arena_max_bytes the configured ceiling\n"
		"# TYPE perfcached_arena_max_bytes gauge\n"
		"perfcached_arena_max_bytes %lu\n", mx);
	emit(&o, "# HELP perfcached_arena_live_bytes bytes in live records\n"
		"# TYPE perfcached_arena_live_bytes gauge\n"
		"perfcached_arena_live_bytes %llu\n",
		(unsigned long long)pcache_arena_live_bytes());
	/* a RATIO, not a percentage: Prometheus convention, and it keeps
	 * an alert threshold readable as 0.05 rather than "5" */
	emit(&o, "# HELP perfcached_arena_headroom_ratio free share of the ceiling\n"
		"# TYPE perfcached_arena_headroom_ratio gauge\n"
		"perfcached_arena_headroom_ratio %.4f\n",
		mx ? (mx > held ? (double)(mx - held) / (double)mx : 0.0) : 1.0);
	emit(&o, "# HELP perfcached_writes_refused_total writes refused, arena full\n"
		"# TYPE perfcached_writes_refused_total counter\n"
		"perfcached_writes_refused_total %lu\n", pr.refused);
	emit(&o, "# HELP perfcached_reclaim_released_bytes_total memory given back\n"
		"# TYPE perfcached_reclaim_released_bytes_total counter\n"
		"perfcached_reclaim_released_bytes_total %lu\n",
		pr.released_bytes);
	emit(&o, "# HELP perfcached_reclaim_giveback_disabled give-back latched off\n"
		"# TYPE perfcached_reclaim_giveback_disabled gauge\n"
		"perfcached_reclaim_giveback_disabled %d\n", pr.giveback_off);

	/* ---- per collection ---------------------------------------- */
	emit(&o, "# HELP perfcached_collection_entries live records\n"
		"# TYPE perfcached_collection_entries gauge\n");
	n = pc_store_count();
	for (i = 0; i < n; i++) {
		pcache_ht_totals_t t;
		char esc[128];

		pcache_ht_totals(pc_store_ht(i), &t);
		label_escape(pc_store_name(i), esc, sizeof esc);
		emit(&o, "perfcached_collection_entries{collection=\"%s\"} %lu\n",
			esc, t.entries);
	}
	emit(&o, "# HELP perfcached_collection_hits_total read hits\n"
		"# TYPE perfcached_collection_hits_total counter\n");
	for (i = 0; i < n; i++) {
		pcache_ht_totals_t t;
		char esc[128];

		pcache_ht_totals(pc_store_ht(i), &t);
		label_escape(pc_store_name(i), esc, sizeof esc);
		emit(&o, "perfcached_collection_hits_total{collection=\"%s\"} %lu\n",
			esc, t.hits);
	}
	emit(&o, "# HELP perfcached_collection_misses_total read misses\n"
		"# TYPE perfcached_collection_misses_total counter\n");
	for (i = 0; i < n; i++) {
		pcache_ht_totals_t t;
		char esc[128];

		pcache_ht_totals(pc_store_ht(i), &t);
		label_escape(pc_store_name(i), esc, sizeof esc);
		emit(&o, "perfcached_collection_misses_total{collection=\"%s\"} %lu\n",
			esc, t.misses);
	}

	/* ---- durability -------------------------------------------- */
	pc_wal_get_stats(&ws);
	emit(&o, "# HELP perfcached_wal_appended_total records written to the WAL\n"
		"# TYPE perfcached_wal_appended_total counter\n"
		"perfcached_wal_appended_total %llu\n", ws.appended);
	/* the S58 metric: a ring-full drop is a record the barrier can
	 * never cover, so it belongs on a dashboard, not only in a log */
	emit(&o, "# HELP perfcached_wal_dropped_total records dropped, ring full\n"
		"# TYPE perfcached_wal_dropped_total counter\n"
		"perfcached_wal_dropped_total %llu\n", ws.dropped);
	emit(&o, "# HELP perfcached_wal_last_seq newest sequence number\n"
		"# TYPE perfcached_wal_last_seq gauge\n"
		"perfcached_wal_last_seq %llu\n", ws.last_seq);
	emit(&o, "# HELP perfcached_wal_synced_seq newest fsynced sequence\n"
		"# TYPE perfcached_wal_synced_seq gauge\n"
		"perfcached_wal_synced_seq %llu\n", ws.synced_seq);

	/* ---- cluster ----------------------------------------------- */
	if (pc_cluster_enabled()) {
		struct pc_cl_stats cs;

		pc_cluster_get_stats(&cs);
		emit(&o, "# HELP perfcached_cluster_nodes nodes in the published map\n"
			"# TYPE perfcached_cluster_nodes gauge\n"
			"perfcached_cluster_nodes %d\n", cs.map_nodes);
		emit(&o, "# HELP perfcached_cluster_map_valid the map is usable\n"
			"# TYPE perfcached_cluster_map_valid gauge\n"
			"perfcached_cluster_map_valid %d\n", cs.map_valid);
	}

	if (o.len >= o.cap)
		o.len = o.cap - 1;
	buf[o.len] = 0;
	return o.len;
}
