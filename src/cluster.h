/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* cluster.h - the peer plane: membership + heartbeats, pull-on-miss, delete tombstones and
 * proxy-mode placement, over UDP datagrams AEAD-sealed with the cluster PSK. */
#ifndef PC_CLUSTER_H
#define PC_CLUSTER_H

#include <netinet/in.h>

#include <stddef.h>
#include <stdint.h>
#include "pc_noise.h"
#include "clmemb.h"                   /* struct clmemb_col */

#define PC_CL_MAGIC   0xA1
/* open_dgram DROPS a datagram whose version byte differs: a mixed fleet splits visibly
 * instead of reading a gathered record batch as garbage */
#define PC_CL_VER     6
#define PC_CL_MAXPEER 256              /* = CL_CTR_MAX_PEERS; a full
                                        * ASSIGN list is ~3KB of 60KB */
/* node ids are 1..PC_CL_NODE_MAX: what a master hands out (alloc_node_id), what an ASSIGN
 * may carry, the pub/sub relay's lane table */
#define PC_CL_NODE_MAX 1023

/* the node's LIFECYCLE state, a separate axis from its membership role; the values are the
 * map's wire encoding (PC_CLMAP_ST_*), asserted in cluster.c */
#define PC_NST_STARTING    0
#define PC_NST_RECOVERING  1
#define PC_NST_READY       2
#define PC_NST_DRAINING    3
/* FAILED: stop selecting me for new work, held keys stay readable.  TERMINAL (only DRAINING
 * follows) because leaving and returning would re-shard the fleet twice per fault */
#define PC_NST_FAILED      4
/* STALLED: an overlay pc_node_state() reports while the cluster thread makes no progress under
 * READY; nobody sets it, it lifts by itself, and peers stop handing the node to clients */
#define PC_NST_STALLED     5
/* HEALING: the WAL lost acknowledged writes and a snapshot is making them durable; transient,
 * stays in the map, refuses writes, serves reads; only pc_node_wal_heal returns it to READY */
#define PC_NST_HEALING     6
/* CONNECTING: admitted, but still proving every path both ways; in nobody's placement or
 * selection, refuses client data, and the master's ONLINE moves it on */
#define PC_NST_CONNECTING  7
/* the states that refuse client data */
static inline int pc_nst_refuses(int st) { return st == PC_NST_RECOVERING || st == PC_NST_CONNECTING; }

/* why a node is in its state: a code travels, the wording lives in pc_node_reason_text */
#define PC_NRE_NONE        0
#define PC_NRE_WAL_DROP    1       /* acknowledged writes lost from the log */
#define PC_NRE_RECOVERING  2       /* replaying its own state */
#define PC_NRE_JOINING     3       /* waiting for the fleet's map */
#define PC_NRE_DRAINING    4       /* asked to hand its data over */
#define PC_NRE_FAILED_OTHER 5      /* failed, and nobody said why - should never be seen */
#define PC_NRE_APPLY_STALL 7       /* the cluster thread made no progress for apply_stall_ms */
#define PC_NRE_WAL_HEAL    8       /* the WAL lost writes; a snapshot is making them durable */
#define PC_NRE_CONNECTING  9       /* proving its links before it serves */
#define PC_NRE_WAL_IO      6       /* the device refused a WAL write or sync; a peer that does
                                    * not know the code shows the state without a reason */
const char *pc_node_reason_text(int code);
int pc_node_reason(void);
void pc_node_reason_set(int code);

/* the state of THIS node, and its name for logs and stats */
int pc_node_state(void);
/* the STALLED overlay's flag (node.c); the detector is cluster.c's */
int pc_node_stalled(void);
void pc_node_set_stalled(int on);
const char *pc_node_state_name(int st);
void pc_node_state_set(int st);
/* @to only while the node is in @from, checked and set under one lock: 1 moved */
int pc_node_state_move(int from, int to);
/* the WAL's shed callback (pc_wal_on_shed): FAILED with the reason.  A daemon WITHOUT a
 * cluster installs it itself, or no WAL failure refuses a write */
void pc_node_wal_shed(int why);
/* the WAL's heal callback (pc_wal_on_heal): 1 = HEALING, 0 = healed */
void pc_node_wal_heal(int phase);

#define PC_ROLE_JOINING 0
#define PC_ROLE_MEMBER  1
#define PC_ROLE_MASTER  2

struct pc_cl_peer_info {
	int node;
	int up;
	int state;                     /* PC_NST_*, from its heartbeat */
	long long last_seen_ms;
	unsigned int free_mb;
	/* the peer's IDENTITY, canonical 8-4-4-4-12 (empty = none reported): node ids are
	 * reused, this says WHICH MACHINE */
	char ident[37];
	int ident_ver;                 /* UUID version: 8 platform-derived, 7 randomly
	                                * minted, else unknown provenance */
};

