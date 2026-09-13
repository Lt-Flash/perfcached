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
#include <sys/stat.h>
#include <sys/vfs.h>
/* statfs.f_type is __fsword_t: long on 64-bit, plain int on i386 and
 * arm32, where RAMFS_MAGIC does not fit and the comparison is a
 * signed/unsigned mismatch under -Werror (the rc4 matrix caught it).
 * Compare the low 32 bits as unsigned, and carry the two constants here
 * with the U suffix rather than trust linux/magic.h's types. */
#define PC_TMPFS_MAGIC 0x01021994u
#define PC_RAMFS_MAGIC 0x858458f6u
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
#include "clcodec.h"                 /* the LE wire codec */
#include "fnv1a.h"                   /* the one FNV-1a */
#include "clmig.h"                   /* the migration in-flight window */
#include "clfwd.h"                   /* the proxy write plane frames */
#include "clinit.h"                  /* the node identity file format */
#include "pc_slot.h"                  /* pc_key_slot(): placement input */
#include "pc_mix.h"                   /* pc_hrw_mix(): the rendezvous weight */
#include "daemon.h"                  /* pc_worker_id() */
#include "compat/ipc.h"               /* ipc_send_rpc_all(): the flush RPC (S110) */
#include "clmap.h"
#include "clterm.h"
#include "clplace.h"
#include "recover.h"
#include "metrics.h"                 /* pc_metrics_uptime() for the ALIVE */
#include "clhist.h"
#include "clsync.h"
#include "clpend.h"
#include "clloc.h"
#include "clres.h"                   /* S90: the id reservations */
#include "clmemb.h"                  /* the keepalive frame layouts */
#include "clpull.h"                  /* the PULL plane frame layouts */
#include "clcol.h"                   /* the collection-announce frame */
#include "clboot.h"
#include "clpush.h"
#include "clwire.h"
#include "clbulk.h"
#include "clstate.h"
#include "clpeers.h"
#include "core/pcache_mem.h"

/* The watchdog re-sends the last keepalive if the peer thread stalls,
 * and clwire_beat_store() SILENTLY RETURNS when a frame will not fit -
 * so a keepalive that outgrew its buffer would freeze the watchdog's
 * copy at the last frame that did fit, with nothing said.  On
 * MASTER_ALIVE that is a stale mastership claim being re-broadcast
 * while the real one is gone.  Caught at compile time instead. */
_Static_assert(CLMEMB_ALIVE_LEN <= sizeof ((struct clwire *)0)->alive,
	"ALIVE outgrew clwire's beat buffer - clwire_beat_store would drop it");
_Static_assert(CLMEMB_MALIVE_LEN <= sizeof ((struct clwire *)0)->malive,
	"MASTER_ALIVE outgrew clwire's beat buffer - it would be dropped");

#define SHARD_GRACE_S  30              /* after a membership change (and
                                        * self-extending while reshard
                                        * migrations still flow), shard
                                        * owner-misses fall back to a
                                        * broadcast pull - data may not
                                        * have moved yet */

#define M_PULL_REQ  2
#define M_PULL_RSP  3
#define M_TOMBSTONE 4
#define M_FWD_OP      5
#define M_FWD_ACK     6
#define M_MIGRATE     7
#define M_MIGRATE_ACK 8
#define M_DEMOTE      9
/* membership (control plane; 1 was the retired static-list heartbeat) */
/* LAYOUTS FOR THE FIVE BELOW LIVE IN clmemb.h, which is the only place
 * they are written down and the only place a test pins them.  The
 * sketches that used to sit here had drifted - JOIN_REQ still listed a
 * k1 and an m1 that no longer exist and omitted the incarnation and the
 * WAL byte - which is what a second copy of a layout does. */
#define M_JOIN_REQ      10   /* mcast, or ucast to master.  The joiner's
                              * whole position: token, config, identity,
                              * and the id it PROPOSES - derived from its
                              * identity so it asks for the same one every
                              * start; the master arbitrates collisions */
#define M_ASSIGN        11   /* ucast: the id you are, who the master is,
                              * and the membership - a 15-byte header then
                              * a 20-byte record per member.  The address
                              * and port in a record are NETWORK ORDER and
                              * pass through verbatim */
#define M_MASTER_ALIVE  12   /* mcast 1/s.  What master-down detection
                              * runs on: silence past MASTER_DEAD_MS
                              * promotes a standby.  Carries the TERM the
                              * sender asserts mastership under, which is
                              * what settles a split brain before any map
                              * could arrive */
#define M_ALIVE         13   /* mcast 1/s; addr = src.  Sixteen fields
                              * behind eleven length gates, all additive.
                              * free feeds placement
                              * (capacity-seeking); liveKB/total feeds
                              * the rebalancer (proportional per-mille
                              * leveling on live CELL bytes, never the
                              * chunk-sticky 'used') */
#define M_GOODBYE       14   /* mcast.  Carries the departing node id,
                              * which NOTHING READS - the sender's address
                              * is the identity here */
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
#define M_JOIN_REJ      19   /* ucast: the master refusing a join
                              * outright.  The reason byte is additive - a
                              * 9-byte refusal from an older build has
                              * none.  Reason 1 =
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
#define M_JOIN_WAIT     23   /* ucast: the master is HOLDING this
                              * join (an identity like the joiner's is still
                              * counted live); keep retrying, do not found.
                              * S108: silence here made a clone found its
                              * own cluster.  A build before the tag ignores
                              * it and behaves as before. */
#define M_COL_SET       24   /* ucast: [op1][bl1][gen8][nlen1][name]
                              * (+ [nlen2][name2] when op is 2) - S69,
                              * a collection created (op 0) or dropped
                              * (op 1) somewhere in the fleet.  Idempotent
                              * and ordered by the Lamport generation, so
                              * a re-announce cannot undo a later drop. */
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

#define COL_RESYNC_BEATS 5   /* S69: the created set re-announced this often */
#define JOIN_WAIT_MS    2000
#define JOIN_DEFER_MS   1000
#define JOIN_DEFER_MAX  4
#define JOIN_HOLD_MS    5000   /* S108: how much one JOIN_WAIT extends the
                                * joiner's deadline; repeated every retry
                                * while the master keeps holding, so a
                                * master that dies mid-hold lets the
                                * deadline expire naturally */
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
/* S82: a peer that arrived EMPTY is not trusted to hold the data for
 * this long after it did - three sweeps, enough for its own backfill
 * of an ordinary keyspace to have landed.  A holdoff rather than the
 * flag alone because backfill[] only clears on the node that ran the
 * backfill; on every other node it goes stale, and after a cold start
 * (everyone joins empty) that would make every node skip every peer
 * and self-designate for good - N senders, the very thing one
 * designated sender exists to prevent. */
#define BACKFILL_SENDER_HOLDOFF_MS 30000
/* S83: the bootstrap PULL.  A node that joins an eager fleet holding
 * nothing asks ONE ready peer for a full walk over the bulk TCP plane
 * and reports ready only when the stream has ended - so "ready" means
 * complete, which no push from a sender could say.  The request rides
 * as a record count no batch can have; a build before it closes on
 * that count and the joiner falls back to the push. */
#define BOOT_MAGIC      0xFFFFFFFFu
#define BOOT_KIND_PULL  1
#define BOOT_BUF        (512u * 1024u)
/* What a node says about how it STARTED - an additive tail byte on the
 * ALIVE (S81).  A peer decides whether to backfill a node from this, never
 * from its record count: a steady sweep can land a couple of records before
 * the first heartbeat is read, and a count of 2 was being taken for
 * "recovered from a WAL" (measured: a 480-record refill stalled at 2).
 * 0 is what a build older than the byte sends. */
#define PC_START_UNKNOWN     0
#define PC_START_COLD        1   /* replayed nothing, within its first holdoff */
#define PC_START_RECOVERED   2   /* replayed a WAL: owed a diff, not a push */
#define PC_START_ESTABLISHED 3   /* past its first holdoff */
/* S90: a departed member's binding - address and identity to id - is
 * RESERVED for a window, so a returning identity or a replacement at the
 * same address gets the id back and a newcomer cannot be handed it
 * meanwhile.  Every node records the departures it sees, so a node that
 * becomes master inside the window already holds them - no sync needed.
 * Bounded (CLRES_MAX, soonest-to-lapse evicted) and expiring
 * (RESERVE_MS), so ids return to the pool.  The table and its five rules
 * are clres.h; the WINDOW stays here because it is policy - and keeping
 * it a parameter is what lets the suite put the expiry boundary exactly
 * on it instead of waiting an hour. */
#define RESERVE_MS  3600000LL
static struct clres reserved;
static int self_start_kind(void);
/* S73: the eager sweep runs every this many beats.  It is REPAIR behind
 * the write-path push, not delivery, and its "dying soon" skip is
 * derived from this - never a fixed number, or the two drift apart. */
#define REPL_SWEEP_BEATS 10
#define PEER_PURGE_MS   6000

#define MAX_FWD_VAL PC_MAX_FWD_VAL
/* Unicast receive buffer.  Sized for the worst burst the forward plane
 * can produce in one go - a fully pipelined client write burst of
 * MAX_FWD_VAL-sized values - because a dropped forward is a refused
 * write, not a retried one.  The kernel doubles this for accounting. */
#define PC_CLUSTER_RCVBUF (8 << 20)

#define MIG_BATCH_CAP  ((4 << 20) + (64 << 10))
#define MIG_WINDOW     3           /* 3 x ~56KB fits a default rmem */
/* S85: a batch whose ack is late is re-sent, up to MIG_RETX times, one
 * per MIG_RETX_MS.  Before this a single late ack dirtied the whole
 * cycle and the sender re-walked the entire keyspace, passive copies
 * included - a lost datagram repaired by re-pushing everything the
 * peer already held.  The bytes are still in migb until the slice
 * ends, so the repair is one datagram, never the keyspace. */
#define MIG_RETX       2
#define MIG_RETX_MS    1500
#define MIG_GATHER_CAP 56000       /* gathered group payload cap */
/* S105: the write-path push's per-peer group - flushed when this full,
 * when this old at the next append, or by the cluster loop's timer */
#define REPL_GROUP_FLUSH_BYTES 48000
#define REPL_FLUSH_MS 3
#define MIG_GHDR       9           /* type+req4+node2+count2 */
#define MIG_RHDR      19           /* per record: ttl4+cn1+klen2+vlen4+ver8 */

static struct {
	int enabled;
	int fd;
	int node_id;
	unsigned char self_ident[16];  /* persisted; stable across restarts */
	char self_ident_hex[33];
	unsigned int self_incarn;      /* fresh every process start */
	long long ready_ms;            /* first READY heartbeat; 0 = not yet */
	int ident_durable;             /* 0 = nothing to persist to */
	char state_dir[512];           /* S80: in use; empty = ephemeral */
	int state_tmpfs;               /* S80: said loudly at startup */
	int proposed_id;               /* derived from the identity */
	uint8_t psk[PC_NOISE_KEYLEN];
	struct clwire wr;              /* sealed frames + the beat pair (M6) */
	struct clpeers pt;
	int pull_timeout_ms, negative_ms, tombstone_ms;
	struct clpend pd;              /* parked requests + the CQs (M2) */
	uint32_t next_req;
	struct clloc lc;               /* locator + negative cache (M3) */
	struct pc_cl_stats st;
	struct pc_proxy_stats px;
	/* the migration batch (S22 pacing fix): victims are stubbed and
	 * their sealed payloads COLLECTED during the scan, then sent
	 * through an ack-gated window - a raw burst overran the peer's UDP
	 * receive buffer and silently dropped transfers (caught by
	 * proxytest's conservation check: 8 of 149 keys lost on one run) */
	struct clmig_buf migb;         /* the gather buffer (M10 slice 2) */
	/* the in-flight window is clmig's (M10 slice 1); losses are
	 * accounted in RECORDS there, because one gathered group carries
	 * many and a lost datagram must not read as one lost record */
	struct clmig mig;
	/* automatic membership (peer thread writes, workers read node_id
	 * and peers[] - int/ptr-free fields, release/acquire on .node) */
	int mfd;                       /* multicast rx socket */
	int rcvbuf;                    /* EFFECTIVE unicast SO_RCVBUF (read
	                                * back: the kernel clamps silently) */
	/* S125: what the receive plane is doing, sampled once a beat.  The
	 * cluster thread owns all of this; workers read only the published
	 * copy in C.st, which the same thread fills. */
	struct {
		long long at_ms;               /* the previous sample */
		unsigned long long applied, older, drops;
	} rxs;
	/* SO_RXQ_OVFL delivers a 32-bit count of datagrams this socket has
	 * dropped, stamped on each datagram AT THE MOMENT IT WAS QUEUED -
	 * so it lags by the queue depth, which at a 1 Hz sample does not
	 * matter, and it wraps, which is why the running total is kept
	 * here as 64 bits.  One slot per receive socket: unicast, mcast. */
	struct {
		unsigned int last;             /* last 32-bit value seen */
		int seen;                      /* a value has been seen at all */
	} ovfl[2];
	unsigned long long rxq_drops;      /* the 64-bit running total */
	/* S30: the interchange-relevant config this node runs, and who we
	 * have already shouted about (once per peer, not once per second) */
	int cfg_mode, cfg_eager, cfg_authoritative;
	long long pend_log_ms;         /* rate limit for the FULL shout */
	int cfg_client_port;           /* S34: where CLIENTS reach this node
	                                * (the peer plane speaks its own
	                                * port; a client needs the listener) */
	int cfg_http_port;             /* S78: this node's HTTP door, 0 = none */
	int cfg_wal;                   /* S73b: 1 = this node logs a WAL */
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
	int join_held;                 /* S108: said "held" once this join */
	/* the bulk TCP plane (records above the datagram ceiling): the
	 * peer thread's tick fills a batch + target, the bulk thread owns
	 * every socket */
	int blfd;                      /* TCP listener on advertise:port */
	struct clbulk bk;              /* framing + the batch handoff (M7) */
	unsigned char *bulkb;          /* tick-side collect buffer */
	size_t bulkb_len;
	unsigned int bulk_recs;
	/* S83: the bootstrap pull, handed to the bulk thread under bmx */
	struct clboot bt;              /* the S83 bootstrap decision (M4) */
	struct clpush wp;              /* per-thread write-path groups (M5) */
	int boot_done;                 /* pulled a full walk this incarnation */
	int boot_rounds;               /* candidate lists tried so far */
	int boot_round_failed;         /* set by the bulk thread: re-pick */
} C;


static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- sealed datagrams -------------------------------------------------- */

/* The sealed datagram lives in clwire (M6); sealing is separate from
 * sending, so this is where the socket is touched and where C.psk and
 * C.fd - which the bulk plane and seventeen other senders also read -
 * are handed in. */
static int seal_send(const struct sockaddr_in *to, const unsigned char *pt,
		size_t n)
{
	unsigned char buf[HDR_LEN + MAX_DGRAM + 16];
	size_t len = clwire_seal(C.psk, pt, n, buf);

	if (!len)
		return -1;
	return sendto(C.fd, buf, len, 0,
		(const struct sockaddr *)to, sizeof *to) < 0 ? -1 : 0;
}

static int open_dgram(unsigned char *buf, size_t n, unsigned char *pt,
		unsigned long long *ptlen)
{
	return clwire_open(C.psk, buf, n, pt, ptlen);
}


