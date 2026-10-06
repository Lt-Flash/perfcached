/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov
 * Part of libperfd - see lib/LICENSE.  This file must stay
 * free of src/core includes; tools/sync-libperfd.sh exports
 * exactly the MIT set to consumers. */
/*
 * perfd-fleet.inc - part of perfd.c, which #includes it in order: one translation
 * unit, so every static stays file-local and consumers still compile
 * perfd.c alone.  Not a compilation unit of its own.
 *
 * Holds the fleet: the member list (S34), per-key routing (S35, S127), the proxy
 * holder table (S295), standbys and failover.
 */
/* ---- cluster awareness (task S34) ---------------------------------------
 * The daemon already knows the fleet; a client that learns it can keep
 * STANDBY connections open and swap onto one when a node dies, instead
 * of discovering the failure and then paying a TCP + Noise handshake.
 *
 * The design rule that keeps this safe: nothing here is load-bearing
 * for correctness.  A stale member list, a policy that picks a busy
 * node, a failover that lands somewhere unexpected - all of it costs a
 * hop at worst, because the daemon re-checks ownership itself. */

static long long now_ms_lib(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* the verbs that may be REPLAYED after a failover.  The store is
 * absolute-upsert, so a repeated set/del/expire is the same as one, and
 * so is a jset or jdel (a path written or removed twice is the same
 * document) - but add/sub and jincr are NOT idempotent and a silent
 * replay would double an increment.  Those surface the error instead;
 * the caller decides. */
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

static int bin_verb_idempotent(int verb)
{
	/* the counter verbs double on a replay; a conditional set replayed
	 * after its reply was lost reads its OWN write and answers
	 * "declined" - a lock the caller holds, reported as lost (S279b).
	 * setnx/setxx are likewise absent from method_idempotent(). */
	return verb != BV_ADD && verb != BV_SUB && verb != BV_SETNX &&
		verb != BV_SETXX && verb != BV_RLHIT &&  /* S314: a hit twice */
		verb != BV_HCMD;               /* S313: HINCRBY twice is not once */
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
	/* S70: the daemon's version rides the same reply; absent on a
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
		/* C7: "ready" | "recovering" | "starting" | "draining".
		 * DRAINING counts as usable: it is being emptied, not doubted,
		 * and still answers for what it holds. */
		sl = pc_tv_get_str(&v, m, "state", state, sizeof state - 1);
		p->mem[n].ready = sl < 0 ||
			(sl == 5 && !memcmp(state, "ready", 5)) ||
			(sl == 8 && !memcmp(state, "draining", 8));
		p->mem[n].down = 0;
		if (!p->mem[n].port)
			continue;      /* a node that cannot be dialled */
		n++;
	}
	/* the ROUTING CONTRACT: only route when the daemon names an
	 * algorithm we actually implement, and a mode where routing means
	 * something.  An unknown algo or a proxy cluster simply leaves
	 * routing off - the client still works, it just pays the forward. */
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
			/* S127: `spread` routes too - the holders are the top K
			 * of the same rendezvous ranking, so shard is this with
			 * K = 1.  An OLDER library does not know the word, fails
			 * this test, turns routing off and dials any node; the
			 * daemon forwards, so it stays correct and only pays a
			 * pull per read. */
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
			/* RV-10: which map this list belongs to */
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
	return n;
}

/* ---- per-key routing (task S35) -----------------------------------------
 * With the member list in hand a client can compute WHERE a key
 * belongs and send the request straight there, so the daemon's forward
 * hop disappears.  Two rules, and they exist for different reasons:
 *
 *   shard - the owner is computable, so routing removes a network hop;
 *   store - there is no owner, but hashing each key to ONE node makes
 *           this client a de-facto single writer for it, which is what
 *           keeps concurrent writers from forking a key (par 6.5b).
 *           Spreading and key-stickiness pull opposite ways here, and
 *           stickiness wins for correctness.
 *
 * proxy is NOT routed: its placement depends on live free space and
 * history, so there is nothing a client can compute.
 *
 * THE INVARIANT: none of this is load-bearing for ONE request.  The
 * daemon re-checks ownership and forwards a wrong guess, so a stale
 * member list, an unrecognised algorithm or a missing standby costs one
 * hop and never correctness.  That is why every failure path below
 * simply returns the active handle.  What IS load-bearing is that every
 * keyed verb decides the same way (see roundtrip_key): two requests on
 * one key from one client are ordered because they meet at one node,
 * not because either was routed (S211). */

/* The placement input: the Redis SLOT, from the KEY ALONE.
 *
 * The collection is deliberately not mixed in.  A RESP client knows
 * nothing about collections, so anything it and we can both compute has
 * to ignore them - and the daemon's pc_shard_owner() ignores them for
 * the same reason.  Feeding col in here would put this client on a
 * different node from every Redis client for the same key. */
static unsigned long long route_keyhash(const char *col, const char *key)
{
	(void)col;
	return pc_key_slot(key, strlen(key));
}

