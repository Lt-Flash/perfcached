/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * cluster.c — the peer plane (tasks S16-S18).  See cluster.h.
 * Membership is AUTOMATIC (S16 as designed, the clusterer_controller
 * shape): multicast JOIN_REQ -> the master assigns node ids and returns
 * the member list; no master within the join window -> defer to a
 * higher-address simultaneous joiner, else found the cluster; 1/s ALIVE
 * keepalives carry free-MB (the placement feed); sticky master;
 * split-brain prevention (join defer) + cure (member-count rank).
 * The session-key/KEY_GRANT layer of the module is deliberately absent:
 * every datagram here is already sealed with the Argon2id cluster PSK.
 * Multicast is CONTROL ONLY - all data stays unicast.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <pthread.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sodium.h>

#include "compat/dprint.h"
#include "compat/pt.h"
#include "compat/timer.h"
#include "core/pcache_htable.h"
#include "core/pcache_arena.h"
#include "config.h"                    /* PC_MAX_COLLECTIONS */
#include "store.h"
#include "cluster.h"
#include "pc_slot.h"                  /* pc_key_slot(): placement input */
#include "pc_mix.h"                   /* pc_hrw_mix(): the rendezvous weight */
#include "daemon.h"                  /* pc_worker_id() */
#include "clmap.h"
#include "clterm.h"
#include "clplace.h"
#include "recover.h"
#include "clhist.h"
#include "clsync.h"
#include "core/pcache_mem.h"

#define HDR_LEN       14               /* magic+ver+nonce12 */
#define SHARD_GRACE_S  30              /* after a membership change (and
                                        * self-extending while reshard
                                        * migrations still flow), shard
                                        * owner-misses fall back to a
                                        * broadcast pull - data may not
                                        * have moved yet */
#define MAX_DGRAM     65000            /* sealed fits UDP's 65507; pull
                                        * answers up to ~64.9KB values -
                                        * the last sliver to cell max
                                        * travels only by bulk TCP */
/* Parked-request table.  Sized at startup from [cluster] max_pending
 * and never resized: pend_find() hands out RAW POINTERS into it and
 * about 117 uses hang off them, so a realloc under load would be a
 * use-after-free.
 *
 * The default was 1024 and that refused a single deeply-pipelined RESP
 * client: 50 connections at pipeline 64 is 3200 requests in flight, and
 * a client that cannot compute the owner forwards nearly all of them.
 * Measured, that peaked at 711 slots on a 16-core host and exhausted
 * 1024 outright on an 8-core one - every shard row at pipeline >= 16
 * was reporting the ceiling rather than the mode.  Raising it was only
 * affordable once pend_find() stopped being a walk of the whole table.
 *
 * PEND_LIMIT is a HARD ceiling, not a preference: the slot index rides
 * in the low PEND_SLOT_BITS of the request id. */
#define PEND_DEFAULT  8192
#define PEND_LIMIT    (1 << PEND_SLOT_BITS)
#define PEND_MIN      64
#define NEG_SLOTS     65536
#define WORKERS_MAX   520

#define M_PULL_REQ  2
#define M_PULL_RSP  3
#define M_TOMBSTONE 4
#define M_FWD_OP      5
#define M_FWD_ACK     6
#define M_MIGRATE     7
#define M_MIGRATE_ACK 8
#define M_DEMOTE      9
/* membership (control plane; 1 was the retired static-list heartbeat) */
#define M_JOIN_REQ      10   /* mcast (or ucast to master): [tok8]
                              * [mode1][k1][m1][eager1][digest8]
                              * [identity16][proposed_id2] - the proposal
                              * is derived from the identity so a node
                              * asks for the same id every start; the
                              * master still arbitrates collisions */
#define M_ASSIGN        11   /* ucast: [tok8][your_id2][master_id2][n2]
                              *        + n x [id2 ip4 port2 free4 total4 live4] */
#define M_MASTER_ALIVE  12   /* mcast 1/s: [id2][count2][digest8]
                              *            [free4][total4][live4] */
#define M_ALIVE         13   /* mcast 1/s: [id2][free4][total4][live4];
                              * addr = src.  free feeds placement
                              * (capacity-seeking); liveKB/total feeds
                              * the rebalancer (proportional per-mille
                              * leveling on live CELL bytes, never the
                              * chunk-sticky 'used') */
#define M_GOODBYE       14   /* mcast: [id2] */
#define M_MIGRATE_MANY  15   /* ucast: [req4][node2][count2] then per
                              * record [ttl4][cn1][klen2][vlen4][ver8]
                              * [col][key][val] - small records GATHERED
                              * to the datagram cap; ack echoes req + a
                              * stored-count.  ver is the version the
                              * SENDER's table committed (A2): it travels
                              * with the bytes, and the receiver refuses
                              * a copy older than the one it holds. */

#define M_FWD_JSON      16   /* ucast: [req4][node2][op1][flags1]
                              *        [ttl4][by8][cn1][klen2][plen2]
                              *        [vlen4][col][key][path][val] */
#define M_JOIN_REJ      19   /* ucast: [tok8][reason1] - the master
                              * refusing a join outright.  Reason 1 =
                              * duplicate identity: another LIVE member
                              * already claims it, which means this node
                              * is a CLONE of one, and two nodes sharing
                              * an identity break everything built on it
                              * (incarnation, backfill targeting). */
#define PC_REJ_DUP_IDENT 1
#define M_CLSYNC        21   /* ucast master -> backup: the control state a
                              * promoted standby needs and cannot rebuild.
                              * [maplen4][map][histlen4][hist].  The map
                              * carries the epoch and the node state
                              * table; the identity history is the part
                              * that CANNOT be reconstructed - only the
                              * master knows which identities this cluster
                              * has seen, so a standby without it would
                              * call every returning node new. */
#define M_CLSYNC_ACK    22   /* ucast backup -> master: [term4][seq4] -
                              * the epoch the standby now holds */

#define M_CLMAP         20   /* mcast: the master's cluster map, encoded by
                              * clmap.h.  ADDITIVE: the dispatch below
                              * has no default case, so a build that
                              * predates this ignores the datagram
                              * rather than mis-parsing it - which is
                              * why this needs no PC_CL_VER bump.
                              * Nothing consumes it yet; placement still
                              * runs through this file's own HRW. */

#define M_REPL_MANY     18   /* ucast: M_MIGRATE_MANY's exact payload,
                              * but the receiver stores PASSIVE and
                              * skips the WAL - an eager-store replica,
                              * not an ownership transfer.  Passive
                              * copies never re-propagate (no echo). */
#define M_FWD_JACK      17   /* ucast: [req4][st1][op1][newval8]
                              *        [count4][fraglen4][frag] */

#define JOIN_WAIT_MS    2000
#define JOIN_DEFER_MS   1000
#define JOIN_DEFER_MAX  4
#define MASTER_DEAD_MS  8000   /* 8 beats of margin at 1 Hz.  3000 was
                                * three: two legitimately stretched
                                * intervals (the 1 Hz block runs AFTER
                                * loop duties) plus receiver jitter
                                * crossed it under instrumentation with
                                * nobody mute and nobody deaf - a false
                                * promotion needs no fault at 3 beats.
                                * Redis Cluster ships 15000 here.  The
                                * data plane serves on the last map
                                * throughout, so slower failover costs
                                * only map-publication latency. */
#define CLMAP_PUB_MS    5000   /* map re-publication interval */
/* Probes per tick.  Deliberately modest: this competes with live
 * traffic, and the pass only has to finish before anyone notices a
 * resurrected key, not immediately. */
#define RECONCILE_PER_TICK 64
#define RECONCILE_BUCKETS  64  /* buckets per scan call */
/* how long a staged change waits for the standby's ack.  One unicast
 * round trip on a LAN; generous, because abandoning burns a sequence. */
#define CLSYNC_ACK_MS   1000
#define PEER_UP_MS      10000  /* 10 beats; was 5 - same margin
                                * arithmetic as MASTER_DEAD_MS.  A dead
                                * peer is noticed in 10s instead of 5;
                                * per-op paths never wait on this (pull
                                * timeouts are their own, much shorter
                                * clocks). */
#define PEER_PURGE_MS   6000

#define LOC_SLOTS   262144
#define LOC_WAYS    4
#define MAX_FWD_VAL 58000
/* Unicast receive buffer.  Sized for the worst burst the forward plane
 * can produce in one go - a fully pipelined client write burst of
 * MAX_FWD_VAL-sized values - because a dropped forward is a refused
 * write, not a retried one.  The kernel doubles this for accounting. */
#define PC_CLUSTER_RCVBUF (8 << 20)
#define SELF_BAND_PCT 25

struct peer {
	struct sockaddr_in addr;
	int node;                      /* learned from heartbeats, 0 unknown */
	long long last_seen_ms;
	unsigned int free_mb;
	unsigned int total_mb;
	int client_port;               /* S34: where CLIENTS dial this peer
	                                * (learned from its ALIVE) */
	int resp_port;                 /* S49: its RESP door, 0 if it has
	                                * none or its build predates this */
	int mem_tier;                  /* PCACHE_MEM_*, from its heartbeat */
	int nstate;                    /* B1: PC_NST_*, learned from its
	                                * ALIVE.  A peer that predates the
	                                * field sends a shorter datagram and
	                                * reads as READY, which is what it
	                                * behaved as. */
	unsigned int live_kb;          /* live cell KB: live/total is the
	                                * rebalancer's leveling metric (free
	                                * and used are chunk-granular and
	                                * peak-sticky until reclaim) */
	/* eager store: the budgeted sweep is RESUMABLE - a cursor per
	 * collection, plus the tick the current cycle started at and
	 * whether anything was lost during it.  Without the cursor a
	 * budget cut restarted at bucket 0 forever, so a keyspace larger
	 * than one budget never replicated past it (measured: exactly
	 * 4MiB, re-sent every tick, indefinitely). */
	unsigned int repl_cursor[PC_MAX_COLLECTIONS];
	unsigned int repl_cycle[PC_MAX_COLLECTIONS];
	unsigned char repl_dirty[PC_MAX_COLLECTIONS];
	/* eager store: per-collection replication high-water (a wtick).
	 * Records newer than the mark still need pushing to this peer.
	 * Zeroed when the slot is (re)claimed: a rejoin resyncs fully. */
	unsigned int repl_mark[PC_MAX_COLLECTIONS];
	/* Identity survives this peer's restarts; incarnation does not.
	 * A change in EITHER means its memory is gone and everything owed
	 * to it must be resent - which the node id cannot tell us, since
	 * a rejoiner keeps its id.  has_ident = 0 for an older build. */
	int has_ident;
	unsigned char ident[16];
	unsigned int incarn;
	/* set when this peer's incarnation changes: it came back empty, so
	 * it needs the PASSIVE copies too - once, not for ever */
	unsigned char backfill[PC_MAX_COLLECTIONS];
};

struct pending {
	uint32_t req;                  /* 0 = free slot; else a handle,
	                                * see PEND_TAG */
	uint32_t gen;                  /* bumped per alloc, rides in req so
	                                * a recycled slot is a NEW id */
	int worker;                    /* pc_worker_id() to deliver to */
	int expect;                    /* answers still awaited */
	int kind;                      /* PC_DONE_* */
	int answered;                  /* completion already posted */
	int first_node;                /* birth-race: the first positive */
	long long deadline_ms;
	char col[40];
	unsigned char collen;
	char key[256];
	unsigned short klen;           /* 0 = too big for demote tracking */
	unsigned char jop;             /* PC_DONE_JSON: the PC_JOP_* code */
	/* probe-before-place (proxy set/add, holder unknown): the deferred
	 * write, forwarded or replayed when the probe answers */
	unsigned char probe_op;        /* 0 none, 1 set, 2 add */
	unsigned char probe_fwd;       /* probe transformed into a forward:
	                                * the ack completes it; stray
	                                * positives go to the demoter */
	char *stash;                   /* set: the malloc'd value */
	int stash_len;
	long long by;
	unsigned int ttl_rel;
};

#define MIG_BATCH_CAP  ((4 << 20) + (64 << 10))
#define MIG_WINDOW     3           /* 3 x ~56KB fits a default rmem */
#define MIG_GATHER_CAP 56000       /* gathered group payload cap */
#define MIG_GHDR       9           /* type+req4+node2+count2 */
#define MIG_RHDR      19           /* per record: ttl4+cn1+klen2+vlen4+ver8 */
#define PULL_RHDR     24           /* type+req4+node2+found1+ttl4+vlen4+ver8 */

struct cqueue {
	pthread_mutex_t mx;
	/* pend_cap entries, malloc'd at worker registration.  It MUST
	 * match the parked-request table: every parked request could
	 * belong to one worker, and post_done_ex() silently DROPS a
	 * completion into a full queue - the request would then only
	 * resolve by timing out.  Sizing them together is what makes that
	 * unreachable. */
	struct pc_pull_done *q;
	int head, tail;
	int efd;
};

static struct {
	int enabled;
	int fd;
	int node_id;
	unsigned char self_ident[16];  /* persisted; stable across restarts */
	char self_ident_hex[33];
	unsigned int self_incarn;      /* fresh every process start */
	int ident_durable;             /* 0 = nothing to persist to */
	int proposed_id;               /* derived from the identity */
	struct { struct in_addr addr; long long first_ms; }
		dup_seen[PC_CL_MAXPEER];   /* joiners with a duplicate id */
	uint8_t psk[PC_NOISE_KEYLEN];
	struct peer peers[PC_CL_MAXPEER];
	int n_peers;
	int pull_timeout_ms, negative_ms, tombstone_ms;
	pthread_mutex_t pmx;           /* pending table */
	struct pending *pend;          /* pend_cap entries, allocated once */
	int pend_cap;                  /* runtime size; see PEND_DEFAULT */
	int pend_used;                 /* EXACT occupancy; see pend_release */
	uint32_t next_req;
	struct cqueue cq[WORKERS_MAX];
	struct { uint64_t h; long long exp_ms; } neg[NEG_SLOTS];
	struct { uint64_t h; uint16_t node; } loc[LOC_SLOTS];
	struct pc_cl_stats st;
	struct pc_proxy_stats px;
	/* the migration batch (S22 pacing fix): victims are stubbed and
	 * their sealed payloads COLLECTED during the scan, then sent
	 * through an ack-gated window - a raw burst overran the peer's UDP
	 * receive buffer and silently dropped transfers (caught by
	 * proxytest's conservation check: 8 of 149 keys lost on one run) */
	unsigned char *migb;
	size_t migb_len;
	int mig_inflight;
	size_t mig_open;               /* offset of the OPEN gather group's
	                                * len2 header, or (size_t)-1 */
	/* per-datagram record counts of the in-flight sends, so losses are
	 * accounted in RECORDS even when a gathered group goes missing */
	struct { uint32_t req; uint16_t recs; } mig_out[MIG_WINDOW * 2];
	/* automatic membership (peer thread writes, workers read node_id
	 * and peers[] - int/ptr-free fields, release/acquire on .node) */
	int mfd;                       /* multicast rx socket */
	int rcvbuf;                    /* EFFECTIVE unicast SO_RCVBUF (read
	                                * back: the kernel clamps silently) */
	/* S30: the interchange-relevant config this node runs, and who we
	 * have already shouted about (once per peer, not once per second) */
	int cfg_mode, cfg_eager, cfg_authoritative;
	long long pend_log_ms;         /* rate limit for the FULL shout */
	int cfg_client_port;           /* S34: where CLIENTS reach this node
	                                * (the peer plane speaks its own
	                                * port; a client needs the listener) */
	int cfg_resp_port;             /* S49: this node's RESP door.  The
	                                * CLUSTER family must advertise RESP
	                                * ports - the native door is Noise
	                                * off-box, so pointing a Redis
	                                * client at it strands the client. */
	struct in_addr cfg_shouted[PC_CL_MAXPEER];
	unsigned long long cfg_refused;
	struct sockaddr_in mcast_dst;  /* group:port */
	struct sockaddr_in self_addr;  /* advertised unicast addr */
	unsigned int last_change_tick; /* membership/reshard activity - a
	                                * 32-bit tick so worker reads are
	                                * tear-free on every arch (the
	                                * ILP32 rule: no cross-thread u64) */
	int role;                      /* PC_ROLE_* */
	int nstate;                    /* PC_NST_* - the lifecycle axis */
	/* the map this node currently believes, and whether it has one.
	 * A master builds its own; everyone else adopts what arrives, and
	 * only ever forwards - see clmap_adopt(). */
	struct pc_clmap map;
	int map_valid;
	long long map_pub_ms;          /* last publication, master side */
	unsigned long long map_pub_n, map_rx_n, map_stale_n, map_bad_n;
	/* the map may be used for placement: its placeable set matches
	 * what this node currently sees as live.  Cached rather than
	 * recomputed per lookup - placement is on the request path. */
	int map_usable;
	/* B4: a reconcile pass is running, with its resumable position */
	/* The master's record of which identities this cluster has seen.
	 * It is what separates a NEW node from a RETURNING one, and it is
	 * not a judgement a node can make about itself - one whose state
	 * directory was wiped sincerely believes it is new. */
	struct pc_clhist hist;
	int backup_id;                 /* the master's chosen standby, 0 = none */
	struct pc_clsync sync;         /* master side: stage -> ack -> publish */
	struct pc_clmap staged;        /* built and staged, not yet published */
	long long staged_deadline_ms;
	unsigned long long stage_timeout_n;
	/* backup side: control state held ON BEHALF of a master, and the
	 * epoch it covers.  Held, never adopted - the map this node places
	 * from is whatever arrived on the broadcast. */
	struct pc_clhist held_hist;
	struct pc_clmap held_map;      /* so a promotion can republish it */
	int held_valid;
	uint32_t held_term, held_seq;
	unsigned long long sync_sent_n, sync_rx_n, sync_ack_n, sync_bad_n;
	int reconcile_on;
	int rec_col;
	unsigned int rec_cursor;
	unsigned long long place_map_n, place_hrw_n;
	int master_id;
	struct sockaddr_in master_addr;
	long long master_seen_ms;
	uint64_t join_tok;
	long long join_deadline_ms, join_last_ms, higher_joiner_ms;
	int join_defers;
	/* the bulk TCP plane (records above the datagram ceiling): the
	 * peer thread's tick fills a batch + target, the bulk thread owns
	 * every socket */
	int blfd;                      /* TCP listener on advertise:port */
	pthread_mutex_t bmx;
	unsigned char *bulkb;          /* tick-side collect buffer */
	size_t bulkb_len;
	unsigned int bulk_recs;
	unsigned char *bulk_tx;        /* handed to the bulk thread */
	size_t bulk_tx_len;
	unsigned int bulk_tx_recs;
	struct sockaddr_in bulk_tx_to;
} C;


static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- sealed datagrams -------------------------------------------------- */

static int seal_send(const struct sockaddr_in *to, const unsigned char *pt,
		size_t n)
{
	unsigned char buf[HDR_LEN + MAX_DGRAM + 16];
	unsigned long long clen = 0;

	if (n > MAX_DGRAM)
		return -1;
	buf[0] = PC_CL_MAGIC;
	buf[1] = PC_CL_VER;
	randombytes_buf(buf + 2, 12);
	crypto_aead_chacha20poly1305_ietf_encrypt(buf + HDR_LEN, &clen,
		pt, n, buf, HDR_LEN, NULL, buf + 2, C.psk);
	return sendto(C.fd, buf, HDR_LEN + (size_t)clen, 0,
		(const struct sockaddr *)to, sizeof *to) < 0 ? -1 : 0;
}

static int open_dgram(unsigned char *buf, size_t n, unsigned char *pt,
		unsigned long long *ptlen)
{
	if (n < HDR_LEN + 16 || buf[0] != PC_CL_MAGIC || buf[1] != PC_CL_VER)
		return -1;
	if (crypto_aead_chacha20poly1305_ietf_decrypt(pt, ptlen, NULL,
	        buf + HDR_LEN, n - HDR_LEN, buf, HDR_LEN, buf + 2,
	        C.psk) != 0)
		return -1;
	return 0;
}

/* ---- the heartbeat watchdog (the donor-starvation fix) ------------------
 * PEER_UP_MS reads silence as death, and the peer thread's long duties
 * used to leave the node mute for up to the SAME 5 seconds: a migration
 * tick's ack window polls to a 5000ms deadline and a big victim walk
 * was unbounded, so a busy donor was evicted as dead MID-MIGRATION,
 * rejoined as a fresh identity, and the fleet churned through
 * elections (ASan's 2-3x slowdown exposed it; natively the margin was
 * zero, not merely thin).  Two layers, both EMISSION-ONLY - membership
 * decisions stay in membership_tick on the peer thread:
 *   - beat_pump(): called from inside the known long duties on the
 *     peer thread itself, builds and sends real current frames;
 *   - pc_cluster_beat_thread(): a watchdog for stalls nobody foresaw.
 *     When nothing has been emitted for BEAT_OVERDUE_MS it re-seals
 *     the LAST frames the peer thread built (free-MB and lamport up to
 *     ~2s stale - staleness this plane tolerates by design; every
 *     seal_send draws a fresh random nonce, so the re-send is a new
 *     sealed datagram, not a wire replay).  The frames cross threads
 *     under a seqlock; the sends share the UDP socket, which is safe -
 *     datagram sendto is atomic, C.psk and C.mcast_dst are immutable
 *     after init. */
#define BEAT_OVERDUE_MS 1600

static long long beat_sent_ms;         /* last emission, either layer */
static unsigned beat_seq;              /* seqlock over the two frames */
static unsigned char beat_alive[64];
static size_t beat_alive_len;
static unsigned char beat_malive[40];
static size_t beat_malive_len;

/* peer thread only (single writer): publish a frame for the watchdog */
static void beat_store(unsigned char *dst, size_t *dlen,
		const unsigned char *src, size_t n)
{
	unsigned s = __atomic_load_n(&beat_seq, __ATOMIC_RELAXED);

	__atomic_store_n(&beat_seq, s + 1, __ATOMIC_RELEASE);
	if (n)
		memcpy(dst, src, n);
	*dlen = n;
	__atomic_store_n(&beat_seq, s + 2, __ATOMIC_RELEASE);
}

void pc_cluster_beat_thread(volatile int *stop)
{
	while (!*stop) {
		struct timespec ts = { 0, 250L * 1000000 };
		unsigned char a[sizeof beat_alive], ma[sizeof beat_malive];
		size_t alen, mlen;
		unsigned s0;

		nanosleep(&ts, NULL);
		if (now_ms() - __atomic_load_n(&beat_sent_ms, __ATOMIC_ACQUIRE)
		        < BEAT_OVERDUE_MS)
			continue;
		for (;;) {
			s0 = __atomic_load_n(&beat_seq, __ATOMIC_ACQUIRE);
			if (s0 & 1)
				continue;
			alen = beat_alive_len;
			mlen = beat_malive_len;
			if (alen)
				memcpy(a, beat_alive, alen);
			if (mlen)
				memcpy(ma, beat_malive, mlen);
			if (__atomic_load_n(&beat_seq, __ATOMIC_ACQUIRE) == s0)
				break;
		}
		if (!alen)
			continue;              /* nothing built yet (joining) */
		seal_send(&C.mcast_dst, a, alen);
		if (mlen)
			seal_send(&C.mcast_dst, ma, mlen);
		C.st.hb_watchdog++;
		/* say so, rate-limited: a silent watchdog is the ONE
		 * discriminator between "the peer thread starved its beats"
		 * (these lines appear) and "the beats flowed but a peer went
		 * deaf" (they do not) */
		{
			static long long logged_ms;
			long long nl = now_ms();

			if (nl - logged_ms >= 10000) {
				logged_ms = nl;
				LM_NOTICE("cluster: heartbeat watchdog "
					"covering for a stalled peer thread "
					"(%llu total)\n",
					(unsigned long long)C.st.hb_watchdog);
			}
		}
		__atomic_store_n(&beat_sent_ms, now_ms(), __ATOMIC_RELEASE);
	}
}

/* ---- the node's lifecycle state (B1) ------------------------------------ */

/* The heartbeat carries this byte and the map encodes the same values,
 * so the two definitions cannot be allowed to drift: a node reporting
 * "ready" as 2 to a peer that reads 2 as "reconciling" would be told to
 * hold traffic it should be serving. */
_Static_assert(PC_NST_STARTING == PC_CLMAP_ST_STARTING &&
               PC_NST_RECOVERING == PC_CLMAP_ST_RECOVERING &&
               PC_NST_READY == PC_CLMAP_ST_READY &&
               PC_NST_DRAINING == PC_CLMAP_ST_DRAINING,
               "node state values must match the map's wire encoding");


const char *pc_node_state_name(int st)
{
	switch (st) {
	case PC_NST_STARTING:    return "starting";
	case PC_NST_RECOVERING:  return "recovering";
	case PC_NST_READY:       return "ready";
	case PC_NST_DRAINING:    return "draining";
	default:
		break;
	}
	return "?";
}

