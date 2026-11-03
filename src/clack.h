/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clack.h - 12ny AT-3: WATERMARKS.  What this member has applied, and
 * made durable, of each origin's numbered deletes.
 *
 * An origin numbers the deletes it originates (AT-2): seq 1, 2, 3 ...
 * within a NUMBERING SPACE, (oid, incarn) - the origin's identity hash
 * and its incarnation, fresh at every process start, so a restart (or a
 * restore from a backup) opens a new space and can never reuse a seq.
 * A member confirms, per space, the highest CONTIGUOUS seq it has
 * applied AND made durable - the WAL synced through every record that
 * applied one at or below it.  Origins forget what every member has
 * confirmed (AT-5); a hole holds the mark below it until a gap request
 * fills it (AT-6), so nothing past a lost delete is ever confirmed.
 *
 * Durability is tracked without a WAL position per seq: as the
 * contiguous mark advances, the WAL seqs that got it there are folded
 * into a running maximum, and (mark, that maximum) pairs wait in a short
 * queue for the WAL's synced seq to pass them.  What arrived out of
 * order is folded in as ONE maximum over everything ahead of the mark -
 * conservative: a seq may be confirmed later than it could have been,
 * never earlier.  A full queue merges its last two pairs the same way.
 *
 * Pure over its own struct: no socket, no clock, no log, no lock - the
 * caller serialises every call on one instance.  The WAL is parameters:
 * clack_applied() takes the WAL seq of the record that applied the
 * delete (0: nothing to wait for - a collection without a WAL, or a
 * record replayed from it), clack_synced() the WAL's synced seq.  With
 * durable_on_apply (fsync = no: never synced) the mark is the applied
 * one, and the caller says so where it reports it.
 */
#ifndef PC_CLACK_H
#define PC_CLACK_H

#include <stdint.h>

#define CLACK_SPACES   1026  /* two a member at PC_CL_MAXMEMBERS (513):
                              * a restarted origin's old space stays
                              * until its entries are gone */
#define CLACK_WIN      4096  /* seqs recorded ahead of the mark: ~30 s
                              * of a hole at 140 deletes a second */
#define CLACK_PEND       16  /* (mark, WAL max) pairs awaiting a sync */

enum {
	CLACK_NEW  = 0,          /* recorded */
	CLACK_DUP  = 1,          /* at or below the mark, or held already */
	CLACK_FAR  = 2,          /* past the window: not recorded - the gap
	                          * request (from the mark) brings it again */
	CLACK_FULL = -1,         /* a new space, and no room for one */
	CLACK_BAD  = -2          /* seq 0: numbering starts at 1 */
};

struct clack_stats {
	unsigned long long applied, dups, far, full;
	unsigned int spaces;
	int durable_on_apply;
};

struct clack;

/* NULL on no memory.  durable_on_apply: confirm on apply (fsync = no) */
struct clack *clack_new(int durable_on_apply);
void clack_free(struct clack *a);

/* a numbered delete applied, its WAL record walseq (0 = durable now) */
int clack_applied(struct clack *a, uint64_t oid, uint32_t incarn,
		uint64_t seq, uint64_t walseq);
/* the WAL is synced through walseq: confirmed marks advance */
void clack_synced(struct clack *a, uint64_t walseq);

/* 1 and the CONFIRMED mark (applied, contiguous, durable) of the space;
 * 0 for a space this member has never heard of */
int clack_mark(const struct clack *a, uint64_t oid, uint32_t incarn,
		uint64_t *mark);
/* 1 and the first hole [*from, *to] when seqs below the highest seen
 * are missing; 0 when there is none (or no such space) */
int clack_gap(const struct clack *a, uint64_t oid, uint32_t incarn,
		uint64_t *from, uint64_t *to);

/* the space's mark as a snapshot recorded it, applied and confirmed -
 * LOWER than before when the snapshot is older (a restore): the member
 * then announces the lower mark, and what lies above it comes again */
int clack_restore(struct clack *a, uint64_t oid, uint32_t incarn,
		uint64_t mark);
/* 12ny AT-7: everything of the space at or below @floor is held here
 * already - every member of the fleet confirmed it before this node held
 * anything of the space, and its state came whole from them.  The mark
 * moves up to @floor (applied and confirmed: the state that reflects it is
 * the fleet's, not this node's WAL), what was held ahead of it connects.
 * Lower than the mark: nothing.  Returns CLACK_NEW, CLACK_DUP, CLACK_FULL */
int clack_floor(struct clack *a, uint64_t oid, uint32_t incarn,
		uint64_t floor);
/* 1 when the space is known here - something of it applied or restored */
int clack_known(const struct clack *a, uint64_t oid, uint32_t incarn);
/* the space is closed and nothing of it is held any more: forget it */
void clack_drop(struct clack *a, uint64_t oid, uint32_t incarn);

/* every space: the keepalive's group (AT-4), the snapshot, /members.
 * mark = confirmed, applied = contiguous applied, hi = highest seen.
 * A non-zero return from cb stops the walk and is returned. */
typedef int (*clack_each_cb)(uint64_t oid, uint32_t incarn, uint64_t mark,
		uint64_t applied, uint64_t hi, void *ctx);
int clack_each(const struct clack *a, clack_each_cb cb, void *ctx);

void clack_get_stats(const struct clack *a, struct clack_stats *st);

#endif /* PC_CLACK_H */