/* xorshift over the handle's own cursor: independent per client, which
 * is what makes uncoordinated spreading work (par 5.6) */
static unsigned int pd_rand(perfd_t *p)
{
	unsigned int x = p->rr ? p->rr : 2463534242u;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	p->rr = x;
	return x;
}

/* The @n-th ranked member for @key (0 = the owner), or -1.  Selection
 * rather than a sort: n is at most the copy factor, which is small, and
 * a per-request sort of the whole member list is not worth it. */
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

/* Public: which member should hold @key, or -1 when routing does not
 * apply (no member list learned, fewer than two members, or the cluster
 * did not advertise a contract this client implements).  Pair it with
 * perfd_member_info() to get the node's address.
 *
 * This exists because per-key routing inside the library needs standby
 * CONNECTIONS, and those are unreachable on the async API - an async
 * caller drives one fd per handle and the library cannot open more
 * behind its back.  An async application therefore opens its own handle
 * per node and asks this which one to use, which is the same
 * computation the blocking path does internally. */
int perfd_owner_of(perfd_t *p, const char *col, const char *key)
{
	return p ? route_owner(p, col, key) : -1;
}

/* a standby died under a routed request: drop it, so the next request
 * does not keep trying a corpse.  The member is marked down too, which
 * makes route_owner() pick the next-best node instead. */
static void route_retire(perfd_t *p, perfd_t *dead)
{
	int i, j;

	for (i = 0; i < p->nmem; i++)
		if (p->mem[i].node == dead->active_node)
			p->mem[i].down = 1;
	for (i = 0; i < p->nspare; i++)
		if (p->spare[i] == dead) {
			perfd_free(dead);
			for (j = i; j < p->nspare - 1; j++)
				p->spare[j] = p->spare[j + 1];
			p->nspare--;
			return;
		}
}

/* ---- S295: the holder table ------------------------------------------
 * key -> the node that last answered for it, until the record's TTL runs
 * out.  Allocated once at connect, never per entry.  Set-associative, 4
 * ways: direct-mapped, 200 keys already lost 8 to collisions in 4,096
 * slots (hinttest), and each colliding pair then costs two hops a round.
 * A new key goes in front and pushes the set's oldest out (newest wins -
 * DESIGN 12hn's TTL-aware rule was to be measured first; not adopted). */
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
	unsigned long long h = hint_hash(col, key);

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

/* the handle that should carry this request: ours, a standby already
 * open to the owner, or - when we are not connected to the owner -
 * ours anyway, which just costs the forward we were trying to avoid */
