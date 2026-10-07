/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov
 * Part of libperfd - see lib/LICENSE.  This file must stay
 * free of src/core includes; tools/sync-libperfd.sh exports
 * exactly the MIT set to consumers. */
/*
 * perfd-async.inc - part of perfd.c, which #includes it in order: one translation
 * unit, so every static stays file-local and consumers still compile
 * perfd.c alone.  Not a compilation unit of its own.
 *
 * Holds the async API (S32): output buffering, the input path, the id -> callback
 * table, the readiness-driven handshake and the public async calls.
 */
/* ---- output buffering -------------------------------------------------- */

static int wbuf_room(perfd_t *p, size_t more)
{
	if (p->wlen + more <= p->wcap)
		return 0;
	while (p->wcap < p->wlen + more)
		p->wcap = p->wcap ? p->wcap * 2 : 65536;
	{
		void *nb = realloc(p->w, p->wcap);

		if (!nb) {
			p->poisoned = 1;
			return err(p, "out of memory");
		}
		p->w = nb;
	}
	return 0;
}

/* stage plaintext for the socket, sealing it first when the channel is
 * encrypted - the same record shape send_bytes() writes, just buffered
 * instead of written, because a non-blocking socket may take a prefix */
static int wbuf_put(perfd_t *p, const char *b, size_t n)
{
	if (!p->encrypted) {
		if (wbuf_room(p, n) != 0)
			return -1;
		memcpy(p->w + p->wlen, b, n);
		p->wlen += n;
		return 0;
	}
	while (n) {
		size_t chunk = n > PC_NOISE_MAXPT ? PC_NOISE_MAXPT : n;
		int cl;

		if (wbuf_room(p, 2 + chunk + PC_NOISE_TAGLEN) != 0)
			return -1;
		cl = pc_transport_encrypt(&p->cs_send, (const uint8_t *)b,
			chunk, (uint8_t *)p->w + p->wlen + 2);
		if (cl < 0) {
			p->poisoned = 1;
			return err(p, "transport encrypt failed");
		}
		p->w[p->wlen] = (char)(unsigned char)cl;
		p->w[p->wlen + 1] = (char)(unsigned char)(cl >> 8);
		p->wlen += 2 + (size_t)cl;
		b += chunk;
		n -= chunk;
	}
	return 0;
}

/* 0 = buffer drained, 1 = partial (watch for writability), -1 = failed */
static int wbuf_push(perfd_t *p)
{
	while (p->woff < p->wlen) {
		ssize_t r = write(p->fd, p->w + p->woff, p->wlen - p->woff);

		if (r > 0) {
			p->woff += (size_t)r;
			continue;
		}
		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return 1;
		p->poisoned = 1;
		err(p, "connection %s (%s)", r == 0 ? "closed" : "failed",
			r == 0 ? "EOF" : strerror(errno));
		return -1;
	}
	p->woff = p->wlen = 0;
	return 0;
}

/* ---- input: socket -> ciphertext -> rbuf ------------------------------- */

/* read whatever the socket has.  1 = bytes arrived, 0 = would block,
 * -1 = closed or failed. */
static int cin_pull(perfd_t *p)
{
	int got = 0;

	for (;;) {
		ssize_t r;

		if (p->cinlen + 65536 > p->cincap) {
			size_t cap = p->cincap ? p->cincap * 2 : 131072;

			while (cap < p->cinlen + 65536)
				cap *= 2;
			{
				void *nb = realloc(p->cin, cap);

				if (!nb) {
					p->poisoned = 1;
					return err(p, "out of memory");
				}
				p->cin = nb;
			}
			p->cincap = cap;
		}
		r = read(p->fd, p->cin + p->cinlen, p->cincap - p->cinlen);
		if (r > 0) {
			p->cinlen += (size_t)r;
			got = 1;
			continue;
		}
		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return got;
		p->poisoned = 1;
		err(p, "connection %s", r == 0 ? "closed" : strerror(errno));
		return -1;
	}
}

/* move complete records out of cin and into rbuf as plaintext.  A record
 * split across reads is simply left in cin until the rest turns up -
 * this is the whole reason the blocking fill() cannot be reused. */
