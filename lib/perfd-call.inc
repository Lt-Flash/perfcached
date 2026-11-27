/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov
 * Part of libperfd - see lib/LICENSE.  This file must stay
 * free of src/core includes; tools/sync-libperfd.sh exports
 * exactly the MIT set to consumers. */
/* perfd-call.inc - part of perfd.c, #included in order (one translation unit, statics file-local):
 * the blocking call path: the pipeline, the data-verb round trip and failover, perfd_command. */
/* a one-shot call starts only on an empty pipeline, so anything queued during one is that call's:
 * dropped on failure, or every later call fails "pipeline in progress" and never fails over */
static void pipeline_drop(perfd_t *p)
{
	p->nids = p->idhead = 0;
	p->qlen = 0;
}

/* one-shot on a data verb: refuse mid-pipeline, send, collect.
 * 0 = *out is the malloc'd response payload (*outlen bytes). */
static int bin_roundtrip_once(perfd_t *p, int verb, const char *col,
		const char *key, const void *val, size_t vlen,
		long long by, long long ttl, char **out, size_t *outlen)
{
	if (p->nids - p->idhead || p->qlen)
		return errk(p, PERFD_E_STATE, "pipeline in progress");
	if (enqueue_bin(p, verb, col, key, val, vlen, by, ttl) != 0 ||
	        perfd_flush(p) != 0) {
		pipeline_drop(p);
		return -1;
	}
	if (collect_head(p, out, outlen) != 0) {
		pipeline_drop(p);
		return -1;
	}
	return 0;
}

/* a member not READY yet, or a FAILED or HEALING one refusing a write, applied nothing: the request
 * (any verb) moves to another member, the refuser marked down until a refresh sees it ready */
static int not_ready_refusal(const perfd_t *t)
{
	return t->err_kind == PERFD_E_REFUSED && t->err[0] &&
		(strstr(t->err, "not READY") || strstr(t->err, "node is FAILED") ||
		 strstr(t->err, "node is HEALING"));
}

/* between calls, so nothing is in flight and no reply is lent out.  The standby must be
 * alive and say itself it is ready: a door opens long before that.  If not, again in a second */
static void spread_apply(perfd_t *p)
{
	long long now = now_ms_lib();
	const struct pd_member *m;
	perfd_t *c;
	int i, node, learned;

	if (now < p->spread_next_ms || p->nids - p->idhead || p->qlen)
		return;
	m = member_of(p, p->active_node);
	if (now > p->spread_until_ms || !spreading(p) ||
	        (p->spread_node == -1 && m && pd_spreadable(m))) {
		p->spread_due = 0;
		return;
	}
	p->spread_next_ms = now + 1000;
	if (p->spread_node == -1) {
		uint32_t seen = 0;
		int j;

		for (i = -1, j = 0; j < p->nspare; j++) {
			m = member_of(p, p->spare[j]->active_node);
			if (m && pd_spreadable(m) && randombytes_uniform(++seen) == 0)
				i = j;
		}
	} else {
		m = member_of(p, p->spread_node);
		i = m && pd_spreadable(m) ? spare_for(p, p->spread_node) : -1;
	}
	if (i < 0)
		return;
	c = p->spare[i];
	if (probe_child(p, c) != 0)
		return;
	/* a node still starting does not list itself: the standby keeps its name */
	node = c->active_node;
	learned = learn_members(c);
	c->active_node = node;
	m = member_of(c, node);
	if (learned <= 0 || !m || !pd_spreadable(m))
		return;
	p->spread_due = 0;
	p->spread_left = p->active_node;
	adopt_spare(p, i);
	p->next_refresh_ms = 0;        /* the old node's standby, at this call's maintenance */
}

static void relink(perfd_t *p);

/* the active link, looked at first if it has been quiet; a closed one is
 * replaced before anything is sent on it */
