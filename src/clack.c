/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clack.c - 12ny AT-3: watermarks - the contiguous applied mark of each
 * numbering space and the part of it the WAL has made durable.  See
 * clack.h for the rule and why the durability is folded the way it is.
 */
#include <stdlib.h>
#include <string.h>
#include "clack.h"

struct pend { uint64_t seq, w; };

struct space {
	uint64_t oid;
	uint32_t incarn;
	unsigned int nahead;          /* bits set in ahead[] */
	uint64_t mark;                /* contiguous applied */
	uint64_t dmark;               /* confirmed: contiguous and durable */
	uint64_t hi;                  /* highest seq seen, a FAR one too */
	uint64_t wmax;                /* the WAL max over 1..mark */
	uint64_t wahead;              /* the WAL max over what is ahead */
	unsigned int npend;
	struct pend pend[CLACK_PEND]; /* oldest first; w ascending */
	/* bit (seq % CLACK_WIN) for a seq in (mark, mark + CLACK_WIN] held
	 * out of order - the window maps each such seq to its own bit */
	uint64_t ahead[CLACK_WIN / 64];
};

struct clack {
	int durable_on_apply;
	unsigned int n;
	uint64_t synced;
	unsigned long long applied, dups, far, full;
	struct space *s[CLACK_SPACES];
};

static int bit(const struct space *sp, uint64_t seq)
{
	unsigned int b = (unsigned int)(seq % CLACK_WIN);

	return (sp->ahead[b / 64] >> (b % 64)) & 1;
}

static void bit_set(struct space *sp, uint64_t seq, int on)
{
	unsigned int b = (unsigned int)(seq % CLACK_WIN);

	if (on)
		sp->ahead[b / 64] |= 1ull << (b % 64);
	else
		sp->ahead[b / 64] &= ~(1ull << (b % 64));
}

static int find(const struct clack *a, uint64_t oid, uint32_t incarn)
{
	unsigned int i;

	for (i = 0; i < a->n; i++)
		if (a->s[i]->oid == oid && a->s[i]->incarn == incarn)
			return (int)i;
	return -1;
}

static struct space *get(struct clack *a, uint64_t oid, uint32_t incarn)
{
	struct space *sp;
	int i = find(a, oid, incarn);

	if (i >= 0)
		return a->s[i];
	if (a->n == CLACK_SPACES || !(sp = calloc(1, sizeof *sp))) {
		a->full++;
		return NULL;
	}
	sp->oid = oid;
	sp->incarn = incarn;
	a->s[a->n++] = sp;
	return sp;
}

struct clack *clack_new(int durable_on_apply)
{
	struct clack *a = calloc(1, sizeof *a);

	if (a)
		a->durable_on_apply = durable_on_apply;
	return a;
}

void clack_free(struct clack *a)
{
	unsigned int i;

	if (!a)
		return;
	for (i = 0; i < a->n; i++)
		free(a->s[i]);
	free(a);
}

/* the mark moved to sp->mark with sp->wmax: confirm it now, or queue it
 * for the sync that covers it */
static void confirm(struct clack *a, struct space *sp)
{
	struct pend *t;

	if (a->durable_on_apply || sp->wmax <= a->synced) {
		sp->dmark = sp->mark;
		sp->npend = 0;             /* every queued w is <= wmax */
		return;
	}
	t = sp->npend ? &sp->pend[sp->npend - 1] : NULL;
	if (t && t->w == sp->wmax)
		t->seq = sp->mark;
	else if (sp->npend == CLACK_PEND) {
		/* full: the last pair waits for the larger max too */
		t->seq = sp->mark;
		t->w = sp->wmax;
	} else {
		sp->pend[sp->npend].seq = sp->mark;
		sp->pend[sp->npend++].w = sp->wmax;
	}
}

int clack_applied(struct clack *a, uint64_t oid, uint32_t incarn,
		uint64_t seq, uint64_t walseq)
{
	struct space *sp;
	uint64_t w;
	int took = 0;

	if (!seq)
		return CLACK_BAD;
	if (!(sp = get(a, oid, incarn)))
		return CLACK_FULL;
	if (seq <= sp->mark || (seq - sp->mark <= CLACK_WIN && bit(sp, seq))) {
		a->dups++;
		return CLACK_DUP;
	}
	if (seq > sp->hi)
		sp->hi = seq;
	if (seq - sp->mark > CLACK_WIN) {
		a->far++;
		return CLACK_FAR;
	}
	a->applied++;
	if (seq != sp->mark + 1) {
		bit_set(sp, seq, 1);
		sp->nahead++;
		if (walseq > sp->wahead)
			sp->wahead = walseq;
		return CLACK_NEW;
	}
	/* the next in line: it, and every held seq it now connects */
	w = walseq > sp->wmax ? walseq : sp->wmax;
	sp->mark = seq;
	while (sp->nahead && bit(sp, sp->mark + 1)) {
		bit_set(sp, sp->mark + 1, 0);
		sp->nahead--;
		sp->mark++;
		took = 1;
	}
	if (took && sp->wahead > w)
		w = sp->wahead;            /* all of it: conservative */
	if (!sp->nahead)
		sp->wahead = 0;
	sp->wmax = w;
	confirm(a, sp);
	return CLACK_NEW;
}

