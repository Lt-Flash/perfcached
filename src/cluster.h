/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * cluster.h — the peer plane (tasks S16-S18): membership + heartbeats,
 * store-mode pull-on-miss, delete tombstones.
 *
 * Wire: UDP datagrams, AEAD-sealed with the CLUSTER PSK directly -
 * [magic u8 0xA1][ver u8][nonce 12B random][ct+tag], AD = the 14-byte
 * prefix.  Deliberate v1 scope trims, all documented:
 *  - no per-peer Noise handshake on the DATAGRAM plane: forward secrecy
 *    lives on the stream channels (client + migration); the datagrams
 *    carry control traffic and pull payloads under the cluster secret.
 *    Replay is tolerated at the application layer (heartbeats and pull
 *    requests are idempotent; answers are matched to a pending slot and
 *    duplicates dropped);
 *  - no elections: nothing in M4 consumes one - membership is flat and
 *    coordinator-free by design;
 *  - large pull answers ride kernel UDP fragmentation (same-LAN scope,
 *    the clusterer_controller precedent; the migration plane, which
 *    moves bulk, is TCP).
 *
 * Auth posture (S19 decision, made autonomously per standing
 * instruction): a peer that cannot authenticate is QUARANTINED - its
 * datagrams are counted and dropped with a rate-limited warning; the
 * daemon never self-terminates over it.  A cache node serving local
 * traffic beats a dead one; the counter is the operator's signal.
 *
 * Inner messages (LE):
 *   1 HEARTBEAT  {node u16, free_mb u32, total_mb u32, live_kb u32,
 *                 - free_mb/total_mb are the CEILING's since S117:
 *                   ceiling - held, and the ceiling (the pool's share
 *                   before; same fields, same units)
 *                 mode u8, eager u8, digest u64,
 *                 client_port u16, identity[16], incarnation u32}
 *                 - identity and incarnation are the TAIL, so a
 *                   peer that predates them simply sends a shorter
 *                   datagram and is handled by node id as before
 *   2 PULL_REQ   {req u32, node u16, collen u8, klen u16, col, key}
 *   3 PULL_RSP   {req u32, node u16, found u8, ttl_left u32, vlen u32,
 *                 ver u64, val} - ver (A2) is the holder's version
 *   4 TOMBSTONE  {collen u8, klen u16, col, key}
 *  M5 additions (proxy mode - the capacity plane):
 *   5 FWD_OP     {req u32, node u16, op u8 (0 set/1 del/2 add), ttl u32,
 *                 delta i64, collen u8, klen u16, col, key, val}
 *   6 FWD_ACK    {req u32, ok u8, newval i64}
 *   7 MIGRATE    {req u32, node u16, ttl_left u32, collen u8, klen u16,
 *                 col, key, val}
 *   8 MIGRATE_ACK{req u32, ok u8[, stored u16]} - the stored-count was
 *                 added with 15; a 6-byte ack still parses (1 record)
 *   9 DEMOTE     {winner u16, collen u8, klen u16, col, key}
 *  15 MIGRATE_MANY {req u32, node u16, count u16} then per record
 *                 {ttl u32, collen u8, klen u16, vlen u32, ver u64, col,
 *                 key, val} - records GATHERED to the ~56KB datagram
 *                 cap, so small records amortize the datagram+ack cycle
 *                 (~200 x 256B records per cycle instead of one); one
 *                 ack confirms the group and in-flight groups are
 *                 tracked by record count, so migrate_lost stays exact
 *                 in RECORDS.  7 remains valid on the wire.
 *
 * Proxy-mode v1 shape (per the decided design):
 *  - the LOCATOR CACHE stores key-hash -> holder node (16B/slot, fixed
 *    table); stale locators self-heal - a unicast miss clears the entry
 *    and the next request broadcasts;
 *  - PLACEMENT at write time: power-of-two-choices on heartbeat free-MB
 *    across self + up peers, with the self-preference band (keep local
 *    when within 25%) and sticky-per-key via the locator; writes to a
 *    known holder FORWARD (op + ack) - counters serialize at the holder;
 *  - the BIRTH RACE detector: a second positive pull answer triggers a
 *    DEMOTE to the higher node id (lower wins, deterministic); the loser
 *    drops its copy and records the winner in its locator;
 *  - the REBALANCER (10s tick, hysteresis vs the fleet mean, byte
 *    budget): stub-first - the donor deletes locally and records the
 *    receiver in its locator BEFORE sending; a lost transfer means a
 *    MISS, never a fork.  v1 rides the datagram plane, so values above
 *    the datagram ceiling stay put (counted); the TCP bulk channel
 *    arrives with the binary verb set (libperfd co-design).  Victim
 *    order is scan order (coldest-first needs access stamps - later).
 */
#ifndef PC_CLUSTER_H
#define PC_CLUSTER_H

#include <netinet/in.h>

#include <stddef.h>
#include <stdint.h>
#include "pc_noise.h"
#include "clmemb.h"                   /* S163: struct clmemb_col */

#define PC_CL_MAGIC   0xA1
/* 2 (A2): the migrate/replicate record carries the sender's version.
 * open_dgram DROPS a datagram whose version byte differs, so a mixed
 * fleet does not mis-parse records - it simply cannot see itself, and
 * splits visibly.  That is the trade this codebase already makes for a
 * config mismatch ("a visible split beats a silent one", handle_alive),
 * and the alternative here is worse: the per-record field sits inside a
 * gathered batch, so an old node reading a new datagram would not read
 * a short record, it would read garbage.
 *
 * 3 (0.2.0, EC removal): HEARTBEAT, JOIN_REQ and MASTER_ALIVE dropped
 * the ec_k/ec_m bytes they carried for the config check, so every field
 * after them shifted two bytes left.  A stale build reading a new
 * datagram would find the digest, identity and incarnation at the wrong
 * offsets and act on the garbage rather than ignore it - the version
 * byte makes that a clean refusal instead. */
