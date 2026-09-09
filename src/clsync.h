/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clsync.h — staging a map change past the backup (control plane, step 3).
 *
 * The ordering is: master STAGES a change, backup ACKS it, master
 * PUBLISHES it.  Ack before publish is the whole mechanism, and it
 * gives one property that nothing else has to enforce:
 *
 *   the backup is never BEHIND the fleet - only ever equal, or one
 *   committed change ahead
 *
 * which holds in all three windows a master can die in:
 *
 *   before the ack       change lost; backup and fleet both hold the old
 *                        map.  Consistent.
 *   after ack, before    backup holds a change the fleet has not seen.
 *   publish              It promotes and publishes it - correct, because
 *                        the change was committed the moment it acked.
 *   after publish        everyone has it.  Consistent.
 *
 * Map changes are TOPOLOGY events - a node joining, failing, or being
 * rebalanced - seconds apart at worst.  Synchronous replication of
 * something that rare costs nothing meaningful; this is not a data-write
 * path, and treating it like one would be the mistake.
 *
 * THE SEQUENCE IS CONSUMED WHEN A CHANGE IS STAGED, NOT WHEN IT IS
 * PUBLISHED.  This is the rule that is easy to get wrong.  If an
 * aborted stage returned its sequence to the pool, then a change staged
 * at seq N, abandoned because the backup went quiet, and re-staged with
 * DIFFERENT CONTENT at seq N would collide with the first - and the
 * backup may have acked the first and had its ack lost.  Two different
 * maps would then exist at the same epoch, which is precisely what the
 * epoch is for.  Burning a sequence number costs nothing; reusing one
 * costs correctness.
 *
 * NO BACKUP IS NOT A BLOCK.  With no standby to ack, changes publish
 * immediately and are marked unsynced.  Refusing to publish would mean a
 * node cannot join while the cluster is already short a controller -
 * turning a degraded state into a stuck one.  The condition is made
 * loud, not fatal.
 */
#ifndef PC_CLSYNC_H
#define PC_CLSYNC_H

#include <stdint.h>

#define PC_CLSYNC_IDLE    0            /* nothing in flight */
#define PC_CLSYNC_STAGED  1            /* sent to the backup, awaiting ack */
#define PC_CLSYNC_ACKED   2            /* committed; publishable */

struct pc_clsync {
	int state;                     /* PC_CLSYNC_* */
	int have_backup;               /* a standby exists to ack */
	uint32_t term;                 /* the term we are issuing under */
	uint32_t published_seq;        /* last sequence the fleet has seen */
	uint32_t next_seq;             /* next sequence to hand out */
	uint32_t staged_seq;           /* the one in flight (valid when != IDLE) */
	int staged_unsynced;           /* staged with no backup to ack */

	/* backup side: a change we acked and have not seen published.  On
	 * promotion this MUST be published - it was committed when we
	 * acked it, and the fleet has never seen it. */
	int hold_pending;
	uint32_t hold_term, hold_seq;

	unsigned long long staged_n, acked_n, published_n, aborted_n,
		unsynced_n;
};

/* Begin at @term with the fleet already holding @published_seq. */
void pc_clsync_init(struct pc_clsync *s, uint32_t term, uint32_t published_seq);

/* A standby exists (or stopped existing).  Losing one mid-flight does
 * NOT abort the staged change: it may already have been acked with the
 * ack in flight, and abandoning it would burn the sequence for nothing. */
void pc_clsync_set_backup(struct pc_clsync *s, int have_backup);

/* Stage the next change.  *out_seq gets the sequence it will publish at,
 * consumed here and never handed out again.  Returns 0, or -1 if a
 * change is already in flight - one at a time, so an ack can never be
 * ambiguous about which change it refers to.
 *
 * With no backup the change is staged AND immediately publishable. */
int pc_clsync_stage(struct pc_clsync *s, uint32_t *out_seq);

/* The backup acked @term/@seq.  Returns 0 if it matched the change in
 * flight, -1 otherwise - a late ack for an abandoned change must not
 * commit the one that replaced it. */
int pc_clsync_ack(struct pc_clsync *s, uint32_t term, uint32_t seq);

/* 1 = the staged change may go to the fleet. */
int pc_clsync_publishable(const struct pc_clsync *s);

/* Record that the staged change reached the fleet. */
int pc_clsync_published(struct pc_clsync *s);

/* Give up on the change in flight.  Its sequence is NOT returned. */
int pc_clsync_abort(struct pc_clsync *s);

/* ---- backup side ------------------------------------------------- */

/* We acked @term/@seq for the master.  Held until we see it published. */
void pc_clsync_hold(struct pc_clsync *s, uint32_t term, uint32_t seq);

/* We saw the fleet reach @term/@seq: anything held at or below it is
 * accounted for. */
void pc_clsync_saw(struct pc_clsync *s, uint32_t term, uint32_t seq);

/* Promoting: 1 if a committed change is still owed to the fleet, with
 * its epoch written out.  It must be republished under the NEW term -
 * the old one belongs to a master that no longer exists. */
int pc_clsync_owed(const struct pc_clsync *s, uint32_t *term, uint32_t *seq);

#endif /* PC_CLSYNC_H */
