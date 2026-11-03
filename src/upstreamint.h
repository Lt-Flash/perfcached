/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * upstreamint.h - the read-through's internal types (upstream.c): the
 * request and single-flight bookkeeping, the completions, one upstream
 * server and one upstream collection, and the counter macros.  For the
 * files the upstream is split into (the 0.7.2 split, wave 1 -
 * doc/MODULARITY.md); upstream.h stays the API the rest of the daemon
 * calls.
 */
#ifndef PC_UPSTREAMINT_H
#define PC_UPSTREAMINT_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "compat/str.h"
#include "core/pcache_htable.h"
#include "store.h"                      /* PC_COL_NAME_MAX */
#include "upstream.h"                   /* PC_UP_MAX_SRV, PC_UP_* */
#include "winctr.h"                     /* struct pc_winmax */

#define UP_MAX_PREFIXES 32
#define UP_MAX_COLS     8

/* ---- the request and single-flight bookkeeping ------------------------ */

/* 12nt (0.7.1): a peer's pull that this node, the key's owner, answers
 * once the fetch is done - the asker, carried on the request */
struct up_pull {
	uint32_t addr;
	uint16_t port;
	uint32_t req;
	int col;
	size_t klen;
	char key[];
};

struct up_req {                        /* one parked command */
	unsigned int id;
	int worker;
	int left;                          /* keys still being fetched */
	int status;                        /* PC_UPV_* so far */
	struct up_pull *pull;              /* 12nt: a pull's asker, else NULL */
};

struct up_wait {
	struct up_wait *next;
	struct up_req *r;
};

struct up_key {                        /* one key in flight */
	struct up_key *next;               /* the collection's in-flight list */
	struct up_key *qnext;              /* the job queue */
	struct up_wait *waiters;
	int own;                           /* an OWNERSHIP check rides on it:
	                                    * the worker below records a
	                                    * no-fall entry if the upstream
	                                    * has the key */
	int own_worker;
	int full;                          /* ST2: a waiter needs the VALUE (a
	                                    * read or a change); 0 = TYPE + PTTL
	                                    * answer everyone (EXISTS, TTL,
	                                    * TYPE, an ownership check).  Set
	                                    * under uc->mx; a fetch that stopped
	                                    * at a PRESENT marker is queued
	                                    * again if it was set meanwhile */
	int fetched;                       /* ST2: what the fetch left (UPF_*) */
	int tried;                         /* S382: retried on another upstream */
	int inv;                           /* S424: an invalidation of the key
	                                    * arrived while this fetch was in
	                                    * flight - its answer may be older
	                                    * than the change.  Under uc->mx */
	int rounds;                        /* S424: fetched again for that */
	int rw;                            /* S431: created by the re-warm */
	size_t klen;
	char key[];
};

/* ---- a completion on its way to a worker ------------------------------ */

#define UPD_REQ  0                     /* a request is done: run it again */
#define UPD_OWN  1                     /* an ownership check is done */
#define UPD_PULL 2                     /* the drain sweep read a key */
#define UPD_WT   3                     /* S385: a forwarded write is done */
#define UPD_INV  4                     /* AR1: Redis changed a key (broadcast) */
#define UPD_INVALL 5                   /* AR1: tracking had a gap - drop every
                                        * record written through */

/* S422 (0.7.1): the keys this node wrote through lately - a 64-bit hash
 * and the time, in a table indexed by the hash (a collision costs one
 * needless refetch).  Redis echoes a write as an invalidation to every
 * tracking connection (the writer's is not the tracked one, so NOLOOP
 * cannot stop it), and that echo used to drop the record just stored: a
 * write-through record was never cached.  Now an invalidation inside the
 * echo window after this node's own write refetches the key instead of
 * leaving the slot empty - the copy that comes back is Redis's current
 * state, so a concurrent change by anyone else is honoured too. */
