/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * upstream.c - read-through from a RESP upstream (task S331, DESIGN 12ix).
 *
 * One-way unless asked: the fetch connections send READ commands only -
 * TYPE, PTTL, GET, HGETALL, JSON.GET, plus the handshake (HELLO/AUTH/
 * SELECT) - and the suites enforce that with an upstream that fails the
 * run on any write.  S385 (upstream_writes = through) adds ONE writer per
 * collection, its own thread and connection to the upstream's master,
 * below "write-through"; nothing else here writes upstream.
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

#include "pcaddr.h"
#include "uprespc.h"                 /* the reply parser (wave 1) */
#include "compat/compat.h"
#include "compat/dprint.h"
#include "compat/mem/mem.h"
#include "compat/str.h"
#include "compat/timer.h"
#include "core/pcache_htable.h"
#include "cluster.h"
#include "pc_slot.h"                   /* 12nt: pc_key_slot for spread's K-set */
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
#include "upstreamint.h"            /* the types and macros (wave 1) */

static void rb_free(struct up_rb *rb, int n);
static struct up_wq *wq;
static int wq_n;
const char *const upws_name[] = { "ok", "starting", "no master",
	"two masters, no replica names one", "the master is not answering" };
const char *const updr_name[] = { "idle", "running", "done",
	"stopped", "failed" };
struct up_col up_cols[UP_MAX_COLS];
static int ncols;
static char up_state_dir[1024];       /* S421: for the re-warm names file */
static unsigned int next_id = 1;

int pc_upstream_any(void)
{
	return ncols > 0;
}


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


