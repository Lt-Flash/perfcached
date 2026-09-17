/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * pubsub.c - the pub/sub engine.  See pubsub.h for the contract and
 * doc/DESIGN.md 12eb (PS1, PS9) for why it is shaped this way.
 *
 * WHO OWNS WHAT.  A subscription belongs to a connection, and a connection
 * belongs to one worker for its whole life - every subscribe, unsubscribe
 * and close runs on that worker.  So each worker keeps its OWN tables (a
 * shard): its channel and pattern entries, their subscriber lists, its
 * subscribed connections.  Nothing else ever touches a shard, so the shard
 * needs no lock, and delivery - which runs on the owner too - walks it
 * directly and writes the frames.
 *
 * WHAT A PUBLISHER NEEDS is not the subscribers themselves but which
 * workers hold any, and how many subscriptions there are (PUBLISH's
 * answer).  That is the SHARED index: one entry per distinct name, with a
 * subscription count and a bitmap of owning workers, patterns filed by
 * literal prefix (psindex.h).  Publishers read it without a lock.  It is
 * written only when a worker gains its first or loses its last
 * subscription to a name, under one writer mutex, and a removed entry is
 * freed only after every reader has passed a quiescent point (quiesce.h).
 * A reader that is not inside the quiescence protocol takes the writer
 * mutex instead.
 *
 * WHY.  The first engine guarded everything with one mutex: every publish
 * resolved under it, every worker's delivery resolved again under it, and
 * every RESP command took it to ask "is this connection subscribed".
 * Measured on 12 cores (bench/psengine): 2.37M publishes/s on one worker
 * fell to 122k/s TOTAL on eight, and a thousand patterns - every one
 * globbed on every publish - brought eight workers to 7k/s with a
 * SUBSCRIBE waiting up to 6.4 ms.
 *
 * DELIVERY.  A publish on worker A delivers inline to A's own subscribers
 * and queues the message for every other owning worker, waking it through
 * its eventfd only when its queue was empty.  The message is
 * reference-counted across the workers it was queued to.
 *
 * SLOW SUBSCRIBERS die at the output cap (proto.c refuses the append),
 * which is redis's client-output-buffer-limit behaviour.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/random.h>
#include <unistd.h>

#include "psindex.h"
#include "psinterest.h"
#include "pubsub.h"
#include "quiesce.h"
#define PS_MAX_WORKERS    512
#define PS_OWNER_WORDS    8                /* 512 owner bits */
#define PS_NAME_MAX       4096
#define PS_SHARED_BUCKETS 65536            /* shared tables cannot grow */
#define PS_SHARD_BUCKETS  64               /* private tables grow */
#define PS_CONN_BUCKETS   256

#define LOAD(x)  __atomic_load_n(&(x), __ATOMIC_ACQUIRE)

/* daemon.c's accessor for the calling thread's worker; declared here to keep
 * this file's includes to its own contract */
int pc_worker_id(void);

/* ---- the shared index: one entry per distinct name on this node ---------- */
struct ps_shared {
	struct psi_node node;              /* first: the index hands it back */
	unsigned long long count;          /* atomic: subscriptions on this node */
	uint64_t owners[PS_OWNER_WORDS];   /* atomic: workers holding at least one */
	int pattern;
	struct ps_shared *retired_next;
	unsigned long long stamp;          /* when it was unlinked */
	char name[];
};

/* ---- a worker's shard ------------------------------------------------------ */
struct ps_sub;

struct ps_local {                      /* one per name per worker */
	struct psi_node node;              /* first; .name is the shared entry's */
	struct ps_shared *shared;
	struct ps_sub *subs;
	int nsubs;
};

struct ps_conn {
	void *conn;
	int nchan, npat;
	struct ps_sub *subs;
	struct ps_conn *next;              /* its registry bucket */
};

struct ps_sub {
	struct ps_sub *next_in_entry;      /* the local entry's subscribers */
	struct ps_sub *next_in_conn;       /* the connection's subscriptions */
	struct ps_local *entry;
	struct ps_conn *owner;
};

struct ps_shard {
	struct psi_table ch, pat;          /* private */
	struct ps_conn *conntab[PS_CONN_BUCKETS];
	int nconns;                        /* subscribed connections here */
};

struct ps_msg {
	int refs;                          /* workers still to deliver it */
	int relayed;
	size_t clen, dlen;
	char *frame;                       /* the "message" frame, built once */
	size_t flen;
	char *chan;
	char *data;
};

struct ps_qnode {
	struct ps_qnode *next;
	struct ps_msg *m;
	size_t bytes;                      /* what it charged its queue */
};

struct ps_wq {
	pthread_mutex_t lk;
	struct ps_qnode *head, *tail;
	int efd;
	int n;
	size_t bytes;
};