#define UP_ECHO_N 8192
struct up_echo {
	uint64_t h;
	long long t0;                      /* mono ms of the write's apply */
};
/* S429 (0.7.1-rc1's upwritethroughtest 15, under ASan): the read-backs of a
 * collection's writes come back from Redis in the order Redis applied them
 * - one writer thread, one connection - but each is APPLIED on the worker
 * that queued its write, so two writes to one key could be applied the
 * other way round and the node keep the older value ("Redis v46, node
 * v45") with no invalidation left to correct it.  The writer stamps each
 * completion; the apply skips a read-back older than the newest it applied
 * for the key.  Slots by key hash, a stripe lock around check and store */
#define UP_ORDER_N 8192
#define UP_ORDER_LOCKS 16
struct up_order {
	uint64_t h;
	unsigned long long seq;
};

/* S385: a key's state read back after a forwarded write - the worker makes
 * the fleet holds exactly that */
#define UPRB_VALUE   0                 /* a value: the fleet's record */
#define UPRB_GONE    1                 /* none upstream: deleted here too */
#define UPRB_RELEASE 2                 /* a type the fleet cannot hold: Redis's */

struct up_rb {
	int what;                          /* UPRB_* */
	char *key;
	size_t klen;
	char *val;
	size_t vlen;
	unsigned int exp;                  /* absolute ticks, 0 = never */
	unsigned char fl;                  /* the TYPE bits */
};

/* S385: what a forwarded write's re-run answers */
#define UPW_R_DONE 0                   /* Redis applied it: relay its reply */
#define UPW_R_NIL  1                   /* a conditional write not applied */
#define UPW_R_ERR  2                   /* an error line (val, NUL-ended) */

/* 12nt TC-2 (0.7.1): a forwarded op (M_FWD_OP) run through at the key's
 * owner - who asked, their req, and the op the ack answers */
struct up_fwd {
	uint32_t addr;
	uint16_t port;
	uint32_t req;
	int op;
};

struct up_done {
	unsigned int id;
	int status;
	int kind;                          /* UPD_* */
	struct up_fwd *fwd;                /* UPD_WT, 12nt: ack the asker, no
	                                    * frame to re-run */
	int col;                           /* UPD_OWN: the collection */
	char *key;                         /* UPD_OWN/PULL: malloc'd */
	size_t klen;
	char *val;                         /* UPD_PULL: the value, malloc'd */
	size_t vlen;
	unsigned int exp;                  /* UPD_PULL: absolute, 0 = never */
	unsigned char fl;                  /* UPD_PULL: the TYPE bits */
	/* S385, UPD_WT: UPW_R_*; val is the reply in RESP2 (DONE, NIL) or
	 * the error text (ERR); ival the reply as a number for the native
	 * doors (OK 1, an integer itself, nil 0); the keys read back */
	int wst;
	long long ival;
	struct up_rb *rb;
	int nrb;
	unsigned long long wseq;           /* S429: the writer's order, UPD_WT */
};

struct up_wq {
	pthread_mutex_t mx;
	struct up_done *q;
	int n, cap;
	int efd;
};

/* ---- one upstream of a collection (S382) --------------------------------- */

#define UPR_UNKNOWN 0                  /* not asked yet (or no ROLE here) */
#define UPR_MASTER  1
#define UPR_REPLICA 2
#define UPR_OTHER   3                  /* a sentinel, or a reply not known */

struct up_col;

/* S385: one forwarded write, queued for the collection's writer.  argv,
 * argl, kidx and the bytes are one allocation.  A second command (JSON.SET's
 * EXPIRE: RedisJSON has no EX) rides after the first, its reply unused -
 * AR5: n2 of them, argc2 arguments each (a native mset's EXPIREs) */
