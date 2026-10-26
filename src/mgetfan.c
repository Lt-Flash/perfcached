/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * mgetfan.c - G8 (DESIGN 12nh): an mget waiting on several remote lookups
 * at once.  See mgetfan.h.
 *
 * A pull's answer finds its key through the worker's map, indexed by the
 * pending slot the request id names.  A slot can carry two of this
 * worker's requests for a moment - one that expired, its timeout still in
 * the completion queue, and the next request that took the slot - so the
 * map chains them through the keys themselves and a lookup compares the
 * whole id.
 *
 * What a single get's park does with a pull's answer, a key does here
 * too - keep a found value as that mode keeps it (pc_pull_found_keep),
 * ask everyone once after a shard owner's miss inside the reshard grace or
 * a proxy locator's miss, remember a store miss in the negative cache -
 * with two differences.  A retry may not outlive the mget's one deadline
 * (it is begun with the deadline capped, and not at all when the deadline
 * has passed), and a lookup that did not answer is unanswered, never a
 * miss: no negative cache, no "absent" for a key the fleet was not heard
 * on.
 */
#include <stdlib.h>
#include <string.h>

#include "mgetfan.h"
#include "cluster.h"
#include "clpend.h"                    /* PEND_SLOT_MASK */
#include "store.h"
#include "proto.h"
#include "ptree.h"
#include "json.h"
#include "respout.h"
#include "compat/timer.h"

/* a retry needs at least this long before the deadline to be worth it */
#define MGET_RETRY_ROOM_MS 2

struct mref {                          /* a key of an mget: (mget, index) */
	struct pc_mget *m;
	unsigned int i;
};

struct mkey {
	uint32_t req;                      /* the pull it waits on */
	unsigned char st;                  /* PC_MK_* */
	unsigned char retried;             /* asked everyone once already */
	int target;                        /* a unicast pull's node, 0: all */
	char *key;                         /* copied while it waits */
	size_t klen;
	char *val;                         /* a hit's value */
	int vlen;
	unsigned int exp;                  /* absolute ticks, 0 = never */
	struct mref next;                  /* the slot's chain */
};

struct pc_mget {
	void *conn;                        /* NULL until the door attaches */
	int door;
	char id[8];
	char col[256];
	size_t collen;
	unsigned int n, waiting, unanswered;
	long long deadline;
	int linked;                        /* its waiting keys are in the map */
	struct pc_mget *next;              /* the worker's attached mgets */
	struct mkey k[];
};

static __thread struct mref *heads;    /* by pending slot */
static __thread unsigned int nheads;
static __thread struct pc_mget *live;
static __thread struct pc_mget *parked_now;

static unsigned long long n_fanout, n_unanswered, n_refused;
static unsigned long long b_fanout, b_unanswered, b_refused;   /* reset */

/* ---- the map ---------------------------------------------------------- */

static int heads_ready(void)
{
	unsigned int cap;

	if (heads)
		return 0;
	cap = (unsigned int)pc_cluster_pend_cap();
	if (!cap)
		cap = 1024;
	heads = calloc(cap, sizeof *heads);
	if (!heads)
		return -1;
	nheads = cap;
	return 0;
}

static struct mref *head_of(uint32_t req)
{
	return &heads[(req & PEND_SLOT_MASK) % nheads];
}

static void link_key(struct pc_mget *m, unsigned int i)
{
	struct mref *h = head_of(m->k[i].req);

	m->k[i].next = *h;
	h->m = m;
	h->i = i;
}

static void unlink_key(struct pc_mget *m, unsigned int i)
{
	struct mref *pp = head_of(m->k[i].req);

	while (pp->m) {
		if (pp->m == m && pp->i == i) {
			*pp = m->k[i].next;
			break;
		}
		pp = &pp->m->k[pp->i].next;
	}
	m->k[i].next.m = NULL;
}

