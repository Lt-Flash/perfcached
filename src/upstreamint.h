/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* upstreamint.h - the read-through's internal types: request and single-flight bookkeeping,
 * completions, one upstream server and collection, the counter macros (upstream.h is the API) */
#ifndef PC_UPSTREAMINT_H
#define PC_UPSTREAMINT_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "compat/str.h"
#include "core/pcache_htable.h"
#include "store.h"                      /* PC_COL_NAME_MAX */
#include "upstream.h"                   /* PC_UP_MAX_SRV, PC_UP_* */
#include "uptls.h"
#include "winctr.h"                     /* struct pc_winmax */

#define UP_MAX_PREFIXES 32
#define UP_MAX_COLS     8

/* ---- the request and single-flight bookkeeping ------------------------ */

/* a peer's pull that this node, the key's owner, answers
 * once the fetch is done - the asker, carried on the request */
struct up_pull {
	uint32_t addr;
	uint16_t port;
	uint32_t req;
	int col;
	struct pcache_relay *relay;        /* the copy that found no room */
	size_t klen;
	char key[];
};

struct up_req {                        /* one parked command */
	unsigned int id;
	int worker;
	int left;                          /* keys still being fetched */
	int status;                        /* PC_UPV_* so far */
	struct up_pull *pull;              /* a pull's asker, else NULL */
	struct pcache_relay *relay;        /* its keys' copies that found no room */
};

struct up_wait {
	struct up_wait *next;
	struct up_req *r;
};

struct up_key {                        /* one key in flight */
	struct up_key *next;               /* its bucket's chain in the in-flight table */
	struct up_key *qnext;              /* the job queue */
	struct up_wait *waiters;
	int own;                           /* an OWNERSHIP check rides on it: the worker
	                                    * records a no-fall entry if the upstream has it */
	int own_worker;
	int full;                          /* a waiter needs the VALUE, else TYPE + PTTL answer
	                                    * all; under uc->mx, a fetch re-queued if set late */
	int fetched;                       /* what the fetch left (UPF_*) */
	int tried;                         /* retried on another upstream */
	int inv;                           /* an invalidation arrived mid-fetch, so the answer
	                                    * may predate the change.  Under uc->mx */
	int rounds;                        /* fetched again for that */
	int rw;                            /* created by the re-warm */
	int bgown;                         /* created by a plain write's ownership check */
	int ask;                           /* an -ASK sent it here: ASKING first */
	int redirs;                        /* MOVED/ASK followed for it */
	char rhost[64];                    /* where the redirect points */
	int rport;
	struct pcache_relay *unkept;       /* the fetched copy the arena had no room for */
	size_t klen;
	char key[];
};

/* ---- a completion on its way to a worker ------------------------------ */

#define UPD_REQ  0                     /* a request is done: run it again */
#define UPD_OWN  1                     /* an ownership check is done */
#define UPD_PULL 2                     /* the drain sweep read a key */
#define UPD_WT   3                     /* a forwarded write is done */
#define UPD_INV  4                     /* Redis changed a key (broadcast) */
#define UPD_INVALL 5                   /* tracking had a gap - drop every
                                        * record written through */

/* keys written through lately (hash, time; a collision costs a refetch): Redis echoes a write as
 * an invalidation NOLOOP cannot stop, so one inside the echo window refetches, not drops */
#define UP_ECHO_N 8192
#define UPO_CONFIG   0                 /* listed in upstream = */
#define UPO_ROLE     1                 /* a master's ROLE named it */
#define UPO_SENTINEL 2                 /* a sentinel named it */
#define UPO_CLUSTER  3                 /* CLUSTER SHARDS / SLOTS named it */
struct up_echo {
	uint64_t h;
	long long t0;                      /* mono ms of the write's apply */
};
/* read-backs return in Redis's apply order but are APPLIED on the queuing worker, so two to a key
 * could invert: the writer stamps each, the apply skips one older than the newest applied */
#define UP_ORDER_N 8192
#define UP_ORDER_LOCKS 16
struct up_order {
	uint64_t h;
	unsigned long long seq;
};

