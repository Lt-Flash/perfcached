/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* rdb.h - the RDB snapshot: a fuzzy lock-free walk, rate limited so the WAL keeps the disk, to a
 * temp file renamed into place; it records the WAL seq at walk start, recovery's replay point */
#ifndef PC_RDB_H
#define PC_RDB_H

#include "pc_attr.h"

#include <stddef.h>
#include <stdint.h>

#include "rdb_rules.h"

/* LE: u32 magic, u32 ver, u64 wal_marker, u64 wall_time, then u8-tagged entries (1 = collection
 * [u8 len][name], 2 = record), 0xFF = end + u32 crc32c of all before it; TTLs in wall-clock s */
#define PC_RDB_MAGIC 0x44524350u       /* 'PCRD' LE */
#define PC_RDB_FILE  "dump.rdb"
#define PC_RDB_FMT   3                 /* format 1 still loads: records versioned 0,
                                        * which a peer's copy outranks */
/* a record's fixed header: [klen4][vlen4][exp8], + [ver8] from format 2,
 * + [flags1] (PCACHE_F_JSON) from format 3 */
#define PC_RDB_RHDR(fmt) ((fmt) >= 3 ? 25u : (fmt) >= 2 ? 24u : 16u)

struct pc_rdb_stats {
	int enabled, running;
	unsigned long long saves, last_bytes, last_marker;
	/* last_marker is the last ATTEMPT's; safe_marker the last
	 * PUBLISHED snapshot's - the one WAL recycling goes by */
	unsigned long long safe_marker, save_errors;
	long long last_dur_ms, last_unix;
	/* seconds since the last COMPLETED snapshot, on the
	 * monotonic clock; -1 before this process has completed one */
	long long last_age_s;
	/* when the last completed snapshot STARTED, on pc_rdb_mono_us() */
	long long last_start_us;
};

/* init (startup, before threads).  rules may be 0 (manual saves only).
 * probe_mb_s feeds the auto budget; max_mb_s > 0 overrides it. */
PC_MUST_CHECK int pc_rdb_init(const char *dir, const struct pc_rdb_rule *rules,
		int nrules, int max_mb_s, double probe_mb_s);

/* the longest configured snapshot interval (seconds); 0 = none */
int pc_rdb_max_interval_s(void);

/* the marker of the last COMPLETED snapshot (0 = none yet): the only
 * safe boundary for WAL-recycle accounting */
unsigned long long pc_rdb_safe_marker(void);

/* the marker of the snapshot recovery LOADED: safe for recycling until
 * this process completes its own (raises only) */
void pc_rdb_seed_marker(unsigned long long marker);

/* the RDB thread body: evaluates triggers ~1/s, runs saves.  Returns
 * only when @stop becomes nonzero. */
void pc_rdb_thread(volatile int *stop);

/* the `save` verb: returns 0 = save requested, 1 = already running */
int pc_rdb_request_save(void);
/* no snapshot while held, not even a clean stop's: a copy of the fleet's state is incomplete,
 * and the files must keep what the node held before it.  _busy: a save is walking now */
void pc_rdb_hold(int on);
int pc_rdb_held(void);
int pc_rdb_busy(void);

/* synchronous save - the post-recovery checkpoint (startup only,
 * before the RDB thread exists) */
PC_MUST_CHECK int pc_rdb_save_sync(void);

/* the unthrottled snapshot a clean stop takes once every thread is joined; tmp + rename as
 * any save, so a stop killed part-way leaves the previous one whole and the WAL holds the rest */
PC_MUST_CHECK int pc_rdb_save_on_stop(void);

void pc_rdb_get_stats(struct pc_rdb_stats *out);
/* an unthrottled snapshot for the WAL's heal, and the clock its start is stamped on */
void pc_rdb_request_urgent(void);
long long pc_rdb_mono_us(void);
const char *pc_rdb_dir(void);      /* "" until pc_rdb_init */

/* validate + summarize a snapshot (tests, -R).
 * Returns record count, or -1 (missing/corrupt - *why says which). */
long pc_rdb_validate(const char *dir, unsigned long long *marker,
		long *ncols, const char **why);

#endif /* PC_RDB_H */
