/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov
 * Part of libperfd - see lib/LICENSE.  This file must stay
 * free of src/core includes; tools/sync-libperfd.sh exports
 * exactly the MIT set to consumers. */
/* perfd-io.inc - part of perfd.c's one translation unit, not compiled alone: raw socket IO, the LE
 * helpers, reply framing (take_msg) and the blocking collector (collect_head) */
/* ---- raw IO ------------------------------------------------------------ */

static int io_all(perfd_t *p, int wr, void *buf, size_t n)
{
	size_t off = 0;

	while (off < n) {
		ssize_t r = wr ? send(p->fd, (char *)buf + off, n - off, MSG_NOSIGNAL)
		               : read(p->fd, (char *)buf + off, n - off);

		if (r <= 0) {
			if (r < 0 && errno == EINTR)
				continue;
			p->poisoned = 1;
			p->io_silent = r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
			return errk(p, PERFD_E_TRANSPORT, "connection %s (%s)",
				r == 0 ? "closed" : "failed",
				r == 0 ? "EOF" : strerror(errno));
		}
		off += (size_t)r;
	}
	return 0;
}

/* the transport scratch, made at the first encrypted transfer */
static uint8_t *xbuf(perfd_t *p)
{
	if (!p->xbuf && !(p->xbuf = malloc(PERFD_XBUF))) {
		p->poisoned = 1;
		errk(p, PERFD_E_NOMEM, "out of memory");
	}
	return p->xbuf;
}

static int send_bytes(perfd_t *p, const char *b, size_t n)
{
	if (!p->encrypted)
		return io_all(p, 1, (void *)b, n);
	while (n) {
		uint8_t *rec = xbuf(p);
		size_t chunk = n > PC_NOISE_MAXPT ? PC_NOISE_MAXPT : n;
		int cl;

		if (!rec)
			return -1;
		cl = pc_transport_encrypt(&p->cs_send, (const uint8_t *)b,
			chunk, rec + 2);

		if (cl < 0) {
			p->poisoned = 1;
			return errk(p, PERFD_E_TRANSPORT, "transport encrypt failed");
		}
		rec[0] = (uint8_t)cl;
		rec[1] = (uint8_t)(cl >> 8);
		if (io_all(p, 1, rec, 2 + (size_t)cl) != 0)
			return -1;
		b += chunk;
		n -= chunk;
	}
	return 0;
}

static int rbuf_reserve(perfd_t *p, size_t more)
{
	/* drop the consumed prefix in ONE move: shifting the remainder after every message was O(n^2)
	 * on a deep pipeline (61% of the CPU in memmove at depth 256) */
	if (p->roff) {
		if (p->rlen > p->roff)
			memmove(p->rbuf, p->rbuf + p->roff, p->rlen - p->roff);
		p->rlen -= p->roff;
		p->roff = 0;
	}
	if (p->rlen + more <= p->rcap)
		return 0;
	while (p->rcap < p->rlen + more)
		p->rcap = p->rcap ? p->rcap * 2 : 65536;
	{
		/* never p->x = realloc(p->x, ...): a failed grow would lose
		 * the old block (leak) AND the buffered bytes */
		void *nb = realloc(p->rbuf, p->rcap);

		if (!nb) {
			p->poisoned = 1;
			return errk(p, PERFD_E_NOMEM, "out of memory");
		}
		p->rbuf = nb;
	}
	return 0;
}

