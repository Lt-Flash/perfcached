/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clterm.h - the mastership term: up on every mastership change, never reused, so a map of a lower
 * term loses whatever its seq; persisted where maps are, BEFORE it is acted on */
#ifndef PC_CLTERM_H
#define PC_CLTERM_H

#include <stdint.h>

/* how far ahead of us a peer's term may be: terms move only on mastership changes, and ONE bad
 * value accepted is permanent (nothing could out-term it, no master elected again) */
#define PC_TERM_MAX_JUMP 1000000u

/* load the persisted term, or start at 0 (@state_dir may be NULL or empty); -1 if the term file
 * there does not validate - worth refusing startup, a node forgetting its term can reissue one */
/* the term file's name, so the cluster can carry it from a WAL directory into a new state_dir;
 * the file itself stays this module's to read and write */
#define PC_TERM_FILE "node-term"
int pc_term_init(const char *state_dir);



uint32_t pc_term_current(void);

/* 1 = this node's term survives a restart */
int pc_term_durable(void);

/* could a peer honestly hold @seen: at or below ours, or at most PC_TERM_MAX_JUMP above?  0 is
 * counted in pc_term_rejected; ours does not move (a frame with such a term is refused whole) */
int pc_term_plausible(uint32_t seen);

/* Fold in a term seen from a peer.  Returns 1 if ours moved, 0 if not,
 * -1 if the value was refused as implausible (counted, never silent). */
int pc_term_observe(uint32_t seen);

/* the next term for a promotion: above everything EVER seen, not just our own last, PERSISTED
 * BEFORE IT IS RETURNED; 0 if it could not be, and then the caller must NOT promote */
uint32_t pc_term_claim(void);

/* peer terms refused as implausible - non-zero means a defect or a
 * hostile peer inside the cluster PSK */
extern unsigned long long pc_term_rejected;

/* --- pure decisions, no state: the rules above, testable alone --- */

#define PC_TERM_STALE    (-1)          /* theirs is older: refuse, never merge */
#define PC_TERM_SAME       0
#define PC_TERM_AHEAD      1           /* theirs is newer: adopt it */

int pc_term_cmp(uint32_t mine, uint32_t theirs);

/* 1 = a master holding @mine steps down at once on seeing @theirs, which bounds split brain to one
 * detection interval; takes is_master so the rule is testable without a cluster */
int pc_term_must_stepdown(int is_master, uint32_t mine, uint32_t theirs);

#endif /* PC_CLTERM_H */
