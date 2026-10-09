/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov
 * Part of libperfd - see lib/LICENSE.  This file must stay
 * free of src/core includes; tools/sync-libperfd.sh exports
 * exactly the MIT set to consumers. */
/*
 * perfd-pubsub.inc - part of perfd.c, which #includes it in order: one translation
 * unit, so every static stays file-local and consumers still compile
 * perfd.c alone.  Not a compilation unit of its own.
 *
 * Holds pub/sub (PS4) and pub/sub over UDP (PS5), with the reply helpers they
 * and the typed verbs share.
 */
/* ---- pub/sub (PS4) ------------------------------------------------------ */

/* the reply map's integer @field, or -1 with p->err */
static int reply_int(perfd_t *p, const char *what, char *res, size_t n,
		const char *field, long long *out)
{
	struct pc_tv v;
	int rc;

	if (tv_root(p, &v, (const unsigned char *)res, n, what) != 0) {
		
		return -1;
	}
	rc = pc_tv_get_int(&v, 0, field, out);
	tv_put(p, &v);
	
	return rc == 0 ? 0 : err(p, "bad %s reply", what);
}

/* the reply map's bool @field (absent = 0), or -1 with p->err */
static int reply_bool(perfd_t *p, const char *what, char *res, size_t n,
		const char *field, int *out)
{
	struct pc_tv v;

	if (tv_root(p, &v, (const unsigned char *)res, n, what) != 0) {
		
		return -1;
	}
	*out = pc_tv_get_bool(&v, 0, field);
	tv_put(p, &v);
	
	return 0;
}

static int ps_count_call(perfd_t *p, const char *method, const char *field,
		const char *key, const char *name)
{
	unsigned char buf[4096 + 64];
	struct pc_tw w;
	char *res;
	size_t rn;
	long long n = 0;

	pc_tw_init(&w, buf, sizeof buf);
	if (name) {
		pc_tw_map(&w);
		pc_tw_key(&w, key);
		pc_tw_str(&w, name);
		pc_tw_end(&w);
		if (pc_tw_done(&w) != 0)
			return err(p, "%s too long", key);
	}
	if (roundtrip(p, method, w.b, w.n, &res, &rn) != 0)
		return -1;
	if (reply_int(p, method, res, rn, field, &n) != 0)
		return -1;
	return (int)n;
}

int perfd_subscribe(perfd_t *p, const char *channel)
{
	return channel ? ps_count_call(p, "subscribe", "subscribed", "channel", channel) : -1;
}

int perfd_psubscribe(perfd_t *p, const char *pattern)
{
	return pattern ? ps_count_call(p, "psubscribe", "subscribed", "pattern", pattern) : -1;
}

int perfd_unsubscribe(perfd_t *p, const char *channel)
{
	return ps_count_call(p, "unsubscribe", "subscribed", "channel", channel);
}

int perfd_punsubscribe(perfd_t *p, const char *pattern)
{
	return ps_count_call(p, "punsubscribe", "subscribed", "pattern", pattern);
}

int perfd_publish(perfd_t *p, const char *channel, const void *payload,
		size_t plen)
{
	struct pc_tw w;
	char *res;
	size_t rn;
	long long n = 0;
	int rc;

	pc_tw_init(&w, NULL, 0);
	pc_tw_map(&w);
	pc_tw_key(&w, "channel");
	pc_tw_str(&w, channel);
	pc_tw_key(&w, "payload");
	pc_tw_bulk(&w, payload, plen);     /* bytes: no b64 leg (S317) */
	pc_tw_end(&w);
	if (pc_tw_done(&w) != 0) {
		pc_tw_free(&w);
		return err(p, "payload too large");
	}
	rc = roundtrip(p, "publish", w.b, w.n, &res, &rn);
	pc_tw_free(&w);
	if (rc != 0)
		return -1;
	if (reply_int(p, "publish", res, rn, "receivers", &n) != 0)
		return -1;
	return (int)n;
}

