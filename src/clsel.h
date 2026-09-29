/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clsel.h — which node should this client talk to (control plane, step 5).
 *
 * A client learns the map on connect, opens connections to the nodes in
 * it, and distributes itself.  The master publishes topology changes;
 * it does not steer individual clients, because the master is also the
 * thing that fails and putting it in every client's path makes its
 * failure larger.
 *
 * SELECTION IS A HINT, NOT AUTHORITY.  The daemon re-checks ownership
 * and forwards a wrong guess, so a client on a stale map pays a hop
 * rather than getting a wrong answer.  That property is what makes
 * best-effort map distribution acceptable at all, and everything here
 * depends on it staying true.
 *
 * ELIGIBILITY IS BY STATE, NOT BY WEIGHT.  Weight decides which node
 * OWNS a key; state decides whether a node can be TALKED to.  A draining
 * node at weight 0 still holds what it has not handed over yet, and in
 * store mode any node can answer anything - so weight is a placement
 * input and, separately, a balancing bias, but it is not a gate on
 * connecting.
 *
 * When nothing is READY this returns nothing, deliberately.  Falling
 * back to a RECOVERING node looks generous and is not obviously safe: a
 * node still replaying a WAL can hold keys that were deleted while it
 * was down, so it answers WRONGLY rather than answering with a miss.
 * Whether a stale answer beats an outage is a judgement for the caller
 * with the request in hand, not for a selection function.
 */
#ifndef PC_CLSEL_H
#define PC_CLSEL_H

#include <stdint.h>

#include "clmap.h"

#define PC_CLSEL_FAILOVER    0         /* stable: same node until it dies */
#define PC_CLSEL_ROUND_ROBIN 1         /* independent start per client */
#define PC_CLSEL_LEAST_CONN  2         /* the most free node */
#define PC_CLSEL_WEIGHTED    3         /* biased by weight */

/* 1 = a client may talk to this node. */
int pc_clsel_eligible(const struct pc_clmap_node *n);

/* How many nodes a client could talk to. */
int pc_clsel_count(const struct pc_clmap *m);

/*
 * Choose a node.  @seed is the client's own - two clients with the same
 * seed choose alike, which is the point of it being per client: a fixed
 * seed would put every client in the fleet on one node.
 *
 * @avoid is a node id to skip (the one that just died), or 0.  Returns
 * a node id, or 0 if nothing is eligible.
 */
uint16_t pc_clsel_pick(const struct pc_clmap *m, int policy, uint64_t seed,
		uint16_t avoid);

/*
 * Does this client need a newer map?  Epoch comparison, term first.
 * A client that is BEHIND should refresh; one that is somehow AHEAD
 * should not be pushed backwards - it may be talking to a node that has
 * not caught up yet, and adopting the older map would undo a correct
 * view.
 */
int pc_clsel_stale(uint32_t my_term, uint32_t my_seq,
		uint32_t their_term, uint32_t their_seq);

#endif /* PC_CLSEL_H */