struct pc_cl_stats {
	int enabled, node_id, peers_up;
	int replicas_short;            /* replicas minus live members, 0 when met */
	int role;                      /* PC_ROLE_* - runtime, elected */
	int state;                     /* PC_NST_* - runtime, lifecycle */
	int master_id;
	unsigned long long hb_sent, hb_seen, bad_auth;
	/* datagrams of another cluster header version - not bad auth: a build this one cannot
	 * talk to, said by name */
	unsigned long long wire_ver_refused;
	/* times the cluster thread was found stalled, the longest (ms), whether it is stalled now
	 * and for how long */
	unsigned long long apply_stalls, apply_stall_ms_max;
	unsigned long long epoch_refused;   /* peers refused on the epoch */
	unsigned long long acks_lowered;    /* a member's mark went down */
	unsigned long long tomb_forgotten;  /* retained deletes every member holds, forgotten */
	/* gap requests sent, gap requests answered here, and the numbered deletes re-sent in
	 * those answers */
	unsigned long long gap_asked, gap_served, gap_resent;
	unsigned long long gap_floored;    /* members told to floor a space */
	unsigned long long gap_dead;       /* holes floored here, the run over */
	/* spaces a node back empty took floors for from its bootstrap sources; holes asked for past
	 * this node's highest seq (an origin announcing more than it holds) */
	unsigned long long acks_floors, gap_tail;
	/* rebuilds asked of members back from an absence, rebuilds run here, asks a pass already
	 * covered; records kept because they were written again after their probe */
	unsigned long long rebuild_sent, rebuilds, rebuild_covered,
		reconcile_rewritten;
	unsigned long long epoch_commits;   /* commits this node published */
	unsigned long long goodbye_refused; /* not the node there now */
	unsigned long long sender_refused;  /* not from a node admitted to send it */
	unsigned long long id_collisions;   /* an id claimed from two addresses */
	unsigned long long mcast_stray;     /* a unicast datagram for another local
	                                     * address, on the multicast socket */
	/* the reconcile's witness: a live peer up while this node was down, whose "no" is evidence
	 * of a delete (0 = none yet); skipped = nobody was, the fleet came back together */
	int reconcile_witness;
	int reconcile_skipped;
	/* resyncs: a returning node the fleet could not vouch for (no witness, or away past
	 * resync_after_s) dropped its store and pulled the fleet's; resyncing = one runs now */
	unsigned long long resyncs;
	unsigned long long resync_dropped;
	int resyncing;
	/* rejoins: a restart, floor or split that copied a survivor's state whole (12ol); its
	 * drops count in resync_dropped, its run in resyncing */
	unsigned long long rejoins;
	/* would a restart now find a reconcile witness?  safe_in_ms = until the first qualifies,
	 * -1 = never */
	int restart_witnesses;
	int restart_safe;
	long long restart_safe_in_ms;
	/* live peers whose reconcile is open (a restart now could take their witness), and how long
	 * this node's own pass has had no live witness (0 = has one) */
	int restart_waits_n;
	int restart_waits_for[8];
	long long reconcile_no_witness_s;
	int reconcile_witnesses_live;      /* of this node's open pass */
	/* threads applying replicas (1 = the receiver), records handed to them, times the receiver
	 * waited on a full ring, bytes queued */
	int apply_threads;
	unsigned long long apply_dispatched, apply_blocks, apply_backlog;
	/* per ring, for a stall's post-mortem: items queued, and the two
	 * stamps the detector judges by (ms old, 0 = not set) */
	struct { unsigned long long backlog; long long busy_ms, wait_ms; }
		apply_ring[16];
	int stalled;
	long long stalled_for_ms;
	unsigned long long fwd_reply_too_big; /* forwarded replies refused
	                                    * for exceeding the ack ceiling */
	unsigned long long hb_watchdog;    /* beats the watchdog re-sent because the peer
	                                    * thread was heads-down past the overdue window */
	unsigned long long joins, assigns, elections, demotions;
	unsigned long long members_asked, members_sent;   /* the member-list repair */
	unsigned long long admission_stuck;  /* joiners held CONNECTING past CONNECT_MS (a gauge) */
	unsigned int term_unpersisted;     /* the followed term not yet on disk, 0 = none */
	int term_errno;                    /* why its store was refused */
	/* the RECEIVE plane's lag: rx_drops_ps is PER SOCKET (SO_RXQ_OVFL); rx_queue comes from
	 * SO_MEMINFO, since FIONREAD on a UDP socket answers only the first datagram */
	unsigned long long rx_applied, rx_drops;
	unsigned int rx_applied_ps, rx_older_ps, rx_drops_ps;
	unsigned int rx_queue, rx_rcvbuf;
	unsigned long long pull_sent, pull_served, pull_hits, pull_misses,
		pull_timeouts;
	/* pulls of a through key this node owns in shard mode that fetched from Redis first, and
	 * the answers sent once fetched */
	unsigned long long pull_fetching, pull_fetched;
	/* forwarded ops on a through key this node owns that the master decided, and the acks
	 * sent once it had */
	unsigned long long fwd_through, fwd_through_acked;
	/* the [cluster] max_pending parked slots are the cluster plane's backpressure: a forward or
	 * pull without one is refused, not queued; pend_peak sizes the knob */
	unsigned long long pend_exhausted;
	/* completions a worker never got: no queue for it, or its queue full */
	unsigned long long pend_cq_dropped;
	int pend_peak, pend_max;
	int pend_peak_win;                 /* the peak of the last 5 min */
	/* live occupancy: a missed release climbs and never returns; quiet fleet = 0 */
	int pend_used;
	int reserved_ids;              /* departed bindings held */
	/* the other reasons a begin() returns 0: no_route (no peer for the key) and send_fail
	 * (last errno in send_errno); relaxed atomics, bumped off the lock */
	unsigned long long fwd_no_route, fwd_send_fail;
	int fwd_send_errno;
	unsigned long long tomb_sent, tomb_applied, neg_hits;
	/* of tomb_sent, deletes sent ONCE to the group; datagrams every originated delete cost;
	 * own group copies that came back and were dropped */
	unsigned long long tomb_group, tomb_dgrams, tomb_own;
	unsigned long long neg_displaced, tomb_displaced;
	unsigned long long joins_rejected;
	/* keys recovered from the WAL that no node still had - deleted while this node was down */
	unsigned long long reconciled, reconcile_probed,
	                   reconcile_deferred;   /* drops refused while a peer was cold */
	/* walks again over keys with no verdict; keys still without one when the asking stopped */
	unsigned long long reconcile_rounds, reconcile_unruled;
	/* keys the walk asked by hold-check (of reconcile_probed), and keys this node answered
	 * in hold-checks */
	unsigned long long reconcile_holdq, holdq_served;
	/* recovered records the sweep held back, and pulls left unanswered, while reconcile had
	 * not yet ruled on them */
	unsigned long long repl_held_recovered, pull_held_recovered;
	/* the reconcile's digest round with the witness: rounds asked, slots that differed, keys
	 * not probed because their slot matched, rounds unanswered */
	unsigned long long reconcile_dig_asked, reconcile_dig_diff,
	                   reconcile_dig_skipped, reconcile_dig_fallback;
	int reconcile_active;
	/* repl_sweep_ms: wall time of the last COMPLETED repair cycle (one peer, one collection);
	 * reconcile_ms: the last completed pass; reconcile_pending is a gauge */
	unsigned long long repl_sweep_ms, repl_sweep_scanned, repl_sweep_sent,
	                   repl_sweep_cycles;
	unsigned long long reconcile_ms, reconcile_pending;
	/* the last pass's fast path: 1 when a witness's replay of deletes since the snapshot ended
	 * it, else why the walk ran; as a witness, replays served and refused */
	int reconcile_fast;
	unsigned long long reconcile_fast_deletes;
	char reconcile_fast_why[160];
	unsigned long long reconcile_fast_served, reconcile_fast_refused;
	/* probes allowed a second now */
	int reconcile_budget;
	/* a pass is open; walk keys visited of those the eager collections held when it began,
	 * keys a second over the last minute, seconds left (-1 = not known) */
	int reconcile_open;
	unsigned long long reconcile_walked, reconcile_walk_total;
	unsigned long long reconcile_rate;
	long long reconcile_eta_s;
	/* the standby this master designated (0 = none) and identities it remembers; reported,
	 * since a master without a standby is one failure from losing the control plane */
	int backup_id, hist_n;
	/* control state held on behalf of a master; held_valid false on the designated standby
	 * means a promotion would start from nothing */
	int held_valid, held_hist_n;
	unsigned int held_term, held_seq;
	unsigned long long sync_sent, sync_rx, sync_ack, sync_bad;
	unsigned long long sync_timeouts;  /* stages the standby let lapse */
	/* the cluster map this node holds (control plane); nothing reads it for placement yet */
	int map_valid, map_nodes, map_master;
	unsigned int map_term, map_seq;
	unsigned long long map_pub, map_rx, map_stale, map_bad;
	unsigned long long map_unadmitted;  /* maps/syncs from refused senders */
	/* ownership from the map, or from own liveness while the map catches up; place_hrw still
	 * climbing on a settled fleet means it never did */
	int map_usable;
	unsigned long long place_map, place_hrw;
};

