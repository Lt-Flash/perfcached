/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov
 * Part of libperfd - see lib/LICENSE.  This file must stay
 * free of src/core includes; tools/sync-libperfd.sh exports
 * exactly the MIT set to consumers. */
/*
 * perfd-connect.inc - part of perfd.c, which #includes it in order: one translation
 * unit, so every static stays file-local and consumers still compile
 * perfd.c alone.  Not a compilation unit of its own.
 *
 * Holds connect and free: the Noise handshake, socket setup and the handle
 * lifecycle.
 */
/* ---- notifications ------------------------------------------------------ */

void perfd_set_notify(perfd_t *p, perfd_notify_cb cb, void *ctx)
{
	p->ncb = cb;
	p->nctx = ctx;
}

/* ---- connect / free ----------------------------------------------------- */

static int psk_for(const char *secret, uint8_t out[PC_NOISE_KEYLEN]);

static int noise_handshake(perfd_t *p, const char *secret)
{
	uint8_t psk[PC_NOISE_KEYLEN], prologue[1] = { PC_PRIN_CLIENT };
	uint8_t ver = PC_CLIENT_VER;
	struct pc_handshake hs;
	uint8_t msg[3 + PERFD_HS_MAX], m2[PERFD_HS_MAX], hdr[2];     /* S369 */
	size_t mlen = 0, plen = 0, cl;
	int rc = -1;

	/* S354: from the process-wide cache, as the async connect does -
	 * deriving here (Argon2id: ~100 ms of CPU and a 64 MiB populated
	 * mapping) made every blocking connect, standby and reconnect pay it */
	if (psk_for(secret, psk) != 0)
		return -1;
	pc_hs_init_initiator(&hs, prologue, 1);
	if (pc_hs_write_msg1(&hs, psk, &ver, 1, msg + 3, &mlen) != 0)
		goto out;
	msg[0] = (uint8_t)(1 + mlen);
	msg[1] = (uint8_t)((1 + mlen) >> 8);
	msg[2] = PC_PRIN_CLIENT;
	if (io_all(p, 1, msg, 3 + mlen) != 0)
		goto out;
	if (io_all(p, 0, hdr, 2) != 0)
		goto out;
	cl = (size_t)hdr[0] | ((size_t)hdr[1] << 8);
	if (cl == 0 || cl > sizeof m2 || io_all(p, 0, m2, cl) != 0)
		goto out;
	if (pc_hs_read_msg2(&hs, m2, cl, NULL, &plen, &p->cs_send,
	        &p->cs_recv) != 0)
		goto out;
	p->encrypted = 1;
	rc = 0;
out:
	sodium_memzero(psk, sizeof psk);
	return rc;
}

/* S104: keep a link honest while it is idle.  The kernel probes after
 * @idle_s of silence, every ten seconds, three times, so a peer that
 * vanished without a FIN fails the next send at once instead of after
 * the io timeout, and a middlebox that ages idle flows sees traffic. */
static void sock_keepalive(int fd, int idle_s)
{
	int one = 1, intvl = idle_s < 10 ? (idle_s > 0 ? idle_s : 1) : 10,
	    cnt = 3;

	if (idle_s <= 0)
		return;
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
#ifdef TCP_KEEPIDLE
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle_s, sizeof idle_s);
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
#endif
}

static int sock_connect(const char *host, int port, const char *unixp,
		int timeout_ms)
{
	int fd = -1;

	if (unixp) {
		struct sockaddr_un sa;

		fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd < 0)
			return -1;
		memset(&sa, 0, sizeof sa);
		sa.sun_family = AF_UNIX;
		snprintf(sa.sun_path, sizeof sa.sun_path, "%s", unixp);
		if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
			close(fd);
			return -1;
		}
		return fd;
	}
	{
		struct addrinfo hints, *res, *ai;
		char ps[16];
		int one = 1;

		(void)timeout_ms;          /* v1: default kernel timeout */
		memset(&hints, 0, sizeof hints);
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		snprintf(ps, sizeof ps, "%d", port);
		if (getaddrinfo(host, ps, &hints, &res) != 0)
			return -1;
		for (ai = res; ai; ai = ai->ai_next) {
			fd = socket(ai->ai_family, ai->ai_socktype, 0);
			if (fd < 0)
				continue;
			if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
				break;
			close(fd);
			fd = -1;
		}
		freeaddrinfo(res);
		if (fd >= 0)
			setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one,
				sizeof one);
		return fd;
	}
}

