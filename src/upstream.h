/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* upstream.h - read-through from a RESP upstream, never written: fetched values live in the
 * node-local SHADOW table, and the gate parks a command whose key it must fetch, re-run after. */
#ifndef PC_UPSTREAM_H
#define PC_UPSTREAM_H

#include <stddef.h>
#include "config.h"                    /* PC_UP_MAX_SRV */
#include "core/pcache_htable.h"

struct pc_config;
struct pc_jw;
struct pc_tw;

/* the gate's answer */
#define PC_UPG_PROCEED  0             /* run the command */
#define PC_UPG_HELD     1             /* parked: the command runs again later */
#define PC_UPG_ANSWERED 2             /* the gate wrote the reply (an error) */

/* a completion's verdict, carried to the re-run on the connection */
#define PC_UPV_NONE     0
#define PC_UPV_OK       1             /* every key was fetched (or found absent) */
#define PC_UPV_ERROR    2             /* the upstream failed: on_error decides */
#define PC_UPV_WT       3             /* a forwarded write is done - the
                                       * gate answers with Redis's reply */

/* after the store has its collections, before the workers start; @nworkers sizes the completion
 * queues (worker ids index them) */
int pc_upstream_init(const struct pc_config *cfg, int nworkers);
void pc_upstream_shutdown(void);

/* is any collection read-through?  (a cheap gate for the doors) */
int pc_upstream_any(void);

/* the worker side: register the wake fd, drain completions on its event */
void pc_upstream_worker_register(int worker, int efd);
void pc_upstream_drain(int worker);

/* the command classes the gate knows */
#define PC_UPC_NONE   0               /* never falls through */
#define PC_UPC_READ   1               /* reads: fetched on a miss */
#define PC_UPC_MUTATE 2               /* changes the value: fetched, then
                                       * PROMOTED before it runs */
#define PC_UPC_PLAIN  3               /* overwrites whatever is there */
#define PC_UPC_PROBE  4               /* EXISTS, TTL, PTTL, TYPE: TYPE + PTTL on
                                       * a miss, no value */

/* what the gate answered with, for the door to render */
struct pc_upg {
	int wrongtype;                     /* the reply is WRONGTYPE */
	const char *err;                   /* else this error */
	unsigned int req;                  /* HELD: the id the door parks under */
	/* a forwarded write's answer (err NULL): Redis's reply as it came (RESP2), its number for
	 * the native doors (OK 1, an integer, nil 0), and Redis's error text if it refused */
	int done;
	const char *raw;
	size_t rawlen;
	long long ival;
	const char *rerr;
};

/* the Redis command a write means under upstream_writes = through (the RESP door's argv, or built
 * from the native verb); @refuse non-NULL: this write is not forwarded - say that */
#define PC_UPFW_STRING 0              /* read back with GET */
#define PC_UPFW_HASH   1              /* HGETALL */
#define PC_UPFW_JSON   2              /* JSON.GET */
#define PC_UPFW_GONE   3              /* a delete: TYPE and PTTL only */
struct pc_upfw {
	int argc;
	const char *const *argv;
	const size_t *argl;
	int argc2;                         /* a second command after it (JSON.SET's
	                                    * EXPIRE: RedisJSON has no EX), 0 none */
	const char *const *argv2;
	const size_t *argl2;
	int n2;                            /* argv2 holds n2 commands of argc2 arguments
	                                    * each (a native mset's EXPIREs); 0 is one */
	int nk;
	const int *kidx;                   /* the keys among argv */
	int json;                          /* needs RedisJSON on the master */
	int guess;                         /* PC_UPFW_*: the read-back's first ask */
	int unk;                           /* RESP, not known here: Redis classes it */
	const char *refuse;
	const char *what;                  /* the client's verb, for a refusal's
	                                    * stats (NULL: argv[0]) */
	size_t whatlen;
};

/* the class of command @c (a RESP name, or an H command in a binary hcmd), given its arguments
 * (NULL: by name alone); *@del a delete or a re-arm, *@all every argument from 1 is a key */
int pc_upstream_cmd_class(const char *c, size_t cl, char *const *argv,
		const size_t *argl, int nargs, int *del, int *all);

/* the door-neutral gate over @keys in @ht's collection, @verdict the completion's (PC_UPV_*) on a
 * re-run; HELD: park under g->req, re-run on its completion; ANSWERED: g says the reply */