/* ---- pub/sub over UDP (PS5) ----------------------------------------------
 * lib/perfd_push.h has the datagram and the protocol.  The window is the
 * relay's idea (a counter is lost only when it leaves 64 behind the
 * highest seen unseen; inside the window a late one is simply accepted),
 * written again here for this library. */
#define PD_UDP_WAIT_PROBE   1          /* asked; the probe has not come */
#define PD_UDP_CONFIRMING   2          /* async: confirm submitted */
#define PD_UDP_ACTIVE       3
#define PD_UDP_FAILED       4

struct pd_udp {
	int fd;
	int state;
	int port;
	uint64_t stream;
	unsigned char key[PFP_KEY];
	struct sockaddr_in from;           /* the door: address and port */
	uint64_t high, bits;               /* window: bit i = high - i seen */
	unsigned since_ack;
	long long ack_due_ms, probe_deadline_ms;
	int pruned;
	unsigned long long received, gaps, duplicates, rejected, acks;
	unsigned char early[PFP_HDR + PFP_COOKIE + PFP_TAG];
	size_t early_len;                  /* async: a probe that beat the reply */
	unsigned char *tree;               /* a delivery as the NOTIFY tree the
	                                    * connection would carry; reused */
	size_t tcap;
};

static void udp_drop(perfd_t *p, int pruned)
{
	struct pd_udp *u = p->udp;

	if (!u)
		return;
	if (u->fd >= 0)
		close(u->fd);
	u->fd = -1;
	sodium_memzero(u->key, sizeof u->key);
	u->state = PD_UDP_FAILED;
	u->pruned = pruned;
}

/* a hex-text member of the reply map into @out, exactly @len bytes (the
 * stream id and the key travel as hex bulks - D8, so lib/perfd_push.h's
 * consumers see what they always saw) */
static int tv_hex(const struct pc_tv *v, const char *field,
		unsigned char *out, size_t len)
{
	int k = pc_tv_get(v, 0, field);
	size_t got = 0;

	if (k < 0 || v->n[k].type != 'b' || v->n[k].len != 2 * len ||
	    sodium_hex2bin(out, len, (const char *)v->n[k].p, 2 * len, NULL,
	        &got, NULL) != 0 || got != len)
		return -1;
	return 0;
}

static int udp_probe_in(perfd_t *p, const unsigned char *dg, size_t n);

static void udp_confirmed(perfd_t *p, uint64_t seq)
{
	struct pd_udp *u = p->udp;

	u->state = PD_UDP_ACTIVE;
	u->high = seq;
	u->bits = ~0ULL;                   /* what came before is not ours to count */
	u->since_ack = 0;
	u->ack_due_ms = now_ms_lib() + PFP_ACK_MS;
}

/* the replies to our own requests: the raw reply TREE (the library's
 * own calls are never rendered), on both the blocking and the async path */
static void udp_on_reply(char *result, size_t len, const char *errmsg,
		void *ctx)
{
	perfd_t *p = ctx;
	struct pd_udp *u = p->udp;
	struct pc_tv v;
	int parsed = 0;

	memset(&v, 0, sizeof v);
	if (result && !errmsg)
		parsed = pc_tv_parse(&v, (const unsigned char *)result, len) == 0 &&
			v.n[0].type == 'm';
	if (u && u->state == PD_UDP_WAIT_PROBE && !u->stream) {
		unsigned char sid[8];

		if (!parsed || tv_hex(&v, "stream", sid, 8) != 0 ||
		    tv_hex(&v, "key", u->key, PFP_KEY) != 0) {
			err(p, "pubsub_udp: %s", errmsg ? errmsg : "bad reply");
			udp_drop(p, 0);
		} else {
			u->stream = pfp_get64(sid);
			/* the daemon sends its first probe while it answers, so on
			 * an async handle the probe can be read before this reply */
			if (u->early_len) {
				size_t n = u->early_len;

				u->early_len = 0;
				if (udp_probe_in(p, u->early, n) > 0)
					u->rejected++;
			}
		}
	} else if (u && u->state == PD_UDP_CONFIRMING) {
		long long seq = 0;

		if (!parsed || pc_tv_get_int(&v, 0, "seq", &seq) != 0) {
			err(p, "pubsub_udp_confirm: %s", errmsg ? errmsg : "bad reply");
			udp_drop(p, 0);
		} else {
			udp_confirmed(p, (uint64_t)seq);
		}
	}
	pc_tv_free(&v);
	free(result);
}

