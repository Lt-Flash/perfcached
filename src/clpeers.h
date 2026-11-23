/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clpeers.h - the peer table: the peer thread writes it, workers read `.node` with
 * acquire.  Nothing here sends or logs, and every entry point takes `now`. */
#ifndef PC_CLPEERS_H
#define PC_CLPEERS_H

#include <netinet/in.h>

#include "config.h"
#include "cluster.h"
#include "psrelay.h"                  /* struct psr_path */
#include "psinterest.h"                /* struct psv_view */
#include "pcaddr.h"
#define PEER_UP_MS      10000  /* 10 beats, MASTER_DEAD_MS's margin; per-op
                                * paths never wait on this (own timeouts) */
#define DUP_GRACE_MS (3LL * PEER_UP_MS)

struct peer {
	pc_addr addr;
	int node;                      /* learned from heartbeats, 0 unknown */
	long long last_seen_ms;
	long long alive_ms;            /* its own last ALIVE - last_seen_ms moves on hearsay too */
	long long link_since_ms;       /* the link check with it began (0 = none running) */
	long long link_probe_ms, link_ok_ms;   /* a cluster-socket probe sent, answered */
	long long link_tcp_ms[3];      /* the lane, clrpt, the bulk listener took a connect */
	/* when peer_gone() vacated this slot: the purge clears ->node, so a grace
	 * anchored on liveness alone would be bypassed the instant it is purged */
	long long gone_ms;
	unsigned int free_mb;
	unsigned int total_mb;
	int client_port;               /* where CLIENTS dial this peer (from its ALIVE) */
	int resp_port;                 /* its RESP door, 0 if none or not reported */
	int http_port;                 /* its HTTP door, 0 = none or not reported */
	unsigned int entries;          /* records it last reported holding */
	unsigned char start_kind;      /* PC_START_* from its last ALIVE */
	unsigned char features;        /* CLMEMB_FEAT_*; 0 = an older build or none
	                                * heard: cannot carry the new types */
	unsigned char acks;            /* its keepalive said CLMEMB_F32_ACKS - it
	                                * announces marks */
	unsigned char cold_fresh;      /* restarted cold: its next ESTABLISHED means
	                                * it completed alone, no resync from zero owed */
	unsigned char col_synced;      /* this peer has been sent the created set
	                                * once, when it appeared */
	unsigned char clients_told;    /* this node's clients were told it joined;
	                                * told again when it goes */
	int told_state;                /* the state they were told */
	int told_node;                 /* the id they were told - a restart here under
	                                * a new identity changes it in place */
	/* its open clients as its last ALIVE reported them; cl_reported
	 * = 0 for an older build */
	unsigned int cl_open, cl_resp, cl_bin, cl_json, cl_nresp;
	unsigned char cl_reported;
	signed char link_pct;               /* from its ALIVE, -1 = none */
	unsigned char link_level;
	/* its collection block from its last ALIVE */
	unsigned char cols_reported, ncols, ncols_total;
	struct clmemb_col cols[CLMEMB_COLS_MAX];
	long long cols_ms[CLMEMB_COLS_MAX];   /* when each was last carried */
	/* its pub/sub relay port and whether it has proven it answers
	 * there; ps_since_ms / ps_warned say "advertised, never answered" once */
	struct psr_path ps;
	long long ps_since_ms;
	unsigned char ps_warned;
	struct psr_path rp;                /* replication pushes, same port */
	long long rp_since_ms;
	unsigned char rp_warned;
	/* what it wants relayed: pv_mode and the filter's bits are read by publishing
	 * workers under ps_int_seq; the rest is the cluster thread's */
	unsigned char pv_mode;         /* PV_ALL / PV_FILTER / PV_BROADCAST */
	uint8_t *pv_bloom;             /* PSB_BYTES, from its first full state */
	struct psv_view pv;
	uint64_t pv_hb;                /* the interest version it last told us */
	unsigned char pv_everything;   /* its full state said: everything */
	long long pv_uncovered_ms, pv_req_ms;
	long long boot_ms;             /* last_seen_ms minus its uptime (0 = older
	                                * build), so a stale beat does not freeze it */
	int mem_tier;                  /* PCACHE_MEM_*, from its heartbeat */
	int nreason;                   /* PC_NRE_*, why it is in that state; 0 = none given */
	int reconciling;               /* its frames say a reconcile is open */
	int ncfg;                      /* the settings it reported */
	uint32_t cfg_name[PC_CFGSIG_MAX], cfg_val[PC_CFGSIG_MAX];
	int sole_witness;              /* its only live witness (0 none,
	                                * CLMEMB_SOLEWIT_MANY 2+, -1 not said) */
	int nstate;                    /* PC_NST_*, from its ALIVE; an older peer's
	                                * shorter frame reads as READY, as it behaved */
	unsigned int live_kb;          /* live cell KB: live/total is the rebalancer's
	                                * metric (free/used are chunk-granular, sticky) */
	/* the eager sweep is RESUMABLE: a cursor per collection, the cycle's start tick and
	 * whether it lost anything - else a keyspace over one budget never replicates past it */
	unsigned int repl_cursor[PC_MAX_COLLECTIONS];
	unsigned int repl_cycle[PC_MAX_COLLECTIONS];
	unsigned char repl_dirty[PC_MAX_COLLECTIONS];
	/* the cycle's clock - when it started (wall ms), what it has
	 * scanned and sent so far; published when it completes */
	long long repl_cycle_ms[PC_MAX_COLLECTIONS];
	unsigned long long repl_scanned[PC_MAX_COLLECTIONS];
	unsigned long long repl_sent[PC_MAX_COLLECTIONS];
	/* the holder set changed: re-offer what placement now makes it hold; not backfill[],
	 * whose eager-era rules (lowest id sends, stale once ESTABLISHED) are wrong here */
	unsigned char setrepair[PC_MAX_COLLECTIONS];
	/* a write placed here was skipped while it was CONNECTING (a worker sets it, the
	 * cluster thread arms setrepair once it is placeable, and clears it) */
	unsigned char missed;
	/* replication high-water per collection (a wtick): newer records still need pushing;
	 * zeroed when the slot is (re)claimed, so a rejoin resyncs fully */
	unsigned int repl_mark[PC_MAX_COLLECTIONS];
	/* tombstones it is known to have: all planted up to tomb_mark (s), frozen while it
	 * is cut off; tomb_drop_mark tells a return whether the table covers the absence */
	unsigned int tomb_mark, tomb_win;
	unsigned int tomb_gap_from;        /* a silence began here (s), 0 = none */
	unsigned long long tomb_drop_mark, tomb_sent;
	int tomb_owed;
	/* a fast-path replay owed to it as witness: request, window start (s, this node's
	 * clock), sent so far, window size, next number, open.  Peer thread only. */
	uint32_t fp_req, fp_incarn;    /* the asker's incarnation, 0 from an older build */
	unsigned int fp_from, fp_mark, fp_win, fp_seq;
	int fp_on;                     /* 2 = held for its marks */
	/* owed a rebuild (1: asked until taken), and since when
	 * this node had not heard it (ms); the asks */
	unsigned char rb_state, rb_tries;
	long long rb_from_ms, rb_sent_ms;
	long long fp_hold_ms;          /* until when */
	/* Identity survives its restarts, incarnation does not: a change in EITHER means its
	 * memory is gone and all owed is resent.  has_ident = 0 for an older build. */
	int has_ident;
	unsigned char ident[16];
	unsigned int incarn;
	/* set when this peer's incarnation changes: it came back empty, so
	 * it needs the PASSIVE copies too - once, not for ever */
	unsigned char backfill[PC_MAX_COLLECTIONS];
	/* it asked for this collection whole (M_ASKFILL): a backfill walk from every peer */
	unsigned char askfill[PC_MAX_COLLECTIONS];
	/* when this peer last arrived empty - the sender holdoff is
	 * measured from here, not from the flag, which can go stale */
	long long fresh_ms;
	/* this sweep cycle has been a backfill from its FIRST bucket: only such a cycle,
	 * walked clean, may clear backfill[] */
	unsigned char repl_bfcycle[PC_MAX_COLLECTIONS];
};

