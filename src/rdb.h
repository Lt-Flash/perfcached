/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * rdb.h — the RDB snapshot (task S14).
 *
 * A fuzzy, lock-free walk of every collection (the S9 walk core)
 * streamed to a temp file, then rename + DIRECTORY fsync - a torn
 * snapshot can never exist under the final name.  The file records the
 * WAL sequence AT WALK START: recovery (S15) loads the snapshot and
 * replays the WAL from that marker - records the walk raced are simply
 * re-applied, idempotent by the absolute-upsert rule.  No fork, no COW:
 * jitter-free by construction.
 *
 * The writer is RATE LIMITED (a checkpoint must never own the link and
 * blow the WAL's fsync latency): budget = the configured MB/s, or a
 * quarter of the probed sequential bandwidth when the config leaves it
 * at 0.  Triggers are Redis-style OR'd rules "SECS CHANGES, ..."
 * evaluated against the WAL sequence delta, plus the explicit `save`
 * verb.  Expired entries are skipped at write time; TTLs are stored as
 * absolute wall-clock seconds.
 *
 * File format (explicit LE):
 *   u32 magic 'PCRD' | u32 ver | u64 wal_marker | u64 wall_time
 *   entries:  u8 tag: 1 = collection [u8 len][name]
 *                     2 = record [u32 klen][u32 vlen][u64 expires]
 *                                [u64 lamport-ver, format 2+][k][v]
 *             0xFF = end, followed by u32 crc32c of everything before it
 *
 * Format 2 (A1) adds each record's Lamport version, so a snapshot puts a
 * record back as it was rather than renumbering it.  Format 1 still
 * loads; its records come back versioned 0 - unnumbered, which a peer's
 * copy outranks - rather than being refused.
 */
#ifndef PC_RDB_H
#define PC_RDB_H

#include <stddef.h>
#include <stdint.h>

#include "rdb_rules.h"

#define PC_RDB_MAGIC 0x44524350u       /* 'PCRD' LE */
#define PC_RDB_FILE  "dump.rdb"
#define PC_RDB_FMT   2                 /* 1 = pre-A1, no per-record version */

struct pc_rdb_stats {
	int enabled, running;
	unsigned long long saves, last_bytes, last_marker;
	long long last_dur_ms, last_unix;
};

/* init (startup, before threads).  rules may be 0 (manual saves only).
 * probe_mb_s feeds the auto budget; max_mb_s > 0 overrides it. */
int pc_rdb_init(const char *dir, const struct pc_rdb_rule *rules,
		int nrules, int max_mb_s, double probe_mb_s);

/* the longest configured snapshot interval (seconds); 0 = none */
int pc_rdb_max_interval_s(void);

/* the marker of the last COMPLETED snapshot (0 = none yet): the only
 * safe boundary for WAL-recycle accounting */
unsigned long long pc_rdb_safe_marker(void);

/* the RDB thread body: evaluates triggers ~1/s, runs saves.  Returns
 * only when @stop becomes nonzero. */
void pc_rdb_thread(volatile int *stop);

/* the `save` verb: returns 0 = save requested, 1 = already running */
int pc_rdb_request_save(void);

/* synchronous save - the post-recovery checkpoint (startup only,
 * before the RDB thread exists) */
int pc_rdb_save_sync(void);

void pc_rdb_get_stats(struct pc_rdb_stats *out);
const char *pc_rdb_dir(void);      /* "" until pc_rdb_init */

/* validate + summarize a snapshot (tests, -R; S15 builds load on it).
 * Returns record count, or -1 (missing/corrupt - *why says which). */
long pc_rdb_validate(const char *dir, unsigned long long *marker,
		long *ncols, const char **why);

#endif /* PC_RDB_H */
