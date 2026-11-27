/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* respkv.c - the RESP door's key-value and connection commands, answered through verbs.c's
 * dialect-neutral data cores (verbsint.h) like the native doors, so the dialects cannot drift */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "json.h"
#include "store.h"
#include "compat/timer.h"                /* get_ticks */
#include "obs.h"                        /* CLIENT, SLOWLOG */
#include "proto.h"                      /* pc_conn_set_rt_fetcher */
#include "pubsub.h"                     /* RESET leaves pub/sub */
#include "verbs.h"
#include "respout.h"
#include "verbsint.h"
#include "mgetfan.h"
#include "presence.h"

int rh_reset(struct resp_ctx *c)
{
	char name[4096];
	size_t nlen;
	int left;

	/* back to a fresh connection's shape: out of subscribed mode and on db 0 (node-redis v4
	 * sends it on its return paths) */
	while (pc_pubsub_pop(c->conn, 0, name, sizeof name, &nlen, &left))
		;
	while (pc_pubsub_pop(c->conn, 1, name, sizeof name, &nlen, &left))
		;
	c->cur_col[0] = '0';
	*c->cur_collen = 1;
	resp_simple(c->out, "RESET");
	return 0;
}

int rh_echo(struct resp_ctx *c)
{
	if (c->nargs != 2)
		resp_err(c->out, "wrong number of arguments for 'echo' command");
	else
		resp_bulk(c->out, c->argv[1], c->argl[1]);
	return 0;
}

int rh_quit(struct resp_ctx *c)
{
	resp_simple(c->out, "OK");
	*c->quit = 1;
	return 0;
}

int rh_command(struct resp_ctx *c)
{
	resp_arr(c->out, 0);                   /* redis-cli probes; empty is fine */
	return 0;
}

int rh_config(struct resp_ctx *c)
{
	if (c->nargs >= 2 && resp_is(c->argv[1], c->argl[1], "GET"))
		resp_arr(c->out, 0);           /* "no such parameter" */
	else if (c->nargs >= 2 && resp_is(c->argv[1], c->argl[1], "RESETSTAT")) {
		pc_stats_reset();              /* what redis-cli expects */
		resp_simple(c->out, "OK");
	} else {
		unk_sub(c);
		resp_err(c->out, "unsupported CONFIG subcommand");
	}
	return 0;
}

int rh_dbsize(struct resp_ctx *c)
{
	pcache_ht_totals_t tot;
	pcache_htable_t *ht = resp_col(c->out, c->cur_col, *c->cur_collen);

	if (!ht)
		return 0;
	pcache_ht_totals(ht, &tot);
	resp_int(c->out, (long long)tot.entries);
	return 0;
}

int rh_flush(struct resp_ctx *c)
{
	resp_err(c->out, "FLUSHDB is not supported (delete keys "
		"explicitly)");
	return 0;
}

/* CLIENT SETINFO LIB-NAME|LIB-VER <value>, kept for CLIENT LIST and /clients; checked as
 * Redis does, since the value is written into a space-separated list */
static void rh_client_setinfo(struct resp_ctx *c)
{
	int ver;
	size_t i;

	if (c->nargs != 4) {
		resp_err(c->out, "wrong number of arguments for 'client|setinfo' "
			"command");
		return;
	}
	if (resp_is(c->argv[2], c->argl[2], "LIB-NAME"))
		ver = 0;
	else if (resp_is(c->argv[2], c->argl[2], "LIB-VER"))
		ver = 1;
	else {
		resp_err(c->out, "Unrecognized option");
		return;
	}
	for (i = 0; i < c->argl[3]; i++) {
		unsigned char ch = (unsigned char)c->argv[3][i];

		if (ch < '!' || ch > '~') {
			resp_err(c->out, ver ? "lib-ver cannot contain spaces, "
				"newlines or special characters" : "lib-name "
				"cannot contain spaces, newlines or special "
				"characters");
			return;
		}
	}
	pc_obs_conn_lib(c->obs, ver, c->argv[3], c->argl[3]);
	resp_simple(c->out, "OK");
}

