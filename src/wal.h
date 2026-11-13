/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* wal.h - the write-ahead log: each worker encodes into its own SPSC ring (full = drop and count,
 * never block); one thread drains, group-commits and fsyncs.  Replay orders by version, not file. */
#ifndef PC_WAL_H
#define PC_WAL_H

#include "pc_attr.h"

#include <stddef.h>
#include <stdint.h>

enum pc_wal_fsync { PC_WFSYNC_EVERYSEC = 0, PC_WFSYNC_ALWAYS, PC_WFSYNC_NO };

/* PC_WAL_HEAL_ROUTINE heals within PC_WAL_HEAL_WIN_S seconds is routine healing: the log is
 * undersized for the write rate, not unlucky */
#define PC_WAL_HEAL_ROUTINE 3
#define PC_WAL_HEAL_WIN_S   600

#define PC_WAL_MAGIC 0x4C574350u       /* 'PCWL' LE; a segment is u32 magic | u32 gen, then
                                        * records; a zero len, bad CRC or stale gen ends it */
#define PC_WAL_HDR   8
#define PC_WAL_RECHDR 12               /* len + crc32c(gen+payload) + gen; segments are
                                        * recycled unzeroed, the gen echo fences old records */
#define PC_WAL_T_UPSERT 1
#define PC_WAL_T_DEL    2
#define PC_WAL_T_TOUCH  3
#define PC_WAL_T_UPSERT_V 4    /* upsert carrying the record's version */
#define PC_WAL_T_TOUCH_V  5    /* touch carrying the record's version */
/* the collection set itself, so a replay lands records in the collections of their time; the KEY
 * is a one-byte placeholder and the VALUE carries buckets_log2 for a create, nothing for a drop */
#define PC_WAL_T_COL_CREATE 6
#define PC_WAL_T_COL_DROP   7
#define PC_WAL_T_COL_RENAME 8   /* col = the old name, value = the new one */
/* a delete numbered at its origin (clack.h): a DEL header, value = version, oid u64, incarn u32,
 * seq u64; written for every one applied, a copy removed or not - a missing seq is a replay hole */
#define PC_WAL_T_DEL_N 9
#define PC_WAL_DELN_VLEN 28
/* the types this build writes and reads - anything else came from a newer or foreign build;
 * a new type is added HERE and in the replay */
#define PC_WAL_T_KNOWN(t) ((t) >= PC_WAL_T_UPSERT && (t) <= PC_WAL_T_DEL_N)
/* header: u8 type, u8 collen, u16 keylen, u32 vallen, u64 expires (wall s, 0 = never), u64 seq = 24;
 * + u64 version = 32; an UPSERT_V adds its flags byte (PCACHE_F_JSON) = 33 */
#define PC_WAL_HDRLEN(t) ((t) == PC_WAL_T_UPSERT_V ? 33u : \
	PC_WAL_T_HASVER(t) ? 32u : 24u)
/* the types whose header carries a version (32 bytes, not 24) */
#define PC_WAL_T_HASVER(t) ((t) == PC_WAL_T_UPSERT_V || \
	(t) == PC_WAL_T_TOUCH_V)
/* a DELETE carries its version as an 8-byte LE VALUE - not a new type, so an older build reads a
 * plain delete and a rollback still replays; vlen 0 = an unversioned delete, file order */
#define PC_WAL_DEL_VERLEN 8

