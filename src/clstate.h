/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* The cluster plane's internal API: state as accessors, never `C`'s definition; no
 * accessor hands out a mutable pointer into `C`, counters go through functions. */
#ifndef PC_CLSTATE_H
#define PC_CLSTATE_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

#include "clmap.h"                     /* struct pc_clmap */
#include "pcaddr.h"

struct clpeers;
struct clpend;
struct clpush;
struct clapply;

/* ---- module handles -------------------------------------------------- */
struct clpeers *cl_peers(void);
struct clpend  *cl_pend(void);
struct clpush  *cl_push(void);
struct clapply *cl_ap(void);           /* the apply rings */

/* ---- readers --------------------------------------------------------- */
int cl_enabled(void);
int cl_node_id(void);
int cl_role(void);                    /* PC_ROLE_* */
int cl_master_id(void);
const char *cl_state_dir(void);       /* "" = none */
int cl_map_usable(void);
const struct pc_clmap *cl_map(void);
/* cl_map()'s write generation, odd while the peer thread rewrites the map: a reader takes it before
 * it reads the map and retries when it differs after */
unsigned int cl_map_gen(void);
pc_addr cl_self_addr(void);
long long cl_now_ms(void);
unsigned int cl_self_free_mb(void);

/* ---- placement counters (pc_shard_owner_slot's, plain increments) ---- */
void cl_place_map_hit(void);
void cl_place_hrw_hit(void);

/* ---- counters the worker entry points write ------------------------- */
void cl_st_fwd_no_route(void);         /* atomic, as at its call site */
void cl_st_fwd_send_fail(void);        /* atomic */
void cl_st_pull_sent(unsigned long long n);  /* atomic */
void cl_st_fwd_send_errno(int e);      /* a STORED value, not a tally */
void cl_st_epoch_commits(void);        /* plain ++ */
void cl_px_fwd_sent(void);             /* plain ++ */
void cl_px_fwd_fails(void);            /* plain ++ */
void cl_px_migrate_skipped_big(void);  /* plain ++ */
void cl_px_placed_local(void);         /* plain ++ */
void cl_px_placed_remote(void);        /* plain ++ */

/* ---- the "complete request" primitive: cl_set_fail sets the CALLING thread's
 * failure reason, the one pc_cluster_last_fail() reports */
struct pending;
uint32_t cl_pend_alloc(int kind, int expect, int extra_ms,
		struct pending **out);
void cl_pend_release(struct pending *slot);
void cl_set_fail(int reason);
/* the pull behind pc_pull_begin, with the answer's kind: PC_DONE_PULL for
 * a client's miss, PC_DONE_RECONCILE for the reconcile probe (clwork.c) */
uint32_t pc_pull_begin_kind(const char *col, size_t collen,
		const char *key, size_t klen, int kind);

/* Unicast receive buffer, sized for a pipelined burst of MAX_FWD_VAL values: a dropped
 * forward is a refused write, not a retried one.  Also each relay-port socket's. */
#define PC_CLUSTER_RCVBUF (8 << 20)

/* ---- the write-path push (clwork.c) and its counters -------------- */
struct wgroup;
void cl_wgroup_send_cb(struct wgroup *g, void *ctx);   /* clpush's callback */
void cl_px_repl_pushed(unsigned long long n);   /* atomic, as at its call site */
void cl_px_repl_groups(void);                  /* atomic */
void cl_px_spread_not_held(void);              /* plain ++ */
void cl_px_spread_ack(unsigned int parked, unsigned int kept); /* atomic */

/* ---- replication receive/apply (clrecv.c) ----------------------- */
void cl_shard_note_change(void);               /* shard_note_change() */
void cl_px_migrated_in(void);                  /* atomic, as in apply_rec */
void cl_px_migrated_in_plain(void);            /* plain ++, as in handle_migrate */
void cl_px_recv_nomem(void);                   /* atomic */
void cl_px_recv_older(void);                   /* atomic */
void cl_px_recv_tombstoned(void);              /* atomic */
void cl_px_recv_below_dropped(void);           /* atomic */
void cl_px_recv_stale_dropped(void);           /* atomic */

/* ---- tombstones+acks (cltomb.c) --------------------------------- */
struct clres;
struct pc_ack_hole;
const unsigned char *cl_self_ident(void);      /* the 16 raw bytes */
const struct clres *cl_reserved(void);         /* the id reservations */
long long cl_reserve_ms(void);                 /* RESERVE_MS */
long long cl_witness_grace_ms(void);           /* WITNESS_GRACE_MS */
int cl_self_no_acks(void);                     /* the test hooks */
int cl_self_gap_no_copies(void);
long long cl_acks_stuck_ms(void);
int cl_resync_floor(void);
void cl_resync_floor_clear(void);
void cl_st_acks_floors(void);                  /* plain ++ */
unsigned long long cl_st_acks_floors_read(void);
void cl_st_tomb_forgotten(unsigned long long n);   /* plain += */
void cl_st_gap_asked(void);                    /* plain ++, and the five below */
void cl_st_gap_tail(void);
void cl_st_gap_resent(void);
void cl_st_gap_floored(void);
void cl_st_gap_served(void);
void cl_st_rebuild_sent(void);
/* stays in cluster.c: it starts a reconcile pass */
void gap_floor_over(const struct pc_ack_hole *h, long long death);

/* ---- the collection DDL (clddl.c) -------------------------------- */
int cl_self_no_autoscale(void);                /* the test flags */
int cl_self_as_deaf(void);

/* ---- admission (cladmit.c) --------------------------------------- */
struct peer;
pc_addr cl_master_addr(void);
int cl_unicast_on(void);                       /* [cluster] discovery = unicast */
void cl_st_members_asked(void);                /* plain ++ */
void cl_st_members_sent(void);                 /* plain ++ */
void cl_st_admission_stuck(unsigned long long n);   /* a gauge, stored */
unsigned int cl_self_total_mb(void);
unsigned int cl_self_live_kb(void);
int cl_max_datagram(void);                     /* [cluster] max_datagram, 0 = none */
int cl_will_bootstrap(void);                   /* a blank eager member pulls first */
/* cluster.c's: the queue replays a JOIN_REQ, the member list's digest, its members
 * published, and ONLINE ending as an ASSIGN without the connect flag would */
void handle_join_req(const unsigned char *pt, size_t n, const pc_addr *from);
uint64_t member_digest(void);
struct peer *peer_upsert(const pc_addr *addr);
void peer_publish(struct peer *p, int node, unsigned int free_mb, unsigned int total_mb,
		unsigned int live_kb, const unsigned char *ident, unsigned int incarn,
		unsigned int entries, int start);
void joined_state(int members);
void send_alive(void);

#endif /* PC_CLSTATE_H */