/* one sealed datagram to every live peer, in peer-table order */
static void broadcast_live(const unsigned char *msg, size_t n, long long now)
{
	int i;

	for (i = 0; i < C.pt.n_peers; i++)
		if (clpeers_live(&C.pt.peers[i], now))
			seal_send(&C.pt.peers[i].addr, msg, n);
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

void pc_cluster_beat_thread(volatile int *stop)
{
	while (!*stop) {
		struct timespec ts = { 0, 250L * 1000000 };
		unsigned char a[sizeof C.wr.alive], ma[sizeof C.wr.malive];
		size_t alen, mlen;

		nanosleep(&ts, NULL);
		if (now_ms() - clwire_sent_ms(&C.wr) < BEAT_OVERDUE_MS)
			continue;
		clwire_beat_take(&C.wr, a, &alen, ma, &mlen);
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
		clwire_mark_sent(&C.wr, now_ms());
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
               PC_NST_DRAINING == PC_CLMAP_ST_DRAINING &&
               PC_NST_FAILED == PC_CLMAP_ST_FAILED,
               "node state values must match the map's wire encoding");


const char *pc_node_state_name(int st)
{
	switch (st) {
	case PC_NST_STARTING:    return "starting";
	case PC_NST_RECOVERING:  return "recovering";
	case PC_NST_READY:       return "ready";
	case PC_NST_DRAINING:    return "draining";
	case PC_NST_FAILED:      return "failed";
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

	/*
	 * DRAINING and FAILED are both TERMINAL, and FAILED is terminal for
	 * a reason that only shows up in shard mode: ownership is computed
	 * from the member set, so a node going FAILED re-shards the cluster
	 * and a node coming BACK re-shards it again - two full rebalances
	 * for one fault, and a flapping node would thrash the fleet
	 * indefinitely.  The cheap-looking recovery is the expensive one.
	 *
	 * So a FAILED node stays failed until an operator restarts it,
	 * which is also the honest reading of what put it there: its
	 * storage could not keep up, and nothing about that fixes itself.
	 *
	 * FAILED -> DRAINING is the one transition still allowed: a failed
	 * node must still be able to shut down cleanly and say goodbye.
	 */
	if (cur == st || cur == PC_NST_DRAINING)
		return;
	if (cur == PC_NST_FAILED && st != PC_NST_DRAINING)
		return;
	__atomic_store_n(&C.nstate, st, __ATOMIC_RELAXED);
	LM_NOTICE("cluster: node state %s -> %s\n", pc_node_state_name(cur),
		pc_node_state_name(st));
}

/* ---- LE helpers -------------------------------------------------------- */
/* The six moved to clcodec.h so the modules can reach them; these keep
 * the short names the 199 call sites below already use. */
#define p16 pc_p16
#define p32 pc_p32
#define p64 pc_p64
#define g16 pc_g16
#define g32 pc_g32
#define g64 pc_g64

/* ---- negative cache ---------------------------------------------------- */

/* ---- the locator and negative caches -----------------------------------
 * Both tables live in clloc now (M3).  These wrappers keep the shape
 * cluster.h publishes and proto.c and verbs.c call; the module takes the
 * clock as an argument, so the wrapper is where now_ms() is read. */

int pc_neg_hit(const char *col, size_t cn, const char *key, size_t kn)
{
	return clloc_neg_hit(&C.lc, col, cn, key, kn, now_ms());
}

void pc_neg_set(const char *col, size_t cn, const char *key, size_t kn,
		int ms)
{
	clloc_neg_set(&C.lc, col, cn, key, kn, now_ms(), ms);
}

void pc_neg_clear(const char *col, size_t cn, const char *key, size_t kn)
{
	clloc_neg_clear(&C.lc, col, cn, key, kn);
}

int pc_loc_get(const char *col, size_t cn, const char *key, size_t kn)
{
	return clloc_get(&C.lc, col, cn, key, kn);
}

void pc_loc_set(const char *col, size_t cn, const char *key, size_t kn,
		int node)
{
	clloc_set(&C.lc, col, cn, key, kn, node);
}

void pc_loc_clear(const char *col, size_t cn, const char *key, size_t kn)
{
	clloc_clear(&C.lc, col, cn, key, kn);
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
	const char *names[PC_MAX_COLLECTIONS];
	struct clinit_digest_in in;
	int i, n = pc_store_count();

	/* the module carries its own bound; a mismatch would silently fold
	 * a different number of collections on one side of an upgrade and
	 * refuse every join */
	_Static_assert(CLINIT_MAX_NAMES == PC_MAX_COLLECTIONS,
		"clinit's name bound must match the store's");

	if (n > PC_MAX_COLLECTIONS)
		n = PC_MAX_COLLECTIONS;
	for (i = 0; i < n; i++)
		names[i] = pc_store_name(i);
	in.mode = C.cfg_mode;
	in.eager = C.cfg_eager;
	in.wal = C.cfg_wal;
	in.route_algo = PC_ROUTE_ALGO;
	in.names = names;
	in.n_names = n;
	return clinit_config_digest(&in);
}

void pc_cluster_set_config(int mode, int eager,
		int authoritative, int client_port, int resp_port,
		int http_port, int wal)
{
	C.cfg_mode = mode;
	C.cfg_eager = eager;
	C.cfg_authoritative = authoritative;
	C.cfg_client_port = client_port;
	C.cfg_resp_port = resp_port;
	C.cfg_http_port = http_port;
	C.cfg_wal = wal != 0;
}

int pc_cluster_mode(void)
{
	return C.cfg_mode;
}

int pc_cluster_eager(void)
{
	return C.cfg_eager;
}

const char *pc_cluster_mode_name(void)
{
	if (C.cfg_mode == PC_MODE_PROXY)
		return "proxy";
	if (C.cfg_mode == PC_MODE_SHARD)
		return "shard";
	return C.cfg_eager ? "eager" : "store";
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
static unsigned int self_entries(void);      /* defined with the ALIVE */
static int any_eager(void);                  /* S83, defined with the tick */

/* S103: the start kind as a word, for members and the page */
const char *pc_start_kind_name(int kind)
{
	switch (kind) {
	case PC_START_COLD:        return "cold";
	case PC_START_RECOVERED:   return "recovered";
	case PC_START_ESTABLISHED: return "established";
	default:                   return "unknown";
	}
}

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
	out[n].http_port = C.cfg_http_port;
	out[n].uptime_s = (long)pc_metrics_uptime();
	out[n].entries = (long)self_entries();
	out[n].start = pc_start_kind_name(self_start_kind());
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
	for (i = 0; i < C.pt.n_peers && n < max; i++) {
		struct peer *p = &C.pt.peers[i];

		if (!__atomic_load_n(&p->node, __ATOMIC_ACQUIRE))
			continue;
		if (!p->client_port)
			continue;      /* older peer: cannot be dialled */
		out[n].addr = p->addr.sin_addr;
		out[n].client_port = p->client_port;
		out[n].resp_port = p->resp_port;
		out[n].http_port = p->http_port;
		out[n].uptime_s = p->boot_ms ? (long)((now_ms() - p->boot_ms) / 1000) : -1;
		out[n].entries = (long)p->entries;
		out[n].start = pc_start_kind_name(p->start_kind);
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
		int their_eager, uint64_t theirs, int their_wal)
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
		"multicast group.%s%s\n",
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
		"nothing can be checked beyond this digest)",
		/* S73b: when the peer said its posture and it is the
		 * difference, say so - the digest alone cannot */
		!their_wal || (their_wal == 2) == (C.cfg_wal != 0) ? "" :
		C.cfg_wal
		? "  The WAL posture differs: it runs WITHOUT a WAL and we "
		  "log one - a fleet is uniform, give every node a [wal] or "
		  "none (S73b)."
		: "  The WAL posture differs: it logs a WAL and we run without "
		  "one - a fleet is uniform, give every node a [wal] or none "
		  "(S73b).");
}

int pc_cluster_rcvbuf(void)
{
	return C.rcvbuf;
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

/* S117: the figures the beat carries - and the member cards divide -
 * are the CEILING's: what this node can still take before it refuses a
 * write, over the ceiling.  They used to be the slot pool's, and the
 * pool's free share rose when records were freed and FELL when the
 * give-back returned their pages to the host: the one event that makes
 * room on the box read as a loss of headroom (245-247, 2026-09-08:
 * 64 % after the sweep, 31 % after the give-back, beside an arena card
 * saying 60 %).  Placement and rebalancing read the same two numbers,
 * and capacity-to-the-ceiling is what they were after all along - a
 * punched page is re-faulted by the next carve.  Without a ceiling
 * (no arena) the pool's figures stand, as before. */
static unsigned int self_free_mb(void)
{
	unsigned long total = 0, used = 0, freeb = 0, mx, held;
	int active = 0;

	mx = pcache_arena_max_bytes;
	if (mx) {
		held = pcache_arena_held_bytes();
		return (unsigned int)((mx > held ? mx - held : 0) >> 20);
	}
	pcache_arena_hugepage_capacity(&active, &total, &used, &freeb);
	return (unsigned int)(freeb >> 20);
}

static unsigned int self_total_mb(void)
{
	unsigned long total = 0, used = 0, freeb = 0;
	int active = 0;

	if (pcache_arena_max_bytes)
		return (unsigned int)(pcache_arena_max_bytes >> 20);
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
		if (!pc_store_live(i))
			continue;   /* S69: a dropped collection */
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
 * 6MB on a 64MB donor truncated to exactly the band edge, self_per_mille 7 vs
 * mean+5 = 7 - and nothing ever moved). */
static unsigned int live_per_mille(unsigned int live_kb, unsigned int total_mb)
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
	return clpeers_by_id_live(&C.pt, node, now_ms());
}


/* ---- completion queues -------------------------------------------------
 * The queues live in clpend now (M2).  These keep the shape their callers
 * and cluster.h already know. */

void pc_cluster_worker_register(int worker, int efd)
{
	clpend_cq_register(&C.pd, worker, efd);
}

static void post_done_ex(int worker, uint32_t req, int kind, int found,
		int ok, long long newval, int from_node, unsigned int ttl_left,
		const unsigned char *val, int vlen, int jop, int count,
		unsigned long long ver, int timedout)
{
	clpend_cq_post(&C.pd, worker, req, kind, found, ok, newval, from_node,
		ttl_left, val, vlen, jop, count, ver, timedout);
}

static void post_done(int worker, uint32_t req, int found,
		unsigned int ttl_left, const unsigned char *val, int vlen)
{
	post_done_ex(worker, req, PC_DONE_PULL, found, found, 0, 0,
		ttl_left, val, vlen, 0, 0, 0, 0);
}

int pc_cluster_drain(int worker, struct pc_pull_done *out, int max)
{
	return clpend_cq_drain(&C.pd, worker, out, max);
}

/* ---- pending table ------------------------------------------------------ */

/* Free a parked slot.  clpend owns the table; this is still the ONLY way
 * one is released, so that occupancy is exact, and it is idempotent - one
 * caller frees a slot that was answered earlier.
 *
 * The clpend lock must be held. */
static void pend_release(struct pending *p)
{
	clpend_release_locked(&C.pd, p);
}


/* set beside every 0 return from a begin(); see PC_CLFAIL_* */
static __thread int cl_fail;

int pc_cluster_last_fail(void)
{
	return cl_fail;
}

int pc_cluster_pend_cap(void)
{
	return C.pd.cap;
}

static uint32_t pend_alloc(int kind, int expect, int extra_ms,
		struct pending **out)
{
	uint32_t req;
	int reason;

	req = clpend_alloc(&C.pd, kind, expect, extra_ms, pc_worker_id(),
		now_ms(), C.pull_timeout_ms, out, &reason);
	if (!req) {
		unsigned long long n = clpend_exhausted(&C.pd);
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
				C.pd.cap, n);
		}
		cl_fail = reason;
	}
	return req;
}

static size_t build_pull_req(unsigned char *msg, uint32_t req,
		const char *col, size_t collen, const char *key, size_t klen)
{
	struct clpull_req q;

	q.req = req;
	q.node = C.node_id;
	q.col = col; q.collen = (unsigned int)collen;
	q.key = key; q.klen = (unsigned int)klen;
	return clpull_req_write(msg, clpull_req_size(q.collen, q.klen),
		M_PULL_REQ, &q);
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
	int up = 0;
	size_t n;

	cl_fail = PC_CLFAIL_NONE;
	if (!C.enabled || collen > 255 || klen > 4096) {
		cl_fail = PC_CLFAIL_ROUTE;
		__atomic_fetch_add(&C.st.fwd_no_route, 1, __ATOMIC_RELAXED);
		return 0;
	}
	up = clpeers_count_live(&C.pt, now_ms());
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
	broadcast_live(msg, n, now_ms());
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
	struct clfwd_op f = {
		.req = req, .node = (unsigned int)C.node_id,
		.op = (unsigned char)op, .ttl_rel = ttl_rel, .delta = delta,
		.col = col, .collen = (unsigned int)collen,
		.key = key, .klen = (unsigned int)klen,
		.val = val, .vlen = (unsigned int)vlen,
	};

	return clfwd_op_build(msg, M_FWD_OP, &f);
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
		clpend_lock(&C.pd);
		pend_release(slot);
		clpend_unlock(&C.pd);
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
	int up;
	size_t n;

	/* col/key must fit the slot: the transform re-sends them */
	if (!C.enabled || collen >= 40 || klen >= 256 || vlen > MAX_FWD_VAL)
		return 0;
	up = clpeers_count_live(&C.pt, now_ms());
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
	broadcast_live(msg, n, now_ms());
	__atomic_fetch_add(&C.st.pull_sent, 1, __ATOMIC_RELAXED);
	return req;
}

/* ---- S73: replicate on the write ------------------------------------------ */

/* Every write in an eager collection goes to every live peer NOW, from
 * the worker that took it - fire-and-forget, the forward plane's shape:
 * one seal_send per peer and return.  The write grows by an enqueue,
 * never by a round trip.  No TTL filter: a 1 s key gets its copy, which
 * is what redundancy for short-lived data means (S73, the operator's
 * decision; the sweep's 60 s "dying soon" rule left every liveness
 * beacon on one node).  The periodic sweep is repair behind this - a
 * peer that was down, a datagram that was lost - not delivery.
 *
 * On the wire this is one M_REPL_MANY group holding one record, with
 * req 0: the receiver stores it PASSIVE and off the WAL as it does any
 * replica, and does not ack it.  Only records this node authored are
 * pushed; the callers guarantee that. */
/* S110: the write-path push group, per THREAD per peer.  Every thread
 * that pushes (a worker for its client writes) keeps its own open
 * M_REPL_MANY group for each live peer and touches nothing shared: it
 * appends without a lock, seals and sends the group it fills, and
 * flushes its own aged groups when the cluster loop asks - the ask is
 * an RPC handled on the thread itself.  The peer's mutex that used to
 * serialise four workers through one group per peer was the eager
 * write ceiling: 57% of every worker's time in futex, 350k records/s
 * flat from 50 to 400 clients, +52% with one peer instead of two
 * (§12ci, step 3).  Groups are a quarter as full at the same rate and
 * the datagram count rises accordingly; the 3 ms bound on a lone
 * write holds, since the flush RPC rides the loop's 3 ms poll. */

/* A ready group leaves here: clpush decides WHEN, this seals and sends,
 * because the datagram is clwire's (M6) and the counters are the
 * cluster's.  Handed to clpush as its send callback. */
static void wgroup_send_cb(struct wgroup *g, void *ctx)
{
	(void)ctx;
	if (!g->qcount)
		return;
	g->q[0] = M_REPL_MANY;
	p32(g->q + 1, 0);                  /* req 0: no ack, see above */
	p16(g->q + 5, (uint16_t)C.node_id);
	p16(g->q + 7, (uint16_t)g->qcount);
	seal_send(&g->to, g->q, g->qlen);
	__atomic_fetch_add(&C.px.repl_pushed, g->qcount, __ATOMIC_RELAXED);
	__atomic_fetch_add(&C.px.repl_groups, 1, __ATOMIC_RELAXED);
	clpush_sent(&C.wp, g);
}

/* runs ON the thread whose groups these are (an RPC, or the thread
 * itself): any of its groups older than REPL_FLUSH_MS goes out now */
static void repl_flush_mine(long long now)
{
	clpush_flush_mine(&C.wp, pc_worker_id(), now, wgroup_send_cb, NULL);
}

static void repl_flush_rpc(int sender, void *arg)
{
	(void)sender; (void)arg;
	repl_flush_mine(now_ms());
}

/* S105: the cluster loop's timer - any group older than REPL_FLUSH_MS
 * goes out now, so a lone write is never held for more than that.
 * S110: the groups belong to their threads, so the loop asks every
 * thread to flush its own, at most once per REPL_FLUSH_MS. */
static void repl_flush_due(long long now)
{
	static long long last;

	if (!clpush_open(&C.wp))
		return;
	if (now - last < REPL_FLUSH_MS)
		return;
	last = now;
	ipc_send_rpc_all(repl_flush_rpc, NULL);
}

/* S73 / S105: the write-path push.  One record, appended to the open
 * M_REPL_MANY group of every live peer - the coalescing buffer S73
 * decided on: the write's cost grows by an append, not by a seal and a
 * syscall per peer.  S110: the group is THIS THREAD's (no lock): sent
 * by this thread when it fills it or finds it older than REPL_FLUSH_MS
 * on the next append, or when the cluster loop's flush RPC lands here.
 * A record too large for a group goes alone. */
/* S69 */
void pc_cluster_col_announce(const char *name, size_t nlen, int buckets_log2,
		int drop)
{
	unsigned char msg[16 + PC_COL_NAME_MAX];
	unsigned long long gen = pc_lamport_now();
	long long now = now_ms();
	struct clcol_ann a;

	if (!C.enabled || nlen == 0 || nlen >= PC_COL_NAME_MAX)
		return;
	memset(&a, 0, sizeof a);
	a.op = drop ? CLCOL_OP_DROP : CLCOL_OP_SET;
	a.buckets_log2 = buckets_log2;
	a.gen = gen;
	a.name = name;
	a.nlen = (unsigned int)nlen;
	broadcast_live(msg, clcol_write(msg, sizeof msg, M_COL_SET, &a), now);
}

/* S69: a rename, which needs both names in the one datagram */
void pc_cluster_col_announce2(const char *from, size_t flen, const char *to,
		size_t tlen)
{
	unsigned char msg[16 + 2 * PC_COL_NAME_MAX];
	unsigned long long gen = pc_lamport_now();
	long long now = now_ms();
	struct clcol_ann a;

	memset(&a, 0, sizeof a);
	if (!C.enabled || flen == 0 || flen >= PC_COL_NAME_MAX ||
	        tlen == 0 || tlen >= PC_COL_NAME_MAX)
		return;
	a.op = CLCOL_OP_RENAME;
	a.gen = gen;
	a.name = from;
	a.nlen = (unsigned int)flen;
	a.to = to;
	a.tlen = (unsigned int)tlen;
	broadcast_live(msg, clcol_write(msg, sizeof msg, M_COL_SET, &a), now);
}

void pc_repl_push(const char *col, size_t collen, const char *key,
		size_t klen, const char *val, size_t vlen, unsigned int exp,
		unsigned long long ver)
{
	unsigned char rec[MIG_RHDR + 255 + 4096 + MAX_FWD_VAL + 8];
	unsigned int now = get_ticks(), ttl_left = 0;
	long long nowms;
	size_t n;
	int i;

	if (!C.enabled || collen == 0 || collen > 255 || klen == 0 ||
	        klen > 4096)
		return;
	if (vlen > MAX_FWD_VAL) {
		C.px.migrate_skipped_big++;    /* the same ceiling the sweep has */
		return;
	}
	if (exp) {
		if (exp <= now)
			return;                    /* already gone */
		ttl_left = exp - now;
	}
	{
		struct clmig_rec rr = {
			.ttl_left = ttl_left,
			.col = col, .collen = (unsigned int)collen,
			.key = key, .klen = (unsigned int)klen,
			.val = val, .vlen = (unsigned int)vlen,
			.ver = ver
		};

		clmig_rec_write(rec, &rr);
	}
	n = clmig_rec_size((unsigned int)collen, (unsigned int)klen,
		(unsigned int)vlen);
	nowms = now_ms();
	for (i = 0; i < C.pt.n_peers; i++) {
		struct peer *p = &C.pt.peers[i];

		if (!clpeers_live(p, nowms))
			continue;
		clpush_append(&C.wp, pc_worker_id(), i, &p->addr, rec, n,
			nowms, wgroup_send_cb, NULL);
	}
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
	{
		struct clfwd_key fk = {
			.col = col, .collen = (unsigned int)collen,
			.key = key, .klen = (unsigned int)klen };

		n = clfwd_tomb_build(msg, M_TOMBSTONE, &fk);
	}
	for (i = 0; i < C.pt.n_peers; i++)
		seal_send(&C.pt.peers[i].addr, msg, n);
	__atomic_fetch_add(&C.st.tomb_sent, 1, __ATOMIC_RELAXED);
}

/* ---- the peer thread ---------------------------------------------------- */


static void handle_pull_req(const unsigned char *pt, size_t n,
		const struct sockaddr_in *from)
{
	pcache_htable_t *ht;
	unsigned char rsp[CLPULL_RHDR + 65536 + 64];
	unsigned long long ver = 0;
	struct clpull_req q;
	struct clpull_rsp out;
	unsigned int exp = 0;
	str k, v;
	size_t rn;
	int found = 0, rc;

	if (!clpull_req_parse(pt, n, &q))
		return;
	C.st.pull_served++;

	v.s = NULL;
	v.len = 0;
	ht = pc_store_find(q.col, q.collen);
	if (ht) {
		k.s = (char *)q.key;
		k.len = (int)q.klen;
		/* value and version from ONE read: a second lookup for the
		 * version could pair these bytes with a LATER write's number,
		 * and the receiver would then refuse that write as older */
		rc = pcache_ht_fetch_ver(ht, &k, &v, &exp, &ver);
		found = rc == 0;
	}
	out.req = q.req;
	out.node = C.node_id;
	{
		unsigned int now = get_ticks();

		out.ttl_left = found && exp ? (exp > now ? exp - now : 1) : 0;
	}
	/* Too big for the datagram plane: reported as ABSENT.  Decided here
	 * rather than in the codec, and note what does NOT happen - ttl_left
	 * keeps the value computed above, exactly as the pre-extraction code
	 * left it when it zeroed found, vlen and ver in place. */
	if (found && (size_t)v.len > MAX_DGRAM - CLPULL_RHDR)
		found = 0;
	out.found = found;
	out.vlen = found ? (uint32_t)v.len : 0;
	out.ver = found ? ver : 0;
	out.val = v.s;
	rn = clpull_rsp_write(rsp, sizeof rsp, M_PULL_RSP, &out);
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
	{
		struct clfwd_key fk = { .col = col, .collen = cn,
			.key = key, .klen = kn };

		n = clfwd_demote_build(msg, M_DEMOTE, (unsigned int)winner_node,
			&fk);
	}
	seal_send(&pr->addr, msg, n);
	C.px.demotes_sent++;
}

/* S69: a delete reaches the table a resize is filling as well as the live
 * one - the copier has already carried some keys across, and a key deleted
 * after it passed would come back at the swap. */
static int store_remove(pcache_htable_t *ht, const str *k)
{
	pcache_htable_t *sh = pc_store_resize_shadow(ht);

	if (sh)
		(void)pcache_ht_remove(sh, k);
	return pcache_ht_remove(ht, k);
}

