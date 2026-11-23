/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov
 * Part of libperfd - see lib/LICENSE.  This file must stay
 * free of src/core includes; tools/sync-libperfd.sh exports
 * exactly the MIT set to consumers. */
/* perfd-members.inc - part of perfd.c, #included in order (one translation unit): membership
 * pushed by the fleet, recovery and maintenance after a call, the read-only accessors. */
/* ---- membership pushed by the fleet, members recovered ------------------ */

static int spare_for(perfd_t *p, int node)
{
	int i;

	for (i = 0; i < p->nspare; i++)
		if (p->spare[i]->active_node == node)
			return i;
	return -1;
}

/* does this handle spread at all?  Not the default policy, and not one that
 * routes by key or holds subscriptions on its connection */
static int spreading(const perfd_t *p)
{
	return !p->is_child && p->want_spares && p->policy != PERFD_POLICY_FAILOVER &&
		!p->route_ok && !p->route_hints && !p->ps_subs;
}

/* a spreading handle takes its share of a member that came up: 1 in N of the
 * members serving moves, so the shares even out with no rush.  One draw an event */
static void spread_arm(perfd_t *p, const struct pd_member *m)
{
	long long now = now_ms_lib();
	int i, n = 0;

	if (!spreading(p) || !pd_spreadable(m) || m->node == p->active_node ||
	        (p->spread_drawn == m->node && now - p->spread_drawn_ms < 10000))
		return;
	for (i = 0; i < p->nmem; i++)
		n += pd_spreadable(&p->mem[i]);
	p->spread_drawn = m->node;
	p->spread_drawn_ms = now;
	if (n >= 2 && randombytes_uniform((uint32_t)n) == 0) {
		p->spread_due = 1;
		p->spread_node = m->node;
		p->spread_next_ms = 0;
		p->spread_until_ms = now + 300000;
	}
}

/* the member this handle works through stopped serving in full: any serving
 * standby, at the next call.  A failure this is not, so no failover */
static void spread_leave(perfd_t *p)
{
	const struct pd_member *m = member_of(p, p->active_node);

	if (!spreading(p) || !m || pd_spreadable(m) || (p->spread_due && p->spread_node == -1))
		return;
	p->spread_due = 1;
	p->spread_node = -1;
	p->spread_next_ms = 0;
	p->spread_until_ms = now_ms_lib() + 300000;
}

/* a member is routed to while ready or draining */
static int state_usable(const char *s, int sl)
{
	return sl < 0 || (sl == 5 && !memcmp(s, "ready", 5)) ||
		(sl == 8 && !memcmp(s, "draining", 8));
}

/* "joined": a member the fleet just admitted - add it, dial it */
static void member_join(perfd_t *p, const char *addr, int port, int node,
		int ready, int draining)
{
	struct pd_member *m = NULL;
	int i, held = spare_for(p, node) >= 0;

	for (i = 0; i < p->nmem; i++)
		if (p->mem[i].node == node)
			m = &p->mem[i];
	if (!m) {
		if (p->nmem >= PERFD_MAX_MEMBERS)
			return;
		m = &p->mem[p->nmem++];
		memset(m, 0, sizeof *m);
	}
	snprintf(m->addr, sizeof m->addr, "%s", addr);
	m->port = port;
	m->node = node;
	m->ready = ready;
	m->draining = draining;
	m->down = 0;
	m->why[0] = 0;
	if (ready && node != p->active_node && !held && !p->async)
		dial_start(p, m, 1);       /* async: one fd polled, no standby behind it */
}

/* "state": a listed member's state changed */
static void member_state(perfd_t *p, int node, int ready, int draining)
{
	struct pd_member *m = member_of(p, node);
	int was, was_spread;

	if (!m)
		return;
	was = m->ready;
	was_spread = pd_spreadable(m);
	m->ready = ready;
	m->draining = draining;
	if (node == p->active_node) {
		spread_leave(p);
		return;
	}
	if (ready && !was && spare_for(p, node) < 0 && !p->async) {
		dial_start(p, m, !was_spread);
		return;
	}
	if (!was_spread)
		spread_arm(p, m);
}

/* "expelled": drop it completely - member, standby, routing weight; a
 * later heartbeat from that identity is a new member, not a return */
