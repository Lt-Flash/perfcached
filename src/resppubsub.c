/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* resppubsub.c - the RESP door's pub/sub over RESP2 (SUBSCRIBE, PSUBSCRIBE, UNSUBSCRIBE,
 * PUNSUBSCRIBE, PUBLISH, PUBSUB); it reaches verbs.c through verbsint.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "daemon.h"                    /* pc_worker_id */
#include "pubsub.h"
#include "respout.h"
#include "verbsint.h"

static void ps_confirm(struct pc_jw *out, const char *what, const char *name,
		size_t nlen, int count)
{
	resp_arr(out, 3);
	resp_bulk(out, what, strlen(what));
	if (name)
		resp_bulk(out, name, nlen);
	else
		pc_jw_lit(out, "$-1\r\n");
	resp_int(out, count);
}

static int ps_subscribe(struct resp_ctx *c, int pattern)
{
	const char *what = pattern ? "psubscribe" : "subscribe";
	size_t need = 0;
	int i;

	if (c->nargs < 2) {
		resp_err(c->out, pattern
			? "wrong number of arguments for 'psubscribe' command"
			: "wrong number of arguments for 'subscribe' command");
		return 0;
	}
	/* confirmations that outgrow the request's writer go to a heap one: a subscription made
	 * is never answered by "reply too large" alone */
	for (i = 1; i < c->nargs; i++)
		need += c->argl[i] + 48;
	if (need > c->out->cap - c->out->len)
		(void)pc_jw_init_heap(c->out, need + 64);
	for (i = 1; i < c->nargs; i++) {
		int n = pc_pubsub_subscribe(c->conn, pc_worker_id(), c->argv[i],
			c->argl[i], pattern);

		if (n < 0) {
			resp_err(c->out, "cannot subscribe (out of memory or "
				"the name is too long)");
			return 0;
		}
		ps_confirm(c->out, what, c->argv[i], c->argl[i], n);
	}
	return 0;
}

static int ps_unsubscribe(struct resp_ctx *c, int pattern)
{
	const char *what = pattern ? "punsubscribe" : "unsubscribe";
	int i;

	if (c->nargs < 2) {
		/* no names: every subscription of this kind goes, one
		 * confirmation each, and a bare nil when there were none */
		char name[PC_PUBSUB_NAME_MAX];
		size_t nlen, lim;
		int left, any = 0;

		/* a heap writer, and a pop only while the longest confirmation still fits: what is
		 * dropped is confirmed, what is not stays subscribed */
		(void)pc_jw_init_heap(c->out, 65536);
		lim = c->out->owned ? JW_HEAP_MAX : c->out->cap;
		while (c->out->len + sizeof name + 64 <= lim &&
		        pc_pubsub_pop(c->conn, pattern, name, sizeof name,
		        &nlen, &left)) {
			ps_confirm(c->out, what, name, nlen, left);
			any = 1;
		}
		if (!any)
			ps_confirm(c->out, what, NULL, 0, pc_pubsub_count(c->conn));
		return 0;
	}
	for (i = 1; i < c->nargs; i++)
		ps_confirm(c->out, what, c->argv[i], c->argl[i],
			pc_pubsub_unsubscribe(c->conn, c->argv[i], c->argl[i],
				pattern));
	return 0;
}

int rh_subscribe(struct resp_ctx *c)    { return ps_subscribe(c, 0); }
int rh_psubscribe(struct resp_ctx *c)   { return ps_subscribe(c, 1); }
int rh_unsubscribe(struct resp_ctx *c)  { return ps_unsubscribe(c, 0); }
int rh_punsubscribe(struct resp_ctx *c) { return ps_unsubscribe(c, 1); }

int rh_publish(struct resp_ctx *c)
{
	const char *pfx;

	if (c->nargs != 3) {
		resp_err(c->out, "wrong number of arguments for 'publish' command");
		return 0;
	}
	pfx = pc_pubsub_reserved(c->argv[1], c->argl[1]);
	if (pfx) {
		char m[96];

		snprintf(m, sizeof m, "channel prefix %s is reserved for the "
			"daemon's own events", pfx);
		resp_err(c->out, m);
		return 0;
	}
	resp_int(c->out, pc_pubsub_publish(c->argv[1], c->argl[1],
		c->argv[2], c->argl[2], 0));
	return 0;
}

struct ps_names { char **v; size_t *l; int n, cap, oom; };

static int ps_collect(const char *name, size_t nlen, int nsubs, void *arg)
{
	struct ps_names *ns = arg;
	char *cp;

	(void)nsubs;
	if (ns->n == ns->cap) {
		int ncap = ns->cap ? ns->cap * 2 : 32;
		char **nv = realloc(ns->v, sizeof *nv * (size_t)ncap);
		size_t *nl;

		if (!nv)
			return ns->oom = -1;
		/* kept at once: realloc has released the old block, and the
		 * caller frees what ns->v holds */
		ns->v = nv;
		nl = realloc(ns->l, sizeof *nl * (size_t)ncap);
		if (!nl)
			return ns->oom = -1;
		ns->l = nl;
		ns->cap = ncap;
	}
	cp = malloc(nlen ? nlen : 1);
	if (!cp)
		return ns->oom = -1;
	memcpy(cp, name, nlen);
	ns->v[ns->n] = cp;
	ns->l[ns->n] = nlen;
	ns->n++;
	return 0;
}

int rh_pubsub(struct resp_ctx *c)
{
	const char *sub = c->nargs > 1 ? c->argv[1] : "";
	size_t subl = c->nargs > 1 ? c->argl[1] : 0;
	int i;

	if (resp_is(sub, subl, "CHANNELS")) {
		struct ps_names ns = { NULL, NULL, 0, 0, 0 };

		if (c->nargs > 2 && c->argl[2] > PC_PUBSUB_NAME_MAX) {
			resp_err(c->out, "pattern too long");
			return 0;
		}
		pc_pubsub_channels(c->nargs > 2 ? c->argv[2] : NULL,
			c->nargs > 2 ? c->argl[2] : 0, ps_collect, &ns);
		/* a walk cut short is said, never answered as the whole list */
		if (ns.oom)
			resp_err(c->out, "out of memory listing the channels");
		else
			resp_arr(c->out, ns.n);
		for (i = 0; i < ns.n; i++) {
			if (!ns.oom)
				resp_bulk(c->out, ns.v[i], ns.l[i]);
			free(ns.v[i]);
		}
		free(ns.v);
		free(ns.l);
	} else if (resp_is(sub, subl, "NUMSUB")) {
		resp_arr(c->out, 2 * (c->nargs - 2));
		for (i = 2; i < c->nargs; i++) {
			resp_bulk(c->out, c->argv[i], c->argl[i]);
			resp_int(c->out, pc_pubsub_numsub(c->argv[i], c->argl[i]));
		}
	} else if (resp_is(sub, subl, "NUMPAT")) {
		resp_int(c->out, pc_pubsub_numpat());
	} else {
		unk_sub(c);
		resp_err(c->out, "unsupported PUBSUB subcommand (CHANNELS, "
			"NUMSUB, NUMPAT)");
	}
	return 0;
}
