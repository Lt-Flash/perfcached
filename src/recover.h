/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * recover.h — startup recovery (task S15): RDB load, then WAL tail
 * replay from the snapshot's marker, then an immediate fresh snapshot.
 *
 * The ordering matters twice over: recovery must run BEFORE pc_wal_init
 * (activation claims the next generation slot, overwriting the oldest
 * segment's header), and a synchronous post-recovery snapshot must land
 * BEFORE that activation - otherwise a crash after init could lose the
 * replayed-but-only-in-memory records the clobbered segment held.
 *
 * A corrupt snapshot does not refuse startup: this is a cache - it
 * logs CRIT, starts from whatever the WAL alone can rebuild (replay
 * from sequence 0), and moves on.  Expired entries are skipped at
 * load/replay time; wall-clock TTLs convert back to ticks.
 */
#ifndef PC_RECOVER_H
#define PC_RECOVER_H

#include "pc_attr.h"

struct pc_recover_stats {
	/* the WAL's sequence is recovered from the log itself, the way
	 * RocksDB recovers its own: the control file is only a bound */
	unsigned long long last_seq;
	long rdb_records, rdb_skipped_expired, rdb_skipped_nocol;
	long wal_applied, wal_below_marker, wal_skipped;
	long rdb_skipped_cache, wal_skipped_cache;   /* G5: durable = no */
	long wal_older;    /* S240: records a newer one (or tombstone) outranked */
	unsigned long long marker;
	/* the highest WAL sequence this replay saw.  Handed to
	 * pc_wal_resume_seq() so the sequence CONTINUES across a restart:
	 * it used to begin again at 0 every time, which made the snapshot
	 * marker meaningless (every snapshot logged "wal marker 0") and
	 * left the segment free-accounting comparing sequences from
	 * different generations. */
	/* the highest version any recovered record carried: the clock has to
	 * resume ABOVE it, or every write made after a restart would compare
	 * older than the data the restart just restored */
	unsigned long long ver_high;
	int rdb_ok;                    /* 0 = missing or corrupt (cold start) */
	/* S392: the loaded snapshot's header time - wall-clock seconds when
	 * its save BEGAN, so every delete this node had not applied when the
	 * walk passed a key is newer than it */
	unsigned long long rdb_wall;
	/* UP3: a snapshot format newer than this build reads (0 = none), and
	 * WAL records of a type it does not know - how many, the first type */
	unsigned int rdb_fmt_newer;
	long wal_unknown;           /* above the marker: refused unless skip */
	long wal_unknown_covered;   /* under it: the snapshot holds them */
	int wal_unknown_type;
	/* 12ny AT-3b: numbering spaces restored from the snapshot's acks
	 * file (-1: there was one, unreadable), and numbered deletes the
	 * replay counted into them */
	int ack_restored;
	long ack_replayed;
};

/* UP3: [wal] unknown_records = skip - start without the WAL records this
 * build does not know, instead of refusing (the default).  Before pc_recover. */
void pc_recover_set_skip_unknown(int on);

/* returns 0 (stats filled; empty dirs are a normal first boot) or -1 on
 * an I/O-level failure worth refusing startup for */
PC_MUST_CHECK int pc_recover(const char *dir, struct pc_recover_stats *st);

/* How many records the last pc_recover() actually restored.  The cluster
 * asks, because a node that came back holding data has something to
 * reconcile against the fleet and one that did not has nothing. */
long pc_recovered_records(void);
/* S392: the wall-clock second the snapshot this boot loaded began (0 =
 * none: a cold start, or the WAL alone).  The reconcile's fast path asks
 * a witness for every delete since then - an eager replica persists no
 * delete it receives, so those are what a restart brings back. */
unsigned long long pc_recovered_snapshot_wall(void);
/* S352: WAL records the last recovery applied ABOVE the snapshot marker -
 * a clean stop snapshots after the WAL is flushed and leaves none, so a
 * non-zero count says the previous stop was not clean */
long pc_recovered_wal_applied(void);

/* S223: wall-clock ms of the newest write in the storage directory as
 * found at startup - when this node's data stopped being current.  0 if
 * nothing was there.  Errs early (an idle node's last write predates its
 * death), which makes the reconcile keep rather than delete. */
long long pc_recovered_last_write_ms(void);
/* S455: take that sample NOW - before anything at startup writes to @dir
 * (the dir lock does); pc_recover() then keeps it */
void pc_recover_note_last_write(const char *dir);

/* the load verb: import the CURRENT snapshot additively - records
 * whose key already exists are skipped (the live value wins), expired
 * ones dropped.  Returns 0 (counts filled) or -1 (no/corrupt file). */
PC_MUST_CHECK int pc_rdb_import(const char *dir, long *loaded, long *skipped_existing,
		long *skipped_expired);

#endif /* PC_RECOVER_H */
