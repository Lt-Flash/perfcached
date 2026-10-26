/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clackf.h - 12ny AT-4: what the fleet's members SAY they hold - each
 * member's confirmed mark of each numbering space (clack.h), as its
 * keepalive announced it (a rotating slice a beat, CLMEMB_MARKS_MAX).
 *
 * A member's marks belong to its INCARNATION.  A restarted member - or one
 * restored from a backup - reports what its rebuilt state holds, which may
 * be less than its last incarnation said (12ny "Open"), so a new
 * incarnation forgets every mark the old one reported and nothing counts
 * for it until it reports again.  A keepalive without the group (an older
 * build, or one with the marks turned off) clears the member's marks too:
 * it confirms nothing.  So the lowest mark among the members - what an
 * origin may forget at or below (AT-5) - is 0 while any member has not
 * reported the space, and a mixed fleet keeps the time rule by itself.
 *
 * Pure over its own struct: no socket, no clock, no log, no lock.
 */
#ifndef PC_CLACKF_H
#define PC_CLACKF_H

#include <stdint.h>

#define CLACKF_MEMBERS 513        /* PC_CL_MAXMEMBERS (asserted in cluster.c) */
#define CLACKF_SPACES  1026       /* a member's, as CLACK_SPACES */

struct clackf_stats {
	unsigned int members, marks;
	unsigned long long reports, lowered, resets, refused;
};

struct clackf;

struct clackf *clackf_new(void);
void clackf_free(struct clackf *f);

/* a member's keepalive, @incarn its incarnation: a new one forgets what
 * the old one reported.  1 when it did; -1 when the table is full */
int clackf_member(struct clackf *f, uint64_t member, uint32_t incarn);
/* its confirmed mark of space (oid, sincarn), after clackf_member: 1 when
 * the mark went DOWN (a state older than what it said - lowered counts
 * it), 0 otherwise, -1 for an unknown member or a full table.  @now (the
 * caller's clock, ms) stamps the mark when it changes - AT-11's "moved" */
int clackf_report(struct clackf *f, uint64_t member, uint64_t oid,
		uint32_t sincarn, uint64_t mark, long long now);
/* every mark it reported forgotten (a keepalive without the group) */
void clackf_clear(struct clackf *f, uint64_t member);
/* the member left the fleet: it is not counted any more */
void clackf_forget(struct clackf *f, uint64_t member);
/* 12ny AT-8: its keepalive was heard at @now (the caller's clock, ms);
 * returns when the one before was - 0 for the first, and for a member not
 * in the table.  The gap between the two is how long it was away from
 * this node: a member whose incarnation did not change across a long one
 * was cut off, not restarted.  A new incarnation keeps the stamp */
long long clackf_heard(struct clackf *f, uint64_t member, long long now);

/* 1 and its mark of the space; 0 when it has reported none */
int clackf_mark(const struct clackf *f, uint64_t member, uint64_t oid,
		uint32_t sincarn, uint64_t *mark);
/* the lowest mark of the space among @members: 0 when any of them has
 * not reported it (or is unknown), and 0 for no members */
uint64_t clackf_min(const struct clackf *f, uint64_t oid, uint32_t sincarn,
		const uint64_t *members, int n);

/* ---- 12ny AT-11: what the marks say about the members ---- */
/* 1 and when the member's mark of the space last changed (0: reported
 * without a time); 0 when it has reported none */
int clackf_moved(const struct clackf *f, uint64_t member, uint64_t oid,
		uint32_t sincarn, long long *moved);
/* the highest mark of the space among @members (every member when NULL),
 * 0 for none reported */
uint64_t clackf_max(const struct clackf *f, uint64_t oid, uint32_t sincarn,
		const uint64_t *members, int n);
/* the members of @members STUCK on the space: below @head, the mark
 * unmoved since @now - @still_ms or before, while another member's mark
 * of it moved later (@also: the latest such move the caller knows of
 * outside the table - its own) - deletes are being confirmed around it,
 * not by it.  Up to @max into @out, the longest-still first; returns how
 * many */
struct clackf_stuck { uint64_t member; uint64_t mark; long long moved; };
int clackf_stuck(const struct clackf *f, uint64_t oid, uint32_t sincarn,
		const uint64_t *members, int n, uint64_t head, long long also,
		long long now, long long still_ms, struct clackf_stuck *out,
		int max);

typedef int (*clackf_each_cb)(uint64_t member, uint32_t incarn,
		uint64_t oid, uint32_t sincarn, uint64_t mark, void *ctx);
/* every reported mark; a non-zero return stops the walk */
int clackf_each(const struct clackf *f, clackf_each_cb cb, void *ctx);

void clackf_get_stats(const struct clackf *f, struct clackf_stats *st);

#endif /* PC_CLACKF_H */
