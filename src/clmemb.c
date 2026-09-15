/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clmemb.c - the membership keepalive frames.  See clmemb.h.
 *
 * There are no offsets here at all.  A cursor (clcodec.h) puts each
 * field where the previous one ended, so build and parse cannot drift
 * apart, and on the read side "did it fit" IS the length gate.
 */
#include <string.h>

#include "clmemb.h"
#include "clcodec.h"

/* Positions come from the cursor, not from arithmetic: each field is
 * where the one before it ended.  On the read side "did it fit" IS the
 * length gate, and because these frames are ADDITIVE - truncated at some
 * point, everything past that absent - the FIRST group that does not fit
 * ends the parse.  That last part is load-bearing: carrying on after a
 * missing group would read the next field out of the previous field's
 * bytes. */

/* The MINIMUM is peeked as one group too.  Reading it field by field
 * and bailing on the first failure would leave the earlier fields
 * already written into *out on a frame too short to be valid at all -
 * a half-filled struct behind a "no" return.  Peek, then read. */

/* ---- ALIVE, in frame order --------------------------------------- */
size_t clmemb_alive_write(unsigned char *buf, size_t cap, unsigned char type,
		const struct clmemb_alive *in)
{
	struct pc_wcur c;

	pc_wcur_init(&c, buf, cap);
	pc_w8(&c, type);
	pc_w16(&c, (uint16_t)in->node);
	pc_w32(&c, in->free_mb);
	pc_w32(&c, in->total_mb);
	pc_w32(&c, in->live_kb);
	pc_w8(&c, (unsigned)in->mode);
	pc_w8(&c, (unsigned)in->eager);
	pc_w64(&c, in->cfg_digest);
	pc_w16(&c, (uint16_t)in->client_port);
	pc_wraw(&c, in->ident, CLMEMB_IDENT_LEN);
	pc_w32(&c, in->incarn);
	pc_w32(&c, in->entries);
	pc_w64(&c, in->lamport);
	pc_w8(&c, (unsigned)in->nstate);
	pc_w8(&c, (unsigned)in->mem_tier);
	pc_w16(&c, (uint16_t)in->resp_port);
	pc_w8(&c, (unsigned)in->start_kind);
	pc_w16(&c, (uint16_t)in->http_port);
	pc_w32(&c, in->uptime_s);
	return c.ok ? pc_wcur_len(&c, buf) : 0;
}

int clmemb_alive_parse(const unsigned char *p, size_t n,
		struct clmemb_alive *out)
{
	struct pc_rcur c;

	pc_rcur_init(&c, p, n);
	if (!pc_rskip(&c, 1) || !pc_rfits(&c, 2 + 4))
		return 0;         /* below the minimum: *out is NOT touched */
	memset(out, 0, sizeof *out);
	out->node = pc_ru16(&c);
	out->free_mb = pc_ru32(&c);

	if (!pc_rfits(&c, 4 + 4))
		return 1;
	out->total_mb = pc_ru32(&c);
	out->live_kb = pc_ru32(&c);
	out->have |= CLMEMB_A_MEM;

	if (!pc_rfits(&c, 1 + 1 + 8))
		return 1;
	out->mode = pc_ru8(&c);
	out->eager = pc_ru8(&c);
	out->cfg_digest = pc_ru64(&c);
	out->have |= CLMEMB_A_CFG;

	if (!pc_rfits(&c, 2))
		return 1;
	out->client_port = pc_ru16(&c);
	out->have |= CLMEMB_A_CLIPORT;

	if (!pc_rfits(&c, CLMEMB_IDENT_LEN + 4))
		return 1;
	out->ident = pc_rubytes(&c, CLMEMB_IDENT_LEN);
	out->incarn = pc_ru32(&c);
	out->have |= CLMEMB_A_IDENT;

	if (!pc_rfits(&c, 4))
		return 1;
	out->entries = pc_ru32(&c);
	out->have |= CLMEMB_A_ENTRIES;

	if (!pc_rfits(&c, 8))
		return 1;
	out->lamport = pc_ru64(&c);
	out->have |= CLMEMB_A_LAMPORT;

	if (!pc_rfits(&c, 1))
		return 1;
	out->nstate = pc_ru8(&c);
	out->have |= CLMEMB_A_STATE;

	if (!pc_rfits(&c, 1))
		return 1;
	out->mem_tier = pc_ru8(&c);
	out->have |= CLMEMB_A_TIER;

	if (!pc_rfits(&c, 2))
		return 1;
	out->resp_port = pc_ru16(&c);
	out->have |= CLMEMB_A_RESPPORT;

	if (!pc_rfits(&c, 1))
		return 1;
	out->start_kind = pc_ru8(&c);
	out->have |= CLMEMB_A_STARTKIND;