/* the site salt for platform-derived identity; MUST be set before pc_cluster_init(), which
 * derives the identity before anything joins */
void pc_cluster_set_site_salt(const char *salt);
/* how long the cluster thread may go without progress before the node says STALLED; 0 = never.
 * Before pc_cluster_init(). */
void pc_cluster_set_apply_stall_ms(int ms);
/* replica apply sharded by key hash over n threads (1 = on the receiver), set before init; the
 * daemon then runs pc_cluster_apply_thread() for idx 0..pc_cluster_apply_threads()-1 */
void pc_cluster_set_apply_threads(int n);
/* [cluster] sweep_digests - the digest-gated sweep on (1) or off */
void pc_cluster_set_sweep_digests(int on);
/* [cluster] reconcile_fast - a returning node asks a witness for the
 * deletes made since its snapshot before it walks (default on) */
void pc_cluster_set_reconcile_fast(int on);
/* [cluster] reconcile_rate_max - the walk's probes a second at most (64..65536, default 1024) */
void pc_cluster_set_reconcile_rate_max(int n);
/* the sweep reaped @key of @slot at @ver: retained with the deletes so an older copy is refused;
 * @keep = its TTL was shortened (PCACHE_F_SHORTENED), so the expiry keeps the time rule */
void pc_cluster_note_expired(int slot, const char *key, int klen,
		unsigned long long ver, int keep);
/* the TTL of datagrams to the cluster group, 1..255: 1 (the default) keeps the fleet on one L2
 * segment, more lets a multicast router carry it.  Before pc_cluster_init(). */
void pc_cluster_set_multicast_ttl(int ttl);
/* the member ceiling, this node included; 0 = the build's limit */
void pc_cluster_set_max_members(int n);
/* pin the cluster id ([cluster] cluster_id; NULL = found/adopt one) */
void pc_cluster_set_cluster_id(const char *pin);
/* the cluster id as a string ("" when none), its state, and how
 * many datagrams from other clusters were refused */
void pc_cluster_cid_info(char out[37], const char **state,
		unsigned long long *foreign);
int pc_cluster_multicast_ttl(void);
/* [cluster] discovery = unicast + seeds - before pc_cluster_init(),
 * which then takes no multicast group (mcast_addr may be NULL) */
void pc_cluster_set_discovery(int unicast, const char *seeds);
/* unicast mode's frame lane - frames over @max_dgram go by
 * TCP to each peer's @lane_port (0 = no lane) */
void pc_cluster_set_lane(int max_dgram, int lane_port);
/* how often a node that forced its TCP lane to a peer asks, by
 * UDP, whether UDP works again (seconds; 0 = 30) */
void pc_cluster_set_lane_retry(int s);
unsigned long long pc_cluster_lane_restored(void);
/* unicast mode - a blank node founds only once it sees @n
 * blank nodes, itself included (0 = no count) */
void pc_cluster_set_expect(int n);
/* the configured [cluster] expect (0 = not set) */
int pc_cluster_expect(void);
/* peer-announced collection names refused under the rule */
unsigned long long pc_cluster_bad_col_names(void);
/* [cluster] identity_source (PC_IDSRC_*) - before pc_cluster_init */
void pc_cluster_set_identity_source(int src);
/* times this node's own loop ran late and it held purges */
unsigned long long pc_cluster_self_late(void);
/* peers kept alive by the TCP fallback (UDP blocked) */
unsigned long long pc_cluster_lane_saves(void);
/* elections held because another member still hears the master */
unsigned long long pc_cluster_mprobe_holds(void);
/* the other established clusters heard in the last 5 min:
 * ids ("8-4-4-4-12", 37 bytes each), last sender, ms since, frames */
int pc_cluster_foreign_list(char *ids, int cap_ids, char addrs[][24],
		long long *ago_ms, unsigned long long *frames, int cap);
struct clane_figs;
void pc_cluster_lane_info(struct clane_figs *f, int *max_dgram,
		int *lane_port);
void pc_cluster_discovery_info(const char **mode, int *seeds,
		int *seeds_resolved, int *known, unsigned long long *resolve_fail,
		unsigned long long *hints_sent, unsigned long long *hints_recv);
/* may an EAGER miss be answered without asking the fleet?  Only while READY, past
 * WITNESS_GRACE_MS with no peer newly live, and never short of memory for a replica.  1 = final */
int pc_cluster_eager_miss_final(void);
int pc_cluster_eager_doubt_one(void);   /* the one doubted peer, or 0 */
/* an eager collection's records went (an upstream tracking gap): ask the peers for it whole */
void pc_cluster_ask_fill(const char *col, size_t collen);
int pc_cluster_apply_threads(void);
void pc_cluster_apply_thread(volatile int *stop, int idx);

/* [secrets] cluster_accept's key - opened with, never sealed with */
void pc_cluster_set_accept_psk(const uint8_t psk[PC_NOISE_KEYLEN]);
/* node ids come from the elected master at runtime: never freeze pc_my_node_id() at init.
 * @advertise NULL = auto-detected; @state_dir NULL = a fresh identity each start.  0 or -1 */
