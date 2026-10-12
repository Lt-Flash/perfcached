/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * upstream.c - read-through from a RESP upstream (task S331, DESIGN 12ix).
 *
 * One-way: this file speaks RESP to the upstream and sends it READ
 * commands only - TYPE, PTTL, GET, HGETALL, JSON.GET, plus the handshake
 * (HELLO/AUTH/SELECT).  Nothing here writes upstream, and the suites
 * enforce that with an upstream that fails the run on any write command.
 * No Redis code: RESP is a published protocol, the reader is written
 * from it.
 *
 * Shape.  Each upstream collection has a shadow table (node-local) and
 * `upstream_conns` threads, each owning one connection.  A worker whose
 * gate finds a key it must fetch registers a REQUEST (one per command,
 * counting its keys) and joins the key's single-flight entry - one fetch
 * per key however many commands wait for it.  A connection thread takes
 * the key off the queue, fetches it, stores the value (or a marker) in
 * the shadow and counts the request down; the last key posts the
 * completion to the request's worker, whose drain hands it to proto -
 * which lets the held connection run the command again.
 */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <time.h>
#include <unistd.h>

#include "compat/compat.h"
#include "compat/dprint.h"
#include "compat/mem/mem.h"
#include "compat/str.h"
#include "compat/timer.h"
#include "core/pcache_htable.h"
#include "cluster.h"
#include "config.h"
#include "daemon.h"
#include "hashrec.h"
#include "json.h"
#include "ptree.h"
#include "proto.h"
#include "store.h"
#include "upstream.h"
#include "winctr_tw.h"     /* S351 */
#include "verbs.h"

#define UP_MAX_PREFIXES 32
#define UP_MAX_COLS     8
#define UP_RBUF_MAX     (1u << 20)    /* the largest reply read (a value
                                       * over the cell ceiling is refused
                                       * before it is read whole) */

/* ---- the request and single-flight bookkeeping ------------------------ */

struct up_req {                        /* one parked command */
	unsigned int id;
	int worker;
	int left;                          /* keys still being fetched */
	int status;                        /* PC_UPV_* so far */
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
	size_t klen;
	char key[];
};

/* ---- a completion on its way to a worker ------------------------------ */

#define UPD_REQ  0                     /* a request is done: run it again */
#define UPD_OWN  1                     /* an ownership check is done */
#define UPD_PULL 2                     /* the drain sweep read a key */

struct up_done {
	unsigned int id;
	int status;
	int kind;                          /* UPD_* */
	int col;                           /* UPD_OWN: the collection */
	char *key;                         /* UPD_OWN/PULL: malloc'd */
	size_t klen;
	char *val;                         /* UPD_PULL: the value, malloc'd */
	size_t vlen;
	unsigned int exp;                  /* UPD_PULL: absolute, 0 = never */
	unsigned char fl;                  /* UPD_PULL: the TYPE bits */
};

struct up_wq {
	pthread_mutex_t mx;
	struct up_done *q;
	int n, cap;
	int efd;
};

static struct up_wq *wq;
static int wq_n;

/* ---- one upstream of a collection (S382) --------------------------------- */

#define UPR_UNKNOWN 0                  /* not asked yet (or no ROLE here) */
#define UPR_MASTER  1
#define UPR_REPLICA 2
#define UPR_OTHER   3                  /* a sentinel, or a reply not known */

struct up_col;

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
	long long inv_id;                  /* 0: not up */
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
	int refuse;                        /* S348: upstream_writes = refuse */
	char refuse_msg[384];
	size_t nflen;
	int idx;                           /* in cols[] */
	struct up_srv srv[PC_UP_MAX_SRV];  /* S382: one or more */
	int nsrv;
	int role_req;                      /* S382: PC_UP_ROLE_* */
	int master_fallback;               /* S382: a master while no replica */
	int max_lag_ms;                    /* S382: 0 = not checked */
	int allow_pc;                      /* S384: upstream_allow_perfcached */
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
	long long drain_t0, drain_t1;      /* unix seconds */
	int drain_inflight;                /* posted, not yet applied */

	/* stats - the collection's, every upstream's together */
	unsigned long long held, fetches, fill_string, fill_hash, fill_json,
		absent, wrongtype, toobig, errors, timeouts, connects,
		shadow_hits, neg_hits, joined, fill_failed, lat_us_sum, lat_n,
		promoted, nofall_written, nofall_hits, own_checks, own_found,
		writes_refused,
		fill_present, present_hits, refetches,          /* ST2 */
		invalidations, flushes, untracked,
		dr_scanned, dr_pulled, dr_owned, dr_absent, dr_wrongtype,
		dr_toobig, dr_errors;
	/* S358: each counter's value at the last stats reset - /stats
	 * subtracts it.  Written by the (serialised) reset, read fuzzily; the
	 * drain-run figures dr_* describe a run, and are not reset. */
	struct {
		unsigned long long held, fetches, fill_string, fill_hash, fill_json, absent,
			wrongtype, toobig, errors, timeouts, connects, shadow_hits,
			neg_hits, joined, fill_failed, lat_us_sum, lat_n, promoted,
			nofall_written, nofall_hits, own_checks, own_found, writes_refused, invalidations,
			flushes, untracked, fill_present, present_hits, refetches;
	} base;
};

#define UPDR_IDLE    0
#define UPDR_RUNNING 1
#define UPDR_DONE    2
#define UPDR_STOPPED 3
#define UPDR_FAILED  4
static const char *const updr_name[] = { "idle", "running", "done",
	"stopped", "failed" };

static struct up_col cols[UP_MAX_COLS];
static int ncols;
static unsigned int next_id = 1;

#define ST_ADD(f, v) __atomic_add_fetch(&(f), (v), __ATOMIC_RELAXED)
#define ST_GET(f) __atomic_load_n(&(f), __ATOMIC_RELAXED)
/* S358: a counter as /stats shows it - since the last stats reset */
#define UPV(uc, f) (ST_GET((uc)->f) - (uc)->base.f)

int pc_upstream_any(void)
{
	return ncols > 0;
}

static struct up_col *col_of(const char *col, size_t collen);

/* S384: another perfcached's read-through fetcher read @col here, and
 * @col reads through itself - answered from what this node holds, never
 * fetched onward (a daisy-chain cut at one hop); counted for the alert */
void pc_upstream_chain_note(const char *col, size_t collen)
{
	struct up_col *uc = col_of(col, collen);

	if (uc)
		ST_ADD(uc->chained, 1);
}

int pc_upstream_count(void)
{
	return ncols;
}

static int srv_state(const struct up_srv *s);

int pc_upstream_health(int i, struct pc_up_health *h)
{
	struct up_col *uc;
	int j;

	if (i < 0 || i >= ncols)
		return -1;
	uc = &cols[i];
	memset(h, 0, sizeof *h);
	h->col = uc->name;
	h->host = uc->srv[0].host;
	h->port = uc->srv[0].port;
	h->tracking_wanted = uc->tracking;
	h->nsrv = uc->nsrv;
	h->role_replica = uc->role_req == PC_UP_ROLE_REPLICA;
	h->chained = ST_GET(uc->chained);                  /* S384 */
	pthread_mutex_lock(&uc->mx);                       /* S382 part 2 */
	h->failover = uc->mode == PC_UP_MODE_FAILOVER;
	h->primary = uc->primary;
	h->active = uc->active;
	pthread_mutex_unlock(&uc->mx);
	for (j = 0; j < uc->nsrv; j++) {
		const struct up_srv *s = &uc->srv[j];
		struct pc_up_srv_health *sh = &h->srv[j];

		sh->host = s->host;
		sh->port = s->port;
		sh->conns_up = __atomic_load_n(&s->conns_up, __ATOMIC_RELAXED);
		sh->tracking_on = __atomic_load_n(&s->inv_id, __ATOMIC_ACQUIRE) > 0;
		sh->state = srv_state(s);
		sh->role = __atomic_load_n(&s->role, __ATOMIC_RELAXED);
		sh->link_down = __atomic_load_n(&s->link_down, __ATOMIC_RELAXED);
		sh->lag_ms = __atomic_load_n(&s->lag_ms, __ATOMIC_RELAXED);
		sh->perfcached = __atomic_load_n(&s->is_pc, __ATOMIC_RELAXED) &&
			!uc->allow_pc;                         /* S384: refused */
		sh->chain_hop = __atomic_load_n(&s->is_pc, __ATOMIC_RELAXED) &&
			uc->allow_pc;                          /* S384: allowed */
		h->conns_up += sh->conns_up;
		if (sh->state == PC_UPS_SERVE)
			h->serving++;
		else if (sh->state == PC_UPS_FALLBACK && !h->fallback)
			h->fallback = sh;
	}
	if (h->serving)
		h->fallback = NULL;            /* a replica serves: no fallback */
	h->tracking_on = 1;
	for (j = 0; j < uc->nsrv; j++)
		if (!h->srv[j].tracking_on && (uc->nsrv == 1 ||
		    h->srv[j].state != PC_UPS_OFF))
			h->tracking_on = 0;
	return 0;
}

static struct up_col *col_of(const char *col, size_t collen)
{
	int i;

	for (i = 0; i < ncols; i++)
		if (cols[i].nlen == collen && !memcmp(cols[i].name, col, collen))
			return &cols[i];
	return NULL;
}

static int allowed(const struct up_col *uc, const char *k, size_t kl)
{
	int i;

	if (kl > INT_MAX)
		return 0;
	for (i = 0; i < uc->npat; i++)
		if (pc_glob_match(uc->pat[i], uc->patlen[i], k, (int)kl))
			return 1;
	return 0;
}

/* ---- the worker side --------------------------------------------------- */

void pc_upstream_worker_register(int worker, int efd)
{
	if (worker >= 0 && worker < wq_n)
		wq[worker].efd = efd;
}

static void post_done(int worker, const struct up_done *d)
{
	struct up_wq *w;
	uint64_t one = 1;

	if (worker < 0 || worker >= wq_n) {
		free(d->key);
		free(d->val);
		return;
	}
	w = &wq[worker];
	pthread_mutex_lock(&w->mx);
	if (w->n == w->cap) {
		int nc = w->cap ? w->cap * 2 : 64;
		struct up_done *nq = realloc(w->q, (size_t)nc * sizeof *nq);

		if (!nq) {
			/* the request can never complete: its connection
			 * stays held until it closes - logged, and counted
			 * by the caller as an error */
			pthread_mutex_unlock(&w->mx);
			LM_ERR("upstream: no memory to post a completion - a "
				"client connection stays held\n");
			free(d->key);
			free(d->val);
			return;
		}
		w->q = nq;
		w->cap = nc;
	}
	w->q[w->n++] = *d;
	pthread_mutex_unlock(&w->mx);
	if (w->efd >= 0 && write(w->efd, &one, sizeof one) < 0 &&
	        errno != EAGAIN)
		LM_ERR("upstream: cannot wake worker %d: %s\n", worker,
			strerror(errno));
}

static void post(int worker, unsigned int id, int status)
{
	struct up_done d;

	memset(&d, 0, sizeof d);
	d.id = id;
	d.status = status;
	d.kind = UPD_REQ;
	post_done(worker, &d);
}

static void own_apply(struct up_col *uc, const char *k, size_t kl);
static void pull_apply(struct up_col *uc, const struct up_done *d);

void pc_upstream_drain(int worker)
{
	struct up_wq *w;
	struct up_done buf[32];
	int n, i;

	if (worker < 0 || worker >= wq_n)
		return;
	w = &wq[worker];
	for (;;) {
		pthread_mutex_lock(&w->mx);
		/* the wake fd fires for the cluster and pub/sub too: most
		 * drains find nothing - and a queue never posted to has no
		 * buffer (memcpy from NULL, even of nothing, is undefined) */
		if (!w->n) {
			pthread_mutex_unlock(&w->mx);
			return;
		}
		n = w->n < 32 ? w->n : 32;
		memcpy(buf, w->q, (size_t)n * sizeof *buf);
		memmove(w->q, w->q + n, (size_t)(w->n - n) * sizeof *buf);
		w->n -= n;
		pthread_mutex_unlock(&w->mx);
		if (!n)
			return;
		for (i = 0; i < n; i++) {
			if (buf[i].kind == UPD_PULL) {
				if (buf[i].col >= 0 && buf[i].col < ncols) {
					pull_apply(&cols[buf[i].col], &buf[i]);
					__atomic_sub_fetch(&cols[buf[i].col].drain_inflight,
						1, __ATOMIC_RELAXED);
				}
				free(buf[i].key);
				free(buf[i].val);
				continue;
			}
			if (buf[i].kind == UPD_OWN) {
				if (buf[i].col >= 0 && buf[i].col < ncols)
					own_apply(&cols[buf[i].col], buf[i].key,
						buf[i].klen);
				free(buf[i].key);
				continue;
			}
			pc_proto_upstream_complete(buf[i].id, buf[i].status);
		}
	}
}

/* ---- the gate ---------------------------------------------------------- */

/* command classes (upstream.h: PC_UPC_*) */
#define UPC_NONE   PC_UPC_NONE
#define UPC_READ   PC_UPC_READ
#define UPC_MUTATE PC_UPC_MUTATE
#define UPC_PLAIN  PC_UPC_PLAIN
#define UPC_PROBE  PC_UPC_PROBE

/* which arguments are keys */
#define UPK_FIRST  0                   /* argv[1] */
#define UPK_ALL    1                   /* argv[1..] */

struct up_cmd {
	const char *name;
	int cls;
	int keys;
};

static const struct up_cmd up_cmds[] = {
	{ "GET", UPC_READ, UPK_FIRST }, { "MGET", UPC_READ, UPK_ALL },
	{ "EXISTS", UPC_PROBE, UPK_ALL }, { "TYPE", UPC_PROBE, UPK_FIRST },
	{ "TTL", UPC_PROBE, UPK_FIRST }, { "PTTL", UPC_PROBE, UPK_FIRST },
	{ "HGET", UPC_READ, UPK_FIRST }, { "HMGET", UPC_READ, UPK_FIRST },
	{ "HEXISTS", UPC_READ, UPK_FIRST }, { "HLEN", UPC_READ, UPK_FIRST },
	{ "HSTRLEN", UPC_READ, UPK_FIRST }, { "HKEYS", UPC_READ, UPK_FIRST },
	{ "HVALS", UPC_READ, UPK_FIRST }, { "HGETALL", UPC_READ, UPK_FIRST },
	{ "HRANDFIELD", UPC_READ, UPK_FIRST }, { "HSCAN", UPC_READ, UPK_FIRST },
	{ "JSON.GET", UPC_READ, UPK_FIRST },
	{ "SET", UPC_PLAIN, UPK_FIRST },      /* refined by its options */
	{ "SETEX", UPC_PLAIN, UPK_FIRST }, { "PSETEX", UPC_PLAIN, UPK_FIRST },
	{ "JSON.SET", UPC_PLAIN, UPK_FIRST }, /* refined by path and NX/XX */
	{ "DEL", UPC_MUTATE, UPK_ALL }, { "UNLINK", UPC_MUTATE, UPK_ALL },
	{ "DELEX", UPC_MUTATE, UPK_FIRST },
	{ "EXPIRE", UPC_MUTATE, UPK_FIRST }, { "PEXPIRE", UPC_MUTATE, UPK_FIRST },
	{ "EXPIREAT", UPC_MUTATE, UPK_FIRST },
	{ "PEXPIREAT", UPC_MUTATE, UPK_FIRST },
	{ "INCR", UPC_MUTATE, UPK_FIRST }, { "DECR", UPC_MUTATE, UPK_FIRST },
	{ "INCRBY", UPC_MUTATE, UPK_FIRST }, { "DECRBY", UPC_MUTATE, UPK_FIRST },
	{ "HSET", UPC_MUTATE, UPK_FIRST }, { "HSETNX", UPC_MUTATE, UPK_FIRST },
	{ "HMSET", UPC_MUTATE, UPK_FIRST }, { "HDEL", UPC_MUTATE, UPK_FIRST },
	{ "HINCRBY", UPC_MUTATE, UPK_FIRST },
	{ "HINCRBYFLOAT", UPC_MUTATE, UPK_FIRST },
	{ "JSON.DEL", UPC_MUTATE, UPK_FIRST },
	{ "JSON.NUMINCRBY", UPC_MUTATE, UPK_FIRST },
	{ "JSON.ARRAPPEND", UPC_MUTATE, UPK_FIRST },
};

