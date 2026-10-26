/* statsresettest.c - the stats reset and its schedule (statsreset.c; the
 * 0.7.2 split, wave 1), every plane stubbed and the clock faked: this
 * file's time() is the one statsreset.o calls.
 *
 *   A  a reset: every running total's owner told once, the table's
 *      counters reset on the live slots only; the cluster, proxy and WAL
 *      baselines taken, and since_* subtract them from the counters and
 *      leave the gauges; reset_at is the time given
 *   B  a standalone node takes no cluster or proxy baseline (the WAL's
 *      still); the fleet reset is now, and the same time goes to the peers
 *   C  the schedule in UTC: daily at 01:00 - not due a second early, due
 *      on the second, the reset stamped with the scheduled time rather
 *      than the tick's, the next one a day on, with a NOTICE; 7 days =
 *      the next Monday; 2 and 3 days count from a Monday (day number + 3)
 *      so a fleet agrees on the days; 0 days = none, and a tick resets
 *      nothing
 *   D  the schedule in LOCAL time across a DST start (Melbourne,
 *      2026-10-04 02:00 AEST -> 03:00 AEDT): 01:00 stays 01:00 on the
 *      wall clock, so that day's interval is 23 h
 *   E  two resets at once never overlap (S358: the second would fold the
 *      figures into the table's baseline twice)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

#include "compat/dprint.h"
#include "core/pcache_htable.h"
#include "store.h"
#include "upstream.h"
#include "proto.h"
#include "mail.h"
#include "iostat.h"
#include "alerts.h"
#include "cluster.h"
#include "wal.h"
#include "obs.h"
#include "daemon.h"
#include "verbs.h"
#include "statsreset.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* ---- the clock ---------------------------------------------------------- */
static time_t fake_now;

time_t time(time_t *t)
{
	if (t)
		*t = fake_now;
	return fake_now;
}

/* ---- the stubs: every owner a reset tells, the planes' figures ---------- */
int compat_log_level = L_NOTICE;
static char last_log[256];
static int notices;

void compat_log(int level, const char *tag, const char *fmt, ...)
{
	va_list ap;

	(void)level;
	va_start(ap, fmt);
	vsnprintf(last_log, sizeof last_log, fmt, ap);
	va_end(ap);
	if (!strcmp(tag, "NOTICE"))
		notices++;
}

static int doors, unknowns, sizes, upstreams, httpc, mails, iostats, alerts, mgets;
static int cl_on = 1, cl_gets, px_gets, wal_gets, sent_n;
static long long sent_at;
static unsigned long long cl_hb, cl_rxq, px_pl, wal_app, wal_seq;
static char tables[3];
static int ht_resets[3], live[3] = { 1, 0, 1 };
static int in_reset, overlaps;

void pc_doors_reset(void) { doors++; }
void pc_obs_unknown_reset(void) { unknowns++; }
void pc_store_size_reset(void) { sizes++; }
void pc_upstream_stats_reset(void) { upstreams++; }
void pc_http_cache_drop(void) { httpc++; }
void pc_mail_stats_reset(void) { mails++; }
void pc_iostat_reset(void) { iostats++; }
void pc_alerts_stats_reset(void) { alerts++; }
void pc_mget_stats_reset(void) { mgets++; }
int pc_store_count(void) { return 3; }
int pc_store_live(int i) { return live[i]; }
pcache_htable_t *pc_store_ht(int i) { return (pcache_htable_t *)(void *)&tables[i]; }

void pcache_ht_stats_reset(pcache_htable_t *ht)
{
	if (__atomic_fetch_add(&in_reset, 1, __ATOMIC_SEQ_CST))
		__atomic_fetch_add(&overlaps, 1, __ATOMIC_SEQ_CST);
	ht_resets[(char *)(void *)ht - tables]++;
	usleep(50);
	__atomic_fetch_sub(&in_reset, 1, __ATOMIC_SEQ_CST);
}

int pc_cluster_enabled(void) { return cl_on; }

void pc_cluster_get_stats(struct pc_cl_stats *out)
{
	memset(out, 0, sizeof *out);
	out->hb_sent = cl_hb;
	out->rx_queue = (unsigned int)cl_rxq;
	cl_gets++;
}

void pc_proxy_get_stats(struct pc_proxy_stats *out)
{
	memset(out, 0, sizeof *out);
	out->placed_local = px_pl;
	px_gets++;
}

void pc_wal_get_stats(struct pc_wal_stats *out)
{
	memset(out, 0, sizeof *out);
	out->appended = wal_app;
	out->last_seq = wal_seq;
	wal_gets++;
}

int pc_cluster_stats_reset_send(long long at)
{
	sent_at = at;
	sent_n++;
	return 0;
}

/* ---- the harness ------------------------------------------------------- */
static void zone(const char *tz)
{
	setenv("TZ", tz, 1);
	tzset();
}

static long long next_at(void)
{
	long long at, nx;

	statsreset_times(&at, &nx);
	return nx;
}

static long long reset_at(void)
{
	long long at, nx;

	statsreset_times(&at, &nx);
	return at;
}

static void *resetter(void *arg)
{
	int i;

	(void)arg;
	for (i = 0; i < 50; i++)
		pc_stats_reset();
	return NULL;
}

