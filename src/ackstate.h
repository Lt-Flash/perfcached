/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * ackstate.h - 12ny AT-3b: this node's watermarks (clack.h), the one
 * instance every path shares, and the file that carries them over a
 * restart.
 *
 * Counted from three places: a delete this node originates (a worker, or
 * the cluster thread for a forwarded one), a peer's numbered delete
 * applied (the cluster thread), and the WAL replay at startup.  The
 * cluster beat moves the durable point (pc_wal_durable_seq).  One mutex
 * over all of it - a few hundred calls a second at PROD's rate.
 *
 * Over a restart: each published snapshot gets `acks` beside it - the
 * APPLIED marks taken as its walk began (every seq at or below one was
 * applied before the walk, so the snapshot holds what it did), stamped
 * with the snapshot's marker.  Recovery restores them when the marker is
 * the loaded snapshot's, then the replay counts the numbered deletes
 * after it.  A file that does not match (a crash between the snapshot's
 * rename and the file's) restores nothing: the marks start from the
 * replay alone, lower - never higher - than what is held.  A file of its
 * own, not a section of the snapshot: an older build reading a snapshot
 * with a tag it does not know calls the snapshot corrupt.
 */
#ifndef PC_ACKSTATE_H
#define PC_ACKSTATE_H

#include <stdint.h>
#include "clack.h"
#include "clackf.h"

#define PC_ACK_FILE "acks"

/* before the recovery; durable_on_apply = fsync = no, or no WAL */
int pc_ack_init(int durable_on_apply);
int pc_ack_enabled(void);

/* a numbered delete applied here, its record's WAL seq (pc_wal_del_n) */
int pc_ack_applied(uint64_t oid, uint32_t incarn, uint64_t seq,
		uint64_t walseq);
/* the cluster beat: pc_wal_durable_seq(pc_rdb_safe_marker()) */
void pc_ack_synced(uint64_t durable);

/* the snapshot: the applied marks as its walk begins, and the file once
 * it is published (0, or -1 with errno - a warning, never a failed save) */
void pc_ack_capture(void);
int pc_ack_save(const char *dir, uint64_t marker);
/* recovery: restore the file's marks when it is @marker's.  Returns how
 * many spaces were restored, 0 when none (no file, another marker), -1
 * for a file that is not ours or is torn */
int pc_ack_load(const char *dir, uint64_t marker);

int pc_ack_each(clack_each_cb cb, void *ctx);
void pc_ack_get_stats(struct clack_stats *st);

/* 12ny AT-6: the spaces with a hole - [from, to] below the highest seq
 * seen (clack_gap) - up to @max; returns how many */
struct pc_ack_hole { uint64_t oid; uint32_t incarn; uint64_t from, to; };
int pc_ack_holes(struct pc_ack_hole *out, int max);
/* 12ny AT-7: clack_floor and clack_known, locked */
int pc_ack_floor(uint64_t oid, uint32_t incarn, uint64_t floor);
int pc_ack_known(uint64_t oid, uint32_t incarn, uint64_t *applied,
		uint64_t *hi);

/* 12ny AT-4: this beat's slice of the confirmed marks for the keepalive -
 * up to @max, from a cursor that moves on each call, so every space goes
 * out every ceil(spaces / max) beats.  Returns how many; *nspaces all */
int pc_ack_slice(uint64_t *oid, uint32_t *incarn, uint64_t *mark, int max,
		int *nspaces);

/* 12ny AT-4: what the members announce (clackf.h) - the cluster thread
 * writes, any thread reads.  @member is the member's identity hash */
int pc_ack_fleet_member(uint64_t member, uint32_t incarn);
int pc_ack_fleet_report(uint64_t member, uint64_t oid, uint32_t sincarn,
		uint64_t mark, long long now);
void pc_ack_fleet_clear(uint64_t member);
void pc_ack_fleet_forget(uint64_t member);
/* AT-8: its keepalive heard now (ms); when the one before was, 0 if none */
long long pc_ack_fleet_heard(uint64_t member, long long now);
/* AT-11 (clackf.h): when a member's mark last moved; the highest mark of a
 * space among members; the members stuck on it; and a space's head - the
 * highest seq anyone here knows of: this node's own, or any member's */
int pc_ack_fleet_moved(uint64_t member, uint64_t oid, uint32_t sincarn,
		long long *moved);
uint64_t pc_ack_fleet_max(uint64_t oid, uint32_t sincarn,
		const uint64_t *members, int n);
int pc_ack_fleet_stuck(uint64_t oid, uint32_t sincarn, const uint64_t *members,
		int n, uint64_t head, long long also, long long now,
		long long still_ms, struct clackf_stuck *out, int max);
/* AT-11: a member's marks for /members - this node's own confirmed ones
 * (@self) or what @member announced - each with the space's head (the
 * highest any member announced, or this node has seen) and when the mark
 * last moved (0 for this node's own: not kept).  Up to @max; how many */
struct pc_ack_row { uint64_t oid, mark, head; uint32_t incarn; long long moved; };
int pc_ack_member_rows(uint64_t member, int self, struct pc_ack_row *out,
		int max);
int pc_ack_fleet_mark(uint64_t member, uint64_t oid, uint32_t sincarn,
		uint64_t *mark);
uint64_t pc_ack_fleet_min(uint64_t oid, uint32_t sincarn,
		const uint64_t *members, int n);
int pc_ack_fleet_each(clackf_each_cb cb, void *ctx);
void pc_ack_fleet_stats(struct clackf_stats *st);

#endif /* PC_ACKSTATE_H */