	if (!pc_rfits(&c, 2 + 4))
		return 1;
	out->http_port = pc_ru16(&c);
	out->uptime_s = pc_ru32(&c);
	out->have |= CLMEMB_A_HTTP;
	return 1;
}

/* ---- MASTER_ALIVE ------------------------------------------------- */
size_t clmemb_malive_write(unsigned char *buf, size_t cap, unsigned char type,
		const struct clmemb_malive *in)
{
	struct pc_wcur c;

	pc_wcur_init(&c, buf, cap);
	pc_w8(&c, type);
	pc_w16(&c, (uint16_t)in->node);
	pc_w16(&c, (uint16_t)in->members);
	pc_w64(&c, in->member_digest);
	pc_w32(&c, in->free_mb);
	pc_w32(&c, in->total_mb);
	pc_w32(&c, in->live_kb);
	pc_w8(&c, (unsigned)in->mode);
	pc_w8(&c, (unsigned)in->eager);
	pc_w64(&c, in->cfg_digest);
	pc_w32(&c, in->term);
	return c.ok ? pc_wcur_len(&c, buf) : 0;
}

int clmemb_malive_parse(const unsigned char *p, size_t n,
		struct clmemb_malive *out)
{
	struct pc_rcur c;

	pc_rcur_init(&c, p, n);
	if (!pc_rskip(&c, 1) || !pc_rfits(&c, 2 + 2 + 8 + 4))
		return 0;         /* below the minimum: *out is NOT touched */
	memset(out, 0, sizeof *out);
	out->node = pc_ru16(&c);
	out->members = pc_ru16(&c);
	out->member_digest = pc_ru64(&c);
	out->free_mb = pc_ru32(&c);

	if (!pc_rfits(&c, 4 + 4))
		return 1;
	out->total_mb = pc_ru32(&c);
	out->live_kb = pc_ru32(&c);
	out->have |= CLMEMB_M_MEM;

	if (!pc_rfits(&c, 1 + 1 + 8))
		return 1;
	out->mode = pc_ru8(&c);
	out->eager = pc_ru8(&c);
	out->cfg_digest = pc_ru64(&c);
	out->have |= CLMEMB_M_CFG;

	if (!pc_rfits(&c, 4))
		return 1;
	out->term = pc_ru32(&c);
	out->have |= CLMEMB_M_TERM;
	return 1;
}

/* ---- JOIN_REQ ------------------------------------------------------ */
size_t clmemb_joinreq_write(unsigned char *buf, size_t cap, unsigned char type,
		const struct clmemb_joinreq *in)
{
	struct pc_wcur c;

	pc_wcur_init(&c, buf, cap);
	pc_w8(&c, type);
	pc_w64(&c, in->tok);
	pc_w8(&c, (unsigned)in->mode);
	pc_w8(&c, (unsigned)in->eager);
	pc_w64(&c, in->cfg_digest);
	pc_wraw(&c, in->ident, CLMEMB_IDENT_LEN);
	pc_w16(&c, (uint16_t)in->proposed);
	pc_w32(&c, in->incarn);
	pc_w8(&c, (unsigned)in->wal);
	return c.ok ? pc_wcur_len(&c, buf) : 0;
}

int clmemb_joinreq_parse(const unsigned char *p, size_t n,
		struct clmemb_joinreq *out)
{
	struct pc_rcur c;

	pc_rcur_init(&c, p, n);
	if (!pc_rskip(&c, 1) || !pc_rfits(&c, 8))
		return 0;         /* below the minimum: *out is NOT touched */
	memset(out, 0, sizeof *out);
	out->tok = pc_ru64(&c);

	if (!pc_rfits(&c, 1 + 1 + 8))
		return 1;
	out->mode = pc_ru8(&c);
	out->eager = pc_ru8(&c);
	out->cfg_digest = pc_ru64(&c);
	out->have |= CLMEMB_J_CFG;

	if (!pc_rfits(&c, CLMEMB_IDENT_LEN))
		return 1;
	out->ident = pc_rubytes(&c, CLMEMB_IDENT_LEN);
	out->have |= CLMEMB_J_IDENT;

	if (!pc_rfits(&c, 2))
		return 1;
	out->proposed = pc_ru16(&c);
	out->have |= CLMEMB_J_PROP;

	if (!pc_rfits(&c, 4))
		return 1;
	out->incarn = pc_ru32(&c);
	out->have |= CLMEMB_J_INCARN;

	if (!pc_rfits(&c, 1))
		return 1;
	out->wal = pc_ru8(&c);
	out->have |= CLMEMB_J_WAL;
	return 1;
}

/* ---- ASSIGN -------------------------------------------------------
 *
 * The records are an ARRAY, so they are reached by index rather than by
 * walking - a cursor per record, positioned once. */