/* a key's state read back after a forwarded write - the worker makes
 * the fleet hold exactly that */
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

/* what a forwarded write's re-run answers */
#define UPW_R_DONE 0                   /* Redis applied it: relay its reply */
#define UPW_R_NIL  1                   /* a conditional write not applied */
#define UPW_R_ERR  2                   /* an error line (val, NUL-ended) */

/* a forwarded op (M_FWD_OP) run through at the key's
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
	struct up_fwd *fwd;                /* UPD_WT: ack the asker, no
	                                    * frame to re-run */
	int col;                           /* UPD_OWN: the collection */
	char *key;                         /* UPD_OWN/PULL: malloc'd */
	size_t klen;
	char *val;                         /* UPD_PULL: the value, malloc'd */
	size_t vlen;
	unsigned int exp;                  /* UPD_PULL: absolute, 0 = never */
	unsigned char fl;                  /* UPD_PULL: the TYPE bits */
	/* UPD_WT: UPW_R_*; val the reply in RESP2 (DONE, NIL) or the error text (ERR), ival the
	 * reply as a number for the native doors (OK 1, an integer, nil 0); the keys read back */
	int wst;
	long long ival;
	struct up_rb *rb;
	int nrb;
	unsigned long long wseq;           /* the writer's order, UPD_WT */
	struct pcache_relay *relay;        /* UPD_REQ: for the re-run */
};

struct up_wq {
	pthread_mutex_t mx;
	struct up_done *q;
	int n, cap;
	int efd;
};

/* ---- one upstream of a collection --------------------------------------- */

#define UPR_UNKNOWN 0                  /* not asked yet (or no ROLE here) */
#define UPR_MASTER  1
#define UPR_REPLICA 2
#define UPR_OTHER   3                  /* a sentinel, or a reply not known */

struct up_col;

/* one forwarded write for the collection's writer; argv, argl, kidx and the bytes one allocation,
 * then n2 commands of argc2 arguments (JSON.SET's EXPIRE, a native mset's EXPIREs) */
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
	int upfill;                        /* the upfill sweep's: no client waits; an NX
	                                    * Redis declined still reads the key back */
	int upf_vi;                        /* argv's index of the value
	                                    * it offered */
	unsigned int upf_exp;              /* the fleet's expiry then
	                                    * (absolute ticks, 0 = never) */
	size_t *argl;
	int nkeys;
	int *kidx;                         /* the keys among argv */
	long long t0;                      /* queued (mono ms) */
	long long tq;                      /* queued (mono us): the stats' clock */
	struct up_fwd *fwd;                /* a peer's forwarded op (owned) */
	int moved;                         /* 1 a MOVED to re-run, 2 re-run */
	char mhost[64];                    /* where the MOVED points */
	int mport;
};

#define UPWG_STRING 0                  /* GET with TYPE and PTTL */
#define UPWG_HASH   1                  /* HGETALL */
#define UPWG_JSON   2                  /* JSON.GET */
#define UPWG_GONE   3                  /* a delete: TYPE and PTTL say it */

/* why a write-through collection has no write target */
#define UPWS_OK       0
#define UPWS_STARTING 1                /* not looked for yet */
#define UPWS_NOMASTER 2                /* no listed upstream says master */
#define UPWS_AMBIG    3                /* two say master, no replica says which */
#define UPWS_DOWN     4                /* the master stopped answering */

/* what went to the master by command, and the last errors with what Redis said; a row is added
 * by the writer thread only and published by nwcmd, its counters atomic */
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

/* one upstream of a read-through collection: its own fetch threads, queue, tracking connection and
 * shadow table (chained: srv[0]'s read first, each shadow pointer naming the next) */