#define PC_CL_VER     3
#define PC_CL_MAXPEER 256              /* = CL_CTR_MAX_PEERS; a full
                                        * ASSIGN list is ~3KB of 60KB */

/* membership roles (automatic membership, S16 as designed) */
/*
 * The node's LIFECYCLE state (task B1) - a separate axis from the role
 * below.  The role says what authority this node has in the membership
 * (does it hand out ids); the state says whether its data can be
 * trusted yet.  A node can be MASTER and still be RECONCILING.
 *
 * STARTING    up, with nothing of its own to bring back.
 * RECOVERING  re-acquiring data: replaying the WAL and the snapshot,
 *             AND copying keys back per the cluster's mode.  It is
 *             about re-acquisition, whatever the source.
 * READY       serving.
 * DRAINING    leaving: goodbye sent, finishing what is in flight.
 *
 * JOINING is NOT here.  It was, and that was wrong: it is a membership
 * question and PC_ROLE_JOINING already answers it, so a joining node
 * reported the same fact twice on two axes.  RECONCILING is not here
 * either - nothing branched on the difference between it and
 * RECOVERING, and a node that finds a digest mismatch while serving
 * repairs in the background without leaving READY, so reconciliation is
 * an activity rather than a state.
 *
 * STARTING versus RECOVERING is ultimately the MASTER's call, not the
 * node's: a node whose state directory was wiped sincerely believes it
 * is new, and only the master holds the identity history that says
 * otherwise.  What a node reports here is its own best guess; the map
 * is what settles it.
 *
 * The values are the map's wire encoding (PC_CLMAP_ST_*), asserted in
 * cluster.c - the heartbeat carries this byte, so the two cannot drift.
 */
#define PC_NST_STARTING    0
#define PC_NST_RECOVERING  1
#define PC_NST_READY       2
#define PC_NST_DRAINING    3
/*
 * FAILED - this node cannot be trusted with new work, and is NOT
 * leaving.
 *
 * DRAINING already removes a node from selection, and reusing it for
 * this was the obvious shortcut - but its contract is DEPARTURE: hand
 * the data over, then go.  A node that borrows it to say "I am broken"
 * strands whatever it holds, because nothing hands the keys over and
 * nothing expires them if they have no TTL.  In proxy or shard mode,
 * where a key has exactly one home, that is data loss wearing the
 * costume of a health action.
 *
 * So FAILED says the other thing, and only that thing: stop selecting
 * me for new work.  The keys stay mine and stay readable, because they
 * are still correct - what is unsafe is accepting MORE, not answering
 * for what is already held.
 *
 * TERMINAL, like DRAINING, and the reason is shard mode.  Ownership
 * there is computed from the member set, so going FAILED re-shards the
 * cluster and coming BACK re-shards it again: two full rebalances for
 * one fault, and a node that flaps would thrash the fleet for as long
 * as it flapped.  A node stays FAILED until an operator restarts it -
 * which also matches what put it there, since storage that could not
 * keep up does not fix itself.  The only transition out is DRAINING, so
 * a failed node can still shut down cleanly.
 */
#define PC_NST_FAILED      4
/*
 * STALLED (S216) - the cluster thread has stopped making progress.
 *
 * One thread applies every replicated record, answers every pull and
 * probe, and runs the control plane; a SEPARATE thread keeps the node's
 * heartbeat going when that one is heads-down.  So a cluster thread that
 * blocked for good - a lock, a syscall that hangs, a walk that does not
 * yield - left a node that went on heartbeating READY, stayed in every
 * client's selection, and applied nothing.  Overload has a visible
 * answer (S38); a stall had none.
 *
 * Not FAILED: FAILED is terminal and means acknowledged data was lost.
 * A stall loses nothing - it delays - so this state is NOT terminal and
 * nobody sets it: it is an OVERLAY.  pc_node_state() answers STALLED
 * while the heartbeat thread sees no progress and the state underneath
 * is READY; the lifecycle underneath goes on untouched, and when the
 * thread moves again (for a full apply_stall_ms) the overlay lifts by
 * itself.  Like RECOVERING it is a transient state: the node stays in
 * the map, so under shard nothing re-shards over it.
 *
 * What it does: the node's own heartbeat says STALLED with the reason -
 * sent by the one thread still running - so peers drop it from the
 * member list they hand to routing clients, the map records it, and
 * pc_clsel_eligible() stops selecting it.  What it does NOT do: refuse
 * anything.  Requests that reach it are served as well as they can be.
 */
#define PC_NST_STALLED     5
/*
 * HEALING (S229) - the WAL lost acknowledged writes (a ring drop or a
 * segment overrun), and a snapshot is making them durable again.
 *
 * Not FAILED: FAILED is terminal because leaving and returning re-shards
 * the fleet twice, and a heal must not.  So HEALING is TRANSIENT, like
 * RECOVERING: the node stays in the map (nothing re-shards over it),
 * leaves client selection (pc_clsel_eligible wants READY), refuses
 * WRITES - more of what it cannot yet make durable - and serves reads,
 * whose data is correct.  Only the heal itself returns it to READY (see
 * pc_node_wal_heal); a heal whose snapshot fails goes FAILED.
 */
#define PC_NST_HEALING     6

/* S186: why a node is in the state it is in.  A card that says FAILED
 * and nothing else sends an operator to the logs of a node that may not
 * be the one they are looking at - the fleet view is every node's view
 * of every other.  A code travels; the wording lives in one place. */
#define PC_NRE_NONE        0
#define PC_NRE_WAL_DROP    1       /* acknowledged writes lost from the log */
#define PC_NRE_RECOVERING  2       /* replaying its own state */
#define PC_NRE_JOINING     3       /* waiting for the fleet's map */
#define PC_NRE_DRAINING    4       /* asked to hand its data over */
#define PC_NRE_FAILED_OTHER 5      /* failed, and nobody said why - a
                                    * placeholder that should never be
                                    * seen, and names itself if it is */