static pthread_mutex_t ps_wlk = PTHREAD_MUTEX_INITIALIZER;
static struct psi_table ps_sch, ps_spat;       /* shared: names, patterns */
static struct ps_shared *ps_retired, *ps_retired_tail;
static unsigned ps_nretired;                   /* atomic */
static struct ps_shard *ps_shards;
static int ps_nworkers;
static long ps_nsubs;                          /* atomic */
static struct ps_wq ps_wq[PS_MAX_WORKERS];
static unsigned long long ps_published, ps_delivered, ps_slow_kills, ps_queued;
static unsigned long long ps_relay_sent, ps_relay_recv, ps_relay_lost,
	ps_relay_dropped;
static unsigned long long ps_alloc_failed, ps_relay_dup, ps_keyspace;
/* QUEUES.  A worker's queue used to have no bound and its drain no limit:
 * on the two-host rig, relays arriving faster than one subscriber could be
 * written grew a queue that the worker then emptied in ONE pass, serving
 * nothing else and not noticing SIGTERM - the node took 12.9 s to exit.
 * The relay plane (PS11) made that likely, because relays that used to
 * drop at the socket now reach the queues.  So a queue has a byte cap,
 * past which a message for that worker is dropped and counted (delivery
 * is at-most-once), and a drain takes a bounded batch and wakes its worker
 * again for the rest. */
#define PS_DRAIN_BATCH 1024
static size_t ps_queue_cap = (size_t)16 << 20;
static unsigned long long ps_queue_dropped;
/* PS12: what this node tells its peers it holds (psinterest.h).  The filter
 * and the counters change under ps_wlk, where first joins and last leaves
 * already happen; the version is read by the heartbeat without it. */
#define PS_INT_REBUILD_MS 30000
static int ps_int_on;
static uint16_t ps_int_epoch, ps_int_rebuild;
static uint32_t ps_int_adds;
static uint64_t ps_int_version;                /* atomic; 0 = relay me everything */
static uint8_t ps_int_bloom[PSB_BYTES];
static int ps_int_removed;                     /* a name left since the rebuild */
static long long ps_int_rebuilt_ms;
int pc_pubsub_match(const char *p, size_t n, const char *s, size_t len)
{
	return psi_glob(p, n, s, len);
}

static struct ps_shard *shard_of(int worker)
{
	return ps_shards && worker >= 0 && worker < ps_nworkers
		? &ps_shards[worker] : NULL;
}

/* ---- reading the shared index ------------------------------------------- */

static int read_begin(void)
{
	if (pc_qs_inside())
		return 0;
	pthread_mutex_lock(&ps_wlk);           /* not protected: lock instead */
	return 1;
}

static void read_end(int locked)
{
	if (locked)
		pthread_mutex_unlock(&ps_wlk);
}

/* ---- writing it (ps_wlk held) --------------------------------------------- */

static void reclaim(void)
{
	while (ps_retired && pc_qs_clear(ps_retired->stamp)) {
		struct ps_shared *e = ps_retired;

		ps_retired = e->retired_next;
		if (!ps_retired)
			ps_retired_tail = NULL;
		free(e);
		__atomic_sub_fetch(&ps_nretired, 1, __ATOMIC_RELAXED);
	}
}

static struct ps_shared *shared_join(const char *name, size_t nlen,
		int pattern, int worker)
{
	struct psi_table *t = pattern ? &ps_spat : &ps_sch;
	struct psi_node *x;
	struct ps_shared *e;
	uint64_t told = 0;
	uint32_t h1 = 0, h2 = 0;

	pthread_mutex_lock(&ps_wlk);
	x = psi_find(t, name, nlen);
	if (x) {
		e = (struct ps_shared *)x;
	} else {
		e = calloc(1, sizeof *e + nlen + 1);
		if (!e) {
			pthread_mutex_unlock(&ps_wlk);
			return NULL;
		}
		memcpy(e->name, name, nlen);
		e->name[nlen] = 0;
		e->node.name = e->name;
		e->node.nlen = (uint32_t)nlen;
		e->pattern = pattern;
		psi_insert(t, &e->node);
		if (ps_int_on) {                   /* PS12: the node's first */
			if (!pattern) {
				psb_hash(name, nlen, &h1, &h2);
				psb_add(ps_int_bloom, h1, h2);
			}
			told = psv_make(ps_int_epoch, ps_int_rebuild, ++ps_int_adds);
			__atomic_store_n(&ps_int_version, told, __ATOMIC_RELEASE);
		}
	}
	__atomic_or_fetch(&e->owners[worker / 64], 1ULL << (worker % 64),
		__ATOMIC_ACQ_REL);
	reclaim();
	pthread_mutex_unlock(&ps_wlk);
	/* outside the lock: two first joins may reach a peer out of order,
	 * which its window absorbs (psv_add) */
	if (told)
		pc_cluster_pubsub_interest_add(told, pattern, name, nlen, h1, h2);
	return e;
}