int pc_upstream_gate(pcache_htable_t *ht, const char *col, size_t collen,
		int cls, int del, const char *const *keys, const size_t *kl,
		int nkeys, int verdict, struct pc_upg *g);
/* with the Redis command a write means (NULL: none, refused under through): a write-through write
 * is HELD while the master applies it, then ANSWERED with Redis's reply - never run here */
int pc_upstream_gate_fw(pcache_htable_t *ht, const char *col, size_t collen,
		int cls, int del, const char *const *keys, const size_t *kl,
		int nkeys, int verdict, struct pc_upg *g, const struct pc_upfw *fw);
/* is @col write-through (upstream_writes = through)? */
int pc_upstream_through(const char *col, size_t collen);
/* 1 when the config binds @col to an upstream, 2 when it is one's .nofall companion, else 0; by
 * the configured name, so a live rename leaves it behind */
int pc_upstream_binds(const char *col, size_t collen);
/* the expiry tick of a COPY of @col's record that arrived with @ttl whole seconds left (0: none):
 * bound to an upstream, a copy must not outlive its key, so it takes ttl - 1 (a tick at least) */
unsigned int pc_upstream_copy_exp(const char *col, size_t collen,
		unsigned int ttl);
/* the owner's fetch for a peer's pull: 1 started (the answer via pc_upstream_pull_answer_take on
 * the cluster thread), 0 not a through key here - answer now */
int pc_upstream_pull_fetch(const char *col, size_t collen, const char *key,
		size_t klen, uint32_t addr, uint16_t port, uint32_t req);
int pc_upstream_pull_answer_take(uint32_t *addr, uint16_t *port, uint32_t *req,
		char *col, size_t ccap, size_t *collen, char *key, size_t kcap,
		size_t *klen);
/* a pull of @key timed out: the error to answer when its owner fetches it from the upstream
 * (the owner leaves a failed fetch unanswered) and on_error = error, else NULL */
const char *pc_upstream_pull_lost(const char *col, size_t collen,
		const char *key, size_t klen);
/* how much longer than pull_timeout_ms a pull of a read-through key
 * may wait (the owner fetches from Redis first); 0 for any other collection */
int pc_upstream_pull_extra_ms(const char *col, size_t collen);
/* a peer's forwarded op on a through key owned here: 1 queued for the master (ack via
 * pc_upstream_fwd_ack_take, cluster thread), 0 not one here (apply, ack now), -1 refused */
int pc_upstream_fwd_through(const char *col, size_t collen, int op,
		const char *key, size_t klen, const char *val, size_t vlen,
		unsigned int ttl_rel, long long delta, uint32_t addr, uint16_t port,
		uint32_t req);
int pc_upstream_fwd_ack_take(uint32_t *addr, uint16_t *port, uint32_t *req,
		int *ok, long long *newval);
/* a peer's copy of @k stored here (a through collection's record is
 * Redis's by construction) - its "t" mark is written here, passively */
void pc_upstream_note_copy(const char *col, size_t collen, const char *k,
		size_t kl, unsigned int exp, unsigned long long ver);
/* @col a ".nofall" companion and @k's record held in its collection: the mark stays */
int pc_upstream_mark_held(const char *col, size_t collen, const char *k,
		size_t kl);
/* is @k one of @col's upstream keys (upstream_prefixes)? */
int pc_upstream_allowed(const char *col, size_t collen, const char *k,
		size_t kl);
/* stats: a write refused before the gate (a native verb that is not
 * forwarded) - counted, and kept with @what (the client's verb) and why */
void pc_upstream_wt_refused(const char *col, size_t collen, const char *what,
		size_t wl, const char *msg);
/* a RESP command this node does not know, on @col: PROCEED (not write-through, or
 * upstream_unknown = refuse: "unknown command"), HELD, or ANSWERED into @out */
int pc_upstream_resp_pass(const char *col, size_t collen, char *const *argv,
		const size_t *argl, int nargs, struct pc_jw *out, void *conn);

/* the RESP gate before a keyed command's handler; @conn's verdict is read and cleared here.
 * PC_UPG_*: ANSWERED, the reply is in @out; HELD, nothing written and the connection holds */
int pc_upstream_resp_gate(pcache_htable_t *ht, const char *col, size_t collen,
		char *const *argv, const size_t *argl, int nargs,
		struct pc_jw *out, void *conn);
/* after a read the RESP gate let through: 1 a key it reads went meanwhile and is unknown here now
 * - drop the reply and gate the command again (pc_conn_upstream_retry bounds it) */
