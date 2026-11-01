/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * hostmon.c - S380's host monitor: CPU busy, the state device's busy time
 * and host memory, read from /proc and /sys once a second and kept a
 * minute.  Moved from alerts.c unchanged but for its two inputs (the
 * 0.7.2 split, wave 1 - doc/MODULARITY.md); the alarms on these figures
 * stay in alerts.c.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include "hostmon.h"

/* CPU busy from /proc/stat, the state device's busy time (what iostat shows
 * as %util) from its io_ticks, host memory from MemAvailable - sampled once
 * a second; CPU and disk are averaged over the last minute.  NOT iowait or
 * PSI io: on io_uring before kernel 6.15 those read ~100% at idle (12lx). */
struct hostmon HM = { .cpu_pct = -1, .cpu_self_pct = -1, .io_pct = -1, .mem_pct = -1 };

static int hm_proc_stat(unsigned long long *busy, unsigned long long *total,
		int *ncpu)
{
	unsigned long long v[8] = { 0 };
	char line[512];
	FILE *f = fopen("/proc/stat", "r");
	int n = 0, got = 0;

	if (!f)
		return -1;
	while (fgets(line, sizeof line, f)) {
		if (!strncmp(line, "cpu ", 4))
			got = sscanf(line + 4, "%llu %llu %llu %llu %llu %llu %llu %llu",
				&v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
		else if (!strncmp(line, "cpu", 3) && line[3] >= '0' && line[3] <= '9')
			n++;
	}
	fclose(f);
	if (got < 4)
		return -1;
	/* user nice system idle iowait irq softirq steal; guest time is in
	 * user already.  Steal is busy: the CPU was not this machine's. */
	*busy = v[0] + v[1] + v[2] + v[5] + v[6] + v[7];
	*total = *busy + v[3] + v[4];
	*ncpu = n ? n : 1;
	return 0;
}

/* MemTotal, MemAvailable and the huge-page pool (Hugetlb: every size,
 * reserved whether used or not - 0 on a kernel that does not say) */
static int hm_meminfo(unsigned long long *total_kb, unsigned long long *avail_kb,
		unsigned long long *huge_kb)
{
	char line[256];
	FILE *f = fopen("/proc/meminfo", "r");
	int got = 0;

	*huge_kb = 0;
	if (!f)
		return -1;
	while (fgets(line, sizeof line, f)) {
		got += (sscanf(line, "MemTotal: %llu", total_kb) == 1) +
			(sscanf(line, "MemAvailable: %llu", avail_kb) == 1);
		if (sscanf(line, "Hugetlb: %llu", huge_kb) == 1)
			break;                 /* the last of the three */
	}
	fclose(f);
	return got == 2 && *total_kb ? 0 : -1;
}

/* this process's huge pages in use (HugetlbPages:), kB */
static unsigned long long hm_huge_self_kb(void)
{
	char line[256];
	unsigned long long kb = 0;
	FILE *f = fopen("/proc/self/status", "r");

	if (!f)
		return 0;
	while (fgets(line, sizeof line, f))
		if (sscanf(line, "HugetlbPages: %llu", &kb) == 1)
			break;
	fclose(f);
	return kb;
}

static unsigned long long hm_rss_mb(void)
{
	unsigned long long pages = 0, rss = 0;
	FILE *f = fopen("/proc/self/statm", "r");

	if (!f)
		return 0;
	if (fscanf(f, "%llu %llu", &pages, &rss) != 2)
		rss = 0;
	fclose(f);
	return rss * (unsigned long long)sysconf(_SC_PAGESIZE) >> 20;
}

static int hm_io_ticks(unsigned long long *ticks)
{
	unsigned long long v[10];
	FILE *f = fopen(HM.io_stat, "r");
	int n;

	if (!f)
		return -1;
	n = fscanf(f, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
		&v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8],
		&v[9]);
	fclose(f);
	if (n < 10)
		return -1;
	*ticks = v[9];                     /* io_ticks: ms with I/O in flight */
	return 0;
}

static int hm_dev_stat(unsigned maj, unsigned min)
{
	char link[96], tgt[256];
	const char *b;
	ssize_t n;

	snprintf(HM.io_stat, sizeof HM.io_stat, "/sys/dev/block/%u:%u/stat",
		maj, min);
	if (access(HM.io_stat, R_OK) != 0)
		return -1;
	snprintf(link, sizeof link, "/sys/dev/block/%u:%u", maj, min);
	n = readlink(link, tgt, sizeof tgt - 1);
	if (n > 0) {
		tgt[n] = 0;
		b = strrchr(tgt, '/');
		snprintf(HM.dev, sizeof HM.dev, "%.63s", b ? b + 1 : tgt);
	} else
		snprintf(HM.dev, sizeof HM.dev, "%u:%u", maj, min);
	return 0;
}

/* the source of the mount holding @path (/proc/self/mountinfo: the longest
 * mount point that is a prefix of it, the last one listed on a tie) */
static int hm_mount_source(const char *path, char *src, size_t sn)
{
	char line[1024], mp[512], s[256];
	const char *dash;
	size_t best = 0, l;
	FILE *f = fopen("/proc/self/mountinfo", "r");

	src[0] = 0;
	if (!f)
		return -1;
	while (fgets(line, sizeof line, f)) {
		if (sscanf(line, "%*s %*s %*s %*s %511s", mp) != 1)
			continue;
		dash = strstr(line, " - ");
		if (!dash || sscanf(dash + 3, "%*s %255s", s) != 1)
			continue;
		l = strlen(mp);
		if (strncmp(path, mp, l) || (l > 1 && path[l] && path[l] != '/'))
			continue;
		if (l >= best) {
			best = l;
			snprintf(src, sn, "%s", s);
		}
	}
	fclose(f);
	return src[0] ? 0 : -1;
}