static void shared_leave(struct ps_shared *e, int worker)
{
	int i, empty = 1;

	pthread_mutex_lock(&ps_wlk);
	__atomic_and_fetch(&e->owners[worker / 64], ~(1ULL << (worker % 64)),
		__ATOMIC_ACQ_REL);
	for (i = 0; i < PS_OWNER_WORDS; i++)
		if (LOAD(e->owners[i]))
			empty = 0;
	if (empty) {
		ps_int_removed = 1;                /* PS12: the next rebuild drops it */
		psi_remove(e->pattern ? &ps_spat : &ps_sch, &e->node);
		e->stamp = pc_qs_stamp();          /* after the unlink */
		e->retired_next = NULL;
		if (ps_retired_tail)
			ps_retired_tail->retired_next = e;
		else
			ps_retired = e;
		ps_retired_tail = e;
		__atomic_add_fetch(&ps_nretired, 1, __ATOMIC_RELAXED);
	}
	reclaim();
	pthread_mutex_unlock(&ps_wlk);
}

/* ---- a shard's connections ------------------------------------------------ */

static unsigned conn_bucket(void *conn)
{
	uintptr_t v = (uintptr_t)conn;

	v ^= v >> 17;
	return (unsigned)((v >> 4) % PS_CONN_BUCKETS);
}

static struct ps_conn *conn_find(struct ps_shard *sh, void *conn, int create)
{
	struct ps_conn *pc, **head = &sh->conntab[conn_bucket(conn)];

	for (pc = *head; pc; pc = pc->next)
		if (pc->conn == conn)
			return pc;
	if (!create)
		return NULL;
	pc = calloc(1, sizeof *pc);
	if (!pc)
		return NULL;
	pc->conn = conn;
	pc->next = *head;
	*head = pc;
	sh->nconns++;
	/* from here the connection needs the engine when it closes; one that
	 * never got this far closes without it */
	pc_conn_subs_note(conn, 0);
	return pc;
}

static void conn_drop_if_empty(struct ps_shard *sh, struct ps_conn *pc)
{
	struct ps_conn **pp;

	if (pc->subs)
		return;
	for (pp = &sh->conntab[conn_bucket(pc->conn)]; *pp; pp = &(*pp)->next)
		if (*pp == pc) {
			*pp = pc->next;
			break;
		}
	sh->nconns--;
	free(pc);
}

static void local_drop_if_empty(struct ps_shard *sh, struct ps_local *le,
		int worker)
{
	struct ps_shared *se = le->shared;

	if (le->nsubs)
		return;
	psi_remove(se->pattern ? &sh->pat : &sh->ch, &le->node);
	free(le);
	shared_leave(se, worker);
}

static void sub_unlink(struct ps_shard *sh, struct ps_sub *s, int worker)
{
	struct ps_sub **pp;
	struct ps_local *le = s->entry;

	for (pp = &le->subs; *pp; pp = &(*pp)->next_in_entry)
		if (*pp == s) { *pp = s->next_in_entry; break; }
	for (pp = &s->owner->subs; *pp; pp = &(*pp)->next_in_conn)
		if (*pp == s) { *pp = s->next_in_conn; break; }
	le->nsubs--;
	if (le->shared->pattern)
		s->owner->npat--;
	else
		s->owner->nchan--;
	__atomic_sub_fetch(&le->shared->count, 1, __ATOMIC_ACQ_REL);
	__atomic_sub_fetch(&ps_nsubs, 1, __ATOMIC_RELAXED);
	pc_conn_subs_note(s->owner->conn, s->owner->nchan + s->owner->npat);   /* S160 */
	free(s);
	local_drop_if_empty(sh, le, worker);
}

/* ---- lifecycle ------------------------------------------------------------ */

int pc_pubsub_init(int nworkers)
{
	int i;

	if (nworkers < 1)
		nworkers = 1;
	if (nworkers > PS_MAX_WORKERS)
		nworkers = PS_MAX_WORKERS;
	if (psi_init(&ps_sch, PS_SHARED_BUCKETS, 0) != 0 ||
	        psi_init(&ps_spat, PS_SHARED_BUCKETS, PSI_PATTERNS) != 0)
		return -1;
	ps_shards = calloc((size_t)nworkers, sizeof *ps_shards);
	if (!ps_shards)
		return -1;
	for (i = 0; i < nworkers; i++)
		if (psi_init(&ps_shards[i].ch, PS_SHARD_BUCKETS, PSI_PRIVATE) != 0 ||
		        psi_init(&ps_shards[i].pat, PS_SHARD_BUCKETS,
		                PSI_PRIVATE | PSI_PATTERNS) != 0)
			return -1;
	ps_nworkers = nworkers;
	for (i = 0; i < PS_MAX_WORKERS; i++) {
		pthread_mutex_init(&ps_wq[i].lk, NULL);
		ps_wq[i].efd = -1;
	}
	return 0;
}