#define PC_NRE_APPLY_STALL 7       /* S216: the cluster thread made no
                                    * progress for apply_stall_ms */
#define PC_NRE_WAL_HEAL    8       /* S229: the WAL lost writes; a snapshot
                                    * is making them durable again */
#define PC_NRE_WAL_IO      6       /* S215: the device refused a WAL write
                                    * or sync.  Additive: a peer that does
                                    * not know the code shows the state
                                    * without a reason */
const char *pc_node_reason_text(int code);
int pc_node_reason(void);
void pc_node_reason_set(int code);

/* the state of THIS node, and its name for logs and stats */
int pc_node_state(void);
const char *pc_node_state_name(int st);
void pc_node_state_set(int st);
/* S215: the WAL's shed callback (pc_wal_on_shed) - FAILED, with the
 * reason.  pc_cluster_init() installs it; a daemon WITHOUT a cluster
 * installs it itself, or none of the WAL's failures refuse a write. */
void pc_node_wal_shed(int why);
/* S229: the WAL's heal callback (pc_wal_on_heal): 1 = HEALING, 0 = healed */
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
	/* the peer's IDENTITY, canonical 8-4-4-4-12, empty when it has not
	 * reported one.  The node id is a per-fleet handle and is reused;
	 * this is the thing that says WHICH MACHINE, which is what an
	 * operator needs when two of them turn up claiming one id. */
	char ident[37];
	int ident_ver;                 /* UUID version: 8 derived from the
	                                * platform, 7 randomly minted, else
	                                * unknown provenance */
};

struct pc_cl_stats {
	int enabled, node_id, peers_up;
	int replicas_short;            /* S157: replicas minus live members, 0 when met */
	int role;                      /* PC_ROLE_* - runtime, elected */
	int state;                     /* PC_NST_* - runtime, lifecycle */
	int master_id;
	unsigned long long hb_sent, hb_seen, bad_auth;
	/* S216: times the cluster thread was found stalled, the longest
	 * (ms), whether it is stalled now and for how long */
	unsigned long long apply_stalls, apply_stall_ms_max;
	unsigned long long epoch_refused;   /* S221: peers refused on the epoch */
	/* S223: the reconcile's witness - the live peer that was up while
	 * this node was down, whose "no" is therefore evidence of a delete
	 * (0 = none found yet) - and whether the pass was skipped because
	 * nobody was: the fleet came back together and nothing is deleted */
	int reconcile_witness;
	int reconcile_skipped;
	/* S217: threads applying replicas (1 = the receiver), records handed
	 * to them, times the receiver waited on a full ring, bytes queued */
	int apply_threads;
	unsigned long long apply_dispatched, apply_blocks, apply_backlog;
	int stalled;
	long long stalled_for_ms;
	unsigned long long hb_watchdog;    /* beats the watchdog re-sent
	                                    * because the peer thread was
	                                    * heads-down past the overdue
	                                    * window - nonzero = a duty ran
	                                    * long enough to cost liveness
	                                    * before the fix existed */
	unsigned long long joins, assigns, elections, demotions;
	/* S125: the RECEIVE plane's lag.  A node that cannot keep up with
	 * inbound replicas still answers its 1 Hz heartbeat inside the ten
	 * second liveness window and still serves its client door quickly,
	 * so falling behind is invisible from the outside - the only
	 * symptom is a read of a key that exists solely as an unapplied
	 * replica.  These are the figures that say it:
	 *   rx_applied_ps - replica records applied per second, sampled a
	 *                   beat at a time against the real elapsed ms;
	 *   rx_older_ps   - copies refused as older per second, which is
	 *                   the applier losing the version race;
	 *   rx_drops_ps   - datagrams the socket buffer could not hold, per
	 *                   second, PER SOCKET (SO_RXQ_OVFL, not the
	 *                   host-wide RcvbufErrors - a host may run more
	 *                   than one daemon);
	 *   rx_queue      - bytes sitting in the two receive queues right
	 *                   now, and rx_rcvbuf what they are allowed.  Read
	 *                   with SO_MEMINFO: FIONREAD on a Linux UDP socket
	 *                   answers the size of the FIRST datagram, not the
	 *                   queue - measured 1,400 with 23,040 bytes
	 *                   queued - so it cannot be the depth figure. */
	unsigned long long rx_applied, rx_drops;
	unsigned int rx_applied_ps, rx_older_ps, rx_drops_ps;
	unsigned int rx_queue, rx_rcvbuf;
	unsigned long long pull_sent, pull_served, pull_hits, pull_misses,
		pull_timeouts;
	/* The parked-request table is [cluster] max_pending slots and it is the
	 * cluster plane's backpressure: a forward or pull that cannot get
	 * one is refused rather than queued, which is what stops a
	 * pipelining client making the daemon hold unbounded state while a
	 * peer is slow.  Refusing is correct; refusing SILENTLY is not -
	 * clients saw "forward failed" while the log said nothing, and it
	 * took a benchmark to find.
	 *
	 * pend_peak is the high-water mark, so the knob can be sized from
	 * measurement instead of guessed at; pend_exhausted counts what it
	 * cost while it was too small. */
	unsigned long long pend_exhausted;
	int pend_peak, pend_max;
	/* live occupancy.  The peak cannot show a LEAK: if a release
	 * site were ever missed this climbs and never comes back, and
	 * the table reports full with every slot free.  Quiet fleet,
	 * pend_used == 0. */
	int pend_used;
	int reserved_ids;              /* S90: departed bindings held */
	/* The OTHER two reasons a begin() returns 0, split apart because
	 * "forward failed" covering both is what sent an operator to look
	 * at a healthy network in the first place.  Counted separately so
	 * the cause is read off the daemon instead of guessed at:
	 *   no_route  - cluster off, bad args, or NO PEER for the key,
	 *               which under load usually means a peer was declared
	 *               down (PEER_UP_MS) while heartbeats were starved;
	 *   send_fail - the datagram did not leave.  send_errno is the
	 *               last errno for it: ENOBUFS/EAGAIN is a saturated
	 *               socket and is retryable, EPERM is not.
	 * Relaxed atomics: several workers bump these off the lock, and a
	 * diagnostic that costs a lock on the hot path is the wrong
	 * trade. */
	unsigned long long fwd_no_route, fwd_send_fail;
	int fwd_send_errno;
	unsigned long long tomb_sent, tomb_applied, neg_hits;
	unsigned long long joins_rejected;
	/* B4: keys dropped because they were recovered from the WAL and no
	 * node in the fleet still had them - i.e. deleted while this node
	 * was down, and replay brought them back. */
	unsigned long long reconciled, reconcile_probed,
	                   reconcile_deferred;   /* S152: drops refused while a peer was cold */
	int reconcile_active;
	/* S212: the clocks.  repl_sweep_ms is the wall time of the LAST
	 * COMPLETED repair cycle (one peer, one collection, cursor 0 to 0),
	 * with the records it scanned and sent; repl_sweep_cycles counts
	 * them.  reconcile_ms is the last completed reconcile pass, first
	 * probe to last; reconcile_pending is probes issued and not yet
	 * answered (a gauge).  Durations, not rates. */
	unsigned long long repl_sweep_ms, repl_sweep_scanned, repl_sweep_sent,
	                   repl_sweep_cycles;
	unsigned long long reconcile_ms, reconcile_pending;
	/* the standby this master has designated (0 = none), and how many
	 * identities it remembers having seen.  A cluster with a master and
	 * no standby is one failure from losing its control plane, so it is
	 * reported rather than left to be inferred. */
	int backup_id, hist_n;
	/* the control state this node holds on behalf of a master, and the
	 * traffic that put it there.  held_valid false on the designated
	 * standby means a promotion would start from nothing. */
	int held_valid, held_hist_n;
	unsigned int held_term, held_seq;
	unsigned long long sync_sent, sync_rx, sync_ack, sync_bad;
	/* the cluster map this node holds (control plane).  Nothing reads
	 * it for placement yet - these are here so the plumbing can be
	 * watched on a live fleet before anything depends on it. */
	int map_valid, map_nodes, map_master;
	unsigned int map_term, map_seq;
	unsigned long long map_pub, map_rx, map_stale, map_bad;
	/* how ownership was decided: from the map, or from this node's own
	 * liveness while the map catches up.  place_hrw still climbing on a
	 * settled fleet means the map never caught up. */
	int map_usable;
	unsigned long long place_map, place_hrw;
};

