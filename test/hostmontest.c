/* hostmontest.c - S380's host monitor (hostmon.c; the 0.7.2 split, wave
 * 1): hm_apply on seconds made here, the readers against this host's
 * /proc, hm_io_init's outcomes.  No stubs - it calls only libc.
 *
 *   A  the minute's ring: no percentage until a minute is held, then CPU
 *      busy over it (steal counted busy by the reader, a share here),
 *      this process's share of every CPU, the device's busy time - capped
 *      at 100 - and memory, with the huge-page pool out of the base;
 *      PERFCACHED_ALERT_FAST's divisor shortens the window
 *   B  one second's readings from this host: /proc/stat and /proc/meminfo
 *      read, the clock the caller's, no disk read when no device is
 *      watched
 *   C  the device: none without a directory, the error for one that is
 *      not there, and for a real one either its block device or why not
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "hostmon.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static void fresh(void)
{
	memset(&HM, 0, sizeof HM);
	HM.cpu_pct = HM.cpu_self_pct = HM.io_pct = HM.mem_pct = -1;
	HM.io_ok = 1;
}

/* second @k of a 4-CPU host at half busy, this process on one CPU, the
 * device busy 30% of the time */
static void second(int k, int fast, unsigned long long iot_per_s)
{
	struct hm_obs o;

	memset(&o, 0, sizeof o);
	o.stat_ok = 1;
	o.ncpu = 4;
	o.total = 400ULL * (unsigned long long)k;
	o.busy = 200ULL * (unsigned long long)k;
	o.self_us = 1000000ULL * (unsigned long long)k;
	o.io_ok = 1;
	o.iot = iot_per_s * (unsigned long long)k;
	o.now = 1000LL * (k + 1);
	hm_apply(&o, fast);
}

static void memory(unsigned long long total, unsigned long long avail, unsigned long long huge)
{
	struct hm_obs o;

	memset(&o, 0, sizeof o);
	o.mem_ok = 1;
	o.mem_total_kb = total;
	o.mem_avail_kb = avail;
	o.huge_kb = huge;
	hm_apply(&o, 0);
}

int main(void)
{
	struct hm_obs o;
	char dir[64], why[400];
	int k;

	/* A */
	fresh();
	for (k = 0; k < 60; k++)
		second(k, 0, 300);
	CHK(HM.cpu_pct == -1 && HM.io_pct == -1 && HM.have == 60,
		"A: 60 seconds held - not yet a minute, nothing said");
	second(60, 0, 300);
	CHK(HM.cpu_pct == 50 && HM.ncpu == 4, "A: the 61st - CPU 50%% busy over the minute, 4 CPUs");
	CHK(HM.cpu_self_pct == 25, "A: this process on one CPU of four - 25%% (USER_HZ 100)");
	CHK(HM.io_pct == 30, "A: the device busy 300 ms a second - 30%%");
	CHK(HM.i == 0 && HM.have == HM_N, "A: the ring wrapped, %d held", HM_N);
	fresh();
	for (k = 0; k <= 60; k++)
		second(k, 0, 1500);
	CHK(HM.io_pct == 100, "A: io_ticks past the wall clock (queued devices) - capped at 100%%");
	memory(16777216, 4194304, 0);
	CHK(HM.mem_pct == 75 && HM.mem_total_kb == 16777216 && HM.huge_kb == 0,
		"A: 4 GB of 16 available - 75%% used");
	memory(16777216, 4194304, 8388608);
	CHK(HM.mem_pct == 50 && HM.mem_total_kb == 8388608 && HM.huge_kb == 8388608,
		"A: an 8 GB huge-page pool - out of the base: 50%% used, not 75");
	memory(16777216, 9000000, 8388608);
	CHK(HM.mem_pct == 0, "A: more available than the base - 0%%");
	memory(16777216, 4194304, 20000000);
	CHK(HM.mem_pct == 75 && HM.huge_kb == 0, "A: a pool reported past the total - ignored");
	fresh();
	for (k = 0; k < 7; k++)
		second(k, 10, 300);
	CHK(HM.cpu_pct == -1, "A: PERFCACHED_ALERT_FAST=10 - 7 seconds, not yet its window");
	second(7, 10, 300);
	CHK(HM.cpu_pct == 50 && HM.io_pct == 30, "A: the 8th - its window (60 / 10 + 1) held, said");

	/* B */
	HM.io_ok = 0;
	hm_read(&o, 12345);
	CHK(o.now == 12345, "B: the reading carries the caller's clock");
	CHK(o.stat_ok && o.ncpu >= 1 && o.total >= o.busy && o.total > 0,
		"B: /proc/stat - read, %d CPU(s)", o.ncpu);
	CHK(o.mem_ok && o.mem_total_kb > 0 && o.mem_avail_kb <= o.mem_total_kb,
		"B: /proc/meminfo - read, %llu MB", o.mem_total_kb >> 10);
	CHK(!o.io_ok, "B: no device watched - no disk read");

	/* C */
	hm_io_init(NULL);
	CHK(!HM.io_ok && !strcmp(HM.io_why, "no WAL or state directory - disk busy is not watched"),
		"C: no directory - not watched, said");
	hm_io_init("/nonexistent/cfg");
	CHK(!HM.io_ok && strstr(HM.io_why, "/nonexistent/cfg: No such file or directory"),
		"C: a directory not there - not watched, the error said");
	snprintf(dir, sizeof dir, "/tmp/hostmontest.XXXXXX");
	if (!mkdtemp(dir))
		return 2;
	hm_io_init(dir);
	snprintf(why, sizeof why, "no block-device statistics for %s", dir);
	CHK(!strcmp(HM.path, dir) &&
		((HM.io_ok && HM.dev[0] && !strncmp(HM.io_stat, "/sys/dev/block/", 15)) ||
		 (!HM.io_ok && !strncmp(HM.io_why, why, strlen(why)))),
		"C: a real directory - %s", HM.io_ok ? HM.dev : "no block device, said why");
	rmdir(dir);

	printf("hostmontest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
