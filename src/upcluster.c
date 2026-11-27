/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* upcluster.c - a Redis Cluster upstream: a thread asks a node for CLUSTER SHARDS (else
 * SLOTS) every CL_REFRESH_MS and after a MOVED, adds its nodes and swaps the slot map in. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "uprespc.h"
#include "compat/dprint.h"
#include "pc_slot.h"
#include "upstream.h"
#include "upstreamint.h"

#define CL_REFRESH_MS 10000
#define CL_TIMEOUT_MS 2000
#define CL_GAP_MS     500              /* rounds a MOVED asks for, at most this often */
#define CL_RANGES     1024             /* slot ranges kept a shard; more is logged */

static int cl_refresh_ms = CL_REFRESH_MS;   /* PERFCACHED_TEST_CL_REFRESH_MS: the tests' */

_Static_assert(PC_UP_MAX_SRV <= 32, "a shard's nodes are a 32-bit mask");
_Static_assert(PC_SLOTS == UP_SLOTS, "Redis Cluster's 16384 slots");

struct cl_node {
	char host[256];                    /* up_srv's: the asked node's name, whole */
	int port, master;
};

struct cl_shard {
	int lo[CL_RANGES], hi[CL_RANGES], nr, over;
	struct cl_node node[PC_UP_MAX_SRV];
	int nn;
};

/* the field @name's value in a flat [name, value, ...] reply (RESP2 or a RESP3 map) */
static const struct rv *field(const struct rv *m, const char *name)
{
	size_t nl = strlen(name);
	int i;

	for (i = 0; i + 1 < m->nel; i += 2)
		if (m->el[i].s && m->el[i].len == nl && !memcmp(m->el[i].s, name, nl))
			return &m->el[i + 1];
	return NULL;
}

static int fstr(const struct rv *v, char *out, size_t cap)
{
	if (!v || !v->s || !v->len || v->len >= cap)
		return -1;
	memcpy(out, v->s, v->len);
	out[v->len] = 0;
	return 0;
}

static int fint(const struct rv *v)
{
	return !v ? -1 : v->s ? atoi(v->s) : (int)v->n;
}

/* a node's address as the cluster gives it; "" or "?" is the node asked */
static void node_put(struct cl_shard *sh, const char *host, const char *asked, int port,
		int master)
{
	struct cl_node *n;

	if (sh->nn >= PC_UP_MAX_SRV || port <= 0 || port > 65535)
		return;
	n = &sh->node[sh->nn++];
	snprintf(n->host, sizeof n->host, "%s", !host[0] || !strcmp(host, "?") ? asked : host);
	n->port = port;
	n->master = master;
}

static void range_put(struct cl_shard *sh, int lo, int hi)
{
	if (lo < 0 || hi < lo || hi >= UP_SLOTS)
		return;
	if (sh->nr >= CL_RANGES) {
		sh->over++;                    /* said in cl_apply: those slots stay unserved */
		return;
	}
	sh->lo[sh->nr] = lo;
	sh->hi[sh->nr++] = hi;
}

/* CLUSTER SHARDS: [{slots: [lo, hi, ...], nodes: [{endpoint|ip, port, tls-port, role,
 * health}]}] - @tls: rediss://, so a node's tls-port where it has one */
static int parse_shards(const struct rv *v, const char *asked, int tls, struct cl_shard *out,
		int *n)
{
	int i, j;

	*n = 0;
	if (v->t != '*')
		return -1;
	for (i = 0; i < v->nel && *n < PC_UP_MAX_SRV; i++) {
		const struct rv *sl = field(&v->el[i], "slots"), *nd = field(&v->el[i], "nodes");
		struct cl_shard *sh = &out[*n];

		memset(sh, 0, sizeof *sh);
		if (!sl || !nd)
			return -1;
		for (j = 0; j + 1 < sl->nel; j += 2)
			range_put(sh, fint(&sl->el[j]), fint(&sl->el[j + 1]));
		for (j = 0; j < nd->nel; j++) {
			const struct rv *e = &nd->el[j];
			char h[64], role[16], health[16];

			health[0] = role[0] = 0;
			fstr(field(e, "health"), health, sizeof health);
			fstr(field(e, "role"), role, sizeof role);
			if (health[0] && strcmp(health, "online"))
				continue;          /* failed or loading: not read */
			int port = fint(field(e, "port")), tport = fint(field(e, "tls-port"));

			if (fstr(field(e, "endpoint"), h, sizeof h) != 0 || !strcmp(h, "?"))
				if (fstr(field(e, "ip"), h, sizeof h) != 0)
					h[0] = 0;
			if (tport > 0 && (tls || port <= 0))
				port = tport;
			node_put(sh, h, asked, port, !strcmp(role, "master"));
		}
		if (sh->nr)
			(*n)++;
	}
	return 0;
}