size_t clmemb_assign_hdr_write(unsigned char *buf, size_t cap,
		unsigned char type, const struct clmemb_assign *in)
{
	struct pc_wcur c;

	pc_wcur_init(&c, buf, cap);
	pc_w8(&c, type);
	pc_w64(&c, in->tok);
	pc_w16(&c, (uint16_t)in->your_id);
	pc_w16(&c, (uint16_t)in->master_id);
	pc_w16(&c, (uint16_t)in->count);
	return c.ok ? pc_wcur_len(&c, buf) : 0;
}

void clmemb_assign_count_set(unsigned char *buf, int count)
{
	pc_p16(buf + CLMEMB_ASSIGN_HDR - 2, (uint16_t)count);
}

size_t clmemb_member_write(unsigned char *buf, size_t cap,
		const struct clmemb_member *m)
{
	struct pc_wcur c;

	pc_wcur_init(&c, buf, cap);
	pc_w16(&c, (uint16_t)m->node);
	/* verbatim, NOT little-endian: these came off a sockaddr */
	pc_wraw(&c, &m->addr.s_addr, 4);
	pc_wraw(&c, &m->port, 2);
	pc_w32(&c, m->free_mb);
	pc_w32(&c, m->total_mb);
	pc_w32(&c, m->live_kb);
	return c.ok ? pc_wcur_len(&c, buf) : 0;
}

int clmemb_assign_parse(const unsigned char *p, size_t n,
		struct clmemb_assign *out)
{
	struct pc_rcur c;

	pc_rcur_init(&c, p, n);
	if (!pc_rskip(&c, 1) || !pc_rfits(&c, 8 + 2 + 2 + 2))
		return 0;
	out->tok = pc_ru64(&c);
	out->your_id = pc_ru16(&c);
	out->master_id = pc_ru16(&c);
	out->count = pc_ru16(&c);
	return 1;
}

int clmemb_member_at(const unsigned char *p, size_t n, int i,
		struct clmemb_member *out)
{
	struct pc_rcur c;
	size_t off = (size_t)CLMEMB_ASSIGN_HDR + (size_t)i * CLMEMB_ASSIGN_REC;

	/* bounded by the REAL length, never by the header's count: written
	 * this way round so the offset cannot overflow past n first */
	if (i < 0 || off > n || n - off < CLMEMB_ASSIGN_REC)
		return 0;
	pc_rcur_init(&c, p + off, CLMEMB_ASSIGN_REC);
	memset(out, 0, sizeof *out);
	out->node = pc_ru16(&c);
	pc_ruraw(&c, &out->addr.s_addr, 4);
	pc_ruraw(&c, &out->port, 2);
	out->free_mb = pc_ru32(&c);
	out->total_mb = pc_ru32(&c);
	out->live_kb = pc_ru32(&c);
	return 1;
}

/* ---- JOIN_WAIT, JOIN_REJ, GOODBYE --------------------------------- */
size_t clmemb_tok_write(unsigned char *buf, size_t cap, unsigned char type,
		uint64_t tok)
{
	struct pc_wcur c;

	pc_wcur_init(&c, buf, cap);
	pc_w8(&c, type);
	pc_w64(&c, tok);
	return c.ok ? pc_wcur_len(&c, buf) : 0;
}

size_t clmemb_rej_write(unsigned char *buf, size_t cap, unsigned char type,
		uint64_t tok, int reason)
{
	struct pc_wcur c;

	pc_wcur_init(&c, buf, cap);
	pc_w8(&c, type);
	pc_w64(&c, tok);
	pc_w8(&c, (unsigned)reason);
	return c.ok ? pc_wcur_len(&c, buf) : 0;
}

size_t clmemb_goodbye_write(unsigned char *buf, size_t cap, unsigned char type,
		int node)
{
	struct pc_wcur c;

	pc_wcur_init(&c, buf, cap);
	pc_w8(&c, type);
	pc_w16(&c, (uint16_t)node);
	return c.ok ? pc_wcur_len(&c, buf) : 0;
}

int clmemb_tok_parse(const unsigned char *p, size_t n, uint64_t *tok)
{
	struct pc_rcur c;

	pc_rcur_init(&c, p, n);
	if (!pc_rskip(&c, 1) || !pc_rfits(&c, 8))
		return 0;
	*tok = pc_ru64(&c);
	return 1;
}

int clmemb_rej_parse(const unsigned char *p, size_t n, uint64_t *tok,
		int *reason, int *have_reason)
{
	struct pc_rcur c;

	pc_rcur_init(&c, p, n);
	if (!pc_rskip(&c, 1) || !pc_rfits(&c, 8))
		return 0;
	*tok = pc_ru64(&c);
	*have_reason = pc_rfits(&c, 1);
	*reason = *have_reason ? pc_ru8(&c) : 0;
	return 1;
}
