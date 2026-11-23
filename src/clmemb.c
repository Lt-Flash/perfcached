/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clmemb.c - the membership frames.  See clmemb.h. */
#include <string.h>

#include "clmemb.h"
#include "clcodec.h"

/* The cursor puts each field where the last ended; the FIRST group that does not
 * fit ends the parse, or the next field would be read from the previous one's bytes. */

/* The minimum is peeked as one group, so a frame too short to be valid leaves
 * *out untouched rather than half filled. */

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
	pc_w32(&c, in->cl_open);
	pc_w32(&c, in->cl_resp);
	pc_w32(&c, in->cl_bin);
	pc_w32(&c, in->cl_nresp);
	{
		int i, n = in->ncols < 0 ? 0 : in->ncols > CLMEMB_COLS_BEAT
			? CLMEMB_COLS_BEAT : in->ncols;

		pc_w8(&c, (unsigned)n);
		pc_w8(&c, (unsigned)(in->ncols_total > 255 ? 255 : in->ncols_total));
		for (i = 0; i < n; i++) {
			pc_w64(&c, in->cols[i].hash);
			pc_w64(&c, in->cols[i].entries);
			pc_w64(&c, in->cols[i].hits);
			pc_w64(&c, in->cols[i].misses);
			pc_w64(&c, in->cols[i].stores);
			pc_w64(&c, in->cols[i].removes);
			pc_w64(&c, in->cols[i].expired);
		}
	}
	pc_w16(&c, (uint16_t)in->ps_port);           /* after the block */
	pc_w64(&c, in->ps_interest);
	pc_w8(&c, (unsigned)in->nreason);
	pc_w8(&c, (unsigned)in->epoch);
	pc_w8(&c, (unsigned)in->features);
	pc_w16(&c, (uint16_t)in->sole_witness);
	if (in->ncfg > 0 || in->ext) {
		int i, n = in->ncfg < 0 ? 0 : in->ncfg > CLMEMB_CFG_MAX
			? CLMEMB_CFG_MAX : in->ncfg;

		pc_w8(&c, (unsigned)n);         /* 0 ahead of the watermarks group */
		for (i = 0; i < n; i++) {
			pc_w32(&c, in->cfg_name[i]);
			pc_w32(&c, in->cfg_val[i]);
		}
	}
	if (in->ext) {                          /* the watermarks group, last */
		int i, n = in->nmarks < 0 ? 0 : in->nmarks > CLMEMB_MARKS_MAX
			? CLMEMB_MARKS_MAX : in->nmarks;

		pc_w32(&c, in->features32);
		pc_w8(&c, (unsigned)n);
		pc_w16(&c, (uint16_t)(in->nspaces < n ? n : in->nspaces > 0xffff
			? 0xffff : in->nspaces));
		for (i = 0; i < n; i++) {
			pc_w64(&c, in->marks[i].oid);
			pc_w32(&c, in->marks[i].incarn);
			pc_w64(&c, in->marks[i].mark);
		}
		pc_w8(&c, in->link_pct < 0 || in->link_pct > 100 ? 255u
			: (unsigned)in->link_pct);
		pc_w8(&c, (unsigned)in->link_level);
		if (in->cfg2)
			pc_w64(&c, in->cfg_digest2);
	}
	return c.ok ? pc_wcur_len(&c, buf) : 0;
}

int clmemb_alive_patch_state(unsigned char *buf, size_t len, int nstate,
		int nreason)
{
	size_t off;

	/* the shortest frame the writer makes: up to the collection counts, no collections,
	 * then ps_port(2), ps_interest(8), reason, epoch, features, sole witness */
	if (len < 88 + 15)                      /* = CLMEMB_ALIVE_LEN with none */
		return -1;
	/* the reason sits after the collection block (its count at 86) and the relay
	 * fields - counted from the FRONT, as the settings group behind has its own length */
	off = 88 + 56 * (size_t)buf[86] + 10;
	if (off + 5 > len)
		return -1;
	buf[CLMEMB_ALIVE_NSTATE_OFF] = (unsigned char)nstate;
	buf[off] = (unsigned char)nreason;
	return 0;
}

int clmemb_alive_parse(const unsigned char *p, size_t n,
		struct clmemb_alive *out)
{
	struct pc_rcur c;