/* CLUSTER SLOTS: [[lo, hi, [host, port, id], [replica host, port, id], ...], ...] - one
 * entry a range, the master first; ranges of one master make one shard */
static int parse_slots(const struct rv *v, const char *asked, struct cl_shard *out, int *n)
{
	int i, j, k;

	*n = 0;
	if (v->t != '*')
		return -1;
	for (i = 0; i < v->nel; i++) {
		const struct rv *r = &v->el[i];
		char h[64];
		int p;

		if (r->nel < 3 || r->el[2].nel < 2)
			continue;
		if (fstr(&r->el[2].el[0], h, sizeof h) != 0)
			h[0] = 0;
		p = fint(&r->el[2].el[1]);
		for (k = 0; k < *n; k++)
			if (out[k].nn && out[k].node[0].port == p && !strcmp(out[k].node[0].host,
			        !h[0] || !strcmp(h, "?") ? asked : h))
				break;
		if (k == *n) {
			if (*n >= PC_UP_MAX_SRV)
				continue;
			memset(&out[k], 0, sizeof out[k]);
			(*n)++;
			for (j = 2; j < r->nel; j++) {
				char rh[64];

				if (r->el[j].nel < 2)
					continue;          /* malformed: no port */
				if (fstr(&r->el[j].el[0], rh, sizeof rh) != 0)
					rh[0] = 0;
				node_put(&out[k], rh, asked, fint(&r->el[j].el[1]), j == 2);
			}
		}
		range_put(&out[k], fint(&r->el[0]), fint(&r->el[1]));
	}
	return 0;
}

int cl_srv_find(struct up_col *uc, const char *host, int port)
{
	int i, n = srv_n(uc);

	/* not a gone one - its slot may be given to another server */
	for (i = 0; i < n; i++)
		if (!__atomic_load_n(&uc->srv[i].gone, __ATOMIC_ACQUIRE) &&
		    uc->srv[i].port == port && !strcmp(uc->srv[i].host, host))
			return i;
	return -1;
}

/* a node the cluster names: the server listed by that name, else one configured by another
 * name for the same host (a seed named by its DNS name, the cluster saying its address) */
static int cl_seed_of(struct up_col *uc, const char *host, int port)
{
	int i, n = srv_n(uc), k = cl_srv_find(uc, host, port);

	for (i = 0; k < 0 && i < n; i++)
		if (uc->srv[i].origin == UPO_CONFIG && uc->srv[i].port == port &&
		    same_host(uc->srv[i].host, host))
			k = i;
	return k;
}

static int ask(struct uconn *c, int argc, const char **av, struct rv *v)
{
	size_t al[4];
	int i;

	memset(v, 0, sizeof *v);
	for (i = 0; i < argc; i++)
		al[i] = strlen(av[i]);
	return send_cmd(c, now_ms() + CL_TIMEOUT_MS, argc, av, al) ||
		read_reply(c, v, now_ms() + CL_TIMEOUT_MS) ? -1 : 0;
}