int pc_cluster_init(const char *mcast_addr, int mcast_port,
		const char *advertise, const uint8_t psk[PC_NOISE_KEYLEN],
		int pull_timeout_ms, int negative_ms, int tombstone_ms,
		const char *state_dir, const char *legacy_dir, int max_pending);
/* the state directory in use (NULL = ephemeral), and whether it
 * sits on tmpfs - survives a daemon restart, not a host restart */
const char *pc_cluster_state_dir(void);
int pc_cluster_state_tmpfs(void);

/* this node's own identity (hex, NUL-terminated) and incarnation */
const char *pc_cluster_identity(void);
/* the same, canonical 8-4-4-4-12, with the UUID version out */
void pc_cluster_identity_uuid(char out[37], int *ver);
unsigned int pc_cluster_incarnation(void);
/* 1 = the identity is persisted and will survive a restart */
int pc_cluster_identity_durable(void);

int pc_cluster_enabled(void);
int pc_cluster_neg_ms(void);
int pc_my_node_id(void);           /* 0 until assigned/founded */

/* the forward plane's value ceiling (sealed UDP MAX_DGRAM minus the forward envelope), shared
 * so the verb layer refuses what the plane cannot carry */
#define PC_MAX_FWD_VAL 58000

/* the peer thread body (owns the UDP socket; serves pulls, applies
 * tombstones, tracks membership, emits 1/s heartbeats) */
void pc_cluster_thread(volatile int *stop);

/* the bulk migration thread: TCP transfers (cluster-principal Noise sessions) for records above
 * the datagram ceiling; spawned whenever the cluster is enabled */
void pc_bulk_thread(volatile int *stop);

/* the heartbeat watchdog: re-sends the last ALIVE when the peer thread has emitted nothing past
 * the overdue window, so a long duty never reads as dead; emission only */
void pc_cluster_beat_thread(volatile int *stop);

/* ---- the pull path (worker side) --------------------------------------- */

/* why the last pc_*_begin() returned 0: per-thread, read IMMEDIATELY after the 0 */
#define PC_CLFAIL_NONE   0
#define PC_CLFAIL_ROUTE  1     /* cluster off, bad args, no such peer */
#define PC_CLFAIL_BUSY   2     /* parked-request table full: BACKPRESSURE,
                                * and the only one worth retrying */
#define PC_CLFAIL_SEND   3     /* the datagram did not leave */
int pc_cluster_last_fail(void);
/* parked-request capacity (0 = cluster off): a worker's park table must not out-park it, or
 * the cluster's own backpressure never speaks */
int pc_cluster_pend_cap(void);
/* the slots free in it now (a snapshot; 0 when the cluster is off) */
int pc_cluster_pend_free(void);

/* park a missed get: the request id, or 0 = answer the miss locally; the completion reaches
 * the OWNING worker through pc_cluster_drain() */
uint32_t pc_pull_begin(const char *col, size_t collen, const char *key,
		size_t klen);

/* an mget's one deadline: pc_pull_cap_deadline caps pulls begun on this thread at @at_ms
 * (0 clears); pc_pull_cancel gives a slot back and drops its late answer */
long long pc_pull_deadline_ms(const char *col, size_t collen);
long long pc_cluster_now_ms(void);
void pc_pull_cap_deadline(long long at_ms);
void pc_pull_cancel(uint32_t req);

/* negative cache: "the cluster answered no recently" */
int  pc_neg_hit(const char *col, size_t collen, const char *key, size_t klen);
void pc_neg_set(const char *col, size_t collen, const char *key, size_t klen,
		int ms);
/* plant with the delete's version, and read it back */
void pc_neg_set_ver(const char *col, size_t cn, const char *key, size_t kn,
		int ms, unsigned long long ver);
/* the same mark for a drop this node made for itself: retained to refuse an older copy,
 * never replayed to a peer as a delete */
void pc_neg_set_ver_local(const char *col, size_t cn, const char *key,
		size_t kn, int ms, unsigned long long ver);
int pc_neg_ver(const char *col, size_t cn, const char *key, size_t kn,
		unsigned long long *ver);
void pc_neg_clear(const char *col, size_t collen, const char *key,
		size_t klen);

/* completion delivered to the worker that parked the pull */
struct pc_pull_done {
	uint32_t req;
	int kind;                      /* PC_DONE_* */
	int found;                     /* pulls: 1 = value present */
	int ok;                        /* forwards: the holder's outcome */
	long long newval;              /* fwd-add: the absolute result; a pull hit:
	                                * the record's type (PCACHE_F_JSON) */
	int from_node;                 /* pulls: who answered */
	unsigned int ttl_left;         /* relative seconds, 0 = never */
	/* the HOLDER's version, stored with a pulled copy: a local tick could land above the
	 * holder's and refuse its next update as older.  0 = the peer sent none */
	unsigned long long ver;
	/* no answer came back, as distinct from a holder that answered NO (both ok = 0); a JSON
	 * op's ok = 2 for a timeout cannot generalise, since 2 is truthy */
	int timedout;
	char *val;                     /* malloc'd, consumer frees */
	int vlen;
	int jop;                       /* JSON kind: the PC_JOP_* code */
	int count;                     /* jarrappend: elements after */
};

/* worker side: drain completions posted to THIS thread (up to @max, returns the count); also
 * expires overdue pulls into misses */
int pc_cluster_drain(int worker, struct pc_pull_done *out, int max);

/* register the calling worker thread for completion delivery */
void pc_cluster_worker_register(int worker, int efd);

/* a delete this node originates: logged, numbered, counted in its marks and sent.  @ver is the
 * version stamped under the bucket lock; 0 ticks one, for a delete that removed nothing here */
void pc_delete_originate(const char *col, size_t collen, const char *key,
		size_t klen, unsigned long long ver, int removed);
/* retained tombstones (clretain.h) - set before pc_cluster_init */
void pc_cluster_set_retain(int retain_s, int max);
/* [cluster] tombstone_acks - 0 announces no marks (the fleet then
 * keeps the time rule); before pc_cluster_init */
void pc_cluster_set_acks(int on);
/* [cluster] resync_after_s - a node away longer resyncs (0: never by time) */
void pc_cluster_set_resync_after(int s);
/* [cluster] data_multicast - 0 keeps the per-peer delete sends
 * on a multicast fleet; before pc_cluster_init */
void pc_cluster_set_data_multicast(int on);
/* the spaces whose deletes this node keeps by the marks: lowest mark, head, the member at the
 * lowest, the member STUCK on it; refreshed every beat, empty while the fleet does not confirm */