static int cin_drain(perfd_t *p)
{
	if (!p->encrypted) {
		if (!p->cinlen)
			return 0;
		if (rbuf_reserve(p, p->cinlen) != 0)
			return -1;
		memcpy(p->rbuf + p->rlen, p->cin, p->cinlen);
		p->rlen += p->cinlen;
		p->cinlen = 0;
		return 0;
	}
	for (;;) {
		size_t cl;
		int pl;

		if (p->cinlen < 2)
			return 0;
		cl = (size_t)(unsigned char)p->cin[0] |
			((size_t)(unsigned char)p->cin[1] << 8);
		if (cl < PC_NOISE_TAGLEN || cl > PC_NOISE_MAXMSG) {
			p->poisoned = 1;
			return err(p, "bad transport record");
		}
		if (p->cinlen < 2 + cl)
			return 0;
		if (rbuf_reserve(p, cl) != 0)
			return -1;
		pl = pc_transport_decrypt(&p->cs_recv,
			(const uint8_t *)p->cin + 2, cl,
			(uint8_t *)p->rbuf + p->rlen);
		if (pl < 0) {
			p->poisoned = 1;
			return err(p, "transport decrypt failed");
		}
		p->rlen += (size_t)pl;
		memmove(p->cin, p->cin + 2 + cl, p->cinlen - 2 - cl);
		p->cinlen -= 2 + cl;
	}
}

/* An async handle learns the fleet THROUGH ITS OWN PIPELINE.  It cannot
 * use learn_members(): that calls roundtrip_once(), which blocks for a
 * reply, and an async handle's socket is non-blocking and driven by the
 * caller's event loop.  So the request goes out as an ordinary async
 * submit and this callback parses the answer when it arrives.
 *
 * Until this lands, route_ok is 0 and perfd_owner_of() returns -1, which
 * is the honest answer - the fleet is genuinely unknown until the reply
 * comes back.  Callers that need routing from the first request should
 * pump perfd_events() until perfd_routing() turns 1.
 *
 * S34's standbys are deliberately NOT opened here: they are separate
 * connections the caller would never poll.  See perfd_owner_of(). */
static void members_learned(char *result, size_t len, const char *errmsg,
		void *ctx)
{
	perfd_t *p = ctx;

	p->learning = 0;
	if (!errmsg && result)         /* the raw reply tree: never rendered */
		parse_members(p, (const unsigned char *)result, len);
	free(result);                  /* the async contract: callee owns it */
}

static int learn_members_async(perfd_t *p)
{
	if (p->is_child || p->learning || p->learned_async)
		return 0;
	if (!p->route_want)            /* nobody asked to route */
		return 0;
	p->learning = 1;
	p->learned_async = 1;          /* one attempt; a failure is not fatal */
	if (submit_tree(p, "members", NULL, 0, members_learned, p, 0) != 0) {
		p->learning = 0;
		return -1;
	}
	return 0;
}

/* ---- id -> callback ---------------------------------------------------- */

static int call_add(perfd_t *p, unsigned long long id, perfd_reply_cb cb,
		void *ctx, int render)
{
	if (p->ncalls == p->callcap) {
		int cap = p->callcap ? p->callcap * 2 : 64;
		struct pd_call *n = realloc(p->calls, (size_t)cap * sizeof *n);

		if (!n) {
			p->poisoned = 1;
			return err(p, "out of memory");
		}
		p->calls = n;
		p->callcap = cap;
	}
	p->calls[p->ncalls].id = id;
	p->calls[p->ncalls].cb = cb;
	p->calls[p->ncalls].ctx = ctx;
	p->calls[p->ncalls].render = render;
	p->ncalls++;
	return 0;
}

/* hand a completed reply to whoever asked for it.  Arrival order, not
 * request order: that is the point of the async surface - a slow KEYS
 * must not hold up the gets queued behind it.  A perfd_submit() caller
 * asked in JSON and is answered in JSON (render): the reply tree is
 * rendered here, so the callback sees NUL-terminated text and its
 * length; everyone else gets the payload as it came. */
