/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clpend.c - parked requests and the completion queues.  See clpend.h.
 * Logging stays with the caller, so clpendtest links this file alone. */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "clpend.h"
#include "compat/timer.h"                /* get_ticks */
#include "resident.h"

int clpend_init(struct clpend *p, int cap)
{
	int i;

	/* clamped, not rejected: the ceiling is structural (the slot index rides in the
	 * request id); the caller compares the returned cap and says so */
	if (cap <= 0)
		cap = PEND_DEFAULT;
	if (cap > PEND_LIMIT)
		cap = PEND_LIMIT;
	if (cap < PEND_MIN)
		cap = PEND_MIN;
	free(p->pend);
	p->pend = calloc((size_t)cap, sizeof *p->pend);
	if (!p->pend)
		return 0;
	/* fault it in now: slots are taken in turn, so it would otherwise fault a page
	 * at a time as requests walk through it */
	pc_touch_pages(p->pend, (size_t)cap * sizeof *p->pend);
	p->cap = cap;
	p->used = 0;
	p->peak = 0;
	p->exhausted = 0;
	p->reconcile_expired = 0;
	memset(p->used_kind, 0, sizeof p->used_kind);
	pthread_mutex_init(&p->mx, NULL);
	for (i = 0; i < WORKERS_MAX; i++)
		pthread_mutex_init(&p->cq[i].mx, NULL);
	return cap;
}

void clpend_fini(struct clpend *p)
{
	int i;

	for (i = 0; i < WORKERS_MAX; i++) {
		free(p->cq[i].q);
		p->cq[i].q = NULL;
		pthread_mutex_destroy(&p->cq[i].mx);
	}
	free(p->pend);
	p->pend = NULL;
	p->cap = p->used = p->peak = 0;
	pthread_mutex_destroy(&p->mx);
}

void clpend_lock(struct clpend *p)   { pthread_mutex_lock(&p->mx); }
void clpend_unlock(struct clpend *p) { pthread_mutex_unlock(&p->mx); }

/* the ONLY way a slot is released, so occupancy stays exact; idempotent (a
 * caller frees an answered slot again).  Caller holds the lock. */
void clpend_release_locked(struct clpend *p, struct pending *slot)
{
	if (!slot->req)
		return;
	slot->req = 0;
	if (p->used > 0)
		p->used--;
	if (slot->kind >= 0 && slot->kind < 16 && p->used_kind[slot->kind] > 0)
		p->used_kind[slot->kind]--;
}

int clpend_used_kind(const struct clpend *p, int kind)
{
	return kind >= 0 && kind < 16 ? p->used_kind[kind] : 0;
}

void clpend_release(struct clpend *p, struct pending *slot)
{
	pthread_mutex_lock(&p->mx);
	clpend_release_locked(p, slot);
	pthread_mutex_unlock(&p->mx);
}

struct pending *clpend_find(struct clpend *p, uint32_t req)
{
	uint32_t i;

	/* not one of ours: a migration group id, or a peer echoing
	 * something we never issued */
	if (!(req & PEND_TAG))
		return NULL;
	i = req & PEND_SLOT_MASK;
	if (i >= (uint32_t)p->cap)
		return NULL;
	return p->pend[i].req == req ? &p->pend[i] : NULL;
}

uint32_t clpend_alloc(struct clpend *p, int kind, int expect, int extra_ms,
		int worker, long long now, int pull_timeout_ms,
		struct pending **out, int *reason)
{
	struct pending *slot = NULL;
	uint32_t req, gen;
	int i;

	*reason = PC_CLFAIL_NONE;
	pthread_mutex_lock(&p->mx);
	for (i = 0; i < p->cap; i++)
		if (p->pend[i].req == 0) {
			slot = &p->pend[i];
			break;
		}
	if (!slot) {
		p->exhausted++;
		pthread_mutex_unlock(&p->mx);
		*reason = PC_CLFAIL_BUSY;
		return 0;
	}
	/* exact occupancy, so the peak can reach cap (a knob is sized from it) */
	if (++p->used > p->peak)
		p->peak = p->used;
	pc_winmax_note(&p->wpeak, get_ticks(), (unsigned long long)p->used);
	/* the generation is the only field that must survive the memset: it makes
	 * this id differ from the last one this slot carried */
	gen = (slot->gen + 1) & (PEND_GEN_MASK >> PEND_GEN_SHIFT);
	if (!gen)
		gen = 1;
	req = PEND_TAG | ((uint32_t)gen << PEND_GEN_SHIFT) | (uint32_t)i;
	memset(slot, 0, sizeof *slot);
	slot->gen = gen;
	slot->req = req;
	slot->worker = worker;
	slot->expect = expect;
	slot->kind = kind;
	if (kind >= 0 && kind < 16)
		p->used_kind[kind]++;
	slot->deadline_ms = now + pull_timeout_ms + extra_ms;
	pthread_mutex_unlock(&p->mx);
	*out = slot;
	return req;
}