/* grow the stream by at least one byte (blocking, timeout-bounded) */
static int fill(perfd_t *p)
{
	if (!p->encrypted) {
		ssize_t r;

		if (rbuf_reserve(p, 65536) != 0)
			return -1;
		do              /* under SO_RCVTIMEO a handled signal is EINTR, SA_RESTART or not */
			r = read(p->fd, p->rbuf + p->rlen, p->rcap - p->rlen);
		while (r < 0 && errno == EINTR);
		if (r <= 0) {
			p->poisoned = 1;
			p->io_silent = r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
			return errk(p, PERFD_E_TRANSPORT, "connection %s",
				r == 0 ? "closed" : strerror(errno));
		}
		p->rlen += (size_t)r;
		return 0;
	}
	{
		uint8_t hdr[2], *ct = xbuf(p);   /* not 64 KB of stack */
		size_t cl;
		int pl;

		if (!ct)
			return -1;
		if (io_all(p, 0, hdr, 2) != 0)
			return -1;
		cl = (size_t)hdr[0] | ((size_t)hdr[1] << 8);
		if (cl < PC_NOISE_TAGLEN || cl > PC_NOISE_MAXMSG) {
			p->poisoned = 1;
			return errk(p, PERFD_E_PROTOCOL, "bad transport record");
		}
		if (io_all(p, 0, ct, cl) != 0)
			return -1;
		if (rbuf_reserve(p, cl) != 0)
			return -1;
		pl = pc_transport_decrypt(&p->cs_recv, ct, cl,
			(uint8_t *)p->rbuf + p->rlen);
		if (pl < 0) {
			p->poisoned = 1;
			return errk(p, PERFD_E_TRANSPORT, "transport decrypt failed");
		}
		p->rlen += (size_t)pl;
		return 0;
	}
}

/* ---- little-endian field helpers (the binary frames) ------------------- */

static void wle16(unsigned char *b, unsigned int v)
{
	b[0] = (unsigned char)v;
	b[1] = (unsigned char)(v >> 8);
}

static void wle32(unsigned char *b, unsigned int v)
{
	int i;

	for (i = 0; i < 4; i++)
		b[i] = (unsigned char)(v >> (8 * i));
}

static void wle64(unsigned char *b, unsigned long long v)
{
	int i;

	for (i = 0; i < 8; i++)
		b[i] = (unsigned char)(v >> (8 * i));
}

static unsigned int rle32(const char *b)
{
	unsigned int v = 0;
	int i;

	for (i = 0; i < 4; i++)
		v |= (unsigned int)(unsigned char)b[i] << (8 * i);
	return v;
}

static unsigned long long rle64(const char *b)
{
	unsigned long long v = 0;
	int i;

	for (i = 0; i < 8; i++)
		v |= (unsigned long long)(unsigned char)b[i] << (8 * i);
	return v;
}

/* ---- reply parsing ----------------------------------------------------- */

/* the hook's text: a NOTIFY tree rendered as JSON, the UDP path included, so the two texts cannot
 * differ; a byte renders to at most six characters, so 6n + 32 bounds the reused buffer */
static void notify_hook(perfd_t *p, const unsigned char *tree, size_t n)
{
	struct pc_jw w;
	size_t need = 32 + 6 * n;

	if (!p->ncb)
		return;
	if (need > p->njcap) {
		char *nb = realloc(p->nj, need);

		if (!nb)
			return;
		p->nj = nb;
		p->njcap = need;
	}
	pc_jw_init(&w, p->nj, p->njcap);
	if (pc_tree_json(&w, tree, n) != n || w.overflow)
		return;                        /* not a whole tree: nothing to show */
	p->ncb(w.buf, w.len, p->nctx);
}

/* ONE frame from rbuf, socket untouched (blocking and event-loop paths share it): 1 = got (NULL
 * result: an error frame, in ebuf), 2 = a notification consumed, 0 = incomplete, -1 = fatal */