int main(void)
{
	struct pc_cl_stats cs;
	struct pc_proxy_stats px;
	struct pc_wal_stats ws;
	int r0;

	zone("UTC0");

	/* A */
	memset(&cs, 0, sizeof cs);
	cs.hb_sent = 7;
	since_cl(&cs);
	CHK(cs.hb_sent == 7 && reset_at() == 0, "A: before any reset the totals count from the start");
	cl_hb = 10; cl_rxq = 9; px_pl = 4; wal_app = 100; wal_seq = 77;
	CHK(pc_stats_reset_at(5000) == 5000 && reset_at() == 5000, "A: a reset at 5000 is stamped 5000");
	CHK(doors == 1 && unknowns == 1 && sizes == 1 && upstreams == 1 &&
		httpc == 1 && mails == 1 && iostats == 1 && alerts == 1 &&
		mgets == 1, "A: every running total's owner told once");
	CHK(ht_resets[0] == 1 && ht_resets[1] == 0 && ht_resets[2] == 1,
		"A: the table's counters reset on the live slots only (S150)");
	memset(&cs, 0, sizeof cs);
	cs.hb_sent = 15;
	cs.rx_queue = 3;
	since_cl(&cs);
	CHK(cs.hb_sent == 5 && cs.rx_queue == 3, "A: since_cl - the counter less its baseline, the gauge as read");
	memset(&px, 0, sizeof px);
	px.placed_local = 6;
	since_px(&px);
	CHK(px.placed_local == 2, "A: since_px - likewise");
	memset(&ws, 0, sizeof ws);
	ws.appended = 150;
	ws.last_seq = 80;
	since_wal(&ws);
	CHK(ws.appended == 50 && ws.last_seq == 80, "A: since_wal - appended counts from the reset, the sequence is a gauge");

	/* B */
	cl_on = 0;
	r0 = cl_gets;
	cl_hb = 1000;
	wal_app = 200;
	fake_now = 6000;
	CHK(pc_stats_reset_fleet() == 6000 && sent_n == 1 && sent_at == 6000 &&
		reset_at() == 6000, "B: the fleet reset is now, and the peers are sent the same time");
	memset(&cs, 0, sizeof cs);
	cs.hb_sent = 15;
	since_cl(&cs);
	memset(&ws, 0, sizeof ws);
	ws.appended = 250;
	since_wal(&ws);
	CHK(cl_gets == r0 && cs.hb_sent == 15 && ws.appended == 50,
		"B: standalone - no cluster baseline taken, the WAL's still");
	cl_on = 1;

	/* C */
	fake_now = 1792369800;                     /* Mon 2026-10-19 00:30 UTC */
	pc_stats_reset_schedule(1, 3600);
	CHK(next_at() == 1792371600, "C: daily at 01:00 - next is 01:00 the same day");
	r0 = doors;
	fake_now = 1792371599;
	pc_stats_reset_tick();
	CHK(doors == r0, "C: a second early, the tick resets nothing");
	fake_now = 1792371605;
	notices = 0;
	pc_stats_reset_tick();
	CHK(doors == r0 + 1 && reset_at() == 1792371600,
		"C: due - reset, stamped with the scheduled 01:00:00, not the tick's 01:00:05");
	CHK(next_at() == 1792458000, "C: the next one a day on (Tue 01:00)");
	CHK(notices == 1 && strstr(last_log, "1-day schedule") != NULL,
		"C: the scheduled reset is logged: %s", last_log);
	fake_now = 1792584000;                     /* Wed 2026-10-21 12:00 */
	pc_stats_reset_schedule(7, 0);
	CHK(next_at() == 1792972800, "C: 7 days - the next Monday, 2026-10-26 00:00");
	pc_stats_reset_schedule(2, 0);
	CHK(next_at() == 1792713600, "C: 2 days - Fri 10-23 (day 20749 + 3 even), not Thu 10-22");
	pc_stats_reset_schedule(3, 0);
	CHK(next_at() == 1792627200, "C: 3 days - Thu 10-22 (day 20748 + 3 a multiple of 3)");
	pc_stats_reset_schedule(0, 0);
	r0 = doors;
	fake_now += 7 * 86400;
	pc_stats_reset_tick();
	CHK(next_at() == 0 && doors == r0, "C: 0 days - no schedule, and a tick a week later resets nothing");

	/* D */
	zone("AEST-10AEDT,M10.1.0,M4.1.0/3");
	fake_now = 1790951400;                     /* Sat 2026-10-03 00:30 AEST */
	pc_stats_reset_schedule(1, 3600);
	CHK(next_at() == 1790953200, "D: daily at 01:00 local - Sat 01:00 AEST");
	fake_now = 1790953200;
	pc_stats_reset_tick();
	CHK(next_at() == 1791039600, "D: then Sun 01:00 AEST, 24 h on");
	fake_now = 1791039600;
	pc_stats_reset_tick();
	CHK(next_at() == 1791122400 && next_at() - 1791039600 == 23 * 3600,
		"D: then Mon 01:00 AEDT - 23 h on, across the DST start");
	pc_stats_reset_schedule(0, 0);
	zone("UTC0");

	/* E */
	{
		pthread_t t1, t2;

		overlaps = 0;
		r0 = doors;
		pthread_create(&t1, NULL, resetter, NULL);
		pthread_create(&t2, NULL, resetter, NULL);
		pthread_join(t1, NULL);
		pthread_join(t2, NULL);
		CHK(doors == r0 + 100 && overlaps == 0,
			"E: 100 resets from two threads, none overlapping another (S358)");
	}

	printf("statsresettest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