int pc_node_state(void)
{
	return __atomic_load_n(&C.nstate, __ATOMIC_RELAXED);
}

/* One writer for the whole machine, so every transition is logged in the
 * same shape and a state can never be set from two places that disagree
 * about what it means.  DRAINING is terminal: a goodbye has gone out and
 * peers are purging us, so a late ASSIGN must not walk it back to READY. */
void pc_node_state_set(int st)
{
	int cur = __atomic_load_n(&C.nstate, __ATOMIC_RELAXED);

	if (cur == st || cur == PC_NST_DRAINING)
		return;
	__atomic_store_n(&C.nstate, st, __ATOMIC_RELAXED);
	LM_NOTICE("cluster: node state %s -> %s\n", pc_node_state_name(cur),
		pc_node_state_name(st));
}

/* ---- LE helpers -------------------------------------------------------- */

static void p16(unsigned char *p, uint16_t v) { p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8); }
static void p32(unsigned char *p, uint32_t v) { p16(p, (uint16_t)v);
	p16(p + 2, (uint16_t)(v >> 16)); }
static uint16_t g16(const unsigned char *p) { return (uint16_t)(p[0] |
	(p[1] << 8)); }
static uint32_t g32(const unsigned char *p) { return (uint32_t)g16(p) |
	((uint32_t)g16(p + 2) << 16); }
static void p64(unsigned char *p, uint64_t v) { p32(p, (uint32_t)v);
	p32(p + 4, (uint32_t)(v >> 32)); }
static uint64_t g64(const unsigned char *p) { return (uint64_t)g32(p) |
	((uint64_t)g32(p + 4) << 32); }

/* ---- negative cache ---------------------------------------------------- */

static uint64_t neg_hash(const char *col, size_t cn, const char *key,
		size_t kn)
{
	uint64_t h = 1469598103934665603ull;
	size_t i;

	for (i = 0; i < cn; i++)
		h = (h ^ (unsigned char)col[i]) * 1099511628211ull;
	h = (h ^ 0x2f) * 1099511628211ull;
	for (i = 0; i < kn; i++)
		h = (h ^ (unsigned char)key[i]) * 1099511628211ull;
	return h ? h : 1;
}

int pc_neg_hit(const char *col, size_t cn, const char *key, size_t kn)
{
	uint64_t h = neg_hash(col, cn, key, kn);
	size_t i = h % NEG_SLOTS;

	if (C.neg[i].h == h && C.neg[i].exp_ms > now_ms()) {
		__atomic_fetch_add(&C.st.neg_hits, 1, __ATOMIC_RELAXED);
		return 1;
	}
	return 0;
}

void pc_neg_set(const char *col, size_t cn, const char *key, size_t kn,
		int ms)
{
	uint64_t h = neg_hash(col, cn, key, kn);
	size_t i = h % NEG_SLOTS;

	C.neg[i].h = h;
	C.neg[i].exp_ms = now_ms() + ms;
}

void pc_neg_clear(const char *col, size_t cn, const char *key, size_t kn)
{
	uint64_t h = neg_hash(col, cn, key, kn);
	size_t i = h % NEG_SLOTS;

	if (C.neg[i].h == h)
		C.neg[i].h = 0;
}

/* ---- the locator cache -------------------------------------------------- */

/* 4-way associative: direct-mapped slots evicted ~30% of a 200k
 * keyspace by collision, and every evicted entry sent a re-write back
 * through placement (the fork the probe now guards).  Same memory,
 * sets of LOC_WAYS; the victim way is a second hash bit-slice. */
static size_t loc_base(uint64_t h)
{
	return (size_t)(h % (LOC_SLOTS / LOC_WAYS)) * LOC_WAYS;
}

int pc_loc_get(const char *col, size_t cn, const char *key, size_t kn)
{
	uint64_t h = neg_hash(col, cn, key, kn);
	size_t b = loc_base(h), w;

	for (w = 0; w < LOC_WAYS; w++)
		if (C.loc[b + w].h == h && C.loc[b + w].node) {
			__atomic_fetch_add(&C.px.loc_hits, 1, __ATOMIC_RELAXED);
			return C.loc[b + w].node;
		}
	return 0;
}

void pc_loc_set(const char *col, size_t cn, const char *key, size_t kn,
		int node)
{
	uint64_t h = neg_hash(col, cn, key, kn);
	size_t b = loc_base(h), w;

	for (w = 0; w < LOC_WAYS; w++)
		if (C.loc[b + w].h == h) {
			C.loc[b + w].node = (uint16_t)node;
			return;
		}
	for (w = 0; w < LOC_WAYS; w++)
		if (!C.loc[b + w].node) {
			C.loc[b + w].h = h;
			C.loc[b + w].node = (uint16_t)node;
			return;
		}
	w = (size_t)((h >> 32) & (LOC_WAYS - 1));
	C.loc[b + w].h = h;
	C.loc[b + w].node = (uint16_t)node;
}

void pc_loc_clear(const char *col, size_t cn, const char *key, size_t kn)
{
	uint64_t h = neg_hash(col, cn, key, kn);
	size_t b = loc_base(h), w;

	for (w = 0; w < LOC_WAYS; w++)
		if (C.loc[b + w].h == h) {
			C.loc[b + w].node = 0;
			__atomic_fetch_add(&C.px.loc_clears, 1, __ATOMIC_RELAXED);
			return;
		}
}

/* ---- shard mode: deterministic ownership (rendezvous hashing) ----------
 * HRW over the members' STABLE advertise addresses - runtime node ids
 * churn on rejoin and must not decide placement.  Minimal disruption
 * on membership change: only keys whose argmax member changed move.
 * v1 is unweighted (assumes comparable arenas; weighted rendezvous is
 * the recorded follow-up). */

/* ---- S30: the cluster's config identity -------------------------------
 * Every ALIVE carries a digest of the INTERCHANGE-RELEVANT config - the
 * mode, its parameters and the clustered collection SET - so two nodes
 * that disagree about what a collection IS can never quietly exchange
 * data.  Node-local sizing (buckets_log2, arena) is deliberately NOT in
 * it: members may tune those freely.
 *
 * Demonstrated before this existed: node A running collection 'c' as
 * store and node B running it as shard joined the same cluster happily,
 * writes through either vanished for the other, and NOTHING was logged.
 */
static uint64_t config_digest(void)
{
	uint64_t h = 0xcbf29ce484222325ull;   /* FNV-1a */
	int i, j, n = pc_store_count();
	const char *names[PC_MAX_COLLECTIONS];

	#define DIG(b) do { h ^= (unsigned char)(b); \
		h *= 0x100000001b3ull; } while (0)

	DIG(C.cfg_mode);
	DIG(C.cfg_eager);
	/* PLACEMENT is interchange-relevant, and more so than anything
	 * else here: two nodes that hash keys differently agree about
	 * every collection and still put the same key in two places.
	 * Folding the contract string in means the S30 refusal covers a
	 * build whose placement changed - which it now has, twice, if you
	 * count the move from hrw-fnv1a-v1 to hrw-slot16k-v1. */
	{
		const char *q;

		for (q = PC_ROUTE_ALGO; *q; q++)
			DIG(*q);
		DIG(0);
	}
	/* the SET, order-independent: sort the names before folding */
	if (n > PC_MAX_COLLECTIONS)
		n = PC_MAX_COLLECTIONS;
	for (i = 0; i < n; i++)
		names[i] = pc_store_name(i);
	for (i = 1; i < n; i++) {
		const char *tmp = names[i];

		for (j = i; j > 0 && strcmp(names[j - 1], tmp) > 0; j--)
			names[j] = names[j - 1];
		names[j] = tmp;
	}
	for (i = 0; i < n; i++) {
		const char *q;

		for (q = names[i]; *q; q++)
			DIG(*q);
		DIG(0);
	}
	#undef DIG
	return h;
}

void pc_cluster_set_config(int mode, int eager,
		int authoritative, int client_port, int resp_port)
{
	C.cfg_mode = mode;
	C.cfg_eager = eager;
	C.cfg_authoritative = authoritative;
	C.cfg_client_port = client_port;
	C.cfg_resp_port = resp_port;
}

int pc_cluster_mode(void)
{
	return C.cfg_mode;
}

int pc_cluster_port(void)
{
	return ntohs(C.mcast_dst.sin_port);
}

int pc_cluster_authoritative(void)
{
	return C.cfg_authoritative;
}

static unsigned int self_free_mb(void);
static unsigned int self_total_mb(void);

/* S34: the fleet as a CLIENT needs to see it - address, client port,
 * id, role and load per member, self included.  Returns the count. */
int pc_cluster_members(struct pc_member *out, int max)
{
	int i, n = 0;

	if (!C.enabled || max < 1)
		return 0;
	out[n].addr = C.self_addr.sin_addr;
	out[n].client_port = C.cfg_client_port;
	out[n].resp_port = C.cfg_resp_port;
	out[n].node = C.node_id;
	out[n].is_self = 1;
	out[n].is_master = C.role == PC_ROLE_MASTER;
	out[n].is_backup = C.map_valid &&
		C.map.backup_id == (uint16_t)C.node_id;
	out[n].mem_tier = (int)pcache_mem.tier;
	out[n].state = pc_node_state();
	out[n].free_mb = (int)self_free_mb();
	out[n].total_mb = (int)self_total_mb();
	out[n].has_ident = 1;
	memcpy(out[n].ident, C.self_ident, 16);
	out[n].incarn = C.self_incarn;
	n++;
	for (i = 0; i < C.n_peers && n < max; i++) {
		struct peer *p = &C.peers[i];

		if (!__atomic_load_n(&p->node, __ATOMIC_ACQUIRE))
			continue;
		if (!p->client_port)
			continue;      /* older peer: cannot be dialled */
		out[n].addr = p->addr.sin_addr;
		out[n].client_port = p->client_port;
		out[n].resp_port = p->resp_port;
		out[n].node = p->node;
		out[n].is_self = 0;
		out[n].is_master = p->node == C.master_id;
		out[n].is_backup = C.map_valid &&
			C.map.backup_id == (uint16_t)p->node;
		out[n].state = p->nstate;
		out[n].mem_tier = p->mem_tier;
		out[n].free_mb = (int)p->free_mb;
		out[n].total_mb = (int)p->total_mb;
		out[n].has_ident = p->has_ident;
		memcpy(out[n].ident, p->ident, 16);
		out[n].incarn = p->incarn;
		n++;
	}
	return n;
}

/* Loud, but once per peer address: a mismatching node retries its ALIVE
 * every second and must not turn the log into a flood. */
static void shout_mismatch(const struct sockaddr_in *from, int their_mode,
		int their_eager, uint64_t theirs)
{
	int i, slot = -1;

	for (i = 0; i < PC_CL_MAXPEER; i++) {
		if (C.cfg_shouted[i].s_addr == from->sin_addr.s_addr)
			return;
		if (slot < 0 && !C.cfg_shouted[i].s_addr)
			slot = i;
	}
	if (slot >= 0)
		C.cfg_shouted[slot] = from->sin_addr;
	LM_ERR("cluster: REFUSING peer %s - its interchange config differs "
		"from ours.  It runs mode=%s eager=%d (digest "
		"%016llx); we run mode=%s eager=%d placement=%s (digest "
		"%016llx).  A cluster is ONE mode over ONE collection set "
		"with ONE placement function - members that disagree would "
		"lose data silently, so this peer is not joined.  Fix "
		"[cluster] mode/collections to match, upgrade the peer if "
		"its build places keys differently, or run it on its own "
		"multicast group.%s\n",
		inet_ntoa(from->sin_addr),
		their_mode == PC_MODE_PROXY ? "proxy" :
		their_mode == PC_MODE_SHARD ? "shard" : "store",
		their_eager, (unsigned long long)theirs,
		C.cfg_mode == PC_MODE_PROXY ? "proxy" :
		C.cfg_mode == PC_MODE_SHARD ? "shard" : "store",
		C.cfg_eager, PC_ROUTE_ALGO,
		(unsigned long long)config_digest(),
		C.cfg_authoritative ? "" :
		"  (neither side declares [cluster] collections - without it "
		"nothing can be checked beyond this digest)");
}

int pc_cluster_rcvbuf(void)
{
	return C.rcvbuf;
}

/*
 * Ownership.  Returns 0 when THIS node owns the key and a node id
 * otherwise - restated here because the map's placement function returns
 * a real id for every node including self, and handing that back
 * unchanged would tell this node it owns nothing.
 *
 * The map decides only once it has caught up with what this node sees as
 * live (clmap_recheck_usable).  Until then the fleet's own liveness
 * does, which is what it did before there was a map - and the two agree
 * whenever their node sets do, because uniform weights make weighted
 * rendezvous the unweighted argmax.
 */
/* Placement hashes the SLOT, not the key.
 *
 * A RESP client computes crc16(key) % 16384 knowing nothing about our
 * collections, so the slot is the only value it and we can both derive
 * - and both reaching the same owner is the entire point of S44.  The
 * rendezvous function underneath is unchanged: it still mixes the
 * value with each node's stable address, still compares in integers,
 * still moves minimal data when the fleet changes.  Only what goes in
 * has changed.
 *
 * Two consequences, both intended:
 *   - the collection no longer affects placement, so the same key in
 *     two collections shares an owner.  Redis has no collections, so
 *     any scheme a Redis client can compute must ignore ours;
 *   - granularity is 1-in-16384 rather than per-key.  At three nodes
 *     that is ~1.4% imbalance; see the §9.1 note before running very
 *     large fleets or very uneven weights.
 *
 * This entry point takes the SLOT so that CLUSTER SLOTS can walk the
 * slot space directly; pc_shard_owner() below is this function after
 * hashing the key. */
int pc_shard_owner_slot(unsigned slot)
{
	uint64_t kh = slot, best, h;
	long long now = now_ms();
	int i, owner = 0;

	if (!C.enabled)
		return 0;
	if (C.map_usable) {
		uint16_t o = pc_clplace_owner(&C.map, kh);

		if (o) {
			C.place_map_n++;
			return o == (uint16_t)C.node_id ? 0 : (int)o;
		}
	}
	C.place_hrw_n++;
	best = pc_hrw_mix(kh, C.self_addr.sin_addr.s_addr,
		C.self_addr.sin_port);
	for (i = 0; i < C.n_peers; i++) {
		if (!C.peers[i].node ||
		        now - C.peers[i].last_seen_ms >= PEER_UP_MS)
			continue;
		h = pc_hrw_mix(kh, C.peers[i].addr.sin_addr.s_addr,
			C.peers[i].addr.sin_port);
		if (h > best) {
			best = h;
			owner = C.peers[i].node;
		}
	}
	return owner;
}

int pc_shard_owner(const char *col, size_t cn, const char *key, size_t kn)
{
	(void)col; (void)cn;           /* placement is collection-blind now */
	return pc_shard_owner_slot(pc_key_slot(key, kn));
}

int pc_shard_grace(void)
{
	return C.enabled &&
		get_ticks() - C.last_change_tick < SHARD_GRACE_S;
}

static void shard_note_change(void)
{
	C.last_change_tick = get_ticks();
}

/* ---- forward declarations ----------------------------------------------- */

/* defined below with the pending table / peer plane */
static uint32_t pend_alloc(int kind, int expect, int extra_ms,
		struct pending **out);
static struct pending *pend_find(uint32_t req);
static struct peer *peer_by_node(int node);
/* the WAL entry points this file uses; declared here rather than
 * including wal.h, matching the existing local externs below */
extern void pc_wal_upsert(const char *col, const char *key, int klen,
		const char *val, int vlen, unsigned int expires,
		unsigned long long ver);

static size_t build_fwd_op(unsigned char *msg, uint32_t req, int op,
		unsigned int ttl_rel, long long delta, const char *col,
		size_t collen, const char *key, size_t klen, const char *val,
		size_t vlen);
static size_t build_pull_req(unsigned char *msg, uint32_t req,
		const char *col, size_t collen, const char *key, size_t klen);
static void post_done(int worker, uint32_t req, int found,
		unsigned int ttl_left, const unsigned char *val, int vlen);

/* ---- placement (power-of-two on free-MB, self-preference band) ---------- */

static unsigned int self_free_mb(void)
{
	unsigned long total = 0, used = 0, freeb = 0;
	int active = 0;

	pcache_arena_hugepage_capacity(&active, &total, &used, &freeb);
	return (unsigned int)(freeb >> 20);
}

static unsigned int self_total_mb(void)
{
	unsigned long total = 0, used = 0, freeb = 0;
	int active = 0;

	pcache_arena_hugepage_capacity(&active, &total, &used, &freeb);
	return (unsigned int)(total >> 20);
}

/* How many records this node holds, across every collection.  live_kb
 * cannot answer "is it empty": it is KILOBYTES, so a node that recovered
 * a handful of small records still reports 0 and would be mistaken for a
 * node that came back with nothing - which is exactly the case the
 * backfill gate has to tell apart. */
static unsigned int self_entries(void)
{
	unsigned long long n = 0;
	int i;

	for (i = 0; i < pc_store_count(); i++) {
		pcache_ht_totals_t t;

		pcache_ht_totals(pc_store_ht(i), &t);
		n += t.entries;
	}
	return n > 0xffffffffULL ? 0xffffffffU : (unsigned int)n;
}

static unsigned int self_live_kb(void)
{
	return (unsigned int)(pcache_arena_live_bytes() >> 10);
}

/* live-data PER-MILLE of the arena - the proportional leveling metric.
 * Per-mille on KB, not percent on MB: stacked integer floors at
 * percent/MB granularity ate a whole rig-scale imbalance (measured:
 * 6MB on a 64MB donor truncated to exactly the band edge, myutil 7 vs
 * mean+5 = 7 - and nothing ever moved). */
static unsigned int upml(unsigned int live_kb, unsigned int total_mb)
{
	unsigned long long t = (unsigned long long)total_mb << 10;

	if (!t)
		return 0;
	if (live_kb > t)
		return 1000;
	return (unsigned int)((unsigned long long)live_kb * 1000 / t);
}

static struct peer *peer_by_node(int node)
{
	int i;

	for (i = 0; i < C.n_peers; i++)
		if (C.peers[i].node == node &&
		        now_ms() - C.peers[i].last_seen_ms < PEER_UP_MS)
			return &C.peers[i];
	return NULL;
}

int pc_place(void)
{
	struct peer *cand[PC_CL_MAXPEER];
	unsigned int myfree = self_free_mb(), best_free;
	int i, n = 0, a, b, best;
	static unsigned int rr;

	for (i = 0; i < C.n_peers; i++)
		if (C.peers[i].node && now_ms() - C.peers[i].last_seen_ms < PEER_UP_MS)
			cand[n++] = &C.peers[i];
	if (!n) {
		C.px.placed_local++;
		return 0;
	}
	/* two samples (round-robin walk keeps it cheap and fair) */
	a = (int)(rr++ % (unsigned int)n);
	b = (int)(rr++ % (unsigned int)n);
	best = cand[a]->free_mb >= cand[b]->free_mb ? a : b;
	best_free = cand[best]->free_mb;
	/* the self-preference band: keep local unless a peer is
	 * MEANINGFULLY freer - symmetric load stays put */
	if (myfree * 100 >= best_free * (100 - SELF_BAND_PCT) ||
	        myfree >= best_free) {
		C.px.placed_local++;
		return 0;
	}
	C.px.placed_remote++;
	return cand[best]->node;
}

/* ---- completion queues -------------------------------------------------- */

void pc_cluster_worker_register(int worker, int efd)
{
	if (worker < 0 || worker >= WORKERS_MAX)
		return;
	if (!C.cq[worker].q)
		C.cq[worker].q = calloc((size_t)C.pend_cap,
			sizeof *C.cq[worker].q);
	C.cq[worker].efd = efd;
}

static void post_done_ex(int worker, uint32_t req, int kind, int found,
		int ok, long long newval, int from_node, unsigned int ttl_left,
		const unsigned char *val, int vlen, int jop, int count,
		unsigned long long ver, int timedout)
{
	struct cqueue *q;
	uint64_t one = 1;

	if (worker < 0 || worker >= WORKERS_MAX)
		return;
	q = &C.cq[worker];
	pthread_mutex_lock(&q->mx);
	if (q->q && q->tail - q->head < C.pend_cap) {
		struct pc_pull_done *d = &q->q[q->tail % C.pend_cap];

		d->req = req;
		d->kind = kind;
		d->timedout = timedout;
		d->found = found;
		d->ok = ok;
		d->newval = newval;
		d->from_node = from_node;
		d->ttl_left = ttl_left;
		d->ver = ver;
		d->vlen = vlen;
		d->jop = jop;
		d->count = count;
		d->val = NULL;
		if (vlen > 0 && val) {
			d->val = malloc((size_t)vlen);
			if (d->val)
				memcpy(d->val, val, (size_t)vlen);
			else
				d->found = 0;
		}
		q->tail++;
	}
	pthread_mutex_unlock(&q->mx);
	if (q->efd > 0)
		if (write(q->efd, &one, sizeof one) < 0) { /* best effort */ }
}

static void post_done(int worker, uint32_t req, int found,
		unsigned int ttl_left, const unsigned char *val, int vlen)
{
	post_done_ex(worker, req, PC_DONE_PULL, found, found, 0, 0,
		ttl_left, val, vlen, 0, 0, 0, 0);
}

int pc_cluster_drain(int worker, struct pc_pull_done *out, int max)
{
	struct cqueue *q;
	int n = 0;

	if (worker < 0 || worker >= WORKERS_MAX)
		return 0;
	q = &C.cq[worker];
	pthread_mutex_lock(&q->mx);
	while (q->head != q->tail && n < max) {
		out[n++] = q->q[q->head % C.pend_cap];
		q->head++;
	}
	pthread_mutex_unlock(&q->mx);
	return n;
}

/* ---- pending table ------------------------------------------------------ */

/* The parked-request id is a HANDLE, not a ticket: bit 31 marks it as
 * one, the low PEND_SLOT_BITS index the slot, and the bits between are
 * a per-slot generation, so a REUSED slot yields a different id and a
 * reply that arrives after its slot was recycled fails the compare
 * instead of completing somebody else's request.
 *
 * pend_find() runs once per reply and used to walk the whole table.
 * That is affordable at 1024 slots and is exactly what stops the table
 * growing: raising MAX_PENDING to relieve the backpressure would have
 * made every reply proportionally more expensive, so the ceiling could
 * not be lifted without this first.  S38.
 *
 * Migration group ids (C.mig_out) are drawn from the same counter but
 * are matched in their own table under M_MIGRATE_ACK, so the two id
 * spaces never meet in a lookup.  They are masked to 31 bits anyway, so
 * PEND_TAG makes the two provably disjoint rather than merely
 * unreachable. */
#define PEND_TAG        0x80000000u
#define PEND_SLOT_BITS  16
#define PEND_SLOT_MASK  ((1u << PEND_SLOT_BITS) - 1)
#define PEND_GEN_SHIFT  PEND_SLOT_BITS
#define PEND_GEN_MASK   (PEND_TAG - 1 - PEND_SLOT_MASK)

/* Free a parked slot.  The ONLY way one is released, so that occupancy
 * is exact: nine sites used to write `req = 0` by hand, and a count
 * maintained across nine sites is a count that drifts.  Idempotent, so
 * releasing an already-free slot cannot double-count - one caller does
 * exactly that ("answered earlier: just free").
 *
 * C.pmx must be held. */
static void pend_release(struct pending *p)
{
	if (!p->req)
		return;
	p->req = 0;
	if (C.pend_used > 0)
		C.pend_used--;
}

static struct pending *pend_find(uint32_t req)
{
	uint32_t i;

	/* not one of ours: a migration group id, or a peer echoing
	 * something we never issued */
	if (!(req & PEND_TAG))
		return NULL;
	i = req & PEND_SLOT_MASK;
	if (i >= (uint32_t)C.pend_cap)
		return NULL;
	return C.pend[i].req == req ? &C.pend[i] : NULL;
}

/* set beside every 0 return from a begin(); see PC_CLFAIL_* */
static __thread int cl_fail;

int pc_cluster_last_fail(void)
{
	return cl_fail;
}

int pc_cluster_pend_cap(void)
{
	return C.pend_cap;
}

static uint32_t pend_alloc(int kind, int expect, int extra_ms,
		struct pending **out)
{
	struct pending *slot = NULL;
	uint32_t req, gen;
	int i;

