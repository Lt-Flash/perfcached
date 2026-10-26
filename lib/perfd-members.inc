/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov
 * Part of libperfd - see lib/LICENSE.  This file must stay
 * free of src/core includes; tools/sync-libperfd.sh exports
 * exactly the MIT set to consumers. */
/*
 * perfd-members.inc - part of perfd.c, which #includes it in order: one translation
 * unit, so every static stays file-local and consumers still compile
 * perfd.c alone.  Not a compilation unit of its own.
 *
 * Holds membership pushed by the fleet (S107), recovery and maintenance after a
 * call, and the read-only accessors (perfd_member_info, the counters).
 */
/* ---- S107: membership pushed by the fleet, members recovered ------------ */

static int spare_for(perfd_t *p, int node)
{
	int i;

	for (i = 0; i < p->nspare; i++)
		if (p->spare[i]->active_node == node)
			return i;
	return -1;
}

/* open a standby to member @m under a short bound and hold it, when
 * standbys are wanted and there is room.  0 = the member is reachable. */
static int member_bring_up(perfd_t *p, struct pd_member *m)
{
	perfd_t *c;
	int cap = p->want_spares < 0 || p->want_spares > PERFD_MAX_SPARES
		? PERFD_MAX_SPARES : p->want_spares;

	c = open_child(p, m, 200);
	if (!c) {
		const char *e = perfd_error(NULL);

		snprintf(m->why, sizeof m->why, "re-dial: %.*s",
			(int)sizeof m->why - 10, e ? e : "");
		return -1;
	}
	m->down = 0;
	m->why[0] = 0;
	m->backoff_ms = 0;
	if (p->want_spares && m->node != p->active_node &&
	        spare_for(p, m->node) < 0 && p->nspare < cap)
		p->spare[p->nspare++] = c;
	else
		perfd_free(c);
	return 0;
}