void pc_pubsub_worker_register(int worker, int efd)
{
	if (worker < 0 || worker >= ps_nworkers)
		return;
	ps_wq[worker].efd = efd;
}

/* ---- the subscription side (the owning worker) ---------------------------- */

static struct ps_sub *conn_sub_find(struct ps_conn *pc, const char *name,
		size_t nlen, int pattern)
{
	struct ps_sub *s;

	for (s = pc->subs; s; s = s->next_in_conn)
		if (s->entry->shared->pattern == pattern &&
		    s->entry->node.nlen == nlen &&
		    !memcmp(s->entry->node.name, name, nlen))
			return s;
	return NULL;
}

int pc_pubsub_subscribe(void *conn, int worker, const char *name,
		size_t nlen, int pattern)
{
	struct ps_shard *sh = shard_of(worker);
	struct psi_table *t;
	struct psi_node *x;
	struct ps_conn *pc;
	struct ps_local *le;
	struct ps_sub *s;
	int n;

	if (!sh || nlen == 0 || nlen > PS_NAME_MAX)
		return -1;
	pc = conn_find(sh, conn, 1);
	if (!pc)
		return -1;
	if (conn_sub_find(pc, name, nlen, pattern))  /* redis: the unchanged count */
		return pc->nchan + pc->npat;
	t = pattern ? &sh->pat : &sh->ch;
	x = psi_find(t, name, nlen);
	if (x) {
		le = (struct ps_local *)x;
	} else {
		struct ps_shared *se = shared_join(name, nlen, pattern, worker);

		le = se ? calloc(1, sizeof *le) : NULL;
		if (!le) {
			if (se)
				shared_leave(se, worker);
			conn_drop_if_empty(sh, pc);
			return -1;
		}
		le->shared = se;
		le->node.name = se->name;
		le->node.nlen = (uint32_t)nlen;
		psi_insert(t, &le->node);
	}
	s = calloc(1, sizeof *s);
	if (!s) {
		local_drop_if_empty(sh, le, worker);
		conn_drop_if_empty(sh, pc);
		return -1;
	}
	s->entry = le;
	s->owner = pc;
	s->next_in_entry = le->subs;
	le->subs = s;
	s->next_in_conn = pc->subs;
	pc->subs = s;
	le->nsubs++;
	if (pattern)
		pc->npat++;
	else
		pc->nchan++;
	__atomic_add_fetch(&le->shared->count, 1, __ATOMIC_ACQ_REL);
	__atomic_add_fetch(&ps_nsubs, 1, __ATOMIC_RELAXED);
	n = pc->nchan + pc->npat;
	pc_conn_subs_note(conn, n);                    /* S160 */
	return n;
}

int pc_pubsub_unsubscribe(void *conn, const char *name, size_t nlen,
		int pattern)
{
	int worker = pc_worker_id();
	struct ps_shard *sh = shard_of(worker);
	struct ps_conn *pc;
	struct ps_sub *s;
	int n;

	if (!sh || !sh->nconns || !(pc = conn_find(sh, conn, 0)))
		return 0;
	s = conn_sub_find(pc, name, nlen, pattern);
	if (s)
		sub_unlink(sh, s, worker);
	n = pc->nchan + pc->npat;
	conn_drop_if_empty(sh, pc);
	return n;
}

int pc_pubsub_pop(void *conn, int pattern, char *name, size_t cap,
		size_t *nlen, int *left)
{
	int worker = pc_worker_id();
	struct ps_shard *sh = shard_of(worker);
	struct ps_conn *pc;
	struct ps_sub *s;

	if (!sh || !sh->nconns || !(pc = conn_find(sh, conn, 0)))
		return 0;
	for (s = pc->subs; s; s = s->next_in_conn)
		if (s->entry->shared->pattern == pattern) {
			size_t n = s->entry->node.nlen < cap ? s->entry->node.nlen : cap;

			memcpy(name, s->entry->node.name, n);
			*nlen = n;
			sub_unlink(sh, s, worker);
			*left = pc->nchan + pc->npat;
			conn_drop_if_empty(sh, pc);
			return 1;
		}
	return 0;
}

/* on every RESP command (the subscribed-mode gate): a worker with no
 * subscribed connection answers without looking anything up */
int pc_pubsub_count(void *conn)
{
	struct ps_shard *sh = shard_of(pc_worker_id());
	struct ps_conn *pc;

	if (!sh || !sh->nconns || !(pc = conn_find(sh, conn, 0)))
		return 0;
	return pc->nchan + pc->npat;
}

void pc_pubsub_conn_closed(void *conn)
{
	int worker = pc_worker_id();
	struct ps_shard *sh = shard_of(worker);
	struct ps_conn *pc;

	if (!sh || !sh->nconns || !(pc = conn_find(sh, conn, 0)))
		return;
	while (pc->subs)
		sub_unlink(sh, pc->subs, worker);
	conn_drop_if_empty(sh, pc);
}

