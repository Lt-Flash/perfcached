/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clackf.c - 12ny AT-4: the members' announced marks.  See clackf.h.
 */
#include <stdlib.h>
#include <string.h>
#include "clackf.h"

struct fmark { uint64_t oid; uint32_t sincarn; uint64_t mark;
	long long moved; };                /* AT-11: when it last changed */

struct fmember {
	uint64_t member;
	uint32_t incarn;
	unsigned int n, cap;
	struct fmark *m;
	long long heard;               /* AT-8: its last keepalive */
};

struct clackf {
	unsigned int n;
	struct fmember mb[CLACKF_MEMBERS];
	unsigned long long reports, lowered, resets, refused;
};

static struct fmember *mfind(const struct clackf *f, uint64_t member)
{
	unsigned int i;

	for (i = 0; i < f->n; i++)
		if (f->mb[i].member == member)
			return (struct fmember *)&f->mb[i];
	return NULL;
}

static struct fmark *sfind(const struct fmember *m, uint64_t oid,
		uint32_t sincarn)
{
	unsigned int i;

	for (i = 0; i < m->n; i++)
		if (m->m[i].oid == oid && m->m[i].sincarn == sincarn)
			return &m->m[i];
	return NULL;
}

struct clackf *clackf_new(void)
{
	return calloc(1, sizeof(struct clackf));
}

void clackf_free(struct clackf *f)
{
	unsigned int i;

	if (!f)
		return;
	for (i = 0; i < f->n; i++)
		free(f->mb[i].m);
	free(f);
}

int clackf_member(struct clackf *f, uint64_t member, uint32_t incarn)
{
	struct fmember *m = mfind(f, member);

	if (!m) {
		if (f->n == CLACKF_MEMBERS) {
			f->refused++;
			return -1;
		}
		m = &f->mb[f->n++];
		memset(m, 0, sizeof *m);
		m->member = member;
		m->incarn = incarn;
		return 0;
	}
	if (m->incarn == incarn)
		return 0;
	m->incarn = incarn;            /* another state: nothing it said holds */
	m->n = 0;
	f->resets++;
	return 1;
}

long long clackf_heard(struct clackf *f, uint64_t member, long long now)
{
	struct fmember *m = mfind(f, member);
	long long was;

	if (!m)
		return 0;
	was = m->heard;
	m->heard = now;
	return was;
}

int clackf_report(struct clackf *f, uint64_t member, uint64_t oid,
		uint32_t sincarn, uint64_t mark, long long now)
{
	struct fmember *m = mfind(f, member);
	struct fmark *s;

	if (!m)
		return -1;
	f->reports++;
	if ((s = sfind(m, oid, sincarn))) {
		int down = mark < s->mark;

		if (mark != s->mark)
			s->moved = now;
		s->mark = mark;
		f->lowered += (unsigned long long)down;
		return down;
	}
	if (m->n == m->cap) {
		unsigned int cap = m->cap ? m->cap * 2 : 4;
		struct fmark *nm;

		if (cap > CLACKF_SPACES)
			cap = CLACKF_SPACES;
		if (m->n == cap || !(nm = realloc(m->m, cap * sizeof *nm))) {
			f->refused++;
			return -1;
		}
		m->m = nm;
		m->cap = cap;
	}
	m->m[m->n].oid = oid;
	m->m[m->n].sincarn = sincarn;
	m->m[m->n].moved = now;
	m->m[m->n++].mark = mark;
	return 0;
}

void clackf_clear(struct clackf *f, uint64_t member)
{
	struct fmember *m = mfind(f, member);

	if (m)
		m->n = 0;
}

void clackf_forget(struct clackf *f, uint64_t member)
{
	struct fmember *m = mfind(f, member);

	if (!m)
		return;
	free(m->m);
	*m = f->mb[--f->n];
}

int clackf_mark(const struct clackf *f, uint64_t member, uint64_t oid,
		uint32_t sincarn, uint64_t *mark)
{
	const struct fmember *m = mfind(f, member);
	const struct fmark *s;

	if (!m || !(s = sfind(m, oid, sincarn)))
		return 0;
	*mark = s->mark;
	return 1;
}

uint64_t clackf_min(const struct clackf *f, uint64_t oid, uint32_t sincarn,
		const uint64_t *members, int n)
{
	uint64_t lo = ~0ull, v;
	int i;

	if (n <= 0)
		return 0;
	for (i = 0; i < n; i++) {
		if (!clackf_mark(f, members[i], oid, sincarn, &v))
			return 0;          /* not reported: nothing is confirmed */
		if (v < lo)
			lo = v;
	}
	return lo;
}

int clackf_each(const struct clackf *f, clackf_each_cb cb, void *ctx)
{
	unsigned int i, k;
	int rc;

	for (i = 0; i < f->n; i++)
		for (k = 0; k < f->mb[i].n; k++)
			if ((rc = cb(f->mb[i].member, f->mb[i].incarn,
			        f->mb[i].m[k].oid, f->mb[i].m[k].sincarn,
			        f->mb[i].m[k].mark, ctx)))
				return rc;
	return 0;
}

void clackf_get_stats(const struct clackf *f, struct clackf_stats *st)
{
	unsigned int i;

	memset(st, 0, sizeof *st);
	st->members = f->n;
	for (i = 0; i < f->n; i++)
		st->marks += f->mb[i].n;
	st->reports = f->reports;
	st->lowered = f->lowered;
	st->resets = f->resets;
	st->refused = f->refused;
}

int clackf_moved(const struct clackf *f, uint64_t member, uint64_t oid,
		uint32_t sincarn, long long *moved)
{
	const struct fmember *m = mfind(f, member);
	const struct fmark *s;

	if (!m || !(s = sfind(m, oid, sincarn)))
		return 0;
	*moved = s->moved;
	return 1;
}

uint64_t clackf_max(const struct clackf *f, uint64_t oid, uint32_t sincarn,
		const uint64_t *members, int n)
{
	uint64_t hi = 0;
	int i;

	if (!members)
		n = (int)f->n;
	for (i = 0; i < n; i++) {
		const struct fmember *m = members ? mfind(f, members[i]) : &f->mb[i];
		const struct fmark *s;

		if (m && (s = sfind(m, oid, sincarn)) && s->mark > hi)
			hi = s->mark;
	}
	return hi;
}

int clackf_stuck(const struct clackf *f, uint64_t oid, uint32_t sincarn,
		const uint64_t *members, int n, uint64_t head, long long also,
		long long now, long long still_ms, struct clackf_stuck *out,
		int max)
{
	const struct fmark *s;
	long long latest = also;
	int i, k = 0;

	/* the latest any of them moved: the others' progress */
	for (i = 0; i < n; i++) {
		const struct fmember *m = mfind(f, members[i]);

		if (m && (s = sfind(m, oid, sincarn)) && s->moved > latest)
			latest = s->moved;
	}
	for (i = 0; i < n; i++) {
		const struct fmember *m = mfind(f, members[i]);
		int j;

		if (!m || !(s = sfind(m, oid, sincarn)) || s->mark >= head ||
		    !s->moved || s->moved > now - still_ms || latest <= s->moved)
			continue;
		/* kept in order, the longest-still first; a full list keeps
		 * only one stiller than its last */
		j = k < max ? k : max;
		k++;
		if (j == max) {
			if (max <= 0 || out[max - 1].moved <= s->moved)
				continue;
			j = max - 1;
		}
		for (; j > 0 && out[j - 1].moved > s->moved; j--)
			out[j] = out[j - 1];
		out[j].member = members[i];
		out[j].mark = s->mark;
		out[j].moved = s->moved;
	}
	return k;
}
