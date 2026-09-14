/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * walprobe.c — WAL storage probe + policy recommendation (task S12).
 * See walprobe.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/vfs.h>

#include "compat/dprint.h"
#include "walprobe.h"

#define SEQ_CAP_MB    64
#define SEQ_CAP_US    1500000          /* phase A ceiling: 1.5s */
#define APPENDS_MIN   200
#define APPENDS_CAP   4000
#define APPEND_CAP_US 1200000          /* phase B ceiling (auto): 1.2s */

static long long now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int pc_wal_dev_id(const char *dir, char *out, size_t cap)
{
	struct stat st;
	struct statfs sf;

	if (stat(dir, &st) != 0 || statfs(dir, &sf) != 0)
		return -1;
	snprintf(out, cap, "%u:%u/%lx", major(st.st_dev), minor(st.st_dev),
		(unsigned long)sf.f_type);
	return 0;
}

static int cmp_ll(const void *a, const void *b)
{
	long long x = *(const long long *)a, y = *(const long long *)b;

	return x < y ? -1 : x > y;
}

int pc_wal_probe_run(const char *dir, int sustain_secs,
		struct pc_wal_probe *pr)
{
	char path[512], *block;
	long long *lat, t0, t1, tstart, budget_us;
	long long total = 0;
	size_t nlat = 0, maxlat;
	int fd, mb = 0;

	memset(pr, 0, sizeof *pr);
	if (pc_wal_dev_id(dir, pr->dev_id, sizeof pr->dev_id) != 0)
		return -1;

	snprintf(path, sizeof path, "%s/.pc-walprobe.tmp.%d", dir, getpid());
	fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
	if (fd < 0) {
		LM_ERR("wal probe: cannot create a file in %s (%s)\n", dir,
			strerror(errno));
		return -1;
	}
	unlink(path);                      /* separate file, already unlinked */

	block = malloc(PC_WPROBE_SEQ_BS);
	maxlat = (size_t)(APPENDS_CAP + (sustain_secs > 0 ?
		sustain_secs * APPENDS_CAP : 0));
	lat = malloc(maxlat * sizeof *lat);
	if (!block || !lat) {
		free(block);
		free(lat);
		close(fd);
		return -1;
	}
	memset(block, 0x5A, PC_WPROBE_SEQ_BS);
	tstart = now_us();

	/* phase A: sequential bandwidth, the final fdatasync included -
	 * buffered writes alone measure the page cache, not the device */
	t0 = now_us();
	while (mb < SEQ_CAP_MB && now_us() - t0 < SEQ_CAP_US) {
		if (write(fd, block, PC_WPROBE_SEQ_BS) != PC_WPROBE_SEQ_BS)
			goto fail;
		mb++;
	}
	if (fdatasync(fd) != 0)
		goto fail;
	t1 = now_us();
	pr->seq_mb_s = (double)mb * 1e6 / (double)(t1 - t0);

	/* phase B: synced 4K appends - the WAL's actual pattern */
	budget_us = sustain_secs > 0 ? (long long)sustain_secs * 1000000
		: APPEND_CAP_US;
	t0 = now_us();
	while (nlat < maxlat &&
	       (now_us() - t0 < budget_us || nlat < APPENDS_MIN)) {
		long long a = now_us();

		if (write(fd, block, PC_WPROBE_SYNC_BS) != PC_WPROBE_SYNC_BS ||
		        fdatasync(fd) != 0)
			goto fail;
		lat[nlat++] = now_us() - a;
		total += lat[nlat - 1];
	}
	qsort(lat, nlat, sizeof *lat, cmp_ll);
	pr->fsync_p50_us = lat[nlat / 2];
	pr->fsync_p99_us = lat[(size_t)((double)nlat * 0.99)];
	pr->sync_iops = total ? (long long)nlat * 1000000 / total : 0;
	pr->sync_bytes = (long long)nlat * PC_WPROBE_SYNC_BS;
	pr->probed_secs = (int)((now_us() - tstart) / 1000000);
	pr->valid = 1;

