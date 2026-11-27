/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* respjson.c - RedisJSON on the RESP door (JSON.SET/GET/DEL/NUMINCRBY/ARRAPPEND/
 * DEBUG), mapped onto the native path operations (pc_json_rmw) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "jsonpath.h"
#include "store.h"
#include "core/pcache_htable.h"
#include "cluster.h"                    /* pc_cluster_enabled, pc_shard_owner */
#include "verbs.h"
#include "respout.h"
#include "verbsint.h"

/* ---- RedisJSON on the RESP door ---------------------------------
 * JSON.SET's EX is an extension (SET + EXPIRE in one trip); without it, expiry stays */
int rh_json_debug(struct resp_ctx *c)
{
	if (c->nargs >= 2 && resp_is(c->argv[1], c->argl[1], "HELP")) {
		resp_arr(c->out, 2);
		resp_bulk_lit(c->out, "JSON.DEBUG MEMORY <key> [path] - reports "
			"memory usage");
		resp_bulk_lit(c->out, "JSON.DEBUG HELP - this message");
		return 0;
	}
	if (c->nargs >= 3 && resp_is(c->argv[1], c->argl[1], "MEMORY")) {
		char *frag = NULL;
		const char *fmsg = NULL;
		long long nv = 0;
		int fraglen = 0, cnt = 0, rc2;
		const char *jpath = "$";
		int jplen = 1;

		if (c->argl[2] == 0 || c->argl[2] > KEY_MAX) {
			resp_err(c->out, "key too long");
			return 0;
		}
		if (c->nargs >= 4) {
			jpath = c->argv[3];
			jplen = (int)c->argl[3];
		}
		rc2 = pc_json_rmw(c->ht, c->argv[2], (int)c->argl[2], PC_JOP_GET,
			jpath, jplen, NULL, 0, 0, 0, 0, 0, 0, 0, &frag,
			&fraglen, &nv, &cnt, &fmsg);
		if (rc2 < 0) {
			resp_err(c->out, fmsg);
			return 0;
		}
		if (rc2 == 0 && jplen == 1 &&
		        !pc_jp_valid_value(frag, (size_t)fraglen)) {
			resp_err_code(c->out, "WRONGTYPE", "the value at this key "
				"is not a JSON document");
			return 0;
		}
		resp_int(c->out, rc2 == 1 ? 0 : fraglen);
		return 0;
	}
	resp_err(c->out, "JSON.DEBUG: use HELP or MEMORY <key> [path]");
	return 0;
}

