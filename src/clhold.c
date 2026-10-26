/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clhold.c - S394's hold-check frames: the codec (clhold.h).  The frame
 * layout and its bounds as cluster.c had them inline, moved here (the 0.7.2
 * split, wave 1); test/clholdtest.c.
 */
#include <string.h>

#include "clhold.h"
#include "clmsg.h"                     /* M_HOLD_Q, M_HOLD_A */

static void hq_put16(unsigned char *p, unsigned int v)
{
	p[0] = (unsigned char)(v >> 8);
	p[1] = (unsigned char)v;
}

static void hq_put32(unsigned char *p, uint32_t v)
{
	hq_put16(p, v >> 16);
	hq_put16(p + 2, v & 0xFFFFu);
}

static void hq_put64(unsigned char *p, unsigned long long v)
{
	hq_put32(p, (uint32_t)(v >> 32));
	hq_put32(p + 4, (uint32_t)v);
}

static unsigned int hq_get16(const unsigned char *p)
{
	return (unsigned int)p[0] << 8 | p[1];
}

static uint32_t hq_get32(const unsigned char *p)
{
	return (uint32_t)hq_get16(p) << 16 | hq_get16(p + 2);
}

static unsigned long long hq_get64(const unsigned char *p)
{
	return (unsigned long long)hq_get32(p) << 32 | hq_get32(p + 4);
}

int clhold_q_begin(struct clhold_qw *q, unsigned char *buf, size_t cap,
		unsigned int node, const char *col, size_t collen)
{
	if (collen > 255 || 4 + collen + 2 > cap)
		return -1;
	q->buf = buf;
	q->cap = cap;
	buf[0] = M_HOLD_Q;
	hq_put16(buf + 1, node);
	buf[3] = (unsigned char)collen;
	memcpy(buf + 4, col, collen);
	q->nat = 4 + collen;
	q->len = q->nat + 2;
	q->n = 0;
	return 0;
}

int clhold_q_fits(const struct clhold_qw *q, size_t klen)
{
	return q->n < CLHOLD_MAXN && klen <= 0xFFFF &&
		q->len + 6 + klen <= q->cap;
}

int clhold_q_add(struct clhold_qw *q, uint32_t req, const char *key,
		size_t klen)
{
	if (!clhold_q_fits(q, klen))
		return -1;
	hq_put32(q->buf + q->len, req);
	hq_put16(q->buf + q->len + 4, (unsigned int)klen);
	memcpy(q->buf + q->len + 6, key, klen);
	q->len += 6 + klen;
	q->n++;
	return 0;
}

size_t clhold_q_end(struct clhold_qw *q)
{
	hq_put16(q->buf + q->nat, q->n);
	return q->len;
}

int clhold_q_is_col(const struct clhold_qw *q, const char *col, size_t collen)
{
	return q->buf[3] == collen && !memcmp(q->buf + 4, col, collen);
}

int clhold_q_open(struct clhold_qr *r, const unsigned char *p, size_t n)
{
	size_t collen;

	if (n < 6)
		return -1;
	collen = p[3];
	if (6 + collen > n)
		return -1;
	r->p = p;
	r->n = n;
	r->node = hq_get16(p + 1);
	r->col = (const char *)p + 4;
	r->collen = collen;
	r->cnt = hq_get16(p + 4 + collen);
	r->at = 6 + collen;
	r->i = 0;
	return 0;
}

int clhold_q_next(struct clhold_qr *r, uint32_t *req, const char **key,
		size_t *klen)
{
	size_t kl;

	if (r->i >= r->cnt || r->at + 6 > r->n)
		return 0;
	kl = hq_get16(r->p + r->at + 4);
	if (r->at + 6 + kl > r->n)
		return 0;
	*req = hq_get32(r->p + r->at);
	*key = (const char *)r->p + r->at + 6;
	*klen = kl;
	r->at += 6 + kl;
	r->i++;
	return 1;
}

size_t clhold_a_begin(unsigned char *buf, unsigned int node)
{
	buf[0] = M_HOLD_A;
	hq_put16(buf + 1, node);
	hq_put16(buf + 3, 0);
	return CLHOLD_A_HDR;
}

size_t clhold_a_put(unsigned char *buf, size_t at, uint32_t req, int held,
		unsigned long long ver)
{
	hq_put32(buf + at, req);
	buf[at + 4] = (unsigned char)(held != 0);
	hq_put64(buf + at + 5, held ? ver : 0);
	return at + CLHOLD_A_ENTRY;
}

void clhold_a_end(unsigned char *buf, unsigned int n)
{
	hq_put16(buf + 3, n);
}

int clhold_a_open(const unsigned char *p, size_t n, unsigned int *node,
		unsigned int *cnt)
{
	if (n < CLHOLD_A_HDR)
		return -1;
	*node = hq_get16(p + 1);
	*cnt = hq_get16(p + 3);
	return 0;
}

int clhold_a_get(const unsigned char *p, size_t n, unsigned int i,
		uint32_t *req, int *held, unsigned long long *ver)
{
	size_t at = CLHOLD_A_HDR + (size_t)i * CLHOLD_A_ENTRY;

	if (at + CLHOLD_A_ENTRY > n)
		return 0;
	*req = hq_get32(p + at);
	*held = p[at + 4] != 0;
	*ver = hq_get64(p + at + 5);
	return 1;
}