int rh_client(struct resp_ctx *c)
{
	if (c->nargs == 3 && resp_is(c->argv[1], c->argl[1], "SETNAME")) {
		pc_obs_conn_name(c->obs, c->argv[2], c->argl[2]);
		/* a perfcached's read-through fetcher - no read-through
		 * on its behalf, ever */
		if (resp_is(c->argv[2], c->argl[2], "perfcached-readthrough"))
			pc_conn_set_rt_fetcher(c->conn);
		resp_simple(c->out, "OK");
	} else if (c->nargs == 2 && resp_is(c->argv[1], c->argl[1], "LIST")) {
		/* the Grafana Redis datasource's client panel reads
		 * exactly this: one k=v line per connection */
		struct pc_jw b;

		pc_jw_init(&b, c->scratch, c->scratch_cap);
		pc_obs_client_list(&b, get_ticks());
		if (b.overflow)
			resp_err(c->out, "client list too large");
		else
			resp_bulk(c->out, b.buf, b.len);
	} else if (c->nargs == 2 && resp_is(c->argv[1], c->argl[1],
	        "GETNAME")) {
		resp_bulk_lit(c->out, "");     /* name echo: minimal */
	} else if (c->nargs >= 2 &&
	           resp_is(c->argv[1], c->argl[1], "SETINFO")) {
		rh_client_setinfo(c);
	} else {
		/* a known subcommand with the wrong arguments is the client's bug;
		 * only a name we do not implement is a gap */
		if (c->nargs >= 2 &&
		    !resp_is(c->argv[1], c->argl[1], "SETNAME") &&
		    !resp_is(c->argv[1], c->argl[1], "LIST") &&
		    !resp_is(c->argv[1], c->argl[1], "GETNAME"))
			unk_sub(c);
		resp_err(c->out, "unsupported CLIENT subcommand");
	}
	return 0;
}

int rh_slowlog(struct resp_ctx *c)
{
	const char *sub = c->nargs >= 2 ? c->argv[1] : "";
	size_t subl = c->nargs >= 2 ? c->argl[1] : 0;

	if (resp_is(sub, subl, "GET")) {
		long long n = 10;

		if (c->nargs >= 3 &&
		        (resp_ll(c->argv[2], c->argl[2], &n) < 0 || n < -1))
			n = 10;
		pc_obs_slowlog_get(c->out, (int)(n < 0 ? 512 : n));
	} else if (resp_is(sub, subl, "LEN")) {
		resp_int(c->out, pc_obs_slowlog_len());
	} else if (resp_is(sub, subl, "RESET")) {
		pc_obs_slowlog_reset();
		resp_simple(c->out, "OK");
	} else {
		unk_sub(c);
		resp_err(c->out, "unsupported SLOWLOG subcommand");
	}
	return 0;
}

int rh_select(struct resp_ctx *c)
{
	if (c->nargs != 2 || c->argl[1] == 0 || c->argl[1] >= 40) {
		resp_err(c->out, "DB index is out of range");
		return 0;
	}
	{
		char nm[64];
		size_t nl = resp_map_index(c->argv[1], c->argl[1], nm,
			sizeof nm);

		if (!resp_col_allowed(nm, nl)) {
			resp_err(c->out, "DB index is out of range");
			return 0;
		}
		if (!pc_store_find(nm, nl)) {
			resp_err(c->out, "DB index is out of range (no "
				"collection with that name)");
			return 0;
		}
	}
	memcpy(c->cur_col, c->argv[1], c->argl[1]);
	*c->cur_collen = c->argl[1];
	resp_simple(c->out, "OK");
	return 0;
}

/* MEMORY USAGE: the record's own bytes (24-byte header, key, value) without slab rounding,
 * Redis's estimate class; SAMPLES is accepted and ignored */
int rh_memory(struct resp_ctx *c)
{
	unsigned int vln, exp;
	int is_ctr;
	str k;

	if (c->nargs < 3 || !resp_is(c->argv[1], c->argl[1], "USAGE")) {
		resp_err(c->out, "MEMORY supports only MEMORY USAGE "
			"<key> [SAMPLES n]");
		return 0;
	}
	if (c->argl[2] == 0 || c->argl[2] > KEY_MAX) {
		resp_err(c->out, "key too long");
		return 0;
	}
	k.s = c->argv[2];
	k.len = (int)c->argl[2];
	if (pcache_ht_probe(c->ht, &k, &vln, &exp, &is_ctr) == -2) {
		resp_nil(c->out);
		return 0;
	}
	resp_int(c->out, 24 + (long long)k.len + vln);
	return 0;
}

