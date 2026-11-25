/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* ackstate.h - this node's watermarks (clack.h), one mutex-guarded instance all paths share, and
 * the `acks` file beside each snapshot (not in it: an older build calls the snapshot corrupt) */
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
/* recovery: restore the file's marks when it is @marker's (else the replay alone counts, lower
 * never higher); how many spaces restored, 0 none, -1 for a file not ours or torn */
int pc_ack_load(const char *dir, uint64_t marker);

int pc_ack_each(clack_each_cb cb, void *ctx);
void pc_ack_get_stats(struct clack_stats *st);

/* the spaces with a hole - [from, to] below the highest seq
 * seen (clack_gap) - up to @max; returns how many */
struct pc_ack_hole { uint64_t oid; uint32_t incarn; uint64_t from, to; };
int pc_ack_holes(struct pc_ack_hole *out, int max);
/* clack_floor and clack_known, locked */
int pc_ack_floor(uint64_t oid, uint32_t incarn, uint64_t floor);
int pc_ack_known(uint64_t oid, uint32_t incarn, uint64_t *applied,
		uint64_t *hi);

/* this beat's slice of the confirmed marks for the keepalive, from a cursor that moves each
 * call, so every space goes out every ceil(spaces / max) beats.  How many; *nspaces all */
int pc_ack_slice(uint64_t *oid, uint32_t *incarn, uint64_t *mark, int max,
		int *nspaces);

/* what the members announce (clackf.h) - the cluster thread
 * writes, any thread reads.  @member is the member's identity hash */
int pc_ack_fleet_member(uint64_t member, uint32_t incarn);
int pc_ack_fleet_report(uint64_t member, uint64_t oid, uint32_t sincarn,
		uint64_t mark, long long now);
void pc_ack_fleet_clear(uint64_t member);
void pc_ack_fleet_forget(uint64_t member);
/* its keepalive heard now (ms); when the one before was, 0 if none */
long long pc_ack_fleet_heard(uint64_t member, long long now);
/* when a member's mark last moved; a space's highest mark among members; the members stuck on
 * it; a space's head = the highest seq this node or any member knows of (clackf.h) */
int pc_ack_fleet_moved(uint64_t member, uint64_t oid, uint32_t sincarn,
		long long *moved);
uint64_t pc_ack_fleet_max(uint64_t oid, uint32_t sincarn,
		const uint64_t *members, int n);
int pc_ack_fleet_stuck(uint64_t oid, uint32_t sincarn, const uint64_t *members,
		int n, uint64_t head, long long also, long long now,
		long long still_ms, struct clackf_stuck *out, int max);
/* a member's marks for /members (@self: this node's confirmed ones), each with the space's head
 * and when the mark last moved (0 for this node's own: not kept).  Up to @max; how many */
struct pc_ack_row { uint64_t oid, mark, head; uint32_t incarn; long long moved; };
int pc_ack_member_rows(uint64_t member, int self, struct pc_ack_row *out,
		int max);
int pc_ack_fleet_mark(uint64_t member, uint64_t oid, uint32_t sincarn,
		uint64_t *mark);
int pc_ack_fleet_incarn(uint64_t member, uint32_t *incarn);
uint64_t pc_ack_fleet_min(uint64_t oid, uint32_t sincarn,
		const uint64_t *members, int n);
int pc_ack_fleet_each(clackf_each_cb cb, void *ctx);
void pc_ack_fleet_stats(struct clackf_stats *st);

#endif /* PC_ACKSTATE_H */
