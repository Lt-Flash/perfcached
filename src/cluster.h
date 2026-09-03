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

/* the state of THIS node, and its name for logs and stats */
int pc_node_state(void);
const char *pc_node_state_name(int st);
void pc_node_state_set(int st);

#define PC_ROLE_JOINING 0
#define PC_ROLE_MEMBER  1
#define PC_ROLE_MASTER  2

struct pc_cl_peer_info {
	int node;
	int up;
	int state;                     /* PC_NST_*, from its heartbeat */
	long long last_seen_ms;
	unsigned int free_mb;
};

struct pc_cl_stats {
	int enabled, node_id, peers_up;
	int role;                      /* PC_ROLE_* - runtime, elected */
	int state;                     /* PC_NST_* - runtime, lifecycle */
	int master_id;
	unsigned long long hb_sent, hb_seen, bad_auth;
	unsigned long long hb_watchdog;    /* beats the watchdog re-sent
	                                    * because the peer thread was
	                                    * heads-down past the overdue
	                                    * window - nonzero = a duty ran
	                                    * long enough to cost liveness
	                                    * before the fix existed */
	unsigned long long joins, assigns, elections, demotions;
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
	unsigned long long reconciled, reconcile_probed;
	int reconcile_active;
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
int pc_cluster_init(const char *mcast_addr, int mcast_port,
		const char *advertise, const uint8_t psk[PC_NOISE_KEYLEN],
		int pull_timeout_ms, int negative_ms, int tombstone_ms,
		const char *state_dir, int max_pending);

/* this node's own identity (hex, NUL-terminated) and incarnation */
const char *pc_cluster_identity(void);
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
		int authoritative, int client_port, int resp_port);
int pc_cluster_mode(void);
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
	int free_mb, total_mb;
	/* S49: this member's RESP door, 0 when it has none (or its build
	 * predates the field).  The CLUSTER family advertises THIS port,
	 * never client_port: the native door is Noise-only off-box, so a
	 * Redis client sent there cannot even say hello. */
	int resp_port;
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

/* fills @out with up to @max members (self first); returns the count */
int pc_cluster_members(struct pc_member *out, int max);

int pc_shard_owner(const char *col, size_t collen, const char *key,
		size_t klen);

/* the same decision, addressed by slot: what CLUSTER SLOTS enumerates */
int pc_shard_owner_slot(unsigned slot);

/* nonzero while a membership change (or a still-flowing reshard) is
 * recent: shard owner-misses fall back to ONE broadcast pull, because
 * the data may not have moved to its new owner yet */
int pc_shard_grace(void);

/* placement: power-of-two-choices on free-MB with the self band.
 * Returns 0 = place LOCALLY (self won), else the winning peer node. */
int pc_place(void);

/* a proxy-mode pull that may unicast to a known holder */
uint32_t pc_pull_begin_at(const char *col, size_t collen, const char *key,
		size_t klen, int holder_node);

/* forward a write to @node; completes on the worker like a pull.
 * op: 0 set (val/vlen/ttl), 1 del, 2 add (delta, ttl).  0 = failed to
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
 * timeout as ok=0.  @op: 0 set (val/vlen), 2 add (by).  0 = could not
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
		migrate_lost, migrate_dgrams, bulk_out, bulk_in, repl_out,
		demotes_sent, demotes_applied, loc_hits, loc_clears,
		/* A2: copies refused because ours was the same age or newer.
		 * Not an error - it is the comparison doing its job - but a
		 * number that only climbs means a sender is looping on
		 * records nobody will take. */
		recv_older;
};
void pc_proxy_get_stats(struct pc_proxy_stats *out);

/* the rebalancer tick (peer thread, ~10s): donor-initiated, hysteresis
 * vs the fleet mean, byte-budgeted, stub-first */
void pc_rebalance_tick(void);

void pc_cluster_get_stats(struct pc_cl_stats *out);
int  pc_cluster_peers(struct pc_cl_peer_info *out, int max);

#endif /* PC_CLUSTER_H */