int rh_keys(struct resp_ctx *c)
{
	if (c->nargs != 2 || c->argl[1] >= 256) {
		resp_err(c->out, "wrong number of arguments for 'keys' "
			"command");
		return 0;
	}
	/* the verb only DECLARES the walk: the proto layer runs it a bounded chunk
	 * per event-loop turn, so the worker keeps serving between chunks */
	c->es->start = 1;
	c->es->ht = c->ht;
	c->es->patlen = (int)c->argl[1];
	if (c->es->patlen == 1 && c->argv[1][0] == '*')
		c->es->patlen = -1;            /* full walk, skip the matcher */
	else
		memcpy(c->es->pat, c->argv[1], c->argl[1]);
	c->es->limit = 100000;                 /* the keys-verb hard cap */
	return 0;
}

/* a key absent here that a get would ask another node for: parked, the
 * reply written when the pull lands (what the caller writes is the fallback) */
static void pres_park(struct resp_ctx *c, str *k, int kind)
{
	if (op_pres_miss(c->ht, k, kind, c->park_req) == PC_OP_PARKED) {
		*c->keyp = k->s;
		*c->klenp = (size_t)k->len;
	}
}

static const char *type_name(int none, unsigned int fl)
{
	return none ? "none" : (fl & PCACHE_F_JSON) ? "ReJSON-RL" :
		(fl & PCACHE_F_RL) ? "ratelimit" : (fl & PCACHE_F_HASH) ? "hash" :
		(fl & PCACHE_F_LOCK) ? "zset" : "string";
}

/* TTL's seconds (-2 absent, -1 none), or PTTL's milliseconds */
static void ttl_reply(struct pc_jw *w, long long nv, int pttl)
{
	resp_int(w, nv > 0 && pttl ? nv * 1000 : nv);
}

int rh_exists(struct resp_ctx *c)
{
	long long cnt = 0;
	str k;
	int i;

	if (c->nargs < 2) {
		resp_err(c->out, "wrong number of arguments for 'exists' "
			"command");
		return 0;
	}
	if (c->nargs > 2 && c->conn && mget_fans(c->ht)) {
		/* fanned out as MGET is */
		struct pc_mget *mg;
		const char *code, *why;

		if (op_mget_fan(c->ht, (const char *const *)c->argv + 1,
		        c->argl + 1, (unsigned int)(c->nargs - 1), 1, &mg, &code,
		        &why) < 0) {
			resp_err_code(c->out, code, why);
			return 0;
		}
		if (pc_mget_waiting(mg)) {
			pc_mget_park(mg);
			return 0;
		}
		pc_mget_resp(mg, c->out);
		pc_mget_free(mg);
		return 0;
	}
	for (i = 1; i < c->nargs; i++) {
		if (c->argl[i] == 0 || c->argl[i] > KEY_MAX)
			continue;
		k.s = c->argv[i];
		k.len = (int)c->argl[i];
		if (probe_or_present(c->ht, &k, NULL, NULL, NULL, NULL) == 0)
			cnt++;
		else if (c->nargs == 2)
			pres_park(c, &k, PC_PRES_EXISTS);
	}
	resp_int(c->out, cnt);
	return 0;
}

int rh_type(struct resp_ctx *c)
{
	str k;

	if (c->nargs != 2 || c->argl[1] == 0 || c->argl[1] > KEY_MAX) {
		resp_err(c->out, "wrong number of arguments for 'type' command");
		return 0;
	}
	k.s = c->argv[1];
	k.len = (int)c->argl[1];
	{
		/* the recorded type, whichever redis_types - loose
		 * only relaxes what the commands accept */
		unsigned char fl = 0;
		int none = pcache_ht_getflags(c->ht, &k, &fl) != 0 &&
			pcache_ht_probe_present(c->ht, &k, NULL, &fl) != 0;

		if (none)
			pres_park(c, &k, PC_PRES_TYPE);
		resp_simple(c->out, type_name(none, fl));
	}
	return 0;
}