/* the shards as one node gives them: 0 and *n, -1 it did not answer */
static int cl_ask(struct up_col *uc, const struct up_srv *s, struct cl_shard *sh, int *n)
{
	struct uconn c;
	struct rv v;
	int rc = -1;

	memset(&c, 0, sizeof c);
	c.uc = uc;
	c.fd = -1;
	if (uc_open(&c, s->host, s->port, now_ms() + CL_TIMEOUT_MS) != 0)
		return -1;
	if (uc->password) {
		const char *av[3] = { "AUTH", s->user[0] ? s->user : uc->password,
			uc->password };

		if (ask(&c, s->user[0] ? 3 : 2, av, &v) != 0 || !is_ok(&v)) {
			rv_free(&v);
			goto out;
		}
		rv_free(&v);
	}
	{
		const char *av[2] = { "CLUSTER", "SHARDS" };

		if (ask(&c, 2, av, &v) != 0)
			goto out;
		if (v.t != '-')
			rc = parse_shards(&v, s->host, uc->tls != NULL, sh, n);
		rv_free(&v);
	}
	if (rc != 0) {         /* before Redis 7 */
		const char *av[2] = { "CLUSTER", "SLOTS" };

		if (ask(&c, 2, av, &v) == 0 && v.t != '-')
			rc = parse_slots(&v, s->host, sh, n);
		rv_free(&v);
	}
out:
	uc_drop(&c);
	free(c.buf);
	return rc;
}

/* the shards found into a map and the servers: added (or found), mapped, the rest gone */
static void cl_apply(struct up_col *uc, const struct cl_shard *sh, int nsh)
{
	struct up_cmap *m = calloc(1, sizeof *m), *old;
	int seen[PC_UP_MAX_SRV] = { 0 }, i, j, k, nn, nodes = 0, masters = 0;
	char slots[PC_UP_MAX_SRV][96], txt[sizeof uc->cl_text];

	if (!m)
		return;
	memset(m->sh, UP_NOSHARD, sizeof m->sh);
	memset(slots, 0, sizeof slots);
	m->nsh = nsh;
	for (i = 0; i < nsh; i++) {
		m->master[i] = -1;
		for (j = 0; j < sh[i].nn; j++) {
			const struct cl_node *n = &sh[i].node[j];

			k = cl_seed_of(uc, n->host, n->port);
			if (k < 0 && uc->discover != PC_UP_DISC_NO)   /* a gone one back too */
				k = srv_add(uc, n->host, n->port, UPO_CLUSTER);
			if (k < 0)
				continue;
			seen[k] = 1;
			m->nodes[i] |= 1u << k;
			nodes++;
			if (n->master && m->master[i] < 0) {
				m->master[i] = k;
				masters++;
			}
		}
		if (sh[i].over && !__atomic_exchange_n(&uc->cl_over_said, 1, __ATOMIC_RELAXED))
			LM_WARN("collection '%s': a shard of the Redis Cluster serves its slots in more "
				"than %d ranges - %d range(s) not mapped, their keys refused until a "
				"rebalance joins them\n", uc->name, CL_RANGES, sh[i].over);
		for (j = 0; j < sh[i].nr; j++) {
			for (k = sh[i].lo[j]; k <= sh[i].hi[j]; k++)
				if (m->sh[k] == UP_NOSHARD) {
					m->sh[k] = (unsigned char)i;
					m->served++;
				}
		}
	}
	nn = srv_n(uc);
	for (k = 0; k < nn; k++) {
		size_t l = 0;

		for (i = 0; i < nsh; i++)
			if (m->nodes[i] & (1u << k))
				for (j = 0; j < sh[i].nr && l < sizeof slots[k]; j++)
					l += (size_t)snprintf(slots[k] + l, sizeof slots[k] - l,
						sh[i].lo[j] == sh[i].hi[j] ? "%s%d" : "%s%d-%d",
						l ? "," : "", sh[i].lo[j], sh[i].hi[j]);
	}
	pthread_mutex_lock(&uc->mx);
	old = uc->cmap;
	uc->cmap = m;
	for (k = 0; k < nn; k++) {
		uc->srv[k].shard = -1;
		for (i = 0; i < nsh; i++)
			if (m->nodes[i] & (1u << k))
				uc->srv[k].shard = i;
		memcpy(uc->srv[k].slots, slots[k], sizeof slots[k]);
	}
	pthread_mutex_unlock(&uc->mx);
	snprintf(txt, sizeof txt, "%d shard(s), %d master(s), %d node(s); %d of %d slots served",
		nsh, masters, nodes, m->served, UP_SLOTS);
	pthread_mutex_lock(&uc->sen_mx);
	if (strcmp(uc->cl_text, txt))
		LM_NOTICE("collection '%s': the Redis Cluster - %s\n", uc->name, txt);
	memcpy(uc->cl_text, txt, sizeof txt);
	pthread_mutex_unlock(&uc->sen_mx);
	__atomic_store_n(&uc->cl_unserved, (unsigned long long)(UP_SLOTS - m->served),
		__ATOMIC_RELAXED);
	free(old);
	if (uc->discover != PC_UP_DISC_NO)
		for (k = 0; k < nn; k++)
			if (uc->srv[k].origin == UPO_CLUSTER && !seen[k])
				srv_gone(&uc->srv[k]);
	topo_save(uc);
}

