/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clcol.c - the collection-announce frame.  See clcol.h.
 */
#include <string.h>

#include "clcol.h"
#include "clcodec.h"

size_t clcol_size(unsigned int nlen, unsigned int tlen)
{
	return CLCOL_HDR + nlen + (tlen ? 1 + tlen : 0);
}

size_t clcol_write(unsigned char *buf, size_t cap, unsigned char type,
		const struct clcol_ann *a)
{
	unsigned int tlen = a->to ? a->tlen : 0;
	struct pc_wcur c;

	pc_wcur_init(&c, buf, cap);
	pc_w8(&c, type);
	pc_w8(&c, (unsigned)a->op);
	pc_w8(&c, (unsigned)a->buckets_log2);
	pc_w64(&c, a->gen);
	pc_w8(&c, a->nlen);
	pc_wraw(&c, a->name, a->nlen);
	if (tlen) {
		pc_w8(&c, tlen);
		pc_wraw(&c, a->to, tlen);
	}
	return c.ok ? pc_wcur_len(&c, buf) : 0;
}

int clcol_parse(const unsigned char *p, size_t n, struct clcol_ann *a)
{
	struct pc_rcur c;

	pc_rcur_init(&c, p, n);
	if (!pc_rskip(&c, 1) || !pc_rfits(&c, CLCOL_HDR - 1))
		return 0;
	memset(a, 0, sizeof *a);
	a->op = pc_ru8(&c);
	a->buckets_log2 = pc_ru8(&c);
	a->gen = pc_ru64(&c);
	a->nlen = pc_ru8(&c);
	if (!a->nlen || !pc_rfits(&c, a->nlen))
		return 0;                  /* a frame that names nothing */
	a->name = (const char *)pc_rubytes(&c, a->nlen);
	if (a->op != CLCOL_OP_RENAME)
		return 1;
	/* a rename is only whole once its SECOND name is there too */
	if (!pc_rfits(&c, 1))
		return 0;
	a->tlen = pc_ru8(&c);
	if (!a->tlen || !pc_rfits(&c, a->tlen))
		return 0;
	a->to = (const char *)pc_rubytes(&c, a->tlen);
	return 1;
}
