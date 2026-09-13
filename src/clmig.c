/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * clmig.c - the migration in-flight window.  See clmig.h.
 */
#include <stdlib.h>
#include <string.h>

#include "clcodec.h"                 /* the shared LE codec - see M10 */
#include "clmig.h"

void clmig_reset(struct clmig *m)
{
	memset(m->out, 0, sizeof m->out);
}

int clmig_track(struct clmig *m, uint32_t req, uint16_t recs, size_t off,
		long long now)
{
	size_t j;

	for (j = 0; j < sizeof m->out / sizeof m->out[0]; j++) {
		if (m->out[j].req)
			continue;
		m->out[j].req = req;
		m->out[j].recs = recs;
		m->out[j].off = off;
		m->out[j].sent_ms = now;
		m->out[j].retx = 0;
		return 1;
	}
	return 0;
}

uint16_t clmig_ack(struct clmig *m, uint32_t req)
{
	size_t j;

	/* req 0 marks a free slot, so it can never name a real datagram;
	 * without this an ack carrying 0 would retire the first free slot
	 * and report its (zero) records as delivered. */
	if (!req)
		return 0;
	for (j = 0; j < sizeof m->out / sizeof m->out[0]; j++) {
		if (m->out[j].req != req)
			continue;
		{
			uint16_t recs = m->out[j].recs;

			m->out[j].req = 0;
			m->out[j].recs = 0;
			return recs;
		}
	}
	return 0;
}

int clmig_retransmit(struct clmig *m, long long now, clmig_resend_fn f,
		void *ctx)
{
	size_t j;
	int n = 0;

	for (j = 0; j < sizeof m->out / sizeof m->out[0]; j++) {
		if (!m->out[j].req || m->out[j].retx >= CLMIG_RETX ||
		        now - m->out[j].sent_ms < CLMIG_RETX_MS)
			continue;
		f(ctx, m->out[j].off);
		m->out[j].sent_ms = now;
		m->out[j].retx++;
		m->retx_total++;
		n++;
	}
	return n;
}

unsigned long clmig_lost_records(const struct clmig *m)
{
	unsigned long lost = 0;
	size_t j;

	for (j = 0; j < sizeof m->out / sizeof m->out[0]; j++)
		if (m->out[j].req)
			lost += m->out[j].recs;
	return lost;
}

int clmig_inflight(const struct clmig *m)
{
	int n = 0;
	size_t j;

	for (j = 0; j < sizeof m->out / sizeof m->out[0]; j++)
		if (m->out[j].req)
			n++;
	return n;
}

/* ---- the gather buffer ------------------------------------------------ */

int clmig_buf_init(struct clmig_buf *g, size_t cap)
{
	g->b = malloc(cap);
	if (!g->b)
		return -1;
	g->cap = cap;
	g->len = 0;
	g->open = CLMIG_NO_GROUP;
	return 0;
}

void clmig_buf_free(struct clmig_buf *g)
{
	free(g->b);
	g->b = NULL;
	g->cap = g->len = 0;
	g->open = CLMIG_NO_GROUP;
}

void clmig_buf_reset(struct clmig_buf *g)
{
	g->len = 0;
	g->open = CLMIG_NO_GROUP;
}

int clmig_open_for(struct clmig_buf *g, size_t need, size_t ghdr,
		size_t gather_cap, unsigned char **hdr)
{
	if (g->open != CLMIG_NO_GROUP) {
		/* close a group one more record would overfill */
		if (pc_g16(g->b + g->open) + need > gather_cap)
			g->open = CLMIG_NO_GROUP;
	}
	if (g->open == CLMIG_NO_GROUP) {
		if (g->len + 2 + ghdr + need > g->cap)
			return -1;
		g->open = g->len;
		pc_p16(g->b + g->open, (uint16_t)ghdr);
		g->len += 2 + ghdr;
		if (hdr)
			*hdr = g->b + g->open + 2;
		return 1;
	}
	if (g->len + need > g->cap)
		return -1;
	if (hdr)
		*hdr = g->b + g->open + 2;
	return 0;
}

unsigned char *clmig_record_at(struct clmig_buf *g)
{
	return g->b + g->len;
}

void clmig_record_done(struct clmig_buf *g, size_t n, size_t cnt_off)
{
	unsigned char *gh = g->b + g->open;

	g->len += n;
	pc_p16(gh, (uint16_t)(pc_g16(gh) + n));                 /* group length */
	pc_p16(gh + 2 + cnt_off, (uint16_t)(pc_g16(gh + 2 + cnt_off) + 1));   /* record count */
}

int clmig_group_walk(const struct clmig_buf *g, size_t *off,
		const unsigned char **p, unsigned int *n)
{
	unsigned int glen;

	if (*off + 2 > g->len)
		return 0;
	glen = pc_g16(g->b + *off);
	*p = g->b + *off + 2;
	*n = glen;
	*off += 2 + glen;
	return 1;
}

/* ---- the record codec -------------------------------------------------- */

size_t clmig_rec_size(unsigned int collen, unsigned int klen,
		unsigned int vlen)
{
	return CLMIG_RHDR + collen + klen + vlen;
}

void clmig_rec_write(unsigned char *p, const struct clmig_rec *r)
{
	size_t body = (size_t)r->collen + r->klen + r->vlen;
	struct pc_wcur c;

	/* the header has ONE definition, and this is not it */
	clmig_rec_hdr_write(p, r);
	pc_wcur_init(&c, p + CLMIG_RHDR, body);
	pc_wraw(&c, r->col, r->collen);
	pc_wraw(&c, r->key, r->klen);
	pc_wraw(&c, r->val, r->vlen);
}