static void call_fire(perfd_t *p, unsigned long long id, char *result,
		size_t rlen, const char *errmsg)
{
	int i;

	for (i = 0; i < p->ncalls; i++)
		if (p->calls[i].id == id) {
			perfd_reply_cb cb = p->calls[i].cb;
			void *ctx = p->calls[i].ctx;
			int render = p->calls[i].render;

			p->calls[i] = p->calls[--p->ncalls];
			if (!cb) {
				free(result);
				return;
			}
			if (render && result) {
				char *js = tree_to_json(p, (unsigned char *)result,
					rlen, &rlen);

				free(result);
				if (!js) {
					cb(NULL, 0, p->err, ctx);
					return;
				}
				result = js;
			}
			cb(result, rlen, errmsg, ctx);
			return;
		}
	free(result);              /* a reply nobody is waiting for */
}

/* the connection died: tell every caller still waiting, so a consumer
 * never silently loses a request it issued */
static void call_fail_all(perfd_t *p, const char *why)
{
	while (p->ncalls) {
		struct pd_call c = p->calls[--p->ncalls];

		if (c.cb)
			c.cb(NULL, 0, why, c.ctx);
	}
}

/* ---- handshake, driven by readiness ------------------------------------ */

static int hs_build_msg1(perfd_t *p)
{
	uint8_t psk[PC_NOISE_KEYLEN], prologue[1] = { PC_PRIN_CLIENT };
	uint8_t ver = PC_CLIENT_VER, msg[3 + PC_NOISE_MAXMSG];
	size_t mlen = 0;

	if (psk_for(p->sec[p->hs_sec], psk) != 0)
		return err(p, "key derivation failed");
	pc_hs_init_initiator(&p->hs, prologue, 1);
	if (pc_hs_write_msg1(&p->hs, psk, &ver, 1, msg + 3, &mlen) != 0) {
		sodium_memzero(psk, sizeof psk);
		return err(p, "handshake build failed");
	}
	sodium_memzero(psk, sizeof psk);
	msg[0] = (uint8_t)(1 + mlen);
	msg[1] = (uint8_t)((1 + mlen) >> 8);
	msg[2] = PC_PRIN_CLIENT;
	if (wbuf_room(p, 3 + mlen) != 0)
		return -1;
	memcpy(p->w + p->wlen, msg, 3 + mlen);
	p->wlen += 3 + mlen;
	p->hs_stage = PD_HS_SEND;
	return 0;
}

/* consume msg2 if it has all arrived.  1 = established, 0 = need more,
 * -1 = failed. */
static int hs_take_msg2(perfd_t *p)
{
	uint8_t m2[PC_NOISE_MAXMSG];
	size_t cl, plen = 0;

	if (p->cinlen < 2)
		return 0;
	cl = (size_t)(unsigned char)p->cin[0] |
		((size_t)(unsigned char)p->cin[1] << 8);
	if (cl == 0 || cl > sizeof m2) {
		p->poisoned = 1;
		return err(p, "bad handshake frame");
	}
	if (p->cinlen < 2 + cl)
		return 0;
	memcpy(m2, p->cin + 2, cl);
	memmove(p->cin, p->cin + 2 + cl, p->cinlen - 2 - cl);
	p->cinlen -= 2 + cl;
	if (pc_hs_read_msg2(&p->hs, m2, cl, NULL, &plen, &p->cs_send,
	        &p->cs_recv) != 0) {
		p->poisoned = 1;
		return err(p, "handshake failed (wrong secret?)");
	}
	p->encrypted = 1;
	p->hs_stage = PD_HS_DONE;
	p->st = PERFD_ST_READY;
	learn_members_async(p);        /* async fleet discovery; see above */
	return 1;
}

/* ---- public ------------------------------------------------------------ */

int perfd_fd(const perfd_t *p)
{
	return p ? p->fd : -1;
}

int perfd_state(const perfd_t *p)
{
	if (!p)
		return PERFD_ST_FAILED;
	return p->poisoned ? PERFD_ST_FAILED : p->st;
}