	pthread_mutex_lock(&C.pmx);
	for (i = 0; i < C.pend_cap; i++)
		if (C.pend[i].req == 0) {
			slot = &C.pend[i];
			break;
		}
	if (!slot) {
		unsigned long long n = ++C.st.pend_exhausted;
		long long now = now_ms();

		/* Loud, but not once per refusal: this fires under exactly
		 * the load that caused it, so a line per event would be a
		 * flood on the hot path.  Once, then at most every 10s,
		 * carrying the running total so the gap is visible. */
		if (n == 1 || now - C.pend_log_ms >= 10000) {
			C.pend_log_ms = now;
			LM_ERR("cluster: parked-request table FULL (%d slots) "
				"- %llu request(s) refused so far.  A forward "
				"or pull that cannot park is refused, not "
				"queued: that is the backpressure.  A client "
				"that cannot route (any RESP client) forwards "
				"most keys and hits this under pipelining; a "
				"cluster-aware client computes the owner and "
				"parks nothing.  stats.cluster.pend_peak "
				"shows the high-water mark.\n",
				C.pend_cap, n);
		}
		pthread_mutex_unlock(&C.pmx);
		cl_fail = PC_CLFAIL_BUSY;
		return 0;
	}
	/* Exact occupancy, now that pend_release() is the only way out.
	 * This used to be the FIRST-FREE INDEX - a lower bound that topped
	 * out at MAX_PENDING-1, so "peak == max" was unreachable and
	 * exhaustion had to special-case the number to say "full" at all.
	 * A knob sized from a measured peak needs a peak that can be
	 * reached, so the count is kept honestly instead. */
	if (++C.pend_used > C.st.pend_peak)
		C.st.pend_peak = C.pend_used;
	/* Mint the handle from the slot we just took.  The generation is
	 * the ONLY field that has to survive the memset: it is what makes
	 * this id different from the last one this slot carried. */
	gen = (slot->gen + 1) & (PEND_GEN_MASK >> PEND_GEN_SHIFT);
	if (!gen)
		gen = 1;
	req = PEND_TAG | ((uint32_t)gen << PEND_GEN_SHIFT) | (uint32_t)i;
	memset(slot, 0, sizeof *slot);
	slot->gen = gen;
	slot->req = req;
	/* the only identity lookup left on the hot path.  Threading a
	 * worker argument down through every verb to reach here would
	 * touch far more than it is worth; this asks the DAEMON for the
	 * context it already keeps, rather than reading the shim's
	 * global. */
	slot->worker = pc_worker_id();
	slot->expect = expect;
	slot->kind = kind;
	slot->deadline_ms = now_ms() + C.pull_timeout_ms + extra_ms;
	pthread_mutex_unlock(&C.pmx);
	*out = slot;
	return req;
}

static size_t build_pull_req(unsigned char *msg, uint32_t req,
		const char *col, size_t collen, const char *key, size_t klen)
{
	msg[0] = M_PULL_REQ;
	p32(msg + 1, req);
	p16(msg + 5, (uint16_t)C.node_id);
	msg[7] = (unsigned char)collen;
	p16(msg + 8, (uint16_t)klen);
	memcpy(msg + 10, col, collen);
	memcpy(msg + 10 + collen, key, klen);
	return 10 + collen + klen;
}

uint32_t pc_pull_begin_at(const char *col, size_t collen, const char *key,
		size_t klen, int holder_node)
{
	unsigned char msg[16 + 256 + 4096];
	struct pending *slot;
	struct peer *pr = peer_by_node(holder_node);
	uint32_t req;
	size_t n;

	cl_fail = PC_CLFAIL_NONE;
	if (!C.enabled || !pr || collen > 255 || klen > 4096) {
		cl_fail = PC_CLFAIL_ROUTE;
		__atomic_fetch_add(&C.st.fwd_no_route, 1, __ATOMIC_RELAXED);
		return 0;
	}
	req = pend_alloc(PC_DONE_PULL, 1, 0, &slot);
	if (!req)
		return 0;
	if (collen < sizeof slot->col && klen < sizeof slot->key) {
		memcpy(slot->col, col, collen);
		slot->collen = (unsigned char)collen;
		memcpy(slot->key, key, klen);
		slot->klen = (unsigned short)klen;
	}
	n = build_pull_req(msg, req, col, collen, key, klen);
	seal_send(&pr->addr, msg, n);
	__atomic_fetch_add(&C.st.pull_sent, 1, __ATOMIC_RELAXED);
	return req;
}

/* @kind is PC_DONE_PULL for a client's miss, or PC_DONE_RECONCILE for
 * B4's probe - the wire request is identical, only what happens to the
 * answer differs. */
static uint32_t pull_begin_kind(const char *col, size_t collen,
		const char *key, size_t klen, int kind)
{
	unsigned char msg[1 + 4 + 2 + 1 + 2 + 256 + 4096];
	struct pending *slot = NULL;
	uint32_t req;
	int i, up = 0;
	size_t n;

	cl_fail = PC_CLFAIL_NONE;
	if (!C.enabled || collen > 255 || klen > 4096) {
		cl_fail = PC_CLFAIL_ROUTE;
		__atomic_fetch_add(&C.st.fwd_no_route, 1, __ATOMIC_RELAXED);
		return 0;
	}
	for (i = 0; i < C.n_peers; i++)
		if (C.peers[i].node &&
		        now_ms() - C.peers[i].last_seen_ms < PEER_UP_MS)
			up++;
	if (!up)
		return 0;

	req = pend_alloc(kind, up, 0, &slot);
	if (!req)
		return 0;                      /* saturated: answer local miss */
	if (collen < sizeof slot->col && klen < sizeof slot->key) {
		memcpy(slot->col, col, collen);
		slot->collen = (unsigned char)collen;
		memcpy(slot->key, key, klen);
		slot->klen = (unsigned short)klen;
	}

	n = build_pull_req(msg, req, col, collen, key, klen);
	for (i = 0; i < C.n_peers; i++)
		if (C.peers[i].node &&
		        now_ms() - C.peers[i].last_seen_ms < PEER_UP_MS)
			seal_send(&C.peers[i].addr, msg, n);
	__atomic_fetch_add(&C.st.pull_sent, 1, __ATOMIC_RELAXED);
	return req;
}

uint32_t pc_pull_begin(const char *col, size_t collen, const char *key,
		size_t klen)
{
	return pull_begin_kind(col, collen, key, klen, PC_DONE_PULL);
}

/* ---- forwarded writes (proxy mode: the holder serializes) --------------- */

static size_t build_fwd_op(unsigned char *msg, uint32_t req, int op,
		unsigned int ttl_rel, long long delta, const char *col,
		size_t collen, const char *key, size_t klen, const char *val,
		size_t vlen)
{
	msg[0] = M_FWD_OP;
	p32(msg + 1, req);
	p16(msg + 5, (uint16_t)C.node_id);
	msg[7] = (unsigned char)op;
	p32(msg + 8, ttl_rel);
	p32(msg + 12, (uint32_t)delta);
	p32(msg + 16, (uint32_t)((unsigned long long)delta >> 32));
	msg[20] = (unsigned char)collen;
	p16(msg + 21, (uint16_t)klen);
	memcpy(msg + 23, col, collen);
	memcpy(msg + 23 + collen, key, klen);
	if (vlen)
		memcpy(msg + 23 + collen + klen, val, vlen);
	return 23 + collen + klen + vlen;
}

uint32_t pc_fwd_begin(int node, int op, const char *col, size_t collen,
		const char *key, size_t klen, const char *val, size_t vlen,
		unsigned int ttl_rel, long long delta)
{
	unsigned char msg[32 + 256 + 4096 + MAX_FWD_VAL];
	struct pending *slot;
	struct peer *pr = peer_by_node(node);
	uint32_t req;
	size_t n;
	int kind = op == 1 ? PC_DONE_FWD_DEL : op == 2 ? PC_DONE_FWD_ADD
		: PC_DONE_FWD_SET;

	cl_fail = PC_CLFAIL_NONE;
	if (!C.enabled || !pr || collen > 255 || klen > 4096 ||
	        vlen > MAX_FWD_VAL) {
		cl_fail = PC_CLFAIL_ROUTE;
		__atomic_fetch_add(&C.st.fwd_no_route, 1, __ATOMIC_RELAXED);
		return 0;
	}
	/* forwards get real deadline headroom over pulls: a write's
	 * honest-late answer beats a false failure while the holder
	 * stores anyway (the deep-pipeline lesson) */
	req = pend_alloc(kind, 1, 700, &slot);
	if (!req)
		return 0;
	n = build_fwd_op(msg, req, op, ttl_rel, delta, col, collen, key,
		klen, val, vlen);
	if (seal_send(&pr->addr, msg, n) != 0) {
		pthread_mutex_lock(&C.pmx);
		pend_release(slot);
		pthread_mutex_unlock(&C.pmx);
		C.px.fwd_fails++;
		cl_fail = PC_CLFAIL_SEND;
		__atomic_fetch_add(&C.st.fwd_send_fail, 1, __ATOMIC_RELAXED);
		C.st.fwd_send_errno = errno;
		return 0;
	}
	C.px.fwd_sent++;
	return req;
}

/* probe-before-place: see cluster.h.  Broadcasts a pull whose pending
 * slot carries the deferred write; handle_pull_rsp transforms a
 * positive into a forward (same req - the ack completes the client's
 * park), an all-negative replays on the worker, a timeout refuses. */
uint32_t pc_probe_fwd_begin(int op, const char *col, size_t collen,
		const char *key, size_t klen, const char *val, size_t vlen,
		unsigned int ttl_rel, long long by)
{
	unsigned char msg[1 + 4 + 2 + 1 + 2 + 256 + 4096];
	struct pending *slot = NULL;
	char *stash = NULL;
	uint32_t req;
	int i, up = 0;
	size_t n;

	/* col/key must fit the slot: the transform re-sends them */
	if (!C.enabled || collen >= 40 || klen >= 256 || vlen > MAX_FWD_VAL)
		return 0;
	for (i = 0; i < C.n_peers; i++)
		if (C.peers[i].node &&
		        now_ms() - C.peers[i].last_seen_ms < PEER_UP_MS)
			up++;
	if (!up)
		return 0;
	if (vlen) {
		stash = malloc(vlen);
		if (!stash)
			return 0;
		memcpy(stash, val, vlen);
	}
	req = pend_alloc(PC_DONE_PULL, up, 700, &slot);
	if (!req) {
		free(stash);
		return 0;
	}
	/* fields land BEFORE the datagrams leave: no answer can race them */
	memcpy(slot->col, col, collen);
	slot->collen = (unsigned char)collen;
	memcpy(slot->key, key, klen);
	slot->klen = (unsigned short)klen;
	slot->probe_op = op == 2 ? 2 : 1;
	slot->stash = stash;
	slot->stash_len = (int)vlen;
	slot->by = by;
	slot->ttl_rel = ttl_rel;

	n = build_pull_req(msg, req, col, collen, key, klen);
	for (i = 0; i < C.n_peers; i++)
		if (C.peers[i].node &&
		        now_ms() - C.peers[i].last_seen_ms < PEER_UP_MS)
			seal_send(&C.peers[i].addr, msg, n);
	__atomic_fetch_add(&C.st.pull_sent, 1, __ATOMIC_RELAXED);
	return req;
}

/* ---- tombstones --------------------------------------------------------- */

void pc_tombstone_send(const char *col, size_t collen, const char *key,
		size_t klen)
{
	unsigned char msg[1 + 1 + 2 + 256 + 4096];
	size_t n;
	int i;

	if (!C.enabled || collen > 255 || klen > 4096)
		return;
	pc_neg_set(col, collen, key, klen, C.tombstone_ms);
	msg[0] = M_TOMBSTONE;
	msg[1] = (unsigned char)collen;
	p16(msg + 2, (uint16_t)klen);
	memcpy(msg + 4, col, collen);
	memcpy(msg + 4 + collen, key, klen);
	n = 4 + collen + klen;
	for (i = 0; i < C.n_peers; i++)
		seal_send(&C.peers[i].addr, msg, n);
	__atomic_fetch_add(&C.st.tomb_sent, 1, __ATOMIC_RELAXED);
}

/* ---- the peer thread ---------------------------------------------------- */


static void handle_pull_req(const unsigned char *pt, size_t n,
		const struct sockaddr_in *from)
{
	pcache_htable_t *ht;
	unsigned char rsp[PULL_RHDR + 65536 + 64];
	unsigned long long ver = 0;
	uint32_t req;
	unsigned int cn, kn, exp = 0;
	str k, v;
	size_t rn;
	int found = 0, rc;

	if (n < 10)
		return;
	req = g32(pt + 1);
	cn = pt[7];
	kn = g16(pt + 8);
	if (10 + cn + kn > n)
		return;
	C.st.pull_served++;

	v.s = NULL;
	v.len = 0;
	ht = pc_store_find((const char *)pt + 10, cn);
	if (ht) {
		k.s = (char *)pt + 10 + cn;
		k.len = (int)kn;
		/* value and version from ONE read: a second lookup for the
		 * version could pair these bytes with a LATER write's number,
		 * and the receiver would then refuse that write as older */
		rc = pcache_ht_fetch_ver(ht, &k, &v, &exp, &ver);
		found = rc == 0;
	}
	rsp[0] = M_PULL_RSP;
	p32(rsp + 1, req);
	p16(rsp + 5, (uint16_t)C.node_id);
	rsp[7] = (unsigned char)found;
	{
		unsigned int now = get_ticks(), ttl_left = 0;

		if (found && exp)
			ttl_left = exp > now ? exp - now : 1;
		p32(rsp + 8, ttl_left);
	}
	p32(rsp + 12, found ? (uint32_t)v.len : 0);
	p64(rsp + 16, found ? ver : 0);
	rn = PULL_RHDR;
	if (found && v.len > 0 && (size_t)v.len <= MAX_DGRAM - PULL_RHDR) {
		memcpy(rsp + PULL_RHDR, v.s, (size_t)v.len);
		rn += (size_t)v.len;
	} else if (found && (size_t)v.len > MAX_DGRAM - PULL_RHDR) {
		/* NOLINTNEXTLINE(clang-analyzer-deadcode.DeadStores) */
		found = 0;                     /* too big for the datagram plane */
		rsp[7] = 0;
		p32(rsp + 12, 0);
		p64(rsp + 16, 0);
	}
	free(v.s);
	seal_send(from, rsp, rn);
}

static void send_demote(int loser_node, int winner_node, const char *col,
		unsigned int cn, const char *key, unsigned int kn)
{
	unsigned char msg[8 + 256 + 256];
	struct peer *pr = peer_by_node(loser_node);
	size_t n;

	if (!pr || !kn)
		return;
	msg[0] = M_DEMOTE;
	p16(msg + 1, (uint16_t)winner_node);
	msg[3] = (unsigned char)cn;
	p16(msg + 4, (uint16_t)kn);
	memcpy(msg + 6, col, cn);
	memcpy(msg + 6 + cn, key, kn);
	n = 6 + cn + kn;
	seal_send(&pr->addr, msg, n);
	C.px.demotes_sent++;
}

static void handle_pull_rsp(const unsigned char *pt, size_t n)
{
	struct pending *p;
	uint32_t req, vlen;
	int found, from, worker = -1, complete = 0;
	int probe_op = 0, pslen = 0;
	char *pstash = NULL;
	const char *rec_col = NULL;
	unsigned int rec_collen = 0, rec_klen = 0;
	char rec_key[256];
	long long pby = 0;
	unsigned int ttl_left, pttl = 0;
	unsigned long long ver;

	if (n < PULL_RHDR)
		return;
	req = g32(pt + 1);
	from = g16(pt + 5);
	found = pt[7];
	ttl_left = g32(pt + 8);
	vlen = g32(pt + 12);
	ver = g64(pt + 16);
	if (PULL_RHDR + vlen > n)
		return;
	pc_lamport_observe(ver);

	pthread_mutex_lock(&C.pmx);
	p = pend_find(req);
	if (p && p->probe_op && found) {
		/* probe-before-place hit: the responsible holder answered -
		 * TRANSFORM into a forward to it.  Same req rides the FWD
		 * datagram, so the holder's ack completes the client's park
		 * as FWD_SET/FWD_ADD; answered stays 0 (the ack path and the
		 * expiry refusal both key on it), probe_fwd routes any stray
		 * second positive to the demoter, and the huge expect keeps
		 * stray negatives from freeing the slot under the ack. */
		unsigned char fmsg[32 + 256 + 4096 + MAX_FWD_VAL];
		struct peer *pr;
		char col[40], key[256];
		char *stash = p->stash;
		int slen = p->stash_len, op = p->probe_op == 2 ? 2 : 0;
		unsigned int cn = p->collen, kn = p->klen, ttl = p->ttl_rel;
		long long delta = p->by;
		size_t fn;

		memcpy(col, p->col, cn);
		memcpy(key, p->key, kn);
		p->kind = op == 2 ? PC_DONE_FWD_ADD : PC_DONE_FWD_SET;
		p->first_node = from;
		p->probe_op = 0;
		p->probe_fwd = 1;
		p->stash = NULL;
		p->stash_len = 0;
		p->expect = 1 << 30;
		p->deadline_ms = now_ms() + C.pull_timeout_ms + 700;
		pthread_mutex_unlock(&C.pmx);

		pc_loc_set(col, cn, key, kn, from);
		pr = peer_by_node(from);
		if (pr) {
			fn = build_fwd_op(fmsg, req, op, ttl, delta, col, cn,
				key, kn, stash, (size_t)slen);
			if (seal_send(&pr->addr, fmsg, fn) == 0)
				C.px.fwd_sent++;
			else
				C.px.fwd_fails++;
			/* a lost send: the deadline refuses honestly */
		}
		free(stash);
		return;
	}
	if (p) {
		if (found && !p->answered && !p->probe_fwd) {
			/* first positive answer wins; the slot LINGERS so a
			 * second positive can trigger the birth-race demote */
			worker = p->worker;
			p->answered = 1;
			p->first_node = from;
			complete = 1;
			if (--p->expect <= 0)
				pend_release(p);
		} else if (found && (p->answered || p->probe_fwd)) {
			/* TWO holders: deterministic demotion, lower id wins */
			int winner = p->first_node < from ? p->first_node : from;
			int loser = p->first_node < from ? from : p->first_node;
			char col[40], key[256];
			unsigned int cn = p->collen, kn = p->klen;

			memcpy(col, p->col, cn);
			memcpy(key, p->key, kn);
			if (--p->expect <= 0)
				pend_release(p);
			pthread_mutex_unlock(&C.pmx);
			LM_WARN("cluster: birth race on a key - demoting node %d "
				"in favour of node %d\n", loser, winner);
			send_demote(loser, winner, col, cn, key, kn);
			pc_loc_set(col, cn, key, kn, winner);
			return;
		} else {
			if (--p->expect <= 0 && !p->answered) {
				worker = p->worker;
				if (p->probe_op) {
					/* fleet-wide confirmed absent: replay the
					 * write on the worker, probe suppressed */
					probe_op = p->probe_op;
					pstash = p->stash;
					pslen = p->stash_len;
					pby = p->by;
					pttl = p->ttl_rel;
					p->stash = NULL;
					complete = 3;
				} else if (p->kind == PC_DONE_RECONCILE) {
					/*
					 * B4.  Nobody in the fleet has this
					 * key, and in eager mode everyone
					 * holds everything - so it was
					 * DELETED while this node was down,
					 * and replay brought it back.  Drop
					 * it, and log the delete so a second
					 * restart does not resurrect it
					 * again.
					 *
					 * Done here rather than posted to a
					 * worker: nothing is waiting for
					 * this answer, and the pending slot
					 * already carries the key it asked
					 * about.
					 */
					rec_col = p->col;
					rec_collen = p->collen;
					memcpy(rec_key, p->key, p->klen);
					rec_klen = p->klen;
					complete = 4;
					pend_release(p);
				} else {
					complete = 2;  /* everyone said no: a miss */
				}
				pend_release(p);
			} else if (p->expect <= 0) {
				pend_release(p);            /* answered earlier: just free */
			}
		}
	}
	pthread_mutex_unlock(&C.pmx);

	if (complete == 4) {
		/* the mutex is released by now - do the delete outside it */
		pcache_htable_t *ht = pc_store_find(rec_col, rec_collen);
		str k;

		if (ht) {
			extern void pc_wal_del(const char *, const char *, int);
			char colz[64];

			k.s = rec_key;
			k.len = (int)rec_klen;
			if (pcache_ht_remove(ht, &k) == 1) {
				memcpy(colz, rec_col, rec_collen);
				colz[rec_collen] = 0;
				pc_wal_del(colz, rec_key, (int)rec_klen);
				C.st.reconciled++;
				LM_NOTICE("cluster: reconcile dropped %.*s/%.*s "
					"- recovered from the WAL but no node "
					"in the fleet still has it\n",
					(int)rec_collen, rec_col,
					(int)rec_klen, rec_key);
			}
		}
		return;
	}
	if (complete == 1) {
		C.st.pull_hits++;
		post_done_ex(worker, req, PC_DONE_PULL, 1, 1, 0, from, ttl_left,
			pt + PULL_RHDR, (int)vlen, 0, 0, ver, 0);
	} else if (complete == 2) {
		C.st.pull_misses++;
		post_done(worker, req, 0, 0, NULL, 0);
	} else if (complete == 3) {
		C.st.pull_misses++;
		post_done_ex(worker, req, PC_DONE_SET_RESUME, 0, 1, pby, 0,
			pttl, (const unsigned char *)pstash, pslen, probe_op,
			0, 0, 0);
		free(pstash);
	}
}

/* the holder applies a forwarded write and acks with the outcome */
static void handle_fwd_op(const unsigned char *pt, size_t n,
		const struct sockaddr_in *from)
{
	extern void pc_wal_upsert(const char *, const char *, int,
		const char *, int, unsigned int, unsigned long long);
	extern void pc_wal_del(const char *, const char *, int);
	pcache_htable_t *ht;
	unsigned char ack[16];
	uint32_t req, ttl_rel;
	long long delta, newval = 0;
	unsigned int cn, kn, vlen, exp = 0;
	int op, ok = 0;
	str k, v;
	char colz[256];

	if (n < 23)
		return;
	req = g32(pt + 1);
	op = pt[7];
	ttl_rel = g32(pt + 8);
	delta = (long long)((unsigned long long)g32(pt + 12) |
		((unsigned long long)g32(pt + 16) << 32));
	cn = pt[20];
	kn = g16(pt + 21);
	if (23 + cn + kn > n)
		return;
	vlen = (unsigned int)(n - 23 - cn - kn);
	memcpy(colz, pt + 23, cn);
	colz[cn] = 0;
	C.px.fwd_served++;

	ht = pc_store_find(colz, cn);
	if (ht) {
		k.s = (char *)pt + 23 + cn;
		k.len = (int)kn;
		if (ttl_rel && ttl_rel != PCACHE_EXP_PRESERVE)
			exp = get_ticks() + ttl_rel;
		if (op == 0) {
			v.s = (char *)pt + 23 + cn + kn;
			v.len = (int)vlen;
			ok = pcache_ht_store(ht, &k, &v, exp) == 0;
			if (ok) {
				pc_wal_upsert(colz, k.s, k.len, v.s, v.len, exp,
				pcache_last_ver);
				pc_neg_clear(colz, cn, k.s, (size_t)kn);
			}
		} else if (op == 1) {
			ok = pcache_ht_remove(ht, &k) == 1;
			if (ok) {
				pc_wal_del(colz, k.s, k.len);
				pc_neg_set(colz, cn, k.s, kn, C.tombstone_ms);
			}
		} else if (op == 2) {
			/* ttl_rel PCACHE_EXP_PRESERVE = the Redis INCR
			 * contract riding the forward plane: keep the
			 * holder's existing expiry, WAL the effective one */
			unsigned int eff = exp;

			ok = pcache_ht_add_ex(ht, &k, delta,
				ttl_rel == PCACHE_EXP_PRESERVE ?
				PCACHE_EXP_PRESERVE : exp, &newval, &eff) == 0;
			if (ok) {
				char nb[24];
				int nl = snprintf(nb, sizeof nb, "%lld", newval);

				pc_wal_upsert(colz, k.s, k.len, nb, nl, eff,
				pcache_last_ver);
			}
		}
	}
	ack[0] = M_FWD_ACK;
	p32(ack + 1, req);
	ack[5] = (unsigned char)ok;
	p32(ack + 6, (uint32_t)(unsigned long long)newval);
	p32(ack + 10, (uint32_t)((unsigned long long)newval >> 32));
	seal_send(from, ack, 14);
}

static void handle_fwd_ack(const unsigned char *pt, size_t n)
{
	struct pending *p;
	uint32_t req;
	long long newval;
	int ok, worker = -1, kind = 0;

	if (n < 14)
		return;
	req = g32(pt + 1);
	ok = pt[5];
	newval = (long long)((unsigned long long)g32(pt + 6) |
		((unsigned long long)g32(pt + 10) << 32));
	pthread_mutex_lock(&C.pmx);
	p = pend_find(req);
	if (p && !p->answered) {
		worker = p->worker;
		kind = p->kind;
		pend_release(p);
	}
	pthread_mutex_unlock(&C.pmx);
	if (worker >= 0)
		post_done_ex(worker, req, kind, 0, ok, newval, 0, 0, NULL, 0, 0,
			0, 0, 0);
}