static int find_key(uint32_t req, struct mref *out)
{
	struct mref r;

	if (!heads || !req)
		return 0;
	for (r = *head_of(req); r.m; r = r.m->k[r.i].next)
		if (r.m->k[r.i].req == req && r.m->k[r.i].st == PC_MK_WAIT) {
			*out = r;
			return 1;
		}
	return 0;
}

/* ---- building it (the verb) ------------------------------------------- */

struct pc_mget *pc_mget_new(const char *col, size_t collen, unsigned int n)
{
	struct pc_mget *m;

	if (collen >= sizeof m->col)
		return NULL;
	m = calloc(1, sizeof *m + (size_t)n * sizeof m->k[0]);
	if (!m)
		return NULL;
	memcpy(m->col, col, collen);
	m->collen = collen;
	m->n = n;
	m->deadline = pc_pull_deadline_ms(col, collen);
	return m;
}

void pc_mget_hit(struct pc_mget *m, unsigned int i, char *val, int vlen,
		unsigned int exp)
{
	m->k[i].st = PC_MK_HIT;
	m->k[i].val = val;
	m->k[i].vlen = vlen;
	m->k[i].exp = exp;
}

void pc_mget_absent(struct pc_mget *m, unsigned int i)
{
	m->k[i].st = PC_MK_ABSENT;
}

int pc_mget_wait(struct pc_mget *m, unsigned int i, const char *key,
		size_t klen, uint32_t req, const char **why)
{
	int cap = pc_cluster_pend_cap();
	unsigned int ttl;

	/* the guard is read after this lookup took its slot: the mget never
	 * leaves the table below a quarter free for everything else */
	if (m->waiting >= PC_MGET_MAX_REMOTE) {
		*why = "mget needs more than 1000 remote lookups - split it";
		return PC_MGET_E_CAP;
	}
	if (pc_cluster_pend_free() < cap / 4) {
		*why = "mget: the cluster's pending table is busy - retry, "
			"or split the mget";
		return PC_MGET_E_BUSY;
	}
	m->k[i].key = malloc(klen ? klen : 1);
	if (!m->k[i].key) {
		*why = "out of memory";
		return PC_MGET_E_NOMEM;
	}
	memcpy(m->k[i].key, key, klen);
	m->k[i].klen = klen;
	m->k[i].req = req;
	m->k[i].st = PC_MK_WAIT;
	m->k[i].target = pc_fwd_target_of(req, &ttl);
	m->waiting++;
	return 0;
}

unsigned int pc_mget_waiting(const struct pc_mget *m)
{
	return m->waiting;
}

long long pc_mget_deadline(const struct pc_mget *m)
{
	return m->deadline;
}

/* ---- the reply -------------------------------------------------------- */

void pc_mget_tree(const struct pc_mget *m, struct pc_tw *w)
{
	unsigned int i;

	pc_tw_map(w);
	pc_tw_key(w, "values");
	pc_tw_arr(w);
	for (i = 0; i < m->n; i++) {
		const struct mkey *k = &m->k[i];

		pc_tw_map(w);
		pc_tw_key(w, "found");
		pc_tw_bool(w, k->st == PC_MK_HIT);
		if (k->st == PC_MK_HIT) {
			/* the {found, value, ttl} shape of get and mget */
			long long ttl = k->exp ? (long long)k->exp -
				(long long)get_ticks() : -1;

			pc_tw_key(w, "value");
			pc_tw_bulk(w, k->val, (size_t)k->vlen);
			pc_tw_key(w, "ttl");
			pc_tw_i64(w, ttl < -1 ? 0 : ttl);
		} else if (k->st == PC_MK_UNANSWERED) {
			pc_tw_key(w, "unanswered");
			pc_tw_bool(w, 1);
		}
		pc_tw_end(w);
	}
	pc_tw_end(w);
	pc_tw_end(w);
}