int clpend_used(const struct clpend *p) { return p->used; }
int clpend_peak(const struct clpend *p) { return p->peak; }
int clpend_peak_win(const struct clpend *p)
{
	return (int)pc_winmax_get(&p->wpeak, get_ticks());
}
unsigned long long clpend_exhausted(const struct clpend *p)
{
	return p->exhausted;
}

/* ---- completion queues --------------------------------------------- */

void clpend_cq_register(struct clpend *p, int worker, int efd)
{
	if (worker < 0 || worker >= WORKERS_MAX)
		return;
	if (!p->cq[worker].q)
	{
		p->cq[worker].q = calloc((size_t)p->cap,
			sizeof *p->cq[worker].q);
		pc_touch_pages(p->cq[worker].q,
			(size_t)p->cap * sizeof *p->cq[worker].q);
	}
	p->cq[worker].efd = efd;
}

void clpend_cq_post(struct clpend *p, int worker, uint32_t req, int kind,
		int found, int ok, long long newval, int from_node,
		unsigned int ttl_left, const unsigned char *val, int vlen,
		int jop, int count, unsigned long long ver, int timedout)
{
	struct cqueue *q;
	uint64_t one = 1;

	if (worker < 0 || worker >= WORKERS_MAX)
		return;
	q = &p->cq[worker];
	pthread_mutex_lock(&q->mx);
	if (q->q && q->tail - q->head < p->cap) {
		struct pc_pull_done *d = &q->q[q->tail % p->cap];

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

int clpend_cq_drain(struct clpend *p, int worker, struct pc_pull_done *out,
		int max)
{
	struct cqueue *q;
	int n = 0;

	if (worker < 0 || worker >= WORKERS_MAX)
		return 0;
	q = &p->cq[worker];
	pthread_mutex_lock(&q->mx);
	while (q->head != q->tail && n < max) {
		out[n++] = q->q[q->head % p->cap];
		q->head++;
	}
	pthread_mutex_unlock(&q->mx);
	return n;
}

int clpend_retire(struct clpend *p, uint32_t req, int only_unanswered,
		int *worker, int *kind)
{
	struct pending *s;
	int got = 0;

	pthread_mutex_lock(&p->mx);
	s = clpend_find(p, req);
	if (s && !(only_unanswered && s->answered)) {
		*worker = s->worker;
		*kind = s->kind;
		clpend_release_locked(p, s);
		got = 1;
	}
	pthread_mutex_unlock(&p->mx);
	return got;
}

int clpend_answer(struct clpend *p, uint32_t req, int positive, int from_node,
		unsigned long long ver, uint32_t vsum, long long fwd_deadline_ms,
		struct clpend_ans_out *out)
{
	struct pending *s;
	int r = CLPEND_ANS_IGNORE;

	memset(out, 0, sizeof(*out));
	pthread_mutex_lock(&p->mx);
	s = clpend_find(p, req);
	if (!s) {
		pthread_mutex_unlock(&p->mx);
		return CLPEND_ANS_IGNORE;
	}

	if (s->probe_op && positive) {
		/* probe hit: the park becomes a FWD to the holder (same req); answered
		 * stays 0 and expect is huge, so only the holder's ack frees the slot */
		out->probe_op = s->probe_op;
		out->stash = s->stash;
		out->stash_len = s->stash_len;
		out->by = s->by;
		out->ttl_rel = s->ttl_rel;
		out->from_node = from_node;
		memcpy(out->col, s->col, s->collen);
		out->collen = s->collen;
		memcpy(out->key, s->key, s->klen);
		out->klen = s->klen;

		if (s->kind >= 0 && s->kind < 16 && p->used_kind[s->kind] > 0)
			p->used_kind[s->kind]--;          /* the gauge follows */
		s->kind = s->probe_op == 2 ? PC_DONE_FWD_ADD
			: s->probe_op == 3 ? PC_DONE_FWD_TOUCH
			: s->probe_op == 4 ? PC_DONE_FWD_DEL : PC_DONE_FWD_SET;
		p->used_kind[s->kind]++;
		out->kind = s->kind;
		s->first_node = from_node;
		/* and at which version, so a second holder of the same record reads
		 * as SAME below, not as a conflict that demotes a healthy replica */
		s->first_ver = ver;
		s->first_sum = vsum;
		s->probe_op = 0;
		s->probe_fwd = 1;
		s->stash = NULL;
		s->stash_len = 0;
		s->expect = 1 << 30;
		s->deadline_ms = fwd_deadline_ms;
		pthread_mutex_unlock(&p->mx);
		return CLPEND_ANS_PROBE_FWD;
	}

	if (positive) {
		if (!s->answered && !s->probe_fwd) {
			/* the first positive wins; the slot LINGERS so a
			 * second positive can still trigger the demote */
			out->worker = s->worker;
			out->from_node = from_node;
			s->answered = 1;
			s->first_node = from_node;
			s->first_ver = ver;
			s->first_sum = vsum;
			if (s->kind == PC_DONE_POSSESS) {
				/* only a holder that HAS the record may authorise dropping our
				 * copy (liveness proves nothing); one yes is enough, repair restores K */
				memcpy(out->col, s->col, s->collen);
				out->collen = s->collen;
				memcpy(out->key, s->key, s->klen);
				out->klen = s->klen;
				clpend_release_locked(p, s);
				r = CLPEND_ANS_POSSESS;
			} else {
				/* the key, for the receiver's retained-tombstone check */
				memcpy(out->col, s->col, s->collen);
				out->collen = s->collen;
				memcpy(out->key, s->key, s->klen);
				out->klen = s->klen;
				if (--s->expect <= 0)
					clpend_release_locked(p, s);
				r = CLPEND_ANS_HIT;
			}
		} else if (ver && s->first_ver && ver == s->first_ver &&
		           (!vsum || !s->first_sum || vsum == s->first_sum)) {
			/* same record, not a race (eager: every key on every node); equal
			 * versions are one write only if the bytes agree too, vsum 0 cannot tell */
			if (--s->expect <= 0)
				clpend_release_locked(p, s);
			r = CLPEND_ANS_SAME;
		} else {
			/* a conflict: the newer version (the later write) wins; a tie (same
			 * version, other bytes) or an unknown version goes to the lower id */
			int older_first = ver && s->first_ver && ver != s->first_ver
				? s->first_ver < ver
				: s->first_node > from_node;

			out->winner = older_first ? from_node : s->first_node;
			out->loser  = older_first ? s->first_node : from_node;
			/* the copy the demote is for - unknown (0) unless both answers said
			 * their version */
			out->loser_ver = ver && s->first_ver
				? (older_first ? s->first_ver : ver) : 0;
			memcpy(out->col, s->col, s->collen);
			out->collen = s->collen;
			memcpy(out->key, s->key, s->klen);
			out->klen = s->klen;
			if (--s->expect <= 0)
				clpend_release_locked(p, s);
			r = CLPEND_ANS_RACE;
		}
		pthread_mutex_unlock(&p->mx);
		return r;
	}

	if (--s->expect <= 0 && !s->answered) {
		out->worker = s->worker;
		if (s->probe_op) {
			/* fleet-wide confirmed absent: replay the write on
			 * the worker, the probe suppressed */
			out->probe_op = s->probe_op;
			out->stash = s->stash;
			out->stash_len = s->stash_len;
			out->by = s->by;
			out->ttl_rel = s->ttl_rel;
			s->stash = NULL;
			r = CLPEND_ANS_RESUME;
		} else if (s->kind == PC_DONE_RECONCILE) {
			/* nobody has the key, and eager mode puts every key everywhere: it was
			 * DELETED while this node was down and replay brought it back */
			memcpy(out->col, s->col, s->collen);
			out->collen = s->collen;
			memcpy(out->key, s->key, s->klen);
			out->klen = s->klen;
			r = CLPEND_ANS_RECONCILE;
		} else {
			r = CLPEND_ANS_MISS;   /* everyone said no */
		}
		clpend_release_locked(p, s);
	} else if (s->expect <= 0) {
		clpend_release_locked(p, s);   /* answered earlier: just free */
	} else {
		r = CLPEND_ANS_WAIT;
	}
	pthread_mutex_unlock(&p->mx);
	return r;
}

int clpend_expire(struct clpend *p, long long now)
{
	int i, worker, timeouts = 0;
	uint32_t req;

	for (i = 0; i < p->cap; i++) {
		if (!p->pend[i].req)
			continue;
		pthread_mutex_lock(&p->mx);
		if (p->pend[i].req && p->pend[i].deadline_ms <= now) {
			int answered = p->pend[i].answered;
			int kind = p->pend[i].kind;
			int jop = p->pend[i].jop;
			int probe_op = p->pend[i].probe_op;
			char *stash = p->pend[i].stash;

			req = p->pend[i].req;
			worker = p->pend[i].worker;
			p->pend[i].stash = NULL;
			clpend_release_locked(p, &p->pend[i]);
			pthread_mutex_unlock(&p->mx);
			free(stash);
			if (!answered) {
				timeouts++;
				if (kind == PC_DONE_RECONCILE)
					p->reconcile_expired++;
				if (probe_op) {
					/* probe timed out: ambiguous - an honest
					 * refusal beats a potential fork */
					clpend_cq_post(p, worker, req,
						PC_DONE_SET_RESUME, 0, 0, 0, 0,
						0, NULL, 0, probe_op, 0, 0, 1);
				} else {
					/* timeouts surface as status 2 for JSON ops */
					clpend_cq_post(p, worker, req, kind, 0,
						kind == PC_DONE_JSON ? 2 : 0, 0,
						0, 0, NULL, 0, jop, 0, 0, 1);
				}
			}
			continue;
		}
		pthread_mutex_unlock(&p->mx);
	}
	return timeouts;
}