static void udp_on_ack(char *result, size_t len, const char *errmsg,
		void *ctx)
{
	(void)len;
	(void)errmsg;
	(void)ctx;
	free(result);
}

static int submit_tree(perfd_t *p, const char *method,
		const unsigned char *tree, size_t tlen, perfd_reply_cb cb,
		void *ctx, int render);

/* send a request of ours: a round trip on a blocking handle, a submit on
 * an async one; the callback sees the reply tree either way.  0, or -1
 * with the error set */
static int udp_request(perfd_t *p, const char *method,
		const unsigned char *tree, size_t tlen, perfd_reply_cb cb)
{
	char *r;
	size_t n;

	if (p->async)
		return submit_tree(p, method, tree, tlen, cb, p, 0);
	if (roundtrip(p, method, tree, tlen, &r, &n) != 0)
		return -1;
	/* r is BORROWED (S284) from the handle's reply buffer, but a reply
	 * callback OWNS its result and frees it (perfd_reply_cb) - call_fire
	 * hands the async path's malloc'd reply over the same way.  Handing
	 * it r freed the handle's buffer, and the confirm's reply was then
	 * copied into freed memory: musl crashed in udppushtest, glibc
	 * aborted on "double free or corruption" (DESIGN 12iq).  One copy
	 * per UDP stream setup or teardown - not a per-operation path. */
	if (cb) {
		char *c = dup_reply(p, r, n);

		if (!c)
			return -1;
		cb(c, n, NULL, p);
	}
	return 0;
}

/* {"port": N} - the one parameter pubsub_udp takes */
static size_t udp_port_tree(unsigned char *buf, size_t cap, int port)
{
	struct pc_tw w;

	pc_tw_init(&w, buf, cap);
	pc_tw_map(&w);
	pc_tw_key(&w, "port");
	pc_tw_i64(&w, port);
	pc_tw_end(&w);
	return pc_tw_done(&w) == 0 ? w.n : 0;
}