int pc_upstream_resp_lost(pcache_htable_t *ht, const char *col, size_t collen,
		char *const *argv, const size_t *argl, int nargs);
/* that lost read is answered as it stands - its re-gates are spent (counted) */
void pc_upstream_lost_spent(void);
/* tests only: PERFCACHED_TEST_GATE_DELAY_US holds a read the gate let
 * through before its handler runs */
void pc_upstream_test_pause(void);

/* the drain sweep (a privileged one-shot): every upstream key under the allow-list the fleet
 * lacks, for the workers to take; @stop stops a running one.  0 ok, -1 refused (*@msg says) */
int pc_upstream_drain_ctl(const char *col, size_t collen, int stop,
		const char **msg);
/* the upfill sweep on @col (@stop: stop it): keys owned before upstream_writes = through, written
 * to the master with NX; @check compares only.  0 started/checking/stopping, -1 refused (*@msg) */
int pc_upstream_upfill_ctl(const char *col, size_t collen, int stop,
		int check, const char **msg);

/* the collection's `upstream` block in stats (nothing for a collection
 * without one) */
void pc_upstream_stats_tree(struct pc_tw *out, const char *col, size_t collen);
/* the counters /stats shows count from here (a stats reset) */
void pc_upstream_stats_reset(void);

/* what one upstream may do now */
#define PC_UPS_SERVE    0              /* it takes fetches */
#define PC_UPS_UNKNOWN  1              /* not reached yet */
#define PC_UPS_FALLBACK 2              /* a master, serving while no replica
                                        * does (upstream_master_fallback) */
#define PC_UPS_OFF      3              /* unreachable, refused for its role,
                                        * cut off from its master, lagging */

struct pc_up_srv_health {
	const char *host;                  /* hbuf: copied under the lock */
	char hbuf[256];
	int port;
	int conns_up;
	int tracking_on;
	int state;                         /* PC_UPS_* */
	int role;                          /* 0 not asked, 1 master, 2 replica,
	                                    * 3 other */
	int link_down;
	long long lag_ms;                  /* -1 not known */
	int perfcached;                    /* refused as a perfcached */
	int chain_hop;                     /* a perfcached, allowed */
	int quiet;                         /* off by design - a sentinel seed, gone */
};

struct pc_up_health {
	const char *col;                   /* the collection's name */
	const char *host;                  /* the first upstream's */
	int port;
	int conns_up;                      /* fetch connections up now, all */
	int tracking_wanted, tracking_on;  /* asked for; and running on every
	                                    * upstream in use */
	int nsrv;                          /* its upstreams */
	int role_replica;                  /* upstream_role = replica */
	int serving;                       /* how many take fetches */
	const struct pc_up_srv_health *fallback;   /* the master serving, or NULL */
	int failover, primary, active;     /* mode; indexes, -1 */
	unsigned long long chained;        /* reads by another perfcached's
	                                    * read-through, never fetched onward */
	/* write-through: whether, the writer's state (0 a target, 1 starting, 2 no master, 3 two, 4
	 * master silent), the target (-1), RedisJSON (-1, 0, 1), writes forwarded / passed */
	int through, w_state, w_target, w_json;
	unsigned long long w_forwarded, w_passed;
	unsigned long long w_qfull, w_qexpired;      /* refused at a full
	                                              * queue / timed out in it */
	int w_bcast;                                 /* 1 broadcast tracking
	                                              * on, 0 off, -1 refused */
	struct pc_up_srv_health srv[PC_UP_MAX_SRV];
};
int pc_upstream_count(void);
/* a reload's - every rediss:// collection's files read again, staged: how many, or -1
 * (*err says which, nothing staged); then applied (1) or dropped (0) */
int pc_upstream_tls_stage(char *err, size_t el);
void pc_upstream_tls_commit(int apply);
/* another perfcached's read-through fetcher read @col (a read-through
 * collection here): counted - its misses are not fetched onward */
void pc_upstream_chain_note(const char *col, size_t collen);
/* a read-through upstream's health, for the alerts: @i in
 * 0..pc_upstream_count()-1.  0 ok, -1 out of range. */
int pc_upstream_health(int i, struct pc_up_health *h);

/* upstream_writes = refuse - the message for a refused write on
 * @col (counted), NULL when it takes writes; and the plain predicate */
const char *pc_upstream_write_refusal(const char *col, size_t collen);
int pc_upstream_refuses_writes(const char *col, size_t collen);

#endif /* PC_UPSTREAM_H */
