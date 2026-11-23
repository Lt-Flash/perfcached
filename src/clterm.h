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

/* Fold in a term seen from a peer.  1 = ours moved, to be written by pc_term_persist_tick; 0 = not
 * moved; -1 = refused as implausible (counted, never silent) */
int pc_term_observe(uint32_t seen);

/* pc-maint: write the adopted term, or a claim asked for, when above what is on disk; the fsync waits
 * on storage, so never the cluster thread's.  1 written, 0 nothing due, -1 refused (retried next call) */
int pc_term_persist_tick(void);

/* an adopted term its store refused, until a later store lands, else 0: a node holding one must not
 * claim (it could not remember the claim's floor across a restart) - pc-maint retries it */
uint32_t pc_term_unpersisted(void);

/* the refused term since landed: 1 once when it has, 0 nothing pending, -1 still refused */
int pc_term_retry(void);

/* the errno of the last refused term store */
int pc_term_store_errno(void);

/* 1 = pc_term_claim() may be called: the next term is on disk (or its store was refused, or there is
 * no state dir); 0 = pc-maint is asked to write it - promote on a later call */
int pc_term_claim_ready(void);

/* the next term for a promotion: above everything EVER seen, not just our own last, PERSISTED
 * BEFORE IT IS RETURNED; 0 if it was not (pc-maint is asked to write it for a later call), and then
 * the caller must NOT publish under it */
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