struct pc_wal_stats {
	int enabled;
	const char *fsync_mode;
	unsigned long long appended, bytes, dropped, late, recycles;
	unsigned long long ring_waits, ring_wait_ms;   /* a full ring waited */
	/* the pump waited for a snapshot rather than overwrite a
	 * segment it had not covered - episodes, their time, and now */
	unsigned long long full_waits, full_wait_ms;
	int full_waiting, on_full_heal;
	/* the device refused an append, a rotation or a sync (sticky;
	 * failed_errno says which way), and CONTROL writes/syncs that failed */
	int failed, failed_errno;
	unsigned long long ctrl_errors;
	unsigned long long last_seq, synced_seq;
	/* last_seq = ACKed, pending_hi = written, synced_seq = fdatasync'd: last_seq - pending_hi is
	 * lost to kill -9 (or a full ring), pending_hi - synced_seq only to power loss */
	unsigned long long pending_hi;
	/* free_segments: records all inside the last completed snapshot; overruns: recycles that
	 * overwrote NEWER records, each shrinking the replayable window */
	unsigned long long overruns;
	int free_segments;
	/* the ring's segment files and their size (0 while the WAL is off) */
	int segments;
	unsigned long long segment_bytes;
	/* the floor and the cap the ring follows the load
	 * between, and the segments it has added and retired */
	int segments_min, segments_max;
	unsigned long long grown, retired;
	/* fill rate (MB/s, an EWMA of the rotation interval) and how long the free segments last at it
	 * (s, -1 = not measured) - against rdb.last_dur_ms, whether a snapshot can keep up */
	double fill_mb_s;
	double full_in_s;
	/* how long the WHOLE ring takes to fill at that rate (-1 = not measured); below the last
	 * snapshot's duration no trigger can keep a snapshot ahead - that is sizing */
	double ring_fill_s;
	/* a lost record puts the node in HEALING until a snapshot started after the loss completes;
	 * heals counts them, last_heal_ms is the last one's length */
	unsigned long long heals;
	int healing;
	long long last_heal_ms;
	/* heals in the last PC_WAL_HEAL_WIN_S seconds, and whether that
	 * many means healing has become routine */
	int heals_recent;
	int heal_routine;
	/* losses (drops + overruns) no heal has covered: 0 = every ACKed write is durable again.  The heal
	 * starts on the WAL thread's next pass, so READY with unhealed > 0 is one about to start. */
	unsigned long long unhealed;
};

/* at startup, before threads: provisions the segment ring with real zero writes (thin/sparse
 * backends) and allocates @nthreads rings */
PC_MUST_CHECK
int pc_wal_init(const char *dir, enum pc_wal_fsync mode, int segment_mb,
		int segments, int ring_kb, int nthreads, const int *ring_kb_of);

/* before pc_wal_init: lock the WAL's buffers as the arena is, all or none - pc_wal_locked_bytes()
 * is 0 when they could not all be locked, with the mlock errno in *err */
void pc_wal_set_pin(int pin);
unsigned long long pc_wal_locked_bytes(int *err);
unsigned long long pc_wal_buffer_bytes(void);   /* what locking asks for */

/* what the fsyncs ACTUALLY cost: the startup probe (16 MB in ~1.2 s) measures a write-back cache
 * in front of the device; only the running daemon sees that cache fill */
struct pc_wal_fsync_obs {
	unsigned long long fsync_n;    /* how many completed */
	unsigned long long avg_us;     /* mean over all of them */
	unsigned long long recent_us;  /* EWMA - what it costs NOW; a lifetime mean
					* dilutes a later degradation indefinitely */
	unsigned long long max_us;     /* worst single one */
	unsigned long long sum_us;     /* all of them, for a window's mean */
	unsigned long long win_max_us; /* worst of the last five minutes */
	unsigned long long probe_p50_us; /* what startup predicted */
	int probe_underestimated;      /* observed mean >= 4x predicted */
};

void pc_wal_fsync_observed(struct pc_wal_fsync_obs *out);

/* the recorded sequence before the WAL is up: the post-recovery checkpoint runs before
 * pc_wal_init() and would stamp 0, so the replay window would never shrink */
void pc_wal_preload_seq(const char *dir, int segments);

/* raise the sequence to the highest the replay saw (CONTROL is only a
 * lower bound between the syncs that rewrite it) */
void pc_wal_seq_atleast(unsigned long long seq);

/* producer side (any registered thread; its ring is pc_worker_id()); @ver is what the table
 * committed for the write - read pcache_last_ver right after the store */

/* what happens when the WAL drops ACKNOWLEDGED writes for seconds running: the node leaves service
 * (refusing writes would invite retries); @why = a loss (ring drop, overrun) or the device's I/O */
#define PC_WAL_SHED_LOSS 1
#define PC_WAL_SHED_IO   2
void pc_wal_on_shed(void (*fn)(int why));
/* told when a heal starts (1) and completes (0); without one, or with no snapshot directory to
 * heal from, a loss sheds */
void pc_wal_on_heal(void (*fn)(int phase));

