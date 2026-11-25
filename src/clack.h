/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clack.h - watermarks: per numbering space, the highest CONTIGUOUS delete seq this
 * member applied AND made durable.  No lock: the caller serialises calls on one. */
#ifndef PC_CLACK_H
#define PC_CLACK_H

#include <stdint.h>

#define CLACK_SPACES   1026  /* two a member (513 max): an old space
                              * stays until its entries are gone */
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
	unsigned long long dropped;    /* spaces forgotten by clack_drop */
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

/* the space's mark as a snapshot recorded it - LOWER after restoring an older one:
 * the member then announces the lower mark, and what lies above it comes again */
int clack_restore(struct clack *a, uint64_t oid, uint32_t incarn,
		uint64_t mark);
/* the fleet confirmed the space through @floor before this node held any of it, and
 * its state came whole from them: the mark moves up to @floor.  NEW, DUP or FULL. */
int clack_floor(struct clack *a, uint64_t oid, uint32_t incarn,
		uint64_t floor);
/* 1 when the space is known here - something of it applied or restored */
int clack_known(const struct clack *a, uint64_t oid, uint32_t incarn);
/* the space is closed and nothing of it is held any more: forget it */
void clack_drop(struct clack *a, uint64_t oid, uint32_t incarn);

/* every space: mark = confirmed, applied = contiguous applied, hi = highest seen;
 * a non-zero return from cb stops the walk and is returned */
typedef int (*clack_each_cb)(uint64_t oid, uint32_t incarn, uint64_t mark,
		uint64_t applied, uint64_t hi, void *ctx);
int clack_each(const struct clack *a, clack_each_cb cb, void *ctx);

void clack_get_stats(const struct clack *a, struct clack_stats *st);

#endif /* PC_CLACK_H */