/* migration: the receiver becomes the holder */
static void handle_migrate(const unsigned char *pt, size_t n,
		const struct sockaddr_in *from)
{
	extern void pc_wal_upsert(const char *, const char *, int,
		const char *, int, unsigned int, unsigned long long);
	pcache_htable_t *ht;
	unsigned char ack[8];
	uint32_t req, ttl_left;
	unsigned int cn, kn, vlen, exp = 0;
	int ok = 0;
	str k, v;
	char colz[256];

	if (n < 19)
		return;
	req = g32(pt + 1);
	ttl_left = g32(pt + 7);
	cn = pt[11];
	kn = g16(pt + 12);
	if (14 + cn + kn > n)
		return;
	vlen = (unsigned int)(n - 14 - cn - kn);
	memcpy(colz, pt + 14, cn);
	colz[cn] = 0;

	ht = pc_store_find(colz, cn);
	if (ht) {
		k.s = (char *)pt + 14 + cn;
		k.len = (int)kn;
		v.s = (char *)pt + 14 + cn + kn;
		v.len = (int)vlen;
		if (ttl_left)
			exp = get_ticks() + ttl_left;
		ok = pcache_ht_store(ht, &k, &v, exp) == 0;
		if (ok) {
			pc_wal_upsert(colz, k.s, k.len, v.s, v.len, exp,
				pcache_last_ver);
			pc_loc_clear(colz, cn, k.s, kn);   /* we hold it now */
			C.px.migrated_in++;
			if (pc_store_shard_enabled(ht))
				shard_note_change();
		}
	}
	ack[0] = M_MIGRATE_ACK;
	p32(ack + 1, req);
	ack[5] = (unsigned char)ok;
	seal_send(from, ack, 6);
}

/* the gathered variant: one datagram, many records, one ack carrying
 * the stored-count.  Kept alongside single M_MIGRATE (still valid on
 * the wire; future single-record moves may prefer it). */
static void handle_migrate_many(const unsigned char *pt, size_t n,
		const struct sockaddr_in *from, int passive)
{
	extern void pc_wal_upsert(const char *, const char *, int,
		const char *, int, unsigned int, unsigned long long);
	unsigned char ack[8];
	uint32_t req, ttl_left;
	unsigned int cnt, i, cn, kn, vlen, exp, stored = 0;
	size_t off = MIG_GHDR;
	pcache_htable_t *ht;
	str k, v;
	char colz[256];

	if (n < MIG_GHDR)
		return;
	req = g32(pt + 1);
	cnt = g16(pt + 7);
	for (i = 0; i < cnt && off + MIG_RHDR <= n; i++) {
		unsigned long long ver;
		int rc;

		ttl_left = g32(pt + off);
		cn = pt[off + 4];
		kn = g16(pt + off + 5);
		vlen = g32(pt + off + 7);
		ver = g64(pt + off + 11);
		if (off + MIG_RHDR + cn + kn + vlen > n)
			break;                     /* truncated: stop, ack what stored */
		memcpy(colz, pt + off + MIG_RHDR, cn);
		colz[cn] = 0;
		/* the sender's clock is at least this far along.  The store
		 * folds it in too, but only for records that reach a
		 * collection - this covers the ones dropped before that. */
		pc_lamport_observe(ver);
		ht = pc_store_find(colz, cn);
		if (ht) {
			k.s = (char *)pt + off + MIG_RHDR + cn;
			k.len = (int)kn;
			v.s = (char *)pt + off + MIG_RHDR + cn + kn;
			v.len = (int)vlen;
			exp = ttl_left ? get_ticks() + ttl_left : 0;
			/* PASSIVE for an eager replica (it never
			 * re-propagates) and off the WAL (the author persists
			 * it; a restarted replica resyncs).  Both kinds carry
			 * the SENDER's version and both refuse a copy older
			 * than the one already held. */
			rc = pcache_ht_store_ver(ht, &k, &v, exp,
				passive ? PCACHE_F_PASSIVE : 0, ver);
			if (rc == PCACHE_E_OLDER) {
				C.px.recv_older++;
			} else if (rc == 0) {
				if (!passive) {
					pc_wal_upsert(colz, k.s, k.len, v.s,
						v.len, exp, pcache_last_ver);
					pc_loc_clear(colz, cn, k.s, kn);
					if (pc_store_shard_enabled(ht))
						shard_note_change();
				}
				C.px.migrated_in++;
				stored++;
			}
		}
		off += MIG_RHDR + cn + kn + vlen;
	}
	ack[0] = M_MIGRATE_ACK;
	p32(ack + 1, req);
	ack[5] = stored > 0;
	p16(ack + 6, (uint16_t)stored);
	seal_send(from, ack, 8);
}

/* the birth-race loser drops its copy and records the winner */
static void handle_demote(const unsigned char *pt, size_t n)
{
	extern void pc_wal_del(const char *, const char *, int);
	pcache_htable_t *ht;
	unsigned int cn, kn, winner;
	str k;
	char colz[256];

	if (n < 6)
		return;
	winner = g16(pt + 1);
	cn = pt[3];
	kn = g16(pt + 4);
	if (6 + cn + kn > n)
		return;
	memcpy(colz, pt + 6, cn);
	colz[cn] = 0;
	C.px.demotes_applied++;
	ht = pc_store_find(colz, cn);
	if (ht) {
		k.s = (char *)pt + 6 + cn;
		k.len = (int)kn;
		if (pcache_ht_remove(ht, &k) == 1)
			pc_wal_del(colz, k.s, k.len);
	}
	pc_loc_set(colz, cn, (const char *)pt + 6 + cn, kn, (int)winner);
}

static void handle_tombstone(const unsigned char *pt, size_t n)
{
	pcache_htable_t *ht;
	unsigned int cn, kn;
	str k;

	if (n < 4)
		return;
	cn = pt[1];
	kn = g16(pt + 2);
	if (4 + cn + kn > n)
		return;
	C.st.tomb_applied++;
	pc_neg_set((const char *)pt + 4, cn, (const char *)pt + 4 + cn, kn,
		C.tombstone_ms);
	ht = pc_store_find((const char *)pt + 4, cn);
	if (ht) {
		k.s = (char *)pt + 4 + cn;
		k.len = (int)kn;
		pcache_ht_remove(ht, &k);
	}
}

static void expire_pending(void)
{
	long long now = now_ms();
	int i, worker;
	uint32_t req;

	for (i = 0; i < C.pend_cap; i++) {
		if (!C.pend[i].req)
			continue;
		pthread_mutex_lock(&C.pmx);
		if (C.pend[i].req && C.pend[i].deadline_ms <= now) {
			int answered = C.pend[i].answered, kind = C.pend[i].kind;
			int jop = C.pend[i].jop;
			int probe_op = C.pend[i].probe_op;
			char *stash = C.pend[i].stash;

			req = C.pend[i].req;
			worker = C.pend[i].worker;
			C.pend[i].stash = NULL;
			pend_release(&C.pend[i]);
			pthread_mutex_unlock(&C.pmx);
			free(stash);
			if (!answered) {
				C.st.pull_timeouts++;
				if (probe_op) {
					/* probe timed out: ambiguous - an honest
					 * refusal beats a potential fork */
					post_done_ex(worker, req, PC_DONE_SET_RESUME,
						0, 0, 0, 0, 0, NULL, 0, probe_op,
						0, 0, 1);
				} else {
					/* timeouts surface as status 2 for JSON ops */
					post_done_ex(worker, req, kind, 0,
						kind == PC_DONE_JSON ? 2 : 0, 0, 0, 0,
						NULL, 0, jop, 0, 0, 1);
				}
			}
			continue;
		}
		pthread_mutex_unlock(&C.pmx);
	}
}


/* ---- automatic membership (S16 as designed) ----------------------------
 * All functions here run on the peer thread ONLY.  Workers read
 * C.node_id (relaxed int) and peers[] entries; .node is the publish
 * field: written LAST with release, read FIRST with acquire. */

/* (ip,port) order - the election's total order over nodes */
static int addr_cmp(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
	uint32_t ia = ntohl(a->sin_addr.s_addr), ib = ntohl(b->sin_addr.s_addr);

	if (ia != ib)
		return ia < ib ? -1 : 1;
	if (a->sin_port != b->sin_port)
		return ntohs(a->sin_port) < ntohs(b->sin_port) ? -1 : 1;
	return 0;
}

static int addr_eq(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
	return a->sin_addr.s_addr == b->sin_addr.s_addr &&
		a->sin_port == b->sin_port;
}

/* find or create the peer slot for @addr; NULL when the table is full.
 * Stale slots (purged: node 0) are recycled in place - entries never
 * move, so worker-side iteration stays safe. */
/* S30: drop a peer we refuse to federate with (config mismatch).  Same
 * shape as GOODBYE - the slot stays, the node id goes to 0, which is
 * what every read path treats as "not a member". */
static void peer_forget(const struct sockaddr_in *from)
{
	int i;

	for (i = 0; i < C.n_peers; i++)
		if (addr_eq(&C.peers[i].addr, from)) {
			if (__atomic_load_n(&C.peers[i].node, __ATOMIC_ACQUIRE)) {
				__atomic_store_n(&C.peers[i].node, 0,
					__ATOMIC_RELEASE);
				shard_note_change();
			}
			return;
		}
}

static struct peer *peer_upsert(const struct sockaddr_in *addr)
{
	struct peer *freep = NULL;
	int i;

	for (i = 0; i < C.n_peers; i++) {
		if (addr_eq(&C.peers[i].addr, addr))
			return &C.peers[i];
		if (!freep && !__atomic_load_n(&C.peers[i].node,
		        __ATOMIC_ACQUIRE))
			freep = &C.peers[i];
	}
	if (!freep) {
		if (C.n_peers >= PC_CL_MAXPEER)
			return NULL;
		freep = &C.peers[C.n_peers];
		memset(freep->repl_mark, 0, sizeof freep->repl_mark);
	memset(freep->repl_cursor, 0, sizeof freep->repl_cursor);
		memset(freep->repl_cursor, 0, sizeof freep->repl_cursor);
		/* publish the slot after addr is in place */
		freep->addr = *addr;
		__atomic_store_n(&C.n_peers, C.n_peers + 1, __ATOMIC_RELEASE);
		shard_note_change();
		return freep;
	}
	memset(freep->repl_mark, 0, sizeof freep->repl_mark);
	freep->addr = *addr;
	return freep;
}


/* ---- node identity (persisted) + incarnation (per process start) -------
 * Two facts the node id cannot carry.  The identity is written once into
 * the state directory and read back on every start, so it survives a
 * restart; the incarnation is fresh each start.  A peer seeing the same
 * identity with a NEW incarnation knows the node restarted and lost its
 * memory, and must be resent everything - which is the whole point,
 * because the master hands a rejoiner ITS OLD NODE ID and the id alone
 * therefore looks unchanged.
 *
 * The identity is PER INSTANCE, not per machine: several nodes commonly
 * share a host (every test rig here runs three), so anything derived
 * from /etc/machine-id, a DMI UUID or a MAC would collide among them. */

static void ident_hex(const unsigned char *id, char *out)
{
	static const char h[] = "0123456789abcdef";
	int i;

	for (i = 0; i < 16; i++) {
		out[(size_t)i * 2] = h[id[i] >> 4];
		out[(size_t)i * 2 + 1] = h[id[i] & 15];
	}
	out[32] = 0;
}

/* An id DERIVED from the identity, so a node proposes the same one every
 * time it starts.  It is only a proposal: 1023 slots collide at around
 * 38 nodes by the birthday bound, so the master still arbitrates and
 * assigns something else when the proposal is taken. */
static int ident_proposed_id(const unsigned char *id)
{
	uint64_t h = 1469598103934665603ULL;
	int i;

	for (i = 0; i < 16; i++) {
		h ^= id[i];
		h *= 1099511628211ULL;
	}
	return 1 + (int)(h % 1023);
}

static void identity_init(const char *state_dir)
{
	char path[512];
	int fd, got = 0;

	C.ident_durable = 0;
	if (state_dir && *state_dir) {
		snprintf(path, sizeof path, "%s/node-identity", state_dir);
		fd = open(path, O_RDONLY);
		if (fd >= 0) {
			got = read(fd, C.self_ident, 16) == 16;
			close(fd);
		}
		if (!got) {
			randombytes_buf(C.self_ident, 16);
			/* 0600: it names this node to its peers */
			fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
			if (fd >= 0) {
				got = write(fd, C.self_ident, 16) == 16;
				if (got)
					fsync(fd);
				close(fd);
				if (!got)
					unlink(path);
			}
		}
		C.ident_durable = got;
	}
	if (!C.ident_durable) {
		/* Nothing to persist to.  Regenerating each start is the
		 * CORRECT answer rather than a fallback: a node with no
		 * durable store has nothing to bring back, so it should
		 * present to the fleet as a new member. */
		randombytes_buf(C.self_ident, 16);
	}
	do {
		randombytes_buf(&C.self_incarn, sizeof C.self_incarn);
	} while (!C.self_incarn);           /* 0 means "not reported" */
	ident_hex(C.self_ident, C.self_ident_hex);
	C.proposed_id = ident_proposed_id(C.self_ident);
	LM_NOTICE("cluster: identity %s (%s), incarnation %u, proposed "
		"node id %d\n", C.self_ident_hex,
		C.ident_durable ? "persisted" : "EPHEMERAL - no state dir, "
		"this node presents as new after a restart",
		C.self_incarn, C.proposed_id);
}

const char *pc_cluster_identity(void)
{
	return C.self_ident_hex;
}

unsigned int pc_cluster_incarnation(void)
{
	return C.self_incarn;
}

int pc_cluster_identity_durable(void)
{
	return C.ident_durable;
}

/* the paths that learn a peer from something other than its own
 * heartbeat (an ASSIGN member list, a MASTER_ALIVE) carry no identity;
 * they pass this and keep the node-id behaviour until the peer's own
 * ALIVE arrives with the real thing */
static const unsigned char pd_noident[16];

static void peer_publish(struct peer *p, int node, unsigned int free_mb,
		unsigned int total_mb, unsigned int live_kb,
		const unsigned char *ident, unsigned int incarn,
		unsigned int entries)
{
	int fresh;

	p->free_mb = free_mb;
	if (total_mb) {
		p->total_mb = total_mb;
		p->live_kb = live_kb;
	}
	p->last_seen_ms = now_ms();

	/* Has this peer's MEMORY gone?  The node id cannot answer it: the
	 * master hands a rejoiner its old id back, so a node that was
	 * kill -9'd and came back empty looked unchanged and was never
	 * resent anything (measured: an eager fleet left it at 0 entries
	 * for 120 seconds while its peers held 20000).  The incarnation
	 * answers it directly - it is regenerated every process start. */
	if (incarn) {
		fresh = !p->has_ident || p->incarn != incarn ||
			memcmp(p->ident, ident, 16) != 0;
		p->has_ident = 1;
		memcpy(p->ident, ident, 16);
		p->incarn = incarn;
	} else {
		/* an older build reports neither: fall back to the id */
		fresh = __atomic_load_n(&p->node, __ATOMIC_RELAXED) != node;
	}
	if (fresh) {
		/* Assume its memory is gone and resync from zero - mark AND
		 * cursor, or the resumed cycle would skip the head of the
		 * table. */
		memset(p->repl_mark, 0, sizeof p->repl_mark);
		memset(p->repl_cursor, 0, sizeof p->repl_cursor);
		memset(p->repl_dirty, 0, sizeof p->repl_dirty);
		/* Resetting the mark is NOT enough on its own.  The steady
		 * sweep sends only records this node AUTHORED, because
		 * re-sending a copy it merely received would echo the
		 * keyspace around the fleet for ever.  A node that came back
		 * empty therefore gets nothing from peers holding only
		 * copies - which is exactly the case that left an eager
		 * fleet with one node at 0 entries indefinitely.  So arm a
		 * ONE-SHOT pass that includes the copies; it is bounded by a
		 * single clean cycle and cannot become an echo. */
		/* Arm it ONLY for a node that came back with NOTHING.
		 *
		 * A node that replayed a WAL is a different case and must
		 * not be backfilled blindly: replicated records carry no
		 * version, so a peer's older copy would silently overwrite
		 * a newer value the node had logged and recovered - it can
		 * author a write, log it, and die before replicating it.
		 * Pushing "everything" at it would lose that write.
		 *
		 * live_kb is reported in the same heartbeat and recovery
		 * completes BEFORE the daemon serves or heartbeats, so a
		 * zero here really does mean an empty node.  Deciding what
		 * a RECOVERED node is owed needs a reconciliation protocol
		 * (a readiness signal, then a diff), which is filed, not
		 * built. */
		if (incarn && !entries)
			memset(p->backfill, 1, sizeof p->backfill);
		else if (incarn)
			LM_NOTICE("cluster: node %d restarted holding %u "
				"records - NOT backfilling (a recovered node "
				"needs a diff, not a blind push)\n", node,
				entries);
	}
	__atomic_store_n(&p->node, node, __ATOMIC_RELEASE);
	shard_note_change();
}

static int peers_live(void)
{
	long long now = now_ms();
	int i, n = 0;

	for (i = 0; i < C.n_peers; i++)
		if (C.peers[i].node && now - C.peers[i].last_seen_ms < PEER_UP_MS)
			n++;
	return n;
}

/* order-independent membership digest over (node,ip) - the module's
 * MASTER_ALIVE digest shape */
static uint64_t member_digest(void)
{
	uint64_t d = 0;
	long long now = now_ms();
	int i;

	for (i = 0; i < C.n_peers; i++) {
		uint64_t h = 1469598103934665603ull;

		if (!C.peers[i].node ||
		        now - C.peers[i].last_seen_ms >= PEER_UP_MS)
			continue;
		h = (h ^ (uint32_t)C.peers[i].node) * 1099511628211ull;
		h = (h ^ C.peers[i].addr.sin_addr.s_addr) * 1099511628211ull;
		d ^= h;
	}
	{
		uint64_t h = 1469598103934665603ull;

		h = (h ^ (uint32_t)C.node_id) * 1099511628211ull;
		h = (h ^ C.self_addr.sin_addr.s_addr) * 1099511628211ull;
		d ^= h;
	}
	return d;
}

static void send_join_req(const struct sockaddr_in *to)
{
	unsigned char msg[41];
	uint64_t d = config_digest();

	msg[0] = M_JOIN_REQ;
	memcpy(msg + 1, &C.join_tok, 8);
	/* S30: the joiner states its collection config UP FRONT, so a
	 * master that runs something else never assigns it an id - the
	 * join is refused rather than half-completed */
	msg[9] = (unsigned char)C.cfg_mode;
	msg[10] = (unsigned char)C.cfg_eager;
	memcpy(msg + 11, &d, 8);
	/* the id this node would LIKE, derived from its identity so it is
	 * the same every start.  A proposal only - the master rejects it
	 * if taken, which is what keeps two identities that happen to hash
	 * alike from ending up on one id. */
	memcpy(msg + 19, C.self_ident, 16);
	p16(msg + 35, (uint16_t)C.proposed_id);
	/* the incarnation too: identity alone cannot tell "the member you
	 * already know, re-joining" from "a second process claiming to be
	 * it", and only the first of those is ordinary traffic */
	p32(msg + 37, C.self_incarn);
	seal_send(to ? to : &C.mcast_dst, msg, 41);
	C.join_last_ms = now_ms();
	C.st.joins++;
}

static void send_alive(void)
{
	unsigned char msg[63];
	uint64_t d = config_digest();

	msg[0] = M_ALIVE;
	p16(msg + 1, (uint16_t)C.node_id);
	p32(msg + 3, self_free_mb());
	p32(msg + 7, self_total_mb());
	p32(msg + 11, self_live_kb());
	/* S30 trailer: what this node believes the cluster's collections
	 * ARE.  A peer that disagrees is refused rather than fed. */
	msg[15] = (unsigned char)C.cfg_mode;
	msg[16] = (unsigned char)C.cfg_eager;
	memcpy(msg + 17, &d, 8);
	p16(msg + 25, (uint16_t)C.cfg_client_port);   /* S34 */
	/* identity + incarnation LAST, so a peer built before them reads
	 * the prefix it knows and ignores the rest */
	memcpy(msg + 27, C.self_ident, 16);
	p32(msg + 43, C.self_incarn);
	p32(msg + 47, self_entries());
	/* the Lamport clock rides the heartbeat: at 1 Hz every node folds in
	 * every other node's value, so the fleet converges continuously with
	 * no clock agreement and no extra traffic */
	{
		unsigned long long lc = pc_lamport_now();

		memcpy(msg + 51, &lc, 8);
	}
	/* B1: the lifecycle state, at the very tail.  A peer built before it
	 * reads the 61-byte prefix it knows and ignores the rest - the same
	 * shape identity and incarnation were added in. */
	msg[59] = (unsigned char)pc_node_state();
	/* the arena's ACTUAL page tier, at the very tail again.  A peer
	 * built before it sends 60 bytes and is read as unknown. */
	msg[60] = (unsigned char)pcache_mem.tier;
	/* S49: the RESP door, same additive tail.  0 = none configured. */
	p16(msg + 61, (uint16_t)C.cfg_resp_port);
	seal_send(&C.mcast_dst, msg, 63);
	C.st.hb_sent++;
	/* publish for the watchdog; a non-master's stale MASTER_ALIVE
	 * must not outlive the role, so clear it here */
	beat_store(beat_alive, &beat_alive_len, msg, 63);
	if (C.role != PC_ROLE_MASTER)
		beat_store(beat_malive, &beat_malive_len, NULL, 0);
	__atomic_store_n(&beat_sent_ms, now_ms(), __ATOMIC_RELEASE);
}

/* ---- the cluster map (control plane, additive) --------------------------
 * The master builds a map from what it can see and broadcasts it; every
 * other node adopts it if the epoch is newer.  NOTHING READS IT FOR
 * PLACEMENT YET - shard ownership still runs through hrw_mix below.
 * This exists so the plumbing can be exercised on a live fleet before
 * anything depends on it.
 */

static int pick_backup(void);
static void clsync_send(const struct pc_clmap *m);

/* Only the master builds one.  A member that built its own would be
 * publishing an opinion, and two opinions at one epoch is the ambiguity
 * the term exists to remove. */
static int clmap_build(struct pc_clmap *m)
{
	long long now = now_ms();
	unsigned int i;

	if (C.role != PC_ROLE_MASTER)
		return -1;
	memset(m, 0, sizeof *m);
	m->term = pc_term_current();
	m->seq = C.map_valid ? C.map.seq + 1 : 1;
	m->master_id = (uint16_t)C.node_id;
	/* The standby is named in the SAME publication as everything else,
	 * so the fleet learns master and backup atomically.  Observing them
	 * separately leaves a window in which nodes disagree about who the
	 * standby is, and a second failure in that window has no answer. */
	C.backup_id = pick_backup();
	m->backup_id = (uint16_t)C.backup_id;
	m->mode = (uint8_t)C.cfg_mode;
	m->eager = (uint8_t)(C.cfg_eager ? 1 : 0);
	m->config_digest = config_digest();

	/* self first, then every peer heard from inside the up window */
	pc_clhist_note(&C.hist, C.self_ident, (uint16_t)C.node_id);
	m->node[0].node_id = (uint16_t)C.node_id;
	memcpy(m->node[0].ident, C.self_ident, 16);
	m->node[0].addr = C.self_addr.sin_addr.s_addr;
	m->node[0].cluster_port = C.self_addr.sin_port;
	m->node[0].client_port = (uint16_t)C.cfg_client_port;
	m->node[0].state = (uint8_t)pc_node_state();
	m->node[0].master_pref = 0;
	m->node[0].cap_weight = PC_CLMAP_W_NOMINAL;
	m->node[0].admin_weight = PC_CLMAP_W_UNSET;
	m->nnodes = 1;

	for (i = 0; i < (unsigned int)C.n_peers &&
	        m->nnodes < PC_CLMAP_MAXNODE; i++) {
		struct peer *p = &C.peers[i];
		struct pc_clmap_node *nd;
		int id = __atomic_load_n(&p->node, __ATOMIC_ACQUIRE);

		if (!id || now - p->last_seen_ms >= PEER_UP_MS)
			continue;
		if (id == C.node_id)
			continue;      /* never list ourselves twice: a
			                * duplicate id makes the map refuse
			                * to decode on every receiver */
		/* A node joins the map once it is READY and is not dropped
		 * again for a transient state - only by going away.  A peer
		 * still JOINING reports STARTING, and recording that
		 * publishes a map that node cannot be placed from until it
		 * is reissued. */
		if (p->nstate != PC_NST_READY &&
		        !pc_clmap_find(&C.map, (uint16_t)id))
			continue;
		/* the master remembers every identity it places in a map:
		 * that record is what will tell a returning node from a new
		 * one, and a promoted backup cannot rebuild it */
		if (p->has_ident)
			pc_clhist_note(&C.hist, p->ident, (uint16_t)id);
		nd = &m->node[m->nnodes];
		nd->node_id = (uint16_t)id;
		memcpy(nd->ident, p->ident, 16);
		nd->addr = p->addr.sin_addr.s_addr;
		nd->cluster_port = p->addr.sin_port;
		nd->client_port = (uint16_t)p->client_port;
		nd->state = (uint8_t)p->nstate;
		nd->master_pref = 0;
		/* uniform for now.  That is not a placeholder: uniform
		 * weights place identically to the unweighted HRW this
		 * fleet already uses, so the day placement switches over,
		 * not one key moves. */
		nd->cap_weight = PC_CLMAP_W_NOMINAL;
		nd->admin_weight = PC_CLMAP_W_UNSET;
		m->nnodes++;
	}
	return 0;
}