static int keep_live(perfd_t *p)
{
	if (p->is_child || !p->nmem || !link_closed(p))
		return 0;
	if (p->pin)
		return errk(p, PERFD_E_TRANSPORT, "the link to node %d closed while idle - "
			"nothing was sent, and a pinned handle does not move", p->active_node);
	if (failover(p) == 0) {
		relink(p);                     /* named and raised as the closed one was */
		return 0;
	}
	return errk(p, PERFD_E_TRANSPORT, "the link closed while idle and no member "
		"answered - nothing was sent");
}

/* the first member's answer, kept across a move - a re-send that fails too
 * reports both, the first's first, and a move that found nobody leaves it standing */
struct pd_first {
	char err[sizeof ((perfd_t *)0)->err];
	int kind, node;
};

static void first_keep(const perfd_t *p, struct pd_first *f)
{
	memcpy(f->err, p->err, sizeof f->err);
	f->kind = p->err_kind;
	f->node = p->active_node;
}

static int first_report(perfd_t *p, const struct pd_first *f, int moved)
{
	char second[sizeof f->err];

	if (!moved) {
		memcpy(p->err, f->err, sizeof p->err);
		p->err_kind = f->kind;
		return -1;
	}
	memcpy(second, p->err, sizeof second);
	return errk(p, p->err_kind, "node %d: %s; then node %d: %s", f->node, f->err,
		p->active_node, second);
}

/* the same for the key's standby, whose place the handle takes */
static perfd_t *pick_live(perfd_t *p, const char *col, const char *key)
{
	perfd_t *t = route_pick(p, col, key);

	if (t != p && link_closed(t)) {
		route_retire(p, t);
		p->hinted = 0;
		p->hh_got = 0;
		t = p;
	}
	return t;
}

static void relink(perfd_t *p);

/* the data verbs' failover: a dead connection retries once on a standby, a non-idempotent verb
 * does not, and a "not READY" refusal (nothing applied) moves to another member */
static int bin_roundtrip_body(perfd_t *p, int verb, const char *col,
		const char *key, const void *val, size_t vlen,
		long long by, long long ttl, char **out, size_t *outlen)
{
	perfd_t *t;
	struct pd_first f;
	int rc, died;

	if (p->active_stale && !p->is_child) {
		p->active_stale = 0;
		if (failover(p) == 0)          /* it was expelled */
			relink(p);
	}
	if (p->spread_due && !p->is_child)
		spread_apply(p);
	if (!p->is_child)
		relink(p);                     /* before a standby is picked: its failover may adopt one */
	t = pick_live(p, col, key);
	if (t == p && keep_live(p) != 0)
		return -1;
	if (p->route_hints) {
		t->want_hh = 1;                /* ask where it is */
		t->hh_got = 0;
	}
	if (t != p) {
		/* the owner's standby carries it; a failure there is that
		 * connection's problem, and the caller still has us */
		rc = bin_roundtrip_once(t, verb, col, key, val, vlen, by, ttl,
			out, outlen);
		if (rc == 0) {
			if (p->route_hints)
				hint_after(p, t, verb, col, key, ttl, *out,
					*outlen);
			/* *out is BORROWED from t, which the maintenance below can retire: it moves
			 * into this handle's buffer first */
			if (hand_reply(p, *out, *outlen, out) != 0)
				return -1;
			after_routed(p, t);
			return 0;
		}
		if (!t->poisoned && !not_ready_refusal(t))
			return errk(p, perfd_error_kind(t), "%s", perfd_error(t));
		died = t->poisoned;
		if (!died)                     /* not READY: this node answers */
			member_down(p, t->active_node, "", perfd_error(t));
		route_retire(p, t);    /* dead standby: fall through to us */
		p->hinted = 0;
		p->hh_got = 0;
		if (died && !bin_verb_idempotent(verb))
			/* the standby may have applied it before it died */
			return errk(p, PERFD_E_TRANSPORT, "connection lost during a counter update on the key's "
				"node - NOT replayed (add/sub are not idempotent); retry if a double "
				"increment is acceptable");
		if (keep_live(p) != 0)         /* routed calls left the active link unwatched */
			return -1;
	}
	rc = bin_roundtrip_once(p, verb, col, key, val, vlen, by, ttl,
		out, outlen);

	if (rc == 0) {
		if (p->route_hints)
			hint_after(p, p, verb, col, key, ttl, *out, *outlen);
		after_success(p);
		return 0;
	}
	if (!p->is_child && p->nmem && !p->poisoned && !p->ps_subs && not_ready_refusal(p)) {
		/* refused, not applied: another member, whatever the verb - not with
		 * subscriptions on this connection, which a move would end unsaid */
		first_keep(p, &f);
		if (failover(p) != 0)
			return first_report(p, &f, 0);
		relink(p);
		rc = bin_roundtrip_once(p, verb, col, key, val, vlen, by, ttl,
			out, outlen);
		if (rc == 0)
			after_success(p);
		else
			first_report(p, &f, 1);
		return rc;
	}
	if (!p->poisoned || p->is_child || !p->nmem)
		return rc;
	if (!bin_verb_idempotent(verb))
		return errk(p, PERFD_E_TRANSPORT, "connection lost during a counter update - NOT "
			"replayed (add/sub are not idempotent); retry if a double "
			"increment is acceptable");
	first_keep(p, &f);
	if (failover(p) != 0)
		return first_report(p, &f, 0);
	relink(p);
	rc = bin_roundtrip_once(p, verb, col, key, val, vlen, by, ttl,
		out, outlen);
	if (rc == 0)
		after_success(p);
	else
		first_report(p, &f, 1);
	return rc;
}