struct up_srv {
	struct up_col *uc;
	int idx;                           /* in uc->srv[] */
	char host[256];
	int port;
	int db;
	char user[128];
	char addr[272];                    /* host:port, for the log and /stats */
	int origin;                        /* UPO_*, how it came to be listed */
	int gone;                          /* left the topology - off, its slot kept */
	long long gone_ms;                 /* since when (a slot reused past a quarantine) */
	unsigned int incarn;               /* moves when the slot takes another address */
	int direct;                        /* 1 its server answers at this address,
	                                    * 0 through a proxy, -1 not known */
	char behind[160];                  /* the replicas its ROLE names, under run_mx */
	int behind_said;
	pcache_htable_t *shadow;           /* the copies fetched from here */
	pthread_cond_t cv;                 /* its queue's, under uc->mx */
	struct up_key *qhead, *qtail;      /* under uc->mx */
	int queued, busy;                  /* under uc->mx: pick()'s measure */
	pthread_t thr[16];
	int nthr;
	/* tracking: one connection gets the server's invalidations (fetches REDIRECT to it); its
	 * id, and a generation moved by any gap (shadow flushed, fetches re-armed) */
	pthread_t inv_thr;
	int has_inv;
	int inv_quit;                      /* its thread returned */
	int bcast;                         /* 1 broadcast tracking on, -1 the
	                                    * server refused it, 0 not asked */
	long long inv_id;                  /* 0: not up */
	int inv_ready;                     /* tracking up AND its change settled - what the
	                                    * re-warm routes by; 0 once a loss is seen */
	unsigned int inv_gen;
	/* the server the invalidation connection reached (run_id): a client id names a client on
	 * ONE server, so a fetch connection reaching another (a proxy) must not redirect */
	pthread_mutex_t run_mx;
	char inv_run[41];
	int other_said;
	unsigned long long other_server;
	/* health: what ROLE and INFO replication said at the last
	 * connect or check, and whether connecting works */
	int role;                          /* UPR_* */
	int link_down;                     /* a replica's link to its master */
	long long lag_ms;                  /* a replica's, -1 not known */
	char mhost[64];                    /* a replica's master, as its
	                                    * INFO replication names it */
	int mport;
	int failing;                       /* the last connect failed */
	int is_pc;                         /* INFO server said perfcached */
	int is_cluster;                    /* INFO server said redis_mode:cluster */
	int is_sentinel;                   /* redis_mode:sentinel - a topology source */
	char peer_ip[64];                  /* the address it answered at, under run_mx */
	int role_by;                       /* the master whose ROLE named it, -1 */
	int shard;                         /* its shard in the slot map, -1 none */
	char slots[96];                    /* the slot ranges it serves, under uc->mx */
	int refused_said, lag_said, pc_said, cluster_said;
	long long checked_ms;              /* the last ROLE check */
	int conns_up;
	/* its share: fetches sent here, failed, their time */
	unsigned long long fetches, errors, timeouts, connects, lat_us_sum,
		lat_n, invalidations, flushes;
};

/* ---- one upstream collection ------------------------------------------ */

