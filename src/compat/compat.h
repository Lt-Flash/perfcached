/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* compat.h - the daemon's side of the shim the vendored core compiles against; the sibling headers
 * reproduce the slice of the core API it uses, under their original names */
#ifndef PC_COMPAT_H
#define PC_COMPAT_H

/* this thread's identity (`process_no` in vendored code: the stats slot and the 12-bit bucket owner
 * tag): unique per live thread, 0-based, below 4095 */
void compat_thread_register(int thread_idx);

/* the one cross-thread call the core makes (pcache_arena's hoard flush broadcast); until one is
 * registered the callback runs inline on the calling thread only */
void compat_set_broadcast(void (*bcast)(void (*fn)(int sender, void *param),
		void *param));

/* Seconds since daemon start (CLOCK_MONOTONIC_COARSE), the core's clock.
 * compat_ticks_offset lets selftests warp time forward. */
unsigned int compat_ticks(void);
/* the expiry tick of a deadline @ms from now: the last tick starting at or before it (an entry
 * reads expired from its expiry tick on), so never later than the deadline */
unsigned int compat_ticks_deadline(long long ms);
extern unsigned int compat_ticks_offset;

/* ---- Lamport clock: which write is newer WITHOUT synced clocks - every write takes the next value,
 * every message folds the sender's in (observe); ties break on node id where compared ---- */
/* 64-bit because it counts WRITES: at 1.9M SET/s 32 bits wrap in 38 minutes, and after a wrap every
 * new write compares OLDER than everything stored */
unsigned long long pc_lamport_tick(void);  /* next value, for a local write */
/* a delete's version, made the floor for every later tick - under the bucket lock, before its
 * release (a no-op when the clock is one shared counter) */
void pc_lamport_publish(unsigned long long v);
unsigned long long pc_lamport_now(void);   /* current, without advancing */
void pc_lamport_observe(unsigned long long seen);

/* restore from OUR OWN replayed data (the highest version seen), forward only: not observe(), whose
 * PC_LAMPORT_MAX_JUMP guard against a peer would refuse the whole dataset at boot */
void pc_lamport_restore(unsigned long long high);
/* peer values refused as implausibly far ahead - non-zero is a bug */
extern unsigned long long pc_lamport_rejected;

/* log gate, the vendored core's numbering (see dprint.h; lower = more severe) */
extern int compat_log_level;

#endif /* PC_COMPAT_H */