static void member_expel(perfd_t *p, int node)
{
	int i, k = spare_for(p, node);

	if (k >= 0) {
		perfd_free(p->spare[k]);
		for (i = k; i < p->nspare - 1; i++)
			p->spare[i] = p->spare[i + 1];
		p->nspare--;
	}
	for (i = 0; i < p->nmem; i++)
		if (p->mem[i].node == node) {
			memmove(&p->mem[i], &p->mem[i + 1],
				(size_t)(p->nmem - i - 1) * sizeof p->mem[0]);
			p->nmem--;
			break;
		}
	if (node == p->active_node)
		p->active_stale = 1;           /* fail over before the next call */
}

/* a NOTIFY tree, membership acted on before the application's hook (a child's parent hears it too):
 * a delivery is {"method": ..., "params": {..}}, the membership push a flat {"notify": ...} map */
static void notify_internal(perfd_t *p, const unsigned char *tree, size_t n)
{
	struct pc_tv v;
	char addr[46], event[16], state[16];
	long long port, node, members;
	int el, al, sl;

	if (p->is_child)
		return;
	memset(&v, 0, sizeof v);
	if (pc_tv_parse(&v, tree, n) != 0 || v.n[0].type != 'm') {
		pc_tv_free(&v);
		return;
	}
	/* the daemon dropped the UDP stream and every subscription */
	if (p->udp && pc_tv_streq(&v, pc_tv_get(&v, 0, "method"),
	        "pubsub_udp_pruned")) {
		udp_drop(p, 1);
		pc_tv_free(&v);
		return;
	}
	if (p->ignore_push ||              /* a test's stale map */
	    !pc_tv_streq(&v, pc_tv_get(&v, 0, "notify"), "membership")) {
		pc_tv_free(&v);
		return;
	}
	el = pc_tv_get_str(&v, 0, "event", event, sizeof event - 1);
	al = pc_tv_get_str(&v, 0, "addr", addr, sizeof addr - 1);
	sl = pc_tv_get_str(&v, 0, "state", state, sizeof state - 1);
	if (el < 0 || al < 0 || pc_tv_get_int(&v, 0, "port", &port) != 0 ||
	        pc_tv_get_int(&v, 0, "node", &node) != 0 || !node) {
		pc_tv_free(&v);
		return;
	}
	/* a fleet of another size than this handle holds - learn it again */
	if (pc_tv_get_int(&v, 0, "members", &members) == 0 && members != p->nmem)
		p->async_learn_ms = 0;
	event[el] = 0;
	addr[al] = 0;
	pc_tv_free(&v);
	if (!strcmp(event, "joined") && port > 0)
		member_join(p, addr, (int)port, (int)node, state_usable(state, sl),
			sl == 8 && !memcmp(state, "draining", 8));
	else if (!strcmp(event, "state"))
		member_state(p, (int)node, state_usable(state, sl),
			sl == 8 && !memcmp(state, "draining", 8));
	else if (!strcmp(event, "expelled"))
		member_expel(p, (int)node);
	fleet_seen(p);
}

/* whatever the daemon pushed while nothing was read: a bounded non-blocking look, so a push is
 * acted on at the next call rather than at the next reply */
static void drain_link(perfd_t *p)
{
	int rounds;

	if (p->fd < 0 || p->poisoned)
		return;
	for (rounds = 0; rounds < 8; rounds++) {
		struct pollfd pf;
		unsigned long long got;
		char *result;
		size_t rlen;
		int rc;
		char ebuf[256];

		pf.fd = p->fd;
		pf.events = POLLIN;
		if (poll(&pf, 1, 0) <= 0)
			return;
		if (fill(p) != 0)
			return;
		for (;;) {
			rc = take_msg(p, &got, &result, &rlen, ebuf, sizeof ebuf);
			if (rc == 2)
				continue;
			if (rc == 1) {
				/* a reply nobody is waiting for yet: keep it */
				park_msg(p, got, result, rlen, ebuf);
				continue;
			}
			break;
		}
		if (rc < 0)
			return;
	}
}

static void drain_notifications(perfd_t *p)
{
	if (!p->is_child)
		drain_link(p);
}

/* a restart closes a link its caller has not used since.  Looked at before
 * the request goes out, nothing is sent yet, so even a counter update can move */
