/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clsel.h - which node a client should talk to: a hint, not authority (the daemon
 * forwards a wrong guess); eligibility is by state, never by weight */
#ifndef PC_CLSEL_H
#define PC_CLSEL_H

#include <stdint.h>

#include "clmap.h"

#define PC_CLSEL_FAILOVER    0         /* stable: same node until it dies */
#define PC_CLSEL_ROUND_ROBIN 1         /* independent start per client */
#define PC_CLSEL_LEAST_CONN  2         /* the most free node */
#define PC_CLSEL_WEIGHTED    3         /* biased by weight */

/* READY nodes only, deliberately: a RECOVERING node may still hold keys deleted
 * while it was down, and answer wrongly rather than miss */
int pc_clsel_eligible(const struct pc_clmap_node *n);

int pc_clsel_count(const struct pc_clmap *m);

/* @seed is per client so clients spread instead of piling onto one node; @avoid is
 * a node id to skip (the one that just died) or 0; returns 0 if none is eligible */
uint16_t pc_clsel_pick(const struct pc_clmap *m, int policy, uint64_t seed,
		uint16_t avoid);

/* epoch comparison, term first: a client BEHIND refreshes; one AHEAD is never
 * pushed back, as it may be talking to a node that has not caught up yet */
int pc_clsel_stale(uint32_t my_term, uint32_t my_seq,
		uint32_t their_term, uint32_t their_seq);

#endif /* PC_CLSEL_H */