struct up_wjob {
	struct up_wjob *next;
	unsigned int id;
	int worker;
	int unk;                           /* RESP, not known here: Redis's
	                                    * COMMAND INFO classes it first */
	int json;                          /* a JSON write: needs RedisJSON */
	int guess;                         /* the read-back's first ask:
	                                    * UPWG_* */
	int argc, argc2, n2;
	char **argv;                       /* [argc] then n2 x [argc2] */
	int upfill;                        /* AR4: the upfill sweep's - no
	                                    * client waits; an NX Redis declined
	                                    * still reads the key back */
	int upf_vi;                        /* S400: argv's index of the value
	                                    * it offered */
	unsigned int upf_exp;              /* S400: the fleet's expiry then
	                                    * (absolute ticks, 0 = never) */
	size_t *argl;
	int nkeys;
	int *kidx;                         /* the keys among argv */
	long long t0;                      /* queued (mono ms) */
	long long tq;                      /* queued (mono us): the stats' clock */
	struct up_fwd *fwd;                /* 12nt: a peer's forwarded op (owned) */
};

#define UPWG_STRING 0                  /* GET with TYPE and PTTL */
#define UPWG_HASH   1                  /* HGETALL */
#define UPWG_JSON   2                  /* JSON.GET */
#define UPWG_GONE   3                  /* a delete: TYPE and PTTL say it */

/* S385: why a write-through collection has no write target */
#define UPWS_OK       0
#define UPWS_STARTING 1                /* not looked for yet */
#define UPWS_NOMASTER 2                /* no listed upstream says master */
#define UPWS_AMBIG    3                /* two say master, no replica says which */
#define UPWS_DOWN     4                /* the master stopped answering */

/* S385 stats: what went to the master, by command, and the last errors
 * with what Redis said - the page shows both.  A row is added by the
 * writer thread only and published by nwcmd; its counters are atomic */
#define WT_NCMD  24                    /* the last row takes the rest: "other" */
#define WC_OK    0                     /* applied */
#define WC_NIL   1                     /* not applied: its condition said no */
#define WC_RERR  2                     /* Redis answered an error */
#define WC_FAIL  3                     /* not done: no master, lost, timed out */
#define WC_US    4                     /* microseconds, queued to answered */
#define WC_TN    5                     /* the writes timed */
#define WC_N     6
struct wt_cmd {
	char name[24];                     /* lower case, as sent to Redis */
	int passed;                        /* a command perfcached does not know */
	unsigned long long c[WC_N], base[WC_N];
};
#define WT_NERR  8
struct wt_err {
	long long at;                      /* unix seconds */
	char kind[12];                     /* redis, failed, refused */
	char cmd[24];
	char key[64];
	char msg[160];
};

/* S382: a read-through collection has one or more upstreams.  Each has its
 * own fetch threads and queue, its own tracking connection - the run_id
 * rule holds per server - and its own shadow table, so a server that can
 * no longer say a key changed loses its copies and no other's.  The tables
 * are chained: the collection reads srv[0]'s, each table's shadow pointer
 * names the next (pcache_htable.c walks it). */