/* init (startup).  Membership is AUTOMATIC: only the multicast group is
 * configured; node ids are assigned at runtime by the elected master
 * (never freeze pc_my_node_id() at init - the tm ;cid= lesson).
 * @advertise may be NULL: the unicast address is then auto-detected by
 * a route lookup toward the group.  Returns 0 or -1. */
/* @state_dir: where this instance's identity file lives (the [wal] dir
 * is the natural home).  NULL = nothing to persist to, so the identity
 * is regenerated each start and the node presents as NEW on restart -
 * which is correct: a memory-only node has nothing to bring back. */
/* The site salt for platform-derived identity.  MUST be set before
 * pc_cluster_init(), which is where the identity is derived - the whole
 * point is that derivation completes before anything joins. */
void pc_cluster_set_site_salt(const char *salt);
/* S216: how long the cluster thread may go without progress before the
 * node says STALLED; 0 = never.  Before pc_cluster_init(). */
void pc_cluster_set_apply_stall_ms(int ms);
/* S217: replica apply sharded by key hash over this many threads;
 * 1 = on the receiver, as before.  Set before pc_cluster_init(); the
 * daemon then runs pc_cluster_apply_thread() for idx 0..n-1, n being
 * what pc_cluster_apply_threads() answers after init. */
void pc_cluster_set_apply_threads(int n);
int pc_cluster_apply_threads(void);
void pc_cluster_apply_thread(volatile int *stop, int idx);

int pc_cluster_init(const char *mcast_addr, int mcast_port,
		const char *advertise, const uint8_t psk[PC_NOISE_KEYLEN],
		int pull_timeout_ms, int negative_ms, int tombstone_ms,
		const char *state_dir, const char *legacy_dir, int max_pending);
/* S80: the state directory in use (NULL = ephemeral), and whether it
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

/* The forward plane's value ceiling: the largest value a synchronous
 * cluster forward datagram can carry (sealed UDP - MAX_DGRAM minus
 * the forward envelope).  Shared here because the VERB layer must
 * refuse what the plane cannot carry - naked 58000 literals in
 * verbs.c drifted apart from cluster.c's once (S54 ceilings). */
#define PC_MAX_FWD_VAL 58000

/* the peer thread body (owns the UDP socket; serves pulls, applies
 * tombstones, tracks membership, emits 1/s heartbeats) */
void pc_cluster_thread(volatile int *stop);

/* the bulk migration thread: TCP transfers (cluster-principal Noise
 * sessions on advertise:port) for records above the datagram ceiling -
 * donor sends this tick's oversized victims, the acceptor stores them.
 * Spawned by the daemon whenever the cluster is enabled. */
void pc_bulk_thread(volatile int *stop);

/* the heartbeat watchdog thread: re-sends the last built ALIVE (and
 * MASTER_ALIVE) whenever the peer thread has emitted nothing for
 * longer than the overdue window - a peer thread heads-down in a long
 * duty must never read as dead (PEER_UP_MS eviction).  Emission only;
 * membership decisions stay on the peer thread. */
void pc_cluster_beat_thread(volatile int *stop);