int rh_json(struct resp_ctx *c)
{
	char jpath[JP_NAME_PARAM];
	const char *fmsg = NULL, *sval = NULL;
	char *frag = NULL;
	size_t svlen = 0;
	long long jttl = 0, nv = 0, jby = 0;
	int jplen, rc2, fraglen = 0, cnt = 0, have_ttl = 0, nx = 0, xx = 0;
	int op, v2 = 0, i, rootpath = 0;
	str k;
	int rc;

	if (resp_is(c->cmd, c->cl, "JSON.SET"))
		op = PC_JOP_SET;
	else if (resp_is(c->cmd, c->cl, "JSON.GET"))
		op = PC_JOP_GET;
	else if (resp_is(c->cmd, c->cl, "JSON.DEL"))
		op = PC_JOP_DEL;
	else if (resp_is(c->cmd, c->cl, "JSON.NUMINCRBY"))
		op = PC_JOP_INCR;
	else
		op = PC_JOP_APPEND;
	if (c->nargs < 2 || (op == PC_JOP_SET && c->nargs < 4) ||
	        (op == PC_JOP_INCR && c->nargs != 4) ||
	        (op == PC_JOP_APPEND && c->nargs < 4)) {
		resp_err(c->out, "wrong number of arguments for this JSON command");
		return 0;
	}
	/* one path only: more paths or GET's formatting options are not implemented */
	if ((op == PC_JOP_GET || op == PC_JOP_DEL) && c->nargs > 3) {
		resp_err(c->out, op == PC_JOP_GET ? "unsupported JSON.GET option"
			: "unsupported JSON.DEL option");
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
	/* the path: `$...` is v2 (array-shaped reply); `.x`, `x` and `.`
	 * are the legacy spelling (bare reply); absent = the root */
	if (c->nargs >= 3) {
		const char *pp = c->argv[2];
		size_t pl = c->argl[2];

		if (pl == 0 || pl + 2 >= sizeof jpath) {
			resp_err(c->out, "bad path");
			return 0;
		}
		if (pp[0] == '$') {
			v2 = 1;
			memcpy(jpath, pp, pl);
			jplen = (int)pl;
		} else if (pl == 1 && pp[0] == '.') {
			jpath[0] = '$';
			jplen = 1;
		} else if (pp[0] == '.' || pp[0] == '[') {
			jpath[0] = '$';
			memcpy(jpath + 1, pp, pl);
			jplen = (int)pl + 1;
		} else {
			jpath[0] = '$';
			jpath[1] = '.';
			memcpy(jpath + 2, pp, pl);
			jplen = (int)pl + 2;
		}
	} else {
		jpath[0] = '$';
		jplen = 1;
		v2 = 0;                    /* JSON.GET key: the document, bare */
	}
	jpath[jplen] = 0;
	rootpath = jplen == 1;
	if (op == PC_JOP_SET) {
		sval = c->argv[3];
		svlen = c->argl[3];
		for (i = 4; i < c->nargs; i++) {
			if (resp_is(c->argv[i], c->argl[i], "NX")) {
				nx = 1;
			} else if (resp_is(c->argv[i], c->argl[i], "XX")) {
				xx = 1;
			} else if (resp_is(c->argv[i], c->argl[i], "EX") &&
			        i + 1 < c->nargs) {
				if (resp_ll(c->argv[i + 1], c->argl[i + 1], &jttl) < 0 ||
				        jttl <= 0 || jttl > PC_TTL_MAX) {
					resp_err(c->out, "invalid expire time in "
						"'json.set' command");
					return 0;
				}
				have_ttl = 1;
				i++;
			} else {
				resp_err(c->out, "unsupported JSON.SET option");
				return 0;
			}
		}
	} else if (op == PC_JOP_INCR) {
		if (resp_ll(c->argv[3], c->argl[3], &jby) < 0) {
			resp_err(c->out, "value is not an integer (JSON.NUMINCRBY "
				"is integer-only here)");
			return 0;
		}
	}
	/* a read-through key's shard owner, or a spread holder, is pulled
	 * from - the reply rendered when the document lands */
	if (op == PC_JOP_GET && pc_jget_park(c->ht, k.s, (size_t)k.len,
	        jpath, (size_t)jplen, (rootpath ? 1 : 0) | (v2 ? 2 : 0),
	        c->park_req)) {
		resp_err(c->out, "busy");          /* the park's fallback */
		return 0;
	}
	/* a key this node would forward (proxy non-holder, shard
	 * non-owner) is not served here - said, not half-done */
	if (pc_cluster_enabled() &&
	        ((pc_store_proxy_enabled(c->ht) &&
	          pcache_ht_probe(c->ht, &k, NULL, NULL, NULL) != 0) ||
	         (pc_store_shard_enabled(c->ht) &&
	          pc_shard_owner(col_name_of(c->ht), strlen(col_name_of(c->ht)),
	              k.s, (size_t)k.len) != 0))) {
		resp_err(c->out, "JSON on a key another node holds is not "
			"served on the RESP door - dial that node, or use the "
			"native door");
		return 0;
	}
	if (op == PC_JOP_DEL && rootpath) {
		unsigned char fl = 0;

		/* JSON.DEL of a whole string is RedisJSON's wrong-type refusal,
		 * as every other JSON verb on it is */
		if (pc_store_types_strict(c->ht) &&
		        pcache_ht_getflags(c->ht, &k, &fl) == 0 &&
		        !(fl & PCACHE_F_JSON)) {
			resp_err_line(c->out, pc_json_wrongtype_msg);
			return 0;
		}
		rc = op_del(c->ht, &k, c->park_req);
		if (rc == PC_OP_ERR_BUSY)          /* refused: as SET says it */
			resp_err_code(c->out, "TRYAGAIN", "cluster busy, retry");
		else if (rc == PC_OP_ERR_FWD)
			resp_err(c->out, "forward failed");
		else if (rc == PC_OP_ERR_WRFAIL)
			resp_err(c->out, wr_refused_msg());
		else
			resp_int(c->out, rc == PC_OP_OK ? 1 : 0);
		return 0;
	}
	if (op == PC_JOP_APPEND) {
		/* every value validated first, then one call with them joined: all or none */
		size_t tot = 0;
		char *jv, *p;

		for (i = 3; i < c->nargs; i++) {
			if (!pc_jp_valid_value(c->argv[i], c->argl[i])) {
				resp_err(c->out, jp_strerror(PC_JP_E_VAL));
				return 0;
			}
			tot += c->argl[i] + 1;
		}
		/* one value goes as it came: the common append allocates nothing */
		jv = c->nargs == 4 ? c->argv[3] : malloc(tot);
		if (!jv) {
			resp_err(c->out, "out of memory");
			return 0;
		}
		p = jv + (c->nargs == 4 ? c->argl[3] : 0);
		for (i = 3; c->nargs > 4 && i < c->nargs; i++) {
			if (i > 3)
				*p++ = ',';
			memcpy(p, c->argv[i], c->argl[i]);
			p += c->argl[i];
		}
		rc2 = pc_json_rmw(c->ht, k.s, k.len, PC_JOP_APPEND, jpath, jplen,
			jv, (int)(p - jv), c->nargs - 3, 0, 0, 0, 0, 0, &frag,
			&fraglen, &nv, &cnt, &fmsg);
		if (c->nargs > 4)
			free(jv);
		if (rc2 < 0) {
			resp_err(c->out, fmsg);
			return 0;
		}
		if (v2)
			resp_arr(c->out, 1);
		resp_int(c->out, cnt);
		return 0;
	}
	rc2 = pc_json_rmw(c->ht, k.s, k.len, op, jpath, jplen, sval,
		(int)svlen, jby, have_ttl, jttl, nx, xx, 0, &frag,
		&fraglen, &nv, &cnt, &fmsg);
	if (rc2 < 0) {
		/* NX on a present leaf, XX on an absent one: RedisJSON
		 * answers nil, not an error */
		if (op == PC_JOP_SET && ((nx &&
		        fmsg == jp_strerror(PC_JP_E_EXISTS)) || (xx &&
		        (fmsg == jp_strerror(PC_JP_E_NOLEAF) ||
		         !strcmp(fmsg, "no such key"))))) {
			resp_nil(c->out);
			return 0;
		}
		if (fmsg == pc_json_wrongtype_msg)
			resp_err_line(c->out, fmsg);
		else if (fmsg == jp_strerror(PC_JP_E_DOC))
			resp_err_code(c->out, "WRONGTYPE", "the value at this "
				"key is not a JSON document");
		else
			resp_err(c->out, fmsg);
		return 0;
	}
	switch (op) {
	case PC_JOP_SET:
		resp_simple(c->out, "OK");
		break;
	case PC_JOP_DEL:
		resp_int(c->out, rc2 == 1 ? 0 : 1);
		break;
	case PC_JOP_INCR:
		/* the new value, as JSON text: [n] for a $ path, n bare */
		{
			char nb[48];
			int nl = v2 ? snprintf(nb, sizeof nb, "[%lld]", nv)
				: snprintf(nb, sizeof nb, "%lld", nv);

			resp_bulk(c->out, nb, (size_t)nl);
		}
		break;
	default:                       /* GET */
		pc_resp_jget_reply(c->out, rc2 == 1 ? 1 : 0, frag, fraglen,
			rootpath, v2);
	}
	return 0;
}

void pc_resp_jget_reply(struct pc_jw *out, int st, const char *frag,
		int fraglen, int rootpath, int v2)
{
	if (st == 3) {
		resp_err_line(out, pc_json_wrongtype_msg);
	} else if (st == 2) {
		resp_err(out, "holder timed out or rejected");
	} else if (st == 1) {
		/* a missing key is nil; a missing path is [] on a
		 * $ path and an error on a legacy one, as RedisJSON */
		if (rootpath)
			resp_nil(out);
		else if (v2)
			resp_bulk_lit(out, "[]");
		else
			resp_err(out, "path does not exist");
	} else if (rootpath &&
	        !pc_jp_valid_value(frag, (size_t)fraglen)) {
		/* the whole value came back and it is not JSON: a
		 * plain SET lives here, not a document */
		resp_err_code(out, "WRONGTYPE", "the value at this key "
			"is not a JSON document");
	} else if (v2) {
		pc_jw_lit(out, "$");
		pc_jw_i64(out, (long long)fraglen + 2);
		pc_jw_lit(out, "\r\n[");
		pc_jw_raw(out, frag, (size_t)fraglen);
		pc_jw_lit(out, "]\r\n");
	} else
		resp_bulk(out, frag, (size_t)fraglen);
}