static void handle_pull_rsp(const unsigned char *pt, size_t n)
{
	struct clpend_ans_out a;
	struct clpull_rsp r;
	uint32_t req, vlen;
	int found, from;
	unsigned int ttl_left;
	unsigned long long ver;

	if (!clpull_rsp_parse(pt, n, &r))
		return;
	req = r.req;
	from = r.node;
	found = r.found;
	ttl_left = r.ttl_left;
	vlen = r.vlen;
	ver = r.ver;
	/* the declared value must be wholly present - a truncated reply is
	 * dropped, never half-applied */
	if (CLPULL_RHDR + vlen > n)
		return;
	pc_lamport_observe(ver);

	/* what the reply MEANS is clpend's; every side effect below is
	 * this file's.  The decision returns with the lock released and
	 * everything it hands back COPIED out of the slot - which is why
	 * the reconcile arm can touch the store safely. */
	switch (clpend_answer(&C.pd, req, found, from,
			/* only the probe transform uses it, and only a
			 * positive can transform - so a miss pays nothing */
			found ? now_ms() + C.pull_timeout_ms + 700 : 0, &a)) {
	case CLPEND_ANS_PROBE_FWD: {
		/* the responsible holder answered: the park became a
		 * forward to it, and the same req rides the datagram */
		unsigned char fmsg[32 + 256 + 4096 + MAX_FWD_VAL];
		struct peer *pr;
		int op = a.probe_op == 2 ? 2 : 0;
		size_t fn;

		pc_loc_set(a.col, a.collen, a.key, a.klen, from);
		pr = peer_by_node(from);
		if (pr) {
			fn = build_fwd_op(fmsg, req, op, a.ttl_rel, a.by,
				a.col, a.collen, a.key, a.klen, a.stash,
				(size_t)a.stash_len);
			if (seal_send(&pr->addr, fmsg, fn) == 0)
				C.px.fwd_sent++;
			else
				C.px.fwd_fails++;
			/* a lost send: the deadline refuses honestly */
		}
		free(a.stash);
		return;
	}
	case CLPEND_ANS_HIT:
		C.st.pull_hits++;
		post_done_ex(a.worker, req, PC_DONE_PULL, 1, 1, 0, from,
			ttl_left, (const unsigned char *)r.val, (int)vlen, 0, 0, ver, 0);
		return;

	case CLPEND_ANS_RACE:
		LM_WARN("cluster: birth race on a key - demoting node %d "
			"in favour of node %d\n", a.loser, a.winner);
		send_demote(a.loser, a.winner, a.col, a.collen, a.key, a.klen);
		pc_loc_set(a.col, a.collen, a.key, a.klen, a.winner);
		return;

	case CLPEND_ANS_MISS:
		C.st.pull_misses++;
		post_done(a.worker, req, 0, 0, NULL, 0);
		return;

	case CLPEND_ANS_RESUME:
		/* fleet-wide confirmed absent: replay the write on the
		 * worker, the probe suppressed */
		C.st.pull_misses++;
		post_done_ex(a.worker, req, PC_DONE_SET_RESUME, 0, 1, a.by, 0,
			a.ttl_rel, (const unsigned char *)a.stash,
			a.stash_len, a.probe_op, 0, 0, 0);
		free(a.stash);
		return;

	case CLPEND_ANS_RECONCILE: {
		/*
		 * B4.  Nobody in the fleet has this key, and in eager mode
		 * everyone holds everything - so it was DELETED while this
		 * node was down, and replay brought it back.  Drop it, and
		 * log the delete so a second restart does not resurrect it.
		 *
		 * Done here rather than posted to a worker: nothing is
		 * waiting for this answer, and the answer carries the key
		 * it asked about.
		 */
		pcache_htable_t *ht = pc_store_find(a.col, a.collen);
		str k;

		if (ht) {
			extern void pc_wal_del(const char *, const char *, int);
			char colz[64];

			k.s = a.key;
			k.len = (int)a.klen;
			if (store_remove(ht, &k) == 1) {
				memcpy(colz, a.col, a.collen);
				colz[a.collen] = 0;
				pc_wal_del(colz, a.key, (int)a.klen);
				C.st.reconciled++;
				LM_NOTICE("cluster: reconcile dropped %.*s/%.*s "
					"- recovered from the WAL but no node "
					"in the fleet still has it\n",
					(int)a.collen, a.col,
					(int)a.klen, a.key);
			}
		}
		return;
	}
	default:                       /* WAIT, and IGNORE */
		return;
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

	{
		struct clfwd_op f;

		if (!clfwd_op_parse(pt, n, &f))
			return;      /* truncated: dropped, never half-applied */
		req = f.req;
		op = f.op;
		ttl_rel = f.ttl_rel;
		delta = f.delta;
		cn = f.collen;
		kn = f.klen;
		vlen = f.vlen;
		memcpy(colz, f.col, cn);
		colz[cn] = 0;
	}
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
			ok = store_remove(ht, &k) == 1;
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
	{
		struct clfwd_ack a = { .req = req, .ok = ok, .newval = newval };

		seal_send(from, ack, clfwd_ack_build(ack, M_FWD_ACK, &a));
	}
}

static void handle_fwd_ack(const unsigned char *pt, size_t n)
{
	uint32_t req;
	long long newval;
	int ok, worker = -1, kind = 0;

	{
		struct clfwd_ack a;

		if (!clfwd_ack_parse(pt, n, &a))
			return;
		req = a.req;
		ok = a.ok;
		newval = a.newval;
	}
	/* only_unanswered: a probe transformed into a forward completes on
	 * the holder's ack, so a stray second ack must not complete it
	 * twice */
	if (clpend_retire(&C.pd, req, 1, &worker, &kind))
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
	unsigned char ack[CLMIG_ACK_LEN];
	struct clmig_single sg;
	uint32_t req, ttl_left;
	unsigned int cn, kn, vlen, exp = 0;
	int ok = 0;
	str k, v;
	char colz[256];

	/* MIG_RHDR, deliberately: this frame's own header is 14, but the
	 * pre-extraction code required 19 here and that is the behaviour a
	 * peer sees.  Preserved rather than quietly relaxed. */
	if (n < MIG_RHDR || !clmig_single_parse(pt, n, &sg))
		return;
	req = sg.req;
	ttl_left = sg.ttl_left;
	cn = sg.collen;
	kn = sg.klen;
	vlen = sg.vlen;
	memcpy(colz, sg.col, cn);
	colz[cn] = 0;

	ht = pc_store_find(colz, cn);
	if (ht) {
		k.s = (char *)sg.key;
		k.len = (int)kn;
		v.s = (char *)sg.val;
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
	seal_send(from, ack,
		clmig_ack_write(ack, sizeof ack, M_MIGRATE_ACK, req, ok, NULL));
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
	uint32_t req;
	unsigned int cnt, i, exp, stored = 0;
	size_t off = MIG_GHDR;
	pcache_htable_t *ht;
	str k, v;
	char colz[256];

	if (n < MIG_GHDR)
		return;
	req = clmig_group_req(pt);
	cnt = clmig_group_count(pt);
	for (i = 0; i < cnt; i++) {
		struct clmig_rec r;
		unsigned long long ver;
		unsigned int cn, kn;
		int rc;

		/* a truncated record stops the walk rather than reading past
		 * the datagram; what was stored is still acked */
		if (!clmig_rec_parse(pt, n, &off, &r))
			break;
		ver = r.ver;
		cn = r.collen;
		kn = r.klen;
		memcpy(colz, r.col, cn);
		colz[cn] = 0;
		/* the sender's clock is at least this far along.  The store
		 * folds it in too, but only for records that reach a
		 * collection - this covers the ones dropped before that. */
		pc_lamport_observe(ver);
		ht = pc_store_find(colz, cn);
		if (ht) {
			k.s = (char *)r.key;
			k.len = (int)kn;
			v.s = (char *)r.val;
			v.len = (int)r.vlen;
			exp = r.ttl_left ? get_ticks() + r.ttl_left : 0;
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
	}
	/* req 0 is a write-path push (S73): fire-and-forget by design, so
	 * there is nobody to ack.  A sweep batch always carries a nonzero
	 * req and gets its ack as before. */
	if (!req)
		return;
	{
		unsigned int st = (unsigned int)stored;

		seal_send(from, ack, clmig_ack_write(ack, sizeof ack,
			M_MIGRATE_ACK, req, stored > 0, &st));
	}
}

/* the birth-race loser drops its copy and records the winner */
static void handle_demote(const unsigned char *pt, size_t n)
{
	extern void pc_wal_del(const char *, const char *, int);
	pcache_htable_t *ht;
	unsigned int cn, kn, winner;
	str k;
	char colz[256];

	{
		struct clfwd_key fk;

		if (!clfwd_demote_parse(pt, n, &winner, &fk))
			return;
		cn = fk.collen;
		kn = fk.klen;
		memcpy(colz, fk.col, cn);
		colz[cn] = 0;
		C.px.demotes_applied++;
		ht = pc_store_find(colz, cn);
		if (ht) {
			k.s = (char *)fk.key;
			k.len = (int)kn;
			if (store_remove(ht, &k) == 1)
				pc_wal_del(colz, k.s, k.len);
		}
		pc_loc_set(colz, cn, fk.key, kn, (int)winner);
	}
}

static void handle_tombstone(const unsigned char *pt, size_t n)
{
	pcache_htable_t *ht;
	unsigned int cn, kn;
	str k;

	{
		struct clfwd_key fk;

		if (!clfwd_tomb_parse(pt, n, &fk))
			return;
		cn = fk.collen;
		kn = fk.klen;
		C.st.tomb_applied++;
		pc_neg_set(fk.col, cn, fk.key, kn, C.tombstone_ms);
		ht = pc_store_find(fk.col, cn);
		if (ht) {
			k.s = (char *)fk.key;
			k.len = (int)kn;
			store_remove(ht, &k);
		}
	}
}

static void expire_pending(void)
{
	C.st.pull_timeouts += clpend_expire(&C.pd, now_ms());
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

static void ident_hex(const unsigned char *id, char *out);

/* S107: tell this node's clients that a member joined or was expelled -
 * the event, where to dial it, its id and identity, and how many
 * members there are now, so a client can check its next member list
 * against it.  Every node tells its OWN clients when its own view
 * changes, which the master's decisions reach within a beat; a client
 * dialled to a non-master hears it from that node. */
static void clients_membership(const char *event, struct peer *p, int node)
{
	char hx[33], buf[320];
	int n;

	if (p->has_ident)
		ident_hex(p->ident, hx);
	else
		hx[0] = 0;
	n = snprintf(buf, sizeof buf, "{\"notify\":\"membership\",\"event\":"
		"\"%s\",\"addr\":\"%s\",\"port\":%d,\"node\":%d,"
		"\"identity\":\"%s\",\"members\":%d}", event,
		inet_ntoa(p->addr.sin_addr), p->client_port, node, hx,
		clpeers_count_live(&C.pt, now_ms()) + 1);
	if (n > 0 && (size_t)n < sizeof buf) {
		pc_clients_notify(buf, (size_t)n);
		LM_INFO("cluster: telling this node's clients that node %d %s "
			"(%s:%d, %d member(s) now)\n", node, event,
			inet_ntoa(p->addr.sin_addr), p->client_port, clpeers_count_live(&C.pt, now_ms()) + 1);
	}
}

/* a member is gone from this node's view: purged, said goodbye, or
 * forgotten.  Reserves its binding (S90), tells the clients (S107),
 * and clears the slot. */
static void peer_gone(struct peer *p)
{
	if (p->clients_told) {
		p->clients_told = 0;
		clients_membership("expelled", p, p->told_node);
	}
	/* S90.  The log stays on this side: clres holds no opinion about
	 * what is worth saying, and p->node must be read before the store
	 * below clears it. */
	if (clres_note(&reserved, p->node, p->addr.sin_addr, p->ident,
			p->has_ident, now_ms(), RESERVE_MS))
		LM_NOTICE("cluster: node %d (%s) left - its id is reserved for "
			"it for %lld s\n", p->node, inet_ntoa(p->addr.sin_addr),
			RESERVE_MS / 1000);
	__atomic_store_n(&p->node, 0, __ATOMIC_RELEASE);
	shard_note_change();
}

/* S30: drop a peer we refuse to federate with (config mismatch).  Same
 * shape as GOODBYE - the slot stays, the node id goes to 0, which is
 * what every read path treats as "not a member". */
static void peer_forget(const struct sockaddr_in *from)
{
	struct peer *p = clpeers_by_addr(&C.pt, from);

	if (p && __atomic_load_n(&p->node, __ATOMIC_ACQUIRE))
		peer_gone(p);
}

static struct peer *peer_upsert(const struct sockaddr_in *addr)
{
	struct peer *p;
	int created;

	p = clpeers_upsert(&C.pt, addr, &created);
	if (created)
		shard_note_change();
	return p;
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
#define ident_proposed_id clinit_proposed_id   /* M13 */

/* S106: the identity file is TEXT - a version line, the identity as the
 * same 32 hex characters every log line and stats field shows, and a
 * check value - so an operator can read it, correlate it, and be TOLD
 * when it is damaged instead of having a fresh identity minted
 * underneath (S80's id churn by another door, and a silent one):
 *
 *   perfcached-node-identity 1
 *   identity 59bf87e400f2056b0615b69a55ee452f
 *   check 4b1d0f8a9c2e7d31
 *
 * The check is FNV-1a over the sixteen bytes.  It is not a tamper guard
 * - the 0600 mode is the guard, and the one number an operator might
 * want to change (the node id) is not in the file at all - it is a
 * torn-write and typo detector.  The legacy form, the bare sixteen
 * bytes, is read once and rewritten. */
#define IDENT_FILE   "node-identity"
#define IDENT_HEADER "perfcached-node-identity 1"

#define fnv1a64 clinit_fnv1a64      /* moved to clinit (M13) */

/* @n bytes from 2n hex characters; -1 on anything that is not hex */
#define hex2bin clinit_hex2bin      /* moved to clinit (M13) */

/* Written through a temp file, fsync, rename and a directory fsync -
 * the term's discipline - because the old truncate-then-write left an
 * EMPTY file behind a crash, and an empty file used to mean "mint a
 * new one".  0 written, -1 failed with errno set. */
static int ident_store(const char *dir, const unsigned char *id)
{
	char hex[33], fin[600], tmp[620], buf[128];
	int fd, dfd, n, e;

	ident_hex(id, hex);
	n = snprintf(buf, sizeof buf,
		IDENT_HEADER "\nidentity %s\ncheck %016llx\n", hex,
		(unsigned long long)fnv1a64(id, 16));
	snprintf(fin, sizeof fin, "%s/" IDENT_FILE, dir);
	snprintf(tmp, sizeof tmp, "%s.tmp.%d", fin, (int)getpid());
	/* 0600: it names this node to its peers */
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return -1;
	if (write(fd, buf, (size_t)n) != n || fsync(fd) != 0) {
		e = errno;
		close(fd);
		unlink(tmp);
		errno = e;
		return -1;
	}
	close(fd);
	if (rename(tmp, fin) != 0) {
		e = errno;
		unlink(tmp);
		errno = e;
		return -1;
	}
	dfd = open(dir, O_RDONLY | O_DIRECTORY);
	if (dfd >= 0) {
		fsync(dfd);
		close(dfd);
	}
	return 0;
}

/* What the file holds.  1 = the text form, valid, @id filled; 0 = the
 * legacy form (exactly sixteen raw bytes), @id filled; -1 = it does not
 * validate and @why says how.  Strict: three lines, nothing else - a
 * file an editor has been through (a fourth line, a CRLF) is refused,
 * not guessed at. */
static int ident_parse(const unsigned char *buf, size_t len,
		unsigned char *id, char *why, size_t wlen)
{
	return clinit_ident_parse(buf, len, id, why, wlen);
}

/* S80: carry one small state file from @from_dir into @to_dir if it is
 * absent there - the once-only migration when an admin introduces
 * [daemon] state_dir beside a WAL that had been serving as the state
 * directory.  Written temp + fsync + rename + directory fsync: a
 * half-carried term is not a smaller term, it is an arbitrary one.
 * 1 carried, 0 nothing to do, -1 failed (and logged). */
static int carry_file(const char *from_dir, const char *to_dir,
		const char *name)
{
	char src[512], dst[512], tmp[532], buf[4096];
	int in, out, dfd;
	ssize_t n;

	if (!from_dir || !to_dir || !*from_dir || !*to_dir ||
	        strcmp(from_dir, to_dir) == 0)
		return 0;
	snprintf(dst, sizeof dst, "%s/%s", to_dir, name);
	if (access(dst, F_OK) == 0)
		return 0;                      /* already there */
	snprintf(src, sizeof src, "%s/%s", from_dir, name);
	in = open(src, O_RDONLY);
	if (in < 0)
		return 0;                      /* nothing to carry */
	snprintf(tmp, sizeof tmp, "%s.carry", dst);
	out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (out < 0) {
		LM_ERR("cannot carry %s into %s: %s\n", name, to_dir,
			strerror(errno));
		close(in);
		return -1;
	}
	while ((n = read(in, buf, sizeof buf)) > 0)
		if (write(out, buf, (size_t)n) != n) {
			n = -1;
			break;
		}
	close(in);
	if (n < 0 || fsync(out) != 0) {
		LM_ERR("cannot carry %s into %s: %s\n", name, to_dir,
			strerror(errno));
		close(out);
		unlink(tmp);
		return -1;
	}
	close(out);
	if (rename(tmp, dst) != 0) {
		LM_ERR("cannot carry %s into %s: %s\n", name, to_dir,
			strerror(errno));
		unlink(tmp);
		return -1;
	}
	dfd = open(to_dir, O_RDONLY | O_DIRECTORY);
	if (dfd >= 0) {
		fsync(dfd);
		close(dfd);
	}
	LM_NOTICE("carried %s from %s into state_dir %s\n", name, from_dir,
		to_dir);
	return 1;
}

/* 0, or -1 when the persisted identity does not validate: that refuses
 * startup (S106) - never a guess, never a fresh mint over a damaged
 * file.  The remedy is in the message. */
static int identity_init(const char *state_dir, const char *legacy_dir)
{
	char path[512], why[96];
	unsigned char buf[256];
	int fd, got = 0, form;
	ssize_t n;

	C.ident_durable = 0;
	if (state_dir && *state_dir) {
		snprintf(path, sizeof path, "%s/" IDENT_FILE, state_dir);
		/* S80: an identity already persisted in the WAL directory
		 * is carried across once when state_dir is introduced
		 * beside it, so the node stays itself */
		if (legacy_dir)
			carry_file(legacy_dir, state_dir, IDENT_FILE);
		fd = open(path, O_RDONLY);
		if (fd >= 0) {
			n = read(fd, buf, sizeof buf);
			close(fd);
			if (n < 0) {
				LM_CRIT("cluster: cannot read %s: %s - refusing "
					"to start\n", path, strerror(errno));
				return -1;
			}
			form = ident_parse(buf, (size_t)n, C.self_ident, why,
				sizeof why);
			if (form < 0) {
				LM_CRIT("cluster: %s does not validate: %s - "
					"refusing to start.  Restore the file, or "
					"delete it to mint a NEW identity (this "
					"node then presents to the fleet as a new "
					"member)\n", path, why);
				return -1;
			}
			got = 1;
			if (form == 0) {
				/* the legacy sixteen bytes: read once, and
				 * rewritten in the form the next start parses */
				if (ident_store(state_dir, C.self_ident) == 0)
					LM_NOTICE("cluster: rewrote %s from the "
						"legacy sixteen-byte form to the "
						"text form\n", path);
				else
					LM_WARN("cluster: %s is in the legacy "
						"sixteen-byte form and could not be "
						"rewritten as text: %s - kept as "
						"is\n", path, strerror(errno));
			}
		} else if (errno != ENOENT) {
			LM_CRIT("cluster: cannot open %s: %s - refusing to "
				"start\n", path, strerror(errno));
			return -1;
		}
		if (!got) {
			randombytes_buf(C.self_ident, 16);
			if (ident_store(state_dir, C.self_ident) == 0)
				got = 1;
			else
				LM_ERR("cluster: cannot write %s: %s - this "
					"node's identity is EPHEMERAL\n", path,
					strerror(errno));
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
	return 0;
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

const char *pc_cluster_state_dir(void)
{
	return C.state_dir[0] ? C.state_dir : NULL;
}

int pc_cluster_state_tmpfs(void)
{
	return C.state_tmpfs;
}

/* the paths that learn a peer from something other than its own
 * heartbeat (an ASSIGN member list, a MASTER_ALIVE) carry no identity;
 * they pass this and keep the node-id behaviour until the peer's own
 * ALIVE arrives with the real thing */
static const unsigned char pd_noident[16];

static void peer_publish(struct peer *p, int node, unsigned int free_mb,
		unsigned int total_mb, unsigned int live_kb,
		const unsigned char *ident, unsigned int incarn,
		unsigned int entries, int start)
{
	int fresh, known = 0;

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
	 * answers it directly - it is regenerated every process start.
	 *
	 * S103: the record count and the start kind are known ONLY to the
	 * peer's own ALIVE, which is the call that carries an incarnation.
	 * The other callers - a JOIN, an ASSIGN list, a MASTER_ALIVE - know
	 * neither and used to store zeros here anyway, so on every
	 * non-master node the master's slot flapped at 1 Hz between what
	 * its heartbeat said and (0 records, unknown start).  Read at the
	 * wrong phase by lowest_live_sender(), the lowest id looked empty,
	 * the reader self-designated, and two senders walked one backfill:
	 * every CI run of eagertest and stalebackfilltest, never the build
	 * host, where the two datagrams happened to land the other way
	 * round.  What a caller does not know it does not write. */
	if (incarn) {
		p->entries = entries;          /* S82: state, not a timer */
		p->start_kind = (unsigned char)start;
		fresh = !p->has_ident || p->incarn != incarn ||
			memcmp(p->ident, ident, 16) != 0;
		known = p->has_ident;
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
		memset(p->repl_bfcycle, 0, sizeof p->repl_bfcycle);
		/* Resetting the mark is NOT enough on its own.  The steady
		 * sweep sends only records this node AUTHORED, because
		 * re-sending a copy it merely received would echo the
		 * keyspace around the fleet for ever.  A node that came back
		 * empty therefore gets nothing from peers holding only
		 * copies - which is exactly the case that left an eager
		 * fleet with one node at 0 entries indefinitely.  So arm a
		 * ONE-SHOT pass that includes the copies; it is bounded by a
		 * single clean cycle and cannot become an echo. */
		/* Arm it from what the node SAYS about its start (the ALIVE
		 * tail byte), never from its record count - see PC_START_*.
		 *
		 * COLD        - replayed nothing and still young: backfill.
		 * RECOVERED   - replayed a WAL.  It holds most of what it is
		 *               owed and a blind push of everything is the
		 *               wrong tool: the receive path refuses a copy
		 *               older than the one held, so the push would not
		 *               corrupt, only waste.  What it IS owed is a diff
		 *               (S73b), filed, not built.
		 * ESTABLISHED - past its first holdoff: nothing to do.  This is
		 *               what a node that just started sees of a running
		 *               fleet, so it is silent.
		 * UNKNOWN     - an older build: judged by its count as before,
		 *               so a rolling upgrade keeps working.
		 *
		 * And COLD arms only for a restart this node WITNESSED (it knew
		 * the previous incarnation) or when this node is itself past
		 * its cold window.  A node that just started first-sights a
		 * whole fleet that may still be young - with S73's write-path
		 * push a test fleet converges in a second and a restart within
		 * 30 s of start is ordinary - and it has nothing to give them:
		 * measured, the restarted node (lowest id, so the designated
		 * sender) pushed its 280 passive copies at both peers, the echo
		 * the passive flag exists to prevent. */
		if (incarn && ((start == PC_START_COLD &&
		        (known || self_start_kind() != PC_START_COLD)) ||
		        (start == PC_START_UNKNOWN && !entries))) {
			memset(p->backfill, 1, sizeof p->backfill);
			p->fresh_ms = p->last_seen_ms;
			p->cold_fresh = 1;
			LM_NOTICE("cluster: node %d started cold (%u records) - "
				"backfilling it\n", node, entries);
		} else if (incarn && start == PC_START_RECOVERED)
			LM_NOTICE("cluster: node %d restarted holding %u records "
				"recovered from its WAL - NOT backfilling (a recovered "
				"node needs a diff, not a blind push)\n", node, entries);
		else if (incarn && start == PC_START_UNKNOWN)
			LM_NOTICE("cluster: node %d (older build) restarted holding "
				"%u records - NOT backfilling (judged by its count: a "
				"recovered node needs a diff)\n", node, entries);
	}
	/* S83: a peer that restarted cold and now says it is complete - it
	 * pulled its bootstrap, or the push finished it - no longer earns
	 * the resync of this node's AUTHORED records from mark zero that
	 * its fresh incarnation set up: the pull carried them, and anything
	 * written since went by the write-path push.  This is a LATER
	 * heartbeat of the same incarnation, so it cannot live in the block
	 * above (boottest: 2000 duplicates at a node that had just pulled). */
	if (incarn && start == PC_START_ESTABLISHED && p->cold_fresh) {
		int c;

		p->cold_fresh = 0;
		for (c = 0; c < PC_MAX_COLLECTIONS; c++) {
			p->repl_mark[c] = get_ticks();
			p->repl_cursor[c] = 0;
			p->repl_dirty[c] = 0;
		}
	}
	{
		int was = __atomic_load_n(&p->node, __ATOMIC_RELAXED);

		__atomic_store_n(&p->node, node, __ATOMIC_RELEASE);
		/*
		 * Only a REAL change re-arms the reshard grace.  This used to
		 * fire unconditionally, and peer_publish() runs on EVERY
		 * heartbeat - so `get_ticks() - last_change_tick` never
		 * reached SHARD_GRACE_S and pc_shard_grace() was permanently
		 * true.  An owner's miss is supposed to be authoritative;
		 * instead every miss broadcast to all peers and a share of
		 * them waited out pull_timeout_ms.
		 *
		 * Measured on a 3-host shard cluster, 2026-09-12: with ~22%
		 * of reads missing, 5.3M misses produced 10.7M pull datagrams
		 * and 460k timeouts, and aggregate GET fell to 155k/s.  An
		 * idle cluster with stable membership still broadcast a lone
		 * miss (pull_sent +2) minutes after the last change.
		 *
		 * `fresh` is the signal the function already computes for the
		 * replication reset; a node-id change is the other real one.
		 */
		if (fresh || was != node)
			shard_note_change();
	}
}

/* order-independent membership digest over (node,ip) - the module's
 * MASTER_ALIVE digest shape.
 *
 * FNV-1a's constants, but a field goes in a WORD at a time rather than a
 * byte, so this is NOT FNV-1a and matches no published vector.  It does
 * not have to be: it only has to be stable across the fleet and
 * independent of peer order.  Deliberately left in that shape when the
 * basis was corrected - reshaping it too would be a second wire change
 * for nothing. */
static uint64_t member_digest(void)
{
	uint64_t d = 0;
	long long now = now_ms();
	int i;

	for (i = 0; i < C.pt.n_peers; i++) {
		uint64_t h = FNV1A64_BASIS;

		if (!clpeers_live(&C.pt.peers[i], now))
			continue;
		h = (h ^ (uint32_t)C.pt.peers[i].node) * FNV1A64_PRIME;
		h = (h ^ C.pt.peers[i].addr.sin_addr.s_addr) * FNV1A64_PRIME;
		d ^= h;
	}
	{
		uint64_t h = FNV1A64_BASIS;

		h = (h ^ (uint32_t)C.node_id) * FNV1A64_PRIME;
		h = (h ^ C.self_addr.sin_addr.s_addr) * FNV1A64_PRIME;
		d ^= h;
	}
	return d;
}

static void send_join_req(const struct sockaddr_in *to)
{
	unsigned char msg[CLMEMB_JOINREQ_LEN];
	struct clmemb_joinreq j;

	memset(&j, 0, sizeof j);
	j.tok = C.join_tok;
	/* S30: the joiner states its collection config UP FRONT, so a
	 * master that runs something else never assigns it an id - the
	 * join is refused rather than half-completed */
	j.mode = C.cfg_mode;
	j.eager = C.cfg_eager;
	j.cfg_digest = config_digest();
	/* the id this node would LIKE, derived from its identity so it is
	 * the same every start.  A proposal only - the master rejects it
	 * if taken, which is what keeps two identities that happen to hash
	 * alike from ending up on one id. */
	j.ident = C.self_ident;
	j.proposed = C.proposed_id;
	/* the incarnation too: identity alone cannot tell "the member you
	 * already know, re-joining" from "a second process claiming to be
	 * it", and only the first of those is ordinary traffic */
	j.incarn = C.self_incarn;
	/* S73b: the WAL posture, so a refusal can NAME it.  1 = no WAL,
	 * 2 = logs one; a build before the byte sends 41 and reads as unknown */
	j.wal = C.cfg_wal ? 2 : 1;
	seal_send(to ? to : &C.mcast_dst, msg,
		clmemb_joinreq_write(msg, sizeof msg, M_JOIN_REQ, &j));
	C.join_last_ms = now_ms();
	C.st.joins++;
}

/* S81: how this node started, for the ALIVE tail.  COLD lasts one sender
 * holdoff past READY - long enough for every peer to read a heartbeat and
 * arm the one-shot backfill, short enough that a node restarting later does
 * not take a running fleet for a roomful of newcomers.  ready_ms is stamped
 * lazily here, on the cluster thread, at the first heartbeat after READY. */
static int self_start_kind(void)
{
	if (pc_recovered_records() > 0)
		return PC_START_RECOVERED;
	/* S83: a node that pulled its bootstrap KNOWS it is complete - no
	 * holdoff, nothing for a peer to push; the flags they armed on our
	 * cold heartbeat drop as stale at their next tick */
	if (clboot_done(&C.bt) &&
	        pc_node_state() >= PC_NST_READY)
		return PC_START_ESTABLISHED;
	if (!C.ready_ms) {
		if (pc_node_state() < PC_NST_READY)
			return PC_START_COLD;
		C.ready_ms = now_ms();
	}
	return now_ms() - C.ready_ms < BACKFILL_SENDER_HOLDOFF_MS
		? PC_START_COLD : PC_START_ESTABLISHED;
}

static void send_alive(void)
{
	unsigned char msg[CLMEMB_ALIVE_LEN];
	struct clmemb_alive a;
	size_t len;

	memset(&a, 0, sizeof a);
	a.node = C.node_id;
	a.free_mb = self_free_mb();
	a.total_mb = self_total_mb();
	a.live_kb = self_live_kb();
	/* S30 trailer: what this node believes the cluster's collections
	 * ARE.  A peer that disagrees is refused rather than fed. */
	a.mode = C.cfg_mode;
	a.eager = C.cfg_eager;
	a.cfg_digest = config_digest();
	a.client_port = C.cfg_client_port;              /* S34 */
	/* identity + incarnation, then everything after them, are additive
	 * tail fields: a peer built before one reads the prefix it knows and
	 * ignores the rest.  The gates that make that work are clmemb's. */
	a.ident = C.self_ident;
	a.incarn = C.self_incarn;
	a.entries = self_entries();
	/* the Lamport clock rides the heartbeat: at 1 Hz every node folds in
	 * every other node's value, so the fleet converges continuously with
	 * no clock agreement and no extra traffic */
	a.lamport = pc_lamport_now();
	a.nstate = pc_node_state();                     /* B1 */
	a.mem_tier = pcache_mem.tier;   /* the arena's ACTUAL tier; 0 = none */
	a.resp_port = C.cfg_resp_port;                  /* S49 */
	a.start_kind = self_start_kind();               /* S81: PC_START_* */
	a.http_port = C.cfg_http_port;                  /* S78 */
	a.uptime_s = (uint32_t)pc_metrics_uptime();
	len = clmemb_alive_write(msg, sizeof msg, M_ALIVE, &a);
	seal_send(&C.mcast_dst, msg, len);
	C.st.hb_sent++;
	/* publish for the watchdog; a non-master's stale MASTER_ALIVE
	 * must not outlive the role, so clear it here */
	clwire_beat_store(&C.wr, CLWIRE_ALIVE, msg, len);
	if (C.role != PC_ROLE_MASTER)
		clwire_beat_store(&C.wr, CLWIRE_MALIVE, NULL, 0);
	clwire_mark_sent(&C.wr, now_ms());
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

	for (i = 0; i < (unsigned int)C.pt.n_peers &&
	        m->nnodes < PC_CLMAP_MAXNODE; i++) {
		struct peer *p = &C.pt.peers[i];
		struct pc_clmap_node *nd;
		int id = __atomic_load_n(&p->node, __ATOMIC_ACQUIRE);

		if (!id || !clpeers_live(p, now))
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
	for (i = 0; i < (unsigned int)C.pt.n_peers; i++) {
		struct peer *p = &C.pt.peers[i];
		int id = __atomic_load_n(&p->node, __ATOMIC_ACQUIRE);
		int pref = 0;              /* master_preference is not settable
		                            * yet; every node scores the same
		                            * and the tiebreak decides */

		if (!id || id == C.node_id || !clpeers_live(p, now))
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
	for (i = 0; i < (unsigned int)C.pt.n_peers; i++) {
		struct peer *p = &C.pt.peers[i];
		int id = __atomic_load_n(&p->node, __ATOMIC_ACQUIRE);

		if (!id || id == C.node_id || !clpeers_live(p, now))
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
 * that peer stays absent, and the map never becomes usable at all.
 *
 * A member's STATE counts too, and used not to.  The map is reissued on
 * a timer anyway (CLMAP_PUB_MS), so a node going FAILED did reach
 * clients - but up to five seconds late, and every one of those seconds
 * is a client still picking a node that has stopped taking writes.  The
 * timer is a catch-up for lost datagrams, not the mechanism; membership
 * moves publish IMMEDIATELY because "ownership follows the map and a
 * stale one keeps routing at a node that has gone", and a node that has
 * FAILED is the same sentence with a different verb.
 *
 * Self is checked separately: the loop skips this node, so a master
 * that fails would otherwise go on publishing itself as READY. */
static int clmap_membership_moved(void)
{
	long long now = now_ms();
	unsigned int i, want = 1;          /* self */
	const struct pc_clmap_node *me;

	if (!C.map_valid)
		return 1;
	me = pc_clmap_find(&C.map, (uint16_t)C.node_id);
	if (me && me->state != (uint8_t)pc_node_state())
		return 1;                  /* our own state moved */
	for (i = 0; i < (unsigned int)C.pt.n_peers; i++) {
		struct peer *p = &C.pt.peers[i];
		int id = __atomic_load_n(&p->node, __ATOMIC_ACQUIRE);
		const struct pc_clmap_node *nd;

		if (!id || id == C.node_id || !clpeers_live(p, now))
			continue;
		nd = pc_clmap_find(&C.map, (uint16_t)id);
		if (!nd) {
			if (p->nstate != PC_NST_READY)
				continue;  /* not eligible yet, not missing */
			return 1;          /* someone new became READY */
		}
		if (nd->state != (uint8_t)p->nstate)
			return 1;          /* a member's state moved */
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
	ml = pc_clmap_encode(m, msg + CLSYNC_ENV_MAP_AT,
		sizeof msg - CLSYNC_ENV_MAP_AT);
	if (ml <= 0)
		return;
	off = clsync_env_hist_at((uint32_t)ml);
	hl = pc_clhist_encode(&C.hist, msg + off, sizeof msg - off);
	if (hl < 0)
		return;
	off = clsync_env_finish(msg, sizeof msg, M_CLSYNC,
		(uint32_t)ml, (uint32_t)hl);
	if (!off)
		return;
	if (seal_send(&pr->addr, msg, off) == 0)
		C.sync_sent_n++;
}

static void handle_clsync(const unsigned char *pt, size_t n,
		const struct sockaddr_in *from)
{
	struct pc_clmap m;
	unsigned char ack[CLSYNC_ACK_LEN];
	const char *why = "?";
	const unsigned char *mp, *hp;
	uint32_t ml, hl;

	if (n < CLSYNC_ENV_HDR)
		return;
	C.sync_rx_n++;
	if (!clsync_env_parse(pt, n, &mp, &ml, &hp, &hl))
		goto bad;
	if (pc_clmap_decode(mp, ml, &m, &why) != 0)
		goto bad;
	if (pc_clhist_decode(hp, hl, &C.held_hist, &why) != 0)
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
	seal_send(from, ack,
		clsync_ack_write(ack, sizeof ack, M_CLSYNC_ACK, m.term, m.seq));
	return;
bad:
	if (++C.sync_bad_n % 100 == 1)
		LM_WARN("cluster: refusing a control-state sync (%s)\n", why);
}

static void handle_clsync_ack(const unsigned char *pt, size_t n)
{
	uint32_t term, seq;

	if (!clsync_ack_parse(pt, n, &term, &seq))
		return;
	C.sync_ack_n++;
	pc_clsync_ack(&C.sync, term, seq);
}

/* Give up mastership and re-join through @to.  ONE demotion path, on
 * purpose: a second one that forgot the join token or the deadline
 * would leave a node that is no longer master and never becomes a
 * member either.  Callers differ only in why they decided. */
static void yield_mastership(const struct sockaddr_in *to)
{
	C.role = PC_ROLE_JOINING;
	C.st.demotions++;
	C.join_tok = 0;
	randombytes_buf(&C.join_tok, 8);
	C.join_deadline_ms = now_ms() + JOIN_WAIT_MS;
	C.join_held = 0;
	C.join_defers = 0;
	send_join_req(to);
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
	if (pc_term_must_stepdown(C.role == PC_ROLE_MASTER, C.map.term,
	        m.term)) {
		/*
		 * RULE 3 (clterm.h): a master that observes a HIGHER term
		 * steps down immediately and unconditionally - no merge, no
		 * member count, no negotiation.  That is what bounds split
		 * brain to one detection interval rather than to a
		 * reconciliation timer, and it is what makes promoting a
		 * backup aggressively safe in the first place.
		 *
		 * Compared against the term of the map WE PUBLISHED, not
		 * against pc_term_current(): the current term is the highest
		 * this node has SEEN, and pc_term_observe() a few lines above
		 * has just folded the arriving one into it - so comparing
		 * with it asks whether a term beats itself, and no master
		 * would ever step down.  Our own map's term is the one we are
		 * asserting mastership under, and that is the thing being
		 * outranked.  (This is why the old logging-only version could
		 * never have fired as written.)
		 *
		 * Unconditional means unconditional: this deliberately does
		 * NOT consult the member count that handle_master_alive
		 * weighs.  A term is issued once and totally ordered; a
		 * member count is a local observation that two partitioned
		 * nodes can hold different views of at the same instant.
		 * Where the two disagree the term is the one that is right.
		 *
		 * The rule is CALLED, not re-spelled: pc_term_must_stepdown
		 * is what clterm.h documents and what test/clustersim.c
		 * exercises across thousands of schedules, and a second copy
		 * of the comparison here is a second thing to keep in step.
		 */
		LM_WARN("cluster: stepping down - a map at term %u outranks "
			"our own term %u (master is node %u)\n",
			m.term, C.map.term, m.master_id);
		yield_mastership(from);
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
	unsigned char msg[CLMEMB_MALIVE_LEN];
	struct clmemb_malive m;
	size_t len;

	memset(&m, 0, sizeof m);
	m.node = C.node_id;
	m.members = clpeers_count_live(&C.pt, now_ms()) + 1;
	m.member_digest = member_digest();
	m.free_mb = self_free_mb();
	m.total_mb = self_total_mb();
	m.live_kb = self_live_kb();
	/* S30: MASTER_ALIVE admits peers too, so it carries the config
	 * identity as well - otherwise a refused joiner still learns the
	 * master through it and the refusal is one-sided (measured) */
	m.mode = C.cfg_mode;
	m.eager = C.cfg_eager;
	m.cfg_digest = config_digest();
	/* The TERM we are asserting mastership under.  It rides the
	 * keepalive rather than only the map because the keepalive is what
	 * the split-brain decision is actually made on: MASTER_ALIVE flows
	 * every second, a map only on a membership change, so a rank-based
	 * cure reading this message resolves the argument long before a map
	 * could arrive.  Measured: with the term only on the map, the
	 * member-count/address cure seated a stale term-1 master over a
	 * legitimate term-2 one, every time.
	 *
	 * Appended, so a peer that stops reading at 35 is unaffected -
	 * the same length-gated extension this handler already uses for
	 * the config digest. */
	m.term = pc_term_current();
	len = clmemb_malive_write(msg, sizeof msg, M_MASTER_ALIVE, &m);
	seal_send(&C.mcast_dst, msg, len);
	clwire_beat_store(&C.wr, CLWIRE_MALIVE, msg, len);
}

/* Peer-thread-only 1 Hz emitter for long duties: a migration tick's
 * ack windows and the victim walk call this as they grind, so the node
 * keeps saying "alive" (and a master keeps saying "master") while it
 * works.  Emission only - no membership decisions, no draining - so it
 * is safe anywhere on the peer thread, including from a walk callback. */
static void beat_pump(void)
{
	if (now_ms() - clwire_sent_ms(&C.wr)
	        < 1000)
		return;
	if (C.role == PC_ROLE_MEMBER || C.role == PC_ROLE_MASTER)
		send_alive();
	if (C.role == PC_ROLE_MASTER)
		send_master_alive();
}

static void send_goodbye(void)
{
	unsigned char msg[CLMEMB_GOODBYE_LEN];

	seal_send(&C.mcast_dst, msg,
		clmemb_goodbye_write(msg, sizeof msg, M_GOODBYE, C.node_id));
}

/* master: allocate the lowest unused node id (rejoiners keep theirs) */
static int id_taken(int id)
{
	int i;

	if (id == C.node_id)
		return 1;
	if (clres_holds(&reserved, id, now_ms()))   /* S90: held for a departed member */
		return 1;
	for (i = 0; i < C.pt.n_peers; i++)
		if (C.pt.peers[i].node == id)
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

	{
		struct clmemb_assign h;

		h.tok = tok;
		h.your_id = your_id;
		h.master_id = C.node_id;
		h.count = 0;           /* patched below: the walk decides it */
		off = clmemb_assign_hdr_write(msg, sizeof msg, M_ASSIGN, &h);
	}
	/* self first, then every live peer except the addressee */
	{
		struct clmemb_member m;

		memset(&m, 0, sizeof m);
		m.node = C.node_id;
		m.addr = C.self_addr.sin_addr;
		m.port = C.self_addr.sin_port;
		m.free_mb = self_free_mb();
		m.total_mb = self_total_mb();
		m.live_kb = self_live_kb();
		off += clmemb_member_write(msg + off, sizeof msg - off, &m);
	}
	n = 1;
	for (i = 0; i < C.pt.n_peers; i++) {
		struct clmemb_member m;

		if (!clpeers_live(&C.pt.peers[i], now) ||
		        addr_eq(&C.pt.peers[i].addr, to))
			continue;
		memset(&m, 0, sizeof m);
		m.node = C.pt.peers[i].node;
		m.addr = C.pt.peers[i].addr.sin_addr;
		m.port = C.pt.peers[i].addr.sin_port;
		m.free_mb = C.pt.peers[i].free_mb;
		m.total_mb = C.pt.peers[i].total_mb;
		m.live_kb = C.pt.peers[i].live_kb;
		off += clmemb_member_write(msg + off, sizeof msg - off, &m);
		n++;
	}
	clmemb_assign_count_set(msg, n);
	seal_send(to, msg, off);
	C.st.assigns++;
}

static void become_master(int founded)
{
	/*
	 * S92: found under the id this identity has always proposed.
	 *
	 * A joiner's proposal is honoured by whoever is master
	 * (alloc_node_id), so an id already survives a restart back into a
	 * LIVE cluster.  The founder was the one node that threw its own
	 * proposal away and stamped itself 1 - which makes a whole-fleet
	 * restart, where every identity file is intact, precisely the case
	 * that churned the ids: whichever node happened to boot first
	 * became node 1 and the fleet re-numbered around it.  Two
	 * partitions that each found are worse still, both claiming 1 for
	 * two different identities, to be told apart on the merge.
	 *
	 * A founder has no peers and no reservations, so nothing is taken
	 * and this yields the proposal itself.  A node with no durable
	 * identity proposes 0 and still falls out of the scan as 1,
	 * exactly as before.
	 */
	if (!C.node_id)
		C.node_id = alloc_node_id(C.proposed_id);
	if (!C.node_id)
		C.node_id = 1;
	C.role = PC_ROLE_MASTER;
	/* founding or winning an election settles membership.  B4 will put
	 * a node with data to settle into RECOVERING first. */
	if (founded && any_eager() && !self_entries() &&
	        pc_recovered_records() == 0 &&
	        !clboot_done(&C.bt) &&
	        clpeers_count_live(&C.pt, now_ms()) > 0) {
		/* S83: a founder WITH live peers - a master that restarted
		 * faster than its death was noticed, so nobody answered its
		 * join - holds nothing they hold.  It pulls like a joiner
		 * before it serves; a founder alone has nobody to pull from. */
		pc_node_state_set(PC_NST_RECOVERING);
		clboot_arm(&C.bt, now_ms());
		LM_NOTICE("cluster: founding with %d live peer(s) while holding "
			"nothing - pulling a bootstrap before reporting ready\n",
			clpeers_count_live(&C.pt, now_ms()));
	} else
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
		"elected master", C.node_id, clpeers_count_live(&C.pt, now_ms()));
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
	for (i = 0; i < C.pt.n_peers; i++) {
		struct peer *p = &C.pt.peers[i];

		if (!__atomic_load_n(&p->node, __ATOMIC_ACQUIRE) ||
		        !p->has_ident)
			continue;
		if (!clpeers_live(p, now))
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

/* S90/S108: where an identity with a live reservation last lived, without
 * consuming the reservation.  1 = found, *@home filled. */
static void send_join_rej(const struct sockaddr_in *to, uint64_t tok,
		int reason)
{
	unsigned char msg[CLMEMB_REJ_LEN];

	seal_send(to, msg,
		clmemb_rej_write(msg, sizeof msg, M_JOIN_REJ, tok, reason));
}

/* S108: "your join is held" - the one answer that is neither an id nor a
 * refusal.  Sent on every retry the master holds, so the joiner's
 * patience is renewed only while the master is alive to renew it. */
static void send_join_wait(const struct sockaddr_in *to, uint64_t tok)
{
	unsigned char msg[CLMEMB_TOK_LEN];

	seal_send(to, msg,
		clmemb_tok_write(msg, sizeof msg, M_JOIN_WAIT, tok));
}

static void handle_join_wait(const unsigned char *pt, size_t n,
		const struct sockaddr_in *from)
{
	uint64_t tok;
	long long until;

	if (C.role != PC_ROLE_JOINING || !clmemb_tok_parse(pt, n, &tok))
		return;
	if (tok != C.join_tok)
		return;                        /* not our join */
	until = now_ms() + JOIN_HOLD_MS;
	if (until > C.join_deadline_ms)
		C.join_deadline_ms = until;
	if (!C.join_held) {
		C.join_held = 1;
		LM_NOTICE("cluster: the master at %s is holding this join - an "
			"identity like ours is still counted live there.  "
			"Waiting for it to lapse, not founding; a refusal "
			"follows if it never does\n", inet_ntoa(from->sin_addr));
	}
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
	int reason, have_reason;

	if (!clmemb_rej_parse(pt, n, &tok, &reason, &have_reason))
		return;
	if (tok != C.join_tok)
		return;                        /* not our join */
	if (have_reason && reason == PC_REJ_DUP_IDENT) {
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
			inet_ntoa(from->sin_addr), reason);
	}
	/* the ordinary shutdown path: the main thread joins the workers and
	 * tears down cleanly, rather than this thread calling exit() under
	 * everyone else's feet */
	kill(getpid(), SIGTERM);
}

static void handle_join_req(const unsigned char *pt, size_t n,
		const struct sockaddr_in *from)
{
	struct clmemb_joinreq jq;
	uint64_t tok;
	int proposed = 0;
	unsigned int j_incarn = 0;

	if (addr_eq(from, &C.self_addr) || !clmemb_joinreq_parse(pt, n, &jq))
		return;
	tok = jq.tok;
	if (jq.have & CLMEMB_J_PROP)
		proposed = jq.proposed;    /* derived from its identity */
	if (jq.have & CLMEMB_J_INCARN)
		j_incarn = jq.incarn;
	if (n >= 19) {
		uint64_t theirs;

		memcpy(&theirs, pt + 11, 8);
		if (theirs != config_digest() && C.cfg_authoritative) {
			/* S30: refuse the JOIN itself - no id is assigned, so
			 * the node never becomes a member at all */
			shout_mismatch(from, pt[9], pt[10], theirs,
				n >= 42 ? pt[41] : 0);
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
		/* The identity rides at byte 19 (send_join_req); it was read
		 * from 21 from the day the check was written, two bytes of a
		 * field the erasure-coded mode's removal had taken out of
		 * the message, so a clone's claim could never match a live
		 * member's identity and every clone was admitted - see S108.
		 * The suite that proves the refusal was not in `make check`. */
		if (n >= 35 && ident_claimed_live(pt + 19, j_incarn, from)) {
			char hx[33];

			ident_hex(pt + 19, hx);
			if (!clpeers_dup_persisted(&C.pt, from, now_ms())) {
				/* ambiguous: its predecessor may simply not
				 * have aged out yet.  Say so - S108: this used
				 * to answer NOTHING, and a joiner reads two
				 * seconds of silence as "no master" and founds
				 * its own cluster, so a clone became a second
				 * master under a duplicate identity.  A held
				 * joiner keeps retrying; if the incumbent really
				 * did die the claim lapses and the next attempt
				 * is assigned normally. */
				LM_WARN("cluster: join from %s claims identity "
					"%s, still held by a live member - "
					"holding it to see whether that member is "
					"actually gone\n",
					inet_ntoa(from->sin_addr), hx);
				send_join_wait(from, tok);
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
		clpeers_dup_forget(&C.pt, from);
		id = p->node;
		if (!id) {
			/* S90: a returning identity, or a replacement at the same
			 * address, gets the reserved id back - overstamping the
			 * proposal - unless a live member holds it meanwhile */
			const char *how = "";
			struct in_addr home;
			int rid;

			/* S108: TWO claimants of one lapsed identity - the node
			 * restarting at its own address and a copy of its file at
			 * another - are told apart by the address after all: the
			 * one at the identity's last home is the restart, and it
			 * was held (dup_seen) while the old entry was live.  The
			 * other is held until the home claimant is in, when the
			 * ordinary duplicate refusal takes it.  Found by clonetest:
			 * the copy's retry landed first after the lapse and took the
			 * reserved id. */
			if (n >= 35 && clres_home(&reserved, pt + 19, now_ms(), &home) &&
			        home.s_addr != from->sin_addr.s_addr &&
			        clpeers_dup_seen_recent(&C.pt, &home, now_ms())) {
				char hx[33], hs[24];

				ident_hex(pt + 19, hx);
				snprintf(hs, sizeof hs, "%s", inet_ntoa(home));
				LM_WARN("cluster: join from %s claims identity %s "
					"whose last home, %s, is itself re-joining - "
					"holding this one; the copy at a new address "
					"waits for the original\n",
					inet_ntoa(from->sin_addr), hx, hs);
				send_join_wait(from, tok);
				return;
			}
			rid = clres_take(&reserved, n >= 35 ? pt + 19 : NULL,
				&from->sin_addr, now_ms(), &how);

			if (rid && !id_taken(rid)) {
				id = rid;
				LM_NOTICE("cluster: %s returns as node %d (reserved, "
					"matched by %s)\n", inet_ntoa(from->sin_addr), id,
					how);
			}
		}
		if (!id)
			id = alloc_node_id(proposed);
		if (!id)
			return;
		peer_publish(p, id, p->free_mb, 0, 0, pd_noident, 0, 0, PC_START_UNKNOWN);
		send_assign(from, tok, id);
		return;
	}
	if (C.role == PC_ROLE_JOINING && addr_cmp(from, &C.self_addr) > 0)
		C.higher_joiner_ms = now_ms();   /* defer to the higher addr */
}

static void handle_assign(const unsigned char *pt, size_t n,
		const struct sockaddr_in *from)
{
	struct clmemb_assign as;
	struct clmemb_member mr;
	int i;

	if (!clmemb_assign_parse(pt, n, &as))
		return;
	/* the module's guard, transplanted: a master never accepts an
	 * ASSIGN - a stale one from a pre-promotion join would clobber
	 * the identity (demotion goes through handle_master_alive only) */
	if (C.role == PC_ROLE_MASTER)
		return;
	if (as.tok != C.join_tok)
		return;
	C.node_id = as.your_id;
	C.st.node_id = C.node_id;
	C.master_id = as.master_id;
	C.master_addr = *from;
	C.master_seen_ms = now_ms();
	/* clmemb_member_at bounds each record against the REAL length, so a
	 * count that overstates the frame stops at the last whole record */
	for (i = 0; i < as.count && clmemb_member_at(pt, n, i, &mr); i++) {
		struct sockaddr_in pa;
		struct peer *p;

		memset(&pa, 0, sizeof pa);
		pa.sin_family = AF_INET;
		pa.sin_addr = mr.addr;
		pa.sin_port = mr.port;
		if (addr_eq(&pa, &C.self_addr))
			continue;
		p = peer_upsert(&pa);
		if (p)
			peer_publish(p, mr.node, mr.free_mb,
				mr.total_mb, mr.live_kb,
				pd_noident, 0, 0, PC_START_UNKNOWN);
	}
	if (C.role != PC_ROLE_MEMBER) {
		C.role = PC_ROLE_MEMBER;
		/* S83: an eager member that holds NOTHING is not ready until
		 * it has pulled the keyspace - "ready" has to mean complete.
		 * The candidates come from heartbeats the fleet may not have
		 * sent us yet, so the pick is made on the tick, bounded. */
		if (any_eager() && !self_entries() &&
		        pc_recovered_records() == 0 &&
		        !clboot_done(&C.bt)) {
			pc_node_state_set(PC_NST_RECOVERING);
			clboot_arm(&C.bt, now_ms());
			LM_NOTICE("cluster: joined as node %d (master is node %d, "
				"%d member(s)) holding nothing - pulling a bootstrap "
				"before reporting ready\n", C.node_id, C.master_id,
				as.count + 1);
		} else {
			pc_node_state_set(PC_NST_READY);
			LM_NOTICE("cluster: joined as node %d (master is node %d, "
				"%d member(s))\n", C.node_id, C.master_id, as.count + 1);
		}
	}
}

static void handle_alive(const unsigned char *pt, size_t n,
		const struct sockaddr_in *from)
{
	struct clmemb_alive a;
	struct peer *p;

	if (addr_eq(from, &C.self_addr) || !clmemb_alive_parse(pt, n, &a))
		return;
	C.st.hb_seen++;
	if (a.have & CLMEMB_A_CFG) {
		if (a.cfg_digest != config_digest() && C.cfg_authoritative) {
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
			shout_mismatch(from, a.mode, a.eager, a.cfg_digest, 0);
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
		peer_publish(p, a.node, a.free_mb,
			(a.have & CLMEMB_A_MEM) ? a.total_mb : 0,
			(a.have & CLMEMB_A_MEM) ? a.live_kb : 0,
			(a.have & CLMEMB_A_IDENT) ? a.ident : pd_noident,
			(a.have & CLMEMB_A_IDENT) ? a.incarn : 0,
			(a.have & CLMEMB_A_ENTRIES) ? a.entries : 0,
			(a.have & CLMEMB_A_STARTKIND) ? a.start_kind
				: PC_START_UNKNOWN);
		if (a.have & CLMEMB_A_LAMPORT)
			pc_lamport_observe(a.lamport);
		if (a.have & CLMEMB_A_CLIPORT)
			p->client_port = a.client_port;   /* S34 */
		/* B1: a peer that predates the field reads as READY - which
		 * is exactly how it behaves, since it has no state to be in */
		p->nstate = (a.have & CLMEMB_A_STATE) ? a.nstate : PC_NST_READY;
		p->mem_tier = (a.have & CLMEMB_A_TIER)
			? a.mem_tier : 0;                   /* 0 = not reported */
		p->resp_port = (a.have & CLMEMB_A_RESPPORT)
			? a.resp_port : 0;                  /* S49 */
		p->http_port = (a.have & CLMEMB_A_HTTP) ? a.http_port : 0; /* S78 */
		p->boot_ms = (a.have & CLMEMB_A_HTTP)
			? p->last_seen_ms - (long long)a.uptime_s * 1000 : 0;
		/* S107: the first heartbeat with a door to dial is when a
		 * client can act on it - said once per membership.  A node
		 * restarted at the SAME address under a new identity lands
		 * in the same slot with a new id, and no purge ever runs
		 * for the old one - so the id the clients were told is
		 * retired first, and the one that replaced it announced. */
		{
			int now_id = __atomic_load_n(&p->node, __ATOMIC_ACQUIRE);

			if (p->clients_told && now_id && p->told_node != now_id) {
				p->clients_told = 0;
				clients_membership("expelled", p, p->told_node);
			}
			if (!p->clients_told && p->client_port && now_id) {
				p->clients_told = 1;
				p->told_node = now_id;
				clients_membership("joined", p, now_id);
			}
		}
	}
}

static void handle_master_alive(const unsigned char *pt, size_t n,
		const struct sockaddr_in *from)
{
	struct clmemb_malive m;
	struct peer *p;
	int their_id, their_count;

	if (addr_eq(from, &C.self_addr) || !clmemb_malive_parse(pt, n, &m))
		return;
	if (m.have & CLMEMB_M_CFG) {
		if (m.cfg_digest != config_digest() && C.cfg_authoritative) {
			/* a master we disagree with is not our master */
			shout_mismatch(from, m.mode, m.eager, m.cfg_digest, 0);
			C.cfg_refused++;
			peer_forget(from);
			return;
		}
	}
	their_id = m.node;
	their_count = m.members;
	if (C.role == PC_ROLE_MASTER) {
		/* split-brain cure: rank (member count, then address) */
		int mine = clpeers_count_live(&C.pt, now_ms()) + 1;

		/*
		 * RULE 3 FIRST, and this ordering is the whole point.  A term
		 * is issued once and totally ordered; a member count is a
		 * local observation, and an address is arbitrary.  Where they
		 * disagree the term is the one that is right - so it is
		 * checked before them, on the message they are checked on.
		 *
		 * Wiring rule 3 into the map handler alone was not enough:
		 * this cure fires on a 1 Hz keepalive and had already
		 * resolved the argument by the time any map arrived, seating
		 * a term-1 master over a term-2 one on address alone.
		 * Length-gated so a peer that predates the field simply falls
		 * through to the old ranking.
		 */
		if (m.have & CLMEMB_M_TERM) {
			uint32_t theirs_t = m.term;

			if (pc_term_must_stepdown(1, C.map.term, theirs_t)) {
				LM_WARN("cluster: stepping down - node %d holds "
					"term %u and we hold %u\n", their_id,
					theirs_t, C.map.term);
				yield_mastership(from);
				return;
			}
			/*
			 * A LOWER term never outranks us, whatever it counts
			 * or wherever it lives.  Without this the ordering is
			 * not an ordering: rule 3 makes us yield UP the term,
			 * and the member/address cure below could still make
			 * us yield DOWN it to the very node that just stepped
			 * aside - clterm.h rule 1, "a map whose term is below
			 * ours is REFUSED", applied to the keepalive that
			 * carries the same claim.
			 *
			 * The first cut of this shipped without the guard and
			 * passed its test by luck: the loser stopped beaconing
			 * a beat before the winner could act on the stale
			 * keepalive still in flight.
			 *
			 * EQUAL terms fall through on purpose.  Two nodes that
			 * promoted from the same seen term hold the same one,
			 * and rule 3's strict inequality has nothing to say -
			 * so the member count, then the address, breaks the
			 * tie.  That is a total order and it is what already
			 * resolves a double promotion (measured: ~2s).
			 */
			if (pc_term_cmp(C.map.term, theirs_t) == PC_TERM_STALE)
				return;
		}
		if (their_count > mine || (their_count == mine &&
		        addr_cmp(from, &C.self_addr) > 0)) {
			LM_WARN("cluster: yielding mastership to node %d "
				"(%d members vs my %d) - re-joining\n",
				their_id, their_count, mine);
			yield_mastership(from);
		}
		return;
	}
	/* the keepalive is the authoritative master-identity signal */
	C.master_id = their_id;
	C.master_addr = *from;
	C.master_seen_ms = now_ms();
	p = peer_upsert(from);
	if (p)
		peer_publish(p, their_id, m.free_mb,
			(m.have & CLMEMB_M_MEM) ? m.total_mb : 0,
			(m.have & CLMEMB_M_MEM) ? m.live_kb : 0,
			pd_noident, 0, 0, PC_START_UNKNOWN);
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
	for (i = 0; i < C.pt.n_peers; i++) {
		p = &C.pt.peers[i];
		if (addr_eq(&p->addr, from)) {
			if (__atomic_load_n(&p->node, __ATOMIC_ACQUIRE))
				peer_gone(p);
			break;
		}
	}
	if (C.role != PC_ROLE_MASTER && addr_eq(from, &C.master_addr))
		C.master_seen_ms = 0;          /* master left: elect at once */
}

/* S83: is there anything to pull?  Eager is a property of a collection
 * ([collection x] mode = eager) as much as of the fleet ([cluster] mode
 * = eager, which is C.cfg_eager); the pull serves every collection that
 * is eager, so this asks the collections. */
static int any_eager(void)
{
	int c;

	for (c = 0; c < pc_store_count(); c++)
		if (pc_store_live(c) && pc_store_eager_enabled(pc_store_ht(c)))
			return 1;
	return 0;
}

/* S83: who to pull the bootstrap from - live, READY, holding records
 * ahead of holding none, lowest id first - handed to the bulk thread.
 * 1 = a pull is on its way, 0 = nobody qualifies yet. */
static int boot_request(void)
{
	long long now = now_ms();
	struct sockaddr_in addr[BOOT_MAX_CAND];
	int id[BOOT_MAX_CAND], n = 0, i, j;
	unsigned int ent[BOOT_MAX_CAND];

	for (i = 0; i < C.pt.n_peers; i++) {
		struct peer *p = &C.pt.peers[i];
		int pid = __atomic_load_n(&p->node, __ATOMIC_ACQUIRE);
		unsigned int pe = p->entries;

		if (!pid || !clpeers_live(p, now) ||
		        p->nstate != PC_NST_READY)
			continue;
		/* insertion sort: (holds records) desc, id asc */
		for (j = n; j > 0; j--) {
			int before = (ent[j - 1] > 0) != (pe > 0)
				? (pe > 0) : pid < id[j - 1];

			if (!before)
				break;
			if (j < BOOT_MAX_CAND) {
				addr[j] = addr[j - 1];
				id[j] = id[j - 1];
				ent[j] = ent[j - 1];
			}
		}
		if (j < BOOT_MAX_CAND) {
			addr[j] = p->addr;
			id[j] = pid;
			ent[j] = pe;
		}
		if (n < BOOT_MAX_CAND)
			n++;
	}
	if (!n)
		return 0;
	if (!ent[0])
		return 2;                      /* every ready peer holds nothing:
		                                * there is nothing to pull */
	clboot_handoff(&C.bt, clbulk_lock(&C.bk), addr, id, n);
	LM_NOTICE("cluster: pulling a bootstrap from node %d (%u record(s) "
		"reported)%s\n", id[0], ent[0], n > 1 ? ", others as fallback"
		: "");
	return 1;
}

/* S83: the bootstrap decision, on the tick, for a member and a master
 * alike - a founder with live peers holding records pulls too.  The
 * arithmetic is clboot's; what stays here is the peer scan it needs, the
 * three messages an operator reads when a bootstrap gives up, and the
 * node-state change that follows each. */
static void boot_tick(long long now)
{
	int pending, failed_round, act;

	/* S83: a joiner that is still waiting to pull - the first
	 * heartbeats may not have reached it at ASSIGN time */
	if (pc_node_state() != PC_NST_RECOVERING || clboot_done(&C.bt))
		return;
	clboot_tick_state(&C.bt, clbulk_lock(&C.bk), &pending, &failed_round);
	act = clboot_before_pick(&C.bt, now, pending, failed_round);
	if (act == CLBOOT_GIVE_UP) {
		LM_WARN("cluster: no peer served a bootstrap in %d "
			"rounds - reporting ready and relying on the push "
			"backfill (the keyspace arrives over the next "
			"sweeps)\n", C.bt.rounds);
		pc_node_state_set(PC_NST_READY);
		return;
	}
	if (act != CLBOOT_PICK)
		return;
	act = clboot_after_pick(&C.bt, now, failed_round, boot_request());
	if (act == CLBOOT_EMPTY) {
		LM_NOTICE("cluster: every ready peer reports "
			"holding nothing - nothing to pull; ready\n");
		pc_node_state_set(PC_NST_READY);
	} else if (act == CLBOOT_DEADLINE) {
		LM_WARN("cluster: no ready peer to pull a "
			"bootstrap from - reporting ready and "
			"relying on the push backfill\n");
		pc_node_state_set(PC_NST_READY);
	}
}

/* the 1s membership cadence + the join FSM (called from the thread) */
/* S69: the created set to one peer - every runtime name this node knows,
 * live or tombstoned, each with the generation that last moved it.  Sent
 * once per peer, the first time it is seen, which is what carries a
 * create across a node's downtime. */
static void col_sync_peer(struct peer *p)
{
	unsigned char msg[16 + PC_COL_NAME_MAX];
	struct clcol_ann a;
	const char *name;
	unsigned long long gen;
	int i, bl, live;

	for (i = 0; i < pc_store_count(); i++) {
		size_t nl;

		if (pc_store_entry(i, &name, &bl, &gen, &live) != 0)
			continue;              /* not a runtime name */
		nl = strlen(name);
		if (nl == 0 || nl >= PC_COL_NAME_MAX)
			continue;
		memset(&a, 0, sizeof a);
		a.op = live ? CLCOL_OP_SET : CLCOL_OP_DROP;
		a.buckets_log2 = bl;
		a.gen = gen;
		a.name = name;
		a.nlen = (unsigned int)nl;
		seal_send(&p->addr, msg,
			clcol_write(msg, sizeof msg, M_COL_SET, &a));
	}
}

/* S125: bytes sitting in a receive queue right now.  SO_MEMINFO's first
 * word is sk_rmem_alloc, the bytes charged to the queue - measured: 0
 * empty, 23,040 with ten 1,400-byte datagrams waiting, 66,816 under a
 * flood.  FIONREAD cannot be used for this: on a Linux UDP socket it
 * answers the size of the FIRST datagram (measured 1,400 with 23,040
 * queued), so it reads as a flat number while a node drowns. */
static unsigned int rx_queue_bytes(int fd)
{
	unsigned int mi[16];
	socklen_t l = sizeof mi;

	if (fd < 0 || getsockopt(fd, SOL_SOCKET, SO_MEMINFO, mi, &l) != 0 ||
	        l < sizeof mi[0])
		return 0;
	return mi[0];
}

/* S125: the receive plane's rates, sampled ON READ by whichever thread is
 * answering the operator - not on any periodic tick.
 *
 * Both candidate ticks are starved by exactly the load these figures
 * exist to report, which was measured rather than assumed:
 *
 *   The CLUSTER thread is the subject of the measurement.  It is the one
 *   that falls behind, its 1 Hz duties run late when it does, and the
 *   tree already carries a heartbeat watchdog whose job is covering for
 *   it when it goes heads-down.  A sampler there goes quiet at the moment
 *   a node starts lagging.
 *
 *   The MAINTENANCE thread looked like the right home - it is where
 *   pc_obs_tick_1hz() takes the daemon's other per-second figures - and
 *   it is not.  Measured on a two-node eager pair under a three-writer
 *   drive: pc-maint stopped completing an iteration for minutes at a
 *   time, stuck in the S109 held-bytes walk contending with the applier
 *   on the overflow leg's single lock, and every 1 Hz duty behind it
 *   stopped with it - the splitter included, which is why the table sat
 *   at 8,832 buckets holding 1.26M entries.  The gauges froze at a stale
 *   reading while the node kept applying, which is the exact failure this
 *   task exists to prevent.  Filed as its own defect.
 *
 * What is NOT starved is the thread serving the operator: a lagging
 * node's client door stays fast, which is the whole premise of the task.
 * So the sample is taken by the reader, and the published rate is "since
 * the last read" measured against real elapsed milliseconds.  A floor of
 * RX_SAMPLE_MIN_MS keeps two readers arriving together from computing a
 * rate over a sliver; below it the previous reading is served again.
 *
 * The mutex makes the sample atomic between concurrent readers.  It is
 * off every hot path - only stats, /metrics and the status page take it. */
#define RX_SAMPLE_MIN_MS 500

static pthread_mutex_t rx_mx = PTHREAD_MUTEX_INITIALIZER;

static void rx_sample(void)
{
	unsigned long long applied, older, drops;
	long long now, dt;

	if (!C.enabled)
		return;
	pthread_mutex_lock(&rx_mx);
	now = now_ms();
	dt = now - C.rxs.at_ms;
	if (C.rxs.at_ms && dt < RX_SAMPLE_MIN_MS) {
		pthread_mutex_unlock(&rx_mx);
		return;                /* too soon to divide: the last reading stands */
	}
	applied = __atomic_load_n(&C.px.migrated_in, __ATOMIC_RELAXED);
	older = __atomic_load_n(&C.px.recv_older, __ATOMIC_RELAXED);
	drops = __atomic_load_n(&C.rxq_drops, __ATOMIC_RELAXED);
	if (!C.rxs.at_ms) {
		C.st.rx_applied_ps = C.st.rx_older_ps = C.st.rx_drops_ps = 0;
	} else {
		C.st.rx_applied_ps = (unsigned int)
			((applied - C.rxs.applied) * 1000 / (unsigned long long)dt);
		C.st.rx_older_ps = (unsigned int)
			((older - C.rxs.older) * 1000 / (unsigned long long)dt);
		C.st.rx_drops_ps = (unsigned int)
			((drops - C.rxs.drops) * 1000 / (unsigned long long)dt);
	}
	C.rxs.at_ms = now;
	C.rxs.applied = applied;
	C.rxs.older = older;
	C.rxs.drops = drops;
	pthread_mutex_unlock(&rx_mx);
}

static void membership_tick(void)
{
	long long now = now_ms();
	int i;

	/* S69: the created set to every live peer - at once when a peer is
	 * first seen, and again every COL_RESYNC_BEATS seconds.  The
	 * announcement a create sends is one fire-and-forget datagram, so a
	 * peer that was restarting, partitioned or simply unlucky misses it
	 * and would otherwise stay short a collection for ever; this is the
	 * anti-entropy behind it, the same posture the repair sweep has
	 * behind the write-path push.  A handful of ~20-byte datagrams per
	 * peer per interval, and idempotent by generation at the receiver. */
	{
		static int col_beat;
		int resync = ++col_beat % COL_RESYNC_BEATS == 0;

		for (i = 0; i < C.pt.n_peers; i++)
			if (clpeers_live(&C.pt.peers[i], now) &&
			        (resync || !C.pt.peers[i].col_synced)) {
				C.pt.peers[i].col_synced = 1;
				col_sync_peer(&C.pt.peers[i]);
			}
	}

	/* purge silent peers */
	for (i = 0; i < C.pt.n_peers; i++)
		if (C.pt.peers[i].node &&
		        now - C.pt.peers[i].last_seen_ms >= PEER_PURGE_MS)
			peer_gone(&C.pt.peers[i]);

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
		boot_tick(now);
		if (now - C.master_seen_ms >= MASTER_DEAD_MS) {
			/* the highest live (ip,port) self-promotes; stickiness
			 * holds because a live master never reaches this path */
			int best = 1;

			for (i = 0; i < C.pt.n_peers; i++)
				if (C.pt.peers[i].node &&
				        now - C.pt.peers[i].last_seen_ms < MASTER_DEAD_MS &&
				        addr_cmp(&C.pt.peers[i].addr, &C.self_addr) > 0) {
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
		boot_tick(now);                /* S83: a founder may be pulling */
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

/* S69: a collection created or dropped elsewhere in the fleet.  Applied
 * whatever this node's own allow_create says - the gate governs who may
 * ORIGINATE, and a collection set that differs per node is the failure
 * S69 exists to remove.  Idempotent: the generation orders the create
 * and the drop of one name, so a re-announce never undoes a later drop. */
static void handle_col_set(const unsigned char *pt, size_t n)
{
	char name[PC_COL_NAME_MAX];
	struct clcol_ann a;
	unsigned long long gen;
	size_t nl;
	int bl, drop;

	if (!clcol_parse(pt, n, &a))
		return;
	drop = a.op == CLCOL_OP_DROP;
	bl = a.buckets_log2;
	gen = a.gen;
	nl = a.nlen;
	/* the NAME BOUND is this file's: clcol refuses what will not fit the
	 * datagram, PC_COL_NAME_MAX is what the store will accept */
	if (nl >= PC_COL_NAME_MAX)
		return;
	memcpy(name, a.name, nl);
	name[nl] = 0;
	pc_lamport_observe(gen);
	if (a.op == CLCOL_OP_RENAME) {     /* S69: a rename, two names */
		char to[PC_COL_NAME_MAX];
		size_t tl = a.tlen;

		if (tl >= PC_COL_NAME_MAX)
			return;
		memcpy(to, a.to, tl);
		to[tl] = 0;
		if (pc_store_find(name, nl) && !pc_store_find(to, tl)) {
			(void)pc_store_rename(name, nl, to, tl, gen);
			LM_NOTICE("collection '%s' renamed to '%s' by the "
				"fleet\n", name, to);
		}
		return;
	}
	if (drop) {
		if (pc_store_find(name, nl))
			pc_store_drop_gen(name, nl, gen);
		else
			pc_store_note_drop(name, nl, gen);
	} else if (a.op == CLCOL_OP_RESIZE) {   /* S69: resize to bl */
		if (pc_store_find(name, nl))
			(void)pc_store_resize_start(name, nl, bl);
	} else if (!pc_store_find(name, nl)) {
		int rc = pc_store_create_gen(name, nl, bl, gen);

		if (rc == 0)
			LM_NOTICE("collection '%s' created by the fleet: "
				"2^%d buckets\n", name, bl);
		else if (rc != -4)
			LM_ERR("collection '%s' announced by a peer could not "
				"be created here (%d)\n", name, rc);
	}
	pc_store_persist();
}

/* S125: the per-socket drop count that rode in with a datagram.  The
 * kernel stamps each skb with sk_drops as it is QUEUED, so the value
 * reported is the drops that had happened by then - it lags by whatever
 * is still in the queue, which at a 1 Hz sample is nothing, and it is
 * exact rather than the host-wide RcvbufErrors a second daemon on the
 * same box would pollute.  It is 32 bits and wraps; the delta is taken in
 * 32-bit arithmetic and accumulated into a 64-bit total. */
static void note_ovfl(int slot, unsigned int now)
{
	if (!C.ovfl[slot].seen) {
		C.ovfl[slot].seen = 1;
	} else if (now != C.ovfl[slot].last) {
		__atomic_fetch_add(&C.rxq_drops, (unsigned long long)
			(unsigned int)(now - C.ovfl[slot].last),
			__ATOMIC_RELAXED);
	}
	C.ovfl[slot].last = now;
}

static int drain_one_fd(int fd, int slot)
{
	unsigned char buf[HDR_LEN + MAX_DGRAM + 64];
	unsigned char pt[MAX_DGRAM + 16];
	int handled = 0, seen = 0;

	while (seen++ < DRAIN_BATCH) {
		struct sockaddr_in from;
		unsigned long long ptlen = 0;
		struct msghdr mh;
		struct iovec iov;
		struct cmsghdr *cm;
		char cbuf[CMSG_SPACE(sizeof(unsigned int)) + 32];
		ssize_t r;

		iov.iov_base = buf;
		iov.iov_len = sizeof buf;
		memset(&mh, 0, sizeof mh);
		mh.msg_name = &from;
		mh.msg_namelen = sizeof from;
		mh.msg_iov = &iov;
		mh.msg_iovlen = 1;
		mh.msg_control = cbuf;
		mh.msg_controllen = sizeof cbuf;
		r = recvmsg(fd, &mh, 0);

		if (r <= 0)
			break;
		for (cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm))
			if (cm->cmsg_level == SOL_SOCKET &&
			        cm->cmsg_type == SO_RXQ_OVFL) {
				unsigned int d;

				memcpy(&d, CMSG_DATA(cm), sizeof d);
				note_ovfl(slot, d);
			}
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
		case M_COL_SET:   handle_col_set(pt, ptlen); break;   /* S69 */
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
			unsigned int mstored;
			int mok;

			if (!clmig_ack_parse(pt, ptlen, &mreq, &mok, &mstored))
				break;
			/* an ack for req 0 can only come from a peer on an
			 * older build acking a write-path push; it owns no
			 * window slot and must not free one */
			if (!mreq)
				break;
			/* S85: only a matching ack frees a window slot;
			 * the ack of a re-sent batch whose first ack
			 * arrived after all is a duplicate and frees
			 * nothing.  clmig_ack returns the records that
			 * slot carried, 0 for a duplicate. */
			clmig_ack(&C.mig, mreq);
			/* the long ack credits RECORDS; the short one from an
			 * older peer can only say "at least one" */
			if (ptlen >= CLMIG_ACK_LEN)
				C.px.migrated_out += mstored;
			else if (mok)
				C.px.migrated_out++;
			break; }
		case M_DEMOTE:    handle_demote(pt, ptlen); break;
		case M_JOIN_REQ:     handle_join_req(pt, ptlen, &from); break;
		case M_JOIN_REJ:     handle_join_rej(pt, ptlen, &from); break;
		case M_JOIN_WAIT:    handle_join_wait(pt, ptlen, &from); break;
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
	int n = drain_one_fd(C.fd, 0);

	if (C.mfd >= 0)
		n += drain_one_fd(C.mfd, 1);
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
			if (++tick % REPL_SWEEP_BEATS == 0)
				pc_rebalance_tick();
			last_hb = now;
		}
		/* the join FSM needs sub-second deadlines */
		if (C.role == PC_ROLE_JOINING && now >= C.join_deadline_ms)
			membership_tick();
		/* S105: an open push group must not wait for the 100 ms poll */
		if (poll(pf, 2, clpush_open(&C.wp)
		        ? REPL_FLUSH_MS : 100) <= 0) {
			repl_flush_due(now_ms());
			continue;
		}
		drain_dgrams();
		repl_flush_due(now_ms());
	}
	if (C.role != PC_ROLE_JOINING) {
		pc_node_state_set(PC_NST_DRAINING);
		send_goodbye();                /* leave cleanly: peers purge now */
	}
}

/*
 * The WAL has been discarding acknowledged writes for several seconds:
 * leave the client map so the load goes to nodes that can honour it.
 *
 * Same two steps a clean shutdown takes - DRAINING makes
 * pc_clsel_eligible() reject us, the goodbye makes peers purge us now
 * rather than after a liveness timeout - but the process stays up and
 * keeps serving READS, which are still correct: what is unsafe here is
 * accepting new writes, not answering for data already held.
 */
extern void pc_wal_on_shed(void (*fn)(void));

static void cluster_wal_shed(void)
{
	if (C.role == PC_ROLE_JOINING)
		return;
	/*
	 * FAILED, not DRAINING, and NO goodbye.  Draining means departure -
	 * hand the data over and go - and a node that borrows it to say
	 * "I am broken" strands every key it holds that has no TTL.  A
	 * goodbye would compound that by making peers purge us outright.
	 *
	 * FAILED keeps us a member whose state everyone can see: out of
	 * pc_clsel_eligible() so no client picks us for new work, still
	 * heartbeating so the fleet watches us recover, still answering
	 * reads because the data we hold is correct.
	 */
	pc_node_state_set(PC_NST_FAILED);
	LM_CRIT("cluster: node state -> FAILED after sustained WAL loss - "
		"still a member and still answering reads, but refusing "
		"writes until the storage or the [wal] sizing is fixed\n");
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
	{
		struct clfwd_json j = {
			.req = req, .node = (unsigned int)C.node_id,
			.jop = (unsigned char)jop,
			.flags = (unsigned char)(
				(nx ? CLFWD_J_NX : 0) | (xx ? CLFWD_J_XX : 0) |
				(mkpath ? CLFWD_J_MKPATH : 0) |
				(have_ttl ? CLFWD_J_HAVETTL : 0)),
			.ttl = (unsigned int)(ttl > 0 ? ttl : 0),
			.by = by,
			.col = col, .collen = (unsigned int)collen,
			.key = key, .klen = (unsigned int)klen,
			.path = path, .plen = (unsigned int)plen,
			.val = val, .vlen = (unsigned int)vlen,
		};

		n = clfwd_json_build(msg, M_FWD_JSON, &j);
	}
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
	uint32_t req, ttl;
	unsigned int cn, kn, pl2, vl;
	long long by, newval = 0;
	int jop, flags, rc, fraglen = 0, cnt = 0, st;
	char *frag = NULL, colz[256], pathz[512];
	const char *emsg = NULL, *kp = NULL, *vp = NULL;

	{
		struct clfwd_json j;

		if (!clfwd_json_parse(pt, n, &j))
			return;
		ttl = j.ttl;
		/* the two bounds the frame cannot express: colz and pathz are
		 * fixed buffers here, so a frame declaring more is refused */
		if (j.collen > 255 || j.plen > 511)
			return;
		req = j.req;
		jop = j.jop;
		flags = j.flags;
		by = j.by;
		cn = j.collen;
		kn = j.klen;
		pl2 = j.plen;
		vl = j.vlen;
		memcpy(colz, j.col, cn);
		colz[cn] = 0;
		memcpy(pathz, j.path, pl2);
		pathz[pl2] = 0;
		kp = j.key;
		vp = j.val;
	}
	ht = pc_store_find(colz, cn);
	if (!ht) {
		st = 2;
	} else {
		rc = pc_json_rmw(ht, kp, (int)kn,
			jop, pathz, (int)pl2,
			vp, (int)vl,
			by, (flags & 8) ? 1 : 0, (long long)ttl,
			flags & 1, (flags & 2) ? 1 : 0, (flags & 4) ? 1 : 0,
			&frag, &fraglen, &newval, &cnt, &emsg);
		st = rc == 0 ? 0 : rc == 1 ? 1 : 2;
	}
	if (fraglen > (int)MAX_FWD_VAL) {  /* fragment too big to ship */
		st = 2;
		fraglen = 0;
	}
	{
		struct clfwd_jack a = {
			.req = req, .st = (unsigned char)st,
			.jop = (unsigned char)jop, .newval = newval,
			.cnt = (unsigned int)cnt,
			.frag = frag, .fraglen = (unsigned int)fraglen,
		};
		size_t an = clfwd_jack_build(ack, M_FWD_JACK, &a);

		free(frag);
		seal_send(from, ack, an);
	}
}

static void handle_fwd_jack(const unsigned char *pt, size_t n)
{
	uint32_t req;
	long long newval = 0;
	unsigned int fraglen;
	int worker = -1, kind = 0, st, jop, cnt;

	{
		struct clfwd_jack a;

		if (!clfwd_jack_parse(pt, n, &a))
			return;
		req = a.req;
		st = a.st;
		jop = a.jop;
		newval = a.newval;
		cnt = (int)a.cnt;
		fraglen = a.fraglen;
	}
	/* the JSON ack retires whether or not the request was answered -
	 * unlike the FWD ack above; see clpend_retire */
	if (!clpend_retire(&C.pd, req, 0, &worker, &kind))
		return;
	if (kind != PC_DONE_JSON)
		return;
	post_done_ex(worker, req, PC_DONE_JSON, st == 0, st, newval, 0, 0,
		fraglen ? pt + 23 : NULL, (int)fraglen, jop, cnt, 0, 0);
}

/* ---- the rebalancer (donor-initiated, hysteresis, byte budget) ---------- */

void pc_proxy_get_stats(struct pc_proxy_stats *out)
{
	*out = C.px;
	out->loc_hits = clloc_loc_hits(&C.lc);
	out->loc_clears = clloc_loc_clears(&C.lc);
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
		/* Reshard and proxy placement skip a record that dies before
		 * it is next read: moving it is pure waste.  The eager sweep
		 * is REPAIR behind the write-path push (S73), so it keeps a
		 * skip too - but one derived from its own period: a record
		 * that expires within one sweep is not worth a repair
		 * datagram, anything longer-lived is.  Delivery itself has
		 * no threshold at all; that is pc_repl_push(). */
		unsigned int horizon = m->repl ? REPL_SWEEP_BEATS : 60;

		if (exp <= m->now + horizon) {
			if (m->repl)
				C.px.repl_skipped_dying++;
			return 0;                  /* dying soon: not worth moving */
		}
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
		/* since = 0 is NO lower bound: the fresh-peer reset and a
		 * backfill both mean "everything", and a record stamped in
		 * the daemon's first second carries wtick 0 */
		if (m->since && wtick <= m->since)
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
			/* the SAME record codec the datagram path below
			 * uses - this was a second hand-written copy of
			 * it, which is the drift clmig exists to stop */
			struct clmig_rec br = {
				.ttl_left = ttl_left,
				.col = m->col, .collen = cn,
				.key = key->s, .klen = (unsigned int)key->len,
				.val = val->s, .vlen = (unsigned int)val->len,
				.ver = ver
			};

			clmig_rec_write(rp, &br);
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
	{
		int opened = clmig_open_for(&C.migb, n, MIG_GHDR,
			MIG_GATHER_CAP, &msg);

		if (opened < 0)
			return -1;
		if (opened) {
			/* a fresh group: stamp its header.  clmig knows the
			 * shape [len2][header][records] and the sizes; the
			 * CONTENTS are the migration protocol's, so they are
			 * written here, beside the rest of it. */
			/* 31 bits: PEND_TAG is what marks a parked-request
			 * handle, and a migration group id must never look
			 * like one */
			req = ++C.next_req & ~PEND_TAG;
			if (!req)
				req = ++C.next_req & ~PEND_TAG;
			clmig_group_hdr(msg, MIG_GHDR,
				m->repl ? M_REPL_MANY : M_MIGRATE_MANY,
				req, C.node_id);
		}
	}
	{
		struct clmig_rec r = {
			.ttl_left = ttl_left,
			.col = m->col, .collen = cn,
			.key = key->s, .klen = (unsigned int)key->len,
			.val = val->s, .vlen = (unsigned int)val->len,
			.ver = ver,
		};

		clmig_rec_write(clmig_record_at(&C.migb), &r);
		clmig_record_done(&C.migb, n, 7);   /* count2 sits at +7 */
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
/* S85: re-send every in-flight batch whose ack is MIG_RETX_MS overdue,
 * up to MIG_RETX times.  The receiver stores a duplicate as "not newer"
 * and acks it again; the ack handler frees the slot on the first ack it
 * sees for the req and ignores the rest. */
/* clmig hands back the OFFSET of each datagram due for re-send and this
 * does the sending, which is how the module needs no seal_send of its
 * own - the one dependency M10's measurement found. */
static void mig_resend_one(void *ctx, size_t off)
{
	const struct sockaddr_in *to = ctx;

	seal_send(to, C.migb.b + off + 2, pc_g16(C.migb.b + off));
	C.px.migrate_retx++;
}

static void mig_retransmit(const struct sockaddr_in *to)
{
	clmig_retransmit(&C.mig, now_ms(), mig_resend_one, (void *)to);
}

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
	if (!C.migb.b) {
		if (clmig_buf_init(&C.migb, MIG_BATCH_CAP) != 0)
			return 0;
	}
	if (!C.bulkb) {
		C.bulkb = malloc(MIG_BATCH_CAP);
		if (!C.bulkb)
			return 0;
	}
	clmig_buf_reset(&C.migb);
	C.bulkb_len = 0;
	C.bulk_recs = 0;
	clmig_reset(&C.mig);
	for (i = 0; i < pc_store_count() && m.budget > 0; i++) {
		if (!pc_store_live(i))
			continue;   /* S69: a dropped collection */
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
	moved = C.migb.len > 0 || C.bulkb_len > 0;

	/* the pump: send the collected payloads through an ack-gated window
	 * so the receiver's UDP buffer is never overrun (victims are
	 * already stubbed - anything undeliverable within the deadline is a
	 * lost transfer = a miss by the invariant, counted honestly) */
	if (C.migb.len) {
		long long deadline = now_ms() + 5000;
		size_t off = 0;
		unsigned int sent = 0, lost = 0, n = 0;
		const unsigned char *gp = NULL;
		struct pollfd pf = { .fd = C.fd, .events = POLLIN };

		while (clmig_group_walk(&C.migb, &off, &gp, &n)) {
			while (clmig_inflight(&C.mig) >= MIG_WINDOW &&
			        now_ms() < deadline) {
				if (poll(&pf, 1, 20) > 0)
					drain_dgrams();
				beat_pump();   /* the ack window may poll for as
				                * long as PEER_UP_MS - never mute */
				mig_retransmit(&m.to->addr);   /* S85 */
			}
			if (now_ms() >= deadline)
				break;
			seal_send(&m.to->addr, gp, n);
			/* remember req -> record-count while it is in flight,
			 * so a lost gathered datagram is charged in RECORDS -
			 * and where its bytes are, so a late ack costs one
			 * re-send, not a cycle (S85) */
			/* the group's own offset, which the retransmit reads
			 * back from: the walk has already advanced `off` past
			 * it, but gp points just past its len2 header */
			clmig_track(&C.mig, clmig_group_req(gp), clmig_group_count(gp),
				(size_t)(gp - C.migb.b) - 2, now_ms());
			C.px.migrate_dgrams++;
			sent++;
		}
		while (clmig_inflight(&C.mig) > 0 && now_ms() < deadline) {
			if (poll(&pf, 1, 20) > 0)
				drain_dgrams();
			beat_pump();
			mig_retransmit(&m.to->addr);   /* S85 */
		}
		/* unsent groups + unacked sends are the losses, in records -
		 * after MIG_RETX re-sends each, so a loss here is a peer that
		 * did not answer three times in five seconds */
		while (clmig_group_walk(&C.migb, &off, &gp, &n))
			lost += clmig_group_count(gp);
		lost += (unsigned int)clmig_lost_records(&C.mig);
		if (lost) {
			C.px.migrate_lost += lost;
			LM_WARN("rebalancer: %u record(s) unconfirmed across "
				"%u datagram(s) after re-sending each up to %d "
				"times (receiver slow or unreachable) - stubs "
				"stand, the keys read as misses\n",
				lost, sent, MIG_RETX);
		}
		clmig_buf_reset(&C.migb);
		clmig_reset(&C.mig);
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
		if (clbulk_offer(&C.bk, C.bulkb, C.bulkb_len, C.bulk_recs,
		        &m.to->addr)) {
			C.bulkb = NULL;        /* re-alloc next tick */
		} else {
			/* the bulk thread is still on the last batch: these
			 * stubs are lost - honest misses, counted */
			C.px.migrate_lost += C.bulk_recs;
			LM_WARN("rebalancer: bulk channel busy, %u oversized "
				"record(s) dropped to misses\n", C.bulk_recs);
		}
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
/* Am I the sender for @target's backfill of collection @col?  The
 * lowest-numbered live member other than @target does it, so a node
 * that came back empty is not sent the whole keyspace once per
 * surviving peer.
 *
 * S82: a peer that arrived empty within BACKFILL_SENDER_HOLDOFF_MS
 * does not count.  It is still being filled, and if its id is the
 * lowest it would take the role, send @target the little it holds,
 * walk clean and clear its own flag - while its own fill, landing a
 * moment later as passive copies, never goes on.  Observed on a
 * rolling restart eight seconds apart.  Skipping it means a node that
 * HOLDS the data self-designates; a peer that rejoined WITH data is
 * never flagged and is a sender at once.
 *
 * Each node judges from its own view, so two may send during the
 * holdoff - the target absorbs the duplicate through the version
 * compare - and none is impossible: the lowest node that is not itself
 * fresh always qualifies.  A keyspace whose fill outlasts the
 * holdoff is covered by the reported-count test above - an empty
 * candidate never qualifies, at any age.  What remains uncovered is a
 * candidate that holds SOME of the keyspace and is still filling: it
 * qualifies and sends a partial set.  That needs the joining node to
 * pull its own bootstrap and know when it is done (S83), which is
 * filed, not built. */
static int lowest_live_sender(int target, int col)
{
	long long now = now_ms();
	int i;

	for (i = 0; i < C.pt.n_peers; i++) {
		struct peer *p = &C.pt.peers[i];
		int id = __atomic_load_n(&p->node, __ATOMIC_ACQUIRE);

		if (!id || id == target)
			continue;
		if (!clpeers_live(p, now))
			continue;
		if (p->nstate == PC_NST_RECOVERING)
			continue;              /* S83: still pulling its own */
		/* S82: a candidate that HOLDS NOTHING cannot send anything,
		 * however long ago it restarted.  The holdoff below is a
		 * timer and this is the state it was standing in for: a node
		 * whose own fill outlasts the holdoff used to become
		 * eligible while still empty, take the role as the lowest
		 * id, hand over the little it had and clear its flag - which
		 * is exactly the failure the holdoff exists to prevent, one
		 * timeout later.  The count is what the peer itself reports
		 * in its ALIVE, so this needs no new state and no wire
		 * change; zero is unambiguous whatever the collection. */
		if (!p->entries || p->start_kind == PC_START_COLD)
			continue;
		if (p->backfill[col] &&
		        now - p->fresh_ms < BACKFILL_SENDER_HOLDOFF_MS)
			continue;              /* arrived empty just now:
			                        * holds nothing worth sending */
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
		if (!pc_store_live(c))
			continue;   /* S69: a dropped collection */
		if (!pc_store_eager_enabled(pc_store_ht(c)))
			continue;
		for (i = 0; i < C.pt.n_peers; i++) {
			struct peer *pr = &C.pt.peers[i];
			unsigned int start, since;
			int clean = 0, bf;

			if (!clpeers_live(pr, now))
				continue;
			/* S83: a peer pulling its own bootstrap gets no sweep at
			 * all - not the backfill walk, not the ordinary cycle:
			 * the pull carries everything it holds a copy of, and
			 * writes made meanwhile go by the write-path push.  Its
			 * flag stays armed for the fallback (below). */
			if (pr->nstate == PC_NST_RECOVERING)
				continue;
			/* ONE designated sender for a backfill.  Every live
			 * peer holds the same copies, so if they all pushed
			 * at once the empty node would receive the keyspace
			 * N times over.  The lowest live node id does it -
			 * deterministic, needs no agreement, and does not
			 * care which node is master. */
			/* S102: the flag is only ever CLEARED by the node that
			 * walked the backfill; on every other node it goes stale
			 * (see BACKFILL_SENDER_HOLDOFF_MS).  The moment the lowest
			 * live id changes - the sender died, or in the fleet's case
			 * simply restarted - a stale holder becomes the designated
			 * sender and pushes the WHOLE keyspace at a node that is
			 * full, then logs "backfilled node N after its restart"
			 * for a restart long past.  Seen live (.245, "node 846"),
			 * and it is what failed eagertest's quiet window on
			 * alternate tags: the same first slice from bucket 0, 148
			 * records, every time.
			 *
			 * So a walk may START only while the target still says it
			 * is cold (S81's start byte; an older build, its count).
			 * A walk in flight runs to completion - the target turning
			 * ESTABLISHED mid-way must not cut its sender off.  A flag
			 * whose target is established, on a node that is not
			 * walking, is stale by definition: dropped here, and said,
			 * rather than left to fire when the ids reshuffle.
			 *
			 * NOT covered: a sender that dies after the target's cold
			 * window has closed leaves it with a partial set and nobody
			 * armed.  That is S83's - the joiner pulling what it is
			 * owed - and is filed there. */
			if (pr->backfill[c] && !pr->repl_bfcycle[c] &&
			    !(pr->start_kind == PC_START_COLD ||
			      (pr->start_kind == PC_START_UNKNOWN && !pr->entries))) {
				pr->backfill[c] = 0;
				LM_NOTICE("cluster: node %d is established - a backfill "
					"armed for it here was never this node's to send, "
					"dropped as stale (%s)\n", pr->node,
					pc_store_name(c));
			}
			/* S83: a target that is pulling its own bootstrap gets no
			 * walk - the flag stays armed and drops as stale once it
			 * reports established, or walks if its pull failed and it
			 * came up ready but cold (the push is the fallback) */
			bf = pr->backfill[c] && pr->nstate != PC_NST_RECOVERING &&
				lowest_live_sender(pr->node, c);
			/* S81: a backfill is a WHOLE cycle walked as the
			 * sender.  The role moves when the lowest node dies:
			 * gaining it part-way through an ordinary cycle
			 * means the head of the table went by without the
			 * copies - start over; losing it part-way means the
			 * tail will - this cycle can no longer clear the
			 * flag, whoever took the role walks its own. */
			if (bf && pr->repl_cursor[c] && !pr->repl_bfcycle[c])
				pr->repl_cursor[c] = 0;
			if (!bf)
				pr->repl_bfcycle[c] = 0;
			/* one budgeted SLICE of a resumable cycle.  The
			 * mark may only advance when a whole cycle has been
			 * walked with nothing lost - so the cycle's start
			 * tick is captured when the cursor is at 0, not on
			 * every tick, or records written mid-cycle would be
			 * skipped by the next pass. */
			if (pr->repl_cursor[c] == 0) {
				pr->repl_cycle[c] = get_ticks();
				pr->repl_dirty[c] = 0;
				pr->repl_bfcycle[c] = (unsigned char)bf;
			}
			start = pr->repl_cycle[c];
			/* S81: the mark says how far this node's AUTHORED
			 * records have been pushed.  It says nothing about
			 * copies, whose write-ticks were stamped on receipt
			 * and sit anywhere below it.  A backfill walks from
			 * zero.  Walking it from the mark is how a rolling
			 * restart left a node without the fleet's records:
			 * an ordinary cycle ran while another node held the
			 * sender role and advanced the mark past every copy;
			 * the role then moved here, the backfill discarded
			 * them all on wtick <= since, walked "clean", and
			 * logged itself complete. */
			since = bf ? 0 : pr->repl_mark[c];
			run_migration(pr, 4 << 20, 2, since,
				pc_store_name(c), &clean, &pr->repl_cursor[c],
				bf);
			if (!clean)
				pr->repl_dirty[c] = 1;
			if (pr->repl_cursor[c] == 0 && !pr->repl_dirty[c]) {
				pr->repl_mark[c] = start ? start - 1 : 0;
				/* a whole cycle walked cleanly AS A BACKFILL:
				 * the copies are delivered, stop resending */
				if (pr->repl_bfcycle[c]) {
					pr->backfill[c] = 0;
					pr->repl_bfcycle[c] = 0;
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
		if (!pc_store_live(i))
			continue;   /* S69: a dropped collection */
		if (pc_store_shard_enabled(pc_store_ht(i))) {
			have = 1;
			break;
		}
	}
	if (!have)
		return;
	for (i = 0; i < C.pt.n_peers; i++) {
		if (!clpeers_live(&C.pt.peers[i], now))
			continue;
		if (run_migration(&C.pt.peers[i], 4 << 20, 1, 0, NULL, NULL, NULL, 0))
			shard_note_change();
	}
}

void pc_rebalance_tick(void)
{
	unsigned int self_per_mille = live_per_mille(self_live_kb(),
		self_total_mb());
	unsigned int sum = self_per_mille, mean, best_util = 1000, excess;
	struct peer *best = NULL;
	int i, n = 1;

	if (!C.enabled)
		return;
	shard_reshard_tick();              /* ownership first: deterministic
	                                    * moves are not load leveling */
	eager_repl_tick();                 /* then the eager synchronizer */
	for (i = 0; i < C.pt.n_peers; i++) {
		unsigned int u;

		if (!clpeers_live(&C.pt.peers[i], now_ms()) || !C.pt.peers[i].total_mb)
			continue;
		u = live_per_mille(C.pt.peers[i].live_kb, C.pt.peers[i].total_mb);
		sum += u;
		n++;
		/* equal utilization ties break toward more ABSOLUTE free -
		 * else a tiny empty node wins over a huge empty one and the
		 * data double-hops through it (measured: B->A->C cascade) */
		if (u < best_util || (u == best_util && best &&
		        C.pt.peers[i].free_mb > best->free_mb)) {
			best_util = u;
			best = &C.pt.peers[i];
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
	if (self_per_mille <= mean + 50 || best_util >= self_per_mille)
		return;                        /* band: 50 per-mille = 5% */
	{
		long long budget = 4 << 20;    /* 4MB per 10s tick */

		excess = (self_per_mille - mean) * self_total_mb() / 1000;
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

/* ---- S83: the bootstrap pull over the bulk plane ------------------------
 * Joiner: [count u32 = BOOT_MAGIC][kind u8][node u16][0 u8], then reads
 * M_MIGRATE_MANY-format records until a header of all zeros, applies
 * each PASSIVE (a copy never re-propagates) and off the WAL (the author
 * persists it), and answers [stored u32].  Peer: walks every eager
 * collection, streaming as the buffer fills.  The stream is written
 * from inside the walk callback; in the bucket phase no lock is held,
 * in the overflow leg the overflow lock is - a flush there stalls
 * writers that need it for as long as the socket takes the buffer,
 * microseconds against a receiver that applies at memory speed, and
 * bounded by the socket's 5 s send timeout. */
struct boot_ctx {
	int fd;
	struct pc_cipherstate *cs;
	unsigned char *buf;
	size_t len, cap;
	const char *col;
	unsigned int cn, now, streamed;
	int err;
};

static int boot_flush(struct boot_ctx *bc)
{
	if (bc->len && clbulk_send(bc->fd, bc->cs, bc->buf, bc->len) != 0) {
		bc->err = 1;
		return -1;
	}
	bc->len = 0;
	return 0;
}

static int boot_cb(const str *key, const str *val, unsigned int exp,
		unsigned int wtick, unsigned char rflags, unsigned long long ver,
		void *arg)
{
	struct boot_ctx *bc = arg;
	unsigned int ttl_left = 0;
	size_t n;
	unsigned char *rp;

	(void)wtick;
	(void)rflags;
	if (bc->err)
		return -1;
	if (exp) {
		if (exp <= bc->now)
			return 0;                  /* expired: nothing to hand on */
		ttl_left = exp - bc->now;
	}
	if (key->len > 4096)
		return 0;
	n = MIG_RHDR + bc->cn + (size_t)key->len + (size_t)val->len;
	if (n > bc->cap)
		return 0;                      /* cannot happen: cap > max record */
	if (bc->len + n > bc->cap && boot_flush(bc) != 0)
		return -1;
	rp = bc->buf + bc->len;
	{
		struct clmig_rec br = {
			.ttl_left = ttl_left,
			.col = bc->col, .collen = bc->cn,
			.key = key->s, .klen = (unsigned int)key->len,
			.val = val->s, .vlen = (unsigned int)val->len,
			.ver = ver
		};

		clmig_rec_write(rp, &br);
	}
	bc->len += n;
	bc->streamed++;
	return 0;
}

/* the peer's side: stream every eager collection, end marker, read the
 * joiner's count.  The handshake is done; @cs_s/@cs_r are ours. */
static void bulk_serve_boot(int cfd, struct pc_cipherstate *cs_s,
		struct pc_cipherstate *cs_r, unsigned char *stash, size_t *slen,
		size_t scap)
{
	unsigned char req[4], end[MIG_RHDR], cnt4[4];
	struct boot_ctx bc;
	int c, node;

	if (clbulk_recv_exact(cfd, cs_r, stash, slen, scap, req, 4) != 0)
		return;
	if (req[0] != BOOT_KIND_PULL)
		return;
	{
		int bkind;

		if (!clbulk_bootreq_parse(req, CLBULK_BOOTREQ_LEN, &bkind, &node))
			return;
	}
	memset(&bc, 0, sizeof bc);
	bc.fd = cfd;
	bc.cs = cs_s;
	bc.cap = BOOT_BUF;
	bc.buf = malloc(bc.cap);
	if (!bc.buf)
		return;
	bc.now = get_ticks();
	for (c = 0; c < pc_store_count() && !bc.err; c++) {
		if (!pc_store_live(c))
			continue;   /* S69: a dropped collection */
		pcache_htable_t *ht = pc_store_ht(c);

		if (!pc_store_eager_enabled(ht))
			continue;
		bc.col = pc_store_name(c);
		bc.cn = (unsigned int)strlen(bc.col);
		if (bc.cn > 255)
			continue;
		pcache_ht_iter_meta(ht, boot_cb, &bc);
	}
	if (bc.err || boot_flush(&bc) != 0)
		goto out;
	memset(end, 0, sizeof end);
	if (clbulk_send(cfd, cs_s, end, sizeof end) != 0)
		goto out;
	__atomic_fetch_add(&C.px.boot_out, bc.streamed, __ATOMIC_RELAXED);
	if (clbulk_recv_exact(cfd, cs_r, stash, slen, scap, cnt4, 4) == 0)
		LM_INFO("cluster: streamed a bootstrap of %u record(s) to node %d, "
			"which stored %u\n", bc.streamed, node, g32(cnt4));
	else
		LM_INFO("cluster: streamed a bootstrap of %u record(s) to node %d "
			"(no count back)\n", bc.streamed, node);
out:
	free(bc.buf);
}

/* the joiner's side, one candidate.  0 = the stream ended cleanly;
 * -2 = nothing came back before the first record (an older build closed
 * on the magic count, or a refusal); -1 = the stream broke. */
static int bulk_pull_from(const struct sockaddr_in *peer,
		unsigned int *streamed, unsigned int *stored, unsigned int *older)
{
	unsigned char hdr[3 + PC_NOISE_MAXMSG], m2[PC_NOISE_MAXMSG];
	unsigned char stash[PC_NOISE_MAXMSG], cnt4[4], req[4], rhdr[MIG_RHDR];
	unsigned char *rec = NULL, ver1 = PC_CLIENT_VER;
	struct sockaddr_in to = *peer;
	struct pc_handshake hs;
	struct pc_cipherstate cs_s, cs_r;
	struct timeval tv = { 5, 0 };
	uint8_t prologue[1] = { PC_PRIN_CLUSTER };
	size_t mlen = 0, plen = 0, cl, slen = 0;
	int fd, rc = -2;

	*streamed = *stored = *older = 0;
	to.sin_port = C.mcast_dst.sin_port;    /* the cluster port, TCP */
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	if (connect(fd, (struct sockaddr *)&to, sizeof to) != 0)
		goto out;
	pc_hs_init_initiator(&hs, prologue, 1);
	if (pc_hs_write_msg1(&hs, C.psk, &ver1, 1, hdr + 3, &mlen) != 0)
		goto out;
	hdr[0] = (unsigned char)(1 + mlen);
	hdr[1] = (unsigned char)((1 + mlen) >> 8);
	hdr[2] = PC_PRIN_CLUSTER;
	if (clbulk_io(fd, 1, hdr, 3 + mlen) != 0 || clbulk_io(fd, 0, hdr, 2) != 0)
		goto out;
	cl = (size_t)hdr[0] | ((size_t)hdr[1] << 8);
	if (cl == 0 || cl > sizeof m2 || clbulk_io(fd, 0, m2, cl) != 0 ||
	        pc_hs_read_msg2(&hs, m2, cl, NULL, &plen, &cs_s, &cs_r) != 0)
		goto out;
	p32(cnt4, BOOT_MAGIC);
	clbulk_bootreq_write(req, sizeof req, BOOT_KIND_PULL, C.node_id);
	if (clbulk_send(fd, &cs_s, cnt4, 4) != 0 ||
	        clbulk_send(fd, &cs_s, req, 4) != 0)
		goto out;
	rec = malloc(11u + 255 + 4096 + PCACHE_CELL_MAX);
	if (!rec) {
		rc = -1;
		goto out;
	}
	for (;;) {
		unsigned int ttl_left, cn, kn, vl, exp;
		unsigned long long ver;
		pcache_htable_t *ht;
		char colz[256];
		str k, v;
		int brc;

		if (clbulk_recv_exact(fd, &cs_r, stash, &slen, sizeof stash,
		        rhdr, MIG_RHDR) != 0)
			goto out;                  /* -2 before any record, -1 after */
		rc = -1;
		{
			struct clmig_rec hr;

			clmig_rec_hdr_parse(rhdr, &hr);
			ttl_left = hr.ttl_left;
			cn = hr.collen;
			kn = hr.klen;
			vl = hr.vlen;
			ver = hr.ver;
		}
		if (!cn && !kn && !vl)
			break;                     /* the end marker */
		if (cn > 255 || kn > 4096 || vl > PCACHE_CELL_MAX)
			goto out;
		if (clbulk_recv_exact(fd, &cs_r, stash, &slen, sizeof stash,
		        rec, (size_t)cn + kn + vl) != 0)
			goto out;
		(*streamed)++;
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
		brc = pcache_ht_store_ver(ht, &k, &v, exp, PCACHE_F_PASSIVE, ver);
		if (brc == PCACHE_E_OLDER) {
			(*older)++;
			__atomic_fetch_add(&C.px.recv_older, 1, __ATOMIC_RELAXED);
		} else if (brc == 0) {
			pc_loc_clear(colz, cn, k.s, kn);
			(*stored)++;
		}
	}
	p32(cnt4, *stored);
	clbulk_send(fd, &cs_s, cnt4, 4);   /* so the peer can say what landed */
	rc = 0;
out:
	free(rec);
	if (fd >= 0)
		close(fd);
	return rc;
}

/* the joiner's side: the candidates in order, first clean stream wins */
static void bulk_pull(void)
{
	struct sockaddr_in cand[BOOT_MAX_CAND];
	int node[BOOT_MAX_CAND], n, k;

	n = clboot_take_cands(&C.bt, clbulk_lock(&C.bk), cand, node);
	for (k = 0; k < n; k++) {
		unsigned int streamed, stored, older;
		long long t0 = now_ms();
		int rc = bulk_pull_from(&cand[k], &streamed, &stored, &older);

		if (rc == 0) {
			__atomic_fetch_add(&C.px.boot_in, stored, __ATOMIC_RELAXED);
			clboot_pull_ok(&C.bt, clbulk_lock(&C.bk));
			LM_NOTICE("cluster: bootstrapped from node %d - %u record(s) "
				"streamed, %u stored, %u already held at the same or a "
				"newer version, in %lld ms; ready\n", node[k], streamed,
				stored, older, now_ms() - t0);
			if (pc_node_state() == PC_NST_RECOVERING)
				pc_node_state_set(PC_NST_READY);
			return;
		}
		__atomic_fetch_add(&C.px.boot_failed, 1, __ATOMIC_RELAXED);
		LM_WARN("cluster: the bootstrap pull from node %d failed after %u "
			"record(s): %s%s\n", node[k], streamed,
			rc == -2 ? "nothing came back - a build before the pull, "
			"or unreachable" : "the stream broke",
			k + 1 < n ? " - trying the next candidate" : "");
	}
	/* every candidate of this round failed: hand the decision back to
	 * the tick, which re-picks with the peers known by now, or gives
	 * up after three rounds */
	clboot_pull_failed(&C.bt, clbulk_lock(&C.bk));
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
	if (!clbulk_take(&C.bk, &batch, &blen, &recs, &to))
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
	if (clbulk_io(fd, 1, hdr, 3 + mlen) != 0 ||
	        clbulk_io(fd, 0, hdr, 2) != 0)
		goto out;
	cl = (size_t)hdr[0] | ((size_t)hdr[1] << 8);
	if (cl == 0 || cl > sizeof m2 || clbulk_io(fd, 0, m2, cl) != 0 ||
	        pc_hs_read_msg2(&hs, m2, cl, NULL, &plen, &cs_s, &cs_r) != 0)
		goto out;

	p32(cnt4, recs);
	if (clbulk_send(fd, &cs_s, cnt4, 4) != 0 ||
	        clbulk_send(fd, &cs_s, batch, blen) != 0)
		goto out;
	if (clbulk_recv_exact(fd, &cs_r, stash, &slen, sizeof stash, cnt4, 4)
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
	clbulk_done(&C.bk);
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
	if (clbulk_io(cfd, 0, frame, 2) != 0)
		goto out;
	flen = (size_t)frame[0] | ((size_t)frame[1] << 8);
	if (flen < 1 + 48 || flen > PC_NOISE_MAXMSG ||
	        clbulk_io(cfd, 0, frame + 2, flen) != 0)
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
	if (clbulk_io(cfd, 1, m2, 2 + mlen) != 0)
		goto out;
	/* pc_hs_write_msg2 hands the responder ITS oriented pair already */

	if (clbulk_recv_exact(cfd, &cs_r, stash, &slen, sizeof stash, cnt4, 4)
	        != 0)
		goto out;
	nrec = g32(cnt4);
	if (nrec == BOOT_MAGIC) {              /* S83: a pull, not a batch */
		bulk_serve_boot(cfd, &cs_s, &cs_r, stash, &slen, sizeof stash);
		goto out;
	}
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

		if (clbulk_recv_exact(cfd, &cs_r, stash, &slen, sizeof stash,
		        rhdr, MIG_RHDR) != 0)
			goto out;
		{
			struct clmig_rec hr;

			clmig_rec_hdr_parse(rhdr, &hr);
			ttl_left = hr.ttl_left;
			cn = hr.collen;
			kn = hr.klen;
			vl = hr.vlen;
			ver = hr.ver;
		}
		if (cn > 255 || kn > 4096 || vl > PCACHE_CELL_MAX)
			goto out;
		if (clbulk_recv_exact(cfd, &cs_r, stash, &slen, sizeof stash,
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
	clbulk_send(cfd, &cs_s, cnt4, 4);
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
		int have_tx, have_boot;

		/* both under ONE acquisition, as before: splitting them is the
		 * defect M4 fixed in boot_tick */
		pthread_mutex_lock(clbulk_lock(&C.bk));
		have_tx = C.bk.tx != NULL;
		have_boot = C.bt.pending;
		pthread_mutex_unlock(clbulk_lock(&C.bk));
		if (have_boot)
			bulk_pull();               /* S83 */
		if (have_tx)
			bulk_tx(stop);
		if (poll(&pf, 1, 200) > 0) {
			int cfd = accept(C.blfd, NULL, NULL);

			if (cfd >= 0)
				bulk_rx(cfd);
		}
	}
}

/* ---- the cluster plane's internal API (clstate.h) -----------------------
 * These are the ONLY way an extracted translation unit reaches the
 * state: handles, readers, and a function per counter so that no
 * mutable pointer into C escapes.  Each counter reproduces the ordering
 * its call site used - three atomic, five plain - because rule 6 moves
 * code without rewording it. */

struct clpeers *cl_peers(void) { return &C.pt; }
struct clpend  *cl_pend(void)  { return &C.pd; }
struct clpush  *cl_push(void)  { return &C.wp; }

int cl_enabled(void)    { return C.enabled; }
int cl_node_id(void)    { return C.node_id; }
int cl_map_usable(void) { return C.map_usable; }
const struct pc_clmap *cl_map(void) { return &C.map; }
struct sockaddr_in cl_self_addr(void) { return C.self_addr; }

void cl_place_map_hit(void) { C.place_map_n++; }
void cl_place_hrw_hit(void) { C.place_hrw_n++; }

void cl_st_fwd_no_route(void)
{
	__atomic_fetch_add(&C.st.fwd_no_route, 1, __ATOMIC_RELAXED);
}

void cl_st_fwd_send_fail(void)
{
	__atomic_fetch_add(&C.st.fwd_send_fail, 1, __ATOMIC_RELAXED);
}

void cl_st_pull_sent(unsigned long long n)
{
	__atomic_fetch_add(&C.st.pull_sent, n, __ATOMIC_RELAXED);
}

void cl_st_fwd_send_errno(int e) { C.st.fwd_send_errno = e; }

void cl_px_fwd_sent(void)            { C.px.fwd_sent++; }
void cl_px_fwd_fails(void)           { C.px.fwd_fails++; }
void cl_px_migrate_skipped_big(void) { C.px.migrate_skipped_big++; }
void cl_px_placed_local(void)        { C.px.placed_local++; }
void cl_px_placed_remote(void)       { C.px.placed_remote++; }

/* clwork needs the clock and the free-space read; both stay defined
 * here (now_ms has 82 call sites in this file) and are reached through
 * a wrapper rather than renamed at every site. */
long long cl_now_ms(void) { return now_ms(); }
unsigned int cl_self_free_mb(void) { return self_free_mb(); }

/* ---- init / stats ------------------------------------------------------- */

int pc_cluster_init(const char *mcast_addr, int mcast_port,
		const char *advertise, const uint8_t psk[PC_NOISE_KEYLEN],
		int pull_timeout_ms, int negative_ms, int tombstone_ms,
		const char *state_dir, const char *legacy_dir, int max_pending)
{
	/* the WAL takes this node out of service if it starts discarding
	 * acknowledged writes - see cluster_wal_shed() */
	pc_wal_on_shed(cluster_wal_shed);

	struct sockaddr_in sa;
	struct in_addr self_ip;
	struct ip_mreqn mreq;
	int one = 1;

	memset(&C.st, 0, sizeof C.st);
	/* Size the parked-request table ONCE.  Clamped, not rejected: the
	 * ceiling is structural (the slot index rides in the request id)
	 * and a config that asks for more should still start, saying so.
	 * Allocated once and never resized - clpend_find() hands out raw
	 * pointers into this. */
	clpush_init(&C.wp, PC_CL_MAXPEER, MIG_GHDR, MIG_GATHER_CAP,
		REPL_GROUP_FLUSH_BYTES, REPL_FLUSH_MS);
	if (!clpend_init(&C.pd, max_pending)) {
		LM_ERR("cluster: cannot allocate %d parked-request slots\n",
			max_pending);
		return -1;
	}
	if (max_pending > 0 && C.pd.cap != max_pending)
		LM_WARN("cluster: max_pending %d clamped to %d - the slot index "
			"rides in the request id\n", max_pending, C.pd.cap);
	/* S80: the state directory must exist and be writable before
	 * anything is persisted into it.  A missing directory whose parent
	 * exists is created (0700: it will hold the node's name); anything
	 * else refuses startup, because a node that silently ran ephemeral
	 * would present as new after every restart while the admin
	 * believed otherwise.  tmpfs is accepted and said loudly: it
	 * survives a daemon restart, not a host restart. */
	C.state_dir[0] = 0;
	C.state_tmpfs = 0;
	if (state_dir && *state_dir) {
		struct statfs sf;
		struct stat st;

		if (stat(state_dir, &st) != 0) {
			if (mkdir(state_dir, 0700) != 0) {
				LM_CRIT("cluster: state_dir %s is unusable: %s "
					"- refusing to start\n", state_dir,
					strerror(errno));
				return -1;
			}
		} else if (!S_ISDIR(st.st_mode)) {
			LM_CRIT("cluster: state_dir %s is not a directory - "
				"refusing to start\n", state_dir);
			return -1;
		}
		if (access(state_dir, W_OK) != 0) {
			LM_CRIT("cluster: state_dir %s is not writable: %s - "
				"refusing to start\n", state_dir, strerror(errno));
			return -1;
		}
		snprintf(C.state_dir, sizeof C.state_dir, "%s", state_dir);
		if (statfs(state_dir, &sf) == 0 &&
		        ((unsigned int)sf.f_type == PC_TMPFS_MAGIC ||
		         (unsigned int)sf.f_type == PC_RAMFS_MAGIC)) {
			C.state_tmpfs = 1;
			LM_WARN("cluster: state_dir %s is on tmpfs - identity "
				"and mastership term survive a daemon restart "
				"but NOT a host restart; after a reboot this "
				"node presents as new\n", state_dir);
		}
	}
	if (identity_init(state_dir, legacy_dir) != 0)
		return -1;
	/* The mastership term lives beside the identity and in the same
	 * durability class, because it has to outlive any map somebody
	 * could still believe.  A node that cannot read a term it wrote
	 * refuses to start: silently forgetting one is how a term gets
	 * reissued with different content behind it. */
	/* S80: the term already persisted in the WAL directory is carried
	 * across once when state_dir is introduced beside it - a reset
	 * term is an election hazard, not a cosmetic one */
	if (legacy_dir && state_dir && *state_dir)
		carry_file(legacy_dir, state_dir, PC_TERM_FILE);
	if (pc_term_init(state_dir) != 0) {
		LM_CRIT("cluster: %s/" PC_TERM_FILE " does not validate - "
			"refusing to start rather than risk reissuing a term "
			"someone has already seen.  Restore the file, or delete "
			"it to start the term from 0\n",
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
		/* S125: ask the kernel to report what this socket drops.  Per
		 * socket, so a second daemon on the same host cannot pollute
		 * it the way the host-wide UDP RcvbufErrors would. */
		{
			int on = 1;

			setsockopt(C.fd, SOL_SOCKET, SO_RXQ_OVFL, &on, sizeof on);
		}
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
	setsockopt(C.mfd, SOL_SOCKET, SO_RXQ_OVFL, &one, sizeof one);   /* S125 */
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
	clbulk_init(&C.bk);
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
	C.join_held = 0;
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

	rx_sample();                   /* S125: sampled by the reader */
	*out = C.st;
	/* the two receive TOTALS and the two queue gauges are read live
	 * rather than copied from the sample, so they can never disagree
	 * with migrated_in in the same reply.  Only the three per-second
	 * figures need the previous sample to exist. */
	out->rx_applied = __atomic_load_n(&C.px.migrated_in, __ATOMIC_RELAXED);
	out->rx_drops = __atomic_load_n(&C.rxq_drops, __ATOMIC_RELAXED);
	out->rx_queue = rx_queue_bytes(C.fd) + rx_queue_bytes(C.mfd);
	out->rx_rcvbuf = (unsigned int)(C.rcvbuf > 0 ? C.rcvbuf : 0);
	out->pend_max = C.pd.cap;      /* the capacity peak is measured against */
	out->pend_used = clpend_used(&C.pd);
	out->pend_peak = clpend_peak(&C.pd);
	out->pend_exhausted = clpend_exhausted(&C.pd);
	out->neg_hits = clloc_neg_hits(&C.lc);
	out->node_id = pc_my_node_id();
	out->peers_up = 0;
	out->reserved_ids = clres_count(&reserved, now_ms());   /* S90 */
	out->peers_up = clpeers_count_live(&C.pt, now_ms());
}

int pc_cluster_peers(struct pc_cl_peer_info *out, int max)
{
	int i, n = 0;

	for (i = 0; i < C.pt.n_peers && n < max; i++) {
		out[n].node = C.pt.peers[i].node;
		out[n].up = now_ms() - C.pt.peers[i].last_seen_ms < PEER_UP_MS;
		out[n].last_seen_ms = C.pt.peers[i].last_seen_ms;
		out[n].free_mb = C.pt.peers[i].free_mb;
		n++;
	}
	return n;
}
