/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * statsreset.c - the stats reset (S123, S358): it zeroes or re-baselines
 * every running total, keeps the baselines of the cluster, proxy and WAL
 * planes the tree subtracts (since_*), and runs the reset schedule.  Owns
 * reset_mx, reset_run_mx, the three baselines and the schedule.  Moved
 * from verbs.c unchanged (the 0.7.2 split, wave 1 - doc/MODULARITY.md).
 */
#include <string.h>
#include <time.h>
#include <pthread.h>

#include "compat/dprint.h"
#include "core/pcache_htable.h"
#include "store.h"
#include "upstream.h"
#include "proto.h"                      /* pc_http_cache_drop */
#include "mail.h"
#include "iostat.h"
#include "alerts.h"
#include "cluster.h"
#include "wal.h"
#include "obs.h"                        /* pc_obs_unknown_reset */
#include "mgetfan.h"                    /* G8 */
#include "daemon.h"                     /* pc_doors_reset */
#include "verbs.h"
#include "statsreset.h"

/* S123: the running totals start again.  The door counters are
 * re-baselined in proto.c; the cluster plane's, the proxy plane's and the
 * WAL's structs are snapshotted here and the block below reports the
 * difference; the table's own counters go through the core's
 * pcache_ht_stats_reset(); the store's size tallies are zeroed.  Live
 * gauges - entries, buckets, open connections, memory, sequence numbers,
 * roles, pending peaks - are untouched. */
static pthread_mutex_t reset_mx = PTHREAD_MUTEX_INITIALIZER;
/* S358: one reset at a time.  Four callers now - the page (and through it
 * every peer), RESP's CONFIG RESETSTAT, the native verb and the schedule -
 * and pcache_ht_stats_reset() folds the current figures into its baseline:
 * two at once would fold them twice and every counter would read negative
 * (as a huge unsigned number).  Held across the whole reset; reset_mx stays
 * the short lock the readers take. */
static pthread_mutex_t reset_run_mx = PTHREAD_MUTEX_INITIALIZER;
static struct pc_cl_stats cl_base;
static struct pc_proxy_stats px_base;
static struct pc_wal_stats wal_base;
static long long reset_at_s;
static int sched_days, sched_at_s;     /* S358: 0 days = no schedule */
static long long next_reset_s;         /* S358: under reset_mx; 0 = none */

long long pc_stats_reset_at(long long at)
{
	struct pc_cl_stats cs;
	struct pc_proxy_stats px;
	struct pc_wal_stats ws;
	int i, n = pc_store_count();

	pthread_mutex_lock(&reset_run_mx);
	pc_doors_reset();
	pc_obs_unknown_reset();                /* S165 */
	for (i = 0; i < n; i++)
		if (pc_store_live(i))          /* S150 C: a retired slot holds no table */
			pcache_ht_stats_reset(pc_store_ht(i));
	pc_store_size_reset();
	pc_upstream_stats_reset();             /* S358: the read-through figures */
	pc_http_cache_drop();                  /* 0.6.2: no cached /stats from before */
	pc_mail_stats_reset();                 /* S352: the mailer's */
	pc_iostat_reset();                     /* 0.6.0: the syscall counts */
	pc_alerts_stats_reset();               /* S352c: the relays' */
	pc_mget_stats_reset();                 /* G8: the mget fan-out's */
	memset(&cs, 0, sizeof cs);
	memset(&px, 0, sizeof px);
	memset(&ws, 0, sizeof ws);
	if (pc_cluster_enabled()) {
		pc_cluster_get_stats(&cs);
		pc_proxy_get_stats(&px);
	}
	pc_wal_get_stats(&ws);
	pthread_mutex_lock(&reset_mx);
	cl_base = cs;
	px_base = px;
	wal_base = ws;
	reset_at_s = at;
	pthread_mutex_unlock(&reset_mx);
	pthread_mutex_unlock(&reset_run_mx);
	return at;
}

long long pc_stats_reset(void)
{
	return pc_stats_reset_at((long long)time(NULL));
}

/* S358: the page's reset - this node now, every live peer with the same
 * time (M_STATS_RESET), so the fleet's figures all count from one moment.
 * CONFIG RESETSTAT and the native verb stay this node's alone. */
long long pc_stats_reset_fleet(void)
{
	long long at = pc_stats_reset();

	pc_cluster_stats_reset_send(at);
	return at;
}