int rh_ttl(struct resp_ctx *c)
{
	unsigned int vln, exp;
	int is_ctr, rc, pttl;
	long long nv;
	str k;

	if (c->nargs != 2 || c->argl[1] == 0 || c->argl[1] > KEY_MAX) {
		resp_err(c->out, "wrong number of arguments for 'ttl' command");
		return 0;
	}
	k.s = c->argv[1];
	k.len = (int)c->argl[1];
	pttl = resp_is(c->cmd, c->cl, "PTTL");
	rc = probe_or_present(c->ht, &k, &vln, &exp, &is_ctr, NULL);
	if (rc == -2) {
		nv = -2;
		pres_park(c, &k, pttl ? PC_PRES_PTTL : PC_PRES_TTL);
	} else if (exp == 0)
		nv = -1;
	else
		nv = (long long)exp - (long long)get_ticks();
	ttl_reply(c->out, nv, pttl);
	return 0;
}

void pc_pres_resp(struct pc_jw *w, int kind, int found, unsigned int fl,
		unsigned int ttl_left)
{
	if (kind == PC_PRES_EXISTS)
		resp_int(w, found ? 1 : 0);
	else if (kind == PC_PRES_TYPE)
		resp_simple(w, type_name(!found, fl));
	else
		ttl_reply(w, !found ? -2 : ttl_left ? (long long)ttl_left : -1,
			kind == PC_PRES_PTTL);
}

int rh_mget(struct resp_ctx *c)
{
	unsigned int exp;
	str k, v;
	int i;

	if (c->nargs < 2) {
		resp_err(c->out, "wrong number of arguments for 'mget' "
			"command");
		return 0;
	}
	if (c->conn && mget_fans(c->ht)) {
		/* a key on another node is looked up there, never a nil for a key the fleet
		 * holds; one whose lookup did not answer turns the reply into -TRYAGAIN */
		struct pc_mget *mg;
		const char *code, *why;

		if (op_mget_fan(c->ht, (const char *const *)c->argv + 1,
		        c->argl + 1, (unsigned int)(c->nargs - 1), 0, &mg, &code,
		        &why) < 0) {
			resp_err_code(c->out, code, why);
			return 0;
		}
		if (pc_mget_waiting(mg)) {
			pc_mget_park(mg);          /* the door attaches it */
			return 0;
		}
		pc_mget_resp(mg, c->out);
		pc_mget_free(mg);
		return 0;
	}
	/* local fetches, misses stay nil: right for a node that holds every key
	 * it may be asked for */
	resp_arr(c->out, c->nargs - 1);
	for (i = 1; i < c->nargs; i++) {
		if (c->argl[i] == 0 || c->argl[i] > KEY_MAX) {
			resp_nil(c->out);
			continue;
		}
		k.s = c->argv[i];
		k.len = (int)c->argl[i];
		HOT(c->ht, k.s, k.len);
		{
			unsigned char fl = 0;

			if (pcache_ht_fetch_ex(c->ht, &k, &v, &exp, &fl) == 0) {
				/* Redis answers nil for a document
				 * here, not an error */
				if ((fl & PCACHE_F_TYPES) &&
				        pc_store_types_strict(c->ht))
					resp_nil(c->out);
				else
					resp_bulk(c->out, v.s, (size_t)v.len);
				free(v.s);
			} else {
				resp_nil(c->out);
			}
		}
	}
	return 0;
}