struct up_srv {
	struct up_col *uc;
	int idx;                           /* in uc->srv[] */
	char host[256];
	int port;
	int db;
	char user[128];
	char addr[272];                    /* host:port, for the log and /stats */
	pcache_htable_t *shadow;           /* the copies fetched from here */
	pthread_cond_t cv;                 /* its queue's, under uc->mx */
	struct up_key *qhead, *qtail;      /* under uc->mx */
	int queued, busy;                  /* under uc->mx: pick()'s measure */
	pthread_t thr[16];
	int nthr;
	/* tracking (phase D): one connection receives this server's
	 * invalidations (its fetch connections REDIRECT to it); its id, and a
	 * generation that moves whenever tracking had a gap - this server's
	 * shadow is flushed and its fetch connections re-arm */
	pthread_t inv_thr;
	int has_inv;
	int bcast;                         /* AR1: 1 broadcast tracking on, -1 the
	                                    * server refused it, 0 not asked */
	long long inv_id;                  /* 0: not up */
	int inv_ready;                     /* S431: tracking up AND its change
	                                    * settled (the flush done) - what
	                                    * the re-warm routes by; 0 from the
	                                    * moment a loss is seen */
	unsigned int inv_gen;
	/* the server the invalidation connection reached (its run_id).  A
	 * client id names a client on ONE server: a fetch connection that
	 * reached another one (a round-robin proxy in front of replicas)
	 * must not redirect to it - there that id is nobody, or somebody
	 * else, and the invalidations would never arrive here */
	pthread_mutex_t run_mx;
	char inv_run[41];
	int other_said;
	unsigned long long other_server;
	/* health (S382): what ROLE and INFO replication said at the last
	 * connect or check, and whether connecting works */
	int role;                          /* UPR_* */
	int link_down;                     /* a replica's link to its master */
	long long lag_ms;                  /* a replica's, -1 not known */
	char mhost[64];                    /* S385: a replica's master, as its
	                                    * INFO replication names it */
	int mport;
	int failing;                       /* the last connect failed */
	int is_pc;                         /* S384: INFO server said perfcached */
	int refused_said, lag_said, pc_said;
	long long checked_ms;              /* the last ROLE check */
	int conns_up;
	/* its share (S382): fetches sent here, failed, their time */
	unsigned long long fetches, errors, timeouts, connects, lat_us_sum,
		lat_n, invalidations, flushes;
};

/* ---- one upstream collection ------------------------------------------ */

struct up_col {
	char name[PC_COL_NAME_MAX];
	size_t nlen;
	char nf[PC_COL_NAME_MAX];          /* the companion: <name>.nofall */
	int inv_all_pending;               /* AR1: a tracking gap seen before a
	                                    * worker could take it (at start) */
	int refuse;                        /* S348: upstream_writes = refuse */
	char refuse_msg[384];
	size_t nflen;
	int idx;                           /* in up_cols[] */
	struct up_srv srv[PC_UP_MAX_SRV];  /* S382: one or more */
	int nsrv;
	int role_req;                      /* S382: PC_UP_ROLE_* */
	int master_fallback;               /* S382: a master while no replica */
	int max_lag_ms;                    /* S382: 0 = not checked */
	int echo_ms;                       /* S422: upstream_echo_ms, 0 off */
	struct up_echo *echo;              /* S422: UP_ECHO_N slots, through only */
	unsigned long long wt_seq;         /* S429: the writer thread's own count */
	struct up_order *order;            /* S429: UP_ORDER_N slots, through only */
	pthread_mutex_t order_mx[UP_ORDER_LOCKS];
	int allow_pc;                      /* S384: upstream_allow_perfcached */
	/* S385: write-through - one thread with one connection to the master
	 * takes every forwarded write, in the order the workers queued them */
	int through;                       /* upstream_writes = through */
	int unknown_pass;                  /* upstream_unknown = pass */
	pthread_t wthr;
	int has_wthr;
	pthread_cond_t wcv;                /* its queue's, under mx */
	struct up_wjob *wq_head, *wq_tail; /* under mx */
	int wq_n;
	int wq_max;                        /* AR2: upstream_write_queue - queued
	                                    * + the writer's batch, at most */
	int w_inflight;                    /* AR2: the batch the writer holds */
	int pr_n;                          /* AR2: the pass reader's queue */
	int w_master;                      /* the write target (index), -1 none */
	int w_json;                        /* -1 not asked, 0 no RedisJSON, 1 yes */
	int w_state;                       /* UPWS_*: why there is no target */
	char w_why[160];                   /* the last failure, under mx */
	pthread_t rthr;                    /* the pass reader (unknown reads) */
	int has_rthr;
	pthread_cond_t prcv;
	struct up_wjob *pr_head, *pr_tail; /* under mx */
	int pass_max;                      /* upstream_pass_max_kb, in bytes */
	unsigned long long wt_fwd, wt_ok, wt_nil, wt_rerr, wt_failed,
		wt_timeouts, wt_refused, wt_json_refused, wt_us_sum, wt_n,
		wt_rb_value, wt_rb_gone, wt_rb_release, pass_w, pass_r,
		pass_refused, wt_checks;
	unsigned long long wt_qfull, wt_qexpired;   /* AR2 */
	unsigned long long wt_inval, wt_invall;     /* AR1: records dropped on a
	                                             * broadcast invalidation; flushes */
	unsigned long long wt_bare;                 /* S404: a "t" mark found with
	                                             * no record, dropped */
	unsigned long long wt_echo;                 /* S422: refetches after the
	                                             * echo of this node's write */
	unsigned long long wt_echo_master;          /* S423: of the fetches inside the echo
	                                            * window, those sent to the master */
	unsigned long long wt_stale;                /* S429: read-backs older than one applied */
	unsigned long long wt_cpu_us;               /* S403: the writer thread's
	                                             * CPU so far (it sets it) */
	unsigned long long wt_pipelined;            /* AR3: writes sent in a run
	                                             * with others */
	struct wt_cmd wcmd[WT_NCMD];       /* S385 stats: by command */
	int nwcmd;                         /* rows in use (release-stored) */
	struct wt_err werr[WT_NERR];       /* the last errors: a ring, under mx */
	unsigned int werr_n;               /* recorded ever, under mx */
	struct pc_winmax w_maxus;          /* the slowest write, 5 min */
	/* S382 part 2: failover - one ACTIVE upstream takes every fetch */
	int mode;                          /* PC_UP_MODE_* */
	int primary;                       /* the one active first (index) */
	int preempt;                       /* a returning primary takes over */
	long long hold_ms;                 /* ...once it has served this long */
	int active;                        /* under mx: -1 before the first */
	long long active_since;            /* wall ms it became active */
	long long pri_ok_since;            /* mono ms the primary has served
	                                    * since, 0 while it does not */
	unsigned long long failovers;      /* switches away from the primary */
	unsigned long long chained;        /* S384: reads by another perfcached's
	                                    * read-through, never fetched onward */
	int rr;                            /* under mx: where pick() starts */
	int fb_said;                       /* S382: the fallback logged */
	const char *password;              /* the config's; never logged */
	const char *wuser, *wpassword;     /* AR7: the writer's own, NULL =
	                                    * the above; never logged */
	char *pat[UP_MAX_PREFIXES];
	int patlen[UP_MAX_PREFIXES];
	int npat;
	int neg_ms, timeout_ms, conns, tracking, on_error;

