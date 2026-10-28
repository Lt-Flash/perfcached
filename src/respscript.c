/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * respscript.c - EVAL, EVALSHA and SCRIPT on the RESP door (S170a): the
 * approved Symfony Lock scripts, named by their bodies' SHA1, run as C
 * through op_slock; any other script is refused.  Moved from verbs.c
 * unchanged (the 0.7.2 split, wave 1 - doc/MODULARITY.md).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "sha1.h"
#include "verbs.h"                      /* PC_SLOCK_*, PC_OP_*, pc_wrongtype_msg */
#include "respout.h"
#include "verbsint.h"

/* ---- S170a: EVAL / EVALSHA / SCRIPT - the approved Symfony scripts only.
 * A script is named by the SHA1 of its body, as Redis names it; one on
 * this list runs as C (slock_local), anything else is refused - clients
 * never run code of their own (DESIGN 12hp, 12il). */
static const struct {
	const char *sha;
	int script;
	int now_argv;                  /* "now" from ARGV[1], not the clock */
	int nargv;                     /* ARGV entries the body reads */
} slock_approved[] = {
	{ "c481d49b4a48e48a9eeacd5d51655b5fcbb19925", PC_SLOCK_PROBE, 0, 0 },
	{ "6fdee501a3714553b74c0d67687b921b90b05d18", PC_SLOCK_SAVE, 0, 3 },
	{ "abdbb1413a25737fc4cfb9cf79ace1a22dad6ab3", PC_SLOCK_SAVE, 1, 3 },
	{ "b5745b5be27d9033eb5d3eb254df2faf502b88df", PC_SLOCK_READ, 0, 3 },
	{ "dee54f77398d3c6d122b4111f3af8ccf1a93ac93", PC_SLOCK_READ, 1, 3 },
	{ "63be12a3515941092212f15c08327d60916de9fa", PC_SLOCK_REFRESH, 0, 3 },
	{ "fe7bee082a8220c6b621ace999a1ab57c4ab5145", PC_SLOCK_REFRESH, 1, 3 },
	{ "a70f7f5087a0cb2c2c9ca50d1f64e234c2628f0c", PC_SLOCK_RELEASE, 0, 1 },
	{ "3b6bdbbe16f821f49177b16c7707f331f015a779", PC_SLOCK_EXISTS, 0, 2 },
	{ "e27f94ac94f5d96f23f2e8697462233b6349a566", PC_SLOCK_EXISTS, 1, 2 },
};

static const char script_refused_msg[] = "script not approved on this "
	"server: perfcached runs only the Symfony Lock scripts it knows";

/* the approved entry for a 40-hex SHA (any case), or -1 */
static int slock_by_sha(const char *s, size_t n)
{
	char low[41];
	size_t i;

	if (n != 40)
		return -1;
	for (i = 0; i < 40; i++)
		low[i] = (char)(s[i] >= 'A' && s[i] <= 'F' ? s[i] + 32 : s[i]);
	low[40] = 0;
	for (i = 0; i < sizeof slock_approved / sizeof slock_approved[0]; i++)
		if (!strcmp(low, slock_approved[i].sha))
			return (int)i;
	return -1;
}

static int slock_by_body(const char *b, size_t n)
{
	char hex[41];

	pc_sha1_hex(b, n, hex);
	return slock_by_sha(hex, 40);
}