/* ---- the pull path (worker side) --------------------------------------- */

/* a worker parks a missed get here; the peer thread broadcasts the
 * request; the completion is posted back to the OWNING worker's
 * completion queue and delivered via pc_cluster_drain() on its thread.
 * Returns the request id (>0) or 0 (cluster off / no peers up - answer
 * the miss locally). */
/*
 * Why the last pc_*_begin() returned 0.  Read it IMMEDIATELY after a 0;
 * it is per-thread and the next begin overwrites it.
 *
 * Those functions return 0 for three unrelated reasons and every caller
 * used to map all of them onto "forward failed" - which sends an
 * operator to look at the network when the truth is that the daemon
 * refused to park one more request.  S38.
 */
#define PC_CLFAIL_NONE   0
#define PC_CLFAIL_ROUTE  1     /* cluster off, bad args, no such peer */
#define PC_CLFAIL_BUSY   2     /* parked-request table full: BACKPRESSURE,
                                * and the only one worth retrying */
#define PC_CLFAIL_SEND   3     /* the datagram did not leave */
int pc_cluster_last_fail(void);
/* Parked-request capacity, so the per-worker park table can match it:
 * a worker must never out-park the cluster, or parking becomes the
 * first thing to refuse and the cluster's own backpressure never
 * speaks. 0 when the cluster is off. */
int pc_cluster_pend_cap(void);

uint32_t pc_pull_begin(const char *col, size_t collen, const char *key,
		size_t klen);

/* negative cache: "the cluster answered no recently" */
int  pc_neg_hit(const char *col, size_t collen, const char *key, size_t klen);
void pc_neg_set(const char *col, size_t collen, const char *key, size_t klen,
		int ms);
/* S209: plant with the delete's version, and read it back */
void pc_neg_set_ver(const char *col, size_t cn, const char *key, size_t kn,
		int ms, unsigned long long ver);
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
	long long newval;              /* fwd-add: the absolute result */
	int from_node;                 /* pulls: who answered */
	unsigned int ttl_left;         /* relative seconds, 0 = never */
	/* the HOLDER's version for this record (A2).  A pulled copy has to
	 * be stored with it, not with a fresh local tick: a local number
	 * would be meaningless off this node, and - because the fleet's
	 * clocks converge to the maximum - it can land ABOVE the holder's,
	 * so the holder's very next genuine update would be refused as
	 * older.  0 from a peer that did not send one. */
	unsigned long long ver;
	/* the answer never came back, as distinct from a holder that
	 * answered NO.  Both arrive with ok = 0, and rendering them with
	 * one message sends an operator to look at the wrong thing: a
	 * timeout is an overloaded holder, a rejection is the write being
	 * refused.  (JSON ops encode a timeout as ok = 2, which cannot be
	 * generalised - 2 is truthy, so every `if (d->ok)` would render a
	 * timeout as success.) */
	int timedout;
	char *val;                     /* malloc'd, consumer frees */
	int vlen;
	int jop;                       /* JSON kind: the PC_JOP_* code */
	int count;                     /* jarrappend: elements after */
};

/* worker side: drain completions posted to THIS thread (returns count,
 * fills up to @max entries) - called when the worker's eventfd fires or
 * its epoll times out; also expires overdue pulls into misses. */
int pc_cluster_drain(int worker, struct pc_pull_done *out, int max);

/* register the calling worker thread for completion delivery */
void pc_cluster_worker_register(int worker, int efd);

/* tombstones: called by the delete path */
void pc_tombstone_send(const char *col, size_t collen, const char *key,
		size_t klen);
/* S73: replicate one AUTHORED record of an eager collection to every
 * live peer now - on the write path, fire-and-forget, no TTL filter.
 * Worker thread.  @exp is the absolute expiry tick (0 = never); @ver the
 * version the table stamped on the write (pcache_last_ver). */
/* Returns 1 if THIS node should keep the record, 0 if it must not.
 * Under mode = spread a node that placement leaves out of the top-K
 * set may accept and forward a write, but must not retain it: the
 * mode's whole promise is that a record costs K copies, and an extra
 * copy on every writer is an ORPHAN - outside the set, so the repair
 * sweep neither maintains nor reclaims it, and it holds arena until
 * TTL.  Anything that means the record could NOT be replicated (the
 * plane is off, the value is over the wire ceiling) returns 1, because
 * dropping the only copy would be data loss rather than placement. */
int pc_repl_push(const char *col, size_t collen, const char *key,
		size_t klen, const char *val, size_t vlen, unsigned int exp,
		unsigned long long ver);

/* ---- proxy mode (M5) ---------------------------------------------------- */

/* completion kinds (struct pc_pull_done.kind) */
#define PC_DONE_PULL    0
#define PC_DONE_FWD_SET 1
#define PC_DONE_FWD_DEL 2
#define PC_DONE_FWD_ADD 3
#define PC_DONE_JSON    4              /* forwarded JSON path op */
/* B4: a reconcile probe.  Not a client request - it asks the fleet
 * whether a key this node recovered still exists anywhere, and the
 * ANSWER IS ACTED ON INSIDE THE PEER THREAD rather than posted to a
 * worker, because no worker is waiting for it. */
#define PC_DONE_RECONCILE  6

/* S146: a POSSESSION probe.  Asks ONE designated holder "do you have
 * this key", and a positive answer is the only thing that may authorise
 * dropping our own surplus copy.  Like the reconcile probe it is acted
 * on inside the peer thread; unlike it, the trigger is the POSITIVE
 * answer, not a unanimous negative.  A negative completes as an
 * ordinary miss and a timeout has no worker-side handler at all - both
 * mean NOT-KNOWN, and the surplus simply stays. */
#define PC_DONE_POSSESS    7
#define PC_DONE_FWD_TOUCH  8           /* S213: a forwarded re-arm */

