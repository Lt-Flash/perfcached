/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* cluster.c - the peer plane: automatic membership, placement, replication.  Every datagram is
 * sealed with the cluster PSK (no session-key layer); multicast carries control only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <pthread.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/vfs.h>
/* statfs.f_type is int on 32-bit targets: compare the low 32 bits as unsigned, against our own
 * U-suffixed constants, so the tmpfs test is no signed/unsigned mismatch */
#define PC_TMPFS_MAGIC 0x01021994u
#define PC_RAMFS_MAGIC 0x858458f6u
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sodium.h>

#include "iostat.h"
#include "compat/dprint.h"
#include "quiesce.h"
#include "psindex.h"                 /* the fleet pattern index */
#include "pubsub.h"
#include "compat/pt.h"
#include "compat/timer.h"
#include "core/pcache_htable.h"
#include "core/pcache_arena.h"
#include "config.h"                    /* PC_MAX_COLLECTIONS */
#include "store.h"
#include "winctr.h"
#include "upstream.h"
#include "version.h"
#include "upgrade.h"
#include <sodium.h>
#include "cluster.h"
#include "alerts.h"                     /* the link in the ALIVE */
#include "clrpt.h"
#include "clcodec.h"                 /* the LE wire codec */
#include "fnv1a.h"                   /* the one FNV-1a */
#include "clmig.h"                   /* the migration in-flight window */
#include "clfwd.h"                   /* the proxy write plane frames */
#include "clinit.h"                  /* the node identity file format */
#include "pc_slot.h"                  /* pc_key_slot(): placement input */
#include "pc_mix.h"                   /* pc_hrw_mix(): the rendezvous weight */
#include "daemon.h"                  /* pc_worker_id() */
#include "ptree.h"                   /* the membership push is a tree */
#include "compat/ipc.h"               /* ipc_send_rpc_all(): the flush RPC */
#include "clmap.h"
#include "clterm.h"
#include "clplace.h"
#include "recover.h"
#include "metrics.h"                 /* pc_metrics_uptime() for the ALIVE */
#include "clhist.h"
#include "clsync.h"
#include "clpend.h"
#include "clloc.h"
#include "clretain.h"
#include "clorder.h"
#include "clrp.h"
#include "clres.h"                   /* the id reservations */
#include "wal.h"                     /* PC_WAL_SHED_*, pc_wal_on_shed() */
#include "rdb.h"                     /* pc_rdb_safe_marker() */
#include "ackstate.h"                /* the watermarks */
#include "clapply.h"                 /* apply sharded by key hash */
#include "clplat.h"                  /* platform-derived identity */
#include "clcid.h"                   /* the cluster id */
#include "statefile.h"               /* the state dir's small files */
#include "clmesh.h"                  /* discovery = unicast */
#include "cladmit.h"
#include "clddl.h"                   /* the collection DDL */
#include "clfound.h"
#include "clane.h"                   /* the frame lane */
#include "clprobe.h"                 /* the lane probes' table */
#include "clmemb.h"                  /* the keepalive frame layouts */
#include "proto.h"                      /* the door gauges */
#include "clpull.h"                  /* the PULL plane frame layouts */
#include "crc32c.h"                   /* a pull answer's value checksum */
#include "clhold.h"                   /* the hold-check frames */
#include "clrecon.h"                    /* the reconcile's pacing */
#include "clepoch.h"                     /* the wire epoch */
#include "respmap.h"
#include "clcol.h"                   /* the collection-announce frame */
#include "clunk.h"                   /* unknown commands, fleet-wide */
#include "obs.h"                     /* this node's table */
#include "clboot.h"
#include "clpush.h"
#include "clwire.h"
#include "clreplay.h"
#include "clbulk.h"
#include "pubsub.h"                     /* the relay */
#include "psrelay.h"                    /* the relay's sequence accounting */
#include "events.h"                     /* applied-here notifications */
#include "clstate.h"
#include "logrl.h"
#include "clpeers.h"
#include "core/pcache_mem.h"

/* the watchdog re-sends the last stored keepalive and clwire_beat_store() silently drops one that
 * does not fit, so an outgrown frame would re-broadcast a stale claim: caught at compile time */
_Static_assert(CLMEMB_ALIVE_BUF <= sizeof ((struct clwire *)0)->alive,
	"ALIVE outgrew clwire's beat buffer - clwire_beat_store would drop it");
_Static_assert(PC_CFGSIG_MAX == CLMEMB_CFG_MAX,
	"the settings the cluster keeps must be the ones the keepalive carries");
_Static_assert(CLMEMB_MALIVE_BUF <= sizeof ((struct clwire *)0)->malive,
	"MASTER_ALIVE outgrew clwire's beat buffer - it would be dropped");

#define SHARD_GRACE_S  30              /* after a membership change (extended while reshard
                                        * migrations flow), owner-misses broadcast-pull */

#include "clmsg.h"                  /* the datagram types */
#include "clsend.h"                  /* the send path */
#include "clps.h"                    /* the pub/sub relay plane */
#include "clspread.h"                /* spread's decisions */
#include "clrecv.h"                  /* replication receive/apply */
#include "cltomb.h"                  /* tombstones+acks */
#include "clsgmix.h"                 /* the sweep gate's digest term */

#define COL_RESYNC_BEATS 5   /* the created set re-announced this often */
#define JOIN_WAIT_MS    2000
#define JOIN_HOLD_MS    5000   /* one JOIN_WAIT extends the joiner's deadline this
                                * much, per retry: a master dying mid-hold lets it lapse */
#define MASTER_DEAD_MS  8000   /* 8 beats at 1 Hz: two stretched beats plus jitter
                                * must not promote; slower failover only delays the map */
#define CLMAP_PUB_MS    5000   /* map re-publication interval */
/* buckets per scan call; a call is never stopped part-way (a stopped call re-asks its window), so
 * a tick may overshoot its budget by one small window */
#define RECONCILE_BUCKETS  16
/* how long a staged change waits for the standby's ack.  One unicast
 * round trip on a LAN; generous, because abandoning burns a sequence. */
#define CLSYNC_ACK_MS   1000
/* a standby that let a staged change time out is passed over this long, or pick_backup() names the
 * same silent peer again; six re-publication intervals */
#define CLSYNC_PASS_OVER_MS  30000
/* a handover the named member has not taken by then is given up */
#define HANDOVER_GIVEUP_MS   20000
/* a peer that arrived EMPTY is not trusted as sender for three sweeps: a holdoff, because
 * backfill[] clears only where the backfill ran and would leave every node self-designating */
#define BACKFILL_SENDER_HOLDOFF_MS 30000
/* the bootstrap PULL: a joiner holding nothing asks ONE ready peer for a full bulk-TCP walk, ready
 * when it ends; the ask is a record count no batch can have, and an older peer just closes */
#define BOOT_MAGIC      0xFFFFFFFFu
#define BOOT_KIND_PULL  1
#define BOOT_BUF        (512u * 1024u)
/* how a node STARTED: an additive tail byte on the ALIVE, 0 from an older build */
#define PC_START_UNKNOWN     0
#define PC_START_COLD        1   /* replayed nothing, within its first holdoff */
#define PC_START_RECOVERED   2   /* replayed a WAL: owed a diff, not a push */
#define PC_START_ESTABLISHED 3   /* past its first holdoff */
/* a departed member's address/identity -> id binding is RESERVED, so it gets its id back and no
 * newcomer is handed it; every node records departures, so no sync (table: clres.h) */
/* a week: the window only keeps the id from newcomers, rejoining is never limited by it */
#define RESERVE_MS  604800000LL
static struct clres reserved;
static int self_start_kind(void);
static int any_live_peer_cold(void);
static int reconcile_witness_live(void);
static void handle_tomb_req(const unsigned char *pt, size_t n,
		const pc_addr *from);     /* the witness's half */
static void fp_replay_tick(void);
static void handle_rebuild(const unsigned char *pt, size_t n, const pc_addr *from);
static void spread_note_placeable(const struct pc_clmap *m);
static int addr_eq(const pc_addr *a, const pc_addr *b);
static void join_again(const pc_addr *to);
static void spread_reclaim_tick(void);
static void spread_missed_tick(void);
static int admitted_peer_live(long long now);
static void bulk_bind_self(int fd);
static int rec_sole_witness(void);
static void handle_upg_switch(const unsigned char *pt, size_t n,
		const pc_addr *from);
static void handle_upg_ack(const unsigned char *pt, size_t n,
		const pc_addr *from);
static void upg_msg_tick(void);
static int rec_witness_count(int *id);
static int rec_held(unsigned int wtick);
/* the digest-gated sweep's two messages (defined with the sweep) */
static void handle_sweep_dig(const unsigned char *pt, size_t n,
		const pc_addr *from);
static void handle_sweep_dig_r(const unsigned char *pt, size_t n);
/* the reconcile's digest round (defined with the sweep's) */
static int reconcile_digest_step(void);
static const unsigned char *reconcile_digest_filter(int c);
static int reconcile_digest_same(const unsigned char *diff, const str *key);
static void rdg_drop(void);
static void beat_pump(void);           /* called from the reconcile walk */
/* the eager sweep runs every this many beats, repair behind the write-path push; its "dying soon"
 * skip derives from this, never a fixed number */
#define REPL_SWEEP_BEATS 10
#define PEER_PURGE_MS   6000

#define MAX_FWD_VAL PC_MAX_FWD_VAL

#define MIG_BATCH_CAP  ((4 << 20) + (64 << 10))
#define MIG_WINDOW     3           /* 3 x ~56KB fits a default rmem */
/* a batch whose ack is late is re-sent up to MIG_RETX times, one per MIG_RETX_MS; the bytes stay
 * in migb until the slice ends, so a lost datagram costs one datagram, not a re-walk */
#define MIG_RETX       2
#define MIG_RETX_MS    1500
#define MIG_GATHER_CAP 56000       /* gathered group payload cap */
/* the write-path push's per-peer group - flushed when this full,
 * when this old at the next append, or by the cluster loop's timer */
#define REPL_GROUP_FLUSH_BYTES 48000
#define REPL_FLUSH_MS 3
#define MIG_GHDR       9           /* type+req4+node2+count2 */
#define MIG_RHDR      20           /* per record: ttl4+cn1+klen2+vlen4+ver8+flags1 */

static struct {
	int enabled;
	int fd;
	int node_id;
	unsigned char self_ident[16];  /* persisted; stable across restarts */
	char self_ident_hex[33];
	unsigned int self_incarn;      /* fresh every process start */
	/* the last delete this incarnation numbered - its
	 * numbering space is (identity hash, incarnation), seq from 1 */
	unsigned long long tomb_seq;
	long long ready_ms;            /* first READY heartbeat; 0 = not yet */
	int ident_durable;             /* 0 = nothing to persist to */
	char state_dir[512];           /* in use; empty = ephemeral */
	int state_tmpfs;               /* said loudly at startup */
	int proposed_id;               /* derived from the identity */
	uint8_t psk[PC_NOISE_KEYLEN];
	struct clwire wr;              /* sealed frames + the beat pair */
	struct clpeers pt;
	int pull_timeout_ms, negative_ms, tombstone_ms;
	struct clpend pd;              /* parked requests + the CQs */
	uint32_t next_req;
	struct clloc lc;               /* locator + negative cache */
	struct pc_cl_stats st;
	struct pc_proxy_stats px;
	/* the migration batch: payloads collected during the scan go through an ack-gated
	 * window - a raw burst overruns the peer's UDP receive buffer */
	struct clmig_buf migb;         /* the gather buffer */
	/* the in-flight window; losses count in RECORDS, since one gathered group carries many */
	struct clmig mig;
	/* automatic membership (peer thread writes, workers read node_id
	 * and peers[] - int/ptr-free fields, release/acquire on .node) */
	int mfd;                       /* multicast rx socket */
	int rcvbuf;                    /* EFFECTIVE unicast SO_RCVBUF (read
	                                * back: the kernel clamps silently) */
	/* the receive plane, sampled once a beat: the cluster thread owns it, workers
	 * read only the copy it publishes in C.st */
	struct {
		long long at_ms;               /* the previous sample */
		unsigned long long applied, older, drops;
	} rxs;
	/* SO_RXQ_OVFL's 32-bit drop count is stamped at queue time (lags by the queue
	 * depth) and wraps, hence the 64-bit total; one slot per socket: unicast, mcast */
	struct {
		unsigned int last;             /* last 32-bit value seen */
		int seen;                      /* a value has been seen at all */
	} ovfl[2];
	unsigned long long rxq_drops;      /* the 64-bit running total */
	/* the interchange-relevant config this node runs, and who we
	 * have already shouted about (once per peer, not once per second) */
	int cfg_mode, cfg_eager, cfg_authoritative;
	int cfg_replicas;              /* K under spread, else 0 */
	const char *cfg_site_salt;      /* [cluster] site_salt */
	long long pend_log_ms;         /* rate limit for the FULL shout */
	int cfg_client_port;           /* where CLIENTS reach this node, not the peer port */
	int told_self_state;           /* this node's state its clients were told */
	int cfg_http_port;             /* this node's HTTP door, 0 = none */
	int cfg_wal;                   /* 1 = this node logs a WAL */
	int cfg_resp_port;             /* this node's RESP door: CLUSTER replies advertise
	                                * it, since the native door is Noise off-box */
	struct in_addr cfg_shouted[PC_CL_MAXPEER];
	unsigned long long cfg_refused;
	pc_addr mcast_dst;  /* group:port */
	pc_addr self_addr;  /* advertised unicast addr */
	unsigned int last_change_tick; /* membership/reshard activity; 32-bit so worker
	                                * reads are tear-free on ILP32 (no cross-thread u64) */
	int role;                      /* PC_ROLE_* */
	/* the map this node believes: a master builds its own, everyone else adopts
	 * what arrives and only forwards (clmap_adopt()) */
	struct pc_clmap map;
	int map_valid;
	long long map_pub_ms;          /* last publication, master side */
	unsigned long long map_pub_n, map_rx_n, map_stale_n, map_bad_n;
	unsigned long long map_unadmitted_n;
	/* the map's placeable set matches what this node sees as live; cached, since
	 * placement is on the request path */
	int map_usable;
	unsigned int map_gen;          /* odd while map is rewritten (map_set) */
	/* the master's record of identities this cluster has seen: NEW vs RETURNING is
	 * no judgement a node can make about itself (a wiped state dir believes it is new) */
	struct pc_clhist hist;
	int backup_id;                 /* the master's chosen standby, 0 = none */
	struct pc_clsync sync;         /* master side: stage -> ack -> publish */
	struct pc_clmap staged;        /* built and staged, not yet published */
	long long staged_deadline_ms;
	unsigned long long stage_timeout_n;
	/* the standby that last let a stage time out, and until when
	 * pick_backup() passes it over */
	int passed_over_id;
	long long passed_over_until_ms;
	/* backup side: control state held ON BEHALF of a master, never adopted - this
	 * node places from the broadcast map */
	struct pc_clhist held_hist;
	struct pc_clmap held_map;      /* so a promotion can republish it */
	int held_valid;
	uint32_t held_term, held_seq;
	unsigned long long sync_sent_n, sync_rx_n, sync_ack_n, sync_bad_n;
	int reconcile_on;
	/* the tick recovery finished at: while reconcile_on, a record written at or below
	 * it is unruled recovered data (after a rebuild ask: stored before the absence) */
	unsigned int rec_tick;
	/* the boundary of the last rebuild pass started - an ask
	 * at or below it is covered by it */
	unsigned int rb_cover;
	/* the hold is on: reconcile is asking or its answers are due; retaken each tick,
	 * so a record's check is one load */
	int rec_hold;
	/* the pass is a REBUILD: back without a restart, so the fleet was up through the absence */
	int rec_rebuild;
	int rec_floored;                   /* a floor's pass, never a rebuild's */
	/* a resync: the records in question dropped by a sliced walk (hold on), then the
	 * fleet's state pulled as a bootstrap; resync_floor gives it an empty node's floors */
	int resync_on;
	int resync_floor;
	unsigned long long resync_dropped_now;  /* this resync's drops */
	/* a rejoin's pull asks this survivor first (0 = none), takes survivors only while rejoin_on;
	 * a snapshot follows any resync */
	int boot_prefer;
	int rejoin_on;
	int rejoin_whole;              /* the survivor picked is short of no delete: so are its fallbacks */
	long long surv_since;          /* a survivor's pick waits on marks too fresh to judge, since (ms) */
	/* RECOVERING from its ONLINE (or a copy's end) until the open pass first rules: what it
	 * replayed is never served before a survivor's copy replaces it */
	int decide_on;
	long long decide_ms;
	int snap_due;
	/* the copy counts once the node that served it (rejoin_src, the bulk thread's) is heard
	 * again at the incarnation it had when picked, still a survivor */
	int rejoin_src;
	long long rejoin_end_ms;
	int rejoin_ncand;
	int rejoin_cand[BOOT_MAX_CAND];
	unsigned int rejoin_inc[BOOT_MAX_CAND];
	/* when this node raises its own SIGTERM (0 = not scheduled): written by a worker
	 * or the cluster thread, read by the cluster thread's tick */
	long long stop_self_at_ms;
	int rec_col;
	unsigned int rec_cursor;
	long long rec_start_ms;        /* the pass's first probe */
	struct clrecon_fp fp;          /* the fast path's state */
	struct clrecon recon;          /* the walk's pacing */
	unsigned long long last_run_scanned;   /* run_migration's last walk */
	unsigned long long place_map_n, place_hrw_n;
	int master_id;
	pc_addr master_addr;
	long long master_seen_ms;
	/* the cluster thread's progress clock: stamped by that thread, read by the
	 * heartbeat thread (all atomics; see stall_check) */
	long long prog_ms;
	int prog_what;
	long long stall_since_ms, stall_last_ms;
	int apply_stall_ms;            /* config; 0 = the detector is off */
	struct clapply ap;             /* the apply rings; ap.n = 0 inline */
	int apply_threads_cfg;
	uint64_t join_tok;
	long long join_deadline_ms, join_last_ms;
	int join_held;                 /* said "held" once this join */
	/* the bulk TCP plane (records above the datagram ceiling): the peer thread's
	 * tick fills a batch and target, the bulk thread owns every socket */
	int blfd;                      /* TCP listener on advertise:port */
	struct clbulk bk;              /* framing + the batch handoff */
	unsigned char *bulkb;          /* tick-side collect buffer */
	size_t bulkb_len;
	unsigned int bulk_recs;
	/* the bootstrap pull, handed to the bulk thread under bmx */
	struct clboot bt;              /* the bootstrap decision */
	struct clpush wp;              /* per-thread write-path groups */
	int boot_done;                 /* pulled a full walk this incarnation */
	int boot_rounds;               /* candidate lists tried so far */
	int boot_round_failed;         /* set by the bulk thread: re-pick */
} C;

/* the settings the fleet must agree on: set once before the threads start, read by them after;
 * the keepalive and the members reply carry them, with the facts (put_facts, self_features) */
static struct pc_cfgsig cfgsig[PC_CFGSIG_MAX];
static int ncfgsig;
static int put_facts(uint32_t *name, uint32_t *val, int at, int cap, int features);
static int self_features(void);
/* PERFCACHED_TEST_NO_RESPMAP: announce no RESPMAP bit, as an older build, so a suite sees the map
 * verb refused */
static int self_no_respmap;
/* PERFCACHED_TEST_NO_AUTOSCALE: no AUTOSCALE bit, no digest, no fleet modes (an older build);
 * PERFCACHED_TEST_AUTOSCALE_DEAF: advertise both but ignore the fleet's modes */
static int self_no_autoscale, self_as_deaf;
/* PERFCACHED_TEST_NO_ACKS: send an older build's keepalive (no features word, no marks), so a
 * suite sees the fleet's gate name it */
static int self_no_acks;
/* PERFCACHED_TEST_FREEZE_MARKS: announce each space's first mark for ever while applying as usual;
 * PERFCACHED_TEST_ACKS_STUCK_MS: how long a mark must stand still to be stuck */
static int self_freeze_marks;
/* PERFCACHED_TEST_GAP_NO_COPIES: re-send no copy of another origin's deletes, as past its time
 * rule (a 10 s tick) */
static int self_gap_no_copies;
static long long acks_stuck_ms = 30000;    /* a stuck mark: still 30 s */
_Static_assert(CLACKF_MEMBERS == PC_CL_MAXMEMBERS,
	"clackf holds every member a fleet can have");
/* PERFCACHED_TEST_NO_HOLDQ: announce no HOLDQ bit and drop M_HOLD_Q, as an older build, so a suite
 * sees such a fleet walk with pulls */
static int self_no_holdq;
/* PERFCACHED_TEST_NO_PROMOTE: never take mastership, so a suite can stand up the higher address
 * the bounded deferral exists for */
static int self_no_promote;
/* PERFCACHED_TEST_RECONCILE: a restart's or a floor's records are ruled on one by one with a
 * witness, as before 12ol, so the suites of that pass and its hold can reach it */
static int self_test_reconcile;
/* PERFCACHED_TEST_SURVIVOR_WAIT_MS: a rejoin's survivor is picked no sooner after the first ask, so a
 * suite sees what the node serves once admitted and before its copy */
static int self_survivor_wait_ms;
/* PERFCACHED_TEST_BOOT_DELAY_MS: this node's first bootstrap pull waits, so a suite can stop its
 * sources mid-copy */
static int self_boot_delay_ms;
/* PERFCACHED_TEST_RECONCILE_WALK_WAIT_MS: a narrowed reconcile pass waits before it walks, so a
 * suite can reach the pass between its digests and its walk */
static int self_recon_walk_wait_ms;
/* PERFCACHED_TEST_MIG_SCAN_RECS: a migration walk stops after this many records as at its
 * deadline, so a suite reaches a table too large for one tick's walk */
static unsigned int self_mig_scan_recs;
/* PERFCACHED_TEST_WALK_NOMEM_MS: for this long after the start the sweep and migration walks fail
 * as when their buffers cannot be allocated */
static long long self_walk_nomem_until;
/* a member's election defers to a live higher address; since when,
 * and to which.  Cleared by a master beacon followed, and by promoting */
static long long elect_defer_ms;
static pc_addr elect_defer_to;


static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* the same clock, coarse - a few ns through the vDSO, at most a
 * jiffy behind now_ms(); for the write path's "anything to clear?" */
static long long now_ms_coarse(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- sealed datagrams -------------------------------------------------- */

/* the send path is clsend's, handed C.fd and C.psk at init */
static int seal_send(const pc_addr *to, const unsigned char *pt,
		size_t n)
{
	return clsend_seal(to, pt, n);
}

/* [cluster] discovery = unicast - set before pc_cluster_init */
static int cl_unicast;
static const char *cl_seeds;
static unsigned long long mesh_hints_sent, mesh_hints_recv;
static void mesh_hint_tick(long long now);
static void mesh_note(const pc_addr *from, int type, int trial);

void pc_cluster_set_discovery(int unicast, const char *seeds)
{
	cl_unicast = unicast;
	cl_seeds = seeds;
}

static int cl_max_dgram, cl_lane_port;
static int cl_expect;

void pc_cluster_set_expect(int n)
{
	cl_expect = n;
}

/* the page's header counts the fleet against it */
int pc_cluster_expect(void)
{
	return cl_expect;
}

static int cl_id_source;                       /* PC_IDSRC_* */

void pc_cluster_set_identity_source(int src)
{
	cl_id_source = src;
}

void pc_cluster_set_lane(int max_dgram, int lane_port)
{
	cl_max_dgram = max_dgram;
	cl_lane_port = lane_port;
	clrp_set_tcp_min(max_dgram > 0 ? (size_t)max_dgram : 0);
}

/* a forced lane is not for ever - see lane_retry_tick */
static long long cl_lane_retry_ms = 30000;
static unsigned long long lane_restored;

void pc_cluster_set_lane_retry(int s)
{
	cl_lane_retry_ms = s > 0 ? (long long)s * 1000 : 30000;
}

unsigned long long pc_cluster_lane_restored(void)
{
	return lane_restored;
}

/* a CONTROL frame to the fleet: the multicast group, or in unicast mode one sealed copy per target
 * (clmesh.h); data never comes here.  0 if a copy left (or no target), -1 if every send failed */
static int ctl_send(const unsigned char *pt, size_t n)
{
	static __thread pc_addr tgt[CLMESH_MAX_SEEDS * 8 +
		CLMESH_MAX_KNOWN];
	int i, k, ok = 0;

	if (!cl_unicast)
		return seal_send(&C.mcast_dst, pt, n);
	k = clmesh_targets(tgt, (int)(sizeof tgt / sizeof tgt[0]),
		&C.self_addr, now_ms());
	for (i = 0; i < k; i++)
		if (seal_send(&tgt[i], pt, n) == 0)
			ok = 1;
	return !k || ok ? 0 : -1;
}

/* ---- the pub/sub relay plane: decided in clps.c, said here ---------------- */

int pc_cluster_pubsub_rx_open(int port, int threads)
{
	int why, err, n = clps_rx_open(port, threads, &why, &err);

	if (why == CLPS_RX_TAKEN)
		LM_WARN("cluster: pub/sub relay port %s:%d is taken - relays stay on "
			"the cluster socket (set [cluster] pubsub_port, or "
			"pubsub_rx_threads = 0 to silence this)\n",
			pc_addr_str(&C.self_addr), port);
	else if (why == CLPS_RX_BIND)
		LM_WARN("cluster: cannot bind pub/sub relay port %s:%d (%s) - "
			"relays stay on the cluster socket\n",
			pc_addr_str(&C.self_addr), port, strerror(err));
	else if (why == CLPS_RX_OPEN)
		LM_NOTICE("cluster: pub/sub relays on %s:%d, %d receive thread(s)%s\n",
			pc_addr_str(&C.self_addr), port, n, n < 2 ? "" :
			clps_rx_lanes() ? ", one per sending lane" :
			", spread by the kernel's hash (no lane filter)");
	return n;
}

/* the cluster thread, once a second: probe each live peer's relay port
 * until it answers, re-confirm while it does, and say so on a change */
static void ps_probe_tick(long long now)
{
	int i;

	for (i = 0; i < C.pt.n_peers; i++) {
		struct peer *p = &C.pt.peers[i];

		switch (clps_probe_peer(p, now)) {
		case CLPS_EV_DIRECT:
			LM_NOTICE("cluster: pub/sub relays to node %d now use its relay "
				"port %u\n", p->node, (unsigned)p->ps.port);
			break;
		case CLPS_EV_STOPPED:
			LM_WARN("cluster: node %d stopped answering on relay port %u - "
				"relays to it fall back to the cluster socket\n",
				p->node, (unsigned)p->ps.port);
			break;
		case CLPS_EV_SILENT:
			LM_WARN("cluster: node %d advertises pub/sub relay port %u but "
				"has not answered there in 10 s - relays to it stay on the "
				"cluster socket (a firewall between the nodes?)\n",
				p->node, (unsigned)p->ps.port);
			break;
		}
		/* replication's own answer on the same port */
		switch (clrp_probe_peer(p, now)) {
		case CLPS_EV_DIRECT:
			LM_NOTICE("cluster: replication to node %d now uses its relay "
				"port %u\n", p->node, (unsigned)p->rp.port);
			break;
		case CLPS_EV_STOPPED:
			LM_WARN("cluster: node %d stopped answering replication probes "
				"on relay port %u - replication to it falls back to the "
				"cluster socket\n", p->node, (unsigned)p->rp.port);
			break;
		case CLPS_EV_SILENT:
			if (psr_path_direct(&p->ps, now))
				LM_NOTICE("cluster: node %d takes relays on port %u but "
					"not replication (an older build, or apply_threads) - "
					"replication to it stays on the cluster socket\n", p->node,
					(unsigned)p->rp.port);
			break;
		}
	}
}

void pc_cluster_repl_tcp_figures(unsigned long long out[6])
{
	struct clrp_figures f;
	struct clrpt_figs t;

	clrp_figures(&f);
	clrpt_figures(&t);
	out[0] = f.sent_tcp;
	out[1] = t.fallbacks;
	out[2] = t.lost_frames;
	out[3] = t.rx_frames;
	out[4] = t.connects;
	out[5] = t.connect_fail;
}

void pc_cluster_repl_plane_figures(int *peers_direct,
		unsigned long long *sent_direct, unsigned long long *sent_cluster,
		unsigned long long *rx_groups, unsigned long long *rx_refused)
{
	struct clrp_figures f;

	clrp_figures(&f);
	*peers_direct = f.peers_direct;
	*sent_direct = f.sent_direct;
	*sent_cluster = f.sent_cluster;
	*rx_groups = f.rx_groups;
	*rx_refused = f.rx_refused;
}

/* ---- unknown commands, fleet-wide: each node sends its whole obs.c table to every live peer,
 * a lost one repaired by the next; the peer thread writes, /stats folds, under unk_mx ---- */
#define UNK_REFRESH_MS 30000
#define UNK_STALE_MS   (3 * UNK_REFRESH_MS + 5000)
struct unk_peer {
	int node;
	long long at_ms;
	struct clunk_table t;
};
static pthread_mutex_t unk_mx = PTHREAD_MUTEX_INITIALIZER;
static struct unk_peer *unk_peers[PC_CL_MAXPEER];   /* by peer slot, lazily */
static unsigned long long unk_sent_gen;
static long long unk_sent_ms = -UNK_REFRESH_MS;
static int unk_force;                  /* a peer is new: send ours now */
static unsigned long long unk_sent_set; /* the live peers it last went to */
static unsigned long long unk_recv, unk_bad;

static void unk_tick(long long now)
{
	static unsigned char buf[1 + CLUNK_FRAME_MAX];
	static struct clunk_table t;
	unsigned long long gen = pc_obs_unknown_gen(), set = 0;
	size_t n;
	int i;

	if (!C.node_id || now - unk_sent_ms < 1000)
		return;
	/* the live set as a signature: a peer that became live since the last send has
	 * not heard us, and must not wait 30 s for the refresh */
	for (i = 0; i < C.pt.n_peers; i++) {
		struct peer *p = &C.pt.peers[i];
		int node = __atomic_load_n(&p->node, __ATOMIC_ACQUIRE);

		if (node && clpeers_live(p, now))
			set += ((unsigned long long)(unsigned)node << 20) ^
				(unsigned long long)(i + 1) * 0x9E3779B97F4A7C15ULL;
	}
	if (gen == unk_sent_gen && !unk_force && set == unk_sent_set &&
	    now - unk_sent_ms < UNK_REFRESH_MS)
		return;
	pc_obs_unknown_local(&t);
	buf[0] = M_UNKCMD;
	n = clunk_write(&t, (unsigned)C.node_id, buf + 1, sizeof buf - 1);
	if (!n)
		return;
	for (i = 0; i < C.pt.n_peers; i++) {
		struct peer *p = &C.pt.peers[i];

		if (!__atomic_load_n(&p->node, __ATOMIC_ACQUIRE) ||
		    !clpeers_live(p, now))
			continue;
		seal_send(&p->addr, buf, n + 1);
	}
	unk_sent_gen = gen;
	unk_sent_set = set;
	unk_sent_ms = now;
	unk_force = 0;
}

static void handle_unkcmd(const unsigned char *pt, size_t n,
		const pc_addr *from)
{
	static struct clunk_table t;       /* the peer thread's alone */
	struct unk_peer *u;
	struct peer *p;
	unsigned node;
	int slot;

	if (n < 1 || clunk_parse(pt + 1, n - 1, &node, &t) != 0) {
		__atomic_add_fetch(&unk_bad, 1, __ATOMIC_RELAXED);
		return;
	}
	if (!(p = clpeers_by_id_from(&C.pt, node, from)))
		return;
	slot = (int)(p - C.pt.peers);
	pthread_mutex_lock(&unk_mx);
	if (!unk_peers[slot])
		unk_peers[slot] = calloc(1, sizeof *unk_peers[slot]);
	u = unk_peers[slot];
	if (u) {
		if (u->node != (int)node)
			unk_force = 1;         /* first word from it: answer with ours */
		u->node = (int)node;
		u->at_ms = now_ms();
		u->t = t;
	}
	pthread_mutex_unlock(&unk_mx);
	__atomic_add_fetch(&unk_recv, 1, __ATOMIC_RELAXED);
}

int pc_cluster_unknown_fleet(const struct clunk_table *self,
		struct clunk_table *out, int *members, int *reporting)
{
	long long now = now_ms();
	int i;

	memset(out, 0, sizeof *out);
	clunk_fold(out, self, 0);
	*members = *reporting = 1;
	if (C.enabled) {
		pthread_mutex_lock(&unk_mx);
		for (i = 0; i < C.pt.n_peers; i++) {
			struct peer *p = &C.pt.peers[i];
			int node = __atomic_load_n(&p->node, __ATOMIC_ACQUIRE);
			const struct unk_peer *u = unk_peers[i];

			if (!node || !clpeers_live(p, now))
				continue;
			(*members)++;
			if (u && u->node == node && now - u->at_ms < UNK_STALE_MS) {
				clunk_fold(out, &u->t, node);
				(*reporting)++;
			}
		}
		pthread_mutex_unlock(&unk_mx);
		for (i = 0; i < out->n; i++)
			if (!out->rows[i].node)
				out->rows[i].node = C.node_id;
	}
	clunk_sort(out);
	return C.enabled;
}

void pc_cluster_unknown_figures(unsigned long long *recv,
		unsigned long long *bad)
{
	*recv = __atomic_load_n(&unk_recv, __ATOMIC_RELAXED);
	*bad = __atomic_load_n(&unk_bad, __ATOMIC_RELAXED);
}
/* the header of the datagram this thread is dispatching, so a handler can read the sender's
 * cluster id and flags (handle_assign adopts it, handle_master_alive yields to it) */
static __thread const unsigned char *rx_hdr;
/* ...a COPY of it, never the receive buffer (drain_one_fd's stack); rx_hdr points here while a
 * datagram is dispatched, NULL otherwise */
static __thread unsigned char rx_hdr_copy[19];
static unsigned long long cid_foreign_seen;

/* every OTHER established cluster this node hears (same seeds or group): which, from where, how
 * recently, and an ERROR the first time; never a merge.  Cluster thread writes, /stats reads */
#define FOREIGN_MAX     16
#define FOREIGN_KEEP_MS (5 * 60 * 1000)
static pthread_mutex_t foreign_mx = PTHREAD_MUTEX_INITIALIZER;
static struct {
	unsigned char cid[16];
	pc_addr a;              /* the last sender */
	long long first_ms, last_ms;
	unsigned long long frames;
} foreign[FOREIGN_MAX];
static int foreign_n;

static void foreign_note(const unsigned char *cid, const unsigned char *mine,
		const pc_addr *from, long long now)
{
	int i, slot = -1, oldest = 0;
	char a[37], b[37];

	pthread_mutex_lock(&foreign_mx);
	for (i = 0; i < foreign_n; i++) {
		if (!memcmp(foreign[i].cid, cid, 16)) {
			slot = i;
			break;
		}
		if (foreign[i].last_ms < foreign[oldest].last_ms)
			oldest = i;
	}
	if (slot >= 0 && now - foreign[slot].last_ms < FOREIGN_KEEP_MS) {
		foreign[slot].a = *from;
		foreign[slot].last_ms = now;
		foreign[slot].frames++;
		pthread_mutex_unlock(&foreign_mx);
		return;
	}
	if (slot < 0)
		slot = foreign_n < FOREIGN_MAX ? foreign_n++ : oldest;
	memcpy(foreign[slot].cid, cid, 16);
	foreign[slot].a = *from;
	foreign[slot].first_ms = foreign[slot].last_ms = now;
	foreign[slot].frames = 1;
	pthread_mutex_unlock(&foreign_mx);
	pc_cid_str(cid, a);
	pc_cid_str(mine, b);
	LM_ERR("cluster: a SECOND cluster is on this network - %s:%d belongs "
		"to cluster %s, this node to %s.  Nothing is exchanged with it "
		"and the two never merge; if one fleet was meant, a node has "
		"another cluster's id in its state directory (or its pin "
		"differs)\n", pc_addr_str(from), pc_addr_port(from),
		a, b);
}

/* the last datagram each sender had refused as another cluster's: UDP reaches us from it, so a
 * silence it shows is no blocked path (a cold-start race), and the lane is not forced for it */
#define FRX_MAX 16
static pthread_mutex_t frx_mx = PTHREAD_MUTEX_INITIALIZER;
static struct { pc_addr a; long long ms; } frx[FRX_MAX];
static int frx_n;

static void frx_note(const pc_addr *a, long long now)
{
	int i, old = 0;

	pthread_mutex_lock(&frx_mx);
	for (i = 0; i < frx_n && !addr_eq(&frx[i].a, a); i++)
		if (frx[i].ms < frx[old].ms)
			old = i;
	if (i == frx_n)
		i = frx_n < FRX_MAX ? frx_n++ : old;
	frx[i].a = *a;
	frx[i].ms = now;
	pthread_mutex_unlock(&frx_mx);
}

static int frx_since(const pc_addr *a, long long since)
{
	int i, r = 0;

	pthread_mutex_lock(&frx_mx);
	for (i = 0; i < frx_n && !r; i++)
		r = addr_eq(&frx[i].a, a) && frx[i].ms >= since;
	pthread_mutex_unlock(&frx_mx);
	return r;
}

int pc_cluster_foreign_list(char *ids, int cap_ids, char addrs[][24],
		long long *ago_ms, unsigned long long *frames, int cap)
{
	long long now = now_ms();
	int i, n = 0;

	pthread_mutex_lock(&foreign_mx);
	for (i = 0; i < foreign_n && n < cap && n < cap_ids; i++) {
		if (now - foreign[i].last_ms >= FOREIGN_KEEP_MS)
			continue;
		pc_cid_str(foreign[i].cid, ids + n * 37);
		snprintf(addrs[n], 24, "%s:%d",
			pc_addr_str(&foreign[i].a),
			pc_addr_port(&foreign[i].a));
		ago_ms[n] = now - foreign[i].last_ms;
		frames[n] = foreign[i].frames;
		n++;
	}
	pthread_mutex_unlock(&foreign_mx);
	return n;
}

/* the id a node with NONE last heard on a live member's beat, so it founds INTO that cluster, not
 * under an id its old members would refuse; written only while id-less, so the lock is free */
static pthread_mutex_t cid_heard_mx = PTHREAD_MUTEX_INITIALIZER;
static unsigned char cid_heard[CLWIRE_CID_LEN];
static long long cid_heard_ms;
#define CID_HEARD_FRESH_MS 3000

/* may this datagram be handled (clcid.h)?  No id yet or the same id: all; zero id: no authority
 * kinds; foreign: refused, but a provisional non-member hears a foreign master (cold start) */
static int cid_admit(const unsigned char *buf, int type,
		const pc_addr *from)
{
	unsigned char mine[16], z[16];
	const unsigned char *theirs = CLWIRE_HDR_CID(buf);
	int st = clwire_cid(mine);
	long long now;
	char a[37], b[37];

	memset(z, 0, sizeof z);
	if (st == CLWIRE_CID_NONE) {
		if ((type == M_ALIVE || type == M_MASTER_ALIVE) &&
		        memcmp(theirs, z, 16) != 0) {
			pthread_mutex_lock(&cid_heard_mx);
			memcpy(cid_heard, theirs, 16);
			cid_heard_ms = now_ms();
			pthread_mutex_unlock(&cid_heard_mx);
		}
		return 1;
	}
	if (!memcmp(theirs, mine, 16))
		return 1;
	/* a zero id is a node that has joined nothing (restarted without state, still
	 * owning keys): its data plane flows, but a blank node is never a master */
	if (!memcmp(theirs, z, 16))
		return type != M_MASTER_ALIVE && type != M_CLMAP &&
			type != M_ASSIGN && type != M_JOIN_REJ &&
			type != M_JOIN_WAIT && type != M_CLSYNC &&
			type != M_CLSYNC_ACK && type != M_DEMOTE &&
			type != M_FLEETSTOP && type != M_COL_SET &&
			type != M_STATS_RESET && type != M_ALERT_RELAY;
	if (st == CLWIRE_CID_PROVISIONAL && C.role != PC_ROLE_MEMBER &&
	        type == M_MASTER_ALIVE)
		return 1;
	/* our provisional cluster's master now speaks for another: it yielded inside its grace, and
	 * a member left behind would wait for ever in a cluster nobody else is in - follow it */
	if (st == CLWIRE_CID_PROVISIONAL && C.role == PC_ROLE_MEMBER &&
	        (type == M_ALIVE || type == M_MASTER_ALIVE) && addr_eq(from, &C.master_addr)) {
		pc_cid_str(theirs, a);
		LM_WARN("cluster: master %s now speaks for cluster %s - it yielded while ours was "
			"provisional; re-joining through it\n", pc_addr_str(from), a);
		pc_cid_drop();
		C.master_id = 0;               /* a member there now: the winner's beacon is joined */
		C.master_seen_ms = 0;
		join_again(NULL);
		return 0;
	}
	__atomic_fetch_add(&cid_foreign_seen, 1, __ATOMIC_RELAXED);
	frx_note(from, now_ms());
	/* in unicast mode a refused PROVISIONAL founder is still passed to the master, or
	 * two founders of a cold start never meet; never an established foreigner */
	if (cl_unicast && (CLWIRE_HDR_FLAGS(buf) & CLWIRE_F_PROVISIONAL))
		mesh_note(from, type, 1);
	/* once a minute per sender, and only between established clusters:
	 * a provisional foreigner is a founder about to yield */
	if ((CLWIRE_HDR_FLAGS(buf) & CLWIRE_F_PROVISIONAL) ||
	        st != CLWIRE_CID_ESTABLISHED)
		return 0;
	now = now_ms();
	foreign_note(theirs, mine, from, now);
	if (!pc_logrl_ok(LRL_FOREIGN_CID, from))
		return 0;
	pc_cid_str(theirs, a);
	pc_cid_str(mine, b);
	LM_WARN("cluster: ignoring %s - it belongs to cluster %s, this node "
		"to cluster %s; clusters never merge, nothing is exchanged "
		"with it (foreign_seen %llu)\n", pc_addr_str(from), a,
		b, __atomic_load_n(&cid_foreign_seen, __ATOMIC_RELAXED));
	return 0;
}

/* about to join this datagram's sender: drop a provisional id only if it is ANOTHER cluster (a
 * cold-start race); a step-down inside this one keeps it, or its own members refuse a new id */
static void cid_follow_foreign(void)
{
	unsigned char mine[CLWIRE_CID_LEN];

	if (rx_hdr && clwire_cid(mine) == CLWIRE_CID_PROVISIONAL &&
	        memcmp(CLWIRE_HDR_CID(rx_hdr), mine, CLWIRE_CID_LEN) != 0)
		pc_cid_drop();
}

void pc_cluster_cid_info(char out[37], const char **state,
		unsigned long long *foreign)
{
	unsigned char c[16];
	int st = pc_cid_get(c);

	pc_cid_str(st ? c : NULL, out);
	*state = pc_cid_state_name(st);
	*foreign = __atomic_load_n(&cid_foreign_seen, __ATOMIC_RELAXED);
}

/* a rotation's other secret, opened with and never sealed with */
static uint8_t psk_accept[PC_NOISE_KEYLEN];
static int have_psk_accept;

void pc_cluster_set_accept_psk(const uint8_t psk[PC_NOISE_KEYLEN])
{
	memcpy(psk_accept, psk, PC_NOISE_KEYLEN);
	have_psk_accept = 1;
}

static int open_dgram(unsigned char *buf, size_t n, unsigned char *pt,
		unsigned long long *ptlen)
{
	return clwire_open(C.psk, buf, n, pt, ptlen) == 0 ||
		(have_psk_accept && clwire_open(psk_accept, buf, n, pt, ptlen) == 0)
		? 0 : -1;
}


/* ---- the heartbeat watchdog: keeps a busy node audible (silence reads as death); emission only.
 * The frames cross threads under a seqlock; sendto is atomic, C.psk/C.mcast_dst fixed at init */
#define BEAT_OVERDUE_MS 1600

/* ---- the progress clock: stamped wherever the cluster thread moves, read by the heartbeat
 * thread; no stamp for apply_stall_ms is STALLED, patched into the re-sent frames ---------- */
#define PC_PROG_LOOP    0
#define PC_PROG_TICK    1              /* the 1 Hz duties */
#define PC_PROG_REBAL   2              /* the rebalance / repair sweep */
#define PC_PROG_DGRAM   3              /* handling a datagram */
#define PC_PROG_FLUSH   4              /* flushing a push group */
#define PC_PROG_PUMP    5              /* a long duty, saying so */
#define PC_PROG_APPLY   6              /* an apply thread, not the receiver */

static void prog(int what)
{
	__atomic_store_n(&C.prog_what, what, __ATOMIC_RELAXED);
	__atomic_store_n(&C.prog_ms, now_ms(), __ATOMIC_RELAXED);
}

static const char *prog_name(int what)
{
	switch (what) {
	case PC_PROG_TICK:  return "the 1 Hz membership duties";
	case PC_PROG_REBAL: return "the rebalance / repair sweep";
	case PC_PROG_DGRAM: return "handling a datagram";
	case PC_PROG_FLUSH: return "flushing a push group";
	case PC_PROG_PUMP:  return "a long duty (migration, a victim walk)";
	case PC_PROG_APPLY: return "an apply thread with records queued";
	default:            return "between duties";
	}
}

void pc_cluster_set_apply_stall_ms(int ms)
{
	C.apply_stall_ms = ms;
}

/* ---- the apply threads ------------------------------------------------- */
void pc_cluster_set_apply_threads(int n)
{
	C.apply_threads_cfg = n;
}

int pc_cluster_apply_threads(void)
{
	return C.ap.n;
}

/* heartbeat thread, 4 Hz.  Returns 1 while the overlay is up. */
static int stall_check(long long now)
{
	static long long last_check_ms, grace_until_ms;
	long long seen = __atomic_load_n(&C.prog_ms, __ATOMIC_RELAXED);
	int limit = C.apply_stall_ms;
	long long idle = now - seen;
	int what = -1;                     /* -1 = the receiver's own stamp */

	if (limit <= 0 || !seen)
		return 0;                  /* off, or the thread has not started */
	/* this thread wakes at 4 Hz: if IT was away over a second, the whole process was
	 * stopped (SIGSTOP, VM pause), not wedged - give the cluster thread a second */
	if (last_check_ms && now - last_check_ms > 1000)
		grace_until_ms = now + 1000;
	last_check_ms = now;
	if (now < grace_until_ms)
		return pc_node_stalled();
	/* an apply thread on ONE item for the limit stalls 1/N of the keyspace, and must
	 * be as visible as a stall of all of it */
	{
		int i;

		for (i = 0; i < C.ap.n; i++) {
			struct clapply_ring *rg = &C.ap.ring[i];
			long long b = __atomic_load_n(&rg->busy_since_ms,
				__ATOMIC_RELAXED);
			long long w = __atomic_load_n(&rg->wait_since_ms,
				__ATOMIC_RELAXED);

			/* on one item too long */
			if (b && now - b > idle) {
				idle = now - b;
				seen = b;
				what = PC_PROG_APPLY;
			}
			/* or an untouched backlog: stamp the first sighting; every take
			 * and an empty ring clear it, as a sighting can land after a take */
			if (clapply_backlog(&C.ap, i) == 0) {
				if (w)
					__atomic_store_n(&rg->wait_since_ms, 0LL,
						__ATOMIC_RELAXED);
				continue;
			}
			if (!w) {
				__atomic_store_n(&rg->wait_since_ms, now,
					__ATOMIC_RELAXED);
				continue;
			}
			if (now - w > idle) {
				idle = now - w;
				seen = w;
				what = PC_PROG_APPLY;
			}
		}
	}
	if (idle >= limit) {
		C.stall_last_ms = now;
		if (!pc_node_stalled()) {
			C.stall_since_ms = seen;
			C.st.apply_stalls++;
			pc_node_set_stalled(1);
			LM_WARN("cluster: the cluster thread has made no "
				"progress for %lld ms (last seen %s) - this "
				"node reports STALLED: replication, pulls and "
				"the control plane are not being served here, "
				"and clients are steered to other members "
				"until it moves again\n", idle,
				prog_name(what >= 0 ? what
					: __atomic_load_n(&C.prog_what,
						__ATOMIC_RELAXED)));
		}
		if ((unsigned long long)(now - C.stall_since_ms) >
		        C.st.apply_stall_ms_max)
			C.st.apply_stall_ms_max =
				(unsigned long long)(now - C.stall_since_ms);
		return 1;
	}
	if (pc_node_stalled() &&
	        now - C.stall_last_ms >= limit) {
		pc_node_set_stalled(0);
		LM_NOTICE("cluster: the cluster thread is moving again - "
			"stalled for %lld ms, steady for %d ms since; this "
			"node reports %s\n",
			C.stall_last_ms - C.stall_since_ms, limit,
			pc_node_state_name(pc_node_state()));
	}
	return pc_node_stalled();
}

void pc_cluster_beat_thread(volatile int *stop)
{
	while (!*stop) {
		struct timespec ts = { 0, 250L * 1000000 };
		unsigned char a[sizeof C.wr.alive], ma[sizeof C.wr.malive];
		size_t alen, mlen;

		int stalled, was = pc_node_stalled();

		nanosleep(&ts, NULL);
		stalled = stall_check(now_ms());
		/* the beat that says STALLED goes out NOW, not when
		 * the overdue window next comes round */
		if (now_ms() - clwire_sent_ms(&C.wr) < BEAT_OVERDUE_MS &&
		        !(stalled && !was))
			continue;
		clwire_beat_take(&C.wr, a, &alen, ma, &mlen);
		if (!alen)
			continue;              /* nothing built yet (joining) */
		/* the LAST frame the cluster thread built; this thread corrects only
		 * the state, through the same overlay, so a node not READY keeps its own */
		if (stalled)
			(void)clmemb_alive_patch_state(a, alen, pc_node_state(),
				pc_node_reason());
		ctl_send(a, alen);
		/* a stalled MASTER's claim is not re-sent: it would hold the fleet to a master
		 * that admits nobody; silence lets MASTER_DEAD_MS elect */
		if (mlen && !stalled)
			ctl_send(ma, mlen);
		C.st.hb_watchdog++;
		/* rate-limited: these lines tell "the peer thread starved its beats" from "the
		 * beats flowed but a peer went deaf" */
		{
			static long long logged_ms;
			long long nl = now_ms();

			/* !logged_ms: the clock counts from boot, so the interval
			 * alone would silence the first line */
			if (!logged_ms || nl - logged_ms >= 10000) {
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

/* ---- the node's lifecycle state ---------------------------------------- */

_Static_assert(PC_NST_STARTING == PC_CLMAP_ST_STARTING &&
               PC_NST_RECOVERING == PC_CLMAP_ST_RECOVERING &&
               PC_NST_READY == PC_CLMAP_ST_READY &&
               PC_NST_DRAINING == PC_CLMAP_ST_DRAINING &&
               PC_NST_FAILED == PC_CLMAP_ST_FAILED &&
               PC_NST_STALLED == PC_CLMAP_ST_STALLED &&
               PC_NST_HEALING == PC_CLMAP_ST_HEALING &&
               PC_NST_CONNECTING == PC_CLMAP_ST_CONNECTING,
               "node state values must match the map's wire encoding");


/* ---- LE helpers -------------------------------------------------------- */
/* clcodec.h's LE helpers, under the short names used below */
#define p16 pc_p16
#define p32 pc_p32
#define p64 pc_p64
#define g16 pc_g16
#define g32 pc_g32
#define g64 pc_g64

/* ---- the locator and negative caches: clloc's, the wrappers read now_ms() --- */

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
	/* every negative entry has expired: the write skips the key's hash and slot; 20 ms
	 * covers the coarse clock's lag */
	if (now_ms_coarse() > __atomic_load_n(&C.lc.neg_until_ms,
	        __ATOMIC_ACQUIRE) + 20)
		return;
	clloc_neg_clear(&C.lc, col, cn, key, kn);
}

/* the delete's version rides the negative entry, and every versioned one is RETAINED past it
 * (clretain.h): local deletes and those applied from peers all pass here */
void pc_neg_set_ver(const char *col, size_t cn, const char *key, size_t kn,
		int ms, unsigned long long ver)
{
	long long now = now_ms();

	clloc_neg_set_ver(&C.lc, col, cn, key, kn, now, ms, ver);
	clretain_note(col, cn, key, kn, ver, (unsigned int)(now / 1000));
}

/* a numbered delete - planted as any, retained with its number
 * so the members' marks can forget it */
static void neg_set_ver_num(const char *col, size_t cn, const char *key,
		size_t kn, int ms, unsigned long long ver,
		const struct clfwd_tomb_num *num)
{
	long long now = now_ms();

	clloc_neg_set_ver(&C.lc, col, cn, key, kn, now, ms, ver);
	clretain_note_n(col, cn, key, kn, ver, (unsigned int)(now / 1000),
		num->oid, num->incarn, num->seq);
}

/* a replayed EXPIRY: planted as any tombstone, retained as the expiry the peer kept - never as a
 * delete, which would be replayed onward as one */
static void neg_set_ver_expiry(const char *col, size_t cn, const char *key,
		size_t kn, int ms, unsigned long long ver, int keep)
{
	long long now = now_ms();

	clloc_neg_set_ver(&C.lc, col, cn, key, kn, now, ms, ver);
	clretain_note_expiry(col, cn, key, kn, ver, (unsigned int)(now / 1000),
		keep);
}

/* a local drop's mark: retained apart from deletes, or the per-peer sweep would replay a restarted
 * node's startup flush to the peers as deletes */
void pc_neg_set_ver_local(const char *col, size_t cn, const char *key,
		size_t kn, int ms, unsigned long long ver)
{
	long long now = now_ms();

	clloc_neg_set_ver(&C.lc, col, cn, key, kn, now, ms, ver);
	clretain_note_local(col, cn, key, kn, ver, (unsigned int)(now / 1000));
}

/* when the 2-s entry has lapsed (or held no version) the retained table answers, so every receive
 * path refuses a copy older than a retained delete, for tombstone_retain_s */
int pc_neg_ver(const char *col, size_t cn, const char *key, size_t kn,
		unsigned long long *ver)
{
	long long now = now_ms();
	uint64_t v = 0, rv;
	int r = clloc_neg_ver(&C.lc, col, cn, key, kn, now, &v);

	/* the later of the two - a cached version can lag the retained one */
	if ((rv = clretain_get(col, cn, key, kn, (unsigned int)(now / 1000))) > v) {
		*ver = rv;
		return 1;
	}
	*ver = v;
	return r;
}

void pc_cluster_set_acks(int on)
{
	if (!on)
		self_no_acks = 1;
}

/* [cluster] resync_after_s (a day by default; 0 = never by time) */
static long long resync_after_ms = 86400000LL;

void pc_cluster_set_resync_after(int s)
{
	resync_after_ms = s > 0 ? (long long)s * 1000 : 0;
}

/* [cluster] data_multicast (auto, the default) */
static int cl_data_mcast = 1;

void pc_cluster_set_data_multicast(int on)
{
	cl_data_mcast = on;
}

void pc_cluster_set_retain(int retain_s, int max)
{
	clretain_init(retain_s > 0 ? (unsigned int)retain_s : 0,
		max > 0 ? (unsigned int)max : 0);
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

/* ---- the config identity: every ALIVE carries a digest of the interchange config (mode, its
 * parameters, the collection SET); disagreeing nodes never exchange.  Local sizing is not in it */
static uint64_t digest_of(int cfg_only)
{
	const char *names[PC_MAX_COLLECTIONS];
	char ro[PC_MAX_COLLECTIONS][PC_COL_NAME_MAX + 4];
	struct clinit_digest_in in;
	int i, k = 0, n = pc_store_count();

	/* a mismatch would fold a different number of collections on one side of an
	 * upgrade and refuse every join */
	_Static_assert(CLINIT_MAX_NAMES == PC_MAX_COLLECTIONS,
		"clinit's name bound must match the store's");

	/* zeroed first: an unset field is not a wrong digest but a RANDOM one - every
	 * join refused, looking like a network fault */
	memset(&in, 0, sizeof in);
	if (n > PC_MAX_COLLECTIONS)
		n = PC_MAX_COLLECTIONS;
	/* upstream_writes = refuse must be agreed on, so a refusing collection is
	 * digested as <name>!ro; without the flag the digest is unchanged */
	for (i = 0; i < n; i++) {
		const char *nm = cfg_only ? pc_store_cfg_name(i) : pc_store_name(i);

		if (cfg_only && !nm)
			continue;
		names[k] = nm;
		if (nm && pc_upstream_refuses_writes(nm, strlen(nm))) {
			snprintf(ro[k], sizeof ro[k], "%s!ro", nm);
			names[k] = ro[k];
		}
		k++;
	}
	in.mode = C.cfg_mode;
	in.eager = C.cfg_eager;
	in.replicas = C.cfg_replicas;
	in.wal = C.cfg_wal;
	in.route_algo = PC_ROUTE_ALGO;
	in.names = names;
	in.n_names = k;
	return clinit_config_digest(&in);
}

/* every name in the registry, runtime creates included: what a 0.7.5 peer compares */
static uint64_t config_digest(void)
{
	return digest_of(0);
}

/* the config's collection set alone: a runtime create is agreed by its announce, and folding it
 * here split the fleet while the announce was on its way (cfg2 on the wire) */
static uint64_t config_digest2(void)
{
	return digest_of(1);
}

/* a peer's interchange config differs from ours: by the config's set when it sends one, else
 * (an older build) by the digest that folds every name; @ours gets the one compared */
static int digest_differs(int have2, uint64_t theirs2, uint64_t theirs, uint64_t *ours)
{
	*ours = have2 ? config_digest2() : config_digest();
	return (have2 ? theirs2 : theirs) != *ours;
}

void pc_cluster_set_config(int mode, int eager,
		int authoritative, int client_port, int resp_port,
		int http_port, int wal, int replicas)
{
	C.cfg_mode = mode;
	C.cfg_eager = eager;
	C.cfg_replicas = mode == PC_MODE_SPREAD ? replicas : 0;
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

/* may the fleet hold a record type needing @feat?  Only if every live member advertises it (an
 * older one keeps it as untyped bytes for good); 0 sets *@lacking; outside a cluster, always 1 */
_Static_assert(PC_FEAT_TYPES2 == CLMEMB_FEAT_TYPES2,
	"the verbs' feature bit and the ALIVE's must be the same bit");
_Static_assert(PC_FEAT_HREAD == CLMEMB_FEAT_HREAD,
	"the HREAD feature bit must match its wire bit");
_Static_assert(PC_FEAT_SLOCK == CLMEMB_FEAT_SLOCK,
	"the SLOCK feature bit must match its wire bit");
_Static_assert(PC_FEAT_RESPMAP == CLMEMB_FEAT_RESPMAP,
	"the RESPMAP feature bit must match its wire bit");
_Static_assert(PC_FEAT_AUTOSCALE == CLMEMB_FEAT_AUTOSCALE,
	"the AUTOSCALE feature bit must match its wire bit");

int pc_cluster_fleet_has(int feat, int *lacking)
{
	long long now = now_ms();
	int i;

	if (lacking)
		*lacking = 0;
	if (!pc_cluster_enabled())
		return 1;
	for (i = 0; i < C.pt.n_peers; i++) {
		struct peer *p = &C.pt.peers[i];
		int node = __atomic_load_n(&p->node, __ATOMIC_ACQUIRE);

		if (!node || !clpeers_live(p, now))
			continue;
		if (!(p->features & feat)) {
			if (lacking)
				*lacking = node;
			return 0;
		}
	}
	return 1;
}

/* may an origin forget by the members' marks?  Only while every live member announces them - a
 * feature bit, so an older member blocks it by silence; 0 lists up to @max lacking in @lack */
int pc_cluster_acks_gate(int *lack, int max, int *nlack)
{
	long long now = now_ms();
	int i, n = 0;

	if (!pc_cluster_enabled() || !pc_ack_enabled() || self_no_acks) {
		if (nlack)
			*nlack = 0;
		return 0;
	}
	for (i = 0; i < C.pt.n_peers; i++) {
		struct peer *p = &C.pt.peers[i];
		int node = __atomic_load_n(&p->node, __ATOMIC_ACQUIRE);

		if (!node || !clpeers_live(p, now) || p->acks)
			continue;
		if (n < max && lack)
			lack[n] = node;
		n++;
	}
	if (nlack)
		*nlack = n;
	return n == 0;
}

int pc_cluster_replicas(void)
{
	return C.cfg_replicas;
}

int pc_cluster_map_stamp(unsigned int *term, unsigned int *seq)
{
	if (!C.st.map_valid)
		return 0;
	*term = C.st.map_term;
	*seq = C.st.map_seq;
	return 1;
}

const char *pc_cluster_mode_name(void)
{
	if (C.cfg_mode == PC_MODE_PROXY)
		return "proxy";
	if (C.cfg_mode == PC_MODE_SHARD)
		return "shard";
	if (C.cfg_mode == PC_MODE_SPREAD)
		return "spread";
	return C.cfg_eager ? "eager" : "store";
}

int pc_cluster_port(void)
{
	return pc_addr_port(&C.mcast_dst);
}

int pc_cluster_authoritative(void)
{
	return C.cfg_authoritative;
}

static unsigned int self_free_mb(void);
static unsigned int self_total_mb(void);
static unsigned int self_entries(void);
static void self_cols(struct clmemb_col *cols, int *ncols, int *ntotal);
static int any_eager(void);

/* the start kind as a word, for members and the page */
const char *pc_start_kind_name(int kind)
{
	switch (kind) {
	case PC_START_COLD:        return "cold";
	case PC_START_RECOVERED:   return "recovered";
	case PC_START_ESTABLISHED: return "established";
	default:                   return "unknown";
	}
}

/* the fleet as a CLIENT needs to see it - address, client port,
 * id, role and load per member, self included.  Returns the count. */
const char *pc_cluster_gone_reason(int why)
{
	return clres_why_name(why);
}

int pc_cluster_members(struct pc_member *out, int max)
{
	int i, n = 0;

	if (!C.enabled || max < 1)
		return 0;
	out[n].addr = C.self_addr.in4.sin_addr;
	out[n].client_port = C.cfg_client_port;
	out[n].resp_port = C.cfg_resp_port;
	out[n].http_port = C.cfg_http_port;
	out[n].uptime_s = (long)pc_metrics_uptime();
	pc_alerts_link_state(&out[n].link_pct, &out[n].link_level);
	out[n].entries = (long)self_entries();
	out[n].start = pc_start_kind_name(self_start_kind());
	out[n].node = C.node_id;
	out[n].is_self = 1;
	out[n].is_master = C.role == PC_ROLE_MASTER;
	out[n].is_backup = C.map_valid &&
		C.map.backup_id == (uint16_t)C.node_id;
	out[n].mem_tier = (int)pcache_mem.tier;
	out[n].state = pc_node_state();
	out[n].reason = pc_node_reason();
	out[n].reconciling = C.rec_hold && C.cfg_eager;
	if (out[n].reconciling) {
		int wid, wn = rec_witness_count(&wid);

		out[n].sole_witness = wn == 0 ? 0 : wn == 1 ? wid
			: CLMEMB_SOLEWIT_MANY;
	} else
		out[n].sole_witness = CLMEMB_SOLEWIT_MANY;
	out[n].gone_s = -1;                             /* present */
	out[n].live = 1;
	out[n].gone_why = 0;
	out[n].gone_silent_s = 0;
	out[n].held_s = 0;
	out[n].free_mb = (int)self_free_mb();
	out[n].total_mb = (int)self_total_mb();
	out[n].has_ident = 1;
	memcpy(out[n].ident, C.self_ident, 16);
	out[n].incarn = C.self_incarn;
	out[n].cl_reported = 1;
	out[n].cl_open = (long)PC_RESP_READ(pc_clients_open);
	out[n].cl_resp = (long)PC_RESP_READ(pc_resp_open);
	out[n].cl_bin = (long)PC_RESP_READ(pc_nat_bin_open);
	out[n].cl_nresp = (long)PC_RESP_READ(pc_nat_resp_open);
	out[n].cols_reported = 1;
	out[n].relay_port = clps_rx_port();
	out[n].relay_direct = 0;
	self_cols(out[n].cols, &out[n].ncols, &out[n].ncols_total);
	out[n].ncfg = ncfgsig;
	for (i = 0; i < ncfgsig; i++) {
		out[n].cfg_name[i] = cfgsig[i].name_h;
		out[n].cfg_val[i] = cfgsig[i].val_h;
	}
	out[n].ncfg = put_facts(out[n].cfg_name, out[n].cfg_val,
		out[n].ncfg, PC_CFGSIG_MAX, self_features());
	n++;
	for (i = 0; i < C.pt.n_peers && n < max; i++) {
		struct peer *p = &C.pt.peers[i];

		if (!__atomic_load_n(&p->node, __ATOMIC_ACQUIRE))
			continue;
		if (!p->client_port)
			continue;      /* older peer: cannot be dialled */
		out[n].addr = p->addr.in4.sin_addr;
		out[n].client_port = p->client_port;
		out[n].resp_port = p->resp_port;
		out[n].http_port = p->http_port;
		out[n].uptime_s = p->boot_ms ? (long)((now_ms() - p->boot_ms) / 1000) : -1;
		out[n].link_pct = p->link_pct;
		out[n].link_level = p->link_level;
		out[n].entries = (long)p->entries;
		out[n].start = pc_start_kind_name(p->start_kind);
		out[n].node = p->node;
		out[n].is_self = 0;
		out[n].is_master = p->node == C.master_id;
		out[n].is_backup = C.map_valid &&
			C.map.backup_id == (uint16_t)p->node;
		out[n].state = p->nstate;
		out[n].reason = p->nreason;
		out[n].reconciling = p->reconciling;
		out[n].sole_witness = p->sole_witness;
		out[n].gone_s = -1;
		out[n].live = clpeers_live(p, now_ms());
		out[n].gone_why = 0;
		out[n].gone_silent_s = 0;
		out[n].held_s = 0;
		out[n].mem_tier = p->mem_tier;
		out[n].cl_reported = p->cl_reported;
		out[n].cl_open = (long)p->cl_open;
		out[n].cl_resp = (long)p->cl_resp;
		out[n].cl_bin = (long)p->cl_bin;
		out[n].cl_nresp = (long)p->cl_nresp;
		out[n].cols_reported = p->cols_reported;
		out[n].relay_port = __atomic_load_n(&p->ps.port, __ATOMIC_ACQUIRE);
		out[n].relay_direct = psr_path_direct(&p->ps, now_ms());
		out[n].relay_interest = __atomic_load_n(&p->pv_mode, __ATOMIC_ACQUIRE);
		out[n].ncols = p->ncols;
		out[n].ncols_total = p->ncols_total;
		memcpy(out[n].cols, p->cols, (size_t)p->ncols * sizeof p->cols[0]);
		out[n].free_mb = (int)p->free_mb;
		out[n].total_mb = (int)p->total_mb;
		out[n].has_ident = p->has_ident;
		memcpy(out[n].ident, p->ident, 16);
		out[n].incarn = p->incarn;
		out[n].ncfg = p->ncfg;
		memcpy(out[n].cfg_name, p->cfg_name, sizeof out[n].cfg_name);
		memcpy(out[n].cfg_val, p->cfg_val, sizeof out[n].cfg_val);
		n++;
	}
	/* and the members that LEFT, for as long as their reservation holds the id: purged from the
	 * peer table and dropped by the map's next build, only the reservation remembers them */
	{
		long long now = now_ms();
		int r, k;

		for (r = 0; r < CLRES_MAX && n < max; r++) {
			const struct clres_ent *e = &reserved.e[r];
			long long left_ms;

			if (!e->id || e->until_ms <= now)
				continue;
			left_ms = now - (e->until_ms - RESERVE_MS);
			if (left_ms < 0)
				continue;
			for (k = 0; k < n; k++)
				if (out[k].node == e->id)
					break;
			if (k < n)
				continue;              /* it is back */
			memset(&out[n], 0, sizeof out[n]);
			out[n].node = e->id;
			out[n].addr = e->addr;
			out[n].state = PC_NST_STARTING;   /* not reporting */
			out[n].reconciling = 0;
			out[n].sole_witness = -1;
			out[n].gone_s = (int)(left_ms / 1000);
			/* and how much longer the id is held, so the card
			 * says when it will go of its own accord */
			out[n].held_s = (int)((e->until_ms - now) / 1000);
			out[n].gone_why = e->why;
			out[n].gone_silent_s = e->silent_s;
			out[n].has_ident = e->has_ident;
			memcpy(out[n].ident, e->ident, 16);
			out[n].uptime_s = -1;
			out[n].link_pct = -1;
			n++;
		}
	}
	return n;
}

/* Loud, but once per peer address: a mismatching node retries its ALIVE
 * every second and must not turn the log into a flood. */
static void shout_mismatch(const pc_addr *from, int their_mode,
		int their_eager, uint64_t theirs, uint64_t ours, int their_wal)
{
	int i, slot = -1;

	for (i = 0; i < PC_CL_MAXPEER; i++) {
		if (C.cfg_shouted[i].s_addr == from->in4.sin_addr.s_addr)
			return;
		if (slot < 0 && !C.cfg_shouted[i].s_addr)
			slot = i;
	}
	if (slot >= 0)
		C.cfg_shouted[slot] = from->in4.sin_addr;
	else if (!pc_logrl_ok(LRL_CFG_MISMATCH, from))
		return;                /* table full: the shared rate limit */
	/* the keepalive carries only the peer's mode and eager, so print ALL our digest inputs to
	 * compare with this line on the other node */
	{
		char cols[256];
		int i, n = pc_store_count(), off = 0;

		cols[0] = 0;
		for (i = 0; i < n && off < (int)sizeof cols - 2; i++)
			off += snprintf(cols + off, sizeof cols - (size_t)off,
				"%s%s", i ? "," : "", pc_store_name(i));
		LM_ERR("cluster: REFUSING peer %s - our full interchange "
			"config: mode=%s eager=%d wal=%s replicas=%d "
			"placement=%s collections=%s.%s\n",
			pc_addr_str(from),
			C.cfg_mode == PC_MODE_PROXY ? "proxy" :
			C.cfg_mode == PC_MODE_SPREAD ? "spread" :
			C.cfg_mode == PC_MODE_SHARD ? "shard" : "store",
			C.cfg_eager, C.cfg_wal ? "on" : "off", C.cfg_replicas,
			PC_ROUTE_ALGO, cols,
			their_mode == C.cfg_mode && their_eager == C.cfg_eager
			? "  Mode and eager MATCH, so the digests differ in the "
			  "WAL posture, replicas, placement or the collection "
			  "set - compare with this line on the refused node." : "");
	}
	LM_ERR("cluster: REFUSING peer %s - its interchange config differs "
		"from ours.  It runs mode=%s eager=%d (digest "
		"%016llx); we run mode=%s eager=%d placement=%s (digest "
		"%016llx).  A cluster is ONE mode over ONE collection set "
		"with ONE placement function - members that disagree would "
		"lose data silently, so this peer is not joined.  Fix "
		"[cluster] mode/collections to match, upgrade the peer if "
		"its build places keys differently, or run it on its own "
		"multicast group.%s%s\n",
		pc_addr_str(from),
		their_mode == PC_MODE_PROXY ? "proxy" :
		their_mode == PC_MODE_SPREAD ? "spread" :
		their_mode == PC_MODE_SHARD ? "shard" : "store",
		their_eager, (unsigned long long)theirs,
		C.cfg_mode == PC_MODE_PROXY ? "proxy" :
		C.cfg_mode == PC_MODE_SPREAD ? "spread" :
		C.cfg_mode == PC_MODE_SHARD ? "shard" : "store",
		C.cfg_eager, PC_ROUTE_ALGO,
		(unsigned long long)ours,
		C.cfg_authoritative ? "" :
		"  (neither side declares [cluster] collections - without it "
		"nothing can be checked beyond this digest)",
		/* when the peer said its posture and it is the
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

/* a warning when `replicas` exceeds the live members - the
 * reading and when to say it are clspread's, the words are here */
static struct clspread_short replicas_short = CLSPREAD_SHORT_INIT;

static void replicas_short_tick(long long now)
{
	int k = pc_cluster_replicas(), live;

	if (k <= 0)
		return;
	live = 1 + clpeers_count_live(&C.pt, now);
	switch (clspread_short_step(&replicas_short, k,
	        pc_node_state() >= PC_NST_READY, pc_shard_grace(), live)) {
	case CLSPREAD_SHORT:
		LM_WARN("cluster: replicas = %d with %d live member(s): every "
			"key has at most %d copies until the fleet grows\n",
			k, live, live);
		break;
	case CLSPREAD_MET:
		LM_NOTICE("cluster: replicas = %d met again by %d live "
			"members\n", k, live);
		break;
	}
}

int pc_cluster_replicas_short(void)
{
	return clspread_short_now(&replicas_short);
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

/* the beat's free/total figures are the CEILING's (what this node takes before refusing a write):
 * the pool's free share falls when give-back returns pages.  No arena: the pool's */
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

/* one collection's block as this node counts it, named by hash: stores and removes are the client
 * range too, or one eager write would count on every member */
static void col_fill(int i, struct clmemb_col *c)
{
	pcache_ht_totals_t t, ct;

	pcache_ht_totals(pc_store_ht(i), &t);
	pcache_ht_totals_client(pc_store_ht(i), &ct);
	c->hash = clmemb_col_hash(pc_store_name(i));
	c->entries = t.entries;
	c->hits = ct.hits;
	c->misses = ct.misses;
	c->stores = ct.stores;
	c->removes = ct.removes;
	c->expired = t.expired;
}

/* a beat carries a window of a peer's collections - each kept by
 * hash until it has gone unsaid for COLS_STALE_MS */
#define COLS_STALE_MS 5000
static void cols_merge(struct peer *p, const struct clmemb_col *c, int n,
		long long now)
{
	int i, j;

	for (i = 0; i < n; i++) {
		for (j = 0; j < p->ncols && p->cols[j].hash != c[i].hash; j++)
			;
		if (j == p->ncols) {
			if (p->ncols >= CLMEMB_COLS_MAX)
				continue;
			p->ncols++;
		}
		p->cols[j] = c[i];
		p->cols_ms[j] = now;
	}
	for (j = 0; j < p->ncols; )
		if (now - p->cols_ms[j] > COLS_STALE_MS) {
			p->ncols--;
			p->cols[j] = p->cols[p->ncols];
			p->cols_ms[j] = p->cols_ms[p->ncols];
		} else
			j++;
}

static void self_cols(struct clmemb_col *cols, int *ncols, int *ntotal)
{
	int i, n = 0, tot = 0;

	for (i = 0; i < pc_store_count(); i++) {
		if (!pc_store_live(i))
			continue;
		tot++;
		if (n >= CLMEMB_COLS_MAX)
			continue;
		col_fill(i, &cols[n]);
		n++;
	}
	*ncols = n;
	*ntotal = tot;
}

/* one member's counters for a collection, replaced by its last five minutes, keyed by member and
 * collection (winctr.h); 0 when any has no baseline yet */
static int col_win(struct clmemb_col *c, const char *who, int from_zero)
{
	static const char *const nm[5] = { "hits", "misses", "stores",
		"removes", "expired" };
	uint64_t *f[5] = { &c->hits, &c->misses, &c->stores, &c->removes,
		&c->expired };
	char k[96];
	int i, ok = 1;

	for (i = 0; i < 5; i++) {
		long long d;

		snprintf(k, sizeof k, "fleet.%s.%016llx.%s", who,
			(unsigned long long)c->hash, nm[i]);
		d = pc_win_delta(k, *f[i], from_zero, NULL);
		if (d < 0) {
			ok = 0;
			d = 0;
		}
		*f[i] = (uint64_t)d;
	}
	return ok;
}

int pc_cluster_col_fleet(int store, int basis, struct pc_col_fleet *out)
{
	struct clmemb_col own, wown;
	struct clfleet a, aw;
	struct pc_col_fleet wf;
	int wok;
	long long now = now_ms();
	int i;

	memset(out, 0, sizeof *out);
	if (!C.enabled)
		return 0;
	col_fill(store, &own);
	clfleet_begin(&a, basis, &own);        /* the rules are clfleet's */
	wown = own;
	wok = col_win(&wown, "self", 1);
	clfleet_begin(&aw, basis, &wown);
	for (i = 0; i < C.pt.n_peers; i++) {
		struct peer *p = &C.pt.peers[i];
		const struct clmemb_col *c;
		struct clmemb_col snap;

		if (!__atomic_load_n(&p->node, __ATOMIC_ACQUIRE) ||
		    !clpeers_live(p, now))
			continue;
		c = clfleet_find(p->cols, p->cols_reported ? p->ncols : 0,
			own.hash);
		if (c)
			snap = *c;             /* the peer thread may rewrite it */
		clfleet_member(&a, c ? &snap : NULL);
		if (c) {
			char who[16];

			snprintf(who, sizeof who, "%d", p->node);
			if (!col_win(&snap, who, 0))
				wok = 0;
		}
		clfleet_member(&aw, c ? &snap : NULL);
	}
	clfleet_end(&a, pc_cluster_replicas(), out);
	clfleet_end(&aw, pc_cluster_replicas(), &wf);
	out->w_ok = wok;
	out->w_expired = wf.expired;
	out->w_hits = wf.hits;
	out->w_misses = wf.misses;
	out->w_stores = wf.stores;
	out->w_removes = wf.removes;
	return 1;
}

static unsigned int self_entries_bias;  /* PERFCACHED_TEST_ENTRIES_BIAS: a peer ahead for good */

static unsigned int self_entries(void)
{
	unsigned long long n = 0;
	int i;

	for (i = 0; i < pc_store_count(); i++) {
		if (!pc_store_live(i))
			continue;   /* a dropped collection */
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

/* live-data PER-MILLE of the arena (the leveling metric), on KB: percent-on-MB integer floors
 * swallow a small node's whole imbalance */
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

/* the member named to take over (master_prefer), 0 = none; held by this
 * master until it hands over, gives up, or the node goes */
static int prefer_id;
static long long prefer_ms;

static void prefer_clear(void)
{
	__atomic_store_n(&prefer_id, 0, __ATOMIC_RELAXED);
}

const char *pc_cluster_master_prefer(int node)
{
	struct peer *p;

	if (!C.enabled)
		return "this node is not clustered";
	if (C.role != PC_ROLE_MASTER)
		return "ask the master - only it hands mastership over";
	if (node && (node == C.node_id || !(p = peer_by_node(node)) ||
	        p->nstate == PC_NST_STALLED))
		return "not a live, unstalled member other than the master";
	prefer_ms = now_ms();
	__atomic_store_n(&prefer_id, node, __ATOMIC_RELEASE);
	if (node)
		LM_NOTICE("cluster: the operator hands mastership to node %d\n", node);
	else
		LM_NOTICE("cluster: the operator cancels the handover\n");
	return NULL;
}

/* ---- completion queues (clpend) ---------------------------------------- */

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

/* the ONLY release of a parked slot, so occupancy is exact; idempotent.  The clpend lock
 * must be held */
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

int pc_cluster_pend_free(void)
{
	int f = C.pd.cap - clpend_used(&C.pd);

	return f > 0 ? f : 0;
}

/* see cluster.h */
static __thread long long pend_cap_ms;

void pc_pull_cap_deadline(long long at_ms)
{
	pend_cap_ms = at_ms;
}

long long pc_pull_deadline_ms(const char *col, size_t collen)
{
	return now_ms() + C.pull_timeout_ms +
		pc_upstream_pull_extra_ms(col, collen);
}

long long pc_cluster_now_ms(void)
{
	return now_ms();
}

void pc_pull_cancel(uint32_t req)
{
	int worker, kind;

	(void)clpend_retire(&C.pd, req, 0, &worker, &kind);
}

static uint32_t pend_alloc(int kind, int expect, int extra_ms,
		struct pending **out)
{
	uint32_t req;
	int reason;
	long long now = now_ms();

	if (pend_cap_ms) {                 /* not past the mget's deadline */
		long long room = pend_cap_ms - now - C.pull_timeout_ms;

		if (extra_ms > room)
			extra_ms = (int)room;
	}
	req = clpend_alloc(&C.pd, kind, expect, extra_ms, pc_worker_id(),
		now, C.pull_timeout_ms, out, &reason);
	if (!req) {
		unsigned long long n = clpend_exhausted(&C.pd);

		/* once, then at most every 10 s with the running total: it fires under the
		 * very load that caused it */
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
	return clpull_req_frame(msg, req, C.node_id, col, collen, key, klen);
}

/* ask ONE designated holder whether it HAS @key, over the ordinary pull with its own completion
 * kind; only a positive counts - silence and "no" are NOT-KNOWN and never delete */
static uint32_t possess_probe(const char *col, size_t collen,
		const char *key, size_t klen, int holder_node);

/* ---- a non-holder's copy goes when the holder acks its group (keys ride with the records):
 * only a full ack drops; an unapplied record or a missing ack leaves the group's copies */
#define SPREAD_ACK_WAIT_MS 2000

static void spread_ack_drop(struct clpush_ack *a, unsigned int stored)
{
	extern void pc_wal_del(const char *, const char *, int);
	const unsigned char *r = a->nh, *end = a->nh + a->nhlen;
	pcache_htable_t *ht = NULL, *f = NULL, *o = NULL;
	const unsigned char *htcol = NULL;
	unsigned int htcollen = 0, dropped = 0;

	if (stored != a->count) {
		C.px.spread_ack_kept += a->nhcount;
		C.px.spread_ack_short += a->nhcount;
		return;
	}
	while (r + 11 <= end) {
		unsigned int collen = r[0], klen = (unsigned int)r[1] << 8 | r[2];
		unsigned long long ver = 0;
		const unsigned char *col = r + 11, *key = col + collen;
		char colz[256];
		str k;
		int b, live;

		if (key + klen > end)
			break;
		for (b = 0; b < 8; b++)
			ver = ver << 8 | r[3 + b];
		r = key + klen;
		/* a group is nearly always one collection: resolve it once */
		if (!htcol || collen != htcollen ||
		        memcmp(col, htcol, collen) != 0) {
			ht = pc_store_find((const char *)col, collen);
			f = ht ? pc_store_del_order(ht, &o) : NULL;
			htcol = col;
			htcollen = collen;
		}
		if (!ht || !ver)
			continue;
		k.s = (char *)key;
		k.len = (int)klen;
		/* version-guarded under the bucket lock, and from the table
		 * a resize is filling too */
		if (f != ht)                       /* the copier's source first */
			(void)pcache_ht_remove_below(f, &k, ver + 1);
		live = pcache_ht_remove_below(ht, &k, ver + 1) == 1;
		if (f == ht && o)
			(void)pcache_ht_remove_below(o, &k, ver + 1);
		if (live) {
			memcpy(colz, col, collen);
			colz[collen] = 0;
			pc_wal_del(colz, (const char *)key, (int)klen);
			dropped++;
		}
	}
	C.px.spread_ack_dropped += dropped;
}

static uint32_t possess_probe(const char *col, size_t collen,
		const char *key, size_t klen, int holder_node)
{
	unsigned char msg[16 + 256 + 4096];
	struct pending *slot = NULL;
	struct peer *pr = peer_by_node(holder_node);
	uint32_t req;
	size_t n;

	if (!C.enabled || !pr || collen > 255 || klen > 4096)
		return 0;
	req = pend_alloc(PC_DONE_POSSESS, 1, 0, &slot);
	if (!req)
		return 0;
	if (collen >= sizeof slot->col || klen >= sizeof slot->key) {
		/* the answer has to carry the key back or we could not tell
		 * WHICH record was confirmed - refuse rather than guess */
		clpend_release(&C.pd, slot);
		return 0;
	}
	memcpy(slot->col, col, collen);
	slot->collen = (unsigned char)collen;
	memcpy(slot->key, key, klen);
	slot->klen = (unsigned short)klen;
	n = build_pull_req(msg, req, col, collen, key, klen);
	seal_send(&pr->addr, msg, n);
	__atomic_fetch_add(&C.st.pull_sent, 1, __ATOMIC_RELAXED);
	return req;
}

/* the live holders of @slot, in peer order: the first (@after 0) or the one after @after; 0 = none
 * left, -1 = @after is no holder now */
static int spread_next_holder(unsigned int slot, int k, int after)
{
	long long now = now_ms();
	int i, seen = !after;

	for (i = 0; i < C.pt.n_peers; i++) {
		struct peer *p = &C.pt.peers[i];
		int id = __atomic_load_n(&p->node, __ATOMIC_ACQUIRE);

		if (!id || id == C.node_id || !clpeers_live(p, now) ||
		        !pc_spread_in_set(slot, id, k))
			continue;
		if (seen)
			return id;
		if (id == after)
			seen = 1;
	}
	return seen ? 0 : -1;
}

/* ---- forwarded writes (proxy mode: the holder serializes) --------------- */

static size_t build_fwd_op(unsigned char *msg, uint32_t req, int op,
		unsigned int ttl_rel, long long delta, const char *col,
		size_t collen, const char *key, size_t klen, const char *val,
		size_t vlen)
{
	return clfwd_op_frame(msg, req, C.node_id, op, ttl_rel, delta, col,
		collen, key, klen, val, vlen);
}

/* ---- replicate on the write ---------------------------------------------- */

/* every write in an eager collection goes to every live peer NOW, fire-and-forget (req 0, stored
 * PASSIVE, unacked); no TTL filter; the sweep is repair, not delivery; own records only */
/* the push group is per THREAD per peer: appended without a lock, sent by its thread when full or
 * aged, flushed on the cluster loop's RPC - a shared per-peer group serialised the workers */

/* runs ON the thread whose groups these are (an RPC, or the thread
 * itself): any of its groups older than REPL_FLUSH_MS goes out now */
static void repl_flush_mine(long long now)
{
	clpush_flush_mine(&C.wp, pc_worker_id(), now, cl_wgroup_send_cb, NULL);
	clrpt_flush();                     /* what this thread queued by TCP */
}

static void repl_flush_rpc(int sender, void *arg)
{
	(void)sender; (void)arg;
	repl_flush_mine(now_ms());
}

/* the cluster loop's timer: the groups are their threads', so it asks every thread to flush its
 * own, at most once per REPL_FLUSH_MS - a lone write waits no longer */
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

/* ---- tombstones --------------------------------------------------------- */


static void tomb_send_num(const char *col, size_t collen, const char *key,
		size_t klen, unsigned long long ver, const struct clfwd_tomb_num *num)
{
	unsigned char msg[CLFWD_TOMB_MAX];
	size_t n;
	int i;

	if (!C.enabled || collen > 255 || klen > 4096)
		return;
	/* the delete takes a fresh tick, newer than every write here: the push batches
	 * while this goes at once, so a peer needs it to refuse an overtaken set */
	if (!ver)
		ver = pc_lamport_tick();
	neg_set_ver_num(col, collen, key, klen, C.tombstone_ms, ver, num);
	{
		struct clfwd_key fk = {
			.col = col, .collen = (unsigned int)collen,
			.key = key, .klen = (unsigned int)klen };

		n = clfwd_tomb_build_n(msg, M_TOMBSTONE, &fk, ver, 0, num);
	}
	__atomic_fetch_add(&C.st.tomb_sent, 1, __ATOMIC_RELAXED);
	if (!n)
		return;
	/* while the fleet confirms, ONCE to the group (a lost copy is a hole the member
	 * asks for); else one copy a peer: unicast, data_multicast off, or it would fragment */
	if (!cl_unicast && cl_data_mcast && num &&
	    n + HDR_LEN + CLWIRE_TAG <= (size_t)(cl_max_dgram > 0 ?
	        cl_max_dgram : 1400) &&
	    pc_cluster_acks_gate(NULL, 0, NULL) &&
	    seal_send(&C.mcast_dst, msg, n) == 0) {
		__atomic_fetch_add(&C.st.tomb_group, 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&C.st.tomb_dgrams, 1, __ATOMIC_RELAXED);
		return;
	}
	for (i = 0; i < C.pt.n_peers; i++)
		if (seal_send(&C.pt.peers[i].addr, msg, n) == 0)
			__atomic_fetch_add(&C.st.tomb_dgrams, 1,
				__ATOMIC_RELAXED);
}

/* a delete this node originates: numbered in its space (identity hash, incarnation), logged WITH
 * the number (replay rebuilds the marks), counted in its own marks, then sent */
void pc_delete_originate(const char *col, size_t collen, const char *key,
		size_t klen, unsigned long long ver, int removed)
{
	struct clfwd_tomb_num num;
	unsigned long long ws;

	if (!C.enabled || collen > 255 || klen > 4096) {
		if (removed)
			pc_wal_del(col, key, (int)klen);
		return;
	}
	if (!ver)
		ver = pc_lamport_tick();       /* see tomb_send_num */
	num.oid = fnv1a64(C.self_ident, sizeof C.self_ident);
	num.incarn = C.self_incarn;
	num.seq = __atomic_add_fetch(&C.tomb_seq, 1, __ATOMIC_RELAXED);
	ws = pc_wal_del_n(col, key, (int)klen, ver, num.oid, num.incarn,
		num.seq);
	(void)pc_ack_applied(num.oid, num.incarn, num.seq, ws);
	tomb_send_num(col, collen, key, klen, ver, &num);
}

/* ---- the peer thread ---------------------------------------------------- */


/* does @key's through copy belong HERE - owner (shard) or one of its K holders (spread)?  Eager
 * and store: no, every node fetches for itself */
static int through_here(pcache_htable_t *ht, const char *col, size_t collen,
		const char *key, size_t klen)
{
	int k;

	if (pc_store_shard_enabled(ht))
		return pc_shard_owner(col, collen, key, klen) == 0;
	k = pc_cluster_replicas();
	return k > 0 && pc_store_eager_enabled(ht) &&
		pc_spread_in_set(pc_key_slot(key, klen), 0, k);
}

static void pull_serve(const pc_addr *from, uint32_t req,
		const char *col, size_t collen, const char *key, size_t klen,
		int deferred)
{
	pcache_htable_t *ht;
	unsigned char rsp[CLPULL_RHDR + 65536 + 64];
	unsigned long long ver = 0;
	struct clpull_rsp out;
	unsigned int exp = 0;
	unsigned char rfl = 0;                 /* the record's type */
	str k, v;
	size_t rn;
	int found = 0, rc;

	v.s = NULL;
	v.len = 0;
	ht = pc_store_find(col, collen);
	if (ht) {
		k.s = (char *)key;
		k.len = (int)klen;
		/* value and version from ONE read: a second lookup could pair these bytes with a
		 * LATER write's number, and the receiver would refuse that write */
		rc = pcache_ht_fetch_full(ht, &k, &v, &exp, &rfl, &ver);
		found = rc == 0;
		/* a recovered record the fleet has not ruled on is no answer: silence, as
		 * RECOVERING gives, leaves the slot inconclusive for a holder */
		if (found && C.rec_hold && C.cfg_eager) {
			unsigned long long mv;
			unsigned int wt = 0;

			if (pcache_ht_getmeta(ht, &k, &mv, &wt) == 0 &&
			        rec_held(wt)) {
				C.st.pull_held_recovered++;
				free(v.s);
				return;
			}
		}
		if (!found && pc_upstream_allowed(col, collen, key, klen) &&
		    through_here(ht, col, collen, key, klen)) {
			/* the OWNER's (shard) or a HOLDER's (spread) fetched copy, else its
			 * fetch; any other node a pull reaches answers absent */
			pcache_ht_shadow_reads(1);
			rc = pcache_ht_fetch_full(ht, &k, &v, &exp, &rfl, &ver);
			pcache_ht_shadow_reads(0);
			found = rc == 0;
			if (!found && !deferred &&
			    pc_upstream_pull_fetch(col, collen, key, klen,
			        from->in4.sin_addr.s_addr, from->in4.sin_port, req)) {
				C.st.pull_fetching++;
				return;            /* answered when the fetch is done */
			}
		}
	}
	out.req = req;
	out.node = C.node_id;
	{
		unsigned int now = get_ticks();

		out.ttl_left = found && exp ? (exp > now ? exp - now : 1) : 0;
	}
	/* too big for the datagram plane: reported ABSENT, ttl_left as computed */
	if (found && (size_t)v.len > MAX_DGRAM - CLPULL_RHDR)
		found = 0;
	out.found = found;
	out.vlen = found ? (uint32_t)v.len : 0;
	out.ver = found ? ver : 0;
	out.flags = (unsigned char)(rfl & PCACHE_F_CARRY);
	out.val = v.s;
	rn = clpull_rsp_write(rsp, sizeof rsp, M_PULL_RSP, &out);
	free(v.s);
	seal_send(from, rsp, rn);
}

/* the pulls whose fetch is done - answered here, on the cluster
 * thread, each beat */
static void pull_answer_tick(void)
{
	uint32_t addr, req;
	uint16_t port;
	char col[PC_COL_NAME_MAX], key[4200];
	size_t collen, klen;

	while (pc_upstream_pull_answer_take(&addr, &port, &req, col, sizeof col,
	        &collen, key, sizeof key, &klen)) {
		pc_addr to;

		memset(&to, 0, sizeof to);
		pc_addr_from_v4(&to, addr, ntohs(port));
		pull_serve(&to, req, col, collen, key, klen, 1);
		C.st.pull_fetched++;
	}
}

/* the acks of forwarded ops the master decided, sent from here -
 * the only sender on the cluster socket */
static void fwd_ack_tick(void)
{
	uint32_t addr, req;
	uint16_t port;
	long long newval;
	int ok;

	while (pc_upstream_fwd_ack_take(&addr, &port, &req, &ok, &newval)) {
		pc_addr to;
		unsigned char ack[16];
		struct clfwd_ack a = { .req = req, .ok = ok, .newval = newval };

		memset(&to, 0, sizeof to);
		pc_addr_from_v4(&to, addr, ntohs(port));
		seal_send(&to, ack, clfwd_ack_build(ack, M_FWD_ACK, &a));
		C.st.fwd_through_acked++;
	}
}

static void handle_pull_req(const unsigned char *pt, size_t n,
		const pc_addr *from)
{
	struct clpull_req q;

	if (!clpull_req_parse(pt, n, &q))
		return;
	/* RECOVERING must not answer: its "absent" is AUTHORITATIVE to a reconcile, silence
	 * INCONCLUSIVE.  A cold node answers: why it is asked is the asker's slot's affair */
	if (pc_node_state() == PC_NST_RECOVERING)
		return;
	C.st.pull_served++;
	pull_serve(from, q.req, q.col, q.collen, (const char *)q.key, q.klen, 0);
}

static void send_demote(int loser_node, int winner_node, const char *col,
		unsigned int cn, const char *key, unsigned int kn,
		unsigned long long loser_ver)
{
	unsigned char msg[16 + 256 + 256];
	struct peer *pr = peer_by_node(loser_node);
	size_t n;

	if (!pr || !kn)
		return;
	{
		struct clfwd_key fk = { .col = col, .collen = cn,
			.key = key, .klen = kn };

		n = clfwd_demote_build(msg, M_DEMOTE, (unsigned int)winner_node,
			&fk, loser_ver);
	}
	seal_send(&pr->addr, msg, n);
	C.px.demotes_sent++;
}

/* a delete reaches the table a resize is filling as well as the live one, or a key the copier
 * already carried comes back at the swap */
static int store_remove(pcache_htable_t *ht, const str *k)
{
	pcache_htable_t *o, *f = pc_store_del_order(ht, &o);
	int rs = pcache_ht_remove(f, k);                /* the copier's source first */

	return (o ? pcache_ht_remove(o, k) : 0) == 1 || rs == 1;
}

/* as store_remove, with the delete's version stamped under the live
 * table's bucket lock - for a remove a tombstone follows */
static int store_remove_ver(pcache_htable_t *ht, const str *k,
		unsigned long long *ver)
{
	pcache_htable_t *o, *f = pc_store_del_order(ht, &o);
	int rc, rs = 0;

	if (f != ht)                                    /* the copier's source first */
		rs = pcache_ht_remove(f, k);
	rc = pcache_ht_remove_ver(ht, k, ver) == 1;
	if (f == ht && o)
		rs = pcache_ht_remove(o, k);
	return rc || rs == 1;
}

/* a birth race: two holders at different versions - the loser demoted in
 * favour of the winner.  From a pull's answers and a hold-check's */
static void birth_race(const struct clpend_ans_out *a)
{
	LM_WARN("cluster: birth race on a key - node %d and node %d "
		"hold DIFFERENT versions; demoting %d in favour of "
		"%d\n", a->loser, a->winner, a->loser, a->winner);
	send_demote(a->loser, a->winner, a->col, a->collen, a->key, a->klen,
		a->loser_ver);
	pc_loc_set(a->col, a->collen, a->key, a->klen, a->winner);
}

/* nobody in the fleet has this key and eager holds everything: it was DELETED while this node was
 * down and replay revived it - drop it and log the delete, here: nothing waits on it */
static void reconcile_drop(const struct clpend_ans_out *a)
{
	pcache_htable_t *ht = pc_store_find(a->col, a->collen);
	str k;

	/* a peer can restart between the probe and its answers: a "no" that may
	 * come from an empty store authorises nothing */
	if (any_live_peer_cold() || !reconcile_witness_live()) {
		C.st.reconcile_deferred++;
		C.recon.unruled++;
		return;
	}
	if (ht) {
		extern void pc_wal_del(const char *, const char *, int);
		char colz[64];

		k.s = (char *)a->key;
		k.len = (int)a->klen;
		/* written again since the probe - the write is the
		 * truth now, not the fleet's "no" about the old record */
		{
			unsigned long long v;
			unsigned int wt;

			if (pcache_ht_getmeta(ht, &k, &v, &wt) == 0 && !rec_held(wt)) {
				C.st.reconcile_rewritten++;
				return;
			}
		}
		if (store_remove(ht, &k) == 1) {
			memcpy(colz, a->col, a->collen);
			colz[a->collen] = 0;
			pc_wal_del(colz, a->key, (int)a->klen);
			C.st.reconciled++;
			LM_NOTICE("cluster: reconcile dropped %.*s/%.*s "
				"- recovered from the WAL but no node "
				"in the fleet still has it\n",
				(int)a->collen, a->col,
				(int)a->klen, a->key);
		}
	}
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
	 * dropped, never half-applied (and a length near 2^32 with it) */
	if (!clpull_rsp_whole(&r, n))
		return;
	pc_lamport_observe(ver);

	/* what the reply MEANS is clpend's, the side effects this file's; it returns unlocked with
	 * everything COPIED out of the slot, so the reconcile arm can touch the store */
	switch (clpend_answer(&C.pd, req, found, from, ver,
			/* the bytes answered, so equal versions with different values
			 * read as the race they are; never 0 when a value came */
			found && vlen ? pc_crc32c(0, r.val, vlen) | 1u : 0,
			/* only the probe transform uses it, and only a
			 * positive can transform - so a miss pays nothing */
			found ? now_ms() + C.pull_timeout_ms + 700 : 0, &a)) {
	case CLPEND_ANS_PROBE_FWD: {
		/* the responsible holder answered: the park became a
		 * forward to it, and the same req rides the datagram */
		unsigned char fmsg[32 + 256 + 4096 + MAX_FWD_VAL];
		struct peer *pr;
		int op = a.probe_op == 2 ? 2 : a.probe_op == 3 ? 3
			: a.probe_op == 4 ? 1 : 0;
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
	case CLPEND_ANS_HIT: {
		unsigned long long tv;

		/* a copy older than a remembered delete is a stale holder that missed it:
		 * stored, it would undo the delete after a heal - answered as a miss */
		if (a.collen && pc_neg_ver(a.col, a.collen, a.key, a.klen, &tv) &&
		    tv && ver <= tv) {
			__atomic_fetch_add(&C.px.recv_tombstoned, 1,
				__ATOMIC_RELAXED);
			C.st.pull_misses++;
			post_done(a.worker, req, 0, 0, NULL, 0);
			return;
		}
	}
		C.st.pull_hits++;
		/* a pull's newval carries the record's type */
		post_done_ex(a.worker, req, PC_DONE_PULL, 1, 1,
			(long long)(r.flags & PCACHE_F_CARRY), from,
			ttl_left, (const unsigned char *)r.val, (int)vlen, 0, 0, ver, 0);
		return;

	case CLPEND_ANS_SAME:
		/* two holders at the same version: the ordinary eager state -
		 * counted, never logged per key */
		C.st.pull_hits++;
		return;

	case CLPEND_ANS_RACE:
		birth_race(&a);
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

	case CLPEND_ANS_POSSESS: {
		/* every holder confirmed it has this record, so our surplus copy goes - a plain
		 * remove: a tombstone would delete the holders' copies too */
		pcache_htable_t *ht = pc_store_find(a.col, a.collen);
		str k;

		if (ht && a.klen) {
			extern void pc_wal_del(const char *, const char *, int);
			char colz[64];
			int kk = pc_cluster_replicas(), next = kk ? spread_next_holder(
				pc_key_slot(a.key, a.klen), kk, a.from_node) : 0;

			/* one holder's copy may be PASSIVE, which no sweep re-sends: ask the next
			 * holder too, and only the last one's yes drops ours */
			if (next) {
				if (next > 0 && possess_probe(a.col, a.collen, a.key, a.klen, next))
					C.px.spread_possess_sent++;
				else
					C.px.spread_possess_kept++;
				return;
			}

			k.s = a.key;
			k.len = (int)a.klen;
			/* only if the holder's copy is at least as new as ours, compared under our
			 * bucket lock (remove_below), so a newer write landing here is kept */
			/* and from the table a resize is filling: the copier may already
			 * have carried this record across */
			pcache_htable_t *o, *f = pc_store_del_order(ht, &o);
			int live;

			if (ver && f != ht)            /* the copier's source first */
				(void)pcache_ht_remove_below(f, &k, ver + 1);
			live = ver && pcache_ht_remove_below(ht, &k, ver + 1) == 1;
			if (ver && f == ht && o)
				(void)pcache_ht_remove_below(o, &k, ver + 1);
			if (live) {
				memcpy(colz, a.col, a.collen);
				colz[a.collen] = 0;
				pc_wal_del(colz, a.key, (int)a.klen);
				C.px.spread_reclaimed++;
			}
		}
		return;
	}

	case CLPEND_ANS_RECONCILE:
		reconcile_drop(&a);
		return;
	default:                       /* WAIT, and IGNORE */
		return;
	}
}

/* the holder applies a forwarded write and acks with the outcome */
static void handle_fwd_op(const unsigned char *pt, size_t n,
		const pc_addr *from)
{
	extern void pc_wal_upsert(const char *, const char *, int,
		const char *, int, unsigned int, unsigned long long);
	extern void pc_wal_del(const char *, const char *, int);
	extern int pc_op_expire_apply(const char *, size_t, const char *,
		size_t, unsigned int);
	extern int pc_op_setcond_apply(const char *, size_t, const char *,
		size_t, const char *, size_t, long long, int);
	extern int pc_op_cmp_apply(const char *, size_t, const char *, size_t,
		const char *, size_t, const char *, size_t, long long, int);
	pcache_htable_t *ht;
	unsigned char ack[16];
	uint32_t req, ttl_rel;
	long long delta, newval = 0;
	unsigned long long dver = 0;
	unsigned int cn, kn, vlen, exp = 0;
	int op, ok = 0;
	str k, v;
	char colz[256];
	const char *fkey, *fval;

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
		/* the parse already located these; recomputing them from a
		 * literal 23 is how the header length gets two spellings */
		fkey = f.key;
		fval = f.val;
		memcpy(colz, f.col, cn);
		colz[cn] = 0;
	}
	C.px.fwd_served++;

	ht = pc_store_find(colz, cn);
	if (ht) {
		int th;

		k.s = (char *)fkey;
		k.len = (int)kn;
		/* a through key this node owns: the master decides, the read-back is stored
		 * here and acked from fwd_ack_tick; -1 is a refusal (queue full), acked now */
		th = pc_upstream_fwd_through(colz, cn, op, fkey, kn, fval, vlen,
			ttl_rel, delta, from->in4.sin_addr.s_addr, from->in4.sin_port, req);
		if (th > 0) {
			C.st.fwd_through++;
			return;
		}
		if (th < 0)
			goto acked;
		if (ttl_rel && ttl_rel != PCACHE_EXP_PRESERVE)
			exp = get_ticks() + ttl_rel;
		if (op == 0) {
			v.s = (char *)fval;
			v.len = (int)vlen;
			ok = pcache_ht_store(ht, &k, &v, exp) == 0;
			if (ok) {
				pc_ev_applied(ht, &k);          /* the holder */
				pc_wal_upsert(colz, k.s, k.len, v.s, v.len, exp,
				pcache_last_ver);
				pc_neg_clear(colz, cn, k.s, (size_t)kn);
			}
		} else if (op == 1) {
			int dead;

			ok = store_remove_ver(ht, &k, &dver) == 1;
			dead = ok && pcache_last_del_dead;
			if (ok) {
				/* logged below - numbered where it is sent */
				pc_neg_set(colz, cn, k.s, kn, C.tombstone_ms);
				/* shard and proxy keep ONE copy: the remove was the
				 * delete; else this holder's clock tombstones the rest */
				if (!pc_store_shard_enabled(ht) &&
				    !pc_store_proxy_enabled(ht))
					pc_delete_originate(colz, cn, k.s, kn,
						dver, 1);
				else
					pc_wal_del(colz, k.s, k.len);
			}
			ok = ok && !dead;    /* past its deadline: removed, acked absent as a DEL */
		} else if (op == 2) {
			/* PCACHE_EXP_PRESERVE = the Redis INCR contract: keep the
			 * holder's existing expiry, WAL the effective one */
			unsigned int eff = exp;
			unsigned char tfl = 0;

			/* INCR on a document is WRONGTYPE at the holder too */
			if (pc_store_types_strict(ht) &&
			        pcache_ht_getflags(ht, &k, &tfl) == 0 &&
			        (tfl & PCACHE_F_TYPES)) {
				newval = PC_FWD_E_TYPE;
				goto acked;
			}
			ok = pcache_ht_add_ex(ht, &k, delta,
				ttl_rel == PCACHE_EXP_PRESERVE ?
				PCACHE_EXP_PRESERVE : exp, &newval, &eff) == 0;
			if (ok) {
				char nb[24];
				int nl = snprintf(nb, sizeof nb, "%lld", newval);

				pc_wal_upsert(colz, k.s, k.len, nb, nl, eff,
				pcache_last_ver);
			}
		} else if (op == 3) {
			/* a re-arm forwarded by the node that took it: the same adopt/WAL/push
			 * as a local expire, so a spread holder carries it on */
			ok = pc_op_expire_apply(colz, cn, k.s, (size_t)kn, exp) == 1;
		} else if (op == 4 || op == 5) {
			/* SET NX (4) / XX (5): this node decides; ok = decided, newval = stored.
			 * An older node acks ok 0, reported as a refusal, never as a store */
			int r = pc_op_setcond_apply(colz, cn, k.s, (size_t)kn,
				fval, vlen,
				ttl_rel == PCACHE_EXP_PRESERVE ? 0 : (long long)ttl_rel,
				op == 4 ? 1 : 2);

			ok = r >= 0;
			newval = r == 1;
		} else if (op >= 6 && op <= 9) {
			/* SET IFEQ (6) / IFNE (7), DELEX IFEQ (8) / IFNE (9): value is
			 * [cmplen u32 LE][cmp][new]; an older node acks ok 0 = "holder rejected" */
			uint32_t cl = vlen >= 4 ? (uint32_t)(unsigned char)fval[0] |
				(uint32_t)(unsigned char)fval[1] << 8 |
				(uint32_t)(unsigned char)fval[2] << 16 |
				(uint32_t)(unsigned char)fval[3] << 24 : 0;

			if (vlen >= 4 && cl <= vlen - 4) {
				int r = pc_op_cmp_apply(colz, cn, k.s, (size_t)kn,
					fval + 4, cl, fval + 4 + cl, vlen - 4 - cl,
					ttl_rel == PCACHE_EXP_PRESERVE ? 0 :
					(long long)ttl_rel, op - 5);

				if (r == -11) {        /* PC_OP_ERR_TYPE */
					newval = PC_FWD_E_TYPE;
				} else {
					ok = r >= 0;
					newval = r == 1;
				}
			}
		}
	}
acked:
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
	/* only_unanswered: a probe turned forward completes on the holder's ack, so a
	 * stray second ack must not complete it twice */
	if (clpend_retire(&C.pd, req, 1, &worker, &kind))
		post_done_ex(worker, req, kind, 0, ok, newval, 0, 0, NULL, 0, 0,
			0, 0, 0);
}

/* a record this node recovered and the fleet has not ruled on - EAGER ONLY: only an eager node
 * runs the pass that clears reconcile_on, so a shard node would be silenced for ever */
static int rec_held(unsigned int wtick)
{
	return C.rec_hold && C.cfg_eager && wtick <= C.rec_tick;
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
		uint64_t ver;

		if (!clfwd_demote_parse(pt, n, &winner, &fk, &ver))
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
			/* the copy the race found and nothing newer: a later write or the winner's
			 * own replica stays; ver 0 (an older sender) removes unconditionally */
			if (ver) {
				str lv;
				unsigned int le;
				unsigned long long lver = 0;

				if (pcache_ht_fetch_ver(ht, &k, &lv, &le, &lver) == 0) {
					free(lv.s);
					if (lver > ver) {
						C.px.demotes_kept++;
						pc_loc_set(colz, cn, fk.key, kn, (int)winner);
						return;
					}
				}
			}
			if (store_remove(ht, &k) == 1)
				pc_wal_del(colz, k.s, k.len);
		}
		pc_loc_set(colz, cn, fk.key, kn, (int)winner);
	}
}

/* 1 when it removed this node's copy (the caller logs that to the WAL);
 * 0 for a numbered one, which is logged here */
static int handle_tombstone(const unsigned char *pt, size_t n)
{
	pcache_htable_t *ht;
	unsigned int cn, kn;
	str k;

	{
		struct clfwd_key fk;
		struct clfwd_tomb_num num;
		uint64_t ver;
		unsigned int tf;
		int removed = 0;

		if (!clfwd_tomb_parse_n(pt, n, &fk, &ver, &tf, &num))
			return 0;
		cn = fk.collen;
		kn = fk.klen;
		C.st.tomb_applied++;
		/* keep the delete's version with the negative entry, so the set it
		 * superseded is refused when its batch lands later; 0 is an older sender */
		if (ver)
			pc_lamport_observe(ver);
		if (tf & CLFWD_TOMB_F_EXPIRY)
			neg_set_ver_expiry(fk.col, cn, fk.key, kn, C.tombstone_ms,
				ver, (tf & CLFWD_TOMB_F_KEEP) != 0);
		else if (num.seq)
			neg_set_ver_num(fk.col, cn, fk.key, kn, C.tombstone_ms,
				ver, &num);
		else
			pc_neg_set_ver(fk.col, cn, fk.key, kn, C.tombstone_ms, ver);
		ht = pc_store_find(fk.col, cn);
		if (ht) {
			k.s = (char *)fk.key;
			k.len = (int)kn;
			pcache_htable_t *o, *f = pc_store_del_order(ht, &o);

			removed = clorder_del(f, o, &k, ver);
		}
		/* a numbered delete is logged with its number and counted whether or not a
		 * copy went: a newer write or no copy is still this node holding it */
		if (num.seq) {
			char colz[256];
			unsigned long long ws;

			memcpy(colz, fk.col, cn);
			colz[cn] = 0;
			ws = pc_wal_del_n(colz, fk.key, (int)kn, ver, num.oid,
				num.incarn, num.seq);
			(void)pc_ack_applied(num.oid, num.incarn, num.seq, ws);
			return 0;
		}
		return removed;
	}
}

/* the pending table's timeouts, every PEND_SWEEP_MS while a request waits (the loop's poll is
 * capped to it), so a pull gives up close to pull_timeout_ms; skipped while nothing waits */
#define PEND_SWEEP_MS 25

static void expire_pending(void)
{
	C.st.pull_timeouts += clpend_expire(&C.pd, now_ms());
	C.recon.unruled += C.pd.reconcile_expired;
	C.pd.reconcile_expired = 0;
}

/* ---- automatic membership: the peer thread ONLY ------------------------- */

/* (ip,port): the election's total order over nodes; pc_addr_cmp keeps IPv4's order exactly */
static int addr_cmp(const pc_addr *a, const pc_addr *b)
{
	return pc_addr_cmp(a, b);
}

static int addr_eq(const pc_addr *a, const pc_addr *b)
{
	return pc_addr_eq(a, b);
}

static void ident_hex(const unsigned char *id, char *out);

/* tell this node's OWN clients a member joined or left; "members" lets a client check its next
 * member list.  A ptree map in a NOTIFY frame, keys in the JSON line's order. */
static void clients_tell(const char *event, const pc_addr *addr, int port, int node,
		const unsigned char *ident, int state)
{
	unsigned char buf[256];
	char hx[33];
	struct pc_tw w;
	int members = clpeers_count_live(&C.pt, now_ms()) + 1;

	if (ident)
		ident_hex(ident, hx);
	else
		hx[0] = 0;
	pc_tw_init(&w, buf, sizeof buf);
	pc_tw_map(&w);
	pc_tw_key(&w, "notify");
	pc_tw_str(&w, "membership");
	pc_tw_key(&w, "event");
	pc_tw_str(&w, event);
	pc_tw_key(&w, "addr");
	pc_tw_str(&w, pc_addr_str(addr));
	pc_tw_key(&w, "port");
	pc_tw_i64(&w, port);
	pc_tw_key(&w, "node");
	pc_tw_i64(&w, node);
	pc_tw_key(&w, "identity");
	pc_tw_str(&w, hx);
	pc_tw_key(&w, "members");
	pc_tw_i64(&w, members);
	pc_tw_key(&w, "state");
	pc_tw_str(&w, pc_node_state_name(state));
	pc_tw_end(&w);
	if (pc_tw_done(&w) == 0) {
		pc_clients_notify((const char *)w.b, w.n);
		LM_INFO("cluster: telling this node's clients that node %d %s "
			"(%s:%d, %d member(s) now)\n", node, event,
			pc_addr_str(addr), port, members);
	}
}

static void clients_membership(const char *event, struct peer *p, int node)
{
	clients_tell(event, &p->addr, p->client_port, node,
		p->has_ident ? p->ident : NULL, p->nstate);
}

/* a client linked to this node hears no peer report this node's state: tell it ourselves,
 * each beat, rather than leave it to its refresh */
static void self_state_tick(void)
{
	int st = pc_node_state();

	if (st == C.told_self_state || !C.cfg_client_port || !C.node_id)
		return;
	C.told_self_state = st;
	clients_tell("state", &C.self_addr, C.cfg_client_port, C.node_id, C.self_ident, st);
}

/* a member left this node's view (purged, goodbye or forgotten): reserve its id, tell the
 * clients, clear the slot */
static void peer_gone(struct peer *p, int why)
{
	long long now = now_ms();
	int silent_s = (int)((now - p->last_seen_ms) / 1000);

	/* a departed peer's forced lane goes: whatever returns at that address starts on UDP,
	 * and the detector forces it again if UDP really is blocked */
	if (clane_forced(&p->addr))
		clane_force(&p->addr, 0);
	if (p->node && p->node == __atomic_load_n(&prefer_id, __ATOMIC_RELAXED))
		prefer_clear();                /* it left before the handover */

	if (p->clients_told) {
		p->clients_told = 0;
		clients_membership("expelled", p, p->told_node);
	}
	/* logged here, not in clres, and before the store below clears p->node */
	if (clres_note(&reserved, p->node, p->addr.in4.sin_addr, p->ident,
			p->has_ident, now, RESERVE_MS)) {
		clres_why(&reserved, p->node, why, silent_s);
		if (why == CLRES_GONE_SILENT)
			LM_WARN("cluster: node %d (%s) went silent %d s ago and "
				"was purged - its id is reserved for it for "
				"%lld s\n", p->node, pc_addr_str(&p->addr),
				silent_s, RESERVE_MS / 1000);
		else if (why == CLRES_GONE_FORGOTTEN)
			LM_NOTICE("cluster: node %d (%s) was dropped (refused) - "
				"its id is reserved for it for %lld s\n", p->node,
				pc_addr_str(&p->addr), RESERVE_MS / 1000);
		else
			LM_NOTICE("cluster: node %d (%s) said goodbye - its id is "
				"reserved for it for %lld s\n", p->node,
				pc_addr_str(&p->addr), RESERVE_MS / 1000);
	}
	/* stamp BEFORE clearing the id - the spread grace reads this
	 * to keep the departed node's placement slots for a while longer */
	p->gone_ms = now_ms();
	clps_forget(p);                        /* the slot may be reused */
	__atomic_store_n(&p->node, 0, __ATOMIC_RELEASE);
	shard_note_change();
}

/* drop a peer we refuse to federate with (config mismatch): like GOODBYE, the slot stays and
 * node goes to 0, which every read path treats as "not a member" */
static void peer_forget(const pc_addr *from)
{
	struct peer *p = clpeers_by_addr(&C.pt, from);

	if (p && __atomic_load_n(&p->node, __ATOMIC_ACQUIRE))
		peer_gone(p, CLRES_GONE_FORGOTTEN);
}

struct peer *peer_upsert(const pc_addr *addr)
{
	struct peer *p;
	int created;

	p = clpeers_upsert(&C.pt, addr, &created);
	if (created)
		shard_note_change();
	return p;
}

/* ---- node identity (persisted) + incarnation (per process start) -------
 * Same identity, NEW incarnation = restarted empty (yet given its old id back): resend all */

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

/* the proposed id is derived from the identity, so stable across starts; only a proposal -
 * 1023 slots collide early (birthday bound), so the master still arbitrates */
#define ident_proposed_id clinit_proposed_id

/* the identity file is text: a version line, the 32-hex identity and an FNV-1a check of the 16
 * bytes (a torn-write/typo detector; 0600 is the guard).  Legacy raw bytes are rewritten. */
#define IDENT_FILE   "node-identity"
#define IDENT_HEADER "perfcached-node-identity 1"

#define fnv1a64 clinit_fnv1a64

/* @n bytes from 2n hex characters; -1 on anything that is not hex */
#define hex2bin clinit_hex2bin

/* temp file, fsync, rename, directory fsync (statefile.c; 0600 - it names this node to its
 * peers): a crash never leaves an empty or torn file.  0 written, -1 failed with errno set */
static int ident_store(const char *dir, const unsigned char *id)
{
	char hex[33], buf[128];
	int n;

	ident_hex(id, hex);
	n = snprintf(buf, sizeof buf,
		IDENT_HEADER "\nidentity %s\ncheck %016llx\n", hex,
		(unsigned long long)fnv1a64(id, 16));
	return pc_state_write(dir, IDENT_FILE, buf, (size_t)n);
}

/* 1 = text form valid, 0 = legacy 16 raw bytes (@id filled either way), -1 = invalid (@why).
 * Strict: three lines exactly - an edited file (a fourth line, a CRLF) is refused, not guessed */
static int ident_parse(const unsigned char *buf, size_t len,
		unsigned char *id, char *why, size_t wlen)
{
	return clinit_ident_parse(buf, len, id, why, wlen);
}

/* carry state file @name (a few lines) from @from_dir (a WAL dir that served as state_dir) into
 * @to_dir if absent, through pc_state_write.  1 carried, 0 nothing to do, -1 failed (logged) */
static int carry_file(const char *from_dir, const char *to_dir,
		const char *name)
{
	char src[512], dst[512], buf[4096];
	int in;
	ssize_t n = 0;
	size_t len = 0;

	if (!from_dir || !to_dir || !*from_dir || !*to_dir ||
	        strcmp(from_dir, to_dir) == 0)
		return 0;
	snprintf(dst, sizeof dst, "%s/%s", to_dir, name);
	if (access(dst, F_OK) == 0)
		return 0;
	snprintf(src, sizeof src, "%s/%s", from_dir, name);
	in = open(src, O_RDONLY);
	if (in < 0)
		return 0;
	while (len < sizeof buf && (n = read(in, buf + len, sizeof buf - len)) > 0)
		len += (size_t)n;
	if (len < sizeof buf && n < 0) {
		LM_ERR("cannot carry %s into %s: %s\n", name, to_dir,
			strerror(errno));
		close(in);
		return -1;
	}
	n = len == sizeof buf ? read(in, buf, 1) : 0;
	close(in);
	if (n != 0) {
		LM_ERR("cannot carry %s into %s: larger than %zu bytes, not a "
			"state file\n", name, to_dir, sizeof buf);
		return -1;
	}
	if (pc_state_write(to_dir, name, buf, len) != 0) {
		LM_ERR("cannot carry %s into %s: %s\n", name, to_dir,
			strerror(errno));
		return -1;
	}
	LM_NOTICE("carried %s from %s into state_dir %s\n", name, from_dir,
		to_dir);
	return 1;
}

/* auto's salt when no site_salt is set: 32 random bytes minted once per instance (node-salt); a
 * cloned disk copies it but not the platform, so derives anew.  0 ok, -1 damaged, -2 unkeepable */
#define SALT_FILE "node-salt"
static int node_salt(const char *state_dir, const char *legacy_dir,
		unsigned char key[32])
{
	char path[600], buf[160], hex[65];
	size_t bl = 0;
	ssize_t n;
	int fd;

	if (legacy_dir)
		(void)carry_file(legacy_dir, state_dir, SALT_FILE);
	snprintf(path, sizeof path, "%s/" SALT_FILE, state_dir);
	fd = open(path, O_RDONLY);
	if (fd >= 0) {
		n = read(fd, buf, sizeof buf - 1);
		close(fd);
		if (n < 64 || sodium_hex2bin(key, 32, buf, 64, NULL, &bl,
		        NULL) != 0 || bl != 32) {
			LM_CRIT("cluster: %s does not validate - refusing to "
				"start.  Restore it, or delete it AND %s/"
				IDENT_FILE " to derive a NEW identity (the node "
				"then presents as a new member)\n", path,
				state_dir);
			return -1;
		}
		return 0;
	}
	if (errno != ENOENT) {
		LM_CRIT("cluster: cannot open %s: %s - refusing to start\n",
			path, strerror(errno));
		return -1;
	}
	randombytes_buf(key, 32);
	sodium_bin2hex(hex, sizeof hex, key, 32);
	hex[64] = '\n';
	if (pc_state_write(state_dir, SALT_FILE, hex, 65) != 0) {
		LM_ERR("cluster: cannot write %s: %s\n", path, strerror(errno));
		sodium_memzero(hex, sizeof hex);
		return -2;
	}
	sodium_memzero(hex, sizeof hex);
	return 0;
}

/* is @name in the state directory, counting one carried from the legacy WAL directory */
static int state_has(const char *state_dir, const char *legacy_dir,
		const char *name)
{
	char path[600];

	if (legacy_dir)
		(void)carry_file(legacy_dir, state_dir, name);
	snprintf(path, sizeof path, "%s/%s", state_dir, name);
	return access(path, F_OK) == 0;
}

/* identity_init: 0, or -1 when the persisted identity does not validate - refuse to start,
 * never mint a fresh one over a damaged file */
void pc_cluster_set_site_salt(const char *salt)
{
	C.cfg_site_salt = salt;            /* the config owns the storage */
}

static int identity_init(const char *state_dir, const char *legacy_dir)
{
	char path[512], why[96];
	unsigned char buf[256];
	unsigned char plat[16];
	int fd, got = 0, form = -1, derived = 0;
	int soft = 0, plat_src = CLPLAT_NONE;
	int keep_src = CLPLAT_NONE;                   /* kept, not derived */
	ssize_t n;

	C.ident_durable = 0;
	/* platform first: a disk clone copies the file, but a hypervisor regenerates the UUID (VM)
	 * or MAC (LXC), so a clone derives a NEW identity; the salt keeps two fleets apart */
	/* a named identity_source refuses to start without it: env = $PERFCACHED_PLATFORM_KEY (a
	 * rescheduled pod is a new member), file = the identity file alone, auto = the cascade */
	if (cl_id_source == PC_IDSRC_DMI || cl_id_source == PC_IDSRC_MAC ||
	        cl_id_source == PC_IDSRC_ENV) {
		char pkey[CLPLAT_KEY_MAX];
		const char *pwhy = "";
		int want = cl_id_source == PC_IDSRC_DMI ? CLPLAT_DMI :
			cl_id_source == PC_IDSRC_MAC ? CLPLAT_MAC : CLPLAT_ENV;
		const char *salt = C.cfg_site_salt ? C.cfg_site_salt : "";

		if (want == CLPLAT_DMI) {
			const char *ctr;

			if (clplat_in_container(&ctr))
				LM_WARN("cluster: identity_source = dmi inside a "
					"container (%s) - its DMI uuid is the HOST's, so "
					"every container on that host derives the same "
					"identity\n", ctr);
		}
		if (clplat_probe_one(want, pkey, sizeof pkey, &pwhy) == CLPLAT_NONE) {
			LM_CRIT("cluster: identity_source = %s, but %s - refusing to "
				"start rather than fall back to an identity a clone "
				"would share\n", clplat_src_name(want), pwhy);
			return -1;
		}
		clplat_derive((const unsigned char *)salt, strlen(salt), pkey,
			plat);
		derived = 1;
		LM_NOTICE("cluster: identity derived from %s (identity_source)\n",
			clplat_src_name(want));
	} else if (cl_id_source == PC_IDSRC_AUTO &&
	        ((C.cfg_site_salt && *C.cfg_site_salt) ||
	         (state_dir && *state_dir))) {
		/* auto derives from the platform by default: without a site_salt the node salt is
		 * the key; with no state directory there is nothing to derive for */
		char pkey[CLPLAT_KEY_MAX];
		const char *pwhy = "";
		int src = clplat_probe(pkey, sizeof pkey, &pwhy);

		if (src != CLPLAT_NONE) {
			/* held aside: the file load below writes C.self_ident, and the derived
			 * value must survive it to override it */
			if (C.cfg_site_salt && *C.cfg_site_salt) {
				clplat_derive((const unsigned char *)C.cfg_site_salt,
					strlen(C.cfg_site_salt), pkey, plat);
				derived = 1;
				plat_src = src;
			} else if (!state_has(state_dir, legacy_dir, SALT_FILE) &&
			        state_has(state_dir, legacy_dir, IDENT_FILE)) {
				/* no node salt beside an identity on disk = it predates derivation:
				 * kept (no new member on upgrade), and no salt is minted for it */
				keep_src = src;
			} else {
				unsigned char dk[32];
				int sr = node_salt(state_dir, legacy_dir, dk);

				if (sr == -1)
					return -1;
				if (sr == 0) {
					clplat_derive(dk, sizeof dk, pkey, plat);
					soft = 1;
					derived = 1;
					plat_src = src;
				} else {
					LM_WARN("cluster: no node salt could be kept "
						"in %s - the identity is not derived "
						"this start\n", state_dir);
				}
				sodium_memzero(dk, sizeof dk);
			}
		} else {
			LM_WARN("cluster: no platform id (%s) - falling back to "
				"the identity file; a CLONE of this machine "
				"would carry its identity\n", pwhy);
		}
	}
	if (state_dir && *state_dir) {
		snprintf(path, sizeof path, "%s/" IDENT_FILE, state_dir);
		/* a WAL-directory identity is carried once into a new state_dir */
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
		if (keep_src != CLPLAT_NONE && got) {
			/* the identity kept above: say what it costs and how to switch */
			char was[33];

			ident_hex(C.self_ident, was);
			LM_WARN("cluster: identity %s was not derived from this "
				"machine (minted at random, or under a salt this node "
				"no longer has) - a CLONE of this disk would carry "
				"it.  This machine offers %s: set [cluster] "
				"identity_source = %s to derive the identity from it "
				"(the node then joins as a NEW member)\n", was,
				clplat_src_name(keep_src),
				keep_src == CLPLAT_DMI ? "dmi" : "mac");
		}
		if (derived) {
			if (plat_src != CLPLAT_NONE)   /* auto's; a named source said so */
				LM_NOTICE("cluster: identity derived from the "
					"platform (%s%s)\n", clplat_src_name(plat_src),
					soft ? ", with this node's salt" : "");
			/* the platform is the authority, the file a cache: compare BEFORE
			 * overwriting so a mismatch (a clone) is reported */
			if (got && memcmp(C.self_ident, plat, 16) != 0) {
				char was[33];

				ident_hex(C.self_ident, was);
				LM_WARN("cluster: the stored identity %s does "
					"not match the one derived from this "
					"platform - a clone, or a moved image.  "
					"Taking the derived one\n", was);
			}
			memcpy(C.self_ident, plat, 16);
			(void)ident_store(state_dir, C.self_ident);
			got = 1;
		}
		if (!got) {
			/* v7: the timestamp says WHEN, so two nodes claiming
			 * one identity can be told apart by inspection */
			clplat_random_v7(C.self_ident, (uint64_t)now_ms());
			if (ident_store(state_dir, C.self_ident) == 0)
				got = 1;
			else
				LM_ERR("cluster: cannot write %s: %s - this "
					"node's identity is EPHEMERAL\n", path,
					strerror(errno));
		}
		C.ident_durable = got;
	}
	if (derived && !(state_dir && *state_dir)) {
		/* derived with nowhere to cache it: the derived value is the identity */
		memcpy(C.self_ident, plat, 16);
		if (plat_src != CLPLAT_NONE)
			LM_NOTICE("cluster: identity derived from the platform "
				"(%s)\n", clplat_src_name(plat_src));
	}
	if (!C.ident_durable && !derived) {
		/* nothing to persist to: a fresh identity each start is correct - with no
		 * durable store the node has nothing to bring back, so it is a new member */
		clplat_random_v7(C.self_ident, (uint64_t)now_ms());
	}
	do {
		randombytes_buf(&C.self_incarn, sizeof C.self_incarn);
	} while (!C.self_incarn);           /* 0 means "not reported" */
	/* a receiver tells this process's relay sequence from the last one's by it */
	clps_new_epoch();
	ident_hex(C.self_ident, C.self_ident_hex);
	C.proposed_id = ident_proposed_id(C.self_ident);
	LM_NOTICE("cluster: identity %s uuid-v%d (%s), incarnation %u, "
		"proposed node id %d\n", C.self_ident_hex,
		clplat_uuid_version(C.self_ident),
		C.ident_durable ? "persisted" : "EPHEMERAL - no state dir, "
		"this node presents as new after a restart",
		C.self_incarn, C.proposed_id);
	return 0;
}

/* this node's own identity, canonical, and the version that says where
 * it came from - see clplat.h */
void pc_cluster_identity_uuid(char out[37], int *ver)
{
	clplat_uuid_str(C.self_ident, out);
	if (ver)
		*ver = clplat_uuid_version(C.self_ident);
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

/* for paths that learn a peer other than by its own ALIVE (ASSIGN list, MASTER_ALIVE): no
 * identity, so node-id behaviour holds until its ALIVE brings the real one */
static const unsigned char pd_noident[16];

/* a returning member may hold copies of keys remembered at another holder (a fork); a fork heals
 * only when one lookup gets two answers, and a remembered holder is asked alone: forget them */
static void holders_forget(int node, const char *why)
{
	unsigned long n = clloc_forget_holders(&C.lc);

	if (n)
		LM_NOTICE("cluster: node %d %s - forgot %lu remembered "
			"holder(s): the next read of each asks every member\n",
			node, why, n);
}

void peer_publish(struct peer *p, int node, unsigned int free_mb,
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
	/* a silence over 3 s may have swallowed a delete broadcast: note where it began for the
	 * catch-up replay - here, as a short cut can fall between two sweep ticks */
	{
		long long nowh = now_ms();

		if (p->last_seen_ms && nowh - p->last_seen_ms > 3000) {
			unsigned int from = (unsigned int)(p->last_seen_ms / 1000);

			holders_forget(node, "is back from a silence");

			from = from > 2 ? from - 2 : 0;
			if (!p->tomb_gap_from || from < p->tomb_gap_from)
				p->tomb_gap_from = from ? from : 1;
			/* under spread a silence is a repair event: pushes during it are lost,
			 * and a cut inside the slot grace never changes the holder set */
			if (pc_cluster_replicas()) {
				memset(p->setrepair, 1, sizeof p->setrepair);
				memset(p->repl_cursor, 0, sizeof p->repl_cursor);
				C.px.spread_gap_repaired++;
				LM_NOTICE("cluster: node %d is back after %lld s "
					"of silence - re-placing what this node "
					"shares with it\n", node,
					(nowh - p->last_seen_ms) / 1000);
			}
		}
	}
	p->last_seen_ms = now_ms();

	/* the incarnation (new each start) tells a restart; entries and start kind come ONLY with
	 * its own ALIVE: JOIN, ASSIGN and MASTER_ALIVE callers know neither, so write neither */
	if (incarn) {
		p->entries = entries;          /* state, not a timer */
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
		/* assume its memory is gone: reset mark AND cursor, or the resumed cycle
		 * skips the head of the table */
		memset(p->repl_mark, 0, sizeof p->repl_mark);
		memset(p->repl_cursor, 0, sizeof p->repl_cursor);
		memset(p->repl_dirty, 0, sizeof p->repl_dirty);
		memset(p->repl_bfcycle, 0, sizeof p->repl_bfcycle);
		memset(p->askfill, 0, sizeof p->askfill);
		/* the sweep sends only AUTHORED records (copies would echo), so an empty node needs
		 * a ONE-SHOT pass that includes copies, bounded to one clean cycle */
		/* armed by the start kind it reports (PC_START_*), never its count; COLD arms only
		 * on a restart this node witnessed or once this node is past its own cold window */
		if (incarn && ((start == PC_START_COLD &&
		        (known || self_start_kind() != PC_START_COLD)) ||
		        (start == PC_START_UNKNOWN && !entries))) {
			memset(p->backfill, 1, sizeof p->backfill);
			p->fresh_ms = p->last_seen_ms;
			p->cold_fresh = 1;
			/* under spread a cold member is a repair event: one sender holds only K
			 * of P, and a bounce inside the slot grace never arms setrepair */
			if (pc_cluster_replicas()) {
				memset(p->setrepair, 1, sizeof p->setrepair);
				memset(p->repl_cursor, 0, sizeof p->repl_cursor);
			}
			LM_NOTICE("cluster: node %d started cold (%u records) - "
				"backfilling it%s\n", node, entries,
				pc_cluster_replicas()
				? ", and re-placing its share from every holder"
				: "");
		} else if (incarn && start == PC_START_RECOVERED &&
		        pc_cluster_replicas()) {
			/* under spread a recovered member is a repair event too: its copies are
			 * off the WAL, so one overwritten after its snapshot comes back old */
			memset(p->setrepair, 1, sizeof p->setrepair);
			memset(p->repl_cursor, 0, sizeof p->repl_cursor);
			LM_NOTICE("cluster: node %d restarted holding %u recovered "
				"records - re-placing its share from every holder "
				"(its copies came from a snapshot and may be older)\n",
				node, entries);
		} else if (incarn && start == PC_START_RECOVERED)
			LM_NOTICE("cluster: node %d restarted holding %u records "
				"recovered from its WAL - NOT backfilling (a recovered "
				"node needs a diff, not a blind push)\n", node, entries);
		else if (incarn && start == PC_START_UNKNOWN)
			LM_NOTICE("cluster: node %d (older build) restarted holding "
				"%u records - NOT backfilling (judged by its count: a "
				"recovered node needs a diff)\n", node, entries);
	}
	/* a cold peer now ESTABLISHED (pulled, or the push finished it) no longer needs the resync
	 * from mark zero: the pull carried it, and later writes went by the write-path push */
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
		/* only a REAL change re-arms the reshard grace: this runs every heartbeat, and an
		 * always-on grace makes every owner's miss broadcast instead of authoritative */
		if (fresh || was != node)
			shard_note_change();
	}
}

/* order-independent membership digest over (node,ip): FNV-1a constants fed a word at a time, so
 * NOT FNV-1a - it need only be fleet-stable, and reshaping it would change the wire */
uint64_t member_digest(void)
{
	uint64_t d = 0;
	long long now = now_ms();
	int i;

	for (i = 0; i < C.pt.n_peers; i++) {
		uint64_t h = FNV1A64_BASIS;

		if (!clpeers_live(&C.pt.peers[i], now))
			continue;
		h = (h ^ (uint32_t)C.pt.peers[i].node) * FNV1A64_PRIME;
		h = (h ^ C.pt.peers[i].addr.in4.sin_addr.s_addr) * FNV1A64_PRIME;
		d ^= h;
	}
	{
		uint64_t h = FNV1A64_BASIS;

		h = (h ^ (uint32_t)C.node_id) * FNV1A64_PRIME;
		h = (h ^ C.self_addr.in4.sin_addr.s_addr) * FNV1A64_PRIME;
		d ^= h;
	}
	return d;
}

static void send_join_req(const pc_addr *to)
{
	unsigned char msg[CLMEMB_JOINREQ_BUF];
	struct clmemb_joinreq j;

	memset(&j, 0, sizeof j);
	j.tok = C.join_tok;
	/* the collection config goes UP FRONT: a master running another one refuses the
	 * join rather than assign an id and half-complete it */
	j.mode = C.cfg_mode;
	j.eager = C.cfg_eager;
	j.cfg_digest = config_digest();
	j.cfg2 = 1;
	j.cfg_digest2 = config_digest2();
	/* a proposal only: the master rejects it if taken (two identities may hash alike) */
	j.ident = C.self_ident;
	j.proposed = C.proposed_id;
	/* identity alone cannot tell a member re-joining from a second process claiming
	 * to be it; the incarnation can */
	j.incarn = C.self_incarn;
	/* the WAL posture, so a refusal can NAME it.  1 = no WAL,
	 * 2 = logs one; a build before the byte sends 41 and reads as unknown */
	j.wal = C.cfg_wal ? 2 : 1;
	j.epoch = pc_cluster_epoch();            /* what the fleet speaks */
	{
		size_t jn = clmemb_joinreq_write(msg, sizeof msg, M_JOIN_REQ, &j);

		if (to)
			seal_send(to, msg, jn);
		else {
			/* no target: the group, or every seed and known
			 * address; a seed that moved is re-resolved now */
			if (cl_unicast)
				clmesh_resolve_soon();
			ctl_send(msg, jn);
		}
	}
	C.join_last_ms = now_ms();
	C.st.joins++;
}

/* the ALIVE's start kind: COLD lasts one sender holdoff past READY, enough for peers to arm the
 * backfill; ready_ms is stamped lazily on the cluster thread at the first beat after READY */
static int self_start_kind(void)
{
	if (pc_recovered_records() > 0)
		return PC_START_RECOVERED;
	/* a node that pulled its bootstrap KNOWS it is complete; flags peers armed on our
	 * cold beat drop as stale at their next tick */
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

/* this node can mail (pc_cluster_set_can_mail, from the alert tick) */
static int self_can_mail;

/* cfgsig/ncfgsig are defined with C, above: the members reply reads them too */
void pc_cluster_set_cfgsig(const struct pc_cfgsig *e, int n)
{
	if (n < 0)
		n = 0;
	if (n > PC_CFGSIG_MAX)
		n = PC_CFGSIG_MAX;
	memcpy(cfgsig, e, (size_t)n * sizeof cfgsig[0]);
	ncfgsig = n;
}

int pc_cluster_cfgsig(const struct pc_cfgsig **e)
{
	*e = cfgsig;
	return ncfgsig;
}

/* PC_BUILD_REV's first 7 hex digits (git's shortest abbreviation, so every build has them) as a
 * word: 1, or 0 for a rev too short or not hex */
static int build_rev_word(uint32_t *w)
{
	const char *r = PC_BUILD_REV;
	uint32_t v = 0;
	int i;

	for (i = 0; i < 7; i++) {
		char c = r[i];

		if (c >= '0' && c <= '9')
			v = v << 4 | (uint32_t)(c - '0');
		else if (c >= 'a' && c <= 'f')
			v = v << 4 | (uint32_t)(c - 'a' + 10);
		else
			return 0;
	}
	*w = v;
	return 1;
}

/* this node's facts, after its settings, in @name/@val from @at; returns the new count */
static int put_facts(uint32_t *name, uint32_t *val, int at, int cap, int features)
{
	if (at + PC_CL_NFACTS > cap)
		return at;
	name[at] = PC_FACT_VERSION;
	val[at++] = (uint32_t)PC_VERSION_TWEAK << 24 | (uint32_t)PC_VERSION_MAJOR << 16 |
		(uint32_t)PC_VERSION_MINOR << 8 | (uint32_t)PC_VERSION_PATCH;
	name[at] = PC_FACT_EPOCHS;
	val[at++] = (uint32_t)PC_CL_EPOCH_MIN << 8 | (uint32_t)pc_cluster_epoch_max();
	name[at] = PC_FACT_FEATURES;
	val[at++] = (uint32_t)features;
	name[at] = PC_FACT_UPGRADE;                    /* a cached word */
	val[at++] = pc_upg_fact();
	/* in a SPARE slot only (see PC_FACT_AUTOSCALE) */
	if (at < cap && (features & CLMEMB_FEAT_AUTOSCALE)) {
		name[at] = PC_FACT_AUTOSCALE;
		val[at++] = pc_store_autoscale_sig(NULL);
	}
	if (at < cap && build_rev_word(&val[at]))
		name[at++] = PC_FACT_REV;
	return at;
}

static int self_features(void)
{
	int f = CLMEMB_FEAT_TYPES2 | CLMEMB_FEAT_HREAD |
		CLMEMB_FEAT_SLOCK | CLMEMB_FEAT_STAGE;

	/* a statement of its own: symlocktest and hashfwdtest build their
	 * old-member variants by editing the line above as text */
	if (!self_no_respmap)
		f |= CLMEMB_FEAT_RESPMAP;
	if (!self_no_holdq)
		f |= CLMEMB_FEAT_HOLDQ;
	if (!self_no_autoscale)
		f |= CLMEMB_FEAT_AUTOSCALE;

	if (__atomic_load_n(&self_can_mail, __ATOMIC_RELAXED))
		f |= CLMEMB_FEAT_MAILER;
	return f;
}

int pc_member_fact(const struct pc_member *m, uint32_t key, uint32_t *val)
{
	int k;

	for (k = 0; k < m->ncfg; k++)
		if (m->cfg_name[k] == key) {
			*val = m->cfg_val[k];
			return 1;
		}
	return 0;
}

void send_alive(void)
{
	unsigned char msg[CLMEMB_ALIVE_BUF];
	struct clmemb_alive a;
	size_t len;

	/* the durable point, every beat: the marks this frame announces are confirmed against it */
	pc_ack_synced(pc_wal_durable_seq(pc_rdb_safe_marker()));

	memset(&a, 0, sizeof a);
	a.node = C.node_id;
	a.free_mb = self_free_mb();
	a.total_mb = self_total_mb();
	a.live_kb = self_live_kb();
	/* what this node believes the collections ARE: a peer that disagrees is refused, not fed */
	a.mode = C.cfg_mode;
	a.eager = C.cfg_eager;
	a.cfg_digest = config_digest();
	a.cfg2 = 1;                        /* written behind the ext group */
	a.cfg_digest2 = config_digest2();
	a.client_port = C.cfg_client_port;
	/* identity, incarnation and all after them are additive tail fields: an older peer reads
	 * the prefix it knows and ignores the rest (clmemb's gates) */
	a.ident = C.self_ident;
	a.incarn = C.self_incarn;
	a.entries = self_entries() + self_entries_bias;
	/* the Lamport clock rides the heartbeat: every node folds in every other's at 1 Hz,
	 * so the fleet converges with no extra traffic */
	a.lamport = pc_lamport_now();
	a.nstate = pc_node_state();
	a.epoch = pc_cluster_epoch();
	a.sole_witness = rec_sole_witness();
	a.features = self_features();
	/* the ext group always goes, for the link; ACKS and marks only below */
	a.ext = 1;
	a.features32 = (uint32_t)a.features;
	pc_alerts_link_state(&a.link_pct, &a.link_level);
	/* the features word and this beat's slice of the marks */
	if (!self_no_acks && pc_ack_enabled()) {
		uint64_t o[CLMEMB_MARKS_MAX], m[CLMEMB_MARKS_MAX];
		uint32_t in[CLMEMB_MARKS_MAX];
		int i, ns;

		a.ext = 1;
		a.features32 = (uint32_t)a.features | CLMEMB_F32_ACKS;
		a.nmarks = pc_ack_slice(o, in, m, CLMEMB_MARKS_MAX, &ns);
		a.nspaces = ns;
		for (i = 0; i < a.nmarks; i++) {
			a.marks[i].oid = o[i];
			a.marks[i].incarn = in[i];
			a.marks[i].mark = self_freeze_marks ?
				freeze_mark(o[i], in[i], m[i]) : m[i];
		}
	}
	a.nreason = pc_node_reason();
	a.mem_tier = pcache_mem.tier;   /* the arena's ACTUAL tier; 0 = none */
	a.resp_port = C.cfg_resp_port;
	a.start_kind = self_start_kind();
	a.http_port = C.cfg_http_port;
	a.uptime_s = (uint32_t)pc_metrics_uptime();
	/* open clients ride the heartbeat, so the page sums the map
	 * and never fetches another node */
	a.cl_open = PC_RESP_READ(pc_clients_open);
	a.cl_resp = PC_RESP_READ(pc_resp_open);
	a.cl_bin = PC_RESP_READ(pc_nat_bin_open);
	a.cl_nresp = PC_RESP_READ(pc_nat_resp_open);
	{
		struct clmemb_col all[CLMEMB_COLS_MAX];
		static unsigned rot;                   /* a window a beat */
		int n, k;

		self_cols(all, &n, &a.ncols_total);
		for (k = 0; k < n && k < CLMEMB_COLS_BEAT; k++)
			a.cols[k] = all[n <= CLMEMB_COLS_BEAT ? k
				: (int)((rot * CLMEMB_COLS_BEAT + (unsigned)k) % (unsigned)n)];
		a.ncols = k;
		rot++;
	}
	a.ps_port = clps_rx_port();
	a.ps_interest = pc_pubsub_interest_version();
	{
		int k;

		a.ncfg = ncfgsig;
		for (k = 0; k < ncfgsig; k++) {
			a.cfg_name[k] = cfgsig[k].name_h;
			a.cfg_val[k] = cfgsig[k].val_h;
		}
		a.ncfg = put_facts(a.cfg_name, a.cfg_val, a.ncfg,
			CLMEMB_CFG_MAX, a.features);
	}
	len = clmemb_alive_write(msg, sizeof msg, M_ALIVE, &a);
	ctl_send(msg, len);
	C.st.hb_sent++;
	/* publish for the watchdog; a non-master's stale MASTER_ALIVE
	 * must not outlive the role, so clear it here */
	clwire_beat_store(&C.wr, CLWIRE_ALIVE, msg, len);
	if (C.role != PC_ROLE_MASTER)
		clwire_beat_store(&C.wr, CLWIRE_MALIVE, NULL, 0);
	clwire_mark_sent(&C.wr, now_ms());
}

/* ---- the cluster map (control plane, additive) --------------------------
 * The master builds and broadcasts it; every other node adopts it if the epoch is newer. */

static int pick_backup(void);
static void clsync_send(const struct pc_clmap *m);

/* master only: two maps at one epoch is the ambiguity the term exists to remove */
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
	/* the backup rides the same publication, so the fleet learns master and backup atomically;
	 * clmap_stage() picked it, so the map names the node that acks it */
	m->backup_id = (uint16_t)C.backup_id;
	m->mode = (uint8_t)C.cfg_mode;
	m->eager = (uint8_t)(C.cfg_eager ? 1 : 0);
	m->config_digest = config_digest();

	/* self first, then every peer heard from inside the up window */
	pc_clhist_note(&C.hist, C.self_ident, (uint16_t)C.node_id);
	m->node[0].node_id = (uint16_t)C.node_id;
	memcpy(m->node[0].ident, C.self_ident, 16);
	m->node[0].addr = C.self_addr.in4.sin_addr.s_addr;
	m->node[0].cluster_port = C.self_addr.in4.sin_port;
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
			continue;      /* a duplicate id makes every receiver refuse the map */
		/* a node enters the map once READY and leaves only by going away: a
		 * JOINING peer listed as STARTING could not be placed until a reissue */
		if (p->nstate != PC_NST_READY &&
		        !pc_clmap_find(&C.map, (uint16_t)id))
			continue;
		/* the master records every identity it maps: it tells a returning node from a new
		 * one, and a promoted backup cannot rebuild it */
		if (p->has_ident)
			pc_clhist_note(&C.hist, p->ident, (uint16_t)id);
		nd = &m->node[m->nnodes];
		nd->node_id = (uint16_t)id;
		memcpy(nd->ident, p->ident, 16);
		nd->addr = p->addr.in4.sin_addr.s_addr;
		nd->cluster_port = p->addr.in4.sin_port;
		nd->client_port = (uint16_t)p->client_port;
		nd->state = (uint8_t)p->nstate;
		nd->master_pref = 0;
		/* uniform weights place exactly as the unweighted HRW does, so switching
		 * placement to the map moves no key */
		nd->cap_weight = PC_CLMAP_W_NOMINAL;
		nd->admin_weight = PC_CLMAP_W_UNSET;
		m->nnodes++;
	}
	return 0;
}

/* ---- reconcile a replayed keyspace against the live fleet: EAGER ONLY ---
 * (a key no peer holds was deleted); the cost is a write this node never replicated. */
/* ---- the hold-check: "held, and at which version?" for a batch of keys,
 * never a value (M_HOLD_Q / M_HOLD_A; the frames are clhold.c's) ---------- */

/* a key's slot times from when it joins the batch (pull_timeout_ms): a batch must go this
 * soon after its first key, or a slow walk lets its first slots expire unsent */
#define HQ_HOLD_MS 10

static unsigned char hq_msg[CLHOLD_MAX];   /* the batch the walk is filling */
static struct clhold_qw hq;
static long long hq_t0;                /* when its first key joined */

/* the batch to every live peer: a batch never outlives the walk's slice or spans two collections */
static void hq_flush(void)
{
	size_t n;

	if (!hq.n)
		return;
	n = clhold_q_end(&hq);
	(void)clsend_live(&C.pt, hq_msg, n, now_ms());
	hq.n = 0;
}

/* one key in, with its own pending slot as a pull's probe has (same outcome).  1 asked; 0 not:
 * no live peer or no slot (a full pending table keeps the key, as for an unasked pull) */
static int hq_add(const char *col, size_t collen, const char *key,
		size_t klen)
{
	struct pending *slot = NULL;
	uint32_t req;
	int up = clpeers_count_live(&C.pt, now_ms());

	if (!up || collen > 255 || klen > 4096 ||
	    4 + collen + 2 + 6 + klen > CLHOLD_MAX)
		return 0;
	if (hq.n && (!clhold_q_fits(&hq, klen) ||
	    !clhold_q_is_col(&hq, col, collen) ||
	    now_ms() - hq_t0 >= HQ_HOLD_MS))
		hq_flush();
	req = cl_pend_alloc(PC_DONE_RECONCILE, up, 0, &slot);
	if (!req)
		return 0;
	if (collen < sizeof slot->col && klen < sizeof slot->key) {
		memcpy(slot->col, col, collen);
		slot->collen = (unsigned char)collen;
		memcpy(slot->key, key, klen);
		slot->klen = (unsigned short)klen;
	}
	if (!hq.n) {
		hq_t0 = now_ms();
		(void)clhold_q_begin(&hq, hq_msg, sizeof hq_msg,
			(unsigned int)C.node_id, col, collen);
	}
	(void)clhold_q_add(&hq, req, key, klen);
	return 1;
}

/* answer a hold-check as pull_serve reads; a key this node cannot speak for is left out
 * (the asker's slot times out and keeps the key) */
static void handle_hold_q(const unsigned char *pt, size_t n,
		const pc_addr *from)
{
	unsigned char ans[CLHOLD_A_HDR + CLHOLD_MAXN * CLHOLD_A_ENTRY];
	pcache_htable_t *ht;
	struct clhold_qr q;
	const char *key;
	size_t an, kl;
	unsigned int out = 0;
	uint32_t req;

	if (self_no_holdq || clhold_q_open(&q, pt, n) != 0)
		return;                    /* the test flag: as an older build */
	/* a RECOVERING node's store is incomplete: silence, as its pulls get
	 * (handle_pull_req) - a "no" from it would authorise a drop */
	if (pc_node_state() == PC_NST_RECOVERING)
		return;
	ht = pc_store_find(q.col, q.collen);
	an = clhold_a_begin(ans, (unsigned int)C.node_id);
	while (out < CLHOLD_MAXN && clhold_q_next(&q, &req, &key, &kl)) {
		unsigned long long ver = 0;
		unsigned int wt = 0;
		int held = 0;
		str k;

		k.s = (char *)key;
		k.len = (int)kl;
		/* the version alone, as pull_serve's one read gives it with
		 * the value; an expired record reads as absent there too */
		if (ht && pcache_ht_getmeta(ht, &k, &ver, &wt) == 0) {
			held = 1;
			/* a recovered record the fleet has not ruled on yet is no answer */
			if (C.rec_hold && C.cfg_eager && rec_held(wt)) {
				C.st.pull_held_recovered++;
				continue;
			}
		}
		an = clhold_a_put(ans, an, req, held, ver);
		out++;
	}
	if (!out)
		return;
	clhold_a_end(ans, out);
	C.st.holdq_served += out;
	seal_send(from, ans, an);
}

/* a peer's answers to this node's hold-check - each into its key's slot, as
 * a pull's answer goes (handle_pull_rsp): every peer held-not or a race */
static void handle_hold_a(const unsigned char *pt, size_t n)
{
	unsigned int node, cnt, i;

	if (clhold_a_open(pt, n, &node, &cnt) != 0)
		return;
	for (i = 0; i < cnt; i++) {
		struct clpend_ans_out a;
		unsigned long long ver;
		uint32_t req;
		int held;

		if (!clhold_a_get(pt, n, i, &req, &held, &ver))
			break;
		if (held)
			pc_lamport_observe(ver);
		switch (clpend_answer(&C.pd, req, held, (int)node, ver, 0, 0, &a)) {
		case CLPEND_ANS_RACE:
			birth_race(&a);
			break;
		case CLPEND_ANS_RECONCILE:
			reconcile_drop(&a);
			break;
		default:
			break;     /* held (HIT, SAME): kept; WAIT and IGNORE */
		}
	}
}

struct rec_ctx {
	pcache_htable_t *ht;               /* whose records are walked */
	const char *col;
	size_t collen;
	int budget;
	const unsigned char *diff;     /* only these slots, or NULL */
	unsigned int seen;             /* records walked this tick */
	int hold;                      /* asked by hold-check, not pull */
};

static int reconcile_cb(const str *key, const str *val, unsigned int exp,
		void *arg)
{
	struct rec_ctx *r = arg;

	(void)val;
	(void)exp;
	C.st.reconcile_walked++;           /* skipped ones too */
	/* only a record written at or before rec_tick is in question: a newer one is this node's
	 * own, and nobody else holding it yet is no evidence of a delete */
	{
		unsigned long long v;
		unsigned int wt;

		if (r->ht && pcache_ht_getmeta(r->ht, key, &v, &wt) == 0 &&
		    !rec_held(wt))
			return 0;
	}
	/* no budget stop: digest-skipped keys cost none, so one tick may walk most of a table -
	 * keep the beat going meanwhile */
	if ((r->seen++ & 511) == 511) {
		hq_flush();                /* not held across the pump */
		beat_pump();
	}
	/* a slot the witness holds identically has nothing to ask */
	if (r->diff && reconcile_digest_same(r->diff, key)) {
		C.st.reconcile_dig_skipped++;
		return 0;
	}
	/* by hold-check when every live member answers one - the
	 * same slot and outcome as a pull's probe, without the value */
	if (r->hold ? hq_add(r->col, r->collen, key->s, (size_t)key->len) :
	    pc_pull_begin_kind(r->col, r->collen, key->s, (size_t)key->len,
	        PC_DONE_RECONCILE) != 0) {
		if (!C.rec_start_ms)
			C.rec_start_ms = now_ms();
		C.st.reconcile_probed++;
		if (r->hold)
			C.st.reconcile_holdq++;
	} else {
		C.recon.unruled++;             /* no slot, so not asked */
	}
	r->budget--;                       /* even unasked, or a full table burns the rounds */
	return 0;
}

/* a peer back EMPTY (COLD, or UNKNOWN not yet heard) says no to everything, so the pass waits
 * until none is live, and each drop re-checks; COLD ends within BACKFILL_SENDER_HOLDOFF_MS */
static int any_live_peer_cold(void)
{
	long long now = now_ms();
	int i;

	for (i = 0; i < C.pt.n_peers; i++) {
		struct peer *p = &C.pt.peers[i];

		if (!clpeers_live(p, now))
			continue;
		if (p->start_kind == PC_START_COLD ||
		        p->start_kind == PC_START_UNKNOWN)
			return 1;
	}
	return 0;
}

/* the pass needs a WITNESS: a live peer up since before this node died (after a whole-fleet
 * outage every peer denies the others' keys).  Every clock error here falls toward KEEPING. */
#define ALIVE_STAMP_FILE   "alive"
#define ALIVE_STAMP_BEATS  10          /* ~10 s: bounds the error on death */
/* a witness must have been up THREE sweep periods before we died - up is not complete: in a
 * staggered failure an early riser had not yet been re-sent the others' records */
#define WITNESS_GRACE_MS   (3LL * REPL_SWEEP_BEATS * 1000)

static long long alive_stamp_mtime_ms(void)
{
	char path[600];
	struct stat sb;

	if (!C.state_dir[0])
		return 0;
	snprintf(path, sizeof path, "%s/" ALIVE_STAMP_FILE, C.state_dir);
	if (stat(path, &sb) != 0)
		return 0;
	return (long long)sb.st_mtim.tv_sec * 1000 + sb.st_mtim.tv_nsec / 1000000;
}

/* an idle eager node writes nothing: without this stamp its death estimate could be hours early
 * and no witness found.  Best effort - a failed stamp only makes the reconcile keep more. */
static void alive_stamp_touch(void)
{
	char path[600];
	int fd;

	if (!C.cfg_eager || !C.state_dir[0])
		return;
	snprintf(path, sizeof path, "%s/" ALIVE_STAMP_FILE, C.state_dir);
	fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
	if (fd < 0)
		return;
	(void)futimens(fd, NULL);
	close(fd);
}

/* Called ONCE at init, before the first beat touches the stamp. */
static long long reconcile_death_mono(void)
{
	struct timespec w;

	clock_gettime(CLOCK_REALTIME, &w);
	return clrecon_death_mono(pc_recovered_last_write_ms(),
		alive_stamp_mtime_ms(),
		(long long)w.tv_sec * 1000 + w.tv_nsec / 1000000, now_ms());
}

/* THE predicate (clrecon): this peer was up, and complete, before we went
 * down - by the grace this file keeps */
static int is_witness(const struct peer *p)
{
	return clrecon_is_witness(&C.recon, p->boot_ms, WITNESS_GRACE_MS);
}

/* asked every tick and at every drop: the witness that started the pass can die, leaving only
 * peers that came back with us */
static int reconcile_witness_live(void)
{
	return clrecon_first_witness(&C.recon, &C.pt, now_ms(),
		WITNESS_GRACE_MS, NULL) != NULL;
}

/* the keepalive's sole_witness: 0 none live, the one's node id, or CLMEMB_SOLEWIT_MANY (two or
 * more, or no reconcile open); a peer refuses a restart only of the last witness */
static int rec_witness_count(int *id)        /* no side effects */
{
	return clrecon_witnesses(&C.recon, &C.pt, now_ms(), WITNESS_GRACE_MS, id);
}

static int rec_sole_witness(void)
{
	int id, n;

	if (!(C.rec_hold && C.cfg_eager)) {
		C.st.reconcile_witnesses_live = 0;
		return CLMEMB_SOLEWIT_MANY;
	}
	n = rec_witness_count(&id);
	C.st.reconcile_witnesses_live = n;
	return n == 0 ? 0 : n == 1 ? id : CLMEMB_SOLEWIT_MANY;
}

/* would a restart of this node NOW be witnessed, and when: a peer counts once up for the grace
 * plus both clock errors (uptime_s truncation, one alive-stamp period) */
#define RESTART_WITNESS_MS \
	(WITNESS_GRACE_MS + ALIVE_STAMP_BEATS * 1000LL + 1000LL)

static void restart_witness_note(void)
{
	struct clrecon_restart o;

	clrecon_restart(&C.pt, now_ms(), RESTART_WITNESS_MS, C.node_id, &o);
	memcpy(C.st.restart_waits_for, o.waits_for,
		sizeof o.waits_for[0] * (size_t)o.waits_n);
	C.st.restart_witnesses = o.witnesses;
	C.st.restart_waits_n = o.waits_n;
	/* only an eager node reconciles, a lone one has nothing to ask; and not while a peer's
	 * reconcile is open - this node may be the only witness it has left */
	C.st.restart_safe = (!C.cfg_eager || !o.live || o.witnesses > 0) &&
		!o.waits_n;
	C.st.restart_safe_in_ms = C.st.restart_safe ? 0 : o.waits_n ? -1 :
		o.wait_ms;
}

/* an eager miss is final only on a node holding every record, which READY does not say: not
 * before this node has been up WITNESS_GRACE_MS (set once at start; 0 = not yet ticked) */
static long long eager_settle_ms;

/* nor before it has caught up: holds at least the record count of every live READY peer. Latched
 * once per process (counts move both ways later); only when every live collection is eager */
static int eager_caught_up;

static int all_live_eager(void)
{
	int c, any = 0;

	for (c = 0; c < pc_store_count(); c++) {
		if (!pc_store_live(c))
			continue;
		if (!pc_store_eager_enabled(pc_store_ht(c)))
			return 0;
		any = 1;
	}
	return any;
}

/* T600: the counts alone never settle while the peers hold records no client reads (a 7k gap
 * stood on PROD for hours, every miss parked on a pull) - so the pulls' own answers decide too */
#define CATCHUP_EVIDENCE   1000        /* pull answers per window */
#define CATCHUP_HIT_PCT    1           /* at most this share found a record: the doubt is spent */
#define CATCHUP_MIN_MS     (4 * WITNESS_GRACE_MS)  /* the sweeps' re-sends land first */

static void eager_catchup_tick(long long now)
{
	static unsigned long long h0, m0;
	unsigned long long dh, dm;
	unsigned int mine, most = 0;
	int i, seen = 0, behind = 0;

	if (__atomic_load_n(&eager_caught_up, __ATOMIC_RELAXED))
		return;
	if (!all_live_eager()) {
		__atomic_store_n(&eager_caught_up, 1, __ATOMIC_RELAXED);
		return;
	}
	if (C.role == PC_ROLE_JOINING || pc_node_state() != PC_NST_READY)
		return;
	mine = self_entries();
	for (i = 0; i < C.pt.n_peers; i++) {
		struct peer *p = &C.pt.peers[i];

		if (!__atomic_load_n(&p->node, __ATOMIC_ACQUIRE) ||
		        !clpeers_live(p, now) || p->nstate != PC_NST_READY)
			continue;
		if (p->entries > mine) {
			behind = 1;            /* behind this peer: keep asking */
			if (p->entries > most)
				most = p->entries;
		}
		seen++;
	}
	if (!seen) {
		h0 = C.st.pull_hits;       /* nobody to compare with yet */
		m0 = C.st.pull_misses;
		return;
	}
	if (behind) {
		long long t0 = __atomic_load_n(&eager_settle_ms, __ATOMIC_RELAXED);

		if (!t0 || now - t0 < CATCHUP_MIN_MS) {
			h0 = C.st.pull_hits;   /* the window opens after it */
			m0 = C.st.pull_misses;
			return;
		}
		dh = C.st.pull_hits - h0;
		dm = C.st.pull_misses - m0;
		if (dh + dm < CATCHUP_EVIDENCE)
			return;
		h0 = C.st.pull_hits;
		m0 = C.st.pull_misses;
		if (dh * 100 > (dh + dm) * CATCHUP_HIT_PCT)
			return;            /* the peers still hold what it lacks */
		__atomic_store_n(&eager_caught_up, 1, __ATOMIC_RELAXED);
		LM_NOTICE("cluster: caught up - holding %u record(s) to a peer's %u, but %llu of "
			"the last %llu pull answers found nothing this node lacks; an eager miss "
			"here is final from now on\n", mine, most, dm, dh + dm);
		return;
	}
	__atomic_store_n(&eager_caught_up, 1, __ATOMIC_RELAXED);
	LM_NOTICE("cluster: caught up - holding %u record(s), as many as every "
		"live peer; an eager miss here is final from now on\n", mine);
}

/* a returning peer is doubted alone, and only if it may hold what this node lacks: it ran on
 * cut off (same incarnation) or replayed a WAL tail (RECOVERED) */
#define DOUBT_MAX 64
static struct { int node; unsigned int incarn; unsigned char live, seen, young;
	long long doubt_ms; } dq[DOUBT_MAX];
static int eager_doubt_n, eager_doubt_node;      /* read by workers */

static int dq_slot(int node)
{
	int k, free_k = -1;

	for (k = 0; k < DOUBT_MAX; k++) {
		if (dq[k].node == node)
			return k;
		if (!dq[k].node && free_k < 0)
			free_k = k;
	}
	if (free_k >= 0) {
		memset(&dq[free_k], 0, sizeof dq[free_k]);
		dq[free_k].node = node;
	}
	return free_k;
}

static void eager_doubt_tick(long long now, int own)
{
	int i, k, n = 0, one = 0, young = 0;

	for (k = 0; k < DOUBT_MAX; k++)
		dq[k].seen = 0;
	for (i = 0; i < C.pt.n_peers; i++) {
		struct peer *p = &C.pt.peers[i];
		int node = __atomic_load_n(&p->node, __ATOMIC_ACQUIRE);

		if (!node || !clpeers_live(p, now) || (k = dq_slot(node)) < 0)
			continue;
		/* a restart inside PEER_UP_MS never left: its new incarnation says so */
		if (p->incarn == dq[k].incarn ? dq[k].incarn && !dq[k].live :
		        p->start_kind == PC_START_RECOVERED) {
			dq[k].doubt_ms = now;
			dq[k].young = (unsigned char)own;  /* opened in our own start window */
			if (pc_logrl_ok(LRL_EAGER_DOUBT, &p->addr))
				LM_NOTICE("cluster: node %d is back %s - its writes may "
					"not have reached this node, so a miss asks it for "
					"%lld s\n", node, p->incarn == dq[k].incarn ?
					"from a partition" : "with a recovered log",
					WITNESS_GRACE_MS / 1000);
		}
		dq[k].incarn = p->incarn;
		dq[k].live = dq[k].seen = 1;
	}
	for (k = 0; k < DOUBT_MAX; k++) {
		if (!dq[k].seen)
			dq[k].live = 0;
		if (dq[k].doubt_ms && now - dq[k].doubt_ms >= WITNESS_GRACE_MS)
			dq[k].doubt_ms = 0;
		if (dq[k].doubt_ms) {
			n++;
			one = dq[k].node;
			young |= dq[k].young;
		}
	}
	/* ask one peer alone only if this node was complete when it came back: a young
	 * node would ask that peer for another's keys */
	__atomic_store_n(&eager_doubt_node, n == 1 && !young ? one : 0,
		__ATOMIC_RELAXED);
	__atomic_store_n(&eager_doubt_n, n, __ATOMIC_RELEASE);
}

static void askfill_tick(long long now);

static void eager_settle_tick(long long now)
{
	long long t = __atomic_load_n(&eager_settle_ms, __ATOMIC_RELAXED);

	if (!t)                                /* this node's own start */
		__atomic_store_n(&eager_settle_ms, (t = now), __ATOMIC_RELAXED);
	eager_doubt_tick(now, now - t < WITNESS_GRACE_MS);
	eager_catchup_tick(now);
	askfill_tick(now);
}

int pc_cluster_eager_miss_final(void)
{
	long long t = __atomic_load_n(&eager_settle_ms, __ATOMIC_RELAXED);

	return t && now_ms() - t >= WITNESS_GRACE_MS &&
		!__atomic_load_n(&eager_doubt_n, __ATOMIC_ACQUIRE) &&
		__atomic_load_n(&eager_caught_up, __ATOMIC_RELAXED) &&
		pc_node_state() == PC_NST_READY &&
		__atomic_load_n(&C.px.recv_nomem, __ATOMIC_RELAXED) == 0;
}

/* the one peer a miss asks while only it is doubted; 0 = ask all - as a node
 * not yet caught up does, or it would ask one peer for another's keys */
int pc_cluster_eager_doubt_one(void)
{
	long long t = __atomic_load_n(&eager_settle_ms, __ATOMIC_RELAXED);

	if (!t || now_ms() - t < WITNESS_GRACE_MS ||
	    !__atomic_load_n(&eager_caught_up, __ATOMIC_RELAXED))
		return 0;
	return __atomic_load_n(&eager_doubt_node, __ATOMIC_RELAXED);
}

/* ---- resync: drop what was stored before the absence, pull the fleet's state (RECOVERING) --
 * When away > resync_after_s, or a rebuild with no witness; a restart with none keeps all. */

/* a live READY peer that can stand for the fleet: up WITNESS_GRACE_MS and not younger than this
 * node by more (a peer restarted meanwhile may hold only its own WAL); most records wins */
static struct peer *resync_source(long long now)
{
	long long self_boot = now - (long long)pc_metrics_uptime() * 1000;
	struct peer *best = NULL;
	int i;

	for (i = 0; i < C.pt.n_peers; i++) {
		struct peer *p = &C.pt.peers[i];

		if (!__atomic_load_n(&p->node, __ATOMIC_ACQUIRE) ||
		    !clpeers_live(p, now) || p->nstate != PC_NST_READY ||
		    !p->entries || !p->boot_ms ||
		    p->boot_ms + WITNESS_GRACE_MS > now ||
		    p->boot_ms > self_boot + WITNESS_GRACE_MS)
			continue;
		if (!best || p->entries > best->entries)
			best = p;
	}
	return best;
}

static void resync_arm(void)
{
	C.decide_on = 0;                   /* the copy's end reports ready */
	C.resync_on = 1;
	C.resync_dropped_now = 0;
	C.rec_col = 0;
	C.rec_cursor = 0;
	C.recon.witness = 0;
	C.st.resyncing = 1;
	pc_node_state_set(PC_NST_RECOVERING);
}

static void resync_begin(const struct peer *src, const char *why)
{
	C.boot_prefer = 0;
	C.st.resyncs++;
	resync_arm();
	LM_WARN("cluster: resync - %s: dropping every record this node stored "
		"before its absence and pulling the fleet's state (node %d holds "
		"%u record(s)); RECOVERING until it holds it\n", why,
		__atomic_load_n(&src->node, __ATOMIC_ACQUIRE), src->entries);
}

#define SURVIVOR_STILL_MS 3000
#define DECIDE_MAX_MS 10000

/* the hold ends with no copy to wait for: READY, serving the records the pass still rules on, as
 * before the hold; @why when it ran out */
static void decide_end(const char *why)
{
	C.decide_on = 0;
	if (why)
		LM_WARN("cluster: %s - ready, serving its records while the fleet rules on them\n",
			why);
	if (C.role != PC_ROLE_JOINING)
		pc_node_state_set(PC_NST_READY);
}

struct spcs { struct { uint64_t oid; uint32_t incarn; } v[256]; int n; };

static int spcs_cb(uint64_t member, uint32_t incarn, uint64_t oid,
		uint32_t sincarn, uint64_t mark, void *ctx)
{
	struct spcs *s = ctx;
	int i;

	(void)member; (void)incarn; (void)mark;
	for (i = 0; i < s->n; i++)
		if (s->v[i].oid == oid && s->v[i].incarn == sincarn)
			return 0;
	if (s->n == (int)(sizeof s->v / sizeof s->v[0]))
		return 1;
	s->v[s->n].oid = oid;
	s->v[s->n++].incarn = sincarn;
	return 0;
}

/* short of deletes: its mark of a space below the highest any member reports, and still for
 * SURVIVOR_STILL_MS - its state lacks them, so copying it brings keys back.  -1: a mark below it
 * that moved since, or none yet (a delete in flight, a table just filling); @final reads those as
 * moving (not short) and missing (short) */
static int peer_short(const struct peer *p, const struct spcs *s, long long now,
		int final)
{
	uint64_t member, m, hi, applied;
	long long moved;
	int i, open = 0;

	if (!p->has_ident || !p->acks)
		return 0;                      /* nothing to judge it by */
	member = fnv1a64(p->ident, sizeof p->ident);
	for (i = 0; i < s->n; i++) {
		uint64_t oid = s->v[i].oid, head;
		uint32_t inc = s->v[i].incarn;

		if (oid == member && inc == p->incarn)
			continue;              /* its own run's deletes are its own */
		head = pc_ack_fleet_max(oid, inc, NULL, 0);
		if (pc_ack_known(oid, inc, &applied, &hi) && hi > head)
			head = hi;
		if (!head)
			continue;
		if (!pc_ack_fleet_mark(member, oid, inc, &m)) {
			if (final)
				return 1;
			open = 1;
			continue;
		}
		if (m >= head)
			continue;
		if (pc_ack_fleet_moved(member, oid, inc, &moved) &&
		    now - moved < SURVIVOR_STILL_MS)
			open |= !final;
		else
			return 1;
	}
	return open ? -1 : 0;
}

/* 12ol: a survivor - live, READY, and up since before this node's absence (a witness), the
 * fullest of those short of no delete (*whole), else the fullest.  Its state is the fleet's.
 * *wait: a witness's marks are too fresh to judge yet - ask again next tick, for at most
 * SURVIVOR_STILL_MS */
static struct peer *survivor(long long now, int *whole, int *wait)
{
	struct peer *best = NULL, *full = NULL;
	struct spcs s;
	int i, final, sh;
	long long open = -1;               /* the most records an undecided witness holds */
	static long long first_ask;

	if (!first_ask)
		first_ask = now;
	if (self_survivor_wait_ms && now - first_ask < self_survivor_wait_ms) {
		*whole = 0;
		*wait = 1;
		return NULL;
	}
	if (!C.surv_since || now - C.surv_since > 4 * SURVIVOR_STILL_MS)
		C.surv_since = now;            /* this pick's first ask, not an earlier pass's */
	final = now - C.surv_since >= SURVIVOR_STILL_MS;
	s.n = 0;
	(void)pc_ack_fleet_each(spcs_cb, &s);
	for (i = 0; i < C.pt.n_peers; i++) {
		struct peer *p = &C.pt.peers[i];

		if (!__atomic_load_n(&p->node, __ATOMIC_ACQUIRE) ||
		    !clpeers_live(p, now) || p->nstate != PC_NST_READY ||
		    !is_witness(p))
			continue;
		if (!full || p->entries > full->entries)
			full = p;
		sh = peer_short(p, &s, now, final);
		if (sh < 0 && (long long)p->entries > open)
			open = p->entries;
		if (!sh && (!best || p->entries > best->entries))
			best = p;
	}
	*whole = best != NULL;
	/* only an undecided witness that would be picked over the best one judged holds the pick */
	*wait = open >= 0 && (!best || open > (long long)best->entries);
	if (*wait)
		return NULL;
	C.surv_since = 0;
	if (best && best != full)
		LM_NOTICE("cluster: rejoin - node %d holds more records (%u) but is short "
			"of deletes the fleet made; node %d (%u) is copied instead\n",
			__atomic_load_n(&full->node, __ATOMIC_ACQUIRE), full->entries,
			__atomic_load_n(&best->node, __ATOMIC_ACQUIRE), best->entries);
	return best ? best : full;
}

/* 12ol: drop the records in question and copy the survivor's state whole (a write this node
 * never pushed goes with them); no snapshot until the copy is complete */
static void rejoin_begin(const struct peer *s, int whole)
{
	int node = __atomic_load_n(&s->node, __ATOMIC_ACQUIRE);

	C.boot_prefer = node;
	C.rejoin_on = 1;
	C.rejoin_whole = whole;
	__atomic_store_n(&C.rejoin_src, 0, __ATOMIC_RELEASE);
	C.st.rejoins++;
	pc_rdb_hold(1);
	resync_arm();
	LM_NOTICE("cluster: rejoin - node %d has been up since before %s "
		"(%lld s before), so it holds the fleet's state: dropping the "
		"%s and copying that state whole (%u record(s)); RECOVERING "
		"until it holds it\n", node, C.rec_floored ? "the deletes in "
		"question were lost" : C.rec_rebuild ? "this node was cut off"
		: "this node went down", (C.recon.death_mono - s->boot_ms) / 1000,
		C.rec_floored || C.rec_rebuild ? "records stored before then"
		: "replayed records", s->entries);
}

/* every survivor gone before the copy finished: start again from this node's own files, which
 * no snapshot has touched since the copy began; with no survivor then, it keeps them */
static void rejoin_lost(const char *why)
{
	LM_WARN("cluster: rejoin - %s: stopping without a snapshot to start again "
		"from this node's own files\n", why ? why : "no peer that was up "
		"through this node's absence is left to copy from");
	C.rejoin_on = -1;                  /* stopping: the bootstrap is not re-picked */
	pc_daemon_restart_from_files();
}

/* a pull completed: its source must still be the survivor picked - a peer restarted since
 * serves what it replayed, not the fleet's state; ready only once it says so */
static void rejoin_verify(void)
{
	int node = __atomic_load_n(&C.rejoin_src, __ATOMIC_ACQUIRE), i;
	long long end = __atomic_load_n(&C.rejoin_end_ms, __ATOMIC_ACQUIRE);
	struct peer *p = peer_by_node(node);
	unsigned int inc = 0;
	char why[160];

	for (i = 0; i < C.rejoin_ncand; i++)
		if (C.rejoin_cand[i] == node)
			inc = C.rejoin_inc[i];
	if (!p || p->alive_ms <= end) {
		if (now_ms() - end < PEER_UP_MS)
			return;                    /* its next ALIVE says */
		snprintf(why, sizeof why, "node %d, which served the copy, has not "
			"been heard from since", node);
		rejoin_lost(why);
		return;
	}
	if (!inc || p->incarn != inc || !is_witness(p)) {
		snprintf(why, sizeof why, "node %d restarted after it was picked, so "
			"the copy it served is what it replayed, not a survivor's state",
			node);
		rejoin_lost(why);
		return;
	}
	__atomic_store_n(&C.rejoin_src, 0, __ATOMIC_RELEASE);
	if (C.reconcile_on && C.cfg_eager && !self_test_reconcile) {
		/* a pass asked during the copy rules first: no READY between two copies */
		LM_NOTICE("cluster: rejoin - node %d is still the survivor that served the "
			"copy; a pass asked during it rules before this node serves\n", node);
		C.decide_on = 1;
		C.decide_ms = now_ms();
		return;
	}
	LM_NOTICE("cluster: rejoin - node %d is still the survivor that served the "
		"copy; ready\n", node);
	pc_node_state_set(PC_NST_READY);
}

struct rs_ctx {
	pcache_htable_t *ht;
	const char *col;
	int n, cap, nf;
	str *k;
};

static int resync_cb(const str *key, const str *val, unsigned int exp,
		void *arg)
{
	struct rs_ctx *r = arg;
	unsigned long long v;
	unsigned int wt;

	(void)val;
	(void)exp;
	/* only a record in question (recovered, or stored before a rebuilt
	 * member's absence): one written since is this node's own */
	if (pcache_ht_getmeta(r->ht, key, &v, &wt) != 0 || !rec_held(wt))
		return 0;
	/* a write-through mark goes with its record, never alone: the record's walk may have
	 * passed before the boundary moved up past both */
	if (r->nf && pc_upstream_mark_held(r->col, strlen(r->col), key->s,
	        (size_t)key->len))
		return 0;
	if (r->n == r->cap) {
		int nc = r->cap ? r->cap * 2 : 64;
		str *nk = realloc(r->k, (size_t)nc * sizeof *nk);

		if (!nk)
			return 0;          /* kept: the next resync or walk drops it */
		r->k = nk;
		r->cap = nc;
	}
	r->k[r->n].s = malloc((size_t)key->len);
	if (!r->k[r->n].s)
		return 0;
	memcpy(r->k[r->n].s, key->s, (size_t)key->len);
	r->k[r->n].len = key->len;
	r->n++;
	return 0;
}

/* the drop done: the hold off, the pass replaced by a joiner's pull */
static void resync_pull(void)
{
	const char *what = C.rec_floored ? "in question" : "stored before the absence";

	C.resync_on = 0;
	C.reconcile_on = 0;
	C.rec_rebuild = 0;
	C.rec_floored = 0;
	C.resync_floor = 1;                /* the floors, in boot_request */
	clrecon_walk_stop(&C.recon);
	clboot_rearm(&C.bt, now_ms());
	LM_NOTICE("cluster: %s - dropped %llu record(s) %s; pulling the fleet's "
		"state now\n", C.rejoin_on ? "rejoin" : "resync",
		C.resync_dropped_now, what);
}

/* a ~2 ms slice of the drop.  No tombstone (the fleet deleted them, or the pull brings them
 * back) and no refusal mark (the pull's copy of a live key is the version dropped here) */
static void resync_step(long long now)
{
	struct rs_ctx r;
	int i;

	if (C.rejoin_on && pc_rdb_busy())
		return;                    /* a snapshot begun before the hold walks: let it land */
	memset(&r, 0, sizeof r);
	while (C.rec_col < pc_store_count()) {
		pcache_htable_t *ht = pc_store_ht(C.rec_col);

		if (!ht || !pc_store_live(C.rec_col) ||
		    !pc_store_eager_enabled(ht)) {
			C.rec_col++;
			C.rec_cursor = 0;
			continue;
		}
		r.ht = ht;
		r.col = pc_store_name(C.rec_col);
		r.nf = pc_upstream_binds(r.col, strlen(r.col)) == 2;
		r.n = 0;
		pc_store_scan(ht, &C.rec_cursor, RECONCILE_BUCKETS,
			PCACHE_SCAN_NOVAL, resync_cb, &r);
		for (i = 0; i < r.n; i++) {
			if (store_remove(ht, &r.k[i]) == 1) {
				C.st.resync_dropped++;
				C.resync_dropped_now++;
			}
			free(r.k[i].s);
		}
		if (!C.rec_cursor) {
			C.rec_col++;
			continue;
		}
		if (now_ms() - now >= 2) {
			free(r.k);
			return;
		}
	}
	free(r.k);
	resync_pull();
}

static int reconcile_find_witness(void)
{
	long long now = now_ms();
	int live;
	struct peer *src = resync_source(now);
	struct peer *p;

	/* away longer than resync_after_s - the fleet's whole state,
	 * whoever could rule on the records one by one */
	if (resync_after_ms && src && !C.rec_floored &&
	    C.recon.death_mono != LLONG_MIN &&
	    now - C.recon.death_mono > resync_after_ms) {
		char why[128];

		snprintf(why, sizeof why, "away ~%lld s, longer than "
			"resync_after_s (%lld)", (now - C.recon.death_mono) / 1000,
			resync_after_ms / 1000);
		resync_begin(src, why);
		return 0;
	}
	p = clrecon_first_witness(&C.recon, &C.pt, now, WITNESS_GRACE_MS, &live);
	/* 12ol: a survivor holds the fleet's state - take it whole.  With none READY there is
	 * nobody to copy from or to ask: a split resyncs, a restart or a floor keeps all, below */
	if (!self_test_reconcile) {
		int whole, wait;
		struct peer *s = survivor(now, &whole, &wait);

		if (wait)
			return 0;              /* a witness's marks too fresh to judge: next tick */
		if (s) {
			rejoin_begin(s, whole);
			return 0;
		}
		p = NULL;
	}
	if (p) {
		int node = __atomic_load_n(&p->node, __ATOMIC_ACQUIRE);

		C.recon.witness = node;
		C.st.reconcile_witness = node;
		LM_NOTICE("cluster: reconcile - node %d has been up since "
			"before this node went down (%lld s before), so "
			"it saw anything deleted meanwhile; asking the "
			"fleet about %ld replayed record(s)\n", node,
			(C.recon.death_mono - p->boot_ms) / 1000,
			pc_recovered_records());
		if (C.decide_on)
			decide_end(NULL);      /* ruled on one by one: served meanwhile, as before */
		return 1;
	}
	/* back from a split with no witness (peers up through it, none long enough before it):
	 * pull the fleet's state.  A RESTART with none skips, below */
	if (live && src && C.rec_rebuild) {
		resync_begin(src, "no peer was up long enough before this "
			"absence to witness it");
		return 0;
	}
	if (live) {
		C.reconcile_on = 0;
		C.st.reconcile_skipped = 1;
		LM_NOTICE("cluster: reconcile SKIPPED - none of the %d live "
			"peer(s) was up and ready through this node's absence, "
			"so the fleet came back together and a \"no\" from any "
			"of them would only mean it lost its copy too.  Nothing "
			"is deleted.  "
			"A key deleted during the outage, if there was one, "
			"stays\n", live);
		if (C.decide_on)
			decide_end(NULL);
	}
	return 0;
}

/* ---- the reconcile's fast path, the returning node's half ------------------
 * A witness replays the deletes planted since the snapshot BEGAN; if whole, no walk is needed. */
static int pc_reconcile_fast = 1;      /* [cluster] reconcile_fast */
static int pc_reconcile_rate_max = RECONCILE_PER_TICK_MAX;

void pc_cluster_set_reconcile_rate_max(int n)
{
	pc_reconcile_rate_max = n < RECONCILE_PER_TICK ? RECONCILE_PER_TICK : n;
}

void pc_cluster_set_reconcile_fast(int on)
{
	pc_reconcile_fast = on ? 1 : 0;
}

/* an expiry sends no tombstone: retain it as a delete at the reaped version, so an older copy is
 * refused on replay and receive.  Eager only; maintenance thread, clretain locks per shard */
void pc_cluster_note_expired(int slot, const char *key, int klen,
		unsigned long long ver, int keep)
{
	pcache_htable_t *ht;
	const char *col;

	if (!C.enabled || !C.cfg_eager || !ver || slot < 0 ||
	        slot >= pc_store_count() || !pc_store_live(slot))
		return;
	ht = pc_store_ht(slot);
	if (!ht || !pc_store_eager_enabled(ht))
		return;
	col = pc_store_name(slot);
	/* counted as an expiry, or a TTL workload's notes read as deletes */
	clretain_note_expiry(col, strlen(col), key, (size_t)klen, ver,
		(unsigned int)(now_ms() / 1000), keep);
}

static void fp_put32(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)(v >> 24);
	p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);
	p[3] = (unsigned char)v;
}

static uint32_t fp_get32(const unsigned char *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
		(uint32_t)p[2] << 8 | p[3];
}

static void fp_fall_back(const char *why)
{
	clrecon_fp_fall_back(&C.fp);
	C.st.reconcile_fast = 0;
	snprintf(C.st.reconcile_fast_why, sizeof C.st.reconcile_fast_why,
		"%s", why);
	LM_NOTICE("cluster: reconcile fast path not taken - %s; asking the "
		"fleet about the replayed records one by one\n", why);
}

/* a live witness: the one the pass found first, else any */
static struct peer *fp_witness(void)
{
	long long now = now_ms();
	struct peer *w = peer_by_node(C.recon.witness);

	if (w && clpeers_live(w, now) && is_witness(w))
		return w;
	return clrecon_first_witness(&C.recon, &C.pt, now, WITNESS_GRACE_MS, NULL);
}

/* "every delete planted since my snapshot began, @age seconds ago" */
static void fp_ask(struct peer *w, unsigned int age)
{
	unsigned char m[13];
	long long now = now_ms();
	uint32_t req = clrecon_fp_asked(&C.fp, w->node, now);

	m[0] = M_TOMB_REQ;
	fp_put32(m + 1, req);
	fp_put32(m + 5, age);
	fp_put32(m + 9, C.self_incarn);
	(void)seal_send(&w->addr, m, sizeof m);
	if (!C.rec_start_ms)
		C.rec_start_ms = now;          /* the pass's clock */
	LM_NOTICE("cluster: reconcile - asking node %d for every delete made "
		"since this node's snapshot began (%u s ago), instead of asking "
		"the fleet about %ld replayed record(s) one by one%s\n",
		w->node, age, pc_recovered_records(),
		C.fp.tries > 1 ? " (asked again)" : "");
}

static unsigned int fp_snapshot_age(void)
{
	unsigned long long snap = pc_recovered_snapshot_wall();
	time_t wall = time(NULL);

	return (unsigned long long)wall > snap ?
		(unsigned int)((unsigned long long)wall - snap) : 0;
}

/* 1 while the fast path holds the pass; 0 when it ended it (FPS_DONE)
 * or gave way to the walk (FPS_WALK).  Reached only with a witness live. */
static int fp_step(void)
{
	struct peer *w;
	char why[128];

	switch (C.fp.state) {
	case FPS_IDLE:
		if (!pc_reconcile_fast) {
			fp_fall_back("reconcile_fast = no");
			return 0;
		}
		if (!pc_recovered_snapshot_wall()) {
			fp_fall_back("no snapshot was loaded - what came back "
				"is the WAL alone");
			return 0;
		}
		if ((unsigned long long)time(NULL) < pc_recovered_snapshot_wall()) {
			fp_fall_back("this node's clock reads earlier than its "
				"own snapshot");
			return 0;
		}
		if (!(w = fp_witness()))
			return 1;
		fp_ask(w, fp_snapshot_age());
		return 1;
	case FPS_WAIT:
		switch (clrecon_fp_wait(&C.fp, now_ms())) {
		case CLRECON_FP_DONE:
			return 0;
		case CLRECON_FP_HOLD:
			return 1;
		case CLRECON_FP_GIVE_UP:
			if (C.fp.n >= 0)
				snprintf(why, sizeof why, "node %d's replay arrived "
					"short %d times (%llu of %lld)", C.fp.node,
					C.fp.tries, C.fp.got, C.fp.n);
			else
				snprintf(why, sizeof why, "node %d did not answer "
					"(a build before 0.6.4 does not)", C.fp.node);
			fp_fall_back(why);
			return 0;
		default:
			break;             /* CLRECON_FP_ASK: again */
		}
		if (!(w = fp_witness()))
			return 1;
		fp_ask(w, fp_snapshot_age());
		return 1;
	default:
		return 0;
	}
}

static void fp_finish(void)
{
	long long now = now_ms();

	C.reconcile_on = 0;
	clrecon_walk_stop(&C.recon);
	rdg_drop();                        /* nothing was asked */
	C.st.reconcile_ms = C.rec_start_ms ?
		(unsigned long long)(now - C.rec_start_ms) : 0;
	C.st.reconcile_fast = 1;
	C.st.reconcile_fast_deletes = C.fp.got;
	C.st.reconcile_fast_why[0] = 0;
	C.st.reconcile_open = 0;
	C.st.reconcile_eta_s = 0;
	clrecon_fp_drop_seen(&C.fp);
	LM_NOTICE("cluster: reconcile finished by the fast path in %llu ms - "
		"node %d replayed %llu delete(s) made since this node's "
		"snapshot began, so the %ld replayed record(s) were not asked "
		"about one by one\n", C.st.reconcile_ms, C.fp.node, C.fp.got,
		pc_recovered_records());
}

/* a numbered delete of the witness's replay */
static void handle_tomb_rpl(const unsigned char *pt, size_t n,
		const pc_addr *from)
{
	extern void pc_wal_del(const char *, const char *, int);
	struct peer *p = clpeers_by_addr(&C.pt, from);
	uint32_t req, seq;

	if (n < 10 || pt[9] != M_TOMBSTONE)
		return;
	req = fp_get32(pt + 1);
	seq = fp_get32(pt + 5);
	/* applied whatever became of the request, as a broadcast would be, and logged
	 * so a second restart does not bring it back */
	if (handle_tombstone(pt + 9, n - 9)) {
		struct clfwd_key fk;
		uint64_t ver;
		char colz[256];

		if (clfwd_tomb_parse(pt + 9, n - 9, &fk, &ver)) {
			memcpy(colz, fk.col, fk.collen);
			colz[fk.collen] = 0;
			pc_wal_del(colz, fk.key, (int)fk.klen);
		}
	}
	if (!p || !clrecon_fp_mine(&C.fp, req,
	        __atomic_load_n(&p->node, __ATOMIC_ACQUIRE), now_ms()))
		return;
	switch (clrecon_fp_number(&C.fp, seq)) {
	case -1:
		fp_fall_back("the witness's replay is larger than this node "
			"numbers");
		return;
	case -2:
		fp_fall_back("no memory to number the witness's replay");
		return;
	default:
		return;
	}
}

/* the witness's verdict, and how many it sent */
static void handle_tomb_done(const unsigned char *pt, size_t n,
		const pc_addr *from)
{
	struct peer *p = clpeers_by_addr(&C.pt, from);
	char why[160];

	if (n < 10 || !p || !clrecon_fp_mine(&C.fp, fp_get32(pt + 1),
	        __atomic_load_n(&p->node, __ATOMIC_ACQUIRE), now_ms()))
		return;
	switch (pt[5]) {
	case TOMBFP_OK:
		clrecon_fp_count(&C.fp, fp_get32(pt + 6));   /* DONE: the next
		                                              * beat ends the pass */
		return;
	case TOMBFP_OFF:
		snprintf(why, sizeof why, "node %d keeps no retained deletes "
			"(tombstone_retain_s = 0)", C.fp.node);
		break;
	case TOMBFP_LOST:
		snprintf(why, sizeof why, "node %d's tombstone_retain_max cap "
			"dropped deletes made since this node's snapshot",
			C.fp.node);
		break;
	case TOMBFP_BEHIND:                 /* it said "older" */
		snprintf(why, sizeof why, "node %d forgot deletes made since this "
			"node's snapshot once all members confirmed them, and this "
			"node's marks are below them (an older state)", C.fp.node);
		break;
	default:
		snprintf(why, sizeof why, "node %d's retained deletes do not "
			"reach back to this node's snapshot (it is older than "
			"tombstone_retain_s there, or than that node's start)",
			C.fp.node);
		break;
	}
	fp_fall_back(why);
}

/* ---- the probe budget, and how far the walk has got -------------------- */

/* the slice's probes (clrecon): the pending table's room and its refusals
 * and timeouts in, the second's budget out to the stats when renewed */
static int reconcile_slice_budget(long long now)
{
	int renewed, n = clrecon_slice_budget(&C.recon, now,
		C.pd.cap - clpend_used(&C.pd), clpend_exhausted(&C.pd),
		C.st.pull_timeouts, pc_reconcile_rate_max, &renewed);

	if (renewed)
		C.st.reconcile_budget = C.recon.budget;
	return n;
}

/* the keys the walk will visit, counted as it starts */
static void reconcile_walk_total(void)
{
	unsigned long long t = 0;
	int c;

	if (C.st.reconcile_walk_total)
		return;
	for (c = 0; c < pc_store_count(); c++) {
		pcache_htable_t *ht = pc_store_ht(c);

		if (pc_store_live(c) && ht && pc_store_eager_enabled(ht))
			t += pcache_ht_entries(ht);
	}
	C.st.reconcile_walk_total = t ? t : 1;
}

/* "finished asking" is not "ruled": a key kept by a silent peer or a refused drop is asked
 * only by another walk; the hold stays on through it.  1 = the pass goes on */
static int reconcile_walk_again(void)
{
	switch (clrecon_walk_end(&C.recon,
	        clpend_used_kind(&C.pd, PC_DONE_RECONCILE))) {
	case CLRECON_END_WAIT:
		return 1;
	case CLRECON_END_AGAIN:
		LM_NOTICE("cluster: reconcile walks again (%d of %d) - %llu key(s) "
			"got no verdict\n", C.recon.rounds, RECONCILE_ROUNDS,
			C.recon.unruled);
		C.recon.unruled = 0;
		C.st.reconcile_rounds++;
		C.rec_col = 0;
		C.rec_cursor = 0;
		C.st.reconcile_walk_total = 0;
		reconcile_walk_total();
		C.st.reconcile_walk_total += C.st.reconcile_walked;
		return 1;
	case CLRECON_END_GIVEUP:
		C.st.reconcile_unruled += C.recon.unruled;
		LM_WARN("cluster: reconcile stops asking - %llu key(s) still have no "
			"verdict after %d more walks: a live peer stayed silent, or was "
			"not fit to answer.  They are kept, so one the fleet deleted stays "
			"readable here\n", C.recon.unruled, RECONCILE_ROUNDS);
		return 0;
	default:
		return 0;
	}
}

/* one slice of probes: from the beat after reconcile_tick's checks, and from reconcile_pump
 * between beats while the beat's verdict stands */
static void reconcile_walk(void)
{
	struct rec_ctx r;
	/* a member that cannot answer hold-checks would leave every slot without its "no":
	 * pull while one is live */
	int hold_ok = pc_cluster_fleet_has(CLMEMB_FEAT_HOLDQ, NULL);

	reconcile_walk_total();
	/* a scan call finishes its window, so a slice can overshoot: the next slice owes it,
	 * keeping the per-second rate at the budget */
	r.budget = clrecon_slice_take(&C.recon, reconcile_slice_budget(now_ms()));
	if (r.budget <= 0)
		return;
	r.seen = 0;
	while (r.budget > 0 && C.rec_col < pc_store_count()) {
		pcache_htable_t *ht = pc_store_ht(C.rec_col);

		r.ht = ht;
		r.col = pc_store_name(C.rec_col);
		r.collen = strlen(r.col);
		r.diff = reconcile_digest_filter(C.rec_col);
		/* not for a read-through collection: a pull there fetches from Redis (pull_serve),
		 * which a hold-check does not */
		r.hold = hold_ok && pc_upstream_binds(r.col, r.collen) != 1;
		/* only an EAGER collection has copies to ask about: a legacy eager-mode config
		 * leaves plain stores, where every peer truthfully answers "absent" */
		if (!ht || !pc_store_eager_enabled(ht)) {
			C.rec_col++;
			C.rec_cursor = 0;
			continue;
		}
		/* the store's cursor carries the table generation: across a resize the walk
		 * restarts, so a key may be asked twice but never skipped.  Keys only. */
		pc_store_scan(ht, &C.rec_cursor, RECONCILE_BUCKETS,
			PCACHE_SCAN_NOVAL, reconcile_cb, &r);
		if (!C.rec_cursor) {
			C.rec_col++;       /* this collection is done */
			continue;
		}
		if (r.budget <= 0) {
			clrecon_owe(&C.recon, r.budget);
			hq_flush();        /* the slice's last batch */
			return;            /* out of budget mid-collection */
		}
	}
	hq_flush();                        /* before "finished asking" */
	if (C.rec_col >= pc_store_count()) {
		if (reconcile_walk_again())
			return;
		C.reconcile_on = 0;
		clrecon_walk_stop(&C.recon);
		rdg_drop();
		C.st.reconcile_ms = C.rec_start_ms ? (unsigned long long)
			(now_ms() - C.rec_start_ms) : 0;
		/* probes ISSUED, not resolved: each drop logs itself as its answer arrives */
		LM_NOTICE("cluster: reconcile pass finished asking - %llu "
			"key(s) probed; any that nobody still has are dropped "
			"as their answers arrive\n", C.st.reconcile_probed);
	}
}

static void reconcile_tick(void)
{
	/* the beat decides afresh whether the slices may walk */
	clrecon_walk_stop(&C.recon);
	C.st.reconcile_open = C.reconcile_on && C.cfg_eager;
	clrecon_progress(&C.recon, now_ms(), C.st.reconcile_open,
		C.fp.state == FPS_WALK, C.st.reconcile_walked,
		C.st.reconcile_walk_total, &C.st.reconcile_rate,
		&C.st.reconcile_eta_s);
	/* the hold outlives the pass until the last answer is in: a record the sweep pushes
	 * meanwhile reaches a peer that already answered "absent" - dropped here, kept there */
	C.rec_hold = C.reconcile_on ||
		clpend_used_kind(&C.pd, PC_DONE_RECONCILE) > 0;
	clwire_set_reconciling(C.rec_hold && C.cfg_eager);
	/* a resync is over once its pull has made this node READY; then a snapshot holds the
	 * fleet's state, and the files from before it go (12ol) */
	if (C.rejoin_on > 0 && pc_node_state() == PC_NST_RECOVERING &&
	    __atomic_load_n(&C.rejoin_src, __ATOMIC_ACQUIRE))
		rejoin_verify();
	if (C.st.resyncing && !C.resync_on &&
	    pc_node_state() >= PC_NST_READY) {
		/* a rejoin ends only by its copy: READY or CONNECTING any other way is not that, and
		 * a stop under way keeps the hold, so no snapshot replaces the files */
		if (C.rejoin_on < 0)
			return;                    /* stopping, to start again from its files */
		if (C.rejoin_on && !clboot_done(&C.bt)) {
			if (pc_node_state() != PC_NST_DRAINING)
				rejoin_lost(NULL);
			return;
		}
		C.st.resyncing = 0;
		C.boot_prefer = 0;
		C.rejoin_on = 0;
		pc_rdb_hold(0);
		C.snap_due = 1;
	}
	if (C.snap_due && pc_rdb_request_save() == 0)
		C.snap_due = 0;
	if (C.decide_on && (!C.reconcile_on || !C.cfg_eager))
		decide_end(NULL);                  /* the pass closed with no copy */
	else if (C.decide_on && now_ms() - C.decide_ms > DECIDE_MAX_MS)
		decide_end("no ruling in 10 s on whether a peer up through this node's absence holds "
			"the fleet's state");
	if (!C.reconcile_on || !C.cfg_eager) {
		clrecon_nowit_stop(&C.recon);
		C.st.reconcile_no_witness_s = 0;
		return;
	}
	if (C.resync_on)
		return;                    /* the drop runs (reconcile_pump) */
	if (C.role == PC_ROLE_JOINING)
		return;                    /* nobody to ask yet */
	if (any_live_peer_cold())
		return;                    /* their "no" is not evidence yet */
	if (!C.recon.witness) {
		if (!reconcile_find_witness())
			return;            /* nobody was watching */
	} else if (!reconcile_witness_live()) {
		C.st.reconcile_deferred++;
		/* and no other peer saw the outage either - every one
		 * restarted since.  Nothing here can end that: say so once */
		if (clrecon_nowit(&C.recon, now_ms(),
		        &C.st.reconcile_no_witness_s))
			LM_WARN("cluster: reconcile has had no witness for %llds - "
				"every live peer restarted after this node went down, "
				"so none can say what was deleted meanwhile; the pass "
				"waits and this node holds its replayed records until "
				"it can finish.  Restart THIS node: its peers have "
				"been up long enough to witness that\n",
				C.st.reconcile_no_witness_s);
		return;                    /* the witness left mid-pass */
	}
	clrecon_nowit_end(&C.recon);
	C.st.reconcile_no_witness_s = 0;
	/* the fast path first: wait while it holds the pass, finish when it ended it, else walk */
	if (fp_step())
		return;
	if (C.fp.state == FPS_DONE) {
		fp_finish();
		return;
	}
	if (!reconcile_digest_step())
		return;                    /* the witness's digests first */
	/* the beat's slice now, the rest between beats (reconcile_pump) */
	clrecon_walk_start(&C.recon, now_ms());
	reconcile_walk();
}

/* the walk's further slices between beats, only while the beat's verdict stands
 * (C.recon.may_walk); a drop re-checks the witness when its answer lands anyway */
static void reconcile_pump(long long now)
{
	if (C.resync_on) {
		resync_step(now);
		return;
	}
	if (!clrecon_pump_due(&C.recon, now, C.reconcile_on))
		return;
	reconcile_walk();
}

/* the standby: preferred first, then the highest (ip,port) - whom the election promotes. Keyed
 * on liveness, not state: a node still recovering its keyspace can hold the small control state */
static int pick_backup(void)
{
	long long now = now_ms();
	const pc_addr *best_addr = NULL;
	unsigned int i;
	int best = 0, best_pref = -1;

	if (C.role != PC_ROLE_MASTER)
		return 0;
	for (i = 0; i < (unsigned int)C.pt.n_peers; i++) {
		struct peer *p = &C.pt.peers[i];
		int id = __atomic_load_n(&p->node, __ATOMIC_ACQUIRE);
		int pref = id == __atomic_load_n(&prefer_id, __ATOMIC_RELAXED);

		if (!id || id == C.node_id || !clpeers_usable(p, now) ||
		    p->nstate == PC_NST_STALLED)   /* the election skips it too */
			continue;
		if (id == C.passed_over_id && now < C.passed_over_until_ms)
			continue;          /* it let a stage time out */
		if (pref > best_pref || (pref == best_pref &&
		        (!best || addr_cmp(&p->addr, best_addr) > 0))) {
			best_pref = pref;
			best = id;
			best_addr = &p->addr;
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

/* a peer turning READY or any member's state change republishes at once (the timer only
 * covers lost datagrams); self is checked apart, or a failing master stays READY in it */
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

/* fold in a peer's term: followed at once, written by pc-maint - the node claims nothing above it
 * until it is on disk (become_master), and a refused write is said by the membership tick */
static void term_seen(uint32_t t)
{
	(void)pc_term_observe(t);
}

/* build, stage to the standby, broadcast only once it acked: the standby is never BEHIND the
 * fleet.  No standby = publish at once; a hung stage is abandoned and its seq never reused. */
/* the workers rank by C.map with no lock: the copy runs between two generation bumps, and a reader
 * that saw the generation odd or changed reads again (cl_map_gen) */
static void map_set(const struct pc_clmap *m)
{
	__atomic_store_n(&C.map_gen, C.map_gen + 1, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	C.map = *m;
	__atomic_store_n(&C.map_gen, C.map_gen + 1, __ATOMIC_RELEASE);
}

/* a mastership claim that could not be stored: no map until claim_term lands it */
static int claim_owed;
static int claim_term(void);

static void clmap_stage(void)
{
	uint32_t seq = 0;

	if (C.role != PC_ROLE_MASTER)
		return;
	/* a claim not stored yet: the term in hand is the last master's, so retried each beat */
	if (claim_owed && !claim_term())
		return;
	if (!pc_term_current()) {
		/* no term claimed: a map under term 0 would be unorderable
		 * against the next master's.  Silence is the safer failure. */
		return;
	}
	if (C.sync.state != PC_CLSYNC_IDLE)
		return;                    /* one in flight already */
	/* pick FIRST, so the change waits on the ack of the standby this map names */
	C.backup_id = pick_backup();
	pc_clsync_set_backup(&C.sync, C.backup_id != 0);
	if (pc_clsync_stage(&C.sync, &seq) != 0)
		return;
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
	n = pc_clmap_encode(&C.staged, msg + CLMAP_FRAME_AT,
		sizeof msg - CLMAP_FRAME_AT);
	if (n <= 0) {
		pc_clsync_abort(&C.sync);
		return;
	}
	msg[0] = M_CLMAP;   /* CLMAP_FRAME_AT: the map follows it */
	if (ctl_send(msg, (size_t)n + 1) != 0)
		return;                    /* retry on the next tick */
	pc_clsync_published(&C.sync);
	spread_note_placeable(&C.staged);
	map_set(&C.staged);
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
		/* pass it over: the next stage (due at once) goes to another live peer or
		 * publishes unsynced, rather than stall on the same silent node */
		if (C.passed_over_id != C.backup_id ||
		        now >= C.passed_over_until_ms)
			LM_WARN("cluster: the standby (node %d) has not "
				"acknowledged a control-state sync in %d ms - "
				"passing it over for %d s\n", C.backup_id,
				CLSYNC_ACK_MS, CLSYNC_PASS_OVER_MS / 1000);
		C.passed_over_id = C.backup_id;
		C.passed_over_until_ms = now + CLSYNC_PASS_OVER_MS;
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

/* @from is admitted: a member by id, or the master we follow.  A node refused at the doors must
 * not bind the fleet via a map's term; a new master's keepalive precedes its map and admits it */
static int sender_admitted(const pc_addr *from)
{
	struct peer *p = clpeers_by_addr(&C.pt, from);

	if (p && __atomic_load_n(&p->node, __ATOMIC_ACQUIRE))
		return 1;
	return C.master_id && addr_eq(from, &C.master_addr);
}

/* admission-gated types are re-sent (COL_SET) or member-only with a timeout as "no answer"; the
 * data plane needs only a known address, as a node restarted without state has no id yet */
static int sender_known(const pc_addr *from)
{
	return clpeers_by_addr(&C.pt, from) != NULL ||
		(C.master_id && addr_eq(from, &C.master_addr));
}

static int sender_gate(int type, const pc_addr *from)
{
	/* the group loops this node's own delete back from its address, which no slot
	 * holds: let through only to be counted at the dispatch (tomb_own) */
	if (type == M_TOMBSTONE && addr_eq(from, &C.self_addr))
		return 1;
	switch (type) {
	case M_COL_SET:
	case M_HOLD_Q:
	case M_SWEEP_DIG:
	case M_MPROBE:
	case M_MPROBE_R:
		return sender_admitted(from);
	case M_CLSYNC_ACK:
		return C.backup_id &&
			clpeers_by_id_from(&C.pt, (unsigned)C.backup_id, from) != NULL;
	/* a lane ask may arrive before the peer's first ALIVE: a slot at the address (even
	 * a purged one's) or the master will do; a stranger could force a lane or draw UDP */
	case M_LANE_FORCE:
	case M_LANE_PROBE:
	case M_LANE_PROBE_R:
		return C.role == PC_ROLE_JOINING || sender_known(from);
	/* the data plane, from a known address: a stranger's PULL_REQ would send the
	 * value, many times its own size, to any address it named */
	case M_PULL_REQ:
	case M_FWD_OP:
	case M_FWD_JSON:
	case M_MIGRATE:
	case M_MIGRATE_MANY:
	case M_REPL_MANY:
	case M_TOMBSTONE:
	case M_PUBLISH:
	case M_DEMOTE:
	case M_ASKFILL:
		/* ... except on a joiner: restarted inside the purge window it knows nobody,
		 * yet the map still forwards its keys' writes to it; refused, each times out */
		return C.role == PC_ROLE_JOINING || sender_known(from);
	default:
		return 1;
	}
}

/* a live slot at another address holds this identity and incarnation: one process has one
 * address, so the frame is a replay or forgery (a restart or clone has a new incarnation) */
static int ident_elsewhere(const unsigned char *ident, unsigned int incarn,
		const pc_addr *from)
{
	long long now = now_ms();
	int i;

	if (!incarn || !memcmp(ident, pd_noident, 16))
		return 0;                      /* an older build: nothing to compare */
	if (incarn == C.self_incarn && !memcmp(ident, C.self_ident, 16))
		return !addr_eq(from, &C.self_addr);
	for (i = 0; i < C.pt.n_peers; i++) {
		struct peer *p = &C.pt.peers[i];

		if (!p->has_ident || p->incarn != incarn ||
		        !clpeers_live(p, now) || addr_eq(&p->addr, from))
			continue;
		if (!memcmp(ident, p->ident, 16))
			return 1;
	}
	return 0;
}

static void handle_clsync(const unsigned char *pt, size_t n,
		const pc_addr *from)
{
	struct pc_clmap m;
	unsigned char ack[CLSYNC_ACK_LEN];
	const char *why = "?";
	const unsigned char *mp, *hp;
	uint32_t ml, hl;

	if (n < CLSYNC_ENV_HDR)
		return;
	if (!sender_admitted(from)) {
		C.map_unadmitted_n++;
		return;
	}
	C.sync_rx_n++;
	if (!clsync_env_parse(pt, n, &mp, &ml, &hp, &hl))
		goto bad;
	if (pc_clmap_decode(mp, ml, &m, &why) != 0)
		goto bad;
	if (pc_clhist_decode(hp, hl, &C.held_hist, &why) != 0)
		goto bad;

	/* held, not adopted: a copy kept for a failover; this node still places from the
	 * broadcast map, so a standby never acts on state the fleet has not been told */
	C.held_valid = 1;
	C.held_map = m;
	C.held_term = m.term;
	C.held_seq = m.seq;
	/* acked but not yet seen published is OWED: if this node is promoted before
	 * the master broadcasts it, publishing it is this node's job */
	pc_clsync_hold(&C.sync, m.term, m.seq);

	/* acked only after it is stored, so the master never
	 * publishes a change this node failed to keep */
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

/* JOINING again through @to, with a fresh token and deadline */
static void join_again(const pc_addr *to)
{
	C.role = PC_ROLE_JOINING;
	/* a joiner sends no ALIVE, so the watchdog would re-send these for ever:
	 * the old claim held members and joiners to a master that admits nobody */
	clwire_beat_store(&C.wr, CLWIRE_ALIVE, NULL, 0);
	clwire_beat_store(&C.wr, CLWIRE_MALIVE, NULL, 0);
	C.join_tok = 0;
	randombytes_buf(&C.join_tok, 8);
	C.join_deadline_ms = now_ms() + JOIN_WAIT_MS;
	clfound_window_open(now_ms());
	C.join_held = 0;
	send_join_req(to);
}

/* give up mastership and re-join through @to: the ONE demotion path, so none can forget the
 * join token or deadline and leave a node neither master nor member */
static void yield_mastership(const pc_addr *to)
{
	prefer_clear();                        /* handed over, or moot */
	C.st.demotions++;
	cid_follow_foreign();
	join_again(to);
}

/* a master that yields to a beacon follows its sender at once, rather than name itself until the
 * winner's next beacon */
static void yield_follow(const pc_addr *from, int their_id)
{
	yield_mastership(from);
	C.master_id = their_id;
	C.master_addr = *from;
	C.master_seen_ms = now_ms();
}

static void handle_clmap(const unsigned char *pt, size_t n,
		const pc_addr *from)
{
	struct pc_clmap m;
	const char *why = "?";

	if (n < 2)
		return;
	/* our own multicast comes back to us: skipped, so received/stale counts stay honest */
	if (addr_eq(from, &C.self_addr))
		return;
	/* before the decode and the term observe: a refused node's term must not even raise ours */
	if (!sender_admitted(from)) {
		if (++C.map_unadmitted_n % 100 == 1)
			LM_WARN("cluster: ignoring a cluster map from %s - it is "
				"not a member this node admits (refused at the "
				"door: config or epoch), so its term does not "
				"bind this fleet\n", pc_addr_str(from));
		return;
	}
	C.map_rx_n++;
	if (pc_clmap_decode(pt + CLMAP_FRAME_AT, n - CLMAP_FRAME_AT,
			&m, &why) != 0) {
		/* counted and named, never silently dropped: a map we cannot
		 * read is a fleet that disagrees about its own format */
		if (++C.map_bad_n % 100 == 1)
			LM_WARN("cluster: refusing a cluster map (%s)\n", why);
		return;
	}
	/* only a master publishes a map, stamped with its own id; the sender's id is its slot's
	 * (or the master's), not from the map's node list: a wildcard bind need not match it */
	{
		struct peer *sp = clpeers_by_addr(&C.pt, from);
		int sid = sp ? __atomic_load_n(&sp->node, __ATOMIC_ACQUIRE) : 0;

		if (!sid && C.master_id && addr_eq(from, &C.master_addr))
			sid = C.master_id;
		if ((int)m.master_id != sid) {
			C.st.sender_refused++;
			if (++C.map_bad_n % 100 == 1)
				LM_WARN("cluster: refusing a cluster map from %s - it "
					"names node %d its master, and the sender is "
					"node %d\n", pc_addr_str(from),
					(int)m.master_id, sid);
			return;
		}
	}
	/* the term is a peer's claim: a map the jump guard refuses is not adopted, or one
	 * implausible term demotes a master and pins the fleet to it (clterm.h) */
	if (!pc_term_plausible(m.term)) {
		if (++C.map_bad_n % 100 == 1)
			LM_WARN("cluster: refusing a cluster map at term %u - "
				"implausible from our term %u\n", m.term,
				pc_term_current());
		return;
	}
	term_seen(m.term);

	if (C.map_valid &&
	        pc_clmap_epoch_cmp(m.term, m.seq, C.map.term, C.map.seq) <= 0) {
		C.map_stale_n++;
		return;                /* not newer: nothing to learn */
	}
	if (pc_term_must_stepdown(C.role == PC_ROLE_MASTER, C.map.term,
	        m.term)) {
		/* rule 3 (clterm.h): a master seeing a higher term steps down at once, no member
		 * count; compared with our published map's term - pc_term_current() holds theirs */
		LM_WARN("cluster: stepping down - a map at term %u outranks "
			"our own term %u (master is node %u)\n",
			m.term, C.map.term, m.master_id);
		yield_mastership(from);
	}
	spread_note_placeable(&m);
	map_set(&m);
	C.map_valid = 1;
	if (cl_unicast) {
		long long nw = now_ms();
		unsigned int mi;

		for (mi = 0; mi < m.nnodes; mi++) {
			pc_addr na;

			memset(&na, 0, sizeof na);
			pc_addr_from_v4(&na, m.node[mi].addr,
				ntohs(m.node[mi].cluster_port));
			if (m.node[mi].addr && m.node[mi].cluster_port)
				clmesh_heard(&na, nw, 0);   /* the map's: confirmed */
		}
	}
	clmap_recheck_usable();
	/* nothing is owed at or below the fleet's epoch, and a later term clears
	 * the debt too: whatever we held belonged to a master that is gone */
	pc_clsync_saw(&C.sync, m.term, m.seq);
	if (C.map_rx_n == 1 || !(C.map_rx_n % 60))
		LM_NOTICE("cluster: holding map epoch (%u,%u), %u node(s), "
			"master %u\n", m.term, m.seq, m.nnodes, m.master_id);
}

/* @to NULL: the fleet and the beat watchdog's copy; else that one address - a master
 * told of a joiner answers it inside the joiner's JOIN_WAIT_MS */
static void send_master_alive_to(const pc_addr *to)
{
	unsigned char msg[CLMEMB_MALIVE_BUF];
	struct clmemb_malive m;
	size_t len;

	memset(&m, 0, sizeof m);
	m.node = C.node_id;
	m.members = clpeers_count_live(&C.pt, now_ms()) + 1;
	m.member_digest = member_digest();
	m.free_mb = self_free_mb();
	m.total_mb = self_total_mb();
	m.live_kb = self_live_kb();
	/* MASTER_ALIVE admits peers too, so it carries the config identity, or a
	 * refused joiner still learns the master and the refusal is one-sided */
	m.mode = C.cfg_mode;
	m.eager = C.cfg_eager;
	m.cfg_digest = config_digest();
	m.cfg2 = 1;
	m.cfg_digest2 = config_digest2();
	/* the term rides the 1 Hz keepalive, not only the map: the split-brain cure reads
	 * this message.  Appended, so a peer that stops reading at 35 is unaffected */
	m.term = pc_term_current();
	m.epoch = pc_cluster_epoch();
	m.committed = pc_cluster_epoch();            /* the fleet's epoch */
	len = clmemb_malive_write(msg, sizeof msg, M_MASTER_ALIVE, &m);
	if (to) {
		seal_send(to, msg, len);
		return;
	}
	ctl_send(msg, len);
	clwire_beat_store(&C.wr, CLWIRE_MALIVE, msg, len);
}

static void send_master_alive(void)
{
	send_master_alive_to(NULL);
}

/* peer-thread-only 1 Hz emitter for long duties (migration ack windows, the victim walk):
 * emission only, no membership decisions, so safe anywhere on the peer thread */
static void beat_pump(void)
{
	prog(PC_PROG_PUMP);                /* a long duty, still moving */
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

	ctl_send(msg, clmemb_goodbye_write(msg, sizeof msg, M_GOODBYE,
		C.node_id, C.self_incarn));
}

/* master: allocate the lowest unused node id (rejoiners keep theirs) */
static int id_taken(int id)
{
	int i;

	if (id == C.node_id)
		return 1;
	if (clres_holds(&reserved, id, now_ms()))   /* held for a departed member */
		return 1;
	for (i = 0; i < C.pt.n_peers; i++)
		if (C.pt.peers[i].node == id)
			return 1;
	return 0;
}

/* honour the joiner's proposal (derived from its identity) when free: 1023 slots collide near 38
 * nodes, so the master arbitrates and two identities never share an id.  @preferred 0 = none */
static int alloc_node_id(int preferred)
{
	int id;

	if (preferred > 0 && preferred <= PC_CL_NODE_MAX && !id_taken(preferred))
		return preferred;
	if (preferred > 0)
		LM_NOTICE("cluster: node id %d proposed but taken - "
			"assigning another\n", preferred);
	for (id = 1; id <= PC_CL_NODE_MAX; id++)
		if (!id_taken(id))
			return id;
	return 0;
}

static void send_assign(const pc_addr *to, uint64_t tok, int your_id)
{
	send_member_list(M_ASSIGN, to, tok, your_id);
	C.st.assigns++;
}

/* claim a term above everything this node has SEEN, not its own last; persisted before returned,
 * so a master that dies cannot reissue it with other content.  0 while it cannot be stored */
static int claim_term(void)
{
	uint32_t t = pc_term_claim();

	if (!t) {
		/* nowhere to write it: serve as master, but publish no map - a map
		 * under a term we cannot remember is one we could contradict */
		if (!claim_owed)
			LM_CRIT("cluster: could not persist a mastership term "
				"- serving as master WITHOUT publishing a "
				"cluster map until it can (check the state directory)\n");
		claim_owed = 1;
		return 0;
	}
	claim_owed = 0;
	{
		uint32_t owed_t = 0, owed_s = 0;
		int owed = pc_clsync_owed(&C.sync, &owed_t, &owed_s);

		/* a promoted standby starts from the identity history it was handed:
		 * without it every returning node looks NEW and is backfilled */
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

		/* the staging machine issues under this term: a sequence carried
		 * over would be ordered against maps that no longer matter */
		pc_clsync_init(&C.sync, t, 0);

		/* acked but never published = COMMITTED: republished under the NEW
		 * term, since reissuing the old one would put two maps at one epoch */
		if (owed) {
			spread_note_placeable(&C.held_map);
			map_set(&C.held_map);
			C.map_valid = 1;
			LM_NOTICE("cluster: republishing a change the "
				"previous master committed but never "
				"broadcast (was %u,%u)\n",
				owed_t, owed_s);
		}
		LM_NOTICE("cluster: claimed mastership term %u\n", t);
	}
	return 1;
}

static void become_master(int founded)
{
	static long long said_ms;
	uint32_t unsaved = pc_term_unpersisted();

	/* its claim's floor is not on disk: after a restart it could claim the term it follows */
	if (unsaved) {
		if (!said_ms || now_ms() - said_ms >= 10000) {
			said_ms = now_ms();
			LM_CRIT("cluster: not taking mastership - term %u, which this "
				"node follows, is not persisted (%s); retrying it every "
				"beat\n", unsaved, strerror(pc_term_store_errno()));
		}
		if (founded)
			C.join_deadline_ms = now_ms() + 1000;  /* re-announcing meanwhile */
		return;
	}
	/* the term it claims is written by pc-maint before it is used: the caller comes back */
	if (!pc_term_claim_ready())
		return;
	/* found under the id this identity always proposes, so a whole-fleet restart
	 * keeps its ids; with no durable identity the proposal is 0 and the scan gives 1 */
	if (!C.node_id)
		C.node_id = alloc_node_id(C.proposed_id);
	if (!C.node_id)
		C.node_id = 1;
	/* a master with no cluster id founds one before its first map or keepalive (which
	 * carry it), unless it hears live members of one: then it founds into theirs */
	{
		unsigned char h[CLWIRE_CID_LEN];
		long long t, now = now_ms();

		pthread_mutex_lock(&cid_heard_mx);
		memcpy(h, cid_heard, sizeof h);
		t = cid_heard_ms;
		pthread_mutex_unlock(&cid_heard_mx);
		if (t && now - t < CID_HEARD_FRESH_MS)
			pc_cid_adopt(h, now);
		pc_cid_mint(now);              /* no-op if it now has one */
	}
	C.role = PC_ROLE_MASTER;
	cladmit_master_begin(now_ms());        /* the admission gate counts from here */
	/* founding or winning an election settles membership */
	if (founded && any_eager() && !self_entries() &&
	        pc_recovered_records() == 0 &&
	        !clboot_done(&C.bt) &&
	        clpeers_count_live(&C.pt, now_ms()) > 0) {
		/* a founder with live peers (restarted before its death was noticed) holds
		 * nothing they hold: it pulls like a joiner before it serves */
		pc_node_state_set(PC_NST_RECOVERING);
		clboot_arm(&C.bt, now_ms());
		LM_NOTICE("cluster: founding with %d live peer(s) while holding "
			"nothing - pulling a bootstrap before reporting ready\n",
			clpeers_count_live(&C.pt, now_ms()));
	} else if (pc_node_state() == PC_NST_RECOVERING && C.bt.pick_ms &&
	           !clboot_done(&C.bt)) {
		/* elected mid-bootstrap: READY means complete, so boot_tick still decides */
		LM_NOTICE("cluster: taking mastership while its bootstrap pull is out - "
			"RECOVERING until it settles\n");
	} else if (!C.st.resyncing)        /* a copy under way: its pull reports ready */
		pc_node_state_set(PC_NST_READY);
	C.master_id = C.node_id;
	C.master_addr = C.self_addr;
	C.st.elections++;
	claim_owed = 0;
	(void)claim_term();
	LM_NOTICE("cluster: %s as node %d (%d live peer(s))\n",
		founded ? "founded the cluster - master" :
		"elected master", C.node_id, clpeers_count_live(&C.pt, now_ms()));
	send_master_alive();
	/* stage the first map at once rather than waiting a tick - a fleet
	 * should not wait to learn who its master is */
	clmap_stage();
}

/* once the published map names the preferred standby (it acked the sync), tell it to promote;
 * this master steps down on its beacon */
static void handover_tick(long long now)
{
	static long long sent_ms;
	int id = __atomic_load_n(&prefer_id, __ATOMIC_ACQUIRE);
	unsigned char m[M_HANDOVER_LEN];
	struct pc_wcur c;
	struct peer *p;

	if (!id || now - sent_ms < 1000)
		return;
	sent_ms = now;
	if (now - prefer_ms > HANDOVER_GIVEUP_MS || !(p = peer_by_node(id))) {
		LM_WARN("cluster: the handover to node %d did not happen in %d s "
			"- this node stays master\n", id, HANDOVER_GIVEUP_MS / 1000);
		prefer_clear();
		return;
	}
	if (!C.map_valid || C.map.backup_id != id) {
		clmap_stage();                 /* no-op while one is in flight */
		return;
	}
	pc_wcur_init(&c, m, sizeof m);
	pc_w8(&c, M_HANDOVER);
	pc_w32(&c, C.map.term);
	if (c.ok && seal_send(&p->addr, m, sizeof m) == 0)
		LM_NOTICE("cluster: handing mastership to node %d, the synced "
			"standby (term %u)\n", id, C.map.term);
}

/* ---- T600: a collection asked for whole (M_ASKFILL) ---- */
static long long askfill_at[PC_MAX_COLLECTIONS];  /* when to ask, 0 = not; workers set it */

static int store_index(const char *col, size_t collen)
{
	int c;

	for (c = 0; c < pc_store_count(); c++)
		if (pc_store_live(c) && strlen(pc_store_name(c)) == collen &&
		    !memcmp(pc_store_name(c), col, collen))
			return c;
	return -1;
}

/* the drop leaves a negative version for 2 s (pc_verb_forget_local): the peers' copies of the
 * same version must arrive after it, so the ask waits 3 s */
void pc_cluster_ask_fill(const char *col, size_t collen)
{
	int c;

	if (!C.enabled || (c = store_index(col, collen)) < 0 ||
	    !pc_store_eager_enabled(pc_store_ht(c)))
		return;
	__atomic_store_n(&askfill_at[c], now_ms() + 3000, __ATOMIC_RELEASE);
}

static void askfill_tick(long long now)
{
	unsigned char m[2 + 255];
	struct pc_wcur w;
	int c, up;

	for (c = 0; c < pc_store_count(); c++) {
		long long t = __atomic_load_n(&askfill_at[c], __ATOMIC_ACQUIRE);
		const char *cn = pc_store_name(c);
		size_t cl = strlen(cn);

		if (!t || now < t || cl > 255)
			continue;
		up = clpeers_count_live(&C.pt, now);
		if (!up)
			continue;              /* nobody yet: asked once one is up */
		pc_wcur_init(&w, m, sizeof m);
		pc_w8(&w, M_ASKFILL);
		pc_w8(&w, (unsigned char)cl);
		pc_wraw(&w, cn, cl);
		if (!w.ok)
			continue;
		(void)clsend_live(&C.pt, m, 2 + cl, now);
		__atomic_store_n(&askfill_at[c], 0, __ATOMIC_RELEASE);
		LM_NOTICE("cluster: asked %d peer(s) for collection '%s' whole - its "
			"records written through went with the upstream tracking gap\n",
			up, cn);
	}
}

static void handle_askfill(const unsigned char *pt, size_t n, const pc_addr *from)
{
	struct peer *p = clpeers_by_addr(&C.pt, from);
	size_t cl;
	int c;

	if (n < 2 || !p || !p->node || !clpeers_live(p, now_ms()))
		return;
	cl = pt[1];
	if (n < 2 + cl || (c = store_index((const char *)pt + 2, cl)) < 0 ||
	    !pc_store_eager_enabled(pc_store_ht(c)) || p->askfill[c])
		return;
	p->askfill[c] = 1;
	p->repl_cursor[c] = 0;             /* a whole cycle from its first bucket */
	p->repl_bfcycle[c] = 0;
	LM_NOTICE("cluster: node %d asks for collection '%s' whole (its upstream tracking "
		"had a gap) - sending every record held here, copies included\n",
		p->node, pc_store_name(c));
}

static void handle_handover(const unsigned char *pt, size_t n,
		const pc_addr *from)
{
	struct pc_rcur c;

	if (n < M_HANDOVER_LEN || C.role != PC_ROLE_MEMBER ||
	    !addr_eq(from, &C.master_addr) || !C.map_valid ||
	    C.map.backup_id != C.node_id)
		return;
	pc_rcur_init(&c, pt, n);
	(void)pc_ru8(&c);
	if (pc_ru32(&c) != C.map.term)
		return;                        /* a replay from an older map */
	LM_NOTICE("cluster: node %d hands mastership to this node, its standby\n",
		C.master_id);
	become_master(0);
}

/* is this identity claimed by a member still ALIVE?  Liveness is the test, never the address:
 * a clone can come from any address, and a dead node's slot ages out to free the identity */
static int ident_claimed_live(const unsigned char *ident,
		unsigned int incarn, const pc_addr *from)
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
		/* a member re-joins on every mastership move, and refusing it deadlocks the fleet:
		 * same identity and incarnation (per process, unpersisted) = the same process */
		if (incarn && p->incarn == incarn)
			continue;
		return 1;
	}
	return 0;
}

static void send_join_rej(const pc_addr *to, uint64_t tok,
		int reason)
{
	unsigned char msg[CLMEMB_REJ_LEN];

	seal_send(to, msg,
		clmemb_rej_write(msg, sizeof msg, M_JOIN_REJ, tok, reason));
}

/* "your join is held": a duplicate is not condemned while the incumbent may just have died,
 * only once it heartbeats through a purge window; each held retry renews the joiner's patience */
static void send_join_wait(const pc_addr *to, uint64_t tok)
{
	unsigned char msg[CLMEMB_TOK_LEN];

	seal_send(to, msg,
		clmemb_tok_write(msg, sizeof msg, M_JOIN_WAIT, tok));
}

static void handle_join_wait(const unsigned char *pt, size_t n,
		const pc_addr *from)
{
	struct clmemb_wait w;
	uint64_t tok;
	long long until;

	if (C.role != PC_ROLE_JOINING || !clmemb_wait_parse(pt, n, &tok, &w))
		return;
	if (tok != C.join_tok)
		return;                        /* not our join */
	if (w.reason == CLMEMB_WAIT_QUEUED || w.reason == CLMEMB_WAIT_ELECTING) {
		until = clfound_wait(&w, from, now_ms());   /* the settle window restarts */
		if (until > C.join_deadline_ms)
			C.join_deadline_ms = until;
		return;
	}
	until = now_ms() + JOIN_HOLD_MS;
	if (until > C.join_deadline_ms)
		C.join_deadline_ms = until;
	if (!C.join_held) {
		C.join_held = 1;
		LM_NOTICE("cluster: the master at %s is holding this join - an "
			"identity like ours is still counted live there.  "
			"Waiting for it to lapse, not founding; a refusal "
			"follows if it never does\n", pc_addr_str(from));
	}
}

/* a duplicate-identity refusal stops the daemon: a clone corrupts the cluster's shared state,
 * unlike a failed AUTH, which is quarantined and never fatal */
static int join_over_limit(const pc_addr *from, int *limit);

static void handle_join_rej(const unsigned char *pt, size_t n,
		const pc_addr *from)
{
	uint64_t tok;
	int reason, have_reason;

	if (!clmemb_rej_parse(pt, n, &tok, &reason, &have_reason))
		return;
	/* a refusal answers a join only: a member or master must not shut down on a JOIN_REJ
	 * carrying its old token, which a holder of the secret could read off its JOIN_REQ */
	if (C.role != PC_ROLE_JOINING)
		return;
	if (tok != C.join_tok)
		return;
	if (have_reason && reason == PC_REJ_DUP_IDENT) {
		LM_ERR("cluster: the master at %s REFUSED this node - its "
			"identity %s is already held by a live member, so "
			"this daemon is a clone of one.  Shutting down: two "
			"nodes sharing an identity corrupt membership.  Fix: "
			"delete node-identity in the state directory (a fresh "
			"one is generated on the next start).\n",
			pc_addr_str(from), C.self_ident_hex);
	} else if (have_reason && reason == PC_REJ_FULL) {
		LM_ERR("cluster: the master at %s REFUSED this node - the fleet "
			"is at its member limit ([cluster] max_members, or the "
			"build's 256 peers).  Shutting down rather than founding "
			"a second cluster beside it.  Raise max_members on the "
			"fleet, or point this node at another group.\n",
			pc_addr_str(from));
	} else {
		LM_ERR("cluster: the master at %s refused this node "
			"(reason %d).  Shutting down.\n",
			pc_addr_str(from), reason);
	}
	/* the ordinary shutdown path: the main thread joins the workers and tears
	 * down, rather than this thread calling exit() under them */
	kill(getpid(), SIGTERM);
}

void handle_join_req(const unsigned char *pt, size_t n,
		const pc_addr *from)
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
	/* the wire epoch, before the config digest: a peer that means something else by
	 * these bytes cannot be reasoned with about collections; absent = 0, refused */
	if (!epoch_ok((jq.have & CLMEMB_J_EPOCH) ? jq.epoch : 0, from, "join")) {
		C.st.epoch_refused++;
		peer_forget(from);
		return;
	}
	if (jq.have & CLMEMB_J_CFG) {
		int have2 = (jq.have & CLMEMB_J_CFG2) != 0;
		uint64_t theirs = have2 ? jq.cfg_digest2 : jq.cfg_digest, ours;

		if (digest_differs(have2, jq.cfg_digest2, jq.cfg_digest, &ours) &&
		    C.cfg_authoritative) {
			/* refuse the JOIN itself - no id is assigned, so
			 * the node never becomes a member at all */
			shout_mismatch(from, jq.mode, jq.eager, theirs, ours,
				(jq.have & CLMEMB_J_WAL) ? jq.wal : 0);
			C.cfg_refused++;
			peer_forget(from);
			return;
		}
	}
	if (C.role == PC_ROLE_MASTER) {
		struct peer *p;
		int id, lim;

		if (cladmit_hold(pt, n, from, tok))   /* in turn, told its place meanwhile */
			return;
		/* a fleet at its ceiling refuses a joiner out loud: a joiner reads
		 * silence as "no master" and founds a second cluster */
		/* identity and incarnation live at another address: a replay or forgery,
		 * which answered would make a slot at its source and draw an ASSIGN there */
		if ((jq.have & CLMEMB_J_IDENT) &&
		    ident_elsewhere(jq.ident, j_incarn, from)) {
			C.st.sender_refused++;
			if (pc_logrl_ok(LRL_SENDER_JOIN, from))
				LM_NOTICE("cluster: refused a join from %s - its "
					"identity and incarnation are a live member's "
					"at another address\n", pc_addr_str(from));
			return;
		}
		if (join_over_limit(from, &lim) || !(p = peer_upsert(from))) {
			static long long said_ms;

			if (now_ms() - said_ms > 10000) {
				said_ms = now_ms();
				LM_WARN("cluster: REFUSING join from %s - the "
					"fleet is at its member limit (%d, this "
					"node included; [cluster] max_members)\n",
					pc_addr_str(from), lim);
			}
			send_join_rej(from, tok, PC_REJ_FULL);
			C.st.joins_rejected++;
			return;
		}
		if ((jq.have & CLMEMB_J_IDENT) &&
				ident_claimed_live(jq.ident, j_incarn, from)) {
			char hx[33];

			ident_hex(jq.ident, hx);
			if (!clpeers_dup_persisted(&C.pt, from, now_ms())) {
				/* ambiguous: the predecessor may not have aged out; held,
				 * not ignored - silence makes a joiner found its own cluster */
				if (pc_logrl_ok(LRL_JOIN_HELD, from))
					LM_WARN("cluster: join from %s claims identity "
						"%s, still held by a live member - "
						"holding it to see whether that member is "
						"actually gone\n",
						pc_addr_str(from), hx);
				send_join_wait(from, tok);
				return;
			}
			if (pc_logrl_ok(LRL_JOIN_CLONE, from))
				LM_ERR("cluster: REFUSING join from %s - identity %s "
					"is held by a member that has kept "
					"heartbeating throughout.  A node that had "
					"died could not, so this daemon is a CLONE of "
					"it: its identity file was copied with the "
					"machine.  Delete node-identity in its state "
					"directory and restart it.\n",
					pc_addr_str(from), hx);
			send_join_rej(from, tok, PC_REJ_DUP_IDENT);
			C.st.joins_rejected++;
			return;
		}
		clpeers_dup_forget(&C.pt, from);
		id = p->node;
		if (id && (id == C.node_id || clpeers_id_elsewhere(&C.pt, id, from, now_ms())))
			id = 0;                /* a merge left it two holders: this one gets another */
		if (!id) {
			/* a returning identity, or a replacement at its address, gets the
			 * reserved id back over its proposal, unless a live member holds it */
			const char *how = "";
			struct in_addr home;
			int rid;

			/* of two claimants to a lapsed identity, the one at its last home is the
			 * restart; the other is held, then refused as a duplicate */
			if ((jq.have & CLMEMB_J_IDENT) &&
			        clres_home(&reserved, jq.ident, now_ms(), &home) &&
			        home.s_addr != from->in4.sin_addr.s_addr &&
			        clpeers_dup_seen_recent(&C.pt, &home, now_ms())) {
				char hx[33], hs[24];

				ident_hex(jq.ident, hx);
				snprintf(hs, sizeof hs, "%s", inet_ntoa(home));
				if (pc_logrl_ok(LRL_JOIN_HOME, from))
					LM_WARN("cluster: join from %s claims identity %s "
						"whose last home, %s, is itself re-joining - "
						"holding this one; the copy at a new address "
						"waits for the original\n",
						pc_addr_str(from), hx, hs);
				send_join_wait(from, tok);
				return;
			}
			rid = clres_take(&reserved,
				(jq.have & CLMEMB_J_IDENT) ? jq.ident : NULL,
				&from->in4.sin_addr, now_ms(), &how);

			if (rid && !id_taken(rid)) {
				id = rid;
				LM_NOTICE("cluster: %s returns as node %d (reserved, "
					"matched by %s)\n", pc_addr_str(from), id,
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
	if (C.role == PC_ROLE_MEMBER)
		cladmit_electing(from, tok, C.master_seen_ms);   /* a fleet without a master */
	if (C.role == PC_ROLE_JOINING)
		clfound_note(from, jq.have & CLMEMB_J_IDENT ? jq.ident : NULL, now_ms());
}

/* an eager member holding nothing pulls the keyspace first - READY means complete */
int cl_will_bootstrap(void)
{
	return any_eager() && !self_entries() && pc_recovered_records() == 0 && !clboot_done(&C.bt);
}

/* a member at last: RECOVERING for a bootstrap, or READY */
void joined_state(int members)
{
	if (C.st.resyncing) {              /* a copy under way: its pull reports ready */
		LM_NOTICE("cluster: joined as node %d (master is node %d, %d "
			"member(s)) - still copying the fleet's state\n", C.node_id,
			C.master_id, members);
		return;
	}
	/* the candidates come from heartbeats not all sent yet: picked on the tick, bounded */
	if (cl_will_bootstrap()) {
		pc_node_state_set(PC_NST_RECOVERING);
		clboot_arm(&C.bt, now_ms());
		LM_NOTICE("cluster: joined as node %d (master is node %d, "
			"%d member(s)) holding nothing - pulling a bootstrap "
			"before reporting ready\n", C.node_id, C.master_id, members);
	} else if (C.reconcile_on && C.cfg_eager && !self_test_reconcile) {
		/* a survivor's copy may replace what it replayed: not served before the pass rules */
		pc_node_state_set(PC_NST_RECOVERING);
		C.decide_on = 1;
		C.decide_ms = now_ms();
		LM_NOTICE("cluster: joined as node %d (master is node %d, %d member(s)) holding "
			"%ld replayed record(s) - RECOVERING until it knows whether a peer up "
			"through its absence holds the fleet's state\n", C.node_id, C.master_id,
			members, pc_recovered_records());
	} else {
		pc_node_state_set(PC_NST_READY);
		LM_NOTICE("cluster: joined as node %d (master is node %d, "
			"%d member(s))\n", C.node_id, C.master_id, members);
	}
}

static void handle_assign(const unsigned char *pt, size_t n,
		const pc_addr *from)
{
	struct clmemb_assign as;
	struct clmemb_member mr;
	int i;

	if (!clmemb_assign_parse(pt, n, &as))
		return;
	/* an ASSIGN answers a join, so only a JOINING node takes one: a late duplicate,
	 * a rival master's or a replay would reset a member's ids and master address */
	if (C.role != PC_ROLE_JOINING) {
		cladmit_assign_part(pt, n, &as, from);   /* a later part of its own */
		return;
	}
	if (as.tok != C.join_tok)
		return;
	/* a master hands out 1..PC_CL_NODE_MAX (alloc_node_id); an id outside
	 * that is not an assignment it made - 0 reads as "no id" everywhere */
	if (as.your_id < 1 || as.your_id > PC_CL_NODE_MAX)
		return;
	/* the cluster this node now belongs to is the master's */
	if (rx_hdr)
		pc_cid_adopt(CLWIRE_HDR_CID(rx_hdr), now_ms());
	C.node_id = as.your_id;
	C.st.node_id = C.node_id;
	C.master_id = as.master_id;
	C.master_addr = *from;
	C.master_seen_ms = now_ms();
	/* clmemb_member_at bounds each record against the REAL length, so a
	 * count that overstates the frame stops at the last whole record */
	for (i = 0; i < as.count && clmemb_member_at(pt, n, i, &mr); i++) {
		pc_addr pa;
		struct peer *p;

		memset(&pa, 0, sizeof pa);
		pc_addr_from_v4(&pa, mr.addr.s_addr, ntohs(mr.port));
		if (addr_eq(&pa, &C.self_addr))
			continue;
		if (cl_unicast)
			clmesh_heard(&pa, now_ms(), 0);   /* the master's word */
		p = peer_upsert(&pa);
		if (p)
			peer_publish(p, mr.node, mr.free_mb,
				mr.total_mb, mr.live_kb,
				pd_noident, 0, 0, PC_START_UNKNOWN);
	}
	if (C.role != PC_ROLE_MEMBER) {
		struct clmemb_trailer t;

		C.role = PC_ROLE_MEMBER;
		if (clmemb_trailer_parse(pt, n, as.count, &t) && (t.flags & CLMEMB_ASSIGN_CONNECT)) {
			/* admitted, and every path to every member proved before it serves */
			pc_node_state_set(PC_NST_CONNECTING);
			cladmit_connect_begin(now_ms(), as.tok);
			LM_NOTICE("cluster: admitted as node %d (master is node %d, %d member(s), part %d "
				"of %d) - CONNECTING: proving every link before it serves\n", C.node_id,
				C.master_id, as.count + 1, t.part + 1, t.parts);
		} else {
			joined_state(as.count + 1);
		}
	}
	/* the token is spent: a fresh random one (never 0, which a forged reply could
	 * carry) matches nothing until a yield makes this node join again */
	do
		randombytes_buf(&C.join_tok, sizeof C.join_tok);
	while (!C.join_tok);
	/* announced at once, not on the next beat: members take this node's pulls and
	 * forwards only once they hold its slot, which this ALIVE makes */
	send_alive();
}

static void handle_alive(const unsigned char *pt, size_t n,
		const pc_addr *from)
{
	struct clmemb_alive a;
	struct peer *p;

	if (addr_eq(from, &C.self_addr) || !clmemb_alive_parse(pt, n, &a))
		return;
	/* an ALIVE is the other door into a fleet (a member never re-sends JOIN_REQ): same rule */
	if (!epoch_ok((a.have & CLMEMB_A_EPOCH) ? a.epoch : 0, from, "keepalive")) {
		C.st.epoch_refused++;
		peer_forget(from);
		return;
	}
	C.st.hb_seen++;
	/* an ALIVE comes from a member or a master, and every one holds
	 * an id a master handed out; and its identity is at one address */
	if (a.node < 1 || a.node > PC_CL_NODE_MAX ||
	    ((a.have & CLMEMB_A_IDENT) && ident_elsewhere(a.ident, a.incarn, from))) {
		C.st.sender_refused++;
		if (pc_logrl_ok(LRL_SENDER_ALIVE, from))
			LM_NOTICE("cluster: refused a keepalive from %s as node %d - "
				"%s\n", pc_addr_str(from), a.node,
				a.node < 1 || a.node > PC_CL_NODE_MAX ?
				"an id no master hands out" :
				"its identity is live at another address");
		return;
	}
	/* an id live at another address is counted and logged, not refused: the halves
	 * of a split can each admit one under it, and refusing would hide that */
	{
		struct peer *q = clpeers_by_id_live(&C.pt, a.node, now_ms());

		if (q && !addr_eq(&q->addr, from)) {
			C.st.id_collisions++;
			if (pc_logrl_ok(LRL_ID_COLLISION, from))
				LM_WARN("cluster: node %d is claimed by %s and by %s - two "
					"members under one id: the halves of a split each "
					"admitted a joiner, or a forgery\n", a.node,
					pc_addr_str(&q->addr), pc_addr_str(from));
		}
	}
	if (a.have & CLMEMB_A_CFG) {
		int have2 = (a.have & CLMEMB_A_CFG2) != 0;
		uint64_t ours;

		if (digest_differs(have2, a.cfg_digest2, a.cfg_digest, &ours) &&
		    C.cfg_authoritative) {
			/* differing collection config is not admitted, so the fleet splits visibly;
			 * only a node that declared [cluster] collections can enforce it */
			shout_mismatch(from, a.mode, a.eager, have2 ? a.cfg_digest2
				: a.cfg_digest, ours, 0);
			C.cfg_refused++;
			peer_forget(from);
			return;
		}
	} else if (C.cfg_authoritative) {
		if (pc_logrl_ok(LRL_NO_DIGEST, from))
			LM_WARN("cluster: peer %s sends no config digest (older "
				"build) - its collection config CANNOT be checked\n",
				pc_addr_str(from));
	}
	/* a member that cannot speak the fleet's committed epoch is refused by name, before it is
	 * taken in: it joined before the commit, or an old build is back */
	if (a.have & CLMEMB_A_CFGSIG) {
		int k, e = pc_cluster_epoch();

		for (k = 0; k < a.ncfg && a.cfg_name[k] != PC_FACT_EPOCHS; k++)
			;
		if (k < a.ncfg && (e < (int)(a.cfg_val[k] >> 8 & 0xff) ||
		                   e > (int)(a.cfg_val[k] & 0xff))) {
			if (pc_logrl_ok(LRL_EPOCH_ALIVE, from))
				LM_ERR("cluster: REFUSING node %d (%s) - it speaks wire "
					"epochs %d..%d and the fleet is committed to %d; "
					"upgrade it\n", a.node, pc_addr_str(from),
					(int)(a.cfg_val[k] >> 8 & 0xff),
					(int)(a.cfg_val[k] & 0xff), e);
			C.st.epoch_refused++;
			peer_forget(from);
			return;
		}
	}
	/* this node's id at another address: two halves of a merge each handed it out.
	 * The higher identity re-joins for a new one; a master keeps its own */
	if (a.node == C.node_id && C.role == PC_ROLE_MEMBER && (addr_eq(from, &C.master_addr) ||
	    ((a.have & CLMEMB_A_IDENT) && memcmp(C.self_ident, a.ident, 16) > 0))) {
		LM_WARN("cluster: node %d is claimed by %s too - re-joining for an id "
			"of this node's own\n", a.node, pc_addr_str(from));
		join_again(&C.master_addr);
	}
	p = peer_upsert(from);
	if (p) {
		p->alive_ms = now_ms();            /* before the publish, which may admit */
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
			p->client_port = a.client_port;
		/* a peer that predates the field reads as READY - which
		 * is exactly how it behaves, since it has no state to be in */
		{
			int ns = (a.have & CLMEMB_A_STATE) ? a.nstate
				: PC_NST_READY;

			/* a RECOVERING member is silent on pulls, so lookups meanwhile learned
			 * only the other holder: forget them when it turns READY */
			if (ns == PC_NST_READY && p->nstate != PC_NST_READY)
				holders_forget(a.node, "is ready");
			p->nstate = ns;
		}
		/* an older build never sets the bit - reads as not */
		p->reconciling = rx_hdr &&
			(CLWIRE_HDR_FLAGS(rx_hdr) & CLWIRE_F_RECONCILING) != 0;
		/* a member that says it reconciles but not who its witnesses
		 * are is unknown (-1), so a restart waits for it */
		p->sole_witness = (a.have & CLMEMB_A_SOLEWIT)
			? a.sole_witness : -1;
		/* its marks belong to its incarnation; a keepalive without them (an older
		 * build) confirms nothing, so its marks are cleared */
		p->acks = (a.have & CLMEMB_A_FEAT32) &&
			(a.features32 & CLMEMB_F32_ACKS);
		if (a.have & CLMEMB_A_IDENT) {
			uint64_t mo = fnv1a64(a.ident, CLMEMB_IDENT_LEN);
			int i;

			int rs = pc_ack_fleet_member(mo, a.incarn);
			long long now = now_ms(), was;

			/* silent for a purge or more and back with its incarnation:
			 * cut off, not restarted (a restart is the reconcile's case) */
			was = rs >= 0 ? pc_ack_fleet_heard(mo, now) : 0;
			if (rs == 0 && p->acks && C.cfg_eager && was &&
			    now - was >= PEER_PURGE_MS)
				acks_back_check(p, mo, was, now);
			if (rs >= 0) {
				if (!p->acks)
					pc_ack_fleet_clear(mo);
				else if (a.have & CLMEMB_A_MARKS)
					for (i = 0; i < a.nmarks; i++)
						if (pc_ack_fleet_report(mo,
						        a.marks[i].oid,
						        a.marks[i].incarn,
						        a.marks[i].mark, now) == 1)
							C.st.acks_lowered++;
			}
		}
		/* its settings; absent from older builds */
		p->ncfg = (a.have & CLMEMB_A_CFGSIG) ? a.ncfg : 0;
		if (p->ncfg) {
			memcpy(p->cfg_name, a.cfg_name,
				(size_t)p->ncfg * sizeof p->cfg_name[0]);
			memcpy(p->cfg_val, a.cfg_val,
				(size_t)p->ncfg * sizeof p->cfg_val[0]);
		}
		p->nreason = (a.have & CLMEMB_A_REASON) ? a.nreason : PC_NRE_NONE;
		p->mem_tier = (a.have & CLMEMB_A_TIER)
			? a.mem_tier : 0;                   /* 0 = not reported */
		p->resp_port = (a.have & CLMEMB_A_RESPPORT)
			? a.resp_port : 0;
		p->features = (a.have & CLMEMB_A_FEATURES)
			? (unsigned char)a.features : 0;
		p->http_port = (a.have & CLMEMB_A_HTTP) ? a.http_port : 0;
		p->boot_ms = (a.have & CLMEMB_A_HTTP)
			? p->last_seen_ms - (long long)a.uptime_s * 1000 : 0;
		p->cl_reported = !!(a.have & CLMEMB_A_CLIENTS);
		p->link_pct = (a.have & CLMEMB_A_LINK) ? (signed char)a.link_pct : -1;
		p->link_level = (a.have & CLMEMB_A_LINK) ? (unsigned char)a.link_level : 0;
		p->cl_open = p->cl_reported ? a.cl_open : 0;
		p->cl_resp = p->cl_reported ? a.cl_resp : 0;
		p->cl_bin = p->cl_reported ? a.cl_bin : 0;
		p->cl_nresp = p->cl_reported ? a.cl_nresp : 0;
		p->cols_reported = !!(a.have & CLMEMB_A_COLS);
		p->ncols_total = p->cols_reported ? (unsigned char)a.ncols_total : 0;
		if (p->cols_reported)
			cols_merge(p, a.cols, a.ncols, p->last_seen_ms);
		else
			p->ncols = 0;
		{
			/* its relay port and what it wants relayed -
			 * a new or withdrawn port starts unproven (clps) */
			uint16_t np = (a.have & CLMEMB_A_PSRELAY) ? (uint16_t)a.ps_port : 0;

			/* a withdrawn port is a peer's configuration, not an
			 * outage: say so, and do not report it as a fallback */
			if (clps_heard(p, np, (a.have & CLMEMB_A_PSRELAY) ?
			        a.ps_interest : 0, now_ms()))
				LM_NOTICE("cluster: node %d no longer has a pub/sub "
					"relay port - relays to it use the cluster "
					"socket\n", p->node);
		}
		/* clients hear of a member once per membership; a restart under a new identity
		 * reuses the slot with no purge, so the id clients were told is retired first */
		{
			int now_id = __atomic_load_n(&p->node, __ATOMIC_ACQUIRE);

			if (p->clients_told && now_id && p->told_node != now_id) {
				p->clients_told = 0;
				clients_membership("expelled", p, p->told_node);
			}
			if (!p->clients_told && p->client_port && now_id &&
			        p->nstate != PC_NST_CONNECTING) {   /* once it is ONLINE */
				p->clients_told = 1;
				p->told_node = now_id;
				p->told_state = p->nstate;
				clients_membership("joined", p, now_id);
			} else if (p->clients_told && p->told_state != p->nstate) {
				p->told_state = p->nstate;
				clients_membership("state", p, now_id);
			}
		}
	}
	if (C.role == PC_ROLE_MASTER)
		cladmit_alive(now_ms());       /* one in flight arrived */
}

static void handle_master_alive(const unsigned char *pt, size_t n,
		const pc_addr *from)
{
	struct clmemb_malive m;
	struct peer *p;
	int their_id, their_count;

	if (addr_eq(from, &C.self_addr) || !clmemb_malive_parse(pt, n, &m))
		return;
	/* a master holds an id a master handed out and counts 1..PC_CL_MAXPEER + 1:
	 * the count decides who yields, and a forged 0 or 65535 made a real one yield */
	if (m.node < 1 || m.node > PC_CL_NODE_MAX ||
	    m.members < 1 || m.members > PC_CL_MAXPEER + 1) {
		C.st.sender_refused++;
		if (pc_logrl_ok(LRL_SENDER_MRANGE, from))
			LM_NOTICE("cluster: refused a master keepalive from %s - node "
				"%d counting %d member(s) is out of range\n",
				pc_addr_str(from), m.node, m.members);
		return;
	}
	/* a master keepalive admits a peer on its own, so the epoch is checked here
	 * too, before the config digest: the coarser question first */
	if (!epoch_ok((m.have & CLMEMB_M_EPOCH) ? m.epoch : 0, from,
			"master keepalive")) {
		C.st.epoch_refused++;
		peer_forget(from);
		return;
	}
	/* a member adopts and persists the committed epoch the master beacons (it may be
	 * the next master); one this build cannot speak is a fleet it cannot stay in */
	/* only from the map's master, or a replayed old beacon rolls the epoch back; a
	 * new master beacons the epoch it adopted as a member, so nothing waits for its map */
	if ((m.have & CLMEMB_M_COMMIT) && m.committed != pc_cluster_epoch() &&
	    C.role != PC_ROLE_MASTER &&
	    (!C.map_valid || m.node == (int)C.map.master_id)) {
		if (m.committed >= PC_CL_EPOCH_MIN && m.committed <= pc_cluster_epoch_max()) {
			LM_NOTICE("cluster: wire epoch %d -> %d committed by node %d\n",
				pc_cluster_epoch(), m.committed, m.node);
			epoch_adopt(m.committed);
		} else {
			static long long said;

			if (now_ms() - said > 60000) {
				said = now_ms();
				LM_ERR("cluster: the master (node %d) committed the fleet "
					"to wire epoch %d, which this build does not speak "
					"(%d..%d) - this node must be upgraded; it keeps "
					"speaking %d and the fleet will refuse it\n", m.node,
					m.committed, PC_CL_EPOCH_MIN, pc_cluster_epoch_max(),
					pc_cluster_epoch());
			}
		}
	}
	if (m.have & CLMEMB_M_CFG) {
		int have2 = (m.have & CLMEMB_M_CFG2) != 0;
		uint64_t ours;

		if (digest_differs(have2, m.cfg_digest2, m.cfg_digest, &ours) &&
		    C.cfg_authoritative) {
			/* a master we disagree with is not our master */
			shout_mismatch(from, m.mode, m.eager, have2 ? m.cfg_digest2
				: m.cfg_digest, ours, 0);
			C.cfg_refused++;
			peer_forget(from);
			return;
		}
	}
	their_id = m.node;
	their_count = m.members;
	/* a foreign master gets here only while this node is provisional (cid_admit): an
	 * established foreign cluster wins; a provisional one falls to the rules below */
	if (rx_hdr && C.role == PC_ROLE_MASTER) {
		unsigned char mine_c[16];

		if (clwire_cid(mine_c) == CLWIRE_CID_PROVISIONAL &&
		        memcmp(CLWIRE_HDR_CID(rx_hdr), mine_c, 16) != 0 &&
		        !(CLWIRE_HDR_FLAGS(rx_hdr) & CLWIRE_F_PROVISIONAL)) {
			char t[37];

			pc_cid_str(CLWIRE_HDR_CID(rx_hdr), t);
			LM_WARN("cluster: yielding to node %d - its cluster %s is "
				"established and ours was founded moments ago; "
				"re-joining\n", their_id, t);
			yield_follow(from, their_id);
			return;
		}
	}
	if (C.role == PC_ROLE_MASTER) {
		/* split-brain cure: rank (term, then address).  A member count is the claimant's own
		 * word; the only reach this node sees is its own peer table, the same for both sides */
		int mine = clpeers_count_live(&C.pt, now_ms()) + 1;

		/* rule 3 first: a term is totally ordered and an address arbitrary;
		 * a beacon without a term falls to the address */
		if (m.have & CLMEMB_M_TERM) {
			uint32_t theirs_t = m.term;
			/* a promoted master's map is the last master's until its first
			 * is published: the term it claimed is what its beacon asserts */
			uint32_t mine_t = C.sync.term > C.map.term ?
				C.sync.term : C.map.term;

			/* rule 3 obeys any higher term, so an implausible one is refused first,
			 * with the beacon (counted in term_rejected, as the map's guard counts) */
			if (!pc_term_plausible(theirs_t)) {
				if (pc_term_rejected % 100 == 1)
					LM_WARN("cluster: ignoring node %d's "
						"beacon - term %u is implausible "
						"from our %u\n", their_id,
						theirs_t, pc_term_current());
				return;
			}
			/* seen, so a later claim is above it: it yields here before any map from
			 * the winner arrives, and must not promote at the term it yielded to */
			term_seen(theirs_t);
			if (pc_term_must_stepdown(1, mine_t, theirs_t)) {
				LM_WARN("cluster: stepping down - node %d holds "
					"term %u and we hold %u\n", their_id,
					theirs_t, mine_t);
				yield_follow(from, their_id);
				return;
			}
			/* a LOWER term never outranks us (clterm.h rule 1 on the keepalive);
			 * EQUAL terms fall through to the address: a total order */
			if (pc_term_cmp(mine_t, theirs_t) == PC_TERM_STALE)
				return;
		}
		if (addr_cmp(from, &C.self_addr) > 0) {
			LM_WARN("cluster: yielding mastership to node %d "
				"(the higher address; it says %d members, %d live "
				"here) - re-joining\n", their_id, their_count, mine);
			yield_follow(from, their_id);
		}
		return;
	}
	/* the keepalive is the authoritative master-identity signal */
	/* a member follows only its master or a node it knows by that id at that address,
	 * so a replayed beacon cannot move master_addr; a JOINING node follows any */
	if (C.role == PC_ROLE_MEMBER && !addr_eq(from, &C.master_addr) &&
	    !clpeers_by_id_from(&C.pt, (unsigned)their_id, from)) {
		C.st.sender_refused++;
		if (pc_logrl_ok(LRL_SENDER_MFOLLOW, from))
			LM_NOTICE("cluster: not following node %d at %s - this "
				"member follows node %d at %s, and knows no node %d "
				"there\n", their_id, pc_addr_str(from), C.master_id,
				pc_addr_str(&C.master_addr), their_id);
		return;
	}
	/* no beacon below the map's term is followed (clterm.h rule 1): the loser's
	 * last one would move a member's master for a beat */
	if (C.role == PC_ROLE_MEMBER && (m.have & CLMEMB_M_TERM) &&
	    pc_term_cmp(C.map.term, m.term) == PC_TERM_STALE) {
		C.st.sender_refused++;
		return;
	}
	/* the term of the master followed is seen, as its map's is - a
	 * member that must promote before its first map claims above it */
	if ((m.have & CLMEMB_M_TERM) && pc_term_plausible(m.term))
		term_seen(m.term);
	C.master_id = their_id;
	C.master_addr = *from;
	C.master_seen_ms = now_ms();
	elect_defer_ms = 0;                    /* a master is followed */
	p = peer_upsert(from);
	if (p)
		peer_publish(p, their_id, m.free_mb,
			(m.have & CLMEMB_M_MEM) ? m.total_mb : 0,
			(m.have & CLMEMB_M_MEM) ? m.live_kb : 0,
			pd_noident, 0, 0, PC_START_UNKNOWN);
	if (C.role == PC_ROLE_MEMBER)
		cladmit_members_check(m.member_digest, from);
	if (C.role == PC_ROLE_JOINING) {
		cid_follow_foreign();
		send_join_req(from);           /* fast path: join the announcer */
	}
}

/* a fleet stop: believed only from a live member not below our term, it raises SIGTERM a moment
 * later (the originator sends three copies; an exiting node cannot answer the first) */
static void schedule_self_stop(long long delay_ms)
{
	long long at = now_ms() + delay_ms, cur = 0;

	(void)__atomic_compare_exchange_n(&C.stop_self_at_ms, &cur, at, 0,
		__ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

/* a fleet-wide stats reset: every live peer resets with the SENDER's time, so all count from one
 * moment; sent twice (lane, else UDP) and de-duplicated by the time */
static void (*stats_reset_cb)(long long at);
static long long stats_reset_seen;     /* the cluster thread's only */

void pc_cluster_set_stats_reset_cb(void (*cb)(long long at))
{
	stats_reset_cb = cb;
}

int pc_cluster_stats_reset_send(long long at)
{
	unsigned char msg[11];
	int i, sent = 0;

	if (!C.enabled)
		return 0;
	msg[0] = M_STATS_RESET;
	msg[1] = (unsigned char)(C.node_id >> 8);
	msg[2] = (unsigned char)C.node_id;
	for (i = 0; i < 8; i++)
		msg[3 + i] = (unsigned char)((unsigned long long)at >> (56 - 8 * i));
	for (i = 0; i < 2; i++)
		sent = clsend_live(&C.pt, msg, sizeof msg, now_ms());
	return sent;
}

static void handle_statsreset(const unsigned char *pt, size_t n,
		const pc_addr *from)
{
	struct peer *p = clpeers_by_addr(&C.pt, from);
	unsigned long long at = 0;
	int i, node;

	if (n < 11 || !p || !__atomic_load_n(&p->node, __ATOMIC_ACQUIRE))
		return;                    /* not a member we know: ignored */
	node = (pt[1] << 8) | pt[2];
	for (i = 0; i < 8; i++)
		at = (at << 8) | pt[3 + i];
	if ((long long)at == stats_reset_seen)
		return;                    /* the second copy */
	stats_reset_seen = (long long)at;
	if (pc_logrl_ok(LRL_STATS_RESET, from))
		LM_NOTICE("cluster: statistics reset by node %d - the fleet's figures "
			"count from %lld\n", node, (long long)at);
	if (stats_reset_cb)
		stats_reset_cb((long long)at);
}

/* alerts go to a member that can mail: a node with [mail] advertises CLMEMB_FEAT_MAILER, and one
 * without relays its alert there (M_ALERT_RELAY) rather than hold another's SMTP password */
void pc_cluster_set_can_mail(int can)
{
	__atomic_store_n(&self_can_mail, can ? 1 : 0, __ATOMIC_RELAXED);
}

unsigned long long pc_cluster_rx_drops(void)
{
	return __atomic_load_n(&C.rxq_drops, __ATOMIC_RELAXED);
}

/* the lowest live READY member id that can mail, this node included: computed alike everywhere,
 * so a fleet agreeing on membership agrees on the mailer */
int pc_cluster_mailer(int *is_self)
{
	long long now = now_ms();
	int i, best = 0, self = 0;

	if (is_self)
		*is_self = 0;
	if (!C.enabled)
		return 0;
	if (__atomic_load_n(&self_can_mail, __ATOMIC_RELAXED) && C.node_id > 0 &&
	        pc_node_state() == PC_NST_READY) {
		best = C.node_id;
		self = 1;
	}
	for (i = 0; i < C.pt.n_peers; i++) {
		struct peer *p = &C.pt.peers[i];
		int node = __atomic_load_n(&p->node, __ATOMIC_ACQUIRE);

		if (!node || !clpeers_live(p, now) ||
		        !(p->features & CLMEMB_FEAT_MAILER) ||
		        p->nstate != PC_NST_READY)
			continue;
		if (!best || node < best) {
			best = node;
			self = 0;
		}
	}
	if (is_self)
		*is_self = self;
	return best;
}

static void (*alert_relay_cb)(const struct pc_alert_relay *r);
/* the last relays heard, (node, seq): the second copy of each is dropped
 * here.  The cluster thread's only. */
static struct { int node; uint32_t seq; } relay_seen[32];
static int relay_seen_i;

void pc_cluster_set_alert_relay_cb(void (*cb)(const struct pc_alert_relay *r))
{
	alert_relay_cb = cb;
}

int pc_cluster_alert_relay_send(int to, const struct pc_alert_relay *r)
{
	unsigned char msg[16 + sizeof r->key + sizeof r->host + sizeof r->text];
	size_t kl = strnlen(r->key, sizeof r->key - 1);
	size_t hl = strnlen(r->host, sizeof r->host - 1);
	size_t tl = strnlen(r->text, sizeof r->text - 1);
	size_t n = 0;
	int i, sent = 0;

	if (!C.enabled || to <= 0 || to == C.node_id || C.node_id <= 0)
		return -1;
	msg[n++] = M_ALERT_RELAY;
	msg[n++] = (unsigned char)(C.node_id >> 8);
	msg[n++] = (unsigned char)C.node_id;
	for (i = 0; i < 4; i++)
		msg[n++] = (unsigned char)(r->seq >> (24 - 8 * i));
	msg[n++] = (unsigned char)r->level;
	msg[n++] = r->cleared ? 1 : 0;
	msg[n++] = (unsigned char)kl;
	memcpy(msg + n, r->key, kl);
	n += kl;
	msg[n++] = (unsigned char)hl;
	memcpy(msg + n, r->host, hl);
	n += hl;
	msg[n++] = (unsigned char)(tl >> 8);
	msg[n++] = (unsigned char)tl;
	memcpy(msg + n, r->text, tl);
	n += tl;
	/* twice, as the stats reset is sent: one lost datagram would lose
	 * the alert, and the pair de-duplicates on arrival */
	for (i = 0; i < 2; i++)
		if (clsend_to_node(&C.pt, to, msg, n, now_ms()) == 0)
			sent++;
	return sent ? 0 : -1;
}

/* a relayed field into @dst (@cap with its NUL), any control byte made a
 * '?' - the text reaches a log line and a mail's Subject */
static void relay_field(char *dst, size_t cap, const unsigned char *src, size_t n)
{
	size_t i;

	if (n > cap - 1)
		n = cap - 1;
	for (i = 0; i < n; i++)
		dst[i] = (src[i] < 0x20 || src[i] == 0x7f) ? '?' : (char)src[i];
	dst[n] = 0;
}

static void handle_alertrelay(const unsigned char *pt, size_t n,
		const pc_addr *from)
{
	struct peer *p = clpeers_by_addr(&C.pt, from);
	struct pc_alert_relay r;
	size_t o = 9, kl, hl, tl;
	int i, node;

	if (n < 13 || !p)
		return;
	node = (pt[1] << 8) | pt[2];
	/* believed only from the member it says it is */
	if (!node || __atomic_load_n(&p->node, __ATOMIC_ACQUIRE) != node)
		return;
	memset(&r, 0, sizeof r);
	r.node = node;
	for (i = 0; i < 4; i++)
		r.seq = (r.seq << 8) | pt[3 + i];
	r.level = pt[7];
	r.cleared = pt[8] & 1;
	kl = pt[o++];
	if (o + kl + 1 > n)
		return;
	relay_field(r.key, sizeof r.key, pt + o, kl);
	o += kl;
	hl = pt[o++];
	if (o + hl + 2 > n)
		return;
	relay_field(r.host, sizeof r.host, pt + o, hl);
	o += hl;
	tl = ((size_t)pt[o] << 8) | pt[o + 1];
	o += 2;
	if (o + tl > n)
		return;
	relay_field(r.text, sizeof r.text, pt + o, tl);
	for (i = 0; i < 32; i++)
		if (relay_seen[i].node == node && relay_seen[i].seq == r.seq)
			return;
	relay_seen[relay_seen_i].node = node;
	relay_seen[relay_seen_i].seq = r.seq;
	relay_seen_i = (relay_seen_i + 1) % 32;
	if (alert_relay_cb)
		alert_relay_cb(&r);
}

static void handle_fleetstop(const unsigned char *pt, size_t n,
		const pc_addr *from)
{
	struct peer *p = clpeers_by_addr(&C.pt, from);
	uint32_t their_term, ours;
	int node;

	if (n < 7 || !p || !__atomic_load_n(&p->node, __ATOMIC_ACQUIRE))
		return;
	node = (pt[1] << 8) | pt[2];
	their_term = ((uint32_t)pt[3] << 24) | ((uint32_t)pt[4] << 16) |
		((uint32_t)pt[5] << 8) | pt[6];
	/* against the term of the map this node FOLLOWS, not its own claim, which a healed
	 * isolated node holds above it; a replayed stop is still below the current map's */
	ours = C.map_valid ? C.map.term : pc_term_current();
	if (pc_term_cmp(ours, their_term) == PC_TERM_STALE) {
		if (pc_logrl_ok(LRL_FLEETSTOP, from))
			LM_WARN("cluster: IGNORING a fleet stop from node %d at term %u "
				"- the fleet's map is at term %u, so it is from a fleet "
				"that has since moved on\n", node, their_term, ours);
		return;
	}
	if (__atomic_load_n(&C.stop_self_at_ms, __ATOMIC_ACQUIRE))
		return;                    /* already going: a retransmit */
	LM_NOTICE("cluster: FLEET STOP requested by node %d - stopping "
		"cleanly: draining, then a snapshot, then goodbye\n", node);
	schedule_self_stop(300);
}

/* the node ids that said GOODBYE, newest last, so a fleet stop calls a peer STOPPED, not gone;
 * one writer (the peer thread): the id is stored before the count is released */
static int bye_ids[PC_CL_MAXPEER];
static unsigned int bye_n;

static void bye_record(int node)
{
	unsigned int n = __atomic_load_n(&bye_n, __ATOMIC_RELAXED);

	__atomic_store_n(&bye_ids[n % PC_CL_MAXPEER], node, __ATOMIC_RELAXED);
	__atomic_store_n(&bye_n, n + 1, __ATOMIC_RELEASE);
}

/* did @node say goodbye since the count read @mark? */
static int bye_since(int node, unsigned int mark)
{
	unsigned int n = __atomic_load_n(&bye_n, __ATOMIC_ACQUIRE), k;

	if (n - mark > PC_CL_MAXPEER)
		mark = n - PC_CL_MAXPEER;      /* older entries were overwritten */
	for (k = mark; k != n; k++)
		if (__atomic_load_n(&bye_ids[k % PC_CL_MAXPEER],
		        __ATOMIC_RELAXED) == node)
			return 1;
	return 0;
}

static void handle_goodbye(const unsigned char *pt, size_t n,
		const pc_addr *from)
{
	struct peer *p;
	unsigned int incarn;
	int i, node, said, have_incarn;

	if (addr_eq(from, &C.self_addr) ||
	        !clmemb_goodbye_parse(pt, n, &said, &incarn, &have_incarn))
		return;
	for (i = 0; i < C.pt.n_peers; i++) {
		p = &C.pt.peers[i];
		if (addr_eq(&p->addr, from)) {
			node = __atomic_load_n(&p->node, __ATOMIC_ACQUIRE);
			/* only the node at this address in this run: a replayed goodbye would
			 * expel whatever runs there now; an unknown incarnation cannot compare */
			if (!node || said != node ||
			    (have_incarn && p->incarn && incarn != p->incarn)) {
				C.st.goodbye_refused++;
				return;
			}
			bye_record(node);
			peer_gone(p, CLRES_GONE_GOODBYE);
			break;
		}
	}
	if (C.role != PC_ROLE_MASTER && addr_eq(from, &C.master_addr) &&
	        said == C.master_id)
		C.master_seen_ms = 0;          /* master left: elect at once */
}

/* is there anything to pull?  Eager is a collection's property as much as the fleet's, and the
 * pull serves every eager collection, so this asks the collections */
static int any_eager(void)
{
	int c;

	for (c = 0; c < pc_store_count(); c++)
		if (pc_store_live(c) && pc_store_eager_enabled(pc_store_ht(c)))
			return 1;
	return 0;
}

/* who to pull the bootstrap from: live, READY, holding records ahead of none, lowest id first,
 * handed to the bulk thread.  1 = a pull is on its way, 0 = nobody qualifies yet */
static int boot_request(void)
{
	long long now = now_ms();
	pc_addr addr[BOOT_MAX_CAND];
	int id[BOOT_MAX_CAND], n = 0, i, j, held_unready = 0, anyent = 0;
	unsigned int ent[BOOT_MAX_CAND];
	int pref = C.boot_prefer;
	struct spcs sp;

	sp.n = 0;
	if (C.rejoin_on && C.rejoin_whole)
		(void)pc_ack_fleet_each(spcs_cb, &sp);
	for (i = 0; i < C.pt.n_peers; i++) {
		struct peer *p = &C.pt.peers[i];
		int pid = __atomic_load_n(&p->node, __ATOMIC_ACQUIRE);
		unsigned int pe = p->entries;

		if (pid && clpeers_live(p, now) && p->nstate != PC_NST_READY && pe)
			held_unready = 1;      /* still replaying, or not admitted yet */
		if (!pid || !clpeers_live(p, now) ||
		        p->nstate != PC_NST_READY || (C.rejoin_on && !is_witness(p)) ||
		        (C.rejoin_on && C.rejoin_whole && pid != pref &&
		         peer_short(p, &sp, now, 1) == 1))
			continue;
		anyent |= pe > 0;
		/* insertion sort: a rejoin's survivor first, then (holds records) desc, id asc */
		for (j = n; j > 0; j--) {
			int before = id[j - 1] == pref ? 0 : pid == pref ? 1
				: (ent[j - 1] > 0) != (pe > 0)
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
	/* empty ready peers say nothing while one not ready yet holds records: after a whole-fleet
	 * restart only the authors' WALs hold the keyspace, and done here, nobody backfills us */
	if (!n || (!anyent && held_unready && !C.rejoin_on))
		return 0;
	acks_boot_floors(id, n);
	if (C.rejoin_on) {
		for (j = 0; j < n; j++) {
			struct peer *q = peer_by_node(id[j]);

			C.rejoin_cand[j] = id[j];
			C.rejoin_inc[j] = q ? q->incarn : 0;
		}
		C.rejoin_ncand = n;
	}
	if (!anyent)
		return 2;                      /* every ready peer holds nothing:
		                                * there is nothing to pull */
	clboot_handoff(&C.bt, clbulk_lock(&C.bk), addr, id, n);
	LM_NOTICE("cluster: pulling a bootstrap from node %d (%u record(s) "
		"reported)%s\n", id[0], ent[0], n > 1 ? ", others as fallback"
		: "");
	return 1;
}

/* the bootstrap decision on the tick, for a member and a master alike (a founder with live
 * peers pulls too); the arithmetic is clboot's, the peer scan and messages are here */
static void boot_tick(long long now)
{
	int pending, failed_round, act;

	/* a joiner that is still waiting to pull - the first
	 * heartbeats may not have reached it at ASSIGN time */
	if (pc_node_state() != PC_NST_RECOVERING || clboot_done(&C.bt) ||
	    C.rejoin_on < 0 || C.decide_on)
		return;                        /* held for the pass's ruling: nothing to pull yet */
	clboot_tick_state(&C.bt, clbulk_lock(&C.bk), &pending, &failed_round);
	act = clboot_before_pick(&C.bt, now, pending, failed_round);
	if (act == CLBOOT_PICK)
		act = clboot_after_pick(&C.bt, now, failed_round, boot_request());
	if (C.rejoin_on && (act == CLBOOT_GIVE_UP || act == CLBOOT_DEADLINE)) {
		rejoin_lost(NULL);             /* no survivor left to copy from */
		return;
	}
	if (act == CLBOOT_GIVE_UP) {
		LM_WARN("cluster: no peer served a bootstrap in %d "
			"rounds - reporting ready and relying on the push "
			"backfill (the keyspace arrives over the next "
			"sweeps)\n", C.bt.rounds);
		pc_node_state_set(PC_NST_READY);
	} else if (act == CLBOOT_EMPTY) {
		LM_NOTICE("cluster: every %s reports holding nothing - "
			"nothing to pull; ready\n", C.rejoin_on ? "survivor"
			: "ready peer");
		pc_node_state_set(PC_NST_READY);
	} else if (act == CLBOOT_DEADLINE) {
		LM_WARN("cluster: no ready peer to pull a "
			"bootstrap from - reporting ready and "
			"relying on the push backfill\n");
		pc_node_state_set(PC_NST_READY);
	}
}

/* bytes in a receive queue now: SO_MEMINFO's first word (sk_rmem_alloc); FIONREAD on a Linux
 * UDP socket answers only the FIRST datagram's size, flat while a node drowns */
static unsigned int rx_queue_bytes(int fd)
{
	unsigned int mi[16];
	socklen_t l = sizeof mi;

	if (fd < 0 || getsockopt(fd, SOL_SOCKET, SO_MEMINFO, mi, &l) != 0 ||
	        l < sizeof mi[0])
		return 0;
	return mi[0];
}

/* rx rates are sampled on read by the stats reader: the cluster and maintenance ticks starve
 * under the load they report; below RX_SAMPLE_MIN_MS the last reading is served again */
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

/* before a peer silent on UDP is purged or a master declared dead, ask whether TCP still reaches
 * it; alive on TCP means UDP is blocked on this path: force the lane both ways (unicast only) */
#define LANE_PROBE_MS 3000
/* TCP alone may be a new process on the old address: a UDP probe answered inside this says so */
#define LANE_ASK_MS 1500
static struct clprobe_tab lane_probes;

/* 1 = TCP says it lives (or may - the probe is still out): keep it */
static int lane_keeps(const pc_addr *a, long long silent_ms,
		long long now)
{
	long long age;
	struct lprobe *pm;
	int st;

	if (!cl_unicast || cl_max_dgram <= 0 || frx_since(a, now - silent_ms))
		return 0;
	st = clane_tcp_state(a, &age);
	pm = clprobe_slot(&lane_probes, a, 1, now);
	if (st == CLANE_TCP_UP) {
		pm->ms = 0;
		if (!clane_forced(a)) {
			unsigned char m = M_LANE_FORCE;

			/* asked inside this silence and answered by UDP: the path works, the silence
			 * is the incarnation we knew - it is gone, whatever now listens on TCP */
			if (pm->ask_ms >= now - silent_ms && pm->heard_ms >= pm->ask_ms)
				return 0;
			if (pm->ask_ms < now - silent_ms) {
				m = M_LANE_PROBE;
				pm->ask_ms = now;
				(void)clsend_seal_udp(a, &m, 1);
				return 1;
			}
			if (now - pm->ask_ms < LANE_ASK_MS)
				return 1;
			clane_force(a, 1);
			seal_send(a, &m, 1);       /* forced: it goes by the lane */
			lane_probes.saves++;
			if (pc_logrl_ok(LRL_LANE_FORCE, a))
				LM_ERR("cluster: %s:%d has sent no UDP for %lld s but answers "
					"on TCP - UDP is blocked or lost on this path; "
					"everything between the two now goes over the TCP "
					"lane\n", pc_addr_str(a), pc_addr_port(a),
					silent_ms / 1000);
		}
		return 1;
	}
	if (!pm->ms) {
		pm->ms = now;                  /* ask once */
		clane_probe(a);
		return 1;
	}
	return now - pm->ms < LANE_PROBE_MS && st == CLANE_TCP_PENDING;
}

/* whether the frame being dispatched came by the TCP lane - set by
 * drain_lane around its dispatch, so a lane frame never counts as UDP */
static int rx_via_lane;

/* every lane_retry_s, probe each peer this node forced to the lane by plain UDP; the answer comes
 * by UDP too, proving both directions, and only then is the force lifted (lane_probe_r) */
static void lane_retry_tick(long long now)
{
	static long long last;
	unsigned char m = M_LANE_PROBE;
	int i;

	if (!cl_unicast || cl_max_dgram <= 0 || now - last < cl_lane_retry_ms)
		return;
	last = now;
	for (i = 0; i < C.pt.n_peers; i++)
		if (C.pt.peers[i].node && clane_forced(&C.pt.peers[i].addr))
			(void)clsend_seal_udp(&C.pt.peers[i].addr, &m, 1);
}

static void handle_lane_probe(const pc_addr *from)
{
	unsigned char m = M_LANE_PROBE_R;

	if (!rx_via_lane)
		(void)clsend_seal_udp(from, &m, 1);
}

static void handle_lane_probe_r(const pc_addr *from)
{
	struct lprobe *pm;

	if (!rx_via_lane && (pm = clprobe_slot(&lane_probes, from, 0, now_ms())))
		pm->heard_ms = now_ms();
	if (!rx_via_lane)
		cladmit_probe_answered(clpeers_by_addr(&C.pt, from), now_ms());
	if (rx_via_lane || !clane_forced(from))
		return;
	clane_force(from, 0);
	lane_restored++;
	if (pc_logrl_ok(LRL_LANE_BACK, from))
		LM_NOTICE("cluster: UDP to and from %s:%d works again - frames to it go "
			"by UDP, the TCP lane is the fallback again\n",
			pc_addr_str(from), pc_addr_port(from));
}

static void handle_lane_force(const pc_addr *from)
{
	if (!cl_unicast || cl_max_dgram <= 0 || clane_forced(from))
		return;
	clane_force(from, 1);
	LM_WARN("cluster: %s:%d asks for TCP - it hears none of this node's "
		"UDP; everything to it now goes over the lane\n",
		pc_addr_str(from), pc_addr_port(from));
}

unsigned long long pc_cluster_lane_saves(void)
{
	return lane_probes.saves;
}

/* a member whose master went silent asks up to MPROBE_ASK peers if they hear it; if one does,
 * only this node's path is broken and it holds the election, at most MPROBE_HOLD_MS */
#define MPROBE_ASK      3
#define MPROBE_FRESH_MS 2000
#define MPROBE_HOLD_MS  30000
static long long mprobe_first_ms, mprobe_ok_ms, mprobe_said_ms;
static int mprobe_ok_from;
static unsigned long long mprobe_holds;

static void mprobe_ask(long long now)
{
	unsigned char m[3];
	int i, asked = 0;

	m[0] = M_MPROBE;
	m[1] = (unsigned char)(C.master_id >> 8);
	m[2] = (unsigned char)C.master_id;
	for (i = 0; i < C.pt.n_peers && asked < MPROBE_ASK; i++) {
		struct peer *p = &C.pt.peers[i];

		if (!p->node || p->node == C.master_id || !clpeers_live(p, now))
			continue;
		if (seal_send(&p->addr, m, sizeof m) == 0)
			asked++;
	}
}

static void handle_mprobe(const unsigned char *pt, size_t n,
		const pc_addr *from)
{
	unsigned char r[7];
	uint32_t ago = 0xFFFFFFFFu;
	int id;

	if (n < 3)
		return;
	id = (pt[1] << 8) | pt[2];
	if (C.role == PC_ROLE_MASTER && id == C.node_id)
		ago = 0;
	else if (C.role == PC_ROLE_MEMBER && id == C.master_id &&
	        C.master_seen_ms) {
		long long a = now_ms() - C.master_seen_ms;

		ago = a < 0 ? 0 : a > 0x7FFFFFFF ? 0x7FFFFFFFu : (uint32_t)a;
	}
	r[0] = M_MPROBE_R;
	r[1] = pt[1];
	r[2] = pt[2];
	r[3] = (unsigned char)(ago >> 24);
	r[4] = (unsigned char)(ago >> 16);
	r[5] = (unsigned char)(ago >> 8);
	r[6] = (unsigned char)ago;
	seal_send(from, r, sizeof r);
}

static void handle_mprobe_r(const unsigned char *pt, size_t n,
		const pc_addr *from)
{
	uint32_t ago;
	struct peer *p;

	if (n < 7 || ((pt[1] << 8) | pt[2]) != C.master_id)
		return;
	ago = ((uint32_t)pt[3] << 24) | ((uint32_t)pt[4] << 16) |
		((uint32_t)pt[5] << 8) | pt[6];
	if (ago < MPROBE_FRESH_MS) {
		mprobe_ok_ms = now_ms();
		p = peer_upsert(from);
		mprobe_ok_from = p ? p->node : 0;
	}
}

/* 1 = hold the election: another member hears the master */
static int mprobe_holds_election(long long now)
{
	if (!mprobe_first_ms) {
		mprobe_first_ms = now;
		mprobe_ok_ms = 0;
		mprobe_ask(now);
		return 1;                      /* one tick for the answers */
	}
	if (now - mprobe_first_ms >= MPROBE_HOLD_MS)
		return 0;
	if (now - mprobe_ok_ms < MPROBE_FRESH_MS + 1000) {
		mprobe_ask(now);
		if (now - mprobe_said_ms >= 10000) {
			LM_WARN("cluster: master node %d is silent here but node %d "
				"hears it - this node's path to the master is broken, "
				"not the master; holding the election (up to %d s)\n",
				C.master_id, mprobe_ok_from, MPROBE_HOLD_MS / 1000);
			mprobe_said_ms = now;
			mprobe_holds++;
		}
		return 1;
	}
	return 0;                          /* nobody hears it: elect */
}

unsigned long long pc_cluster_mprobe_holds(void)
{
	return mprobe_holds;
}

#define SELF_LATE_MS 2000
static long long self_late_until;
static unsigned long long self_late_n;

unsigned long long pc_cluster_self_late(void)
{
	return self_late_n;
}

static void membership_tick(void)
{
	long long now = now_ms();
	int i;

	/* the created set to every live peer: when first seen, then every COL_RESYNC_BEATS
	 * beats, behind the create's one fire-and-forget datagram; idempotent by generation */
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

	pc_cid_tick(now);                      /* provisional -> established */
	{
		static uint32_t said;
		int r = pc_term_retry();
		uint32_t u = pc_term_unpersisted();

		if (r == 1)
			LM_NOTICE("cluster: mastership term %u persisted - this node may "
				"take mastership again\n", pc_term_current());
		else if (r < 0 && u && said != u) {
			said = u;
			LM_CRIT("cluster: could not persist mastership term %u (%s) - "
				"following it, but this node takes no mastership until "
				"it is on disk (check the state directory)\n", said,
				strerror(pc_term_store_errno()));
		}
	}
	eager_settle_tick(now);
	mesh_hint_tick(now);
	lane_retry_tick(now);

	/* after its OWN loop ran late (starved, a paused VM) a node holds purges and elections
	 * for as long as the stall (2 s at least): its peers' beats are still unread */
	{
		static long long last_tick;

		if (last_tick && now - last_tick > SELF_LATE_MS) {
			long long hold = now - last_tick;

			if (hold < SELF_LATE_MS)
				hold = SELF_LATE_MS;
			self_late_until = now + hold;
			self_late_n++;
			LM_NOTICE("cluster: this node's own loop ran %lld ms late - "
				"holding purges and elections for %lld ms while "
				"its peers' beats catch up\n", now - last_tick, hold);
		}
		last_tick = now;
	}

	/* purge silent peers */
	for (i = 0; i < C.pt.n_peers && now >= self_late_until; i++)
		if (C.pt.peers[i].node &&
		        now - C.pt.peers[i].last_seen_ms >= PEER_PURGE_MS &&
		        !lane_keeps(&C.pt.peers[i].addr,
		                now - C.pt.peers[i].last_seen_ms, now))
			peer_gone(&C.pt.peers[i], CLRES_GONE_SILENT);

	switch (C.role) {
	case PC_ROLE_JOINING:
		if (now >= C.join_deadline_ms && clfound_gate_blocks(now)) {
			/* a blank unicast node waits - re-announcing */
			C.join_deadline_ms = now + 1000;
			send_join_req(NULL);
		} else if (now >= C.join_deadline_ms && C.master_id &&
		           C.master_seen_ms && now - C.master_seen_ms < MASTER_DEAD_MS) {
			/* a master beacons: wait for its answer - founding beside it
			 * would take mastership back after a handover */
			C.join_deadline_ms = now + 1000;
			send_join_req(&C.master_addr);
		} else if (now >= C.join_deadline_ms) {
			long long at = clfound_settle_at(now);     /* lowest identity founds */

			if (now >= at)
				become_master(1);
			else
				C.join_deadline_ms = at;   /* re-announcing meanwhile, below */
		} else if (now - C.join_last_ms >= 1000) {
			send_join_req(NULL);       /* re-announce while waiting */
		}
		break;
	case PC_ROLE_MEMBER:
		send_alive();
		boot_tick(now);
		if (now - C.master_seen_ms < MASTER_DEAD_MS)
			mprobe_first_ms = 0;          /* heard again */
		/* never admitted, so no candidate: an admitted member elects the next master, which
		 * checks this node again; with none left it waited for ever - a joiner again founds */
		if (pc_node_state() == PC_NST_CONNECTING &&
		        now - C.master_seen_ms >= MASTER_DEAD_MS && now >= self_late_until &&
		        (C.master_seen_ms == 0 ||
		         !lane_keeps(&C.master_addr, now - C.master_seen_ms, now)) &&
		        !admitted_peer_live(now)) {
			LM_NOTICE("cluster: master %d lost while this node was CONNECTING - "
				"joining again\n", C.master_id);
			C.master_id = 0;
			C.master_seen_ms = 0;
			join_again(NULL);
			break;
		}
		/* master_seen_ms 0 is the master's GOODBYE: elect at once, without asking the
		 * lane - a stopping master's lane is often still up, and now - 0 is no silence */
		if (now - C.master_seen_ms >= MASTER_DEAD_MS &&
		        now >= self_late_until &&
		        (C.master_seen_ms == 0 ||
		         !lane_keeps(&C.master_addr,
		                now - C.master_seen_ms, now)) &&
		        !mprobe_holds_election(now) &&
		        pc_node_state() != PC_NST_CONNECTING &&   /* not admitted yet */
		        (!cladmit_online_ms() || now - cladmit_online_ms() >= MASTER_DEAD_MS) &&
		        !self_no_promote) {                   /* a test flag */
			/* the highest live (ip,port) self-promotes; stickiness
			 * holds because a live master never reaches this path */
			int best = 1;
			struct peer *higher = NULL, *sb = NULL;

			/* the map's standby holds the synced state - a live
			 * one is the candidate, itself or the one deferred to */
			/* a CONNECTING one cannot promote (its own election waits for ONLINE): named
			 * in the map the moment it was admitted, deferring to it only ran out the bound */
			if (C.map_valid && C.map.backup_id && C.map.backup_id != C.node_id &&
			    (sb = peer_by_node(C.map.backup_id)) &&
			    (now - sb->last_seen_ms >= MASTER_DEAD_MS ||
			     sb->nstate == PC_NST_STALLED || sb->nstate == PC_NST_CONNECTING))
				sb = NULL;
			if (sb) {
				best = 0;
				higher = sb;
			} else if (!(C.map_valid && C.map.backup_id == C.node_id))
			for (i = 0; i < C.pt.n_peers; i++)
				if (C.pt.peers[i].node &&
				        now - C.pt.peers[i].last_seen_ms < MASTER_DEAD_MS &&
				        /* a STALLED peer is live (its watchdog beats) but not
				         * a candidate: deferring to it deferred for ever */
				        C.pt.peers[i].nstate != PC_NST_STALLED &&
				        C.pt.peers[i].nstate != PC_NST_CONNECTING &&
				        addr_cmp(&C.pt.peers[i].addr, &C.self_addr) > 0) {
					best = 0;
					higher = &C.pt.peers[i];
					break;
				}
			/* deferred longer than a probe hold plus a master's death to a higher
			 * address that never beacons: it is not electing, so this node promotes */
			if (!best && higher) {
				if (!elect_defer_ms ||
				        !addr_eq(&elect_defer_to, &higher->addr)) {
					elect_defer_ms = now;
					elect_defer_to = higher->addr;
				} else if (now - elect_defer_ms >=
				        MPROBE_HOLD_MS + MASTER_DEAD_MS) {
					LM_WARN("cluster: node %d at %s, a higher address, "
						"has been live for %lld s without taking "
						"mastership - promoting instead\n",
						higher->node, pc_addr_str(&higher->addr),
						(now - elect_defer_ms) / 1000);
					best = 1;
				}
			}
			if (best) {
				/* it may come back for its term on disk: the bound stays run out */
				become_master(0);
				if (C.role == PC_ROLE_MASTER)
					elect_defer_ms = 0;
			} else
				C.master_seen_ms = now - MASTER_DEAD_MS + 1000;
				/* re-check in ~1s: the winner's MASTER_ALIVE
				 * arrives, or the live set shrank to us */
		}
		break;
	case PC_ROLE_MASTER:
		send_alive();
		send_master_alive();
		boot_tick(now);                /* a founder may be pulling */
		cladmit_tick(now);
		/* publish what the standby acked and drop what it left hanging; restage every few
		 * seconds so a missed map heals, and at once when membership moves */
		clmap_tick(now);
		handover_tick(now);
		if (now - C.map_pub_ms >= CLMAP_PUB_MS ||
		        clmap_membership_moved())
			clmap_stage();
		break;
	default:
		break;
	}
	/* liveness moves without a map, so the verdict is retaken every tick: a stale
	 * "usable" lets the two bases disagree and fork a re-write */
	clmap_recheck_usable();
	reconcile_tick();
	restart_witness_note();
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
	C.st.map_unadmitted = C.map_unadmitted_n;
	/* the published standby, not this node's own variable: a member has none and would report
	 * "no standby" for a cluster that has one */
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
	C.st.sync_timeouts = C.stage_timeout_n;
	C.st.map_usable = C.map_usable;
	C.st.place_map = C.place_map_n;
	C.st.place_hrw = C.place_hrw_n;
	C.st.master_id = C.role == PC_ROLE_MASTER ? C.node_id : C.master_id;
}

static void handle_fwd_json(const unsigned char *pt, size_t n,
		const pc_addr *from);
static void handle_fwd_jack(const unsigned char *pt, size_t n);

/* cap one socket's drain: a firehose (migration ingest) must not starve the other socket and the
 * 1 Hz tick, or a live master is convicted dead; poll is level-triggered, so the cap is free */
#define DRAIN_BATCH 64

/* the socket's drop count stamped on a datagram as it was queued: per daemon, unlike host-wide
 * RcvbufErrors; it wraps at 32 bits, so deltas are 32-bit, summed into 64 */
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

/* the target set learns from CONTROL frames only (1 Hz per peer, so its lock is cheap); a member
 * hearing an unknown address tells its master, which then beats at the joiner */
static void send_master_alive_to(const pc_addr *to);

/* tell the master about @a, at most once per @every_ms per address */
static void mesh_hint(const pc_addr *a, long long now,
		long long every_ms)
{
	unsigned char m[8];

	if (C.role != PC_ROLE_MEMBER || !pc_addr_port(&C.master_addr) ||
	        pc_addr_eq(&C.master_addr, a) ||
	        !clmesh_hint_due(a, now, every_ms))
		return;
	m[0] = M_PEER_HINT;
	memcpy(m + 1, &a->in4.sin_addr.s_addr, 4);
	memcpy(m + 5, &a->in4.sin_port, 2);
	if (seal_send(&C.master_addr, m, 7) == 0)
		mesh_hints_sent++;
}

static void hint_to(const pc_addr *to, const pc_addr *a)
{
	unsigned char m[8];

	m[0] = M_PEER_HINT;
	memcpy(m + 1, &a->in4.sin_addr.s_addr, 4);
	memcpy(m + 5, &a->in4.sin_port, 2);
	if (seal_send(to, m, 7) == 0)
		mesh_hints_sent++;
}

static void mesh_share(const pc_addr *nw, long long now)
{
	static __thread pc_addr kn[CLMESH_MAX_KNOWN];
	int i, k = clmesh_known(kn, CLMESH_MAX_KNOWN, now);

	for (i = 0; i < k; i++) {
		if (pc_addr_eq(&kn[i], nw) || pc_addr_eq(&kn[i], &C.self_addr))
			continue;
		hint_to(&kn[i], nw);
		hint_to(nw, &kn[i]);
	}
}

static void mesh_note(const pc_addr *from, int type, int trial)
{
	long long now = now_ms();
	int isnew;

	if (type != M_ALIVE && type != M_MASTER_ALIVE && type != M_JOIN_REQ &&
	        type != M_CLMAP && type != M_PEER_HINT)
		return;
	isnew = clmesh_heard(from, now, trial);
	/* a JOINING node is no one's member, so it shares directly, newcomer to each known
	 * address and back: two nodes seeded with a third find each other, and one founds */
	if (isnew && C.role == PC_ROLE_JOINING)
		mesh_share(from, now);
	/* a JOIN request is a node looking for the master now: passed on every time (1 s apart at
	 * most), not only when new - this member may have heard it first while still joining */
	if (type == M_JOIN_REQ)
		mesh_hint(from, now, 1000);
	else if (isnew)
		mesh_hint(from, now, 10000);
}

/* once a second: every known address the map does not name goes to the master (10 s apart per
 * address) - heard before this node was a member, or from a member the master cannot reach */
static void mesh_hint_tick(long long now)
{
	static __thread pc_addr kn[CLMESH_MAX_KNOWN];
	int i, k;
	unsigned int j;

	if (!cl_unicast || C.role != PC_ROLE_MEMBER || !C.map_valid)
		return;
	k = clmesh_known(kn, CLMESH_MAX_KNOWN, now);
	for (i = 0; i < k; i++) {
		int named = pc_addr_eq(&kn[i], &C.self_addr);

		for (j = 0; j < C.map.nnodes && !named; j++)
			named = C.map.node[j].addr == kn[i].in4.sin_addr.s_addr &&
				C.map.node[j].cluster_port == kn[i].in4.sin_port;
		if (!named)
			mesh_hint(&kn[i], now, 10000);
	}
}

static void handle_peer_hint(const unsigned char *pt, size_t n,
		const pc_addr *from)
{
	pc_addr a;

	if (n < 7 || !cl_unicast)
		return;
	memset(&a, 0, sizeof a);
	a.in4.sin_family = AF_INET;
	memcpy(&a.in4.sin_addr.s_addr, pt + 1, 4);
	memcpy(&a.in4.sin_port, pt + 5, 2);
	if (!a.in4.sin_addr.s_addr || !a.in4.sin_port ||
	        (a.in4.sin_addr.s_addr == C.self_addr.in4.sin_addr.s_addr &&
	         a.in4.sin_port == C.self_addr.in4.sin_port))
		return;
	mesh_hints_recv++;
	clmesh_heard(&a, now_ms(), 1);         /* a hint is on trial */
	/* a master answers a hinted address at once, for a joiner's fast path - but only a
	 * member's hint, or any sender could aim the master's beacon anywhere */
	if (C.role == PC_ROLE_MASTER && sender_admitted(from))
		send_master_alive_to(&a);
}

void pc_cluster_lane_info(struct clane_figs *f, int *max_dgram,
		int *lane_port)
{
	clane_figures(f);
	*max_dgram = cl_unicast ? cl_max_dgram : 0;
	*lane_port = cl_unicast && cl_max_dgram ? cl_lane_port : 0;
}

void pc_cluster_discovery_info(const char **mode, int *seeds,
		int *seeds_resolved, int *known, unsigned long long *resolve_fail,
		unsigned long long *hints_sent, unsigned long long *hints_recv)
{
	*mode = cl_unicast ? "unicast" : "multicast";
	clmesh_figures(seeds, seeds_resolved, known, resolve_fail);
	*hints_sent = mesh_hints_sent;
	*hints_recv = mesh_hints_recv;
}

/* each sealed frame once - a copy taken before, one older than its
 * stream's window, or one from a sender's older incarnation is dropped */
static int replay_ok(const unsigned char *buf, const pc_addr *from)
{
	struct clwire_id id;
	int rc;

	clwire_id_read(buf, &id);
	rc = clreplay_admit(&id, from->in4.sin_addr.s_addr, now_ms());
	if (clreplay_taken(rc))
		return 1;
	if (pc_logrl_ok(LRL_REPLAY, from))
		LM_INFO("cluster: dropped a frame from %s - %s\n", pc_addr_str(from),
			rc == CLREPLAY_DUP ? "its sequence was taken before" :
			rc == CLREPLAY_OLD ? "older than its stream's window" :
			rc == CLREPLAY_HOST ? "its header names another host" :
			rc == CLREPLAY_FULL ? "the replay table has no room to track its sender" :
			"from an older incarnation of its sender");
	return 0;
}

/* one sealed frame, from a datagram or the TCP frame lane: open,
 * admit, dispatch.  1 if it was handled, 0 if refused. */
static int dispatch_dgram(unsigned char *buf, size_t r,
		const pc_addr *from, unsigned char *pt)
{
	unsigned long long ptlen = 0;

	if (open_dgram(buf, (size_t)r, pt, &ptlen) != 0) {
		/* the header is clear before the AEAD: another version is a build this one
		 * cannot talk to, not a forgery - said by name, once a minute */
		if (r >= 2 && buf[0] == PC_CL_MAGIC && buf[1] != PC_CL_VER) {
			static long long said;
			long long t = now_ms();
			char ab[PC_ADDR_STRLEN] = "?";

			C.st.wire_ver_refused++;
			if (t - said >= 60000) {
				said = t;
				pc_addr_ntop(from, ab, sizeof ab);
				LM_WARN("cluster: %s speaks cluster header version %u and "
					"this build speaks %u - builds this far apart cannot "
					"talk; upgrade the older side (%llu such datagrams "
					"so far)\n", ab, (unsigned)buf[1],
					(unsigned)PC_CL_VER, C.st.wire_ver_refused);
			}
			return 0;
		}
		/* the quarantine posture: count, name the source, drop -
		 * not a block by source: a spoofed one would silence a member */
		C.st.bad_auth++;
		if (pc_logrl_ok(LRL_BAD_AUTH, from)) {
			char ab[PC_ADDR_STRLEN];

			pc_addr_ntop(from, ab, sizeof ab);
			LM_WARN("cluster: a datagram from %s does not authenticate - "
				"a different cluster secret, or not a member (%llu "
				"such so far)\n", ab, C.st.bad_auth);
		}
		return 0;
	}
	if (!ptlen)
		return 0;
	if (!cid_admit(buf, pt[0], from))
		return 0;
	memcpy(rx_hdr_copy, buf, sizeof rx_hdr_copy);
	rx_hdr = rx_hdr_copy;
	/* an ALIVE's or a map's sender is learned after its handler, once admitted; a JOIN_REQ,
	 * hint or master beacon first: that is how joiners and seeds find each other */
	if (cl_unicast && pt[0] != M_ALIVE && pt[0] != M_CLMAP)
		mesh_note(from, pt[0], 1);  /* on trial, confirmed below */
	if (!sender_gate(pt[0], from)) {
		C.st.sender_refused++;
		/* logged, not only counted: a refusal is a lost pull, a timed-out forward or a
		 * refused write, and the counter alone names neither frame nor sender */
		if (pc_logrl_ok(LRL_SENDER_GATE, from))
			LM_NOTICE("cluster: refused a type %d frame from %s - "
				"not a sender this %s node takes it from\n", pt[0],
				pc_addr_str(from), C.role == PC_ROLE_MASTER ? "master" :
				C.role == PC_ROLE_JOINING ? "joining" : "member");
		rx_hdr = NULL;
		return 1;
	}
	if (!replay_ok(buf, from)) {          /* past the gate */
		rx_hdr = NULL;
		return 0;
	}
	switch (pt[0]) {
	case M_COL_SET:   handle_col_set(pt, ptlen); break;
	case M_SWEEP_DIG:   handle_sweep_dig(pt, ptlen, from); break;
	case M_SWEEP_DIG_R: handle_sweep_dig_r(pt, ptlen); break;
	case M_PUBLISH:   clps_handle_publish(pt, ptlen); break;
	case M_PULL_REQ:  handle_pull_req(pt, ptlen, from); break;
	case M_PULL_RSP:  handle_pull_rsp(pt, ptlen); break;
	case M_TOMBSTONE:
		/* its own delete, already applied, looped back by the group
		 * (single-host fleets need the loop): counted only */
		if (addr_eq(from, &C.self_addr))
			__atomic_fetch_add(&C.st.tomb_own, 1, __ATOMIC_RELAXED);
		else
			(void)handle_tombstone(pt, ptlen);
		break;
	case M_TOMB_REQ:  handle_tomb_req(pt, ptlen, from); break;
	case M_TOMB_RPL:  handle_tomb_rpl(pt, ptlen, from); break;
	case M_TOMB_DONE: handle_tomb_done(pt, ptlen, from); break;
	case M_HOLD_Q:    handle_hold_q(pt, ptlen, from); break;
	case M_HOLD_A:    handle_hold_a(pt, ptlen); break;
	case M_GAP_REQ:   handle_gap_req(pt, ptlen, from); break;
	case M_GAP_FLOOR: handle_gap_floor(pt, ptlen, from); break;
	case M_HANDOVER: handle_handover(pt, ptlen, from); break;
	case M_ASKFILL:  handle_askfill(pt, ptlen, from); break;
	case M_MEMBERS_REQ: cladmit_members_req(pt, ptlen, from); break;
	case M_MEMBERS:  cladmit_members(pt, ptlen, from); break;
	case M_LINK_REPORT: cladmit_link_report(pt, ptlen, from); break;
	case M_ONLINE:   cladmit_online(pt, ptlen, from); break;
	case M_REBUILD:   handle_rebuild(pt, ptlen, from); break;
	case M_UPG_SWITCH: handle_upg_switch(pt, ptlen, from); break;
	case M_UPG_ACK:   handle_upg_ack(pt, ptlen, from); break;
	case M_FWD_OP:    handle_fwd_op(pt, ptlen, from); break;
	case M_FWD_ACK:   handle_fwd_ack(pt, ptlen); break;
	case M_FWD_JSON:  handle_fwd_json(pt, ptlen, from); break;
	case M_FWD_JACK:  handle_fwd_jack(pt, ptlen); break;
	case M_MIGRATE:   handle_migrate(pt, ptlen, from); break;
	case M_MIGRATE_MANY: handle_migrate_many(pt, ptlen, from, 0, 0); break;
	case M_REPL_MANY:    handle_migrate_many(pt, ptlen, from, 1, 0); break;
	case M_MIGRATE_ACK: {
		/* ANY ack frees a window slot; the stored-count (u16
		 * after the ok byte) credits migrated_out in RECORDS */
		uint32_t mreq;
		unsigned int mstored;
		int mok;

		if (!clmig_ack_parse(pt, ptlen, &mreq, &mok, &mstored))
			break;
		/* req 0 is an older peer acking a write-path push:
		 * it owns no window slot and must not free one */
		if (!mreq)
			break;
		/* a push group's ack: its id has the top bit, never a window slot's;
		 * an older peer's short ack does not say how many it stored: all kept */
		if (mreq & CLPUSH_ACK_TAG) {
			struct clpush_ack *pa = clpush_ack_take(&C.wp, mreq,
				from);

			if (pa) {
				spread_ack_drop(pa, ptlen >= CLMIG_ACK_LEN ?
					mstored : 0);
				clpush_ack_done(&C.wp, pa);
			}
			break;
		}
		/* only a matching ack frees a slot and credits migrated_out; a duplicate
		 * or an ack for a slot written off as lost returns 0 */
		if (!clmig_ack(&C.mig, mreq))
			break;
		/* the long ack credits RECORDS; the short one from an
		 * older peer can only say "at least one" */
		if (ptlen >= CLMIG_ACK_LEN)
			C.px.migrated_out += mstored;
		else if (mok)
			C.px.migrated_out++;
		break; }
	case M_DEMOTE:    handle_demote(pt, ptlen); break;
	case M_JOIN_REQ:     handle_join_req(pt, ptlen, from); break;
	case M_JOIN_REJ:     handle_join_rej(pt, ptlen, from); break;
	case M_JOIN_WAIT:    handle_join_wait(pt, ptlen, from); break;
	case M_ASSIGN:       handle_assign(pt, ptlen, from); break;
	case M_MASTER_ALIVE: handle_master_alive(pt, ptlen, from); break;
	case M_ALIVE:        handle_alive(pt, ptlen, from); break;
	case M_GOODBYE:      handle_goodbye(pt, ptlen, from); break;
	case M_FLEETSTOP:    handle_fleetstop(pt, ptlen, from); break;
	case M_CLMAP:        handle_clmap(pt, ptlen, from); break;
	case M_CLSYNC:       handle_clsync(pt, ptlen, from); break;
	case M_CLSYNC_ACK:   handle_clsync_ack(pt, ptlen); break;
	case M_PSPROBE_ACK:  clps_handle_probe_ack(pt, ptlen, from); break;
	case M_RPPROBE_ACK:  clrp_handle_probe_ack(pt, ptlen, from); break;
	case M_PSINT_ADD:    clps_handle_int_add(pt, ptlen, from); break;
	case M_PSINT_REQ:    clps_handle_int_req(pt, ptlen, from); break;
	case M_PSINT_FULL:   clps_handle_int_full(pt, ptlen, from); break;
	case M_UNKCMD:       handle_unkcmd(pt, ptlen, from); break;
	case M_PEER_HINT:    handle_peer_hint(pt, ptlen, from); break;
	case M_LANE_FORCE:   handle_lane_force(from); break;
	case M_LANE_PROBE:   handle_lane_probe(from); break;
	case M_LANE_PROBE_R: handle_lane_probe_r(from); break;
	case M_MPROBE:       handle_mprobe(pt, ptlen, from); break;
	case M_MPROBE_R:     handle_mprobe_r(pt, ptlen, from); break;
	case M_STATS_RESET:  handle_statsreset(pt, ptlen, from); break;
	case M_ALERT_RELAY:  handle_alertrelay(pt, ptlen, from); break;
	default:
		break;
	}
	if (cl_unicast && (pt[0] == M_ALIVE || pt[0] == M_CLMAP ||
	        pt[0] == M_MASTER_ALIVE) && sender_admitted(from))
		mesh_note(from, pt[0], 0);    /* see above - confirmed */
	rx_hdr = NULL;                 /* only during a dispatch */
	return 1;
}

static int drain_one_fd(int fd, int slot)
{
	unsigned char buf[HDR_LEN + MAX_DGRAM + 64];
	unsigned char pt[MAX_DGRAM + 16];
	int handled = 0, seen = 0;

	while (seen++ < DRAIN_BATCH) {
		pc_addr from;
		struct msghdr mh;

		prog(PC_PROG_DGRAM);
		struct iovec iov;
		struct cmsghdr *cm;
		char cbuf[CMSG_SPACE(sizeof(unsigned int)) +
#ifdef IP_PKTINFO
			CMSG_SPACE(sizeof(struct in_pktinfo)) +
#endif
			32];
		ssize_t r;
		int stray = 0;

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
		pc_iostat_udp_rx(PC_UDP_CLUSTER, 1, r > 0);

		if (r <= 0)
			break;
		for (cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
			if (cm->cmsg_level == SOL_SOCKET &&
			        cm->cmsg_type == SO_RXQ_OVFL) {
				unsigned int d;

				memcpy(&d, CMSG_DATA(cm), sizeof d);
				note_ovfl(slot, d);
			}
#ifdef IP_PKTINFO
			/* the multicast socket shares ANY:port with the host's other daemons and
			 * gets unicasts for unbound local addresses; this node's arrive on C.fd */
			if (slot == 1 && cm->cmsg_level == IPPROTO_IP &&
			        cm->cmsg_type == IP_PKTINFO) {
				struct in_pktinfo pi;

				memcpy(&pi, CMSG_DATA(cm), sizeof pi);
				stray = pi.ipi_addr.s_addr !=
					C.mcast_dst.in4.sin_addr.s_addr;
				if (stray && pc_logrl_ok(LRL_MCAST_STRAY, &from)) {
					char to[INET_ADDRSTRLEN];

					inet_ntop(AF_INET, &pi.ipi_addr, to, sizeof to);
					LM_NOTICE("cluster: dropped a datagram from %s "
						"for %s on the multicast socket - another "
						"address on this host, not this node's\n",
						pc_addr_str(&from), to);
				}
			}
#endif
		}
		if (stray) {
			C.st.mcast_stray++;
			continue;
		}
		handled += dispatch_dgram(buf, (size_t)r, &from, pt);
	}
	return handled;
}

/* frames the TCP lane received - the same dispatch as a
 * datagram, on this thread */
static int drain_lane(void)
{
	unsigned char buf[HDR_LEN + MAX_DGRAM + 64];
	unsigned char pt[MAX_DGRAM + 16];
	pc_addr from;
	size_t r;
	int n = 0, seen = 0;

	while (seen++ < DRAIN_BATCH &&
	        (r = clane_rx_take(buf, sizeof buf, &from)) > 0) {
		rx_via_lane = 1;
		n += dispatch_dgram(buf, r, &from, pt);
		rx_via_lane = 0;
	}
	return n;
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
	struct pollfd pf[3];
	long long last_hb = 0, last_sweep = 0;

	pf[0].fd = C.fd;
	pf[0].events = POLLIN;
	pf[1].fd = C.mfd;
	pf[1].events = POLLIN;
	pf[2].fd = clane_rx_fd();              /* -1 = off */
	pf[2].events = POLLIN;
	pc_qs_enter();                     /* tables in hand between waits */
	while (!*stop) {
		long long now = now_ms();
		int prc;

		prog(PC_PROG_LOOP);
		cladmit_link_tick(now);        /* every 500 ms while a link check runs */
		if (now - last_hb >= 1000) {
			static int tick;

			prog(PC_PROG_TICK);
			{
				/* the clean stop a fleet stop asked for: the same SIGTERM
				 * a service manager sends, so it takes the same road down */
				long long at = __atomic_load_n(
					&C.stop_self_at_ms, __ATOMIC_ACQUIRE);

				if (at && now >= at) {
					__atomic_store_n(&C.stop_self_at_ms, -1,
						__ATOMIC_RELEASE);
					kill(getpid(), SIGTERM);
				}
			}
			membership_tick();
			self_state_tick();
			upg_msg_tick();                /* asks and answers */
			pull_answer_tick();            /* the owner's fetched pulls */
			fwd_ack_tick();                /* the master's answers */
			fp_replay_tick();              /* every beat: the asker waits on it */
			replicas_short_tick(now);
			if (clretain_enabled())        /* every beat, */
				acks_forget_tick((unsigned int)(now / 1000));   /* not the 10-s tick */
			acks_gap_tick(now);
			acks_rebuild_tick(now);
			ps_probe_tick(now);
			clps_interest_tick(now);
			unk_tick(now);
			rz_again_tick(now);
			{
				unsigned int lost = clpush_ack_expire(&C.wp, now,
					SPREAD_ACK_WAIT_MS);

				C.px.spread_ack_kept += lost;
				C.px.spread_ack_lost += lost;
			}
			if (tick % ALIVE_STAMP_BEATS == 0)
				alive_stamp_touch();
			spread_missed_tick();      /* a peer owed writes it was skipped for */
			spread_reclaim_tick();     /* surplus copies, once the fleet has settled */
			if (++tick % REPL_SWEEP_BEATS == 0) {
				prog(PC_PROG_REBAL);
				pc_rebalance_tick();
			}
			last_hb = now;
			prog(PC_PROG_LOOP);
		}
		reconcile_pump(now);           /* the walk's slices */
		/* the join FSM needs sub-second deadlines */
		if (C.role == PC_ROLE_JOINING && now >= C.join_deadline_ms)
			membership_tick();
		{
			/* an open push group must not wait for the 100 ms poll, nor a
			 * request on the pending table for its timeout (PEND_SWEEP_MS) */
			int waiting = clpend_used(&C.pd) > 0, to;

			if (waiting && now - last_sweep >= PEND_SWEEP_MS) {
				last_sweep = now;
				expire_pending();
			}
			to = clpush_open(&C.wp) ? REPL_FLUSH_MS : 100;
			if (waiting && to > PEND_SWEEP_MS)
				to = PEND_SWEEP_MS;
			pc_qs_exit();          /* parked across the wait */
			prc = poll(pf, 3, to);
			pc_qs_enter();
		}
		prog(PC_PROG_FLUSH);
		if (prc <= 0) {
			repl_flush_due(now_ms());
			continue;
		}
		drain_dgrams();
		if (pf[2].fd >= 0 && (pf[2].revents & POLLIN))
			drain_lane();
		prog(PC_PROG_FLUSH);
		repl_flush_due(now_ms());
	}
	pc_qs_exit();
	alive_stamp_touch();               /* a clean stop dates its death */
	if (C.role != PC_ROLE_JOINING) {
		pc_node_state_set(PC_NST_DRAINING);
		send_goodbye();                /* leave cleanly: peers purge now */
	}
}

/* ---- forwarded JSON path ops (proxy mode) ------------------------------- */

extern const char pc_json_wrongtype_msg[];      /* verbs.c */

static void handle_fwd_json(const unsigned char *pt, size_t n,
		const pc_addr *from)
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
		/* 3 = the key holds a string (redis_types = strict) */
		st = rc == 0 ? 0 : rc == 1 ? 1 :
			emsg == pc_json_wrongtype_msg ? 3 : 2;
	}
	if (fraglen > (int)MAX_FWD_VAL) {  /* fragment too big to ship */
		/* logged: a reply over the ack ceiling otherwise failed
		 * through a forwarder with nothing to show for it */
		C.st.fwd_reply_too_big++;
		LM_WARN("cluster: a forwarded reply of %d bytes (jop %d) exceeds "
			"the %u-byte ack ceiling - refused\n", fraglen, (int)jop,
			(unsigned)MAX_FWD_VAL);
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

		/* frag is borrowed from pc_json_rmw's thread buffer */
		seal_send(from, ack, an);
	}
}

static void handle_fwd_jack(const unsigned char *pt, size_t n)
{
	uint32_t req;
	long long newval = 0;
	unsigned int fraglen;
	int worker = -1, kind = 0, st, jop, cnt;
	const char *frag;

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
		frag = a.frag;         /* NULL already when fraglen is 0 */
	}
	/* the JSON ack retires whether or not the request was answered -
	 * unlike the FWD ack above; see clpend_retire */
	if (!clpend_retire(&C.pd, req, 0, &worker, &kind))
		return;
	if (kind != PC_DONE_JSON)
		return;
	post_done_ex(worker, req, PC_DONE_JSON, st == 0, st, newval, 0, 0,
		(const unsigned char *)frag, (int)fraglen, jop, cnt, 0, 0);
}

/* ---- the rebalancer (donor-initiated, hysteresis, byte budget) ---------- */

void pc_proxy_get_stats(struct pc_proxy_stats *out)
{
	*out = C.px;
	out->loc_hits = clloc_loc_hits(&C.lc);
	out->loc_clears = clloc_loc_clears(&C.lc);
}

/* digest-gated sweep: a slot bitmap, and the slots the running repair walk is confined to
 * (NULL when the cycle is not gated) */
#define SG_BIT(b, i)   ((b)[(i) >> 3] & (1u << ((i) & 7)))
#define SG_SET(b, i)   ((b)[(i) >> 3] |= (unsigned char)(1u << ((i) & 7)))
static const unsigned char *sg_walk_filter;

struct mig_ctx {
	const char *col;
	struct peer *to;
	long long budget;
	unsigned int now;
	unsigned int min_age;          /* coldest-first threshold, 0 = all */
	int shard;                     /* reshard run: move ONLY the keys
	                                * whose HRW owner is @to */
	int repl;                      /* eager store: COPY records newer than @since
	                                * to @to; the local record stays */
	unsigned int since;
	int incl_passive;              /* one-shot backfill: send copies too */
	int stopped;                   /* the walk hit the budget */
	const char *only_col;          /* restrict the run to one collection
	                                * (per-collection marks), NULL = all */
	long long scan_deadline_ms;    /* walk deadline so a tick never costs heartbeats;
	                                * unreached victims resume next tick, 0 = none */
	unsigned scanned;              /* records seen, for the pump gate */
	int held;                      /* recovered records held back */
};

static int mig_cb(const str *key, const str *val,
		const struct pcache_rec_meta *mt, void *arg)
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
			return -1;
		}
	}
	if (self_mig_scan_recs && m->scanned > self_mig_scan_recs) {
		m->stopped = 1;
		return -1;
	}
	if (m->budget <= 0) {
		m->stopped = 1;
		return -1;
	}
	if (mt->expires) {
		/* a record dying before its next read is not worth moving; for the repair sweep
		 * that horizon is one sweep period (pc_repl_push has no threshold) */
		unsigned int horizon = m->repl ? REPL_SWEEP_BEATS : 60;

		if (mt->expires <= m->now + horizon) {
			if (m->repl)
				C.px.repl_skipped_dying++;
			return 0;
		}
		ttl_left = mt->expires - m->now;
	}
	if (key->len > 4096)
		return 0;
	if (m->repl) {
		/* eager sends only what this node authored: re-sending a PASSIVE (received) copy
		 * would echo the keyspace around the fleet forever */
		if ((mt->rflags & PCACHE_F_PASSIVE) && !m->incl_passive)
			return 0;
		/* since == 0 is no lower bound at all: a record written in the daemon's first
		 * second carries wtick 0 */
		if (m->since && mt->wtick <= m->since)
			return 0;
		/* a recovered record waits for reconcile: expiry leaves no tombstone, so pushing
		 * a restored copy could resurrect a key that expired everywhere else */
		/* not for backfill/set-repair (incl_passive): reconcile waits for a cold peer,
		 * so holding the walk that warms it would hold both forever */
		if (!m->incl_passive && rec_held(mt->wtick)) {
			m->held++;
			return 0;
		}
	}
	if (m->shard) {
		/* the ownership pass: shard keys move by HRW owner */
		if (pc_shard_owner(m->col, cn, key->s,
		        (size_t)key->len) != m->to->node)
			return 0;
	}
	/* the sweep repairs toward the push's replica set only; unscoped, it would copy every
	 * record to every peer and turn spread back into eager */
	{
		int kk = pc_cluster_replicas();

		if (m->repl && kk &&
		        !pc_spread_in_set(pc_key_slot(key->s, (size_t)key->len),
		                m->to->node, kk))
			return 0;
	}
	/* a gated cycle walks only the slots whose digests differed */
	if (m->repl && sg_walk_filter &&
	        !SG_BIT(sg_walk_filter, pc_key_slot(key->s, (size_t)key->len)))
		return 0;
	/* coldest-first: pass A computed a minimum AGE for this tick's
	 * victims - younger records stay put */
	if (m->min_age) {
		unsigned int age = m->now >= mt->wtick ? m->now - mt->wtick
			: 0xffffffffu;

		if (mt->wtick && age < m->min_age)
			return 0;
	}

	/* stub first, never reinstate: delete and locate at @to BEFORE sending - a lost
	 * transfer is a miss, never a fork; a replication copy just retries next tick */
	ht = pc_store_find(m->col, cn);
	if (!ht) {
		m->stopped = 1;
		return -1;
	}
	k.s = key->s;
	k.len = key->len;
	n = MIG_RHDR + cn + (size_t)key->len + (size_t)val->len;
	/* room first: a stubbed record that cannot be sent is gone; this one waits for the next tick */
	if (val->len > MAX_FWD_VAL ? C.bulkb_len + n > MIG_BATCH_CAP
	    : C.migb.len + 2 + MIG_GHDR + n > C.migb.cap) {
		m->stopped = 1;
		return -1;
	}
	if (!m->repl) {
		if (pcache_ht_remove(ht, &k) != 1)
			return 0;
		pc_wal_del(m->col, key->s, key->len);
		pc_loc_set(m->col, cn, key->s, (size_t)key->len, m->to->node);
	} else {
		C.px.repl_out++;
	}

	/* a record above the datagram ceiling goes to the bulk TCP batch (same record format) */
	if (val->len > MAX_FWD_VAL) {
		{
			unsigned char *rp = C.bulkb + C.bulkb_len;
			struct clmig_rec br = {
				.ttl_left = ttl_left,
				.col = m->col, .collen = cn,
				.key = key->s, .klen = (unsigned int)key->len,
				.val = val->s, .vlen = (unsigned int)val->len,
				.ver = mt->ver,
				.flags = (unsigned char)(mt->rflags & PCACHE_F_CARRY)
			};

			clmig_rec_write(rp, &br);
		}
		C.bulkb_len += n;
		C.bulk_recs++;
		m->budget -= (long long)n;
		return 0;
	}

	/* collect, never send here: the pump paces the gathered groups through the ack window
	 * once the scan drops its locks; record [ttl4][cn1][klen2][vlen4][ver8][col][key][val] */
	{
		int opened = clmig_open_for(&C.migb, n, MIG_GHDR,
			MIG_GATHER_CAP, &msg);

		if (opened < 0) {
			m->stopped = 1;
			return -1;
		}
		if (opened) {
			/* a fresh group: the header CONTENTS belong to this protocol, not clmig */
			/* 31 bits: a group id must never look like a parked-request
			 * handle (PEND_TAG) */
			req = ++C.next_req & ~PEND_TAG;
			if (!req)
				req = ++C.next_req & ~PEND_TAG;
			clmig_group_hdr(msg, MIG_GHDR,
				m->repl ? M_REPL_MANY : M_MIGRATE_MANY,
				req, C.node_id, 0);   /* patched as records land */
		}
	}
	{
		struct clmig_rec r = {
			.ttl_left = ttl_left,
			.col = m->col, .collen = cn,
			.key = key->s, .klen = (unsigned int)key->len,
			.val = val->s, .vlen = (unsigned int)val->len,
			.ver = mt->ver,
			.flags = (unsigned char)(mt->rflags & PCACHE_F_CARRY),
		};

		clmig_rec_write(clmig_record_at(&C.migb), &r);
		clmig_record_done(&C.migb, n, 7);   /* count2 sits at +7 */
	}
	m->budget -= (long long)n;
	return 0;
}

/* coldest-first pass A: a log2-age byte histogram picks the age threshold whose older side
 * covers the tick budget, without copying a key */
struct age_hist {
	unsigned long long bytes[32];  /* age is 32-bit: floor(log2) is 0..31 */
	unsigned int now;
};

static int age_cb(const str *key, const str *val,
		const struct pcache_rec_meta *mt, void *arg)
{
	struct age_hist *h = arg;
	unsigned int age, b;

	if (key->len > 4096)
		return 0;
	age = (!mt->wtick || h->now < mt->wtick) ? 0xffffffffu
		: h->now - mt->wtick;
	b = 0;
	while (b < 31 && (age >> (b + 1)))
		b++;                           /* b = floor(log2(age)); the never-written
	                                    * 0xffffffff sentinel lands in 31, the coldest */
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

/* a re-send is idempotent: the receiver stores a duplicate as "not newer" and acks
 * again; the ack handler frees the slot on the first ack and ignores the rest */
/* clmig hands back each due datagram's offset, so it needs no seal_send of its own */
static void mig_resend_one(void *ctx, size_t off)
{
	const pc_addr *to = ctx;

	seal_send(to, C.migb.b + off + 2, pc_g16(C.migb.b + off));
	C.px.migrate_retx++;
}

static void mig_retransmit(const pc_addr *to)
{
	clmig_retransmit(&C.mig, now_ms(), mig_resend_one, (void *)to);
}

/* the run's target slot now holds another node, or none */
static int mig_slot_moved(const struct peer *p, int node0, const pc_addr *to0)
{
	return __atomic_load_n(&p->node, __ATOMIC_ACQUIRE) != node0 || !addr_eq(&p->addr, to0);
}

/* one 20 ms turn of the migration pump: datagrams, and frames the lane received */
static void mig_pump_poll(struct pollfd pf[2])
{
	if (poll(pf, 2, 20) <= 0)
		return;
	drain_dgrams();
	if (pf[1].fd >= 0 && (pf[1].revents & POLLIN))
		drain_lane();
}

/* a table walk of the sweep or a migration; -1 when it could not run, as pcache_ht_iter_meta */
static int cl_walk(pcache_htable_t *ht, unsigned int *cursor, pcache_iter_meta_cb cb, void *ctx)
{
	if (self_walk_nomem_until && now_ms() < self_walk_nomem_until)
		return -1;
	return cursor ? pcache_ht_iter_meta_from(ht, cursor, cb, ctx)
		: pcache_ht_iter_meta(ht, cb, ctx);
}

/* a shard or proxy walk resumes where the last tick's stopped: shard per (peer slot, collection),
 * proxy per collection; a slot that changed hands starts over */
static unsigned int shard_cur[PC_CL_MAXPEER][PC_MAX_COLLECTIONS];
static int shard_cur_node[PC_CL_MAXPEER];
static unsigned int proxy_cur[PC_MAX_COLLECTIONS];

static unsigned int *mig_cursor(const struct peer *to, int shard, int c)
{
	long i = to - C.pt.peers;

	if (c < 0 || c >= PC_MAX_COLLECTIONS)
		return NULL;
	if (!shard)
		return &proxy_cur[c];
	if (i < 0 || i >= PC_CL_MAXPEER)
		return NULL;
	if (shard_cur_node[i] != to->node) {
		memset(shard_cur[i], 0, sizeof shard_cur[i]);
		shard_cur_node[i] = to->node;
	}
	return &shard_cur[i][c];
}

static int run_migration(struct peer *to, long long budget, int mode,
		unsigned int since, const char *only_col, int *clean,
		unsigned int *cursor, int incl_passive)
{
	struct mig_ctx m;
	int i, moved = 0, unfinished = 0, failed = 0, moved_away = 0;
	/* the pump dispatches datagrams: a goodbye and a join there can hand the slot to
	 * another address, and nothing of this run is that node's */
	int node0 = to->node;
	pc_addr to0 = to->addr;

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
	m.held = 0;
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
			continue;
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
		{
			unsigned int *cur = cursor ? cursor
				: m.repl ? NULL : mig_cursor(to, m.shard, i);

			/* -1 with nothing stopping it: the walk could not run, its cursor untouched */
			if (cl_walk(ht, cur, mig_cb, &m) < 0 && !m.stopped)
				failed = 1;
			unfinished |= cur && !cursor && *cur;
		}
	}
	/* a walk that has not reached the table's end yet keeps the shard grace open too */
	moved = C.migb.len > 0 || C.bulkb_len > 0 || (m.shard && unfinished);
	C.last_run_scanned = m.scanned;

	/* the pump: an ack-gated window never overruns the receiver's UDP buffer; victims are
	 * already stubbed, so one undelivered by the deadline is a counted miss */
	if (C.migb.len) {
		long long deadline = now_ms() + 5000;
		size_t off = 0;
		unsigned int sent = 0, lost = 0, n = 0;
		const unsigned char *gp = NULL;
		/* the lane too: a forced peer's acks come back by it */
		struct pollfd pf[2] = { { .fd = C.fd, .events = POLLIN },
			{ .fd = clane_rx_fd(), .events = POLLIN } };

		while (clmig_group_walk(&C.migb, &off, &gp, &n)) {
			while (clmig_inflight(&C.mig) >= MIG_WINDOW &&
			        now_ms() < deadline) {
				mig_pump_poll(pf);
				if ((moved_away = mig_slot_moved(m.to, node0, &to0)))
					break;
				beat_pump();   /* the ack window may poll for as
				                * long as PEER_UP_MS - never mute */
				mig_retransmit(&to0);
			}
			if (moved_away || now_ms() >= deadline) {
				lost += clmig_group_count(gp);   /* walked past, never sent */
				break;
			}
			seal_send(&to0, gp, n);
			/* track each group: a loss is charged in records,
			 * and a late ack costs one re-send, not a cycle */
			/* gp sits just past the group's len2 header; `off` has already moved on */
			clmig_track(&C.mig, clmig_group_req(gp), clmig_group_count(gp),
				(size_t)(gp - C.migb.b) - 2, now_ms());
			C.px.migrate_dgrams++;
			sent++;
		}
		while (!moved_away && clmig_inflight(&C.mig) > 0 && now_ms() < deadline) {
			mig_pump_poll(pf);
			if ((moved_away = mig_slot_moved(m.to, node0, &to0)))
				break;
			beat_pump();
			mig_retransmit(&to0);
		}
		/* losses in records: unsent groups plus sends still unacked
		 * after MIG_RETX re-sends each */
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
			/* with a cursor, the cursor reports the walk's end, so
			 * this only says whether the slice lost anything */
			*clean = !moved_away &&
				(cursor ? lost == 0 : (!m.stopped && lost == 0));
	} else if (clean) {
		*clean = cursor ? 1 : !m.stopped;  /* nothing to send: clean
		                                * = the peer is in sync */
	}

	/* oversized victims (already stubbed) go to the bulk thread */
	if (C.bulkb_len) {
		if (!moved_away && clbulk_offer(&C.bk, C.bulkb, C.bulkb_len, C.bulk_recs,
		        m.repl ? CLBULK_KIND_REPLICA : CLBULK_KIND_MIGRATE,
		        &to0)) {
			C.bulkb = NULL;        /* re-alloc next tick */
		} else if (m.repl) {
			/* repair: the record is still here, so a refused offer only marks
			 * THIS peer's cycle unclean - a node-wide flag would pin every mark */
			__atomic_fetch_add(&C.px.repl_bulk_lost, 1,
				__ATOMIC_RELAXED);
			if (clean)
				*clean = 0;
		} else {
			/* the bulk thread is still on the last batch, or the target left:
			 * these stubs are lost - honest misses, counted */
			C.px.migrate_lost += C.bulk_recs;
			LM_WARN("rebalancer: %s, %u oversized record(s) dropped to "
				"misses\n", moved_away ? "the target left" :
				"bulk channel busy", C.bulk_recs);
		}
		C.bulkb_len = 0;
		C.bulk_recs = 0;
	}
	if (m.held) {
		C.st.repl_held_recovered += (unsigned long long)m.held;
		if (clean)
			*clean = 0;        /* the mark must not pass them */
	}
	if (failed && clean)
		*clean = 0;                /* nothing was scanned: no cycle ended */
	return moved;
}

/* eager store: a peer's mark advances to the sweep's start tick only after a CLEAN pass, so
 * a loss or budget cut just retries; a rejoined peer's zeroed marks force a full resync */
/* backfill for @target is sent by the lowest live member besides it, so an empty node gets
 * the keyspace once; two senders in a race are absorbed by the version compare */
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
		if (p->nstate == PC_NST_RECOVERING || p->nstate == PC_NST_CONNECTING)
			continue;              /* still pulling its own, or not admitted yet */
		/* a candidate that holds nothing (its ALIVE count) can send nothing, at any age */
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

/* a live peer past its admission (not CONNECTING, not STALLED): one that can be elected */
static int admitted_peer_live(long long now)
{
	int i;

	for (i = 0; i < C.pt.n_peers; i++)
		if (C.pt.peers[i].node && clpeers_usable(&C.pt.peers[i], now) &&
		        C.pt.peers[i].nstate != PC_NST_STALLED &&
		        !addr_eq(&C.pt.peers[i].addr, &C.master_addr))
			return 1;
	return 0;
}

/* a write was skipped to this peer while it was CONNECTING (the writer kept a copy): once it is
 * usable and placed, a walk sends what it now holds.  Every beat */
static void spread_missed_tick(void)
{
	long long now = now_ms();
	int i, c;

	if (!pc_cluster_replicas())
		return;
	for (i = 0; i < C.pt.n_peers; i++) {
		struct peer *p = &C.pt.peers[i];
		int id = __atomic_load_n(&p->node, __ATOMIC_ACQUIRE);

		if (!__atomic_load_n(&p->missed, __ATOMIC_RELAXED))
			continue;
		if (!clpeers_holds_slots(p, now)) {
			__atomic_store_n(&p->missed, 0, __ATOMIC_RELAXED);
			continue;              /* gone past its grace: the set moved without it */
		}
		if (!id || !clpeers_usable(p, now) || p->nstate == PC_NST_RECOVERING)
			continue;
		if (cl_map_usable() && !pc_clmap_placeable(pc_clmap_find(cl_map(), (uint16_t)id)))
			continue;              /* not placed yet: a walk now would send nothing */
		__atomic_store_n(&p->missed, 0, __ATOMIC_RELAXED);
		for (c = 0; c < PC_MAX_COLLECTIONS; c++) {
			p->setrepair[c] = 1;
			p->repl_cursor[c] = 0;
		}
		LM_NOTICE("cluster: node %d was skipped by writes while CONNECTING - re-placing the "
			"keys it now holds\n", id);
	}
}

/* a node the incoming map makes placeable is owed what it now holds: a write skipped while it was
 * CONNECTING reached no one, and reclaim must not drop the other copy before the walk sends it */
static void spread_note_placeable(const struct pc_clmap *m)
{
	long long now = now_ms();
	unsigned int i;
	int j, c;

	if (!pc_cluster_replicas() || !C.map_valid)
		return;                        /* the first map after a start re-places nothing */
	for (i = 0; i < m->nnodes; i++) {
		const struct pc_clmap_node *nd = &m->node[i];

		/* keys move to it from holders that were never skipped (formation's placement shifts),
		 * so every holder walks; spread_missed_tick() covers a node skipped while still placed */
		if (nd->node_id == (uint16_t)C.node_id || !pc_clmap_placeable(nd) ||
		        pc_clmap_placeable(pc_clmap_find(&C.map, nd->node_id)))
			continue;
		for (j = 0; j < C.pt.n_peers; j++) {
			struct peer *p = &C.pt.peers[j];

			if (__atomic_load_n(&p->node, __ATOMIC_ACQUIRE) != (int)nd->node_id ||
			        !clpeers_live(p, now))
				continue;
			for (c = 0; c < PC_MAX_COLLECTIONS; c++) {
				p->setrepair[c] = 1;
				p->repl_cursor[c] = 0;
			}
			LM_NOTICE("cluster: node %u is placeable in the new map - re-placing the "
				"keys it now holds\n", nd->node_id);
		}
	}
}

/* a change in the spread holder set arms a backfill (from zero, copies included): a newly
 * promoted holder can only get a key from a holder that received it */
static void spread_note_set_change(void)
{
	static unsigned long long last_fp;
	unsigned long long fp;
	long long now = now_ms();
	int i, c;

	if (!pc_cluster_replicas())
		return;
	fp = clspread_set_fp(&C.pt, now);      /* who is in the set */
	if (fp == last_fp)
		return;
	if (last_fp) {
		LM_NOTICE("cluster: the spread holder set changed - "
			"re-placing the keys whose holders moved\n");
		for (i = 0; i < C.pt.n_peers; i++) {
			if (!clpeers_live(&C.pt.peers[i], now))
				continue;
			for (c = 0; c < PC_MAX_COLLECTIONS; c++) {
				C.pt.peers[i].setrepair[c] = 1;
				C.pt.peers[i].repl_cursor[c] = 0;
			}
		}
		C.px.spread_repaired++;
	}
	last_fp = fp;
}

/* ---- the digest-gated sweep ------------------------------------------------
 * re-sends only slots whose digests differ; the receiver's version check still decides */
#define SG_PER_DGRAM   4000
#define SG_TIMEOUT_MS  8000
#define SG_FAILS       3
#define SG_OFF_MS      (10LL * 60 * 1000)

enum { SG_OFF, SG_SKIP, SG_WAIT, SG_WALK };

struct sg_col {
	unsigned char st;              /* 0 idle, 1 asked, 2 walking diffs */
	uint32_t req;
	unsigned char parts, got;
	unsigned char got_part[32];    /* which parts answered: a duplicated
	                                * reply must not count twice */
	long long deadline_ms;
	unsigned int start;            /* the tick the digests were taken */
	unsigned int rot;              /* the anti-entropy window's turn */
	unsigned char diff[PC_SLOTS / 8];
};
/* each cycle also compares 1/SG_ROT of all slots: a write whose push AND repeat were lost
 * sits below the mark where only this rotation looks */
#define SG_ROT 64
struct sg_peer {
	int node;                      /* whom this is for: a slot is reused */
	int fails;
	long long off_until_ms;
	struct sg_col *c[PC_MAX_COLLECTIONS];
};
static struct sg_peer *sg_tab[PC_CL_MAXPEER];
static uint32_t sg_next_req;
static int pc_sweep_digests = 1;       /* [cluster] sweep_digests */

void pc_cluster_set_sweep_digests(int on)
{
	pc_sweep_digests = on ? 1 : 0;
}

static int pc_mcast_ttl = 1;           /* [cluster] multicast_ttl */
static const char *pc_cid_pin;         /* [cluster] cluster_id */

void pc_cluster_set_cluster_id(const char *pin)
{
	pc_cid_pin = pin;
}
static int pc_max_members;             /* 0 = PC_CL_MAXPEER + 1 */

void pc_cluster_set_max_members(int n)
{
	pc_max_members = n < 0 ? 0 : n > PC_CL_MAXPEER + 1 ? PC_CL_MAXPEER + 1 : n;
}

/* would a NEW member at @from pass the fleet's ceiling?  A peer already holding an id is
 * re-joining and never counts */
static int join_over_limit(const pc_addr *from, int *limit)
{
	int lim = pc_max_members ? pc_max_members : PC_CL_MAXPEER + 1;
	int i, n = 1;                          /* this node */

	*limit = lim;
	for (i = 0; i < C.pt.n_peers; i++) {
		if (!__atomic_load_n(&C.pt.peers[i].node, __ATOMIC_ACQUIRE))
			continue;
		if (addr_eq(&C.pt.peers[i].addr, from))
			return 0;                  /* already a member */
		n++;
	}
	return n >= lim;
}

void pc_cluster_set_multicast_ttl(int ttl)
{
	pc_mcast_ttl = ttl < 1 ? 1 : ttl > 255 ? 255 : ttl;
}

int pc_cluster_multicast_ttl(void)
{
	return pc_mcast_ttl;
}

/* the peer side: this node's digests, recomputed at most once a tick per
 * collection - the parts of one request, and several askers, share it */
struct sg_mine {
	unsigned int tick;
	unsigned long long pub;
	uint64_t dig[PC_SLOTS];
};
static struct sg_mine *sg_mine[PC_MAX_COLLECTIONS];

struct sg_walk {
	uint64_t *dig;
	unsigned char *has;            /* slots holding a record here */
	unsigned char *cand;           /* sender: authored above the mark */
	unsigned char *ungated;        /* sender: a slot not ours to gate */
	int peer;                      /* sender: the peer; 0 = peer side */
	int kk;
	unsigned int now, mark;
	unsigned scanned;
};

static int sg_walk_cb(const str *key, const str *val,
		const struct pcache_rec_meta *mt, void *arg)
{
	struct sg_walk *w = arg;
	unsigned int slot;

	if ((w->scanned++ & 511) == 0)
		beat_pump();
	/* the sweep's own dying-soon skip, so expiry lag does not churn */
	if (mt->expires && mt->expires <= w->now + REPL_SWEEP_BEATS)
		return 0;
	slot = pc_key_slot(key->s, (size_t)key->len);
	if (w->peer) {
		/* spread: only slots both nodes hold; a surplus copy's slot
		 * is never gated (it would always differ) */
		if (w->kk && !pc_spread_in_set(slot, w->peer, w->kk))
			return 0;
		if (w->kk && !pc_spread_in_set(slot, 0, w->kk)) {
			SG_SET(w->ungated, slot);
			return 0;
		}
		if (!(mt->rflags & PCACHE_F_PASSIVE) &&
		        (!w->mark || mt->wtick > w->mark))
			SG_SET(w->cand, slot);
	}
	w->dig[slot] ^= sg_mix(key->s, (size_t)key->len, mt->ver, val->s,
		(size_t)val->len);
	if (w->has)
		SG_SET(w->has, slot);
	return 0;
}

/* ask @to for its digests of the @ask slots, SG_PER_DGRAM per part (sweep gate and
 * reconcile); returns the req id and *parts, 0 when nothing or too much to ask */
static uint32_t sg_ask(const pc_addr *to, const char *col,
		size_t cl, const uint64_t *dig, const unsigned char *ask,
		unsigned int want, unsigned int *parts_out)
{
	unsigned char msg[64 + 256 + SG_PER_DGRAM * 10];
	unsigned int parts, p, s = 0, n;
	uint32_t req;

	*parts_out = 0;
	if (!want || cl > 255)
		return 0;
	parts = (want + SG_PER_DGRAM - 1) / SG_PER_DGRAM;
	if (parts > 255)
		return 0;
	req = ++sg_next_req ? sg_next_req : ++sg_next_req;
	for (p = 0; p < parts; p++) {
		unsigned char *q = msg, *np;

		*q++ = M_SWEEP_DIG;
		*q++ = (unsigned char)(req >> 24); *q++ = (unsigned char)(req >> 16);
		*q++ = (unsigned char)(req >> 8);  *q++ = (unsigned char)req;
		*q++ = (unsigned char)p;
		*q++ = (unsigned char)parts;
		*q++ = (unsigned char)cl;
		memcpy(q, col, cl); q += cl;
		np = q; q += 2;
		for (n = 0; s < PC_SLOTS && n < SG_PER_DGRAM; s++) {
			int b;

			if (!SG_BIT(ask, s))
				continue;
			*q++ = (unsigned char)(s >> 8); *q++ = (unsigned char)s;
			for (b = 56; b >= 0; b -= 8)
				*q++ = (unsigned char)(dig[s] >> b);
			n++;
		}
		np[0] = (unsigned char)(n >> 8); np[1] = (unsigned char)n;
		seal_send(to, msg, (size_t)(q - msg));
	}
	*parts_out = parts;
	return req;
}

static struct sg_col *sg_col_of(struct peer *pr, int i, int c)
{
	struct sg_peer *sp = sg_tab[i];

	if (!sp) {
		sp = sg_tab[i] = calloc(1, sizeof *sp);
		if (!sp)
			return NULL;
	}
	if (sp->node != pr->node) {        /* the slot changed hands */
		int k;

		for (k = 0; k < PC_MAX_COLLECTIONS; k++) {
			free(sp->c[k]);
			sp->c[k] = NULL;
		}
		sp->node = pr->node;
		sp->fails = 0;
		sp->off_until_ms = 0;
	}
	if (!sp->c[c])
		sp->c[c] = calloc(1, sizeof *sp->c[c]);
	return sp->c[c];
}

/* the sender's decision for one ordinary cycle of (peer, collection),
 * taken with the cursor at 0 */
static int sg_gate(struct peer *pr, int i, int c, long long now)
{
	pcache_htable_t *ht = pc_store_ht(c);
	struct sg_peer *sp;
	struct sg_col *g;
	struct sg_walk w;
	uint64_t *dig;
	unsigned char *cand, *ungated;
	const char *col = pc_store_name(c);
	size_t cl = strlen(col);
	unsigned int s, parts, want = 0;
	uint32_t req;

	if (!ht || cl > 255 || !(g = sg_col_of(pr, i, c)))
		return SG_OFF;
	sp = sg_tab[i];
	if (sp->off_until_ms && now < sp->off_until_ms)
		return SG_OFF;
	if (g->st == 1) {
		if (g->got >= g->parts) {
			sp->fails = 0;         /* every part answered */
			C.px.sweep_gate_answered++;
			for (s = 0; s < PC_SLOTS / 8 && !g->diff[s]; s++)
				;
			if (s == PC_SLOTS / 8) {
				/* all equal: no walk.  The mark moves to when the
				 * digests were TAKEN - later writes are unswept */
				pr->repl_mark[c] = g->start ? g->start - 1 : 0;
				g->st = 0;
				C.px.sweep_gate_clean++;
				return SG_SKIP;
			}
			g->st = 2;
			return SG_WALK;
		}
		if (now < g->deadline_ms)
			return SG_WAIT;
		g->st = 0;                 /* unanswered: the old sweep */
		C.px.sweep_gate_unanswered++;
		if (++sp->fails >= SG_FAILS) {
			sp->off_until_ms = now + SG_OFF_MS;
			LM_NOTICE("cluster: node %d does not answer sweep digests "
				"(%d rounds) - it gets the full sweep for %lld s\n",
				pr->node, sp->fails, SG_OFF_MS / 1000);
		}
		return SG_OFF;
	}
	if (g->st == 2)
		return SG_WALK;               /* mid-walk: resumed by the caller */

	dig = calloc(PC_SLOTS, sizeof *dig);
	cand = calloc(PC_SLOTS / 8, 1);
	ungated = calloc(PC_SLOTS / 8, 1);
	if (!dig || !cand || !ungated) {
		free(dig); free(cand); free(ungated);
		return SG_OFF;
	}
	memset(&w, 0, sizeof w);
	w.dig = dig;
	w.cand = cand;
	w.ungated = ungated;
	w.peer = pr->node;
	w.kk = pc_cluster_replicas();
	w.now = get_ticks();
	w.mark = pr->repl_mark[c];
	g->start = w.now;
	if (cl_walk(ht, NULL, sg_walk_cb, &w) < 0) {
		/* digests of nothing would match nothing and move the mark: the old sweep instead */
		free(dig); free(cand); free(ungated);
		return SG_OFF;
	}
	C.px.sweep_gate_digested += w.scanned;
	memset(g->diff, 0, sizeof g->diff);
	g->rot = (g->rot + 1) % SG_ROT;
	for (s = 0; s < PC_SLOTS; s++) {
		if (SG_BIT(ungated, s))
			SG_SET(g->diff, s);     /* never gated: always walked */
		else if (SG_BIT(cand, s) ||
		         (s % SG_ROT == g->rot &&
		          (!w.kk || (pc_spread_in_set(s, pr->node, w.kk) &&
		                     pc_spread_in_set(s, 0, w.kk))))) {
			/* rotation slots by PLACEMENT: an empty slot never reached sg_walk_cb, and
			 * one this node does not hold would always differ */
			SG_SET(cand, s);        /* compared this cycle */
			want++;
		}
	}
	if (!want) {
		/* nothing authored above the mark in a comparable slot: the
		 * old sweep would send nothing but the ungated slots */
		free(dig); free(cand); free(ungated);
		for (s = 0; s < PC_SLOTS / 8 && !g->diff[s]; s++)
			;
		if (s == PC_SLOTS / 8) {
			pr->repl_mark[c] = g->start ? g->start - 1 : 0;
			C.px.sweep_gate_idle++;
			return SG_SKIP;
		}
		g->st = 2;
		return SG_WALK;
	}
	/* the question, in parts: the compared slots are cand's */
	req = sg_ask(&pr->addr, col, cl, dig, cand, want, &parts);
	if (!req) {
		free(dig); free(cand); free(ungated);
		return SG_OFF;
	}
	C.px.sweep_gate_asked++;
	C.px.sweep_gate_slots_asked += want;
	free(dig); free(cand); free(ungated);
	g->req = req;
	g->parts = (unsigned char)parts;
	g->got = 0;
	memset(g->got_part, 0, sizeof g->got_part);
	g->deadline_ms = now + SG_TIMEOUT_MS;
	g->st = 1;
	return SG_WAIT;
}

/* ---- the post-restart reconcile, narrowed by the witness -----------------
 * only keys in slots whose digests differ from the witness's are probed; equal = unchanged */
struct rdg_col {
	uint32_t req;
	unsigned char parts, got;
	unsigned char got_part[32];
	unsigned char diff[PC_SLOTS / 8];
};
static struct rdg_col *rdg_c[PC_MAX_COLLECTIONS];
static int rdg_phase;                  /* 0 not asked, 1 asked, 2 narrowed,
                                        * 3 probe everything */
static long long rdg_deadline_ms;
static long long rdg_narrowed_ms;

static void rdg_drop(void)
{
	int c;

	for (c = 0; c < PC_MAX_COLLECTIONS; c++) {
		free(rdg_c[c]);
		rdg_c[c] = NULL;
	}
}

/* the answer to one part of the reconcile's question; 1 = it was ours */
static int rdg_answer(uint32_t req, unsigned int part,
		const unsigned char *slots, unsigned int cnt)
{
	unsigned int k;
	int c;

	if (rdg_phase != 1)
		return 0;
	for (c = 0; c < PC_MAX_COLLECTIONS; c++) {
		struct rdg_col *r = rdg_c[c];

		if (!r || r->req != req)
			continue;
		if (SG_BIT(r->got_part, part))
			return 1;
		SG_SET(r->got_part, part);
		for (k = 0; k < cnt; k++) {
			unsigned int slot = (unsigned int)slots[2 * k] << 8 |
				slots[2 * k + 1];

			if (slot < PC_SLOTS)
				SG_SET(r->diff, slot);
		}
		C.st.reconcile_dig_diff += cnt;
		r->got++;
		return 1;
	}
	return 0;
}

/* 1 = probe now (narrowed, or everything), 0 = still waiting */
static int reconcile_digest_step(void)
{
	struct peer *w;
	long long now = now_ms();
	int c, asked = 0, waiting = 0;

	if (rdg_phase >= 2)
		return rdg_phase == 3 || now - rdg_narrowed_ms >= self_recon_walk_wait_ms;
	if (rdg_phase == 0) {
		w = peer_by_node(C.recon.witness);
		if (!pc_sweep_digests || !w) {
			rdg_phase = 3;
			return 1;
		}
		for (c = 0; c < pc_store_count(); c++) {
			pcache_htable_t *ht;
			struct sg_walk sw;
			uint64_t *dig;
			unsigned char *has;
			const char *col;
			unsigned int s, want = 0, parts;
			uint32_t req;

			if (!pc_store_live(c) || !(ht = pc_store_ht(c)) ||
			        !pc_store_eager_enabled(ht) || c >= PC_MAX_COLLECTIONS)
				continue;          /* probed as before */
			col = pc_store_name(c);
			if (strlen(col) > 255)
				continue;
			dig = calloc(PC_SLOTS, sizeof *dig);
			has = calloc(PC_SLOTS / 8, 1);
			if (!dig || !has) {
				free(dig); free(has);
				continue;          /* this one probes everything */
			}
			memset(&sw, 0, sizeof sw);
			sw.dig = dig;
			sw.has = has;
			sw.now = get_ticks();
			(void)pcache_ht_iter_meta(ht, sg_walk_cb, &sw);
			for (s = 0; s < PC_SLOTS; s++)
				if (SG_BIT(has, s))
					want++;
			if (want && (rdg_c[c] = calloc(1, sizeof *rdg_c[c]))) {
				req = sg_ask(&w->addr, col, strlen(col), dig, has,
					want, &parts);
				if (req) {
					rdg_c[c]->req = req;
					rdg_c[c]->parts = (unsigned char)parts;
					asked++;
				} else {
					free(rdg_c[c]);
					rdg_c[c] = NULL;
				}
			}
			free(dig); free(has);
		}
		if (!asked) {
			rdg_phase = 3;
			return 1;
		}
		/* the pass starts at the question, so reconcile_ms counts
		 * the round trip and not only the probes it saved */
		if (!C.rec_start_ms)
			C.rec_start_ms = now;
		rdg_phase = 1;
		rdg_deadline_ms = now + SG_TIMEOUT_MS;
		C.st.reconcile_dig_asked++;
		return 0;
	}
	for (c = 0; c < PC_MAX_COLLECTIONS; c++)
		if (rdg_c[c] && rdg_c[c]->got < rdg_c[c]->parts)
			waiting++;
	if (!waiting) {
		rdg_phase = 2;
		rdg_narrowed_ms = now;
		LM_NOTICE("cluster: reconcile - node %d's digests differ in %llu "
			"slot(s); only the keys there are asked about\n",
			C.recon.witness, C.st.reconcile_dig_diff);
		return self_recon_walk_wait_ms <= 0;
	}
	if (now < rdg_deadline_ms)
		return 0;
	rdg_drop();
	rdg_phase = 3;
	C.st.reconcile_dig_fallback++;
	LM_NOTICE("cluster: reconcile - node %d did not answer the digests; "
		"asking about every replayed key\n", C.recon.witness);
	return 1;
}

/* the slots of collection @c the probe walk is confined to, or NULL */
static const unsigned char *reconcile_digest_filter(int c)
{
	return rdg_phase == 2 && c < PC_MAX_COLLECTIONS && rdg_c[c] ?
		rdg_c[c]->diff : NULL;
}

/* 1 = @key's slot matched the witness's digest: nothing to ask */
static int reconcile_digest_same(const unsigned char *diff, const str *key)
{
	return !SG_BIT(diff, pc_key_slot(key->s, (size_t)key->len));
}

/* the peer side: answer with the slots whose digests differ */
static void handle_sweep_dig(const unsigned char *pt, size_t n,
		const pc_addr *from)
{
	unsigned char out[16 + SG_PER_DGRAM * 2];
	unsigned char *q = out, *nq;
	unsigned int cl, cnt, k, diff = 0, now = get_ticks();
	pcache_htable_t *ht;
	struct sg_mine *m;
	char col[256];
	uint32_t req;
	int ci;

	if (n < 9)
		return;
	req = (uint32_t)pt[1] << 24 | (uint32_t)pt[2] << 16 |
		(uint32_t)pt[3] << 8 | pt[4];
	cl = pt[7];
	if (n < 8 + cl + 2)
		return;
	memcpy(col, pt + 8, cl);
	col[cl] = 0;
	cnt = (unsigned int)pt[8 + cl] << 8 | pt[9 + cl];
	if (n < 10 + cl + (size_t)cnt * 10 || cnt > SG_PER_DGRAM)
		return;
	ht = pc_store_find(col, cl);
	for (ci = 0; ci < pc_store_count(); ci++)
		if (pc_store_live(ci) && pc_store_ht(ci) == ht)
			break;
	if (!ht || ci == pc_store_count())
		return;                        /* no such collection: silence -
		                                * the asker sweeps as before */
	m = sg_mine[ci];
	if (!m && !(m = sg_mine[ci] = calloc(1, sizeof *m)))
		return;
	if (!m->tick || m->tick != now || m->pub != pc_store_pub_gen(ci)) {
		struct sg_walk w;

		memset(m->dig, 0, sizeof m->dig);
		memset(&w, 0, sizeof w);
		w.dig = m->dig;
		w.now = now;
		(void)pcache_ht_iter_meta(ht, sg_walk_cb, &w);
		m->tick = now;
		m->pub = pc_store_pub_gen(ci);
		C.px.sweep_gate_walks++;
	}
	*q++ = M_SWEEP_DIG_R;
	memcpy(q, pt + 1, 5); q += 5;      /* req and part, echoed */
	nq = q; q += 2;
	for (k = 0; k < cnt; k++) {
		const unsigned char *e = pt + 10 + cl + (size_t)k * 10;
		unsigned int slot = (unsigned int)e[0] << 8 | e[1];
		uint64_t d = 0;
		int b;

		for (b = 0; b < 8; b++)
			d = d << 8 | e[2 + b];
		if (slot >= PC_SLOTS || m->dig[slot] != d) {
			*q++ = e[0]; *q++ = e[1];
			diff++;
		}
	}
	nq[0] = (unsigned char)(diff >> 8); nq[1] = (unsigned char)diff;
	seal_send(from, out, (size_t)(q - out));
	(void)req;
}

/* the sender side: the answer to one part */
static void handle_sweep_dig_r(const unsigned char *pt, size_t n)
{
	uint32_t req;
	unsigned int cnt, k;
	int i, c;

	if (n < 8)
		return;
	req = (uint32_t)pt[1] << 24 | (uint32_t)pt[2] << 16 |
		(uint32_t)pt[3] << 8 | pt[4];
	cnt = (unsigned int)pt[6] << 8 | pt[7];
	if (n < 8 + (size_t)cnt * 2)
		return;
	if (rdg_answer(req, pt[5], pt + 8, cnt))
		return;                        /* the reconcile's round */
	for (i = 0; i < PC_CL_MAXPEER; i++) {
		struct sg_peer *sp = sg_tab[i];

		if (!sp)
			continue;
		for (c = 0; c < PC_MAX_COLLECTIONS; c++) {
			struct sg_col *g = sp->c[c];

			if (!g || g->st != 1 || g->req != req)
				continue;
			if (SG_BIT(g->got_part, pt[5]))
				return;            /* a duplicate of an answered part */
			SG_SET(g->got_part, pt[5]);
			for (k = 0; k < cnt; k++) {
				unsigned int slot = (unsigned int)pt[8 + 2 * k] << 8 |
					pt[9 + 2 * k];

				if (slot < PC_SLOTS)
					SG_SET(g->diff, slot);
			}
			C.px.sweep_gate_slots_diff += cnt;
			g->got++;
			return;
		}
	}
}

static void eager_repl_tick(void)
{
	spread_note_set_change();

	long long now = now_ms();
	int i, c;

	for (c = 0; c < pc_store_count(); c++) {
		if (!pc_store_live(c))
			continue;
		if (!pc_store_eager_enabled(pc_store_ht(c)))
			continue;
		for (i = 0; i < C.pt.n_peers; i++) {
			struct peer *pr = &C.pt.peers[i];
			unsigned int start, since;
			int clean = 0, bf, gate;
			struct sg_col *gc;

			if (!clpeers_live(pr, now))
				continue;
			/* no sweep for a peer pulling its own bootstrap: the pull
			 * carries its copies, the push its new writes */
			if (pr->nstate == PC_NST_RECOVERING || pr->nstate == PC_NST_CONNECTING)
				continue;          /* nor one still proving its links */
			/* a backfill walk may START only while the target reports cold: only the
			 * walker clears the flag, elsewhere it goes stale and is dropped here */
			if (pr->backfill[c] && !pr->repl_bfcycle[c] &&
			    !pr->setrepair[c] &&
			    !(pr->start_kind == PC_START_COLD ||
			      (pr->start_kind == PC_START_UNKNOWN && !pr->entries))) {
				pr->backfill[c] = 0;
				LM_NOTICE("cluster: node %d is established - a backfill "
					"armed for it here was never this node's to send, "
					"dropped as stale (%s)\n", pr->node,
					pc_store_name(c));
			}
			/* no walk for a target pulling its bootstrap: the armed flag covers a pull
			 * that fails and leaves it ready but cold */
			/* one sender is an EAGER rule; under spread every holder sends -
			 * duplication is bounded by K and the version check drops it */
			bf = pr->backfill[c] && pr->nstate != PC_NST_RECOVERING &&
				(pc_cluster_replicas() ||
				 lowest_live_sender(pr->node, c));
			/* a set repair is a backfill walk without the one-sender rule */
			if (pr->setrepair[c] && pr->nstate != PC_NST_RECOVERING)
				bf = 1;
			/* asked for whole: every peer walks it, the version compare keeps one */
			if (pr->askfill[c] && pr->nstate != PC_NST_RECOVERING)
				bf = 1;
			/* a backfill is a WHOLE cycle as sender: gaining the role mid-cycle
			 * starts over, losing it leaves the flag to the new sender */
			if (bf && pr->repl_cursor[c] && !pr->repl_bfcycle[c])
				pr->repl_cursor[c] = 0;
			if (!bf)
				pr->repl_bfcycle[c] = 0;
			/* an ordinary cycle asks first (SKIP matched, WAIT, WALK the diffs, OFF
			 * the full sweep); a backfill or set repair is never gated */
			gate = SG_OFF;
			gc = NULL;
			if (!bf && pc_sweep_digests) {
				if (pr->repl_cursor[c] == 0)
					gate = sg_gate(pr, i, c, now);
				else if ((gc = sg_col_of(pr, i, c)) && gc->st == 2)
					gate = SG_WALK;
			}
			if (gate == SG_SKIP || gate == SG_WAIT)
				continue;
			if (gate == SG_WALK && !gc)
				gc = sg_col_of(pr, i, c);
			/* one slice of a resumable cycle: the start tick is taken at cursor 0
			 * only, or records written mid-cycle would fall below the next mark */
			if (pr->repl_cursor[c] == 0) {
				pr->repl_cycle[c] = get_ticks();
				pr->repl_dirty[c] = 0;
				pr->repl_bfcycle[c] = (unsigned char)bf;
				pr->repl_cycle_ms[c] = now_ms();
				pr->repl_scanned[c] = 0;
				pr->repl_sent[c] = 0;
				/* a gated cycle started when its digests were taken */
				if (gate == SG_WALK && gc)
					pr->repl_cycle[c] = gc->start;
			}
			start = pr->repl_cycle[c];
			/* the mark covers only AUTHORED records (a copy's tick is its
			 * receipt), so a backfill walks from zero */
			since = bf ? 0 : pr->repl_mark[c];
			/* in a slot that differed, EVERY authored record goes: that heals
			 * a write whose push and repeat were both lost */
			if (gate == SG_WALK && gc)
				since = 0;
			{
				unsigned long long out0 = C.px.repl_out;
				int node0 = pr->node;
				pc_addr to0 = pr->addr;

				sg_walk_filter = gate == SG_WALK && gc ? gc->diff : NULL;
				run_migration(pr, 4 << 20, 2, since,
					pc_store_name(c), &clean,
					&pr->repl_cursor[c], bf);
				sg_walk_filter = NULL;
				/* its pump handed the slot to another node: that one's
				 * zeroed cursor and marks are no cycle of this run */
				if (mig_slot_moved(pr, node0, &to0))
					continue;
				pr->repl_scanned[c] += C.last_run_scanned;
				pr->repl_sent[c] += C.px.repl_out - out0;
			}
			if (gate == SG_WALK && gc && pr->repl_cursor[c] == 0)
				gc->st = 0;            /* the gated walk is done */
			if (!clean)
				pr->repl_dirty[c] = 1;
			/* the sweep clock counts a cycle at cursor 0, clean or not:
			 * it times the walk, which a lost slice does not change */
			if (pr->repl_cursor[c] == 0) {
				C.st.repl_sweep_ms = (unsigned long long)
					(now_ms() - pr->repl_cycle_ms[c]);
				C.st.repl_sweep_scanned = pr->repl_scanned[c];
				C.st.repl_sweep_sent = pr->repl_sent[c];
				C.st.repl_sweep_cycles++;
			}
			if (pr->repl_cursor[c] == 0 && !pr->repl_dirty[c]) {
				pr->repl_mark[c] = start ? start - 1 : 0;
				/* only a clean cycle that WAS a backfill clears the flags:
				 * an ordinary sweep (target recovering) carried no copies */
				if (pr->setrepair[c] && pr->repl_bfcycle[c]) {
					pr->setrepair[c] = 0;
					LM_NOTICE("cluster: re-placed the keys "
						"node %d now holds (%s)\n",
						pr->node, pc_store_name(c));
				}
				if (pr->repl_bfcycle[c] && pr->askfill[c]) {
					pr->askfill[c] = 0;
					LM_NOTICE("cluster: sent collection '%s' whole "
						"to node %d, as it asked\n",
						pc_store_name(c), pr->node);
				}
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

/* shard mode: ship each key whose HRW owner is now a peer to that owner, budgeted per peer;
 * the shard grace extends while records flow, so readers keep the broadcast fallback */
static void shard_reshard_tick(void)
{
	long long now = now_ms();
	int i, have = 0;

	for (i = 0; i < pc_store_count(); i++) {
		if (!pc_store_live(i))
			continue;
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

/* ---- reclaiming surplus copies ---------------------------------------------
 * a copy outside the holder set gets no writes yet serves local hits: stale until its TTL */
#define SPREAD_RECLAIM_PER_TICK  64    /* keys examined-and-dropped per tick */

/* settled is clspread's decision (reclaim only on the shared map) */
static int spread_settled(int c)
{
	return clspread_settled(pc_cluster_replicas(), pc_shard_grace(),
		cl_map_usable() ? cl_map() : NULL, C.node_id, &C.pt, c,
		now_ms());
}

struct reclaim_ctx {
	int k;
	int n;
	unsigned scanned;
	long long deadline_ms;
	struct { char key[CLSPREAD_RECLAIM_KEYMAX]; int klen; } hit[SPREAD_RECLAIM_PER_TICK];
};

/* collect here, delete after the walk: removing during an iteration skips records */
static int reclaim_cb(const str *key, const str *val,
		const struct pcache_rec_meta *mt, void *arg)
{
	struct reclaim_ctx *r = arg;

	(void)val; (void)mt;
	if ((r->scanned++ & 511) == 0) {
		beat_pump();
		if (now_ms() >= r->deadline_ms)
			return -1;             /* liveness: resume next tick */
	}
	if (r->n >= SPREAD_RECLAIM_PER_TICK)
		return -1;
	if (!clspread_reclaim_candidate(key->s, key->len, r->k))
		return 0;                      /* ours, degraded, or unbuffered */
	memcpy(r->hit[r->n].key, key->s, (size_t)key->len);
	r->hit[r->n].klen = key->len;
	r->n++;
	return 0;
}

/* every beat: the settle hold counts beats (30 s, not ten sweep periods' 100 s), a pass runs each
 * sweep period, and every beat while the last one still found surplus copies */
static void spread_reclaim_tick(void)
{
	extern void pc_wal_del(const char *, const char *, int);
	static unsigned int cursor[PC_MAX_COLLECTIONS];
	static unsigned int settled_for[PC_MAX_COLLECTIONS];
	static unsigned char busy[PC_MAX_COLLECTIONS];
	static unsigned int beat;
	int c, i, k = pc_cluster_replicas();

	if (!k || !C.enabled)
		return;
	beat++;
	for (c = 0; c < pc_store_count(); c++) {
		pcache_htable_t *ht;
		struct reclaim_ctx r;
		const char *nm;

		if (!pc_store_live(c) || !pc_store_eager_enabled(pc_store_ht(c)))
			continue;
		/* settled, and for long enough - or the sweep restarts */
		if (!clspread_hold(&settled_for[c], &cursor[c], spread_settled(c))) {
			busy[c] = 0;
			continue;
		}
		if (!busy[c] && beat % REPL_SWEEP_BEATS)
			continue;              /* idle: one pass a sweep period */
		ht = pc_store_ht(c);
		nm = pc_store_name(c);
		r.k = k;
		r.n = 0;
		r.scanned = 0;
		r.deadline_ms = now_ms() + 200;
		pcache_ht_iter_meta_from(ht, &cursor[c], reclaim_cb, &r);
		busy[c] = r.n > 0;
		/* ask before dropping: a live K-set does not prove a holder HOLDS the record;
		 * only every holder's yes (CLPEND_ANS_POSSESS) deletes; anything else keeps it */
		for (i = 0; i < r.n; i++) {
			unsigned kslot;
			int holder;

			kslot = pc_key_slot(r.hit[i].key,
				(size_t)r.hit[i].klen);
			holder = spread_next_holder(kslot, k, 0);
			if (holder <= 0) {
				C.px.spread_possess_kept++;
				continue;      /* nobody to ask: keep it */
			}
			if (possess_probe(nm, strlen(nm), r.hit[i].key,
			        (size_t)r.hit[i].klen, holder))
				C.px.spread_possess_sent++;
			else
				C.px.spread_possess_kept++;
		}
	}
}

/* a pass over the records up to @bt, the hold on; a floor's never resyncs */
static void rebuild_pass(unsigned int bt, int floor, long long death,
		const char *why)
{
	C.rec_tick = bt;
	C.rec_rebuild = !floor;
	C.rec_floored = floor;
	C.recon.witness = 0;
	C.recon.death_mono = death;
	C.rec_col = 0;
	C.rec_cursor = 0;
	C.rec_start_ms = 0;
	clrecon_rounds_reset(&C.recon);
	rdg_drop();                        /* digests asked afresh */
	rdg_phase = 0;
	C.rec_hold = 1;
	C.reconcile_on = 1;
	fp_fall_back(why);
}

/* a rebuild folded into a pass under way: its deletes are held nowhere, and the fast path replays
 * only retained ones, so it may not end the pass - the pass walks */
static void rebuild_into_pass(const char *why)
{
	if (C.reconcile_on && C.fp.state != FPS_WALK)
		fp_fall_back(why);
}

/* nobody will resend these, so the mark goes past them, and every record
 * stored so far is re-checked: a key whose delete was in the hole is among them */
void gap_floor_over(const struct pc_ack_hole *h, long long death)
{
	unsigned int bt = get_ticks();
	uint64_t applied = 0, hi = 0, floor = h->to;

	if (pc_ack_known(h->oid, h->incarn, &applied, &hi) && hi > floor)
		floor = hi;
	if (pc_ack_floor(h->oid, h->incarn, floor) != CLACK_NEW)
		return;
	C.st.acks_floors++;
	C.st.gap_dead++;
	/* store keeps a pulled copy's version until it expires (doc/concepts.md): no pass to run */
	if (!C.cfg_eager) {
		LM_NOTICE("cluster: origin %016llx's run %u is over and nobody "
			"resends its deletes %llu..%llu - this node's mark floored "
			"there\n", (unsigned long long)h->oid, h->incarn,
			(unsigned long long)h->from, (unsigned long long)floor);
		return;
	}
	LM_WARN("cluster: origin %016llx's run %u is over and nobody resends its "
		"deletes %llu..%llu - this node's mark floored there; reconciling "
		"the records stored so far with the fleet\n",
		(unsigned long long)h->oid, h->incarn,
		(unsigned long long)h->from, (unsigned long long)floor);
	if ((C.reconcile_on && C.rec_tick >= bt) || (C.rb_cover && C.rb_cover >= bt)) {
		rebuild_into_pass("an origin's finished run left deletes past this "
			"node's marks that nobody resends");
		return;
	}
	C.rb_cover = bt;
	C.st.rebuilds++;
	if (C.reconcile_on) {
		C.rec_tick = bt;
		C.rec_col = 0;
		C.rec_cursor = 0;
		clrecon_rounds_reset(&C.recon);
		rdg_drop();                    /* the walk starts over: digests asked afresh */
		rdg_phase = 0;
		rebuild_into_pass("an origin's finished run left deletes past this "
			"node's marks that nobody resends");
		return;
	}
	rebuild_pass(bt, 1, death, "the deletes of an origin's finished run are "
		"out of reach");
}

static void handle_rebuild(const unsigned char *pt, size_t n,
		const pc_addr *from)
{
	struct peer *p = clpeers_by_addr(&C.pt, from);
	struct pc_rcur c;
	unsigned int kind, age_ms, nowt, bt;
	int floor;
	char why[128];

	if (!p || !__atomic_load_n(&p->node, __ATOMIC_ACQUIRE) ||
	    n < M_REBUILD_LEN)
		return;
	pc_rcur_init(&c, pt, n);
	(void)pc_ru8(&c);
	kind = pc_ru8(&c);
	age_ms = pc_ru32(&c);
	if (kind == M_REBUILD_TAKEN) {
		if (p->rb_state == 1)
			LM_NOTICE("cluster: node %d took the rebuild asked of it\n",
				p->node);
		p->rb_state = 0;
		return;
	}
	if (kind != M_REBUILD_ASK || !C.cfg_eager)
		return;
	floor = age_ms == M_REBUILD_FLOOR;
	/* in question: every record stored before the absence, held and walked until the fleet
	 * rules; whole ticks rounded up, so the absence's first second counts */
	nowt = get_ticks();
	/* a floor's is every record stored so far: its age was no absence, and as one it walked none */
	bt = floor ? nowt : nowt > age_ms / 1000 ? nowt - age_ms / 1000 : 0;
	if (floor)
		snprintf(why, sizeof why, "node %d forgot deletes past this "
			"node's marks - no replay brings them back", p->node);
	else
		snprintf(why, sizeof why, "node %d no longer holds every delete "
			"made while this node was away (~%u s)", p->node,
			age_ms / 1000);
	if ((C.reconcile_on && C.rec_tick >= bt) || (C.rb_cover && C.rb_cover >= bt)) {
		C.st.rebuild_covered++;
		rebuild_into_pass(why);
		rebuild_send(from, M_REBUILD_TAKEN, 0);
		return;
	}
	C.rb_cover = bt;
	C.st.rebuilds++;
	if (C.reconcile_on) {
		/* a pass under way with an earlier boundary: the boundary moves up and the walk
		 * starts over, so every record stored before it is walked */
		C.rec_tick = bt;
		C.rec_col = 0;
		C.rec_cursor = 0;
		clrecon_rounds_reset(&C.recon);
		rdg_drop();                    /* the walk starts over: digests asked afresh */
		rdg_phase = 0;
		rebuild_into_pass(why);
		rebuild_send(from, M_REBUILD_TAKEN, 0);
		if (floor)
			LM_WARN("cluster: node %d no longer holds deletes this "
				"node's marks were below - the reconcile under way "
				"now covers them\n", p->node);
		else
			LM_WARN("cluster: node %d says this node was away ~%u s - "
				"the reconcile under way now covers every record "
				"stored before then\n", p->node, age_ms / 1000);
		return;
	}
	/* a split's rebuild - the fleet was up through it.  A floor is no
	 * absence: the origin made those deletes since its boot, so it witnesses */
	rebuild_pass(bt, floor, !floor ? now_ms() - (long long)age_ms :
		p->boot_ms ? p->boot_ms + WITNESS_GRACE_MS + 1 : LLONG_MIN,
		"asked to rebuild - the fleet no longer holds every "
		"delete made while this node was away");
	rebuild_send(from, M_REBUILD_TAKEN, 0);
	if (floor)
		LM_WARN("cluster: node %d no longer holds deletes this node's "
			"marks were below - reconciling the records in question "
			"with the fleet\n", p->node);
	else
		LM_WARN("cluster: node %d says this node was away ~%u s, longer "
			"than what it kept of the deletes made meanwhile - "
			"reconciling every record stored before then with the "
			"fleet\n", p->node, age_ms / 1000);
}

/* ---- the reconcile's fast path, the witness's half ----------------------
 * replay the deletes since the asker's snapshot only if this node retained that window whole */
#define TOMBFP_MARGIN_S 10
#define FP_MARKS_WAIT_MS 3000          /* under the asker's FP_QUIET_MS */

static void fp_wire_done(const struct peer *p, uint32_t req, int status,
		unsigned int n)
{
	unsigned char m[10];

	m[0] = M_TOMB_DONE;
	fp_put32(m + 1, req);
	m[5] = (unsigned char)status;
	fp_put32(m + 6, n);
	(void)seal_send(&p->addr, m, sizeof m);
}

/* a delete forgotten here after @from_s must be reached by the asker's current marks, or its
 * state predates what it confirmed (a backup restore); -1 = a mark not in yet */
struct fpm { uint64_t member; unsigned int from_s; int st; };

static int fp_marks_cb(uint64_t oid, uint32_t incarn, uint64_t through,
		unsigned int at_s, void *ctx)
{
	struct fpm *f = ctx;
	uint64_t m;

	if (at_s <= f->from_s)
		return 0;                      /* before its snapshot: it holds it */
	if (!pc_ack_fleet_mark(f->member, oid, incarn, &m)) {
		f->st = -1;
		return 0;                      /* keep looking for a BEHIND */
	}
	if (m < through) {
		f->st = TOMBFP_BEHIND;
		return 1;
	}
	return 0;
}

static int fp_vouch_marks(const struct peer *p, unsigned int from_s)
{
	struct fpm f = { 0, from_s, TOMBFP_OK };
	uint32_t inc;

	if (!p->has_ident)
		return TOMBFP_OK;              /* an older build forgets nothing */
	f.member = fnv1a64(p->ident, sizeof p->ident);
	/* marks its earlier run reported say nothing of what a restore brought back: not in yet */
	if (p->fp_incarn && (!pc_ack_fleet_incarn(f.member, &inc) || inc != p->fp_incarn))
		return -1;
	clretain_forgot_each(fp_marks_cb, &f);
	return f.st;
}

/* TOMBFP_OK when every delete planted after @from_s is held here */
static int fp_vouch(unsigned int from_s, unsigned int now_s)
{
	unsigned long up = (unsigned long)pc_metrics_uptime();
	unsigned int boot_s = now_s > up ? now_s - (unsigned int)up : 0;
	unsigned int rs = clretain_retain_s();

	if (!clretain_enabled())
		return TOMBFP_OFF;
	if (from_s < boot_s + (unsigned int)(WITNESS_GRACE_MS / 1000) ||
	    from_s < (now_s > rs ? now_s - rs : 0))
		return TOMBFP_OLDER;
	return from_s < clretain_complete_since(now_s) ? TOMBFP_LOST : TOMBFP_OK;
}

static void fp_rpl_cb(const char *col, size_t cn, const char *key,
		size_t kn, uint64_t ver, int expiry, uint64_t oid, uint32_t incarn,
		uint64_t seq, void *ctx)
{
	unsigned char msg[9 + CLFWD_TOMB_MAX];
	struct peer *p = ctx;
	struct clfwd_key fk = {
		.col = col, .collen = (unsigned int)cn,
		.key = key, .klen = (unsigned int)kn };
	struct clfwd_tomb_num num = { oid, incarn, seq };
	size_t n = clfwd_tomb_build_n(msg + 9, M_TOMBSTONE, &fk, ver,
		(expiry ? CLFWD_TOMB_F_EXPIRY : 0) |
		(expiry == 2 ? CLFWD_TOMB_F_KEEP : 0), &num);

	if (n) {
		msg[0] = M_TOMB_RPL;
		fp_put32(msg + 1, p->fp_req);
		fp_put32(msg + 5, p->fp_seq);
		(void)seal_send(&p->addr, msg, 9 + n);
	}
	p->fp_seq++;
}

static const char *fp_status_why(int st)
{
	return st == TOMBFP_OFF ? "no retained deletes here" :
		st == TOMBFP_LOST ? "the cap dropped deletes inside it" :
		st == TOMBFP_BEHIND ? "deletes inside it were forgotten once every "
			"member held them, and its marks say it does not" :
		"it is older than the retained deletes reach";
}

/* ---- a member asks this node to install a build; the asking side ---------- */
_Static_assert(UPGSW_ACCEPTED == 0 && UPGSW_NOT_STAGED == 1 &&
	UPGSW_UNVERIFIED == 2 && UPGSW_NO_HELPER == 3 && UPGSW_BUSY == 4 &&
	UPGSW_OTHER == 5, "upgrade.c's copies of UPGSW_*");

static void handle_upg_switch(const unsigned char *pt, size_t n,
		const pc_addr *from)
{
	struct peer *p = clpeers_by_addr(&C.pt, from);

	if (n < 1 + 4 + 32 || !p || !__atomic_load_n(&p->node, __ATOMIC_ACQUIRE))
		return;                        /* a member we know, or nobody */
	LM_NOTICE("upgrade: node %d asks this node to install a staged build\n",
		p->node);
	pc_upg_switch_ask(from->in4.sin_addr.s_addr, from->in4.sin_port,
		fp_get32(pt + 1), pt + 5);
}

static void handle_upg_ack(const unsigned char *pt, size_t n,
		const pc_addr *from)
{
	if (n < 1 + 4 + 1)
		return;
	pc_upg_ask_acked(from->in4.sin_addr.s_addr, fp_get32(pt + 1), pt[5]);
}

/* every beat: an answer to send, an ask to send */
static void upg_msg_tick(void)
{
	pc_addr to;
	unsigned char m[1 + 4 + 32];
	unsigned int st;
	uint32_t req, a;
	uint16_t port;

	if (pc_upg_switch_answer(&a, &port, &req, &st)) {
		struct peer *p;

		memset(&to, 0, sizeof to);
		pc_addr_from_v4(&to, a, ntohs(port));
		p = clpeers_by_addr(&C.pt, &to);   /* as fp_wire_done answers */
		m[0] = M_UPG_ACK;
		fp_put32(m + 1, req);
		m[5] = (unsigned char)st;
		(void)seal_send(p ? &p->addr : &to, m, 6);
	}
	if (pc_upg_ask_take(&a, &req, m + 5)) {
		struct peer *p;

		memset(&to, 0, sizeof to);
		pc_addr_from_v4(&to, a, pc_addr_port(&C.mcast_dst));   /* the cluster port */
		p = clpeers_by_addr(&C.pt, &to);
		m[0] = M_UPG_SWITCH;
		fp_put32(m + 1, req);
		(void)seal_send(p ? &p->addr : &to, m, sizeof m);
	}
}

static void handle_tomb_req(const unsigned char *pt, size_t n,
		const pc_addr *from)
{
	struct peer *p = clpeers_by_addr(&C.pt, from);
	unsigned int now_s = (unsigned int)(now_ms() / 1000), age, from_s;
	uint32_t req;
	int st;

	if (n < 9 || !p || !__atomic_load_n(&p->node, __ATOMIC_ACQUIRE))
		return;                        /* silence: it asks again, then walks */
	req = fp_get32(pt + 1);
	if (p->fp_on && p->fp_req == req)
		return;                        /* replaying it already */
	age = fp_get32(pt + 5);
	p->fp_incarn = n >= 13 ? fp_get32(pt + 9) : 0;
	from_s = age < now_s && now_s - age > TOMBFP_MARGIN_S ?
		now_s - age - TOMBFP_MARGIN_S : 0;
	st = fp_vouch(from_s, now_s);
	if (st == TOMBFP_OK && (st = fp_vouch_marks(p, from_s)) < 0) {
		/* its marks are not in yet - its keepalives carry them
		 * a slice a beat; held, and decided by the replay's tick */
		p->fp_req = req;
		p->fp_from = from_s;
		p->fp_hold_ms = now_ms() + FP_MARKS_WAIT_MS;
		p->fp_on = 2;
		return;
	}
	if (st != TOMBFP_OK) {
		C.st.reconcile_fast_refused++;
		fp_wire_done(p, req, st, 0);
		LM_NOTICE("cluster: node %d asked for the deletes made since its "
			"snapshot began (%u s ago) - this node cannot vouch for "
			"that window: %s\n", p->node, age, fp_status_why(st));
		return;
	}
	p->fp_req = req;
	p->fp_from = from_s;
	p->fp_mark = from_s;
	p->fp_win = 60;
	p->fp_seq = 0;
	p->fp_on = 1;
	LM_NOTICE("cluster: node %d asked for the deletes made since its "
		"snapshot began (%u s ago) - replaying them\n", p->node, age);
}

static void fp_replay_tick(void)
{
	long long now = now_ms();
	unsigned int now_s = (unsigned int)(now / 1000);
	int i;

	for (i = 0; i < C.pt.n_peers; i++) {
		struct peer *p = &C.pt.peers[i];
		unsigned int upto, sent, win;
		int st;

		if (!p->fp_on)
			continue;
		if (!__atomic_load_n(&p->node, __ATOMIC_ACQUIRE) ||
		    !clpeers_live(p, now)) {
			p->fp_on = 0;              /* gone: it asks again */
			continue;
		}
		if (p->fp_on == 2) {               /* held for its marks */
			st = fp_vouch_marks(p, p->fp_from);
			if (st < 0 && now < p->fp_hold_ms)
				continue;
			if (st == TOMBFP_OK)
				st = fp_vouch(p->fp_from, now_s);
			if (st != TOMBFP_OK) {
				if (st < 0)
					st = TOMBFP_BEHIND;   /* its marks never came */
				C.st.reconcile_fast_refused++;
				fp_wire_done(p, p->fp_req, st, 0);
				p->fp_on = 0;
				LM_NOTICE("cluster: node %d's fast path - this node "
					"cannot vouch for its window: %s\n", p->node,
					fp_status_why(st));
				continue;
			}
			p->fp_mark = p->fp_from;
			p->fp_win = 60;
			p->fp_seq = 0;
			p->fp_on = 1;
			LM_NOTICE("cluster: node %d's marks reach what this node "
				"forgot - replaying the deletes made since its snapshot "
				"began\n", p->node);
		}
		win = p->fp_win ? p->fp_win : 60;
		upto = p->fp_mark + win < now_s ? p->fp_mark + win : now_s;
		sent = clretain_each(p->fp_mark, upto, fp_rpl_cb, p);
		p->fp_mark = upto;
		/* the catch-up's pace: a few thousand a tick */
		if (sent > 4000 && win > 1)
			p->fp_win = win / 2;
		else if (sent < 1000 && win < 3600)
			p->fp_win = win * 2;
		if (upto < now_s)
			continue;
		/* the window is out: was it whole to the end, and does
		 * the asker still reach what was forgotten meanwhile? */
		st = fp_vouch(p->fp_from, now_s);
		if (st == TOMBFP_OK && (st = fp_vouch_marks(p, p->fp_from)) < 0)
			st = TOMBFP_BEHIND;
		fp_wire_done(p, p->fp_req, st, st == TOMBFP_OK ? p->fp_seq : 0);
		p->fp_on = 0;
		if (st == TOMBFP_OK) {
			C.st.reconcile_fast_served++;
			LM_NOTICE("cluster: node %d's fast path - %u delete(s) "
				"made since its snapshot replayed to it\n",
				p->node, p->fp_seq);
		} else {
			C.st.reconcile_fast_refused++;
			LM_NOTICE("cluster: node %d's fast path - %u delete(s) "
				"replayed, but the window did not stay whole (%s): "
				"it will walk\n", p->node, p->fp_seq,
				fp_status_why(st));
		}
	}
}

/* the rebalancer's last verdict: written by the peer thread alone, read relaxed - a reader
 * may mix fields from adjacent ticks, which is still true of the fleet */
enum { RB_NONE = 0, RB_SMALL, RB_BAND, RB_NOPEER, RB_SHED };
static struct {
	int code, node, settled;
	unsigned int self, mean;
	long long kb, settled_since, last_move;
	unsigned long long moved, sheds;
} rb;

static void rb_set(int code, unsigned int self, unsigned int mean, int node,
		long long kb)
{
	int settled = code != RB_SHED;

	__atomic_store_n(&rb.self, self, __ATOMIC_RELAXED);
	__atomic_store_n(&rb.mean, mean, __ATOMIC_RELAXED);
	__atomic_store_n(&rb.node, node, __ATOMIC_RELAXED);
	__atomic_store_n(&rb.kb, kb, __ATOMIC_RELAXED);
	if (settled && !__atomic_load_n(&rb.settled, __ATOMIC_RELAXED))
		__atomic_store_n(&rb.settled_since, (long long)time(NULL),
			__ATOMIC_RELAXED);
	if (!settled)
		__atomic_store_n(&rb.settled_since, 0LL, __ATOMIC_RELAXED);
	__atomic_store_n(&rb.settled, settled, __ATOMIC_RELAXED);
	__atomic_store_n(&rb.code, code, __ATOMIC_RELEASE);
}

void pc_cluster_rebalance(struct pc_rebalance *out)
{
	int code = __atomic_load_n(&rb.code, __ATOMIC_ACQUIRE);
	unsigned int self = __atomic_load_n(&rb.self, __ATOMIC_RELAXED);
	unsigned int mean = __atomic_load_n(&rb.mean, __ATOMIC_RELAXED);

	memset(out, 0, sizeof *out);
	out->settled = code != RB_SHED;
	out->settled_since = __atomic_load_n(&rb.settled_since, __ATOMIC_RELAXED);
	out->last_move = __atomic_load_n(&rb.last_move, __ATOMIC_RELAXED);
	out->moved_records = __atomic_load_n(&rb.moved, __ATOMIC_RELAXED);
	out->ticks_shed = __atomic_load_n(&rb.sheds, __ATOMIC_RELAXED);
	switch (code) {
	case RB_SMALL:
		snprintf(out->reason, sizeof out->reason,
			"no live peer to level against");
		break;
	case RB_BAND:
		snprintf(out->reason, sizeof out->reason,
			"within band: this node %u.%u%%, fleet mean %u.%u%% "
			"(sheds only above mean + 5%%)", self / 10, self % 10,
			mean / 10, mean % 10);
		break;
	case RB_NOPEER:
		snprintf(out->reason, sizeof out->reason,
			"no peer less utilized than this node (%u.%u%%)",
			self / 10, self % 10);
		break;
	case RB_SHED:
		snprintf(out->reason, sizeof out->reason,
			"shedding up to %lld KB to node %d (this node %u.%u%%, "
			"fleet mean %u.%u%%)",
			__atomic_load_n(&rb.kb, __ATOMIC_RELAXED),
			__atomic_load_n(&rb.node, __ATOMIC_RELAXED),
			self / 10, self % 10, mean / 10, mean % 10);
		break;
	default:
		snprintf(out->reason, sizeof out->reason,
			"no rebalance tick yet");
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
	tomb_catchup_tick();               /* deletes a peer missed */
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
		/* ties go to more ABSOLUTE free space, or data double-hops
		 * through a tiny empty node */
		if (u < best_util || (u == best_util && best &&
		        C.pt.peers[i].free_mb > best->free_mb)) {
			best_util = u;
			best = &C.pt.peers[i];
		}
	}
	if (n < 2 || !best) {
		rb_set(RB_SMALL, self_per_mille, self_per_mille, 0, 0);
		return;
	}
	mean = sum / (unsigned int)n;
	/* level by UTILIZATION, not free bytes: shed only above mean + band, to the least
	 * utilized peer, and only the EXCESS, so it cannot overshoot and oscillate */
	if (self_per_mille <= mean + 50) {     /* band: 50 per-mille = 5% */
		rb_set(RB_BAND, self_per_mille, mean, 0, 0);
		return;
	}
	if (best_util >= self_per_mille) {
		rb_set(RB_NOPEER, self_per_mille, mean, 0, 0);
		return;
	}
	{
		long long budget = 4 << 20;    /* 4MB per 10s tick */
		unsigned long long before = C.px.migrated_out;

		excess = (self_per_mille - mean) * self_total_mb() / 1000;
		if ((long long)excess << 20 < budget)
			budget = (long long)excess << 20;
		if (budget <= 0) {
			rb_set(RB_BAND, self_per_mille, mean, 0, 0);
			return;
		}
		rb_set(RB_SHED, self_per_mille, mean, best->node, budget >> 10);
		run_migration(best, budget, 0, 0, NULL, NULL, NULL, 0);
		/* what this tick moved, by the records the migration counted out */
		if (C.px.migrated_out > before) {
			__atomic_fetch_add(&rb.moved, C.px.migrated_out - before,
				__ATOMIC_RELAXED);
			__atomic_store_n(&rb.last_move, (long long)time(NULL),
				__ATOMIC_RELAXED);
		}
		__atomic_fetch_add(&rb.sheds, 1, __ATOMIC_RELAXED);
	}
}

/* ---- the bulk TCP plane (records above the datagram ceiling) ------------
 * the bulk thread owns every TCP socket, so membership never stalls on TCP timeouts */

/* ---- the bootstrap pull over the bulk plane --------------------------------
 * the walk callback writes the stream, so a flush in the overflow leg holds its lock (5 s cap) */
struct boot_ctx {
	int fd;
	struct pc_cipherstate *cs;
	unsigned char *buf;
	size_t len, cap;
	const char *col;
	unsigned int cn, now, streamed;
	unsigned int skipped;          /* outside the joiner's share */
	int node;                      /* the joiner */
	int k;                         /* copy factor; 0 = not spread */
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

static int boot_cb(const str *key, const str *val,
		const struct pcache_rec_meta *mt, void *arg)
{
	struct boot_ctx *bc = arg;
	unsigned int ttl_left = 0;
	size_t n;
	unsigned char *rp;

	if (bc->err)
		return -1;
	if (mt->expires) {
		if (mt->expires <= bc->now)
			return 0;                  /* expired: nothing to hand on */
		ttl_left = mt->expires - bc->now;
	}
	if (key->len > 4096)
		return 0;
	/* under spread the joiner gets only its share, placed as a write would; one the map
	 * does not list yet gets nothing here - set repair delivers it */
	if (bc->k && !pc_spread_in_set(pc_key_slot(key->s, (size_t)key->len),
	        bc->node, bc->k)) {
		bc->skipped++;
		return 0;
	}
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
			.ver = mt->ver,
			.flags = (unsigned char)(mt->rflags & PCACHE_F_CARRY)
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
	unsigned char req[CLBULK_BOOTREQ_LEN], end[MIG_RHDR], cnt4[4];
	int bkind;
	struct boot_ctx bc;
	int c, node;

	if (clbulk_recv_exact(cfd, cs_r, stash, slen, scap, req,
	        CLBULK_BOOTREQ_LEN) != 0)
		return;
	/* parse BEFORE testing the kind: -Wall cannot flag an uninitialised
	 * bkind, since its address is taken */
	if (!clbulk_bootreq_parse(req, CLBULK_BOOTREQ_LEN, &bkind, &node))
		return;
	if (bkind != BOOT_KIND_PULL)
		return;
	memset(&bc, 0, sizeof bc);
	bc.fd = cfd;
	bc.cs = cs_s;
	bc.cap = BOOT_BUF;
	bc.buf = malloc(bc.cap);
	if (!bc.buf)
		return;
	bc.now = get_ticks();
	bc.node = node;
	bc.k = pc_cluster_replicas();      /* scope the walk under spread */
	for (c = 0; c < pc_store_count() && !bc.err; c++) {
		if (!pc_store_live(c))
			continue;
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
			"which stored %u (%u outside its share skipped)\n",
			bc.streamed, node, g32(cnt4), bc.skipped);
	else
		LM_INFO("cluster: streamed a bootstrap of %u record(s) to node %d "
			"(no count back; %u outside its share skipped)\n",
			bc.streamed, node, bc.skipped);
out:
	free(bc.buf);
}

/* one candidate: 0 = clean end; -2 = nothing before the first record (an older build or a
 * refusal); -1 = the stream broke */
/* the bulk thread's stop flag, for the transfers that run longer than one poll */
static int bulk_never;
static volatile int *bulk_stop = &bulk_never;

static int bulk_pull_from(const pc_addr *peer,
		unsigned int *streamed, unsigned int *stored, unsigned int *older)
{
	unsigned char hdr[3 + PC_NOISE_MAXMSG], m2[PC_NOISE_MAXMSG];
	unsigned char stash[PC_NOISE_MAXMSG], cnt4[4], req[4], rhdr[MIG_RHDR];
	unsigned char *rec = NULL, ver1 = PC_CLIENT_VER;
	pc_addr to = *peer;
	struct pc_handshake hs;
	struct pc_cipherstate cs_s, cs_r;
	struct timeval tv = { 5, 0 };
	uint8_t prologue[1] = { PC_PRIN_CLUSTER };
	size_t mlen = 0, plen = 0, cl, slen = 0;
	int fd, rc = -2;

	*streamed = *stored = *older = 0;
	pc_addr_set_port(&to, pc_addr_port(&C.mcast_dst));    /* the cluster port, TCP */
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	bulk_bind_self(fd);
	if (connect(fd, &to.sa, pc_addr_len(&to)) != 0)
		goto out;
	pc_hs_init_initiator(&hs, prologue, 1);
	if (pc_hs_write_msg1(&hs, C.psk, &ver1, 1, hdr + CLBULK_HS1_AT,
			&mlen) != 0)
		goto out;
	if (clbulk_io(fd, 1, hdr,
	        clbulk_hs1_frame(hdr, sizeof hdr, PC_PRIN_CLUSTER, mlen)) != 0 ||
	    clbulk_io(fd, 0, hdr, CLBULK_LEN_PREFIX) != 0)
		goto out;
	cl = clbulk_len_get(hdr);
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
		unsigned char rtype;
		pcache_htable_t *ht;
		char colz[256];
		str k, v;
		int brc;

		if (*bulk_stop)
			goto out;
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
			rtype = (unsigned char)(hr.flags & PCACHE_F_CARRY);
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
		exp = pc_upstream_copy_exp(colz, cn, ttl_left);
		pc_lamport_observe(ver);
		{
			unsigned long long tv;

			/* superseded by a newer tombstone - resolved, not
			 * stored (the batch must not be retried for it) */
			if (pc_neg_ver(colz, cn, k.s, kn, &tv) && tv && ver <= tv) {
				(*older)++;
				__atomic_fetch_add(&C.px.recv_tombstoned, 1,
					__ATOMIC_RELAXED);
				continue;
			}
		}
		if (dropfloor_refuses(colz, cn, k.s, kn, ver)) {
			(*older)++;
			continue;
		}
		brc = pcache_ht_store_peer(ht, &k, &v, exp,      /* a replica: ties broken */
			(unsigned char)(PCACHE_F_PASSIVE | rtype), ver);
		if (brc == PCACHE_E_OLDER) {
			(*older)++;
			__atomic_fetch_add(&C.px.recv_older, 1, __ATOMIC_RELAXED);
		} else if (brc == -2) {
			nomem_drop(ht, colz, cn, &k, ver);
		} else if (brc == 0) {
			unsigned long long tv;

			if (pc_neg_ver(colz, cn, k.s, kn, &tv) &&
			        clorder_take_back(ht, &k, ver, tv)) {
				__atomic_fetch_add(&C.px.recv_tombstoned, 1,
					__ATOMIC_RELAXED);
				continue;
			}
			pc_loc_clear(colz, cn, k.s, kn);
			pc_upstream_note_copy(colz, cn, k.s, kn, exp, ver);
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
	pc_addr cand[BOOT_MAX_CAND];
	int node[BOOT_MAX_CAND], n, k;

	n = clboot_take_cands(&C.bt, clbulk_lock(&C.bk), cand, node);
	if (n && self_boot_delay_ms > 0) {
		usleep((useconds_t)self_boot_delay_ms * 1000);
		self_boot_delay_ms = 0;
	}
	for (k = 0; k < n && !*bulk_stop; k++) {
		unsigned int streamed, stored, older;
		long long t0 = now_ms();
		int rc = bulk_pull_from(&cand[k], &streamed, &stored, &older);

		if (rc == 0) {
			int rj = __atomic_load_n(&C.rejoin_on, __ATOMIC_ACQUIRE) > 0;

			__atomic_fetch_add(&C.px.boot_in, stored, __ATOMIC_RELAXED);
			clboot_pull_ok(&C.bt, clbulk_lock(&C.bk));
			LM_NOTICE("cluster: bootstrapped from node %d - %u record(s) "
				"streamed, %u stored, %u already held at the same or a "
				"newer version, in %lld ms; %s\n", node[k], streamed,
				stored, older, now_ms() - t0, rj ? "ready once it is "
				"heard again as the survivor picked" : "ready");
			if (rj) {
				__atomic_store_n(&C.rejoin_end_ms, now_ms(),
					__ATOMIC_RELEASE);
				__atomic_store_n(&C.rejoin_src, node[k],
					__ATOMIC_RELEASE);
			} else if (pc_node_state() == PC_NST_RECOVERING)
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
	/* every candidate failed: the tick re-picks from the peers known by then, or gives up
	 * after three rounds */
	clboot_pull_failed(&C.bt, clbulk_lock(&C.bk));
}

/* runs on the bulk thread: setting repl_dirty races the cluster thread's writes, harmlessly -
 * the flag only ever costs an extra walk */
static void repl_bulk_peer_failed(const pc_addr *to)
{
	int i, c;

	for (i = 0; i < C.pt.n_peers; i++) {
		struct peer *pr = &C.pt.peers[i];

		if (!__atomic_load_n(&pr->node, __ATOMIC_ACQUIRE) ||
		    !pc_addr_eq_host(&pr->addr, to))
			continue;
		for (c = 0; c < PC_MAX_COLLECTIONS; c++)
			__atomic_store_n(&pr->repl_dirty[c], 1,
				__ATOMIC_RELAXED);
		return;
	}
}

static void bulk_tx(volatile int *stop)
{
	unsigned char *batch = NULL, hdr[3 + PC_NOISE_MAXMSG];
	unsigned char m2[PC_NOISE_MAXMSG], cnt4[4], ver = PC_CLIENT_VER;
	unsigned char stash[PC_NOISE_MAXMSG];
	pc_addr to;
	struct pc_handshake hs;
	struct pc_cipherstate cs_s, cs_r;
	struct timeval tv = { 5, 0 };
	uint8_t prologue[1] = { PC_PRIN_CLUSTER };
	size_t blen, mlen = 0, plen = 0, cl, slen = 0;
	unsigned int recs, stored = 0;
	int fd = -1, ok = 0, kind = CLBULK_KIND_MIGRATE;

	(void)stop;
	if (!clbulk_take(&C.bk, &batch, &blen, &recs, &kind, &to))
		return;

	pc_addr_set_port(&to, pc_addr_port(&C.mcast_dst));
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		goto out;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	bulk_bind_self(fd);
	if (connect(fd, &to.sa, pc_addr_len(&to)) != 0)
		goto out;

	pc_hs_init_initiator(&hs, prologue, 1);
	{
		/* [client version][batch kind]: an older receiver reads byte 0 only,
		 * taking a replica batch for a placement move */
		unsigned char pay[2] = { ver, (unsigned char)kind };

		if (pc_hs_write_msg1(&hs, C.psk, pay, sizeof pay,
				hdr + CLBULK_HS1_AT, &mlen) != 0)
			goto out;
	}
	if (clbulk_io(fd, 1, hdr,
	        clbulk_hs1_frame(hdr, sizeof hdr, PC_PRIN_CLUSTER, mlen)) != 0 ||
	    clbulk_io(fd, 0, hdr, CLBULK_LEN_PREFIX) != 0)
		goto out;
	cl = clbulk_len_get(hdr);
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
	if (kind == CLBULK_KIND_STAGE) {
		/* the answer is the receiver's status, 0 = staged */
		pc_upg_relay_note(to.in4.sin_addr.s_addr, ok ? stored : PC_UPG_RELAY_LOST);
		free(batch);
		clbulk_done(&C.bk);
		return;
	}
	__atomic_fetch_add(&C.px.bulk_out, stored, __ATOMIC_RELAXED);
	if (!ok || stored < recs) {
		unsigned int miss = recs - (ok ? stored : 0);

		if (kind == CLBULK_KIND_REPLICA) {
			/* the copies are still here: only this peer's cycle is unproven,
			 * so its mark must not advance past them */
			__atomic_fetch_add(&C.px.repl_bulk_lost, 1,
				__ATOMIC_RELAXED);
			repl_bulk_peer_failed(&to);
			LM_WARN("eager sweep: %u of %u oversized copy(ies) "
				"unconfirmed - the next sweep offers them "
				"again\n", miss, recs);
		} else {
			__atomic_fetch_add(&C.px.migrate_lost, miss,
				__ATOMIC_RELAXED);
			LM_WARN("bulk migration: %u of %u record(s) "
				"unconfirmed\n", miss, recs);
		}
	}
	free(batch);
	clbulk_done(&C.bk);
}

/* ---- a staged build over bulk: [size8][sha256 32][signed1][sig64][vlen1][version][build],
 * answered [status u32]; verified against the receiver's OWN keys and never relayed further */
#define STAGE_HDR (8 + 32 + 1 + PC_UPG_SIG_LEN + 1)

static void bulk_rx_stage(int cfd, struct pc_cipherstate *cs_s,
		struct pc_cipherstate *cs_r, unsigned char *stash, size_t *slen,
		size_t scap)
{
	unsigned char h[STAGE_HDR], cnt4[4], *buf = NULL;
	char ver[48], shx[65], id[33], who[96];
	const char *e = "";
	unsigned long long size, got = 0;
	unsigned int code = PC_UPG_RELAY_OK, vl;
	long long g = 0;

	if (clbulk_recv_exact(cfd, cs_r, stash, slen, scap, h, STAGE_HDR) != 0)
		return;
	size = g64(h);
	vl = h[STAGE_HDR - 1];
	if (vl == 0 || vl >= sizeof ver || size == 0 ||
	        size > (unsigned long long)PC_UPG_ARTIFACT_MAX ||
	        clbulk_recv_exact(cfd, cs_r, stash, slen, scap,
	        (unsigned char *)ver, vl) != 0) {
		p32(cnt4, PC_UPG_RELAY_MALFORMED);
		clbulk_send(cfd, cs_s, cnt4, 4);
		return;
	}
	ver[vl] = 0;
	sodium_bin2hex(shx, sizeof shx, h + 8, 32);
	if (pc_upg_stage_begin((long long)size, shx, h[40] ? h + 41 : NULL, ver,
	        id, sizeof id, &e) != 0) {
		code = PC_UPG_RELAY_BEGIN;
		LM_WARN("upgrade: a member's build %s (%s) not staged here: %s\n",
			ver, shx, e);
	}
	buf = malloc(64 * 1024);
	if (!buf && code == PC_UPG_RELAY_OK) {
		pc_upg_stage_abort(id, &e);
		code = PC_UPG_RELAY_CHUNK;
	}
	/* the whole stream is read, refused or not, so the answer arrives
	 * where the sender looks for it */
	while (got < size) {
		size_t n = size - got < 64 * 1024 ? (size_t)(size - got) : 64 * 1024;
		unsigned char sink[512];
		size_t k;

		if (!buf) {
			for (k = 0; k < n; k += sizeof sink)
				if (clbulk_recv_exact(cfd, cs_r, stash, slen, scap, sink,
				        n - k < sizeof sink ? n - k : sizeof sink) != 0)
					return;
		} else if (clbulk_recv_exact(cfd, cs_r, stash, slen, scap, buf,
		        n) != 0) {
			if (code == PC_UPG_RELAY_OK)
				pc_upg_stage_abort(id, &e);
			free(buf);
			return;
		}
		if (code == PC_UPG_RELAY_OK &&
		        pc_upg_stage_chunk(id, (long long)got, buf, n, &g, &e) != 0) {
			code = PC_UPG_RELAY_CHUNK;
			LM_WARN("upgrade: a member's build %s: %s\n", ver, e);
			pc_upg_stage_abort(id, &e);
		}
		got += n;
	}
	free(buf);
	if (code == PC_UPG_RELAY_OK &&
	        pc_upg_stage_end(id, shx, sizeof shx, who, sizeof who, &e) != 0) {
		code = PC_UPG_RELAY_REFUSED;
		LM_WARN("upgrade: a member's build %s refused here: %s\n", ver, e);
	}
	p32(cnt4, code);
	clbulk_send(cfd, cs_s, cnt4, 4);
}

int pc_cluster_stage_relay(const void *data, size_t len,
		const unsigned char *sha, const unsigned char *sig,
		const char *version, int *older)
{
	struct pc_member *m;
	size_t vl = strlen(version), tot = STAGE_HDR + vl + len;
	int n, i, offered = 0;

	*older = 0;
	if (!vl || vl > 47)
		return 0;
	m = calloc(PC_CL_MAXPEER, sizeof *m);
	if (!m)
		return 0;
	n = pc_cluster_members(m, PC_CL_MAXPEER);
	for (i = 0; i < n; i++) {
		pc_addr to;
		unsigned char *b;
		uint32_t f = 0;

		if (m[i].is_self || !m[i].live || m[i].gone_s >= 0)
			continue;                  /* down or departed: nobody to ask */
		if (!pc_member_fact(&m[i], PC_FACT_FEATURES, &f) ||
		        !(f & CLMEMB_FEAT_STAGE)) {
			(*older)++;
			pc_upg_relay_note(m[i].addr.s_addr, PC_UPG_RELAY_OLDER);
			continue;
		}
		b = malloc(tot);
		if (!b)
			break;
		p64(b, (unsigned long long)len);
		memcpy(b + 8, sha, 32);
		b[40] = sig != NULL;
		if (sig)
			memcpy(b + 41, sig, PC_UPG_SIG_LEN);
		else
			memset(b + 41, 0, PC_UPG_SIG_LEN);
		b[STAGE_HDR - 1] = (unsigned char)vl;
		memcpy(b + STAGE_HDR, version, vl);
		memcpy(b + STAGE_HDR + vl, data, len);
		memset(&to, 0, sizeof to);
		to.in4.sin_family = AF_INET;
		to.in4.sin_addr = m[i].addr;
		/* "sending" BEFORE the offer: the bulk thread may answer first */
		pc_upg_relay_note(m[i].addr.s_addr, PC_UPG_RELAY_SENDING);
		if (!clbulk_offer(&C.bk, b, tot, CLBULK_STAGE_MAGIC,
		        CLBULK_KIND_STAGE, &to)) {
			free(b);
			pc_upg_relay_note(m[i].addr.s_addr, PC_UPG_RELAY_BUSY);
			continue;
		}
		offered++;
	}
	free(m);
	return offered;
}

/* from this node's own host, as the receiver's gate below expects -
 * unbound, a multi-homed host or a loopback-alias fleet connects from another */
static void bulk_bind_self(int fd)
{
	pc_addr me = C.self_addr;

	pc_addr_set_port(&me, 0);
	if (bind(fd, &me.sa, pc_addr_len(&me)) != 0) { /* unbound: the route's */ }
}

/* the bulk plane is served one connection at a time, so only a host
 * the peer table knows (a joiner's slot too, before its id) is read from */
static int bulk_host_known(const pc_addr *a)
{
	int i;

	for (i = 0; i < C.pt.n_peers; i++)
		if (pc_addr_eq_host(&C.pt.peers[i].addr, a))
			return 1;
	return 0;
}

#define BULK_M1_PAY 16                 /* message 1's payload: the version, the kind */

/* exactly @n bytes by the monotonic @deadline: a receive timeout restarts with every byte read */
static int bulk_read_by(int fd, unsigned char *buf, size_t n, long long deadline)
{
	size_t off = 0;

	while (off < n) {
		struct pollfd pf = { .fd = fd, .events = POLLIN };
		long long left = deadline - now_ms();
		ssize_t r;
		int pr;

		if (left <= 0)
			return -1;
		pr = poll(&pf, 1, (int)left);
		if (pr < 0 && errno == EINTR)
			continue;
		if (pr <= 0)
			return -1;
		r = read(fd, buf + off, n - off);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return -1;
		off += (size_t)r;
	}
	return 0;
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
	long long dl = now_ms() + 1000;
	size_t flen, mlen = 0, slen = 0;
	unsigned int nrec, i, resolved = 0;
	int replica = 0, stage = 0;

	/* the opening message within a second - a silent connection
	 * holds the bulk thread, and everything queued behind it, that long */
	setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	if (bulk_read_by(cfd, frame, 2, dl) != 0)
		goto out;
	flen = (size_t)frame[0] | ((size_t)frame[1] << 8);
	/* message 1 is the principal, e, a payload of a byte or two and the tag: one longer would
	 * be opened past pay[] below, written even when its tag fails */
	if (flen < 1 + 48 || flen > 1 + 32 + BULK_M1_PAY + PC_NOISE_TAGLEN ||
	        bulk_read_by(cfd, frame + 2, flen, dl) != 0)
		goto out;
	setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	if (frame[2] != PC_PRIN_CLUSTER)
		goto out;                      /* cluster principal ONLY */
	pc_hs_init_responder(&hs, frame + 2, 1);
	{
		unsigned char pay[BULK_M1_PAY];
		size_t pl2 = 0;

		if (pc_hs_read_msg1(&hs, C.psk, frame + 3, flen - 1, pay,
		        &pl2) != 0) {
			if (!have_psk_accept)              /* the other secret */
				goto out;
			pc_hs_init_responder(&hs, frame + 2, 1);
			if (pc_hs_read_msg1(&hs, psk_accept, frame + 3, flen - 1,
			        pay, &pl2) != 0)
				goto out;
		}
		if (pl2 < 1 || pay[0] != PC_CLIENT_VER)
			goto out;
		/* the batch kind is an ADDITIVE second byte - a peer
		 * on an older build sends one and means a placement move */
		if (pl2 >= 2 && pay[1] == CLBULK_KIND_REPLICA)
			replica = 1;
		if (pl2 >= 2 && pay[1] == CLBULK_KIND_STAGE)
			stage = 1;
	}
	if (pc_hs_write_msg2(&hs, NULL, 0, m2 + CLBULK_HS2_AT, &mlen,
	        &cs_s, &cs_r) != 0)
		goto out;
	{
		size_t f2 = clbulk_hs2_frame(m2, sizeof m2, mlen);

		if (!f2 || clbulk_io(cfd, 1, m2, f2) != 0)
			goto out;
	}
	/* pc_hs_write_msg2 hands the responder ITS oriented pair already */

	if (clbulk_recv_exact(cfd, &cs_r, stash, &slen, sizeof stash, cnt4, 4)
	        != 0)
		goto out;
	nrec = g32(cnt4);
	if (nrec == BOOT_MAGIC) {              /* a pull, not a batch */
		bulk_serve_boot(cfd, &cs_s, &cs_r, stash, &slen, sizeof stash);
		goto out;
	}
	if (stage || nrec == CLBULK_STAGE_MAGIC) {     /* both, or neither */
		if (stage && nrec == CLBULK_STAGE_MAGIC)
			bulk_rx_stage(cfd, &cs_s, &cs_r, stash, &slen, sizeof stash);
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
		unsigned char rtype;
		pcache_htable_t *ht;
		char colz[256];
		str k, v;
		int brc;

		if (*bulk_stop)
			goto out;                  /* unanswered: the sender keeps its batch */
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
			rtype = (unsigned char)(hr.flags & PCACHE_F_CARRY);
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
		exp = pc_upstream_copy_exp(colz, cn, ttl_left);
		pc_lamport_observe(ver);
		/* a replica lands PASSIVE (never re-sent) and off the WAL (its author persists
		 * it); a placement move makes this node the owner */
		{
			unsigned long long tv;

			/* superseded by a newer tombstone for the key */
			if (pc_neg_ver(colz, cn, k.s, kn, &tv) && tv && ver <= tv) {
				__atomic_fetch_add(&C.px.recv_tombstoned, 1,
					__ATOMIC_RELAXED);
				continue;
			}
		}
		if (dropfloor_refuses(colz, cn, k.s, kn, ver))
			continue;
		brc = (replica ? pcache_ht_store_peer : pcache_ht_store_ver)(ht, &k, &v, exp,
			(unsigned char)((replica ? PCACHE_F_PASSIVE : 0) | rtype), ver);
		if (brc == -2)
			nomem_drop(ht, colz, cn, &k, ver);
		if (brc == PCACHE_E_OLDER) {
			__atomic_fetch_add(&C.px.recv_older, 1,
				__ATOMIC_RELAXED);
			/* refused as older = held here at this version or newer: delivered */
			resolved++;
		}
		if (brc == 0) {
			unsigned long long tv;

			if (pc_neg_ver(colz, cn, k.s, kn, &tv) &&
			        clorder_take_back(ht, &k, ver, tv)) {
				__atomic_fetch_add(&C.px.recv_tombstoned, 1,
					__ATOMIC_RELAXED);
				continue;
			}
			pc_upstream_note_copy(colz, cn, k.s, kn, exp, ver);
			if (replica) {
				pc_ev_applied(ht, &k);
			} else {
				pc_wal_upsert_fl(colz, k.s, k.len, v.s, v.len,
					exp, pcache_last_ver, rtype);
				pc_loc_clear(colz, cn, k.s, kn);
				if (pc_store_shard_enabled(ht))
					shard_note_change();
			}
			__atomic_fetch_add(&C.px.migrated_in, 1,
				__ATOMIC_RELAXED);
			__atomic_fetch_add(&C.px.bulk_in, 1,
				__ATOMIC_RELAXED);
			resolved++;
		}
	}
	/* answer RESOLVED, not stored: what this node now holds is all the sender can act on */
	p32(cnt4, resolved);
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
	bulk_stop = stop;
	pf.fd = C.blfd;
	pf.events = POLLIN;
	pc_qs_enter();                     /* a pull or a boot walk holds a table */
	while (!*stop) {
		int have_tx, have_boot, prc;

		/* both read under ONE acquisition, as boot_tick does - never split them */
		pthread_mutex_lock(clbulk_lock(&C.bk));
		have_tx = C.bk.n > 0;              /* a queue, not one */
		have_boot = C.bt.pending;
		pthread_mutex_unlock(clbulk_lock(&C.bk));
		if (have_boot)
			bulk_pull();
		if (have_tx)
			bulk_tx(stop);
		pc_qs_exit();
		prc = poll(&pf, 1, 200);
		pc_qs_enter();
		if (prc > 0) {
			pc_addr a;
			socklen_t al = sizeof a;
			int cfd = accept(C.blfd, &a.sa, &al);

			if (cfd >= 0 && !bulk_host_known(&a)) {
				if (pc_logrl_ok(LRL_BULK_HOST, &a)) {
					char ab[PC_ADDR_STRLEN];

					pc_addr_ntop(&a, ab, sizeof ab);
					LM_NOTICE("cluster: a bulk connection from %s, "
						"which is no member's host - closed\n", ab);
				}
				close(cfd);
			} else if (cfd >= 0)
				bulk_rx(cfd);
		}
	}
	pc_qs_exit();
}

/* ---- the cluster plane's internal API (clstate.h) -----------------------
 * the only way other units reach C; no mutable pointer escapes, counters keep their ordering */

struct clpeers *cl_peers(void) { return &C.pt; }
struct clpend  *cl_pend(void)  { return &C.pd; }
struct clpush  *cl_push(void)  { return &C.wp; }
struct clapply *cl_ap(void)    { return &C.ap; }

int cl_enabled(void)    { return C.enabled; }
int cl_node_id(void)    { return C.node_id; }
int cl_role(void)       { return C.role; }
int cl_master_id(void)  { return C.master_id; }
const char *cl_state_dir(void) { return C.state_dir; }
int cl_map_usable(void) { return C.map_usable; }
const struct pc_clmap *cl_map(void) { return &C.map; }
unsigned int cl_map_gen(void) { return __atomic_load_n(&C.map_gen, __ATOMIC_ACQUIRE); }
pc_addr cl_self_addr(void) { return C.self_addr; }

/* per thread, folded on its first hit and then 1,024 at a time: a shared ++ from every
 * worker is a contended line, and loses counts */
void cl_place_map_hit(void)
{
	static __thread unsigned int n;

	if (++n == 1 || n == 1025) {
		__atomic_add_fetch(&C.place_map_n, n == 1 ? 1 : 1024,
			__ATOMIC_RELAXED);
		n = 1;
	}
}
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
void cl_st_epoch_commits(void) { C.st.epoch_commits++; }       /* plain ++ */

void cl_px_fwd_sent(void)            { C.px.fwd_sent++; }
void cl_px_fwd_fails(void)           { C.px.fwd_fails++; }
void cl_px_migrate_skipped_big(void) { C.px.migrate_skipped_big++; }
void cl_px_placed_local(void)        { C.px.placed_local++; }
void cl_px_placed_remote(void)       { C.px.placed_remote++; }

/* the primitives clwork.c's entry points call instead of statics */
uint32_t cl_pend_alloc(int kind, int expect, int extra_ms,
		struct pending **out)
{
	return pend_alloc(kind, expect, extra_ms, out);
}

void cl_pend_release(struct pending *slot)
{
	clpend_lock(&C.pd);
	pend_release(slot);
	clpend_unlock(&C.pd);
}

void cl_set_fail(int reason) { cl_fail = reason; }
void cl_px_repl_pushed(unsigned long long n)
{
	__atomic_fetch_add(&C.px.repl_pushed, n, __ATOMIC_RELAXED);
}
void cl_px_repl_groups(void)
{
	__atomic_fetch_add(&C.px.repl_groups, 1, __ATOMIC_RELAXED);
}
void cl_px_spread_not_held(void)     { C.px.spread_not_held++; }
void cl_px_spread_ack(unsigned int parked, unsigned int kept)
{
	if (parked)
		__atomic_fetch_add(&C.px.spread_ack_parked, parked,
			__ATOMIC_RELAXED);
	if (kept)
		__atomic_fetch_add(&C.px.spread_ack_kept, kept, __ATOMIC_RELAXED);
}

/* clwork reaches these statics through wrappers rather than a rename at every site */
long long cl_now_ms(void) { return now_ms(); }
unsigned int cl_self_free_mb(void) { return self_free_mb(); }
unsigned int cl_self_total_mb(void) { return self_total_mb(); }
unsigned int cl_self_live_kb(void) { return self_live_kb(); }
int cl_max_datagram(void) { return cl_max_dgram; }

/* clrecv.c's writes, each as its call site had it - migrated_in
 * both ways (handle_migrate's plain ++, apply_rec's atomic) */
void cl_shard_note_change(void) { shard_note_change(); }
void cl_px_migrated_in(void)
{
	__atomic_fetch_add(&C.px.migrated_in, 1, __ATOMIC_RELAXED);
}
void cl_px_migrated_in_plain(void)   { C.px.migrated_in++; }
void cl_px_recv_nomem(void)
{
	__atomic_fetch_add(&C.px.recv_nomem, 1, __ATOMIC_RELAXED);
}
void cl_px_recv_older(void)
{
	__atomic_fetch_add(&C.px.recv_older, 1, __ATOMIC_RELAXED);
}
void cl_px_recv_tombstoned(void)
{
	__atomic_fetch_add(&C.px.recv_tombstoned, 1, __ATOMIC_RELAXED);
}
void cl_px_recv_below_dropped(void)
{
	__atomic_fetch_add(&C.px.recv_below_dropped, 1, __ATOMIC_RELAXED);
}
void cl_px_recv_stale_dropped(void)
{
	__atomic_fetch_add(&C.px.recv_stale_dropped, 1, __ATOMIC_RELAXED);
}

/* what cltomb.c reads and writes: the identity, the test hooks, two policy constants, the
 * resync flag and its counters (each a plain ++ as it was) */
const unsigned char *cl_self_ident(void) { return C.self_ident; }
const struct clres *cl_reserved(void) { return &reserved; }
long long cl_reserve_ms(void)        { return RESERVE_MS; }
long long cl_witness_grace_ms(void)  { return WITNESS_GRACE_MS; }
int cl_self_no_acks(void)            { return self_no_acks; }
int cl_self_gap_no_copies(void)      { return self_gap_no_copies; }
int cl_self_no_autoscale(void)       { return self_no_autoscale; }
int cl_self_as_deaf(void)            { return self_as_deaf; }
long long cl_acks_stuck_ms(void)     { return acks_stuck_ms; }
int cl_resync_floor(void)            { return C.resync_floor; }
void cl_resync_floor_clear(void)     { C.resync_floor = 0; }
void cl_st_acks_floors(void)         { C.st.acks_floors++; }
unsigned long long cl_st_acks_floors_read(void) { return C.st.acks_floors; }
void cl_st_tomb_forgotten(unsigned long long n) { C.st.tomb_forgotten += n; }
void cl_st_gap_asked(void)           { C.st.gap_asked++; }
void cl_st_gap_tail(void)            { C.st.gap_tail++; }
void cl_st_gap_resent(void)          { C.st.gap_resent++; }
void cl_st_gap_floored(void)         { C.st.gap_floored++; }
void cl_st_gap_served(void)          { C.st.gap_served++; }
void cl_st_rebuild_sent(void)        { C.st.rebuild_sent++; }
pc_addr cl_master_addr(void)         { return C.master_addr; }
int cl_unicast_on(void)              { return cl_unicast; }
void cl_st_members_asked(void)       { C.st.members_asked++; }
void cl_st_members_sent(void)        { C.st.members_sent++; }
void cl_st_admission_stuck(unsigned long long n) { C.st.admission_stuck = n; }

/* ---- init / stats ------------------------------------------------------- */

int pc_cluster_init(const char *mcast_addr, int mcast_port,
		const char *advertise, const uint8_t psk[PC_NOISE_KEYLEN],
		int pull_timeout_ms, int negative_ms, int tombstone_ms,
		const char *state_dir, const char *legacy_dir, int max_pending)
{
	/* the WAL takes this node out of service if it starts discarding
	 * acknowledged writes - see pc_node_wal_shed() */
	pc_wal_on_shed(pc_node_wal_shed);
	pc_wal_on_heal(pc_node_wal_heal);

	pc_addr sa;
	struct in_addr self_ip;
	struct ip_mreqn mreq;
	int one = 1;

	memset(&C.st, 0, sizeof C.st);
	/* patterns the peers hold; fixed buckets, read without a lock */
	clps_init();
	/* the parked-request table: sized ONCE, clamped not refused (the slot index rides in
	 * the request id), never resized - clpend_find() hands out raw pointers into it */
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
	/* an unusable state_dir refuses startup: a node silently running ephemeral would present
	 * as new after every restart.  Created 0700 (it holds the node's name) */
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
	/* the newest epoch this build speaks (the suites raise it to
	 * prove a commit), then the committed epoch a clean stop left */
	{
		const char *e = getenv("PERFCACHED_TEST_EPOCH_MAX");
		int v = e ? atoi(e) : 0;

		if (v >= PC_CL_EPOCH_MIN && v <= 255 && v != PC_CL_EPOCH_MAX) {
			epoch_set_max(v);
			LM_WARN("cluster: PERFCACHED_TEST_EPOCH_MAX=%d - this build "
				"CLAIMS to speak wire epochs %d..%d (a test flag; the "
				"codecs are those of %d)\n", v, PC_CL_EPOCH_MIN, v,
				PC_CL_EPOCH);
		}
		epoch_load();
	}
	{
		const char *e = getenv("PERFCACHED_TEST_ENTRIES_BIAS");

		self_entries_bias = e ? (unsigned int)atoi(e) : 0;
		if (self_entries_bias)
			LM_WARN("cluster: PERFCACHED_TEST_ENTRIES_BIAS=%u - this node CLAIMS "
				"that many more records to its peers (a test flag)\n",
				self_entries_bias);
	}
	if (getenv("PERFCACHED_TEST_NO_RESPMAP")) {
		self_no_respmap = 1;
		LM_WARN("cluster: PERFCACHED_TEST_NO_RESPMAP - this member does not "
			"announce the runtime RESP map (a test flag): the fleet "
			"refuses map/unmap while it is a member\n");
	}
	if (getenv("PERFCACHED_TEST_NO_AUTOSCALE")) {
		self_no_autoscale = 1;
		LM_WARN("cluster: PERFCACHED_TEST_NO_AUTOSCALE - this member does "
			"not take the fleet's autoscale modes (a test flag): the "
			"fleet refuses the autoscale verb while it is a member\n");
	}
	if (getenv("PERFCACHED_TEST_FREEZE_MARKS")) {
		self_freeze_marks = 1;
		LM_WARN("cluster: PERFCACHED_TEST_FREEZE_MARKS - this member "
			"announces each space's first mark for ever (a test flag): "
			"the fleet keeps for it every delete since\n");
	}
	if (getenv("PERFCACHED_TEST_ACKS_STUCK_MS"))
		acks_stuck_ms = atoll(getenv("PERFCACHED_TEST_ACKS_STUCK_MS"));
	self_gap_no_copies = getenv("PERFCACHED_TEST_GAP_NO_COPIES") != NULL;
	if (getenv("PERFCACHED_TEST_NO_ACKS")) {
		self_no_acks = 1;
		LM_WARN("cluster: PERFCACHED_TEST_NO_ACKS - this member announces "
			"no watermarks (a test flag): the fleet's origins forget "
			"nothing by marks while it is a member\n");
	}
	if (getenv("PERFCACHED_TEST_AUTOSCALE_DEAF")) {
		self_as_deaf = 1;
		LM_WARN("cluster: PERFCACHED_TEST_AUTOSCALE_DEAF - this member "
			"ignores the fleet's autoscale modes (a test flag)\n");
	}
	if (getenv("PERFCACHED_TEST_NO_PROMOTE")) {
		self_no_promote = 1;
		LM_WARN("cluster: PERFCACHED_TEST_NO_PROMOTE - this member never "
			"takes mastership (a test flag)\n");
	}
	if (getenv("PERFCACHED_TEST_RECONCILE")) {
		self_test_reconcile = 1;
		LM_WARN("cluster: PERFCACHED_TEST_RECONCILE - a restart's or a "
			"floor's records are ruled on one by one instead of taken "
			"whole from a survivor (a test flag)\n");
	}
	if (getenv("PERFCACHED_TEST_SURVIVOR_WAIT_MS")) {
		self_survivor_wait_ms = atoi(getenv("PERFCACHED_TEST_SURVIVOR_WAIT_MS"));
		LM_WARN("cluster: PERFCACHED_TEST_SURVIVOR_WAIT_MS - a rejoin's survivor is "
			"picked no sooner than %d ms after the first ask (a test flag)\n",
			self_survivor_wait_ms);
	}
	if (getenv("PERFCACHED_TEST_RECONCILE_WALK_WAIT_MS")) {
		self_recon_walk_wait_ms = atoi(getenv("PERFCACHED_TEST_RECONCILE_WALK_WAIT_MS"));
		LM_WARN("cluster: PERFCACHED_TEST_RECONCILE_WALK_WAIT_MS - a narrowed "
			"reconcile pass waits %d ms before it walks (a test flag)\n",
			self_recon_walk_wait_ms);
	}
	if (getenv("PERFCACHED_TEST_BOOT_DELAY_MS")) {
		self_boot_delay_ms = atoi(getenv("PERFCACHED_TEST_BOOT_DELAY_MS"));
		LM_WARN("cluster: PERFCACHED_TEST_BOOT_DELAY_MS - this node's first "
			"bootstrap pull waits %d ms (a test flag)\n",
			self_boot_delay_ms);
	}
	if (getenv("PERFCACHED_TEST_MIG_SCAN_RECS")) {
		self_mig_scan_recs = (unsigned int)atoi(getenv("PERFCACHED_TEST_MIG_SCAN_RECS"));
		LM_WARN("cluster: PERFCACHED_TEST_MIG_SCAN_RECS - a migration walk stops after "
			"%u records, as at its deadline (a test flag)\n", self_mig_scan_recs);
	}
	if (getenv("PERFCACHED_TEST_WALK_NOMEM_MS")) {
		int ms = atoi(getenv("PERFCACHED_TEST_WALK_NOMEM_MS"));

		self_walk_nomem_until = now_ms() + ms;
		LM_WARN("cluster: PERFCACHED_TEST_WALK_NOMEM_MS - the sweep and migration walks "
			"fail for the first %d ms (a test flag)\n", ms);
	}
	if (getenv("PERFCACHED_TEST_NO_HOLDQ")) {
		self_no_holdq = 1;
		LM_WARN("cluster: PERFCACHED_TEST_NO_HOLDQ - this member does not "
			"answer hold-checks (a test flag): the fleet's reconcile "
			"asks with pulls while it is a member\n");
	}
	/* the term must outlive any map somebody could still believe: an unreadable term refuses
	 * startup, since forgetting one reissues it with different content */
	/* a term persisted in the legacy (WAL) directory moves over once: a reset term is an
	 * election hazard */
	if (legacy_dir && state_dir && *state_dir)
		carry_file(legacy_dir, state_dir, PC_TERM_FILE);
	/* the cluster id, before anything is sealed */
	if (pc_cid_init(state_dir, pc_cid_pin) != 0)
		return -1;
	if (pc_term_init(state_dir) != 0) {
		LM_CRIT("cluster: %s/" PC_TERM_FILE " does not validate - "
			"refusing to start rather than risk reissuing a term "
			"someone has already seen.  Restore the file, or delete "
			"it to start the term from 0\n",
			state_dir ? state_dir : "(none)");
		return -1;
	}
	/* the reconcile arms only when this node came back holding data */
	pc_clhist_init(&C.hist);
	pc_clsync_init(&C.sync, pc_term_current(), 0);
	C.reconcile_on = pc_recovered_records() > 0;
	C.rec_rebuild = 0;                 /* a restart's pass */
	C.rec_floored = 0;
	C.rec_tick = get_ticks();
	C.rec_hold = C.reconcile_on;
	C.rec_col = 0;
	C.rec_cursor = 0;
	C.recon.witness = 0;
	C.recon.death_mono = reconcile_death_mono();
	if (pc_term_durable() || !(state_dir && *state_dir))
		LM_NOTICE("cluster: mastership term %u (%s)\n", pc_term_current(),
			pc_term_durable() ? "persisted"
			: "EPHEMERAL - nothing persisted here, so nothing to "
			  "contradict either");
	else
		LM_ERR("cluster: mastership term %u (NOT persisted - %s refused "
			"it: %s)\n", pc_term_current(), state_dir,
			strerror(pc_term_store_errno()));
	memcpy(C.psk, psk, PC_NOISE_KEYLEN);
	C.pull_timeout_ms = pull_timeout_ms;
	C.negative_ms = negative_ms;
	C.tombstone_ms = tombstone_ms;

	/* the port is the cluster port in both modes (the bulk listener and
	 * pc_cluster_port() read it here); the group only in multicast */
	memset(&C.mcast_dst, 0, sizeof C.mcast_dst);
	C.mcast_dst.in4.sin_family = AF_INET;
	C.mcast_dst.in4.sin_port = htons((uint16_t)mcast_port);
	C.mfd = -1;
	if (cl_unicast) {
		/* seeds resolved before anything is sent */
		if (clmesh_start(cl_seeds, mcast_port) != 0)
			return -1;
	} else if (!mcast_addr ||
	        inet_pton(AF_INET, mcast_addr, &C.mcast_dst.in4.sin_addr) != 1 ||
	        !IN_MULTICAST(ntohl(C.mcast_dst.in4.sin_addr.s_addr))) {
		LM_ERR("cluster: '%s' is not a multicast group\n",
			mcast_addr ? mcast_addr : "");
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
		pc_addr probe_sa, toward = C.mcast_dst;
		socklen_t sl = sizeof probe_sa;
		int pfd;

		/* unicast has no group to route toward - a seed */
		if (cl_unicast && clmesh_targets(&toward, 1, NULL, 0) != 1) {
			LM_ERR("cluster: discovery = unicast and no seed resolved "
				"- cannot auto-detect the advertise address; set "
				"advertise = <ip>\n");
			return -1;
		}
		pfd = socket(AF_INET, SOCK_DGRAM, 0);
		if (pfd < 0)
			return -1;
		if (connect(pfd, (struct sockaddr *)&toward,
		        sizeof toward) != 0 ||
		        getsockname(pfd, (struct sockaddr *)&probe_sa, &sl) != 0) {
			LM_ERR("cluster: cannot auto-detect the advertise "
				"address (%s) - set advertise = <ip>\n",
				strerror(errno));
			close(pfd);
			return -1;
		}
		close(pfd);
		self_ip = probe_sa.in4.sin_addr;
	}

	/* the unicast socket: the data plane, and the source address every
	 * peer learns us by */
	C.fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (C.fd < 0)
		return -1;
	clsend_init(C.fd, C.psk, NULL);        /* the socket and key, once */
	if (have_psk_accept)
		clsend_set_accept(psk_accept);
	{
		/* a dropped forward is a refused client write (forwards never retry): force the
		 * buffer past rmem_max where allowed, read it back, and warn if it was clamped */
		int rb = PC_CLUSTER_RCVBUF, got = 0;
		socklen_t gl = sizeof got;

#ifdef SO_RCVBUFFORCE
		if (setsockopt(C.fd, SOL_SOCKET, SO_RCVBUFFORCE, &rb,
		        sizeof rb) != 0)
#endif
			setsockopt(C.fd, SOL_SOCKET, SO_RCVBUF, &rb, sizeof rb);
		/* per-socket drop counts: host-wide RcvbufErrors
		 * would mix in any other daemon on the host */
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
	/* the mcast rx socket shares this port, so both need REUSEADDR; SO_REUSEPORT is
	 * deliberately unset - it lets a stray daemon join a live port unnoticed */
	setsockopt(C.fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	memset(&sa, 0, sizeof sa);
	sa.in4.sin_family = AF_INET;
	sa.in4.sin_port = htons((uint16_t)mcast_port);
	sa.in4.sin_addr = self_ip;
	if (bind(C.fd, &sa.sa, pc_addr_len(&sa)) != 0) {
		LM_ERR("cluster: cannot bind %s:%d (%s)\n",
			inet_ntoa(self_ip), mcast_port, strerror(errno));
		return -1;
	}
	C.self_addr = sa;
	clrpt_set_source(&C.self_addr);    /* the receiver gates on our host */
	clwire_set_self(sa.in4.sin_addr.s_addr, (uint16_t)mcast_port);
	fcntl(C.fd, F_SETFL, O_NONBLOCK);
	/* unicast mode carries frames over max_datagram on the
	 * TCP frame lane - nothing fragments */
	if (cl_unicast && cl_max_dgram > 0) {
		if (clane_start(&C.self_addr, cl_lane_port - mcast_port) != 0)
			return -1;
		clsend_set_lane(clane_send, (size_t)cl_max_dgram, clane_forced);
		LM_NOTICE("cluster: frames over %d bytes go by the TCP frame lane "
			"(port %d)\n", cl_max_dgram, cl_lane_port);
	}
	/* multicast SENDS leave the unicast socket, so peers learn our
	 * unicast address from the datagram source itself */
	if (!cl_unicast) {
		unsigned char ttl = (unsigned char)pc_mcast_ttl, loop = 1;

		/* 1 by default - no router forwards it; more lets a
		 * multicast router carry the cluster plane across segments */
		setsockopt(C.fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl,
			sizeof ttl);
		if (pc_mcast_ttl > 1)
			LM_NOTICE("cluster: multicast TTL %d - the group may be "
				"routed beyond this segment; the unicast planes "
				"(cluster, relay, bulk ports to every advertise "
				"address) must route too\n", pc_mcast_ttl);
		setsockopt(C.fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop,
			sizeof loop);
		memset(&mreq, 0, sizeof mreq);
		mreq.imr_address = self_ip;
		setsockopt(C.fd, IPPROTO_IP, IP_MULTICAST_IF, &mreq,
			sizeof mreq);
	}

	/* the multicast rx socket, ANY:port + membership; none in unicast mode, where every
	 * control frame arrives on C.fd */
	if (cl_unicast)
		goto group_done;
	C.mfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (C.mfd < 0)
		return -1;
	setsockopt(C.mfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	setsockopt(C.mfd, SOL_SOCKET, SO_RXQ_OVFL, &one, sizeof one);
#ifdef IP_PKTINFO
	/* each datagram's destination, so drain_one_fd takes only the
	 * group's here - see there */
	setsockopt(C.mfd, IPPROTO_IP, IP_PKTINFO, &one, sizeof one);
#endif
#ifdef IP_MULTICAST_ALL
	{
		int zero = 0;          /* the group it joined, not every one on the port */

		setsockopt(C.mfd, IPPROTO_IP, IP_MULTICAST_ALL, &zero, sizeof zero);
	}
#endif
	memset(&sa, 0, sizeof sa);
	sa.in4.sin_family = AF_INET;
	sa.in4.sin_port = htons((uint16_t)mcast_port);
	sa.in4.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(C.mfd, &sa.sa, pc_addr_len(&sa)) != 0) {
		LM_ERR("cluster: cannot bind multicast rx port %d (%s)\n",
			mcast_port, strerror(errno));
		return -1;
	}
	memset(&mreq, 0, sizeof mreq);
	mreq.imr_multiaddr = C.mcast_dst.in4.sin_addr;
	mreq.imr_address = self_ip;
	if (setsockopt(C.mfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq,
	        sizeof mreq) != 0) {
		LM_ERR("cluster: cannot join group %s (%s)\n", mcast_addr,
			strerror(errno));
		return -1;
	}
	fcntl(C.mfd, F_SETFL, O_NONBLOCK);
group_done:

	/* the bulk TCP listener shares the cluster port number */
	clbulk_init(&C.bk);
	C.blfd = socket(AF_INET, SOCK_STREAM, 0);
	if (C.blfd >= 0) {
		int bone = 1;

		setsockopt(C.blfd, SOL_SOCKET, SO_REUSEADDR, &bone,
			sizeof bone);
		memset(&sa, 0, sizeof sa);
		sa.in4.sin_family = AF_INET;
		sa.in4.sin_port = htons((uint16_t)mcast_port);
		sa.in4.sin_addr = self_ip;
		if (bind(C.blfd, &sa.sa, pc_addr_len(&sa)) != 0 ||
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
	clfound_window_open(now_ms());
	C.join_held = 0;
	C.join_last_ms = 0;
	C.st.enabled = 1;
	C.enabled = 1;
	/* the apply rings; 1 = apply inline on the receiver */
	if (C.apply_threads_cfg >= 2) {
		int n = clapply_init(&C.ap, C.apply_threads_cfg, apply_rec,
			ack_group, NULL);

		C.ap.clock = cl_now_ms;    /* busy_since_ms stamps at the take */

		if (n < 0)
			LM_ERR("cluster: no memory for %d apply rings - "
				"applying on the receiver\n", C.apply_threads_cfg);
		else
			LM_NOTICE("cluster: replica apply sharded over %d "
				"threads by key hash\n", n);
		/* its apply threads keep the pushes, so peers keep the cluster socket */
		clrp_take(n < 1);
	}
	LM_NOTICE("cluster: automatic membership - %s %s:%d, "
		"advertising %s:%d, pull timeout %dms, negative %dms, "
		"tombstone %dms\n", cl_unicast ? "unicast (seeds), port" :
		"group", cl_unicast ? "*" : mcast_addr, mcast_port,
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

int pc_cluster_fleet_stop(long long timeout_ms, int *gone, int *ngone,
		int *still, int *nstill, int *lost, int *nlost, int cap)
{
	unsigned int bye_mark = __atomic_load_n(&bye_n, __ATOMIC_ACQUIRE);
	struct clpeers *pt = &C.pt;
	unsigned char msg[8];
	int targets[PC_CL_MAXPEER], nt = 0, i, k, sent = 0;
	long long now = now_ms(), until;
	uint32_t term = pc_term_current();

	*ngone = *nstill = *nlost = 0;
	if (!C.enabled)
		return -1;
	/* the targets are fixed now: a node joining mid-stop is not waited for, and one that
	 * leaves on its own is still accounted for */
	for (i = 0; i < pt->n_peers && nt < PC_CL_MAXPEER; i++) {
		int node = __atomic_load_n(&pt->peers[i].node, __ATOMIC_ACQUIRE);

		if (node && clpeers_live(&pt->peers[i], now))
			targets[nt++] = node;
	}
	msg[0] = M_FLEETSTOP;
	msg[1] = (unsigned char)(C.node_id >> 8);
	msg[2] = (unsigned char)C.node_id;
	msg[3] = (unsigned char)(term >> 24);
	msg[4] = (unsigned char)(term >> 16);
	msg[5] = (unsigned char)(term >> 8);
	msg[6] = (unsigned char)term;
	LM_NOTICE("cluster: FLEET STOP - asking %d peer(s) to stop cleanly; "
		"this node follows once they have gone\n", nt);
	for (k = 0; k < 3; k++) {        /* one datagram each: send it thrice */
		sent = clsend_live(pt, msg, 7, now_ms());
		usleep(150000);
	}
	(void)sent;
	/* wait for them to leave - a clean stop ends in a goodbye, which
	 * takes a node out of the live set at once */
	until = now_ms() + timeout_ms;
	for (;;) {
		int up = 0;

		now = now_ms();
		for (i = 0; i < nt; i++)
			if (clpeers_by_id_live(pt, targets[i], now))
				up++;
		if (!up || now >= until)
			break;
		usleep(100000);
	}
	now = now_ms();
	for (i = 0; i < nt; i++) {
		if (clpeers_by_id_live(pt, targets[i], now)) {
			if (*nstill < cap)
				still[(*nstill)++] = targets[i];
		} else if (bye_since(targets[i], bye_mark)) {
			if (*ngone < cap)
				gone[(*ngone)++] = targets[i];
		} else if (*nlost < cap)
			lost[(*nlost)++] = targets[i];
	}
	if (*nstill)
		LM_WARN("cluster: FLEET STOP - %d peer(s) did not go within "
			"%lld ms; they are still up and still serving\n",
			*nstill, timeout_ms);
	/* gone from the live set is not stopped: only a goodbye says it stopped */
	if (*nlost)
		LM_WARN("cluster: FLEET STOP - %d peer(s) left without a "
			"goodbye; nothing says they stopped\n", *nlost);
	schedule_self_stop(500);         /* after the reply has gone out */
	return nt;
}

void pc_cluster_get_stats(struct pc_cl_stats *out)
{

	rx_sample();                   /* sampled by the reader */
	*out = C.st;
	out->stalled = pc_node_stalled();
	out->reconcile_pending = (unsigned long long)
		clpend_used_kind(&C.pd, PC_DONE_RECONCILE);
	out->apply_threads = C.ap.n;
	out->apply_dispatched = C.ap.dispatched;
	out->apply_blocks = 0;
	out->apply_backlog = 0;
	{
		int i;

		long long now = now_ms();

		for (i = 0; i < C.ap.n; i++) {
			long long b = __atomic_load_n(&C.ap.ring[i].busy_since_ms,
				__ATOMIC_RELAXED);
			long long w = __atomic_load_n(&C.ap.ring[i].wait_since_ms,
				__ATOMIC_RELAXED);

			out->apply_blocks += C.ap.ring[i].blocks;
			out->apply_backlog += clapply_backlog(&C.ap, i);
			if (i < 16) {
				out->apply_ring[i].backlog = clapply_backlog(&C.ap, i);
				out->apply_ring[i].busy_ms = b ? now - b : 0;
				out->apply_ring[i].wait_ms = w ? now - w : 0;
			}
		}
	}
	out->stalled_for_ms = out->stalled ? now_ms() - C.stall_since_ms : 0;
	/* totals and queue gauges are read live, so they never disagree with migrated_in in one
	 * reply; only the per-second figures come from the sample */
	out->rx_applied = __atomic_load_n(&C.px.migrated_in, __ATOMIC_RELAXED);
	out->rx_drops = __atomic_load_n(&C.rxq_drops, __ATOMIC_RELAXED);
	out->rx_queue = rx_queue_bytes(C.fd) + rx_queue_bytes(C.mfd);
	out->rx_rcvbuf = (unsigned int)(C.rcvbuf > 0 ? C.rcvbuf : 0);
	out->pend_max = C.pd.cap;      /* the capacity peak is measured against */
	out->pend_used = clpend_used(&C.pd);
	out->pend_peak = clpend_peak(&C.pd);
	out->pend_peak_win = clpend_peak_win(&C.pd);
	out->pend_exhausted = clpend_exhausted(&C.pd);
	out->neg_hits = clloc_neg_hits(&C.lc);
	out->neg_displaced = clloc_neg_displaced(&C.lc);
	out->tomb_displaced = clloc_tomb_displaced(&C.lc);
	out->node_id = pc_my_node_id();
	out->peers_up = 0;
	out->reserved_ids = clres_count(&reserved, now_ms());
	out->peers_up = clpeers_count_live(&C.pt, now_ms());
	out->replicas_short = clspread_short_now(&replicas_short);
	out->term_unpersisted = pc_term_unpersisted();
	out->term_errno = out->term_unpersisted ? pc_term_store_errno() : 0;
}

int pc_cluster_peers_known(void)
{
	int i, n = 0;

	for (i = 0; i < C.pt.n_peers; i++)
		if (__atomic_load_n(&C.pt.peers[i].node, __ATOMIC_ACQUIRE))
			n++;
	/* a purged silent member keeps its id reserved - and its share */
	return n + clres_count_why(&reserved, CLRES_GONE_SILENT, now_ms());
}

int pc_cluster_peers(struct pc_cl_peer_info *out, int max)
{
	int i, n = 0;

	for (i = 0; i < C.pt.n_peers && n < max; i++) {
		out[n].node = C.pt.peers[i].node;
		out[n].up = now_ms() - C.pt.peers[i].last_seen_ms < PEER_UP_MS;
		out[n].last_seen_ms = C.pt.peers[i].last_seen_ms;
		out[n].free_mb = C.pt.peers[i].free_mb;
		out[n].state = C.pt.peers[i].nstate;
		if (C.pt.peers[i].has_ident) {
			clplat_uuid_str(C.pt.peers[i].ident, out[n].ident);
			out[n].ident_ver = clplat_uuid_version(
				C.pt.peers[i].ident);
		} else {
			out[n].ident[0] = 0;
			out[n].ident_ver = 0;
		}
		n++;
	}
	return n;
}