int perfd_pubsub_udp(perfd_t *p, int port, int timeout_ms)
{
	struct pd_udp *u;
	struct sockaddr_in me;
	socklen_t sl = sizeof me;
	unsigned char params[32];
	size_t pn;

	if (!p || p->is_child)
		return -1;
	if (port < 0 || port > 65535)
		return err(p, "port must be 0..65535");
	if (p->udp)
		perfd_pubsub_udp_off(p);
	if (!p->udp) {
		p->udp = calloc(1, sizeof *p->udp);
		if (!p->udp)
			return err(p, "out of memory");
	}
	u = p->udp;
	{
		/* a new stream starts from nothing but keeps the delivery scratch */
		unsigned char *tree = u->tree;
		size_t tcap = u->tcap;

		memset(u, 0, sizeof *u);
		u->tree = tree;
		u->tcap = tcap;
	}
	u->fd = -1;
	sl = sizeof u->from;
	if (getpeername(p->fd, (struct sockaddr *)&u->from, &sl) != 0 ||
	    u->from.sin_family != AF_INET)
		return err(p, "UDP pushes need an IPv4 connection");
	u->fd = socket(AF_INET, SOCK_DGRAM, 0);
	memset(&me, 0, sizeof me);
	me.sin_family = AF_INET;
	me.sin_port = htons((uint16_t)port);
	if (u->fd < 0 || bind(u->fd, (struct sockaddr *)&me, sizeof me) != 0) {
		err(p, "cannot bind UDP port %d: %s", port, strerror(errno));
		udp_drop(p, 0);
		return -1;
	}
	sl = sizeof me;
	getsockname(u->fd, (struct sockaddr *)&me, &sl);
	u->port = ntohs(me.sin_port);
	fcntl(u->fd, F_SETFL, fcntl(u->fd, F_GETFL) | O_NONBLOCK);
	u->state = PD_UDP_WAIT_PROBE;
	u->probe_deadline_ms = now_ms_lib() + (timeout_ms > 0 ? timeout_ms : 3000);
	pn = udp_port_tree(params, sizeof params, u->port);
	if (udp_request(p, "pubsub_udp", params, pn, udp_on_reply) != 0 ||
	    (!p->async && u->state != PD_UDP_WAIT_PROBE)) {
		udp_drop(p, 0);
		return -1;
	}
	if (p->async)
		return 0;
	/* the probe proves the path; its cookie goes back on the connection */
	while (u->state == PD_UDP_WAIT_PROBE) {
		long long left = u->probe_deadline_ms - now_ms_lib();
		struct pollfd pf;

		if (left <= 0) {
			err(p, "no probe from the daemon in %d ms - UDP does not "
				"reach this client (a firewall or NAT between?)",
				timeout_ms > 0 ? timeout_ms : 3000);
			udp_drop(p, 0);
			perfd_pubsub_udp_off(p);
			return -1;
		}
		pf.fd = u->fd;
		pf.events = POLLIN;
		if (poll(&pf, 1, (int)left) > 0 && perfd_pubsub_udp_ready(p) < 0)
			return -1;
	}
	return u->state == PD_UDP_ACTIVE ? 0 : -1;
}

int perfd_pubsub_udp_fd(const perfd_t *p)
{
	return p && p->udp ? p->udp->fd : -1;
}

int perfd_pubsub_udp_off(perfd_t *p)
{
	int was;

	if (!p || !p->udp)
		return 0;
	was = p->udp->fd >= 0 && p->udp->state == PD_UDP_ACTIVE;
	udp_drop(p, 0);
	p->udp->state = 0;
	if (was && !p->poisoned) {
		unsigned char params[32];
		size_t pn = udp_port_tree(params, sizeof params, 0);

		udp_request(p, "pubsub_udp", params, pn,
			p->async ? udp_on_ack : NULL);
	}
	return 0;
}

int perfd_pubsub_udp_stats(const perfd_t *p, struct perfd_udp_stats *s)
{
	memset(s, 0, sizeof *s);
	if (!p || !p->udp)
		return 0;
	s->active = p->udp->state == PD_UDP_ACTIVE;
	s->pruned = p->udp->pruned;
	s->port = p->udp->port;
	s->received = p->udp->received;
	s->gaps = p->udp->gaps;
	s->duplicates = p->udp->duplicates;
	s->rejected = p->udp->rejected;
	s->acks = p->udp->acks;
	return 0;
}

/* a probe datagram from the door: open it and send its cookie back.  0 taken
 * (or a late probe, ignored), 1 not this stream's probe, -1 the stream failed */