void hm_io_init(const char *path)
{
	struct stat st, bs;
	char src[256] = "";

	HM.io_ok = 0;
	if (!path) {
		snprintf(HM.io_why, sizeof HM.io_why, "no WAL or state directory - "
			"disk busy is not watched");
		return;
	}
	snprintf(HM.path, sizeof HM.path, "%s", path);
	if (stat(path, &st) != 0) {
		snprintf(HM.io_why, sizeof HM.io_why, "%.120s: %.40s - disk busy is "
			"not watched", path, strerror(errno));
		return;
	}
	if (hm_dev_stat(major(st.st_dev), minor(st.st_dev)) == 0) {
		HM.io_ok = 1;
		return;
	}
	/* btrfs, zfs, overlay: an anonymous device - try the mount's source */
	if (hm_mount_source(path, src, sizeof src) == 0 && stat(src, &bs) == 0 &&
	    S_ISBLK(bs.st_mode) &&
	    hm_dev_stat(major(bs.st_rdev), minor(bs.st_rdev)) == 0) {
		HM.io_ok = 1;
		return;
	}
	snprintf(HM.io_why, sizeof HM.io_why, "no block-device statistics for "
		"%.80s (device %u:%u%s%.50s) - disk busy is not watched", path,
		major(st.st_dev), minor(st.st_dev), src[0] ? ", mounted from " : "",
		src);
}

void hm_read(struct hm_obs *o, long long now)
{
	struct rusage ru;

	memset(o, 0, sizeof *o);
	o->now = now;
	o->stat_ok = hm_proc_stat(&o->busy, &o->total, &o->ncpu) == 0;
	if (getrusage(RUSAGE_SELF, &ru) == 0)
		o->self_us = ((unsigned long long)ru.ru_utime.tv_sec +
			(unsigned long long)ru.ru_stime.tv_sec) * 1000000ULL +
			(unsigned long long)ru.ru_utime.tv_usec +
			(unsigned long long)ru.ru_stime.tv_usec;
	o->io_ok = HM.io_ok && hm_io_ticks(&o->iot) == 0;
	o->mem_ok = hm_meminfo(&o->mem_total_kb, &o->mem_avail_kb,
		&o->huge_kb) == 0;
	o->rss_mb = hm_rss_mb();
	o->huge_self_kb = hm_huge_self_kb();
}

/* under A.mx: the readings into the minute's ring, and the percentages;
 * @fast is PERFCACHED_ALERT_FAST's divisor (the window shortens with it) */
void hm_apply(const struct hm_obs *o, int fast)
{
	unsigned long long dt;
	long long dms;
	int cur = HM.i, old, win;

	if (o->stat_ok) {
		HM.busy[cur] = o->busy;
		HM.total[cur] = o->total;
		HM.ncpu = o->ncpu;
	}
	HM.self_us[cur] = o->self_us;
	if (o->io_ok) {
		HM.iot[cur] = o->iot;
		HM.iot_ms[cur] = o->now;
	}
	/* the sample a minute ago - a shorter window under
	 * PERFCACHED_ALERT_FAST, as every hold and clear time is */
	win = fast > 1 ? (HM_N - 1) / fast + 1 : HM_N - 1;
	if (win > HM_N - 1)
		win = HM_N - 1;
	old = (cur + HM_N - win) % HM_N;
	HM.i = (cur + 1) % HM_N;
	if (HM.have < HM_N)
		HM.have++;
	if (HM.have > win) {
		dt = HM.total[cur] - HM.total[old];
		if (dt > 0) {
			HM.cpu_pct = (int)((HM.busy[cur] - HM.busy[old]) * 100 / dt);
			/* this process's share of the same capacity: CPU seconds
			 * against the jiffies every CPU had */
			HM.cpu_self_pct = (int)((HM.self_us[cur] - HM.self_us[old]) *
				(unsigned long long)sysconf(_SC_CLK_TCK) / dt / 10000);
		}
		dms = HM.iot_ms[cur] - HM.iot_ms[old];
		if (HM.io_ok && dms > 0 && HM.iot_ms[old]) {
			unsigned long long p = (HM.iot[cur] - HM.iot[old]) * 100 /
				(unsigned long long)dms;

			HM.io_pct = p > 100 ? 100 : (int)p;
		}
	}
	if (o->mem_ok) {
		/* the huge-page pool is reserved memory, never available
		 * (MemAvailable leaves it out) - most often the arena's own.
		 * Measured beside it, or a pool for a 1 GB arena on a 2 GB
		 * host reads as 70% used by something else */
		unsigned long long base = o->mem_total_kb;

		if (o->huge_kb && o->huge_kb < base)
			base -= o->huge_kb;
		HM.mem_total_kb = base;
		HM.mem_avail_kb = o->mem_avail_kb;
		HM.mem_pct = o->mem_avail_kb >= base ? 0 :
			(int)(100 - o->mem_avail_kb * 100 / base);
		HM.huge_kb = o->huge_kb < o->mem_total_kb ? o->huge_kb : 0;
	}
	HM.rss_mb = o->rss_mb;
	HM.huge_self_kb = o->huge_self_kb;
}