/* The membership plane's state.  `C` embeds one of these; every function
 * below takes the pointer. */
struct clpeers {
	struct peer peers[PC_CL_MAXPEER];
	int n_peers;
	struct { struct in_addr addr; long long first_ms, last_ms; }
		dup_seen[PC_CL_MAXPEER];   /* joiners with a duplicate id */
};

/* ---- liveness, one predicate ---- the acquire on ->node is what cluster.h promises
 * workers; no lock, so any thread may ask and sees what the last tick published */
static inline int clpeers_live(const struct peer *p, long long now);
/* a CONNECTING peer is live - it is probed and reports - but holds, takes and serves
 * nothing until the master moves it ONLINE */
static inline int clpeers_usable(const struct peer *p, long long now)
{
	return clpeers_live(p, now) && p->nstate != PC_NST_CONNECTING;
}

static inline int clpeers_live(const struct peer *p, long long now)
{
	return __atomic_load_n(&p->node, __ATOMIC_ACQUIRE) &&
	        now - p->last_seen_ms < PEER_UP_MS;
}

/* a peer still holding its PLACEMENT SLOTS: wider than clpeers_live(), so a blip does
 * not move the set (no rebuild storm); ranking uses this, sending clpeers_live() */
#define PEER_SLOT_GRACE_MS  30000