int pc_upstream_health(int i, struct pc_up_health *h)
{
	struct up_col *uc;
	int j;

	if (i < 0 || i >= ncols)
		return -1;
	uc = &up_cols[i];
	memset(h, 0, sizeof *h);
	h->col = uc->name;
	h->host = uc->srv[0].host;
	h->port = uc->srv[0].port;
	h->tracking_wanted = uc->tracking;
	h->nsrv = uc->nsrv;
	h->role_replica = uc->role_req == PC_UP_ROLE_REPLICA;
	h->chained = ST_GET(uc->chained);                  /* S384 */
	h->through = uc->through;                          /* S385 */
	h->w_state = __atomic_load_n(&uc->w_state, __ATOMIC_RELAXED);
	h->w_target = __atomic_load_n(&uc->w_master, __ATOMIC_RELAXED);
	h->w_json = __atomic_load_n(&uc->w_json, __ATOMIC_RELAXED);
	h->w_forwarded = UPV(uc, wt_fwd);
	h->w_passed = UPV(uc, pass_w);
	h->w_qfull = UPV(uc, wt_qfull);                    /* AR2 */
	h->w_qexpired = UPV(uc, wt_qexpired);
	h->w_bcast = bcast_state(uc);                      /* AR1 */
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

struct up_col *col_of(const char *col, size_t collen)
{
	int i;

	for (i = 0; i < ncols; i++)
		if (up_cols[i].nlen == collen && !memcmp(up_cols[i].name, col, collen))
			return &up_cols[i];
	return NULL;
}

/* AR1: 1 every upstream whose tracking connection is up follows every
 * change (broadcast), -1 one refused it, 0 none up or not asked */
int bcast_state(const struct up_col *uc)
{
	int i, up = 0, on = 0;

	for (i = 0; i < uc->nsrv; i++) {
		int b = __atomic_load_n(&uc->srv[i].bcast, __ATOMIC_RELAXED);

		if (b < 0)
			return -1;
		if (__atomic_load_n(&uc->srv[i].inv_id, __ATOMIC_ACQUIRE) > 0) {
			up++;
			on += b == 1;
		}
	}
	return up && on == up ? 1 : 0;
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
		rb_free(d->rb, d->nrb);
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
			rb_free(d->rb, d->nrb);
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
static void wt_apply(struct up_col *uc, const struct up_done *d);
static void fa_push(struct up_fwd *f, const struct up_done *d);
/* the writer's job plumbing, used by the forwarded-op path above it */
static struct up_wjob *wt_job_new(const struct pc_upfw *fw);
static void wt_job_free(struct up_wjob *j);
static void wt_queue(struct up_col *uc, struct up_wjob *j);
static void wt_err_add(struct up_col *uc, const char *kind, const char *cmd,
		size_t cl, const char *key, size_t kl, const char *msg, size_t ml);
static void wt_count_owned(struct up_col *uc);                     /* AR4 */
static void wt_refused_note(struct up_col *uc, const char *what, size_t wl,
		const char *msg);
/* ---- AR1: Redis changed a key a write-through collection holds ------------
 * The broadcast tracking (CLIENT TRACKING ON BCAST, the collection's
 * prefixes) tells every node of every change Redis makes - by a client of
 * Redis, or replicated from this fleet's own write.  The node's record of
 * the key and its no-fall entry go, HERE only (every node hears it); the
 * next read fetches what Redis holds from a serving replica.  The tracking
 * thread posts the key; a worker drops it. */
/* the worker that applies them: the first registered.  Workers are slots
 * 1..n - there is no worker 0, and what was posted there waited for a wake
 * that never came (0.6.3's first CI: no invalidation was ever applied).
 * -1 before any worker has registered */
/* 12nt (0.7.1): is @key's through copy another node's to fetch and hold -
 * the owner's in shard, the K holders' in spread when this node is not one
 * of them?  0 in eager, store, proxy and outside a cluster: here. */
static int through_elsewhere(pcache_htable_t *ht, const char *col,
		size_t collen, const char *key, size_t kl)
{
	int k;

	if (!pc_cluster_enabled())
		return 0;
	if (pc_store_shard_enabled(ht))
		return pc_shard_owner(col, collen, key, kl) != 0;
	k = pc_cluster_replicas();
	if (k > 0 && pc_store_eager_enabled(ht))
		return !pc_spread_in_set(pc_key_slot(key, kl), 0, k);
	return 0;
}

static int inv_worker(void)
{
	int i;

	for (i = 0; i < wq_n; i++)
		if (wq[i].efd >= 0)
			return i;
	return -1;
}

static void inv_post(struct up_col *uc, const char *k, size_t kl)
{
	struct up_done d;
	int w = inv_worker();

	if (w < 0)
		return;                        /* at start: nothing held yet */
	memset(&d, 0, sizeof d);
	d.kind = UPD_INV;
	d.col = uc->idx;
	d.key = malloc(kl ? kl : 1);
	if (!d.key)
		return;
	memcpy(d.key, k, kl);
	d.klen = kl;
	post_done(w, &d);
}

/* a gap in the tracking (a connection lost or new, a FLUSHALL): any change
 * may have been missed, so every record written through goes - not the
 * keys the fleet owned under allow ("1"), which Redis may not have */
static void inv_post_all(struct up_col *uc)
{
	struct up_done d;
	int w = inv_worker();

	if (w < 0) {
		/* at start, before the workers: the records recovered from the
		 * WAL may have missed a change while the node was down - the
		 * first drain on any worker drops them */
		__atomic_store_n(&uc->inv_all_pending, 1, __ATOMIC_RELEASE);
		return;
	}
	memset(&d, 0, sizeof d);
	d.kind = UPD_INVALL;
	d.col = uc->idx;
	post_done(w, &d);
}

/* S404: the no-fall entry's value - '1', 't' or 'g' - or 0 when there is
 * none.  Uncounted, as the gate's every look at the companion is (S340) */
static int nofall_mark(struct up_col *uc, const str *k)
{
	pcache_htable_t *nf = pc_store_find(uc->nf, uc->nflen);
	unsigned char m = 0;

	if (!nf || pcache_ht_peek_main_byte(nf, k, &m) != 0)
		return 0;
	return m;
}

/* S404: a written-through key's mark with no record beside it.  A write
 * reaches the other members as two replicated writes - the record, then
 * its mark - and Redis's broadcast invalidation of that same write reaches
 * every member on its own.  Landing between the two, the invalidation
 * dropped the record and the mark arrived after it: alone, it told the
 * gate the fleet holds the key, and reads here answered nil for a key
 * Redis holds (staging, 2026-10-14).  Such a mark is dropped (here only)
 * and the key falls through.  1 it was one (and is gone), 0 not */
static int nofall_bare_t(struct up_col *uc, const str *k)
{
	pcache_htable_t *nf = pc_store_find(uc->nf, uc->nflen);

	if (!nf || nofall_mark(uc, k) != 't')
		return 0;
	pc_verb_forget_local(nf, k);
	ST_ADD(uc->wt_bare, 1);
	return 1;
}

static int join_key(struct up_col *uc, const char *k, size_t kl,
		struct up_req *r, int own, int own_worker, int full);
static void rewarm_save(struct up_col *uc, const char *state_dir);   /* S421 */
static void rewarm_load(struct up_col *uc, const char *state_dir);

/* S422: FNV-1a over the key */
static uint64_t echo_hash(const char *k, size_t kl)
{
	uint64_t h = 1469598103934665603ULL;
	size_t i;

	for (i = 0; i < kl; i++)
		h = (h ^ (unsigned char)k[i]) * 1099511628211ULL;
	return h ? h : 1;
}

/* S435: a removal of copies or records, bracketed for a read the gate let
 * through while it ran (pc_upstream_resp_lost): begun before the first
 * entry goes, done after the last */
static void rm_begin(struct up_col *uc)
{
	__atomic_add_fetch(&uc->rm_begin, 1, __ATOMIC_SEQ_CST);
}

static void rm_end(struct up_col *uc)
{
	__atomic_add_fetch(&uc->rm_end, 1, __ATOMIC_RELEASE);
}

/* S422: this node applied a write-through of @k now (wt_apply) */
static void echo_note(struct up_col *uc, const char *k, size_t kl)
{
	uint64_t h;

	if (!uc->echo)
		return;
	h = echo_hash(k, kl);
	uc->echo[h % UP_ECHO_N].h = h;
	uc->echo[h % UP_ECHO_N].t0 = now_ms();
}

/* S422: is an invalidation of @k arriving now the echo of this node's own
 * write (inside upstream_echo_ms of its apply)? */
static int echo_hit(struct up_col *uc, const char *k, size_t kl)
{
	uint64_t h;
	struct up_echo *e;

	if (!uc->echo || uc->echo_ms <= 0)
		return 0;
	h = echo_hash(k, kl);
	e = &uc->echo[h % UP_ECHO_N];
	return e->h == h && now_ms() - e->t0 <= uc->echo_ms;
}

/* S422: the echo left no copy of @k anywhere here - fetch it again now, so
 * the next read hits (the fetched copy is Redis's current state) */
static void echo_refetch(struct up_col *uc, pcache_htable_t *ht, const str *k)
{
	unsigned char fl = 0, mark = 0;
	unsigned int exp = 0;

	if (pcache_ht_shadow_peek(ht, k, &fl, &mark, &exp) == 0)
		return;                        /* another upstream's copy answers */
	pthread_mutex_lock(&uc->mx);
	if (join_key(uc, k->s, (size_t)k->len, NULL, 0, -1, 1) == 0)
		ST_ADD(uc->wt_echo, 1);
	pthread_mutex_unlock(&uc->mx);
}

static void inv_apply(struct up_col *uc, const char *key, size_t klen)
{
	pcache_htable_t *ht = pc_store_find(uc->name, uc->nlen);
	pcache_htable_t *nf = pc_store_find(uc->nf, uc->nflen);
	str k;
	int had = 0;

	/* outside upstream_prefixes the fleet keeps its own keys: a change
	 * Redis makes there is none of this collection's business */
	if (!ht || klen > INT_MAX || !allowed(uc, key, klen))
		return;
	k.s = (char *)key;
	k.len = (int)klen;
	/* S404: the record goes, and its mark with it - but a "t" mark found
	 * WITHOUT its record stays: the record may still be on its way from
	 * the member that made the write, and it must land marked, not as the
	 * fleet's own.  A read that later finds such a mark alone drops it
	 * and falls through (nofall_bare_t).  "1" and "g" marks go either
	 * way.  (Dropping both together keeps the usual path as it was: every
	 * write-through write comes back as an invalidation, and the read
	 * after it must not pay for a mark left behind on purpose.) */
	rm_begin(uc);                      /* S435: a read on another worker */
	if (pcache_ht_probe_main(ht, &k, NULL) == 0) {
		pc_verb_forget_local(ht, &k);
		had = 1;
	}
	if (nf && pcache_ht_probe_main(nf, &k, NULL) == 0 &&
	    (had || nofall_mark(uc, &k) != 't')) {
		pc_verb_forget_local(nf, &k);
		had = 1;
	}
	rm_end(uc);
	if (had)
		ST_ADD(uc->wt_inval, 1);
	/* S422: the echo of this node's own write (or a change racing it)
	 * inside the window - the key is read again rather than left empty;
	 * every tracked upstream echoes once, so a later echo that dropped
	 * the refetched copy refetches too (one fetch per tracked upstream
	 * at most, inside the window) */
	if (echo_hit(uc, key, klen))
		echo_refetch(uc, ht, &k);
}

struct inv_keys {
	str *k;
	char *mk;                          /* S421: the mark, 't' or 'g' */
	unsigned int *wt;                  /* S421: the record's last write tick */
	int n, cap;
};

static int inv_collect(const str *key, const str *val,
		const struct pcache_rec_meta *mt, void *ctx)
{
	struct inv_keys *ik = ctx;

	(void)mt;
	if (!val || val->len != 1 || (val->s[0] != 't' && val->s[0] != 'g'))
		return 0;                      /* the fleet's own ("1") stays */
	if (ik->n == ik->cap) {
		int nc = ik->cap ? ik->cap * 2 : 256;
		str *nk = realloc(ik->k, (size_t)nc * sizeof *nk);
		char *nm = nk ? realloc(ik->mk, (size_t)nc) : NULL;
		unsigned int *nw = nm ? realloc(ik->wt, (size_t)nc * sizeof *nw) : NULL;

		if (nk)
			ik->k = nk;
		if (nm)
			ik->mk = nm;
		if (nw)
			ik->wt = nw;
		if (!nk || !nm || !nw)
			return 0;
		ik->cap = nc;
	}
	ik->k[ik->n].s = malloc((size_t)key->len ? (size_t)key->len : 1);
	if (!ik->k[ik->n].s)
		return 0;
	memcpy(ik->k[ik->n].s, key->s, (size_t)key->len);
	ik->k[ik->n].len = key->len;
	ik->mk[ik->n] = val->s[0];
	ik->wt[ik->n] = mt ? mt->wtick : 0;
	ik->n++;
	return 0;
}

/* S421: newest-written first - the keys a client is likeliest to ask for
 * next come back first */
static int rw_cmp_desc(const void *a, const void *b)
{
	unsigned int x = *(const unsigned int *)a, y = *(const unsigned int *)b;

	return x < y ? 1 : x > y ? -1 : 0;
}

/* S421: the names of the records a gap dropped go on the re-warm list
 * (the 't' ones: a 'g' is a key the upstream did not have), bounded by
 * upstream_rewarm_max; the pacer (rewarm_tick) fetches them again */
static void rewarm_queue(struct up_col *uc, struct inv_keys *ik)
{
	int i, kept = 0, dropped = 0, nt = 0;
	unsigned int *order;

	for (i = 0; i < ik->n; i++)
		if (ik->mk[i] == 't')
			nt++;
	order = nt ? malloc((size_t)nt * 2 * sizeof *order) : NULL;
	if (order) {
		int j = 0;

		for (i = 0; i < ik->n; i++)
			if (ik->mk[i] == 't') {
				order[2 * j] = ik->wt[i];
				order[2 * j + 1] = (unsigned int)i;
				j++;
			}
		qsort(order, (size_t)nt, 2 * sizeof *order, rw_cmp_desc);
	}
	pthread_mutex_lock(&uc->mx);
	for (i = 0; i < ik->n; i++) {
		int at = order ? (int)order[2 * i + 1] : i;

		if (i >= nt)
			break;
		if (!order && ik->mk[at] != 't')
			continue;
		if (uc->rewarm_max <= 0 || uc->rw_n - uc->rw_head >= uc->rewarm_max) {
			dropped++;
			continue;
		}
		if (uc->rw_n == uc->rw_cap) {
			int nc = uc->rw_cap ? uc->rw_cap * 2 : 1024;
			str *nr = realloc(uc->rw, (size_t)nc * sizeof *nr);

			if (!nr) {
				dropped++;
				continue;
			}
			uc->rw = nr;
			uc->rw_cap = nc;
		}
		uc->rw[uc->rw_n++] = ik->k[at];   /* the name moves: not freed below */
		ik->k[at].s = NULL;
		kept++;
	}
	if (kept && uc->rw_n - uc->rw_head == kept)
		uc->rw_runs++;                 /* a run starts (nothing was queued) */
	uc->rw_dropped += (unsigned long long)dropped;
	uc->rw_last_ms = now_ms();
	pthread_mutex_unlock(&uc->mx);
	free(order);
	if (kept)
		LM_NOTICE("collection '%s': re-warming %d of them in the background "
			"at %d keys/s%s\n", uc->name, kept, uc->drain_rate,
			dropped ? " (the rest over upstream_rewarm_max: when read)" : "");
}

static void inv_apply_all(struct up_col *uc)
{
	pcache_htable_t *ht = pc_store_find(uc->name, uc->nlen);
	pcache_htable_t *nf = pc_store_find(uc->nf, uc->nflen);
	struct inv_keys ik = { NULL, NULL, NULL, 0, 0 };
	int i;

	if (!nf)
		return;
	pcache_ht_iter_meta(nf, inv_collect, &ik);   /* collected, then dropped */
	rm_begin(uc);                                      /* S435 */
	for (i = 0; i < ik.n; i++) {
		pc_verb_forget_local(nf, &ik.k[i]);
		if (ht)
			pc_verb_forget_local(ht, &ik.k[i]);
	}
	rm_end(uc);
	ST_ADD(uc->wt_invall, 1);
	if (ik.n)
		LM_NOTICE("collection '%s': the tracking had a gap - %d record(s) "
			"written through are dropped here and fetched again from "
			"the upstream when read\n", uc->name, ik.n);
	if (ik.n && uc->rewarm_max > 0)
		rewarm_queue(uc, &ik);         /* S421: or in the background */
	for (i = 0; i < ik.n; i++)
		free(ik.k[i].s);
	free(ik.k);
	free(ik.mk);
	free(ik.wt);
}

/* S431: one name onto the re-warm list, under mx - a copy that a
 * tracking change flushed, or that was fetched while its server's tracking
 * was not up (it would live at most upstream_negative_ms).  1 queued, 0 not
 * (over upstream_rewarm_max: when read) */
static int rewarm_add_locked(struct up_col *uc, const char *k, size_t kl)
{
	char *c;

	if (uc->rewarm_max <= 0 || uc->rw_n - uc->rw_head >= uc->rewarm_max) {
		uc->rw_dropped++;
		return 0;
	}
	if (uc->rw_n == uc->rw_cap) {
		int nc = uc->rw_cap ? uc->rw_cap * 2 : 1024;
		str *nr = realloc(uc->rw, (size_t)nc * sizeof *nr);

		if (!nr) {
			uc->rw_dropped++;
			return 0;
		}
		uc->rw = nr;
		uc->rw_cap = nc;
	}
	c = malloc(kl ? kl : 1);
	if (!c) {
		uc->rw_dropped++;
		return 0;
	}
	memcpy(c, k, kl);
	if (uc->rw_n == uc->rw_head)
		uc->rw_runs++;                 /* a run starts (nothing was queued) */
	uc->rw[uc->rw_n].s = c;
	uc->rw[uc->rw_n].len = (int)kl;
	uc->rw_n++;
	uc->rw_requeued++;
	return 1;
}

static struct up_srv *pick_rw(struct up_col *uc);       /* S431 */

/* S421: the pacer - once a loop of an invalidation thread (at most once a
 * second when quiet): up to drain_rate names a second, never more than
 * 512 fetches queued at the upstreams, each name first checked against the
 * shadow (a client read may have fetched it already: skipped).  Under mx,
 * as join_key wants. */
#define RW_QUEUE_CAP 512
static void rewarm_tick(struct up_col *uc)
{
	pcache_htable_t *ht;
	long long now, budget;
	int queued = 0, i, started = 0, skipped = 0;

	/* a gap seen before any worker registered (the start: the WAL's
	 * records may have missed a change) waited for the first request's
	 * drain - the re-warm needs it applied now, traffic or not */
	if (__atomic_load_n(&uc->inv_all_pending, __ATOMIC_ACQUIRE) &&
	    inv_worker() >= 0 &&
	    __atomic_exchange_n(&uc->inv_all_pending, 0, __ATOMIC_ACQ_REL))
		inv_post_all(uc);
	if (!uc->rw_n)
		return;
	ht = pc_store_find(uc->name, uc->nlen);
	if (!ht)
		return;
	pthread_mutex_lock(&uc->mx);
	if (uc->rw_head >= uc->rw_n) {
		pthread_mutex_unlock(&uc->mx);
		return;
	}
	/* S431: not before a server's tracking is up.  This runs on each
	 * server's invalidation thread, so at a start the first server up
	 * began the run while another was still connecting: a name fetched
	 * there was untracked (gone after upstream_negative_ms), or flushed
	 * with that server's shadow when its tracking came up - counted done,
	 * and the copy lost (upwritethroughtest 20 (a), 1 run in 4-5) */
	if (!pick_rw(uc)) {
		pthread_mutex_unlock(&uc->mx);
		return;
	}
	now = now_ms();
	budget = (long long)uc->drain_rate * (now - uc->rw_last_ms) / 1000;
	if (budget > uc->drain_rate)
		budget = uc->drain_rate;
	if (budget <= 0) {
		pthread_mutex_unlock(&uc->mx);
		return;
	}
	uc->rw_last_ms = now;
	for (i = 0; i < uc->nsrv; i++)
		queued += uc->srv[i].queued;
	if (budget > RW_QUEUE_CAP - queued)
		budget = RW_QUEUE_CAP - queued;
	uc->rw_dispatch = 1;               /* S431: join_key routes to pick_rw */
	while (budget > 0 && uc->rw_head < uc->rw_n) {
		str *k = &uc->rw[uc->rw_head++];
		unsigned char fl = 0, mark = 0;
		unsigned int exp = 0;

		if (pcache_ht_shadow_peek(ht, k, &fl, &mark, &exp) == 0) {
			skipped++;                 /* a read got there first */
		} else if (join_key(uc, k->s, (size_t)k->len, NULL, 0, -1, 1) == 0) {
			started++;
			budget--;
		}
		free(k->s);
		k->s = NULL;
	}
	uc->rw_dispatch = 0;
	uc->rw_started += (unsigned long long)started;
	uc->rw_skipped += (unsigned long long)skipped;
	if (uc->rw_head >= uc->rw_n) {
		uc->rw_head = uc->rw_n = 0;    /* the run is over: the list is reused */
		pthread_mutex_unlock(&uc->mx);
		LM_NOTICE("collection '%s': the re-warm is done - %llu fetched again, "
			"%llu already back by a read\n", uc->name,
			(unsigned long long)uc->rw_started,
			(unsigned long long)uc->rw_skipped);
		return;
	}
	pthread_mutex_unlock(&uc->mx);
}

/* S385: the result being re-run on this worker (pc_upstream_drain sets it
 * for the one completion it hands to proto; the gate takes it once) */
static __thread struct up_done *wt_cur;

void pc_upstream_drain(int worker)
{
	struct up_wq *w;
	struct up_done buf[32];
	int n, i;

	if (worker < 0 || worker >= wq_n)
		return;
	/* AR1: a tracking gap no worker could take when it was seen */
	for (i = 0; i < ncols; i++)
		if (__atomic_load_n(&up_cols[i].inv_all_pending, __ATOMIC_ACQUIRE) &&
		    __atomic_exchange_n(&up_cols[i].inv_all_pending, 0,
		        __ATOMIC_ACQ_REL))
			inv_apply_all(&up_cols[i]);
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
					pull_apply(&up_cols[buf[i].col], &buf[i]);
					__atomic_sub_fetch(&up_cols[buf[i].col].drain_inflight,
						1, __ATOMIC_RELAXED);
				}
				free(buf[i].key);
				free(buf[i].val);
				continue;
			}
			if (buf[i].kind == UPD_OWN) {
				if (buf[i].col >= 0 && buf[i].col < ncols)
					own_apply(&up_cols[buf[i].col], buf[i].key,
						buf[i].klen);
				free(buf[i].key);
				continue;
			}
			if (buf[i].kind == UPD_INV || buf[i].kind == UPD_INVALL) {
				if (buf[i].col >= 0 && buf[i].col < ncols) {
					if (buf[i].kind == UPD_INV)
						inv_apply(&up_cols[buf[i].col], buf[i].key,
							buf[i].klen);
					else
						inv_apply_all(&up_cols[buf[i].col]);
				}
				free(buf[i].key);
				continue;
			}
			if (buf[i].kind == UPD_WT) {
				/* S385: the fleet takes Redis's state of the keys
				 * first, then the parked command answers - with
				 * Redis's reply, handed over in wt_cur */
				if (buf[i].col >= 0 && buf[i].col < ncols)
					wt_apply(&up_cols[buf[i].col], &buf[i]);
				if (buf[i].fwd) {
					/* 12nt: a peer's forwarded op - the owner
					 * holds what Redis holds now; the asker
					 * gets the ack on the cluster thread */
					fa_push(buf[i].fwd, &buf[i]);
				} else {
					wt_cur = &buf[i];
					pc_proto_upstream_complete(buf[i].id, PC_UPV_WT);
					wt_cur = NULL;
				}
				rb_free(buf[i].rb, buf[i].nrb);
				free(buf[i].val);
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

	rm_begin(uc);
	for (j = 0; j < uc->nsrv; j++)
		pcache_ht_remove(uc->srv[j].shadow, k);
	rm_end(uc);
}

/* what an upstream may do now - PC_UPS_*.  SERVE: it takes fetches.
 * UNKNOWN: not connected yet, so not known - it may, its first connect
 * checks.  FALLBACK: a master that upstream_master_fallback lets serve
 * while no replica does.  OFF: it cannot reach it, or it is a master (or
 * says nothing) where a replica is asked for, or its master link is down,
 * or it lags more than upstream_max_lag_ms */
int srv_state(const struct up_srv *s)
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

static long long wall_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* why an upstream is not used (PC_UPS_OFF), "" when it is - /stats, the
 * page and the failover log line */
const char *srv_why(const struct up_srv *s)
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

/* S431: is @s's tracking up (or is tracking off for the collection) - a
 * copy fetched there now is tracked */
static int srv_tracked(const struct up_srv *s)
{
	/* inv_ready, not inv_id: the id is published before tracking_moved()
	 * flushes the server's shadow, and the re-warm runs on EVERY server's
	 * invalidation thread - a copy routed here by another thread in that
	 * instant was stored and flushed, and never queued again (rc9's ASan
	 * core1 on GitHub, upwritethroughtest 20 (d): one copy) */
	return !s->uc->tracking || __atomic_load_n(&s->inv_ready, __ATOMIC_ACQUIRE);
}

/* S431: the re-warm's pick - pick()'s order among the servers whose
 * tracking is up; NULL while none is (the run waits) */
static struct up_srv *pick_rw(struct up_col *uc)
{
	static const int order[3] = { PC_UPS_SERVE, PC_UPS_UNKNOWN,
		PC_UPS_FALLBACK };
	int o, j;

	if (uc->mode == PC_UP_MODE_FAILOVER) {
		struct up_srv *s = pick(uc, NULL);

		return s && srv_tracked(s) ? s : NULL;
	}
	for (o = 0; o < 3; o++) {
		struct up_srv *best = NULL;

		for (j = 0; j < uc->nsrv; j++) {
			struct up_srv *s = &uc->srv[(uc->rr + j) % uc->nsrv];

			if (srv_state(s) != order[o] || !srv_tracked(s))
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

/* S423 (AR13, 2026-10-20): the master among the listed upstreams, when its
 * ROLE says so and it is reachable; NULL else.  Under uc->mx */
static struct up_srv *master_srv(struct up_col *uc)
{
	int j;

	for (j = 0; j < uc->nsrv; j++) {
		struct up_srv *s = &uc->srv[j];

		if (__atomic_load_n(&s->role, __ATOMIC_RELAXED) == UPR_MASTER &&
		    !__atomic_load_n(&s->failing, __ATOMIC_RELAXED) &&
		    !(__atomic_load_n(&s->is_pc, __ATOMIC_RELAXED) && !uc->allow_pc))
			return s;
	}
	return NULL;
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
		e->inv = 0;
		e->rounds = 0;
		e->rw = uc->rw_dispatch;           /* S431 */
		e->next = uc->inflight;
		uc->inflight = e;
		{
			/* S382: none serving - the first takes it, and fails it
			 * fast as a single dead upstream does.  S423: inside the
			 * echo window of this node's own write the key goes to the
			 * master, which has the write by definition - a replica may
			 * not yet (its lag), and its answer would be the older
			 * value or none: a client read nil for the key it had just
			 * set (AR13 on 245-247, ~4% of writes).  The echo refetch
			 * and a read that misses in the window both come here */
			struct up_srv *s = NULL;

			if (echo_hit(uc, k, kl) &&
			    (s = master_srv(uc)) != NULL)
				ST_ADD(uc->wt_echo_master, 1);
			else if (!uc->rw_dispatch || !(s = pick_rw(uc)))
				s = pick(uc, NULL);
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
static int nofall_put_m(struct up_col *uc, const str *k, unsigned int exp,
		const char *m)
{
	pcache_htable_t *nf = pc_store_find(uc->nf, uc->nflen);

	sh_remove(uc, k);
	if (!nf)
		return -1;
	if (pc_verb_store_local(nf, k, m, 1, exp, 0) != 0)
		return -1;
	ST_ADD(uc->nofall_written, 1);
	return 0;
}

/* the entry's value says who left it (AR1): "1" the fleet took the key
 * over (a client write under allow, a promote) - Redis may not have it;
 * "t" a write-through read-back found a value, "g" found it gone - Redis
 * holds that state, so a broadcast invalidation or a tracking gap may
 * drop it */
static int nofall_put(struct up_col *uc, const str *k, unsigned int exp)
{
	return nofall_put_m(uc, k, exp, "1");
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
	if (pcache_ht_probe_main(ht, &k, NULL) == 0 ||
	    (nofall_has(uc, &k) && !nofall_bare_t(uc, &k))) {      /* S404 */
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

/* the read-through gate (S331); S385's write-through gate is wt_gate */
static int gate_rt(pcache_htable_t *ht, const char *col, size_t collen,
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
		/* 12nt (0.7.1): in shard mode a key this node does not own is the
		 * owner's to fetch, in spread one this node does not hold is its
		 * K holders' - the read pulls it from there (op_get_miss) and the
		 * holder's pull handler runs the fetch.  Nothing here. */
		if ((cls == UPC_READ || cls == UPC_PROBE) &&
		    through_elsewhere(ht, col, collen, keys[i], kl[i]))
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
		if (nofall_has(uc, &k) && !nofall_bare_t(uc, &k)) {   /* S404 */
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

/* S385: what the read-back of a RESP write asks first */
static int resp_guess(const char *c, size_t cl)
{
	if (cl >= 5 && !strncasecmp(c, "JSON.", 5))
		return PC_UPFW_JSON;
	if (cl >= 1 && (c[0] == 'H' || c[0] == 'h'))
		return PC_UPFW_HASH;
	if (name_is(c, cl, "DEL") || name_is(c, cl, "UNLINK") ||
	    name_is(c, cl, "DELEX"))
		return PC_UPFW_GONE;
	return PC_UPFW_STRING;
}

static void resp_answer(struct pc_jw *out, const struct pc_upg *g)
{
	if (g->done && g->raw)
		pc_jw_raw(out, g->raw, g->rawlen);  /* Redis's reply, as it came */
	else if (g->wrongtype)
		wrongtype(out);
	else {
		const char *e = g->err ? g->err : "upstream: no answer";

		pc_jw_lit(out, "-ERR ");
		pc_jw_raw(out, e, strlen(e));
		pc_jw_lit(out, "\r\n");
	}
}

/* S435: the read this worker's gate last let through - its collection and
 * the removals done before it looked (on the run after a fetch: when the
 * gate parked it).  pc_upstream_resp_lost takes it once */
static __thread struct up_col *lost_uc;
static __thread unsigned long long lost_end0;

/* S435: did a read the gate let through lose a key before its handler
 * read it?  The gate looks (a record, a copy) and the handler reads after
 * it, with no lock between: an invalidation (any tracked server's echo of
 * a write takes the key's copies from every server's shadow), an echo's
 * record drop on another worker, or a flush can take the key in between,
 * and the handler answered nil for a key Redis holds - upechomastertest A
 * and B, 1 key in 40 on some runs, each just after a replica's echo.  The
 * run after a fetch is the other door: its key is not fetched again by
 * rule, and a change that took the fresh copy before the run left it
 * unknown.  1: a key the command reads is now unknown here (no record, no
 * no-fall entry, no copy or marker) and a removal overlapped the read (or
 * on the run after a fetch, since the command parked) - the caller drops
 * the reply and gates the command again.  A key left unknown by the fetch
 * itself (absent under upstream_negative_ms = 0, a fill that found no
 * room) with nothing removed meanwhile is its answer, not a loss.  Pure
 * reads only (GET, MGET, the probes, the hash reads, JSON.GET); one atomic
 * load when no removal overlapped. */
int pc_upstream_resp_lost(pcache_htable_t *ht, const char *col, size_t collen,
		char *const *argv, const size_t *argl, int nargs)
{
	struct up_col *uc = lost_uc;
	int cls, del, all, n, i;
	static int off = -1;

	lost_uc = NULL;
	if (off < 0) {
		/* tests only: the defect shown (upechomastertest F) */
		off = getenv("PERFCACHED_TEST_LOST_OFF") != NULL;
		if (off)
			LM_WARN("upstream: PERFCACHED_TEST_LOST_OFF - a read that "
				"lost its key answers what it read (a test setting)\n");
	}
	if (!uc || nargs < 2 || off)
		return 0;
	if (__atomic_load_n(&uc->rm_begin, __ATOMIC_SEQ_CST) == lost_end0)
		return 0;                      /* nothing went meanwhile */
	cls = pc_upstream_cmd_class(argv[0], argl[0], argv, argl, nargs, &del,
		&all);
	if ((cls != UPC_READ && cls != UPC_PROBE) || del)
		return 0;
	n = all ? nargs - 1 : 1;
	for (i = 1; i <= n; i++) {
		unsigned char fl = 0, mark = 0;
		unsigned int exp = 0;
		str k;

		/* the gate's own skips: outside the prefixes, another node's */
		if (!allowed(uc, argv[i], argl[i]) ||
		    through_elsewhere(ht, col, collen, argv[i], argl[i]))
			continue;
		k.s = argv[i];
		k.len = (int)argl[i];
		if (pcache_ht_probe_main(ht, &k, NULL) == 0 || nofall_has(uc, &k) ||
		    pcache_ht_shadow_peek(ht, &k, &fl, &mark, &exp) == 0)
			continue;                  /* known here: the answer stands */
		ST_ADD(uc->read_lost, 1);
		return 1;
	}
	return 0;
}

/* S435, tests only: PERFCACHED_TEST_GATE_DELAY_US=<us> holds a read the
 * gate let through that long before its handler runs - the window a
 * removal must land in, made wide (upechomastertest F) */
void pc_upstream_test_pause(void)
{
	static int us = -1;

	if (us < 0) {
		const char *e = getenv("PERFCACHED_TEST_GATE_DELAY_US");

		us = e && atoi(e) > 0 ? atoi(e) : 0;
		if (us)
			LM_WARN("upstream: PERFCACHED_TEST_GATE_DELAY_US=%d - every "
				"read the gate lets through waits %d us before it "
				"reads (a test setting)\n", us, us);
	}
	if (us && lost_uc)
		usleep((useconds_t)us);
}

int pc_upstream_resp_gate(pcache_htable_t *ht, const char *col, size_t collen,
		char *const *argv, const size_t *argl, int nargs,
		struct pc_jw *out, void *conn)
{
	struct pc_upg g;
	struct up_col *uc;
	int cls, del, all, rc, n, verdict;
	struct pc_upfw fw, *fwp = NULL;
	int kidx[64];
	const char **av = NULL;
	size_t *al = NULL;
	const char *a2[3];
	size_t l2[3];
	char ttl[24];
	unsigned long long end0 = 0;

	lost_uc = NULL;                    /* S435 */
	if (!ncols || nargs < 2)
		return PC_UPG_PROCEED;
	verdict = pc_conn_upstream_verdict(conn);
	cls = pc_upstream_cmd_class(argv[0], argl[0], argv, argl, nargs, &del,
		&all);
	uc = col_of(col, collen);
	/* S435: the removals done before the gate looks - read first */
	if (uc && (cls == UPC_READ || cls == UPC_PROBE) && !del)
		end0 = __atomic_load_n(&uc->rm_end, __ATOMIC_ACQUIRE);
	if (uc && uc->through) {
		/* S385: writes go to the master.  A write this gate does not
		 * class (perfcached's own: its lock scripts, its rate limiter)
		 * has no meaning there and would change the fleet only */
		if (cls == UPC_NONE && verdict != PC_UPV_WT) {
			if (name_is(argv[0], argl[0], "EVAL") ||
			    name_is(argv[0], argl[0], "EVALSHA") ||
			    name_is(argv[0], argl[0], "RL.HIT")) {
				wt_refused_note(uc, argv[0], argl[0], "not forwarded - "
					"it writes perfcached only");
				pc_jw_lit(out, "-ERR upstream_writes = through: ");
				pc_jw_raw(out, argv[0], argl[0]);
				pc_jw_lit(out, " is not forwarded - it writes perfcached "
					"only\r\n");
				return PC_UPG_ANSWERED;
			}
			return PC_UPG_PROCEED;
		}
		if (cls == UPC_MUTATE || cls == UPC_PLAIN || del ||
		    verdict == PC_UPV_WT) {
			int i;

			memset(&fw, 0, sizeof fw);
			fw.argc = nargs;
			fw.argv = (const char *const *)argv;
			fw.argl = argl;
			n = all ? nargs - 1 : 1;
			for (i = 0; i < n && i < 64; i++)
				kidx[i] = i + 1;
			fw.nk = i;
			fw.kidx = kidx;
			fw.json = argl[0] >= 5 && !strncasecmp(argv[0], "JSON.", 5);
			fw.guess = resp_guess(argv[0], argl[0]);
			if (name_is(argv[0], argl[0], "JSON.SET")) {
				/* perfcached's EX on JSON.SET: RedisJSON has none -
				 * the document, then an EXPIRE */
				int k, ex = -1;

				for (k = 4; k + 1 < nargs; k++)
					if (argl[k] == 2 && !strncasecmp(argv[k], "EX", 2)) {
						ex = k;
						break;
					}
				if (ex > 0) {
					av = malloc((size_t)nargs * sizeof *av);
					al = malloc((size_t)nargs * sizeof *al);
					if (!av || !al) {
						free(av);
						free(al);
						pc_jw_lit(out, "-ERR upstream: out of memory\r\n");
						return PC_UPG_ANSWERED;
					}
					for (k = 0, i = 0; k < nargs; k++)
						if (k != ex && k != ex + 1) {
							av[i] = argv[k];
							al[i++] = argl[k];
						}
					fw.argc = i;
					fw.argv = av;
					fw.argl = al;
					a2[0] = "EXPIRE";
					l2[0] = 6;
					a2[1] = argv[1];
					l2[1] = argl[1];
					l2[2] = argl[ex + 1] < sizeof ttl ? argl[ex + 1] : 0;
					memcpy(ttl, argv[ex + 1], l2[2]);
					a2[2] = ttl;
					fw.argc2 = 3;
					fw.argv2 = a2;
					fw.argl2 = l2;
				}
			}
			fwp = &fw;
			n = all ? nargs - 1 : 1;
			rc = pc_upstream_gate_fw(ht, col, collen, cls, del,
				(const char *const *)argv + 1, argl + 1, n, verdict, &g,
				fwp);
			free(av);
			free(al);
			if (rc == PC_UPG_HELD)
				pc_conn_upstream_hold(conn, g.req);
			else if (rc == PC_UPG_ANSWERED)
				resp_answer(out, &g);
			return rc;
		}
	}
	if (cls == UPC_NONE)
		return PC_UPG_PROCEED;
	n = all ? nargs - 1 : 1;
	rc = pc_upstream_gate(ht, col, collen, cls, del,
		(const char *const *)argv + 1, argl + 1, n, verdict, &g);
	if (rc == PC_UPG_HELD) {
		pc_conn_upstream_hold(conn, g.req);
		if (uc && (cls == UPC_READ || cls == UPC_PROBE) && !del)
			pc_conn_upstream_set_mark(conn, end0);    /* S435 */
	} else if (rc == PC_UPG_ANSWERED)
		resp_answer(out, &g);
	else if (uc && (cls == UPC_READ || cls == UPC_PROBE) && !del &&
	         verdict != PC_UPV_ERROR) {
		/* S435: for pc_upstream_resp_lost - on the run after a fetch,
		 * what was removed since the command parked (the change that
		 * took the fresh copy came before this run looked).  Not after
		 * a failed fetch: on_error answered, and a re-gate would ask
		 * the failing upstream twice more */
		lost_uc = uc;
		lost_end0 = verdict == PC_UPV_OK ?
			pc_conn_upstream_mark(conn) : end0;
	}
	return rc;
}

/* ---- a connection's tracking: the shadow flushes and the pushes ---------
 * (the RESP client itself - connect, send, read, ROLE - is upconn.c) */

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
void flush_shadow(struct up_srv *s)
{
	rm_begin(s->uc);                                   /* S435 */
	pcache_ht_iter_meta(s->shadow, rm_cb, s->shadow);
	rm_end(s->uc);
	ST_ADD(s->uc->flushes, 1);
	ST_ADD(s->flushes, 1);
}

/* S431: the names of a shadow's value copies (its marks left out) */
struct name_list {
	str *k;
	int n, cap;
};

static int name_collect(const str *key, const str *val,
		const struct pcache_rec_meta *mt, void *ctx)
{
	struct name_list *nl = ctx;

	(void)val;
	if (mt && (mt->rflags & PCACHE_F_UPMARK))
		return 0;
	if (nl->n == nl->cap) {
		int nc = nl->cap ? nl->cap * 2 : 256;
		str *nk = realloc(nl->k, (size_t)nc * sizeof *nk);

		if (!nk)
			return 0;
		nl->k = nk;
		nl->cap = nc;
	}
	nl->k[nl->n].s = malloc((size_t)key->len ? (size_t)key->len : 1);
	if (!nl->k[nl->n].s)
		return 0;
	memcpy(nl->k[nl->n].s, key->s, (size_t)key->len);
	nl->k[nl->n].len = key->len;
	nl->n++;
	return 0;
}

/* an upstream's invalidation connection changed (lost, or newly up): its
 * fetch connections must re-arm toward the new one, and what was fetched
 * under the old one may have missed an invalidation.  S431: on a through
 * collection the copies the flush drops are fetched again in the
 * background, as a restart's are (S421) - before, a lost tracking
 * connection left them to the next read */
static void tracking_moved(struct up_srv *s)
{
	struct up_col *uc = s->uc;
	struct name_list nl = { NULL, 0, 0 };
	int i, q = 0;

	__atomic_add_fetch(&s->inv_gen, 1, __ATOMIC_ACQ_REL);
	if (uc->through && uc->rewarm_max > 0)    /* S421's scope */
		pcache_ht_iter_meta(s->shadow, name_collect, &nl);
	flush_shadow(s);
	if (nl.n) {
		pthread_mutex_lock(&uc->mx);
		for (i = 0; i < nl.n; i++)
			q += rewarm_add_locked(uc, nl.k[i].s, (size_t)nl.k[i].len);
		pthread_mutex_unlock(&uc->mx);
		LM_NOTICE("collection '%s': the tracking of %s changed - its %d "
			"copy(ies) are dropped and %d re-warmed in the background\n",
			uc->name, s->addr, nl.n, q);
	}
	for (i = 0; i < nl.n; i++)
		free(nl.k[i].s);
	free(nl.k);
	if (uc->through)
		inv_post_all(uc);              /* AR1: so may the records written */
}

/* S424 (AR13, 2026-10-20): an invalidation of @k (all keys: @k NULL)
 * while a fetch of it is in flight.  The fetch may have read the value
 * BEFORE the change, and its reply - on the fetch connection - may land
 * after this push (on the invalidation connection) was handled: the copy
 * it stores would be older than Redis's, and nothing invalidates it again
 * (245, 14:07:40: the previous value served 500 ms and more after a
 * write).  The entry is marked; when its fetch completes, the copy goes
 * and the key is fetched again (conn_main) */
static void inflight_inv(struct up_col *uc, const char *k, size_t kl)
{
	struct up_key *e;

	pthread_mutex_lock(&uc->mx);
	for (e = uc->inflight; e; e = e->next)
		if (!k || (e->klen == kl && !memcmp(e->key, k, kl))) {
			if (!e->inv)
				ST_ADD(uc->inv_inflight, 1);
			e->inv = 1;
		}
	pthread_mutex_unlock(&uc->mx);
}

/* push messages (RESP3 '>') between replies.  ["invalidate", [keys]] drops
 * those keys from the shadow; ["invalidate", null] (a FLUSHALL upstream)
 * drops everything; "tracking-redir-broken" on a fetch connection means
 * its invalidations have nowhere to go - it re-arms */
void on_push(struct uconn *c, struct rv *v)
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
			inflight_inv(uc, NULL, 0);                  /* S424 */
			if (uc->through)
				inv_post_all(uc);      /* AR1 */
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
			inflight_inv(uc, k.s, (size_t)k.len);       /* S424 */
			if (uc->through)
				inv_post(uc, k.s, (size_t)k.len);   /* AR1 */
			ST_ADD(uc->invalidations, 1);
			ST_ADD(c->s->invalidations, 1);
		}
		return;
	}
	if (n->len == 21 && !memcmp(n->s, "tracking-redir-broken", 21))
		c->gen = (unsigned int)-1;     /* re-arm before the next fetch */
}

/* read ONE reply (pushes handled in between).  0 ok, -1 error, -2 timeout,
 * -3 a value too large to hold (the connection must be dropped).  S385:
 * @sp/@ep, when given, the reply's bytes in c->buf [*sp, *ep) - valid until
 * the next read - for a relay that copies them as they came */
/* ---- one fetch --------------------------------------------------------- */

/* a DURATION (ms) as an expiry tick - the negative cache, a cover window:
 * rounded up, a second at least */
static unsigned int ttl_ticks(long long ms)
{
	long long s = (ms + 999) / 1000;

	if (s < 1)
		s = 1;
	if (s > 0x7fffffffLL)
		s = 0x7fffffffLL;
	return get_ticks() + (unsigned int)s;
}

/* S449: an upstream KEY's time left (PTTL, ms) as its copy's expiry tick -
 * the last tick boundary at or before the key's deadline, so the copy never
 * outlives the key and keeps all but the deadline's fraction of a tick.
 * ttl_ticks rounded the seconds up, on a tick up to a second old: a copy -
 * an EXISTS's present mark among them - answered for up to a second after
 * Redis had expired the key (staging 2026-10-25, a registration's
 * device_info: EXISTS 1 from the copy, then JSON.SET $.status refused, "new
 * objects must be created at the root").  A deadline inside the current
 * tick: that tick, which reads expired - nothing held for that fraction */
static unsigned int pttl_ticks(long long ms)
{
	unsigned int e = compat_ticks_deadline(ms);

	return e ? e : 1;                  /* 0 is "never" */
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
	exp = tt.t == ':' && tt.n >= 0 ? pttl_ticks(tt.n) : 0;   /* S449 */

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

/* ---- 12nt (0.7.1): a pull answered after the fetch -------------------------
 * In shard mode a through key is fetched by its OWNER: a peer that missed
 * it pulls from the owner (op_get_miss), and the owner, holding nothing,
 * fetches from Redis instead of answering absent.  The fetch completes on
 * the upstream worker, which stores the copy in the shadow as for a client
 * read; the asker is queued here and the cluster thread (the only sender
 * on the cluster socket) reads the copy and sends the M_PULL_RSP. */
static struct {
	pthread_mutex_t mx;
	struct up_pull **q;
	int n, cap;
} PA = { PTHREAD_MUTEX_INITIALIZER, NULL, 0, 0 };

static void pa_push(struct up_pull *p)
{
	pthread_mutex_lock(&PA.mx);
	if (PA.n == PA.cap) {
		int nc = PA.cap ? PA.cap * 2 : 32;
		struct up_pull **nq = realloc(PA.q, (size_t)nc * sizeof *nq);

		if (!nq) {
			pthread_mutex_unlock(&PA.mx);
			free(p);                   /* the asker's slot times out */
			return;
		}
		PA.q = nq;
		PA.cap = nc;
	}
	PA.q[PA.n++] = p;
	pthread_mutex_unlock(&PA.mx);      /* the cluster loop's next beat takes it */
}

int pc_upstream_pull_fetch(const char *col, size_t collen, const char *key,
		size_t klen, uint32_t addr, uint16_t port, uint32_t req)
{
	struct up_col *uc = ncols ? col_of(col, collen) : NULL;
	struct up_req *r;
	struct up_pull *p;

	if (!uc || !uc->through || klen > INT_MAX || !allowed(uc, key, klen))
		return 0;
	{
		/* the owner's negative marker answers a pull as it answers a
		 * client's read (gate_rt): a key every upstream said is absent
		 * is not fetched again within upstream_negative_ms */
		pcache_htable_t *ht = pc_store_find(uc->name, uc->nlen);
		unsigned char fl = 0, mark = 0;
		unsigned int exp = 0;
		str k;

		k.s = (char *)key;
		k.len = (int)klen;
		if (ht && pcache_ht_shadow_peek(ht, &k, &fl, &mark, &exp) == 0 &&
		    (fl & PCACHE_F_UPMARK) && mark == PCACHE_UPMARK_ABSENT) {
			ST_ADD(uc->neg_hits, 1);
			return 0;
		}
	}
	r = calloc(1, sizeof *r);
	p = malloc(sizeof *p + klen);
	if (!r || !p) {
		free(r);
		free(p);
		return 0;
	}
	p->addr = addr;
	p->port = port;
	p->req = req;
	p->col = uc->idx;
	p->klen = klen;
	memcpy(p->key, key, klen);
	r->worker = -1;
	r->status = PC_UPV_OK;
	r->id = __atomic_fetch_add(&next_id, 1, __ATOMIC_RELAXED);
	r->pull = p;
	pthread_mutex_lock(&uc->mx);
	if (join_key(uc, key, klen, r, 0, -1, 1) != 0 || !r->left) {
		pthread_mutex_unlock(&uc->mx);
		free(r);
		free(p);
		return 0;
	}
	ST_ADD(uc->pull_fetches, 1);
	pthread_mutex_unlock(&uc->mx);
	return 1;
}

int pc_upstream_pull_answer_take(uint32_t *addr, uint16_t *port, uint32_t *req,
		char *col, size_t ccap, size_t *collen, char *key, size_t kcap,
		size_t *klen)
{
	struct up_pull *p;
	struct up_col *uc;

	pthread_mutex_lock(&PA.mx);
	if (!PA.n) {
		pthread_mutex_unlock(&PA.mx);
		return 0;
	}
	p = PA.q[0];
	memmove(PA.q, PA.q + 1, (size_t)(PA.n - 1) * sizeof *PA.q);
	PA.n--;
	pthread_mutex_unlock(&PA.mx);
	uc = p->col >= 0 && p->col < ncols ? &up_cols[p->col] : NULL;
	if (!uc || uc->nlen >= ccap || p->klen >= kcap) {
		free(p);
		return pc_upstream_pull_answer_take(addr, port, req, col, ccap,
			collen, key, kcap, klen);
	}
	*addr = p->addr;
	*port = p->port;
	*req = p->req;
	memcpy(col, uc->name, uc->nlen);
	col[uc->nlen] = 0;
	*collen = uc->nlen;
	memcpy(key, p->key, p->klen);
	*klen = p->klen;
	free(p);
	return 1;
}

/* ---- 12nt TC-2 (0.7.1): a forwarded write run through at the owner ------
 * In shard mode a write-through on a non-owner is forwarded as every write
 * is (pc_upstream_gate_fw PROCEEDs; pc_op_* -> pc_fwd_begin -> the owner's
 * handle_fwd_op).  The owner composes the Redis command the op means and
 * queues it for the collection's writer like a client's write - the master
 * first, the keys read back, wt_apply stores what Redis holds HERE, at the
 * owner - and the completion acks the asker instead of re-running a frame.
 * Redis's reply text does not fit the binary ack: ok is "applied", newval
 * the number the op's ack carries; a refusal at the master is ok 0 (the
 * asker says "holder rejected") and its text stays in last_errors here. */
static struct {
	pthread_mutex_t mx;
	struct up_fwd_ack {
		uint32_t addr;
		uint16_t port;
		uint32_t req;
		int ok;
		long long newval;
	} *q;
	int n, cap;
} FA = { PTHREAD_MUTEX_INITIALIZER, NULL, 0, 0 };

static void fa_queue(uint32_t addr, uint16_t port, uint32_t req, int ok,
		long long newval)
{
	struct up_fwd_ack *a;

	pthread_mutex_lock(&FA.mx);
	if (FA.n == FA.cap) {
		int nc = FA.cap ? FA.cap * 2 : 32;
		struct up_fwd_ack *nq = realloc(FA.q, (size_t)nc * sizeof *nq);

		if (!nq) {
			pthread_mutex_unlock(&FA.mx);
			return;                    /* the asker's slot times out */
		}
		FA.q = nq;
		FA.cap = nc;
	}
	a = &FA.q[FA.n++];
	a->addr = addr;
	a->port = port;
	a->req = req;
	a->ok = ok;
	a->newval = newval;
	pthread_mutex_unlock(&FA.mx);      /* the cluster loop's next beat sends it */
}

/* the forward plane's answer to what Redis said (handle_fwd_op's ok/newval
 * for each op, with the master deciding instead of the holder's table) */
static void fa_push(struct up_fwd *f, const struct up_done *d)
{
	int iserr = d->wst == UPW_R_ERR ||
		(d->val && d->vlen > 0 && (d->val[0] == '-' || d->val[0] == '!'));
	int ok = 0;
	long long nv = 0;

	if (iserr) {
		if (d->val && strstr(d->val, "WRONGTYPE"))
			nv = PC_FWD_E_TYPE;        /* S280/S297: the type error, typed */
	} else {
		switch (f->op) {
		case 0:                        /* SET: +OK */
		case 1:                        /* DEL: the count */
		case 3:                        /* EXPIRE/PERSIST: 1 re-armed */
			ok = d->ival != 0;
			break;
		case 2:                        /* INCRBY/DECRBY: the new value */
			ok = 1;
			nv = d->ival;
			break;
		case 4: case 5:                /* SET NX/XX: decided; stored? */
			ok = 1;
			nv = d->ival != 0;
			break;
		default:
			break;
		}
	}
	fa_queue(f->addr, f->port, f->req, ok, nv);
	free(f);
}

int pc_upstream_fwd_through(const char *col, size_t collen, int op,
		const char *key, size_t klen, const char *val, size_t vlen,
		unsigned int ttl_rel, long long delta, uint32_t addr, uint16_t port,
		uint32_t req)
{
	struct up_col *uc = ncols ? col_of(col, collen) : NULL;
	const char *av[6], *a2[3];
	size_t al[6], l2[3];
	char tb[24], db[24];
	static const int k1[1] = { 1 };
	struct pc_upfw fw;
	struct up_wjob *j;
	struct up_fwd *f;
	int ac = 0, n2 = 0, guess = UPWG_STRING;

	if (!uc || !uc->through || op < 0 || op > 5 || !allowed(uc, key, klen))
		return 0;                      /* not a through key here: as before */
	/* the Redis command the op means (handle_fwd_op's contract: ttl_rel in
	 * seconds, PCACHE_EXP_PRESERVE keeps the key's, 0 none) */
	switch (op) {
	case 0: case 4: case 5:
		av[ac] = "SET"; al[ac++] = 3;
		av[ac] = key; al[ac++] = klen;
		av[ac] = val; al[ac++] = vlen;
		if (op != 0) {
			av[ac] = op == 4 ? "NX" : "XX";
			al[ac++] = 2;
		}
		if (ttl_rel && ttl_rel != PCACHE_EXP_PRESERVE) {
			av[ac] = "EX"; al[ac++] = 2;
			av[ac] = tb;
			al[ac++] = (size_t)snprintf(tb, sizeof tb, "%u", ttl_rel);
		}
		break;
	case 1:
		av[ac] = "DEL"; al[ac++] = 3;
		av[ac] = key; al[ac++] = klen;
		guess = UPWG_GONE;
		break;
	case 2:
		av[ac] = delta < 0 ? "DECRBY" : "INCRBY"; al[ac++] = 6;
		av[ac] = key; al[ac++] = klen;
		av[ac] = db;
		al[ac++] = (size_t)snprintf(db, sizeof db, "%lld",
			delta < 0 ? -delta : delta);
		if (ttl_rel != PCACHE_EXP_PRESERVE) {
			a2[0] = ttl_rel ? "EXPIRE" : "PERSIST";
			l2[0] = ttl_rel ? 6 : 7;
			a2[1] = key;
			l2[1] = klen;
			n2 = 2;
			if (ttl_rel) {
				a2[2] = tb;
				l2[2] = (size_t)snprintf(tb, sizeof tb, "%u", ttl_rel);
				n2 = 3;
			}
		}
		break;
	case 3:
		if (ttl_rel && ttl_rel != PCACHE_EXP_PRESERVE) {
			av[ac] = "EXPIRE"; al[ac++] = 6;
			av[ac] = key; al[ac++] = klen;
			av[ac] = tb;
			al[ac++] = (size_t)snprintf(tb, sizeof tb, "%u", ttl_rel);
		} else {
			av[ac] = "PERSIST"; al[ac++] = 7;
			av[ac] = key; al[ac++] = klen;
		}
		break;
	}
	memset(&fw, 0, sizeof fw);
	fw.argc = ac;
	fw.argv = av;
	fw.argl = al;
	fw.argc2 = n2;
	fw.argv2 = a2;
	fw.argl2 = l2;
	fw.nk = 1;
	fw.kidx = k1;
	fw.guess = guess;
	j = wt_job_new(&fw);
	f = j ? malloc(sizeof *f) : NULL;
	if (!j || !f) {
		wt_job_free(j);
		free(f);
		return -1;                     /* refused now: the asker hears no */
	}
	f->addr = addr;
	f->port = port;
	f->req = req;
	f->op = op;
	j->fwd = f;
	j->worker = inv_worker();
	j->id = __atomic_fetch_add(&next_id, 1, __ATOMIC_RELAXED);
	if (!j->id)
		j->id = __atomic_fetch_add(&next_id, 1, __ATOMIC_RELAXED);
	pthread_mutex_lock(&uc->mx);
	if (j->worker < 0 ||
	    uc->wq_n + __atomic_load_n(&uc->w_inflight, __ATOMIC_RELAXED) >=
	    uc->wq_max) {
		static const char qmsg[] = "upstream: the write-through queue is "
			"full - a peer's forwarded write not applied";

		pthread_mutex_unlock(&uc->mx);
		ST_ADD(uc->wt_qfull, 1);
		wt_err_add(uc, "refused", av[0], al[0], key, klen, qmsg,
			sizeof qmsg - 1);
		wt_job_free(j);
		free(f);
		return -1;
	}
	wt_queue(uc, j);
	pthread_mutex_unlock(&uc->mx);
	ST_ADD(uc->fwd_through, 1);
	return 1;
}

int pc_upstream_fwd_ack_take(uint32_t *addr, uint16_t *port, uint32_t *req,
		int *ok, long long *newval)
{
	struct up_fwd_ack a;

	pthread_mutex_lock(&FA.mx);
	if (!FA.n) {
		pthread_mutex_unlock(&FA.mx);
		return 0;
	}
	a = FA.q[0];
	memmove(FA.q, FA.q + 1, (size_t)(FA.n - 1) * sizeof *FA.q);
	FA.n--;
	pthread_mutex_unlock(&FA.mx);
	*addr = a.addr;
	*port = a.port;
	*req = a.req;
	*ok = a.ok;
	*newval = a.newval;
	return 1;
}

int pc_upstream_pull_extra_ms(const char *col, size_t collen)
{
	struct up_col *uc = ncols ? col_of(col, collen) : NULL;

	return uc && uc->through ? uc->timeout_ms + 100 : 0;
}

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
			if (r->pull)
				pa_push(r->pull);  /* 12nt: the cluster thread answers */
			else
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
/* AR1: upstream_prefixes as BCAST PREFIXes: "abc*" is PREFIX abc, an exact
 * key is a PREFIX of itself (a superset - inv_apply filters on the
 * patterns), and "*" or a wildcard anywhere else means every key (no
 * PREFIX at all; the cost is an invalidation for every change Redis makes
 * in the database).  Returns how many it set in @pv/@pl, or 0 for every
 * key. */
static int bcast_prefixes(const struct up_col *uc, const char **pv, size_t *pl,
		int max)
{
	int i, n = 0;

	for (i = 0; i < uc->npat && n < max; i++) {
		const char *p = uc->pat[i];
		size_t l = (size_t)uc->patlen[i], m;

		for (m = 0; m < l; m++)
			if (p[m] == '*' || p[m] == '?' || p[m] == '[' || p[m] == '\\')
				break;
		if (m == l) {                  /* an exact key */
			pv[n] = p;
			pl[n++] = l;
			continue;
		}
		if (m == 0 || m != l - 1 || p[m] != '*')
			return 0;                  /* "*", or a glob a prefix cannot say */
		pv[n] = p;
		pl[n++] = m;
	}
	return i < uc->npat ? 0 : n;
}

/* AR1: this connection hears every change Redis makes under the
 * collection's prefixes - not only the keys fetched through it */
static void bcast_on(struct up_srv *s, struct uconn *c, long long dl)
{
	struct up_col *uc = s->uc;
	const char *av[4 + 2 * 32];
	size_t al[4 + 2 * 32];
	const char *pv[32];
	size_t pl[32];
	int n = 4, np, i, ok;
	struct rv v;

	av[0] = "CLIENT"; al[0] = 6;
	av[1] = "TRACKING"; al[1] = 8;
	av[2] = "ON"; al[2] = 2;
	av[3] = "BCAST"; al[3] = 5;
	np = bcast_prefixes(uc, pv, pl, 32);
	for (i = 0; i < np; i++) {
		av[n] = "PREFIX"; al[n++] = 6;
		av[n] = pv[i]; al[n++] = pl[i];
	}
	ok = send_cmd(c, dl, n, av, al) == 0 && read_reply(c, &v, dl) == 0;
	if (ok) {
		ok = is_ok(&v);
		if (!ok)
			LM_WARN("collection '%s': %s refused broadcast tracking "
				"(%.*s) - a change another Redis client makes to a "
				"key written through here is NOT seen until the key "
				"expires.  Allow CLIENT TRACKING for the upstream user "
				"(Redis 6+)\n", uc->name, s->addr,
				(int)(v.s ? (v.len < 120 ? v.len : 120) : 1),
				v.s ? v.s : "?");
		rv_free(&v);
	}
	__atomic_store_n(&s->bcast, ok ? 1 : -1, __ATOMIC_RELAXED);
	if (ok)
		LM_NOTICE("collection '%s': broadcast tracking on %s - %s\n",
			uc->name, s->addr, np ? "the collection's prefixes" :
			"every key (upstream_prefixes is not a list of prefixes)");
}

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
			__atomic_store_n(&s->inv_ready, 1, __ATOMIC_RELEASE);   /* S431 */
			LM_NOTICE("collection '%s': tracking on - %s's "
				"invalidations reach connection %lld on server "
				"%.12s\n", uc->name, s->addr, s->inv_id, c.run_id);
			if (uc->through)               /* AR1 */
				bcast_on(s, &c, now_ms() + uc->timeout_ms);
		}
		rc = read_reply(&c, &v, now_ms() + 1000);
		rewarm_tick(uc);               /* S421: at most once a second quiet */
		if (rc == -2)
			continue;                  /* quiet: nothing changed */
		if (rc != 0) {
			LM_WARN("collection '%s': the tracking connection to %s "
				"was lost - the copies fetched from it are dropped\n",
				uc->name, s->addr);
			__atomic_store_n(&s->inv_ready, 0, __ATOMIC_RELEASE);   /* S431 */
			uc_close(&c);
			__atomic_store_n(&s->inv_id, 0, __ATOMIC_RELEASE);
			__atomic_store_n(&s->bcast, 0, __ATOMIC_RELAXED);   /* AR1 */
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
			/* S424: overtaken by an invalidation of the key - what it
			 * stored may be older than the change: dropped, and the
			 * key fetched again (its waiters stay); three rounds at
			 * most, then the copy is kept for the waiters */
			if (e->inv && e->rounds < 3) {
				str ik;

				ik.s = e->key;
				ik.len = (int)e->klen;
				e->inv = 0;
				e->rounds++;
				rm_begin(uc);                      /* S435 */
				pcache_ht_remove(s->shadow, &ik);
				rm_end(uc);
				enqueue(s, e);
				pthread_mutex_unlock(&uc->mx);
				ST_ADD(uc->inv_refetch, 1);
				continue;
			}
			if (e->inv) {
				/* S433: ...but no longer than an untracked fill.  The
				 * change that overtook it spent its invalidation on
				 * this fetch, and nothing else will reach the copy:
				 * kept whole, it answered the value before the key's
				 * last change until the key changed again (eight
				 * writers on one key, fetches slower than the writes:
				 * upwritethroughtest 15, "Redis v46, node 'v47'") */
				str ik;
				unsigned char kf = 0, km = 0;
				unsigned int kx = 0, cap = ttl_ticks(uc->neg_ms > 0 ?
					uc->neg_ms : 1000);

				ik.s = e->key;
				ik.len = (int)e->klen;
				if (pcache_ht_peek(s->shadow, &ik, &kf, &km, &kx) == 0 &&
				    (!kx || kx > cap))
					pcache_ht_touch(s->shadow, &ik, cap);
				ST_ADD(uc->inv_kept, 1);
			}
			if (e->fetched == UPF_PRESENT && e->full) {
				enqueue(s, e);
				pthread_mutex_unlock(&uc->mx);
				ST_ADD(uc->refetches, 1);
				continue;
			}
			/* S431: a re-warm's copy that landed untracked anyway, or on
			 * a connection armed before this server's tracking moved
			 * while the fetch was out (a loss: its invalidations go
			 * nowhere; or the flush took it and the name was not
			 * collected): it goes, and the name is queued again - the
			 * run fetches it from a server whose tracking is settled */
			if (e->rw && (fill_capped || c.gen !=
			        __atomic_load_n(&s->inv_gen, __ATOMIC_ACQUIRE))) {
				str ik;

				ik.s = e->key;
				ik.len = (int)e->klen;
				rm_begin(uc);                      /* S435 */
				pcache_ht_remove(s->shadow, &ik);
				rm_end(uc);
				rewarm_add_locked(uc, e->key, e->klen);
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

/* ---- write-through (S385, DESIGN 12mf) --------------------------------------
 * upstream_writes = through: a client write to the collection goes to the
 * upstream's MASTER first.  One thread per collection (wr_main) holds one
 * RESP2 connection to it and takes the queued writes in the order the
 * workers queued them.  Each write is pipelined with a read-back of its
 * keys (TYPE, PTTL and the value), and the worker that queued it makes the
 * fleet hold exactly what Redis holds after it (UPD_WT, wt_apply) - so the
 * local state follows Redis's even when the client has gone meanwhile -
 * then the parked command answers with Redis's own reply.  Unknown commands
 * (upstream_unknown = pass) are classified by Redis's COMMAND INFO: a write
 * the same way, a read on a replica by the pass reader (pr_main).
 * Checks never scale with traffic: the master and RedisJSON are looked for
 * at the start, at a reconnect and after a refusal, at most once per
 * backoff step (1 s doubling to 10 s; RedisJSON missing: every 60 s). */

#define WT_BATCH       32              /* writes per pipelined round (AR3) */
#define WT_ROLE_MS     10000           /* the master's ROLE, while idle */
#define WT_BACKOFF_MAX 10000
#define WT_JSON_MS     60000           /* RedisJSON missing: asked again */
#define WT_OWNED_MS    60000           /* AR4: the fleet's own keys counted */
#define WT_UNK_CACHE   128             /* commands classed per collection */


/* a growing byte buffer (a relayed reply, a job's copy) */
struct wbuf {
	char *p;
	size_t n, cap;
};

static int wb_put(struct wbuf *b, const void *p, size_t n)
{
	if (b->n + n > b->cap) {
		size_t nc = b->cap ? b->cap : 256;
		char *np;

		while (nc < b->n + n)
			nc *= 2;
		np = realloc(b->p, nc);
		if (!np)
			return -1;
		b->p = np;
		b->cap = nc;
	}
	memcpy(b->p + b->n, p, n);
	b->n += n;
	return 0;
}

/* S385 stats: set on the writer thread - only its writes are counted by
 * command (the pass reader's are reads) */
static __thread int wt_is_writer;

/* printable ASCII of @s[@n] into @d (a key or Redis's text may hold
 * anything; the page and the log want neither control bytes nor UTF-8
 * cut in half) */
static void wt_asc(char *d, size_t dn, const char *s, size_t n)
{
	size_t i;

	if (!s)
		n = 0;
	if (n >= dn)
		n = dn - 1;
	for (i = 0; i < n; i++)
		d[i] = s[i] >= 32 && s[i] < 127 ? s[i] : '?';
	d[n] = 0;
}

static void wt_err_add(struct up_col *uc, const char *kind, const char *cmd,
		size_t cl, const char *key, size_t kl, const char *msg, size_t ml)
{
	struct wt_err *e;
	size_t i;

	pthread_mutex_lock(&uc->mx);
	e = &uc->werr[uc->werr_n++ % WT_NERR];
	e->at = (long long)time(NULL);
	snprintf(e->kind, sizeof e->kind, "%s", kind);
	wt_asc(e->cmd, sizeof e->cmd, cmd, cl);
	for (i = 0; e->cmd[i]; i++)
		e->cmd[i] = (char)tolower((unsigned char)e->cmd[i]);
	wt_asc(e->key, sizeof e->key, key, kl);
	while (ml && (msg[ml - 1] == '\r' || msg[ml - 1] == '\n'))
		ml--;
	wt_asc(e->msg, sizeof e->msg, msg, ml);
	pthread_mutex_unlock(&uc->mx);
}

/* the row of command @nm (case folded); the last row takes the rest */
static struct wt_cmd *wc_row(struct up_col *uc, const char *nm, size_t nl,
		int passed)
{
	char lc[24];
	int i, n = __atomic_load_n(&uc->nwcmd, __ATOMIC_ACQUIRE);
	struct wt_cmd *r;

	if (nl >= sizeof lc)
		nl = sizeof lc - 1;
	for (i = 0; i < (int)nl; i++)
		lc[i] = (char)tolower((unsigned char)nm[i]);
	lc[nl] = 0;
	for (i = 0; i < n; i++)
		if (!strcmp(uc->wcmd[i].name, lc))
			return &uc->wcmd[i];
	if (n == WT_NCMD)
		return &uc->wcmd[WT_NCMD - 1];
	r = &uc->wcmd[n];
	if (n == WT_NCMD - 1)
		strcpy(r->name, "other");
	else {
		memcpy(r->name, lc, nl + 1);
		r->passed = passed;
	}
	__atomic_store_n(&uc->nwcmd, n + 1, __ATOMIC_RELEASE);
	return r;
}

/* one write's outcome (WC_*), by its command, timed from its queueing
 * unless it failed; an error or a failure is also kept with its text */
static void wt_note(struct up_col *uc, const struct up_wjob *j, int what,
		const char *msg, size_t ml)
{
	struct wt_cmd *r;
	const char *k = NULL;
	size_t kl = 0;
	long long us;

	if (!wt_is_writer || !j || j->argc < 1)
		return;
	r = wc_row(uc, j->argv[0], j->argl[0], j->unk);
	ST_ADD(r->c[what], 1);
	us = now_us() - j->tq;
	if (what != WC_FAIL && us >= 0) {
		ST_ADD(r->c[WC_US], (unsigned long long)us);
		ST_ADD(r->c[WC_TN], 1);
		pc_winmax_note(&uc->w_maxus, get_ticks(), (unsigned long long)us);
	}
	if (what != WC_RERR && what != WC_FAIL)
		return;
	if (j->nkeys > 0 && j->kidx) {
		k = j->argv[j->kidx[0]];
		kl = j->argl[j->kidx[0]];
	} else if (j->argc > 1) {
		k = j->argv[1];
		kl = j->argl[1];
	}
	wt_err_add(uc, what == WC_RERR ? "redis" : "failed", j->argv[0],
		j->argl[0], k, kl, msg, ml);
}

/* a write refused here, before any round trip: counted, and kept with
 * the client's command and why */
static void wt_refused_note(struct up_col *uc, const char *what, size_t wl,
		const char *msg)
{
	ST_ADD(uc->wt_refused, 1);
	wt_err_add(uc, "refused", what ? what : "", what ? wl : 0, NULL, 0,
		msg, strlen(msg));
}

void pc_upstream_wt_refused(const char *col, size_t collen, const char *what,
		size_t wl, const char *msg)
{
	struct up_col *uc;

	if (!ncols)
		return;
	uc = col_of(col, collen);
	if (uc && uc->through)
		wt_refused_note(uc, what, wl, msg);
}

static void wt_finish(struct up_col *uc, struct up_wjob *j, int wst,
		const char *val, size_t vlen, long long ival, struct up_rb *rb,
		int nrb)
{
	struct up_done d;

	memset(&d, 0, sizeof d);
	d.kind = UPD_WT;
	d.id = j->id;
	d.col = uc->idx;
	d.wseq = ++uc->wt_seq;             /* S429: the writer thread alone counts */
	d.fwd = j->fwd;                    /* 12nt: the drain acks the asker */
	j->fwd = NULL;
	d.wst = wst;
	d.ival = ival;
	d.rb = rb;
	d.nrb = nrb;
	if (val) {
		d.val = malloc(vlen + 1);
		if (d.val) {
			memcpy(d.val, val, vlen);
			d.val[vlen] = 0;
			d.vlen = vlen;
		}
	}
	post_done(j->worker, &d);
}

static void wt_fail(struct up_col *uc, struct up_wjob *j, const char *msg)
{
	ST_ADD(uc->wt_failed, 1);
	if (j->upfill) {                   /* AR4 */
		ST_ADD(uc->upf_errors, 1);
		ST_ADD(uc->upf_answered, 1);
	}
	wt_note(uc, j, WC_FAIL, msg, strlen(msg));
	wt_finish(uc, j, UPW_R_ERR, msg, strlen(msg), 0, NULL, 0);
}

/* AR2: the time a write must have left to be sent - a quarter of
 * upstream_timeout_ms, at least 5 ms */
static long long wt_need_ms(const struct up_col *uc)
{
	long long need = uc->timeout_ms / 4;

	return need < 5 ? 5 : need;
}

static void wt_job_free(struct up_wjob *j)
{
	if (j)
		free(j->fwd);                  /* 12nt: a context not handed over */
	free(j);
}

static void wt_why(struct up_col *uc, const char *fmt, const char *a)
{
	pthread_mutex_lock(&uc->mx);
	snprintf(uc->w_why, sizeof uc->w_why, fmt, a);
	pthread_mutex_unlock(&uc->mx);
}

/* is @s's address the one a replica names as its master?  By name, then by
 * the addresses both resolve to (a replica names its master by IP) */
static int same_host(const char *a, const char *b)
{
	struct addrinfo hints, *ra = NULL, *rb = NULL, *x, *y;
	int same = 0;

	if (!strcasecmp(a, b))
		return 1;
	memset(&hints, 0, sizeof hints);
	hints.ai_family = pc_resolve_family();     /* 12np: no AAAA without IPv6 */
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(a, NULL, &hints, &ra) != 0)
		return 0;
	if (getaddrinfo(b, NULL, &hints, &rb) != 0) {
		freeaddrinfo(ra);
		return 0;
	}
	for (x = ra; x && !same; x = x->ai_next)
		for (y = rb; y && !same; y = y->ai_next)
			if (x->ai_family == y->ai_family &&
			    x->ai_addrlen == y->ai_addrlen) {
				pc_addr ax, ay;

				if (pc_addr_from_sa(&ax, x->ai_addr, x->ai_addrlen) == 0 &&
				    pc_addr_from_sa(&ay, y->ai_addr, y->ai_addrlen) == 0)
					same = pc_addr_eq_host(&ax, &ay);
			}
	freeaddrinfo(ra);
	freeaddrinfo(rb);
	return same;
}

/* RedisJSON on the master: COMMAND INFO JSON.SET JSON.DEL (an ordinary ACL
 * user may run it; MODULE LIST is @admin).  1 yes, 0 no, -1 the connection
 * failed */
static int wt_json_check(struct uconn *c)
{
	struct up_col *uc = c->uc;
	const char *av[4] = { "COMMAND", "INFO", "JSON.SET", "JSON.DEL" };
	size_t al[4] = { 7, 4, 8, 8 };
	long long dl = now_ms() + uc->timeout_ms;
	struct rv v;
	int yes;

	ST_ADD(uc->wt_checks, 1);
	if (send_cmd(c, dl, 4, av, al) != 0 || read_reply(c, &v, dl) != 0)
		return -1;
	yes = v.t == '*' && v.nel == 2 && v.el[0].t == '*' && v.el[1].t == '*';
	rv_free(&v);
	return yes;
}

/* the master among the listed upstreams: every one asked ROLE (a replica
 * also says which master it follows); one master is it, two are decided by
 * the replicas, none or a tie is no target.  On success *@c is connected to
 * it.  The caller rate-limits */
static int wt_discover(struct up_col *uc, struct uconn *c)
{
	int masters[PC_UP_MAX_SRV], nm = 0, j, k, pick = -1, state;
	long long dl;

	uc_close(c);
	for (j = 0; j < uc->nsrv; j++) {
		struct uconn p;

		memset(&p, 0, sizeof p);
		p.uc = uc;
		p.s = &uc->srv[j];
		p.fd = -1;
		p.wr = 1;
		p.wcred = 1;                   /* AR7: the writer's */
		dl = now_ms() + uc->timeout_ms;
		if (uc_connect(&p, dl) != 0)
			continue;
		if (__atomic_load_n(&uc->srv[j].role, __ATOMIC_RELAXED) == UPR_MASTER)
			masters[nm++] = j;
		uc_close(&p);
		free(p.buf);
	}
	if (nm == 1)
		pick = masters[0];
	else if (nm > 1) {
		/* S385's split-brain guard: the master the serving replicas
		 * follow; a master nobody follows is one cut off */
		int votes[PC_UP_MAX_SRV], best = -1, tie = 0;

		for (k = 0; k < nm; k++) {
			struct up_srv *m = &uc->srv[masters[k]];

			votes[k] = 0;
			for (j = 0; j < uc->nsrv; j++) {
				struct up_srv *r = &uc->srv[j];
				char mh[64];

				if (__atomic_load_n(&r->role, __ATOMIC_RELAXED) != UPR_REPLICA ||
				    __atomic_load_n(&r->link_down, __ATOMIC_RELAXED))
					continue;
				pthread_mutex_lock(&r->run_mx);
				memcpy(mh, r->mhost, sizeof mh);
				pthread_mutex_unlock(&r->run_mx);
				if (__atomic_load_n(&r->mport, __ATOMIC_RELAXED) == m->port &&
				    mh[0] && same_host(mh, m->host))
					votes[k]++;
			}
		}
		for (k = 0; k < nm; k++) {
			if (!votes[k])
				continue;
			if (best < 0)
				best = k;
			else
				tie = 1;
		}
		if (best >= 0 && !tie)
			pick = masters[best];
	}
	state = pick >= 0 ? UPWS_OK : nm > 1 ? UPWS_AMBIG : UPWS_NOMASTER;
	if (pick >= 0) {
		memset(c, 0, offsetof(struct uconn, buf));
		c->uc = uc;
		c->s = &uc->srv[pick];
		c->fd = -1;
		c->wr = 1;
		c->wcred = 1;
		dl = now_ms() + uc->timeout_ms;
		if (uc_connect(c, dl) != 0 || __atomic_load_n(&uc->srv[pick].role,
		        __ATOMIC_RELAXED) != UPR_MASTER) {
			uc_close(c);
			pick = -1;
			state = UPWS_DOWN;
		}
	}
	if (pick >= 0) {
		int js = wt_json_check(c);

		if (js < 0) {
			uc_close(c);
			pick = -1;
			state = UPWS_DOWN;
		} else {
			int was = __atomic_exchange_n(&uc->w_json, js, __ATOMIC_RELAXED);

			if (!js && was != 0)
				LM_WARN("collection '%s': the master %s has no RedisJSON "
					"(COMMAND INFO JSON.SET) - JSON writes are refused, "
					"not forwarded; asked again every %d s\n", uc->name,
					uc->srv[pick].addr, WT_JSON_MS / 1000);
		}
	}
	if (__atomic_load_n(&uc->w_master, __ATOMIC_RELAXED) != pick) {
		if (pick >= 0)
			LM_NOTICE("collection '%s': write-through to the master %s\n",
				uc->name, uc->srv[pick].addr);
		else
			LM_WARN("collection '%s': write-through has no target - %s; "
				"writes are refused until one is found\n", uc->name,
				upws_name[state]);
	}
	__atomic_store_n(&uc->w_master, pick, __ATOMIC_RELAXED);
	__atomic_store_n(&uc->w_state, state, __ATOMIC_RELAXED);
	if (pick < 0)
		wt_why(uc, "%s", upws_name[state]);
	return pick >= 0 ? 0 : -1;
}

/* one key's read-back, its replies already read: TYPE's name, PTTL, and the
 * value asked with them (the guess); 1 a second ask is needed (*@need the
 * command), 0 *@rb filled */
static int rb_from(struct up_col *uc, const char *tname, const struct rv *tt,
		const struct rv *gv, int asked, struct up_rb *rb, int *need)
{
	long long pttl = tt->t == ':' ? tt->n : -1;
	int want;

	rb->exp = pttl >= 0 ? pttl_ticks(pttl) : 0;   /* S449: as its copies */
	if (!strcmp(tname, "none") || pttl == -2) {
		rb->what = UPRB_GONE;
		return 0;
	}
	want = !strcmp(tname, "string") ? UPWG_STRING : !strcmp(tname, "hash") ?
		UPWG_HASH : !strcmp(tname, "ReJSON-RL") ? UPWG_JSON : -1;
	if (want < 0) {
		rb->what = UPRB_RELEASE;       /* list, set, zset, stream, a module */
		return 0;
	}
	if (want != asked) {
		*need = want;
		return 1;
	}
	if (want == UPWG_HASH) {
		unsigned char *rec = NULL;
		long n;

		if ((gv->t != '*' && gv->t != '%') || gv->nel == 0) {
			rb->what = UPRB_GONE;
			return 0;
		}
		n = hash_record(gv, &rec);
		if (n < 0) {
			rb->what = UPRB_RELEASE;   /* larger than a record: Redis's */
			return 0;
		}
		rb->val = (char *)rec;
		rb->vlen = (size_t)n;
		rb->fl = PCACHE_F_HASH;
	} else {
		if (gv->t != '$' && gv->t != '+') {
			rb->what = UPRB_GONE;      /* changed meanwhile */
			return 0;
		}
		rb->val = malloc(gv->len ? gv->len : 1);
		if (!rb->val) {
			rb->what = UPRB_RELEASE;
			return 0;
		}
		memcpy(rb->val, gv->s, gv->len);
		rb->vlen = gv->len;
		rb->fl = want == UPWG_JSON ? PCACHE_F_JSON : 0;
	}
	rb->what = UPRB_VALUE;
	(void)uc;
	return 0;
}

static const char *const rb_cmd[3] = { "GET", "HGETALL", "JSON.GET" };
static const size_t rb_cmdl[3] = { 3, 7, 8 };

/* send the read-back of @k: TYPE, PTTL and the guessed value */
static int rb_send(struct uconn *c, long long dl, const char *k, size_t kl,
		int guess)
{
	const char *a1[2] = { "TYPE", k }, *a2[2] = { "PTTL", k }, *a3[2];
	size_t l1[2] = { 4, kl }, l2[2] = { 4, kl }, l3[2];

	if (send_cmd(c, dl, 2, a1, l1) != 0 || send_cmd(c, dl, 2, a2, l2) != 0)
		return -1;
	if (guess == UPWG_GONE)
		return 0;
	a3[0] = rb_cmd[guess];
	l3[0] = rb_cmdl[guess];
	a3[1] = k;
	l3[1] = kl;
	return send_cmd(c, dl, 2, a3, l3);
}

/* read one key's read-back (its replies follow in order), a second ask on
 * the same connection when the guess was the wrong type.  0 ok, <0 the
 * connection (read_reply's code) */
static int rb_read(struct uconn *c, long long dl, const char *k, size_t kl,
		int guess, struct up_rb *rb)
{
	struct rv ty, tt, gv;
	char tname[32];
	size_t tn;
	int rc, need = -1;

	memset(&gv, 0, sizeof gv);
	if ((rc = read_reply(c, &ty, dl)) != 0)
		return rc;
	tn = ty.t == '+' && ty.len < sizeof tname ? ty.len : 0;
	memcpy(tname, ty.s ? ty.s : "", tn);
	tname[tn] = 0;
	rv_free(&ty);
	if ((rc = read_reply(c, &tt, dl)) != 0)
		return rc;
	if (guess != UPWG_GONE && (rc = read_reply(c, &gv, dl)) != 0) {
		rv_free(&tt);
		return rc;
	}
	rc = rb_from(c->uc, tname, &tt, &gv, guess, rb, &need);
	rv_free(&gv);
	if (rc == 1) {
		/* the type was not the one asked: its value now */
		const char *a[2] = { rb_cmd[need], k };
		size_t l[2] = { rb_cmdl[need], kl };

		if (send_cmd(c, dl, 2, a, l) != 0) {
			rv_free(&tt);
			return -1;
		}
		if ((rc = read_reply(c, &gv, dl)) != 0) {
			rv_free(&tt);
			return rc;
		}
		rc = rb_from(c->uc, tname, &tt, &gv, need, rb, &need);
		rv_free(&gv);
		if (rc == 1)
			rb->what = UPRB_RELEASE;   /* changed type twice: Redis's */
	}
	rv_free(&tt);
	rb->key = malloc(kl ? kl : 1);
	if (!rb->key) {
		free(rb->val);
		rb->val = NULL;
		return 0;                      /* nothing applied for it */
	}
	memcpy(rb->key, k, kl);
	rb->klen = kl;
	return 0;
}

static void rb_free(struct up_rb *rb, int n)
{
	int i;

	if (!rb)
		return;                        /* failed before it was made */
	for (i = 0; i < n; i++) {
		free(rb[i].key);
		free(rb[i].val);
	}
	free(rb);
}

/* the number a native verb answers with: OK 1, an integer itself, nil and
 * anything else 0 */
static long long reply_ival(const struct rv *v)
{
	if (v->t == ':')
		return v->n;
	if (v->t == '+')
		return 1;
	return 0;
}

/* ---- the unknown commands' classes (upstream_unknown = pass) -------------- */

#define UK_WRITE   0x01
#define UK_READ    0x02
#define UK_REFUSE  0x04                /* admin, dangerous, blocking, pubsub,
                                        * another database, a transaction */
#define UK_MOVABLE 0x08                /* keys only COMMAND GETKEYS knows */
#define UK_NONE    0x10                /* Redis has no such command */

struct uk_ent {
	char name[40];
	int flags, first, last, step;
	const char *why;                   /* UK_REFUSE: what to say */
};

struct uk_cache {
	struct uk_ent e[WT_UNK_CACHE];
	int n;
};

static int rv_has(const struct rv *a, const char *w)
{
	int i;
	size_t wl = strlen(w);

	if (!a || (a->t != '*' && a->t != '~'))
		return 0;
	for (i = 0; i < a->nel; i++)
		if (a->el[i].s && a->el[i].len == wl &&
		    !strncasecmp(a->el[i].s, w, wl))
			return 1;
	return 0;
}

/* Redis's COMMAND INFO of @name (a container's "name|sub"): flags, the key
 * positions and the ACL categories decide.  Cached for the master's life */
static const struct uk_ent *uk_class(struct uconn *c, struct uk_cache *uk,
		const char *name, size_t nl)
{
	struct up_col *uc = c->uc;
	const char *av[3] = { "COMMAND", "INFO", name };
	size_t al[3] = { 7, 4, nl };
	long long dl = now_ms() + uc->timeout_ms;
	struct uk_ent *e;
	struct rv v;
	int i;

	for (i = 0; i < uk->n; i++)
		if (strlen(uk->e[i].name) == nl && !strncasecmp(uk->e[i].name, name, nl))
			return &uk->e[i];
	if (nl >= sizeof uk->e[0].name)
		return NULL;
	if (send_cmd(c, dl, 3, av, al) != 0 || read_reply(c, &v, dl) != 0)
		return NULL;
	ST_ADD(uc->wt_checks, 1);
	e = uk->n < WT_UNK_CACHE ? &uk->e[uk->n++] : &uk->e[WT_UNK_CACHE - 1];
	memset(e, 0, sizeof *e);
	memcpy(e->name, name, nl);
	if (v.t != '*' || v.nel < 1 || v.el[0].t != '*' || v.el[0].nel < 6) {
		e->flags = UK_NONE;
	} else {
		const struct rv *d = &v.el[0], *fl = &d->el[2],
			*cat = d->nel > 6 ? &d->el[6] : NULL;

		e->first = d->el[3].t == ':' ? (int)d->el[3].n : 0;
		e->last = d->el[4].t == ':' ? (int)d->el[4].n : 0;
		e->step = d->el[5].t == ':' ? (int)d->el[5].n : 1;
		e->flags = rv_has(fl, "write") ? UK_WRITE : UK_READ;
		if (rv_has(fl, "movablekeys") || (d->nel > 9 && d->el[9].t == '*' &&
		    d->el[9].nel > 0))
			e->flags |= UK_MOVABLE;
		if (rv_has(fl, "admin") || rv_has(cat, "@admin") ||
		    rv_has(cat, "@dangerous")) {
			e->flags |= UK_REFUSE;
			e->why = "an admin or dangerous command";
		} else if (rv_has(fl, "blocking") || rv_has(cat, "@blocking")) {
			e->flags |= UK_REFUSE;
			e->why = "a blocking command";
		} else if (rv_has(fl, "pubsub") || rv_has(cat, "@pubsub")) {
			e->flags |= UK_REFUSE;
			e->why = "pub/sub - not bridged (S387)";
		} else if (rv_has(cat, "@transaction") || rv_has(fl, "no_multi") ||
		    rv_has(cat, "@scripting")) {
			e->flags |= UK_REFUSE;
			e->why = "transactions and scripts are not passed";
		}
	}
	rv_free(&v);
	return e;
}

/* the keys of @argv, as Redis sees them: positions, or COMMAND GETKEYS.
 * Returns how many (into kidx), 0 keyless, -1 the connection failed */
static int uk_keys(struct uconn *c, const struct uk_ent *e, int argc,
		char *const *argv, const size_t *argl, int *kidx, int max)
{
	struct up_col *uc = c->uc;
	int n = 0, i;

	if (!(e->flags & UK_MOVABLE)) {
		int last = e->last < 0 ? argc + e->last : e->last;

		if (e->first <= 0)
			return 0;
		for (i = e->first; i <= last && i < argc && n < max;
		     i += e->step > 0 ? e->step : 1)
			kidx[n++] = i;
		return n;
	}
	{
		/* COMMAND GETKEYS <the command>: names the keys, which are
		 * found among the arguments again (by content) */
		const char **av = malloc((size_t)(argc + 2) * sizeof *av);
		size_t *al = malloc((size_t)(argc + 2) * sizeof *al);
		long long dl = now_ms() + uc->timeout_ms;
		struct rv v;
		int rc;

		if (!av || !al) {
			free(av);
			free(al);
			return -1;
		}
		av[0] = "COMMAND";
		al[0] = 7;
		av[1] = "GETKEYS";
		al[1] = 7;
		for (i = 0; i < argc; i++) {
			av[i + 2] = argv[i];
			al[i + 2] = argl[i];
		}
		rc = send_cmd(c, dl, argc + 2, av, al) != 0 ||
			read_reply(c, &v, dl) != 0;
		free(av);
		free(al);
		if (rc)
			return -1;
		if (v.t == '*')
			for (i = 0; i < v.nel && n < max; i++) {
				int a;

				for (a = 1; a < argc; a++)
					if (v.el[i].s && argl[a] == v.el[i].len &&
					    !memcmp(argv[a], v.el[i].s, argl[a])) {
						kidx[n++] = a;
						break;
					}
			}
		rv_free(&v);
		return n;
	}
}

/* ---- the writer --------------------------------------------------------- */

/* AR2: a write whose deadline went (all but a quarter) while it queued */
#define WT_EXPIRED_MSG "upstream: the write waited too long for the master " \
	"(under a quarter of upstream_timeout_ms left) - not applied"

/* run one write on the master: the command (and its second, if any), each
 * key's read-back after it, all in one round trip; the worker is told.  0
 * done (whatever Redis answered), -1 the connection failed (the job is
 * answered with the error; the caller reconnects) */
/* the commands after a job's first - @n2 of its trailing ones, then the
 * read-back of every key */
static int wt_send_rest(struct uconn *c, long long dl,
		const struct up_wjob *j, const int *kidx, int nk, int n2)
{
	int k;

	for (k = 0; k < n2; k++)
		if (send_cmd(c, dl, j->argc2, (const char *const *)j->argv +
		        j->argc + k * j->argc2, j->argl + j->argc + k * j->argc2) != 0)
			return -1;
	for (k = 0; k < nk; k++)
		if (rb_send(c, dl, j->argv[kidx[k]], j->argl[kidx[k]],
		        j->guess) != 0)
			return -1;
	return 0;
}

/* ---- S400: what Redis's value replaces ------------------------------------
 * An upfill whose NX Redis declined reads Redis's value back, and wt_apply
 * stores it as the fleet's record on every member.  Before that, it is
 * compared with the value the sweep offered: the same, the same with
 * another expiry, or different (another value, another type, or Redis no
 * longer holds it).  `upfill <col> check` makes the same comparison on its
 * own connection and applies nothing. */
#define UPF_SAME     0
#define UPF_RETIMED  1
#define UPF_DIFFERED 2
#define UPF_MISSING  3                 /* check: Redis lacks it - a run writes it */
#define UPF_NAME_MAX 100               /* differing keys named per sweep */
#define UPF_EXP_SLACK 2                /* ticks: the PTTL's rounding */
#define UPF_JDEPTH   64                /* deeper: compared as text */

/* an integer literal: digits, perhaps a minus - compared by its digits,
 * since a double holds only 2^53 of them exactly */
static int jint(const char *b, const struct pc_jtok *t)
{
	int i = t->start;

	if (i < t->end && b[i] == '-')
		i++;
	if (i >= t->end)
		return 0;
	for (; i < t->end; i++)
		if (b[i] < '0' || b[i] > '9')
			return 0;
	return 1;
}

/* a number's value, from its token's text; 0 not a finite number */
static int jnum(const char *b, const struct pc_jtok *t, double *d)
{
	char tmp[64], *e;
	int n = t->end - t->start;

	if (n <= 0 || n >= (int)sizeof tmp)
		return 0;
	memcpy(tmp, b + t->start, (size_t)n);
	tmp[n] = 0;
	*d = strtod(tmp, &e);
	return *e == 0 && *d == *d && *d - *d == 0;
}

/* the token after @i's whole value */
static int jnext(const struct pc_jtok *t, int n, int i)
{
	int j = i + 1;

	while (j < n && t[j].start < t[i].end)
		j++;
	return j;
}

/* a string token, unescaped; -1 a bad escape */
static int jstr(const char *b, const struct pc_jtok *t, char **out)
{
	size_t cap = (size_t)(t->end - t->start) + 1;
	int n;

	*out = malloc(cap);
	if (!*out)
		return -1;
	n = pc_json_unescape(b, t, *out, cap);
	if (n < 0) {
		free(*out);
		*out = NULL;
	}
	return n;
}

/* whitespace JSON allows between tokens */
static int jws(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* the bytes the text comparison stops at: outside a string, whitespace and
 * the quote; inside one, the quote and the backslash */
static const unsigned char jsp_out[256] = { [' '] = 1, ['\t'] = 1, ['\n'] = 1,
	['\r'] = 1, ['"'] = 1 };
static const unsigned char jsp_in[256] = { ['"'] = 1, ['\\'] = 1 };

/* the same document spelt with other spacing, settled without parsing:
 * byte for byte, whitespace outside strings skipped - equal runs of plain
 * bytes in one tight loop, the special ones one at a time.  1 the same
 * text, 0 not decided here (the walk below decides) */
static int json_same_text(const char *a, size_t al, const char *b, size_t bl)
{
	size_t i = 0, j = 0;
	int ins = 0;

	for (;;) {
		if (ins) {
			while (i < al && j < bl && a[i] == b[j] &&
			       !jsp_in[(unsigned char)a[i]]) {
				i++;
				j++;
			}
		} else {
			for (;;) {
				while (i < al && j < bl && a[i] == b[j] &&
				       !jsp_out[(unsigned char)a[i]]) {
					i++;
					j++;
				}
				if (i < al && jws(a[i]))
					i++;
				else if (j < bl && jws(b[j]))
					j++;
				else
					break;
			}
		}
		if (i == al || j == bl)
			return i == al && j == bl;
		if (a[i] != b[j])
			return 0;
		/* equal and special: outside, the quote; inside, the quote or
		 * an escape, whose next byte is taken as it is */
		if (!ins)
			ins = 1;
		else if (a[i] == '\\') {
			i++;
			j++;
			if (i == al || j == bl || a[i] != b[j])
				return 0;
		} else
			ins = 0;
		i++;
		j++;
	}
}

/* two string tokens: their raw spellings first, unescaped only when a
 * backslash lets two spellings be one value.  1 equal, 0 not, -1 not
 * decided (out of memory, a bad escape) */
static int jstreq(const char *ab, const struct pc_jtok *x, const char *bb,
		const struct pc_jtok *y)
{
	int lx = x->end - x->start, ly = y->end - y->start, r;
	char *sx, *sy;

	if (lx == ly && !memcmp(ab + x->start, bb + y->start, (size_t)lx))
		return 1;
	if (!memchr(ab + x->start, '\\', (size_t)lx) &&
	    !memchr(bb + y->start, '\\', (size_t)ly))
		return 0;
	if ((lx = jstr(ab, x, &sx)) < 0)
		return -1;
	if ((ly = jstr(bb, y, &sy)) < 0) {
		free(sx);
		return -1;
	}
	r = lx == ly && !memcmp(sx, sy, (size_t)lx);
	free(sx);
	free(sy);
	return r;
}

/* an object's members by raw name, for the lookup when the order differs */
struct jkey {
	const char *s;
	int n;
	int v;                             /* its value's token */
};

static int jkey_cmp(const void *p, const void *q)
{
	const struct jkey *a = p, *b = q;
	int c = memcmp(a->s, b->s, (size_t)(a->n < b->n ? a->n : b->n));

	return c ? c : a->n - b->n;
}

/* the value token of the member of y (b[ib]) named as a[ka], or -1 none,
 * -2 not decided.  *@ix is built at the first call (sorted raw names;
 * NULL with *@esc set when a name holds a backslash - then a scan) */
static int jfind(const char *ab, const struct pc_jtok *a, int ka,
		const char *bb, const struct pc_jtok *b, int nb, int ib,
		struct jkey **ix, int *nix, int *esc)
{
	const struct pc_jtok *y = &b[ib];
	struct jkey key, *hit;
	int j, r;

	if (!*ix && !*esc) {
		*ix = malloc((size_t)(y->size ? y->size : 1) * sizeof **ix);
		if (!*ix)
			return -2;
		for (j = ib + 1, *nix = 0; j < nb && b[j].start < y->end &&
		     *nix < y->size; j = jnext(b, nb, j + 1)) {
			(*ix)[*nix].s = bb + b[j].start;
			(*ix)[*nix].n = b[j].end - b[j].start;
			(*ix)[*nix].v = j + 1;
			if (memchr((*ix)[*nix].s, '\\', (size_t)(*ix)[*nix].n))
				*esc = 1;
			(*nix)++;
		}
		if (*esc) {
			free(*ix);
			*ix = NULL;
		} else
			qsort(*ix, (size_t)*nix, sizeof **ix, jkey_cmp);
	}
	key.s = ab + a[ka].start;
	key.n = a[ka].end - a[ka].start;
	if (*ix && !memchr(key.s, '\\', (size_t)key.n)) {
		hit = bsearch(&key, *ix, (size_t)*nix, sizeof **ix, jkey_cmp);
		return hit ? hit->v : -1;
	}
	for (j = ib + 1; j < nb && b[j].start < y->end; j = jnext(b, nb, j + 1))
		if ((r = jstreq(ab, &a[ka], bb, &b[j])) != 0)
			return r < 0 ? -2 : j + 1;
	return -1;
}

/* are a[ia] and b[ib] the same JSON value: members by name in any order,
 * elements in order, strings unescaped, numbers by value.  1 yes, 0 no,
 * -1 not decided here (too deep, out of memory) */
static int jeq(const char *ab, const struct pc_jtok *a, int na, int ia,
		const char *bb, const struct pc_jtok *b, int nb, int ib, int depth)
{
	const struct pc_jtok *x = &a[ia], *y = &b[ib];
	struct jkey *ix = NULL;
	int i, j, r, nix = 0, esc = 0;

	if (depth > UPF_JDEPTH)
		return -1;
	if (x->type != y->type || x->size != y->size)
		return 0;
	if (x->type == PC_J_PRIM) {
		double dx, dy;
		int lx = x->end - x->start, ly = y->end - y->start;

		if (lx == ly && !memcmp(ab + x->start, bb + y->start, (size_t)lx))
			return 1;
		if (jint(ab, x) && jint(bb, y))        /* "-0" and "0" aside */
			return lx + ly == 3 && (ab[x->start] == '-' ?
				!memcmp(ab + x->start, "-0", 2) && bb[y->start] == '0' :
				!memcmp(bb + y->start, "-0", 2) && ab[x->start] == '0');
		return jnum(ab, x, &dx) && jnum(bb, y, &dy) && dx == dy;
	}
	if (x->type == PC_J_STR)
		return jstreq(ab, x, bb, y);
	if (x->type == PC_J_ARR) {
		for (i = ia + 1, j = ib + 1; i < na && j < nb && a[i].start < x->end;
		     i = jnext(a, na, i), j = jnext(b, nb, j))
			if ((r = jeq(ab, a, na, i, bb, b, nb, j, depth + 1)) != 1)
				return r;
		return 1;
	}
	/* an object: each of x's members in y by name - in the same place
	 * first (RedisJSON keeps members in the order they were written),
	 * looked up when it is not there */
	r = 1;
	for (i = ia + 1, j = ib + 1; r == 1 && i < na && a[i].start < x->end;
	     i = jnext(a, na, i + 1)) {
		int v = -1;

		if (j < nb && b[j].start < y->end) {
			if ((r = jstreq(ab, &a[i], bb, &b[j])) < 0)
				break;
			if (r)
				v = j + 1;
			j = jnext(b, nb, j + 1);
		}
		if (v < 0)
			v = jfind(ab, a, i, bb, b, nb, ib, &ix, &nix, &esc);
		r = v >= 0 ? jeq(ab, a, na, i + 1, bb, b, nb, v, depth + 1) :
			v == -1 ? 0 : -1;
	}
	free(ix);
	return r;
}

/* the walk's tokens: one buffer a thread, reused - a document of n bytes
 * has at most n / 2 + 1 of them (every value but the first follows a
 * [, : or , and takes a byte).  One larger than UPF_JTOK_KEEP is freed
 * after use, so a rare big document pins nothing */
#define UPF_JTOK_KEEP 65536
static __thread struct pc_jtok *jtok;
static __thread size_t jtok_cap;

static void json_same_release(void)
{
	free(jtok);
	jtok = NULL;
	jtok_cap = 0;
}

/* two documents: 1 the same JSON value, 0 not, -1 not decided (deeper than
 * the walk goes, refused by the tokenizer, out of memory) - counted
 * uncompared, never different (S402).  *@walked: the text alone did not
 * settle it */
static int json_same(const char *a, size_t al, const char *b, size_t bl,
		int *walked)
{
	size_t ca = al / 2 + 2, cb = bl / 2 + 2;
	int na = -1, nb = -1, r = -1;

	*walked = 0;
	if (json_same_text(a, al, b, bl))
		return 1;
	*walked = 1;
	if (ca + cb > jtok_cap) {
		struct pc_jtok *t = realloc(jtok, (ca + cb) * sizeof *t);

		if (t) {
			jtok = t;
			jtok_cap = ca + cb;
		}
	}
	if (ca + cb <= jtok_cap) {
		na = pc_json_parse(a, al, jtok, (int)ca);
		nb = pc_json_parse(b, bl, jtok + ca, (int)cb);
	}
	if (na > 0 && nb > 0)
		r = jeq(a, jtok, na, 0, b, jtok + ca, nb, 0, 0);
	if (jtok_cap > UPF_JTOK_KEEP)
		json_same_release();
	return r;
}

#define UPF_UNCOMPARED 4                /* S402: not decided - counted apart */
#define UPF_HOW_TEXT   0                /* the bytes (or the text pass) said */
#define UPF_HOW_WALKED 1                /* a document walked */

/* Redis's read-back against what the fleet had: UPF_*; *@how the path */
static int upf_compare(const char *v, size_t vl, int json, unsigned int exp,
		const struct up_rb *rb, int *how)
{
	int same, walked = 0;

	*how = UPF_HOW_TEXT;
	if (rb->what == UPRB_GONE)
		return UPF_MISSING;
	if (rb->what != UPRB_VALUE)
		return UPF_DIFFERED;           /* a type the fleet cannot hold */
	if (!!(rb->fl & PCACHE_F_JSON) != !!json || (rb->fl & PCACHE_F_HASH))
		return UPF_DIFFERED;
	same = json ? json_same(v, vl, rb->val, rb->vlen, &walked) :
		vl == rb->vlen && !memcmp(v, rb->val, vl);
	if (walked)
		*how = UPF_HOW_WALKED;
	if (same < 0)
		return UPF_UNCOMPARED;
	if (!same)
		return UPF_DIFFERED;
	if (!exp != !rb->exp)
		return UPF_RETIMED;
	if (exp && (exp > rb->exp ? exp - rb->exp : rb->exp - exp) > UPF_EXP_SLACK)
		return UPF_RETIMED;
	return UPF_SAME;
}

/* "never" or "in N s" */
static void upf_exp_txt(char *d, size_t dn, unsigned int exp)
{
	unsigned int now = get_ticks();

	if (!exp)
		snprintf(d, dn, "never");
	else
		snprintf(d, dn, "in %u s", exp > now ? exp - now : 0);
}

/* count one comparison; a value that differs is named (never its bytes:
 * a collection may hold credentials), at most UPF_NAME_MAX a sweep.  In a
 * check, Redis lacking the key means a run would write it */
static void upf_judge(struct up_col *uc, const char *k, size_t kl,
		const char *v, size_t vl, int json, unsigned int exp,
		const struct up_rb *rb)
{
	int how, r = upf_compare(v, vl, json, exp, rb, &how);
	char kb[128], e1[24], e2[24];

	if (r == UPF_MISSING && !uc->upf_check)
		r = UPF_DIFFERED;              /* declined, then deleted: gone here too */
	if (r == UPF_UNCOMPARED) {         /* S402: never folded into differed */
		ST_ADD(uc->upf_uncompared, 1);
		return;
	}
	if (r != UPF_MISSING)              /* S402: how a verdict on a key Redis had came */
		ST_ADD(*(how == UPF_HOW_WALKED ? &uc->upf_cmp_walked :
			&uc->upf_cmp_text), 1);
	ST_ADD(*(r == UPF_SAME ? &uc->upf_same : r == UPF_RETIMED ?
		&uc->upf_retimed : r == UPF_MISSING ? &uc->upf_missing :
		&uc->upf_differed), 1);
	if (r != UPF_DIFFERED)
		return;
	if (__atomic_fetch_add(&uc->upf_named, 1, __ATOMIC_RELAXED) >=
	    UPF_NAME_MAX)
		return;
	wt_asc(kb, sizeof kb, k, kl);
	upf_exp_txt(e1, sizeof e1, exp);
	if (rb->what != UPRB_VALUE) {
		LM_NOTICE("collection '%s': upfill%s: Redis %s '%s' - %s\n",
			uc->name, uc->upf_check ? " check" : "",
			rb->what == UPRB_GONE ? "no longer holds" :
			"holds a type the fleet cannot keep for", kb,
			uc->upf_check ? "a run would drop the fleet's copy" :
			"the fleet's copy is dropped on every member");
		return;
	}
	upf_exp_txt(e2, sizeof e2, rb->exp);
	LM_NOTICE("collection '%s': upfill%s: Redis holds '%s' with another "
		"value - %s (the fleet: %s, %zu bytes, expires %s; Redis: %s, "
		"%zu bytes, expires %s)\n", uc->name, uc->upf_check ? " check" : "",
		kb, uc->upf_check ? "a run would replace the fleet's with it" :
		"it replaces the fleet's on every member", json ? "a document" :
		"a string", vl, e1, (rb->fl & PCACHE_F_JSON) ? "a document" :
		(rb->fl & PCACHE_F_HASH) ? "a hash" : "a string", rb->vlen, e2);
}

/* ---- S402: the comparison on the sweep's own thread ------------------------
 * The writer carries every write-through write of the collection; a
 * document's walk there held the clients' writes behind it (S400's bench,
 * 12nk).  It hands pc-upf-<col> copies of the key, the value offered and
 * Redis's read-back, and goes on - the verdict only feeds counters and the
 * log, and the read-back is applied as before.  The queue is bounded by
 * entries and bytes; the writer never waits on it, and a key that does
 * not fit is counted uncompared.  The sweep's thread drains it between
 * keys and in its pacing sleeps, and the sweep is done when it is empty. */
#define UPF_CQ_MAX   1024
#define UPF_CQ_BYTES (8u << 20)

struct upf_cj {
	struct upf_cj *next;
	char *key, *val;
	size_t klen, vlen, bytes;
	int json;
	unsigned int exp;
	struct up_rb rb;                   /* Redis's: what, its own val copy */
};

static void upf_cj_free(struct upf_cj *c)
{
	free(c->key);
	free(c->val);
	free(c->rb.val);
	free(c);
}

/* the writer: one declined NX's comparison to the sweep's thread */
static void upf_handoff(struct up_col *uc, const struct up_wjob *j,
		const struct up_rb *rb)
{
	const char *v = j->argv[j->upf_vi];
	size_t vl = j->argl[j->upf_vi], rl = rb->what == UPRB_VALUE ? rb->vlen : 0;
	size_t bytes = rb->klen + vl + rl;
	struct upf_cj *c;
	int fits;

	/* a look without the lock first: a full queue costs no allocation */
	if (__atomic_load_n(&uc->upf_qn, __ATOMIC_RELAXED) >= UPF_CQ_MAX ||
	    __atomic_load_n(&uc->upf_qbytes, __ATOMIC_RELAXED) + bytes >
	    UPF_CQ_BYTES || !(c = calloc(1, sizeof *c)))
		goto uncompared;
	c->key = malloc(rb->klen ? rb->klen : 1);
	c->val = malloc(vl ? vl : 1);
	c->rb.val = malloc(rl ? rl : 1);   /* a byte for an empty value too */
	if (!c->key || !c->val || !c->rb.val) {
		upf_cj_free(c);
		goto uncompared;
	}
	memcpy(c->key, rb->key, rb->klen);
	memcpy(c->val, v, vl);
	if (rl)
		memcpy(c->rb.val, rb->val, rl);
	c->klen = rb->klen;
	c->vlen = vl;
	c->json = j->json;
	c->exp = j->upf_exp;
	c->rb.what = rb->what;
	c->rb.vlen = rl;
	c->rb.exp = rb->exp;
	c->rb.fl = rb->fl;
	c->bytes = bytes;
	pthread_mutex_lock(&uc->upf_qmx);
	fits = uc->upf_qn < UPF_CQ_MAX && uc->upf_qbytes + bytes <= UPF_CQ_BYTES;
	if (fits) {
		if (uc->upf_qt)
			uc->upf_qt->next = c;
		else
			uc->upf_qh = c;
		uc->upf_qt = c;
		__atomic_store_n(&uc->upf_qn, uc->upf_qn + 1, __ATOMIC_RELAXED);
		__atomic_store_n(&uc->upf_qbytes, uc->upf_qbytes + bytes,
			__ATOMIC_RELAXED);
	}
	pthread_mutex_unlock(&uc->upf_qmx);
	if (fits)
		return;
	upf_cj_free(c);
uncompared:
	ST_ADD(uc->upf_uncompared, 1);
}

/* the sweep's thread: judge what the writer handed over (@judge 0: drop
 * it, at shutdown) */
static void upf_drain_q(struct up_col *uc, int judge)
{
	struct upf_cj *c;

	for (;;) {
		pthread_mutex_lock(&uc->upf_qmx);
		c = uc->upf_qh;
		if (c) {
			uc->upf_qh = c->next;
			if (!uc->upf_qh)
				uc->upf_qt = NULL;
			__atomic_store_n(&uc->upf_qn, uc->upf_qn - 1,
				__ATOMIC_RELAXED);
			__atomic_store_n(&uc->upf_qbytes, uc->upf_qbytes - c->bytes,
				__ATOMIC_RELAXED);
		}
		pthread_mutex_unlock(&uc->upf_qmx);
		if (!c)
			return;
		if (judge)
			upf_judge(uc, c->key, c->klen, c->val, c->vlen, c->json,
				c->exp, &c->rb);
		upf_cj_free(c);
	}
}

/* a pacing sleep that keeps the comparisons moving */
static void upf_nap(struct up_col *uc, int ms)
{
	upf_drain_q(uc, 1);
	nap_ms(ms);
}

/* AR4: an upfill job with trailing commands sends them only after its
 * first command's answer (its PEXPIRE only where the NX applied) - it is
 * never pipelined behind or ahead of another job */
#define WT_EARLY(j) ((j)->upfill && (j)->n2)

/* AR2: a job that waited (almost) its whole deadline in the queue: not
 * sent, so certainly not applied - sent with a few ms left it would time
 * out too, and the next one behind it, each an uncertain "may or may
 * not".  1 failed here, 0 it may go */
static int wt_expired(struct up_col *uc, struct up_wjob *j, long long now)
{
	if (j->t0 + uc->timeout_ms - now >= wt_need_ms(uc))
		return 0;
	ST_ADD(uc->wt_qexpired, 1);
	wt_fail(uc, j, WT_EXPIRED_MSG);
	return 1;
}

/* a job's commands on the wire: its first, then - unless it waits for the
 * first's answer - the trailing ones and the read-backs.  0, or -1 the
 * connection failed (how much went out is not known) */
static int wt_put(struct uconn *c, const struct up_wjob *j, const int *kidx,
		int nk, long long dl)
{
	if (send_cmd(c, dl, j->argc, (const char *const *)j->argv, j->argl) != 0)
		return -1;
	if (!WT_EARLY(j) && wt_send_rest(c, dl, j, kidx, nk, j->n2) != 0)
		return -1;
	return 0;
}

#define WT_GET_DEMOTED -4              /* READONLY: the master stepped down */

/* a sent job's answer: its replies read in order, the fleet told, the
 * client answered.  0, or <0 the job failed here and the connection is
 * closed (WT_GET_DEMOTED: it answered READONLY) */
static int wt_get(struct up_col *uc, struct uconn *c, struct up_wjob *j,
		const int *kidx, int nk)
{
	long long dl = j->t0 + uc->timeout_ms;     /* AR2: from the queueing */
	struct up_rb *rb = NULL;
	struct wbuf out = { NULL, 0, 0 };
	struct rv v, v2;
	size_t sp = 0, ep = 0;
	long long ival;
	int rc, k, isnil, iserr;
	/* AR4: an upfill's trailing PEXPIRE waits for its NX's answer */
	int n2 = j->n2, early = WT_EARLY(j);
	char em[160];
	size_t eml = 0;

	if ((rc = read_reply_span(c, &v, dl, &sp, &ep)) != 0)
		goto io_rc;
	/* the reply as it came - copied NOW, the next read may move it */
	if (wb_put(&out, c->buf + sp, ep - sp) != 0) {
		rv_free(&v);
		goto io;
	}
	iserr = v.t == '-' || v.t == '!';
	isnil = v.t == '_';
	ival = reply_ival(&v);
	if (iserr) {                       /* its text, for the stats */
		eml = v.s ? (v.len < sizeof em ? v.len : sizeof em) : 0;
		if (eml)
			memcpy(em, v.s, eml);
	}
	if (iserr && v.s && v.len >= 8 && !strncmp(v.s, "READONLY", 8)) {
		/* the master was demoted: looked for again (rate-limited) */
		rv_free(&v);
		free(out.p);
		LM_WARN("collection '%s': %s answers READONLY - it is no longer "
			"the master\n", uc->name, c->s->addr);
		wt_fail(uc, j, "upstream: the master stepped down - write not "
			"applied, retry");
		uc_close(c);
		__atomic_store_n(&uc->w_master, -1, __ATOMIC_RELAXED);
		__atomic_store_n(&uc->w_state, UPWS_DOWN, __ATOMIC_RELAXED);
		return WT_GET_DEMOTED;
	}
	if (iserr && j->json && v.s && v.len >= 15 &&
	    !strncmp(v.s, "ERR unknown command", 19 <= v.len ? 19 : v.len)) {
		/* the refusal is the evidence: no RedisJSON there now */
		if (__atomic_exchange_n(&uc->w_json, 0, __ATOMIC_RELAXED) != 0)
			LM_WARN("collection '%s': the master %s refused a JSON "
				"command as unknown - JSON writes are refused until "
				"it has RedisJSON\n", uc->name, c->s->addr);
	}
	rv_free(&v);
	if (early) {
		/* AR4: the PEXPIRE only where the NX applied - a value Redis
		 * kept keeps Redis's expiry */
		if (iserr || isnil)
			n2 = 0;
		if (wt_send_rest(c, dl, j, kidx, nk, n2) != 0)
			goto io;
	}
	for (k = 0; k < n2; k++) {
		if ((rc = read_reply(c, &v2, dl)) != 0)
			goto io_rc;
		rv_free(&v2);
	}
	if (nk) {
		rb = calloc((size_t)nk, sizeof *rb);
		if (!rb)
			goto io;                   /* replies unread: reconnect */
	}
	for (k = 0; k < nk; k++)
		if ((rc = rb_read(c, dl, j->argv[kidx[k]], j->argl[kidx[k]],
		        j->guess, &rb[k])) != 0)
			goto io_rc;
	if (j->upfill) {                   /* AR4 */
		ST_ADD(*(iserr ? &uc->upf_errors : isnil ? &uc->upf_kept :
			&uc->upf_written), 1);
		if (!iserr && isnil) {             /* S400, S402: compared there */
			if (nk == 1 && rb[0].key)
				upf_handoff(uc, j, &rb[0]);
			else
				ST_ADD(uc->upf_uncompared, 1);
		}
		ST_ADD(uc->upf_answered, 1);
	}
	if (iserr || (isnil && !j->upfill)) {
		/* refused or not applied: the fleet keeps what it had - but an
		 * upfill's NX that Redis declined reads the key back all the
		 * same: Redis's value wins (AR4) */
		rb_free(rb, nk);
		rb = NULL;
		nk = 0;
		if (iserr)
			ST_ADD(uc->wt_rerr, 1);
		else
			ST_ADD(uc->wt_nil, 1);
	} else {
		ST_ADD(*(isnil ? &uc->wt_nil : &uc->wt_ok), 1);
		for (k = 0; k < nk; k++)
			ST_ADD(*(rb[k].what == UPRB_VALUE ? &uc->wt_rb_value :
				rb[k].what == UPRB_GONE ? &uc->wt_rb_gone :
				&uc->wt_rb_release), 1);
	}
	ST_ADD(uc->wt_us_sum, (unsigned long long)(now_us() - j->tq));
	ST_ADD(uc->wt_n, 1);
	wt_note(uc, j, iserr ? WC_RERR : isnil ? WC_NIL : WC_OK, em, eml);
	wt_finish(uc, j, UPW_R_DONE, out.p, out.n, ival, rb, nk);
	free(out.p);
	return 0;
io:
	rc = -1;
io_rc:
	rb_free(rb, nk);
	free(out.p);
	if (rc == -2)
		ST_ADD(uc->wt_timeouts, 1);
	if (rc == -3)
		wt_fail(uc, j, "upstream: the reply is larger than "
			"upstream_pass_max_kb - not passed");
	else
		wt_fail(uc, j, rc == -2 ? "upstream: the write-through timed out - "
			"it may or may not have been applied" : "upstream: the "
			"connection to the master failed - the write may or may not "
			"have been applied");
	uc_close(c);
	return -1;
}

/* one job alone: the quarter rule, its commands, its answer.  0, or -1 the
 * connection failed (the job answered) */
static int wt_run(struct up_col *uc, struct uconn *c, struct up_wjob *j,
		const int *kidx, int nk)
{
	if (wt_expired(uc, j, now_ms()))
		return 0;
	if (wt_put(c, j, kidx, nk, j->t0 + uc->timeout_ms) != 0) {
		wt_fail(uc, j, "upstream: the connection to the master failed - "
			"the write may or may not have been applied");
		uc_close(c);
		return -1;
	}
	ST_ADD(uc->wt_fwd, 1);
	return wt_get(uc, c, j, kidx, nk) < 0 ? -1 : 0;
}

/* AR3: a run of plain jobs pipelined - every job's commands first, then
 * the answers in the same order: per-key order holds (one connection,
 * Redis answers in order) and the run costs one round trip, not one a
 * write.  A send that fails: that job may or may not have been applied,
 * the ones after it were never sent - not applied.  An answer that fails:
 * that job and every one sent after it may or may not have been applied.
 * READONLY: the jobs sent with it went to a replica - not applied.  0, or
 * -1 the connection failed (every job answered) */
static int wt_pipe(struct up_col *uc, struct uconn *c, struct up_wjob **js,
		int n)
{
	long long now = now_ms();
	int i, lim = n, dead = 0, sent = 0;
	char *go = calloc((size_t)n, 1);   /* 1: on the wire */

	if (!go) {
		for (i = 0; i < n; i++)
			wt_fail(uc, js[i], "upstream: out of memory - write not "
				"applied");
		return 0;
	}
	for (i = 0; i < n; i++) {
		if (wt_expired(uc, js[i], now))
			continue;
		if (wt_put(c, js[i], js[i]->kidx, js[i]->nkeys,
		        js[i]->t0 + uc->timeout_ms) != 0) {
			wt_fail(uc, js[i], "upstream: the connection to the master "
				"failed - the write may or may not have been applied");
			lim = i;                   /* the answers before it may come */
			break;
		}
		ST_ADD(uc->wt_fwd, 1);
		go[i] = 1;
		sent++;
	}
	for (i = lim + 1; i < n; i++)
		wt_fail(uc, js[i], "upstream: the connection to the master failed "
			"- write not applied (never sent)");
	if (sent > 1)
		ST_ADD(uc->wt_pipelined, (unsigned long long)sent);
	for (i = 0; i < lim; i++) {
		int k;

		if (!go[i])
			continue;
		if (dead || c->fd < 0) {
			/* an answer before it failed, or the send side closed */
			wt_fail(uc, js[i], dead == WT_GET_DEMOTED ? "upstream: the "
				"master stepped down - write not applied, retry" :
				"upstream: the connection to the master failed - the "
				"write may or may not have been applied");
			if (!dead)
				dead = -1;
			continue;
		}
		k = wt_get(uc, c, js[i], js[i]->kidx, js[i]->nkeys);
		if (k < 0)
			dead = k;                  /* it answered; the conn is closed */
	}
	free(go);
	if (dead || lim < n) {
		uc_close(c);
		return -1;
	}
	return 0;
}

static void pr_queue(struct up_col *uc, struct up_wjob *j);

/* an unknown command: classed, then written here or read by the pass
 * reader.  0 answered (or handed on), -1 the connection failed */
static int wt_unknown(struct up_col *uc, struct uconn *c, struct uk_cache *uk,
		struct up_wjob *j)
{
	char nm[96];
	size_t nl = j->argl[0];
	const struct uk_ent *e;
	int kidx[64], nk, i;
	char msg[200];

	if (nl >= 40) {
		wt_fail(uc, j, "upstream: unknown command");
		return 0;
	}
	for (i = 0; i < (int)nl; i++)
		nm[i] = (char)tolower((unsigned char)j->argv[0][i]);
	nm[nl] = 0;
	/* never, whatever Redis says: another database, a transaction */
	if (!strcmp(nm, "move") || !strcmp(nm, "swapdb") || !strcmp(nm, "select") ||
	    !strcmp(nm, "multi") || !strcmp(nm, "exec") || !strcmp(nm, "discard") ||
	    !strcmp(nm, "watch") || !strcmp(nm, "unwatch")) {
		ST_ADD(uc->pass_refused, 1);
		snprintf(msg, sizeof msg, "upstream: %s is not passed (another "
			"database, or a transaction)", nm);
		wt_fail(uc, j, msg);
		return 0;
	}
	if (!strcmp(nm, "copy"))
		for (i = 3; i < j->argc; i++)
			if (j->argl[i] == 2 && !strncasecmp(j->argv[i], "DB", 2)) {
				ST_ADD(uc->pass_refused, 1);
				wt_fail(uc, j, "upstream: COPY ... DB is not passed "
					"(another database)");
				return 0;
			}
	e = uk_class(c, uk, j->argv[0], nl);
	if (e && (e->flags & UK_MOVABLE) && j->argc > 1 && nl + 1 + j->argl[1] < 40) {
		/* a container: its subcommand's class ("object|encoding") */
		char sub[96];
		size_t sl = 0;
		const struct uk_ent *se;

		memcpy(sub, j->argv[0], nl);
		sl = nl;
		sub[sl++] = '|';
		memcpy(sub + sl, j->argv[1], j->argl[1]);
		sl += j->argl[1];
		for (i = 0; i < (int)sl; i++)
			sub[i] = (char)tolower((unsigned char)sub[i]);
		se = uk_class(c, uk, sub, sl);
		if (se && !(se->flags & UK_NONE))
			e = se;
	}
	if (!e)
		return -1;
	if (e->flags & UK_NONE) {
		snprintf(msg, sizeof msg, "unknown command '%s' (the upstream has "
			"none either)", nm);
		ST_ADD(uc->pass_refused, 1);
		wt_fail(uc, j, msg);
		return 0;
	}
	if (e->flags & UK_REFUSE) {
		snprintf(msg, sizeof msg, "upstream: %s is not passed - %s", nm,
			e->why);
		ST_ADD(uc->pass_refused, 1);
		wt_fail(uc, j, msg);
		return 0;
	}
	nk = uk_keys(c, e, j->argc, j->argv, j->argl, kidx, 64);
	if (nk < 0)
		return -1;
	for (i = 0; i < nk; i++)
		if (!allowed(uc, j->argv[kidx[i]], j->argl[kidx[i]])) {
			ST_ADD(uc->pass_refused, 1);
			wt_fail(uc, j, "upstream: a key outside upstream_prefixes - "
				"not passed");
			return 0;
		}
	if (e->flags & UK_WRITE) {
		if (!nk) {
			snprintf(msg, sizeof msg, "upstream: %s is not passed - a "
				"write that names no key would change Redis under the "
				"fleet", nm);
			ST_ADD(uc->pass_refused, 1);
			wt_fail(uc, j, msg);
			return 0;
		}
		ST_ADD(uc->pass_w, 1);
		j->guess = UPWG_STRING;
		return wt_run(uc, c, j, kidx, nk);
	}
	ST_ADD(uc->pass_r, 1);
	pr_queue(uc, j);                   /* the reader owns it now */
	return 1;
}

/* the job queue: under uc->mx */
static void wt_queue(struct up_col *uc, struct up_wjob *j)
{
	j->next = NULL;
	if (uc->wq_tail)
		uc->wq_tail->next = j;
	else
		uc->wq_head = j;
	uc->wq_tail = j;
	uc->wq_n++;
	pthread_cond_signal(&uc->wcv);
}

/* a job from the forward spec: one allocation holding the argv of every
 * command, their lengths, the key indexes and the bytes */
static struct up_wjob *wt_job_new(const struct pc_upfw *fw)
{
	int n2 = fw->argc2 ? (fw->n2 > 0 ? fw->n2 : 1) : 0;
	int na2 = n2 * fw->argc2, na = fw->argc + na2, i, nk = fw->nk;
	size_t bytes = 0, sz;
	struct up_wjob *j;
	char *p;

	for (i = 0; i < fw->argc; i++)
		bytes += fw->argl[i];
	for (i = 0; i < na2; i++)
		bytes += fw->argl2[i];
	sz = sizeof *j + (size_t)na * (sizeof(char *) + sizeof(size_t)) +
		(size_t)(nk > 0 ? nk : 1) * sizeof(int) + bytes + 8;
	j = calloc(1, sz);
	if (!j)
		return NULL;
	j->argv = (char **)(j + 1);
	j->argl = (size_t *)(j->argv + na);
	j->kidx = (int *)(j->argl + na);
	p = (char *)(j->kidx + (nk > 0 ? nk : 1));
	for (i = 0; i < fw->argc; i++) {
		j->argv[i] = p;
		j->argl[i] = fw->argl[i];
		memcpy(p, fw->argv[i], fw->argl[i]);
		p += fw->argl[i];
	}
	for (i = 0; i < na2; i++) {
		j->argv[fw->argc + i] = p;
		j->argl[fw->argc + i] = fw->argl2[i];
		memcpy(p, fw->argv2[i], fw->argl2[i]);
		p += fw->argl2[i];
	}
	j->argc = fw->argc;
	j->argc2 = fw->argc2;
	j->n2 = n2;
	for (i = 0; i < nk; i++)
		j->kidx[i] = fw->kidx[i];
	j->nkeys = nk;
	j->json = fw->json;
	j->guess = fw->guess;
	j->unk = fw->unk;
	j->t0 = now_ms();
	j->tq = now_us();
	return j;
}

/* Redis's error text without its '-' and line end, for the native doors */
static __thread char wt_rerr_buf[256];

/* S385: a write on a write-through collection */
static int wt_gate(struct up_col *uc, const char *const *keys,
		const size_t *kl, int nkeys, int verdict, struct pc_upg *g,
		const struct pc_upfw *fw)
{
	struct up_wjob *j;
	int i, any = 0;

	if (verdict == PC_UPV_WT) {
		/* the re-run: the answer is the master's, and the worker
		 * already made the fleet hold what Redis holds */
		struct up_done *d = wt_cur;

		wt_cur = NULL;
		if (!d) {
			g->err = "upstream: the write-through's answer was lost";
			return PC_UPG_ANSWERED;
		}
		if (d->wst == UPW_R_ERR) {
			g->err = d->val ? d->val : "upstream: the write-through failed";
			return PC_UPG_ANSWERED;
		}
		g->done = 1;
		g->raw = d->val;
		g->rawlen = d->vlen;
		g->ival = d->ival;
		if (d->val && d->vlen > 1 && (d->val[0] == '-' || d->val[0] == '!')) {
			size_t n = d->vlen - 1;

			while (n && (d->val[n] == '\r' || d->val[n] == '\n'))
				n--;
			if (n >= sizeof wt_rerr_buf)
				n = sizeof wt_rerr_buf - 1;
			memcpy(wt_rerr_buf, d->val + 1, n);
			wt_rerr_buf[n] = 0;
			g->rerr = wt_rerr_buf;
		}
		return PC_UPG_ANSWERED;
	}
	if (!fw || fw->refuse) {
		g->err = fw && fw->refuse ? fw->refuse :
			"upstream_writes = through does not forward this command";
		if (fw && fw->what)
			wt_refused_note(uc, fw->what, fw->whatlen, g->err);
		else if (fw && fw->argc > 0)
			wt_refused_note(uc, fw->argv[0], fw->argl[0], g->err);
		else
			wt_refused_note(uc, NULL, 0, g->err);
		return PC_UPG_ANSWERED;
	}
	if (!fw->unk) {
		for (i = 0; i < nkeys; i++)
			if (allowed(uc, keys[i], kl[i]))
				any = 1;
		if (!any)
			return PC_UPG_PROCEED;     /* not the upstream's keys: here */
	}
	if (fw->json && __atomic_load_n(&uc->w_json, __ATOMIC_RELAXED) == 0) {
		/* known to have no RedisJSON: refused here, no round trip */
		ST_ADD(uc->wt_json_refused, 1);
		g->err = "upstream: the master has no RedisJSON - JSON writes are "
			"not forwarded";
		wt_err_add(uc, "refused", fw->argc > 0 ? fw->argv[0] : NULL,
			fw->argc > 0 ? fw->argl[0] : 0, NULL, 0, g->err,
			strlen(g->err));
		return PC_UPG_ANSWERED;
	}
	j = wt_job_new(fw);
	if (!j) {
		g->err = "upstream: out of memory parking the write";
		return PC_UPG_ANSWERED;
	}
	j->worker = pc_worker_id();
	j->id = __atomic_fetch_add(&next_id, 1, __ATOMIC_RELAXED);
	if (!j->id)
		j->id = __atomic_fetch_add(&next_id, 1, __ATOMIC_RELAXED);
	g->req = j->id;
	/* the completion is drained on this worker, busy until the door has
	 * parked the request: it cannot arrive first */
	pthread_mutex_lock(&uc->mx);
	if (uc->wq_n + __atomic_load_n(&uc->w_inflight, __ATOMIC_RELAXED) >=
	    uc->wq_max) {
		/* AR2: past upstream_write_queue - refused now, certainly not
		 * applied, instead of parked behind a master that is slow */
		static __thread char qmsg[160];

		pthread_mutex_unlock(&uc->mx);
		snprintf(qmsg, sizeof qmsg, "upstream: the write-through queue is "
			"full (%d waiting for the master) - not applied", uc->wq_max);
		ST_ADD(uc->wt_qfull, 1);
		wt_err_add(uc, "refused", fw->argc > 0 ? fw->argv[0] : NULL,
			fw->argc > 0 ? fw->argl[0] : 0, NULL, 0, qmsg, strlen(qmsg));
		wt_job_free(j);
		g->req = 0;
		g->err = qmsg;
		return PC_UPG_ANSWERED;
	}
	wt_queue(uc, j);
	pthread_mutex_unlock(&uc->mx);
	ST_ADD(uc->held, 1);
	return PC_UPG_HELD;
}

/* 12nt TC-2: is @fw a write the forward plane carries as one of its ops
 * (handle_fwd_op's 0-5: SET [NX|XX] [EX], DEL, INCRBY/DECRBY, EXPIRE,
 * PERSIST)?  A hash write, a JSON write or an MSET is not: those run
 * through where the client is, as before. */
static int fw_plane_op(const struct pc_upfw *fw)
{
	static const char *const ops[] = { "SET", "DEL", "INCRBY", "DECRBY",
		"EXPIRE", "PERSIST" };
	size_t i;

	if (fw->argc < 2 || fw->json)
		return 0;
	for (i = 0; i < sizeof ops / sizeof *ops; i++)
		if (fw->argl[0] == strlen(ops[i]) &&
		    !strncasecmp(fw->argv[0], ops[i], fw->argl[0]))
			return 1;
	return 0;
}

int pc_upstream_gate_fw(pcache_htable_t *ht, const char *col, size_t collen,
		int cls, int del, const char *const *keys, const size_t *kl,
		int nkeys, int verdict, struct pc_upg *g, const struct pc_upfw *fw)
{
	struct up_col *uc;

	memset(g, 0, sizeof *g);
	if (!ncols)
		return PC_UPG_PROCEED;
	uc = col_of(col, collen);
	if (uc && uc->through && verdict != PC_UPV_WT && fw && !fw->refuse &&
	    !fw->unk && nkeys == 1 && fw_plane_op(fw) && pc_store_shard_enabled(ht) &&
	    pc_cluster_enabled() && pc_shard_owner(col, collen, keys[0], kl[0]) != 0)
		/* 12nt TC-2: in shard mode the OWNER runs the write-through -
		 * the op forwards as every write does (pc_fwd_begin) and the
		 * owner's handle_fwd_op hands it to pc_upstream_fwd_through */
		return PC_UPG_PROCEED;
	if (uc && uc->through && (verdict == PC_UPV_WT || (fw && fw->unk) ||
	    cls == UPC_MUTATE || cls == UPC_PLAIN || del))
		return wt_gate(uc, keys, kl, nkeys, verdict, g, fw);
	return gate_rt(ht, col, collen, cls, del, keys, kl, nkeys, verdict, g);
}

int pc_upstream_gate(pcache_htable_t *ht, const char *col, size_t collen,
		int cls, int del, const char *const *keys, const size_t *kl,
		int nkeys, int verdict, struct pc_upg *g)
{
	return pc_upstream_gate_fw(ht, col, collen, cls, del, keys, kl, nkeys,
		verdict, g, NULL);
}

int pc_upstream_binds(const char *col, size_t collen)
{
	int i;

	for (i = 0; i < ncols; i++) {
		if (up_cols[i].nlen == collen &&
		    !memcmp(up_cols[i].name, col, collen))
			return 1;
		if (up_cols[i].nflen == collen &&
		    !memcmp(up_cols[i].nf, col, collen))
			return 2;
	}
	return 0;
}

unsigned int pc_upstream_copy_exp(const char *col, size_t collen,
		unsigned int ttl)
{
	unsigned int e;

	if (!ttl)
		return 0;
	if (!ncols || pc_upstream_binds(col, collen) != 1)
		return get_ticks() + ttl;
	e = get_ticks() + ttl - 1;
	return e ? e : 1;                  /* 0 is "never" */
}

int pc_upstream_through(const char *col, size_t collen)
{
	struct up_col *uc = ncols ? col_of(col, collen) : NULL;

	return uc && uc->through;
}

/* S418: a record of a through collection that arrived from a peer is
 * Redis's by construction - its author stored it only after Redis
 * confirmed the write - so the copy is marked "t" HERE, on arrival:
 * passively (never re-propagated, off the WAL, like the copy itself) and
 * with the record's own version.  The author's mark travels as a second
 * record, in a push of its own, and an invalidation landing between the
 * two left a copy without its mark that the owned count and the upfill
 * then took for the fleet's own (staging, 2026-10-15: one key written
 * through on .247, its copy on .245 unmarked for good). */
void pc_upstream_note_copy(const char *col, size_t collen, const char *k,
		size_t kl, unsigned int exp, unsigned long long ver)
{
	struct up_col *uc = ncols ? col_of(col, collen) : NULL;
	pcache_htable_t *nf;
	str key, v;
	int rc;

	if (!uc || !uc->through || kl > INT_MAX || !allowed(uc, k, kl))
		return;
	nf = pc_store_find(uc->nf, uc->nflen);
	if (!nf)
		return;
	key.s = (char *)k;
	key.len = (int)kl;
	v.s = "t";
	v.len = 1;
	sh_remove(uc, &key);
	rc = pcache_ht_store_ver(nf, &key, &v, exp, PCACHE_F_PASSIVE, ver);
	if (rc == 0)
		ST_ADD(uc->copies_marked, 1);
	else if (rc == PCACHE_E_OLDER)
		/* the author's mark - written after the record, so newer -
		 * came in first: the copy is marked already, by that */
		ST_ADD(uc->copies_premarked, 1);
}

int pc_upstream_allowed(const char *col, size_t collen, const char *k,
		size_t kl)
{
	struct up_col *uc = ncols ? col_of(col, collen) : NULL;

	return uc && allowed(uc, k, kl);
}

int pc_upstream_resp_pass(const char *col, size_t collen, char *const *argv,
		const size_t *argl, int nargs, struct pc_jw *out, void *conn)
{
	struct up_col *uc = ncols ? col_of(col, collen) : NULL;
	struct pc_upfw fw;
	struct pc_upg g;
	int rc;

	if (!uc || !uc->through || !uc->unknown_pass || nargs < 1 || !conn)
		return PC_UPG_PROCEED;
	memset(&fw, 0, sizeof fw);
	fw.argc = nargs;
	fw.argv = (const char *const *)argv;
	fw.argl = argl;
	fw.unk = 1;
	memset(&g, 0, sizeof g);
	rc = wt_gate(uc, NULL, NULL, 0, pc_conn_upstream_verdict(conn), &g, &fw);
	if (rc == PC_UPG_HELD)
		pc_conn_upstream_hold(conn, g.req);
	else if (rc == PC_UPG_ANSWERED)
		resp_answer(out, &g);
	return rc;
}

/* on the worker that queued the write, before its command answers: the
 * fleet holds what Redis holds - a value as its record (owned, with Redis's
 * expiry), gone as gone (a short no-fall entry covers the replicas that
 * have not applied the delete yet), a type it cannot hold left to Redis */
static void wt_apply(struct up_col *uc, const struct up_done *d)
{
	pcache_htable_t *ht = pc_store_find(uc->name, uc->nlen);
	pcache_htable_t *nf = pc_store_find(uc->nf, uc->nflen);
	int i;

	if (!ht)
		return;
	for (i = 0; i < d->nrb; i++) {
		const struct up_rb *r = &d->rb[i];
		str k;

		uint64_t oh = 0;
		pthread_mutex_t *om = NULL;

		if (!r->key)
			continue;
		k.s = r->key;
		k.len = (int)r->klen;
		/* S429: a read-back older than the newest applied for this key
		 * (another worker applied a later write first) is not applied */
		if (uc->order && d->wseq) {
			struct up_order *o;

			oh = echo_hash(r->key, r->klen);
			om = &uc->order_mx[oh % UP_ORDER_LOCKS];
			o = &uc->order[oh % UP_ORDER_N];
			pthread_mutex_lock(om);
			if (o->h == oh && o->seq > d->wseq) {
				pthread_mutex_unlock(om);
				ST_ADD(uc->wt_stale, 1);
				continue;
			}
			o->h = oh;
			o->seq = d->wseq;
		}
		if (r->what == UPRB_VALUE) {
			/* S435: the record lands BEFORE the copies go - a read
			 * on another worker finds the one or the other.  The
			 * copies first left a moment with neither, and a read
			 * the gate had let through on a copy answered nil */
			if (pc_verb_store_local(ht, &k, r->val, r->vlen, r->exp,
			        r->fl) == 0) {
				nofall_put_m(uc, &k, r->exp, "t");     /* AR1 */
				echo_note(uc, k.s, (size_t)k.len);   /* S422 */
			}
			sh_remove(uc, &k);
		} else if (r->what == UPRB_GONE) {
			long long cover = uc->max_lag_ms > 2000 ? uc->max_lag_ms : 2000;

			rm_begin(uc);                      /* S435 */
			sh_remove(uc, &k);
			pc_verb_remove_local(ht, &k);
			nofall_put_m(uc, &k, ttl_ticks(cover), "g");   /* AR1 */
			rm_end(uc);
		} else {
			rm_begin(uc);                      /* S435 */
			sh_remove(uc, &k);
			pc_verb_remove_local(ht, &k);
			if (nf)
				pc_verb_remove_local(nf, &k);
			rm_end(uc);
		}
		if (om)
			pthread_mutex_unlock(om);
	}
}

static void *wr_main(void *arg)
{
	struct up_col *uc = arg;
	struct uconn c;
	struct uk_cache *uk = calloc(1, sizeof *uk);
	long long next_try = 0, backoff = 1000, next_role = 0, next_json = 0;
	long long next_own = 0;            /* AR4 */

	memset(&c, 0, sizeof c);
	c.uc = uc;
	c.s = &uc->srv[0];
	c.fd = -1;
	c.wr = 1;
	c.wcred = 1;                       /* AR7 */
	wt_is_writer = 1;                  /* its writes are counted by command */
	rv_max = (size_t)uc->pass_max;
#ifdef __linux__
	{
		char tn[16];

		snprintf(tn, sizeof tn, "pc-wr-%.9s", uc->name);
		prctl(PR_SET_NAME, (unsigned long)tn, 0, 0, 0);
	}
#endif
	for (;;) {
		struct up_wjob *batch[WT_BATCH];
		int nb = 0, i;
		long long now;

		pthread_mutex_lock(&uc->mx);
		if (!uc->wq_head && !uc->stop) {
			struct timespec ts;

			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_sec += 1;
			pthread_cond_timedwait(&uc->wcv, &uc->mx, &ts);
		}
		if (uc->stop) {
			pthread_mutex_unlock(&uc->mx);
			break;
		}
		{
			/* S403: this thread's CPU, once a loop - once a second idle,
			 * once a batch busy; its share of five minutes is in stats */
			struct timespec tc;

			if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &tc) == 0)
				__atomic_store_n(&uc->wt_cpu_us,
					(unsigned long long)tc.tv_sec * 1000000ULL +
					(unsigned long long)tc.tv_nsec / 1000ULL,
					__ATOMIC_RELAXED);
		}
		while (uc->wq_head && nb < WT_BATCH) {
			batch[nb++] = uc->wq_head;
			uc->wq_head = uc->wq_head->next;
			uc->wq_n--;
		}
		/* AR2: still waiting for the master, so still under the bound */
		__atomic_store_n(&uc->w_inflight, nb, __ATOMIC_RELAXED);
		if (!uc->wq_head)
			uc->wq_tail = NULL;
		pthread_mutex_unlock(&uc->mx);

		now = now_ms();
		/* the target: at the start, after a failure (backoff), and the
		 * master's ROLE every 10 s while connected */
		if (c.fd >= 0 && now >= next_role) {
			long long dl = now + uc->timeout_ms;

			next_role = now + WT_ROLE_MS;
			if (role_check(&c, dl) != 0 ||
			    __atomic_load_n(&c.s->role, __ATOMIC_RELAXED) != UPR_MASTER) {
				LM_WARN("collection '%s': %s is no longer the master "
					"(ROLE) - looking again\n", uc->name, c.s->addr);
				uc_close(&c);
				__atomic_store_n(&uc->w_master, -1, __ATOMIC_RELAXED);
				__atomic_store_n(&uc->w_state, UPWS_DOWN, __ATOMIC_RELAXED);
				next_try = 0;
			}
		}
		if (c.fd < 0 && now >= next_try) {
			if (wt_discover(uc, &c) == 0) {
				backoff = 1000;
				next_role = now_ms() + WT_ROLE_MS;
				next_json = now_ms() + WT_JSON_MS;
			} else {
				next_try = now_ms() + backoff;
				backoff = backoff * 2 > WT_BACKOFF_MAX ? WT_BACKOFF_MAX :
					backoff * 2;
			}
		}
		if (c.fd >= 0 && now >= next_json &&
		    !__atomic_load_n(&uc->w_json, __ATOMIC_RELAXED)) {
			int js = wt_json_check(&c);

			next_json = now + WT_JSON_MS;
			if (js > 0) {
				__atomic_store_n(&uc->w_json, 1, __ATOMIC_RELAXED);
				LM_NOTICE("collection '%s': the master %s has RedisJSON "
					"now - JSON writes are forwarded\n", uc->name,
					c.s->addr);
			} else if (js < 0)
				uc_close(&c);
		}
		if (now >= next_own) {
			/* AR4: the keys the fleet still owns here, for /stats */
			next_own = now + WT_OWNED_MS;
			wt_count_owned(uc);
		}
		for (i = 0; i < nb; i++) {
			struct up_wjob *j = batch[i];
			int rc, n = 1, k;

			__atomic_store_n(&uc->w_inflight, nb - i, __ATOMIC_RELAXED);

			/* AR3: the plain jobs that follow it go with it, one round
			 * trip - not an unknown command (classed first), not an
			 * upfill that waits for its first answer, not a JSON write
			 * the master cannot take */
			while (c.fd >= 0 && i + n < nb && !j->unk && !WT_EARLY(j) &&
			       !batch[i + n]->unk && !WT_EARLY(batch[i + n]) &&
			       !(j->json && !__atomic_load_n(&uc->w_json,
			         __ATOMIC_RELAXED)) &&
			       !(batch[i + n]->json && !__atomic_load_n(&uc->w_json,
			         __ATOMIC_RELAXED)))
				n++;
			if (n > 1) {
				if (wt_pipe(uc, &c, batch + i, n) < 0) {
					next_try = 0;      /* looked for at once */
					__atomic_store_n(&uc->w_state, UPWS_DOWN,
						__ATOMIC_RELAXED);
					__atomic_store_n(&uc->w_master, -1,
						__ATOMIC_RELAXED);
				}
				for (k = 0; k < n; k++)
					wt_job_free(batch[i + k]);
				i += n - 1;
				continue;
			}

			if (c.fd < 0) {
				char msg[200];

				/* AR2: behind a write that timed out and took the
				 * connection with it, the rest of the batch had spent
				 * their deadline queueing - that is the queue's
				 * expiry, answered and counted as wt_run does, not an
				 * unreachable master.  Which writes shared the timed-out
				 * one's batch is timing: GitLab 110394's check-asan told
				 * nine of them "no master reachable" */
				if (wt_expired(uc, j, now_ms())) {
					wt_job_free(j);
					continue;
				}
				snprintf(msg, sizeof msg, "upstream: no master reachable "
					"for collection %.40s (%s) - write not applied", uc->name,
					upws_name[__atomic_load_n(&uc->w_state,
					__ATOMIC_RELAXED)]);
				wt_fail(uc, j, msg);
				wt_job_free(j);
				continue;
			}
			if (j->json && !__atomic_load_n(&uc->w_json, __ATOMIC_RELAXED)) {
				char msg[300];

				ST_ADD(uc->wt_json_refused, 1);
				snprintf(msg, sizeof msg, "upstream: the master %.200s has "
					"no RedisJSON - JSON writes are not forwarded",
					c.s->addr);
				wt_fail(uc, j, msg);
				wt_job_free(j);
				continue;
			}
			if (j->unk)
				rc = wt_unknown(uc, &c, uk, j);
			else
				rc = wt_run(uc, &c, j, j->kidx, j->nkeys);
			if (rc == 1)
				continue;          /* the pass reader has it */
			if (rc < 0 && c.fd >= 0 && j->unk)
				wt_fail(uc, j, "upstream: the connection to the master "
					"failed classing the command");
			if (rc < 0) {
				uc_close(&c);
				next_try = 0;      /* looked for at once - then backoff */
				__atomic_store_n(&uc->w_state, UPWS_DOWN, __ATOMIC_RELAXED);
				__atomic_store_n(&uc->w_master, -1, __ATOMIC_RELAXED);
			}
			wt_job_free(j);
		}
		__atomic_store_n(&uc->w_inflight, 0, __ATOMIC_RELAXED);
	}
	uc_close(&c);
	free(c.buf);
	free(uk);
	return NULL;
}

/* ---- the pass reader: unknown READS, on a replica ------------------------ */

static void pr_queue(struct up_col *uc, struct up_wjob *j)
{
	pthread_mutex_lock(&uc->mx);
	if (uc->pr_n >= uc->wq_max) {          /* AR2: the same bound */
		pthread_mutex_unlock(&uc->mx);
		ST_ADD(uc->wt_qfull, 1);
		wt_fail(uc, j, "upstream: the pass reader's queue is full - the "
			"command was not passed");
		wt_job_free(j);
		return;
	}
	uc->pr_n++;
	j->next = NULL;
	if (uc->pr_tail)
		uc->pr_tail->next = j;
	else
		uc->pr_head = j;
	uc->pr_tail = j;
	pthread_cond_signal(&uc->prcv);
	pthread_mutex_unlock(&uc->mx);
}

static void *pr_main(void *arg)
{
	struct up_col *uc = arg;
	struct uconn c;

	memset(&c, 0, sizeof c);
	c.uc = uc;
	c.fd = -1;
	c.wr = 1;
	rv_max = (size_t)uc->pass_max;
#ifdef __linux__
	{
		char tn[16];

		snprintf(tn, sizeof tn, "pc-pr-%.9s", uc->name);
		prctl(PR_SET_NAME, (unsigned long)tn, 0, 0, 0);
	}
#endif
	for (;;) {
		struct up_wjob *j;
		struct up_srv *s;
		struct rv v;
		size_t sp = 0, ep = 0;
		long long dl;
		int rc;

		pthread_mutex_lock(&uc->mx);
		if (!uc->pr_head && !uc->stop) {
			struct timespec ts;

			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_sec += 1;
			pthread_cond_timedwait(&uc->prcv, &uc->mx, &ts);
		}
		if (uc->stop) {
			pthread_mutex_unlock(&uc->mx);
			break;
		}
		j = uc->pr_head;
		if (j) {
			uc->pr_head = j->next;
			if (!uc->pr_head)
				uc->pr_tail = NULL;
			uc->pr_n--;
		}
		/* a replica that serves - the fetches' choice */
		s = j ? pick(uc, NULL) : NULL;
		pthread_mutex_unlock(&uc->mx);
		if (!j)
			continue;
		if (!s || srv_state(s) == PC_UPS_OFF) {
			wt_fail(uc, j, "upstream: no upstream serves reads - the "
				"command was not passed");
			wt_job_free(j);
			continue;
		}
		if (c.fd >= 0 && c.s != s)
			uc_close(&c);
		c.s = s;
		dl = j->t0 + uc->timeout_ms;      /* AR2: from the queueing */
		if (dl - now_ms() < wt_need_ms(uc)) {
			ST_ADD(uc->wt_qexpired, 1);
			wt_fail(uc, j, "upstream: the command waited too long for a "
				"replica (under a quarter of upstream_timeout_ms left) - "
				"not passed");
			wt_job_free(j);
			continue;
		}
		if (c.fd < 0 && uc_connect(&c, dl) != 0) {
			wt_fail(uc, j, "upstream: the replica did not answer - the "
				"command was not passed");
			wt_job_free(j);
			continue;
		}
		if (send_cmd(&c, dl, j->argc, (const char *const *)j->argv,
		        j->argl) != 0)
			rc = -1;
		else
			rc = read_reply_span(&c, &v, dl, &sp, &ep);
		if (rc == 0) {
			wt_finish(uc, j, UPW_R_DONE, c.buf + sp, ep - sp,
				reply_ival(&v), NULL, 0);
			rv_free(&v);
		} else {
			wt_fail(uc, j, rc == -3 ? "upstream: the reply is larger than "
				"upstream_pass_max_kb - not passed" : rc == -2 ?
				"upstream: the read timed out" : "upstream: the replica's "
				"connection failed");
			uc_close(&c);
		}
		wt_job_free(j);
	}
	uc_close(&c);
	free(c.buf);
	return NULL;
}

/* ---- the drain sweep ----------------------------------------------------- */

#define DR_BATCH    256                /* keys per SCAN and per pipeline */
#define DR_INFLIGHT 8192               /* posted, not yet applied */

static int pull_worker;                /* round-robin over the workers */

/* S401: when a sweep that has moved @done keys and @bytes since @t0 (mono
 * ms) may go on - the later of what upstream_drain_rate (keys) and
 * upstream_drain_mb_rate (bytes) allow; 0 for either is no cap */
static long long sweep_due(const struct up_col *uc, long long t0,
		long long done, long long bytes)
{
	long long a = uc->drain_rate > 0 ? t0 + done * 1000 / uc->drain_rate : 0;
	long long b = uc->drain_mb_rate > 0 ?
		t0 + bytes * 1000 / ((long long)uc->drain_mb_rate << 20) : 0;

	return a > b ? a : b;
}

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
	__atomic_add_fetch(&uc->dr_bytes, (long long)vl, __ATOMIC_RELAXED);  /* S401 */
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
		exps[i] = tt.t == ':' && tt.n >= 0 ? pttl_ticks(tt.n) : 0;   /* S449 */
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
		"the fleet has not taken it over (at most %d keys and %d MB a "
		"second)\n", uc->name, uc->npat, uc->drain_rate,
		uc->drain_mb_rate);
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
				if (ok) {
					/* 12nt TC-4 (0.7.1): the sweep takes over the
					 * keys that belong HERE - the owner's share in
					 * shard, a holder's in spread; the rest are
					 * their owners' sweeps' (every node runs its
					 * own), skipped before the GET */
					pcache_htable_t *ht = pc_store_find(uc->name,
						uc->nlen);
					int m = 0;

					for (i = 0; i < n; i++) {
						if (ht && through_elsewhere(ht, uc->name,
						        uc->nlen, ks[i], kl[i])) {
							ST_ADD(uc->dr_elsewhere, 1);
							free(ks[i]);
							continue;
						}
						ks[m] = ks[i];
						kl[m++] = kl[i];
					}
					for (i = m; i < n; i++)
						ks[i] = NULL;
					done += n - m;
					n = m;
				}
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
			/* the rate caps, keys and bytes (S401): the upstream may
			 * be production */
			{
				long long want = sweep_due(uc, t0, done,
					__atomic_load_n(&uc->dr_bytes, __ATOMIC_RELAXED));

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
		"cannot hold, %llu too big, %llu errors, %llu other nodes' to take\n",
		uc->name, updr_name[uc->drain_state], ST_GET(uc->dr_scanned),
		ST_GET(uc->dr_pulled), ST_GET(uc->dr_owned), ST_GET(uc->dr_absent),
		ST_GET(uc->dr_wrongtype), ST_GET(uc->dr_toobig),
		ST_GET(uc->dr_errors), ST_GET(uc->dr_elsewhere));
	uc_close(&c);
	free(c.buf);
	return NULL;
}

/* ---- AR4: the keys the fleet owned before through, to Redis -----------------
 * A key written under upstream_writes = allow is the fleet's own: Redis may
 * never have had it, and under through it stays fleet-only - a field
 * update inside it fails in Redis, which has no root to update.  Which keys
 * those are is read from the COLLECTION, not from the no-fall companion: a
 * local record inside upstream_prefixes is the fleet's own unless its
 * companion entry says Redis holds its state ("t" written through, "g"
 * found gone).  The companion's "1" is left only where the fleet took a key
 * over from Redis (an overwrite, a promote) - a key Redis never had gets
 * none, and the first walk of "1"s found nothing to upfill (0.6.4-rc2's
 * CI, upwritethroughtest 14); a walk of the collection also finds keys an
 * older build wrote under allow.  The writer counts them a minute apart
 * (/stats fleet_owned); the upfill sweep, started by hand on ONE node,
 * collects them and queues each to the master THROUGH THE WRITER, so it is ordered
 * with the clients' writes: SET ... NX [PX] for a string or a counter (its
 * value on this node), JSON.SET $ ... NX and, only where that applied, a
 * PEXPIRE for a document.  NX: a value Redis holds meanwhile wins, and
 * the read-back makes the fleet hold whichever Redis kept - the key is
 * then written through ("t") like any other.  A hash (Redis has no NX for
 * a whole one), a rate-limit or a lock record (perfcached's own) is
 * skipped and counted.  Paced by upstream_drain_rate, and never more than
 * half of upstream_write_queue at once: the clients' writes keep room. */

struct upf_keys {
	const struct up_col *uc;
	unsigned int now;
	str *k;
	int n, cap;
};

/* the walk of the COLLECTION: every live record inside upstream_prefixes.
 * The companion is not read here - this runs under the collection's bucket
 * lock; upf_filter reads it afterwards, with no lock held */
static int upf_collect(const str *key, const str *val,
		const struct pcache_rec_meta *mt, void *ctx)
{
	struct upf_keys *uk = ctx;

	(void)val;
	if ((mt && mt->expires && mt->expires <= uk->now) ||
	    !allowed(uk->uc, key->s, (size_t)key->len))
		return 0;
	if (uk->n == uk->cap) {
		int nc = uk->cap ? uk->cap * 2 : 256;
		str *nk = realloc(uk->k, (size_t)nc * sizeof *nk);

		if (!nk)
			return 0;
		uk->k = nk;
		uk->cap = nc;
	}
	uk->k[uk->n].s = malloc((size_t)key->len ? (size_t)key->len : 1);
	if (!uk->k[uk->n].s)
		return 0;
	memcpy(uk->k[uk->n].s, key->s, (size_t)key->len);
	uk->k[uk->n].len = key->len;
	uk->n++;
	return 0;
}

/* keep the keys whose state Redis does not hold: no companion entry, or
 * "1" (taken over); drop "t" (written through) and "g" (found gone) */
static void upf_filter(struct up_col *uc, struct upf_keys *uk)
{
	pcache_htable_t *nf = pc_store_find(uc->nf, uc->nflen);
	int i, n = 0;

	for (i = 0; i < uk->n; i++) {
		str v = { NULL, 0 };
		unsigned int exp = 0;
		unsigned char fl = 0;
		int m = 0;

		if (nf && pcache_ht_fetch_ex(nf, &uk->k[i], &v, &exp, &fl) == 0) {
			m = v.len > 0 ? (unsigned char)v.s[0] : 0;
			free(v.s);
		}
		if (m == 't' || m == 'g') {
			free(uk->k[i].s);
			continue;
		}
		uk->k[n++] = uk->k[i];
	}
	uk->n = n;
}

static void upf_gather(struct up_col *uc, struct upf_keys *uk)
{
	pcache_htable_t *ht = pc_store_find(uc->name, uc->nlen);

	memset(uk, 0, sizeof *uk);
	uk->uc = uc;
	uk->now = get_ticks();
	if (!ht)
		return;
	pcache_ht_iter_meta(ht, upf_collect, uk);
	upf_filter(uc, uk);
}

static void upf_release(struct upf_keys *uk)
{
	int i;

	for (i = 0; i < uk->n; i++)
		free(uk->k[i].s);
	free(uk->k);
	uk->k = NULL;
	uk->n = uk->cap = 0;
}

/* the writer, a minute apart: how many keys the fleet still owns here */
static void wt_count_owned(struct up_col *uc)
{
	struct upf_keys uk;

	upf_gather(uc, &uk);
	__atomic_store_n(&uc->wt_owned, (long long)uk.n, __ATOMIC_RELAXED);
	upf_release(&uk);
}

/* one owned key to the writer's queue: 0 queued, 1 skipped (its type),
 * -2 gone meanwhile, -1 out of memory */
static int upf_one(struct up_col *uc, pcache_htable_t *ht, const str *k,
		size_t *vlen)
{
	static const int k1[1] = { 1 };
	str v = { NULL, 0 };
	unsigned int exp = 0, now = get_ticks();
	unsigned char fl = 0;
	const char *av[6], *a2[3];
	size_t al[6], l2[3];
	char ms[24];
	struct pc_upfw fw;
	struct up_wjob *j;
	int ac = 0, ml = 0;

	if (pcache_ht_fetch_ex(ht, k, &v, &exp, &fl) != 0)
		return -2;
	if (exp && exp <= now) {
		free(v.s);
		return -2;
	}
	if (fl & (PCACHE_F_HASH | PCACHE_F_RL | PCACHE_F_LOCK)) {
		free(v.s);
		return 1;
	}
	if (exp)
		ml = snprintf(ms, sizeof ms, "%lld", (long long)(exp - now) * 1000);
	memset(&fw, 0, sizeof fw);
	if (fl & PCACHE_F_JSON) {
		av[ac] = "JSON.SET"; al[ac++] = 8;
		av[ac] = k->s; al[ac++] = (size_t)k->len;
		av[ac] = "$"; al[ac++] = 1;
		av[ac] = v.s; al[ac++] = (size_t)v.len;
		av[ac] = "NX"; al[ac++] = 2;
		if (ml > 0) {                  /* sent only if the NX applied */
			a2[0] = "PEXPIRE"; l2[0] = 7;
			a2[1] = k->s; l2[1] = (size_t)k->len;
			a2[2] = ms; l2[2] = (size_t)ml;
			fw.argc2 = 3;
			fw.argv2 = a2;
			fw.argl2 = l2;
		}
		fw.json = 1;
		fw.guess = PC_UPFW_JSON;
	} else {
		av[ac] = "SET"; al[ac++] = 3;
		av[ac] = k->s; al[ac++] = (size_t)k->len;
		av[ac] = v.s; al[ac++] = (size_t)v.len;
		av[ac] = "NX"; al[ac++] = 2;
		if (ml > 0) {
			av[ac] = "PX"; al[ac++] = 2;
			av[ac] = ms; al[ac++] = (size_t)ml;
		}
		fw.guess = PC_UPFW_STRING;
	}
	fw.argc = ac;
	fw.argv = av;
	fw.argl = al;
	fw.nk = 1;
	fw.kidx = k1;
	j = wt_job_new(&fw);
	*vlen = (size_t)v.len;             /* S401: what the byte rate counts */
	free(v.s);
	if (!j)
		return -1;
	j->upfill = 1;
	j->upf_vi = (fl & PCACHE_F_JSON) ? 3 : 2;  /* S400: SET k v / JSON.SET k $ v */
	j->upf_exp = exp;
	/* its read-back is applied on a worker - a registered one (there is
	 * no worker 0: 0.6.3's first CI) */
	j->worker = inv_worker();
	if (j->worker < 0) {
		wt_job_free(j);
		return -1;
	}
	j->id = __atomic_fetch_add(&next_id, 1, __ATOMIC_RELAXED);
	if (!j->id)
		j->id = __atomic_fetch_add(&next_id, 1, __ATOMIC_RELAXED);
	pthread_mutex_lock(&uc->mx);
	wt_queue(uc, j);
	pthread_mutex_unlock(&uc->mx);
	return 0;
}

/* S400: `check` - one owned key read from the master on the sweep's own
 * connection (RESP2 and never tracked, as the pass reader's; the read
 * credentials) and compared; nothing written, nothing applied.  0 compared,
 * 1 skipped (its type), -2 gone here meanwhile, -1 the read failed */
static int upf_check_one(struct up_col *uc, pcache_htable_t *ht,
		struct uconn *c, const str *k, size_t *vlen)
{
	str v = { NULL, 0 };
	unsigned int exp = 0, now = get_ticks();
	unsigned char fl = 0;
	struct up_rb rb;
	long long dl;
	int json, g;

	if (pcache_ht_fetch_ex(ht, k, &v, &exp, &fl) != 0)
		return -2;
	if (exp && exp <= now) {
		free(v.s);
		return -2;
	}
	if (fl & (PCACHE_F_HASH | PCACHE_F_RL | PCACHE_F_LOCK)) {
		free(v.s);
		return 1;
	}
	json = !!(fl & PCACHE_F_JSON);
	g = json ? UPWG_JSON : UPWG_STRING;
	*vlen = (size_t)v.len;             /* S401 */
	dl = now_ms() + uc->timeout_ms;
	memset(&rb, 0, sizeof rb);
	if ((c->fd < 0 && uc_connect(c, dl) != 0) ||
	    rb_send(c, dl, k->s, (size_t)k->len, g) != 0 ||
	    rb_read(c, dl, k->s, (size_t)k->len, g, &rb) != 0) {
		uc_close(c);
		free(rb.key);
		free(rb.val);
		free(v.s);
		return -1;
	}
	if (rb.what != UPRB_GONE)
		ST_ADD(uc->upf_kept, 1);
	upf_judge(uc, k->s, (size_t)k->len, v.s, (size_t)v.len, json, exp, &rb);
	free(rb.key);
	free(rb.val);
	free(v.s);
	return 0;
}

static void *upf_main(void *arg)
{
	struct up_col *uc = arg;
	pcache_htable_t *ht = pc_store_find(uc->name, uc->nlen);
	pcache_htable_t *nf = pc_store_find(uc->nf, uc->nflen);
	struct upf_keys uk;
	struct uconn c;
	long long t0 = now_ms(), done = 0;
	int i, m;

	/* S400: a check reads the writer's master itself */
	memset(&c, 0, sizeof c);
	c.uc = uc;
	c.fd = -1;
	c.notrack = 1;
	c.wr = 1;
	m = __atomic_load_n(&uc->w_master, __ATOMIC_RELAXED);
	c.s = m >= 0 ? &uc->srv[m] : NULL;
#ifdef __linux__
	{
		char tn[16];

		snprintf(tn, sizeof tn, "pc-upf-%.8s", uc->name);
		prctl(PR_SET_NAME, (unsigned long)tn, 0, 0, 0);
	}
#endif
	if (ht && nf)
		upf_gather(uc, &uk);
	else
		memset(&uk, 0, sizeof uk);
	uc->upf_found = (unsigned long long)uk.n;
	if (uc->upf_check)
		LM_NOTICE("collection '%s': upfill check starts - %d key(s) the "
			"fleet owned before write-through, each read from the "
			"master and compared, nothing written (at most %d keys and "
			"%d MB a second)\n", uc->name, uk.n, uc->drain_rate,
			uc->drain_mb_rate);
	else
		LM_NOTICE("collection '%s': the upfill sweep starts - %d key(s) "
			"the fleet owned before write-through, each written to the "
			"master unless Redis has it (at most %d keys and %d MB a "
			"second)\n", uc->name, uk.n, uc->drain_rate,
			uc->drain_mb_rate);
	for (i = 0; i < uk.n && !uc->upf_stop && !uc->stop; i++) {
		int r;
		size_t vl = 0;                 /* S401 */

		if (uc->upf_check) {           /* S400: counted as compared */
			r = c.s ? upf_check_one(uc, ht, &c, &uk.k[i], &vl) : -1;
			if (r == 1)
				ST_ADD(uc->upf_skipped, 1);
			else if (r == -2)
				ST_ADD(uc->upf_gone, 1);
			else if (r == -1) {
				ST_ADD(uc->upf_errors, 1);
				nap_ms(250);       /* not once a key at a dead master */
			}
		} else {
			/* half the write queue at most: the clients' writes keep
			 * room */
			while (!uc->upf_stop && !uc->stop &&
			       uc->wq_n + __atomic_load_n(&uc->w_inflight,
			       __ATOMIC_RELAXED) >= (uc->wq_max + 1) / 2)
				upf_nap(uc, 5);    /* S402: comparisons go on */
			if (uc->upf_stop || uc->stop)
				break;
			r = upf_one(uc, ht, &uk.k[i], &vl);
			ST_ADD(*(r == 0 ? &uc->upf_queued : r == 1 ?
				&uc->upf_skipped : r == -2 ? &uc->upf_gone :
				&uc->upf_errors), 1);
			upf_drain_q(uc, 1);        /* S402 */
		}
		done++;
		if (r == 0)
			__atomic_add_fetch(&uc->upf_bytes, (long long)vl,
				__ATOMIC_RELAXED);         /* S401 */
		{
			/* the caps, keys and bytes (S401) */
			long long want = sweep_due(uc, t0, done,
				__atomic_load_n(&uc->upf_bytes, __ATOMIC_RELAXED));

			while (now_ms() < want && !uc->upf_stop && !uc->stop)
				upf_nap(uc, (int)(want - now_ms() < 50 ?
					want - now_ms() : 50));   /* S402 */
		}
	}
	upf_release(&uk);
	uc_close(&c);
	free(c.buf);
	/* the writer finishes what was queued before the state is final, and
	 * every comparison it handed over is made (S402) */
	while (!uc->stop && ST_GET(uc->upf_answered) < ST_GET(uc->upf_queued))
		upf_nap(uc, 10);
	upf_drain_q(uc, !uc->stop);
	json_same_release();               /* S400: this thread's walk buffer -
	                                    * after its last comparison */
	/* the count before the state: "done" means fleet_owned says what is
	 * left.  It was taken after, and a reader that saw done read the count
	 * from before the sweep (GitLab 111557 core1, upwritethroughtest 14:
	 * fleet_owned 4, the three written still counted) */
	wt_count_owned(uc);
	uc->upf_state = uc->upf_stop ? UPDR_STOPPED : UPDR_DONE;
	uc->upf_t1 = (long long)time(NULL);
	if (uc->upf_check)
		LM_NOTICE("collection '%s': upfill check %s - %llu found: %llu "
			"Redis lacks (a run writes them), %llu Redis holds the same, "
			"%llu the same with another expiry, %llu differently (a run "
			"replaces the fleet's with Redis's), %llu not compared; "
			"%llu skipped (a hash, a rate-limit or a lock record), %llu "
			"gone, %llu errors%s\n",
			uc->name, updr_name[uc->upf_state], ST_GET(uc->upf_found),
			ST_GET(uc->upf_missing), ST_GET(uc->upf_same),
			ST_GET(uc->upf_retimed), ST_GET(uc->upf_differed),
			ST_GET(uc->upf_uncompared),
			ST_GET(uc->upf_skipped), ST_GET(uc->upf_gone),
			ST_GET(uc->upf_errors), uc->upf_named > UPF_NAME_MAX ?
			" - more keys differed than the log names" : "");
	else
		LM_NOTICE("collection '%s': the upfill sweep %s - %llu found, %llu "
			"written to the master, %llu Redis had (%llu the same, %llu "
			"the same with another expiry, %llu differently - Redis's "
			"value replaced the fleet's on every member - and %llu not "
			"compared), %llu skipped (a hash, a rate-limit or a lock "
			"record), %llu gone, %llu errors%s\n", uc->name,
			updr_name[uc->upf_state],
			ST_GET(uc->upf_found), ST_GET(uc->upf_written),
			ST_GET(uc->upf_kept), ST_GET(uc->upf_same),
			ST_GET(uc->upf_retimed), ST_GET(uc->upf_differed),
			ST_GET(uc->upf_uncompared),
			ST_GET(uc->upf_skipped), ST_GET(uc->upf_gone),
			ST_GET(uc->upf_errors), uc->upf_named > UPF_NAME_MAX ?
			" - more keys differed than the log names" : "");
	return NULL;
}

int pc_upstream_upfill_ctl(const char *col, size_t collen, int stop,
		int check, const char **msg)
{
	struct up_col *uc = col_of(col, collen);

	if (!uc) {
		*msg = "this collection has no upstream";
		return -1;
	}
	if (stop) {
		if (uc->upf_state != UPDR_RUNNING) {
			*msg = "no upfill sweep is running";
			return -1;
		}
		uc->upf_stop = 1;
		*msg = "stopping";
		return 0;
	}
	if (!uc->through) {
		*msg = "the upfill sweep writes to the upstream's master - the "
			"collection needs upstream_writes = through";
		return -1;
	}
	if (uc->upf_state == UPDR_RUNNING) {
		*msg = "an upfill sweep is already running";
		return -1;
	}
	if (check && __atomic_load_n(&uc->w_master, __ATOMIC_RELAXED) < 0) {
		*msg = "upfill check reads the upstream's master - the writer has "
			"none now (see upstream.write_through.state)";
		return -1;
	}
	if (uc->upf_joinable) {
		pthread_join(uc->upf_thr, NULL);
		uc->upf_joinable = 0;
	}
	uc->upf_stop = 0;
	uc->upf_check = check;             /* S400 */
	uc->upf_same = uc->upf_retimed = uc->upf_differed = uc->upf_missing = 0;
	uc->upf_cmp_text = uc->upf_cmp_walked = uc->upf_uncompared = 0;  /* S402 */
	__atomic_store_n(&uc->upf_bytes, 0, __ATOMIC_RELAXED);          /* S401 */
	uc->upf_named = 0;
	uc->upf_state = UPDR_RUNNING;
	uc->upf_t0 = (long long)time(NULL);
	uc->upf_t1 = 0;
	uc->upf_found = uc->upf_queued = uc->upf_written = uc->upf_kept =
		uc->upf_skipped = uc->upf_gone = uc->upf_errors =
		uc->upf_answered = 0;
	if (pthread_create(&uc->upf_thr, NULL, upf_main, uc) != 0) {
		uc->upf_state = UPDR_FAILED;
		*msg = "cannot start the upfill thread";
		return -1;
	}
	uc->upf_joinable = 1;
	*msg = check ? "checking" : "started";
	return 0;
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
	uc->dr_elsewhere = 0;                  /* 12nt TC-4 */
	__atomic_store_n(&uc->dr_bytes, 0, __ATOMIC_RELAXED);  /* S401 */
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

/* redis://[user@]host:port[/db] - IP6-2: the host an IPv6 literal in
 * brackets too, "redis://[::1]:6379" */
static int parse_url(struct up_srv *s, const char *url)
{
	const char *p = url + 8, *at = strchr(p, '@'), *slash;

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
	{
		/* the one host:port parser (pcaddr.c): "[v6]:port" unwrapped,
		 * the port a plain decimal number in 1..65535 */
		char hp[sizeof s->host + 16];
		size_t n = slash ? (size_t)(slash - p) : strlen(p);

		if (!n || n >= sizeof hp)
			return -1;
		memcpy(hp, p, n);
		hp[n] = 0;
		if (pc_hostport_split(hp, s->host, sizeof s->host, &s->port) != 0 ||
		    !s->host[0])
			return -1;
	}
	s->db = slash ? atoi(slash + 1) : 0;
	if (s->db < 0 || s->db > 1023)
		return -1;
	{
		/* from a copy: addr and host are both in up_cols[], which gcc 14
		 * reads as a possible overlap (-Wrestrict) */
		char hb[sizeof s->host];

		memcpy(hb, s->host, sizeof hb);
		snprintf(s->addr, sizeof s->addr, strchr(hb, ':') ? "[%.253s]:%d" :
			"%.255s:%d", hb, s->port);
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
		/* the collection must decide its keys HERE - or, since 0.7.1
		 * (DESIGN 12nt), at the key's OWNER: in shard mode a miss pulls
		 * from the owner and the owner fetches (TC-1).  A proxy or spread
		 * collection still places a key where this node's shadow means
		 * nothing (TC-3 for spread). */
		if (pc_store_proxy_enabled(ht)) {
			/* 12nt TC-3 (0.7.1): spread is allowed - the K holders
			 * fetch and hold, as the owner does in shard */
			LM_ERR("upstream: [collection %s] read-through needs a "
				"store, eager, shard or spread collection (not "
				"proxy)\n", cc->name);
			return -1;
		}
		uc = &up_cols[ncols];
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
			 * up_cols[], which gcc 14 reads as a possible overlap */
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
		uc->through = cc->up_write_through;           /* S385 */
		uc->unknown_pass = cc->up_write_through &&
			cc->up_unknown == PC_UP_UNKNOWN_PASS;
		uc->wq_max = cc->up_write_queue > 0 ? cc->up_write_queue : 10000;   /* AR2 */
		uc->pass_max = (cc->up_pass_max_kb > 0 ? cc->up_pass_max_kb : 1024)
			* 1024;
		uc->w_master = -1;
		uc->w_json = -1;
		uc->w_state = UPWS_STARTING;
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
		uc->wuser = cc->up_wuser;                     /* AR7 */
		uc->wpassword = cc->up_wpassword;
		uc->neg_ms = cc->up_negative_ms;
		uc->rewarm_max = cc->up_rewarm_max;              /* S421 */
		/* S422: at least twice the lag a tracked replica may echo with */
		uc->echo_ms = cc->up_echo_ms;
		if (uc->echo_ms > 0 && uc->max_lag_ms * 2 > uc->echo_ms)
			uc->echo_ms = uc->max_lag_ms * 2;
		if (uc->echo_ms > 0 && cc->up_write_through)
			uc->echo = calloc(UP_ECHO_N, sizeof *uc->echo);
		if (cc->up_write_through) {                      /* S429 */
			int l;

			uc->order = calloc(UP_ORDER_N, sizeof *uc->order);
			for (l = 0; l < UP_ORDER_LOCKS; l++)
				pthread_mutex_init(&uc->order_mx[l], NULL);
		}
		uc->timeout_ms = cc->up_timeout_ms;
		uc->conns = cc->up_conns;
		uc->tracking = cc->up_tracking;
		uc->on_error = cc->up_on_error;
		uc->drain_rate = cc->up_drain_rate;
		uc->drain_mb_rate = cc->up_drain_mb_rate;      /* S401 */
		uc->wt_owned = -1;                            /* AR4 */
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
		pthread_mutex_init(&uc->upf_qmx, NULL);        /* S402 */
		pthread_cond_init(&uc->wcv, NULL);            /* S385 */
		pthread_cond_init(&uc->prcv, NULL);
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
		/* S385: the writer starts now, not on the first write - the
		 * master and RedisJSON are known before any traffic */
		if (uc->through) {
			if (pthread_create(&uc->wthr, NULL, wr_main, uc) != 0) {
				LM_ERR("upstream: cannot start the writer for '%s'\n",
					cc->name);
				return -1;
			}
			uc->has_wthr = 1;
			if (uc->unknown_pass) {
				if (pthread_create(&uc->rthr, NULL, pr_main, uc) != 0) {
					LM_ERR("upstream: cannot start the pass reader for "
						"'%s'\n", cc->name);
					return -1;
				}
				uc->has_rthr = 1;
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
				"timeout %d ms, on_error %s - %s\n", cc->name, list,
				uc->srv[0].db, uc->role_req == PC_UP_ROLE_REPLICA ?
				(uc->master_fallback ? ", replicas (the master while "
				"none serves)" : ", replicas only") : "", uc->npat,
				uc->conns, uc->nsrv > 1 ? " each" : "", uc->neg_ms,
				uc->timeout_ms,
				uc->on_error == PC_UP_ON_ERROR_MISS ? "miss" : "error",
				uc->through ? (uc->unknown_pass ? "WRITE-THROUGH to its "
				"master, unknown commands passed (pub/sub is not "
				"bridged)" : "WRITE-THROUGH to its master (pub/sub is not "
				"bridged)") : "this node READS the upstream and never "
				"writes it");
		}
	}
	/* S421: the names a clean stop saved re-warm now (the pacer runs on
	 * the invalidation threads once they connect) */
	snprintf(up_state_dir, sizeof up_state_dir, "%s",
		cfg->state_dir ? cfg->state_dir : "");
	for (i = 0; i < ncols; i++)
		rewarm_load(&up_cols[i], up_state_dir);
	return 0;
}

void pc_upstream_shutdown(void)
{
	int i, j, k;

	for (i = 0; i < ncols; i++) {
		struct up_col *uc = &up_cols[i];

		/* S421: the names held, taken while the shadows are still whole -
		 * a closing tracked connection drops its upstream's copies */
		rewarm_save(uc, up_state_dir);
		uc->drain_stop = 1;
		uc->upf_stop = 1;                             /* AR4 */
		if (uc->drain_joinable) {
			pthread_join(uc->drain_thr, NULL);
			uc->drain_joinable = 0;
		}
		pthread_mutex_lock(&uc->mx);
		uc->stop = 1;
		for (k = 0; k < uc->nsrv; k++)
			pthread_cond_broadcast(&uc->srv[k].cv);
		pthread_cond_broadcast(&uc->wcv);             /* S385 */
		pthread_cond_broadcast(&uc->prcv);
		pthread_mutex_unlock(&uc->mx);
		if (uc->upf_joinable) {                       /* AR4 */
			pthread_join(uc->upf_thr, NULL);
			uc->upf_joinable = 0;
		}
		if (uc->has_wthr) {
			pthread_join(uc->wthr, NULL);
			uc->has_wthr = 0;
		}
		if (uc->has_rthr) {
			pthread_join(uc->rthr, NULL);
			uc->has_rthr = 0;
		}
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

/* ---- S421: the names held across a clean restart ---------------------------
 * The shadow (the copies fetched) is not persisted and the records written
 * through are dropped at the first gap, so a restarted node was cold.  A
 * clean stop writes the NAMES of what it held - every upstream's shadow
 * (its absent marks excluded) and whatever the re-warm list still had - to
 * <state_dir>/upstream-rewarm-<collection>, newest fetched first, at most
 * upstream_rewarm_max; the start reads the file onto the re-warm list and
 * removes it (one shot: a crash loop does not re-warm from stale names). */
struct rw_names {
	str *k;
	unsigned int *wt;
	int n, cap;
};

static int rw_collect(const str *key, const str *val,
		const struct pcache_rec_meta *mt, void *ctx)
{
	struct rw_names *r = ctx;

	(void)val;
	if (mt && (mt->rflags & PCACHE_F_UPMARK))
		return 0;                      /* a marker, not a copy */
	if (r->n == r->cap) {
		int nc = r->cap ? r->cap * 2 : 1024;
		str *nk = realloc(r->k, (size_t)nc * sizeof *nk);
		unsigned int *nw = nk ? realloc(r->wt, (size_t)nc * sizeof *nw) : NULL;

		if (nk)
			r->k = nk;
		if (nw)
			r->wt = nw;
		if (!nk || !nw)
			return 1;                  /* out of memory: what we have */
		r->cap = nc;
	}
	r->k[r->n].s = malloc((size_t)key->len ? (size_t)key->len : 1);
	if (!r->k[r->n].s)
		return 1;
	memcpy(r->k[r->n].s, key->s, (size_t)key->len);
	r->k[r->n].len = key->len;
	r->wt[r->n] = mt ? mt->wtick : 0;
	r->n++;
	return 0;
}

static void rw_path(char *p, size_t n, const char *state_dir,
		const struct up_col *uc)
{
	snprintf(p, n, "%.1024s/upstream-rewarm-%.*s", state_dir, (int)uc->nlen,
		uc->name);
}

static void rewarm_save(struct up_col *uc, const char *state_dir)
{
	struct rw_names r = { NULL, NULL, 0, 0 };
	unsigned int *order = NULL;
	char path[1400], tmp[1420];
	FILE *f;
	int i, k, written = 0;

	if (!state_dir || !*state_dir || !uc->through || uc->rewarm_max <= 0)
		return;
	for (k = 0; k < uc->nsrv; k++)
		if (uc->srv[k].shadow)
			pcache_ht_iter_meta(uc->srv[k].shadow, rw_collect, &r);
	/* what the re-warm list still had: it was wanted, keep wanting it */
	for (i = uc->rw_head; i < uc->rw_n; i++) {
		struct pcache_rec_meta mt = { 0, 0, 0, 0 };

		if (uc->rw[i].s)
			rw_collect(&uc->rw[i], NULL, &mt, &r);
	}
	rw_path(path, sizeof path, state_dir, uc);
	if (!r.n) {
		unlink(path);
		LM_NOTICE("collection '%s': nothing held to re-warm after the "
			"restart\n", uc->name);
		return;
	}
	order = malloc((size_t)r.n * 2 * sizeof *order);
	if (order) {
		for (i = 0; i < r.n; i++) {
			order[2 * i] = r.wt[i];
			order[2 * i + 1] = (unsigned int)i;
		}
		qsort(order, (size_t)r.n, 2 * sizeof *order, rw_cmp_desc);
	}
	snprintf(tmp, sizeof tmp, "%.1400s.tmp", path);
	f = fopen(tmp, "wb");
	if (!f)
		LM_WARN("collection '%s': cannot write %s: %s - the restart "
			"re-warms nothing\n", uc->name, tmp, strerror(errno));
	if (f) {
		for (i = 0; i < r.n && written < uc->rewarm_max; i++) {
			int at = order ? (int)order[2 * i + 1] : i;
			unsigned char len[4];
			size_t l = (size_t)r.k[at].len;

			len[0] = l & 255; len[1] = (l >> 8) & 255;
			len[2] = (l >> 16) & 255; len[3] = (l >> 24) & 255;
			if (fwrite(len, 1, 4, f) != 4 || fwrite(r.k[at].s, 1, l, f) != l)
				break;
			written++;
		}
		if (fclose(f) == 0 && rename(tmp, path) == 0)
			LM_NOTICE("collection '%s': %d key name(s) held saved for the "
				"re-warm after the restart\n", uc->name, written);
		else
			unlink(tmp);
	}
	for (i = 0; i < r.n; i++)
		free(r.k[i].s);
	free(r.k);
	free(r.wt);
	free(order);
}

static void rewarm_load(struct up_col *uc, const char *state_dir)
{
	char path[1400];
	FILE *f;
	int n = 0;

	if (!state_dir || !*state_dir || !uc->through)
		return;
	rw_path(path, sizeof path, state_dir, uc);
	f = fopen(path, "rb");
	if (!f)
		return;
	unlink(path);                      /* one shot */
	if (uc->rewarm_max <= 0) {
		fclose(f);
		return;
	}
	for (;;) {
		unsigned char len[4];
		size_t l;
		str k;

		if (fread(len, 1, 4, f) != 4)
			break;
		l = (size_t)len[0] | (size_t)len[1] << 8 | (size_t)len[2] << 16 |
			(size_t)len[3] << 24;
		if (l == 0 || l > 65536)
			break;                     /* not ours */
		k.s = malloc(l);
		if (!k.s || fread(k.s, 1, l, f) != l) {
			free(k.s);
			break;
		}
		k.len = (int)l;
		if (uc->rw_n >= uc->rewarm_max) {
			free(k.s);
			break;
		}
		if (uc->rw_n == uc->rw_cap) {
			int nc = uc->rw_cap ? uc->rw_cap * 2 : 1024;
			str *nr = realloc(uc->rw, (size_t)nc * sizeof *nr);

			if (!nr) {
				free(k.s);
				break;
			}
			uc->rw = nr;
			uc->rw_cap = nc;
		}
		uc->rw[uc->rw_n++] = k;
		n++;
	}
	fclose(f);
	if (n) {
		uc->rw_runs++;
		uc->rw_last_ms = now_ms();
		LM_NOTICE("collection '%s': re-warming %d key(s) held before the "
			"restart, in the background at %d keys/s\n", uc->name, n,
			uc->drain_rate);
	}
}