int rh_get(struct resp_ctx *c)
{
	unsigned int exp;
	int rc;
	str k;

	if (c->nargs != 2) {
		resp_err(c->out, "wrong number of arguments for 'get' command");
		return 0;
	}
	if (c->argl[1] == 0 || c->argl[1] > KEY_MAX) {
		resp_err(c->out, "key too long");
		return 0;
	}
	k.s = c->argv[1];
	k.len = (int)c->argl[1];
	*c->keyp = c->argv[1];
	*c->klenp = c->argl[1];
	{
		char *gb = get_buf();
		unsigned int gl = 0;

		if (!gb) {
			resp_err(c->out, "get failed");
			return 0;
		}
		rc = op_get_buf(c->ht, &k, gb, VAL_MAX, &gl, &exp,
			c->park_req);
		if (rc == PC_OP_OK) {
			resp_bulk(c->out, gb, (size_t)gl);
			return 0;
		}
	}
	if (rc == PC_OP_ERR_TYPE)
		resp_err_line(c->out, pc_wrongtype_msg);
	else if (rc == PC_OP_ERR_GET) {
		resp_err(c->out, "get failed");
	} else {
		resp_nil(c->out);              /* miss; also the park fallback */
	}
	return 0;
}

/* DELEX key [IFEQ v | IFNE v] (Redis 8.4); without a condition it is DEL of one key.
 * IFDEQ/IFDNE compare a DIGEST, which perfcached does not have: refused by name */
int rh_delex(struct resp_ctx *c)
{
	int rc, kind = 0;
	str k;

	if (c->nargs != 2 && c->nargs != 4) {
		resp_err(c->out, "wrong number of arguments for 'delex' command");
		return 0;
	}
	if (c->argl[1] == 0 || c->argl[1] > KEY_MAX) {
		resp_err(c->out, "key too long");
		return 0;
	}
	k.s = c->argv[1];
	k.len = (int)c->argl[1];
	*c->keyp = c->argv[1];
	*c->klenp = c->argl[1];
	if (c->nargs == 2) {
		rc = op_del(c->ht, &k, c->park_req);
		resp_int(c->out, rc == PC_OP_OK ? 1 : 0);   /* parked: DEL's fallback */
		return 0;
	}
	if (resp_is(c->argv[2], c->argl[2], "IFEQ"))
		kind = PC_CMP_DEL_IFEQ;
	else if (resp_is(c->argv[2], c->argl[2], "IFNE"))
		kind = PC_CMP_DEL_IFNE;
	else if (resp_is(c->argv[2], c->argl[2], "IFDEQ") ||
	        resp_is(c->argv[2], c->argl[2], "IFDNE")) {
		resp_err(c->out, "IFDEQ/IFDNE are not supported: perfcached "
			"has no DIGEST - compare the value with IFEQ/IFNE");
		return 0;
	} else {
		resp_err(c->out, "syntax error");
		return 0;
	}
	rc = op_cmp(c->ht, &k, c->argv[3], c->argl[3], NULL, 0, kind,
		c->park_req);
	if (rc == PC_OP_OK)
		resp_int(c->out, 1);
	else if (rc == PC_OP_ABSENT)
		resp_int(c->out, 0);
	else if (rc == PC_OP_ERR_TYPE)
		resp_err(c->out, delex_type_msg);
	else if (rc == PC_OP_ERR_MODE)
		resp_err(c->out, cmp_mode_msg);
	else if (rc == PC_OP_PARKED)
		resp_err(c->out, "busy");         /* park fallback only */
	else if (rc == PC_OP_ERR_BUSY)
		resp_err_code(c->out, "TRYAGAIN", "cluster busy, retry");
	else if (rc == PC_OP_ERR_WRFAIL)
		resp_err(c->out, wr_refused_msg());
	else if (rc == PC_OP_ERR_2BIG)
		resp_err(c->out, "value too large");
	else
		resp_err(c->out, "forward failed");
	return 0;
}