struct up_col {
	char name[PC_COL_NAME_MAX];
	size_t nlen;
	char nf[PC_COL_NAME_MAX];          /* the companion: <name>.nofall */
	int inv_all_pending;               /* a tracking gap seen before a
	                                    * worker could take it (at start) */
	int refuse;                        /* upstream_writes = refuse */
	char refuse_msg[384];
	size_t nflen;
	int idx;                           /* in up_cols[] */
	struct up_srv srv[PC_UP_MAX_SRV];  /* one or more */
	int nsrv;                          /* read with srv_n() - one may be added running */
	pthread_mutex_t add_mx;            /* one adder at a time */
	int shadow_log2, full_said;        /* a new shadow's size; the limit, said once */
	int discover;                      /* PC_UP_DISC_* */
	const char *smaster, *spassword;   /* upstream_sentinel_*; the password never logged */
	struct up_tls *tls;                /* rediss:// - every connection of it, NULL plain */
	const char *tls_name, *tls_ca, *tls_cert, *tls_key;
	struct up_tls *tls_next;           /* a reload's, staged until it applies */
	int tls_verify;
	pthread_mutex_t tls_mx;            /* tls_last, tls_said */
	unsigned long long tls_hs, tls_failed;
	char tls_last[200], tls_said[200]; /* the last failure, for /stats; the last one logged */
	int sen_started;                   /* the topology thread, once */
	int sen_joinable;                  /* sen_thr runs; joined at the shutdown */
	pthread_t sen_thr;
	pthread_mutex_t sen_mx;            /* sen_text, sen_known */
	char sen_text[256];                /* what the sentinels said, for /stats */
	struct {
		char host[64];
		int port;
	} sen_known[8];                    /* the sentinels learned, seeds first */
	int sen_n;
	int sen_master;                    /* the master the sentinels name (srv index), -1 */
	int sen_alone_said;                /* one sentinel's all-down view, said once */
	unsigned long long topo_sum;       /* what the topology file last held */
	/* a Redis Cluster - its slot map (under mx), the topology thread */
	int cluster;                       /* a seed said redis_mode:cluster */
	struct up_cmap *cmap;              /* under mx; NULL until the first answer */
	int cl_started, cl_kick, cl_over_said;
	int cl_joinable;                   /* cl_thr runs; joined at the shutdown */
	pthread_t cl_thr;
	char cl_text[256];                 /* what the cluster said, under sen_mx */
	unsigned long long cl_moved, cl_asked, cl_unserved, cl_rounds;
	int role_req;                      /* PC_UP_ROLE_* */
	int master_fallback;               /* a master while no replica */
	int max_lag_ms;                    /* 0 = not checked */
	int echo_ms;                       /* upstream_echo_ms, 0 off */
	struct up_echo *echo;              /* UP_ECHO_N slots, through only */
	unsigned long long wt_seq;         /* the writer and the pass reader count, atomically */
	struct up_order *order;            /* UP_ORDER_N slots, through only */
	pthread_mutex_t order_mx[UP_ORDER_LOCKS];
	int allow_pc;                      /* upstream_allow_perfcached */
	/* write-through - one thread with one connection to the master
	 * takes every forwarded write, in the order the workers queued them */
	int through;                       /* upstream_writes = through */
	int unknown_pass;                  /* upstream_unknown = pass */
	pthread_t wthr;
	int has_wthr;
	pthread_cond_t wcv;                /* its queue's, under mx */
	struct up_wjob *wq_head, *wq_tail; /* under mx */
	int wq_n;
	int wq_max;                        /* upstream_write_queue - queued
	                                    * + the writer's batch, at most */
	int w_inflight;                    /* the batch the writer holds */
	int pr_n;                          /* the pass reader's queue */
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
	unsigned long long wt_qfull, wt_qexpired;
	unsigned long long wt_inval, wt_invall;     /* records dropped on a
	                                             * broadcast invalidation; flushes */
	unsigned long long wt_bare;                 /* a "t" mark found with
	                                             * no record, dropped */
	unsigned long long wt_echo;                 /* refetches after the
	                                             * echo of this node's write */
	unsigned long long wt_echo_master;          /* of the fetches inside the echo
	                                            * window, those sent to the master */
	unsigned long long wt_stale;                /* read-backs older than one applied */
	unsigned long long wt_cpu_us;               /* the writer thread's
	                                             * CPU so far (it sets it) */
	unsigned long long wt_pipelined;            /* writes sent in a run
	                                             * with others */
	struct wt_cmd wcmd[WT_NCMD];       /* stats: by command */
	int nwcmd;                         /* rows in use (release-stored) */
	struct wt_err werr[WT_NERR];       /* the last errors: a ring, under mx */
	unsigned int werr_n;               /* recorded ever, under mx */
	struct pc_winmax w_maxus;          /* the slowest write, 5 min */
	/* failover - one ACTIVE upstream takes every fetch */
	int mode;                          /* PC_UP_MODE_* */
	int primary;                       /* the one active first (index) */
	int preempt;                       /* a returning primary takes over */
	long long hold_ms;                 /* ...once it has served this long */
	int active;                        /* under mx: -1 before the first */
	long long active_since;            /* wall ms it became active */
	long long pri_ok_since;            /* mono ms the primary has served
	                                    * since, 0 while it does not */
	unsigned long long failovers;      /* switches away from the primary */
	unsigned long long chained;        /* reads by another perfcached's
	                                    * read-through, never fetched onward */
	int rr;                            /* under mx: where pick() starts */
	int fb_said;                       /* the fallback logged */
	const char *password;              /* the config's; never logged */
	const char *wuser, *wpassword;     /* the writer's own, NULL =
	                                    * the above; never logged */
	char *pat[UP_MAX_PREFIXES];
	int patlen[UP_MAX_PREFIXES];
	int npat;
	int neg_ms, timeout_ms, conns, tracking, on_error;