static int link_closed(perfd_t *t)
{
	if (!(t->nids - t->idhead) && !t->qlen &&
	        now_ms_lib() - t->last_reply_ms >= LOOK_QUIET_MS)
		drain_link(t);
	return t->poisoned;
}

/* members marked down are re-dialled in the background, one per scan, at most once a second, on
 * a backoff of 1 s doubling to 30 s (dial_failed), while requests flow over the live links */
static void maybe_recover(perfd_t *p)
{
	long long now = now_ms_lib();
	int i;

	if (p->is_child || now < p->next_recover_ms)
		return;
	p->next_recover_ms = now + 1000;
	for (i = 0; i < p->nmem; i++) {
		struct pd_member *m = &p->mem[i];

		if (!m->down || now < m->retry_ms || m->node == p->active_node ||
		        dialling(p, m->node))
			continue;
		dial_start(p, m, 1);
		return;                        /* one dial per scan */
	}
}

/* the daemon forwarded @t's reply: a ROUTING handle whose map stamp differs is behind and re-learns
 * now (maybe_refresh, right after), at most once a second so a lagging node cannot drive it */
static void hint_check(perfd_t *p, perfd_t *t)
{
	long long now;

	if (!t->hint_seen)
		return;
	t->hint_seen = 0;
	if (!p->route_want)
		return;
	if (p->map_known && t->hint_term == p->map_term &&
	        t->hint_seq == p->map_seq)
		return;
	now = now_ms_lib();
	if (now < p->hint_next_ms)
		return;
	p->hint_next_ms = now + 1000;
	p->moved_hints++;
	p->next_refresh_ms = 0;
}

static void after_success(perfd_t *p)
{
	p->last_reply_ms = now_ms_lib();
	hint_check(p, p);
	maybe_refresh(p);
	maybe_ping(p);
	drain_notifications(p);
	dials_run(p, 0);
	maybe_recover(p);
}

/* a call routed to a standby is a successful call too: maintenance must not wait for a key on the
 * active node.  The active's own last_reply_ms is NOT touched: it did not answer. */
static void after_routed(perfd_t *p, perfd_t *t)
{
	t->last_reply_ms = now_ms_lib();   /* the standby did */
	hint_check(p, t);
	maybe_refresh(p);
	maybe_ping(p);
	drain_notifications(p);
	dials_run(p, 0);
	maybe_recover(p);
}

int perfd_maintain(perfd_t *p)
{
	if (!p || p->is_child)
		return 0;
	if (p->poisoned)
		return -1;
	drain_notifications(p);
	maybe_ping(p);
	dials_run(p, 0);
	maybe_recover(p);
	return 0;
}

unsigned long long perfd_recovered(const perfd_t *p)
{
	return p ? p->recovered : 0;
}

/* standby @i becomes this handle's connection */
static void adopt_spare(perfd_t *p, int i)
{
	perfd_t *sp = p->spare[i];
	int j;

	adopt_conn(p, sp);
	perfd_free(sp);
	for (j = i; j < p->nspare - 1; j++)
		p->spare[j] = p->spare[j + 1];
	p->nspare--;
}

/* choose the member this handle should work through, per policy, and
 * move onto it if it is not where we already are */
