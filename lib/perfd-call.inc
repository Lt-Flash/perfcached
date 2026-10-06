/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov
 * Part of libperfd - see lib/LICENSE.  This file must stay
 * free of src/core includes; tools/sync-libperfd.sh exports
 * exactly the MIT set to consumers. */
/*
 * perfd-call.inc - part of perfd.c, which #includes it in order: one translation
 * unit, so every static stays file-local and consumers still compile
 * perfd.c alone.  Not a compilation unit of its own.
 *
 * Holds the blocking call path: the pipeline, the data-verb round trip with its
 * failover, the method round trips and perfd_command.
 */
/*
 * S72: a one-shot call refuses to start unless the pipeline is empty
 * (the guard in each of the two functions below), so anything queued
 * DURING one belongs to that call.  When it fails, drop it.
 *
 * Leaving it behind is not a cosmetic leak: the id was never collected
 * and nothing else will ever collect it, so every LATER call fails the
 * same guard with "pipeline in progress" - on a handle whose caller
 * never pipelined anything.  Worse, that refusal happens before the
 * socket is touched, so `poisoned` is never set and the failover that
 * would have rescued the handle is never reached.  The handle stays
 * dead for the life of the process, and the message sends the reader
 * looking for a pipeline that does not exist.
 *
 * Confirmed in production 2026-09-04 by restarting a perfcached under
 * a live OpenSIPS fleet: one EOF, then "pipeline in progress" forever,
 * on every worker, until OpenSIPS itself was restarted.
 *
 * This does NOT reconnect - that stays the caller's decision, made
 * through perfd_state()/PERFD_ST_FAILED - it only ensures the handle
 * tells the truth about itself afterwards.
 */
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
		return err(p, "pipeline in progress");
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

/* the data verbs' half of the S34 failover; same rule - a dead
 * connection retries once on a standby, a non-idempotent verb does not */
static int bin_roundtrip(perfd_t *p, int verb, const char *col,
		const char *key, const void *val, size_t vlen,
		long long by, long long ttl, char **out, size_t *outlen)
{
	perfd_t *t;
	int rc;

	if (p->active_stale && !p->is_child) {
		p->active_stale = 0;
		failover(p);                   /* S107: it was expelled */
	}
	t = route_pick(p, col, key);       /* S35 (S295: or a hint) */
	if (p->route_hints) {
		t->want_hh = 1;                /* S295: ask where it is */
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
			/* S284: *out is BORROWED from t's buffer, and the
			 * maintenance below can free t - an "expelled" push
			 * drained there, or a failed idle ping, retires the
			 * standby (DESIGN 12iq).  The reply moves into this
			 * handle's own buffer first: nothing in after_routed
			 * collects into it (pings and recovery talk on the
			 * standbys, the members refresh takes an owned copy). */
			if (hand_reply(p, *out, *outlen, out) != 0)
				return -1;
			after_routed(p, t);
			return 0;
		}
		if (!t->poisoned)
			return err(p, "%s", perfd_error(t));
		route_retire(p, t);    /* dead standby: fall through to us */
		p->hinted = 0;
		p->hh_got = 0;
	}
	rc = bin_roundtrip_once(p, verb, col, key, val, vlen, by, ttl,
		out, outlen);

	if (rc == 0) {
		if (p->route_hints)
			hint_after(p, p, verb, col, key, ttl, *out, *outlen);
		after_success(p);
		return 0;
	}
	if (!p->poisoned || p->is_child || !p->nmem)
		return rc;
	if (!bin_verb_idempotent(verb))
		return err(p, "connection lost during a counter update - NOT "
			"replayed (add/sub are not idempotent); retry if a double "
			"increment is acceptable");
	if (failover(p) != 0)
		return rc;
	rc = bin_roundtrip_once(p, verb, col, key, val, vlen, by, ttl,
		out, outlen);
	if (rc == 0)
		after_success(p);
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
		return err(p, "connection poisoned");
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
		err(p, "flush before collecting replies");
		return NULL;
	}
	if (collect_head(p, &r, &n) != 0)
		return NULL;
	if (outlen)
		*outlen = n;
	/* the API hands the caller a buffer it frees (S284: r is borrowed) */
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
		return err(p, "pipeline in progress");
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

