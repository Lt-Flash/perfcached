/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov
 * Part of libperfd - see lib/LICENSE.  This file must stay
 * free of src/core includes; tools/sync-libperfd.sh exports
 * exactly the MIT set to consumers. */
/* perfd-fleet.inc - part of perfd.c, #included in order (one translation unit): the fleet - the
 * member list, per-key routing, the proxy holder table, standbys and failover. */
/* ---- cluster awareness: STANDBY links to swap onto when a node dies.  Nothing here is
 * load-bearing for correctness: a stale view costs a hop at worst; the daemon re-checks owners. */

static long long now_ms_lib(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* the verbs that may be REPLAYED after a failover: the store is absolute-upsert, so set/del/expire
 * and jset/jdel twice are once; add/sub and jincr would double, so they surface the error */
static int method_idempotent(const char *m)
{
	static const char *const ok[] = { "get", "set", "del", "exists",
		"ttl", "expire", "mget", "mset", "keys", "scan", "ping",
		"stats", "members", "jget", "jset", "jdel", NULL };
	int i;

	for (i = 0; ok[i]; i++)
		if (!strcmp(m, ok[i]))
			return 1;
	return 0;
}

/* a reply that did not come says nothing of whether a write was applied: the node may still apply
 * it after a replay elsewhere and after the caller's next write, so only a read is sent again */
static int method_reads(const char *m)
{
	static const char *const rd[] = { "get", "exists", "ttl", "mget", "keys", "scan",
		"ping", "stats", "members", "jget", NULL };
	int i;

	for (i = 0; rd[i]; i++)
		if (!strcmp(m, rd[i]))
			return 1;
	return 0;
}

static int bin_verb_reads(int verb)
{
	return verb == BV_PING || verb == BV_GET || verb == BV_EXISTS || verb == BV_TTL;
}

static int bin_verb_idempotent(int verb)
{
	/* the counter verbs double on a replay; a conditional set replayed after its reply was lost
	 * reads its OWN write and answers "declined" - a lock the caller holds, reported as lost */
	return verb != BV_ADD && verb != BV_SUB && verb != BV_SETNX &&
		verb != BV_SETXX && verb != BV_RLHIT &&  /* a hit twice */
		verb != BV_HCMD;               /* HINCRBY twice is not once */
}

/* parse a members reply tree into p->mem[] */
static int parse_members(perfd_t *p, const unsigned char *tree, size_t tn)
{
	struct pc_tv v;
	int tm, i, n = 0, cnt;

	memset(&v, 0, sizeof v);
	if (pc_tv_parse(&v, tree, tn) != 0 || v.n[0].type != 'm') {
		pc_tv_free(&v);
		return -1;
	}
	tm = pc_tv_get(&v, 0, "members");
	if (tm < 0 || v.n[tm].type != 'a') {
		pc_tv_free(&v);
		return -1;
	}
	/* the daemon's version rides the same reply; absent on a
	 * daemon older than the field, and then it stays "" */
	{
		int vl = pc_tv_get_str(&v, 0, "version", p->srv_version,
			sizeof p->srv_version - 1);

		if (vl >= 0)
			p->srv_version[vl] = 0;
	}
	cnt = (int)v.n[tm].len;
	for (i = 0; i < cnt && n < PERFD_MAX_MEMBERS; i++) {
		int m = pc_tv_at(&v, tm, (unsigned int)i), al;
		long long port, node, mb;
		char state[16];
		int sl;

		if (m < 0 || v.n[m].type != 'm')
			continue;
		al = pc_tv_get_str(&v, m, "addr", p->mem[n].addr,
			sizeof p->mem[n].addr - 1);
		if (al < 0 || pc_tv_get_int(&v, m, "port", &port) != 0 ||
		        pc_tv_get_int(&v, m, "node", &node) != 0)
			continue;
		p->mem[n].addr[al] = 0;
		p->mem[n].port = (int)port;
		p->mem[n].node = (int)node;
		p->mem[n].master = pc_tv_get_bool(&v, m, "master");
		/* the node answering us marks itself "self" - that is where
		 * this handle currently is */
		if (pc_tv_get_bool(&v, m, "self"))
			p->active_node = (int)node;
		p->mem[n].free_mb = pc_tv_get_int(&v, m, "free_mb", &mb) == 0
			? (int)mb : 0;
		p->mem[n].total_mb = pc_tv_get_int(&v, m, "total_mb", &mb) == 0
			? (int)mb : 0;
		/* "ready" | "recovering" | "starting" | "draining"; DRAINING is usable: being
		 * emptied, not doubted, it still answers for what it holds */
		sl = pc_tv_get_str(&v, m, "state", state, sizeof state - 1);
		p->mem[n].ready = sl < 0 ||
			(sl == 5 && !memcmp(state, "ready", 5)) ||
			(sl == 8 && !memcmp(state, "draining", 8));
		p->mem[n].draining = sl == 8 && !memcmp(state, "draining", 8);
		p->mem[n].down = 0;
		p->mem[n].returning = 0;
		if (!p->mem[n].port)
			continue;      /* a node that cannot be dialled */
		n++;
	}
	/* the ROUTING CONTRACT: route only when the daemon names an algorithm known here, in a mode
	 * where routing means something; else it stays off and the forward is paid */
	{
		int tr = pc_tv_get(&v, 0, "routing");

		p->route_ok = 0;
		if (tr >= 0 && v.n[tr].type == 'm') {
			long long ep, k, cp, mt, ms;
			int their_epoch = pc_tv_get_int(&v, tr, "epoch", &ep) == 0
				? (int)ep : 0;
			int epoch_ok = their_epoch == PERFD_WIRE_EPOCH;
			int algo_ok = pc_tv_streq(&v, pc_tv_get(&v, tr, "algo"),
				PERFD_ROUTE_ALGO);
			/* `spread` routes too, its holders the ranking's top K (shard is K = 1); a
			 * library not knowing a mode leaves routing off */
			int tmd = pc_tv_get(&v, tr, "mode");
			int mode_ok = pc_tv_streq(&v, tmd, "shard") ||
				pc_tv_streq(&v, tmd, "spread") ||
				pc_tv_streq(&v, tmd, "store");
			int tmap;

			p->route_k = pc_tv_get_int(&v, tr, "replicas", &k) == 0
				? (int)k : 0;
			if (p->route_k < 1)
				p->route_k = 1;
			if (pc_tv_get_int(&v, tr, "cport", &cp) == 0)
				p->route_cport = (int)cp;
			/* which map this list belongs to */
			tmap = pc_tv_get(&v, tr, "map");
			p->map_known = tmap >= 0 &&
				pc_tv_get_int(&v, tmap, "term", &mt) == 0 &&
				pc_tv_get_int(&v, tmap, "seq", &ms) == 0;
			if (p->map_known) {
				p->map_term = (unsigned int)mt;
				p->map_seq = (unsigned int)ms;
			}
			p->route_epoch = their_epoch;
			p->route_ok = p->route_want && algo_ok && mode_ok &&
				epoch_ok && p->route_cport > 0;
		}
	}
	pc_tv_free(&v);
	p->nmem = n;
	fleet_seen(p);
	return n;
}

/* ---- per-key routing: shard sends a key to its computable owner, store to ONE node per key as its
 * de-facto single writer; proxy is not routed.  A wrong guess costs a hop, never correctness. */

/* the placement input: the Redis SLOT of the KEY ALONE - a RESP client knows no collections and the
 * daemon's pc_shard_owner() ignores them, so mixing col in would part ways with Redis clients */
static unsigned long long route_keyhash(const char *col, const char *key)
{
	(void)col;
	return pc_key_slot(key, strlen(key));
}

/* xorshift over the handle's own cursor: independent per client, which
 * is what makes uncoordinated spreading work */
static unsigned int pd_rand(perfd_t *p)
{
	unsigned int x = p->rr ? p->rr : 2463534242u;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	p->rr = x;
	return x;
}

/* the @n-th ranked member for @key (0 = the owner), or -1; selection rather than a sort, as n is at
 * most the small copy factor */
static int route_rank_nth(perfd_t *p, const char *col, const char *key, int n)
{
	unsigned long long kh, best = 0;
	unsigned short nport;
	int i, r, pick = -1;
	int taken[PERFD_MAX_MEMBERS];

	if (!p->route_ok || p->nmem < 2 || !col || !key || n < 0)
		return -1;
	if (n >= p->nmem)
		return -1;
	memset(taken, 0, sizeof taken);
	kh = route_keyhash(col, key);
	nport = htons((unsigned short)p->route_cport);
	for (r = 0; r <= n; r++) {
		pick = -1;
		best = 0;
		for (i = 0; i < p->nmem; i++) {
			unsigned long long h;
			struct in_addr ia;

			if (taken[i] || !pd_usable(&p->mem[i]))
				continue;
			if (!inet_aton(p->mem[i].addr, &ia))
				continue;
			h = pc_hrw_mix(kh, (unsigned int)ia.s_addr, nport);
			if (pick < 0 || h > best) {
				best = h;
				pick = i;
			}
		}
		if (pick < 0)
			return -1;
		taken[pick] = 1;
	}
	return pick;
}

/* the member that should hold @key, or -1 when routing does not apply */
static int route_owner(perfd_t *p, const char *col, const char *key)
{
	unsigned long long kh, best = 0;
	unsigned short nport;
	int i, owner = -1;

	if (!p->route_ok || p->nmem < 2 || !col || !key)
		return -1;
	kh = route_keyhash(col, key);
	nport = htons((unsigned short)p->route_cport);
	for (i = 0; i < p->nmem; i++) {
		unsigned long long h;
		struct in_addr ia;

		if (!pd_usable(&p->mem[i]))
			continue;
		if (!inet_aton(p->mem[i].addr, &ia))
			continue;
		h = pc_hrw_mix(kh, (unsigned int)ia.s_addr, nport);
		if (owner < 0 || h > best) {
			best = h;
			owner = i;
		}
	}
	return owner;
}

/* which member should hold @key, or -1 when routing does not apply: for async callers, who drive
 * one fd per handle and so keep a handle per node themselves (address: perfd_member_info) */
int perfd_owner_of(perfd_t *p, const char *col, const char *key)
{
	return p ? route_owner(p, col, key) : -1;
}

/* a standby died under a routed request: drop it, and mark its member down so route_owner()
 * picks the next-best node */
static void route_retire(perfd_t *p, perfd_t *dead)
{
	int i, j;

	for (i = 0; i < p->nmem; i++)
		if (p->mem[i].node == dead->active_node)
			p->mem[i].down = 1;
	fleet_seen(p);
	for (i = 0; i < p->nspare; i++)
		if (p->spare[i] == dead) {
			perfd_free(dead);
			for (j = i; j < p->nspare - 1; j++)
				p->spare[j] = p->spare[j + 1];
			p->nspare--;
			return;
		}
}

/* ---- the holder table: key -> the node that last answered for it, until its TTL; allocated at
 * connect, 4-way set-associative, a new key in front pushing the set's oldest out */
static unsigned long long hint_hash(const char *col, const char *key)
{
	unsigned long long h = 1469598103934665603ULL;
	const unsigned char *s;

	for (s = (const unsigned char *)col; *s; s++)
		h = (h ^ *s) * 1099511628211ULL;
	h = (h ^ 0xFF) * 1099511628211ULL;     /* col and key apart */
	for (s = (const unsigned char *)key; *s; s++)
		h = (h ^ *s) * 1099511628211ULL;
	return h | 1;                          /* 0 marks an empty slot */
}

static struct hint_e *hint_set(perfd_t *p, unsigned long long h)
{
	return &p->hints[(h & (HINT_SLOTS / HINT_WAYS - 1)) * HINT_WAYS];
}

static struct hint_e *hint_find(perfd_t *p, unsigned long long h)
{
	struct hint_e *s = hint_set(p, h);
	int w;

	for (w = 0; w < HINT_WAYS; w++)
		if (s[w].h == h && s[w].node)
			return &s[w];
	return NULL;
}

static int hint_lookup(perfd_t *p, unsigned long long h)
{
	struct hint_e *e = hint_find(p, h);

	if (!e)
		return 0;
	if (e->deadline_ms && now_ms_lib() >= e->deadline_ms) {
		e->node = 0;                   /* the record is gone everywhere */
		return 0;
	}
	return e->node;
}

static void hint_put(perfd_t *p, unsigned long long h, int node,
		long long ttl_s)
{
	struct hint_e *e = hint_find(p, h);

	if (!e) {
		/* in front; the set's last (oldest) way falls out */
		e = hint_set(p, h);
		memmove(e + 1, e, sizeof *e * (HINT_WAYS - 1));
	}
	e->h = h;
	e->node = node;
	e->deadline_ms = ttl_s > 0 ? now_ms_lib() + ttl_s * 1000 : 0;
}

static void hint_drop(perfd_t *p, unsigned long long h)
{
	struct hint_e *e = hint_find(p, h);

	if (e)
		e->node = 0;
}

/* after a data call answered on @u: learn from its hint, or confirm the
 * one that routed it */
static void hint_after(perfd_t *p, perfd_t *u, int verb, const char *col,
		const char *key, long long ttl, const char *out, size_t outlen)
{
	unsigned long long h;

	if (!col || !key) {                    /* a ping: no key to learn */
		u->hh_got = 0;
		return;
	}
	h = hint_hash(col, key);
	if (u->hh_got) {
		long long t = u->hh_ttl == HINT_TTL_UNKNOWN ?
			(verb == BV_SET || verb == BV_SETNX || verb == BV_SETXX ?
			 ttl : 0) : (long long)u->hh_ttl;

		u->hh_got = 0;
		if (p->hinted && u->hh_node != p->hinted)
			p->hint_stale++;       /* it had moved: one extra hop */
		hint_put(p, h, u->hh_node, t);
		return;
	}
	if (verb == BV_DEL || (verb == BV_GET && outlen >= 1 && out[0] == 0)) {
		hint_drop(p, h);               /* nothing left to route to */
		return;
	}
	if (p->hinted) {
		p->hint_hits++;                /* answered where the hint said */
		if (verb == BV_SET || verb == BV_SETNX || verb == BV_SETXX)
			hint_put(p, h, p->hinted, ttl);
	}
}

/* the handle that should carry this request: ours, a standby already open to the owner, or ours
 * anyway when not connected to the owner, paying the forward */
static perfd_t *route_pick(perfd_t *p, const char *col, const char *key)
{
	int owner, i, r;

	if (p->is_child)
		return p;
	p->hinted = 0;
	/* a key whose holder a reply named goes straight there */
	if (p->route_hints && p->hints && col && key) {
		unsigned long long h = hint_hash(col, key);
		int hn = hint_lookup(p, h);

		if (hn) {
			p->hinted = hn;
			if (hn == p->active_node)
				return p;
			for (i = 0; i < p->nspare; i++)
				if (p->spare[i]->active_node == hn)
					return p->spare[i];
			/* no link to that member (it left, or is not up yet):
			 * forget the entry and go the default way */
			p->hinted = 0;
			hint_drop(p, h);
		}
	}
	/* under spread any of the K holders can answer: the best-ranked one already connected
	 * (shard is K = 1); a second-ranked holder beats forwarding via a node holding nothing */
	for (r = 0; r < p->route_k; r++) {
		owner = route_rank_nth(p, col, key, r);
		if (owner < 0)
			break;
		if (p->mem[owner].node == p->active_node)
			return p;              /* we ARE a holder */
		for (i = 0; i < p->nspare; i++)
			if (p->spare[i]->active_node == p->mem[owner].node)
				return p->spare[i];
	}
	p->route_missed++;
	return p;
}

static int roundtrip_once(perfd_t *p, const char *method,
		const unsigned char *tree, size_t tlen, char **out,
		size_t *outlen);

static int learn_members(perfd_t *p)
{
	char *res;
	size_t n;
	int rc, own;

	/* owned, not borrowed - this runs inside after_success,
	 * while the caller's own reply still sits in the handle's buffer */
	own = p->own_reply;
	p->own_reply = 1;
	rc = roundtrip_once(p, "members", NULL, 0, &res, &n);
	p->own_reply = own;
	if (rc != 0)
		return -1;
	rc = parse_members(p, (const unsigned char *)res, n);
	free(res);
	p->next_refresh_ms = now_ms_lib() + p->refresh_ms;
	return rc;
}

/* the bound on a fresh dial to any member, when nothing else works: a member
 * gone dark costs it a second, not the handle's whole connect_timeout_ms */
#define REDIAL_CONNECT_MS 1000
#define DIAL_SETTLE_MIN_MS 50          /* conn_ready's least wait for its standbys */
#define DIAL_LOOK_MS 200

/* what a link to another member is dialled with: the secrets from @first round */
static void child_opts(perfd_t *p, perfd_opts *o, const char **sec, int connect_ms, int first)
{
	int i;

	memset(o, 0, sizeof *o);
	for (i = 0; i < p->nsec; i++)
		sec[i] = p->sec[(first + i) % p->nsec];
	sec[p->nsec] = NULL;
	o->secrets = p->nsec ? sec : NULL;
	o->connect_timeout_ms = connect_ms > 0 ? connect_ms : p->o_connect_ms;
	o->io_timeout_ms = p->o_io_ms;
	o->call_timeout_ms = p->call_ms ? p->call_ms : -1;
	o->spares = PERFD_SPARES_NONE;  /* children never recurse */
	o->keepalive_s = p->keepalive_s ? p->keepalive_s : -1;
	o->idle_ping_ms = -1;           /* a child is pinged by its parent */
	o->encrypt = p->want_enc;
}

/* open a link to @m in the caller's time: redial_any, whose caller has no other */
static perfd_t *open_child(perfd_t *p, const struct pd_member *m,
		int connect_ms)
{
	perfd_opts o;
	const char *sec[PERFD_MAX_SECRETS + 1];
	perfd_t *c;

	child_opts(p, &o, sec, connect_ms, 0);
	c = perfd_connect(m->addr, m->port, &o);
	if (c) {
		c->is_child = 1;
		c->active_node = m->node;
		c->last_reply_ms = now_ms_lib();   /* the handshake answered */
	}
	return c;
}

static int dialling(const perfd_t *p, int node)
{
	int i;

	for (i = 0; i < p->ndial; i++)
		if (p->dial[i].node == node)
			return 1;
	return 0;
}

static void dial_failed(struct pd_member *m, const char *why)
{
	if (!m)
		return;
	m->down = 1;
	/* the reason, cut to the field - the bound is explicit so the
	 * compiler can see the truncation is meant */
	snprintf(m->why, sizeof m->why, "re-dial: %.*s",
		(int)sizeof m->why - 10, why ? why : "");
	m->backoff_ms = !m->backoff_ms ? 1000 : m->backoff_ms < 15000 ? m->backoff_ms * 2 : 30000;
	m->retry_ms = now_ms_lib() + m->backoff_ms;
}

/* a standby's dial must not hold a caller: a member can accept and then say
 * nothing, as a frozen daemon's kernel does.  Started here, run by dials_run */
static int dial_start_try(perfd_t *p, struct pd_member *m, int arm, int tries, int secn)
{
	perfd_opts o;
	const char *sec[PERFD_MAX_SECRETS + 1];
	struct pd_dial *d;
	perfd_t *c;

	if (p->ndial >= PERFD_MAX_SPARES || dialling(p, m->node))
		return -1;
	/* the handshake takes one secret: the one this handle got in with, then the rest */
	child_opts(p, &o, sec, 0, p->nsec ? (p->hs_sec + secn) % p->nsec : 0);
	c = perfd_connect_async(m->addr, m->port, &o);
	if (!c) {
		dial_failed(m, perfd_error(NULL));
		fleet_seen(p);
		return -1;
	}
	d = &p->dial[p->ndial++];
	d->c = c;
	d->node = m->node;
	d->arm = arm;
	d->tries = tries;
	d->secn = secn;
	d->until_ms = now_ms_lib() + DEF_TIMEOUT_MS;
	return 0;
}

static int dial_start(perfd_t *p, struct pd_member *m, int arm)
{
	return dial_start_try(p, m, arm, 0, 0);
}

/* the handshake is done: a plain blocking link from here on.  Its look at where
 * it landed is one round trip on a link that has just answered, under a short bound */
static void dial_done(perfd_t *p, struct pd_dial d)
{
	struct pd_member *m = member_of(p, d.node);
	perfd_t *c = d.c;
	int fl = fcntl(c->fd, F_GETFL, 0), mine, least, workers, was_down, look, serving = 1;
	struct timeval tv = { 0, DIAL_LOOK_MS * 1000L };
	int cap = p->want_spares < 0 || p->want_spares > PERFD_MAX_SPARES
		? PERFD_MAX_SPARES : p->want_spares;

	if (!m || c->cinlen || fl < 0 || fcntl(c->fd, F_SETFL, fl & ~O_NONBLOCK) != 0) {
		perfd_free(c);
		return;
	}
	c->async = 0;
	setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	look = conn_where(c, &mine, &least, &workers, &serving) == 0;
	if (c->worker_bal && look && workers >= 2 &&
	    mine - 1 > least && d.tries < 8 * workers && d.tries < 64) {
		perfd_free(c);
		dial_start_try(p, m, d.arm, d.tries + 1, d.secn);
		return;
	}
	if (c->poisoned) {
		dial_failed(m, perfd_error(c));
		perfd_free(c);
		return;
	}
	/* by its own word it does not serve (a restarted process the fleet has not admitted again):
	 * no standby and no draw; its push or the next member list brings it back */
	if (look && !serving) {
		m->returning = m->returning || m->down;
		m->ready = 0;
		m->down = 0;
		m->why[0] = 0;
		perfd_free(c);
		return;
	}
	tv.tv_sec = c->io_ms / 1000;
	tv.tv_usec = (c->io_ms % 1000) * 1000L;
	setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	c->is_child = 1;
	c->active_node = d.node;
	c->last_reply_ms = now_ms_lib();   /* the handshake answered */
	was_down = m->down || m->returning;
	m->down = 0;
	m->returning = 0;
	m->why[0] = 0;
	m->backoff_ms = 0;
	if (was_down)
		p->recovered++;
	if (p->want_spares && d.node != p->active_node &&
	        spare_for(p, d.node) < 0 && p->nspare < cap)
		p->spare[p->nspare++] = c;
	else
		perfd_free(c);
	if (d.arm)
		spread_arm(p, m);
}

/* every dial as far as it goes: at once, or for @wait_ms while any is still out */
static void dials_run(perfd_t *p, int wait_ms)
{
	long long until = now_ms_lib() + wait_ms;

	while (p->ndial) {
		struct pollfd pf[PERFD_MAX_SPARES];
		long long now = now_ms_lib();
		int i, n = p->ndial;

		for (i = 0; i < n; i++) {
			int ev = perfd_events(p->dial[i].c);

			pf[i].fd = p->dial[i].c->fd;
			pf[i].events = (short)((ev & PERFD_EV_READ ? POLLIN : 0) |
				(ev & PERFD_EV_WRITE ? POLLOUT : 0));
			pf[i].revents = 0;
		}
		if (poll(pf, (nfds_t)n, until > now ? (int)(until - now) : 0) < 0 && errno != EINTR)
			return;
		now = now_ms_lib();
		/* from the top: taking one out moves the last into its place */
		for (i = n - 1; i >= 0; i--) {
			struct pd_dial d = p->dial[i];
			perfd_t *c = d.c;

			if (pf[i].revents & (POLLOUT | POLLERR | POLLHUP))
				(void)perfd_write_ready(c);
			if (pf[i].revents & (POLLIN | POLLERR | POLLHUP) && perfd_state(c) != PERFD_ST_FAILED)
				(void)perfd_read_ready(c);
			if (perfd_state(c) == PERFD_ST_READY && c->woff == c->wlen && !c->wstlen) {
				p->dial[i] = p->dial[--p->ndial];
				dial_done(p, d);
				fleet_seen(p);
			} else if (perfd_state(c) == PERFD_ST_FAILED || now >= d.until_ms) {
				struct pd_member *m = member_of(p, d.node);
				int refused = perfd_state(c) == PERFD_ST_FAILED;

				p->dial[i] = p->dial[--p->ndial];
				/* turned away in the handshake: rotation may have another secret good here */
				if (m && refused && c->hs_stage == PD_HS_WAIT && d.secn + 1 < p->nsec)
					dial_start_try(p, m, d.arm, d.tries, d.secn + 1);
				else
					dial_failed(m, refused ? perfd_error(c) : "no answer");
				perfd_free(c);
				fleet_seen(p);
			}
		}
		if (now_ms_lib() >= until)
			return;
	}
}

/* up to want_spares standbys on members other than the active one, a RANDOM subset per client
 * (spreads with no coordination); @arm: one added gets its share of traffic, not the one left */
static void warm_spares(perfd_t *p, int arm)
{
	int want = p->want_spares, i, tries;

	if (p->is_child || want == 0 || p->nmem < 2)
		return;
	if (want < 0 || want > PERFD_MAX_SPARES)
		want = p->nmem - 1 > PERFD_MAX_SPARES ? PERFD_MAX_SPARES
			: p->nmem - 1;
	for (tries = 0; p->nspare + p->ndial < want && tries < p->nmem * 2; tries++) {
		int idx = (int)((p->rr + (unsigned int)tries) % (unsigned int)p->nmem);
		struct pd_member *m = &p->mem[idx];
		int dup = dialling(p, m->node);

		if (m->node == p->active_node || !pd_usable(m))
			continue;
		for (i = 0; i < p->nspare; i++)
			if (p->spare[i]->active_node == m->node)
				dup = 1;
		if (dup)
			continue;
		dial_start(p, m, arm && m->node != p->spread_left);
	}
}

/* move a standby's CONNECTION into @dst; @src keeps nothing live */
static void adopt_conn(perfd_t *dst, perfd_t *src)
{
	if (dst->fd >= 0)
		close(dst->fd);
	free(dst->rbuf);
	free(dst->q);
	free(dst->ids);
	early_free(dst);

	dst->fd = src->fd;
	dst->encrypted = src->encrypted;
	dst->cs_send = src->cs_send;
	dst->cs_recv = src->cs_recv;
	dst->rbuf = src->rbuf; dst->rlen = src->rlen; dst->rcap = src->rcap;
	dst->roff = src->roff;         /* the unread window moves with it */
	dst->q = src->q; dst->qlen = src->qlen; dst->qcap = src->qcap;
	dst->ids = src->ids; dst->nids = src->nids;
	dst->idhead = src->idhead; dst->idcap = src->idcap;
	dst->early = src->early;
	/* ids mean nothing to the server; going back would reuse ones the dead link's replies carry */
	if (src->next_id > dst->next_id)
		dst->next_id = src->next_id;
	dst->active_node = src->active_node;
	dst->poisoned = 0;
	dst->io_silent = dst->reply_late = 0;

	src->fd = -1;
	src->rbuf = NULL; src->q = NULL; src->ids = NULL; src->early = NULL;
	src->rlen = src->rcap = src->qlen = src->qcap = 0;
	src->roff = 0;
	src->nids = src->idhead = src->idcap = 0;
	/* privilege was the old connection's - raised again before
	 * the next call (enable_replay), not here, inside a call's retry */
	if (dst->enable_sec)
		dst->enable_due = 1;
	if (dst->conn_name)                 /* the new connection is named too */
		dst->name_due = 1;
}

/* a cheap roundtrip under a short bound, the io timeout restored after: 0 = it answered.  The
 * failover must never adopt a link it has not just seen alive. */
static int probe_child(perfd_t *p, perfd_t *c)
{
	int bound = p->io_ms < 500 ? p->io_ms : 500, rc;
	struct timeval tv = { 0, bound * 1000L };

	setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	rc = perfd_ping(c);
	tv.tv_sec = p->io_ms / 1000;
	tv.tv_usec = (p->io_ms % 1000) * 1000L;
	setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	return rc;
}

/* the standby whose link most recently carried a reply - the one most
 * likely to still be alive; -1 when there is none */
static int pick_spare(perfd_t *p)
{
	int i, best = -1, pass;
	long long bt = -1;

	/* a member the fleet says is serving first; any link beats none */
	for (pass = 0; pass < 2 && best < 0; pass++)
		for (i = 0; i < p->nspare; i++) {
			const struct pd_member *m = member_of(p, p->spare[i]->active_node);

			if (!pass && m && !pd_usable(m))
				continue;
			if (p->spare[i]->last_reply_ms > bt) {
				bt = p->spare[i]->last_reply_ms;
				best = i;
			}
		}
	return best;
}

static void drop_spare(perfd_t *p, int k)
{
	int i;

	perfd_free(p->spare[k]);
	for (i = k; i < p->nspare - 1; i++)
		p->spare[i] = p->spare[i + 1];
	p->nspare--;
}

static void member_down(perfd_t *p, int node, const char *prefix,
		const char *why)
{
	int j;

	for (j = 0; j < p->nmem; j++)
		if (p->mem[j].node == node) {
			p->mem[j].down = 1;
			snprintf(p->mem[j].why, sizeof p->mem[j].why, "%s%.*s",
				prefix, (int)(sizeof p->mem[j].why - 1 - strlen(prefix)),
				why ? why : "");
			/* retried in the background from now on */
			if (!p->mem[j].backoff_ms)
				p->mem[j].backoff_ms = 1000;
			p->mem[j].retry_ms = now_ms_lib() + p->mem[j].backoff_ms;
		}
	fleet_seen(p);
}

/* a FRESH dial to any learned member, the ones not down first and the dead one last, bounded by
 * REDIAL_CONNECT_MS, adopting the first handshake that completes: 0 = usable again */

static int redial_any(perfd_t *p, long long until)
{
	int pass, i, dead = p->active_node;

	if (p->is_child || !p->nmem)
		return -1;
	for (pass = 0; pass < 2; pass++)
		for (i = 0; i < p->nmem; i++) {
			struct pd_member *m = &p->mem[i];
			perfd_t *c;
			int cms = p->o_connect_ms > 0 && p->o_connect_ms < REDIAL_CONNECT_MS
				? p->o_connect_ms : REDIAL_CONNECT_MS;

			if ((pass == 0) != (!m->down && m->node != dead))
				continue;
			if (until) {                   /* 0 = no total bound (perfd_redial) */
				long long left = until - now_ms_lib();

				if (left <= 0)
					return -1;
				if (left < cms)
					cms = (int)left;
			}
			c = open_child(p, m, cms);
			if (!c) {
				member_down(p, m->node, "re-dial: ", perfd_error(NULL));
				continue;
			}
			adopt_conn(p, c);
			perfd_free(c);
			p->active_node = m->node;
			m->down = 0;
			m->why[0] = 0;
			p->last_reply_ms = now_ms_lib();
			p->failovers++;
			p->next_refresh_ms = 0;    /* re-learn, re-warm on the next call */
			fleet_seen(p);
			return 0;
		}
	return -1;
}

/* a pinned handle comes back to its own node or not at all */
static int redial_own(perfd_t *p)
{
	int i, node = p->active_node;

	for (i = 0; i < p->nmem; i++) {
		struct pd_member *m = &p->mem[i];
		perfd_t *c;

		if (m->node != node)
			continue;
		c = open_child(p, m, p->o_connect_ms > 0 &&
			p->o_connect_ms < REDIAL_CONNECT_MS
			? p->o_connect_ms : REDIAL_CONNECT_MS);
		if (!c)
			return errk(p, PERFD_E_TRANSPORT, "re-dial: node %d did not answer - %s",
				node, perfd_error(NULL));
		adopt_conn(p, c);
		perfd_free(c);
		p->active_node = node;
		m->down = 0;
		m->why[0] = 0;
		p->last_reply_ms = now_ms_lib();
		p->next_refresh_ms = 0;
		fleet_seen(p);
		return 0;
	}
	return errk(p, PERFD_E_TRANSPORT, "re-dial: node %d, the one this pinned handle "
		"is on, is not in its member list", node);
}

/* the dead connection is replaced: a standby PROBED first (most recently alive first), else a fresh
 * dial to any learned member, so only the call that found it dead can fail.  0 = usable again. */
static int failover(perfd_t *p)
{
	long long until = now_ms_lib() + p->io_ms;     /* the whole move, not each step */
	int k;

	if (p->is_child || p->pin)             /* the error stays the dialled node's */
		return -1;
	member_down(p, p->active_node, "", "the link died");
	while (now_ms_lib() < until && (k = pick_spare(p)) >= 0) {
		perfd_t *c = p->spare[k];

		if (probe_child(p, c) == 0) {
			/* the stream belonged to the connection that died */
			if (p->udp)
				udp_drop(p, 0);
			adopt_conn(p, c);
			drop_spare(p, k);
			p->last_reply_ms = now_ms_lib();
			p->failovers++;
			/* re-warm in the background of the next call, not now:
			 * the caller is waiting on a request */
			p->next_refresh_ms = 0;
			return 0;
		}
		p->pings++;
		p->pings_failed++;
		member_down(p, c->active_node, "probe: ", perfd_error(c));
		drop_spare(p, k);
	}
	return redial_any(p, until);
}

int perfd_redial(perfd_t *p)
{
	if (!p)
		return -1;
	if (!p->poisoned)
		return 0;
	if (p->is_child)
		return -1;
	if (p->pin)
		return redial_own(p);
	while (p->nspare) {
		/* whatever standbys are still held died with the active, or
		 * would have been adopted; a fresh dial is the honest start */
		int k = pick_spare(p);

		if (probe_child(p, p->spare[k]) == 0) {
			adopt_conn(p, p->spare[k]);
			drop_spare(p, k);
			p->last_reply_ms = now_ms_lib();
			p->failovers++;
			p->next_refresh_ms = 0;
			return 0;
		}
		member_down(p, p->spare[k]->active_node, "probe: ",
			perfd_error(p->spare[k]));
		drop_spare(p, k);
	}
	if (redial_any(p, 0) == 0)
		return 0;
	return errk(p, PERFD_E_TRANSPORT, "re-dial: no learned member answered");
}

/* after a successful call: refresh the fleet view and top the spares
 * back up, at most once per refresh_ms */
static void maybe_refresh(perfd_t *p)
{
	if (p->is_child || !p->want_spares)
		return;
	if (now_ms_lib() < p->next_refresh_ms)
		return;
	learn_members(p);
	/* a standby this refresh adds is a member the push did not tell */
	warm_spares(p, 1);
	p->spread_left = -1;
	spread_leave(p);
}

/* a standby carries nothing until a failover, so it may have died unnoticed: idle ones are pinged
 * under a short bound at most once a second, a failing one retired and its member marked down */
static void maybe_ping(perfd_t *p)
{
	long long now = now_ms_lib();
	int i;

	if (p->is_child || !p->idle_ping_ms || now < p->next_ping_ms)
		return;
	p->next_ping_ms = now + 1000;
	for (i = 0; i < p->nspare; ) {
		perfd_t *c = p->spare[i];

		if (now - c->last_reply_ms < p->idle_ping_ms) {
			i++;
			continue;
		}
		p->pings++;
		if (probe_child(p, c) == 0) {
			i++;
			continue;
		}
		p->pings_failed++;
		member_down(p, c->active_node, "idle ping: ", perfd_error(c));
		route_retire(p, c);            /* frees it; nspare shrinks, so
		                                * the same index is the next one */
	}
}