static void *cl_main(void *arg)
{
	struct up_col *uc = arg;
	struct cl_shard *sh = calloc(PC_UP_MAX_SRV, sizeof *sh);
	long long last = 0;

	if (!sh)
		return NULL;
	while (!uc->stop) {
		int n = srv_n(uc), i, nsh = 0, ok = 0;

		/* the seeds first, then the nodes found; any node of the cluster answers */
		for (i = 0; i < n && !ok; i++)
			if (!__atomic_load_n(&uc->srv[i].gone, __ATOMIC_RELAXED) &&
			    (uc->srv[i].origin == UPO_CLUSTER ||
			     __atomic_load_n(&uc->srv[i].is_cluster, __ATOMIC_RELAXED)))
				ok = cl_ask(uc, &uc->srv[i], sh, &nsh) == 0 && nsh > 0;
		ST_ADD(uc->cl_rounds, 1);
		if (ok)
			cl_apply(uc, sh, nsh);
		else {
			pthread_mutex_lock(&uc->sen_mx);
			snprintf(uc->cl_text, sizeof uc->cl_text, "no node of the cluster answered "
				"CLUSTER SHARDS or SLOTS - the last map stands");
			pthread_mutex_unlock(&uc->sen_mx);
		}
		last = now_ms();
		for (;;) {
			long long t = now_ms();

			if (t - last >= cl_refresh_ms || uc->stop)
				break;
			if (t - last >= CL_GAP_MS &&
			    __atomic_exchange_n(&uc->cl_kick, 0, __ATOMIC_ACQ_REL))
				break;
			usleep(50000);
		}
	}
	free(sh);
	return NULL;
}

void cl_kick(struct up_col *uc)
{
	__atomic_store_n(&uc->cl_kick, 1, __ATOMIC_RELEASE);
}

void cl_start(struct up_col *uc)
{
	const char *t = getenv("PERFCACHED_TEST_CL_REFRESH_MS");

	if (__atomic_exchange_n(&uc->cl_started, 1, __ATOMIC_ACQ_REL))
		return;
	if (t && atoi(t) > 0)
		cl_refresh_ms = atoi(t);
	__atomic_store_n(&uc->cluster, 1, __ATOMIC_RELEASE);
	if (pthread_create(&uc->cl_thr, NULL, cl_main, uc) != 0) {
		LM_ERR("collection '%s': cannot start the Redis Cluster's thread\n", uc->name);
		__atomic_store_n(&uc->cl_started, 0, __ATOMIC_RELEASE);
		return;
	}
	pthread_detach(uc->cl_thr);
}

uint32_t cl_key_nodes(const struct up_col *uc, const char *k, size_t kl, int *master)
{
	const struct up_cmap *m = uc->cmap;
	unsigned char sh;

	if (master)
		*master = -1;
	if (!m)
		return ~0u;
	sh = m->sh[pc_key_slot(k, kl)];
	if (sh == UP_NOSHARD)
		return 0;
	if (master)
		*master = m->master[sh];
	return m->nodes[sh];
}