void pc_mget_resp(const struct pc_mget *m, struct pc_jw *w)
{
	unsigned int i;

	if (m->unanswered) {
		/* all or nothing: never values beside nils for keys it could
		 * not reach - a nil would read as "absent" */
		pc_jw_lit(w, "-TRYAGAIN mget: ");
		pc_jw_i64(w, (long long)m->unanswered);
		pc_jw_lit(w, " key(s) not answered\r\n");
		return;
	}
	resp_arr(w, (int)m->n);
	for (i = 0; i < m->n; i++) {
		if (m->k[i].st == PC_MK_HIT)
			resp_bulk(w, m->k[i].val, (size_t)m->k[i].vlen);
		else
			resp_nil(w);
	}
}

/* ---- its life --------------------------------------------------------- */

void pc_mget_free(struct pc_mget *m)
{
	struct pc_mget **pp;
	unsigned int i;

	if (!m)
		return;
	for (i = 0; i < m->n; i++) {
		if (m->k[i].st == PC_MK_WAIT) {
			if (m->linked)
				unlink_key(m, i);
			pc_pull_cancel(m->k[i].req);
		}
		free(m->k[i].key);
		free(m->k[i].val);
	}
	for (pp = &live; *pp; pp = &(*pp)->next)
		if (*pp == m) {
			*pp = m->next;
			break;
		}
	free(m);
}

void pc_mget_park(struct pc_mget *m)
{
	/* one at a time: the door takes each right after its verb; one
	 * never taken is given up rather than left waiting for ever */
	if (parked_now && parked_now != m)
		pc_mget_free(parked_now);
	parked_now = m;
}

int pc_mget_parked(void)
{
	return parked_now != NULL;
}

struct pc_mget *pc_mget_take(void)
{
	struct pc_mget *m = parked_now;

	parked_now = NULL;
	return m;
}

int pc_mget_attach(struct pc_mget *m, void *conn, int door, const char *id)
{
	unsigned int i;

	if (heads_ready() != 0)
		return -1;
	m->conn = conn;
	m->door = door;
	if (id)
		memcpy(m->id, id, sizeof m->id);
	for (i = 0; i < m->n; i++)
		if (m->k[i].st == PC_MK_WAIT)
			link_key(m, i);
	m->linked = 1;
	m->next = live;
	live = m;
	__atomic_add_fetch(&n_fanout, 1, __ATOMIC_RELAXED);
	return 0;
}

void *pc_mget_conn(const struct pc_mget *m)
{
	return m->conn;
}

int pc_mget_door(const struct pc_mget *m)
{
	return m->door;
}

const char *pc_mget_id(const struct pc_mget *m)
{
	return m->id;
}

void pc_mget_drop_conn(void *conn)
{
	struct pc_mget *m = live, *nx;

	for (; m; m = nx) {
		nx = m->next;
		if (m->conn == conn)
			pc_mget_free(m);
	}
}

/* ---- a pull's answer -------------------------------------------------- */

static void unanswered(struct pc_mget *m, struct mkey *k)
{
	k->st = PC_MK_UNANSWERED;
	m->unanswered++;
}

/* ask everyone once more, inside the deadline: 1 when it was asked */
static int ask_all(struct pc_mget *m, unsigned int i)
{
	struct mkey *k = &m->k[i];
	uint32_t req;

	if (pc_cluster_now_ms() + MGET_RETRY_ROOM_MS > m->deadline)
		return 0;
	pc_pull_cap_deadline(m->deadline);
	req = pc_pull_begin(m->col, m->collen, k->key, k->klen);
	pc_pull_cap_deadline(0);
	if (!req)
		return 0;
	k->req = req;
	k->retried = 1;
	k->target = 0;
	k->st = PC_MK_WAIT;
	link_key(m, i);
	m->waiting++;
	return 1;
}