#define PC_ACK_HOLD_MAX 16
struct pc_ack_hold {
	uint64_t oid, through, head, stuck_mark;
	uint32_t incarn;
	int node, self, stuck_node;
	unsigned int stuck_s;
};
int pc_cluster_acks_holds(struct pc_ack_hold *out, int max);
unsigned long long pc_cluster_tomb_catchup_sent(void);
/* settings the fleet's members must agree on, built from the config at start and carried in
 * the keepalive as two 32-bit hashes the alerts compare */
#define PC_CFGSIG_MAX 24
struct pc_cfgsig {
	char name[48];                     /* the alert key's tail: "wal.fsync",
	                                    * "<col>.writes" */
	char what[64];                     /* said in the alert: "[wal] fsync" */
	char val[64];                      /* this node's value, as said */
	uint32_t name_h, val_h;
};
void pc_cluster_set_cfgsig(const struct pc_cfgsig *e, int n);
/* the entry named as @e is, its value from @e: after a reload (none of that name: nothing) */
void pc_cluster_set_cfgsig_one(const struct pc_cfgsig *e);
/* a member's fact (PC_FACT_*): 1 and *val, or 0 when it did not say */
struct pc_member;
int pc_member_fact(const struct pc_member *m, uint32_t key, uint32_t *val);
/* this node's; returns how many */
int pc_cluster_cfgsig(const struct pc_cfgsig **e);

/* the retained tombstones for the alerts, and members heard back after a silence longer than
 * the table covers (gap n's line at gap_text[(n - 1) % PC_TOMB_GAP_KEEP]); 0 when the table is off */
#define PC_TOMB_GAP_KEEP 4
struct pc_tomb_alert {
	unsigned int retain_s, max;
	unsigned long long entries, dropped_early, noted, gaps;
	/* of the drops and the notes, the expiries */
	unsigned long long dropped_early_expiries, noted_expiries;
	char gap_text[PC_TOMB_GAP_KEEP][256];
};
int pc_cluster_tomb_alert(struct pc_tomb_alert *out);
/* replicate one AUTHORED eager record to every live peer now (worker thread, fire-and-forget);
 * 1 = keep it, 0 = a spread-mode non-holder, whose push group drops it on the holder's ack */
int pc_repl_push(const char *col, size_t collen, const char *key,
		size_t klen, const char *val, size_t vlen, unsigned int exp,
		unsigned long long ver, unsigned char type);

/* ---- proxy mode --------------------------------------------------------- */

/* completion kinds (struct pc_pull_done.kind) */
#define PC_DONE_PULL    0
#define PC_DONE_FWD_SET 1
#define PC_DONE_FWD_DEL 2
#define PC_DONE_FWD_ADD 3
#define PC_DONE_JSON    4              /* forwarded JSON path op */
/* a reconcile probe: no worker waits, so the answer is acted on inside the peer thread */
#define PC_DONE_RECONCILE  6

/* a POSSESSION probe of ONE holder: only a positive answer may authorise dropping our surplus
 * copy (acted on in the peer thread); a negative or a timeout leaves it */
#define PC_DONE_POSSESS    7
#define PC_DONE_FWD_TOUCH  8           /* a forwarded re-arm */
#define PC_DONE_FWD_SETCOND 9          /* a forwarded SET NX/XX/IFEQ/IFNE -
                                        * ok = decided, newval = stored */
#define PC_DONE_FWD_DELCOND 10         /* a forwarded DELEX IFEQ/IFNE
                                        * - ok = decided, newval = deleted */
/* a forwarded op refused because the key holds a JSON document: the ack is ok 0 with this in
 * newval, and the asking node answers WRONGTYPE */
#define PC_FWD_E_TYPE   (-11)

#define PC_DONE_SET_RESUME 5           /* probe-before-place: ok=1 absent, replay (jop 1 set /
                                        * 2 add, newval=by, ttl_left, val); ok=0 timeout */

/* the locator cache */
int  pc_loc_get(const char *col, size_t collen, const char *key, size_t klen);
void pc_loc_set(const char *col, size_t collen, const char *key, size_t klen,
		int node);
void pc_loc_clear(const char *col, size_t collen, const char *key,
		size_t klen);

/* ---- shard mode (deterministic ownership) ------------------------------- */
/* the EFFECTIVE unicast receive buffer in bytes, read back: the kernel clamps to rmem_max
 * silently, and a clamped buffer drops forwards */
int pc_cluster_rcvbuf(void);

/* the interchange-relevant config ALIVE carries, so mismatching peers are refused;
 * @authoritative = declared via [cluster] collections */
void pc_cluster_set_config(int mode, int eager,
		int authoritative, int client_port, int resp_port,
		int http_port, int wal, int replicas);
/* K under mode = spread, 0 otherwise; the write path asks this so its K is the one the digest
 * agreed with the fleet about */
int pc_cluster_replicas(void);
int pc_cluster_fleet_has(int feat, int *lacking);
/* 1 while every live member announces its marks (cluster.c) */
int pc_cluster_acks_gate(int *lack, int max, int *nlack);
/* one RESP map override to every live peer that reads CLCOL_OP_MAP */
void pc_cluster_respmap_announce(int index, const char *col, uint64_t gen,
		int set);
/* one collection's autoscale mode, set here at @gen, to every live
 * peer that reads CLCOL_OP_AUTOSCALE */
void pc_cluster_autoscale_announce(const char *name, size_t nlen, int mode,
		unsigned long long gen);
/* the ALIVE feature bit for the rate-limit and hash record types
 * (clmemb.h CLMEMB_FEAT_TYPES2; cluster.c asserts the two agree) */
#define PC_FEAT_TYPES2 0x01
#define PC_FEAT_HREAD  0x02            /* forwarded hash reads return the record */
#define PC_FEAT_SLOCK  0x04            /* lock records, Symfony lock scripts */
#define PC_FEAT_RESPMAP 0x20           /* the RESP map at runtime (CLCOL_OP_MAP) */
#define PC_FEAT_AUTOSCALE 0x80         /* autoscale modes are fleet state */
/* the placement map's stamp a client must hold to route as this node does: 1 and the stamp
 * when valid, else 0; read relaxed - it rides a hint, never a decision */
int pc_cluster_map_stamp(unsigned int *term, unsigned int *seq);
/* 1 while placement reads the map, 0 while it falls back to the peers' liveness */
int pc_cluster_map_usable(void);
/* a digest of what placement reads from the map (nodes, addresses, weights), 0 with no valid
 * map; a re-publication that moves only the seq keeps it */
