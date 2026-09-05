/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * wal.h — the write-ahead log (task S13).
 *
 * Producers (worker threads) encode records into their own SPSC byte
 * ring; the WAL thread merge-drains the rings by sequence stamp, writes
 * group-committed batches into preallocated recycled segments, and
 * fsyncs per policy.  A full ring DROPS the record and counts it -
 * a worker never blocks on storage, ever.
 *
 * Ordering: the sequence is taken right after the table op commits, not
 * under the bucket lock - a preemption in that gap while a concurrent
 * writer hits the SAME key can invert two records across drain passes.
 * The WAL thread detects the inversion (seq below the written watermark)
 * and counts it (wal_late); records are idempotent absolute upserts, so
 * the impact needs a crash inside the window with no later write to the
 * key.  If wal_late ever moves in practice, the stamp moves under the
 * bucket lock (a small vendored-core hook) - measure first.
 *
 * Segment format (explicitly little-endian):
 *   header:  u32 magic 'PCWL' | u32 gen
 *   records: u32 len | u32 crc32c(gen+payload) | u32 gen | payload
 *   payload: u8 type (1 upsert, 2 del, 3 touch, 4 upsert+ver,
 *            5 touch+ver) |
 *            u8 collen | u16 keylen | u32 vallen | u64 expires (WALL
 *            seconds, 0 = never) | u64 seq | [u64 ver, type 4 only] |
 *            col | key | val
 *
 * Type 4 (A1) is type 1 plus the record's Lamport version, and is what
 * the producer writes now; 1 stays readable so a log written before the
 * field existed still replays - those records come back numbered 0,
 * which is the honest answer for a copy whose provenance predates
 * versioning, and lets a peer's copy outrank it rather than losing the
 * data outright.  The alternative - bumping the segment magic - would
 * have discarded exactly the tail A1 exists to protect.  A DELETE
 * carries no version: nothing ticks the clock on a remove, so there is
 * none to log (the delete-versus-stale-replay problem is task B7's, and
 * inventing a number here would prejudge it).
 *
 * Type 5 (touch+ver) arrived with A2.  A1 shipped without it on the
 * reasoning that pcache_ht_touch did not tick, so a touch had no version
 * to log - true then.  A2 made a receiver refuse a copy that is not
 * newer, which turned a TTL re-arm that kept its old number into a
 * duplicate the peer drops: the refresh stopped at the author, measured.
 * Touch ticks now, so it has a version, and the log has to carry it or
 * replay would put the record back under-numbered.
 * A zero len, a CRC mismatch or a stale gen ends the segment - recycled
 * segments are NOT re-zeroed (that is the fdatasync-only trick), the gen
 * echo is what fences the previous generation's records.
 */
#ifndef PC_WAL_H
#define PC_WAL_H

#include "pc_attr.h"

#include <stddef.h>
#include <stdint.h>

enum pc_wal_fsync { PC_WFSYNC_EVERYSEC = 0, PC_WFSYNC_ALWAYS, PC_WFSYNC_NO };

#define PC_WAL_MAGIC 0x4C574350u       /* 'PCWL' LE */
#define PC_WAL_HDR   8
#define PC_WAL_RECHDR 12               /* len + crc + gen */
#define PC_WAL_T_UPSERT 1
#define PC_WAL_T_DEL    2
#define PC_WAL_T_TOUCH  3
#define PC_WAL_T_UPSERT_V 4    /* upsert carrying the record's version */
#define PC_WAL_T_TOUCH_V  5    /* touch carrying the record's version */

struct pc_wal_stats {
	int enabled;
	const char *fsync_mode;
	unsigned long long appended, bytes, dropped, late, recycles;
	unsigned long long last_seq, synced_seq;
	/* the fixed ring cannot grow at runtime (a resize would stall the
	 * workers): free_segments counts segments whose records are all
	 * inside the last COMPLETED snapshot (reclaimable space); overruns
	 * counts recycles that overwrote NEWER records - each one shrank
	 * the replayable window and was logged as an error */
	unsigned long long overruns;
	int free_segments;
};

/* init at startup (before threads): provisions the segment ring with
 * real zero writes (thin/sparse backends), allocates @nthreads rings.
 * segment_mb/segments/ring_kb are validated config values. */