static int take_msg(perfd_t *p, unsigned long long *got, char **result,
		size_t *rlen, char *ebuf, size_t ecap)
{
	char *rb;
	size_t rn;
	uint32_t plen;
	int type;

	*got = 0;
	*result = NULL;
	*rlen = 0;
	if (ecap)
		ebuf[0] = 0;
	if (p->rlen == p->roff)
		return 0;
	rb = p->rbuf + p->roff;                /* unread window */
	rn = p->rlen - p->roff;
	if ((unsigned char)rb[0] != BIN_MAGIC) {
		p->poisoned = 1;
		return errk(p, PERFD_E_PROTOCOL, "unparseable reply");
	}
	if (rn < BIN_HDR)
		return 0;
	if (rb[1] != BIN_VER) {
		p->poisoned = 1;
		return errk(p, PERFD_E_PROTOCOL, "bad frame version");
	}
	plen = rle32(rb + 4);
	if (plen > BIN_MAX_RSP) {
		p->poisoned = 1;
		return errk(p, PERFD_E_PROTOCOL, "oversized frame");
	}
	if (rn < BIN_HDR + plen)
		return 0;
	type = (unsigned char)rb[2];
	*got = rle64(rb + 8);
	if (type == BIN_NOTIFY) {
		/* consumed first: a hook that calls its handle must not meet this frame again */
		p->roff += BIN_HDR + plen;
		notify_internal(p, (const unsigned char *)rb + BIN_HDR, plen);
		p->hook_refuses = p->in_call > 0;
		notify_hook(p, (const unsigned char *)rb + BIN_HDR, plen);
		p->hook_refuses = 0;
		return 2;
	} else if (type != BIN_RSP) {
		p->poisoned = 1;
		return errk(p, PERFD_E_PROTOCOL, "bad frame type");
	} else if (rb[3] & BIN_F_ERR) {
		/* ecap is a parameter, so guard the empty case: ecap - 1
		 * would wrap and turn this into an overflow */
		size_t cap = ecap ? ecap - 1 : 0;
		size_t el = plen < cap ? plen : cap;

		if (cap) {
			memcpy(ebuf, rb + BIN_HDR, el);
			ebuf[el] = 0;
			/* this library sends no verb a 0.4.5 daemon lacks, so "unknown
			 * verb" means an OLDER daemon: say so */
			if (plen == 12 && !memcmp(rb + BIN_HDR, "unknown verb", 12))
				snprintf(ebuf, ecap, "the daemon does not speak this "
					"library's wire (perfcached < "
					PERFD_NEEDS_PERFCACHED "; libperfd "
					PERFD_VERSION " needs " PERFD_NEEDS_PERFCACHED
					" or later)");
		}
	} else {
		size_t body = plen;

		/* the holder hint is the LAST trailer */
		if ((rb[3] & BIN_F_HINT) && body >= 8) {
			body -= 8;
			p->hh_node = (int)rle32(rb + BIN_HDR + body);
			p->hh_ttl = rle32(rb + BIN_HDR + body + 4);
			p->hh_got = 1;
		}
		if ((rb[3] & BIN_F_MOVED) && body >= 8) {
			body = body - 8;
			p->hint_term = rle32(rb + BIN_HDR + body);
			p->hint_seq = rle32(rb + BIN_HDR + body + 4);
			p->hint_seen = 1;
		}
		/* into rbuf, not a copy - valid until the next fill();
		 * every caller copies it before reading the socket again */
		*result = rb + BIN_HDR;
		*rlen = body;
	}
	p->roff += BIN_HDR + plen;             /* consume by offset */
	return 1;
}

/* a malloc'd copy of a reply (out of order, or asked for owned) */
static char *dup_reply(perfd_t *p, const char *r, size_t n)
{
	char *c = malloc(n ? n : 1);

	if (!c) {
		p->poisoned = 1;
		errk(p, PERFD_E_NOMEM, "out of memory");
		return NULL;
	}
	memcpy(c, r, n);
	return c;
}

/* a request writer over the handle's scratch, sized to the caller's upper bound @need; a short
 * bound makes the writer "over", which tw_whole reports as an error, never a short request */
static int tw_scratch(perfd_t *p, struct pc_tw *w, size_t need);

/* grow-once - to the largest reply this handle has seen */
static int grow_buf(perfd_t *p, void *bufp, size_t *capp, size_t need)
{
	void **b = bufp;
	void *nb;
	size_t cap;

	if (need <= *capp && *b)
		return 0;
	cap = need < 4096 ? 4096 : need;
	nb = realloc(*b, cap);
	if (!nb) {
		p->poisoned = 1;
		return errk(p, PERFD_E_NOMEM, "out of memory");
	}
	*b = nb;
	*capp = cap;
	return 0;
}

static int tw_scratch(perfd_t *p, struct pc_tw *w, size_t need)
{
	if (grow_buf(p, &p->wsc, &p->wscap, need) != 0)
		return -1;
	pc_tw_init(w, p->wsc, p->wscap);
	return 0;
}