/* S35: the same request, but on the handle that owns the key.  Used by
 * the typed verbs, which know their collection and key; perfd_command()
 * and the pipeline API stay on the caller's handle, because only the
 * caller knows what an opaque params tree addresses.
 *
 * EVERY verb that names one key comes through here; roundtrip() is for
 * the keyless ones (ping, stats, members, keys, mget, publish).  S211:
 * jdel and jincr took roundtrip() while jset took this, so on an eager
 * fleet a jset landed on the key's owner and the jdel behind it on the
 * primary, where the owner's push had not arrived yet - it deleted
 * nothing and the document survived on every member.  Two operations
 * on one key from one client are ordered only when both take the same
 * road: routing is not load-bearing, uniformity is. */
static int roundtrip_key(perfd_t *p, const char *method,
		const unsigned char *tree, size_t tlen, const char *col,
		const char *key, char **out, size_t *outlen)
{
	perfd_t *t = route_pick(p, col, key);

	if (t == p)
		return roundtrip(p, method, tree, tlen, out, outlen);
	if (roundtrip_once(t, method, tree, tlen, out, outlen) == 0) {
		/* S284: as bin_roundtrip - out of t's buffer before the
		 * maintenance that may free t (DESIGN 12iq) */
		if (hand_reply(p, *out, *outlen, out) != 0)
			return -1;
		after_routed(p, t);
		return 0;
	}
	if (!t->poisoned)
		return err(p, "%s", perfd_error(t));
	route_retire(p, t);            /* dead standby: we carry it */
	if (!method_idempotent(method))
		/* S211: the standby may have applied it before it died and
		 * the primary would apply it again - the same rule as the
		 * primary's own failover in roundtrip() */
		return err(p, "connection lost during '%s' on the key's node - "
			"NOT replayed (the verb is not idempotent); retry if the "
			"operation is safe to repeat", method);
	return roundtrip(p, method, tree, tlen, out, outlen);
}

/* S34: one retry on a pre-warmed standby when the CONNECTION died.  A
 * verb-level error (bad params, no such collection) is not a failover
 * and is never retried; nor is a non-idempotent verb, which would risk
 * applying twice - the caller sees the error and decides. */
static int roundtrip(perfd_t *p, const char *method,
		const unsigned char *tree, size_t tlen, char **out,
		size_t *outlen)
{
	int rc;

	if (p->active_stale && !p->is_child) {
		p->active_stale = 0;
		failover(p);                   /* S107: it was expelled */
	}
	rc = roundtrip_once(p, method, tree, tlen, out, outlen);
	if (rc == 0) {
		after_success(p);
		return 0;
	}
	if (!p->poisoned || p->is_child || !p->nmem)
		return rc;
	if (!method_idempotent(method))
		return err(p, "connection lost during '%s' - NOT replayed (the "
			"verb is not idempotent); retry if the operation is safe "
			"to repeat", method);
	if (failover(p) != 0)
		return rc;
	rc = roundtrip_once(p, method, tree, tlen, out, outlen);
	if (rc == 0)
		after_success(p);
	return rc;
}

unsigned char *perfd_command_tree(perfd_t *p, const char *method,
		const unsigned char *params, size_t plen, size_t *outlen)
{
	char *r;
	size_t n;

	if (roundtrip(p, method, params, plen, &r, &n) != 0)
		return NULL;
	if (outlen)
		*outlen = n;
	/* the API hands the caller a buffer it frees (S284: r is borrowed) */
	return (unsigned char *)dup_reply(p, r, n);
}

/* the JSON edge: params text -> tree, reply tree -> text.  A convenience
 * for the tools, not a wire (DESIGN 12if). */
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