/* "joined": a member the fleet just admitted - add it, dial it */
static void member_join(perfd_t *p, const char *addr, int port, int node)
{
	struct pd_member *m = NULL;
	int i;

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
	m->ready = 1;
	m->down = 0;
	m->why[0] = 0;
	if (node != p->active_node)
		member_bring_up(p, m);
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

/* a NOTIFY frame's tree: act on membership before the application's
 * hook sees it.  Children carry no member table; their parent hears
 * the same push on its own link.  Shapes (DESIGN 12if): a delivery is
 * {"method": "message"|"pmessage"|"pubsub_udp_pruned", "params": {..}};
 * the membership push is the flat map {"notify": "membership", "event",
 * "addr", "port", "node", "identity", "members"}. */
static void notify_internal(perfd_t *p, const unsigned char *tree, size_t n)
{
	struct pc_tv v;
	char addr[46], event[16];
	long long port, node;
	int el, al;

	if (p->is_child)
		return;
	memset(&v, 0, sizeof v);
	if (pc_tv_parse(&v, tree, n) != 0 || v.n[0].type != 'm') {
		pc_tv_free(&v);
		return;
	}
	/* PS5: the daemon dropped the UDP stream and every subscription */
	if (p->udp && pc_tv_streq(&v, pc_tv_get(&v, 0, "method"),
	        "pubsub_udp_pruned")) {
		udp_drop(p, 1);
		pc_tv_free(&v);
		return;
	}
	if (p->ignore_push ||              /* RV-10: a test's stale map */
	    !pc_tv_streq(&v, pc_tv_get(&v, 0, "notify"), "membership")) {
		pc_tv_free(&v);
		return;
	}
	el = pc_tv_get_str(&v, 0, "event", event, sizeof event - 1);
	al = pc_tv_get_str(&v, 0, "addr", addr, sizeof addr - 1);
	if (el < 0 || al < 0 || pc_tv_get_int(&v, 0, "port", &port) != 0 ||
	        pc_tv_get_int(&v, 0, "node", &node) != 0 || !node) {
		pc_tv_free(&v);
		return;
	}
	event[el] = 0;
	addr[al] = 0;
	pc_tv_free(&v);
	if (!strcmp(event, "joined") && port > 0)
		member_join(p, addr, (int)port, (int)node);
	else if (!strcmp(event, "expelled"))
		member_expel(p, (int)node);
}

/* whatever the daemon pushed while nothing was being read: a bounded,
 * non-blocking look at the socket, so a push is acted on at the next
 * call rather than at the next reply */
static void drain_notifications(perfd_t *p)
{
	int rounds;

	if (p->is_child || p->fd < 0 || p->poisoned)
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

/* S107: members marked down are re-dialled in the background, one per
 * scan, at most once a second, on a backoff of 1 s doubling to 30 s -
 * while every request keeps flowing over the live links */
static void maybe_recover(perfd_t *p)
{
	long long now = now_ms_lib();
	int i;

	if (p->is_child || now < p->next_recover_ms)
		return;
	p->next_recover_ms = now + 1000;
	for (i = 0; i < p->nmem; i++) {
		struct pd_member *m = &p->mem[i];

		if (!m->down || now < m->retry_ms || m->node == p->active_node)
			continue;
		if (member_bring_up(p, m) == 0) {
			p->recovered++;
			return;
		}
		m->backoff_ms = m->backoff_ms < 15000 ? m->backoff_ms * 2 : 30000;
		m->retry_ms = now + m->backoff_ms;
		return;                        /* one dial per scan */
	}
}

/* every successful call lands here: the link is known good NOW */
/* RV-10: the reply that just arrived on @t said the daemon had to forward
 * it.  If this handle ROUTES, and the map the daemon routed by is not the
 * one the member list was learned under, the handle is behind: re-learn
 * now - maybe_refresh() does it right behind this - instead of at the
 * next push or the next refresh_ms.  A handle that does not route asks
 * any node on purpose, and the same stamp means the forward was not this
 * handle's doing (a holder mid-convergence); neither re-learns.  At most
 * once a second: a node that is itself behind must not make a client
 * ask for the member list on every reply. */
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
	maybe_recover(p);
}

/* S107: a call routed to a standby (S35) is a successful call too, and
 * the handle's maintenance - the pushes waiting on its own link, the
 * idle standbys, the members to recover - must not wait for a key that
 * happens to live on the active node.  Measured: a client whose keys
 * all routed away from its active never read the "joined" push sitting
 * in that socket, and learned nothing for as long as that held.  The
 * active's own last_reply_ms is NOT touched: it did not answer. */
static void after_routed(perfd_t *p, perfd_t *t)
{
	t->last_reply_ms = now_ms_lib();   /* the standby did */
	hint_check(p, t);
	maybe_refresh(p);
	maybe_ping(p);
	drain_notifications(p);
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
	maybe_recover(p);
	return 0;
}

unsigned long long perfd_recovered(const perfd_t *p)
{
	return p ? p->recovered : 0;
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
		/* POWER OF TWO CHOICES, not strict least-loaded.  Picking
		 * the emptiest member outright makes every client choose
		 * the SAME one - measured: 11 of 12 clients on one node,
		 * which is the herding par 6.5 already rejects for
		 * write-time placement.  Sampling two and taking the
		 * better spreads without coordination and still avoids
		 * the worst node. */
		if (p->nmem >= 2) {
			int a = (int)(pd_rand(p) % (unsigned int)p->nmem);
			int b = (int)(pd_rand(p) % (unsigned int)p->nmem);

			if (b == a)
				b = (a + 1) % p->nmem;
			if (!pd_usable(&p->mem[a]))
				a = b;
			best = p->mem[a].free_mb >= p->mem[b].free_mb ||
				!pd_usable(&p->mem[b]) ? a : b;
		}
		break;
	case PERFD_POLICY_WEIGHTED: {
		/* random, weighted BY free space - the name's promise.
		 * Taking the maximum is not weighting, it is a tie-break
		 * that every client resolves identically. */
		long total = 0, r;

		for (i = 0; i < p->nmem; i++)
			if (pd_usable(&p->mem[i]))
				total += p->mem[i].free_mb > 0 ?
					p->mem[i].free_mb : 1;
		if (total <= 0)
			break;
		r = (long)(pd_rand(p) % (unsigned int)total);
		for (i = 0; i < p->nmem; i++) {
			if (!pd_usable(&p->mem[i]))
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
		/* round robin: random start, step.  It skipped NOTHING
		 * before C7 - not even a member this client had already
		 * seen fail - so the cursor is advanced past unusable
		 * members rather than landing on one and failing over. */
		int tries;

		best = -1;
		for (tries = 0; tries < p->nmem; tries++) {
			int cand = (int)(p->rr % (unsigned int)p->nmem);

			p->rr++;
			if (pd_usable(&p->mem[cand])) {
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
	for (i = 0; i < p->nspare; i++)
		if (p->spare[i]->active_node == p->mem[best].node) {
			perfd_t *sp = p->spare[i];
			int j;

			adopt_conn(p, sp);
			perfd_free(sp);
			for (j = i; j < p->nspare - 1; j++)
				p->spare[j] = p->spare[j + 1];
			p->nspare--;
			return;
		}
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

/* S70: what this handle knows, from its own dials, about member @i.
 * "active" serves this handle; "standby" has a spare open from here;
 * "down" is a member this handle failed to dial, and @why says how -
 * nothing is inferred from anyone else's view; "not-ready" the member
 * itself says it is not serving data yet; "idle" is fine as far as
 * known with no connection held from here. */
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

/* S70: the daemon version the member list reported; "" until a member
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

