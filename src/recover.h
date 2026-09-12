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
};

/* returns 0 (stats filled; empty dirs are a normal first boot) or -1 on
 * an I/O-level failure worth refusing startup for */
PC_MUST_CHECK int pc_recover(const char *dir, struct pc_recover_stats *st);

/* How many records the last pc_recover() actually restored.  The cluster
 * asks, because a node that came back holding data has something to
 * reconcile against the fleet and one that did not has nothing. */
long pc_recovered_records(void);

/* the load verb: import the CURRENT snapshot additively - records
 * whose key already exists are skipped (the live value wins), expired
 * ones dropped.  Returns 0 (counts filled) or -1 (no/corrupt file). */
PC_MUST_CHECK int pc_rdb_import(const char *dir, long *loaded, long *skipped_existing,
		long *skipped_expired);

#endif /* PC_RECOVER_H */