static int udp_probe_in(perfd_t *p, const unsigned char *dg, size_t n)
{
	struct pd_udp *u = p->udp;
	unsigned char pt[64], nonce[12], params[96];
	unsigned long long ptlen = 0;
	char hex[2 * PFP_COOKIE + 1];
	struct pc_tw w;

	if (n < PFP_HDR + PFP_TAG || n > sizeof pt + PFP_HDR ||
	    pfp_get64(dg + 3) != u->stream)
		return 1;
	pfp_nonce(nonce, pfp_get64(dg + 11));
	if (crypto_aead_chacha20poly1305_ietf_decrypt(pt, &ptlen, NULL,
	        dg + PFP_HDR, (unsigned long long)n - PFP_HDR, dg, PFP_HDR,
	        nonce, u->key) != 0 || ptlen != PFP_COOKIE)
		return 1;
	if (u->state != PD_UDP_WAIT_PROBE)
		return 0;                      /* a late probe */
	/* the cookie goes back as hex text (D8), as it always did */
	sodium_bin2hex(hex, sizeof hex, pt, PFP_COOKIE);
	pc_tw_init(&w, params, sizeof params);
	pc_tw_map(&w);
	pc_tw_key(&w, "cookie");
	pc_tw_bulk(&w, hex, 2 * PFP_COOKIE);
	pc_tw_end(&w);
	u->state = PD_UDP_CONFIRMING;
	if (udp_request(p, "pubsub_udp_confirm", params, w.n, udp_on_reply) != 0 ||
	    (!p->async && (!p->udp || p->udp->state != PD_UDP_ACTIVE))) {
		udp_drop(p, 0);
		return -1;
	}
	return 0;
}

/* 1 new, 0 duplicate; counts what left the window unseen */
static int udp_window(struct pd_udp *u, uint64_t seq)
{
	if (seq > u->high) {
		uint64_t d = seq - u->high;

		if (d >= 64) {
			u->gaps += (uint64_t)(64 - __builtin_popcountll(u->bits)) +
				(d - 64);
			u->bits = 1;
		} else {
			u->gaps += d - (uint64_t)__builtin_popcountll(
				u->bits >> (64 - d));
			u->bits = (u->bits << d) | 1;
		}
		u->high = seq;
		return 1;
	}
	if (u->high - seq >= 64)
		return 1;                      /* already counted as a gap: late */
	if (u->bits & (1ULL << (u->high - seq)))
		return 0;
	u->bits |= 1ULL << (u->high - seq);
	return 1;
}

static void udp_deliver(perfd_t *p, const unsigned char *pt, size_t n)
{
	struct pd_udp *u = p->udp;
	size_t plen, clen, dlen, need;
	struct pc_tw w;

	if (n < 7)
		goto bad;
	plen = pfp_get16(pt + 1);
	if (n < 3 + plen + 2)
		goto bad;
	clen = pfp_get16(pt + 3 + plen);
	if (n < 3 + plen + 2 + clen + 4)
		goto bad;
	dlen = pfp_get32(pt + 3 + plen + 2 + clen);
	if (n != 3 + plen + 2 + clen + 4 + dlen)
		goto bad;
	u->received++;
	u->since_ack++;
	if (!p->ncb)
		return;
	/* the tree a delivery on the connection would have carried (D6):
	 * {"method": "message"|"pmessage", "params": {"pattern"?, "channel",
	 * "payload"}} - built here and rendered by the SAME function the
	 * connection's NOTIFY goes through, so the hook cannot tell them apart */
	need = 128 + plen + clen + dlen;
	if (need > u->tcap) {
		unsigned char *nt = realloc(u->tree, need);

		if (!nt)
			return;
		u->tree = nt;
		u->tcap = need;
	}
	pc_tw_init(&w, u->tree, u->tcap);
	pc_tw_map(&w);
	pc_tw_key(&w, "method");
	pc_tw_str(&w, pt[0] ? "pmessage" : "message");
	pc_tw_key(&w, "params");
	pc_tw_map(&w);
	if (pt[0]) {
		pc_tw_key(&w, "pattern");
		pc_tw_bulk(&w, pt + 3, plen);
	}
	pc_tw_key(&w, "channel");
	pc_tw_bulk(&w, pt + 3 + plen + 2, clen);
	pc_tw_key(&w, "payload");
	pc_tw_bulk(&w, pt + 3 + plen + 2 + clen + 4, dlen);
	pc_tw_end(&w);
	pc_tw_end(&w);
	if (pc_tw_done(&w) == 0)
		notify_hook(p, w.b, w.n);
	return;
bad:
	u->rejected++;
}