	free(block);
	free(lat);
	close(fd);
	return 0;
fail:
	LM_ERR("wal probe: I/O failed on %s (%s)\n", dir, strerror(errno));
	free(block);
	free(lat);
	close(fd);
	return -1;
}


/* ---- policy: the label + the measurement ------------------------------- */

void pc_wal_policy_from(const struct pc_wal_probe *pr,
		const struct pc_st_id *id, struct pc_wal_policy *pol)
{
	int network = id && (id->cls == PC_ST_NETWORK);
	int memory = id && (id->cls == PC_ST_MEMORY);

	memset(pol, 0, sizeof *pol);
	pol->fsync_recommend = "everysec";
	pol->segment_mb = 64;
	pol->ring_kb_always = 0;

	if (!pr || !pr->valid) {
		pol->note = "no probe: conservative defaults";
		pol->max_durable_wps = 0;
		return;
	}

	/* Ring depth for fsync = always, from the ONE number that governs
	 * it: how long a drain cycle is blocked in fdatasync.  Scaled off
	 * an NVMe-class baseline (300us), where the shipped 1 MB default
	 * is already right, and clamped to the config's own range. */
	{
		long long mult = (pr->fsync_p99_us + 299) / 300;

		if (mult < 1)
			mult = 1;
		pol->ring_kb_always = (int)(1024 * mult);
		if (pol->ring_kb_always > 16384)
			pol->ring_kb_always = 16384;
	}

	/* safety factor: measured burst is optimistic, network stalls are
	 * hangs - halve locally, quarter on network-class stacks */
	pol->max_durable_wps = pr->sync_iops / (network ? 4 : 2);

	if (memory) {
		pol->fsync_recommend = "everysec";
		pol->note = "tmpfs: blazing numbers are NOT durability - "
			"crash-safe only";
		return;
	}
	if (network) {
		pol->note = "network-class storage: stalls hang, everysec + "
			"deep rings + watchdog";
		return;
	}
	if (pr->fsync_p99_us <= 300 && pr->sync_iops >= 20000) {
		pol->fsync_recommend = "always";
		pol->note = "NVMe-class sync latency: fsync=always viable";
	} else if (pr->fsync_p99_us <= 2000) {
		pol->note = "SSD-class: always only at low rates, everysec "
			"recommended";
	} else {
		pol->segment_mb = 256;
		pol->note = "ms-class sync latency: everysec, large batches, "
			"bigger segments";
	}
}

int pc_wal_probe_format(const struct pc_wal_probe *pr,
		const struct pc_wal_policy *pol, char *buf, size_t cap)
{
	size_t n = 0;

	if (pr && pr->valid)
		n += (size_t)snprintf(buf + n, cap - n,
			"wal: probe dev %s: seq %.0f MB/s (%dKB buffered + "
			"final fdatasync), synced-append p50 %lldus p99 %lldus, "
			"%lld iops (%dKB+fdatasync, QD%d, single writer; %ds)\n",
			pr->dev_id, pr->seq_mb_s,
			PC_WPROBE_SEQ_BS >> 10,
			pr->fsync_p50_us, pr->fsync_p99_us, pr->sync_iops,
			PC_WPROBE_SYNC_BS >> 10, PC_WPROBE_QD,
			pr->probed_secs);
	else
		n += (size_t)snprintf(buf + n, cap - n, "wal: probe skipped\n");
	if (pol && n < cap)
		n += (size_t)snprintf(buf + n, cap - n,
			"wal: policy: fsync %s recommended, segments %d MB "
			"(%s); burst ceiling ~%lld synced writes/s measured "
			"over %lld KB unbatched - an UPPER BOUND, not a "
			"sustained rate: a write-back cache (host page cache "
			"on a VM) absorbs a burst this small, and the pump "
			"group-commits.  Verify with fio before fsync = "
			"always.\n",
			pol->fsync_recommend, pol->segment_mb,
			pol->note ? pol->note : "", pol->max_durable_wps,
			pr && pr->valid ? pr->sync_bytes >> 10 : 0);
	return (int)(n < cap ? n : cap);
}