static perfd_t *route_pick(perfd_t *p, const char *col, const char *key)
{
	int owner, i, r;

	if (p->is_child)
		return p;
	p->hinted = 0;
	/* S295: a key whose holder a reply named goes straight there */
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
	/* S127: under spread any of the K holders can answer, so walk the
	 * ranking and take the best one we already have a connection to.
	 * K = 1 makes this exactly the shard behaviour it replaces.  Being
	 * connected to the SECOND-ranked holder beats forwarding through a
	 * node that holds nothing. */
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

	/* S284: owned, not borrowed - this runs inside after_success,
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

/* open a standby to @m; NULL on failure (a member being down is normal).
 * @connect_ms bounds the dial (0 = the handle's own setting). */
static perfd_t *open_child(perfd_t *p, const struct pd_member *m,
		int connect_ms)
{
	perfd_opts o;
	const char *sec[PERFD_MAX_SECRETS + 1];
	perfd_t *c;
	int i;

	memset(&o, 0, sizeof o);
	for (i = 0; i < p->nsec; i++)
		sec[i] = p->sec[i];
	sec[p->nsec] = NULL;
	o.secrets = p->nsec ? sec : NULL;
	o.connect_timeout_ms = connect_ms > 0 ? connect_ms : p->o_connect_ms;
	o.io_timeout_ms = p->o_io_ms;
	o.spares = PERFD_SPARES_NONE;  /* children never recurse */
	o.keepalive_s = p->keepalive_s ? p->keepalive_s : -1;   /* S104 */
	o.idle_ping_ms = -1;           /* a child is pinged by its parent */
	c = perfd_connect(m->addr, m->port, &o);
	if (c) {
		c->is_child = 1;
		c->active_node = m->node;
		c->last_reply_ms = now_ms_lib();   /* the handshake answered */
	}
	return c;
}

/* keep up to want_spares standbys open, on members other than the
 * active one.  Which ones is a RANDOM subset per client: independent
 * random choices spread a big fleet without any coordination. */
static void warm_spares(perfd_t *p)
{
	int want = p->want_spares, i, tries;

	if (p->is_child || want == 0 || p->nmem < 2)
		return;
	if (want < 0 || want > PERFD_MAX_SPARES)
		want = p->nmem - 1 > PERFD_MAX_SPARES ? PERFD_MAX_SPARES
			: p->nmem - 1;
	for (tries = 0; p->nspare < want && tries < p->nmem * 2; tries++) {
		int idx = (int)((p->rr + (unsigned int)tries) % (unsigned int)p->nmem);
		struct pd_member *m = &p->mem[idx];
		perfd_t *c;
		int dup = 0;

		if (m->node == p->active_node || !pd_usable(m))
			continue;
		for (i = 0; i < p->nspare; i++)
			if (p->spare[i]->active_node == m->node)
				dup = 1;
		if (dup)
			continue;
		c = open_child(p, m, 0);
		if (!c) {
			const char *e = perfd_error(NULL);

			m->down = 1;
			/* S70: the reason, cut to the field - the bound is explicit so
			 * the compiler can see the truncation is meant */
			snprintf(m->why, sizeof m->why, "%.*s",
				(int)sizeof m->why - 1, e ? e : "");
			continue;
		}
		p->spare[p->nspare++] = c;
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

	dst->fd = src->fd;
	dst->encrypted = src->encrypted;
	dst->cs_send = src->cs_send;
	dst->cs_recv = src->cs_recv;
	dst->rbuf = src->rbuf; dst->rlen = src->rlen; dst->rcap = src->rcap;
	dst->roff = src->roff;         /* the unread window moves with it */
	dst->q = src->q; dst->qlen = src->qlen; dst->qcap = src->qcap;
	dst->ids = src->ids; dst->nids = src->nids;
	dst->idhead = src->idhead; dst->idcap = src->idcap;
	dst->next_id = src->next_id;
	dst->active_node = src->active_node;
	dst->poisoned = 0;

	src->fd = -1;
	src->rbuf = NULL; src->q = NULL; src->ids = NULL;
	src->rlen = src->rcap = src->qlen = src->qcap = 0;
	src->roff = 0;
	src->nids = src->idhead = src->idcap = 0;
}

/* a cheap roundtrip on a link under a short bound, the io timeout
 * restored after.  0 = it answered.  Shared by the idle scan and the
 * failover, which must never adopt a link it has not just seen alive. */
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
	int i, best = -1;
	long long bt = -1;

	for (i = 0; i < p->nspare; i++)
		if (p->spare[i]->last_reply_ms > bt) {
			bt = p->spare[i]->last_reply_ms;
			best = i;
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
			/* S107: retried in the background from now on */
			if (!p->mem[j].backoff_ms)
				p->mem[j].backoff_ms = 1000;
			p->mem[j].retry_ms = now_ms_lib() + p->mem[j].backoff_ms;
		}
}

/* S104: a FRESH dial to any member this handle has learned - the ones
 * not marked down first, the one that just died last - under a short
 * connect bound, adopted on the first handshake that completes.  The
 * members are the whole fleet as last reported, so this reaches a
 * fleet whose seed is the one node still down.  0 = usable again. */
#define REDIAL_CONNECT_MS 1000

static int redial_any(perfd_t *p)
{
	int pass, i, dead = p->active_node;

	if (p->is_child || !p->nmem)
		return -1;
	for (pass = 0; pass < 2; pass++)
		for (i = 0; i < p->nmem; i++) {
			struct pd_member *m = &p->mem[i];
			perfd_t *c;

			if ((pass == 0) != (!m->down && m->node != dead))
				continue;
			c = open_child(p, m, p->o_connect_ms > 0 &&
				p->o_connect_ms < REDIAL_CONNECT_MS
				? p->o_connect_ms : REDIAL_CONNECT_MS);
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
			return 0;
		}
	return -1;
}

/* The dead connection is replaced.  S104: never by adoption alone - a
 * standby is PROBED first, most recently alive first, and one that
 * fails is closed and its member marked down; when none is alive the
 * handle re-dials fresh to any member it has learned, inside this same
 * failover, so the operation that found the dead active is the only
 * one that can fail - and with the replay under it, not even that.
 * Returns 0 when the handle is usable again. */
static int failover(perfd_t *p)
{
	int k;

	if (p->is_child)
		return -1;
	member_down(p, p->active_node, "", "the link died");
	while ((k = pick_spare(p)) >= 0) {
		perfd_t *c = p->spare[k];

		if (probe_child(p, c) == 0) {
			/* PS5: the stream belonged to the connection that died */
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
	return redial_any(p);
}

int perfd_redial(perfd_t *p)
{
	if (!p)
		return -1;
	if (!p->poisoned)
		return 0;
	if (p->is_child)
		return -1;
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
	if (redial_any(p) == 0)
		return 0;
	return err(p, "re-dial: no learned member answered");
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
	warm_spares(p);
}

/* S104: a standby is the link most likely to have died unnoticed - it
 * carries nothing until a failover, and a failover used to adopt it
 * blind.  After a successful call, at most once a second, every
 * standby idle longer than idle_ping_ms is pinged under a short bound
 * (a corpse must not hold the caller for the io timeout); one that
 * fails is retired and its member marked down with the reason, so the
 * next failover cannot pick it.  A child is never the pinger. */
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