#define PC_DONE_SET_RESUME 5           /* probe-before-place resolved:
                                        * ok=1 fleet-confirmed absent
                                        * (replay the write, probe
                                        * suppressed - jop carries 1
                                        * set / 2 add, newval=by,
                                        * ttl_left=ttl, val=the value);
                                        * ok=0 probe timeout (refuse) */

/* the locator cache */
int  pc_loc_get(const char *col, size_t collen, const char *key, size_t klen);
void pc_loc_set(const char *col, size_t collen, const char *key, size_t klen,
		int node);
void pc_loc_clear(const char *col, size_t collen, const char *key,
		size_t klen);

/* ---- shard mode (deterministic ownership) -------------------------------
 * Rendezvous hashing over the members' STABLE advertise addresses:
 * returns the owner's node id, or 0 when SELF owns the key (also when
 * the cluster is off - local is always safe).  No locator, no
 * broadcast, no birth race: an owner miss is authoritative. */
/* the EFFECTIVE unicast receive buffer in bytes (read back after the
 * request: the kernel clamps to net.core.rmem_max silently, and a
 * clamped buffer drops forwards, which refuses client writes) */
int pc_cluster_rcvbuf(void);

/* S30: tell the cluster plane what interchange-relevant config this
 * node runs, so ALIVE can carry it and mismatching peers are refused
 * instead of silently fed.  @authoritative = the config declared it via
 * [cluster] collections (as opposed to the legacy per-collection form). */
void pc_cluster_set_config(int mode, int eager,
		int authoritative, int client_port, int resp_port,
		int http_port, int wal, int replicas);
/* S127: K under mode = spread, 0 otherwise.  The write path asks
 * this rather than reading the config, so a caller cannot get a K
 * that the digest never agreed with the fleet about. */
int pc_cluster_replicas(void);
/* RV-10: the placement map's stamp - what a client must hold to route
 * the way this node does.  1 and the stamp when a map is valid, else 0.
 * Read relaxed off the stats snapshot: it rides a hint, never a decision. */
int pc_cluster_map_stamp(unsigned int *term, unsigned int *seq);
/* S69: tell the fleet a collection was created or dropped here.  Sent to
 * every live peer, and re-sent in full to a peer the first time it is
 * seen, so a node that was down through the change picks it up when it
 * returns.  @gen orders the two operations on one name across the fleet. */
/* @op is a CLCOL_OP_* value (SET / DROP / RESIZE), sent as itself.  It
 * was once an int named `drop` collapsed to a boolean, which turned
 * every RESIZE announce into a fleet-wide DROP.  A rename needs two
 * names - use pc_cluster_col_announce2. */
void pc_cluster_col_announce(const char *name, size_t nlen, int buckets_log2,
		int op);

/* S69: the same, for a rename - the message carries both names */
void pc_cluster_col_announce2(const char *from, size_t flen, const char *to,
		size_t tlen);

int pc_cluster_mode(void);
/* eager is legal ONLY with mode=store (config.c refuses anything else),
 * so the pair names four states, not six: proxy, shard, store, and
 * store-with-eager.  pc_cluster_mode_name() collapses them to the one
 * operational name, because reporting the mode alone silently drops the
 * eager half - which is exactly how a dashboard came to call an eager
 * cluster "store". */
int pc_cluster_eager(void);
const char *pc_cluster_mode_name(void);
int pc_cluster_port(void);   /* the shared peer-plane port */

int pc_cluster_authoritative(void);

/* S34: one fleet member as a CLIENT sees it */
struct pc_member {
	struct in_addr addr;
	int client_port;
	int node;
	int is_self, is_master;
	/* B1: PC_NST_*.  Two axes - is_master says what authority this
	 * member has, state says whether its data can be trusted yet. */
	int state;
	int reason;                    /* S186: PC_NRE_*, WHY it is in that
	                                * state; 0 = none given, which is
	                                * also what a peer predating the
	                                * field sends */
	/* S192: seconds since this member LEFT, or -1 while it is present.
	 * A node that says goodbye is purged from the peer table, so its
	 * card vanished from every other node's fleet view and "3 of 3
	 * members" quietly became 2 - a restart looked exactly like a
	 * departure.  The id is reserved for a week; the CARD is kept for
	 * a couple of minutes, which is what a restart takes. */
	int gone_s;
	int held_s;                    /* S192: how much longer its id is
	                                * reserved - the card goes when this
	                                * lapses, not on a display timer */
	int free_mb, total_mb;
	/* S49: this member's RESP door, 0 when it has none (or its build
	 * predates the field).  The CLUSTER family advertises THIS port,
	 * never client_port: the native door is Noise-only off-box, so a
	 * Redis client sent there cannot even say hello. */
	int resp_port;
	int http_port;                 /* S78: 0 = no HTTP door, or not reported */
	long uptime_s;                 /* S78: -1 = not reported (older build) */
	/* S103: what this node's view of the member holds - the record
	 * count and start kind its own ALIVE last reported.  This is the
	 * view lowest_live_sender() decides from, so a reader can see the
	 * flap that used to make the lowest id look empty. */
	long entries;                  /* -1 = self unknown (never) */
	const char *start;             /* cold|recovered|established|unknown */
	/* Identity is PERSISTED per instance and survives a restart;
	 * incarnation is regenerated every process start.  Together they
	 * separate "a new node joined" (identity unseen) from "a node
	 * restarted and lost its memory" (same identity, new incarnation)
	 * from "a heartbeat was missed" (both unchanged) - which the node
	 * id alone cannot do, because rejoiners keep their id.
	 * has_ident = 0 for a peer whose build predates this. */
	int has_ident;
	unsigned char ident[16];
	unsigned int incarn;
	/* the master's chosen standby.  The map has named one since C1 and
	 * nothing surfaced it, so "who takes over" was invisible to an
	 * operator looking at the fleet. */
	int is_backup;
	/* PCACHE_MEM_* - which huge-page tier this node's arena actually
	 * got, not which one it asked for.  Fleet-visible on purpose: a
	 * node that fell back to 4K while its peers hold hugepages has
	 * quite different latency, and reading it off each node's startup
	 * log one at a time is how that goes unnoticed. */
	int mem_tier;
	/* S160: open client connections, total and by door/dialect, as the
	 * member last gossiped them; cl_reported = 0 for an older build */
	int cl_reported;
	long cl_open, cl_resp, cl_bin, cl_json, cl_nresp;
	/* S163: its per-collection figures as gossiped (the first ncols of
	 * ncols_total live collections); cols_reported = 0 for an older build */
	int cols_reported, ncols, ncols_total;
	struct clmemb_col cols[CLMEMB_COLS_MAX];
	/* PS11: its pub/sub relay port (0 = none) and whether relays to it take
	 * that port - this node's view, proven by a probe answer */
	int relay_port, relay_direct;
	int relay_interest;            /* PS12: 0 all, 1 filtered, 2 broadcast */
};
#define PC_CL_MAXMEMBERS 64
/* the routing contract clients are told about: algorithm + VERSION.
 * Bump the version whenever owner selection changes, so a client that
 * does not recognise it falls back to plain spreading. */
