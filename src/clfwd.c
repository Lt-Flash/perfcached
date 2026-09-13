/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * clfwd.c - the proxy write plane's wire frames.  See clfwd.h.
 *
 * No literal offsets: a cursor (clcodec.h) puts each field where the
 * previous one ended.  On the read side a single pc_rfits() per header
 * is the bounds check and the unchecked readers run inside it - measured
 * parity with hand-written offsets on parse and faster on build, where
 * per-field checking cost +3.3ns on a frame this size.
 *
 * Each builder hands its cursor the EXACT length the frame will be, the
 * same number clfwd_*_size() computes and the function returns.  Those
 * were two independent computations of one quantity with nothing making
 * them agree; now a disagreement makes the cursor refuse rather than
 * run off the buffer.
 */
#include <string.h>

#include "clcodec.h"
#include "clfwd.h"

size_t clfwd_op_size(unsigned int collen, unsigned int klen,
		unsigned int vlen)
{
	return CLFWD_OP_HDR + collen + klen + vlen;
}

size_t clfwd_op_build(unsigned char *msg, unsigned char type,
		const struct clfwd_op *f)
{
	size_t len = clfwd_op_size(f->collen, f->klen, f->vlen);
	struct pc_wcur c;

	pc_wcur_init(&c, msg, len);
	pc_w8(&c, type);
	pc_w32(&c, f->req);
	pc_w16(&c, (uint16_t)f->node);
	pc_w8(&c, f->op);
	pc_w32(&c, f->ttl_rel);
	pc_w64(&c, (uint64_t)f->delta);        /* low half first */
	pc_w8(&c, (unsigned)f->collen);
	pc_w16(&c, (uint16_t)f->klen);
	pc_wraw(&c, f->col, f->collen);
	pc_wraw(&c, f->key, f->klen);
	if (f->vlen)
		pc_wraw(&c, f->val, f->vlen);
	return c.ok ? len : 0;
}

int clfwd_op_parse(const unsigned char *pt, size_t n, struct clfwd_op *f)
{
	struct pc_rcur c;

	pc_rcur_init(&c, pt, n);
	if (!pc_rskip(&c, 1) || !pc_rfits(&c, CLFWD_OP_HDR - 1))
		return 0;
	f->req = pc_ru32(&c);
	f->node = pc_ru16(&c);
	f->op = (unsigned char)pc_ru8(&c);
	f->ttl_rel = pc_ru32(&c);
	f->delta = (long long)pc_ru64(&c);
	f->collen = pc_ru8(&c);
	f->klen = pc_ru16(&c);
	if (CLFWD_OP_HDR + f->collen + f->klen > n)
		return 0;
	f->col = (const char *)pt + CLFWD_OP_HDR;
	f->key = f->col + f->collen;
	f->val = f->key + f->klen;
	/* whatever remains is the value: none for a delete or an add */
	f->vlen = (unsigned int)(n - CLFWD_OP_HDR - f->collen - f->klen);
	return 1;
}

size_t clfwd_ack_build(unsigned char *msg, unsigned char type,
		const struct clfwd_ack *a)
{
	struct pc_wcur c;

	pc_wcur_init(&c, msg, CLFWD_ACK_LEN);
	pc_w8(&c, type);
	pc_w32(&c, a->req);
	pc_w8(&c, (unsigned)a->ok);
	pc_w64(&c, (uint64_t)a->newval);
	return c.ok ? CLFWD_ACK_LEN : 0;
}

int clfwd_ack_parse(const unsigned char *pt, size_t n, struct clfwd_ack *a)
{
	struct pc_rcur c;

	pc_rcur_init(&c, pt, n);
	if (!pc_rskip(&c, 1) || !pc_rfits(&c, CLFWD_ACK_LEN - 1))
		return 0;
	a->req = pc_ru32(&c);
	a->ok = pc_ru8(&c);
	a->newval = (long long)pc_ru64(&c);
	return 1;
}

/* ---- the ownership-correction frames ----------------------------------- */

/* the col+key tail both frames share: [collen1][klen2][col][key] */
static void key_tail_build(struct pc_wcur *c, const struct clfwd_key *k)
{
	pc_w8(c, (unsigned)k->collen);
	pc_w16(c, (uint16_t)k->klen);
	pc_wraw(c, k->col, k->collen);
	pc_wraw(c, k->key, k->klen);
}

static size_t key_tail_size(const struct clfwd_key *k)
{
	return 3 + k->collen + k->klen;
}

/* the cursor is already past whatever precedes the tail; `hdr` is the
 * whole header length, which is where col and key begin */
static int key_tail_parse(struct pc_rcur *c, const unsigned char *pt,
		size_t n, size_t hdr, struct clfwd_key *k)
{
	if (!pc_rfits(c, 3))
		return 0;
	k->collen = pc_ru8(c);
	k->klen = pc_ru16(c);
	if (hdr + k->collen + k->klen > n)
		return 0;
	k->col = (const char *)pt + hdr;
	k->key = k->col + k->collen;
	return 1;
}