uint64_t pc_cluster_map_place(void);
/* announce a collection SET/DROP/RESIZE (@op, CLCOL_OP_*) to every live peer, re-sent in full to
 * a newly seen one; returns the generation sent (0 = not); a RESIZE is re-sent 1, 3 and 7 s on */
unsigned long long pc_cluster_col_announce(const char *name, size_t nlen,
		int buckets_log2, int op);
/* the same under @gen, the generation the store stamped (0 = chosen as above) */
unsigned long long pc_cluster_col_announce_gen(const char *name, size_t nlen,
		int buckets_log2, int op, unsigned long long gen);

/* the same, for a rename - the message carries both names */
void pc_cluster_col_announce2(const char *from, size_t flen, const char *to,
		size_t tlen);

int pc_cluster_mode(void);
/* eager is legal ONLY with mode=store, so pc_cluster_mode_name() names one of four states,
 * keeping the eager half a bare mode would drop */
int pc_cluster_eager(void);
const char *pc_cluster_mode_name(void);
int pc_cluster_port(void);   /* the shared peer-plane port */

int pc_cluster_authoritative(void);

/* one fleet member as a CLIENT sees it */
struct pc_member {
	struct in_addr addr;
	int client_port;
	int node;
	int is_self, is_master;
	/* PC_NST_*: is_master is its authority, state whether its data can be trusted yet */
	int state;
	int reconciling;               /* a reconcile is open on it */
	int sole_witness;              /* and its only live witness: 0 none,
	                                * 0xFFFF two or more, -1 not said */
	int reason;                    /* PC_NRE_*, WHY it is in that state;
	                                * 0 = none given, or a peer predating the field */
	/* seconds since this member LEFT, -1 while present; the card is kept a couple of minutes,
	 * so a restart does not look like a departure */
	int gone_s;
	int live;                      /* heard from inside PEER_UP_MS -
	                                * self 1, a departed member 0 */
	int gone_why;                  /* CLRES_GONE_*, with gone_s >= 0 */
	int gone_silent_s;             /* silent this long before purge */
	int held_s;                    /* how much longer its id is reserved -
	                                * the card goes when this lapses */
	int free_mb, total_mb;
	/* its RESP door (0 = none or not reported); the CLUSTER family advertises THIS port, since
	 * the native door is Noise-only off-box */
	int resp_port;
	int http_port;                 /* 0 = no HTTP door, or not reported */
	long uptime_s;                 /* -1 = not reported (older build) */
	int link_pct, link_level;      /* its link's busier direction (-1 = not
	                                * watched or not said) and host.link's level */
	/* what this node's view of the member holds: the record count and start kind its own ALIVE
	 * last reported, the view lowest_live_sender() decides from */
	long entries;                  /* -1 = self unknown (never) */
	const char *start;             /* cold|recovered|established|unknown */
	/* identity is PERSISTED, incarnation regenerated every start: together they tell a new
	 * node, a restart that lost memory and a missed heartbeat apart (has_ident 0: not sent) */
	int has_ident;
	unsigned char ident[16];
	unsigned int incarn;
	/* the master's chosen standby */
	int is_backup;
	/* PCACHE_MEM_*: the huge-page tier the arena actually got; fleet-visible, as a node fallen
	 * back to 4K has quite different latency */
	int mem_tier;
	/* open client connections, total and by door (resp door / native binary / native RESP), as
	 * last gossiped; cl_reported = 0 for an older build */
	int cl_reported;
	long cl_open, cl_resp, cl_bin, cl_nresp;
	/* its per-collection figures as gossiped (the first ncols of
	 * ncols_total live collections); cols_reported = 0 for an older build */
	int cols_reported, ncols, ncols_total;
	struct clmemb_col cols[CLMEMB_COLS_MAX];
	/* its pub/sub relay port (0 = none) and whether relays to it take
	 * that port - this node's view, proven by a probe answer */
	int relay_port, relay_direct;
	int relay_interest;            /* 0 all, 1 filtered, 2 broadcast */
	int ncfg;                          /* the settings it reported (0: none) */
	uint32_t cfg_name[PC_CFGSIG_MAX], cfg_val[PC_CFGSIG_MAX];
};
/* self + every peer the table holds + every departed member whose id is reserved (CLRES_MAX):
 * the list /members, CLUSTER NODES/SLOTS and MOVED read */
#define PC_CL_MAXMEMBERS (PC_CL_MAXPEER + 1 + 256)
/* the routing contract: rendezvous over the Redis slot, VERSIONED - a client that does not know
 * the string stops routing; change it whenever the placement input changes */
#define PC_ROUTE_ALGO "hrw-slot16k-v1"

/* the CLUSTER WIRE EPOCH: what a datagram's plaintext MEANS (not PC_CL_VER, not the release).
 * Bump it for a change an older peer cannot safely ignore, not for a length-gated trailing field */
#define PC_CL_EPOCH 4
/* a build speaks a range of epochs, a fleet ONE committed epoch the master raises once every
 * member can speak it; PERFCACHED_TEST_EPOCH_MAX raises the newest for the suites */
#define PC_CL_EPOCH_MIN PC_CL_EPOCH    /* at a bump: the epoch before it */
#define PC_CL_EPOCH_MAX 4
int pc_cluster_epoch(void);            /* the committed epoch this node speaks */
int pc_cluster_epoch_min(void);
int pc_cluster_epoch_max(void);
/* the master raises (or lowers) the fleet's epoch: 0, or -1 with @why */
int pc_cluster_epoch_commit(int epoch, char *why, size_t n);
/* FACTS ride the settings group under keys with bit 31 set: reported, never compared; a new
 * fact is a new key, and a key never changes meaning */
#define PC_FACT_BIT       0x80000000u
#define PC_FACT_VERSION   0x80000001u   /* tweak<<24 | major<<16 | minor<<8 | patch */
#define PC_FACT_EPOCHS    0x80000002u   /* oldest<<8 | newest wire epoch */
#define PC_FACT_FEATURES  0x80000003u   /* CLMEMB_FEAT_* it advertises */
#define PC_FACT_UPGRADE   0x80000004u   /* helper, upgrade state, the
                                         * staged build (upgrade.h) */
#define PC_CL_NFACTS      4
/* the digest of runtime autoscale modes (pc_store_autoscale_sig), in a SPARE slot outside
 * PC_CL_NFACTS: a member whose settings fill the keepalive does not report it */