	pthread_mutex_t mx;                /* inflight, the queues, requests */
	struct up_key *inflight;
	volatile int stop;
	/* the drain sweep (an operator's one-shot): a thread reading every
	 * allowed key the upstream holds, the workers writing what the fleet
	 * does not have yet */
	pthread_t drain_thr;
	int drain_joinable;
	volatile int drain_state;          /* UPDR_* */
	volatile int drain_stop;
	int drain_rate;
	int drain_mb_rate;                 /* S401: MB a second, 0 = no cap */
	/* S421 (0.7.1): the names of the records a tracking gap dropped, fetched
	 * again in the background at drain_rate - under mx */
	int rewarm_max;                    /* upstream_rewarm_max, 0 = off */
	str *rw;
	int rw_n, rw_cap, rw_head;
	long long rw_last_ms;              /* the pacer's last tick */
	unsigned long long rw_started, rw_skipped, rw_dropped, rw_runs;
	int rw_dispatch;                   /* S431: rewarm_tick is joining - join_key
	                                    * routes to a tracked server */
	unsigned long long rw_requeued;    /* S431: names queued again (a copy
	                                    * flushed at a tracking change, or
	                                    * fetched untracked) */
	long long dr_bytes;                /* S401: the drain sweep's bytes read */
	long long drain_t0, drain_t1;      /* unix seconds */
	int drain_inflight;                /* posted, not yet applied */
	/* AR4: the upfill sweep (an operator's one-shot under through): the
	 * keys the fleet owned before the switch ("1" in the no-fall
	 * companion) written to the master with NX, through the writer */
	pthread_t upf_thr;
	int upf_joinable;
	volatile int upf_state;            /* UPDR_* */
	volatile int upf_stop;
	long long upf_t0, upf_t1;          /* unix seconds */
	unsigned long long upf_found, upf_queued, upf_written, upf_kept,
		upf_skipped, upf_gone, upf_errors,
		upf_answered;              /* the writer's verdicts, any */
	/* S400: of upf_kept, how Redis's value compared with the one
	 * offered; check: what a run would do, nothing applied */
	unsigned long long upf_same, upf_retimed, upf_differed, upf_missing;
	unsigned int upf_named;            /* differing keys logged, this sweep */
	int upf_check;                     /* this sweep is `check` */
	/* S402: how each verdict was reached - the text alone, a walk, or
	 * none (uncompared: not decided, or the hand-off queue was full) -
	 * and the hand-off from the writer to the sweep's thread */
	unsigned long long upf_cmp_text, upf_cmp_walked, upf_uncompared;
	long long upf_bytes;               /* S401: the values offered / compared */
	pthread_mutex_t upf_qmx;
	struct upf_cj *upf_qh, *upf_qt;
	unsigned int upf_qn;
	size_t upf_qbytes;
	long long wt_owned;                /* AR4: "1" under the prefixes, as
	                                    * the writer last counted them (a
	                                    * minute apart); -1 not yet */