static void apply_policy(perfd_t *p)
{
	int i, best = -1, bestfree = -1;

	if (p->is_child || p->policy == PERFD_POLICY_FAILOVER || p->nmem < 2)
		return;
	switch (p->policy) {
	case PERFD_POLICY_LEAST_CONN:
		/* POWER OF TWO CHOICES: the emptiest member outright makes every client choose the
		 * SAME one; the better of two spreads without coordination and avoids the worst */
		if (p->nmem >= 2) {
			int a = (int)(pd_rand(p) % (unsigned int)p->nmem);
			int b = (int)(pd_rand(p) % (unsigned int)p->nmem);

			if (b == a)
				b = (a + 1) % p->nmem;
			if (!pd_spreadable(&p->mem[a]))
				a = b;
			best = p->mem[a].free_mb >= p->mem[b].free_mb ||
				!pd_spreadable(&p->mem[b]) ? a : b;
		}
		break;
	case PERFD_POLICY_WEIGHTED: {
		/* random, weighted BY free space: the maximum would be a tie-break every client
		 * resolves identically */
		long total = 0, r;

		for (i = 0; i < p->nmem; i++)
			if (pd_spreadable(&p->mem[i]))
				total += p->mem[i].free_mb > 0 ?
					p->mem[i].free_mb : 1;
		if (total <= 0)
			break;
		r = (long)(pd_rand(p) % (unsigned int)total);
		for (i = 0; i < p->nmem; i++) {
			if (!pd_spreadable(&p->mem[i]))
				continue;
			r -= p->mem[i].free_mb > 0 ? p->mem[i].free_mb : 1;
			if (r < 0) {
				best = i;
				break;
			}
		}
		break;
	}
	default: {
		/* round robin: random start, step, past unusable members rather than landing on one
		 * and failing over */
		int tries;

		best = -1;
		for (tries = 0; tries < p->nmem; tries++) {
			int cand = (int)(p->rr % (unsigned int)p->nmem);

			p->rr++;
			if (pd_spreadable(&p->mem[cand])) {
				best = cand;
				break;
			}
		}
		break;
	}
	}
	(void)bestfree;
	if (best < 0 || p->mem[best].node == p->active_node)
		return;
	i = spare_for(p, p->mem[best].node);
	if (i >= 0)
		adopt_spare(p, i);
}

int perfd_member_count(const perfd_t *p)
{
	return p ? p->nmem : 0;
}

int perfd_member_info(const perfd_t *p, int i, char *addr, size_t acap,
		int *port, int *node, int *active)
{
	if (!p || i < 0 || i >= p->nmem)
		return -1;
	if (addr && acap)
		snprintf(addr, acap, "%s", p->mem[i].addr);
	if (port)
		*port = p->mem[i].port;
	if (node)
		*node = p->mem[i].node;
	if (active)
		*active = p->mem[i].node == p->active_node;
	return 0;
}

int perfd_spare_count(const perfd_t *p)
{
	return p ? p->nspare : 0;
}

/* what this handle knows of member @i from its own dials: "down" it failed to dial (@why how), not
 * inferred from others' views; "not-ready" by the member's own word; "idle" fine, no link held */
const char *perfd_member_state(const perfd_t *p, int i, char *why,
		size_t wcap)
{
	int j;

	if (why && wcap)
		why[0] = 0;
	if (!p || i < 0 || i >= p->nmem)
		return "unknown";
	if (p->mem[i].node == p->active_node)
		return "active";
	for (j = 0; j < p->nspare; j++)
		if (p->spare[j] && p->spare[j]->active_node == p->mem[i].node)
			return "standby";
	if (p->mem[i].down) {
		if (why && wcap)
			snprintf(why, wcap, "%s", p->mem[i].why);
		return "down";
	}
	if (!p->mem[i].ready)
		return "not-ready";
	return "idle";
}

/* the daemon version the member list reported; "" until a member
 * list has been read (standbys off, or a daemon older than the field) */
const char *perfd_server_version(const perfd_t *p)
{
	return p && p->srv_version[0] ? p->srv_version : "";
}

unsigned long long perfd_pings(const perfd_t *p)
{
	return p ? p->pings : 0;
}

unsigned long long perfd_pings_failed(const perfd_t *p)
{
	return p ? p->pings_failed : 0;
}

long long perfd_last_reply_ms(const perfd_t *p)
{
	return p ? p->last_reply_ms : 0;
}

unsigned long long perfd_failovers(const perfd_t *p)
{
	return p ? p->failovers : 0;
}

int perfd_active_node(const perfd_t *p)
{
	return p ? p->active_node : 0;
}

int perfd_wire_epoch(const perfd_t *p)
{
	return p ? p->route_epoch : 0;
}

int perfd_routing(const perfd_t *p)
{
	return p ? p->route_ok : 0;
}

unsigned long long perfd_moved_hints(const perfd_t *p)
{
	return p ? p->moved_hints : 0;
}

unsigned long long perfd_hint_hits(const perfd_t *p)
{
	return p ? p->hint_hits : 0;
}

unsigned long long perfd_hint_stale(const perfd_t *p)
{
	return p ? p->hint_stale : 0;
}

unsigned long long perfd_route_missed(const perfd_t *p)
{
	return p ? p->route_missed : 0;
}