/* a notify hook inside a call would take that call's replies or overwrite its answer */
static int hook_refused(perfd_t *p)
{
	return errk(p, PERFD_E_STATE, "a notify hook may not call its handle while a call on it "
		"runs - deliveries between calls (perfd_maintain) may");
}

static int bin_roundtrip(perfd_t *p, int verb, const char *col,
		const char *key, const void *val, size_t vlen,
		long long by, long long ttl, char **out, size_t *outlen)
{
	int rc;

	if (p->hook_refuses)
		return hook_refused(p);
	p->in_call++;
	rc = bin_roundtrip_body(p, verb, col, key, val, vlen, by, ttl, out, outlen);
	p->in_call--;
	return rc;
}

int perfd_append_tree(perfd_t *p, const char *method,
		const unsigned char *params, size_t plen)
{
	return enqueue_cmd(p, method, params, plen);
}

/* the JSON edge of the pipeline: the params text becomes the request
 * tree here, and perfd_next_reply renders the reply tree back */
int perfd_append(perfd_t *p, const char *method, const char *params_json)
{
	struct pc_tw w;
	int rc;

	if (json_to_tree(p, params_json, &w) != 0)
		return -1;
	rc = enqueue_cmd(p, method, w.b, w.n);
	pc_tw_free(&w);
	return rc;
}

int perfd_flush(perfd_t *p)
{
	if (p->poisoned)
		return errk(p, PERFD_E_TRANSPORT, "connection poisoned");
	if (p->qlen && send_bytes(p, p->q, p->qlen) != 0)
		return -1;
	p->qlen = 0;
	return 0;
}

unsigned char *perfd_next_reply_tree(perfd_t *p, size_t *outlen)
{
	char *r;
	size_t n;

	if (p->qlen) {
		errk(p, PERFD_E_STATE, "flush before collecting replies");
		return NULL;
	}
	if (collect_head(p, &r, &n) != 0)
		return NULL;
	if (outlen)
		*outlen = n;
	/* the API hands the caller a buffer it frees (r is borrowed) */
	return (unsigned char *)dup_reply(p, r, n);
}

char *perfd_next_reply(perfd_t *p)
{
	unsigned char *t;
	size_t n;
	char *js;

	t = perfd_next_reply_tree(p, &n);
	if (!t)
		return NULL;
	js = tree_to_json(p, t, n, NULL);
	free(t);
	return js;
}

int perfd_pending(const perfd_t *p)
{
	return p->nids - p->idhead;
}

/* one-shot: refuse mid-pipeline, send, collect this reply.  0 = *out is
 * the malloc'd reply tree (*outlen bytes); -1 with p->err set. */