PC_MUST_CHECK
int pc_wal_init(const char *dir, enum pc_wal_fsync mode, int segment_mb,
		int segments, int ring_kb, int nthreads);

/* Read the recorded sequence before the WAL is up.  The post-recovery
 * checkpoint stamps its marker with the WAL sequence and runs BEFORE
 * pc_wal_init(), so without this it stamps 0 - which makes the replay
 * window never shrink and the segment spans incomparable. */
/* What the fsyncs ACTUALLY cost, as opposed to what the startup probe
 * predicted.  The probe writes at most 16 MB in ~1.2s, so on storage
 * with a write-back cache in front of it (any VM running
 * cache=writeback) it measures the cache; only the running daemon is
 * still measuring when that cache fills.  See DESIGN section 12am. */
struct pc_wal_fsync_obs {
	unsigned long long fsync_n;    /* how many completed */
	unsigned long long avg_us;     /* mean over all of them */
	unsigned long long recent_us;  /* EWMA - what it costs NOW.  A
					* lifetime mean is the wrong
					* instrument for "the device got
					* worse": early fast syncs dilute a
					* later degradation indefinitely. */
	unsigned long long max_us;     /* worst single one */
	unsigned long long probe_p50_us; /* what startup predicted */
	int probe_underestimated;      /* observed mean >= 4x predicted */
};

void pc_wal_fsync_observed(struct pc_wal_fsync_obs *out);

void pc_wal_preload_seq(const char *dir, int segments);

/* raise the sequence to the highest the replay saw (CONTROL is only a
 * lower bound between the syncs that rewrite it) */
void pc_wal_seq_atleast(unsigned long long seq);

/* producer side (any registered thread; its ring is pc_worker_id()) */
/* @ver is the version the table committed for this write - read it from
 * pcache_last_ver at the call site, immediately after the store. */

/* Install what happens when the WAL has been dropping ACKNOWLEDGED
 * writes for several seconds running: the node leaves service, because
 * a full ring under sustained load never drains on its own and a node
 * that keeps advertising itself will keep losing data.  Refusing each
 * write instead would keep it in the client map and invite retries -
 * an avalanche, not backpressure. */
void pc_wal_on_shed(void (*fn)(void));

void pc_wal_upsert(const char *col, const char *key, int klen,
		const char *val, int vlen, unsigned int expires_ticks,
		unsigned long long ver);
void pc_wal_del(const char *col, const char *key, int klen);
void pc_wal_touch(const char *col, const char *key, int klen,
		unsigned int expires_ticks, unsigned long long ver);

/* the WAL thread: one pump = drain + sort + append + fsync-per-policy.
 * Returns the poll timeout (ms) until the next duty. */
int pc_wal_pump(void);

/* the daemon wires the WAL thread's eventfd so producers can kick a
 * sleeping WAL thread (fsync=always latency), and brackets its poll
 * with the sleeping mark */
void pc_wal_set_wakeup(int efd);
void pc_wal_mark_sleeping(int s);

/* clean shutdown: final pump + fdatasync */
void pc_wal_shutdown(void);

void pc_wal_get_stats(struct pc_wal_stats *out);

/* the sync verb's barrier: block (bounded) until everything appended
 * so far is fdatasync'd.  0 synced, 1 WAL off, -1 timeout. */
PC_MUST_CHECK int pc_wal_sync(int timeout_ms);

/* watchdog surface: microsecond timestamp when an fsync began, 0 when
 * not syncing - the maintenance thread flags a hung one */
extern volatile long long pc_wal_fsync_start_us;

/* scan a WAL directory (tests, -W, and S15's replay): cb per valid
 * record, in segment-gen then file order.  Returns records seen, and
 * fills *why with "end"/"torn"/"stale-gen" for the stop reason. */
typedef int (*pc_wal_scan_cb)(int type, const char *col, int collen,
		const char *key, int klen, const char *val, int vlen,
		unsigned long long expires_wall, unsigned long long seq,
		unsigned long long ver, void *ctx);
long pc_wal_scan(const char *dir, pc_wal_scan_cb cb, void *ctx,
		const char **why);

/* CRC32C with per-arch dispatch (sw slice-by-8 fallback) */
uint32_t pc_crc32c(uint32_t init, const void *buf, size_t len);

#endif /* PC_WAL_H */