int perfd_pubsub_udp_ready(perfd_t *p)
{
	struct pd_udp *u = p ? p->udp : NULL;
	unsigned char dg[PFP_MAX + 64], pt[PFP_MAX + 64], nonce[12];
	int rounds, delivered = 0;
	long long now;

	if (!u || u->fd < 0)
		return u && u->pruned ? -1 : 0;
	for (rounds = 0; rounds < 1024; rounds++) {
		struct sockaddr_in src;
		socklen_t sl = sizeof src;
		ssize_t r = recvfrom(u->fd, dg, sizeof dg, 0,
			(struct sockaddr *)&src, &sl);
		unsigned long long ptlen = 0;
		uint64_t seq;

		if (r < 0)
			break;
		if (src.sin_addr.s_addr != u->from.sin_addr.s_addr ||
		    src.sin_port != u->from.sin_port || r < PFP_HDR + PFP_TAG ||
		    dg[0] != PFP_MAGIC || dg[1] != PFP_VERSION) {
			u->rejected++;
			continue;
		}
		if (!u->stream) {
			/* async: the reply naming the stream has not been read
			 * yet - keep a probe for it, it cannot be checked now */
			if (u->state == PD_UDP_WAIT_PROBE && dg[2] == PFP_PROBE &&
			    (size_t)r <= sizeof u->early) {
				memcpy(u->early, dg, (size_t)r);
				u->early_len = (size_t)r;
			} else {
				u->rejected++;
			}
			continue;
		}
		if (dg[2] == PFP_PROBE) {
			int rc = udp_probe_in(p, dg, (size_t)r);

			if (rc < 0)
				return -1;
			if (rc > 0)
				u->rejected++;
			continue;
		}
		if (pfp_get64(dg + 3) != u->stream) {
			u->rejected++;
			continue;
		}
		seq = pfp_get64(dg + 11);
		pfp_nonce(nonce, seq);
		if (crypto_aead_chacha20poly1305_ietf_decrypt(pt, &ptlen, NULL,
		        dg + PFP_HDR, (unsigned long long)r - PFP_HDR, dg, PFP_HDR,
		        nonce, u->key) != 0) {
			u->rejected++;         /* not sealed by the daemon */
			continue;
		}
		if (dg[2] != PFP_MESSAGE || u->state != PD_UDP_ACTIVE)
			continue;
		if (!udp_window(u, seq)) {
			u->duplicates++;
			continue;
		}
		udp_deliver(p, pt, (size_t)ptlen);
		delivered++;
		if (!p->udp || p->udp->fd < 0)
			break;                     /* the hook turned it off */
	}
	u = p->udp;
	if (!u || u->fd < 0)
		return delivered;
	now = now_ms_lib();
	if (u->state == PD_UDP_WAIT_PROBE && p->async &&
	    now >= u->probe_deadline_ms) {
		err(p, "no probe from the daemon - UDP does not reach this client");
		udp_drop(p, 0);
		return -1;
	}
	if (u->state == PD_UDP_ACTIVE &&
	    (u->since_ack >= PFP_ACK_EVERY || now >= u->ack_due_ms)) {
		unsigned char params[32];
		struct pc_tw w;

		pc_tw_init(&w, params, sizeof params);
		pc_tw_map(&w);
		pc_tw_key(&w, "seq");
		pc_tw_i64(&w, (long long)u->high);
		pc_tw_end(&w);
		u->since_ack = 0;
		u->ack_due_ms = now + PFP_ACK_MS;
		if (udp_request(p, "pubsub_udp_ack", params, w.n,
		        p->async ? udp_on_ack : NULL) != 0)
			return -1;
		if (p->udp)
			p->udp->acks++;
	}
	return delivered;
}