/* run approved script @ai: argv[2] numkeys, argv[3] the key, argv[4..] ARGV */
static int slock_run(struct resp_ctx *c, int ai)
{
	int sc = slock_approved[ai].script, res = 0, rc, lacking = 0;
	long long nk, ttl = 0, now_arg = -1;
	const char *tok = NULL;
	size_t toklen = 0;
	int argc;
	str k;

	if (resp_ll(c->argv[2], c->argl[2], &nk) < 0 || nk < 0 ||
	        nk > c->nargs - 3) {
		resp_err(c->out, "Number of keys can't be greater than number of args");
		return 0;
	}
	if (nk != 1) {
		resp_err(c->out, "this approved script takes exactly 1 key");
		return 0;
	}
	argc = c->nargs - 4;
	if (argc < slock_approved[ai].nargv) {
		resp_err(c->out, "too few arguments for this approved script");
		return 0;
	}
	if (c->argl[3] == 0 || c->argl[3] > KEY_MAX) {
		resp_err(c->out, "key too long");
		return 0;
	}
	k.s = c->argv[3];
	k.len = (int)c->argl[3];
	*c->keyp = c->argv[3];
	*c->klenp = c->argl[3];
	switch (sc) {
	case PC_SLOCK_RELEASE:         /* ARGV[1] token */
		tok = c->argv[4];
		toklen = c->argl[4];
		break;
	case PC_SLOCK_PROBE:
		break;
	default:                       /* ARGV[1] microtime, [2] token, [3] ttl ms */
		tok = c->argv[5];
		toklen = c->argl[5];
		if (sc != PC_SLOCK_EXISTS &&
		        (resp_ll(c->argv[6], c->argl[6], &ttl) < 0 || ttl < 0)) {
			resp_err(c->out, "the lock ttl (ARGV[3]) must be a whole "
				"number of milliseconds");
			return 0;
		}
		if (slock_approved[ai].now_argv) {
			/* now = math.floor(tonumber(ARGV[1]) * 1000) */
			char nb[64], *end;
			double d;
			size_t l = c->argl[4] < sizeof nb - 1 ? c->argl[4]
				: sizeof nb - 1;

			memcpy(nb, c->argv[4], l);
			nb[l] = 0;
			d = strtod(nb, &end);
			if (end == nb || d < 0) {
				resp_err(c->out, "ARGV[1] is not a time");
				return 0;
			}
			now_arg = (long long)(d * 1000.0);   /* d >= 0: floor */
		}
		break;
	}
	rc = op_slock(c->ht, &k, sc, now_arg, ttl, tok, toklen, &res,
		c->park_req, &lacking);
	if (rc == PC_OP_OK) {
		if (res)
			resp_int(c->out, 1);           /* Lua true */
		else
			resp_nil(c->out);              /* Lua false */
	} else if (rc == PC_OP_PARKED)
		resp_err(c->out, "busy");         /* park fallback only */
	else if (rc == PC_OP_ERR_GATE) {
		char msg[160];

		snprintf(msg, sizeof msg, slock_gate_fmt, lacking);
		resp_err(c->out, msg);
	} else if (rc == PC_OP_ERR_MODE)
		resp_err(c->out, slock_mode_msg);
	else if (rc == PC_OP_ERR_TYPE)
		resp_err_line(c->out, pc_wrongtype_msg);
	else if (rc == PC_OP_ERR_BUSY)
		resp_err_code(c->out, "TRYAGAIN", "cluster busy, retry");
	else if (rc == PC_OP_ERR_FWD)
		resp_err(c->out, "forward failed");
	else if (rc == PC_OP_ERR_WRFAIL)
		resp_err(c->out, wr_refused_msg());
	else if (rc == PC_OP_ERR_2BIG)
		resp_err(c->out, "lock token too long");
	else
		resp_err(c->out, "lock script failed");
	return 0;
}

/* EVAL body numkeys key [arg ...] / EVALSHA sha numkeys key [arg ...] */
int rh_eval(struct resp_ctx *c)
{
	int sha = resp_is(c->cmd, c->cl, "EVALSHA"), ai;

	if (c->nargs < 3) {
		resp_err(c->out, sha ? "wrong number of arguments for 'evalsha' "
			"command" : "wrong number of arguments for 'eval' command");
		return 0;
	}
	ai = sha ? slock_by_sha(c->argv[1], c->argl[1])
		: slock_by_body(c->argv[1], c->argl[1]);
	if (ai < 0) {
		if (sha)
			/* Redis's words: a client then sends SCRIPT LOAD, which
			 * says why when the body is not on the list */
			resp_err_line(c->out, "NOSCRIPT No matching script. Please "
				"use EVAL.");
		else
			resp_err(c->out, script_refused_msg);
		return 0;
	}
	if (c->nargs < 4) {
		resp_err(c->out, "this approved script takes exactly 1 key");
		return 0;
	}
	return slock_run(c, ai);
}

/* SCRIPT LOAD body | EXISTS sha [sha ...] | FLUSH [ASYNC|SYNC] */
int rh_script(struct resp_ctx *c)
{
	int i;

	if (c->nargs < 2) {
		resp_err(c->out, "wrong number of arguments for 'script' command");
		return 0;
	}
	if (resp_is(c->argv[1], c->argl[1], "LOAD") && c->nargs == 3) {
		char hex[41];

		pc_sha1_hex(c->argv[2], c->argl[2], hex);
		if (slock_by_sha(hex, 40) < 0) {
			resp_err(c->out, script_refused_msg);
			return 0;
		}
		resp_bulk(c->out, hex, 40);
		return 0;
	}
	if (resp_is(c->argv[1], c->argl[1], "EXISTS") && c->nargs >= 3) {
		resp_arr(c->out, c->nargs - 2);
		for (i = 2; i < c->nargs; i++)
			resp_int(c->out, slock_by_sha(c->argv[i], c->argl[i]) >= 0);
		return 0;
	}
	if (resp_is(c->argv[1], c->argl[1], "FLUSH") && c->nargs <= 3) {
		resp_simple(c->out, "OK");     /* nothing is ever cached */
		return 0;
	}
	resp_err(c->out, "unknown SCRIPT subcommand or wrong number of "
		"arguments");
	return 0;
}