	pthread_mutex_t mx;                /* inflight, the queues, requests */
	struct up_key **inflight;          /* UP_IFH buckets by echo_hash, made on first use */
	int own_bg;                        /* in-flight entries a plain write's check created */
	volatile int stop;
	/* the drain sweep (an operator's one-shot): a thread reading every allowed upstream key,
	 * the workers writing what the fleet does not have yet */
	pthread_t drain_thr;
	int drain_joinable;
	volatile int drain_state;          /* UPDR_* */
	volatile int drain_stop;
	int drain_rate;
	int drain_mb_rate;                 /* MB a second, 0 = no cap */
	/* the names of the records a tracking gap dropped, fetched
	 * again in the background at drain_rate - under mx */
	int rewarm_max;                    /* upstream_rewarm_max, 0 = off */
	str *rw;
	int rw_n, rw_cap, rw_head;
	long long rw_last_ms;              /* the pacer's last tick */
	unsigned long long rw_started, rw_skipped, rw_dropped, rw_runs;
	int rw_dispatch;                   /* rewarm_tick is joining - join_key
	                                    * routes to a tracked server */
	unsigned long long rw_requeued;    /* names queued again (a copy flushed at a
	                                    * tracking change, or fetched untracked) */
	long long dr_bytes;                /* the drain sweep's bytes read */
	long long drain_t0, drain_t1;      /* unix seconds */
	int drain_inflight;                /* posted, not yet applied */
	/* the upfill sweep (a one-shot under through): keys owned before the switch ("1" in the
	 * no-fall companion), written to the master with NX via the writer */
	pthread_t upf_thr;
	int upf_joinable;
	volatile int upf_state;            /* UPDR_* */
	volatile int upf_stop;
	long long upf_t0, upf_t1;          /* unix seconds */
	unsigned long long upf_found, upf_queued, upf_written, upf_kept,
		upf_skipped, upf_gone, upf_errors,
		upf_answered;              /* the writer's verdicts, any */
	/* of upf_kept, how Redis's value compared with the one
	 * offered; check: what a run would do, nothing applied */
	unsigned long long upf_same, upf_retimed, upf_differed, upf_missing;
	unsigned int upf_named;            /* differing keys logged, this sweep */
	int upf_check;                     /* this sweep is `check` */
	/* how each verdict was reached: the text alone, a walk, or none (undecided, or the hand-off
	 * queue full); and the writer's hand-off to the sweep's thread */
	unsigned long long upf_cmp_text, upf_cmp_walked, upf_uncompared;
	long long upf_bytes;               /* the values offered / compared */
	pthread_mutex_t upf_qmx;
	struct upf_cj *upf_qh, *upf_qt;
	unsigned int upf_qn;
	size_t upf_qbytes;
	long long wt_owned;                /* "1" under the prefixes as the writer last
	                                    * counted them (a minute apart); -1 not yet */
	pthread_mutex_t own_mx;            /* the last walk's unmarked */
	unsigned long long *own_prev;      /* keys, hashed and sorted */
	int own_prev_n, own_seen;

	unsigned long long copies_marked;  /* peers' copies marked "t" on arrival */
	unsigned long long copies_premarked;  /* ones whose author's mark,
	                                    * newer, had arrived first */
	unsigned long long rm_begin, rm_end;   /* removals of copies or records begun / done:
	                                    * a read seeing neither move lost nothing */