static void settle(struct pc_mget *m, unsigned int i,
		const struct pc_pull_done *d)
{
	struct mkey *k = &m->k[i];
	pcache_htable_t *ht = pc_store_find(m->col, m->collen);

	if (d->found) {
		pc_pull_found_keep(m->col, m->collen, k->key, k->klen, d);
		/* S280: a document answers an mget as a miss under strict */
		if (ht && (d->newval & PCACHE_F_TYPES) &&
		        pc_store_types_strict(ht)) {
			k->st = PC_MK_ABSENT;
			return;
		}
		k->val = malloc(d->vlen > 0 ? (size_t)d->vlen : 1);
		if (!k->val) {
			unanswered(m, k);
			return;
		}
		if (d->vlen > 0)
			memcpy(k->val, d->val, (size_t)d->vlen);
		k->vlen = d->vlen;
		k->exp = d->ttl_left ? get_ticks() + d->ttl_left : 0;
		k->st = PC_MK_HIT;
		return;
	}
	if (d->timedout) {
		/* not everyone asked answered by the deadline */
		unanswered(m, k);
		return;
	}
	if (ht && pc_store_shard_enabled(ht)) {
		/* an owner miss is authoritative - but inside the reshard
		 * grace the data may still sit on the old owner: ask everyone
		 * once, as a get does */
		if (!k->retried && pc_shard_grace()) {
			if (!ask_all(m, i))
				unanswered(m, k);
			return;
		}
		k->st = PC_MK_ABSENT;
		return;
	}
	if (ht && pc_store_proxy_enabled(ht) && k->target && !k->retried) {
		/* S321: the locator can be stale - forget it, ask everyone */
		pc_loc_clear(m->col, m->collen, k->key, k->klen);
		if (!ask_all(m, i))
			unanswered(m, k);
		return;
	}
	pc_neg_set(m->col, m->collen, k->key, k->klen, pc_cluster_neg_ms());
	pc_loc_clear(m->col, m->collen, k->key, k->klen);
	k->st = PC_MK_ABSENT;
}

int pc_mget_pull_done(const struct pc_pull_done *d, struct pc_mget **done)
{
	struct mref r;
	struct pc_mget *m;

	*done = NULL;
	if (d->kind != PC_DONE_PULL || !find_key(d->req, &r))
		return 0;
	m = r.m;
	unlink_key(m, r.i);
	m->k[r.i].req = 0;
	m->waiting--;
	settle(m, r.i, d);
	if (!m->waiting) {
		if (m->unanswered)
			__atomic_add_fetch(&n_unanswered, 1, __ATOMIC_RELAXED);
		*done = m;
	}
	return 1;
}

/* ---- counters --------------------------------------------------------- */

void pc_mget_stats(int since_reset, unsigned long long *fanout,
		unsigned long long *unans, unsigned long long *refused)
{
	*fanout = __atomic_load_n(&n_fanout, __ATOMIC_RELAXED);
	*unans = __atomic_load_n(&n_unanswered, __ATOMIC_RELAXED);
	*refused = __atomic_load_n(&n_refused, __ATOMIC_RELAXED);
	if (since_reset) {
		*fanout -= __atomic_load_n(&b_fanout, __ATOMIC_RELAXED);
		*unans -= __atomic_load_n(&b_unanswered, __ATOMIC_RELAXED);
		*refused -= __atomic_load_n(&b_refused, __ATOMIC_RELAXED);
	}
}

/* the stats reset (statsreset.c, one at a time): a baseline, so /metrics
 * keeps counting from the start */
void pc_mget_stats_reset(void)
{
	__atomic_store_n(&b_fanout, __atomic_load_n(&n_fanout,
		__ATOMIC_RELAXED), __ATOMIC_RELAXED);
	__atomic_store_n(&b_unanswered, __atomic_load_n(&n_unanswered,
		__ATOMIC_RELAXED), __ATOMIC_RELAXED);
	__atomic_store_n(&b_refused, __atomic_load_n(&n_refused,
		__ATOMIC_RELAXED), __ATOMIC_RELAXED);
}

void pc_mget_count_refused(void)
{
	__atomic_add_fetch(&n_refused, 1, __ATOMIC_RELAXED);
}
