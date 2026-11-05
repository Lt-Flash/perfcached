/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * hostmon.h - S380's host monitor (hostmon.c), for alerts.c: the minute's
 * ring and the percentages it reads, one second's readings, and the three
 * steps - pick the device, read, apply.
 */
#ifndef PC_HOSTMON_H
#define PC_HOSTMON_H

#define HM_N    61                     /* a minute of 1 s samples, plus one */
#define HM_HYST 5                      /* points under a level before it drops */

struct hostmon {
	unsigned long long busy[HM_N], total[HM_N], self_us[HM_N], iot[HM_N];
	long long iot_ms[HM_N];
	int i, have, ncpu, io_ok;
	char path[256], dev[64], io_stat[96], io_why[256];
	int cpu_pct, cpu_self_pct, io_pct, mem_pct;  /* -1 = not known yet */
	unsigned long long mem_total_kb, mem_avail_kb, rss_mb;
	unsigned long long huge_kb, huge_self_kb;    /* the huge-page pool, and
	                                              * this process's part */
};
extern struct hostmon HM;

/* one second's readings - taken BEFORE the tick takes A.mx, so no file is
 * read while the stats reader may be waiting on it */
struct hm_obs {
	int stat_ok, io_ok, mem_ok, ncpu;
	unsigned long long busy, total, self_us, iot, mem_total_kb, mem_avail_kb;
	unsigned long long rss_mb, huge_kb, huge_self_kb;
	long long now;
};

/* the device holding @path (the WAL or state directory) for disk busy; NULL,
 * or no block-device statistics for it: not watched, HM.io_why says why */
void hm_io_init(const char *path);
/* one second's readings at @now (monotonic ms), no lock held */
void hm_read(struct hm_obs *o, long long now);
void hm_apply(const struct hm_obs *o, int fast);

#endif