static inline int clpeers_holds_slots(const struct peer *p, long long now)
{
	if (__atomic_load_n(&p->node, __ATOMIC_ACQUIRE))
		return now - p->last_seen_ms < PEER_UP_MS + PEER_SLOT_GRACE_MS;
	/* purged, but still inside the grace: its slots are its own until
	 * the window closes, and only then does the set change */
	return p->gone_ms && now - p->gone_ms < PEER_SLOT_GRACE_MS;
}

/* the live peer holding this node id, or NULL.  Any thread. */
struct peer *clpeers_by_id_live(struct clpeers *pt, int node, long long now);

/* a live peer holds node id @node at an address other than @from's */
int clpeers_id_elsewhere(struct clpeers *pt, int node, const pc_addr *from,
		long long now);

/* the peer holding id @node at @from's address, live or not, or NULL (always for
 * node 0); for frames that name their sender.  Peer thread. */
struct peer *clpeers_by_id_from(struct clpeers *pt, unsigned node,
		const pc_addr *from);

/* the slot at this address, live or not, or NULL.  Peer thread. */
struct peer *clpeers_by_addr(struct clpeers *pt, const pc_addr *a);

/* how many peers are live at `now`.  Any thread. */
int clpeers_count_live(struct clpeers *pt, long long now);

/* find or create @addr's slot, NULL when full; purged slots are reused in place so
 * workers can iterate.  *created only when one was APPENDED.  Peer thread. */
struct peer *clpeers_upsert(struct clpeers *pt, const pc_addr *a,
		int *created);

/* Duplicate-id arbitration: a colliding joiner is tolerated for DUP_GRACE_MS (it
 * may be this node's stale view), then told.  Peer thread. */
int clpeers_dup_persisted(struct clpeers *pt, const pc_addr *from,
		long long now);
void clpeers_dup_forget(struct clpeers *pt, const pc_addr *from);
int clpeers_dup_seen_recent(struct clpeers *pt, const struct in_addr *addr,
		long long now);

#endif /* PC_CLPEERS_H */