/* S34: a freshly connected handle learns the fleet, opens its standbys
 * and settles onto the member its policy prefers.  All of it is best
 * effort: a standalone daemon answers no members, an unreachable peer
 * is simply skipped, and the handle works exactly as before either
 * way. */
static perfd_t *conn_ready(perfd_t *p)
{
	if (p->is_child || !p->want_spares)
		return p;
	/* an independent random start per client is what makes
	 * round-robin spread without any coordination (par 5.6) */
	p->rr = (unsigned int)(now_ms_lib() ^ (long long)(uintptr_t)p ^
		((long long)getpid() << 7));
	if (learn_members(p) > 0) {
		/* parse_members set active_node from the "self" flag: the
		 * node answering us is the one we are on */
		warm_spares(p);
		apply_policy(p);
	}
	return p;
}

/* allocate a handle and copy the options into it.  Shared by the
 * blocking and the event-loop connect (S32) so the two cannot drift on
 * what an option means. */
static perfd_t *pd_alloc(const perfd_opts *opts)
{
	static const perfd_opts defaults = { 0 };
	const perfd_opts *o = opts ? opts : &defaults;
	int nsec = 0, i;
	perfd_t *p;

	if (sodium_init() < 0) {
		snprintf(g_connect_err, sizeof g_connect_err,
			"libsodium init failed");
		return NULL;
	}
	while (o->secrets && o->secrets[nsec])
		nsec++;

	p = calloc(1, sizeof *p);
	if (!p) {
		snprintf(g_connect_err, sizeof g_connect_err,
			"out of memory");
		return NULL;
	}
	p->io_ms = o->io_timeout_ms > 0 ? o->io_timeout_ms : DEF_TIMEOUT_MS;
	p->eager_push = o->eager_push != 0;
	p->fd = -1;
	/* S34: remember how to open standbys (secrets COPIED - the
	 * caller's array need not outlive the handle) */
	p->policy = o->policy;
	/* S104: the default is every member (0 and -1 alike); only
	 * PERFD_SPARES_NONE asks for the bare handle */
	p->want_spares = o->spares == PERFD_SPARES_NONE ? 0 :
		o->spares == 0 ? -1 : o->spares;
	/* S104: 0 = the default, -1 = off, else as given */
	p->keepalive_s = o->keepalive_s > 0 ? o->keepalive_s :
		o->keepalive_s < 0 ? 0 : DEF_KEEPALIVE_S;
	p->idle_ping_ms = o->idle_ping_ms > 0 ? o->idle_ping_ms :
		o->idle_ping_ms < 0 ? 0 : DEF_IDLE_PING_MS;
	p->route_want = o->route_keys;
	/* S295: the holder table, once; without it the option is off */
	if (o->route_hints) {
		p->hints = calloc(HINT_SLOTS, sizeof *p->hints);
		p->route_hints = p->want_hh = p->hints != NULL;
	}
	p->ignore_push = o->ignore_member_push != 0;
	p->refresh_ms = o->refresh_ms > 0 ? o->refresh_ms : 30000;
	p->o_connect_ms = o->connect_timeout_ms;
	p->o_io_ms = o->io_timeout_ms;
	for (i = 0; i < nsec && i < PERFD_MAX_SECRETS; i++) {
		p->sec[i] = strdup(o->secrets[i]);
		if (p->sec[i])
			p->nsec++;
	}
	return p;
}

