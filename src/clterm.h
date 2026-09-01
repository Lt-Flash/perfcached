/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clterm.h — the mastership term (control plane, step 2).
 *
 * The term is the half of the epoch that makes two maps comparable when
 * the thing that issued them has changed.  Sequence alone cannot: a
 * partitioned master at seq 100 and a backup promoting from the same
 * synced copy both issue 101, and nothing orders them.  Term increments
 * on every mastership change and is never reused, so a lower term loses
 * whatever its seq.
 *
 * Three rules, and the third is what makes fast promotion safe:
 *
 *   1. A map whose term is below ours is REFUSED, not merged.
 *   2. A promoting node claims a term ABOVE anything it has ever seen -
 *      not above its own last one.  A node that has been away has seen
 *      less than the fleet, and claiming from its own history would
 *      reissue a term someone else already used.
 *   3. A master that observes a HIGHER term steps down immediately and
 *      unconditionally - no merge, no member count, no negotiation.
 *      That bounds split brain to a single detection interval instead of
 *      to a reconciliation timer, which is what lets the backup promote
 *      aggressively.
 *
 * PERSISTENCE.  A term must be written to disk BEFORE it is acted on,
 * or a node that promotes, publishes and dies could reissue the same
 * term with different content after a restart - the exact ambiguity the
 * term exists to remove.
 *
 * How durable it needs to be is not arbitrary: the term must outlive any
 * map that could still be believed by somebody.  So it is persisted
 * exactly where maps are persisted, in the same state directory and the
 * same durability class.  A node that persists nothing has no old map to
 * contradict, so starting from zero is correct for it rather than a
 * compromise - the same reasoning that makes an ephemeral identity the
 * right answer rather than a fallback.
 *
 * A term is u32.  Unlike the Lamport clock it counts MASTERSHIP CHANGES,
 * not writes, so wrap is unreachable - a change every second is 136
 * years.  What is reachable is a peer advertising nonsense, which would
 * poison the fleet permanently; see PC_TERM_MAX_JUMP.
 */
#ifndef PC_CLTERM_H
#define PC_CLTERM_H

#include <stdint.h>

/* How far ahead of us a peer's term may legitimately be.  Terms move on
 * mastership changes, so even a node absent for a week through a badly
 * flapping cluster is thousands behind, not millions.  A million is
 * absurdly generous and still keeps 0xFFFFFFFF unreachable in one step.
 *
 * Same reasoning as the Lamport clock's jump guard, and for the same
 * reason: ONE bad value accepted here is permanent.  Nothing can ever
 * out-term it, so the fleet could never elect a master again. */
#define PC_TERM_MAX_JUMP 1000000u

/* load the persisted term, or start at 0.  @state_dir may be NULL or
 * empty - see the durability note above.  Returns 0, or -1 if the
 * directory exists but the term there is unreadable, which is worth
 * refusing startup for: a node that silently forgets its term can
 * reissue one. */
int pc_term_init(const char *state_dir);

uint32_t pc_term_current(void);

/* 1 = this node's term survives a restart */
int pc_term_durable(void);

/* Fold in a term seen from a peer.  Returns 1 if ours moved, 0 if not,
 * -1 if the value was refused as implausible (counted, never silent). */
int pc_term_observe(uint32_t seen);

/* Allocate the next term for a promotion: one above everything seen,
 * PERSISTED BEFORE IT IS RETURNED.  Returns 0 if it could not be
 * persisted - the caller must then NOT promote, because a term it
 * cannot remember is one it can reissue. */
uint32_t pc_term_claim(void);

/* peer terms refused as implausible - non-zero means a defect or a
 * hostile peer inside the cluster PSK */
extern unsigned long long pc_term_rejected;

/* --- pure decisions, no state: the rules above, testable alone --- */

#define PC_TERM_STALE    (-1)          /* theirs is older: refuse it */
#define PC_TERM_SAME       0
#define PC_TERM_AHEAD      1           /* theirs is newer: adopt it */

int pc_term_cmp(uint32_t mine, uint32_t theirs);

/* Rule 3.  1 = a master holding @mine must demote on seeing @theirs.
 * Deliberately takes is_master rather than reading global state, so the
 * rule can be exercised without a cluster. */
int pc_term_must_stepdown(int is_master, uint32_t mine, uint32_t theirs);

#endif /* PC_CLTERM_H */