/* S358: days from 1970-01-01 to the civil date y-m-d (H. Hinnant) */
static long long days_from_civil(long long y, unsigned int m, unsigned int d)
{
	long long era;
	unsigned int yoe, doy, doe;

	y -= m <= 2;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = (unsigned int)(y - era * 400);
	doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (long long)doe - 719468;
}

/* S358: the first scheduled reset after @now, in LOCAL time - so every
 * node of a fleet in one timezone resets at the same moment and reports
 * the same since.reset_at.  Every @sched_days days at sched_at_s past
 * midnight; 7 days = every Monday, 2-6 days count from a Monday (day
 * number + 3, 1970-01-05 being one) so the fleet agrees on which days. */
static long long sched_next(time_t now)
{
	struct tm tm, t;
	int k;

	localtime_r(&now, &tm);
	for (k = 0; k <= sched_days + 1; k++) {
		time_t at;

		t = tm;
		t.tm_mday += k;
		t.tm_hour = sched_at_s / 3600;
		t.tm_min = sched_at_s / 60 % 60;
		t.tm_sec = sched_at_s % 60;
		t.tm_isdst = -1;
		at = mktime(&t);               /* normalises the date */
		if (at == (time_t)-1 || at <= now)
			continue;
		if (sched_days > 1 && (days_from_civil(t.tm_year + 1900,
		        (unsigned int)t.tm_mon + 1, (unsigned int)t.tm_mday) + 3) %
		        sched_days)
			continue;
		return (long long)at;
	}
	return (long long)now + 86400LL * sched_days;
}

void pc_stats_reset_schedule(int days, int at_s)
{
	pthread_mutex_lock(&reset_mx);
	sched_days = days;
	sched_at_s = at_s;
	next_reset_s = days ? sched_next(time(NULL)) : 0;
	pthread_mutex_unlock(&reset_mx);
}

void pc_stats_reset_tick(void)
{
	long long due, now = (long long)time(NULL);

	pthread_mutex_lock(&reset_mx);
	due = next_reset_s;
	pthread_mutex_unlock(&reset_mx);
	if (!due || now < due)
		return;
	LM_NOTICE("statistics reset: the %d-day schedule - /stats and the page "
		"count from here (/metrics is never reset)\n", sched_days);
	pc_stats_reset_at(due);
	pthread_mutex_lock(&reset_mx);
	next_reset_s = sched_next((time_t)now);
	pthread_mutex_unlock(&reset_mx);
}

/* S123/S358: when the running totals were last reset (0 = never - they
 * count from the start) and when the schedule resets them next (0 = no
 * schedule) */
void statsreset_times(long long *at, long long *next)
{
	pthread_mutex_lock(&reset_mx);
	*at = reset_at_s;
	*next = next_reset_s;
	pthread_mutex_unlock(&reset_mx);
}

