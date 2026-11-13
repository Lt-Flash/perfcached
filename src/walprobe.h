/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* walprobe.h - measures the WAL device with the WAL's own I/O pattern and recommends a policy;
 * the report always carries the identity class, so a tmpfs number never reads as durable */
#ifndef PC_WALPROBE_H
#define PC_WALPROBE_H

#include <stddef.h>
#include "storage.h"

enum pc_probe_mode { PC_PROBE_AUTO = 0, PC_PROBE_ALWAYS, PC_PROBE_NO };

/* REPORTED with every number (iops mean nothing without block size and QD); phase B is serial
 * at QD 1 because the WAL thread is the daemon's single writer, so policy follows its pattern */
#define PC_WPROBE_SEQ_BS  (1 << 20)
#define PC_WPROBE_SYNC_BS 4096
#define PC_WPROBE_QD      1

struct pc_wal_probe {
	int valid;
	long long sync_bytes;          /* phase B volume: why the ceiling below is only one */
	char dev_id[64];               /* maj:min+fstype of the dir */
	double seq_mb_s;               /* seq write incl. final fdatasync */
	long long fsync_p50_us, fsync_p99_us;
	long long sync_iops;           /* synced 4K appends per second */
	int probed_secs;               /* wall time the probe spent */
};

struct pc_wal_policy {
	const char *fsync_recommend;   /* "always" | "everysec" */
	/* a CEILING from a burst, never a sustained rate: phase B writes at most 16 MB,
	 * which a write-back cache in front of the device absorbs */
	long long max_durable_wps;
	int segment_mb;                /* recommended segment size */
	/* the ring depth fsync = always needs here: it holds what arrives during one
	 * drain+fsync cycle, so a slower fdatasync needs a deeper ring.  0 = no probe */
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