	/* stats - the collection's, every upstream's together */
	unsigned long long held, fetches, fill_string, fill_hash, fill_json,
		absent, wrongtype, toobig, errors, timeouts, connects,
		shadow_hits, pull_fetches, fwd_through, neg_hits, joined, fill_failed, lat_us_sum, lat_n,
		promoted, nofall_written, nofall_hits, own_checks, own_found,
		writes_refused,
		fill_present, present_hits, refetches,
		inv_inflight, inv_refetch, inv_kept,
		read_lost,
		read_lost_spent,
		invalidations, flushes, untracked,
		dr_scanned, dr_pulled, dr_owned, dr_absent, dr_wrongtype,
		dr_toobig, dr_errors,
		dr_elsewhere;                  /* another node's to take */
	/* each counter at the last stats reset, which /stats subtracts; written by the serialised
	 * reset, read fuzzily. dr_* describe a run and are not reset. */
	struct {
		unsigned long long held, fetches, fill_string, fill_hash, fill_json, absent,
			wrongtype, toobig, errors, timeouts, connects, shadow_hits, pull_fetches, fwd_through,
			neg_hits, joined, fill_failed, lat_us_sum, lat_n, promoted,
			nofall_written, nofall_hits, own_checks, own_found, writes_refused, invalidations,
			flushes, untracked, fill_present, present_hits, refetches;
		unsigned long long inv_inflight, inv_refetch, inv_kept;
		unsigned long long read_lost, read_lost_spent;
		unsigned long long wt_fwd, wt_ok, wt_nil, wt_rerr, wt_failed, wt_timeouts, wt_refused, wt_json_refused, wt_us_sum, wt_n, wt_rb_value, wt_rb_gone, wt_rb_release, pass_w, pass_r, pass_refused, wt_checks;
		unsigned long long wt_qfull, wt_qexpired;
		unsigned long long wt_inval, wt_invall;
		unsigned long long wt_bare;
		unsigned long long wt_echo;
		unsigned long long wt_echo_master;
		unsigned long long wt_stale;
		unsigned long long wt_pipelined;
	} base;
};

#define UPDR_IDLE    0
#define UPDR_RUNNING 1
#define UPDR_DONE    2
#define UPDR_STOPPED 3
#define UPDR_FAILED  4

#define ST_ADD(f, v) __atomic_add_fetch(&(f), (v), __ATOMIC_RELAXED)
#define ST_GET(f) __atomic_load_n(&(f), __ATOMIC_RELAXED)
/* a counter as /stats shows it - since the last stats reset */
#define UPV(uc, f) (ST_GET((uc)->f) - (uc)->base.f)

/* a Redis Cluster's slots - each to a shard, each shard a master and its nodes (a
 * bit per uc->srv[] index); built aside, swapped in under uc->mx */
#define UP_SLOTS 16384
#define UP_NOSHARD 0xff
struct up_cmap {
	unsigned char sh[UP_SLOTS];        /* the slot's shard, UP_NOSHARD none */
	int nsh;
	int master[PC_UP_MAX_SRV];         /* the shard's master (srv index), -1 */
	uint32_t nodes[PC_UP_MAX_SRV];     /* its master and replicas */
	int served;                        /* slots with a shard */
};

/* ---- one connection to an upstream (upconn.c) ------------------------- */

struct rv;

struct uconn {
	struct up_col *uc;
	struct up_srv *s;                  /* the upstream it talks to */
	int fd;
	struct up_tls_conn *tls;           /* over TLS, NULL plain */
	char peer[80];                     /* host:port dialled, for a TLS failure */
	int resp3;
	int tracked;                       /* tracking on, redirected */
	int untrackable;                   /* tracking refused, or another server */
	unsigned int gen;                  /* the s->inv_gen it armed at */
	unsigned int incarn;               /* the s->incarn it dialled */
	int inv;                           /* THE invalidation connection */
	int notrack;                       /* never armed: the drain sweep's */
	int wr;                            /* the writer's or the pass reader's: RESP2 (what
	                                    * a RESP2 client gets), never tracked, a master */
	int redir;                         /* the fetch follows a MOVED or ASK - a
	                                    * server off for its role is read this once */
	int wcred;                         /* the writer's own (not the pass reader's):
	                                    * AUTHs with upstream_write_* when set */
	char run_id[41];                   /* its server's run_id, "" unknown */
	char *buf;
	size_t len, cap, off;              /* buf[off..len) unread */
	long long backoff_until;           /* no reconnect before (ms) */
};