	unsigned long long copies_marked;  /* S418: peers' copies marked "t" on arrival */
	unsigned long long copies_premarked;  /* S418: ones whose author's mark,
	                                    * newer, had arrived first */
	unsigned long long rm_begin, rm_end;   /* S435: removals of copies or
	                                    * records (an invalidation, a flush,
	                                    * an apply) begun / done - a read
	                                    * that saw neither move lost nothing */

	/* stats - the collection's, every upstream's together */
	unsigned long long held, fetches, fill_string, fill_hash, fill_json,
		absent, wrongtype, toobig, errors, timeouts, connects,
		shadow_hits, pull_fetches, fwd_through, neg_hits, joined, fill_failed, lat_us_sum, lat_n,
		promoted, nofall_written, nofall_hits, own_checks, own_found,
		writes_refused,
		fill_present, present_hits, refetches,          /* ST2 */
		inv_inflight, inv_refetch, inv_kept,            /* S424 */
		read_lost,                                      /* S435 */
		invalidations, flushes, untracked,
		dr_scanned, dr_pulled, dr_owned, dr_absent, dr_wrongtype,
		dr_toobig, dr_errors,
		dr_elsewhere;                  /* 12nt TC-4: another node's to take */
	/* S358: each counter's value at the last stats reset - /stats
	 * subtracts it.  Written by the (serialised) reset, read fuzzily; the
	 * drain-run figures dr_* describe a run, and are not reset. */
	struct {
		unsigned long long held, fetches, fill_string, fill_hash, fill_json, absent,
			wrongtype, toobig, errors, timeouts, connects, shadow_hits, pull_fetches, fwd_through,
			neg_hits, joined, fill_failed, lat_us_sum, lat_n, promoted,
			nofall_written, nofall_hits, own_checks, own_found, writes_refused, invalidations,
			flushes, untracked, fill_present, present_hits, refetches;
		unsigned long long inv_inflight, inv_refetch, inv_kept;   /* S424 */
		unsigned long long read_lost;                      /* S435 */
		unsigned long long wt_fwd, wt_ok, wt_nil, wt_rerr, wt_failed, wt_timeouts, wt_refused, wt_json_refused, wt_us_sum, wt_n, wt_rb_value, wt_rb_gone, wt_rb_release, pass_w, pass_r, pass_refused, wt_checks;   /* S385 */
		unsigned long long wt_qfull, wt_qexpired;          /* AR2 */
		unsigned long long wt_inval, wt_invall;            /* AR1 */
		unsigned long long wt_bare;                        /* S404 */
		unsigned long long wt_echo;                        /* S422 */
		unsigned long long wt_echo_master;                 /* S423 */
		unsigned long long wt_stale;                       /* S429 */
		unsigned long long wt_pipelined;                   /* AR3 */
	} base;
};

