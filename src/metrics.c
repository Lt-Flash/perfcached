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

#include "daemon.h"
#include "obs.h"                        /* S159: the command rows */
#include "metrics.h"
#include "version.h"
#include "store.h"
#include "wal.h"
#include "cluster.h"
#include "proto.h"                      /* S161: the client-limit counters */
#include "pubsub.h"                     /* PS1: pub/sub figures */
#include "core/pcache_arena.h"
#include "core/pcache_htable.h"

static time_t started;

void pc_metrics_mark_start(void)
{
	started = time(NULL);
}

long pc_metrics_uptime(void)
{
	return started ? (long)(time(NULL) - started) : 0;
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

	/* S123: connections open right now, per door and dialect - gauges,
	 * untouched by a counter reset */
	emit(&o, "# HELP perfcached_connections_open connections open now, by door and dialect\n"
		"# TYPE perfcached_connections_open gauge\n"
		"perfcached_connections_open{door=\"resp\",dialect=\"resp\"} %u\n"
		"perfcached_connections_open{door=\"native\",dialect=\"binary\"} %u\n"
		"perfcached_connections_open{door=\"native\",dialect=\"json\"} %u\n"
		"perfcached_connections_open{door=\"native\",dialect=\"resp\"} %u\n",
		PC_RESP_READ(pc_resp_open), PC_RESP_READ(pc_nat_bin_open),
		PC_RESP_READ(pc_nat_text_open), PC_RESP_READ(pc_nat_resp_open));
	emit(&o, "# HELP perfcached_clients_open client connections open across the data doors (S161)\n"
		"# TYPE perfcached_clients_open gauge\n"
		"perfcached_clients_open %u\n"
		"# HELP perfcached_max_clients the client limit in force\n"
		"# TYPE perfcached_max_clients gauge\n"
		"perfcached_max_clients %d\n"
		"# HELP perfcached_clients_refused_total connections refused at the limit\n"
		"# TYPE perfcached_clients_refused_total counter\n"
		"perfcached_clients_refused_total %llu\n",
		PC_RESP_READ(pc_clients_open), pc_max_clients,
		__atomic_load_n(&pc_clients_refused, __ATOMIC_RELAXED));
	{
		struct pc_pubsub_stats ps;   /* PS1/PS3 */

		pc_pubsub_stats(&ps);
		emit(&o, "# HELP perfcached_pubsub_subscribers subscriptions held by connections on this node\n"
			"# TYPE perfcached_pubsub_subscribers gauge\n"
			"perfcached_pubsub_subscribers %d\n"
			"# HELP perfcached_pubsub_channels channels with at least one local subscriber\n"
			"# TYPE perfcached_pubsub_channels gauge\n"
			"perfcached_pubsub_channels %d\n"
			"# HELP perfcached_pubsub_patterns patterns with at least one local subscriber\n"
			"# TYPE perfcached_pubsub_patterns gauge\n"
			"perfcached_pubsub_patterns %d\n"
			"# HELP perfcached_pubsub_published_total messages published here, own and relayed\n"
			"# TYPE perfcached_pubsub_published_total counter\n"
			"perfcached_pubsub_published_total %llu\n"
			"# HELP perfcached_pubsub_delivered_total frames delivered to local subscribers\n"
			"# TYPE perfcached_pubsub_delivered_total counter\n"
			"perfcached_pubsub_delivered_total %llu\n"
			"# HELP perfcached_pubsub_slow_kills_total subscribers closed at the output cap\n"
			"# TYPE perfcached_pubsub_slow_kills_total counter\n"
			"perfcached_pubsub_slow_kills_total %llu\n"
			"# HELP perfcached_pubsub_relay_sent_total messages relayed to peers\n"
			"# TYPE perfcached_pubsub_relay_sent_total counter\n"
			"perfcached_pubsub_relay_sent_total %llu\n"
			"# HELP perfcached_pubsub_relay_recv_total messages received from peers\n"
			"# TYPE perfcached_pubsub_relay_recv_total counter\n"
			"perfcached_pubsub_relay_recv_total %llu\n"
			"# HELP perfcached_pubsub_relay_lost_total relay sequence gaps seen\n"
			"# TYPE perfcached_pubsub_relay_lost_total counter\n"
			"perfcached_pubsub_relay_lost_total %llu\n"
			"# HELP perfcached_pubsub_relay_dropped_total publishes too large for a datagram, delivered locally only\n"
			"# TYPE perfcached_pubsub_relay_dropped_total counter\n"
			"perfcached_pubsub_relay_dropped_total %llu\n"
			"# HELP perfcached_pubsub_queue_dropped_total messages dropped because a worker's queue was at its cap\n"
			"# TYPE perfcached_pubsub_queue_dropped_total counter\n"
			"perfcached_pubsub_queue_dropped_total %llu\n"
			"# HELP perfcached_pubsub_publish_paused_total publishing connections paused because a worker queue they feed passed half its cap\n"
			"# TYPE perfcached_pubsub_publish_paused_total counter\n"
			"perfcached_pubsub_publish_paused_total %llu\n"
			"# HELP perfcached_pubsub_queue_bytes bytes queued for workers right now\n"
			"# TYPE perfcached_pubsub_queue_bytes gauge\n"
			"perfcached_pubsub_queue_bytes %llu\n"
			"# HELP perfcached_pubsub_relay_duplicates_total relayed datagrams that arrived twice and were dropped\n"
			"# TYPE perfcached_pubsub_relay_duplicates_total counter\n"
			"perfcached_pubsub_relay_duplicates_total %llu\n"
			"# HELP perfcached_pubsub_alloc_failed_total deliveries not made because an allocation failed on this node\n"
			"# TYPE perfcached_pubsub_alloc_failed_total counter\n"
			"perfcached_pubsub_alloc_failed_total %llu\n"
			"# HELP perfcached_pubsub_keyspace_events_total keyspace notifications emitted on this node (PS8)\n"
			"# TYPE perfcached_pubsub_keyspace_events_total counter\n"
			"perfcached_pubsub_keyspace_events_total %llu\n",
			ps.subscribers, ps.channels, ps.patterns, ps.published,
			ps.delivered, ps.slow_kills, ps.relay_sent, ps.relay_recv,
			ps.relay_lost, ps.relay_dropped, ps.queue_dropped, ps.publish_paused, ps.queue_bytes, ps.relay_duplicates, ps.alloc_failed, ps.keyspace);
	}
	{
		/* PS11: the dedicated relay plane */
		int rport, rthreads, rdirect;
		unsigned long long sd, sc, rx;

		pc_cluster_pubsub_relay_figures(&rport, &rthreads, &rdirect, &sd, &sc, &rx);
		emit(&o, "# HELP perfcached_pubsub_relay_rx_threads threads reading pub/sub relays on their own port (0 = the cluster socket)\n"
			"# TYPE perfcached_pubsub_relay_rx_threads gauge\n"
			"perfcached_pubsub_relay_rx_threads %d\n"
			"# HELP perfcached_pubsub_relay_peers_direct peers that answered on their relay port and receive relays there\n"
			"# TYPE perfcached_pubsub_relay_peers_direct gauge\n"
			"perfcached_pubsub_relay_peers_direct %d\n"
			"# HELP perfcached_pubsub_relay_sent_path_total relays sent, by path\n"
			"# TYPE perfcached_pubsub_relay_sent_path_total counter\n"
			"perfcached_pubsub_relay_sent_path_total{path=\"direct\"} %llu\n"
			"perfcached_pubsub_relay_sent_path_total{path=\"cluster\"} %llu\n"
			"# HELP perfcached_pubsub_relay_rx_datagrams_total datagrams read by the relay receive threads\n"
			"# TYPE perfcached_pubsub_relay_rx_datagrams_total counter\n"
			"perfcached_pubsub_relay_rx_datagrams_total %llu\n",
			rthreads, rdirect, sd, sc, rx);
	}
	{
		/* PS12: relaying only what a peer wants */
		struct pc_psint_figures fi;

		pc_cluster_pubsub_interest_figures(&fi);
		emit(&o, "# HELP perfcached_pubsub_relay_skipped_total relays not sent because the peer's interest ruled them out\n"
			"# TYPE perfcached_pubsub_relay_skipped_total counter\n"
			"perfcached_pubsub_relay_skipped_total %llu\n"
			"# HELP perfcached_pubsub_relay_peers_filtered peers relayed only what matches their interest\n"
			"# TYPE perfcached_pubsub_relay_peers_filtered gauge\n"
			"perfcached_pubsub_relay_peers_filtered %d\n"
			"# HELP perfcached_pubsub_relay_peers_broadcast peers that filter but are sent everything until their interest is current here\n"
			"# TYPE perfcached_pubsub_relay_peers_broadcast gauge\n"
			"perfcached_pubsub_relay_peers_broadcast %d\n"
			"# HELP perfcached_pubsub_interest_updates_total first-subscription updates, by direction\n"
			"# TYPE perfcached_pubsub_interest_updates_total counter\n"
			"perfcached_pubsub_interest_updates_total{dir=\"sent\"} %llu\n"
			"perfcached_pubsub_interest_updates_total{dir=\"received\"} %llu\n"
			"# HELP perfcached_pubsub_interest_resyncs_total full interest states, by direction\n"
			"# TYPE perfcached_pubsub_interest_resyncs_total counter\n"
			"perfcached_pubsub_interest_resyncs_total{dir=\"sent\"} %llu\n"
			"perfcached_pubsub_interest_resyncs_total{dir=\"received\"} %llu\n",
			fi.skipped, fi.peers_filtered, fi.peers_broadcast,
			fi.updates_sent, fi.updates_received, fi.resyncs_sent,
			fi.resyncs_received);
	}
	{
		/* PS5: pushes over UDP */
		struct pc_udp_figures uf;

		pc_conn_udp_figures(&uf);
		emit(&o, "# HELP perfcached_pubsub_udp_streams connections receiving pub/sub pushes over UDP, probing or confirmed\n"
			"# TYPE perfcached_pubsub_udp_streams gauge\n"
			"perfcached_pubsub_udp_streams %ld\n"
			"# HELP perfcached_pubsub_udp_datagrams_total deliveries sent as sealed datagrams\n"
			"# TYPE perfcached_pubsub_udp_datagrams_total counter\n"
			"perfcached_pubsub_udp_datagrams_total %llu\n"
			"# HELP perfcached_pubsub_udp_oversized_total deliveries too large for a datagram, sent over TCP\n"
			"# TYPE perfcached_pubsub_udp_oversized_total counter\n"
			"perfcached_pubsub_udp_oversized_total %llu\n"
			"# HELP perfcached_pubsub_udp_send_errors_total datagrams the socket refused\n"
			"# TYPE perfcached_pubsub_udp_send_errors_total counter\n"
			"perfcached_pubsub_udp_send_errors_total %llu\n"
			"# HELP perfcached_pubsub_udp_expired_total streams never confirmed\n"
			"# TYPE perfcached_pubsub_udp_expired_total counter\n"
			"perfcached_pubsub_udp_expired_total %llu\n"
			"# HELP perfcached_pubsub_udp_pruned_total streams pruned, their connections' subscriptions dropped, by reason\n"
			"# TYPE perfcached_pubsub_udp_pruned_total counter\n"
			"perfcached_pubsub_udp_pruned_total{reason=\"no_ack\"} %llu\n"
			"perfcached_pubsub_udp_pruned_total{reason=\"no_progress\"} %llu\n",
			uf.streams, uf.pushed, uf.oversized, uf.send_errors, uf.expired,
			uf.pruned_no_ack, uf.pruned_no_progress);
	}
	{
		/* S159: the command rows by name - RESP under its Redis name,
		 * the native doors as json.<verb> and bin.<verb> - and the log2
		 * latency histogram behind them, in seconds as Prometheus wants
		 * it: le = 1 us, 2 us, ... 32.768 ms, +Inf */
		struct pc_obs_cmdsum cs[128];
		int nc = pc_obs_cmd_merge(cs, 128), ci, k;
		char esc[64];

		emit(&o, "# HELP perfcached_command_calls_total commands executed, by name: RESP under its Redis name, the native doors as json.<verb> and bin.<verb> (S159)\n"
			"# TYPE perfcached_command_calls_total counter\n");
		for (ci = 0; ci < nc; ci++) {
			label_escape(cs[ci].name, esc, sizeof esc);
			emit(&o, "perfcached_command_calls_total{cmd=\"%s\"} %llu\n",
				esc, cs[ci].calls);
		}
		emit(&o, "# HELP perfcached_command_usec_total microseconds the workers spent executing them\n"
			"# TYPE perfcached_command_usec_total counter\n");
		for (ci = 0; ci < nc; ci++) {
			label_escape(cs[ci].name, esc, sizeof esc);
			emit(&o, "perfcached_command_usec_total{cmd=\"%s\"} %llu\n",
				esc, cs[ci].usec);
		}
		emit(&o, "# HELP perfcached_command_latency_seconds per-command latency, log2 buckets from 1 us to 32.768 ms\n"
			"# TYPE perfcached_command_latency_seconds histogram\n");
		for (ci = 0; ci < nc; ci++) {
			unsigned long long cum = 0;

			label_escape(cs[ci].name, esc, sizeof esc);
			for (k = 0; k < PC_OBS_HIST - 1; k++) {
				cum += cs[ci].hist[k];
				emit(&o, "perfcached_command_latency_seconds_bucket{cmd=\"%s\",le=\"%.6f\"} %llu\n",
					esc, (double)(1ULL << k) / 1e6, cum);
			}
			cum += cs[ci].hist[PC_OBS_HIST - 1];
			emit(&o, "perfcached_command_latency_seconds_bucket{cmd=\"%s\",le=\"+Inf\"} %llu\n"
				"perfcached_command_latency_seconds_sum{cmd=\"%s\"} %.6f\n"
				"perfcached_command_latency_seconds_count{cmd=\"%s\"} %llu\n",
				esc, cum, esc, (double)cs[ci].usec / 1e6, esc,
				cs[ci].calls);
		}
	}

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
	emit(&o, "# HELP perfcached_arena_regions_bytes index regions held (bucket directories, hint tables, counters - carved at creation and on growth, given back when a table is retired)\n"
		"# TYPE perfcached_arena_regions_bytes gauge\n"
		"perfcached_arena_regions_bytes %lu\n", pr.regions_bytes);
	emit(&o, "# HELP perfcached_arena_warm_free_bytes free slots kept resident\n"
		"# TYPE perfcached_arena_warm_free_bytes gauge\n"
		"perfcached_arena_warm_free_bytes %lu\n", pr.warm_free_bytes);
	/* S150 C: the region zone's free set and how the carve is served */
	emit(&o, "# HELP perfcached_arena_regions_free_warm_bytes retired index slots kept resident (inside warm_free)\n"
		"# TYPE perfcached_arena_regions_free_warm_bytes gauge\n"
		"perfcached_arena_regions_free_warm_bytes %lu\n", pr.regions_free_warm_bytes);
	emit(&o, "# HELP perfcached_arena_regions_free_cold_bytes retired index slots punched out (inside cold)\n"
		"# TYPE perfcached_arena_regions_free_cold_bytes gauge\n"
		"perfcached_arena_regions_free_cold_bytes %lu\n", pr.regions_free_cold_bytes);
	emit(&o, "# HELP perfcached_arena_regions_retired_total index slots given back by retired tables\n"
		"# TYPE perfcached_arena_regions_retired_total counter\n"
		"perfcached_arena_regions_retired_total %lu\n", pr.regions_retired);
	emit(&o, "# HELP perfcached_arena_region_reuse_total index carves served from retired slots instead of the frontier\n"
		"# TYPE perfcached_arena_region_reuse_total counter\n"
		"perfcached_arena_region_reuse_total %lu\n", pr.region_reuse);
	emit(&o, "# HELP perfcached_arena_class_chunk_bytes chunks the size classes own (live records inside, free cells that belong to the class)\n"
		"# TYPE perfcached_arena_class_chunk_bytes gauge\n"
		"perfcached_arena_class_chunk_bytes %lu\n", pr.class_chunk_bytes);
	emit(&o, "# HELP perfcached_arena_page_slack_bytes alignment slot of every shm page; regions + class chunks + warm free + page slack = held\n"
		"# TYPE perfcached_arena_page_slack_bytes gauge\n"
		"perfcached_arena_page_slack_bytes %lu\n", pr.page_slack_bytes);
	emit(&o, "# HELP perfcached_arena_committed_bytes bytes of the reservation committed - the initial commit plus every 2 MB group a carve reached, less what the give-back punched; RSS follows this\n"
		"# TYPE perfcached_arena_committed_bytes gauge\n"
		"perfcached_arena_committed_bytes %lu\n", pr.committed_bytes);
	emit(&o, "# HELP perfcached_arena_reserved_bytes the address space reserved for the arena: the cap\n"
		"# TYPE perfcached_arena_reserved_bytes gauge\n"
		"perfcached_arena_reserved_bytes %lu\n", pr.reserved_bytes);
	/* a RATIO, not a percentage: Prometheus convention, and it keeps
	 * an alert threshold readable as 0.05 rather than "5" */
	emit(&o, "# HELP perfcached_arena_headroom_ratio free share of the ceiling\n"
		"# TYPE perfcached_arena_headroom_ratio gauge\n"
		"perfcached_arena_headroom_ratio %.4f\n",
		mx ? (mx > held ? (double)(mx - held) / (double)mx : 0.0) : 1.0);
	emit(&o, "# HELP perfcached_writes_refused_total writes refused, arena full\n"
		"# TYPE perfcached_writes_refused_total counter\n"
		"perfcached_writes_refused_total %lu\n", pr.refused);
	emit(&o, "# HELP perfcached_hugetlb_pool_empty_total punched groups not re-committed, pool empty\n"
		"# TYPE perfcached_hugetlb_pool_empty_total counter\n"
		"perfcached_hugetlb_pool_empty_total %lu\n", pr.pool_empty);
	emit(&o, "# HELP perfcached_arena_at_ceiling 1 while carves are being refused at the ceiling\n"
		"# TYPE perfcached_arena_at_ceiling gauge\n"
		"perfcached_arena_at_ceiling %d\n", pr.at_ceiling_since ? 1 : 0);
	emit(&o, "# HELP perfcached_reclaim_tail_released_bytes_total the reservation's never-carved tail given back (inside released_bytes)\n"
		"# TYPE perfcached_reclaim_tail_released_bytes_total counter\n"
		"perfcached_reclaim_tail_released_bytes_total %lu\n", pr.tail_released_bytes);
	emit(&o, "# HELP perfcached_reclaim_released_bytes_total memory given back\n"
		"# TYPE perfcached_reclaim_released_bytes_total counter\n"
		"perfcached_reclaim_released_bytes_total %lu\n",
		pr.released_bytes);
	emit(&o, "# HELP perfcached_reclaim_punch_calls_total madvise calls that punched memory out, one per run\n"
		"# TYPE perfcached_reclaim_punch_calls_total counter\n"
		"perfcached_reclaim_punch_calls_total %lu\n", pr.punch_calls);
	emit(&o, "# HELP perfcached_reclaim_punch_groups_total 2 MB groups those calls covered\n"
		"# TYPE perfcached_reclaim_punch_groups_total counter\n"
		"perfcached_reclaim_punch_groups_total %lu\n", pr.punch_groups);
	emit(&o, "# HELP perfcached_reclaim_shrink_step_bytes the most give-back one tick may do\n"
		"# TYPE perfcached_reclaim_shrink_step_bytes gauge\n"
		"perfcached_reclaim_shrink_step_bytes %lu\n", pr.shrink_step);
	emit(&o, "# HELP perfcached_reclaim_giveback_disabled give-back latched off\n"
		"# TYPE perfcached_reclaim_giveback_disabled gauge\n"
		"perfcached_reclaim_giveback_disabled %d\n", pr.giveback_off);

	/* ---- per collection ---------------------------------------- */
	emit(&o, "# HELP perfcached_collection_entries live records\n"
		"# TYPE perfcached_collection_entries gauge\n");
	n = pc_store_count();
	for (i = 0; i < n; i++) {
		if (!pc_store_live(i))
			continue;   /* S69: a dropped collection */
		pcache_ht_totals_t t;
		char esc[128];

		pcache_ht_totals(pc_store_ht(i), &t);
		label_escape(pc_store_name(i), esc, sizeof esc);
		emit(&o, "perfcached_collection_entries{collection=\"%s\"} %lu\n",
			esc, t.entries);
	}
	/* S131: the overflow leg, and what the walk across it costs.  A
	 * table at its target load factor keeps the leg near empty; one
	 * that has stopped growing puts everything there, and the walk
	 * blocks every other maintenance duty for its whole duration.
	 * Alert on the leg, not on entries. */
	emit(&o, "# HELP perfcached_collection_overflow records in the overflow leg rather than in a bucket's slots\n"
		"# TYPE perfcached_collection_overflow gauge\n");
	for (i = 0; i < n; i++) {
		char esc[128];

		if (!pc_store_live(i))
			continue;
		label_escape(pc_store_name(i), esc, sizeof esc);
		emit(&o, "perfcached_collection_overflow{collection=\"%s\"} %u\n",
			esc, pcache_ht_overflow(pc_store_ht(i)));
	}
	emit(&o, "# HELP perfcached_collection_held_walk_seconds duration of the last held-bytes walk, which every other maintenance duty waits behind\n"
		"# TYPE perfcached_collection_held_walk_seconds gauge\n");
	for (i = 0; i < n; i++) {
		char esc[128];

		if (!pc_store_live(i))
			continue;
		label_escape(pc_store_name(i), esc, sizeof esc);
		emit(&o, "perfcached_collection_held_walk_seconds{collection=\"%s\"} %.6f\n",
			esc, pc_store_held_walk_us(i) / 1000000.0);
	}
	/* S120: the budget's parts per collection */
	emit(&o, "# HELP perfcached_collection_index_bytes index regions carved for the collection's table (creation and growth), never freed\n"
		"# TYPE perfcached_collection_index_bytes gauge\n");
	for (i = 0; i < n; i++) {
		if (!pc_store_live(i))
			continue;   /* S69: a dropped collection */
		char esc[128];

		label_escape(pc_store_name(i), esc, sizeof esc);
		emit(&o, "perfcached_collection_index_bytes{collection=\"%s\"} %lu\n",
			esc, pc_store_index_bytes(i));
	}
	emit(&o, "# HELP perfcached_collection_record_bytes the records held as the cells they occupy (class-rounded), from the walk\n"
		"# TYPE perfcached_collection_record_bytes gauge\n");
	for (i = 0; i < n; i++) {
		if (!pc_store_live(i))
			continue;   /* S69: a dropped collection */
		char esc[128];

		label_escape(pc_store_name(i), esc, sizeof esc);
		emit(&o, "perfcached_collection_record_bytes{collection=\"%s\"} %llu\n",
			esc, pc_store_held_cells(i));
	}
	emit(&o, "# HELP perfcached_collection_held_bytes key and value bytes of the records held, from the maintenance thread's paced walk\n"
		"# TYPE perfcached_collection_held_bytes gauge\n");
	for (i = 0; i < n; i++) {
		if (!pc_store_live(i))
			continue;   /* S69: a dropped collection */
		unsigned int age = 0;
		unsigned long long hb = pc_store_held(i, &age);
		char esc[128];

		if (age == (unsigned int)-1)
			continue;                  /* not walked yet: no sample */
		label_escape(pc_store_name(i), esc, sizeof esc);
		emit(&o, "perfcached_collection_held_bytes{collection=\"%s\"} %llu\n",
			esc, hb);
	}
	emit(&o, "# HELP perfcached_collection_hits_total read hits\n"
		"# TYPE perfcached_collection_hits_total counter\n");
	for (i = 0; i < n; i++) {
		if (!pc_store_live(i))
			continue;   /* S69: a dropped collection */
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
		if (!pc_store_live(i))
			continue;   /* S69: a dropped collection */
		pcache_ht_totals_t t;
		char esc[128];

		pcache_ht_totals(pc_store_ht(i), &t);
		label_escape(pc_store_name(i), esc, sizeof esc);
		emit(&o, "perfcached_collection_misses_total{collection=\"%s\"} %lu\n",
			esc, t.misses);
	}
	/* S151: the client-only view.  The totals above count every origin -
	 * a peer serving a pull and recovery probing its replay both land
	 * there - so a hit rate built from them is "anyone", not "clients". */
	emit(&o, "# HELP perfcached_collection_client_hits_total read hits from client workers only\n"
		"# TYPE perfcached_collection_client_hits_total counter\n");
	for (i = 0; i < n; i++) {
		if (!pc_store_live(i))
			continue;
		pcache_ht_totals_t t;
		char esc[128];

		pcache_ht_totals_client(pc_store_ht(i), &t);
		label_escape(pc_store_name(i), esc, sizeof esc);
		emit(&o, "perfcached_collection_client_hits_total{collection=\"%s\"} %lu\n",
			esc, t.hits);
	}
	emit(&o, "# HELP perfcached_collection_client_misses_total read misses from client workers only\n"
		"# TYPE perfcached_collection_client_misses_total counter\n");
	for (i = 0; i < n; i++) {
		if (!pc_store_live(i))
			continue;
		pcache_ht_totals_t t;
		char esc[128];

		pcache_ht_totals_client(pc_store_ht(i), &t);
		label_escape(pc_store_name(i), esc, sizeof esc);
		emit(&o, "perfcached_collection_client_misses_total{collection=\"%s\"} %lu\n",
			esc, t.misses);
	}

	/* ---- durability -------------------------------------------- */
	pc_wal_get_stats(&ws);
	/* S145: staged (acked, still in a producer ring) and unsynced (in the
	 * file, not yet fdatasync'd) are different losses - the first goes on
	 * kill -9 and can be DROPPED, the second goes only with the power. */
	emit(&o, "# HELP perfcached_wal_staged_records acknowledged writes not yet in the log file (lost on kill -9)\n"
		"# TYPE perfcached_wal_staged_records gauge\n"
		"perfcached_wal_staged_records %llu\n",
		ws.last_seq > ws.pending_hi ? ws.last_seq - ws.pending_hi : 0ULL);
	emit(&o, "# HELP perfcached_wal_unsynced_records records in the log file not yet fsynced (lost only to power loss)\n"
		"# TYPE perfcached_wal_unsynced_records gauge\n"
		"perfcached_wal_unsynced_records %llu\n",
		ws.pending_hi > ws.synced_seq ? ws.pending_hi - ws.synced_seq : 0ULL);
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
		/* S125: replica lag.  A node falling behind on inbound copies
		 * looks healthy from every other angle - it heartbeats, it
		 * serves clients fast, it stays a member - so these are the
		 * figures to alert on.  applied_total is a counter; the rest
		 * are the daemon's own 1 Hz readings, published as gauges
		 * rather than recomputed by the scraper, because the useful
		 * rate is the one measured against the real elapsed tick. */
		emit(&o, "# HELP perfcached_cluster_rx_applied_total replica records applied\n"
			"# TYPE perfcached_cluster_rx_applied_total counter\n"
			"perfcached_cluster_rx_applied_total %llu\n", cs.rx_applied);
		emit(&o, "# HELP perfcached_cluster_rx_applied_per_second replica records applied per second\n"
			"# TYPE perfcached_cluster_rx_applied_per_second gauge\n"
			"perfcached_cluster_rx_applied_per_second %u\n", cs.rx_applied_ps);
		emit(&o, "# HELP perfcached_cluster_rx_older_per_second replica copies refused as older per second\n"
			"# TYPE perfcached_cluster_rx_older_per_second gauge\n"
			"perfcached_cluster_rx_older_per_second %u\n", cs.rx_older_ps);
		emit(&o, "# HELP perfcached_cluster_replicas_short replicas minus live members, 0 when met (S157)\n"
			"# TYPE perfcached_cluster_replicas_short gauge\n"
			"perfcached_cluster_replicas_short %d\n", pc_cluster_replicas_short());
		emit(&o, "# HELP perfcached_cluster_rx_drops_total datagrams the receive buffers could not hold\n"
			"# TYPE perfcached_cluster_rx_drops_total counter\n"
			"perfcached_cluster_rx_drops_total %llu\n", cs.rx_drops);
		emit(&o, "# HELP perfcached_cluster_rx_drops_per_second datagrams dropped per second\n"
			"# TYPE perfcached_cluster_rx_drops_per_second gauge\n"
			"perfcached_cluster_rx_drops_per_second %u\n", cs.rx_drops_ps);
		emit(&o, "# HELP perfcached_cluster_rx_queue_bytes bytes waiting in the cluster receive queues\n"
			"# TYPE perfcached_cluster_rx_queue_bytes gauge\n"
			"perfcached_cluster_rx_queue_bytes %u\n", cs.rx_queue);
		emit(&o, "# HELP perfcached_cluster_rx_rcvbuf_bytes the effective receive buffer\n"
			"# TYPE perfcached_cluster_rx_rcvbuf_bytes gauge\n"
			"perfcached_cluster_rx_rcvbuf_bytes %u\n", cs.rx_rcvbuf);
	}

	if (o.len >= o.cap)
		o.len = o.cap - 1;
	buf[o.len] = 0;
	return o.len;
}