size_t clfwd_demote_build(unsigned char *msg, unsigned char type,
		unsigned int winner, const struct clfwd_key *k)
{
	size_t len = 3 + key_tail_size(k);
	struct pc_wcur c;

	pc_wcur_init(&c, msg, len);
	pc_w8(&c, type);
	pc_w16(&c, (uint16_t)winner);
	key_tail_build(&c, k);
	return c.ok ? len : 0;
}

int clfwd_demote_parse(const unsigned char *pt, size_t n,
		unsigned int *winner, struct clfwd_key *k)
{
	struct pc_rcur c;

	pc_rcur_init(&c, pt, n);
	if (!pc_rskip(&c, 1) || !pc_rfits(&c, 2))
		return 0;
	*winner = pc_ru16(&c);
	return key_tail_parse(&c, pt, n, CLFWD_DEMOTE_HDR, k);
}

size_t clfwd_tomb_build(unsigned char *msg, unsigned char type,
		const struct clfwd_key *k)
{
	size_t len = 1 + key_tail_size(k);
	struct pc_wcur c;

	pc_wcur_init(&c, msg, len);
	pc_w8(&c, type);
	key_tail_build(&c, k);
	return c.ok ? len : 0;
}

int clfwd_tomb_parse(const unsigned char *pt, size_t n, struct clfwd_key *k)
{
	struct pc_rcur c;

	pc_rcur_init(&c, pt, n);
	if (!pc_rskip(&c, 1))
		return 0;
	return key_tail_parse(&c, pt, n, CLFWD_TOMB_HDR, k);
}

/* ---- the JSON forward frames ------------------------------------------- */

size_t clfwd_json_build(unsigned char *msg, unsigned char type,
		const struct clfwd_json *j)
{
	size_t len = CLFWD_JSON_HDR + j->collen + j->klen + j->plen + j->vlen;
	struct pc_wcur c;

	pc_wcur_init(&c, msg, len);
	pc_w8(&c, type);
	pc_w32(&c, j->req);
	pc_w16(&c, (uint16_t)j->node);
	pc_w8(&c, j->jop);
	pc_w8(&c, j->flags);
	pc_w32(&c, j->ttl);
	pc_w64(&c, (uint64_t)j->by);
	pc_w8(&c, (unsigned)j->collen);
	pc_w16(&c, (uint16_t)j->klen);
	pc_w16(&c, (uint16_t)j->plen);
	pc_w32(&c, j->vlen);
	pc_wraw(&c, j->col, j->collen);
	pc_wraw(&c, j->key, j->klen);
	pc_wraw(&c, j->path, j->plen);
	if (j->vlen)
		pc_wraw(&c, j->val, j->vlen);
	return c.ok ? len : 0;
}

int clfwd_json_parse(const unsigned char *pt, size_t n, struct clfwd_json *j)
{
	struct pc_rcur c;

	pc_rcur_init(&c, pt, n);
	if (!pc_rskip(&c, 1) || !pc_rfits(&c, CLFWD_JSON_HDR - 1))
		return 0;
	j->req = pc_ru32(&c);
	j->node = pc_ru16(&c);
	j->jop = (unsigned char)pc_ru8(&c);
	j->flags = (unsigned char)pc_ru8(&c);
	j->ttl = pc_ru32(&c);
	j->by = (long long)pc_ru64(&c);
	j->collen = pc_ru8(&c);
	j->klen = pc_ru16(&c);
	j->plen = pc_ru16(&c);
	j->vlen = pc_ru32(&c);
	if ((size_t)CLFWD_JSON_HDR + j->collen + j->klen + j->plen + j->vlen > n)
		return 0;
	j->col = (const char *)pt + CLFWD_JSON_HDR;
	j->key = j->col + j->collen;
	j->path = j->key + j->klen;
	j->val = j->path + j->plen;
	return 1;
}

size_t clfwd_jack_build(unsigned char *msg, unsigned char type,
		const struct clfwd_jack *a)
{
	/* a fragment only travels with a successful result */
	unsigned int fl = a->st == 0 ? a->fraglen : 0;
	size_t len = CLFWD_JACK_HDR + fl;
	struct pc_wcur c;

	pc_wcur_init(&c, msg, len);
	pc_w8(&c, type);
	pc_w32(&c, a->req);
	pc_w8(&c, a->st);
	pc_w8(&c, a->jop);
	pc_w64(&c, (uint64_t)a->newval);
	pc_w32(&c, a->cnt);
	pc_w32(&c, fl);
	if (fl)
		pc_wraw(&c, a->frag, fl);
	return c.ok ? len : 0;
}

int clfwd_jack_parse(const unsigned char *pt, size_t n, struct clfwd_jack *a)
{
	struct pc_rcur c;

	pc_rcur_init(&c, pt, n);
	if (!pc_rskip(&c, 1) || !pc_rfits(&c, CLFWD_JACK_HDR - 1))
		return 0;
	a->req = pc_ru32(&c);
	a->st = (unsigned char)pc_ru8(&c);
	a->jop = (unsigned char)pc_ru8(&c);
	a->newval = (long long)pc_ru64(&c);
	a->cnt = pc_ru32(&c);
	a->fraglen = pc_ru32(&c);
	if ((size_t)CLFWD_JACK_HDR + a->fraglen > n)
		return 0;
	a->frag = a->fraglen ? (const char *)pt + CLFWD_JACK_HDR : NULL;
	return 1;
}