static int roundtrip_once(perfd_t *p, const char *method,
		const unsigned char *tree, size_t tlen, char **out,
		size_t *outlen)
{
	*out = NULL;
	*outlen = 0;
	if (p->nids - p->idhead || p->qlen)
		return errk(p, PERFD_E_STATE, "pipeline in progress");
	/* every sender of enable passes here: the replay on an adopted link and perfd_command too */
	if (!p->encrypted && p->nsec && !strcmp(method, "enable"))
		return errk(p, PERFD_E_STATE, "enable needs an encrypted connection - this one is "
			"authenticated but plaintext (libperfd: perfd_opts.encrypt = 1; perfcli: "
			"--encrypt, which -E and -e imply)");
	if (enqueue_cmd(p, method, tree, tlen) != 0 || perfd_flush(p) != 0) {
		pipeline_drop(p);
		return -1;
	}
	if (collect_head(p, out, outlen) != 0) {
		pipeline_drop(p);
		return -1;
	}
	return 0;
}

static int roundtrip(perfd_t *p, const char *method,
		const unsigned char *tree, size_t tlen, char **out,
		size_t *outlen);

/* the same request on the handle that owns the key: EVERY typed verb naming one key comes here, as
 * two operations on one key from one client are ordered only when both take the same road */
static int roundtrip_key(perfd_t *p, const char *method,
		const unsigned char *tree, size_t tlen, const char *col,
		const char *key, char **out, size_t *outlen)
{
	perfd_t *t = pick_live(p, col, key);

	if (t == p)
		return roundtrip(p, method, tree, tlen, out, outlen);
	if (roundtrip_once(t, method, tree, tlen, out, outlen) == 0) {
		/* as bin_roundtrip: out of t's buffer before the maintenance that may free t */
		if (hand_reply(p, *out, *outlen, out) != 0)
			return -1;
		after_routed(p, t);
		return 0;
	}
	if (!t->poisoned && !not_ready_refusal(t))
		return errk(p, perfd_error_kind(t), "%s", perfd_error(t));
	if (!t->poisoned) {            /* refused, not applied - the active link carries it */
		member_down(p, t->active_node, "", perfd_error(t));
		route_retire(p, t);
		p->hinted = 0;
		p->hh_got = 0;
		return roundtrip(p, method, tree, tlen, out, outlen);
	}
	route_retire(p, t);            /* dead standby: we carry it */
	if (!method_idempotent(method) || p->once)
		/* the standby may have applied it before it died and the primary would apply it
		 * again - the rule of the primary's own failover in roundtrip() */
		return errk(p, PERFD_E_TRANSPORT, "connection lost during '%s' on the key's node - "
			"NOT replayed (the verb is not idempotent); retry if the "
			"operation is safe to repeat", method);
	return roundtrip(p, method, tree, tlen, out, outlen);
}

static void name_replay(perfd_t *p);
static void enable_replay(perfd_t *p);

/* the adopted link is named and raised as the old one was, before the re-send */
static void relink(perfd_t *p)
{
	name_replay(p);
	enable_replay(p);
}

/* one retry on a pre-warmed standby when the CONNECTION died; a verb-level error is never retried,
 * nor a non-idempotent verb, which could apply twice - the caller sees the error and decides */