/*
 * May the map decide placement yet?
 *
 * Only when the set it says is placeable matches what this node sees as
 * live.  Map-based placement LAGS fleet formation - a peer sends its
 * first ALIVE while still JOINING, so it reports STARTING, and until the
 * master notices it becoming READY and reissues, the map is short a
 * node.  Placing from a short map hands one node nothing at all
 * (measured: a three-node fleet splitting 0/152/148) and moves keys
 * again when the map catches up, forking re-writes.
 *
 * Falling back does not split placement, which is the usual objection.
 * The map is published with uniform weights, so weighted rendezvous over
 * it selects IDENTICALLY to the unweighted argmax over the same nodes -
 * a node using the map and a node using its own liveness agree whenever
 * their sets agree, and disagree only when they do not, which is
 * precisely when this check sends both to the fallback.
 */
/* ---- B4: reconcile a recovered keyspace against the live fleet -------
 *
 * A node that comes back with a WAL replays everything it logged,
 * including keys the fleet DELETED while it was down.  Nothing
 * remembers those deletes - tombstones live seconds, and after a real
 * outage the fleet's current contents are the only truth left.  So the
 * node asks: does anyone still have this?
 *
 * EAGER MODE ONLY, and that restriction is load-bearing.  Eager
 * replicates every record to every node, so a key nobody has was
 * deleted.  In plain store mode peers legitimately lack keys they never
 * pulled, and "absent means deleted" would destroy live data.
 *
 * What it costs: a key this node authored, logged, and never managed to
 * replicate to anyone is indistinguishable from a deleted one, and is
 * dropped.  That is a real loss, bounded by the last unreplicated
 * writes before death, and it is the price of not resurrecting deletes.
 * A key that WAS replicated is safe even if this node holds a newer
 * value - peers still have it, so the probe finds it and the newer
 * local copy stands (that is rejointest's obligation 1).
 *
 * The node keeps serving throughout.  The window in which a resurrected
 * key is visible shrinks from for ever to the length of this pass; it
 * does not close entirely, which would need the node to withhold
 * service until it finished.
 */
struct rec_ctx {
	const char *col;
	size_t collen;
	int budget;
};

static int reconcile_cb(const str *key, const str *val, unsigned int exp,
		void *arg)
{
	struct rec_ctx *r = arg;

	(void)val;
	(void)exp;
	if (r->budget <= 0)
		return -1;                 /* stop the walk; resume next tick */
	if (pull_begin_kind(r->col, r->collen, key->s, (size_t)key->len,
	        PC_DONE_RECONCILE)) {
		C.st.reconcile_probed++;
		r->budget--;
	}
	return 0;
}

static void reconcile_tick(void)
{
	struct rec_ctx r;

	if (!C.reconcile_on || !C.cfg_eager)
		return;
	if (C.role == PC_ROLE_JOINING)
		return;                    /* nobody to ask yet */

	r.budget = RECONCILE_PER_TICK;
	while (r.budget > 0 && C.rec_col < pc_store_count()) {
		pcache_htable_t *ht = pc_store_ht(C.rec_col);

		r.col = pc_store_name(C.rec_col);
		r.collen = strlen(r.col);
		if (!ht) {
			C.rec_col++;
			C.rec_cursor = 0;
			continue;
		}
		pcache_ht_scan(ht, &C.rec_cursor, RECONCILE_BUCKETS,
			reconcile_cb, &r);
		if (!C.rec_cursor) {
			C.rec_col++;       /* this collection is done */
			continue;
		}
		if (r.budget <= 0)
			return;            /* out of budget mid-collection */
	}
	if (C.rec_col >= pc_store_count()) {
		C.reconcile_on = 0;
		/* Probes ISSUED, not resolved.  The answers arrive over the
		 * next round trips and each drop logs itself, so counting
		 * drops here would always report zero and read as "nothing
		 * was wrong". */
		LM_NOTICE("cluster: reconcile pass finished asking - %llu "
			"key(s) probed; any that nobody still has are dropped "
			"as their answers arrive\n", C.st.reconcile_probed);
	}
}

/*
 * Pick the standby.  Highest master_preference wins, then the lowest
 * node id as a stable tiebreak, so every node reaching this
 * independently would name the same one.
 *
 * KEYED ON LIVENESS, NOT ON NODE STATE.  Holding the control plane has
 * nothing to do with whether a node's DATA is complete: the state is
 * small and synced, and a node still recovering its keyspace can carry
 * it perfectly well.  Excluding recovering nodes would shrink the
 * candidate pool exactly when the cluster is most likely to need one.
 */
static int pick_backup(void)
{
	long long now = now_ms();
	unsigned int i;
	int best = 0, best_pref = -1;

	if (C.role != PC_ROLE_MASTER)
		return 0;
	for (i = 0; i < (unsigned int)C.n_peers; i++) {
		struct peer *p = &C.peers[i];
		int id = __atomic_load_n(&p->node, __ATOMIC_ACQUIRE);
		int pref = 0;              /* master_preference is not settable
		                            * yet; every node scores the same
		                            * and the tiebreak decides */

		if (!id || id == C.node_id ||
		        now - p->last_seen_ms >= PEER_UP_MS)
			continue;
		if (pref > best_pref || (pref == best_pref &&
		        (!best || id < best))) {
			best_pref = pref;
			best = id;
		}
	}
	return best;
}

static void clmap_recheck_usable(void)
{
	long long now = now_ms();
	unsigned int i, live = 1, ok = 0;
	const struct pc_clmap_node *nd;

	C.map_usable = 0;
	if (!C.map_valid)
		return;
	nd = pc_clmap_find(&C.map, (uint16_t)C.node_id);
	if (!nd || !pc_clmap_placeable(nd))
		return;                    /* we are not placeable in it */
	ok = 1;
	for (i = 0; i < (unsigned int)C.n_peers; i++) {
		struct peer *p = &C.peers[i];
		int id = __atomic_load_n(&p->node, __ATOMIC_ACQUIRE);

		if (!id || id == C.node_id ||
		        now - p->last_seen_ms >= PEER_UP_MS)
			continue;
		live++;
		nd = pc_clmap_find(&C.map, (uint16_t)id);
		if (nd && pc_clmap_placeable(nd))
			ok++;
	}
	/* every live node placeable, and nothing placeable we cannot see */
	C.map_usable = (live == ok) &&
		(ok == (unsigned int)pc_clmap_placeable_count(&C.map));
}

/* Has the set the map should contain changed since it was published?
 * A peer becoming READY counts - without it the map is never reissued,
 * that peer stays absent, and the map never becomes usable at all. */
static int clmap_membership_moved(void)
{
	long long now = now_ms();
	unsigned int i, want = 1;          /* self */

	if (!C.map_valid)
		return 1;
	for (i = 0; i < (unsigned int)C.n_peers; i++) {
		struct peer *p = &C.peers[i];
		int id = __atomic_load_n(&p->node, __ATOMIC_ACQUIRE);

		if (!id || id == C.node_id ||
		        now - p->last_seen_ms >= PEER_UP_MS)
			continue;
		if (!pc_clmap_find(&C.map, (uint16_t)id)) {
			if (p->nstate != PC_NST_READY)
				continue;  /* not eligible yet, not missing */
			return 1;          /* someone new became READY */
		}
		want++;
	}
	return want != C.map.nnodes;       /* or someone went away */
}

/*
 * Staging a map change past the standby.
 *
 * The order is: build, STAGE, hand it to the standby, and broadcast only
 * once the standby has acknowledged holding it.  That buys one property
 * nothing downstream then has to enforce - the standby is never BEHIND
 * the fleet, only ever equal or one committed change ahead - and it
 * holds in every window the master can die in.  Dying before the ack
 * loses the change and leaves standby and fleet agreeing on the old map.
 * Dying after it leaves the standby holding something the fleet has not
 * seen, which it publishes on promotion; that is correct, because the
 * change was committed the moment it was acked.
 *
 * With no standby, changes publish immediately and are counted as
 * unsynced.  Refusing would mean a node cannot JOIN while the cluster is
 * already short a controller, turning a degraded state into a stuck one.
 *
 * A standby that never answers must not wedge the control plane either,
 * so a staged change has a deadline and is abandoned past it.  Its
 * SEQUENCE IS NOT REUSED: the standby may have acked with only the ack
 * lost, and a different map published at the same epoch is exactly the
 * ambiguity the epoch exists to remove.
 */
static void clmap_stage(void)
{
	uint32_t seq = 0;

	if (C.role != PC_ROLE_MASTER)
		return;
	if (!pc_term_current()) {
		/* no term claimed: a map under term 0 would be unorderable
		 * against the next master's.  Silence is the safer failure. */
		return;
	}
	pc_clsync_set_backup(&C.sync, C.backup_id != 0);
	if (pc_clsync_stage(&C.sync, &seq) != 0)
		return;                    /* one in flight already */
	if (clmap_build(&C.staged) != 0) {
		pc_clsync_abort(&C.sync);
		return;
	}
	C.staged.seq = seq;
	C.staged_deadline_ms = now_ms() + CLSYNC_ACK_MS;
	clsync_send(&C.staged);
}

static void clmap_publish(void)
{
	unsigned char msg[1 + PC_CLMAP_MAXBYTES];
	long n;

	if (!pc_clsync_publishable(&C.sync))
		return;
	n = pc_clmap_encode(&C.staged, msg + 1, sizeof msg - 1);
	if (n <= 0) {
		pc_clsync_abort(&C.sync);
		return;
	}
	msg[0] = M_CLMAP;
	if (seal_send(&C.mcast_dst, msg, (size_t)n + 1) != 0)
		return;                    /* retry on the next tick */
	pc_clsync_published(&C.sync);
	C.map = C.staged;
	C.map_valid = 1;
	C.map_pub_ms = now_ms();
	C.map_pub_n++;
	clmap_recheck_usable();
}

/* drive the machine: publish what is ready, abandon what has hung */
static void clmap_tick(long long now)
{
	if (C.role != PC_ROLE_MASTER)
		return;
	clmap_publish();
	if (C.sync.state == PC_CLSYNC_STAGED &&
	        now >= C.staged_deadline_ms) {
		pc_clsync_abort(&C.sync);
		C.stage_timeout_n++;
		if (C.stage_timeout_n % 20 == 1)
			LM_WARN("cluster: the standby (node %d) has not "
				"acknowledged a control-state sync - "
				"publishing is stalled behind it\n",
				C.backup_id);
	}
}

/* ---- the control state a promoted standby cannot rebuild ------------- */

static void clsync_send(const struct pc_clmap *m)
{
	static unsigned char msg[1 + 4 + PC_CLMAP_MAXBYTES + 4 +
		2 + PC_CLHIST_MAX * PC_CLHIST_ENTSZ];
	struct peer *pr;
	long ml, hl;
	size_t off;

	if (C.role != PC_ROLE_MASTER || !C.backup_id)
		return;
	pr = peer_by_node(C.backup_id);
	if (!pr)
		return;
	msg[0] = M_CLSYNC;
	ml = pc_clmap_encode(m, msg + 5, sizeof msg - 5);
	if (ml <= 0)
		return;
	p32(msg + 1, (uint32_t)ml);
	off = 5 + (size_t)ml;
	hl = pc_clhist_encode(&C.hist, msg + off + 4, sizeof msg - off - 4);
	if (hl < 0)
		return;
	p32(msg + off, (uint32_t)hl);
	off += 4 + (size_t)hl;
	if (seal_send(&pr->addr, msg, off) == 0)
		C.sync_sent_n++;
}

static void handle_clsync(const unsigned char *pt, size_t n,
		const struct sockaddr_in *from)
{
	struct pc_clmap m;
	unsigned char ack[9];
	const char *why = "?";
	uint32_t ml, hl;
	size_t off;

	if (n < 9)
		return;
	C.sync_rx_n++;
	ml = g32(pt + 1);
	if (5 + (size_t)ml + 4 > n)
		goto bad;
	if (pc_clmap_decode(pt + 5, ml, &m, &why) != 0)
		goto bad;
	off = 5 + (size_t)ml;
	hl = g32(pt + off);
	if (off + 4 + (size_t)hl != n)
		goto bad;
	if (pc_clhist_decode(pt + off + 4, hl, &C.held_hist, &why) != 0)
		goto bad;

	/* Held, not adopted.  This is a copy kept ON BEHALF of the master
	 * for the moment it is needed; the map this node PLACES from is
	 * still whatever arrived on the broadcast.  Conflating the two
	 * would let a standby act on state the fleet has not been told. */
	C.held_valid = 1;
	C.held_map = m;
	C.held_term = m.term;
	C.held_seq = m.seq;
	/* what we have acked and not yet seen published is OWED to the
	 * fleet: it was committed the moment we acked it, so if this node
	 * is promoted before the master broadcasts it, publishing it is
	 * this node's job. */
	pc_clsync_hold(&C.sync, m.term, m.seq);

	/* Acked only after it is stored.  Acking first and storing after
	 * would let the master publish a change this node then failed to
	 * keep - which is the one thing the ordering exists to prevent. */
	ack[0] = M_CLSYNC_ACK;
	p32(ack + 1, m.term);
	p32(ack + 5, m.seq);
	seal_send(from, ack, sizeof ack);
	return;
bad:
	if (++C.sync_bad_n % 100 == 1)
		LM_WARN("cluster: refusing a control-state sync (%s)\n", why);
}

static void handle_clsync_ack(const unsigned char *pt, size_t n)
{
	if (n < 9)
		return;
	C.sync_ack_n++;
	pc_clsync_ack(&C.sync, g32(pt + 1), g32(pt + 5));
}

static void handle_clmap(const unsigned char *pt, size_t n,
		const struct sockaddr_in *from)
{
	struct pc_clmap m;
	const char *why = "?";

	if (n < 2)
		return;
	/* our own multicast comes back to us.  Skipping it keeps received
	 * and stale meaning what they say - without this a lone master
	 * reports receiving and rejecting a map every publication, which
	 * reads like a fleet arguing with itself. */
	if (addr_eq(from, &C.self_addr))
		return;
	C.map_rx_n++;
	if (pc_clmap_decode(pt + 1, n - 1, &m, &why) != 0) {
		/* counted and named, never silently dropped: a map we cannot
		 * read is a fleet that disagrees about its own format */
		if (++C.map_bad_n % 100 == 1)
			LM_WARN("cluster: refusing a cluster map (%s)\n", why);
		return;
	}
	/* the term is a peer's claim, so it goes through the jump guard */
	pc_term_observe(m.term);

	if (C.map_valid &&
	        pc_clmap_epoch_cmp(m.term, m.seq, C.map.term, C.map.seq) <= 0) {
		C.map_stale_n++;
		return;                /* not newer: nothing to learn */
	}
	if (C.role == PC_ROLE_MASTER &&
	        pc_clmap_epoch_cmp(m.term, m.seq, pc_term_current(), 0) > 0 &&
	        m.term > pc_term_current()) {
		/* a master seeing a higher TERM steps down, unconditionally.
		 * Not wired to the role machine yet - logged so the case is
		 * visible if it ever happens before that lands. */
		LM_WARN("cluster: a map at term %u arrived while we are master "
			"at term %u - the step-down rule is not wired yet\n",
			m.term, pc_term_current());
	}
	C.map = m;
	C.map_valid = 1;
	clmap_recheck_usable();
	/* the fleet has reached this epoch, so nothing is owed at or below
	 * it - and a LATER TERM clears the debt just as surely, since
	 * whatever we held belonged to a master that no longer exists */
	pc_clsync_saw(&C.sync, m.term, m.seq);
	if (C.map_rx_n == 1 || !(C.map_rx_n % 60))
		LM_NOTICE("cluster: holding map epoch (%u,%u), %u node(s), "
			"master %u\n", m.term, m.seq, m.nnodes, m.master_id);
}

static void send_master_alive(void)
{
	unsigned char msg[35];
	uint64_t d = member_digest();

	msg[0] = M_MASTER_ALIVE;
	p16(msg + 1, (uint16_t)C.node_id);
	p16(msg + 3, (uint16_t)(peers_live() + 1));
	memcpy(msg + 5, &d, 8);
	p32(msg + 13, self_free_mb());
	p32(msg + 17, self_total_mb());
	p32(msg + 21, self_live_kb());
	/* S30: MASTER_ALIVE admits peers too, so it carries the config
	 * identity as well - otherwise a refused joiner still learns the
	 * master through it and the refusal is one-sided (measured) */
	{
		uint64_t cd = config_digest();

		msg[25] = (unsigned char)C.cfg_mode;
		msg[26] = (unsigned char)C.cfg_eager;
		memcpy(msg + 27, &cd, 8);
	}
	seal_send(&C.mcast_dst, msg, 35);
	beat_store(beat_malive, &beat_malive_len, msg, 35);
}

/* Peer-thread-only 1 Hz emitter for long duties: a migration tick's
 * ack windows and the victim walk call this as they grind, so the node
 * keeps saying "alive" (and a master keeps saying "master") while it
 * works.  Emission only - no membership decisions, no draining - so it
 * is safe anywhere on the peer thread, including from a walk callback. */
static void beat_pump(void)
{
	if (now_ms() - __atomic_load_n(&beat_sent_ms, __ATOMIC_ACQUIRE)
	        < 1000)
		return;
	if (C.role == PC_ROLE_MEMBER || C.role == PC_ROLE_MASTER)
		send_alive();
	if (C.role == PC_ROLE_MASTER)
		send_master_alive();
}

static void send_goodbye(void)
{
	unsigned char msg[3];

	msg[0] = M_GOODBYE;
	p16(msg + 1, (uint16_t)C.node_id);
	seal_send(&C.mcast_dst, msg, 3);
}

/* master: allocate the lowest unused node id (rejoiners keep theirs) */
static int id_taken(int id)
{
	int i;

	if (id == C.node_id)
		return 1;
	for (i = 0; i < C.n_peers; i++)
		if (C.peers[i].node == id)
			return 1;
	return 0;
}

/* Allocate a node id, honouring the joiner's PROPOSAL when it is free.
 * The proposal is derived from the joiner's identity, so a node that
 * restarts asks for - and normally gets - the id it had before, without
 * the master having to remember anything.
 *
 * It stays a proposal because 1023 slots collide at around 38 nodes by
 * the birthday bound: two unrelated identities can hash alike, and if
 * both were simply granted their proposal they would SHARE an id.  The
 * master is the arbiter, so overlap is impossible rather than unlikely.
 * @preferred 0 = no proposal (an older joiner). */
static int alloc_node_id(int preferred)
{
	int id;

	if (preferred > 0 && preferred <= 1023 && !id_taken(preferred))
		return preferred;
	if (preferred > 0)
		LM_NOTICE("cluster: node id %d proposed but taken - "
			"assigning another\n", preferred);
	for (id = 1; id <= 1023; id++)
		if (!id_taken(id))
			return id;
	return 0;
}

/* master: unicast ASSIGN (echoing @tok) with the full member list */
static void send_assign(const struct sockaddr_in *to, uint64_t tok,
		int your_id)
{
	unsigned char msg[16 + PC_CL_MAXPEER * 20 + 20];
	long long now = now_ms();
	size_t off;
	int i, n = 0;

	msg[0] = M_ASSIGN;
	memcpy(msg + 1, &tok, 8);
	p16(msg + 9, (uint16_t)your_id);
	p16(msg + 11, (uint16_t)C.node_id);
	off = 15;
	/* self first, then every live peer except the addressee */
	p16(msg + off, (uint16_t)C.node_id);
	memcpy(msg + off + 2, &C.self_addr.sin_addr.s_addr, 4);
	memcpy(msg + off + 6, &C.self_addr.sin_port, 2);
	p32(msg + off + 8, self_free_mb());
	p32(msg + off + 12, self_total_mb());
	p32(msg + off + 16, self_live_kb());
	off += 20;
	n = 1;
	for (i = 0; i < C.n_peers; i++) {
		if (!C.peers[i].node ||
		        now - C.peers[i].last_seen_ms >= PEER_UP_MS ||
		        addr_eq(&C.peers[i].addr, to))
			continue;
		p16(msg + off, (uint16_t)C.peers[i].node);
		memcpy(msg + off + 2, &C.peers[i].addr.sin_addr.s_addr, 4);
		memcpy(msg + off + 6, &C.peers[i].addr.sin_port, 2);
		p32(msg + off + 8, C.peers[i].free_mb);
		p32(msg + off + 12, C.peers[i].total_mb);
		p32(msg + off + 16, C.peers[i].live_kb);
		off += 20;
		n++;
	}
	p16(msg + 13, (uint16_t)n);
	seal_send(to, msg, off);
	C.st.assigns++;
}

static void become_master(int founded)
{
	if (!C.node_id)
		C.node_id = 1;
	C.role = PC_ROLE_MASTER;
	/* founding or winning an election settles membership.  B4 will put
	 * a node with data to settle into RECOVERING first. */
	pc_node_state_set(PC_NST_READY);
	C.master_id = C.node_id;
	C.master_addr = C.self_addr;
	C.st.elections++;
	/* Claim a term, above everything this node has SEEN rather than
	 * above its own last: a node that was away has seen less than the
	 * fleet, and claiming from its own history reissues a term someone
	 * else already used.  It is persisted before it is returned, so a
	 * master that publishes and dies cannot come back and reissue the
	 * same term with different content behind it. */
	{
		uint32_t t = pc_term_claim();

		if (!t) {
			/* Nowhere to write it.  This node still serves as
			 * master for everything that worked before - it just
			 * publishes no map, because a map under a term we
			 * cannot remember is one we could contradict. */
			LM_CRIT("cluster: could not persist a mastership term "
				"- serving as master WITHOUT publishing a "
				"cluster map (check the state directory)\n");
		} else {
			uint32_t owed_t = 0, owed_s = 0;
			int owed = pc_clsync_owed(&C.sync, &owed_t, &owed_s);

			/*
			 * A promoted standby starts from what it was handed,
			 * not from nothing.
			 *
			 * The identity history is the piece that matters and
			 * the piece it could not rebuild: only a master ever
			 * learns which identities this cluster has seen, so a
			 * promotion without it would call every returning node
			 * NEW - backfilling nodes that only needed
			 * reconciling, and losing the STARTING/RECOVERING
			 * distinction entirely.
			 */
			if (C.held_valid) {
				C.hist = C.held_hist;
				LM_NOTICE("cluster: promoted holding the "
					"previous master's state - %u "
					"identities, map epoch (%u,%u)\n",
					C.hist.n, C.held_term, C.held_seq);
			} else {
				LM_WARN("cluster: promoted with NO synced "
					"state - every returning node will "
					"look new until this master has seen "
					"it once\n");
			}

			/* the staging machine issues under this term: a
			 * sequence carried over from the previous one would
			 * be ordered against maps that no longer matter */
			pc_clsync_init(&C.sync, t, 0);

			/*
			 * A change acked but never seen published was
			 * COMMITTED, and the fleet has never been told.  It is
			 * republished here under the NEW term rather than the
			 * old one - the term it was staged under belongs to a
			 * master that no longer exists, and reissuing it would
			 * put two maps at one epoch.
			 */
			if (owed) {
				C.map = C.held_map;
				C.map_valid = 1;
				LM_NOTICE("cluster: republishing a change the "
					"previous master committed but never "
					"broadcast (was %u,%u)\n",
					owed_t, owed_s);
			}
			LM_NOTICE("cluster: claimed mastership term %u\n", t);
		}
	}
	LM_NOTICE("cluster: %s as node %d (%d live peer(s))\n",
		founded ? "founded the cluster - master" :
		"elected master", C.node_id, peers_live());
	send_master_alive();
	/* stage the first map at once rather than waiting a tick - a fleet
	 * should not wait to learn who its master is */
	clmap_stage();
}