/* Rendezvous over the Redis SLOT (crc16(key) % 16384), not over a
 * key hash of ours.  The version is the safety mechanism: a client that
 * does not recognise the string turns routing OFF and falls back to
 * letting the daemon forward, so a mismatch costs a hop and never
 * correctness.  Change this string whenever the placement input
 * changes, or an old client will route confidently to the wrong node. */
#define PC_ROUTE_ALGO "hrw-slot16k-v1"

/*
 * S221: the CLUSTER WIRE EPOCH - what the plaintext of a cluster
 * datagram MEANS, as distinct from PC_CL_VER (the sealed envelope) and
 * PC_VERSION (the release).  Two daemons may differ in release and
 * interoperate; they may not differ in this.
 *
 * BUMP IT when a change cannot be safely ignored by a peer that does
 * not know it.  Do NOT bump it for a trailing field with a defined
 * default, which the length-gated parsers in clmemb.c genuinely ignore.
 * The test is not "is this new" but "what does an older peer DO with
 * it", and the answer is written in the commit that makes the change:
 *
 *   safe    - a new M_* message type (drain_one_fd's switch drops what
 *             it does not know), a new trailing field, a new value in a
 *             field only ever compared against a known one (S216's node
 *             state 5 reads as "not READY", the safe direction);
 *   NOT safe - a new value dispatched through a chain with no else.
 *             S213 added forward op 3 and probe ops 3 and 4; a pre-rc22
 *             peer acks op 3 as failure, and - worse - maps probe ops 3
 *             and 4 onto 0, so a forwarded DELETE executes as a SET.
 *             That is the class this epoch exists to refuse.
 *
 * 1 = the rc22..rc29 contract.  0 means the peer predates the epoch;
 * see the grandfather rule in cluster.c, which expires at the next bump.
 */
#define PC_CL_EPOCH 1

/* fills @out with up to @max members (self first); returns the count */
int pc_cluster_members(struct pc_member *out, int max);

/* S165: the fleet's unknown commands - @self (this node's table, from
 * pc_obs_unknown_local) folded with every live peer's latest, sorted
 * newest first; `here` is this node's share and `node` who saw a row
 * last.  @members counts this node and every live peer, @reporting those
 * whose table is current (a build before S165 never sends one).  Returns
 * 0 on an unclustered daemon, where @out is @self alone. */
struct clunk_table;
int pc_cluster_unknown_fleet(const struct clunk_table *self,
		struct clunk_table *out, int *members, int *reporting);
void pc_cluster_unknown_figures(unsigned long long *recv,
		unsigned long long *bad);

/* S164: one collection's figures for the whole fleet - this node's own
 * block and every live peer's gossiped one (S163).  The rules and the
 * types are clfleet's (M15); this walks the peers.  @store is the
 * collection's store index.  Returns 0 on an unclustered daemon. */
#include "clfleet.h"
int pc_cluster_col_fleet(int store, int basis, struct pc_col_fleet *out);

/* PS11: the dedicated pub/sub relay plane.  _open binds @threads sockets
 * on the relay port (SO_REUSEPORT, the node's address) and returns how many
 * receive threads to start, 0 when the plane stays off (disabled, or the
 * port is unusable - relays then keep to the cluster socket).  Each thread
 * runs _rx_thread with its index. */
int  pc_cluster_pubsub_rx_open(int port, int threads);
void pc_cluster_pubsub_rx_thread(volatile int *stop, int idx);
/* PS12: relaying only what a peer wants */
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
const char *pc_start_kind_name(int kind);   /* S103 */

int pc_shard_owner(const char *col, size_t collen, const char *key,
		size_t klen);

/* the same decision, addressed by slot: what CLUSTER SLOTS enumerates */
int pc_shard_owner_slot(unsigned slot);
/* S127: is @node_id (0 = this node) one of the K holders of @slot?
 * Mirrors pc_shard_owner_slot's map-then-HRW structure so spread and
 * shard can never disagree about where a key lives. */
int pc_spread_in_set(unsigned slot, int node_id, int k);
/* S127: the best-ranked live PEER holding @slot, 0 if none.  Self is
 * excluded - this runs after a local miss, so self is either not a
 * holder or a holder that does not have it. */
int pc_spread_pick_peer(unsigned slot, int k);
/* S127: 1 when every member of @slot's K-set is LIVE.  The reclaim
 * pass refuses to drop a surplus copy unless this holds - a set can
 * be complete on paper while a member sits inside its departure
 * grace, and deleting what we can see for copies we cannot is the
 * wrong bias. */
int pc_spread_set_live(unsigned slot, int k);