static int name_is(const char *a, size_t al, const char *b)
{
	size_t bl = strlen(b);

	return al == bl && !strncasecmp(a, b, al);
}

static const struct up_cmd *cmd_of(const char *c, size_t cl)
{
	size_t i;

	for (i = 0; i < sizeof up_cmds / sizeof up_cmds[0]; i++)
		if (name_is(c, cl, up_cmds[i].name))
			return &up_cmds[i];
	return NULL;
}

/* SET k v [options]: plain unless it carries a condition or reads */
static int set_class(char *const *argv, const size_t *argl, int nargs)
{
	int i;

	for (i = 3; i < nargs; i++)
		if (name_is(argv[i], argl[i], "NX") ||
		    name_is(argv[i], argl[i], "XX") ||
		    name_is(argv[i], argl[i], "GET") ||
		    name_is(argv[i], argl[i], "KEEPTTL") ||
		    name_is(argv[i], argl[i], "IFEQ") ||
		    name_is(argv[i], argl[i], "IFNE"))
			return UPC_MUTATE;
	return UPC_PLAIN;
}

/* JSON.SET k path v [NX|XX]: plain at the root without a condition */
static int jset_class(char *const *argv, const size_t *argl, int nargs)
{
	int i;

	if (nargs < 3 || !(name_is(argv[2], argl[2], "$") ||
	                   name_is(argv[2], argl[2], ".")))
		return UPC_MUTATE;
	for (i = 4; i < nargs; i++)
		if (name_is(argv[i], argl[i], "NX") ||
		    name_is(argv[i], argl[i], "XX"))
			return UPC_MUTATE;
	return UPC_PLAIN;
}

/* join @k's single-flight entry (or start one and queue it); called with
 * uc->mx held.  @r the request waiting on it (NULL: none - a background
 * ownership check); @own: the fetch also decides whether this fleet now
 * owns the key (own_apply, on worker @own_worker). */
/* ---- S382: the upstreams' shadow chain, and which upstream fetches --- */

/* the first copy or marker of @k in any upstream's table: 0 found */
static int sh_peek(struct up_col *uc, const str *k, unsigned char *fl,
		unsigned char *mark, unsigned int *exp)
{
	int j;

	for (j = 0; j < uc->nsrv; j++)
		if (pcache_ht_peek(uc->srv[j].shadow, k, fl, mark, exp) == 0)
			return 0;
	return -2;
}

/* @k changed or is the fleet's: no upstream's copy may answer */
static void sh_remove(struct up_col *uc, const str *k)
{
	int j;

	for (j = 0; j < uc->nsrv; j++)
		pcache_ht_remove(uc->srv[j].shadow, k);
}

/* what an upstream may do now - PC_UPS_*.  SERVE: it takes fetches.
 * UNKNOWN: not connected yet, so not known - it may, its first connect
 * checks.  FALLBACK: a master that upstream_master_fallback lets serve
 * while no replica does.  OFF: it cannot reach it, or it is a master (or
 * says nothing) where a replica is asked for, or its master link is down,
 * or it lags more than upstream_max_lag_ms */
static int srv_state(const struct up_srv *s)
{
	const struct up_col *uc = s->uc;
	int role = __atomic_load_n(&s->role, __ATOMIC_RELAXED);
	long long lag = __atomic_load_n(&s->lag_ms, __ATOMIC_RELAXED);

	if (__atomic_load_n(&s->failing, __ATOMIC_RELAXED))
		return PC_UPS_OFF;
	/* S384: a ring of read-throughs (perfcached <-> perfcached, a fleet
	 * reading its own VIP) is a fetch storm - refused unless allowed,
	 * and then one hop at most (the fetcher's name, pc_conn_rt_fetcher) */
	if (__atomic_load_n(&s->is_pc, __ATOMIC_RELAXED) && !uc->allow_pc)
		return PC_UPS_OFF;
	if (uc->role_req == PC_UP_ROLE_REPLICA) {
		if (role == UPR_UNKNOWN && !__atomic_load_n(&s->checked_ms,
		        __ATOMIC_RELAXED))
			return PC_UPS_UNKNOWN;
		if (role == UPR_MASTER)
			return uc->master_fallback ? PC_UPS_FALLBACK : PC_UPS_OFF;
		if (role != UPR_REPLICA)
			return PC_UPS_OFF;
	} else if (!__atomic_load_n(&s->checked_ms, __ATOMIC_RELAXED))
		return PC_UPS_UNKNOWN;
	if (role == UPR_REPLICA) {
		if (__atomic_load_n(&s->link_down, __ATOMIC_RELAXED))
			return PC_UPS_OFF;
		if (uc->max_lag_ms > 0 && lag > uc->max_lag_ms)
			return PC_UPS_OFF;
	}
	return PC_UPS_SERVE;
}

static long long now_ms(void);
static const char *srv_why(const struct up_srv *s);

static long long wall_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* why an upstream is not used (PC_UPS_OFF), "" when it is - /stats, the
 * page and the failover log line */
static const char *srv_why(const struct up_srv *s)
{
	int ro = __atomic_load_n(&s->role, __ATOMIC_RELAXED);

	if (srv_state(s) != PC_UPS_OFF)
		return "";
	if (__atomic_load_n(&s->failing, __ATOMIC_RELAXED))
		return "unreachable";
	if (__atomic_load_n(&s->is_pc, __ATOMIC_RELAXED) && !s->uc->allow_pc)
		return "a perfcached: daisy-chaining off";       /* S384 */
	if (ro == UPR_MASTER)
		return "a master";
	if (ro != UPR_REPLICA)
		return "not a replica";
	if (__atomic_load_n(&s->link_down, __ATOMIC_RELAXED))
		return "its master link is down";
	return "lagging";
}

/* under uc->mx: S382 part 2's order - the primary, then the list - and,
 * by tier, the first that serves, else one not reached yet, else a master
 * the fallback allows; @not left out (a retry); -1 none */
static int fo_first(struct up_col *uc, const struct up_srv *not)
{
	static const int tier[3] = { PC_UPS_SERVE, PC_UPS_UNKNOWN,
		PC_UPS_FALLBACK };
	int t, j;

	for (t = 0; t < 3; t++)
		for (j = -1; j < uc->nsrv; j++) {
			int i = j < 0 ? uc->primary : j;

			if (j >= 0 && i == uc->primary)
				continue;
			if (&uc->srv[i] != not && srv_state(&uc->srv[i]) == tier[t])
				return i;
		}
	return -1;
}

/* under uc->mx: @to becomes the active upstream */
static void fo_switch(struct up_col *uc, int to)
{
	int from = uc->active;

	uc->active = to;
	uc->active_since = wall_ms();
	if (from < 0)
		return;
	if (from == uc->primary && to != uc->primary)
		uc->failovers++;
	if (to == uc->primary)
		LM_NOTICE("collection '%s': primary %s is back - it takes over "
			"from %s (serving again for %lld s)\n", uc->name,
			uc->srv[to].addr, uc->srv[from].addr, uc->hold_ms / 1000);
	else
		LM_WARN("collection '%s': failed over from %s to %s - %s is %s\n",
			uc->name, uc->srv[from].addr, uc->srv[to].addr,
			uc->srv[from].addr, srv_why(&uc->srv[from]));
}

/* under uc->mx: failover's decision, on every pick and once a second.
 * The active one stays while it serves (or is not reached yet - at the
 * start the primary gets its first connect, not a switch away); when it
 * stops, or is the master while a replica serves again, the first in the
 * order takes over.  With upstream_preempt a primary that has served for
 * upstream_preempt_hold_s takes over again - not sooner, so a primary that
 * flaps does not drag the fetches back and forth */
static void fo_eval(struct up_col *uc)
{
	const struct up_srv *p = &uc->srv[uc->primary];
	long long now = now_ms();
	int a = uc->active, st, best;

	if (srv_state(p) == PC_UPS_SERVE) {
		if (!uc->pri_ok_since)
			uc->pri_ok_since = now;
	} else
		uc->pri_ok_since = 0;
	if (a >= 0 && a != uc->primary && uc->preempt && uc->pri_ok_since &&
	    now - uc->pri_ok_since >= uc->hold_ms) {
		fo_switch(uc, uc->primary);
		return;
	}
	best = fo_first(uc, NULL);
	if (a < 0) {
		if (best >= 0)
			fo_switch(uc, best);
		return;
	}
	st = srv_state(&uc->srv[a]);
	if (st == PC_UPS_SERVE || st == PC_UPS_UNKNOWN)
		return;
	if (st == PC_UPS_FALLBACK && (best < 0 ||
	    srv_state(&uc->srv[best]) != PC_UPS_SERVE))
		return;                        /* the master, while nothing better */
	if (best >= 0 && best != a)
		fo_switch(uc, best);
}

/* under uc->mx: the upstream a fetch goes to - of those that serve, the
 * one with the fewest fetches queued or running (a slow one gets fewer,
 * unlike round-robin); else one not known yet; else a master the fallback
 * allows; else NULL.  @not: one to leave out (a retry), or NULL */
static struct up_srv *pick(struct up_col *uc, const struct up_srv *not)
{
	static const int order[3] = { PC_UPS_SERVE, PC_UPS_UNKNOWN,
		PC_UPS_FALLBACK };
	int o, j;

	if (uc->mode == PC_UP_MODE_FAILOVER) {         /* S382 part 2 */
		int i;

		fo_eval(uc);
		if (!not)
			return uc->active >= 0 ? &uc->srv[uc->active] : NULL;
		i = fo_first(uc, not);         /* a retry: the next in order */
		return i >= 0 ? &uc->srv[i] : NULL;
	}

	for (o = 0; o < 3; o++) {
		struct up_srv *best = NULL;

		for (j = 0; j < uc->nsrv; j++) {
			struct up_srv *s = &uc->srv[(uc->rr + j) % uc->nsrv];

			if (s == not || srv_state(s) != order[o])
				continue;
			if (!best || s->queued + s->busy < best->queued + best->busy)
				best = s;
		}
		if (best) {
			uc->rr = (best->idx + 1) % uc->nsrv;
			return best;
		}
	}
	return NULL;
}

/* under uc->mx: @e onto @s's queue */
static void enqueue(struct up_srv *s, struct up_key *e)
{
	e->qnext = NULL;
	if (s->qtail)
		s->qtail->qnext = e;
	else
		s->qhead = e;
	s->qtail = e;
	s->queued++;
	pthread_cond_signal(&s->cv);
}

static int join_key(struct up_col *uc, const char *k, size_t kl,
		struct up_req *r, int own, int own_worker, int full)
{
	struct up_key *e;
	struct up_wait *w = NULL;

	if (r && !(w = malloc(sizeof *w)))
		return -1;
	for (e = uc->inflight; e; e = e->next)
		if (e->klen == kl && !memcmp(e->key, k, kl))
			break;
	if (e) {
		ST_ADD(uc->joined, 1);
	} else {
		e = malloc(sizeof *e + kl);
		if (!e) {
			free(w);
			return -1;
		}
		e->klen = kl;
		memcpy(e->key, k, kl);
		e->waiters = NULL;
		e->own = 0;
		e->own_worker = -1;
		e->full = 0;
		e->fetched = 0;
		e->tried = 0;
		e->next = uc->inflight;
		uc->inflight = e;
		{
			/* S382: none serving - the first takes it, and fails it
			 * fast as a single dead upstream does */
			struct up_srv *s = pick(uc, NULL);

			enqueue(s ? s : &uc->srv[0], e);
		}
	}
	if (own && !e->own) {
		e->own = 1;
		e->own_worker = own_worker;
	}
	if (full)
		e->full = 1;
	if (r) {
		w->r = r;
		w->next = e->waiters;
		e->waiters = w;
		r->left++;
	}
	return 0;
}

/* ---- ownership: the companion's no-fall entries ------------------------ */

/* has this fleet written or deleted @k (a no-fall entry)?  Read on a
 * worker, in its turn - the companion's table is resolved by name each
 * time, so a resize of it is never a stale pointer */
static int nofall_has(struct up_col *uc, const str *k)
{
	pcache_htable_t *nf = pc_store_find(uc->nf, uc->nflen);

	return nf && pcache_ht_probe_main(nf, k, NULL) == 0;
}

/* the shadow's gate (pcache_ht_set_shadow_gate): a fetched copy answers
 * only while the fleet has not taken the key over */
static int shadow_ok(void *ctx, const str *k)
{
	return !nofall_has(ctx, k);
}

/* record that this fleet owns @k from here on, until the upstream's copy
 * would expire (@exp absolute ticks, 0 = it has none); the shadow's copy
 * of it goes.  A worker only: it is a write, logged and replicated. */
static int nofall_put(struct up_col *uc, const str *k, unsigned int exp)
{
	pcache_htable_t *nf = pc_store_find(uc->nf, uc->nflen);

	sh_remove(uc, k);
	if (!nf)
		return -1;
	if (pc_verb_store_local(nf, k, "1", 1, exp, 0) != 0)
		return -1;
	ST_ADD(uc->nofall_written, 1);
	return 0;
}

/* PROMOTE: the shadow's copy of @k becomes a record of the collection - a
 * real write, logged and replicated - before a command changes it, and
 * the key is the fleet's from here (a no-fall entry as long as the
 * upstream's copy lives).  0 done, -1 not (the copy went meanwhile, or
 * no memory). */
static int promote(struct up_col *uc, pcache_htable_t *ht, const str *k)
{
	str v;
	unsigned int exp = 0;
	unsigned char fl = 0;
	int rc, j;

	v.s = NULL;
	v.len = 0;
	for (j = 0; j < uc->nsrv; j++)                 /* S382: the chain */
		if (pcache_ht_fetch_full(uc->srv[j].shadow, k, &v, &exp, &fl,
		        NULL) == 0)
			break;
	if (j == uc->nsrv)
		return -1;
	if (fl & PCACHE_F_UPMARK) {
		pkg_free(v.s);
		return -1;
	}
	rc = pc_verb_store_local(ht, k, v.s, (size_t)v.len, exp, fl);
	pkg_free(v.s);
	if (rc != 0)
		return -1;
	ST_ADD(uc->promoted, 1);
	nofall_put(uc, k, exp);
	return 0;
}

/* an ownership check is back (a worker, before the request it rode with
 * runs again): if the upstream has the key, the fleet owns it now */
static void own_apply(struct up_col *uc, const char *kp, size_t kl)
{
	unsigned char fl = 0, mark = 0;
	unsigned int exp = 0;
	str k;

	k.s = (char *)kp;
	k.len = (int)kl;
	if (sh_peek(uc, &k, &fl, &mark, &exp) != 0)
		return;                        /* nothing came back (an error) */
	if ((fl & PCACHE_F_UPMARK) && mark == PCACHE_UPMARK_ABSENT)
		return;                        /* not upstream: nothing to own */
	ST_ADD(uc->own_found, 1);
	nofall_put(uc, &k, (fl & PCACHE_F_UPMARK) ? 0 : exp);
}

/* the drain sweep read a key (a worker): the fleet takes it over - a real
 * write and a no-fall entry, as a promotion makes - unless the fleet
 * already holds it, wrote it, or deleted it.  The fleet's version wins. */
static void pull_apply(struct up_col *uc, const struct up_done *d)
{
	pcache_htable_t *ht = pc_store_find(uc->name, uc->nlen);
	str k;

	k.s = d->key;
	k.len = (int)d->klen;
	if (!ht)
		return;
	if (d->exp && d->exp <= get_ticks()) {
		ST_ADD(uc->dr_absent, 1);      /* expired on the way */
		return;
	}
	if (pcache_ht_probe_main(ht, &k, NULL) == 0 || nofall_has(uc, &k)) {
		ST_ADD(uc->dr_owned, 1);
		return;
	}
	if (pc_verb_store_local(ht, &k, d->val, d->vlen, d->exp, d->fl) != 0) {
		ST_ADD(uc->dr_errors, 1);
		return;
	}
	nofall_put(uc, &k, d->exp);
	ST_ADD(uc->dr_pulled, 1);
}

/* deletes and expiries: on a LOCAL key whose ownership was never decided,
 * the upstream is asked first - otherwise the key would read through to
 * the upstream's copy the moment it is gone here */