static perfd_t *do_connect(const char *host, int port, const char *unixp,
		const perfd_opts *opts)
{
	static const perfd_opts defaults = { 0 };
	const perfd_opts *o = opts ? opts : &defaults;
	int io_ms = o->io_timeout_ms > 0 ? o->io_timeout_ms : DEF_TIMEOUT_MS;
	int nsec = 0, i;
	perfd_t *p;

	while (o->secrets && o->secrets[nsec])
		nsec++;
	p = pd_alloc(opts);
	if (!p)
		return NULL;

	/* the secret LIST: try each in order on a fresh connection -
	 * rotation is add-new/drain-old with no client downtime */
	for (i = 0; i < (nsec ? nsec : 1); i++) {
		struct timeval tv = { io_ms / 1000, (io_ms % 1000) * 1000L };

		p->fd = sock_connect(host, port, unixp,
			o->connect_timeout_ms);
		if (p->fd < 0) {
			snprintf(g_connect_err, sizeof g_connect_err,
				"cannot connect: %s", strerror(errno));
			free(p);
			return NULL;
		}
		setsockopt(p->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
		setsockopt(p->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
		sock_keepalive(p->fd, p->keepalive_s);                  /* S104 */
		if (!nsec)
			return conn_ready(p);  /* plaintext */
		if (noise_handshake(p, o->secrets[i]) == 0)
			return conn_ready(p);
		close(p->fd);
		p->fd = -1;
		p->poisoned = 0;
	}
	snprintf(g_connect_err, sizeof g_connect_err,
		"handshake failed with every secret (%d tried)", nsec);
	free(p);
	return NULL;
}

perfd_t *perfd_connect(const char *host, int port, const perfd_opts *opts)
{
	return do_connect(host, port, NULL, opts);
}

perfd_t *perfd_connect_unix(const char *path, const perfd_opts *opts)
{
	return do_connect(NULL, 0, path, opts);
}

void perfd_free(perfd_t *p)
{
	struct early *e;
	int i;

	if (!p)
		return;
	for (i = 0; i < p->nspare; i++)      /* S34: standbys go too */
		perfd_free(p->spare[i]);
	p->nspare = 0;
	for (i = 0; i < p->nsec; i++) {
		/* S59: the lib re-derives the PSK per handshake, so the
		 * raw copies must live until close - which is now */
		if (p->sec[i])
			sodium_memzero(p->sec[i], strlen(p->sec[i]));
		free(p->sec[i]);
	}
	if (p->udp) {
		udp_drop(p, 0);
		free(p->udp->tree);
		free(p->udp);
	}
	if (p->fd >= 0)
		close(p->fd);
	while ((e = p->early)) {
		p->early = e->next;
		free(e->result);
		free(e);
	}
	free(p->rbuf);
	free(p->xbuf);                     /* S369 */
	free(p->rsc);                      /* S284 */
	free(p->wsc);
	free(p->tvn);
	free(p->hints);                    /* S295 */
	free(p->q);
	free(p->ids);
	free(p->w);
	free(p->cin);
	free(p->calls);
	free(p->nj);
	free(p);
}

/* ======================================================================
 * S32 — the event-loop surface
 *
 * Everything above this line blocks by design and is unchanged.  What
 * follows is the same protocol driven by someone else's event loop: the
 * consumer owns the loop, watches perfd_fd() for perfd_events(), and
 * hands control back in through the two readiness entry points.  The
 * library starts no thread and never calls back from one.
 *
 * The wire is shared, not reimplemented: requests are built by the same
 * enqueue_cmd() / enqueue_bin(), replies are extracted by the same
 * take_msg().  The only
 * genuinely new machinery is (a) an output buffer, because a
 * non-blocking write may take a prefix, (b) incremental Noise record
 * framing, because a record can arrive in pieces, and (c) an id ->
 * callback table replacing the in-order FIFO.
 * ====================================================================== */

/* Argon2id at INTERACTIVE limits costs ~100 ms of CPU.  Paying that per
 * connect is merely slow for a blocking client, but for an event loop it
 * is a stall - and RECONNECT happens inside the loop, which is exactly
 * when a consumer can least afford it.  So derived PSKs are cached for
 * the life of the process, keyed by the secret.  The secret itself is
 * already held for the life of a handle (spares and failover need it),
 * so this keeps nothing that was not kept before. */
/* S355: single-flight.  The first caller for a secret derives while the
 * others for the SAME secret wait on g_psk_cond - before, every thread that
 * connected while the first derive ran did its own (a 100-thread client
 * mapped 100 x 64 MiB at once and was OOM-killed).  Different secrets do not
 * wait for one another.  An entry is PSK_DERIVING while its first caller
 * runs Argon2id outside the lock, PSK_READY once the key is in it. */
#define PSK_FREE     0
#define PSK_READY    1
#define PSK_DERIVING 2
static pthread_mutex_t g_psk_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_psk_cond = PTHREAD_COND_INITIALIZER;
static pthread_once_t g_psk_once = PTHREAD_ONCE_INIT;
static struct psk_ent {
	char sec[192];
	uint8_t psk[PC_NOISE_KEYLEN];
	int used;                              /* PSK_* */
} g_psk[8];

/* S355: a fork while another thread derives would leave the child an entry
 * marked PSK_DERIVING that nothing will ever finish - its connects would
 * wait for good.  Hold the lock across fork(); in the child, start the lock
 * and the condition afresh and free what was being derived (the child
 * derives it itself when it needs it).  READY keys are inherited, which is
 * the point of perfd_psk_prepare(). */
static void psk_atfork_prepare(void)
{
	pthread_mutex_lock(&g_psk_lock);
}

static void psk_atfork_parent(void)
{
	pthread_mutex_unlock(&g_psk_lock);
}

static void psk_atfork_child(void)
{
	int i;

	for (i = 0; i < (int)(sizeof g_psk / sizeof g_psk[0]); i++)
		if (g_psk[i].used == PSK_DERIVING) {
			g_psk[i].used = PSK_FREE;
			g_psk[i].sec[0] = 0;
		}
	pthread_mutex_init(&g_psk_lock, NULL);
	pthread_cond_init(&g_psk_cond, NULL);
}

static void psk_once(void)
{
	pthread_atfork(psk_atfork_prepare, psk_atfork_parent,
		psk_atfork_child);
}

static int psk_for(const char *secret, uint8_t out[PC_NOISE_KEYLEN])
{
	int i, slot, rc;
	size_t n = strlen(secret);

	if (n >= sizeof g_psk[0].sec)          /* too long to cache; derive */
		return pc_psk_derive(secret, n, PC_PRIN_CLIENT, out);

	pthread_once(&g_psk_once, psk_once);
	pthread_mutex_lock(&g_psk_lock);
	for (;;) {
		int busy = 0;

		slot = -1;
		for (i = 0; i < (int)(sizeof g_psk / sizeof g_psk[0]); i++) {
			if (g_psk[i].used != PSK_FREE &&
			    !strcmp(g_psk[i].sec, secret)) {
				if (g_psk[i].used == PSK_READY) {
					memcpy(out, g_psk[i].psk,
						PC_NOISE_KEYLEN);
					pthread_mutex_unlock(&g_psk_lock);
					return 0;
				}
				busy = 1;              /* being derived */
			} else if (g_psk[i].used == PSK_FREE && slot < 0) {
				slot = i;
			}
		}
		if (!busy)
			break;
		pthread_cond_wait(&g_psk_cond, &g_psk_lock);
	}
	/* this caller derives; a full cache derives without keeping it */
	if (slot >= 0) {
		snprintf(g_psk[slot].sec, sizeof g_psk[slot].sec, "%s", secret);
		g_psk[slot].used = PSK_DERIVING;
	}
	pthread_mutex_unlock(&g_psk_lock);

	rc = pc_psk_derive(secret, n, PC_PRIN_CLIENT, out);

	if (slot >= 0) {
		pthread_mutex_lock(&g_psk_lock);
		if (rc == 0) {
			memcpy(g_psk[slot].psk, out, PC_NOISE_KEYLEN);
			g_psk[slot].used = PSK_READY;
		} else {                       /* a waiter will try again */
			g_psk[slot].used = PSK_FREE;
			g_psk[slot].sec[0] = 0;
		}
		pthread_cond_broadcast(&g_psk_cond);
		pthread_mutex_unlock(&g_psk_lock);
	}
	return rc == 0 ? 0 : -1;
}

int perfd_psk_prepare(const char *const *secrets)
{
	uint8_t k[PC_NOISE_KEYLEN];
	int rc = 0;

	for (; secrets && *secrets; secrets++) {
		if (strlen(*secrets) >= sizeof g_psk[0].sec)
			continue;                  /* never cached: nothing to prepare */
		if (psk_for(*secrets, k) != 0)
			rc = -1;
	}
	sodium_memzero(k, sizeof k);
	return rc;
}