#define PC_FACT_AUTOSCALE 0x80000005u
/* the build's git rev (its first 7 hex digits), in a SPARE slot after the autoscale digest */
#define PC_FACT_REV       0x80000006u

/* fills @out with up to @max members (self first); returns the count */
int pc_cluster_members(struct pc_member *out, int max);
/* offer a staged build to every member that takes one (CLMEMB_FEAT_STAGE); returns how many,
 * *older = members that do not; the results arrive in pc_upg_relay_note */
int pc_cluster_stage_relay(const void *data, size_t len,
		const unsigned char *sha, const unsigned char *sig,
		const char *version, int *older);
/* "goodbye" | "silent" | "forgotten" | "unknown" */
const char *pc_cluster_gone_reason(int why);

/* the fleet's unknown commands: @self folded with every live peer's latest, newest first;
 * @reporting = members whose table is current.  0 when unclustered (@out = @self) */
struct clunk_table;
int pc_cluster_unknown_fleet(const struct clunk_table *self,
		struct clunk_table *out, int *members, int *reporting);
void pc_cluster_unknown_figures(unsigned long long *recv,
		unsigned long long *bad);

/* one collection's figures for the whole fleet: this node's block and every live peer's
 * gossiped one (@store = its store index).  0 when unclustered */
#include "clfleet.h"
int pc_cluster_col_fleet(int store, int basis, struct pc_col_fleet *out);

/* the pub/sub relay plane: _open binds @threads SO_REUSEPORT sockets on the relay port and
 * returns how many _rx_thread to start, 0 = off (relays keep to the cluster socket) */
int  pc_cluster_pubsub_rx_open(int port, int threads);
void pc_cluster_pubsub_rx_thread(volatile int *stop, int idx);
/* relaying only what a peer wants */
struct pc_psint_figures {
	unsigned long long version;        /* this node's; 0 = it wants everything */
	int peers_filtered, peers_broadcast;
	unsigned long long skipped;        /* relays a peer's interest ruled out */
	unsigned long long updates_sent, updates_received;
	unsigned long long resyncs_sent, resyncs_received, requests_sent;
};
void pc_cluster_pubsub_interest_figures(struct pc_psint_figures *f);
void pc_cluster_pubsub_relay_figures(int *port, int *threads, int *peers_direct,
		unsigned long long *sent_direct, unsigned long long *sent_cluster,
		unsigned long long *rx_dgrams);
/* write-path replication on the relay port.  TCP path: [0] groups sent, [1] fallbacks, [2] lost
 * with a broken connection, [3] frames received, [4] connects, [5] failed */
void pc_cluster_repl_tcp_figures(unsigned long long out[6]);
void pc_cluster_repl_plane_figures(int *peers_direct,
		unsigned long long *sent_direct, unsigned long long *sent_cluster,
		unsigned long long *rx_groups, unsigned long long *rx_refused);
const char *pc_start_kind_name(int kind);

/* rendezvous over the members' STABLE advertise addresses: the owner's node id, 0 = SELF (or
 * cluster off); an owner miss is authoritative */
int pc_shard_owner(const char *col, size_t collen, const char *key,
		size_t klen);

/* the same decision, addressed by slot: what CLUSTER SLOTS enumerates */
int pc_shard_owner_slot(unsigned slot);
/* is @node_id (0 = this node) one of @slot's K holders?  Same map-then-HRW structure as
 * pc_shard_owner_slot, so spread and shard never disagree */
int pc_spread_in_set(unsigned slot, int node_id, int k);
/* the best-ranked live PEER holding @slot, 0 if none; self is excluded - this runs after a
 * local miss */
int pc_spread_pick_peer(unsigned slot, int k);
int pc_spread_pick_peer_except(unsigned slot, int k, int except);
/* 1 when every member of @slot's K-set is LIVE: the reclaim pass drops a surplus copy only
 * then, never while a member sits in its departure grace */
int pc_spread_set_live(unsigned slot, int k);

/* nonzero while a membership change is recent: shard owner-misses fall back to ONE broadcast
 * pull, as the data may not have moved to its new owner yet */
int pc_shard_grace(void);
int pc_cluster_replicas_short(void);

/* placement: power-of-two-choices on free-MB with the self band.
 * Returns 0 = place LOCALLY (self won), else the winning peer node. */
int pc_place(void);

/* a proxy-mode pull that may unicast to a known holder */
uint32_t pc_pull_begin_at(const char *col, size_t collen, const char *key,
		size_t klen, int holder_node);

/* forward a JSON path op to the holder, parked like pc_fwd_begin; the holder runs pc_json_rmw
 * under ITS stripe and the ack carries status/newval/count/fragment */
uint32_t pc_fwd_json_begin(int node, int jop, const char *col,
		size_t collen, const char *key, size_t klen,
		const char *path, size_t plen, const char *val, size_t vlen,
		long long by, int have_ttl, long long ttl, int nx, int xx,
		int mkpath);

/* probe-before-place: a positive answer turns the pull into a forward, all-negative completes
 * SET_RESUME ok=1 (touch and del never place), a timeout ok=0; 0 = not begun, place as before */
uint32_t pc_probe_fwd_begin(int op, const char *col, size_t collen,
		const char *key, size_t klen, const char *val, size_t vlen,
		unsigned int ttl_rel, long long by);

/* forward a write to @node, completing on the worker like a pull; op 0 set, 1 del, 2 add,
 * 3 touch.  0 = not sent, the caller answers locally */
uint32_t pc_fwd_begin(int node, int op, const char *col, size_t collen,
		const char *key, size_t klen, const char *val, size_t vlen,
		unsigned int ttl_rel, long long delta);
/* the node the last pc_fwd_begin/pc_pull_begin_at on THIS thread sent @req to (0 = none), and
 * a forward's ttl_rel; keyed by req, so an unparked request lends nothing to the next */
int pc_fwd_target_of(uint32_t req, unsigned int *ttl);

/* whether the rebalancer has anything to do, and why; the reason is formatted when READ, so a
 * reader never sees a string torn by a concurrent tick */
struct pc_rebalance {
	int settled;                   /* 1 = nothing to move */
	char reason[160];
	long long settled_since;       /* unix s, 0 while leveling */
	long long last_move;           /* unix s, 0 = never */
	unsigned long long moved_records;
	unsigned long long ticks_shed;
};
void pc_cluster_rebalance(struct pc_rebalance *out);