static int deletes_key(const char *c, size_t cl)
{
	return name_is(c, cl, "DEL") || name_is(c, cl, "UNLINK") ||
		name_is(c, cl, "DELEX") || name_is(c, cl, "HDEL") ||
		name_is(c, cl, "JSON.DEL") || name_is(c, cl, "EXPIRE") ||
		name_is(c, cl, "PEXPIRE") || name_is(c, cl, "EXPIREAT") ||
		name_is(c, cl, "PEXPIREAT");
}

static void wrongtype(struct pc_jw *out)
{
	pc_jw_lit(out, "-");
	pc_jw_raw(out, pc_wrongtype_msg, strlen(pc_wrongtype_msg));
	pc_jw_lit(out, "\r\n");
}

/* the class of a command named @c (RESP, or an H command inside a binary
 * hcmd) with its arguments; *@del: it deletes or re-arms the key; *@all:
 * every argument from 1 is a key.  UPC_NONE: never falls through. */
int pc_upstream_cmd_class(const char *c, size_t cl, char *const *argv,
		const size_t *argl, int nargs, int *del, int *all)
{
	const struct up_cmd *cm = cmd_of(c, cl);
	int cls;

	*del = 0;
	*all = 0;
	if (!cm)
		return UPC_NONE;
	cls = cm->cls;
	if (argv && name_is(c, cl, "SET"))
		cls = set_class(argv, argl, nargs);
	else if (argv && name_is(c, cl, "JSON.SET"))
		cls = jset_class(argv, argl, nargs);
	*del = deletes_key(c, cl);
	*all = cm->keys == UPK_ALL;
	return cls;
}

int pc_upstream_gate(pcache_htable_t *ht, const char *col, size_t collen,
		int cls, int del, const char *const *keys, const size_t *kl,
		int nkeys, int verdict, struct pc_upg *g)
{
	struct up_col *uc;
	struct up_req *r;
	int i, nj = 0, nbg = 0, need = 0;
	/* pass 1 decides (and does the local writes - promotion, no-fall -
	 * with no lock held); pass 2 joins what must be fetched under ONE
	 * hold of the lock.  The door parks the request after this returns:
	 * a completion is drained on this same worker, which is busy until
	 * the door is done, so it cannot arrive first. */
	int *jk = NULL, *jown = NULL, *bg = NULL;

	memset(g, 0, sizeof *g);
	if (!ncols || cls == UPC_NONE || nkeys < 1)
		return PC_UPG_PROCEED;
	uc = col_of(col, collen);
	if (!uc)
		return PC_UPG_PROCEED;
	/* S348: a collection that only mirrors its upstream refuses every
	 * write here, before the gate's own local writes (promotion, no-fall)
	 * - a write that landed would make the key this fleet's and Redis'
	 * later changes to it would never reach the readers */
	if (uc->refuse && (cls == UPC_MUTATE || cls == UPC_PLAIN || del)) {
		ST_ADD(uc->writes_refused, 1);
		g->err = uc->refuse_msg;
		return PC_UPG_ANSWERED;
	}
	for (i = 0; i < nkeys; i++) {
		str k;
		unsigned char fl = 0, mark = 0;
		unsigned int exp = 0;
		int own = 0, sh;

		k.s = (char *)keys[i];
		k.len = (int)kl[i];
		if (!allowed(uc, keys[i], kl[i]))
			continue;
		if (pcache_ht_probe_main(ht, &k, NULL) == 0) {
			/* a local key: it answers.  Only a delete or an expiry
			 * of one whose ownership was never decided asks the
			 * upstream first (and records a no-fall entry if the
			 * upstream has it) - else the upstream's copy would
			 * read through the moment it is gone here */
			if (!del || nofall_has(uc, &k))
				continue;
			sh = pcache_ht_shadow_peek(ht, &k, &fl, &mark, &exp);
			if (sh == 0 && (fl & PCACHE_F_UPMARK) &&
			    mark == PCACHE_UPMARK_ABSENT)
				continue;          /* known absent upstream */
			if (sh == 0 && (!(fl & PCACHE_F_UPMARK) ||
			    mark == PCACHE_UPMARK_PRESENT)) {
				nofall_put(uc, &k, exp);   /* known present */
				continue;
			}
			if (verdict != PC_UPV_NONE) {
				if (verdict == PC_UPV_ERROR &&
				    uc->on_error == PC_UP_ON_ERROR_ERROR) {
					g->err = "upstream: the ownership check "
						"failed (upstream unreachable or timed "
						"out) - not deleting a key the upstream "
						"may hold";
					goto answered;
				}
				continue;
			}
			own = 1;
			goto fetch;
		}
		if (nofall_has(uc, &k)) {
			ST_ADD(uc->nofall_hits, 1);
			continue;                  /* the fleet's: no fall-through */
		}
		sh = pcache_ht_shadow_peek(ht, &k, &fl, &mark, &exp);
		if (sh == 0 && !(fl & PCACHE_F_UPMARK)) {
			/* a fetched copy */
			if (cls == UPC_READ || cls == UPC_PROBE) {
				ST_ADD(uc->shadow_hits, 1);
				continue;
			}
			if (cls == UPC_PLAIN) {
				nofall_put(uc, &k, exp);   /* overwritten: owned */
				continue;
			}
			if (promote(uc, ht, &k) != 0) {
				g->err = "upstream: could not take the fetched "
					"value over before changing it";
				goto answered;
			}
			continue;
		}
		if (sh == 0) {
			/* a marker */
			if (mark == PCACHE_UPMARK_ABSENT) {
				ST_ADD(uc->neg_hits, 1);
				continue;
			}
			if (cls == UPC_PLAIN) {
				/* overwriting a key the upstream holds as
				 * something this collection cannot - owned from
				 * here, for as long as that copy may live */
				nofall_put(uc, &k, 0);
				continue;
			}
			if (mark == PCACHE_UPMARK_PRESENT) {
				/* ST2: the upstream has it, its value unread.
				 * A probe is answered by the marker; a read or
				 * a change needs the value - fetched, also on a
				 * re-run (a GET that joined a probe's fetch too
				 * late is queued again by finish(); this covers
				 * the marker coming back between the two) */
				if (cls == UPC_PROBE) {
					ST_ADD(uc->present_hits, 1);
					continue;
				}
				if (verdict == PC_UPV_ERROR &&
				    (uc->on_error == PC_UP_ON_ERROR_ERROR ||
				     cls == UPC_MUTATE)) {
					g->err = "upstream: the read-through "
						"fetch failed (upstream unreachable "
						"or timed out)";
					goto answered;
				}
				if (verdict == PC_UPV_ERROR)
					continue;
				goto fetch;
			}
			if (mark == PCACHE_UPMARK_WRONGTYPE) {
				g->wrongtype = 1;
				goto answered;
			}
			g->err = "upstream: the value is larger than this "
				"collection can hold";
			goto answered;
		}
		/* nothing known about it */
		if (cls == UPC_PLAIN) {
			/* a plain write does not wait: the ownership check runs
			 * in the background, and records a no-fall entry if
			 * the upstream has the key */
			if (!bg && !(bg = malloc((size_t)nkeys * sizeof *bg)))
				continue;
			bg[nbg++] = i;
			continue;
		}
		if (verdict != PC_UPV_NONE) {
			/* the run after a completion: a key still unknown is
			 * not fetched again.  A mutation never runs on a guess */
			if (verdict == PC_UPV_ERROR &&
			    (uc->on_error == PC_UP_ON_ERROR_ERROR ||
			     cls == UPC_MUTATE)) {
				g->err = "upstream: the read-through fetch failed "
					"(upstream unreachable or timed out)";
				goto answered;
			}
			continue;
		}
fetch:
		if (!jk) {
			jk = malloc((size_t)nkeys * sizeof *jk);
			jown = malloc((size_t)nkeys * sizeof *jown);
			if (!jk || !jown) {
				g->err = "upstream: out of memory parking the "
					"request";
				goto answered;
			}
		}
		jk[nj] = i;
		jown[nj] = own;
		nj++;
	}
	if (!nj && !nbg) {
		free(bg);
		return PC_UPG_PROCEED;
	}
	r = nj ? calloc(1, sizeof *r) : NULL;
	if (nj && !r) {
		g->err = "upstream: out of memory parking the request";
		goto answered;
	}
	pthread_mutex_lock(&uc->mx);
	for (i = 0; i < nbg; i++) {
		ST_ADD(uc->own_checks, 1);
		join_key(uc, keys[bg[i]], kl[bg[i]], NULL, 1, pc_worker_id(), 0);
	}
	if (r) {
		r->worker = pc_worker_id();
		r->status = PC_UPV_OK;
		r->id = __atomic_fetch_add(&next_id, 1, __ATOMIC_RELAXED);
		if (!r->id)
			r->id = __atomic_fetch_add(&next_id, 1, __ATOMIC_RELAXED);
		for (i = 0; i < nj; i++) {
			if (jown[i])
				ST_ADD(uc->own_checks, 1);
			if (join_key(uc, keys[jk[i]], kl[jk[i]], r, jown[i],
			        r->worker, cls == UPC_READ || cls == UPC_MUTATE) == 0)
				need++;
		}
	}
	if (!need) {
		pthread_mutex_unlock(&uc->mx);
		free(r);
		free(jk);
		free(jown);
		free(bg);
		return PC_UPG_PROCEED;
	}
	g->req = r->id;
	ST_ADD(uc->held, 1);
	pthread_mutex_unlock(&uc->mx);
	free(jk);
	free(jown);
	free(bg);
	return PC_UPG_HELD;
answered:
	free(jk);
	free(jown);
	free(bg);
	return PC_UPG_ANSWERED;
}

int pc_upstream_resp_gate(pcache_htable_t *ht, const char *col, size_t collen,
		char *const *argv, const size_t *argl, int nargs,
		struct pc_jw *out, void *conn)
{
	struct pc_upg g;
	int cls, del, all, rc, n;

	if (!ncols || nargs < 2)
		return PC_UPG_PROCEED;
	cls = pc_upstream_cmd_class(argv[0], argl[0], argv, argl, nargs, &del,
		&all);
	if (cls == UPC_NONE)
		return PC_UPG_PROCEED;
	n = all ? nargs - 1 : 1;
	rc = pc_upstream_gate(ht, col, collen, cls, del,
		(const char *const *)argv + 1, argl + 1, n,
		pc_conn_upstream_verdict(conn), &g);
	if (rc == PC_UPG_HELD)
		pc_conn_upstream_hold(conn, g.req);
	else if (rc == PC_UPG_ANSWERED) {
		if (g.wrongtype)
			wrongtype(out);
		else {
			pc_jw_lit(out, "-ERR ");
			pc_jw_raw(out, g.err, strlen(g.err));
			pc_jw_lit(out, "\r\n");
		}
	}
	return rc;
}

/* ---- the RESP client --------------------------------------------------- */

/* one reply, RESP2 or RESP3 */
struct rv {
	char t;                            /* the type byte */
	long long n;                       /* integer, or an aggregate's size */
	char *s;                           /* bulk/simple/error text (in buf) */
	size_t len;
	struct rv *el;                     /* aggregate elements (malloc'd) */
	int nel;
};

struct uconn {
	struct up_col *uc;
	struct up_srv *s;                  /* S382: the upstream it talks to */
	int fd;
	int resp3;
	int tracked;                       /* tracking on, redirected */
	unsigned int gen;                  /* the s->inv_gen it armed at */
	int inv;                           /* THE invalidation connection */
	int notrack;                       /* never armed: the drain sweep's */
	char run_id[41];                   /* its server's run_id, "" unknown */
	char *buf;
	size_t len, cap, off;              /* buf[off..len) unread */
	long long backoff_until;           /* no reconnect before (ms) */
};

static void rv_free(struct rv *v)
{
	int i;

	if (!v)
		return;
	for (i = 0; i < v->nel; i++)
		rv_free(&v->el[i]);
	free(v->el);
	v->el = NULL;
	v->nel = 0;
}

/* parse one reply at buf[*pos..len): 1 complete (*pos advanced), 0 need
 * more bytes, -1 malformed.  Strings point into buf (valid until the
 * next read compacts it). */
static int rv_parse(char *buf, size_t len, size_t *pos, struct rv *v, int depth)
{
	size_t p = *pos, e;
	char *nl;
	long long n;
	char *end;
	int i, cnt;

	memset(v, 0, sizeof *v);
	if (depth > 8)
		return -1;
	if (p >= len)
		return 0;
	nl = memchr(buf + p, '\n', len - p);
	if (!nl)
		return 0;
	e = (size_t)(nl - buf);            /* index of '\n' */
	if (e == p || buf[e - 1] != '\r')
		return -1;
	v->t = buf[p];
	switch (v->t) {
	case '+': case '-': case '(': case ',': case '#':
		v->s = buf + p + 1;
		v->len = e - 1 - (p + 1);
		*pos = e + 1;
		return 1;
	case ':':
		buf[e - 1] = 0;
		v->n = strtoll(buf + p + 1, &end, 10);
		buf[e - 1] = '\r';
		*pos = e + 1;
		return 1;
	case '_':
		*pos = e + 1;
		return 1;
	case '$': case '=': case '!':
		buf[e - 1] = 0;
		n = strtoll(buf + p + 1, &end, 10);
		buf[e - 1] = '\r';
		if (n < 0) {                   /* RESP2 null bulk */
			v->t = '_';
			*pos = e + 1;
			return 1;
		}
		if ((size_t)n > UP_RBUF_MAX)
			return -3;             /* too large to hold: the caller
			                        * marks the key and drops the
			                        * connection (the stream is mid
			                        * value) */
		if (len - (e + 1) < (size_t)n + 2)
			return 0;
		v->s = buf + e + 1;
		v->len = (size_t)n;
		if (v->t == '=' && v->len >= 4) {   /* verbatim: "txt:" prefix */
			v->s += 4;
			v->len -= 4;
		}
		*pos = e + 1 + (size_t)n + 2;
		return 1;
	case '*': case '%': case '~': case '>': case '|':
		buf[e - 1] = 0;
		n = strtoll(buf + p + 1, &end, 10);
		buf[e - 1] = '\r';
		p = e + 1;
		if (n < 0) {
			v->t = '_';
			*pos = p;
			return 1;
		}
		if (n > 1000000)
			return -1;
		cnt = (v->t == '%' || v->t == '|') ? (int)n * 2 : (int)n;
		v->n = n;
		if (cnt) {
			v->el = calloc((size_t)cnt, sizeof *v->el);
			if (!v->el)
				return -1;
		}
		v->nel = cnt;
		for (i = 0; i < cnt; i++) {
			int rc = rv_parse(buf, len, &p, &v->el[i], depth + 1);

			if (rc <= 0 || rc == -3) {
				v->nel = i;
				rv_free(v);
				return rc;
			}
		}
		if (v->t == '|') {             /* an attribute: skip, read the
		                                * value it annotates */
			rv_free(v);
			*pos = p;
			return rv_parse(buf, len, pos, v, depth + 1);
		}
		*pos = p;
		return 1;
	default:
		return -1;
	}
}

static void flush_shadow(struct up_srv *s);

static void uc_close(struct uconn *c)
{
	if (c->fd >= 0) {
		close(c->fd);
		c->fd = -1;
		__atomic_sub_fetch(&c->s->conns_up, 1, __ATOMIC_RELAXED);
		/* the upstream forgets what a closed connection read: what it
		 * fetched is no longer tracked (the others' still is) */
		if (c->tracked && !c->inv)
			flush_shadow(c->s);
	}
	c->tracked = 0;
	c->len = c->off = 0;
}