int rh_del(struct resp_ctx *c)
{
	long long cnt = 0;
	int i, rc;
	str k;

	if (c->nargs < 2) {
		resp_err(c->out, "wrong number of arguments for 'del' command");
		return 0;
	}
	if (c->nargs == 2) {
		/* single key: parks exactly (proxy/shard forward) */
		if (c->argl[1] == 0 || c->argl[1] > KEY_MAX) {
			resp_err(c->out, "key too long");
			return 0;
		}
		k.s = c->argv[1];
		k.len = (int)c->argl[1];
		*c->keyp = c->argv[1];
		*c->klenp = c->argl[1];
		rc = op_del(c->ht, &k, c->park_req);
		if (rc == PC_OP_PARKED)
			resp_int(c->out, 0);   /* park fallback only */
		else
			resp_int(c->out, rc == PC_OP_OK ? 1 : 0);
		return 0;
	}
	/* multi-key: a forwarded delete still runs at its owner but its ack
	 * completes into nobody, so it counts optimistically */
	for (i = 1; i < c->nargs; i++) {
		unsigned int throwaway = 0;

		if (c->argl[i] == 0 || c->argl[i] > KEY_MAX)
			continue;
		k.s = c->argv[i];
		k.len = (int)c->argl[i];
		rc = op_del(c->ht, &k, &throwaway);
		if (rc == PC_OP_OK || rc == PC_OP_PARKED)
			cnt++;
	}
	resp_int(c->out, cnt);
	return 0;
}

int rh_expire(struct resp_ctx *c)
{
	long long ttl;
	int rc;
	str k;

	if (c->nargs != 3 || c->argl[1] == 0 || c->argl[1] > KEY_MAX ||
	        resp_ll(c->argv[2], c->argl[2], &ttl) < 0) {
		resp_err(c->out, "wrong number of arguments for 'expire' "
			"command");
		return 0;
	}
	/* the AT forms become relative here: the TTL machinery is tick-based and never
	 * learns wall time; a past deadline takes the ttl <= 0 delete branch, as in Redis */
	if (resp_is(c->cmd, c->cl, "PEXPIREAT")) {
		struct timespec tw;

		clock_gettime(CLOCK_REALTIME, &tw);
		ttl -= (long long)tw.tv_sec * 1000 +
			tw.tv_nsec / 1000000;
	} else if (resp_is(c->cmd, c->cl, "EXPIREAT")) {
		ttl -= (long long)time(NULL);
	}
	if (resp_is(c->cmd, c->cl, "PEXPIRE") ||
	        resp_is(c->cmd, c->cl, "PEXPIREAT"))
		ttl = (ttl + 999) / 1000;
	if (ttl <= 0) {
		/* Redis deletes on a non-positive relative expire */
		k.s = c->argv[1];
		k.len = (int)c->argl[1];
		*c->keyp = c->argv[1];
		*c->klenp = c->argl[1];
		rc = op_del(c->ht, &k, c->park_req);
		if (rc == PC_OP_PARKED)
			resp_int(c->out, 0);
		else
			resp_int(c->out, rc == PC_OP_OK ? 1 : 0);
		return 0;
	}
	k.s = c->argv[1];
	k.len = (int)c->argl[1];
	*c->keyp = c->argv[1];
	*c->klenp = c->argl[1];
	rc = op_expire(c->ht, &k, ttl, c->park_req);
	if (rc == PC_OP_PARKED)
		resp_int(c->out, 0);           /* park fallback only */
	else
		resp_int(c->out, rc == PC_OP_OK ? 1 : 0);
	return 0;
}