#define UPDR_IDLE    0
#define UPDR_RUNNING 1
#define UPDR_DONE    2
#define UPDR_STOPPED 3
#define UPDR_FAILED  4

#define ST_ADD(f, v) __atomic_add_fetch(&(f), (v), __ATOMIC_RELAXED)
#define ST_GET(f) __atomic_load_n(&(f), __ATOMIC_RELAXED)
/* S358: a counter as /stats shows it - since the last stats reset */
#define UPV(uc, f) (ST_GET((uc)->f) - (uc)->base.f)

/* ---- one connection to an upstream (upconn.c) ------------------------- */

struct rv;

struct uconn {
	struct up_col *uc;
	struct up_srv *s;                  /* S382: the upstream it talks to */
	int fd;
	int resp3;
	int tracked;                       /* tracking on, redirected */
	unsigned int gen;                  /* the s->inv_gen it armed at */
	int inv;                           /* THE invalidation connection */
	int notrack;                       /* never armed: the drain sweep's */
	int wr;                            /* S385: the writer's or the pass
	                                    * reader's - RESP2 (what is relayed
	                                    * is what a RESP2 client gets), never
	                                    * tracked, a master kept */
	int wcred;                         /* AR7: the writer's own (not the
	                                    * pass reader's): AUTHs with
	                                    * upstream_write_* when set */
	char run_id[41];                   /* its server's run_id, "" unknown */
	char *buf;
	size_t len, cap, off;              /* buf[off..len) unread */
	long long backoff_until;           /* no reconnect before (ms) */
};

/* the RESP client (upconn.c): connect with the handshake (HELLO 3 / AUTH,
 * SELECT, INFO server, ROLE, CLIENT TRACKING), close, send a command as
 * an array of bulks, read one reply (a push in between goes to on_push).
 * 0 done; -1 the connection failed; -2 the deadline @dl (now_ms) passed;
 * -3 a reply larger than this thread's rv_max */
int uc_connect(struct uconn *c, long long dl);
void uc_close(struct uconn *c);
int send_cmd(struct uconn *c, long long dl, int argc,
		const char *const *av, const size_t *al);
int read_reply(struct uconn *c, struct rv *v, long long dl);
/* as read_reply, and the reply's bytes are buf[*sp, *ep) */
int read_reply_span(struct uconn *c, struct rv *v, long long dl,
		size_t *sp, size_t *ep);
/* S382: what the server is - ROLE, and a replica's INFO replication */
int role_check(struct uconn *c, long long dl);
void nap_ms(int ms);
long long now_ms(void);                         /* CLOCK_MONOTONIC */
long long now_us(void);

/* ---- owned by upstream.c ---------------------------------------------- */

extern struct up_col up_cols[UP_MAX_COLS];      /* ncols of them configured */
extern const char *const upws_name[];           /* by UPWS_* */
extern const char *const updr_name[];           /* by UPDR_* */
struct up_col *col_of(const char *col, size_t collen);
/* an upstream's state (PC_UPS_*) and why it is not used ("" when it is) */
int srv_state(const struct up_srv *s);
const char *srv_why(const struct up_srv *s);
/* AR1: 1 every upstream whose tracking connection is up follows every
 * change (broadcast), -1 one refused it, 0 none up or not asked */
int bcast_state(const struct up_col *uc);
/* a RESP3 push read on a connection - an invalidation (tracking) */
void on_push(struct uconn *c, struct rv *v);
/* the copies fetched from @s are no longer tracked: drop them */
void flush_shadow(struct up_srv *s);

#endif