/* the reply the caller borrows: in rsc, or an owned copy if asked */
static int hand_reply(perfd_t *p, const char *r, size_t n, char **out)
{
	if (p->own_reply) {
		*out = dup_reply(p, r, n);
		return *out ? 0 : -1;
	}
	if (grow_buf(p, &p->rsc, &p->rscap, n) != 0)
		return -1;
	memcpy(p->rsc, r, n);
	*out = p->rsc;
	return 0;
}

/* the parked replies belong to one connection: they go with it */
static void early_free(perfd_t *p)
{
	struct early *e;

	while ((e = p->early)) {
		p->early = e->next;
		free(e->result);
		free(e);
	}
}

/* is @got a request still waiting, with no reply parked for it yet? */
static int park_due(const perfd_t *p, unsigned long long got)
{
	const struct early *e;
	int i;

	for (e = p->early; e; e = e->next)
		if (e->id == got)
			return 0;
	for (i = p->idhead; i < p->nids; i++)
		if (p->ids[i] == got)
			return 1;
	return 0;
}

/* park an out-of-order reply until its turn comes (blocking collector); one nobody waits for
 * (never issued, late, a duplicate) is dropped and counted */
static int park_msg(perfd_t *p, unsigned long long got, char *result,
		size_t rlen, const char *ebuf)
{
	struct early *e;

	if (!park_due(p, got)) {
		p->stray_replies++;
		return 0;
	}
	e = malloc(sizeof *e);
	if (!e) {
		p->poisoned = 1;
		return errk(p, PERFD_E_NOMEM, "out of memory");
	}
	/* result points into rbuf (take_msg): keep a copy */
	e->result = NULL;
	if (result && !(e->result = dup_reply(p, result, rlen))) {
		free(e);
		return -1;
	}
	e->id = got;
	e->rlen = rlen;
	snprintf(e->err, sizeof e->err, "%s", ebuf);
	e->next = p->early;
	p->early = e;
	return 0;
}

/* read replies until the FIFO head's arrives, parking those that overtake it: 0 = *out (*outlen
 * bytes) BORROWED until the next collect, malloc'd only with own_reply; -1 with p->err set */
static int collect_head(perfd_t *p, char **out, size_t *outlen)
{
	unsigned long long want;
	struct early **pe, *e;

	*out = NULL;
	*outlen = 0;
	if (p->idhead >= p->nids)
		return errk(p, PERFD_E_STATE, "no reply pending");
	want = p->ids[p->idhead];

	/* parked already? */
	for (pe = &p->early; (e = *pe); pe = &e->next)
		if (e->id == want) {
			int ok = e->result != NULL;

			*pe = e->next;
			p->idhead++;
			if (p->idhead == p->nids)
				p->nids = p->idhead = 0;
			*outlen = e->rlen;
			if (!ok)
				errk(p, server_kind(e->err), "%s", e->err);
			if (ok && p->own_reply) {
				*out = e->result;     /* already owned */
			} else if (ok) {
				ok = hand_reply(p, e->result, e->rlen, out) == 0;
				free(e->result);
			}
			free(e);
			return ok ? 0 : -1;
		}

	for (;;) {
		unsigned long long got;
		char *result, ebuf[256];
		size_t rlen;
		int rc;

		rc = take_msg(p, &got, &result, &rlen, ebuf, sizeof ebuf);
		if (rc < 0)
			return -1;
		if (rc == 2)
			continue;
		if (rc == 0) {
			if (p->poisoned)
				return errk(p, PERFD_E_TRANSPORT, "connection poisoned");
			if (fill(p) != 0)
				return -1;
			continue;
		}
		if (got == want) {
			p->idhead++;
			if (p->idhead == p->nids)
				p->nids = p->idhead = 0;
			*outlen = rlen;
			if (!result)
				return errk(p, server_kind(ebuf), "%s", ebuf);
			return hand_reply(p, result, rlen, out);
		}
		if (park_msg(p, got, result, rlen, ebuf) != 0)
			return -1;
	}
}

