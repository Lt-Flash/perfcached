/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * compat.h — daemon-facing controls of the OpenSIPS compatibility shim.
 *
 * The vendored core (src/core/) was written against a small slice of the
 * OpenSIPS core API.  That slice is reproduced by the sibling headers in
 * this directory under their original names (dprint.h, locking.h, str.h,
 * pt.h, timer.h, ipc.h, mem/, mi/), so the vendored files compile with
 * nothing but their include-path prefix rewritten.  This header is the
 * daemon's side of the bargain: registration and hooks the shim needs
 * filled in as the real threading frame arrives (task S6).
 */
#ifndef PC_COMPAT_H
#define PC_COMPAT_H

/* Assigns this thread's identity (becomes `process_no` in vendored code:
 * per-thread stats slot and the 12-bit bucket owner tag).  idx must be
 * unique per live thread, 0-based, and below 4095 — the owner tag packs
 * process_no+1 into 12 bits. */
void compat_thread_register(int thread_idx);

/* The one cross-thread call the core makes: pcache_arena's hoard flush
 * broadcast (ipc_send_rpc_all).  Until a broadcaster is registered the
 * shim runs the callback inline on the calling thread only — correct for
 * the single-threaded selftests; S6 registers the real fan-out. */
void compat_set_broadcast(void (*bcast)(void (*fn)(int sender, void *param),
		void *param));

/* Seconds since daemon start (CLOCK_MONOTONIC_COARSE), the core's clock.
 * compat_ticks_offset lets selftests warp time forward. */
unsigned int compat_ticks(void);
extern unsigned int compat_ticks_offset;

/* ---- Lamport clock: which write is newer, WITHOUT a synced clock -----
 * Nothing in the cluster could say which of two copies of a key is newer.
 * A wall-clock stamp would answer it only if every node's clock agreed,
 * and this design deliberately refuses that dependency - the pull path
 * ships TTL as REMAINING seconds and the receiver rebases it precisely so
 * two nodes never have to agree on the time.
 *
 * A Lamport counter needs no clock at all: every write takes the next
 * value, and every message received carries the sender's, which the
 * receiver folds in with observe().  The heartbeat plane already runs at
 * 1 Hz, so the fleet converges continuously; a node returning after a
 * week learns the current value from the first heartbeat it sees and so
 * cannot claim to be newer than reality.
 *
 * Ties (the same value on two nodes) are broken by node id at the point
 * of comparison, not here. */
/* 64-bit because it counts WRITES: at the measured 1.9M SET/s a 32-bit
 * counter wraps in 38 minutes, and after a wrap every new write compares
 * OLDER than everything stored - which would make the whole mechanism
 * worse than none.  (DESIGN's "no cross-thread 64-bit atomics" rule is
 * noted; the WAL sequence already takes the same liberty at wal.c:495,
 * and the two should be settled together.) */
unsigned long long pc_lamport_tick(void);  /* next value, for a local write */
unsigned long long pc_lamport_now(void);   /* current, without advancing */
void pc_lamport_observe(unsigned long long seen);

/* Restore the clock from OUR OWN persisted data (A1's recovery): the
 * highest version any replayed record carried, so writes made after a
 * restart outrank everything that came back.  Deliberately NOT
 * observe(): that guards against a peer advertising nonsense and refuses
 * a jump beyond PC_LAMPORT_MAX_JUMP, which at boot - clock 0, a fleet
 * long past a billion writes - would refuse the whole dataset and count
 * it as a defect.  A record read out of our own WAL is not a peer claim.
 * Moves the clock forward only; never rewinds it. */
void pc_lamport_restore(unsigned long long high);
/* peer values refused as implausibly far ahead - non-zero is a bug */
extern unsigned long long pc_lamport_rejected;

/* Log gate, OpenSIPS numbering (see dprint.h; lower = more severe). */
extern int compat_log_level;

#endif /* PC_COMPAT_H */