	pc_rcur_init(&c, p, n);
	if (!pc_rskip(&c, 1) || !pc_rfits(&c, 2 + 4))
		return 0;
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
	if (!pc_rfits(&c, 4 * 4))
		return 1;
	out->cl_open = pc_ru32(&c);
	out->cl_resp = pc_ru32(&c);
	out->cl_bin = pc_ru32(&c);
	out->cl_nresp = pc_ru32(&c);
	out->have |= CLMEMB_A_CLIENTS;
	{
		int i, n;                              /* all or nothing */

		if (!pc_rfits(&c, 2))
			return 1;
		n = pc_ru8(&c);
		out->ncols_total = pc_ru8(&c);
		if (n > CLMEMB_COLS_BEAT || !pc_rfits(&c, (size_t)n * 56))
			return 1;
		for (i = 0; i < n; i++) {
			out->cols[i].hash = pc_ru64(&c);
			out->cols[i].entries = pc_ru64(&c);
			out->cols[i].hits = pc_ru64(&c);
			out->cols[i].misses = pc_ru64(&c);
			out->cols[i].stores = pc_ru64(&c);
			out->cols[i].removes = pc_ru64(&c);
			out->cols[i].expired = pc_ru64(&c);
		}
		out->ncols = n;
		out->have |= CLMEMB_A_COLS;
	}
	if (!pc_rfits(&c, 2 + 8))
		return 1;
	out->ps_port = pc_ru16(&c);
	out->ps_interest = pc_ru64(&c);
	out->have |= CLMEMB_A_PSRELAY;

	if (!pc_rfits(&c, 1))
		return 1;
	out->nreason = pc_ru8(&c);
	out->have |= CLMEMB_A_REASON;

	if (!pc_rfits(&c, 1))
		return 1;
	out->epoch = pc_ru8(&c);
	out->have |= CLMEMB_A_EPOCH;

	if (!pc_rfits(&c, 1))
		return 1;
	out->features = pc_ru8(&c);
	out->have |= CLMEMB_A_FEATURES;

	if (!pc_rfits(&c, 2))
		return 1;
	out->sole_witness = pc_ru16(&c);
	out->have |= CLMEMB_A_SOLEWIT;
	{
		int i, n;                              /* all or nothing */

		if (!pc_rfits(&c, 1))
			return 1;
		n = pc_ru8(&c);
		/* 0 is "no settings"; the watermarks group follows the count either way */
		if (n > CLMEMB_CFG_MAX || !pc_rfits(&c, (size_t)n * 8))
			return 1;
		for (i = 0; i < n; i++) {
			out->cfg_name[i] = pc_ru32(&c);
			out->cfg_val[i] = pc_ru32(&c);
		}
		if (n) {
			out->ncfg = n;
			out->have |= CLMEMB_A_CFGSIG;
		}
	}
	{
		int i, n;                              /* the watermarks group */

		if (!pc_rfits(&c, 4 + 1 + 2))
			return 1;
		out->features32 = pc_ru32(&c);
		n = pc_ru8(&c);
		out->nspaces = pc_ru16(&c);
		out->have |= CLMEMB_A_FEAT32;
		if (n > CLMEMB_MARKS_MAX ||
		        !pc_rfits(&c, (size_t)n * CLMEMB_MARK_LEN))
			return 1;               /* the pairs: all or nothing */
		for (i = 0; i < n; i++) {
			out->marks[i].oid = pc_ru64(&c);
			out->marks[i].incarn = pc_ru32(&c);
			out->marks[i].mark = pc_ru64(&c);
		}
		out->nmarks = n;
		out->have |= CLMEMB_A_MARKS;
		if (pc_rfits(&c, 2)) {
			unsigned v = pc_ru8(&c);

			out->link_pct = v > 100 ? -1 : (int)v;
			out->link_level = pc_ru8(&c);
			out->have |= CLMEMB_A_LINK;
			if (pc_rfits(&c, 8)) {
				out->cfg_digest2 = pc_ru64(&c);
				out->have |= CLMEMB_A_CFG2;
			}
		}
	}
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
	pc_w8(&c, (unsigned)in->epoch);
	pc_w8(&c, (unsigned)in->committed);
	if (in->cfg2)
		pc_w64(&c, in->cfg_digest2);
	return c.ok ? pc_wcur_len(&c, buf) : 0;
}

int clmemb_malive_parse(const unsigned char *p, size_t n,
		struct clmemb_malive *out)
{
	struct pc_rcur c;

	pc_rcur_init(&c, p, n);
	if (!pc_rskip(&c, 1) || !pc_rfits(&c, 2 + 2 + 8 + 4))
		return 0;
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

	if (!pc_rfits(&c, 1))
		return 1;
	out->epoch = pc_ru8(&c);
	out->have |= CLMEMB_M_EPOCH;

