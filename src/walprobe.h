/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * walprobe.h — WAL storage probe + policy recommendation (task S12).
 *
 * The probe MEASURES the WAL directory's device with the WAL's own I/O
 * pattern (buffered appends + fdatasync): burst sequential bandwidth,
 * synced-append latency distribution (p50/p99), synced-append IOPS.
 * Identity (S11) stays the LABEL; these measurements are what policy
 * follows.  Results are cached in the directory keyed by device id -
 * `probe = auto` reuses a valid cache, `always` re-measures (optionally
 * with a longer sustained phase: a short probe sees a cheap SSD's SLC
 * burst, not its sustained cliff), `no` skips measuring entirely.
 *
 * The probe writes through a separate O_EXCL temp file, unlinked at
 * open, tens of MB at most.  A blazing number on tmpfs is never allowed
 * to read as durable: the report always carries the identity class.
 */
#ifndef PC_WALPROBE_H
#define PC_WALPROBE_H

#include <stddef.h>
#include "storage.h"

enum pc_probe_mode { PC_PROBE_AUTO = 0, PC_PROBE_ALWAYS, PC_PROBE_NO };

/* the measurement parameters, REPORTED with every number - iops and
 * latency mean nothing without block size and queue depth.  Phase A =
 * SEQ_BS buffered writes + one final fdatasync (the everysec pump
 * pattern); phase B = SYNC_BS write+fdatasync strictly serial, QD 1 -
 * honest because the WAL thread is the daemon's single writer.  A
 * higher-QD sweep would characterize the DEVICE, not the WAL: policy
 * follows the workload's own pattern. */
#define PC_WPROBE_SEQ_BS  (1 << 20)
#define PC_WPROBE_SYNC_BS 4096
#define PC_WPROBE_QD      1

struct pc_wal_probe {
	int valid;
	int cached;                    /* came from the cache file */
	char dev_id[64];               /* maj:min+fstype of the dir */
	double seq_mb_s;               /* seq write incl. final fdatasync */
	long long fsync_p50_us, fsync_p99_us;
	long long sync_iops;           /* synced 4K appends per second */
	int probed_secs;               /* wall time the probe spent */
};

struct pc_wal_policy {
	const char *fsync_recommend;   /* "always" | "everysec" */
	long long max_durable_wps;     /* safety-factored sustainable rate */
	int segment_mb;                /* recommended segment size */
	/* the ring depth fsync = always NEEDS on this device.  The pump
	 * fsyncs once per drained BATCH, so the ring does not have to hold
	 * the workload - it has to hold what arrives during ONE
	 * drain+fsync cycle.  A device whose fdatasync takes milliseconds
	 * therefore needs a proportionally deeper ring, and the default
	 * 1 MB silently dropped ~13% of acknowledged writes at 3 ms per
	 * fsync (measured 2026-08-29).  0 = no probe, cannot size it. */
	int ring_kb_always;
	const char *note;              /* one-line rationale */
};

/* compute the dir's device id (for cache validity) */
int pc_wal_dev_id(const char *dir, char *out, size_t cap);

/* run the measurement (sustain_secs > 0 extends the synced-append phase
 * for probe=always).  Returns 0 or -1 (dir unwritable etc.) */
int pc_wal_probe_run(const char *dir, int sustain_secs,
		struct pc_wal_probe *pr);

/* cache: <dir>/.pc-walprobe, invalid when the device id changed */
int pc_wal_probe_cache_load(const char *dir, struct pc_wal_probe *pr);
int pc_wal_probe_cache_store(const char *dir, const struct pc_wal_probe *pr);

/* the startup probe's result, retained for the stats verb (daemon.c);
 * NULL when no WAL, probe=no, or the probe failed */
const struct pc_wal_probe *pc_wal_probe_result(void);

/* the label+measurement pairing -> policy (identity may be NULL) */
void pc_wal_policy_from(const struct pc_wal_probe *pr,
		const struct pc_st_id *id, struct pc_wal_policy *pol);

/* human report block into buf; returns bytes */
int pc_wal_probe_format(const struct pc_wal_probe *pr,
		const struct pc_wal_policy *pol, char *buf, size_t cap);

#endif /* PC_WALPROBE_H */