int rh_set(struct resp_ctx *c)
{
	int is_setex = resp_is(c->cmd, c->cl, "SETEX");
	int is_psetex = resp_is(c->cmd, c->cl, "PSETEX");
	int vi = (is_setex || is_psetex) ? 3 : 2, i, cond = 0, ckind = 0;
	const char *cmp = NULL;
	size_t cmplen = 0;
	long long ttl;
	int rc;
	str k, v;

	ttl = 0;
	if (is_setex || is_psetex) {
		if (c->nargs != 4) {
			resp_err(c->out, "wrong number of arguments");
			return 0;
		}
		if (resp_ll(c->argv[2], c->argl[2], &ttl) < 0 || ttl <= 0) {
			resp_err(c->out, "invalid expire time");
			return 0;
		}
		if (is_psetex)
			ttl = (ttl + 999) / 1000;
	} else {
		if (c->nargs < 3) {
			resp_err(c->out, "wrong number of arguments for "
				"'set' command");
			return 0;
		}
		for (i = 3; i < c->nargs; i++) {
			if (resp_is(c->argv[i], c->argl[i], "EX") ||
			        resp_is(c->argv[i], c->argl[i], "PX")) {
				int px = resp_is(c->argv[i], c->argl[i], "PX");

				if (i + 1 >= c->nargs ||
				        resp_ll(c->argv[i + 1],
				            c->argl[i + 1], &ttl) < 0 ||
				        ttl <= 0) {
					resp_err(c->out, "invalid expire "
						"time in 'set' command");
					return 0;
				}
				if (px)
					ttl = (ttl + 999) / 1000;
				i++;
			} else if (resp_is(c->argv[i], c->argl[i], "NX") ||
			        resp_is(c->argv[i], c->argl[i], "XX")) {
				int want = resp_is(c->argv[i], c->argl[i], "NX")
					? PC_SETCOND_NX : PC_SETCOND_XX;

				if ((cond && cond != want) || ckind) {
					resp_err(c->out, "syntax error");
					return 0;
				}
				cond = want;
			} else if (resp_is(c->argv[i], c->argl[i], "IFEQ") ||
			        resp_is(c->argv[i], c->argl[i], "IFNE")) {
				/* Redis 8.4's compare-and-set: one condition,
				 * never beside NX/XX */
				if (cond || ckind || i + 1 >= c->nargs) {
					resp_err(c->out, "syntax error");
					return 0;
				}
				ckind = resp_is(c->argv[i], c->argl[i], "IFEQ")
					? PC_CMP_SET_IFEQ : PC_CMP_SET_IFNE;
				cmp = c->argv[i + 1];
				cmplen = c->argl[i + 1];
				i++;
			} else if (resp_is(c->argv[i], c->argl[i], "IFDEQ") ||
			        resp_is(c->argv[i], c->argl[i], "IFDNE")) {
				resp_err(c->out, "IFDEQ/IFDNE are not supported: "
					"perfcached has no DIGEST - compare the value "
					"with IFEQ/IFNE");
				return 0;
			} else {
				/* KEEPTTL/GET/EXAT/PXAT: refuse, never
				 * half-honour */
				resp_err(c->out, "unsupported SET option");
				return 0;
			}
		}
	}
	if (c->argl[1] == 0 || c->argl[1] > KEY_MAX) {
		resp_err(c->out, "key too long");
		return 0;
	}
	k.s = c->argv[1];
	k.len = (int)c->argl[1];
	v.s = c->argv[vi];
	v.len = (int)c->argl[vi];
	*c->keyp = c->argv[1];
	*c->klenp = c->argl[1];
	rc = ckind ? op_cmp(c->ht, &k, cmp, cmplen, &v, ttl, ckind,
			c->park_req)
		: cond ? op_setcond(c->ht, &k, &v, ttl, cond, c->park_req)
		: op_set(c->ht, &k, &v, ttl, c->park_req, 0);
	if (rc == PC_OP_OK)
		resp_simple(c->out, "OK");
	else if (rc == PC_OP_ABSENT)
		resp_nil(c->out);         /* NX/XX or IFEQ/IFNE did not hold */
	else if (rc == PC_OP_ERR_TYPE)
		resp_err_line(c->out, pc_wrongtype_msg);
	else if (rc == PC_OP_ERR_MODE)
		resp_err(c->out, ckind ? cmp_mode_msg : setcond_mode_msg);
	else if (rc == PC_OP_PARKED)
		resp_err(c->out, "busy");         /* park fallback only */
	else if (rc == PC_OP_ERR_BUSY)
		/* Redis Cluster's retryable code: the request has NOT happened, and a
		 * stock client backs off and retries where -ERR would abort it */
		resp_err_code(c->out, "TRYAGAIN", "cluster busy, retry");
	else if (rc == PC_OP_ERR_FWD)
		resp_err(c->out, "forward failed");
	else if (rc == PC_OP_ERR_WRFAIL)
		/* not TRYAGAIN: retrying cannot help while the node
		 * is out of the map */
		resp_err(c->out, wr_refused_msg());
	else if (rc == PC_OP_ERR_FULL)
		resp_err(c->out, "cache full");
	else
		resp_err(c->out, "value too large");
	return 0;
}

/* RL.HIT key window_ms [limit] -> *2 :count :allowed (1 when no limit is given);
 * a perfcached extension, not a Redis command */