struct pc_proxy_stats {
	unsigned long long placed_local, placed_remote, fwd_sent, fwd_served,
		fwd_fails, migrated_out, migrated_in, migrate_skipped_big,
		migrate_lost, migrate_retx, migrate_dgrams, bulk_out, bulk_in, repl_out,
		demotes_sent, demotes_applied, loc_hits, loc_clears,
		demotes_kept,      /* a newer copy than the demoted one */
		/* writes accepted, forwarded to the K holders and NOT kept: zero on every node of a
		 * spread fleet means clients all write to holders, or retention is not applied */
		spread_not_held,
		/* times the holder set changed and a repair was armed */
		spread_repaired,
		/* surplus copies dropped; climbs after a set change and stops - if it never stops,
		 * the fleet churns or a peer disagrees about the set */
		spread_reclaimed,
		spread_possess_sent,
		spread_possess_kept,
		/* writes taken as a NON-holder, parked in an acked push group: kept (left to
		 * reclaim), or dropped on the holder's full ack */
		spread_ack_parked, spread_ack_kept, spread_ack_dropped,
		/* ...of the kept: a short ack, and no ack within the wait */
		spread_ack_short, spread_ack_lost,
		/* the digest-gated sweep: rounds asked/answered/unanswered, clean (all slots
		 * matched), idle, slots compared/different, records digested, peer walks */
		sweep_gate_asked, sweep_gate_answered, sweep_gate_unanswered,
		sweep_gate_clean, sweep_gate_idle, sweep_gate_slots_asked,
		sweep_gate_slots_diff, sweep_gate_digested, sweep_gate_walks,
		/* the diff toward a member back from its WAL: armed, rounds, differing slots
		 * walked, ended all equal, ended still differing, not possible */
		dsync_armed, dsync_rounds, dsync_slots, dsync_done, dsync_stopped,
		dsync_off,
		spread_gap_repaired,  /* re-placements armed toward a peer back from silence */
		/* copies refused because ours was the same age or newer; only climbing means a
		 * sender loops on records nobody will take */
		recv_older,
		/* copies refused because a tombstone carries a NEWER version: a superseded set
		 * arriving after the delete, each one a resurrection that did not happen */
		recv_tombstoned,
		/* copies refused at the arena ceiling, and ones that dropped an OLDER copy so a read
		 * pulls the current value; climbing on an idle fleet = the node is too small */
		recv_nomem, recv_stale_dropped,
		/* copies refused as older than one dropped above - the
		 * stale re-send that would have taken the freed cell */
		recv_below_dropped,
		/* eager bulk-plane BATCHES whose outcome did not confirm; they stop a sweep cycle
		 * advancing its mark */
		repl_bulk_lost;
	/* write-path pushes sent (one per live peer per write), and the
	 * records the repair sweep left alone as dying within one sweep */
	unsigned long long repl_pushed, repl_skipped_dying;
	/* the bootstrap pull - records this node streamed to a joiner,
	 * records this node stored from its own pull, pulls that failed */
	unsigned long long boot_out, boot_in, boot_failed;
	/* write-path push datagrams sent (records per peer are
	 * repl_pushed; a group carries many) */
	unsigned long long repl_groups;
};
void pc_proxy_get_stats(struct pc_proxy_stats *out);

/* the rebalancer tick (peer thread, ~10s): donor-initiated, hysteresis
 * vs the fleet mean, byte-budgeted, stub-first */
void pc_rebalance_tick(void);

/* the receive figures are sampled inside this call by the thread serving the read, never
 * on a tick that may stall; the per-second fields are rates since the last read (>= 0.5 s) */
void pc_cluster_get_stats(struct pc_cl_stats *out);
/* the first node held CONNECTING and the pairs it lacks, "" when none (the alert's text) */
void pc_cluster_admission_stuck_text(char *out, size_t cap);

/* a fleet-wide stats reset: send @at (unix s) to every live peer, applied through the daemon's
 * callback; returns the peers it went to (0 when not clustered) */
int pc_cluster_stats_reset_send(long long at);
void pc_cluster_set_stats_reset_cb(void (*cb)(long long at));
/* the alert relay: a node that cannot mail sends its alert to the elected mailer, which mails
 * it naming the origin; the cluster thread calls the callback per relay, duplicates dropped */
struct pc_alert_relay {
	int node;                      /* the origin (filled on send) */
	unsigned int seq;              /* per origin: the duplicate's key */
	int level;                     /* PC_AL_WARNING / PC_AL_CRITICAL */
	int cleared;
	char key[48];
	char host[64];                 /* the origin's hostname */
	char text[200];
};
void pc_cluster_set_can_mail(int can);
/* the lowest node id among the live, READY members, self included, that
 * can mail; 0 = none (and outside a cluster).  *@is_self: it is this node */
int pc_cluster_mailer(int *is_self);
/* cluster datagrams the receive buffers could not hold, since start */
unsigned long long pc_cluster_rx_drops(void);
/* frames refused as taken before (a seen sequence, older than the window, an older
 * incarnation), let through past full tables, or naming another host than their source */
void pc_cluster_replay_counts(unsigned long long *dup, unsigned long long *old,
		unsigned long long *incarn, unsigned long long *full,
		unsigned long long *host);
/* to the live member @to, twice; 0 when a copy went, -1 not */
int pc_cluster_alert_relay_send(int to, const struct pc_alert_relay *r);
void pc_cluster_set_alert_relay_cb(void (*cb)(const struct pc_alert_relay *r));
/* stop the whole fleet (worker thread): every live peer is told to stop and sorted into @gone
 * (said GOODBYE), @still and @lost; this node stops last.  Peers sent to, -1 if not clustered */
int pc_cluster_fleet_stop(long long timeout_ms, int *gone, int *ngone,
		int *still, int *nstill, int *lost, int *nlost, int cap);
/* hand mastership to live member @node (0 cancels); NULL or why not */
const char *pc_cluster_master_prefer(int node);
/* release departed member @node's reserved id here, on the next beat; NULL or why not */
const char *pc_cluster_forget(int node);
/* bumped by every forget applied - the alerts' fleet size shrinks with it */
unsigned int pc_cluster_forget_gen(void);
int  pc_cluster_peers(struct pc_cl_peer_info *out, int max);
/* peers holding a node id, up or not, plus silent ones purged with their id still reserved */
int  pc_cluster_peers_known(void);

#endif /* PC_CLUSTER_H */