/* Is this identity currently claimed by a member that is still ALIVE?
 *
 * Liveness is the whole test - NOT the address.  A clone can appear from
 * anywhere, and a node that genuinely failed can come back on the same
 * address or a different one; what separates them is whether the holder
 * of that identity is still heartbeating.  If the node really died the
 * master stops hearing it, the slot ages out, and the identity is free
 * for it to reclaim.
 *
 * The address is deliberately NOT consulted.  An earlier version
 * exempted a joiner arriving from the same address as the incumbent,
 * which let a clone in whenever it happened to reuse the address, and
 * treated "same box" as proof of "same node". */
static int ident_claimed_live(const unsigned char *ident,
		unsigned int incarn, const struct sockaddr_in *from)
{
	long long now = now_ms();
	int i;

	if (!memcmp(ident, pd_noident, 16))
		return 0;                      /* older joiner: nothing to check */
	if (!memcmp(ident, C.self_ident, 16) && !addr_eq(from, &C.self_addr))
		return 1;                      /* a clone of the master itself */
	for (i = 0; i < C.n_peers; i++) {
		struct peer *p = &C.peers[i];

		if (!__atomic_load_n(&p->node, __ATOMIC_ACQUIRE) ||
		        !p->has_ident)
			continue;
		if (now - p->last_seen_ms >= PEER_UP_MS)
			continue;              /* not up: the claim has lapsed */
		if (memcmp(ident, p->ident, 16))
			continue;
		/* Identity matches a live member - but is that member the
		 * JOINER ITSELF?  A known member re-joins as ordinary
		 * traffic, every time mastership moves, and refusing that
		 * deadlocks the fleet: the restarted node founds its own
		 * cluster, then rejects the survivor for "already holding"
		 * an identity that is genuinely its own.
		 *
		 * The incarnation settles it, and the ADDRESS deliberately
		 * does not: incarnation is regenerated per process start and
		 * never persisted, so a clone of the identity file cannot
		 * carry it.  Same identity AND same incarnation is the same
		 * running process; a different one is a new process, which
		 * is either a fast restart or a clone - and those two are
		 * told apart by waiting, not by where the datagram came
		 * from. */
		if (incarn && p->incarn == incarn)
			continue;
		return 1;
	}
	return 0;
}

/* "Alive now" and "died a moment ago" are indistinguishable for about
 * one purge window, so a duplicate is not condemned on sight: a node
 * that restarts faster than its predecessor ages out would otherwise be
 * shot for impersonating itself.  The master simply says NOTHING while
 * the answer is ambiguous - the joiner retries anyway - and only rejects
 * once the incumbent has gone on heartbeating right through the window,
 * which a dead node cannot do.
 *
 * So: a genuine restart is delayed at most until its predecessor lapses,
 * and a real clone is refused, without either outcome resting on where
 * the datagram came from. */
#define DUP_GRACE_MS (3LL * PEER_UP_MS)

static int dup_persisted(const struct sockaddr_in *from)
{
	long long now = now_ms();
	int i, slot = -1;

	for (i = 0; i < PC_CL_MAXPEER; i++) {
		if (C.dup_seen[i].addr.s_addr == from->sin_addr.s_addr) {
			if (now - C.dup_seen[i].first_ms >= DUP_GRACE_MS)
				return 1;
			return 0;
		}
		if (slot < 0 && !C.dup_seen[i].addr.s_addr)
			slot = i;
	}
	if (slot >= 0) {
		C.dup_seen[slot].addr = from->sin_addr;
		C.dup_seen[slot].first_ms = now;
	}
	return 0;
}

static void dup_forget(const struct sockaddr_in *from)
{
	int i;

	for (i = 0; i < PC_CL_MAXPEER; i++)
		if (C.dup_seen[i].addr.s_addr == from->sin_addr.s_addr) {
			C.dup_seen[i].addr.s_addr = 0;
			return;
		}
}

static void send_join_rej(const struct sockaddr_in *to, uint64_t tok,
		int reason)
{
	unsigned char msg[10];

	msg[0] = M_JOIN_REJ;
	memcpy(msg + 1, &tok, 8);
	msg[9] = (unsigned char)reason;
	seal_send(to, msg, 10);
}

/* The master has refused us outright.  A duplicate identity means two
 * nodes would claim to BE each other, and everything built on identity -
 * knowing a node restarted, deciding who to backfill, not resending a
 * keyspace twice - stops meaning anything.  Serving on regardless would
 * hand clients stale data under a name that belongs to someone else, so
 * this is the one case the daemon does stop for.  (Its standing posture
 * elsewhere is the opposite: a peer that fails AUTH is quarantined and
 * counted, never fatal - a cache node serving local traffic beats a dead
 * one.  A clone is different in kind: the damage is to the cluster's
 * shared state, not to this node's own traffic.) */
static void handle_join_rej(const unsigned char *pt, size_t n,
		const struct sockaddr_in *from)
{
	uint64_t tok;

	if (n < 9)
		return;
	memcpy(&tok, pt + 1, 8);
	if (tok != C.join_tok)
		return;                        /* not our join */
	if (n >= 10 && pt[9] == PC_REJ_DUP_IDENT) {
		LM_ERR("cluster: the master at %s REFUSED this node - its "
			"identity %s is already held by a live member, so "
			"this daemon is a clone of one.  Shutting down: two "
			"nodes sharing an identity corrupt membership.  Fix: "
			"delete node-identity in the state directory (a fresh "
			"one is generated on the next start).\n",
			inet_ntoa(from->sin_addr), C.self_ident_hex);
	} else {
		LM_ERR("cluster: the master at %s refused this node "
			"(reason %d).  Shutting down.\n",
			inet_ntoa(from->sin_addr), n >= 10 ? pt[9] : 0);
	}
	/* the ordinary shutdown path: the main thread joins the workers and
	 * tears down cleanly, rather than this thread calling exit() under
	 * everyone else's feet */
	kill(getpid(), SIGTERM);
}

static void handle_join_req(const unsigned char *pt, size_t n,
		const struct sockaddr_in *from)
{
	uint64_t tok;
	int proposed = 0;
	unsigned int j_incarn = 0;

	if (n < 9 || addr_eq(from, &C.self_addr))
		return;
	memcpy(&tok, pt + 1, 8);
	if (n >= 37)
		proposed = g16(pt + 35);   /* derived from its identity */
	if (n >= 41)
		j_incarn = g32(pt + 37);
	if (n >= 19) {
		uint64_t theirs;

		memcpy(&theirs, pt + 11, 8);
		if (theirs != config_digest() && C.cfg_authoritative) {
			/* S30: refuse the JOIN itself - no id is assigned, so
			 * the node never becomes a member at all */
			shout_mismatch(from, pt[9], pt[10], theirs);
			C.cfg_refused++;
			peer_forget(from);
			return;
		}
	}
	if (C.role == PC_ROLE_MASTER) {
		struct peer *p = peer_upsert(from);
		int id;

		if (!p) {
			LM_WARN("cluster: peer table full (%d), join from %s "
				"refused\n", PC_CL_MAXPEER,
				inet_ntoa(from->sin_addr));
			return;
		}
		if (n >= 39 && ident_claimed_live(pt + 21, j_incarn, from)) {
			char hx[33];

			ident_hex(pt + 21, hx);
			if (!dup_persisted(from)) {
				/* ambiguous: its predecessor may simply not
				 * have aged out yet.  Answer nothing; the
				 * joiner retries, and if it really did die
				 * the claim lapses and the next attempt is
				 * assigned normally. */
				LM_WARN("cluster: join from %s claims identity "
					"%s, still held by a live member - "
					"waiting to see whether that member is "
					"actually gone\n",
					inet_ntoa(from->sin_addr), hx);
				return;
			}
			LM_ERR("cluster: REFUSING join from %s - identity %s "
				"is held by a member that has kept "
				"heartbeating throughout.  A node that had "
				"died could not, so this daemon is a CLONE of "
				"it: its identity file was copied with the "
				"machine.  Delete node-identity in its state "
				"directory and restart it.\n",
				inet_ntoa(from->sin_addr), hx);
			send_join_rej(from, tok, PC_REJ_DUP_IDENT);
			C.st.joins_rejected++;
			return;
		}
		dup_forget(from);
		id = p->node ? p->node : alloc_node_id(proposed);
		if (!id)
			return;
		peer_publish(p, id, p->free_mb, 0, 0, pd_noident, 0, 0);
		send_assign(from, tok, id);
		return;
	}
	if (C.role == PC_ROLE_JOINING && addr_cmp(from, &C.self_addr) > 0)
		C.higher_joiner_ms = now_ms();   /* defer to the higher addr */
}

static void handle_assign(const unsigned char *pt, size_t n,
		const struct sockaddr_in *from)
{
	uint64_t tok;
	size_t off = 15;
	int cnt, i;

	if (n < 15)
		return;
	/* the module's guard, transplanted: a master never accepts an
	 * ASSIGN - a stale one from a pre-promotion join would clobber
	 * the identity (demotion goes through handle_master_alive only) */
	if (C.role == PC_ROLE_MASTER)
		return;
	memcpy(&tok, pt + 1, 8);
	if (tok != C.join_tok)
		return;
	C.node_id = g16(pt + 9);
	C.st.node_id = C.node_id;
	C.master_id = g16(pt + 11);
	C.master_addr = *from;
	C.master_seen_ms = now_ms();
	cnt = g16(pt + 13);
	for (i = 0; i < cnt && off + 20 <= n; i++, off += 20) {
		struct sockaddr_in pa;
		struct peer *p;
		int id = g16(pt + off);

		memset(&pa, 0, sizeof pa);
		pa.sin_family = AF_INET;
		memcpy(&pa.sin_addr.s_addr, pt + off + 2, 4);
		memcpy(&pa.sin_port, pt + off + 6, 2);
		if (addr_eq(&pa, &C.self_addr))
			continue;
		p = peer_upsert(&pa);
		if (p)
			peer_publish(p, id, g32(pt + off + 8),
				g32(pt + off + 12), g32(pt + off + 16),
				pd_noident, 0, 0);
	}
	if (C.role != PC_ROLE_MEMBER) {
		C.role = PC_ROLE_MEMBER;
		pc_node_state_set(PC_NST_READY);
		LM_NOTICE("cluster: joined as node %d (master is node %d, "
			"%d member(s))\n", C.node_id, C.master_id, cnt + 1);
	}
}

static void handle_alive(const unsigned char *pt, size_t n,
		const struct sockaddr_in *from)
{
	struct peer *p;

	if (n < 7 || addr_eq(from, &C.self_addr))
		return;
	C.st.hb_seen++;
	if (n >= 25) {
		uint64_t theirs;

		memcpy(&theirs, pt + 17, 8);
		if (theirs != config_digest() && C.cfg_authoritative) {
			/* S30: a peer whose collection config differs is NOT
			 * admitted.  Both sides refuse each other, so the
			 * fleet splits along config lines and says so - a
			 * visible split beats a silent one.
			 *
			 * Gated on THIS node being cluster-authoritative (it
			 * declared [cluster] collections).  A legacy config
			 * cannot prove what its peers should look like - it
			 * may legitimately carry node-local collections - so
			 * it keeps today's behaviour and is told to migrate
			 * instead of being broken by an upgrade. */
			shout_mismatch(from, pt[15], pt[16], theirs);
			C.cfg_refused++;
			peer_forget(from);
			return;
		}
	} else if (C.cfg_authoritative) {
		LM_WARN("cluster: peer %s sends no config digest (older "
			"build) - its collection config CANNOT be checked\n",
			inet_ntoa(from->sin_addr));
	}
	p = peer_upsert(from);
	if (p) {
		peer_publish(p, g16(pt + 1), g32(pt + 3),
			n >= 15 ? g32(pt + 7) : 0,
			n >= 15 ? g32(pt + 11) : 0,
			n >= 47 ? pt + 27 : pd_noident,
			n >= 47 ? g32(pt + 43) : 0,
			n >= 51 ? g32(pt + 47) : 0);
		if (n >= 59) {
			unsigned long long lc;

			memcpy(&lc, pt + 51, 8);
			pc_lamport_observe(lc);
		}
		if (n >= 27)
			p->client_port = g16(pt + 25);   /* S34 */
		/* B1: a peer that predates the field reads as READY - which
		 * is exactly how it behaves, since it has no state to be in */
		p->nstate = n >= 60 ? pt[59] : PC_NST_READY;
		p->mem_tier = n >= 61 ? pt[60] : 0;   /* 0 = not reported */
		p->resp_port = n >= 63 ? g16(pt + 61) : 0;   /* S49 */
	}
}

static void handle_master_alive(const unsigned char *pt, size_t n,
		const struct sockaddr_in *from)
{
	struct peer *p;
	int their_id, their_count;

	if (n < 17 || addr_eq(from, &C.self_addr))
		return;
	if (n >= 35) {
		uint64_t theirs;

		memcpy(&theirs, pt + 27, 8);
		if (theirs != config_digest() && C.cfg_authoritative) {
			/* a master we disagree with is not our master */
			shout_mismatch(from, pt[25], pt[26], theirs);
			C.cfg_refused++;
			peer_forget(from);
			return;
		}
	}
	their_id = g16(pt + 1);
	their_count = g16(pt + 3);
	if (C.role == PC_ROLE_MASTER) {
		/* split-brain cure: rank (member count, then address) */
		int mine = peers_live() + 1;

		if (their_count > mine || (their_count == mine &&
		        addr_cmp(from, &C.self_addr) > 0)) {
			LM_WARN("cluster: yielding mastership to node %d "
				"(%d members vs my %d) - re-joining\n",
				their_id, their_count, mine);
			C.role = PC_ROLE_JOINING;
			C.st.demotions++;
			C.join_tok = 0;
			randombytes_buf(&C.join_tok, 8);
			C.join_deadline_ms = now_ms() + JOIN_WAIT_MS;
			C.join_defers = 0;
			send_join_req(from);
		}
		return;
	}
	/* the keepalive is the authoritative master-identity signal */
	C.master_id = their_id;
	C.master_addr = *from;
	C.master_seen_ms = now_ms();
	p = peer_upsert(from);
	if (p)
		peer_publish(p, their_id, g32(pt + 13),
			n >= 25 ? g32(pt + 17) : 0,
			n >= 25 ? g32(pt + 21) : 0, pd_noident, 0, 0);
	if (C.role == PC_ROLE_JOINING)
		send_join_req(from);           /* fast path: join the announcer */
}

static void handle_goodbye(const unsigned char *pt, size_t n,
		const struct sockaddr_in *from)
{
	struct peer *p;
	int i;

	(void)pt;                          /* the address is the identity */
	if (n < 3 || addr_eq(from, &C.self_addr))
		return;
	for (i = 0; i < C.n_peers; i++) {
		p = &C.peers[i];
		if (addr_eq(&p->addr, from)) {
			__atomic_store_n(&p->node, 0, __ATOMIC_RELEASE);
			shard_note_change();
			break;
		}
	}
	if (C.role != PC_ROLE_MASTER && addr_eq(from, &C.master_addr))
		C.master_seen_ms = 0;          /* master left: elect at once */
}

/* the 1s membership cadence + the join FSM (called from the thread) */
static void membership_tick(void)
{
	long long now = now_ms();
	int i;

	/* purge silent peers */
	for (i = 0; i < C.n_peers; i++)
		if (C.peers[i].node &&
		        now - C.peers[i].last_seen_ms >= PEER_PURGE_MS) {
			__atomic_store_n(&C.peers[i].node, 0, __ATOMIC_RELEASE);
			shard_note_change();
		}

	switch (C.role) {
	case PC_ROLE_JOINING:
		if (now >= C.join_deadline_ms) {
			if (now - C.higher_joiner_ms < 2000 &&
			        C.join_defers < JOIN_DEFER_MAX) {
				C.join_defers++;
				C.join_deadline_ms = now + JOIN_DEFER_MS;
				send_join_req(NULL);
				LM_INFO("cluster: higher-address joiner active - "
					"deferring (%d/%d)\n", C.join_defers,
					JOIN_DEFER_MAX);
			} else {
				become_master(1);
			}
		} else if (now - C.join_last_ms >= 1000) {
			send_join_req(NULL);       /* re-announce while waiting */
		}
		break;
	case PC_ROLE_MEMBER:
		send_alive();
		if (now - C.master_seen_ms >= MASTER_DEAD_MS) {
			/* the highest live (ip,port) self-promotes; stickiness
			 * holds because a live master never reaches this path */
			int best = 1;

			for (i = 0; i < C.n_peers; i++)
				if (C.peers[i].node &&
				        now - C.peers[i].last_seen_ms < MASTER_DEAD_MS &&
				        addr_cmp(&C.peers[i].addr, &C.self_addr) > 0) {
					best = 0;
					break;
				}
			if (best)
				become_master(0);
			else
				C.master_seen_ms = now - MASTER_DEAD_MS + 1000;
				/* re-check in ~1s: the winner's MASTER_ALIVE
				 * arrives, or the live set shrank to us */
		}
		break;
	case PC_ROLE_MASTER:
		send_alive();
		send_master_alive();
		/* Publish whatever the standby has already acknowledged, and
		 * abandon anything it has left hanging.  Then stage the next
		 * change: every few seconds rather than only on change, so a
		 * node that missed a datagram catches up without anyone
		 * tracking who has what, and immediately when the membership
		 * has moved, because ownership follows the map and a stale
		 * one keeps routing at a node that has gone. */
		clmap_tick(now);
		if (now - C.map_pub_ms >= CLMAP_PUB_MS ||
		        clmap_membership_moved())
			clmap_stage();
		break;
	default:
		break;
	}
	/* liveness moves without a map arriving, so the verdict is retaken
	 * every tick rather than only when one does - a stale "usable" is
	 * how the two bases end up disagreeing and forking a re-write */
	clmap_recheck_usable();
	reconcile_tick();
	C.st.node_id = C.node_id;
	C.st.role = C.role;
	C.st.state = pc_node_state();
	C.st.map_valid = C.map_valid;
	C.st.map_term = C.map_valid ? C.map.term : 0;
	C.st.map_seq = C.map_valid ? C.map.seq : 0;
	C.st.map_nodes = C.map_valid ? C.map.nnodes : 0;
	C.st.map_master = C.map_valid ? C.map.master_id : 0;
	C.st.map_pub = C.map_pub_n;
	C.st.map_rx = C.map_rx_n;
	C.st.map_stale = C.map_stale_n;
	C.st.map_bad = C.map_bad_n;
	/* the published answer, not this node's local variable: a member
	 * has no opinion of its own and would otherwise report "no standby"
	 * for a cluster that has one */
	C.st.backup_id = C.map_valid ? C.map.backup_id : C.backup_id;
	C.st.hist_n = C.hist.n;
	C.st.held_valid = C.held_valid;
	C.st.held_hist_n = C.held_hist.n;
	C.st.held_term = C.held_term;
	C.st.held_seq = C.held_seq;
	C.st.sync_sent = C.sync_sent_n;
	C.st.sync_rx = C.sync_rx_n;
	C.st.sync_ack = C.sync_ack_n;
	C.st.sync_bad = C.sync_bad_n;
	C.st.map_usable = C.map_usable;
	C.st.place_map = C.place_map_n;
	C.st.place_hrw = C.place_hrw_n;
	C.st.master_id = C.role == PC_ROLE_MASTER ? C.node_id : C.master_id;
}

static void handle_fwd_json(const unsigned char *pt, size_t n,
		const struct sockaddr_in *from);
static void handle_fwd_jack(const unsigned char *pt, size_t n);

/* drain and dispatch every queued datagram (peer thread only).  Also
 * the migration pump's wait step: acks arriving here open the send
 * window.  Returns the number of datagrams handled. */
/* Fairness bound on a single socket's drain.  A firehose on one
 * socket - migration ingest, where the donor's ack window refills it
 * continuously and every record costs a store - must not keep this
 * loop from the OTHER socket and the 1 Hz membership tick: an
 * observer that reads data for longer than MASTER_DEAD_MS convicts a
 * live master of death and promotes over it (the RECEIVER-DEAF half
 * of the donor-starvation pathology; the emit half has beat_pump and
 * the watchdog thread, whose silence in the daemon logs is what
 * convicted this half).  Poll is level-triggered - pending data
 * re-wakes the loop instantly - so the cap costs nothing in
 * throughput; it only guarantees the loop breathes. */
#define DRAIN_BATCH 64

static int drain_one_fd(int fd)
{
	unsigned char buf[HDR_LEN + MAX_DGRAM + 64];
	unsigned char pt[MAX_DGRAM + 16];
	int handled = 0, seen = 0;

	while (seen++ < DRAIN_BATCH) {
		struct sockaddr_in from;
		socklen_t flen = sizeof from;
		unsigned long long ptlen = 0;
		ssize_t r = recvfrom(fd, buf, sizeof buf, 0,
			(struct sockaddr *)&from, &flen);

		if (r <= 0)
			break;
		if (open_dgram(buf, (size_t)r, pt, &ptlen) != 0) {
			/* the quarantine posture: count, warn (rarely), drop */
			if (++C.st.bad_auth % 1000 == 1)
				LM_WARN("cluster: dropping unauthenticated "
					"datagrams (%llu so far)\n", C.st.bad_auth);
			continue;
		}
		if (!ptlen)
			continue;
		handled++;
		switch (pt[0]) {
		case M_PULL_REQ:  handle_pull_req(pt, ptlen, &from); break;
		case M_PULL_RSP:  handle_pull_rsp(pt, ptlen); break;
		case M_TOMBSTONE: handle_tombstone(pt, ptlen); break;
		case M_FWD_OP:    handle_fwd_op(pt, ptlen, &from); break;
		case M_FWD_ACK:   handle_fwd_ack(pt, ptlen); break;
		case M_FWD_JSON:  handle_fwd_json(pt, ptlen, &from); break;
		case M_FWD_JACK:  handle_fwd_jack(pt, ptlen); break;
		case M_MIGRATE:   handle_migrate(pt, ptlen, &from); break;
		case M_MIGRATE_MANY: handle_migrate_many(pt, ptlen, &from, 0); break;
		case M_REPL_MANY:    handle_migrate_many(pt, ptlen, &from, 1); break;
		case M_MIGRATE_ACK: {
			/* ANY ack frees a window slot; the stored-count (u16
			 * after the ok byte) credits migrated_out in RECORDS */
			uint32_t mreq;
			size_t j;

			if (ptlen < 6)
				break;
			mreq = g32(pt + 1);
			for (j = 0; j < sizeof C.mig_out / sizeof C.mig_out[0];
			        j++)
				if (C.mig_out[j].req == mreq) {
					C.mig_out[j].req = 0;
					C.mig_out[j].recs = 0;
					break;
				}
			if (C.mig_inflight > 0)
				C.mig_inflight--;
			if (ptlen >= 8)
				C.px.migrated_out += g16(pt + 6);
			else if (pt[5])
				C.px.migrated_out++;
			break; }
		case M_DEMOTE:    handle_demote(pt, ptlen); break;
		case M_JOIN_REQ:     handle_join_req(pt, ptlen, &from); break;
		case M_JOIN_REJ:     handle_join_rej(pt, ptlen, &from); break;
		case M_ASSIGN:       handle_assign(pt, ptlen, &from); break;
		case M_MASTER_ALIVE: handle_master_alive(pt, ptlen, &from); break;
		case M_ALIVE:        handle_alive(pt, ptlen, &from); break;
		case M_GOODBYE:      handle_goodbye(pt, ptlen, &from); break;
		case M_CLMAP:        handle_clmap(pt, ptlen, &from); break;
		case M_CLSYNC:       handle_clsync(pt, ptlen, &from); break;
		case M_CLSYNC_ACK:   handle_clsync_ack(pt, ptlen); break;
		default:
			break;
		}
	}
	return handled;
}

static int drain_dgrams(void)
{
	int n = drain_one_fd(C.fd);

	if (C.mfd >= 0)
		n += drain_one_fd(C.mfd);
	return n;
}

void pc_cluster_thread(volatile int *stop)
{
	struct pollfd pf[2];
	long long last_hb = 0;

	pf[0].fd = C.fd;
	pf[0].events = POLLIN;
	pf[1].fd = C.mfd;
	pf[1].events = POLLIN;
	while (!*stop) {
		long long now = now_ms();

		if (now - last_hb >= 1000) {
			static int tick;

			membership_tick();
			expire_pending();
			if (++tick % 10 == 0)
				pc_rebalance_tick();
			last_hb = now;
		}
		/* the join FSM needs sub-second deadlines */
		if (C.role == PC_ROLE_JOINING && now >= C.join_deadline_ms)
			membership_tick();
		if (poll(pf, 2, 100) <= 0)
			continue;
		drain_dgrams();
	}
	if (C.role != PC_ROLE_JOINING) {
		pc_node_state_set(PC_NST_DRAINING);
		send_goodbye();                /* leave cleanly: peers purge now */
	}
}

/* ---- forwarded JSON path ops (proxy mode) ------------------------------- */