void clack_synced(struct clack *a, uint64_t walseq)
{
	unsigned int i, k;

	if (walseq <= a->synced)
		return;
	a->synced = walseq;
	for (i = 0; i < a->n; i++) {
		struct space *sp = a->s[i];

		for (k = 0; k < sp->npend && sp->pend[k].w <= walseq; k++)
			if (sp->pend[k].seq > sp->dmark)   /* never back (a floor) */
				sp->dmark = sp->pend[k].seq;
		if (k) {
			memmove(sp->pend, sp->pend + k,
				(sp->npend - k) * sizeof sp->pend[0]);
			sp->npend -= k;
		}
	}
}

int clack_mark(const struct clack *a, uint64_t oid, uint32_t incarn,
		uint64_t *mark)
{
	int i = find(a, oid, incarn);

	if (i < 0)
		return 0;
	*mark = a->s[i]->dmark;
	return 1;
}

int clack_gap(const struct clack *a, uint64_t oid, uint32_t incarn,
		uint64_t *from, uint64_t *to)
{
	const struct space *sp;
	uint64_t s;
	int i = find(a, oid, incarn);

	if (i < 0)
		return 0;
	sp = a->s[i];
	if (sp->hi <= sp->mark)
		return 0;
	*from = sp->mark + 1;
	*to = sp->hi;                  /* only FAR seqs past the mark */
	if (sp->nahead)
		for (s = sp->mark + 2; s <= sp->mark + CLACK_WIN; s++)
			if (bit(sp, s)) {
				*to = s - 1;
				break;
			}
	return 1;
}

int clack_restore(struct clack *a, uint64_t oid, uint32_t incarn,
		uint64_t mark)
{
	struct space *sp = get(a, oid, incarn);

	if (!sp)
		return CLACK_FULL;
	memset(sp->ahead, 0, sizeof sp->ahead);
	sp->nahead = sp->npend = 0;
	sp->mark = sp->dmark = sp->hi = mark;
	sp->wmax = sp->wahead = 0;
	return CLACK_NEW;
}

int clack_floor(struct clack *a, uint64_t oid, uint32_t incarn,
		uint64_t floor)
{
	struct space *sp;
	unsigned int k, n;
	uint64_t s;
	int took = 0;

	if (!(sp = get(a, oid, incarn)))
		return CLACK_FULL;
	if (floor <= sp->mark)
		return CLACK_DUP;
	/* what was held ahead at or below the floor is inside it now; past
	 * the window nothing was held (it would have been FAR) */
	if (floor - sp->mark > CLACK_WIN) {
		memset(sp->ahead, 0, sizeof sp->ahead);
		sp->nahead = 0;
	} else
		for (s = sp->mark + 1; s <= floor && sp->nahead; s++)
			if (bit(sp, s)) {
				bit_set(sp, s, 0);
				sp->nahead--;
			}
	sp->mark = floor;
	if (sp->hi < floor)
		sp->hi = floor;
	if (sp->dmark < floor)
		sp->dmark = floor;             /* the fleet's: nothing to wait for */
	for (k = n = 0; k < sp->npend; k++)    /* pairs inside it are moot */
		if (sp->pend[k].seq > floor)
			sp->pend[n++] = sp->pend[k];
	sp->npend = n;
	while (sp->nahead && bit(sp, sp->mark + 1)) {
		bit_set(sp, sp->mark + 1, 0);
		sp->nahead--;
		sp->mark++;
		took = 1;
	}
	if (took) {                            /* those wait for their records */
		if (sp->wahead > sp->wmax)
			sp->wmax = sp->wahead;
		confirm(a, sp);
	}
	if (!sp->nahead)
		sp->wahead = 0;
	return CLACK_NEW;
}

int clack_known(const struct clack *a, uint64_t oid, uint32_t incarn)
{
	return find(a, oid, incarn) >= 0;
}

void clack_drop(struct clack *a, uint64_t oid, uint32_t incarn)
{
	int i = find(a, oid, incarn);

	if (i < 0)
		return;
	free(a->s[i]);
	a->s[i] = a->s[--a->n];
}

int clack_each(const struct clack *a, clack_each_cb cb, void *ctx)
{
	unsigned int i;
	int rc;

	for (i = 0; i < a->n; i++) {
		const struct space *sp = a->s[i];

		if ((rc = cb(sp->oid, sp->incarn, sp->dmark, sp->mark,
		        sp->hi, ctx)))
			return rc;
	}
	return 0;
}

void clack_get_stats(const struct clack *a, struct clack_stats *st)
{
	st->applied = a->applied;
	st->dups = a->dups;
	st->far = a->far;
	st->full = a->full;
	st->spaces = a->n;
	st->durable_on_apply = a->durable_on_apply;
}