static int roundtrip_body(perfd_t *p, const char *method,
		const unsigned char *tree, size_t tlen, char **out,
		size_t *outlen)
{
	struct pd_first f;
	int rc;

	if (p->active_stale && !p->is_child) {
		p->active_stale = 0;
		if (failover(p) == 0)          /* it was expelled */
			relink(p);
	}
	if (p->spread_due && !p->is_child)
		spread_apply(p);
	if (!p->is_child)
		relink(p);
	if (keep_live(p) != 0)
		return -1;
	rc = roundtrip_once(p, method, tree, tlen, out, outlen);
	if (rc == 0) {
		after_success(p);
		return 0;
	}
	if (!p->is_child && p->nmem && !p->poisoned && !p->ps_subs && not_ready_refusal(p)) {
		/* refused, not applied: another member, whatever the verb (not with
		 * subscriptions here, as above) */
		first_keep(p, &f);
		if (failover(p) != 0)
			return first_report(p, &f, 0);
		relink(p);
		rc = roundtrip_once(p, method, tree, tlen, out, outlen);
		if (rc == 0)
			after_success(p);
		else
			first_report(p, &f, 1);
		return rc;
	}
	if (!p->poisoned || p->is_child || !p->nmem)
		return rc;
	if (!method_idempotent(method) || p->once)
		return errk(p, PERFD_E_TRANSPORT, "connection lost during '%s' - NOT replayed (the "
			"verb is not idempotent); retry if the operation is safe "
			"to repeat", method);
	first_keep(p, &f);
	if (failover(p) != 0)
		return first_report(p, &f, 0);
	relink(p);
	rc = roundtrip_once(p, method, tree, tlen, out, outlen);
	if (rc == 0)
		after_success(p);
	else
		first_report(p, &f, 1);
	return rc;
}

static int roundtrip(perfd_t *p, const char *method,
		const unsigned char *tree, size_t tlen, char **out,
		size_t *outlen)
{
	int rc;

	if (p->hook_refuses)
		return hook_refused(p);
	p->in_call++;
	rc = roundtrip_body(p, method, tree, tlen, out, outlen);
	p->in_call--;
	return rc;
}

/* a handle that raised privilege raises it again on the connection it adopted, once, before its
 * next call; a refusal is not reported here (the next privileged call gets the daemon's answer) */
static char *admin_call(perfd_t *p, const char *method, const char *col,
		const char *flag, const char *secret);
static void enable_replay(perfd_t *p)
{
	char *r;

	if (!p->enable_due || p->in_replay || !p->enable_sec)
		return;
	p->enable_due = 0;
	p->in_replay = 1;
	r = admin_call(p, "enable", NULL, NULL, p->enable_sec);
	free(r);
	p->in_replay = 0;
}

/* an enable that succeeded keeps its secret, however it was sent, and a disable drops it -
 * so a link the handle moves to is raised, or not, as the caller left the one it had */
static void priv_keep(perfd_t *p, const char *method, const unsigned char *params, size_t plen)
{
	struct pc_tv v;
	char sec[256], *keep;
	int sl;

	if (!strcmp(method, "disable")) {
		free(p->enable_sec);
		p->enable_sec = NULL;
		p->enable_due = 0;
		return;
	}
	if (strcmp(method, "enable") || !params || !plen || tv_root(p, &v, params, plen, method) != 0)
		return;
	sl = pc_tv_get_str(&v, 0, "secret", sec, sizeof sec - 1);
	tv_put(p, &v);
	if (sl <= 0)
		return;
	sec[sl] = 0;
	keep = strdup(sec);
	if (keep) {
		free(p->enable_sec);
		p->enable_sec = keep;
	}
	p->enable_due = 0;
}

unsigned char *perfd_command_tree(perfd_t *p, const char *method,
		const unsigned char *params, size_t plen, size_t *outlen)
{
	char *r;
	size_t n;

	name_replay(p);                /* before enable, so its log line is named */
	enable_replay(p);
	if (roundtrip(p, method, params, plen, &r, &n) != 0)
		return NULL;
	if (!p->in_replay)
		priv_keep(p, method, params, plen);
	if (outlen)
		*outlen = n;
	/* the API hands the caller a buffer it frees (r is borrowed) */
	return (unsigned char *)dup_reply(p, r, n);
}

/* the JSON edge: params text -> tree, reply tree -> text.  A convenience
 * for the tools, not a wire. */
char *perfd_command(perfd_t *p, const char *method,
		const char *params_json)
{
	struct pc_tw w;
	unsigned char *t;
	size_t n;
	char *js;

	if (json_to_tree(p, params_json, &w) != 0)
		return NULL;
	t = perfd_command_tree(p, method, w.b, w.n, &n);
	pc_tw_free(&w);
	if (!t)
		return NULL;
	js = tree_to_json(p, t, n, NULL);
	free(t);
	return js;
}