int rh_rlhit(struct resp_ctx *c)
{
	long long win, lim = 0, cnt = 0;
	int al = 0, rc, lacking = 0;
	str k;

	if (c->nargs != 3 && c->nargs != 4) {
		resp_err(c->out, "wrong number of arguments for 'RL.HIT'");
		return 0;
	}
	if (resp_ll(c->argv[2], c->argl[2], &win) < 0 || win < 1 ||
	        win > 86400000LL) {
		resp_err(c->out, "window_ms must be 1..86400000");
		return 0;
	}
	if (c->nargs == 4 && (resp_ll(c->argv[3], c->argl[3], &lim) < 0 ||
	        lim < 1 || lim >= PC_RL_MAX)) {
		resp_err(c->out, "limit must be 1..5999");
		return 0;
	}
	if (c->argl[1] == 0 || c->argl[1] > KEY_MAX) {
		resp_err(c->out, "key too long");
		return 0;
	}
	k.s = c->argv[1];
	k.len = (int)c->argl[1];
	*c->keyp = c->argv[1];
	*c->klenp = c->argl[1];
	rc = op_rlhit(c->ht, &k, win, lim, &cnt, &al, c->park_req, &lacking);
	if (rc == PC_OP_OK) {
		resp_arr(c->out, 2);
		resp_int(c->out, cnt);
		resp_int(c->out, al);
	} else if (rc == PC_OP_PARKED)
		resp_err(c->out, "busy");         /* park fallback only */
	else if (rc == PC_OP_ERR_GATE)
		resp_err(c->out, rl_gate_text(lacking));
	else if (rc == PC_OP_ERR_MODE)
		resp_err(c->out, rl_mode_msg);
	else if (rc == PC_OP_ERR_TYPE)
		resp_err_line(c->out, pc_wrongtype_msg);
	else if (rc == PC_OP_ERR_BUSY)
		resp_err_code(c->out, "TRYAGAIN", "cluster busy, retry");
	else if (rc == PC_OP_ERR_FWD)
		resp_err(c->out, "forward failed");
	else if (rc == PC_OP_ERR_WRFAIL)
		resp_err(c->out, wr_refused_msg());
	else
		resp_err(c->out, "rate-limit hit failed");
	return 0;
}

int rh_incr(struct resp_ctx *c)
{
	int has_by = resp_is(c->cmd, c->cl, "INCRBY") ||
		resp_is(c->cmd, c->cl, "DECRBY");
	int neg = resp_is(c->cmd, c->cl, "DECR") ||
		resp_is(c->cmd, c->cl, "DECRBY");
	long long by, nv;
	int rc;
	str k;

	by = 1;
	if (has_by && (c->nargs != 3 ||
	        resp_ll(c->argv[2], c->argl[2], &by) < 0)) {
		resp_err(c->out, "value is not an integer or out of range");
		return 0;
	}
	if (!has_by && c->nargs != 2) {
		resp_err(c->out, "wrong number of arguments");
		return 0;
	}
	if (c->argl[1] == 0 || c->argl[1] > KEY_MAX) {
		resp_err(c->out, "key too long");
		return 0;
	}
	if (neg)
		by = -by;
	k.s = c->argv[1];
	k.len = (int)c->argl[1];
	*c->keyp = c->argv[1];
	*c->klenp = c->argl[1];
	/* ttl -1 = PRESERVE: Redis INCR never touches the expiry */
	rc = op_addsub(c->ht, &k, by, -1, &nv, c->park_req, 0);
	if (rc == PC_OP_PARKED)
		resp_err(c->out, "busy");         /* park fallback only */
	else if (rc == PC_OP_ERR_BUSY)
		resp_err_code(c->out, "TRYAGAIN", "cluster busy, retry");
	else if (rc == PC_OP_ERR_FWD)
		resp_err(c->out, "forward failed");
	else if (rc == PC_OP_ERR_TYPE)
		resp_err_line(c->out, pc_wrongtype_msg);
	else if (rc == PC_OP_ERR_NOTINT)
		resp_err(c->out, "value is not an integer or out of range");
	else if (rc != PC_OP_OK)
		resp_err(c->out, "cache full");
	else
		resp_int(c->out, nv);
	return 0;
}