uint32_t pc_fwd_json_begin(int node, int jop, const char *col,
		size_t collen, const char *key, size_t klen,
		const char *path, size_t plen, const char *val, size_t vlen,
		long long by, int have_ttl, long long ttl, int nx, int xx,
		int mkpath)
{
	unsigned char msg[64 + 256 + 4096 + 512 + MAX_FWD_VAL];
	struct pending *slot;
	struct peer *pr = peer_by_node(node);
	uint32_t req;
	size_t n;

	if (!C.enabled || !pr || collen > 255 || klen > 4096 || plen > 511 ||
	        vlen > MAX_FWD_VAL)
		return 0;
	req = pend_alloc(PC_DONE_JSON, 1, 700, &slot);
	if (!req)
		return 0;
	slot->jop = (unsigned char)jop;
	msg[0] = M_FWD_JSON;
	p32(msg + 1, req);
	p16(msg + 5, (uint16_t)C.node_id);
	msg[7] = (unsigned char)jop;
	msg[8] = (unsigned char)((nx ? 1 : 0) | (xx ? 2 : 0) |
		(mkpath ? 4 : 0) | (have_ttl ? 8 : 0));
	p32(msg + 9, (uint32_t)(ttl > 0 ? ttl : 0));
	memcpy(msg + 13, &by, 8);          /* LE hosts only, like delta */
	msg[21] = (unsigned char)collen;
	p16(msg + 22, (uint16_t)klen);
	p16(msg + 24, (uint16_t)plen);
	p32(msg + 26, (uint32_t)vlen);
	memcpy(msg + 30, col, collen);
	memcpy(msg + 30 + collen, key, klen);
	memcpy(msg + 30 + collen + klen, path, plen);
	if (vlen)
		memcpy(msg + 30 + collen + klen + plen, val, vlen);
	n = 30 + collen + klen + plen + vlen;
	seal_send(&pr->addr, msg, n);
	return req;
}

static void handle_fwd_json(const unsigned char *pt, size_t n,
		const struct sockaddr_in *from)
{
	extern int pc_json_rmw(pcache_htable_t *, const char *, int, int,
		const char *, int, const char *, int, long long, int,
		long long, int, int, int, char **, int *, long long *,
		int *, const char **);
	unsigned char ack[24 + MAX_FWD_VAL];
	pcache_htable_t *ht;
	uint32_t req;
	unsigned int cn, kn, pl2, vl;
	long long by, newval = 0;
	int jop, flags, rc, fraglen = 0, cnt = 0, st;
	char *frag = NULL, colz[256], pathz[512];
	const char *emsg = NULL;

	if (n < 30)
		return;
	req = g32(pt + 1);
	jop = pt[7];
	flags = pt[8];
	by = 0;
	memcpy(&by, pt + 13, 8);
	cn = pt[21];
	kn = g16(pt + 22);
	pl2 = g16(pt + 24);
	vl = g32(pt + 26);
	if (30 + cn + kn + pl2 + vl > n || cn > 255 || pl2 > 511)
		return;
	memcpy(colz, pt + 30, cn);
	colz[cn] = 0;
	memcpy(pathz, pt + 30 + cn + kn, pl2);
	pathz[pl2] = 0;
	ht = pc_store_find(colz, cn);
	if (!ht) {
		st = 2;
	} else {
		rc = pc_json_rmw(ht, (const char *)pt + 30 + cn, (int)kn,
			jop, pathz, (int)pl2,
			(const char *)pt + 30 + cn + kn + pl2, (int)vl,
			by, (flags & 8) ? 1 : 0, (long long)g32(pt + 9),
			flags & 1, (flags & 2) ? 1 : 0, (flags & 4) ? 1 : 0,
			&frag, &fraglen, &newval, &cnt, &emsg);
		st = rc == 0 ? 0 : rc == 1 ? 1 : 2;
	}
	if (fraglen > (int)MAX_FWD_VAL) {  /* fragment too big to ship */
		st = 2;
		fraglen = 0;
	}
	ack[0] = M_FWD_JACK;
	p32(ack + 1, req);
	ack[5] = (unsigned char)st;
	ack[6] = (unsigned char)jop;
	memcpy(ack + 7, &newval, 8);
	p32(ack + 15, (uint32_t)cnt);
	p32(ack + 19, st == 0 ? (uint32_t)fraglen : 0);
	if (st == 0 && fraglen)
		memcpy(ack + 23, frag, (size_t)fraglen);
	free(frag);
	seal_send(from, ack, 23 + (size_t)(st == 0 ? fraglen : 0));
}

static void handle_fwd_jack(const unsigned char *pt, size_t n)
{
	uint32_t req;
	long long newval = 0;
	unsigned int fraglen;
	int worker = -1, kind = 0, st, jop, cnt;

	if (n < 23)
		return;
	req = g32(pt + 1);
	st = pt[5];
	jop = pt[6];
	memcpy(&newval, pt + 7, 8);
	cnt = (int)g32(pt + 15);
	fraglen = g32(pt + 19);
	if (23 + fraglen > n)
		return;
	pthread_mutex_lock(&C.pmx);
	{
		struct pending *p = pend_find(req);

		if (p) {
			worker = p->worker;
			kind = p->kind;
			pend_release(p);
		}
	}
	pthread_mutex_unlock(&C.pmx);
	if (worker < 0 || kind != PC_DONE_JSON)
		return;
	post_done_ex(worker, req, PC_DONE_JSON, st == 0, st, newval, 0, 0,
		fraglen ? pt + 23 : NULL, (int)fraglen, jop, cnt, 0, 0);
}

/* ---- the rebalancer (donor-initiated, hysteresis, byte budget) ---------- */

void pc_proxy_get_stats(struct pc_proxy_stats *out)
{
	*out = C.px;
}

struct mig_ctx {
	const char *col;
	struct peer *to;
	long long budget;
	unsigned int now;
	unsigned int min_age;          /* coldest-first threshold, 0 = all */
	int shard;                     /* reshard run: move ONLY the keys
	                                * whose HRW owner is @to */
	int repl;                      /* eager store: COPY records newer
	                                * than @since to @to - no stubbing,
	                                * the local record stays */
	unsigned int since;
	int incl_passive;              /* one-shot backfill: send copies too */
	int stopped;                   /* the walk hit the budget */
	const char *only_col;          /* restrict the run to one collection
	                                * (per-collection marks), NULL = all */
	long long scan_deadline_ms;    /* liveness ceiling on the walk: a
	                                * tick must never cost heartbeats,
	                                * victims not reached resume next
	                                * tick (0 = uncapped) */
	unsigned scanned;              /* records seen, for the pump gate */
};

static int mig_cb(const str *key, const str *val, unsigned int exp,
		unsigned int wtick, unsigned char rflags, unsigned long long ver,
		void *arg)
{
	struct mig_ctx *m = arg;
	unsigned char *msg;
	pcache_htable_t *ht;
	unsigned int cn = (unsigned int)strlen(m->col), ttl_left = 0;
	uint32_t req;
	size_t n;
	str k;
	extern void pc_wal_del(const char *, const char *, int);

	if ((m->scanned++ & 511) == 0) {
		beat_pump();
		if (m->scan_deadline_ms && now_ms() >= m->scan_deadline_ms) {
			m->stopped = 1;
			return -1;             /* liveness ceiling: resume next
			                        * tick rather than starve beats */
		}
	}
	if (m->budget <= 0) {
		m->stopped = 1;
		return -1;                     /* budget spent: stop the walk */
	}
	if (exp) {
		if (exp <= m->now + 60)
			return 0;                  /* dying soon: not worth moving */
		ttl_left = exp - m->now;
	}
	if (key->len > 4096)
		return 0;
	if (m->repl) {
		/* eager: only records this node AUTHORED (a pushed or pulled
		 * copy is PASSIVE - propagating those back would echo the
		 * keyspace around the fleet forever), only records newer
		 * than this peer's mark, and only what the datagram plane
		 * carries (oversized counted) */
		if ((rflags & PCACHE_F_PASSIVE) && !m->incl_passive)
			return 0;
		if (wtick <= m->since)
			return 0;
		if (val->len > MAX_FWD_VAL) {
			C.px.migrate_skipped_big++;
			return 0;
		}
	}
	if (m->shard) {
		/* the ownership pass: shard keys move by HRW owner */
		if (pc_shard_owner(m->col, cn, key->s,
		        (size_t)key->len) != m->to->node)
			return 0;
	}
	/* coldest-first: pass A computed a minimum AGE for this tick's
	 * victims - younger records stay put */
	if (m->min_age) {
		unsigned int age = m->now >= wtick ? m->now - wtick
			: 0xffffffffu;

		if (wtick && age < m->min_age)
			return 0;
	}

	/* STUB FIRST, never reinstate: delete locally + record the receiver
	 * in the locator BEFORE sending - a lost transfer is a miss, a fork
	 * is impossible (the decided invariant).  A REPLICATION run copies:
	 * the local record stays, a lost datagram just retries next tick
	 * (the mark only advances on a clean pass) */
	ht = pc_store_find(m->col, cn);
	if (!ht)
		return -1;
	k.s = key->s;
	k.len = key->len;
	if (!m->repl) {
		if (pcache_ht_remove(ht, &k) != 1)
			return 0;
		pc_wal_del(m->col, key->s, key->len);
		pc_loc_set(m->col, cn, key->s, (size_t)key->len, m->to->node);
	} else {
		C.px.repl_out++;
	}

	n = MIG_RHDR + cn + (size_t)key->len + (size_t)val->len;

	/* records above the datagram ceiling route to the BULK TCP batch
	 * (same record format; the bulk thread streams it) - the 58KB
	 * migration ceiling is gone */
	if (val->len > MAX_FWD_VAL) {
		if (C.bulkb_len + n > MIG_BATCH_CAP)
			return -1;
		{
			unsigned char *rp = C.bulkb + C.bulkb_len;

			p32(rp, ttl_left);
			rp[4] = (unsigned char)cn;
			p16(rp + 5, (uint16_t)key->len);
			p32(rp + 7, (uint32_t)val->len);
			p64(rp + 11, ver);
			memcpy(rp + MIG_RHDR, m->col, cn);
			memcpy(rp + MIG_RHDR + cn, key->s, (size_t)key->len);
			memcpy(rp + MIG_RHDR + cn + key->len, val->s,
				(size_t)val->len);
		}
		C.bulkb_len += n;
		C.bulk_recs++;
		m->budget -= (long long)n;
		return 0;
	}

	/* COLLECT, do not send: the pump in pc_rebalance_tick paces these
	 * through the ack window after the scan releases its locks.
	 * Records are GATHERED (M_MIGRATE_MANY): appended to the open group
	 * until the datagram cap, so a 256B record costs ~1/200th of a
	 * datagram+ack cycle instead of a whole one.  Per record:
	 * [ttl4][cn1][klen2][vlen4][ver8][col][key][val]. */
	if (C.mig_open != (size_t)-1) {
		size_t glen = g16(C.migb + C.mig_open);

		/* close a non-empty group the record no longer fits */
		if (glen + n > MIG_GATHER_CAP)
			C.mig_open = (size_t)-1;
	}
	if (C.mig_open == (size_t)-1) {
		/* open a fresh group: [len2][GHDR: type req4 node2 count2] */
		if (C.migb_len + 2 + MIG_GHDR + n > MIG_BATCH_CAP)
			return -1;
		/* 31 bits: PEND_TAG is what marks a parked-request handle,
		 * and a migration group id must never look like one */
		req = ++C.next_req & ~PEND_TAG;
		if (!req)
			req = ++C.next_req & ~PEND_TAG;
		C.mig_open = C.migb_len;
		msg = C.migb + C.migb_len + 2;
		msg[0] = m->repl ? M_REPL_MANY : M_MIGRATE_MANY;
		p32(msg + 1, req);
		p16(msg + 5, (uint16_t)C.node_id);
		p16(msg + 7, 0);
		p16(C.migb + C.mig_open, MIG_GHDR);
		C.migb_len += 2 + MIG_GHDR;
	}
	if (C.migb_len + n > MIG_BATCH_CAP)
		return -1;
	{
		unsigned char *rp = C.migb + C.migb_len;
		unsigned char *gh = C.migb + C.mig_open;

		p32(rp, ttl_left);
		rp[4] = (unsigned char)cn;
		p16(rp + 5, (uint16_t)key->len);
		p32(rp + 7, (uint32_t)val->len);
		p64(rp + 11, ver);
		memcpy(rp + MIG_RHDR, m->col, cn);
		memcpy(rp + MIG_RHDR + cn, key->s, (size_t)key->len);
		memcpy(rp + MIG_RHDR + cn + key->len, val->s, (size_t)val->len);
		C.migb_len += n;
		p16(gh, (uint16_t)(g16(gh) + n));           /* group length */
		p16(gh + 2 + 7, (uint16_t)(g16(gh + 2 + 7) + 1)); /* count */
	}
	m->budget -= (long long)n;
	return 0;
}

/* coldest-first pass A: a log2-age byte histogram - selecting the age
 * threshold whose older side covers the tick budget, without copying a
 * single key */
struct age_hist {
	unsigned long long bytes[32];  /* age is 32-bit: floor(log2) fits
	                                * 0..31 - a 64-bucket histogram was
	                                * how the shift-by-32 UB (S56) and
	                                * its 1u << 63 sibling in
	                                * pick_min_age stayed plausible */
	unsigned int now;
};

static int age_cb(const str *key, const str *val, unsigned int exp,
		unsigned int wtick, unsigned char rflags, unsigned long long ver,
		void *arg)
{
	(void)rflags;
	(void)ver;
	struct age_hist *h = arg;
	unsigned int age, b;

	(void)exp;
	if (key->len > 4096)
		return 0;
	age = (!wtick || h->now < wtick) ? 0xffffffffu : h->now - wtick;
	b = 0;
	while (b < 31 && (age >> (b + 1)))
		b++;                           /* b = floor(log2(age)), 0..31.
	                                    * age is 32-BIT: the old b < 63
	                                    * bound reached age >> 32, which
	                                    * is UB - x86 wraps the shift and
	                                    * the 0xffffffff never-written
	                                    * sentinel landed in bucket 63 by
	                                    * accident; UBSan halted the
	                                    * daemon here on its first
	                                    * rebalance tick instead.  At 31
	                                    * the sentinel still lands at the
	                                    * coldest end. */
	h->bytes[b] += 11u + (unsigned int)key->len +
		(unsigned int)val->len;
	return 0;
}

static unsigned int pick_min_age(pcache_htable_t *ht, unsigned int now,
		long long budget)
{
	struct age_hist h;
	unsigned long long acc = 0;
	int b;

	memset(&h, 0, sizeof h);
	h.now = now;
	pcache_ht_iter_meta(ht, age_cb, &h);
	for (b = 31; b >= 0; b--) {
		acc += h.bytes[b];
		if ((long long)acc >= budget)
			return b ? 1u << b : 0;
	}
	return 0;                          /* everything fits: no threshold */
}

/* one migration run against @to: collect victims across the matching
 * collections (@shard = deterministic-ownership filter, else the proxy
 * coldest-first pass) and pump them through the ack-gated window plus
 * the bulk handoff.  Returns nonzero when anything was collected. */
static int run_migration(struct peer *to, long long budget, int mode,
		unsigned int since, const char *only_col, int *clean,
		unsigned int *cursor, int incl_passive)
{
	struct mig_ctx m;
	int i, moved = 0;

	m.to = to;
	m.budget = budget;
	m.shard = mode == 1;
	m.repl = mode == 2;
	m.since = since;
	m.incl_passive = incl_passive;
	m.stopped = 0;
	m.only_col = only_col;
	m.now = get_ticks();
	m.scanned = 0;
	m.scan_deadline_ms = now_ms() + 1500;
	if (clean)
		*clean = 0;
	if (!C.migb) {
		C.migb = malloc(MIG_BATCH_CAP);
		if (!C.migb)
			return 0;
	}
	if (!C.bulkb) {
		C.bulkb = malloc(MIG_BATCH_CAP);
		if (!C.bulkb)
			return 0;
	}
	C.migb_len = 0;
	C.bulkb_len = 0;
	C.bulk_recs = 0;
	C.mig_inflight = 0;
	C.mig_open = (size_t)-1;
	memset(C.mig_out, 0, sizeof C.mig_out);
	for (i = 0; i < pc_store_count() && m.budget > 0; i++) {
		pcache_htable_t *ht = pc_store_ht(i);

		if (m.repl ? !pc_store_eager_enabled(ht)
		    : m.shard ? !pc_store_shard_enabled(ht)
		              : !pc_store_proxy_enabled(ht))
			continue;
		if (m.only_col && strcmp(m.only_col, pc_store_name(i)))
			continue;
		m.col = pc_store_name(i);
		/* reshard/replication move by OWNERSHIP/AGE MARK, never
		 * temperature */
		m.min_age = (m.shard || m.repl) ? 0
			: pick_min_age(ht, m.now, m.budget);
		if (cursor)
			pcache_ht_iter_meta_from(ht, cursor, mig_cb, &m);
		else
			pcache_ht_iter_meta(ht, mig_cb, &m);
	}
	moved = C.migb_len > 0 || C.bulkb_len > 0;

	/* the pump: send the collected payloads through an ack-gated window
	 * so the receiver's UDP buffer is never overrun (victims are
	 * already stubbed - anything undeliverable within the deadline is a
	 * lost transfer = a miss by the invariant, counted honestly) */
	if (C.migb_len) {
		long long deadline = now_ms() + 5000;
		size_t off = 0;
		unsigned int sent = 0, lost = 0;
		struct pollfd pf = { .fd = C.fd, .events = POLLIN };

		while (off + 2 <= C.migb_len) {
			unsigned int n = g16(C.migb + off);
			size_t j;

			while (C.mig_inflight >= MIG_WINDOW &&
			        now_ms() < deadline) {
				if (poll(&pf, 1, 20) > 0)
					drain_dgrams();
				beat_pump();   /* the ack window may poll for as
				                * long as PEER_UP_MS - never mute */
			}
			if (now_ms() >= deadline)
				break;
			seal_send(&m.to->addr, C.migb + off + 2, n);
			/* remember req -> record-count while it is in flight,
			 * so a lost gathered datagram is charged in RECORDS */
			for (j = 0; j < sizeof C.mig_out /
			        sizeof C.mig_out[0]; j++)
				if (!C.mig_out[j].req) {
					C.mig_out[j].req =
						g32(C.migb + off + 2 + 1);
					C.mig_out[j].recs =
						g16(C.migb + off + 2 + 7);
					break;
				}
			C.mig_inflight++;
			C.px.migrate_dgrams++;
			sent++;
			off += 2 + n;
		}
		while (C.mig_inflight > 0 && now_ms() < deadline) {
			if (poll(&pf, 1, 20) > 0)
				drain_dgrams();
			beat_pump();
		}
		/* unsent groups + unacked sends are the losses, in records */
		while (off + 2 <= C.migb_len) {
			unsigned int n = g16(C.migb + off);

			lost += g16(C.migb + off + 2 + 7);
			off += 2 + n;
		}
		{
			size_t j;

			for (j = 0; j < sizeof C.mig_out /
			        sizeof C.mig_out[0]; j++)
				if (C.mig_out[j].req)
					lost += C.mig_out[j].recs;
		}
		if (lost) {
			C.px.migrate_lost += lost;
			LM_WARN("rebalancer: %u record(s) unconfirmed across "
				"%u datagram(s) (receiver slow or unreachable) "
				"- stubs stand, the keys read as misses\n",
				lost, sent);
		}
		C.migb_len = 0;
		C.mig_inflight = 0;
		if (clean)
			/* with a cursor the walk finishing is reported by
			 * the cursor itself, so this is purely "did this
			 * slice lose anything" */
			*clean = cursor ? lost == 0 : (!m.stopped && lost == 0);
	} else if (clean) {
		*clean = cursor ? 1 : !m.stopped;  /* nothing to send: clean
		                                * = the peer is in sync */
	}

	/* oversized victims (already stubbed) go to the bulk thread */
	if (C.bulkb_len) {
		pthread_mutex_lock(&C.bmx);
		if (!C.bulk_tx) {
			C.bulk_tx = C.bulkb;
			C.bulk_tx_len = C.bulkb_len;
			C.bulk_tx_recs = C.bulk_recs;
			C.bulk_tx_to = m.to->addr;
			C.bulkb = NULL;        /* re-alloc next tick */
		} else {
			/* the bulk thread is still on the last batch: these
			 * stubs are lost - honest misses, counted */
			C.px.migrate_lost += C.bulk_recs;
			LM_WARN("rebalancer: bulk channel busy, %u oversized "
				"record(s) dropped to misses\n", C.bulk_recs);
		}
		pthread_mutex_unlock(&C.bmx);
		C.bulkb_len = 0;
		C.bulk_recs = 0;
	}
	return moved;
}

/* eager store: push records newer than each peer's mark, per
 * collection - the background synchronizer.  The mark advances to the
 * sweep's start tick only after a CLEAN pass (full walk, no losses),
 * so a lost datagram or a budget cut simply retries next tick;
 * re-pushes are idempotent upserts.  A rejoined peer's marks are
 * zeroed with its slot: automatic full resync. */
/* Am I the lowest-numbered live member other than @target?  Used to pick
 * exactly one backfiller, so a node that came back empty is not sent the
 * whole keyspace once per surviving peer. */
static int lowest_live_sender(int target)
{
	long long now = now_ms();
	int i;

	for (i = 0; i < C.n_peers; i++) {
		struct peer *p = &C.peers[i];
		int id = __atomic_load_n(&p->node, __ATOMIC_ACQUIRE);

		if (!id || id == target)
			continue;
		if (now - p->last_seen_ms >= PEER_UP_MS)
			continue;
		if (id < C.node_id)
			return 0;              /* someone smaller will do it */
	}
	return 1;
}

static void eager_repl_tick(void)
{
	long long now = now_ms();
	int i, c;

	for (c = 0; c < pc_store_count(); c++) {
		if (!pc_store_eager_enabled(pc_store_ht(c)))
			continue;
		for (i = 0; i < C.n_peers; i++) {
			struct peer *pr = &C.peers[i];
			unsigned int start;
			int clean = 0, bf;

			if (!pr->node ||
			        now - pr->last_seen_ms >= PEER_UP_MS)
				continue;
			/* one budgeted SLICE of a resumable cycle.  The
			 * mark may only advance when a whole cycle has been
			 * walked with nothing lost - so the cycle's start
			 * tick is captured when the cursor is at 0, not on
			 * every tick, or records written mid-cycle would be
			 * skipped by the next pass. */
			if (pr->repl_cursor[c] == 0) {
				pr->repl_cycle[c] = get_ticks();
				pr->repl_dirty[c] = 0;
			}
			start = pr->repl_cycle[c];
			/* ONE designated sender for a backfill.  Every live
			 * peer holds the same copies, so if they all pushed
			 * at once the empty node would receive the keyspace
			 * N times over.  The lowest live node id does it -
			 * deterministic, needs no agreement, and does not
			 * care which node is master. */
			bf = pr->backfill[c] && lowest_live_sender(pr->node);
			run_migration(pr, 4 << 20, 2, pr->repl_mark[c],
				pc_store_name(c), &clean, &pr->repl_cursor[c],
				bf);
			if (!clean)
				pr->repl_dirty[c] = 1;
			if (pr->repl_cursor[c] == 0 && !pr->repl_dirty[c]) {
				pr->repl_mark[c] = start ? start - 1 : 0;
				/* a whole cycle walked cleanly: the copies
				 * are delivered, so stop resending them */
				if (bf) {
					pr->backfill[c] = 0;
					LM_NOTICE("cluster: backfilled node "
						"%d after its restart\n",
						pr->node);
				}
			}
		}
	}
}

/* deterministic ownership maintenance: after a membership change each
 * node ships the keys whose HRW owner is now a peer straight to that
 * owner, budgeted per peer per tick.  The shard grace self-extends
 * while records still flow, so readers keep their broadcast fallback
 * until the fleet settles. */
static void shard_reshard_tick(void)
{
	long long now = now_ms();
	int i, have = 0;

	for (i = 0; i < pc_store_count(); i++) {
		if (pc_store_shard_enabled(pc_store_ht(i))) {
			have = 1;
			break;
		}
	}
	if (!have)
		return;
	for (i = 0; i < C.n_peers; i++) {
		if (!C.peers[i].node ||
		        now - C.peers[i].last_seen_ms >= PEER_UP_MS)
			continue;
		if (run_migration(&C.peers[i], 4 << 20, 1, 0, NULL, NULL, NULL, 0))
			shard_note_change();
	}
}