	if (!pc_rfits(&c, 1))
		return 1;
	out->committed = pc_ru8(&c);
	out->have |= CLMEMB_M_COMMIT;

	if (!pc_rfits(&c, 8))
		return 1;
	out->cfg_digest2 = pc_ru64(&c);
	out->have |= CLMEMB_M_CFG2;
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
	pc_w8(&c, (unsigned)in->epoch);
	if (in->cfg2)
		pc_w64(&c, in->cfg_digest2);
	return c.ok ? pc_wcur_len(&c, buf) : 0;
}

int clmemb_joinreq_parse(const unsigned char *p, size_t n,
		struct clmemb_joinreq *out)
{
	struct pc_rcur c;

	pc_rcur_init(&c, p, n);
	if (!pc_rskip(&c, 1) || !pc_rfits(&c, 8))
		return 0;
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

	if (!pc_rfits(&c, 1))
		return 1;
	out->epoch = pc_ru8(&c);
	out->have |= CLMEMB_J_EPOCH;

	if (!pc_rfits(&c, 8))
		return 1;
	out->cfg_digest2 = pc_ru64(&c);
	out->have |= CLMEMB_J_CFG2;
	return 1;
}

/* ---- ASSIGN ---- records are an ARRAY: a cursor per record, positioned by index */
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

size_t clmemb_trailer_write(unsigned char *buf, size_t cap, const struct clmemb_trailer *t)
{
	struct pc_wcur c;

	pc_wcur_init(&c, buf, cap);
	pc_w8(&c, t->flags);
	pc_w8(&c, (unsigned)t->part);
	pc_w8(&c, (unsigned)t->parts);
	return c.ok ? pc_wcur_len(&c, buf) : 0;
}

int clmemb_trailer_parse(const unsigned char *p, size_t n, int count,
		struct clmemb_trailer *t)
{
	size_t off = (size_t)CLMEMB_ASSIGN_HDR + (size_t)count * CLMEMB_ASSIGN_REC;

	memset(t, 0, sizeof *t);
	if (count < 0 || off > n || n - off < CLMEMB_ASSIGN_TRAILER)
		return 0;
	t->flags = p[off];
	t->part = p[off + 1];
	t->parts = p[off + 2];
	return 1;
}

size_t clmemb_wait_write(unsigned char *buf, size_t cap, unsigned char type,
		uint64_t tok, const struct clmemb_wait *w)
{
	struct pc_wcur c;

	pc_wcur_init(&c, buf, cap);
	pc_w8(&c, type);
	pc_w64(&c, tok);
	pc_w8(&c, (unsigned)w->reason);
	pc_w32(&c, w->term);
	pc_w16(&c, (uint16_t)w->node);
	pc_w16(&c, (uint16_t)w->count);
	return c.ok ? pc_wcur_len(&c, buf) : 0;
}

int clmemb_wait_parse(const unsigned char *p, size_t n, uint64_t *tok,
		struct clmemb_wait *w)
{
	struct pc_rcur c;

	memset(w, 0, sizeof *w);
	pc_rcur_init(&c, p, n);
	if (!pc_rskip(&c, 1) || !pc_rfits(&c, 8))
		return 0;
	*tok = pc_ru64(&c);
	if (pc_rfits(&c, CLMEMB_WAIT_LEN - CLMEMB_TOK_LEN)) {
		w->reason = pc_ru8(&c);
		w->term = pc_ru32(&c);
		w->node = pc_ru16(&c);
		w->count = pc_ru16(&c);
	}
	return 1;
}

size_t clmemb_goodbye_write(unsigned char *buf, size_t cap, unsigned char type,
		int node, unsigned int incarn)
{
	struct pc_wcur c;

	pc_wcur_init(&c, buf, cap);
	pc_w8(&c, type);
	pc_w16(&c, (uint16_t)node);
	pc_w32(&c, (uint32_t)incarn);
	return c.ok ? pc_wcur_len(&c, buf) : 0;
}

int clmemb_goodbye_parse(const unsigned char *p, size_t n, int *node,
		unsigned int *incarn, int *have_incarn)
{
	struct pc_rcur c;

	*have_incarn = 0;
	*incarn = 0;
	pc_rcur_init(&c, p, n);
	if (!pc_rskip(&c, 1) || !pc_rfits(&c, 2))
		return 0;
	*node = pc_ru16(&c);
	if (pc_rfits(&c, 4)) {
		*incarn = pc_ru32(&c);
		*have_incarn = 1;
	}
	return 1;
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
