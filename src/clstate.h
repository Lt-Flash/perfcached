/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clstate.h - the cluster plane's internal API (M9 prerequisite).
 *
 * doc/MODULARITY.md §182 calls for exactly this: a header for the
 * cluster plane's OWN translation units carrying the state as accessors,
 * never `C`'s definition.  Rule 2 is the reason - a header that exposes
 * `C` to the extracted files is the same coupling in more places, and it
 * is hard to undo once every file has grown `C.*` references.
 *
 * Measured before writing it: 91 distinct `C` fields are reached by 129
 * functions across 4,575 of cluster.c's lines, so this is built a WAVE
 * AT A TIME rather than all at once.  What is here is what M9's eight
 * worker entry points need: three module handles, five readers, two
 * scalar bumps and nine counter accessors.
 *
 * Two rules this header keeps:
 *   - no accessor hands out a mutable pointer into `C`.  Every counter
 *     is written through a function, because a returned `struct
 *     pc_proxy_stats *` would be rule 2's coupling in thinner disguise.
 *   - each counter accessor preserves the ordering its call site had.
 *     Three of these are __atomic_fetch_add and five are a plain ++;
 *     that asymmetry is the tree's, and rule 6 says move code without
 *     rewording it, so it is reproduced rather than tidied.
 *
 * `struct cluster_state` is NOT declared opaque here yet: `C` is an
 * anonymous static struct, and nothing in M9 needs to receive it - the
 * entry points take handles and scalars.  Naming it belongs with the
 * first module that must be handed the state itself.
 */
#ifndef PC_CLSTATE_H
#define PC_CLSTATE_H

#include <netinet/in.h>

#include "clmap.h"                     /* struct pc_clmap */

struct clpeers;
struct clpend;
struct clpush;

/* ---- module handles -------------------------------------------------- */
struct clpeers *cl_peers(void);
struct clpend  *cl_pend(void);
struct clpush  *cl_push(void);

/* ---- readers --------------------------------------------------------- */
int cl_enabled(void);
int cl_node_id(void);
int cl_map_usable(void);
const struct pc_clmap *cl_map(void);
struct sockaddr_in cl_self_addr(void);
long long cl_now_ms(void);
unsigned int cl_self_free_mb(void);

/* ---- placement counters (pc_shard_owner_slot's, plain increments) ---- */
void cl_place_map_hit(void);
void cl_place_hrw_hit(void);

/* ---- the nine counters M9's entry points write ----------------------- */
void cl_st_fwd_no_route(void);         /* atomic, as at its call site */
void cl_st_fwd_send_fail(void);        /* atomic */
void cl_st_pull_sent(unsigned long long n);  /* atomic */
void cl_st_fwd_send_errno(int e);      /* a STORED value, not a tally */
void cl_px_fwd_sent(void);             /* plain ++ */
void cl_px_fwd_fails(void);            /* plain ++ */
void cl_px_migrate_skipped_big(void);  /* plain ++ */
void cl_px_placed_local(void);         /* plain ++ */
void cl_px_placed_remote(void);        /* plain ++ */

#endif /* PC_CLSTATE_H */
