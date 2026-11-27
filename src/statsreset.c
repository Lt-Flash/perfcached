/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* statsreset.c - the stats reset: re-baselines every running total, keeps the cluster, proxy and
 * WAL baselines the tree subtracts (since_*), and runs the reset schedule */
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
#include "mgetfan.h"
#include "daemon.h"                     /* pc_doors_reset */
#include "verbs.h"
#include "statsreset.h"

/* the running totals start again: door counters re-baseline in proto.c, the planes' structs are
 * snapshotted here and since_* subtracts them; live gauges are untouched */
static pthread_mutex_t reset_mx = PTHREAD_MUTEX_INITIALIZER;
/* one reset at a time: pcache_ht_stats_reset() folds the current figures into its baseline, so two at
 * once would fold twice and read negative; held across the reset, reset_mx is the readers' short lock */
static pthread_mutex_t reset_run_mx = PTHREAD_MUTEX_INITIALIZER;
static struct pc_cl_stats cl_base;
static struct pc_proxy_stats px_base;
static struct pc_wal_stats wal_base;
static long long reset_at_s;
static int sched_days, sched_at_s;     /* 0 days = no schedule */
static long long next_reset_s;         /* under reset_mx; 0 = none */

long long pc_stats_reset_at(long long at)
{
	struct pc_cl_stats cs;
	struct pc_proxy_stats px;
	struct pc_wal_stats ws;
	int i, n = pc_store_count();

	pthread_mutex_lock(&reset_run_mx);
	pc_doors_reset();
	pc_obs_unknown_reset();
	for (i = 0; i < n; i++)
		if (pc_store_live(i))          /* a retired slot holds no table */
			pcache_ht_stats_reset(pc_store_ht(i));
	pc_store_size_reset();
	pc_upstream_stats_reset();             /* the read-through figures */
	pc_http_cache_drop();                  /* no cached /stats from before */
	pc_mail_stats_reset();
	pc_iostat_reset();                     /* the syscall counts */
	pc_alerts_stats_reset();               /* the relays' */
	pc_mget_stats_reset();
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

/* the page's reset: this node and every live peer at the same time (M_STATS_RESET), so the fleet
 * counts from one moment; CONFIG RESETSTAT and the native verb stay this node's */
long long pc_stats_reset_fleet(void)
{
	long long at = pc_stats_reset();

	pc_cluster_stats_reset_send(at);
	return at;
}

/* days from 1970-01-01 to the civil date y-m-d (H. Hinnant) */
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

/* the next scheduled reset after @now in LOCAL time, so a fleet in one timezone agrees: every
 * @sched_days days at sched_at_s; 7 = Mondays, 2-6 count from a Monday (day number + 3) */
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

/* when the totals were last reset (0 = never) and when the schedule resets them next (0 = none) */
void statsreset_times(long long *at, long long *next)
{
	pthread_mutex_lock(&reset_mx);
	*at = reset_at_s;
	*next = next_reset_s;
	pthread_mutex_unlock(&reset_mx);
}

/* a counter less its baseline; 0, not a wrapped figure, for a reading taken before a reset that
 * stored a newer baseline in between */
#define SINCE(f) (s->f = s->f > b.f ? s->f - b.f : 0)

void since_cl(struct pc_cl_stats *s)
{
	struct pc_cl_stats b;

	pthread_mutex_lock(&reset_mx);
	b = cl_base;
	pthread_mutex_unlock(&reset_mx);
	SINCE(hb_sent);
	SINCE(hb_seen);
	SINCE(bad_auth);
	SINCE(hb_watchdog);
	SINCE(apply_stalls);      /* the max is a gauge */
	SINCE(epoch_refused);
	SINCE(epoch_commits);
	SINCE(goodbye_refused);
	SINCE(sender_refused);
	SINCE(id_collisions);
	SINCE(mcast_stray);
	SINCE(joins);
	SINCE(assigns);
	SINCE(elections);
	SINCE(demotions);
	SINCE(members_asked);
	SINCE(members_sent);
	SINCE(pull_sent);
	SINCE(pull_served);
	SINCE(pull_fetching);
	SINCE(pull_fetched);
	SINCE(fwd_through);
	SINCE(fwd_through_acked);
	SINCE(pull_hits);
	SINCE(pull_misses);
	SINCE(pull_timeouts);
	SINCE(pend_exhausted);
	SINCE(pend_cq_dropped);
	SINCE(fwd_no_route);
	SINCE(fwd_send_fail);
	SINCE(fwd_reply_too_big);
	SINCE(tomb_sent);
	SINCE(tomb_applied);
	SINCE(neg_hits);
	SINCE(neg_displaced);
	SINCE(tomb_displaced);
	SINCE(joins_rejected);
	SINCE(reconciled);
	SINCE(reconcile_probed);
	SINCE(reconcile_holdq);
	SINCE(holdq_served);
	SINCE(repl_held_recovered);
	SINCE(pull_held_recovered);
	SINCE(reconcile_dig_asked);
	SINCE(reconcile_dig_diff);
	SINCE(reconcile_dig_skipped);
	SINCE(reconcile_dig_fallback);
	SINCE(repl_sweep_cycles);   /* the clocks are gauges */
	SINCE(reconcile_deferred);
	SINCE(reconcile_rounds);
	SINCE(reconcile_unruled);
	SINCE(sync_sent);
	SINCE(sync_rx);
	SINCE(sync_ack);
	SINCE(sync_bad);
	SINCE(map_pub);
	SINCE(map_rx);
	SINCE(map_stale);
	SINCE(map_bad);
	SINCE(place_map);
	SINCE(place_hrw);
	SINCE(tomb_group);
	SINCE(tomb_dgrams);
	SINCE(tomb_own);
	SINCE(sync_timeouts);
	SINCE(gap_asked);
	SINCE(gap_served);
	SINCE(gap_resent);
	SINCE(gap_floored);
	SINCE(gap_dead);
	SINCE(gap_tail);
	SINCE(acks_floors);
	SINCE(rebuild_sent);
	SINCE(rebuilds);
	SINCE(rebuild_covered);
	SINCE(reconcile_rewritten);
	/* the receive plane's TOTALS re-baseline; its per-second figures and queue gauges are live
	 * readings and do not */
	SINCE(rx_applied);
	SINCE(rx_drops);
}

void since_px(struct pc_proxy_stats *s)
{
	struct pc_proxy_stats b;

	pthread_mutex_lock(&reset_mx);
	b = px_base;
	pthread_mutex_unlock(&reset_mx);
	SINCE(placed_local);
	SINCE(placed_remote);
	SINCE(fwd_sent);
	SINCE(fwd_served);
	SINCE(fwd_fails);
	SINCE(migrated_out);
	SINCE(migrated_in);
	SINCE(migrate_skipped_big);
	SINCE(migrate_lost);
	SINCE(migrate_retx);
	SINCE(migrate_dgrams);
	SINCE(bulk_out);
	SINCE(bulk_in);
	SINCE(repl_out);
	SINCE(demotes_sent);
	SINCE(demotes_applied);
	SINCE(demotes_kept);
	SINCE(loc_hits);
	SINCE(loc_clears);
	SINCE(recv_older);
	SINCE(recv_tombstoned);
	SINCE(recv_nomem);
	SINCE(recv_stale_dropped);
	SINCE(recv_below_dropped);
	SINCE(repl_bulk_lost);
	SINCE(repl_pushed);
	SINCE(repl_skipped_dying);
	SINCE(boot_out);
	SINCE(boot_in);
	SINCE(boot_failed);
	SINCE(repl_groups);
	SINCE(spread_not_held);
	SINCE(spread_repaired);
	SINCE(spread_reclaimed);
	SINCE(spread_possess_sent);
	SINCE(spread_possess_kept);
	SINCE(spread_ack_parked);
	SINCE(spread_ack_kept);
	SINCE(spread_ack_dropped);
	SINCE(spread_ack_short);
	SINCE(spread_ack_lost);
	SINCE(spread_gap_repaired);
	SINCE(sweep_gate_asked);
	SINCE(sweep_gate_answered);
	SINCE(sweep_gate_unanswered);
	SINCE(sweep_gate_clean);
	SINCE(sweep_gate_idle);
	SINCE(sweep_gate_slots_asked);
	SINCE(sweep_gate_slots_diff);
	SINCE(sweep_gate_digested);
	SINCE(sweep_gate_walks);
	SINCE(dsync_armed);
	SINCE(dsync_rounds);
	SINCE(dsync_slots);
	SINCE(dsync_done);
	SINCE(dsync_stopped);
	SINCE(dsync_off);
}

void since_wal(struct pc_wal_stats *s)
{
	struct pc_wal_stats b;

	pthread_mutex_lock(&reset_mx);
	b = wal_base;
	pthread_mutex_unlock(&reset_mx);
	SINCE(appended);
	SINCE(bytes);
	SINCE(dropped);
	SINCE(late);
	SINCE(recycles);
	SINCE(overruns);
	SINCE(ring_waits);
	SINCE(ring_wait_ms);
	SINCE(full_waits);
	SINCE(full_wait_ms);
	SINCE(heals);
}