/* nonzero while a membership change (or a still-flowing reshard) is
 * recent: shard owner-misses fall back to ONE broadcast pull, because
 * the data may not have moved to its new owner yet */
int pc_shard_grace(void);
int pc_cluster_replicas_short(void);   /* S157 */

/* placement: power-of-two-choices on free-MB with the self band.
 * Returns 0 = place LOCALLY (self won), else the winning peer node. */
int pc_place(void);

/* a proxy-mode pull that may unicast to a known holder */
uint32_t pc_pull_begin_at(const char *col, size_t collen, const char *key,
		size_t klen, int holder_node);

/* forward a write to @node; completes on the worker like a pull.
 * op: 0 set (val/vlen/ttl), 1 del, 2 add (delta, ttl), 3 touch (ttl;
 * S213).  0 = failed to
 * send (caller answers locally). */
/* forward a JSON path op to the holder (proxy mode).  Same parking
 * contract as pc_fwd_begin; the holder runs pc_json_rmw under ITS
 * stripe and the ack carries status/newval/count/fragment. */
uint32_t pc_fwd_json_begin(int node, int jop, const char *col,
		size_t collen, const char *key, size_t klen,
		const char *path, size_t plen, const char *val, size_t vlen,
		long long by, int have_ttl, long long ttl, int nx, int xx,
		int mkpath);

/* probe-before-place (proxy set/add, holder unknown and not
 * recently-confirmed-absent): broadcast a pull carrying the deferred
 * write.  A positive answer TRANSFORMS the pending into a forward to
 * the answering holder (its ack completes the park as FWD_SET/
 * FWD_ADD); an all-negative completes as PC_DONE_SET_RESUME ok=1; a
 * timeout as ok=0.  @op: 0 set (val/vlen), 2 add (by), 3 touch (ttl_rel
 * only; S213), 1 del (RV-11) - for touch and del all-negative answers
 * absent and never places.  0 = could not
 * begin (no peers / oversized) - the caller places as before. */
uint32_t pc_probe_fwd_begin(int op, const char *col, size_t collen,
		const char *key, size_t klen, const char *val, size_t vlen,
		unsigned int ttl_rel, long long by);

uint32_t pc_fwd_begin(int node, int op, const char *col, size_t collen,
		const char *key, size_t klen, const char *val, size_t vlen,
		unsigned int ttl_rel, long long delta);

struct pc_proxy_stats {
	unsigned long long placed_local, placed_remote, fwd_sent, fwd_served,
		fwd_fails, migrated_out, migrated_in, migrate_skipped_big,
		migrate_lost, migrate_retx, migrate_dgrams, bulk_out, bulk_in, repl_out,
		demotes_sent, demotes_applied, loc_hits, loc_clears,
		/* S127: writes this node accepted, forwarded to the K holders
		 * and then did NOT keep, because placement leaves it out of
		 * the set.  A spread fleet where this is zero on every node
		 * is a fleet whose clients all happen to write to a holder -
		 * or a fleet where retention is not being applied. */
		spread_not_held,
		/* S127: times the holder set changed and a repair was armed.
		 * One per real membership change, not per record. */
		spread_repaired,
		/* S127: surplus copies dropped - records this node held but
		 * placement does not make it a holder of.  Climbs after a set
		 * change and then stops; a number that never stops means the
		 * fleet is churning or a peer disagrees about the set. */
		spread_reclaimed,
		spread_possess_sent,
		spread_possess_kept,
		/* A2: copies refused because ours was the same age or newer.
		 * Not an error - it is the comparison doing its job - but a
		 * number that only climbs means a sender is looping on
		 * records nobody will take. */
		recv_older,
		/* S209: copies refused because a tombstone for the key carries
		 * a NEWER version than the copy: the set that the delete
		 * superseded, arriving after the tombstone because the push
		 * batches and the tombstone does not.  Each one of these is a
		 * resurrection that did not happen. */
		recv_tombstoned,
		/* S174: eager copies too large for a datagram travel on the
		 * bulk plane; this counts the BATCHES whose outcome did not
		 * confirm, which is what stops a sweep cycle advancing its
		 * mark past them */
		repl_bulk_lost;
	/* S73: write-path pushes sent (one per live peer per write), and the
	 * records the repair sweep left alone as dying within one sweep */
	unsigned long long repl_pushed, repl_skipped_dying;
	/* S83: the bootstrap pull - records this node streamed to a joiner,
	 * records this node stored from its own pull, pulls that failed */
	unsigned long long boot_out, boot_in, boot_failed;
	/* S105: write-path push datagrams sent (records per peer are
	 * repl_pushed; a group carries many) */
	unsigned long long repl_groups;
};
void pc_proxy_get_stats(struct pc_proxy_stats *out);

/* the rebalancer tick (peer thread, ~10s): donor-initiated, hysteresis
 * vs the fleet mean, byte-budgeted, stub-first */
void pc_rebalance_tick(void);

/* S125: the receive plane's figures are sampled inside this call, by the
 * thread that is answering the operator - never on a periodic tick.  Both
 * the cluster thread and the maintenance thread were measured stalling
 * under exactly the load the figures report; the client door is what
 * stays responsive.  The three per-second fields are therefore rates
 * SINCE THE LAST READ, floored at half a second. */
void pc_cluster_get_stats(struct pc_cl_stats *out);

/* S222: stop the whole fleet.  Tells every live peer to stop cleanly,
 * waits up to @timeout_ms for them to leave, fills @gone and @still
 * (node ids, at most @cap each), and schedules THIS node's own clean
 * stop to follow - it goes last, after it has reported.  Worker thread.
 * Returns the number of peers it was sent to, -1 if not clustered. */
int pc_cluster_fleet_stop(long long timeout_ms, int *gone, int *ngone,
		int *still, int *nstill, int cap);
int  pc_cluster_peers(struct pc_cl_peer_info *out, int max);

#endif /* PC_CLUSTER_H */