int clmig_rec_parse(const unsigned char *p, size_t n, size_t *off,
		struct clmig_rec *r)
{
	size_t o = *off;

	struct pc_rcur c;

	pc_rcur_init(&c, p + o, n > o ? n - o : 0);
	if (!pc_rfits(&c, CLMIG_RHDR))
		return 0;
	clmig_rec_hdr_parse(p + o, r);
	/* truncated: stop the walk rather than read past the datagram */
	if (o + CLMIG_RHDR + r->collen + r->klen + r->vlen > n)
		return 0;
	r->col = (const char *)p + o + CLMIG_RHDR;
	r->key = r->col + r->collen;
	r->val = r->key + r->klen;
	*off = o + CLMIG_RHDR + r->collen + r->klen + r->vlen;
	return 1;
}

uint32_t clmig_group_req(const unsigned char *g)
{
	return pc_g32(g + 1);
}

unsigned int clmig_group_count(const unsigned char *g)
{
	return pc_g16(g + 7);
}

/* ---- the frames the records travel in.  See clmig.h. -------------- */

size_t clmig_group_hdr(unsigned char *buf, size_t cap, unsigned char type,
		uint32_t req, int node)
{
	struct pc_wcur c;

	pc_wcur_init(&c, buf, cap);
	pc_w8(&c, type);
	pc_w32(&c, req);
	pc_w16(&c, (uint16_t)node);
	pc_w16(&c, 0);                     /* count: patched as records land */
	return c.ok ? pc_wcur_len(&c, buf) : 0;
}

size_t clmig_single_size(unsigned int collen, unsigned int klen,
		unsigned int vlen)
{
	return CLMIG_SHDR + collen + klen + vlen;
}

size_t clmig_single_write(unsigned char *buf, size_t cap, unsigned char type,
		const struct clmig_single *s)
{
	struct pc_wcur c;

	pc_wcur_init(&c, buf, cap);
	pc_w8(&c, type);
	pc_w32(&c, s->req);
	pc_w16(&c, (uint16_t)s->node);
	pc_w32(&c, s->ttl_left);
	pc_w8(&c, (unsigned)s->collen);
	pc_w16(&c, (uint16_t)s->klen);
	pc_wraw(&c, s->col, s->collen);
	pc_wraw(&c, s->key, s->klen);
	if (s->vlen)
		pc_wraw(&c, s->val, s->vlen);
	return c.ok ? pc_wcur_len(&c, buf) : 0;
}

int clmig_single_parse(const unsigned char *p, size_t n,
		struct clmig_single *s)
{
	struct pc_rcur c;

	pc_rcur_init(&c, p, n);
	if (!pc_rskip(&c, 1) || !pc_rfits(&c, CLMIG_SHDR - 1))
		return 0;
	s->req = pc_ru32(&c);
	s->node = pc_ru16(&c);
	s->ttl_left = pc_ru32(&c);
	s->collen = pc_ru8(&c);
	s->klen = pc_ru16(&c);
	if (CLMIG_SHDR + s->collen + s->klen > n)
		return 0;
	s->col = (const char *)p + CLMIG_SHDR;
	s->key = s->col + s->collen;
	s->val = s->key + s->klen;
	s->vlen = (unsigned int)(n - CLMIG_SHDR - s->collen - s->klen);
	return 1;
}

size_t clmig_ack_write(unsigned char *buf, size_t cap, unsigned char type,
		uint32_t req, int ok, const unsigned int *stored)
{
	struct pc_wcur c;

	pc_wcur_init(&c, buf, cap);
	pc_w8(&c, type);
	pc_w32(&c, req);
	pc_w8(&c, (unsigned)ok);
	if (stored)
		pc_w16(&c, (uint16_t)*stored);
	return c.ok ? pc_wcur_len(&c, buf) : 0;
}

int clmig_ack_parse(const unsigned char *p, size_t n, uint32_t *req,
		int *ok, unsigned int *stored)
{
	struct pc_rcur c;

	pc_rcur_init(&c, p, n);
	if (!pc_rskip(&c, 1) || !pc_rfits(&c, 4 + 1))
		return 0;
	*req = pc_ru32(&c);
	*ok = pc_ru8(&c);
	/* the count is an additive tail: an older peer sends six bytes */
	*stored = pc_rfits(&c, 2) ? (unsigned int)pc_ru16(&c) : 0;
	return 1;
}

void clmig_rec_hdr_write(unsigned char *p, const struct clmig_rec *r)
{
	struct pc_wcur c;

	pc_wcur_init(&c, p, CLMIG_RHDR);
	pc_w32(&c, r->ttl_left);
	pc_w8(&c, (unsigned)r->collen);
	pc_w16(&c, (uint16_t)r->klen);
	pc_w32(&c, r->vlen);
	pc_w64(&c, r->ver);
}

void clmig_rec_hdr_parse(const unsigned char *p, struct clmig_rec *r)
{
	struct pc_rcur c;

	pc_rcur_init(&c, p, CLMIG_RHDR);
	r->ttl_left = pc_ru32(&c);
	r->collen = pc_ru8(&c);
	r->klen = pc_ru16(&c);
	r->vlen = pc_ru32(&c);
	r->ver = pc_ru64(&c);
}
