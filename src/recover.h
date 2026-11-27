/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* recover.h - startup recovery: RDB load, WAL replay from its marker, a fresh snapshot,
 * all BEFORE pc_wal_init (which overwrites a segment).  A corrupt snapshot only logs CRIT. */
#ifndef PC_RECOVER_H
#define PC_RECOVER_H

#include "pc_attr.h"

struct pc_recover_stats {
	/* the highest WAL seq seen, recovered from the log itself (the control file is only
	 * a bound); pc_wal_resume_seq() continues from it across a restart */
	unsigned long long last_seq;
	long rdb_records, rdb_skipped_expired, rdb_skipped_nocol;
	long wal_applied, wal_below_marker, wal_skipped;
	long rdb_skipped_cache, wal_skipped_cache;   /* durable = no */
	long wal_older;    /* records a newer one (or tombstone) outranked */
	unsigned long long marker;
	/* the highest version any recovered record carried: the clock resumes ABOVE it, or
	 * writes after a restart would compare older than the data it restored */
	unsigned long long ver_high;
	int rdb_ok;                    /* 0 = missing or corrupt (cold start) */
	/* the loaded snapshot's header time - wall seconds when its save BEGAN, so every
	 * delete this node had not applied when the walk passed a key is newer than it */
	unsigned long long rdb_wall;
	/* a snapshot format newer than this build reads (0 = none), and WAL records
	 * of a type it does not know - how many, the first type */
	unsigned int rdb_fmt_newer;
	long wal_unknown;           /* above the marker: refused unless skip */
	long wal_unknown_covered;   /* under it: the snapshot holds them */
	int wal_unknown_type;
	/* numbering spaces restored from the snapshot's acks file (-1: there was one,
	 * unreadable), and numbered deletes the replay counted into them */
	int ack_restored;
	long ack_replayed;
};

/* [wal] unknown_records = skip: start without the WAL records this build does not
 * know instead of refusing (the default).  Before pc_recover. */
void pc_recover_set_skip_unknown(int on);

/* returns 0 (stats filled; empty dirs are a normal first boot) or -1 on
 * an I/O-level failure worth refusing startup for */
PC_MUST_CHECK int pc_recover(const char *dir, struct pc_recover_stats *st);

/* Records the last pc_recover() restored: a node back holding data has something to
 * reconcile against the fleet. */
long pc_recovered_records(void);
/* the wall second the snapshot this boot loaded began (0 = none); the reconcile asks a
 * witness for every delete since - an eager replica persists none it receives */
unsigned long long pc_recovered_snapshot_wall(void);
/* WAL records the last recovery applied ABOVE the snapshot marker: a clean stop
 * leaves none, so non-zero says the previous stop was not clean */
long pc_recovered_wal_applied(void);

/* wall ms of the newest write in the storage dir at startup (0 = none); it errs early,
 * which makes the reconcile keep rather than delete */
long long pc_recovered_last_write_ms(void);
/* take that sample NOW - before anything at startup writes to @dir
 * (the dir lock does); pc_recover() then keeps it */
void pc_recover_note_last_write(const char *dir);

/* the load verb: import the CURRENT snapshot additively (the live value wins, expired
 * ones dropped).  0 (counts filled) or -1 (no/corrupt file). */
PC_MUST_CHECK int pc_rdb_import(const char *dir, long *loaded, long *skipped_existing,
		long *skipped_expired);

#endif /* PC_RECOVER_H */