void since_cl(struct pc_cl_stats *s)
{
	struct pc_cl_stats b;

	pthread_mutex_lock(&reset_mx);
	b = cl_base;
	pthread_mutex_unlock(&reset_mx);
	s->hb_sent -= b.hb_sent;
	s->hb_seen -= b.hb_seen;
	s->bad_auth -= b.bad_auth;
	s->hb_watchdog -= b.hb_watchdog;
	s->apply_stalls -= b.apply_stalls;      /* S216; the max is a gauge */
	s->epoch_refused -= b.epoch_refused;    /* S221 */
	s->epoch_commits -= b.epoch_commits;    /* 12nu */
	s->goodbye_refused -= b.goodbye_refused;  /* CH-13 */
	s->sender_refused -= b.sender_refused;    /* CH-12 */
	s->id_collisions -= b.id_collisions;      /* CH-12 */
	s->mcast_stray -= b.mcast_stray;
	s->joins -= b.joins;
	s->assigns -= b.assigns;
	s->elections -= b.elections;
	s->demotions -= b.demotions;
	s->pull_sent -= b.pull_sent;
	s->pull_served -= b.pull_served;
	s->pull_fetching -= b.pull_fetching;   /* 12nt */
	s->pull_fetched -= b.pull_fetched;
	s->fwd_through -= b.fwd_through;       /* 12nt TC-2 */
	s->fwd_through_acked -= b.fwd_through_acked;
	s->pull_hits -= b.pull_hits;
	s->pull_misses -= b.pull_misses;
	s->pull_timeouts -= b.pull_timeouts;
	s->pend_exhausted -= b.pend_exhausted;
	s->fwd_no_route -= b.fwd_no_route;
	s->fwd_send_fail -= b.fwd_send_fail;
	s->fwd_reply_too_big -= b.fwd_reply_too_big;   /* S319 */
	s->tomb_sent -= b.tomb_sent;
	s->tomb_applied -= b.tomb_applied;
	s->neg_hits -= b.neg_hits;
	s->neg_displaced -= b.neg_displaced;                     /* RV-15 */
	s->tomb_displaced -= b.tomb_displaced;
	s->joins_rejected -= b.joins_rejected;
	s->reconciled -= b.reconciled;
	s->reconcile_probed -= b.reconcile_probed;
	s->reconcile_holdq -= b.reconcile_holdq;                 /* S394 */
	s->holdq_served -= b.holdq_served;
	s->repl_held_recovered -= b.repl_held_recovered;
	s->pull_held_recovered -= b.pull_held_recovered;
	s->reconcile_dig_asked -= b.reconcile_dig_asked;         /* S261 */
	s->reconcile_dig_diff -= b.reconcile_dig_diff;
	s->reconcile_dig_skipped -= b.reconcile_dig_skipped;
	s->reconcile_dig_fallback -= b.reconcile_dig_fallback;
	s->repl_sweep_cycles -= b.repl_sweep_cycles;   /* S212; the clocks are gauges */
	s->reconcile_deferred -= b.reconcile_deferred;
	s->sync_sent -= b.sync_sent;
	s->sync_rx -= b.sync_rx;
	s->sync_ack -= b.sync_ack;
	s->sync_bad -= b.sync_bad;
	s->map_pub -= b.map_pub;
	s->map_rx -= b.map_rx;
	s->map_stale -= b.map_stale;
	s->map_bad -= b.map_bad;
	s->place_map -= b.place_map;
	s->place_hrw -= b.place_hrw;
	/* S125: the two receive-plane TOTALS are counters and re-baseline
	 * with the rest.  The three per-second figures and the two queue
	 * gauges are live readings, not running totals, so they do not -
	 * the same rule the memory and connection gauges follow. */
	s->rx_applied -= b.rx_applied;
	s->rx_drops -= b.rx_drops;
}

void since_px(struct pc_proxy_stats *s)
{
	struct pc_proxy_stats b;

	pthread_mutex_lock(&reset_mx);
	b = px_base;
	pthread_mutex_unlock(&reset_mx);
	s->placed_local -= b.placed_local;
	s->placed_remote -= b.placed_remote;
	s->fwd_sent -= b.fwd_sent;
	s->fwd_served -= b.fwd_served;
	s->fwd_fails -= b.fwd_fails;
	s->migrated_out -= b.migrated_out;
	s->migrated_in -= b.migrated_in;
	s->migrate_skipped_big -= b.migrate_skipped_big;
	s->migrate_lost -= b.migrate_lost;
	s->migrate_retx -= b.migrate_retx;
	s->migrate_dgrams -= b.migrate_dgrams;
	s->bulk_out -= b.bulk_out;
	s->bulk_in -= b.bulk_in;
	s->repl_out -= b.repl_out;
	s->demotes_sent -= b.demotes_sent;
	s->demotes_applied -= b.demotes_applied;
	s->demotes_kept -= b.demotes_kept;
	s->loc_hits -= b.loc_hits;
	s->loc_clears -= b.loc_clears;
	s->recv_older -= b.recv_older;
	s->recv_tombstoned -= b.recv_tombstoned;
	s->recv_nomem -= b.recv_nomem;
	s->recv_stale_dropped -= b.recv_stale_dropped;
	s->recv_below_dropped -= b.recv_below_dropped;
	s->repl_bulk_lost -= b.repl_bulk_lost;
	s->repl_pushed -= b.repl_pushed;
	s->repl_skipped_dying -= b.repl_skipped_dying;
	s->boot_out -= b.boot_out;
	s->boot_in -= b.boot_in;
	s->boot_failed -= b.boot_failed;
	s->repl_groups -= b.repl_groups;
}

void since_wal(struct pc_wal_stats *s)
{
	struct pc_wal_stats b;

	pthread_mutex_lock(&reset_mx);
	b = wal_base;
	pthread_mutex_unlock(&reset_mx);
	s->appended -= b.appended;
	s->bytes -= b.bytes;
	s->dropped -= b.dropped;
	s->late -= b.late;
	s->recycles -= b.recycles;
	s->overruns -= b.overruns;
}