void pc_wal_upsert(const char *col, const char *key, int klen,
		const char *val, int vlen, unsigned int expires_ticks,
		unsigned long long ver);
/* as pc_wal_upsert, with the record's type (PCACHE_F_JSON) */
void pc_wal_upsert_fl(const char *col, const char *key, int klen,
		const char *val, int vlen, unsigned int expires_ticks,
		unsigned long long ver, unsigned char flags);
void pc_wal_del(const char *col, const char *key, int klen);
/* the numbered delete's record: its WAL seq for pc_wal_durable_seq(), 0 with no WAL (nothing gets
 * more durable), PC_WAL_SEQ_NEVER when the WAL failed or could not take it */
#define PC_WAL_SEQ_NEVER (~0ULL)
unsigned long long pc_wal_del_n(const char *col, const char *key, int klen,
		unsigned long long ver, uint64_t oid, uint32_t incarn,
		uint64_t seq);
/* every record at or below the answer is on the device or inside the snapshot marked @covered;
 * held below a record a full ring dropped until a completed snapshot covers it */
unsigned long long pc_wal_durable_seq(unsigned long long covered);
/* a collection created, dropped or renamed while the daemon serves */
void pc_wal_col_create(const char *col, int buckets_log2);
void pc_wal_col_drop(const char *col);
void pc_wal_col_rename(const char *from, const char *to);
int pc_wal_enabled(void);   /* is a WAL running at all? */
void pc_wal_touch(const char *col, const char *key, int klen,
		unsigned int expires_ticks, unsigned long long ver);

/* the WAL thread: one pump = drain + sort + append + fsync-per-policy.
 * Returns the poll timeout (ms) until the next duty. */
int pc_wal_pump(void);

/* the WAL thread's eventfd, so producers can kick it awake (fsync=always latency); its poll is
 * bracketed with the sleeping mark */
void pc_wal_set_wakeup(int efd);
void pc_wal_mark_sleeping(int s);

/* what a full ring or log does: 0 wait (a writer for the pump, the pump for a snapshot), 1 drop
 * then heal (fsync = always still waits on a full ring) */
void pc_wal_set_on_full(int heal);
/* the cap the ring may grow to (before pc_wal_init; below its
 * segments, or 0, = no growth) */
void pc_wal_set_segments_max(int max);
/* the workers' slots wait for room as long as it takes; any other
 * thread at most PC_WAL_OTHER_WAIT_MS, then the counted drop */
#define PC_WAL_OTHER_WAIT_MS 200
void pc_wal_set_workers(int lo, int hi);

/* is the calling worker's ring down to its last quarter (resume 0) or still under half free
 * (resume 1)?  0 off a worker, with no WAL, under on_full = heal, or once the pump is gone */
int pc_wal_tight(int resume);
/* set while a connection is served - the WAL marks it a writer when
 * one of its records is logged (proto.c pauses writers only) */
extern __thread int *pc_wal_wrote;

/* clean shutdown: final pump + fdatasync */
void pc_wal_shutdown(void);

void pc_wal_get_stats(struct pc_wal_stats *out);

/* the sync verb's barrier: block (bounded) until everything appended is fdatasync'd; 0 synced,
 * 1 WAL off, -1 timeout, -2 the device refused and this process syncs no more */
PC_MUST_CHECK int pc_wal_sync(int timeout_ms);

/* watchdog surface: microsecond timestamp when an fsync began, 0 when
 * not syncing - the maintenance thread flags a hung one */
extern volatile long long pc_wal_fsync_start_us;

/* scan a WAL directory: cb per valid record in segment-gen then file order; returns records seen,
 * *why = "end", "torn" or "stale-gen" */
typedef int (*pc_wal_scan_cb)(int type, const char *col, int collen,
		const char *key, int klen, const char *val, int vlen,
		unsigned long long expires_wall, unsigned long long seq,
		unsigned long long ver, unsigned char flags, void *ctx);
long pc_wal_scan(const char *dir, pc_wal_scan_cb cb, void *ctx,
		const char **why);

/* CRC32C lives in crc32c.h; rdb.c and recover.c find it here */
#include "crc32c.h"

#endif /* PC_WAL_H */