static void nap_ms(int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int send_all(struct uconn *c, const char *p, size_t n, long long dl)
{
	while (n) {
		struct pollfd pf = { c->fd, POLLOUT, 0 };
		long long left = dl - now_ms();
		ssize_t w;

		if (left <= 0)
			return -2;
		if (poll(&pf, 1, (int)left) <= 0)
			return -2;
		w = send(c->fd, p, n, MSG_NOSIGNAL);
		if (w < 0) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			return -1;
		}
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

/* a command as a RESP array of bulks */
static int send_cmd(struct uconn *c, long long dl, int argc,
		const char *const *av, const size_t *al)
{
	char hdr[32], *msg;
	size_t n = 0, i, cap = 16;
	int k, rc;

	for (k = 0; k < argc; k++)
		cap += al[k] + 32;
	msg = malloc(cap);
	if (!msg)
		return -1;
	n = (size_t)snprintf(msg, cap, "*%d\r\n", argc);
	for (k = 0; k < argc; k++) {
		int hn = snprintf(hdr, sizeof hdr, "$%zu\r\n", al[k]);

		memcpy(msg + n, hdr, (size_t)hn);
		n += (size_t)hn;
		for (i = 0; i < al[k]; i++)
			msg[n + i] = av[k][i];
		n += al[k];
		msg[n++] = '\r';
		msg[n++] = '\n';
	}
	rc = send_all(c, msg, n, dl);
	free(msg);
	return rc;
}

static int rm_cb(const str *key, const str *val,
		const struct pcache_rec_meta *mt, void *ctx)
{
	(void)val;
	(void)mt;
	pcache_ht_remove(ctx, key);
	return 0;
}

/* everything an upstream's shadow holds goes: an invalidation from it may
 * have been missed (its tracking connection was lost) - the next read
 * fetches afresh.  S382: that upstream's copies, no other's */
static void flush_shadow(struct up_srv *s)
{
	pcache_ht_iter_meta(s->shadow, rm_cb, s->shadow);
	ST_ADD(s->uc->flushes, 1);
	ST_ADD(s->flushes, 1);
}

/* an upstream's invalidation connection changed (lost, or newly up): its
 * fetch connections must re-arm toward the new one, and what was fetched
 * under the old one may have missed an invalidation */
static void tracking_moved(struct up_srv *s)
{
	__atomic_add_fetch(&s->inv_gen, 1, __ATOMIC_ACQ_REL);
	flush_shadow(s);
}

/* push messages (RESP3 '>') between replies.  ["invalidate", [keys]] drops
 * those keys from the shadow; ["invalidate", null] (a FLUSHALL upstream)
 * drops everything; "tracking-redir-broken" on a fetch connection means
 * its invalidations have nowhere to go - it re-arms */
static void on_push(struct uconn *c, struct rv *v)
{
	struct up_col *uc = c->uc;
	const struct rv *n;

	if (v->nel < 1 || !v->el[0].s)
		return;
	n = &v->el[0];
	if (n->len == 10 && !memcmp(n->s, "invalidate", 10)) {
		const struct rv *ks = v->nel > 1 ? &v->el[1] : NULL;
		int i;

		if (!ks || ks->t == '_') {
			flush_shadow(c->s);        /* a FLUSHALL: each says it */
			return;
		}
		for (i = 0; i < ks->nel; i++) {
			str k;

			if (!ks->el[i].s)
				continue;
			k.s = ks->el[i].s;
			k.len = (int)ks->el[i].len;
			/* S382: changed upstream - a copy from any of them is
			 * stale; each tracking connection says so for its own */
			sh_remove(uc, &k);
			ST_ADD(uc->invalidations, 1);
			ST_ADD(c->s->invalidations, 1);
		}
		return;
	}
	if (n->len == 21 && !memcmp(n->s, "tracking-redir-broken", 21))
		c->gen = (unsigned int)-1;     /* re-arm before the next fetch */
}

/* read ONE reply (pushes handled in between).  0 ok, -1 error, -2 timeout,
 * -3 a value too large to hold (the connection must be dropped) */
static int read_reply(struct uconn *c, struct rv *v, long long dl)
{
	for (;;) {
		size_t pos = c->off;
		int rc = rv_parse(c->buf, c->len, &pos, v, 0);

		if (rc == -3)
			return -3;
		if (rc < 0)
			return -1;
		if (rc > 0) {
			c->off = pos;
			if (v->t == '>') {
				on_push(c, v);
				rv_free(v);
				continue;
			}
			return 0;
		}
		/* need more: compact, grow, read */
		if (c->off && c->off == c->len)
			c->off = c->len = 0;
		if (c->len == c->cap) {
			size_t nc;
			char *nb;

			if (c->off) {
				/* the unread part stays where the strings of a
				 * reply being parsed point - only move it when
				 * nothing parsed is held (we are between replies) */
				memmove(c->buf, c->buf + c->off, c->len - c->off);
				c->len -= c->off;
				c->off = 0;
			}
			if (c->len == c->cap) {
				nc = c->cap ? c->cap * 2 : 65536;
				if (nc > UP_RBUF_MAX + 65536)
					return -1;
				nb = realloc(c->buf, nc);
				if (!nb)
					return -1;
				c->buf = nb;
				c->cap = nc;
			}
		}
		{
			struct pollfd pf = { c->fd, POLLIN, 0 };
			long long left = dl - now_ms();
			ssize_t r;

			if (left <= 0)
				return -2;
			if (poll(&pf, 1, (int)left) <= 0)
				return -2;
			r = recv(c->fd, c->buf + c->len, c->cap - c->len, 0);
			if (r == 0)
				return -1;
			if (r < 0) {
				if (errno == EAGAIN || errno == EINTR)
					continue;
				return -1;
			}
			c->len += (size_t)r;
		}
	}
}

static int is_ok(const struct rv *v)
{
	return v->t == '+' && v->len == 2 && !memcmp(v->s, "OK", 2);
}

/* which server this connection reached: the run_id in INFO server (40 hex
 * characters, a new one every time a server process starts).  An error
 * reply (the user may not run INFO) leaves it unknown; -1 only when the
 * connection itself failed */
static int server_id(struct uconn *c, long long dl)
{
	const char *av[2] = { "INFO", "server" };
	size_t al[2] = { 4, 6 };
	struct rv v;

	c->run_id[0] = 0;
	if (send_cmd(c, dl, 2, av, al) != 0 || read_reply(c, &v, dl) != 0)
		return -1;
	if ((v.t == '$' || v.t == '=') && v.s) {
		const char *p = v.s, *e = v.s + v.len;

		for (; p + 7 <= e; p++) {
			if ((p == v.s || p[-1] == '\n') && !memcmp(p, "run_id:", 7)) {
				size_t n = 0;

				p += 7;
				while (p + n < e && n < 40 && isxdigit((unsigned char)p[n]))
					n++;
				if (n == 40) {
					memcpy(c->run_id, p, 40);
					c->run_id[40] = 0;
				}
				break;
			}
		}
		/* S384: a perfcached says so in its INFO server */
		for (p = v.s; p + 19 <= e; p++)
			if ((p == v.s || p[-1] == '\n') &&
			    !memcmp(p, "perfcached_version:", 19)) {
				__atomic_store_n(&c->s->is_pc, 1, __ATOMIC_RELAXED);
				break;
			}
	}
	rv_free(&v);
	return 0;
}

static int role_check(struct uconn *c, long long dl);

/* S382: this connection's server failed to connect, or answered: the
 * upstream's health, for pick() */
static void srv_reach(struct up_srv *s, int ok)
{
	__atomic_store_n(&s->failing, ok ? 0 : 1, __ATOMIC_RELAXED);
}

/* S382: the number after "<name>:" at a line's start in the INFO text
 * [@b, @e), or -1 - read within it (the text is not terminated) */
static long long info_num(const char *b, const char *e, const char *name)
{
	size_t nl = strlen(name);
	const char *p;

	for (p = b; p + nl < e; p++)
		if ((p == b || p[-1] == '\n') && !memcmp(p, name, nl) &&
		    p[nl] == ':') {
			long long n = 0;
			int any = 0;

			for (p += nl + 1; p < e && *p >= '0' && *p <= '9'; p++) {
				if (n < 100000000000LL)
					n = n * 10 + (*p - '0');
				any = 1;
			}
			return any ? n : -1;
		}
	return -1;
}

/* S382: what the server is - ROLE says master, replica (Redis: "slave")
 * or something else; for a replica, INFO replication says whether its link
 * to the master is up and how long since it last heard from it (whole
 * seconds: that is the lag upstream_max_lag_ms is held to).  The upstream's
 * health from it, logged when that changes what it may do.  0 asked
 * (whatever it said), -1 the connection failed */
static int role_check(struct uconn *c, long long dl)
{
	struct up_srv *s = c->s;
	struct up_col *uc = c->uc;
	const char *av[2] = { "ROLE", NULL }, *iv[2] = { "INFO", "replication" };
	size_t al[2] = { 4, 0 }, il[2] = { 4, 11 };
	struct rv v;
	int role = UPR_OTHER, link_down = 0, before = srv_state(s), now;
	long long lag = -1;

	if (send_cmd(c, dl, 1, av, al) != 0 || read_reply(c, &v, dl) != 0)
		return -1;
	if ((v.t == '*' || v.t == '~') && v.nel >= 1 && v.el[0].s) {
		if (v.el[0].len == 6 && !memcmp(v.el[0].s, "master", 6))
			role = UPR_MASTER;
		else if (v.el[0].len == 5 && !memcmp(v.el[0].s, "slave", 5))
			role = UPR_REPLICA;
	}
	rv_free(&v);
	if (role == UPR_REPLICA) {
		if (send_cmd(c, dl, 2, iv, il) != 0 || read_reply(c, &v, dl) != 0)
			return -1;
		if ((v.t == '$' || v.t == '=') && v.s) {
			const char *p = v.s, *e = v.s + v.len, *q;
			long long io;

			for (q = p; q + 24 <= e; q++)
				if ((q == p || q[-1] == '\n') &&
				    !memcmp(q, "master_link_status:down", 23)) {
					link_down = 1;
					break;
				}
			io = info_num(p, e, "master_last_io_seconds_ago");
			lag = io >= 0 ? io * 1000 : -1;
		}
		rv_free(&v);
	}
	__atomic_store_n(&s->role, role, __ATOMIC_RELAXED);
	__atomic_store_n(&s->link_down, link_down, __ATOMIC_RELAXED);
	__atomic_store_n(&s->lag_ms, lag, __ATOMIC_RELAXED);
	__atomic_store_n(&s->checked_ms, now_ms(), __ATOMIC_RELAXED);
	now = srv_state(s);
	if (now == PC_UPS_OFF && before != PC_UPS_OFF) {
		if (role == UPR_MASTER)        /* by design: listed to follow failover */
			LM_NOTICE("collection '%s': upstream %s is a master - not used "
				"(upstream_role = replica)\n", uc->name, s->addr);
		else if (role != UPR_REPLICA)
			LM_WARN("collection '%s': upstream %s does not say it is a "
				"replica (ROLE) - not used (upstream_role = replica)\n",
				uc->name, s->addr);
		else if (link_down)
			LM_WARN("collection '%s': upstream %s has lost its link to "
				"its master - skipped until it is back\n", uc->name,
				s->addr);
		else
			LM_WARN("collection '%s': upstream %s last heard from its "
				"master %lld ms ago, more than upstream_max_lag_ms "
				"(%d) - skipped until it catches up\n", uc->name,
				s->addr, lag, uc->max_lag_ms);
		s->refused_said = 1;
	} else if (now != PC_UPS_OFF && s->refused_said) {
		LM_NOTICE("collection '%s': upstream %s is %s again - taken "
			"back\n", uc->name, s->addr, role == UPR_MASTER ?
			"usable as the master fallback" : "a replica in step");
		s->refused_said = 0;
	}
	return 0;
}

static int uc_connect(struct uconn *c, long long dl)
{
	struct up_col *uc = c->uc;
	struct up_srv *s = c->s;
	struct addrinfo hints, *ai = NULL;
	char ps[16];
	struct rv v;
	int fd, rc, one = 1;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(ps, sizeof ps, "%d", s->port);
	if (getaddrinfo(s->host, ps, &hints, &ai) != 0 || !ai) {
		srv_reach(s, 0);
		return -1;
	}
	fd = socket(ai->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		freeaddrinfo(ai);
		return -1;
	}
	rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
	freeaddrinfo(ai);
	if (rc < 0 && errno != EINPROGRESS) {
		close(fd);
		srv_reach(s, 0);
		return -1;
	}
	if (rc < 0) {
		struct pollfd pf = { fd, POLLOUT, 0 };
		long long left = dl - now_ms();
		int err = 0;
		socklen_t el = sizeof err;

		if (left <= 0 || poll(&pf, 1, (int)left) <= 0 ||
		    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err) {
			close(fd);
			srv_reach(s, 0);
			return -1;
		}
	}
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	c->fd = fd;
	c->len = c->off = 0;
	c->resp3 = 0;
	__atomic_add_fetch(&s->conns_up, 1, __ATOMIC_RELAXED);
	ST_ADD(uc->connects, 1);
	ST_ADD(s->connects, 1);

	/* RESP3 with the credentials in one step, where the server has it */
	{
		const char *av[5];
		size_t al[5];
		int n = 0;

		av[n] = "HELLO"; al[n++] = 5;
		av[n] = "3"; al[n++] = 1;
		if (uc->password) {
			av[n] = "AUTH"; al[n++] = 4;
			av[n] = s->user[0] ? s->user : "default";
			al[n] = strlen(av[n]);
			n++;
			av[n] = uc->password; al[n] = strlen(uc->password); n++;
		}
		if (send_cmd(c, dl, n, av, al) != 0 || read_reply(c, &v, dl) != 0)
			goto fail;
		if (v.t == '%') {
			c->resp3 = 1;              /* RESP3, credentials accepted */
			rv_free(&v);
		} else {
			/* no HELLO 3 here (an older server, or a perfcached
			 * RESP door): RESP2, AUTH on its own */
			rv_free(&v);
			if (uc->password) {
				const char *a2[3];
				size_t l2[3];
				int m = 0;

				a2[m] = "AUTH"; l2[m++] = 4;
				if (s->user[0]) {
					a2[m] = s->user;
					l2[m] = strlen(s->user);
					m++;
				}
				a2[m] = uc->password;
				l2[m] = strlen(uc->password);
				m++;
				if (send_cmd(c, dl, m, a2, l2) != 0 ||
				    read_reply(c, &v, dl) != 0)
					goto fail;
				if (!is_ok(&v)) {
					rv_free(&v);
					LM_ERR("upstream %s: AUTH refused by %s\n",
						uc->name, s->addr);
					goto fail;
				}
				rv_free(&v);
			}
		}
	}
	if (s->db) {
		char dbs[16];
		const char *av[2] = { "SELECT", dbs };
		size_t al[2];

		al[0] = 6;
		al[1] = (size_t)snprintf(dbs, sizeof dbs, "%d", s->db);
		if (send_cmd(c, dl, 2, av, al) != 0 || read_reply(c, &v, dl) != 0)
			goto fail;
		if (!is_ok(&v)) {
			rv_free(&v);
			LM_ERR("upstream %s: SELECT %d refused by %s\n", uc->name,
				s->db, s->addr);
			goto fail;
		}
		rv_free(&v);
	}
	srv_reach(s, 1);
	/* S384: which server it is, always now - a perfcached upstream is
	 * refused unless upstream_allow_perfcached; allowed, the connection
	 * names itself, so that node never reads through on its behalf */
	if (server_id(c, dl) != 0)
		goto fail;
	if (__atomic_load_n(&s->is_pc, __ATOMIC_RELAXED)) {
		if (!uc->allow_pc) {
			if (!__atomic_exchange_n(&s->pc_said, 1, __ATOMIC_RELAXED))
				LM_WARN("collection '%s': upstream %s is a perfcached "
					"node - not used: read-through from perfcached "
					"is off (upstream_allow_perfcached = no), as a "
					"ring of them is a fetch storm\n", uc->name,
					s->addr);
			__atomic_store_n(&s->checked_ms, now_ms(), __ATOMIC_RELAXED);
			uc_close(c);
			return -1;
		} else {
			const char *av[3] = { "CLIENT", "SETNAME",
				"perfcached-readthrough" };
			size_t al[3] = { 6, 7, 22 };

			if (send_cmd(c, dl, 3, av, al) != 0 ||
			    read_reply(c, &v, dl) != 0)
				goto fail;
			rv_free(&v);
		}
	}
	/* S382: what the server IS, before any fetch goes to it - a master
	 * where a replica is asked for is closed here, unless the fallback
	 * lets it serve while no replica does */
	if (uc->role_req == PC_UP_ROLE_REPLICA) {
		if (role_check(c, dl) != 0)
			goto fail;
		if (srv_state(s) == PC_UPS_OFF) {
			uc_close(c);
			return -1;
		}
	} else
		__atomic_store_n(&s->checked_ms, now_ms(), __ATOMIC_RELAXED);
	/* tracking: this connection's reads are tracked, and the
	 * invalidations go to the collection's invalidation connection.  The
	 * generation is read FIRST: if that connection moves meanwhile, the
	 * next fetch sees a newer one and re-arms. */
	c->tracked = 0;
	if (!c->inv && !c->notrack && uc->tracking && c->resp3) {
		unsigned int gen = __atomic_load_n(&s->inv_gen, __ATOMIC_ACQUIRE);
		long long id = __atomic_load_n(&s->inv_id, __ATOMIC_ACQUIRE);
		char inv_run[41];

		c->gen = gen;
		pthread_mutex_lock(&s->run_mx);
		memcpy(inv_run, s->inv_run, sizeof inv_run);
		pthread_mutex_unlock(&s->run_mx);
		if (id > 0 && strcmp(c->run_id, inv_run) != 0) {
			/* another server than the invalidation connection's: the
			 * id is not ours there, so this connection stays untracked
			 * - its copies live at most upstream_negative_ms */
			ST_ADD(s->other_server, 1);
			if (!__atomic_exchange_n(&s->other_said, 1, __ATOMIC_RELAXED))
				LM_WARN("collection '%s': a connection to the upstream "
					"reached another server (run_id %.12s) than the "
					"tracking connection (%.12s) - a round-robin "
					"proxy?  Its fetches are not tracked: a copy "
					"lives at most upstream_negative_ms (%d ms).  "
					"Point upstream at one server, or a backend "
					"that keeps a client on one (balance source)\n",
					uc->name, c->run_id[0] ? c->run_id : "unknown",
					inv_run, uc->neg_ms);
			id = 0;
		}
		if (id > 0) {
			char ids[24];
			const char *av[5] = { "CLIENT", "TRACKING", "ON", "REDIRECT",
				ids };
			size_t al[5] = { 6, 8, 2, 8, 0 };

			al[4] = (size_t)snprintf(ids, sizeof ids, "%lld", id);
			if (send_cmd(c, dl, 5, av, al) != 0 ||
			    read_reply(c, &v, dl) != 0)
				goto fail;
			c->tracked = is_ok(&v);
			rv_free(&v);
		}
	}
	c->backoff_until = 0;
	return 0;
fail:
	uc_close(c);
	srv_reach(s, 0);                   /* refused us: not usable either */
	return -1;
}

/* ---- one fetch --------------------------------------------------------- */

static unsigned int ttl_ticks(long long ms)
{
	long long s = (ms + 999) / 1000;

	if (s < 1)
		s = 1;
	if (s > 0x7fffffffLL)
		s = 0x7fffffffLL;
	return get_ticks() + (unsigned int)s;
}

static void put_mark(struct up_srv *s, const str *k, char mark)
{
	struct up_col *uc = s->uc;
	str v;
	char b = mark;

	if (uc->neg_ms <= 0 && mark == PCACHE_UPMARK_ABSENT)
		return;                        /* not remembered at all */
	v.s = &b;
	v.len = 1;
	pcache_ht_store_ex(s->shadow, k, &v,
		ttl_ticks(uc->neg_ms > 0 ? uc->neg_ms : 1000), PCACHE_F_UPMARK);
}

/* a copy no invalidation can reach (tracking configured, but this fetch
 * was not tracked, or the tracking moved while it ran) lives no longer
 * than upstream_negative_ms: stale for at most that */
static __thread int fill_capped;

/* ST2: the upstream has @k (TYPE said @tfl's type), its value not read.
 * It lives as a copy would: the upstream's expiry, an invalidation drops
 * it, and an untracked fetch caps it at upstream_negative_ms. */
static void put_present(struct up_srv *s, const str *k, unsigned char tfl,
		unsigned int exp)
{
	struct up_col *uc = s->uc;
	char b[2] = { PCACHE_UPMARK_PRESENT, (char)tfl };
	str v;

	if (fill_capped) {
		unsigned int cap = ttl_ticks(uc->neg_ms > 0 ? uc->neg_ms : 1000);

		if (!exp || exp > cap)
			exp = cap;
		ST_ADD(uc->untracked, 1);
	}
	v.s = b;
	v.len = 2;
	if (pcache_ht_store_ex(s->shadow, k, &v, exp, PCACHE_F_UPMARK) == 0)
		ST_ADD(uc->fill_present, 1);
	else
		ST_ADD(uc->fill_failed, 1);
}

#define UPF_FINAL   1                  /* what any waiter needs is stored */
#define UPF_PRESENT 2                  /* only a PRESENT marker (ST2) */

/* a fetched value into the shadow.  The store refuses a value over the
 * cell ceiling (a marker, so it is not fetched again) and an arena with no
 * room (nothing stored: counted, and the command runs as a miss). */
static void store_fill(struct up_srv *s, const str *k, const str *v,
		unsigned int exp, unsigned char fl, unsigned long long *ok_ctr)
{
	struct up_col *uc = s->uc;
	int rc;

	if (fill_capped) {
		unsigned int cap = ttl_ticks(uc->neg_ms > 0 ? uc->neg_ms : 1000);

		if (!exp || exp > cap)
			exp = cap;
		ST_ADD(uc->untracked, 1);
	}
	rc = pcache_ht_store_ex(s->shadow, k, v, exp, fl);

	if (rc == 0)
		ST_ADD(*ok_ctr, 1);
	else if (rc == -2)
		ST_ADD(uc->fill_failed, 1);
	else {
		put_mark(s, k, PCACHE_UPMARK_TOOBIG);
		ST_ADD(uc->toobig, 1);
	}
}

/* a HGETALL reply as a hash record (hashrec.h); <0 too big / malformed */
static long hash_record(const struct rv *v, unsigned char **out)
{
	size_t n = 8, i;
	int pairs = v->nel / 2, k;
	unsigned char *r, *p;

	if (v->nel % 2 || pairs < 1)
		return -1;
	for (k = 0; k < v->nel; k++) {
		const struct rv *e = &v->el[k];

		if (!e->s && e->t != '+' && e->t != '$')
			return -1;
		if (k % 2 == 0 && (e->len == 0 || e->len > PC_HR_FIELD_MAX))
			return -2;
		n += e->len + (k % 2 == 0 ? 6 : 0);
	}
	if (n > PC_HR_MAX)
		return -2;
	r = malloc(n);
	if (!r)
		return -1;
	r[0] = 1; r[1] = 0; r[2] = 0; r[3] = 0;
	r[4] = (unsigned char)pairs;
	r[5] = (unsigned char)(pairs >> 8);
	r[6] = (unsigned char)(pairs >> 16);
	r[7] = (unsigned char)(pairs >> 24);
	p = r + 8;
	for (k = 0; k < v->nel; k += 2) {
		const struct rv *f = &v->el[k], *val = &v->el[k + 1];

		p[0] = (unsigned char)f->len;
		p[1] = (unsigned char)(f->len >> 8);
		p[2] = (unsigned char)val->len;
		p[3] = (unsigned char)(val->len >> 8);
		p[4] = (unsigned char)(val->len >> 16);
		p[5] = (unsigned char)(val->len >> 24);
		p += 6;
		for (i = 0; i < f->len; i++)
			p[i] = (unsigned char)f->s[i];
		p += f->len;
		for (i = 0; i < val->len; i++)
			p[i] = (unsigned char)val->s[i];
		p += val->len;
	}
	if (!pc_hr_valid(r, n, NULL)) {
		free(r);
		return -1;
	}
	*out = r;
	return (long)n;
}

/* fetch @k into the shadow.  0 ok (a value or a marker stored), -1 the
 * connection failed, -2 timed out */
static int fetch(struct uconn *c, struct up_key *e)
{
	const char *kp = e->key;
	size_t kl = e->klen;
	struct up_col *uc = c->uc;
	struct up_srv *s = c->s;
	long long t0 = now_ms(), dl = t0 + uc->timeout_ms;
	struct rv ty, tt, gv;
	str k, v;
	int rc;
	unsigned int exp;
	char tname[32];
	size_t tn;

	k.s = (char *)kp;
	k.len = (int)kl;
	e->fetched = UPF_FINAL;
	/* tracking moved since this connection armed it: re-arm (a quiet
	 * close - the gap was already flushed) */
	if (c->fd >= 0 && uc->tracking && c->gen !=
	    __atomic_load_n(&s->inv_gen, __ATOMIC_ACQUIRE)) {
		c->tracked = 0;
		uc_close(c);
	}
	if (c->fd < 0 && uc_connect(c, dl) != 0)
		return -1;
	{
		unsigned int g0 = __atomic_load_n(&s->inv_gen, __ATOMIC_ACQUIRE);

		fill_capped = uc->tracking && (!c->tracked || g0 != c->gen);
	}
	{
		/* one round trip for the common case: TYPE, PTTL and GET
		 * pipelined (GET on a hash is a WRONGTYPE reply, unused) */
		const char *a1[2] = { "TYPE", kp }, *a2[2] = { "PTTL", kp },
			*a3[2] = { "GET", kp };
		size_t l1[2] = { 4, kl }, l2[2] = { 4, kl }, l3[2] = { 3, kl };

		if (send_cmd(c, dl, 2, a1, l1) != 0 ||
		    send_cmd(c, dl, 2, a2, l2) != 0 ||
		    send_cmd(c, dl, 2, a3, l3) != 0)
			goto io;
	}
	memset(&ty, 0, sizeof ty);
	memset(&tt, 0, sizeof tt);
	memset(&gv, 0, sizeof gv);
	if ((rc = read_reply(c, &ty, dl)) != 0)
		goto io_rc;
	/* the TYPE text NOW: a reply's strings point into the read buffer,
	 * which the next read may reset or move */
	tn = ty.t == '+' && ty.len < sizeof tname ? ty.len : 0;
	memcpy(tname, ty.s ? ty.s : "", tn);
	tname[tn] = 0;
	rv_free(&ty);
	if ((rc = read_reply(c, &tt, dl)) != 0)
		goto io_rc;
	if ((rc = read_reply(c, &gv, dl)) != 0)
		goto io_rc;

	if (tt.t == ':' && tt.n == -2) {
		put_mark(s, &k, PCACHE_UPMARK_ABSENT);  /* gone meanwhile */
		ST_ADD(uc->absent, 1);
		rv_free(&gv);
		goto done;
	}
	exp = tt.t == ':' && tt.n >= 0 ? ttl_ticks(tt.n) : 0;

	if (!strcmp(tname, "none")) {
		put_mark(s, &k, PCACHE_UPMARK_ABSENT);
		ST_ADD(uc->absent, 1);
		rv_free(&gv);
		goto done;
	}
	if (!strcmp(tname, "string")) {
		if (gv.t != '$' && gv.t != '+') {
			/* it changed type between TYPE and GET */
			rv_free(&gv);
			put_mark(s, &k, PCACHE_UPMARK_ABSENT);
			ST_ADD(uc->absent, 1);
			goto done;
		}
		v.s = gv.s;
		v.len = (int)gv.len;
		store_fill(s, &k, &v, exp, 0, &uc->fill_string);
		rv_free(&gv);
		goto done;
	}
	rv_free(&gv);
	/* ST2: a hash or a document nobody waiting needs the value of (an
	 * EXISTS, a TTL, a TYPE, an ownership check): what TYPE and PTTL said
	 * is the answer - no second round trip, no value moved or stored */
	if ((!strcmp(tname, "hash") || !strcmp(tname, "ReJSON-RL")) &&
	    !__atomic_load_n(&e->full, __ATOMIC_ACQUIRE)) {
		put_present(s, &k, !strcmp(tname, "hash") ? PCACHE_F_HASH
			: PCACHE_F_JSON, exp);
		e->fetched = UPF_PRESENT;
		goto done;
	}
	if (!strcmp(tname, "hash")) {
		const char *a[2] = { "HGETALL", kp };
		size_t l[2] = { 7, kl };
		unsigned char *rec = NULL;
		long n;

		if (send_cmd(c, dl, 2, a, l) != 0)
			goto io;
		if ((rc = read_reply(c, &gv, dl)) != 0)
			goto io_rc;
		if (gv.t != '%' && gv.t != '*') {
			rv_free(&gv);
			put_mark(s, &k, PCACHE_UPMARK_ABSENT);
			ST_ADD(uc->absent, 1);
			goto done;
		}
		if (gv.nel == 0) {
			rv_free(&gv);
			put_mark(s, &k, PCACHE_UPMARK_ABSENT);
			ST_ADD(uc->absent, 1);
			goto done;
		}
		n = hash_record(&gv, &rec);
		rv_free(&gv);
		if (n < 0) {
			put_mark(s, &k, PCACHE_UPMARK_TOOBIG);
			ST_ADD(uc->toobig, 1);
			goto done;
		}
		v.s = (char *)rec;
		v.len = (int)n;
		store_fill(s, &k, &v, exp, PCACHE_F_HASH, &uc->fill_hash);
		free(rec);
		goto done;
	}
	if (!strcmp(tname, "ReJSON-RL")) {
		const char *a[2] = { "JSON.GET", kp };
		size_t l[2] = { 8, kl };

		if (send_cmd(c, dl, 2, a, l) != 0)
			goto io;
		if ((rc = read_reply(c, &gv, dl)) != 0)
			goto io_rc;
		if (gv.t != '$') {
			rv_free(&gv);
			put_mark(s, &k, PCACHE_UPMARK_ABSENT);
			ST_ADD(uc->absent, 1);
			goto done;
		}
		v.s = gv.s;
		v.len = (int)gv.len;
		store_fill(s, &k, &v, exp, PCACHE_F_JSON, &uc->fill_json);
		rv_free(&gv);
		goto done;
	}
	/* list, set, zset, stream, a module type: not held here */
	put_mark(s, &k, PCACHE_UPMARK_WRONGTYPE);
	ST_ADD(uc->wrongtype, 1);
done:
	ST_ADD(uc->lat_us_sum, (unsigned long long)(now_ms() - t0) * 1000);
	ST_ADD(uc->lat_n, 1);
	ST_ADD(s->lat_us_sum, (unsigned long long)(now_ms() - t0) * 1000);
	ST_ADD(s->lat_n, 1);
	return 0;
io:
	rc = -1;
io_rc:
	uc_close(c);
	if (rc == -3) {
		/* a value larger than any record: marked so it is not read
		 * again, the connection dropped (it stopped mid-value) */
		put_mark(s, &k, PCACHE_UPMARK_TOOBIG);
		ST_ADD(uc->toobig, 1);
		return 0;
	}
	return rc;
}

/* ---- the connection threads ------------------------------------------- */

/* finish @e: count every waiting request down, post the ones done */
static void finish(struct up_col *uc, struct up_key *e, int status)
{
	struct up_wait *w, *wn;
	struct up_key **pp;

	pthread_mutex_lock(&uc->mx);
	for (pp = &uc->inflight; *pp && *pp != e; pp = &(*pp)->next)
		;
	if (*pp)
		*pp = e->next;
	if (e->own && status == PC_UPV_OK) {
		/* first, on the worker that asked: the request it rode with
		 * (if any) runs again AFTER the no-fall entry exists */
		struct up_done d;

		memset(&d, 0, sizeof d);
		d.kind = UPD_OWN;
		d.col = uc->idx;
		d.key = malloc(e->klen ? e->klen : 1);
		d.klen = e->klen;
		if (d.key) {
			memcpy(d.key, e->key, e->klen);
			post_done(e->own_worker, &d);
		}
	}
	for (w = e->waiters; w; w = wn) {
		struct up_req *r = w->r;

		wn = w->next;
		if (status != PC_UPV_OK)
			r->status = status;
		if (--r->left == 0) {
			post(r->worker, r->id, r->status);
			free(r);
		}
		free(w);
	}
	pthread_mutex_unlock(&uc->mx);
	free(e);
}

/* the invalidation connection: RESP3, its id published for the fetch
 * connections to redirect to, then nothing but pushes */
static void *inv_main(void *arg)
{
	struct up_srv *s = arg;
	struct up_col *uc = s->uc;
	struct uconn c;
	int said = 0;

	memset(&c, 0, sizeof c);
	c.uc = uc;
	c.s = s;
	c.fd = -1;
	c.inv = 1;
#ifdef __linux__
	{
		char tn[16];

		snprintf(tn, sizeof tn, "pc-inv%d-%.7s", s->idx, uc->name);
		prctl(PR_SET_NAME, (unsigned long)tn, 0, 0, 0);
	}
#endif
	while (!uc->stop) {
		struct rv v;
		int rc;

		if (c.fd < 0) {
			long long dl = now_ms() + uc->timeout_ms;
			const char *av[2] = { "CLIENT", "ID" };
			size_t al[2] = { 6, 2 };

			if (uc_connect(&c, dl) != 0) {
				int w;

				for (w = 0; w < 10 && !uc->stop; w++)
					nap_ms(100);
				continue;
			}
			if (!c.resp3) {
				LM_NOTICE("collection '%s': the upstream has no RESP3 "
					"- no tracking: a fetched copy lives at most "
					"upstream_negative_ms (%d ms)\n", uc->name,
					uc->neg_ms);
				uc_close(&c);
				return NULL;
			}
			if (!c.run_id[0]) {
				LM_WARN("collection '%s': the upstream does not say "
					"which server it is (INFO server refused, or no "
					"run_id) - tracking stays off, because perfcached "
					"cannot confirm its connections reach one server: "
					"a fetched copy lives at most upstream_negative_ms "
					"(%d ms).  Allow +info for the upstream user and "
					"restart\n", uc->name, uc->neg_ms);
				uc_close(&c);
				return NULL;
			}
			if (send_cmd(&c, dl, 2, av, al) != 0 ||
			    read_reply(&c, &v, dl) != 0 || v.t != ':') {
				if (!said++)
					LM_NOTICE("collection '%s': the upstream does "
						"not answer CLIENT ID - no tracking\n",
						uc->name);
				uc_close(&c);
				return NULL;
			}
			/* the server first, then the id: a fetch connection that
			 * sees the id compares against this server */
			pthread_mutex_lock(&s->run_mx);
			memcpy(s->inv_run, c.run_id, sizeof s->inv_run);
			pthread_mutex_unlock(&s->run_mx);
			__atomic_store_n(&s->inv_id, v.n, __ATOMIC_RELEASE);
			rv_free(&v);
			tracking_moved(s);
			LM_NOTICE("collection '%s': tracking on - %s's "
				"invalidations reach connection %lld on server "
				"%.12s\n", uc->name, s->addr, s->inv_id, c.run_id);
		}
		rc = read_reply(&c, &v, now_ms() + 1000);
		if (rc == -2)
			continue;                  /* quiet: nothing changed */
		if (rc != 0) {
			LM_WARN("collection '%s': the tracking connection to %s "
				"was lost - the copies fetched from it are dropped\n",
				uc->name, s->addr);
			uc_close(&c);
			__atomic_store_n(&s->inv_id, 0, __ATOMIC_RELEASE);
			pthread_mutex_lock(&s->run_mx);
			s->inv_run[0] = 0;
			pthread_mutex_unlock(&s->run_mx);
			tracking_moved(s);
			continue;
		}
		rv_free(&v);                   /* not a push: nothing asked */
	}
	uc_close(&c);
	free(c.buf);
	return NULL;
}

/* S382: an upstream's health, on its first fetch thread once a second.
 * Where a replica is asked for, ROLE again every 10 s - roles move at a
 * failover - and an upstream refused for its role is dialled again every
 * 10 s, so a demoted master comes back.  With others to take its fetches,
 * an upstream never reached yet is dialled at once, so pick() knows it, and
 * one that failed every 2 s; with one upstream every fetch dials, as before
 * S382 */
static void srv_tick(struct uconn *c)
{
	struct up_srv *s = c->s;
	struct up_col *uc = c->uc;
	long long now = now_ms(), dl = now + uc->timeout_ms;
	long long chk = __atomic_load_n(&s->checked_ms, __ATOMIC_RELAXED);
	int st = srv_state(s);

	if (c->fd >= 0) {
		if (now - chk < 10000)
			return;
		if (uc->role_req == PC_UP_ROLE_REPLICA) {
			/* failed, or refused now: no fetch on it */
			if (role_check(c, dl) != 0 || srv_state(s) == PC_UPS_OFF)
				uc_close(c);
		} else if (uc->nsrv > 1) {
			/* several upstreams: a PING says this one still answers
			 * - an idle connection to a dead server says nothing,
			 * and failover must not wait for traffic to find out */
			const char *av[1] = { "PING" };
			size_t al[1] = { 4 };
			struct rv v;

			if (send_cmd(c, dl, 1, av, al) != 0 ||
			    read_reply(c, &v, dl) != 0)
				uc_close(c);
			else {
				rv_free(&v);
				__atomic_store_n(&s->checked_ms, now, __ATOMIC_RELAXED);
			}
		}
		return;
	}
	if (uc->nsrv > 1) {
		/* several: this first connection is kept, so each upstream's
		 * health is known without traffic - dialled again at once when
		 * it closes, every 2 s while it fails, every 10 s while it is
		 * refused (a master, a perfcached) */
		long long gap = st == PC_UPS_OFF && !__atomic_load_n(&s->failing,
			__ATOMIC_RELAXED) ? 10000 : 2000;

		if (now - c->backoff_until >= gap && uc_connect(c, dl) != 0)
			c->backoff_until = now_ms();
		return;
	}
	/* one upstream: every fetch dials, as before S382; refused, ROLE
	 * again every 10 s, so a demoted master comes back */
	if (st == PC_UPS_OFF && !__atomic_load_n(&s->failing, __ATOMIC_RELAXED) &&
	    now - chk >= 10000 && uc_connect(c, dl) != 0)
		c->backoff_until = now_ms();
}

/* under uc->mx: a fetch on @s is over - done, or handed on */
static void unbusy(struct up_srv *s)
{
	if (s->busy > 0)
		s->busy--;
}

/* a fetch on @s failed: once more on another upstream that serves (S382),
 * else the waiters get the error */
static void fail_or_retry(struct up_srv *s, struct up_key *e)
{
	struct up_col *uc = s->uc;
	struct up_srv *o;

	pthread_mutex_lock(&uc->mx);
	unbusy(s);
	o = e->tried ? NULL : pick(uc, s);
	if (o) {
		e->tried = 1;
		enqueue(o, e);
		pthread_mutex_unlock(&uc->mx);
		return;
	}
	pthread_mutex_unlock(&uc->mx);
	finish(uc, e, PC_UPV_ERROR);
}

static void *conn_main(void *arg)
{
	struct up_srv *s = arg;
	struct up_col *uc = s->uc;
	struct uconn c;
	long long next_tick = 0;

	memset(&c, 0, sizeof c);
	c.uc = uc;
	c.s = s;
	c.fd = -1;
#ifdef __linux__
	{
		char tn[16];

		snprintf(tn, sizeof tn, "pc-up%d-%.8s", s->idx, uc->name);
		prctl(PR_SET_NAME, (unsigned long)tn, 0, 0, 0);
	}
#endif
	for (;;) {
		struct up_key *e;
		int rc;

		pthread_mutex_lock(&uc->mx);
		while (!s->qhead && !uc->stop) {
			struct timespec ts;

			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_sec += 1;
			pthread_cond_timedwait(&s->cv, &uc->mx, &ts);
			break;
		}
		if (uc->stop) {
			pthread_mutex_unlock(&uc->mx);
			break;
		}
		e = s->qhead;
		if (e) {
			s->qhead = e->qnext;
			if (!s->qhead)
				s->qtail = NULL;
			s->queued--;
			/* S382: refused or unreachable since it was queued here -
			 * another upstream that serves takes it */
			if (srv_state(s) == PC_UPS_OFF) {
				struct up_srv *o = pick(uc, s);

				if (o) {
					enqueue(o, e);
					pthread_mutex_unlock(&uc->mx);
					continue;
				}
			}
			s->busy++;
		}
		pthread_mutex_unlock(&uc->mx);

		/* the shadow's own upkeep, on the first thread: expired
		 * copies reaped, the table widened as it fills; and S382's
		 * health of this upstream */
		if (s->thr[0] == pthread_self() && now_ms() >= next_tick) {
			next_tick = now_ms() + 1000;
			pcache_ht_sweep(s->shadow, get_ticks(), NULL, NULL);
			pcache_ht_grow_at(s->shadow, 75, 1024);
			srv_tick(&c);
			/* S382 part 2: failover decides once a second too, so a
			 * switch (and a preempt) happens without traffic */
			if (s->idx == 0 && uc->mode == PC_UP_MODE_FAILOVER) {
				pthread_mutex_lock(&uc->mx);
				fo_eval(uc);
				pthread_mutex_unlock(&uc->mx);
			}
		}
		if (!e)
			continue;
		if (srv_state(s) == PC_UPS_OFF &&
		    !__atomic_load_n(&s->failing, __ATOMIC_RELAXED)) {
			/* a master, a replica cut off, or a perfcached (S384) -
			 * and nothing else serves: refused, not asked */
			ST_ADD(uc->errors, 1);
			ST_ADD(s->errors, 1);
			fail_or_retry(s, e);
			continue;
		}
		if (c.fd < 0 && c.backoff_until && now_ms() < c.backoff_until) {
			/* reconnecting too soon: fail fast rather than stall the
			 * queue behind a dead upstream */
			ST_ADD(uc->errors, 1);
			ST_ADD(s->errors, 1);
			fail_or_retry(s, e);
			continue;
		}
		ST_ADD(uc->fetches, 1);
		ST_ADD(s->fetches, 1);
		rc = fetch(&c, e);
		if (rc == 0) {
			/* ST2: it stopped at a PRESENT marker, and a read or a
			 * change joined meanwhile: once more, for the value -
			 * the entry stays in flight, its waiters with it */
			pthread_mutex_lock(&uc->mx);
			unbusy(s);
			if (e->fetched == UPF_PRESENT && e->full) {
				enqueue(s, e);
				pthread_mutex_unlock(&uc->mx);
				ST_ADD(uc->refetches, 1);
				continue;
			}
			pthread_mutex_unlock(&uc->mx);
			finish(uc, e, PC_UPV_OK);
			continue;
		}
		if (rc == -2) {
			ST_ADD(uc->timeouts, 1);
			ST_ADD(s->timeouts, 1);
		} else {
			ST_ADD(uc->errors, 1);
			ST_ADD(s->errors, 1);
		}
		/* the next attempt to connect waits a little: an upstream that
		 * is down must not be dialled once per key */
		c.backoff_until = now_ms() + 250;
		fail_or_retry(s, e);
	}
	uc_close(&c);
	free(c.buf);
	return NULL;
}

/* ---- the drain sweep ----------------------------------------------------- */

#define DR_BATCH    256                /* keys per SCAN and per pipeline */
#define DR_INFLIGHT 8192               /* posted, not yet applied */

static int pull_worker;                /* round-robin over the workers */

/* hand one read key to a worker (it decides and writes) */
static void pull_post(struct up_col *uc, const char *k, size_t kl,
		const char *v, size_t vl, unsigned int exp, unsigned char fl)
{
	struct up_done d;
	int i, w = -1;

	while (__atomic_load_n(&uc->drain_inflight, __ATOMIC_RELAXED) >
	       DR_INFLIGHT && !uc->drain_stop)
		nap_ms(5);                     /* the workers are behind */
	for (i = 0; i < wq_n; i++) {
		int j = (pull_worker + i) % wq_n;

		if (wq[j].efd >= 0) {
			w = j;
			break;
		}
	}
	if (w < 0)
		return;
	pull_worker = w + 1;
	memset(&d, 0, sizeof d);
	d.kind = UPD_PULL;
	d.col = uc->idx;
	d.key = malloc(kl ? kl : 1);
	d.val = malloc(vl ? vl : 1);
	if (!d.key || !d.val) {
		free(d.key);
		free(d.val);
		ST_ADD(uc->dr_errors, 1);
		return;
	}
	memcpy(d.key, k, kl);
	memcpy(d.val, v, vl);
	d.klen = kl;
	d.vlen = vl;
	d.exp = exp;
	d.fl = fl;
	__atomic_add_fetch(&uc->drain_inflight, 1, __ATOMIC_RELAXED);
	post_done(w, &d);
}

/* read one batch of keys: TYPE, PTTL and GET for all of them in one
 * pipeline, then HGETALL / JSON.GET for the hashes and documents in a
 * second.  0, or <0 the connection failed (the batch is read again) */
static int drain_batch(struct uconn *c, char **ks, size_t *kl, int n)
{
	struct up_col *uc = c->uc;
	long long dl = now_ms() + uc->timeout_ms * 4LL + 50LL * n;
	unsigned char *kind;               /* 'h' hash, 'j' json, 0 done */
	unsigned int *exps;
	int i, rc = 0;

	if (n <= 0)                        /* a SCAN page may be empty */
		return 0;
	kind = calloc((size_t)n, 1);
	exps = calloc((size_t)n, sizeof *exps);
	if (!kind || !exps) {
		free(kind);
		free(exps);
		return -1;
	}
	for (i = 0; i < n; i++) {
		const char *a1[2] = { "TYPE", ks[i] }, *a2[2] = { "PTTL", ks[i] },
			*a3[2] = { "GET", ks[i] };
		size_t l1[2] = { 4, kl[i] }, l2[2] = { 4, kl[i] },
			l3[2] = { 3, kl[i] };

		if (send_cmd(c, dl, 2, a1, l1) != 0 ||
		    send_cmd(c, dl, 2, a2, l2) != 0 ||
		    send_cmd(c, dl, 2, a3, l3) != 0) {
			rc = -1;
			goto out;
		}
	}
	for (i = 0; i < n; i++) {
		struct rv ty, tt, gv;
		char tname[32];
		size_t tn;

		if (read_reply(c, &ty, dl) != 0) {
			rc = -1;
			goto out;
		}
		tn = ty.t == '+' && ty.len < sizeof tname ? ty.len : 0;
		memcpy(tname, ty.s ? ty.s : "", tn);
		tname[tn] = 0;
		rv_free(&ty);
		if (read_reply(c, &tt, dl) != 0 || read_reply(c, &gv, dl) != 0) {
			rc = -1;
			goto out;
		}
		ST_ADD(uc->dr_scanned, 1);
		if (!strcmp(tname, "none") || (tt.t == ':' && tt.n == -2)) {
			ST_ADD(uc->dr_absent, 1);
			rv_free(&gv);
			continue;
		}
		exps[i] = tt.t == ':' && tt.n >= 0 ? ttl_ticks(tt.n) : 0;
		if (!strcmp(tname, "string")) {
			if (gv.t == '$' || gv.t == '+')
				pull_post(uc, ks[i], kl[i], gv.s, gv.len, exps[i], 0);
			else
				ST_ADD(uc->dr_absent, 1);
		} else if (!strcmp(tname, "hash")) {
			kind[i] = 'h';
		} else if (!strcmp(tname, "ReJSON-RL")) {
			kind[i] = 'j';
		} else {
			ST_ADD(uc->dr_wrongtype, 1);
		}
		rv_free(&gv);
	}
	/* the second pipeline: hashes and documents */
	for (i = 0; i < n; i++) {
		const char *a[2];
		size_t l[2];

		if (!kind[i])
			continue;
		a[0] = kind[i] == 'h' ? "HGETALL" : "JSON.GET";
		l[0] = strlen(a[0]);
		a[1] = ks[i];
		l[1] = kl[i];
		if (send_cmd(c, dl, 2, a, l) != 0) {
			rc = -1;
			goto out;
		}
	}
	for (i = 0; i < n; i++) {
		struct rv gv;

		if (!kind[i])
			continue;
		if (read_reply(c, &gv, dl) != 0) {
			rc = -1;
			goto out;
		}
		if (kind[i] == 'h') {
			unsigned char *rec = NULL;
			long rl;

			if ((gv.t != '%' && gv.t != '*') || !gv.nel) {
				ST_ADD(uc->dr_absent, 1);
			} else if ((rl = hash_record(&gv, &rec)) < 0) {
				ST_ADD(uc->dr_toobig, 1);
			} else {
				pull_post(uc, ks[i], kl[i], (char *)rec, (size_t)rl,
					exps[i], PCACHE_F_HASH);
				free(rec);
			}
		} else if (gv.t == '$') {
			pull_post(uc, ks[i], kl[i], gv.s, gv.len, exps[i],
				PCACHE_F_JSON);
		} else {
			ST_ADD(uc->dr_absent, 1);
		}
		rv_free(&gv);
	}
out:
	free(kind);
	free(exps);
	return rc;
}

static void *drain_main(void *arg)
{
	struct up_col *uc = arg;
	struct uconn c;
	long long t0 = now_ms(), done = 0;
	int p, failures = 0;

	memset(&c, 0, sizeof c);
	c.uc = uc;
	pthread_mutex_lock(&uc->mx);           /* S382: one that serves */
	c.s = pick(uc, NULL);
	if (!c.s)
		c.s = &uc->srv[0];
	pthread_mutex_unlock(&uc->mx);
	c.fd = -1;
	/* never tracked: what the sweep reads goes straight into the
	 * collection, not into the shadow, so there is no copy for an
	 * invalidation to drop - and tracking it would make the upstream
	 * remember every key of the family for this client and send an
	 * invalidation for each later write to any of them */
	c.notrack = 1;
	fill_capped = 0;
#ifdef __linux__
	{
		char tn[16];

		snprintf(tn, sizeof tn, "pc-drn-%.8s", uc->name);
		prctl(PR_SET_NAME, (unsigned long)tn, 0, 0, 0);
	}
#endif
	LM_NOTICE("collection '%s': the drain sweep starts - every key the "
		"upstream holds under %d pattern(s), read and written here where "
		"the fleet has not taken it over (rate %d keys/s)\n", uc->name,
		uc->npat, uc->drain_rate);
	for (p = 0; p < uc->npat && !uc->drain_stop; p++) {
		char cur[32] = "0";

		do {
			const char *av[6] = { "SCAN", cur, "MATCH", uc->pat[p],
				"COUNT", "256" };
			size_t al[6];
			long long dl = now_ms() + uc->timeout_ms * 4LL;
			struct rv v;
			char **ks = NULL;
			size_t *kl = NULL;
			int n = 0, i, ok = 0;

			al[0] = 4;
			al[1] = strlen(cur);
			al[2] = 5;
			al[3] = (size_t)uc->patlen[p];
			al[4] = 5;
			al[5] = 3;
			if (c.fd < 0 && uc_connect(&c, dl) != 0)
				goto retry;
			if (send_cmd(&c, dl, 6, av, al) != 0 ||
			    read_reply(&c, &v, dl) != 0)
				goto retry;
			if ((v.t != '*' && v.t != '>') || v.nel != 2 ||
			    !v.el[0].s || v.el[0].len >= sizeof cur) {
				rv_free(&v);
				goto retry;
			}
			{
				char next[32];

				memcpy(next, v.el[0].s, v.el[0].len);
				next[v.el[0].len] = 0;
				n = v.el[1].nel;
				ks = calloc((size_t)(n ? n : 1), sizeof *ks);
				kl = calloc((size_t)(n ? n : 1), sizeof *kl);
				for (i = 0; ks && kl && i < n; i++) {
					kl[i] = v.el[1].el[i].len;
					ks[i] = malloc(kl[i] ? kl[i] : 1);
					if (ks[i])
						memcpy(ks[i], v.el[1].el[i].s, kl[i]);
				}
				rv_free(&v);
				ok = ks && kl;
				for (i = 0; ok && i < n; i++)
					ok = ks[i] != NULL;
				if (ok && drain_batch(&c, ks, kl, n) == 0) {
					memcpy(cur, next, strlen(next) + 1);
					done += n;
					failures = 0;
				} else {
					ok = 0;
				}
				for (i = 0; ks && i < n; i++)
					free(ks[i]);
				free(ks);
				free(kl);
			}
			if (!ok)
				goto retry;
			/* the rate cap: the upstream may be production */
			if (uc->drain_rate > 0) {
				long long want = t0 + done * 1000 / uc->drain_rate;

				while (now_ms() < want && !uc->drain_stop)
					nap_ms((int)(want - now_ms() < 50 ?
						want - now_ms() : 50));
			}
			continue;
retry:
			uc_close(&c);
			ST_ADD(uc->dr_errors, 1);
			if (++failures > 10) {
				LM_ERR("collection '%s': the drain sweep gave up - the "
					"upstream failed ten times running\n", uc->name);
				uc->drain_state = UPDR_FAILED;
				goto out;
			}
			nap_ms(500);
		} while (strcmp(cur, "0") && !uc->drain_stop);
	}
	uc->drain_state = uc->drain_stop ? UPDR_STOPPED : UPDR_DONE;
out:
	/* the workers finish what was posted before the state is final */
	while (__atomic_load_n(&uc->drain_inflight, __ATOMIC_RELAXED) > 0 &&
	       !uc->stop)
		nap_ms(10);
	uc->drain_t1 = (long long)time(NULL);
	LM_NOTICE("collection '%s': the drain sweep %s - %llu read, %llu taken "
		"over, %llu the fleet already had, %llu gone, %llu of a type it "
		"cannot hold, %llu too big, %llu errors\n", uc->name,
		updr_name[uc->drain_state], ST_GET(uc->dr_scanned),
		ST_GET(uc->dr_pulled), ST_GET(uc->dr_owned), ST_GET(uc->dr_absent),
		ST_GET(uc->dr_wrongtype), ST_GET(uc->dr_toobig),
		ST_GET(uc->dr_errors));
	uc_close(&c);
	free(c.buf);
	return NULL;
}

/* S348: the refusal message when @col refuses writes, counted; NULL when
 * it takes them (or has no upstream).  Every mutation path asks this
 * beside the node's own refusal, so a write the gate never classified
 * (MSET, a CMD method) is refused all the same. */
const char *pc_upstream_write_refusal(const char *col, size_t collen)
{
	struct up_col *uc;

	if (!ncols)
		return NULL;
	uc = col_of(col, collen);
	if (!uc || !uc->refuse)
		return NULL;
	ST_ADD(uc->writes_refused, 1);
	return uc->refuse_msg;
}

/* S348: the same, not counted - for the cluster's config digest */
int pc_upstream_refuses_writes(const char *col, size_t collen)
{
	struct up_col *uc = ncols ? col_of(col, collen) : NULL;

	return uc && uc->refuse;
}

int pc_upstream_drain_ctl(const char *col, size_t collen, int stop,
		const char **msg)
{
	struct up_col *uc = col_of(col, collen);

	if (!uc) {
		*msg = "this collection has no upstream";
		return -1;
	}
	if (stop) {
		if (uc->drain_state != UPDR_RUNNING) {
			*msg = "no drain sweep is running";
			return -1;
		}
		uc->drain_stop = 1;
		*msg = "stopping";
		return 0;
	}
	if (uc->refuse) {                                      /* S348 */
		*msg = "a drain sweep takes the upstream's keys over - it writes "
			"them - and this collection refuses writes (upstream_writes = "
			"refuse)";
		return -1;
	}
	if (uc->drain_state == UPDR_RUNNING) {
		*msg = "a drain sweep is already running";
		return -1;
	}
	if (uc->drain_joinable) {
		pthread_join(uc->drain_thr, NULL);
		uc->drain_joinable = 0;
	}
	uc->drain_stop = 0;
	uc->drain_state = UPDR_RUNNING;
	uc->drain_t0 = (long long)time(NULL);
	uc->drain_t1 = 0;
	uc->dr_scanned = uc->dr_pulled = uc->dr_owned = uc->dr_absent =
		uc->dr_wrongtype = uc->dr_toobig = uc->dr_errors = 0;
	if (pthread_create(&uc->drain_thr, NULL, drain_main, uc) != 0) {
		uc->drain_state = UPDR_FAILED;
		*msg = "cannot start the drain thread";
		return -1;
	}
	uc->drain_joinable = 1;
	*msg = "started";
	return 0;
}

/* ---- init / shutdown --------------------------------------------------- */

/* redis://[user@]host:port[/db] */
static int parse_url(struct up_srv *s, const char *url)
{
	const char *p = url + 8, *at = strchr(p, '@'), *slash, *colon;
	size_t hl;

	if (strncmp(url, "redis://", 8))
		return -1;
	if (at) {
		size_t ul = (size_t)(at - p);

		if (ul >= sizeof s->user)
			return -1;
		memcpy(s->user, p, ul);
		s->user[ul] = 0;
		p = at + 1;
	}
	slash = strchr(p, '/');
	colon = memchr(p, ':', slash ? (size_t)(slash - p) : strlen(p));
	if (!colon)
		return -1;
	hl = (size_t)(colon - p);
	if (!hl || hl >= sizeof s->host)
		return -1;
	memcpy(s->host, p, hl);
	s->host[hl] = 0;
	s->port = atoi(colon + 1);
	if (s->port <= 0 || s->port > 65535)
		return -1;
	s->db = slash ? atoi(slash + 1) : 0;
	if (s->db < 0 || s->db > 1023)
		return -1;
	{
		/* from a copy: addr and host are both in cols[], which gcc 14
		 * reads as a possible overlap (-Wrestrict) */
		char hb[sizeof s->host];

		memcpy(hb, s->host, sizeof hb);
		snprintf(s->addr, sizeof s->addr, "%.255s:%d", hb, s->port);
	}
	return 0;
}

/* S382: `upstream = url[, url...]` - each into its own up_srv */
static int parse_urls(struct up_col *uc, const char *list)
{
	const char *p = list;

	uc->nsrv = 0;
	while (*p) {
		const char *e;
		size_t n;
		char one[512];

		while (*p == ' ' || *p == '\t' || *p == ',')
			p++;
		if (!*p)
			break;
		e = p;
		while (*e && *e != ',')
			e++;
		n = (size_t)(e - p);
		while (n && (p[n - 1] == ' ' || p[n - 1] == '\t'))
			n--;
		if (!n || n >= sizeof one || uc->nsrv >= PC_UP_MAX_SRV)
			return -1;
		memcpy(one, p, n);
		one[n] = 0;
		if (parse_url(&uc->srv[uc->nsrv], one) != 0)
			return -1;
		uc->nsrv++;
		p = e;
	}
	return uc->nsrv ? 0 : -1;
}

static int parse_prefixes(struct up_col *uc, const char *list)
{
	const char *p = list;

	while (*p) {
		const char *e;
		size_t n;

		while (*p == ' ' || *p == '\t' || *p == ',')
			p++;
		if (!*p)
			break;
		e = p;
		while (*e && *e != ',')
			e++;
		n = (size_t)(e - p);
		while (n && (p[n - 1] == ' ' || p[n - 1] == '\t'))
			n--;
		if (n) {
			if (uc->npat >= UP_MAX_PREFIXES)
				return -1;
			uc->pat[uc->npat] = strndup(p, n);
			if (!uc->pat[uc->npat])
				return -1;
			uc->patlen[uc->npat] = (int)n;
			uc->npat++;
		}
		p = e;
	}
	return uc->npat ? 0 : -1;
}

int pc_upstream_init(const struct pc_config *cfg, int nworkers)
{
	int i, j, k;

	wq_n = nworkers;
	wq = calloc((size_t)nworkers, sizeof *wq);
	if (!wq)
		return -1;
	for (i = 0; i < nworkers; i++) {
		pthread_mutex_init(&wq[i].mx, NULL);
		wq[i].efd = -1;
	}
	for (i = 0; i < cfg->n_col; i++) {
		const struct pc_collection *cc = &cfg->col[i];
		struct up_col *uc;
		pcache_htable_t *ht;
		int log2;

		if (!cc->upstream)
			continue;
		if (ncols >= UP_MAX_COLS) {
			LM_ERR("upstream: more than %d read-through collections\n",
				UP_MAX_COLS);
			return -1;
		}
		ht = pc_store_find(cc->name, strlen(cc->name));
		if (!ht) {
			LM_ERR("upstream: collection '%s' does not exist\n",
				cc->name);
			return -1;
		}
		/* the collection must decide its keys HERE: a proxy, shard or
		 * spread collection places a key on another node, where this
		 * node's shadow means nothing */
		if (pc_store_proxy_enabled(ht) || pc_store_shard_enabled(ht) ||
		    pc_cluster_replicas() > 0) {
			LM_ERR("upstream: [collection %s] read-through needs a "
				"store or eager collection (not proxy, shard or "
				"spread)\n", cc->name);
			return -1;
		}
		uc = &cols[ncols];
		memset(uc, 0, sizeof *uc);
		uc->idx = ncols;
		uc->nlen = strlen(cc->name);
		if (uc->nlen >= sizeof uc->name)
			return -1;
		memcpy(uc->name, cc->name, uc->nlen);
		uc->nflen = (size_t)snprintf(uc->nf, sizeof uc->nf, "%s.nofall",
			cc->name);
		uc->refuse = cc->up_refuse_writes;            /* S348 */
		if (uc->nflen >= sizeof uc->nf ||
		    (!uc->refuse && !pc_store_find(uc->nf, uc->nflen))) {
			LM_ERR("upstream: the companion '%s' does not exist\n",
				uc->nf);
			return -1;
		}
		if (parse_urls(uc, cc->upstream) != 0) {
			LM_ERR("upstream: [collection %s] upstream = %s: not "
				"redis://[user@]host:port[/db], one or more\n", cc->name,
				cc->upstream);
			return -1;
		}
		{
			/* from a copy: the message and the host are both in
			 * cols[], which gcc 14 reads as a possible overlap */
			char hb[sizeof uc->srv[0].addr];

			memcpy(hb, uc->srv[0].addr, sizeof hb);
			snprintf(uc->refuse_msg, sizeof uc->refuse_msg, "collection "
				"'%s' is a read-through of %.200s%s and refuses writes "
				"(upstream_writes = refuse)", cc->name, hb, uc->nsrv > 1 ?
				" and its other upstreams" : "");
		}
		uc->role_req = cc->up_role;                   /* S382 */
		uc->master_fallback = cc->up_master_fallback;
		uc->max_lag_ms = cc->up_max_lag_ms;
		uc->allow_pc = cc->up_allow_pc;               /* S384 */
		uc->mode = cc->up_mode;                       /* S382 part 2 */
		uc->preempt = cc->up_preempt;
		uc->hold_ms = (long long)cc->up_preempt_hold_s * 1000;
		uc->active = -1;
		uc->primary = 0;
		if (cc->up_primary)
			for (k = 0; k < uc->nsrv; k++)
				if (!strcmp(uc->srv[k].addr, cc->up_primary))
					uc->primary = k;
		if (parse_prefixes(uc, cc->up_prefixes) != 0) {
			LM_ERR("upstream: [collection %s] upstream_prefixes: "
				"empty, or more than %d patterns\n", cc->name,
				UP_MAX_PREFIXES);
			return -1;
		}
		uc->password = cc->up_password;               /* S383 */
		uc->neg_ms = cc->up_negative_ms;
		uc->timeout_ms = cc->up_timeout_ms;
		uc->conns = cc->up_conns;
		uc->tracking = cc->up_tracking;
		uc->on_error = cc->up_on_error;
		uc->drain_rate = cc->up_drain_rate;
		log2 = cc->buckets_log2 > 0 ? cc->buckets_log2 : 12;
		for (k = 0; k < uc->nsrv; k++) {
			struct up_srv *s = &uc->srv[k];

			s->uc = uc;
			s->idx = k;
			s->lag_ms = -1;
			s->shadow = pcache_htable_new((unsigned int)log2);
			if (!s->shadow) {
				LM_ERR("upstream: no memory for '%s''s shadow table\n",
					cc->name);
				return -1;
			}
			pthread_cond_init(&s->cv, NULL);
			pthread_mutex_init(&s->run_mx, NULL);
			/* S382: the chain - each table names the next */
			if (k)
				pcache_ht_set_shadow(uc->srv[k - 1].shadow, s->shadow);
		}
		pthread_mutex_init(&uc->mx, NULL);
		ncols++;
		pcache_ht_set_shadow_gate(ht, shadow_ok, uc);
		pcache_ht_set_shadow(ht, uc->srv[0].shadow);
		for (k = 0; k < uc->nsrv; k++) {
			struct up_srv *s = &uc->srv[k];

			if (uc->tracking) {
				if (pthread_create(&s->inv_thr, NULL, inv_main, s) != 0) {
					LM_ERR("upstream: cannot start the tracking "
						"thread for '%s'\n", cc->name);
					return -1;
				}
				s->has_inv = 1;
			}
			for (j = 0; j < uc->conns; j++) {
				if (pthread_create(&s->thr[j], NULL, conn_main, s) != 0) {
					LM_ERR("upstream: cannot start a connection "
						"thread for '%s'\n", cc->name);
					return -1;
				}
				s->nthr++;
			}
		}
		{
			char list[PC_UP_MAX_SRV * 32];
			size_t ll = 0;

			list[0] = 0;
			for (k = 0; k < uc->nsrv && ll < sizeof list; k++)
				ll += (size_t)snprintf(list + ll, sizeof list - ll,
					"%s%.24s:%d", k ? ", " : "", uc->srv[k].host,
					uc->srv[k].port);
			LM_NOTICE("collection '%s': read-through from %s db %d%s, "
				"%d pattern(s), %d connection(s)%s, negative %d ms, "
				"timeout %d ms, on_error %s - this node READS the "
				"upstream and never writes it\n", cc->name, list,
				uc->srv[0].db, uc->role_req == PC_UP_ROLE_REPLICA ?
				(uc->master_fallback ? ", replicas (the master while "
				"none serves)" : ", replicas only") : "", uc->npat,
				uc->conns, uc->nsrv > 1 ? " each" : "", uc->neg_ms,
				uc->timeout_ms,
				uc->on_error == PC_UP_ON_ERROR_MISS ? "miss" : "error");
		}
	}
	return 0;
}

void pc_upstream_shutdown(void)
{
	int i, j, k;

	for (i = 0; i < ncols; i++) {
		struct up_col *uc = &cols[i];

		uc->drain_stop = 1;
		if (uc->drain_joinable) {
			pthread_join(uc->drain_thr, NULL);
			uc->drain_joinable = 0;
		}
		pthread_mutex_lock(&uc->mx);
		uc->stop = 1;
		for (k = 0; k < uc->nsrv; k++)
			pthread_cond_broadcast(&uc->srv[k].cv);
		pthread_mutex_unlock(&uc->mx);
		for (k = 0; k < uc->nsrv; k++) {
			struct up_srv *s = &uc->srv[k];

			for (j = 0; j < s->nthr; j++)
				pthread_join(s->thr[j], NULL);
			s->nthr = 0;
			if (s->has_inv) {
				pthread_join(s->inv_thr, NULL);
				s->has_inv = 0;
			}
		}
	}
}

/* ---- stats -------------------------------------------------------------- */

void pc_upstream_stats_tree(struct pc_tw *out, const char *col, size_t collen)
{
	struct up_col *uc = col_of(col, collen);
	unsigned long long n, oth = 0;
	long long up = 0, ents = 0;
	int k, trk = 1;
	static const char *const sname[] = { "serving", "not reached yet",
		"master fallback", "not used" };
	static const char *const rname[] = { "", "master", "replica", "other" };

	if (!uc)
		return;
	for (k = 0; k < uc->nsrv; k++) {
		const struct up_srv *s = &uc->srv[k];

		up += __atomic_load_n(&s->conns_up, __ATOMIC_RELAXED);
		ents += (long long)pcache_ht_entries(s->shadow);
		oth += ST_GET(s->other_server);
		/* one upstream: its tracking, as 0.6.0 said it; several:
		 * every one in use tracked (S382) */
		if (__atomic_load_n(&s->inv_id, __ATOMIC_ACQUIRE) <= 0 &&
		    (uc->nsrv == 1 || srv_state(s) != PC_UPS_OFF))
			trk = 0;
	}
	pc_tw_key(out, "upstream");
	pc_tw_map(out);
	/* the first upstream's address, as 0.6.0 had its only one */
	pc_tw_key(out, "host");
	pc_tw_str(out, uc->srv[0].host);
	pc_tw_key(out, "port");
	pc_tw_i64(out, uc->srv[0].port);
	pc_tw_key(out, "db");
	pc_tw_i64(out, uc->srv[0].db);
	/* S382: each upstream - what it is, what it may do, its share */
	pc_tw_key(out, "role_required");
	pc_tw_str(out, uc->role_req == PC_UP_ROLE_REPLICA ? "replica" : "any");
	pc_tw_key(out, "master_fallback");
	pc_tw_bool(out, uc->master_fallback);
	{
		/* S382 part 2: the mode, and in failover who is active */
		int act, fo = uc->mode == PC_UP_MODE_FAILOVER;
		long long since;
		unsigned long long nfo;

		pthread_mutex_lock(&uc->mx);
		act = uc->active;
		since = uc->active_since;
		nfo = uc->failovers;
		pthread_mutex_unlock(&uc->mx);
		pc_tw_key(out, "mode");
		pc_tw_str(out, fo ? "failover" : "balance");
		if (fo) {
			pc_tw_key(out, "primary");
			pc_tw_str(out, uc->srv[uc->primary].addr);
			pc_tw_key(out, "preempt");
			pc_tw_bool(out, uc->preempt);
			pc_tw_key(out, "preempt_hold_s");
			pc_tw_i64(out, uc->hold_ms / 1000);
			pc_tw_key(out, "active");
			pc_tw_str(out, act >= 0 ? uc->srv[act].addr : "");
			pc_tw_key(out, "active_since_ms");
			pc_tw_i64(out, since);
			pc_tw_key(out, "failovers");
			pc_tw_i64(out, (long long)nfo);
		}
	}
	/* S384: reads by another perfcached's read-through - a daisy-chain
	 * cut here, at one hop (5 min window) */
	pc_tw_ctr_col(out, col, collen, "up.", "chained_reads", ST_GET(uc->chained));
	pc_tw_key(out, "upstreams");
	pc_tw_arr(out);
	for (k = 0; k < uc->nsrv; k++) {
		const struct up_srv *s = &uc->srv[k];
		int st = srv_state(s), ro = __atomic_load_n(&s->role,
			__ATOMIC_RELAXED);
		long long lag = __atomic_load_n(&s->lag_ms, __ATOMIC_RELAXED);
		char sub[24];               /* "up.s" + an int + "." */

		pc_tw_map(out);
		pc_tw_key(out, "addr");
		pc_tw_str(out, s->addr);
		pc_tw_key(out, "role");
		pc_tw_str(out, ro >= 0 && ro <= 3 ? rname[ro] : "");
		pc_tw_key(out, "state");
		pc_tw_str(out, st >= 0 && st <= 3 ? sname[st] : "");
		pc_tw_key(out, "why");
		pc_tw_str(out, srv_why(s));
		pc_tw_key(out, "connections_up");
		pc_tw_i64(out, __atomic_load_n(&s->conns_up, __ATOMIC_RELAXED));
		/* S382 part 2: the failover roles; S384: a perfcached */
		pc_tw_key(out, "primary");
		pc_tw_bool(out, uc->mode == PC_UP_MODE_FAILOVER && k == uc->primary);
		pc_tw_key(out, "active");
		pc_tw_bool(out, uc->mode == PC_UP_MODE_FAILOVER &&
			k == __atomic_load_n(&uc->active, __ATOMIC_RELAXED));
		pc_tw_key(out, "perfcached");
		pc_tw_bool(out, __atomic_load_n(&s->is_pc, __ATOMIC_RELAXED));
		pc_tw_key(out, "tracking");
		pc_tw_bool(out, __atomic_load_n(&s->inv_id, __ATOMIC_ACQUIRE) > 0);
		/* each with its five-minute window: the page's share of
		 * fetches and average fetch are the last five minutes' */
		snprintf(sub, sizeof sub, "up.s%d.", k);
		pc_tw_ctr_col(out, col, collen, sub, "fetches", ST_GET(s->fetches));
		pc_tw_ctr_col(out, col, collen, sub, "errors",
			ST_GET(s->errors) + ST_GET(s->timeouts));
		pc_tw_ctr_col(out, col, collen, sub, "fetch_us_sum",
			ST_GET(s->lat_us_sum));
		pc_tw_ctr_col(out, col, collen, sub, "fetch_n", ST_GET(s->lat_n));
		pc_tw_key(out, "lag_ms");
		if (lag >= 0)
			pc_tw_i64(out, lag);
		else
			pc_tw_nil(out);
		pc_tw_key(out, "shadow_entries");
		pc_tw_i64(out, (long long)pcache_ht_entries(s->shadow));
		pc_tw_key(out, "flushes");
		pc_tw_i64(out, (long long)ST_GET(s->flushes));
		pc_tw_end(out);
	}
	pc_tw_end(out);
	pc_tw_key(out, "connections_up");
	pc_tw_i64(out, up);
	pc_tw_key(out, "connects");
	pc_tw_i64(out, (long long)UPV(uc, connects));
	pc_tw_key(out, "held");
	pc_tw_i64(out, (long long)UPV(uc, held));
	pc_tw_key(out, "joined");
	pc_tw_i64(out, (long long)UPV(uc, joined));
	pc_tw_ctr_col(out, col, collen, "up.", "fetches", UPV(uc, fetches));
	pc_tw_ctr_col(out, col, collen, "up.", "fill_string", UPV(uc, fill_string));
	pc_tw_ctr_col(out, col, collen, "up.", "fill_hash", UPV(uc, fill_hash));
	pc_tw_ctr_col(out, col, collen, "up.", "fill_json", UPV(uc, fill_json));
	pc_tw_ctr_col(out, col, collen, "up.", "absent", UPV(uc, absent));
	pc_tw_key(out, "wrongtype");
	pc_tw_i64(out, (long long)UPV(uc, wrongtype));
	pc_tw_key(out, "too_big");
	pc_tw_i64(out, (long long)UPV(uc, toobig));
	pc_tw_ctr_col(out, col, collen, "up.", "errors", UPV(uc, errors));
	pc_tw_ctr_col(out, col, collen, "up.", "timeouts", UPV(uc, timeouts));
	pc_tw_ctr_col(out, col, collen, "up.", "shadow_hits", UPV(uc, shadow_hits));
	pc_tw_ctr_col(out, col, collen, "up.", "negative_hits", UPV(uc, neg_hits));
	/* ST2: keys found present by TYPE + PTTL alone (a probe asked), the
	 * probes those markers answered, and fetches queued again because a
	 * read joined one after it had stopped at the marker */
	pc_tw_ctr_col(out, col, collen, "up.", "fill_present", UPV(uc, fill_present));
	pc_tw_ctr_col(out, col, collen, "up.", "present_hits", UPV(uc, present_hits));
	pc_tw_key(out, "refetches");
	pc_tw_i64(out, (long long)UPV(uc, refetches));
	pc_tw_key(out, "writes");                                    /* S348 */
	pc_tw_str(out, uc->refuse ? "refuse" : "allow");
	pc_tw_ctr_col(out, col, collen, "up.", "writes_refused", UPV(uc, writes_refused));
	/* every upstream in use tracked (S382) */
	pc_tw_key(out, "tracking");
	pc_tw_bool(out, trk);
	{
		char run[41];

		pthread_mutex_lock(&uc->srv[0].run_mx);
		memcpy(run, uc->srv[0].inv_run, sizeof run);
		pthread_mutex_unlock(&uc->srv[0].run_mx);
		pc_tw_key(out, "server");
		pc_tw_str(out, run);
	}
	pc_tw_key(out, "tracking_other_server");
	pc_tw_i64(out, (long long)oth);
	pc_tw_key(out, "invalidations");
	pc_tw_i64(out, (long long)UPV(uc, invalidations));
	pc_tw_key(out, "flushes");
	pc_tw_i64(out, (long long)UPV(uc, flushes));
	pc_tw_key(out, "untracked_fills");
	pc_tw_i64(out, (long long)UPV(uc, untracked));
	pc_tw_key(out, "drain");
	pc_tw_map(out);
	pc_tw_key(out, "state");
	pc_tw_str(out, updr_name[uc->drain_state]);
	pc_tw_key(out, "started");
	pc_tw_i64(out, uc->drain_t0);
	pc_tw_key(out, "finished");
	pc_tw_i64(out, uc->drain_t1);
	pc_tw_key(out, "read");
	pc_tw_i64(out, (long long)ST_GET(uc->dr_scanned));
	pc_tw_key(out, "taken_over");
	pc_tw_i64(out, (long long)ST_GET(uc->dr_pulled));
	pc_tw_key(out, "fleet_had");
	pc_tw_i64(out, (long long)ST_GET(uc->dr_owned));
	pc_tw_key(out, "gone");
	pc_tw_i64(out, (long long)ST_GET(uc->dr_absent));
	pc_tw_key(out, "wrongtype");
	pc_tw_i64(out, (long long)ST_GET(uc->dr_wrongtype));
	pc_tw_key(out, "too_big");
	pc_tw_i64(out, (long long)ST_GET(uc->dr_toobig));
	pc_tw_key(out, "errors");
	pc_tw_i64(out, (long long)ST_GET(uc->dr_errors));
	pc_tw_end(out);
	pc_tw_key(out, "promoted");
	pc_tw_i64(out, (long long)UPV(uc, promoted));
	pc_tw_key(out, "nofall_written");
	pc_tw_i64(out, (long long)UPV(uc, nofall_written));
	pc_tw_key(out, "nofall_hits");
	pc_tw_i64(out, (long long)UPV(uc, nofall_hits));
	pc_tw_key(out, "ownership_checks");
	pc_tw_i64(out, (long long)UPV(uc, own_checks));
	pc_tw_key(out, "ownership_found");
	pc_tw_i64(out, (long long)UPV(uc, own_found));
	pc_tw_key(out, "fill_failed");
	pc_tw_i64(out, (long long)UPV(uc, fill_failed));
	pc_tw_key(out, "shadow_entries");
	pc_tw_i64(out, ents);
	n = UPV(uc, lat_n);
	pc_tw_key(out, "fetch_us_avg");
	pc_tw_i64(out, n ? (long long)(UPV(uc, lat_us_sum) / n) : 0);
	/* S351: the sum and the count, each with its window - the page's
	 * average fetch is the last five minutes' */
	pc_tw_ctr_col(out, col, collen, "up.", "fetch_us_sum", UPV(uc, lat_us_sum));
	pc_tw_ctr_col(out, col, collen, "up.", "fetch_n", n);
	pc_tw_end(out);
}

/* S358: the stats reset - every collection's counters count from here in
 * /stats (not /metrics, which does not carry them).  The daemon serialises
 * resets; a worker counting concurrently lands on either side of it. */
void pc_upstream_stats_reset(void)
{
	int i;

	for (i = 0; i < UP_MAX_COLS; i++) {
		struct up_col *uc = &cols[i];

		uc->base.held = ST_GET(uc->held);
		uc->base.fetches = ST_GET(uc->fetches);
		uc->base.fill_string = ST_GET(uc->fill_string);
		uc->base.fill_hash = ST_GET(uc->fill_hash);
		uc->base.fill_json = ST_GET(uc->fill_json);
		uc->base.absent = ST_GET(uc->absent);
		uc->base.wrongtype = ST_GET(uc->wrongtype);
		uc->base.toobig = ST_GET(uc->toobig);
		uc->base.errors = ST_GET(uc->errors);
		uc->base.timeouts = ST_GET(uc->timeouts);
		uc->base.connects = ST_GET(uc->connects);
		uc->base.shadow_hits = ST_GET(uc->shadow_hits);
		uc->base.neg_hits = ST_GET(uc->neg_hits);
		uc->base.fill_present = ST_GET(uc->fill_present);     /* ST2 */
		uc->base.present_hits = ST_GET(uc->present_hits);
		uc->base.refetches = ST_GET(uc->refetches);
		uc->base.joined = ST_GET(uc->joined);
		uc->base.fill_failed = ST_GET(uc->fill_failed);
		uc->base.lat_us_sum = ST_GET(uc->lat_us_sum);
		uc->base.lat_n = ST_GET(uc->lat_n);
		uc->base.promoted = ST_GET(uc->promoted);
		uc->base.nofall_written = ST_GET(uc->nofall_written);
		uc->base.nofall_hits = ST_GET(uc->nofall_hits);
		uc->base.own_checks = ST_GET(uc->own_checks);
		uc->base.own_found = ST_GET(uc->own_found);
		uc->base.writes_refused = ST_GET(uc->writes_refused);
		uc->base.invalidations = ST_GET(uc->invalidations);
		uc->base.flushes = ST_GET(uc->flushes);
		uc->base.untracked = ST_GET(uc->untracked);
	}
}