/* the RESP client: uc_connect handshakes (HELLO 3 / AUTH, SELECT, INFO, ROLE, TRACKING); 0 done,
 * -1 the connection failed, -2 the deadline @dl (now_ms) passed, -3 a reply over rv_max */
int uc_connect(struct uconn *c, long long dl);
void uc_close(struct uconn *c);
int send_cmd(struct uconn *c, long long dl, int argc,
		const char *const *av, const size_t *al);
int read_reply(struct uconn *c, struct rv *v, long long dl);
/* as read_reply, and the reply's bytes are buf[*sp, *ep) */
int read_reply_span(struct uconn *c, struct rv *v, long long dl,
		size_t *sp, size_t *ep);
/* what the server is - ROLE, and a replica's INFO replication */
int role_check(struct uconn *c, long long dl);
void nap_ms(int ms);
long long now_ms(void);                         /* CLOCK_MONOTONIC */
/* a connection to @host:@port by @dl, TLS for rediss://: 0 up, -1 not; uc_drop closes one with no
 * up_srv count; uc_buffered: bytes already read and decrypted, unseen by poll() */
int uc_open(struct uconn *c, const char *host, int port, long long dl);
void uc_drop(struct uconn *c);
int uc_buffered(struct uconn *c);
long long now_us(void);

/* ---- owned by upstream.c ---------------------------------------------- */

extern struct up_col up_cols[UP_MAX_COLS];      /* ncols of them configured */
extern const char *const upws_name[];           /* by UPWS_* */
extern const char *const updr_name[];           /* by UPDR_* */
struct up_col *col_of(const char *col, size_t collen);
/* the upstreams' count - a discovered one is published only once its slot is whole */
static inline int srv_n(const struct up_col *uc)
{
	return __atomic_load_n(&uc->nsrv, __ATOMIC_ACQUIRE);
}

/* an upstream's state (PC_UPS_*) and, srv_why, why it is not used ("" when it is) */
int srv_state(const struct up_srv *s);
int srv_add(struct up_col *uc, const char *host, int port, int origin);
void srv_gone(struct up_srv *s);
void srv_discovered(struct up_srv *m, char hosts[][64], const int *ports, int n);
int uc_dial(const char *host, int port, long long dl);
void sen_start(struct up_col *uc);
/* the topology thread, once; a round now (a MOVED); the servers a key's
 * slot may go to (a bit per srv index, under uc->mx) - all when the map says nothing */
void cl_start(struct up_col *uc);
void cl_kick(struct up_col *uc);
uint32_t cl_key_nodes(const struct up_col *uc, const char *k, size_t kl, int *master);
int cl_srv_find(struct up_col *uc, const char *host, int port);
/* the server a MOVED or ASK names, among those the topology knows; -1 none (never dialled: it
 * would get the collection's credentials) */
int cl_redirect_to(struct up_col *uc, const char *host, int port);
/* the two names reach one host (a name and its address, two addresses of one host) */
int same_host(const char *a, const char *b);
void topo_save(struct up_col *uc);
void topo_load(struct up_col *uc, const char *state_dir);
const char *srv_why(const struct up_srv *s);
/* 1 every upstream whose tracking connection is up follows every
 * change (broadcast), -1 one refused it, 0 none up or not asked */
int bcast_state(const struct up_col *uc);
/* a RESP3 push read on a connection - an invalidation (tracking) */
void on_push(struct uconn *c, struct rv *v);
/* the copies fetched from @s are no longer tracked: drop them */
void flush_shadow(struct up_srv *s);

#endif