int perfd_events(const perfd_t *p)
{
	int ev = PERFD_EV_READ;

	if (!p || p->fd < 0)
		return 0;
	/* the TCP connect completes as writability, and a partial write
	 * needs the same signal - so this is not constant and must be
	 * re-read after every entry point */
	if (p->hs_stage == PD_HS_TCP || p->woff < p->wlen)
		ev |= PERFD_EV_WRITE;
	return ev;
}

int perfd_inflight(const perfd_t *p)
{
	return p ? p->ncalls : 0;
}

perfd_t *perfd_connect_async(const char *host, int port,
		const perfd_opts *opts)
{
	perfd_t *p = pd_alloc(opts);
	struct addrinfo hints, *res, *ai;
	char ps[16];
	int one = 1, fl;

	if (!p)
		return NULL;
	p->async = 1;
	p->st = PERFD_ST_CONNECTING;
	p->hs_stage = PD_HS_TCP;
	p->hs_sec = 0;
	snprintf(p->ahost, sizeof p->ahost, "%s", host ? host : "");
	p->aport = port;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(ps, sizeof ps, "%d", port);
	if (getaddrinfo(host, ps, &hints, &res) != 0) {
		err(p, "cannot resolve %s", host ? host : "(null)");
		perfd_free(p);
		return NULL;
	}
	for (ai = res; ai; ai = ai->ai_next) {
		p->fd = socket(ai->ai_family, ai->ai_socktype, 0);
		if (p->fd < 0)
			continue;
		fl = fcntl(p->fd, F_GETFL, 0);
		fcntl(p->fd, F_SETFL, (fl < 0 ? 0 : fl) | O_NONBLOCK);
		setsockopt(p->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
		sock_keepalive(p->fd, p->keepalive_s);                  /* S104 */
		if (connect(p->fd, ai->ai_addr, ai->ai_addrlen) == 0 ||
		        errno == EINPROGRESS)
			break;
		close(p->fd);
		p->fd = -1;
	}
	freeaddrinfo(res);
	if (p->fd < 0) {
		err(p, "cannot connect: %s", strerror(errno));
		perfd_free(p);
		return NULL;
	}
	return p;
}

/* the request just framed into p->q (one CMD or data-verb frame, id =
 * p->next_id) is handed to the async write buffer and correlated by
 * callback.  enqueue_cmd and enqueue_bin both append to p->q and take
 * ++p->next_id without touching the blocking collector's FIFO, so the
 * correlation is the same either way. */
static int submit_framed(perfd_t *p, perfd_reply_cb cb, void *ctx,
		int render)
{
	if (call_add(p, p->next_id, cb, ctx, render) != 0) {
		p->qlen = 0;
		return -1;
	}
	if (wbuf_put(p, p->q, p->qlen) != 0) {
		p->qlen = 0;
		return -1;
	}
	p->qlen = 0;
	if (!p->eager_push)
		return 0;              /* batched; goes out on write-readiness */
	if (wbuf_push(p) < 0) {
		call_fail_all(p, p->err);
		p->st = PERFD_ST_FAILED;
		return -1;
	}
	return 0;
}

static int submit_ok(perfd_t *p)
{
	if (!p->async)
		return err(p, "not an async handle");
	if (p->poisoned)
		return err(p, "connection poisoned");
	if (p->st != PERFD_ST_READY)
		return err(p, "connection not ready");
	return 0;
}

/* a CMD submit with the parameters already a tree; @render = the
 * callback wants JSON text (the public perfd_submit), 0 = the tree */
static int submit_tree(perfd_t *p, const char *method,
		const unsigned char *tree, size_t tlen, perfd_reply_cb cb,
		void *ctx, int render)
{
	if (submit_ok(p) != 0)
		return -1;
	if (enqueue_cmd(p, method, tree, tlen) != 0)
		return -1;
	return submit_framed(p, cb, ctx, render);
}

int perfd_submit(perfd_t *p, const char *method, const char *params_json,
		perfd_reply_cb cb, void *ctx)
{
	struct pc_tw w;
	int rc;

	if (submit_ok(p) != 0)
		return -1;
	if (json_to_tree(p, params_json, &w) != 0)
		return -1;
	rc = submit_tree(p, method, w.b, w.n, cb, ctx, 1);
	pc_tw_free(&w);
	return rc;
}

int perfd_submit_kv(perfd_t *p, int verb, const char *col, const char *key,
		const void *val, size_t vlen, long long by, long long ttl,
		perfd_reply_cb cb, void *ctx)
{
	if (submit_ok(p) != 0)
		return -1;
	if (enqueue_bin(p, verb, col, key, val, vlen, by, ttl) != 0)
		return -1;
	return submit_framed(p, cb, ctx, 0);
}

/* NOT perfd_flush: that name belongs to the SYNC pipeline
 * (perfd_append/perfd_flush/perfd_next_reply), which queues into p->q
 * and sends with send_bytes.  This is the async write buffer. */
int perfd_push(perfd_t *p)
{
	if (!p->async)
		return err(p, "not an async handle");
	if (p->poisoned) {
		p->st = PERFD_ST_FAILED;
		return -1;
	}
	if (p->woff >= p->wlen)
		return 0;                      /* nothing queued */
	if (wbuf_push(p) < 0) {
		call_fail_all(p, p->err);
		p->st = PERFD_ST_FAILED;
		return -1;
	}
	return 0;
}

int perfd_write_ready(perfd_t *p)
{
	if (!p->async)
		return err(p, "not an async handle");
	if (p->poisoned) {
		p->st = PERFD_ST_FAILED;
		return -1;
	}
	if (p->hs_stage == PD_HS_TCP) {
		int se = 0;
		socklen_t sl = sizeof se;

		if (getsockopt(p->fd, SOL_SOCKET, SO_ERROR, &se, &sl) != 0 ||
		        se != 0) {
			p->poisoned = 1;
			p->st = PERFD_ST_FAILED;
			err(p, "connect failed: %s", strerror(se ? se : errno));
			call_fail_all(p, p->err);
			return -1;
		}
		if (!p->nsec) {                /* plaintext: nothing to do */
			p->hs_stage = PD_HS_DONE;
			p->st = PERFD_ST_READY;
			learn_members_async(p);
			return 0;
		}
		if (hs_build_msg1(p) != 0) {
			p->poisoned = 1;
			p->st = PERFD_ST_FAILED;
			return -1;
		}
	}
	if (wbuf_push(p) < 0) {
		p->st = PERFD_ST_FAILED;
		call_fail_all(p, p->err);
		return -1;
	}
	if (p->hs_stage == PD_HS_SEND && p->woff == p->wlen)
		p->hs_stage = PD_HS_WAIT;
	return 0;
}

int perfd_read_ready(perfd_t *p)
{
	int rc;

	if (!p->async)
		return err(p, "not an async handle");
	if (p->poisoned) {
		p->st = PERFD_ST_FAILED;
		return -1;
	}
	if (cin_pull(p) < 0) {
		p->st = PERFD_ST_FAILED;
		call_fail_all(p, p->err);
		return -1;
	}
	if (p->hs_stage == PD_HS_WAIT) {
		rc = hs_take_msg2(p);
		if (rc < 0) {
			p->st = PERFD_ST_FAILED;
			call_fail_all(p, p->err);
			return -1;
		}
		if (rc == 0)
			return 0;              /* msg2 still incomplete */
	}
	if (p->hs_stage != PD_HS_DONE)
		return 0;
	if (cin_drain(p) != 0) {
		p->st = PERFD_ST_FAILED;
		call_fail_all(p, p->err);
		return -1;
	}
	for (;;) {
		unsigned long long got;
		char *result, ebuf[256];
		size_t rlen;

		rc = take_msg(p, &got, &result, &rlen, ebuf, sizeof ebuf);
		if (rc == 0)
			break;
		if (rc == 2)
			continue;
		if (rc < 0) {
			p->st = PERFD_ST_FAILED;
			call_fail_all(p, p->err);
			return -1;
		}
		/* call_fire owns and frees its reply; result points into
		 * rbuf (S284) */
		if (result && !(result = dup_reply(p, result, rlen))) {
			p->st = PERFD_ST_FAILED;
			call_fail_all(p, p->err);
			return -1;
		}
		call_fire(p, got, result, rlen, result ? NULL : ebuf);
	}
	return 0;
}