/* ---- frames --------------------------------------------------------------
 * RESP2 arrays, the one wire every subscriber speaks today:
 *   ["message", channel, payload]
 *   ["pmessage", pattern, channel, payload] */

static size_t bulk_len(size_t n)
{
	size_t d = 1, v = n;

	while (v >= 10) { v /= 10; d++; }
	return 1 + d + 2 + n + 2;          /* $<d>\r\n<n bytes>\r\n */
}

static char *put_bulk(char *p, const char *s, size_t n)
{
	int k = 0;
	char tmp[24];
	size_t v = n;

	*p++ = '$';
	do { tmp[k++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (k)
		*p++ = tmp[--k];
	*p++ = '\r'; *p++ = '\n';
	memcpy(p, s, n); p += n;
	*p++ = '\r'; *p++ = '\n';
	return p;
}

static char *frame_message(const char *chan, size_t clen, const char *data,
		size_t dlen, size_t *flen)
{
	size_t n = 4 + bulk_len(7) + bulk_len(clen) + bulk_len(dlen);
	char *f = malloc(n), *p = f;

	if (!f)
		return NULL;
	memcpy(p, "*3\r\n", 4); p += 4;
	p = put_bulk(p, "message", 7);
	p = put_bulk(p, chan, clen);
	p = put_bulk(p, data, dlen);
	*flen = (size_t)(p - f);
	return f;
}

static char *frame_pmessage(const char *pat, size_t plen, const char *chan,
		size_t clen, const char *data, size_t dlen, size_t *flen)
{
	size_t n = 4 + bulk_len(8) + bulk_len(plen) + bulk_len(clen) + bulk_len(dlen);
	char *f = malloc(n), *p = f;

	if (!f)
		return NULL;
	memcpy(p, "*4\r\n", 4); p += 4;
	p = put_bulk(p, "pmessage", 8);
	p = put_bulk(p, pat, plen);
	p = put_bulk(p, chan, clen);
	p = put_bulk(p, data, dlen);
	*flen = (size_t)(p - f);
	return f;
}

/* ---- delivery (the owning worker, its own shard, no lock) ----------------- */

static void msg_release(struct ps_msg *m)
{
	if (__atomic_sub_fetch(&m->refs, 1, __ATOMIC_ACQ_REL) > 0)
		return;
	free(m->frame);
	free(m->chan);
	free(m->data);
	free(m);
}

/* only a subscriber whose output is at the cap is a slow kill, and only
 * once; our own allocation failure never closes a healthy connection */
static void account(void *conn, int rc)
{
	if (rc == 0)
		__atomic_fetch_add(&ps_delivered, 1, __ATOMIC_RELAXED);
	else if (rc == PC_PS_PUSH_NOMEM)
		__atomic_fetch_add(&ps_alloc_failed, 1, __ATOMIC_RELAXED);
	else if (pc_conn_kill(conn, rc == PC_PS_PUSH_SLOW
	        ? "slow subscriber" : "write error") && rc == PC_PS_PUSH_SLOW)
		__atomic_fetch_add(&ps_slow_kills, 1, __ATOMIC_RELAXED);
}

/* A push never unlinks a subscription: a kill only shuts the socket down
 * and the close runs later in the worker's loop, so the lists walked here
 * stay put while their frames are written. */
static void deliver_pattern(struct psi_node *node, void *arg)
{
	struct ps_msg *m = arg;
	struct ps_local *le = (struct ps_local *)node;
	struct ps_sub *s;
	size_t flen;
	char *f = frame_pmessage(le->node.name, le->node.nlen, m->chan,
		m->clen, m->data, m->dlen, &flen);

	for (s = le->subs; s; s = s->next_in_entry)
		account(s->owner->conn, f ? pc_conn_pubsub_push(s->owner->conn,
			le->node.name, le->node.nlen, m->chan, m->clen, m->data,
			m->dlen, f, flen) : PC_PS_PUSH_NOMEM);
	free(f);
}

static void deliver_local(int worker, struct ps_msg *m)
{
	struct ps_shard *sh = shard_of(worker);
	struct psi_node *x;
	struct ps_sub *s;

	if (!sh || !sh->nconns)
		return;
	x = psi_find(&sh->ch, m->chan, m->clen);
	if (x)
		for (s = ((struct ps_local *)x)->subs; s; s = s->next_in_entry)
			account(s->owner->conn, pc_conn_pubsub_push(s->owner->conn,
				NULL, 0, m->chan, m->clen, m->data, m->dlen,
				m->frame, m->flen));
	if (sh->pat.n)
		psi_match(&sh->pat, m->chan, m->clen, deliver_pattern, m);
}

/* ---- publish ------------------------------------------------------------ */

struct resolve {
	unsigned long long count;
	uint64_t owners[PS_OWNER_WORDS];
};

static void resolve_one(struct psi_node *node, void *arg)
{
	struct resolve *r = arg;
	struct ps_shared *e = (struct ps_shared *)node;
	int i;

	r->count += LOAD(e->count);
	for (i = 0; i < PS_OWNER_WORDS; i++)
		r->owners[i] |= LOAD(e->owners[i]);
}

static void enqueue(int w, struct ps_msg *m)
{
	struct ps_qnode *q = malloc(sizeof *q);
	struct ps_wq *wq = &ps_wq[w];
	uint64_t one = 1;
	int wake;

	if (!q) {
		__atomic_fetch_add(&ps_alloc_failed, 1, __ATOMIC_RELAXED);
		msg_release(m);
		return;
	}
	q->m = m;
	q->next = NULL;
	q->bytes = sizeof *q + sizeof *m + m->clen + m->dlen + m->flen;
	pthread_mutex_lock(&wq->lk);
	if (ps_queue_cap && wq->bytes + q->bytes > ps_queue_cap) {
		pthread_mutex_unlock(&wq->lk);
		__atomic_fetch_add(&ps_queue_dropped, 1, __ATOMIC_RELAXED);
		free(q);
		msg_release(m);
		return;
	}
	__atomic_add_fetch(&wq->bytes, q->bytes, __ATOMIC_RELAXED);
	/* the worker reads its eventfd before it drains, so one write per
	 * empty-to-non-empty is enough - every message after it rides it */
	wake = wq->n == 0;
	if (wq->tail)
		wq->tail->next = q;
	else
		wq->head = q;
	wq->tail = q;
	wq->n++;
	pthread_mutex_unlock(&wq->lk);
	__atomic_fetch_add(&ps_queued, 1, __ATOMIC_RELAXED);
	if (wake && wq->efd >= 0 && write(wq->efd, &one, sizeof one) < 0)
		{ /* best effort: the next event drains it */ }
}

int pc_pubsub_publish(const char *chan, size_t clen, const char *data,
		size_t dlen, int relayed)
{
	struct resolve r;
	struct psi_node *x;
	struct ps_msg *m;
	int locked, nw = 0, w, i, me = pc_worker_id();

	if (clen == 0 || clen > PS_NAME_MAX)
		return 0;
	__atomic_fetch_add(&ps_published, 1, __ATOMIC_RELAXED);
	if (relayed == 2) {
		__atomic_fetch_add(&ps_keyspace, 1, __ATOMIC_RELAXED);
	} else if (relayed == 1) {
		__atomic_fetch_add(&ps_relay_recv, 1, __ATOMIC_RELAXED);
	} else {
		/* PS3: the fleet first, before "nobody here" can cut this
		 * short - the subscribers may all sit on other nodes.  A
		 * message that arrived from a peer is never relayed again. */
		int rs = pc_cluster_pubsub_relay(chan, clen, data, dlen);

		if (rs > 0)
			__atomic_fetch_add(&ps_relay_sent, (unsigned long long)rs,
				__ATOMIC_RELAXED);
		else if (rs < 0)
			__atomic_fetch_add(&ps_relay_dropped, 1, __ATOMIC_RELAXED);
	}
	memset(&r, 0, sizeof r);
	locked = read_begin();
	x = psi_find(&ps_sch, chan, clen);
	if (x)
		resolve_one(x, &r);
	if (LOAD(ps_spat.n))
		psi_match(&ps_spat, chan, clen, resolve_one, &r);
	read_end(locked);
	if (!r.count)
		return 0;
	for (i = 0; i < PS_OWNER_WORDS; i++)
		nw += __builtin_popcountll(r.owners[i]);
	if (!nw)
		return 0;
	/* a message that cannot be built reaches nobody: answer 0, not the
	 * receivers it would have had, and count each of them */
	m = calloc(1, sizeof *m);
	if (m) {
		m->chan = malloc(clen);
		m->data = malloc(dlen ? dlen : 1);
		m->frame = frame_message(chan, clen, data, dlen, &m->flen);
	}
	if (!m || !m->chan || !m->data || !m->frame) {
		if (m) {
			free(m->chan); free(m->data); free(m->frame); free(m);
		}
		__atomic_fetch_add(&ps_alloc_failed, r.count, __ATOMIC_RELAXED);
		return 0;
	}
	memcpy(m->chan, chan, clen);
	memcpy(m->data, data, dlen);
	m->clen = clen;
	m->dlen = dlen;
	m->relayed = relayed;
	m->refs = nw;                          /* before any delivery releases it */
	for (i = 0; i < PS_OWNER_WORDS; i++) {
		uint64_t v = r.owners[i];

		while (v) {
			w = i * 64 + __builtin_ctzll(v);
			v &= v - 1;
			if (w == me) {
				deliver_local(w, m);
				msg_release(m);
			} else if (w < ps_nworkers) {
				enqueue(w, m);
			} else {
				msg_release(m);
			}
		}
	}
	return r.count > 0x7fffffffULL ? 0x7fffffff : (int)r.count;
}

void pc_pubsub_drain(int worker)
{
	struct ps_wq *wq;
	struct ps_qnode *q, *next;
	int more = 0;

	if (worker < 0 || worker >= ps_nworkers)
		return;
	wq = &ps_wq[worker];
	pthread_mutex_lock(&wq->lk);
	q = wq->head;
	if (q) {
		/* a bounded batch: the rest stays queued and the worker is
		 * woken again, so its other connections and its stop flag are
		 * seen between batches */
		struct ps_qnode *last = q;
		size_t bytes = q->bytes;
		int k = 1;

		while (last->next && k < PS_DRAIN_BATCH) {
			last = last->next;
			bytes += last->bytes;
			k++;
		}
		wq->head = last->next;
		if (!wq->head)
			wq->tail = NULL;
		else
			more = 1;
		last->next = NULL;
		wq->n -= k;
		__atomic_sub_fetch(&wq->bytes, bytes, __ATOMIC_RELAXED);
	}
	pthread_mutex_unlock(&wq->lk);
	for (; q; q = next) {
		next = q->next;
		deliver_local(worker, q->m);
		msg_release(q->m);
		free(q);
	}
	if (more && wq->efd >= 0) {
		uint64_t one = 1;

		if (write(wq->efd, &one, sizeof one) < 0)
			{ /* best effort: the next event drains it */ }
	}
	/* free what the writers retired, if nobody is writing right now */
	if (LOAD(ps_nretired) && pthread_mutex_trylock(&ps_wlk) == 0) {
		reclaim();
		pthread_mutex_unlock(&ps_wlk);
	}
}

void pc_pubsub_set_queue_cap(size_t bytes)
{
	ps_queue_cap = bytes;
}

/* ---- PS12: interest ------------------------------------------------------- */

void pc_pubsub_set_interest(int on)
{
	uint16_t epoch = 0;

	pthread_mutex_lock(&ps_wlk);
	ps_int_on = !!on;
	if (on) {
		while (!epoch && getrandom(&epoch, sizeof epoch, 0) == sizeof epoch)
			;
		ps_int_epoch = epoch ? epoch : 1;
		__atomic_store_n(&ps_int_version,
			psv_make(ps_int_epoch, ps_int_rebuild, ps_int_adds),
			__ATOMIC_RELEASE);
	} else {
		__atomic_store_n(&ps_int_version, 0, __ATOMIC_RELEASE);
	}
	pthread_mutex_unlock(&ps_wlk);
}

uint64_t pc_pubsub_interest_version(void)
{
	return __atomic_load_n(&ps_int_version, __ATOMIC_ACQUIRE);
}

struct int_pats {
	unsigned char *p;
	size_t used, cap;
	unsigned n;
	int over;
};

static int int_pat(struct psi_node *x, void *arg)
{
	struct int_pats *w = arg;

	if (w->used + 2 + x->nlen > w->cap) {
		w->over = 1;
		return 1;
	}
	w->p[w->used] = (unsigned char)(x->nlen >> 8);
	w->p[w->used + 1] = (unsigned char)x->nlen;
	memcpy(w->p + w->used + 2, x->name, x->nlen);
	w->used += 2 + x->nlen;
	w->n++;
	return 0;
}

size_t pc_pubsub_interest_full(unsigned char *out, size_t cap)
{
	struct int_pats w;
	uint64_t v;
	int i;

	if (cap < PSF_HDR)
		return 0;
	pthread_mutex_lock(&ps_wlk);
	v = ps_int_on ? psv_make(ps_int_epoch, ps_int_rebuild, ps_int_adds) : 0;
	for (i = 0; i < 8; i++)
		out[i] = (unsigned char)(v >> (56 - 8 * i));
	memcpy(out + 9, ps_int_bloom, PSB_BYTES);
	w.p = out + PSF_HDR;
	w.used = 0;
	w.cap = cap - PSF_HDR < PSF_PATTERNS_MAX ? cap - PSF_HDR : PSF_PATTERNS_MAX;
	w.n = 0;
	w.over = 0;
	psi_each(&ps_spat, int_pat, &w);
	pthread_mutex_unlock(&ps_wlk);
	if (w.over || w.n > 0xffff) {          /* relay me everything */
		out[8] = PSF_EVERYTHING;
		w.used = 0;
		w.n = 0;
	} else {
		out[8] = 0;
	}
	out[PSF_HDR - 2] = (unsigned char)(w.n >> 8);
	out[PSF_HDR - 1] = (unsigned char)w.n;
	return PSF_HDR + w.used;
}

static int int_rebuild_one(struct psi_node *x, void *arg)
{
	uint32_t h1, h2;

	(void)arg;
	psb_hash(x->name, x->nlen, &h1, &h2);
	psb_add(ps_int_bloom, h1, h2);
	return 0;
}

void pc_pubsub_interest_tick(long long now_ms)
{
	if (!ps_int_on || !ps_int_removed ||
	        now_ms - ps_int_rebuilt_ms < PS_INT_REBUILD_MS)
		return;
	pthread_mutex_lock(&ps_wlk);
	/* the filter only ever gains bits between rebuilds; a peer holding the
	 * old one holds a superset, which is safe, and asks for this one when
	 * it sees the rebuild count move */
	memset(ps_int_bloom, 0, sizeof ps_int_bloom);
	psi_each(&ps_sch, int_rebuild_one, NULL);
	ps_int_rebuild++;
	ps_int_removed = 0;
	ps_int_rebuilt_ms = now_ms;
	__atomic_store_n(&ps_int_version,
		psv_make(ps_int_epoch, ps_int_rebuild, ps_int_adds), __ATOMIC_RELEASE);
	pthread_mutex_unlock(&ps_wlk);
}
/* ---- introspection ------------------------------------------------------ */
struct each_ch {
	const char *glob;
	size_t glen;
	pc_pubsub_name_f cb;
	void *arg;
	int n;
};

static int each_channel(struct psi_node *node, void *arg)
{
	struct each_ch *c = arg;
	struct ps_shared *e = (struct ps_shared *)node;

	if (c->glob && !psi_glob(c->glob, c->glen, node->name, node->nlen))
		return 0;
	if (c->cb(node->name, node->nlen, (int)LOAD(e->count), c->arg) != 0)
		return 1;
	c->n++;
	return 0;
}

int pc_pubsub_channels(const char *glob, size_t glen, pc_pubsub_name_f cb,
		void *arg)
{
	struct each_ch c = { glob, glen, cb, arg, 0 };

	pthread_mutex_lock(&ps_wlk);
	psi_each(&ps_sch, each_channel, &c);
	pthread_mutex_unlock(&ps_wlk);
	return c.n;
}

int pc_pubsub_numsub(const char *name, size_t nlen)
{
	struct psi_node *x;
	int n = 0, locked = read_begin();

	x = psi_find(&ps_sch, name, nlen);
	if (x)
		n = (int)LOAD(((struct ps_shared *)x)->count);
	read_end(locked);
	return n;
}

int pc_pubsub_numpat(void)
{
	return (int)LOAD(ps_spat.n);
}

void pc_pubsub_relay_account(unsigned long long sent, unsigned long long lost,
		unsigned long long dropped, unsigned long long duplicates)
{
	if (duplicates)
		__atomic_fetch_add(&ps_relay_dup, duplicates, __ATOMIC_RELAXED);
	if (sent)
		__atomic_fetch_add(&ps_relay_sent, sent, __ATOMIC_RELAXED);
	if (lost)
		__atomic_fetch_add(&ps_relay_lost, lost, __ATOMIC_RELAXED);
	if (dropped)
		__atomic_fetch_add(&ps_relay_dropped, dropped, __ATOMIC_RELAXED);
}

void pc_pubsub_stats(struct pc_pubsub_stats *out)
{
	out->channels = (int)LOAD(ps_sch.n);
	out->patterns = (int)LOAD(ps_spat.n);
	out->subscribers = (int)LOAD(ps_nsubs);
	out->published = __atomic_load_n(&ps_published, __ATOMIC_RELAXED);
	out->delivered = __atomic_load_n(&ps_delivered, __ATOMIC_RELAXED);
	out->slow_kills = __atomic_load_n(&ps_slow_kills, __ATOMIC_RELAXED);
	out->queued = __atomic_load_n(&ps_queued, __ATOMIC_RELAXED);
	out->keyspace = __atomic_load_n(&ps_keyspace, __ATOMIC_RELAXED);
	out->relay_sent = __atomic_load_n(&ps_relay_sent, __ATOMIC_RELAXED);
	out->relay_recv = __atomic_load_n(&ps_relay_recv, __ATOMIC_RELAXED);
	out->relay_lost = __atomic_load_n(&ps_relay_lost, __ATOMIC_RELAXED);
	out->relay_dropped = __atomic_load_n(&ps_relay_dropped, __ATOMIC_RELAXED);
	out->alloc_failed = __atomic_load_n(&ps_alloc_failed, __ATOMIC_RELAXED);
	out->relay_duplicates = __atomic_load_n(&ps_relay_dup, __ATOMIC_RELAXED);
	out->queue_dropped = __atomic_load_n(&ps_queue_dropped, __ATOMIC_RELAXED);
	{
		unsigned long long b = 0;
		int w;

		for (w = 0; w < ps_nworkers; w++)
			b += __atomic_load_n(&ps_wq[w].bytes, __ATOMIC_RELAXED);
		out->queue_bytes = b;
	}
}