void pc_rebalance_tick(void)
{
	unsigned int myutil = upml(self_live_kb(), self_total_mb());
	unsigned int sum = myutil, mean, best_util = 1000, excess;
	struct peer *best = NULL;
	int i, n = 1;

	if (!C.enabled)
		return;
	shard_reshard_tick();              /* ownership first: deterministic
	                                    * moves are not load leveling */
	eager_repl_tick();                 /* then the eager synchronizer */
	for (i = 0; i < C.n_peers; i++) {
		unsigned int u;

		if (!C.peers[i].node || !C.peers[i].total_mb ||
		        now_ms() - C.peers[i].last_seen_ms >= PEER_UP_MS)
			continue;
		u = upml(C.peers[i].live_kb, C.peers[i].total_mb);
		sum += u;
		n++;
		/* equal utilization ties break toward more ABSOLUTE free -
		 * else a tiny empty node wins over a huge empty one and the
		 * data double-hops through it (measured: B->A->C cascade) */
		if (u < best_util || (u == best_util && best &&
		        C.peers[i].free_mb > best->free_mb)) {
			best_util = u;
			best = &C.peers[i];
		}
	}
	if (n < 2 || !best)
		return;
	mean = sum / (unsigned int)n;
	/* the leveling metric is UTILIZATION (used/total), not absolute
	 * free bytes (REVISED on the user's call - the absolute metric
	 * drained small nodes to zero next to a big one): equal arenas
	 * level to equal key counts; mixed arenas to capacity-PROPORTIONAL
	 * shares.  Shed only when meaningfully fuller than the fleet mean,
	 * toward the least-utilized peer, and only by the EXCESS - the
	 * budget then caps churn, and overshoot oscillation is impossible
	 * by construction. */
	if (myutil <= mean + 50 || best_util >= myutil)
		return;                        /* band: 50 per-mille = 5% */
	{
		long long budget = 4 << 20;    /* 4MB per 10s tick */

		excess = (myutil - mean) * self_total_mb() / 1000;
		if ((long long)excess << 20 < budget)
			budget = (long long)excess << 20;
		if (budget <= 0)
			return;
		run_migration(best, budget, 0, 0, NULL, NULL, NULL, 0);
	}
}

/* ---- the bulk TCP plane (records above the datagram ceiling) ------------
 * Cluster-principal Noise sessions on advertise:port/TCP.  The peer
 * thread's tick stubs oversized victims into a batch; this thread owns
 * every socket - connects, handshakes, streams, and accepts inbound
 * transfers - so membership never stalls on TCP timeouts.  Stream:
 * [count u32] then M_MIGRATE_MANY-format records; the ack is one
 * [stored u32] record. */

static int bulk_io(int fd, int wr, void *buf, size_t n)
{
	size_t off = 0;

	while (off < n) {
		ssize_t r = wr ? write(fd, (char *)buf + off, n - off)
		               : read(fd, (char *)buf + off, n - off);

		if (r <= 0) {
			if (r < 0 && errno == EINTR)
				continue;
			return -1;
		}
		off += (size_t)r;
	}
	return 0;
}

static int bulk_send_stream(int fd, struct pc_cipherstate *cs,
		const unsigned char *p, size_t n)
{
	while (n) {
		unsigned char rec[2 + PC_NOISE_MAXPT + PC_NOISE_TAGLEN];
		size_t chunk = n > PC_NOISE_MAXPT ? PC_NOISE_MAXPT : n;
		int cl = pc_transport_encrypt(cs, p, chunk, rec + 2);

		if (cl < 0)
			return -1;
		rec[0] = (unsigned char)cl;
		rec[1] = (unsigned char)(cl >> 8);
		if (bulk_io(fd, 1, rec, 2 + (size_t)cl) != 0)
			return -1;
		p += chunk;
		n -= chunk;
	}
	return 0;
}

/* read exactly @want plaintext bytes from the record stream */
static int bulk_recv_exact(int fd, struct pc_cipherstate *cs,
		unsigned char *stash, size_t *slen, size_t scap,
		unsigned char *out, size_t want)
{
	size_t got = 0;

	for (;;) {
		size_t take = *slen < want - got ? *slen : want - got;

		memcpy(out + got, stash, take);
		memmove(stash, stash + take, *slen - take);
		*slen -= take;
		got += take;
		if (got == want)
			return 0;
		{
			unsigned char hdr[2], ct[PC_NOISE_MAXMSG];
			size_t cl;
			int pl;

			if (bulk_io(fd, 0, hdr, 2) != 0)
				return -1;
			cl = (size_t)hdr[0] | ((size_t)hdr[1] << 8);
			if (cl < PC_NOISE_TAGLEN || cl > PC_NOISE_MAXMSG ||
			        *slen + cl > scap ||
			        bulk_io(fd, 0, ct, cl) != 0)
				return -1;
			pl = pc_transport_decrypt(cs, ct, cl, stash + *slen);
			if (pl < 0)
				return -1;
			*slen += (size_t)pl;
		}
	}
}

static void bulk_tx(volatile int *stop)
{
	unsigned char *batch = NULL, hdr[3 + PC_NOISE_MAXMSG];
	unsigned char m2[PC_NOISE_MAXMSG], cnt4[4], ver = PC_CLIENT_VER;
	unsigned char stash[PC_NOISE_MAXMSG];
	struct sockaddr_in to;
	struct pc_handshake hs;
	struct pc_cipherstate cs_s, cs_r;
	struct timeval tv = { 5, 0 };
	uint8_t prologue[1] = { PC_PRIN_CLUSTER };
	size_t blen, mlen = 0, plen = 0, cl, slen = 0;
	unsigned int recs, stored = 0;
	int fd = -1, ok = 0;

	(void)stop;
	pthread_mutex_lock(&C.bmx);
	batch = C.bulk_tx;
	blen = C.bulk_tx_len;
	recs = C.bulk_tx_recs;
	to = C.bulk_tx_to;
	pthread_mutex_unlock(&C.bmx);
	if (!batch)
		return;

	to.sin_port = C.mcast_dst.sin_port;    /* the cluster port, TCP */
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		goto out;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	if (connect(fd, (struct sockaddr *)&to, sizeof to) != 0)
		goto out;

	pc_hs_init_initiator(&hs, prologue, 1);
	if (pc_hs_write_msg1(&hs, C.psk, &ver, 1, hdr + 3, &mlen) != 0)
		goto out;
	hdr[0] = (unsigned char)(1 + mlen);
	hdr[1] = (unsigned char)((1 + mlen) >> 8);
	hdr[2] = PC_PRIN_CLUSTER;
	if (bulk_io(fd, 1, hdr, 3 + mlen) != 0 ||
	        bulk_io(fd, 0, hdr, 2) != 0)
		goto out;
	cl = (size_t)hdr[0] | ((size_t)hdr[1] << 8);
	if (cl == 0 || cl > sizeof m2 || bulk_io(fd, 0, m2, cl) != 0 ||
	        pc_hs_read_msg2(&hs, m2, cl, NULL, &plen, &cs_s, &cs_r) != 0)
		goto out;

	p32(cnt4, recs);
	if (bulk_send_stream(fd, &cs_s, cnt4, 4) != 0 ||
	        bulk_send_stream(fd, &cs_s, batch, blen) != 0)
		goto out;
	if (bulk_recv_exact(fd, &cs_r, stash, &slen, sizeof stash, cnt4, 4)
	        != 0)
		goto out;
	stored = g32(cnt4);
	ok = 1;
out:
	if (fd >= 0)
		close(fd);
	__atomic_fetch_add(&C.px.bulk_out, stored, __ATOMIC_RELAXED);
	if (!ok || stored < recs) {
		__atomic_fetch_add(&C.px.migrate_lost,
			recs - (ok ? stored : 0), __ATOMIC_RELAXED);
		LM_WARN("bulk migration: %u of %u record(s) unconfirmed\n",
			recs - (ok ? stored : 0), recs);
	}
	free(batch);
	pthread_mutex_lock(&C.bmx);
	C.bulk_tx = NULL;
	pthread_mutex_unlock(&C.bmx);
}

static void bulk_rx(int cfd)
{
	extern void pc_wal_upsert(const char *, const char *, int,
		const char *, int, unsigned int, unsigned long long);
	unsigned char frame[3 + PC_NOISE_MAXMSG], m2[3 + PC_NOISE_MAXMSG];
	unsigned char stash[PC_NOISE_MAXMSG], rhdr[MIG_RHDR], cnt4[4];
	unsigned char *rec = NULL;
	struct pc_handshake hs;
	struct pc_cipherstate cs_s, cs_r;
	struct timeval tv = { 5, 0 };
	size_t flen, mlen = 0, slen = 0;
	unsigned int nrec, i, stored = 0;

	setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	if (bulk_io(cfd, 0, frame, 2) != 0)
		goto out;
	flen = (size_t)frame[0] | ((size_t)frame[1] << 8);
	if (flen < 1 + 48 || flen > PC_NOISE_MAXMSG ||
	        bulk_io(cfd, 0, frame + 2, flen) != 0)
		goto out;
	if (frame[2] != PC_PRIN_CLUSTER)
		goto out;                      /* cluster principal ONLY */
	pc_hs_init_responder(&hs, frame + 2, 1);
	{
		unsigned char pay[16];
		size_t pl2 = 0;

		if (pc_hs_read_msg1(&hs, C.psk, frame + 3, flen - 1, pay,
		        &pl2) != 0)
			goto out;
		if (pl2 < 1 || pay[0] != PC_CLIENT_VER)
			goto out;
	}
	if (pc_hs_write_msg2(&hs, NULL, 0, m2 + 2, &mlen, &cs_s, &cs_r)
	        != 0)
		goto out;
	m2[0] = (unsigned char)mlen;
	m2[1] = (unsigned char)(mlen >> 8);
	if (bulk_io(cfd, 1, m2, 2 + mlen) != 0)
		goto out;
	/* pc_hs_write_msg2 hands the responder ITS oriented pair already */

	if (bulk_recv_exact(cfd, &cs_r, stash, &slen, sizeof stash, cnt4, 4)
	        != 0)
		goto out;
	nrec = g32(cnt4);
	if (nrec > 100000)
		goto out;
	rec = malloc(11u + 255 + 4096 + PCACHE_CELL_MAX);
	if (!rec)
		goto out;
	for (i = 0; i < nrec; i++) {
		unsigned int ttl_left, cn, kn, vl, exp;
		unsigned long long ver;
		pcache_htable_t *ht;
		char colz[256];
		str k, v;
		int brc;

		if (bulk_recv_exact(cfd, &cs_r, stash, &slen, sizeof stash,
		        rhdr, MIG_RHDR) != 0)
			goto out;
		ttl_left = g32(rhdr);
		cn = rhdr[4];
		kn = g16(rhdr + 5);
		vl = g32(rhdr + 7);
		ver = g64(rhdr + 11);
		if (cn > 255 || kn > 4096 || vl > PCACHE_CELL_MAX)
			goto out;
		if (bulk_recv_exact(cfd, &cs_r, stash, &slen, sizeof stash,
		        rec, (size_t)cn + kn + vl) != 0)
			goto out;
		memcpy(colz, rec, cn);
		colz[cn] = 0;
		ht = pc_store_find(colz, cn);
		if (!ht)
			continue;
		k.s = (char *)rec + cn;
		k.len = (int)kn;
		v.s = (char *)rec + cn + kn;
		v.len = (int)vl;
		exp = ttl_left ? get_ticks() + ttl_left : 0;
		pc_lamport_observe(ver);
		brc = pcache_ht_store_ver(ht, &k, &v, exp, 0, ver);
		if (brc == PCACHE_E_OLDER)
			__atomic_fetch_add(&C.px.recv_older, 1,
				__ATOMIC_RELAXED);
		if (brc == 0) {
			pc_wal_upsert(colz, k.s, k.len, v.s, v.len, exp,
				pcache_last_ver);
			pc_loc_clear(colz, cn, k.s, kn);
			__atomic_fetch_add(&C.px.migrated_in, 1,
				__ATOMIC_RELAXED);
			if (pc_store_shard_enabled(ht))
				shard_note_change();
			__atomic_fetch_add(&C.px.bulk_in, 1,
				__ATOMIC_RELAXED);
			stored++;
		}
	}
	p32(cnt4, stored);
	bulk_send_stream(cfd, &cs_s, cnt4, 4);
out:
	free(rec);
	close(cfd);
}

void pc_bulk_thread(volatile int *stop)
{
	struct pollfd pf;

	if (C.blfd < 0)
		return;
	pf.fd = C.blfd;
	pf.events = POLLIN;
	while (!*stop) {
		int have_tx;

		pthread_mutex_lock(&C.bmx);
		have_tx = C.bulk_tx != NULL;
		pthread_mutex_unlock(&C.bmx);
		if (have_tx)
			bulk_tx(stop);
		if (poll(&pf, 1, 200) > 0) {
			int cfd = accept(C.blfd, NULL, NULL);

			if (cfd >= 0)
				bulk_rx(cfd);
		}
	}
}

/* ---- init / stats ------------------------------------------------------- */

int pc_cluster_init(const char *mcast_addr, int mcast_port,
		const char *advertise, const uint8_t psk[PC_NOISE_KEYLEN],
		int pull_timeout_ms, int negative_ms, int tombstone_ms,
		const char *state_dir, int max_pending)
{
	struct sockaddr_in sa;
	struct in_addr self_ip;
	struct ip_mreqn mreq;
	int i, one = 1;

	memset(&C.st, 0, sizeof C.st);
	/* Size the parked-request table ONCE.  Clamped, not rejected: the
	 * ceiling is structural (the slot index rides in the request id)
	 * and a config that asks for more should still start, saying so.
	 * Allocated once and never resized - pend_find() hands out raw
	 * pointers into this. */
	C.pend_cap = max_pending > 0 ? max_pending : PEND_DEFAULT;
	if (C.pend_cap > PEND_LIMIT) {
		LM_WARN("cluster: max_pending %d exceeds the %d the request "
			"id can address - using %d\n",
			C.pend_cap, PEND_LIMIT, PEND_LIMIT);
		C.pend_cap = PEND_LIMIT;
	}
	if (C.pend_cap < PEND_MIN)
		C.pend_cap = PEND_MIN;
	free(C.pend);
	C.pend = calloc((size_t)C.pend_cap, sizeof *C.pend);
	if (!C.pend) {
		LM_ERR("cluster: cannot allocate %d parked-request slots "
			"(%zu bytes)\n", C.pend_cap,
			(size_t)C.pend_cap * sizeof *C.pend);
		return -1;
	}
	C.pend_used = 0;
	identity_init(state_dir);
	/* The mastership term lives beside the identity and in the same
	 * durability class, because it has to outlive any map somebody
	 * could still believe.  A node that cannot read a term it wrote
	 * refuses to start: silently forgetting one is how a term gets
	 * reissued with different content behind it. */
	if (pc_term_init(state_dir) != 0) {
		LM_CRIT("cluster: the stored mastership term in %s is "
			"unreadable - refusing to start rather than risk "
			"reissuing a term someone has already seen\n",
			state_dir ? state_dir : "(none)");
		return -1;
	}
	/* B4: armed when this node came back holding data.  A node that
	 * restored nothing has nothing to reconcile, and a fleet that is
	 * not eager cannot answer the question at all. */
	pc_clhist_init(&C.hist);
	pc_clsync_init(&C.sync, pc_term_current(), 0);
	C.reconcile_on = pc_recovered_records() > 0;
	C.rec_col = 0;
	C.rec_cursor = 0;
	LM_NOTICE("cluster: mastership term %u (%s)\n", pc_term_current(),
		pc_term_durable() ? "persisted"
		: "EPHEMERAL - nothing persisted here, so nothing to "
		  "contradict either");
	memcpy(C.psk, psk, PC_NOISE_KEYLEN);
	C.pull_timeout_ms = pull_timeout_ms;
	C.negative_ms = negative_ms;
	C.tombstone_ms = tombstone_ms;
	pthread_mutex_init(&C.pmx, NULL);
	for (i = 0; i < WORKERS_MAX; i++)
		pthread_mutex_init(&C.cq[i].mx, NULL);

	memset(&C.mcast_dst, 0, sizeof C.mcast_dst);
	C.mcast_dst.sin_family = AF_INET;
	C.mcast_dst.sin_port = htons((uint16_t)mcast_port);
	if (inet_pton(AF_INET, mcast_addr, &C.mcast_dst.sin_addr) != 1 ||
	        !IN_MULTICAST(ntohl(C.mcast_dst.sin_addr.s_addr))) {
		LM_ERR("cluster: '%s' is not a multicast group\n", mcast_addr);
		return -1;
	}

	/* the advertised unicast address: configured, or the interface a
	 * route lookup toward the group picks (the UDP-connect trick) */
	if (advertise) {
		if (inet_pton(AF_INET, advertise, &self_ip) != 1) {
			LM_ERR("cluster: bad advertise address %s\n", advertise);
			return -1;
		}
	} else {
		struct sockaddr_in probe_sa;
		socklen_t sl = sizeof probe_sa;
		int pfd = socket(AF_INET, SOCK_DGRAM, 0);

		if (pfd < 0)
			return -1;
		if (connect(pfd, (struct sockaddr *)&C.mcast_dst,
		        sizeof C.mcast_dst) != 0 ||
		        getsockname(pfd, (struct sockaddr *)&probe_sa, &sl) != 0) {
			LM_ERR("cluster: cannot auto-detect the advertise "
				"address (%s) - set advertise = <ip>\n",
				strerror(errno));
			close(pfd);
			return -1;
		}
		close(pfd);
		self_ip = probe_sa.sin_addr;
	}

	/* the unicast socket: the data plane, and the source address every
	 * peer learns us by */
	C.fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (C.fd < 0)
		return -1;
	{
		/* Pulls, migrations AND forwards burst together: a pipelined
		 * client write burst puts one datagram per key on the wire at
		 * once, each up to MAX_FWD_VAL.  A forward that the kernel
		 * drops is a REFUSED CLIENT WRITE - forwards carry no retry
		 * (unlike migrations, which have the ack window), so the
		 * receive buffer is the whole guarantee.
		 *
		 * MEASURED (2026-08-27, the proxytest 'placement writes'
		 * flake): 120 pipelined 40KB writes = 4.8MB arriving at once.
		 * With net.core.rmem_max at its 208KB default the kernel
		 * SILENTLY clamped this request and dropped 4-6 datagrams a
		 * run - UDP RcvbufErrors matched the refused writes exactly,
		 * one for one.  So: ask with SO_RCVBUFFORCE first (it bypasses
		 * rmem_max wherever we hold CAP_NET_ADMIN), fall back to a
		 * plain request, then READ THE BUFFER BACK and say plainly
		 * when the kernel gave us less than we asked for.  Silent
		 * under-delivery is how this cost a day. */
		int rb = PC_CLUSTER_RCVBUF, got = 0;
		socklen_t gl = sizeof got;

#ifdef SO_RCVBUFFORCE
		if (setsockopt(C.fd, SOL_SOCKET, SO_RCVBUFFORCE, &rb,
		        sizeof rb) != 0)
#endif
			setsockopt(C.fd, SOL_SOCKET, SO_RCVBUF, &rb, sizeof rb);
		if (getsockopt(C.fd, SOL_SOCKET, SO_RCVBUF, &got, &gl) == 0) {
			/* the kernel reports twice the usable payload budget */
			C.rcvbuf = got;
			if (got < rb)
				LM_WARN("cluster: receive buffer is %d KB, asked "
					"for %d KB - the kernel clamped it to "
					"net.core.rmem_max.  Bursts of large "
					"forwards WILL be dropped and their "
					"writes refused; raise "
					"net.core.rmem_max to at least %d or "
					"grant CAP_NET_ADMIN\n",
					got / 1024, rb / 1024, rb);
		}
	}
	/* the mcast rx socket wildcard-binds the SAME port: every socket
	 * sharing it needs REUSEADDR, this one included.  REUSEADDR is
	 * the whole requirement here - SO_REUSEPORT was set alongside it
	 * until 2026-08-31 and did nothing, verified by binding this exact
	 * pair for two daemons on one host without it and confirming both
	 * still receive the group.  It is deliberately NOT set: on the TCP
	 * listeners the same option is what lets a stray daemon join a
	 * live port unnoticed, so it is worth having nowhere it is not
	 * earning its place. */
	setsockopt(C.fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)mcast_port);
	sa.sin_addr = self_ip;
	if (bind(C.fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
		LM_ERR("cluster: cannot bind %s:%d (%s)\n",
			inet_ntoa(self_ip), mcast_port, strerror(errno));
		return -1;
	}
	C.self_addr = sa;
	fcntl(C.fd, F_SETFL, O_NONBLOCK);
	/* multicast SENDS leave the unicast socket, so peers learn our
	 * unicast address from the datagram source itself */
	{
		unsigned char ttl = 1, loop = 1;

		setsockopt(C.fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl,
			sizeof ttl);
		setsockopt(C.fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop,
			sizeof loop);
		memset(&mreq, 0, sizeof mreq);
		mreq.imr_address = self_ip;
		setsockopt(C.fd, IPPROTO_IP, IP_MULTICAST_IF, &mreq,
			sizeof mreq);
	}

	/* the multicast rx socket: ANY:port + membership.  REUSEADDR is
	 * what lets several daemons share one host - the test rigs, and
	 * blue/green restarts - and it is sufficient on its own; see the
	 * note on C.fd above. */
	C.mfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (C.mfd < 0)
		return -1;
	setsockopt(C.mfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)mcast_port);
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(C.mfd, (struct sockaddr *)&sa, sizeof sa) != 0) {
		LM_ERR("cluster: cannot bind multicast rx port %d (%s)\n",
			mcast_port, strerror(errno));
		return -1;
	}
	memset(&mreq, 0, sizeof mreq);
	mreq.imr_multiaddr = C.mcast_dst.sin_addr;
	mreq.imr_address = self_ip;
	if (setsockopt(C.mfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq,
	        sizeof mreq) != 0) {
		LM_ERR("cluster: cannot join group %s (%s)\n", mcast_addr,
			strerror(errno));
		return -1;
	}
	fcntl(C.mfd, F_SETFL, O_NONBLOCK);

	/* the bulk TCP listener shares the cluster port number */
	pthread_mutex_init(&C.bmx, NULL);
	C.blfd = socket(AF_INET, SOCK_STREAM, 0);
	if (C.blfd >= 0) {
		int bone = 1;

		setsockopt(C.blfd, SOL_SOCKET, SO_REUSEADDR, &bone,
			sizeof bone);
		memset(&sa, 0, sizeof sa);
		sa.sin_family = AF_INET;
		sa.sin_port = htons((uint16_t)mcast_port);
		sa.sin_addr = self_ip;
		if (bind(C.blfd, (struct sockaddr *)&sa, sizeof sa) != 0 ||
		        listen(C.blfd, 8) != 0) {
			LM_WARN("cluster: bulk TCP listener unavailable "
				"(%s) - oversized migrations will drop\n",
				strerror(errno));
			close(C.blfd);
			C.blfd = -1;
		} else {
			fcntl(C.blfd, F_SETFL, O_NONBLOCK);
		}
	}

	/* start joining: the peer thread drives the FSM from here */
	C.role = PC_ROLE_JOINING;
	randombytes_buf(&C.join_tok, 8);
	C.join_deadline_ms = now_ms() + JOIN_WAIT_MS;
	C.join_last_ms = 0;
	C.st.enabled = 1;
	C.enabled = 1;
	LM_NOTICE("cluster: automatic membership - group %s:%d, "
		"advertising %s:%d, pull timeout %dms, negative %dms, "
		"tombstone %dms\n", mcast_addr, mcast_port,
		inet_ntoa(self_ip), mcast_port, pull_timeout_ms,
		negative_ms, tombstone_ms);
	return 0;
}

int pc_my_node_id(void)
{
	return __atomic_load_n(&C.node_id, __ATOMIC_RELAXED);
}

int pc_cluster_enabled(void)
{
	return C.enabled;
}

int pc_cluster_neg_ms(void)
{
	return C.negative_ms;
}

void pc_cluster_get_stats(struct pc_cl_stats *out)
{
	int i;

	*out = C.st;
	out->pend_max = C.pend_cap;    /* the capacity peak is measured against */
	out->pend_used = C.pend_used;
	out->node_id = pc_my_node_id();
	out->peers_up = 0;
	for (i = 0; i < C.n_peers; i++)
		if (C.peers[i].node &&
		        now_ms() - C.peers[i].last_seen_ms < PEER_UP_MS)
			out->peers_up++;
}

int pc_cluster_peers(struct pc_cl_peer_info *out, int max)
{
	int i, n = 0;

	for (i = 0; i < C.n_peers && n < max; i++) {
		out[n].node = C.peers[i].node;
		out[n].up = now_ms() - C.peers[i].last_seen_ms < PEER_UP_MS;
		out[n].last_seen_ms = C.peers[i].last_seen_ms;
		out[n].free_mb = C.peers[i].free_mb;
		n++;
	}
	return n;
}
