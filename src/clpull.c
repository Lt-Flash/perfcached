/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clpull.c - the PULL plane's wire frames.  See clpull.h.
 */
#include <string.h>

#include "clpull.h"
#include "clcodec.h"

size_t clpull_req_size(unsigned int collen, unsigned int klen)
{
	return CLPULL_QHDR + collen + klen;
}

size_t clpull_req_write(unsigned char *buf, size_t cap, unsigned char type,
		const struct clpull_req *q)
{
	struct pc_wcur c;

	pc_wcur_init(&c, buf, cap);
	pc_w8(&c, type);
	pc_w32(&c, q->req);
	pc_w16(&c, (uint16_t)q->node);
	pc_w8(&c, (unsigned)q->collen);
	pc_w16(&c, (uint16_t)q->klen);
	pc_wraw(&c, q->col, q->collen);
	pc_wraw(&c, q->key, q->klen);
	return c.ok ? pc_wcur_len(&c, buf) : 0;
}

int clpull_req_parse(const unsigned char *p, size_t n, struct clpull_req *q)
{
	struct pc_rcur c;

	pc_rcur_init(&c, p, n);
	if (!pc_rskip(&c, 1) || !pc_rfits(&c, CLPULL_QHDR - 1))
		return 0;
	q->req = pc_ru32(&c);
	q->node = pc_ru16(&c);
	q->collen = pc_ru8(&c);
	q->klen = pc_ru16(&c);
	if (CLPULL_QHDR + q->collen + q->klen > n)
		return 0;
	q->col = (const char *)p + CLPULL_QHDR;
	q->key = q->col + q->collen;
	return 1;
}

size_t clpull_rsp_write(unsigned char *buf, size_t cap, unsigned char type,
		const struct clpull_rsp *r)
{
	struct pc_wcur c;

	pc_wcur_init(&c, buf, cap);
	pc_w8(&c, type);
	pc_w32(&c, r->req);
	pc_w16(&c, (uint16_t)r->node);
	pc_w8(&c, (unsigned)r->found);
	/* ttl_left is written as given even when found is 0 - see clpull.h */
	pc_w32(&c, r->ttl_left);
	pc_w32(&c, r->vlen);
	pc_w64(&c, r->ver);
	if (r->found && r->vlen)
		pc_wraw(&c, r->val, r->vlen);
	return c.ok ? pc_wcur_len(&c, buf) : 0;
}

int clpull_rsp_parse(const unsigned char *p, size_t n, struct clpull_rsp *r)
{
	struct pc_rcur c;

	pc_rcur_init(&c, p, n);
	if (!pc_rskip(&c, 1) || !pc_rfits(&c, CLPULL_RHDR - 1))
		return 0;
	r->req = pc_ru32(&c);
	r->node = pc_ru16(&c);
	r->found = pc_ru8(&c);
	r->ttl_left = pc_ru32(&c);
	r->vlen = pc_ru32(&c);
	r->ver = pc_ru64(&c);
	r->val = (const char *)p + CLPULL_RHDR;
	return 1;
}
