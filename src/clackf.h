/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clackf.h - what each member SAYS it holds: its confirmed mark of each numbering space (clack.h),
 * from its keepalive.  Pure over its own struct: no socket, clock, log or lock. */
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
/* its confirmed mark of space (oid, sincarn), after clackf_member: 1 when it went DOWN (counted as
 * lowered), 0 otherwise, -1 unknown member or full table; @now (ms) stamps a change */
int clackf_report(struct clackf *f, uint64_t member, uint64_t oid,
		uint32_t sincarn, uint64_t mark, long long now);
/* every mark it reported forgotten (a keepalive without the group) */
void clackf_clear(struct clackf *f, uint64_t member);
/* the member left the fleet: it is not counted any more */
void clackf_forget(struct clackf *f, uint64_t member);
/* its keepalive was heard at @now (ms): returns the one before (0: first, or unknown); a long gap with
 * no new incarnation means it was cut off, not restarted.  A new incarnation keeps the stamp. */
long long clackf_heard(struct clackf *f, uint64_t member, long long now);

/* 1 and its mark of the space; 0 when it has reported none */
int clackf_mark(const struct clackf *f, uint64_t member, uint64_t oid,
		uint32_t sincarn, uint64_t *mark);
/* the lowest mark of the space among @members: 0 when any of them has
 * not reported it (or is unknown), and 0 for no members */
uint64_t clackf_min(const struct clackf *f, uint64_t oid, uint32_t sincarn,
		const uint64_t *members, int n);

/* ---- what the marks say about the members ---- */
/* 1 and when the member's mark of the space last changed (0: reported
 * without a time); 0 when it has reported none */
int clackf_moved(const struct clackf *f, uint64_t member, uint64_t oid,
		uint32_t sincarn, long long *moved);
/* the highest mark of the space among @members (every member when NULL),
 * 0 for none reported */
uint64_t clackf_max(const struct clackf *f, uint64_t oid, uint32_t sincarn,
		const uint64_t *members, int n);
/* members STUCK on the space: below @head, unmoved since @now - @still_ms while another's mark (or
 * @also, the caller's own) moved later; up to @max into @out, longest-still first, returns how many */
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
