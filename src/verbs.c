/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * verbs.c — the v1 verb set, text (JSON-RPC) dialect (task S8).
 * See verbs.h.  TTLs on the wire are RELATIVE seconds; the store keeps
 * absolute ticks (0 = never), converted here at the boundary.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pthread.h>
#include <arpa/inet.h>

#include "compat/timer.h"
#include "json.h"
#include "jsonpath.h"
#include "store.h"
#include "verbs.h"
#include "core/pcache_htable.h"
#include "core/pcache_arena.h"
#include "wal.h"
#include "rdb.h"
#include "recover.h"
#include "compat/compat.h"
#include "cluster.h"
#include "pc_slot.h"                   /* the shared key->slot */
#include "obs.h"                       /* S53: the Grafana surfaces */
#include "core/pcache_mem.h"
#include "clterm.h"
#include "proto.h"                     /* PC_VERB_* / PC_BIN_F_ERR */
#include "version.h"
#include "storage.h"                   /* pc_wal_identity for stats */
#include "walprobe.h"                  /* pc_wal_probe_result for stats */
#include "daemon.h"                    /* pc_wal_reprobe (the probe verb) */

#define VAL_MAX     PCACHE_CELL_MAX          /* largest storable value */
/* the WIRE form of a value can be 4/3 bigger than the stored bytes
 * (the b64 leg of decision #1) - without this headroom binary values
 * silently capped at ~48KB while UTF-8 reached the cell max (caught by
 * the PHP client's 50KB random-bytes roundtrip) */
#define VAL_WIRE_MAX (VAL_MAX + VAL_MAX / 2)
#define KEY_MAX     4096
#define JP_NAME_PARAM 512                    /* longest path string */
#define ERR(code, msg) do { *errmsg = (msg); return (code); } while (0)
#define E_PARAMS    (-32602)                 /* JSON-RPC invalid params */

/* per-thread wire-value scratch: set/mset decoded a value into a
 * malloc'd VAL_WIRE_MAX buffer per request - cheap under a caching
 * allocator, costly under one that mmaps (see proto.c's jw scratch for
 * the full story).  Never freed: workers live as long as the daemon. */
static __thread char *val_scratch;

static char *val_buf(void)
{
	if (!val_scratch)
		val_scratch = malloc(VAL_WIRE_MAX);
	return val_scratch;
}

/* Per-thread READ scratch, same rules as val_scratch above.  A GET used
 * to cost a malloc + memcpy + free on top of the copy-out the seqlock
 * already requires: pcache_ht_fetch_ex() copies the record into its own
 * per-thread buffer, then allocates a second one, copies again, and the
 * verb layer frees it after writing it to the wire.  Measured at ~8% of
 * the binary dialect's CPU (page zeroing 6.06%, cfree 1.27%, malloc
 * 0.75%) - the largest addressable cost on the fastest path.
 * VAL_MAX-sized, so PCACHE_E_TOOSMALL cannot happen for a stored value. */
static __thread char *get_scratch_v;

static char *get_buf(void)
{
	if (!get_scratch_v)
		get_scratch_v = malloc(VAL_MAX);
	return get_scratch_v;
}

/* relative seconds -> absolute ticks (0 stays 0 = never) */
static unsigned int ttl_to_abs(long long ttl)
{
	if (ttl <= 0)
		return 0;
	return get_ticks() + (unsigned int)ttl;
}

/* pull a string param into a caller buffer, honouring "<key>":"..",
 * optional sibling "<key>_enc":"b64".  Returns byte length or -1. */
static int get_str(const char *line, const struct pc_jtok *t, int ntok,
		int obj, const char *key, const char *enc_key, char *buf,
		size_t cap)
{
	int tv = pc_json_get(line, t, ntok, obj, key), tenc, len;

	if (tv < 0 || t[tv].type != PC_J_STR)
		return -1;
	len = pc_json_unescape(line, &t[tv], buf, cap);
	if (len < 0)
		return -1;
	if (enc_key) {
		tenc = pc_json_get(line, t, ntok, obj, enc_key);
		if (tenc >= 0 && pc_json_streq(line, &t[tenc], "b64")) {
			int d = pc_b64_dec(buf, (size_t)len, buf, cap);

			if (d < 0)
				return -1;
			len = d;
		}
	}
	return len;
}

static int get_int(const char *line, const struct pc_jtok *t, int ntok,
		int obj, const char *key, long long *out)
{
	int tv = pc_json_get(line, t, ntok, obj, key);
	char num[24];
	int n;

	if (tv < 0 || t[tv].type != PC_J_PRIM)
		return -1;
	n = t[tv].end - t[tv].start;
	if (n <= 0 || n >= (int)sizeof num)
		return -1;
	memcpy(num, line + t[tv].start, n);
	num[n] = 0;
	*out = strtoll(num, NULL, 10);
	return 0;
}

/* IPv4 dotted-quad without pulling inet_ntoa's static buffer into a
 * multithreaded reply path */
static const char *pc_inet_ntop4(struct in_addr a, char *buf, size_t cap)
{
	unsigned int v = ntohl(a.s_addr);

	snprintf(buf, cap, "%u.%u.%u.%u", (v >> 24) & 0xff, (v >> 16) & 0xff,
		(v >> 8) & 0xff, v & 0xff);
	return buf;
}

/* reverse lookup: the WAL logs by collection NAME */
static const char *col_name_of(pcache_htable_t *ht)
{
	int i;

	for (i = 0; i < pc_store_count(); i++)
		if (pc_store_ht(i) == ht)
			return pc_store_name(i);
	return "?";
}

/* resolve the "col" param to a live collection */
static pcache_htable_t *get_col(const char *line, const struct pc_jtok *t,
		int ntok, int params, const char **errmsg)
{
	int tc = params < 0 ? -1 : pc_json_get(line, t, ntok, params, "col");
	pcache_htable_t *ht;

	if (tc < 0 || t[tc].type != PC_J_STR) {
		*errmsg = "missing col";
		return NULL;
	}
	ht = pc_store_find(line + t[tc].start,
		(size_t)(t[tc].end - t[tc].start));
	if (!ht)
		*errmsg = "no such collection";
	return ht;
}

/* ---- dialect-neutral data-verb cores ------------------------------------
 * Both codecs (text JSON-RPC and the binary frames) answer through
 * these, so the cluster semantics - pull-on-miss, holder forwarding,
 * placement, WAL, tombstones - cannot drift between dialects.  The
 * codec owns only the reply SHAPE (including its own park-failure
 * fallback, pre-written before returning PC_OP_PARKED). */


/* pc_*_begin() returns 0 for three unrelated reasons and this used to
 * flatten all of them into "forward failed".  Only a full parked-request
 * table is the daemon applying backpressure, and only that is worth a
 * client retrying.  S38. */
static int fwd_fail_code(void)
{
	return pc_cluster_last_fail() == PC_CLFAIL_BUSY
		? PC_OP_ERR_BUSY : PC_OP_ERR_FWD;
}

/* Defined beside serving_denied(), where the two gates are explained
 * together; declared here because the op_ helpers come first. */
static int writes_denied(void);

/* The miss tail, shared by both get flavours: everything that happens
 * once the local table has said "absent". */
static int op_get_miss(pcache_htable_t *ht, str *k, unsigned int *park)
{
	if (pc_store_shard_enabled(ht)) {
		/* deterministic ownership: unicast the owner; an owner miss
		 * is AUTHORITATIVE - no broadcast, no negative cache - except
		 * inside the reshard grace, when the data may not have moved
		 * yet and one broadcast covers the window */
		if (pc_cluster_enabled()) {
			const char *cn = col_name_of(ht);
			int owner = pc_shard_owner(cn, strlen(cn), k->s,
				(size_t)k->len);
			unsigned int req = 0;

			if (owner)
				req = pc_pull_begin_at(cn, strlen(cn), k->s,
					(size_t)k->len, owner);
			else if (pc_shard_grace())
				req = pc_pull_begin(cn, strlen(cn), k->s,
					(size_t)k->len);
			if (req) {
				*park = req;
				return PC_OP_PARKED;
			}
		}
		return PC_OP_ABSENT;
	}
	/* a local miss asks the cluster: broadcast (store mode) or
	 * locator-unicast-first (proxy mode) - unless the negative cache
	 * already heard "no" */
	if (pc_cluster_enabled() && pc_store_pull_enabled(ht)) {
		const char *cn = col_name_of(ht);

		if (!pc_neg_hit(cn, strlen(cn), k->s, (size_t)k->len)) {
			unsigned int req = 0;
			int holder = pc_store_proxy_enabled(ht) ?
				pc_loc_get(cn, strlen(cn), k->s, (size_t)k->len) : 0;

			if (holder)
				req = pc_pull_begin_at(cn, strlen(cn), k->s,
					(size_t)k->len, holder);
			if (!req)
				req = pc_pull_begin(cn, strlen(cn), k->s,
					(size_t)k->len);
			if (req) {
				*park = req;
				return PC_OP_PARKED;
			}
		}
	}
	return PC_OP_ABSENT;
}

/* As op_get, but the value lands in @buf and nothing is allocated or
 * freed - @buf stays valid until this thread's next get.  @cap must be
 * VAL_MAX so a stored value can never be too large for it. */
static int op_get_buf(pcache_htable_t *ht, str *k, char *buf, unsigned int cap,
		unsigned int *vlen, unsigned int *exp, unsigned int *park)
{
	unsigned int needed = 0;
	int rc = pcache_ht_fetch_buf_ex(ht, k, buf, cap, vlen, &needed, exp);

	if (rc == 0)
		return PC_OP_OK;
	if (rc != -2)
		return PC_OP_ERR_GET;   /* TOOSMALL lands here too: cap is VAL_MAX */
	return op_get_miss(ht, k, park);
}

static int op_del(pcache_htable_t *ht, str *k, unsigned int *park)
{
	const char *cn;
	int rc;

	if (writes_denied())
		return PC_OP_ERR_WRFAIL;

	if (pc_store_shard_enabled(ht) && pc_cluster_enabled()) {
		int owner;

		cn = col_name_of(ht);
		owner = pc_shard_owner(cn, strlen(cn), k->s, (size_t)k->len);
		if (owner) {
			unsigned int req = pc_fwd_begin(owner, 1, cn,
				strlen(cn), k->s, (size_t)k->len, NULL, 0, 0, 0);

			if (req) {
				*park = req;
				return PC_OP_PARKED;
			}
			return PC_OP_ABSENT;   /* plane down: honest no-op */
		}
		/* self-owned: the local remove is the whole story - one
		 * owner means no tombstone plane */
		rc = pcache_ht_remove(ht, k);
		if (rc == 1)
			pc_wal_del(cn, k->s, k->len);
		return rc == 1 ? PC_OP_OK : PC_OP_ABSENT;
	}
	if (pc_store_proxy_enabled(ht) && pc_cluster_enabled() &&
	        pcache_ht_probe(ht, k, NULL, NULL, NULL) != 0) {
		int target;

		cn = col_name_of(ht);
		target = pc_loc_get(cn, strlen(cn), k->s, (size_t)k->len);
		if (target) {
			unsigned int req = pc_fwd_begin(target, 1, cn,
				strlen(cn), k->s, (size_t)k->len, NULL, 0, 0, 0);

			if (req) {
				pc_loc_clear(cn, strlen(cn), k->s,
					(size_t)k->len);
				*park = req;
				return PC_OP_PARKED;
			}
		}
		/* unknown holder: the tombstone broadcast removes it
		 * wherever it lives; the reply reflects local absence */
		pc_tombstone_send(cn, strlen(cn), k->s, (size_t)k->len);
		return PC_OP_ABSENT;
	}
	rc = pcache_ht_remove(ht, k);
	if (rc == 1) {
		cn = col_name_of(ht);
		pc_wal_del(cn, k->s, k->len);
		if (pc_cluster_enabled())
			pc_tombstone_send(cn, strlen(cn), k->s, (size_t)k->len);
	}
	return rc == 1 ? PC_OP_OK : PC_OP_ABSENT;
}

static int op_expire(pcache_htable_t *ht, str *k, long long ttl)
{
	int rc = pcache_ht_touch(ht, k, ttl_to_abs(ttl));

	if (rc == 1)
		pc_wal_touch(col_name_of(ht), k->s, k->len, ttl_to_abs(ttl),
			pcache_last_ver);
	return rc == 1 ? PC_OP_OK : PC_OP_ABSENT;
}

/* add/sub (by pre-negated for sub): PC_OP_OK fills *nv */
static int op_addsub(pcache_htable_t *ht, str *k, long long by, long long ttl,
		long long *nv, unsigned int *park, int probed)
{

	if (writes_denied())
		return PC_OP_ERR_WRFAIL;
	if (pc_store_shard_enabled(ht) && pc_cluster_enabled()) {
		const char *cn = col_name_of(ht);
		int owner = pc_shard_owner(cn, strlen(cn), k->s,
			(size_t)k->len);

		if (owner) {
			unsigned int req = pc_fwd_begin(owner, 2, cn,
				strlen(cn), k->s, (size_t)k->len, NULL, 0,
				ttl < 0 ? PCACHE_EXP_PRESERVE :
				ttl > 0 ? (unsigned int)ttl : 0, by);

			if (req) {
				*park = req;
				return PC_OP_PARKED;
			}
			return fwd_fail_code();
		}
		/* self-owned: the owner serializes - fall through local */
	}
	if (pc_store_proxy_enabled(ht) && pc_cluster_enabled() &&
	        pcache_ht_probe(ht, k, NULL, NULL, NULL) != 0) {
		const char *cn = col_name_of(ht);
		int target = pc_loc_get(cn, strlen(cn), k->s, (size_t)k->len);

		if (!target && !probed &&
		        !pc_neg_hit(cn, strlen(cn), k->s, (size_t)k->len)) {
			/* same probe gate as op_set: a counter re-write must
			 * find its holder, not re-place */
			unsigned int req = pc_probe_fwd_begin(2, cn,
				strlen(cn), k->s, (size_t)k->len, NULL, 0,
				ttl < 0 ? PCACHE_EXP_PRESERVE :
				ttl > 0 ? (unsigned int)ttl : 0, by);

			if (req) {
				*park = req;
				return PC_OP_PARKED;
			}
		}
		if (!target)
			target = pc_place();
		if (target) {
			unsigned int req = pc_fwd_begin(target, 2, cn,
				strlen(cn), k->s, (size_t)k->len, NULL, 0,
				ttl < 0 ? PCACHE_EXP_PRESERVE :
				ttl > 0 ? (unsigned int)ttl : 0, by);

			if (req) {
				*park = req;
				return PC_OP_PARKED;
			}
			return fwd_fail_code();
		}
	}
	{
		unsigned int eff = 0;

		if (pcache_ht_add_ex(ht, k, by,
		        ttl < 0 ? PCACHE_EXP_PRESERVE : ttl_to_abs(ttl),
		        nv, &eff) < 0)
			return PC_OP_ERR_NOTINT;
		/* the absolute-resulting-value rule: replay is idempotent -
		 * under preserve (ttl < 0, the Redis INCR contract) the WAL
		 * row carries the record's EFFECTIVE expiry */
		{
			char nvbuf[24];
			int nvl = snprintf(nvbuf, sizeof nvbuf, "%lld", *nv);

			pc_wal_upsert(col_name_of(ht), k->s, k->len, nvbuf,
				nvl, eff, pcache_last_ver);
		}
	}
	return PC_OP_OK;
}

static int op_set(pcache_htable_t *ht, str *k, str *v, long long ttl,
		unsigned int *park, int probed)
{
	int rc;

	if (writes_denied())
		return PC_OP_ERR_WRFAIL;

	if (pc_store_shard_enabled(ht) && pc_cluster_enabled()) {
		const char *cn = col_name_of(ht);
		int owner = pc_shard_owner(cn, strlen(cn), k->s,
			(size_t)k->len);

		if (owner) {
			unsigned int req;

			if (v->len > PC_MAX_FWD_VAL)
				return PC_OP_ERR_2BIG; /* the forward plane's
				                        * ceiling; storing it
				                        * locally would break
				                        * ownership - refuse
				                        * honestly */
			req = pc_fwd_begin(owner, 0, cn, strlen(cn), k->s,
				(size_t)k->len, v->s, (size_t)v->len,
				ttl > 0 ? (unsigned int)ttl : 0, 0);
			if (req) {
				*park = req;
				return PC_OP_PARKED;
			}
			return fwd_fail_code();
		}
		/* self-owned: fall through to the local store */
	}
	/* proxy mode: the holder serializes - forward unless we hold it
	 * (or placement keeps the new key local) */
	else if (pc_store_proxy_enabled(ht) && pc_cluster_enabled() &&
	        pcache_ht_probe(ht, k, NULL, NULL, NULL) != 0) {
		const char *cn = col_name_of(ht);
		int target = pc_loc_get(cn, strlen(cn), k->s, (size_t)k->len);

		if (v->len > PC_MAX_FWD_VAL) {
			/* The forward plane cannot carry it, so this write can
			 * never reach a remote holder - and until 2026-09-01 it
			 * silently fell through to a LOCAL store, forking any
			 * remote holder it had (the S54 open defect; the shard
			 * branch has refused this honestly all along).  Only a
			 * key the fleet recently confirmed ABSENT may be BORN
			 * here - the get-miss->set pattern stamps that
			 * confirmation - and the bulk plane can migrate the
			 * born value later. */
			if (target ||
			        !pc_neg_hit(cn, strlen(cn), k->s, (size_t)k->len))
				return PC_OP_ERR_2BIG;
			/* fleet-confirmed absent: fall through to the birth */
		} else {
			if (!target && !probed &&
			        !pc_neg_hit(cn, strlen(cn), k->s, (size_t)k->len)) {
				/* holder unknown and not recently-confirmed-
				 * absent: probe the fleet BEFORE placement can
				 * fork an existing key (the locator is a cache;
				 * an evicted entry must not turn a re-write
				 * into a second holder).  The get-miss->set
				 * pattern skips this: the get already stamped
				 * the negative cache. */
				unsigned int req = pc_probe_fwd_begin(0, cn,
					strlen(cn), k->s, (size_t)k->len, v->s,
					(size_t)v->len,
					ttl > 0 ? (unsigned int)ttl : 0, 0);

				if (req) {
					*park = req;
					return PC_OP_PARKED;
				}
				/* no peers: place as before */
			}
			if (!target)
				target = pc_place();   /* a new key: place it */
			if (target) {
				unsigned int req = pc_fwd_begin(target, 0, cn,
					strlen(cn), k->s, (size_t)k->len, v->s,
					(size_t)v->len,
					ttl > 0 ? (unsigned int)ttl : 0, 0);

				if (req) {
					*park = req;
					return PC_OP_PARKED;
				}
				return fwd_fail_code();
			}
			/* placement chose LOCAL: fall through to the store */
		}
	}
	rc = pcache_ht_store(ht, k, v, ttl_to_abs(ttl));
	if (rc == 0) {
		const char *cn = col_name_of(ht);

		pc_wal_upsert(cn, k->s, k->len, v->s, v->len, ttl_to_abs(ttl),
			pcache_last_ver);
		if (pc_cluster_enabled())
			pc_neg_clear(cn, strlen(cn), k->s, (size_t)k->len);
		return PC_OP_OK;
	}
	return rc == -2 ? PC_OP_ERR_FULL : PC_OP_ERR_2BIG;
}

/* ---- glob (Redis-style: * ? [abc] [a-c] [^a], backslash escapes) ------- */

static int pc_glob(const char *p, int pn, const char *s, int sn)
{
	int pi = 0, si = 0, star_p = -1, star_s = 0;

	while (si < sn) {
		if (pi < pn) {
			char pc = p[pi];

			if (pc == '*') {
				star_p = ++pi;         /* remember the backtrack point */
				star_s = si;
				continue;
			}
			if (pc == '?') {
				pi++; si++;
				continue;
			}
			if (pc == '[') {
				int j = pi + 1, neg = 0, matched = 0, first;

				if (j < pn && (p[j] == '^' || p[j] == '!')) {
					neg = 1; j++;
				}
				first = j;
				while (j < pn && (p[j] != ']' || j == first)) {
					char lo = p[j];

					if (lo == '\\' && j + 1 < pn)
						lo = p[++j];
					if (j + 2 < pn && p[j + 1] == '-' && p[j + 2] != ']') {
						if (s[si] >= lo && s[si] <= p[j + 2])
							matched = 1;
						j += 3;
					} else {
						if (s[si] == lo)
							matched = 1;
						j++;
					}
				}
				if (j >= pn)
					return 0;          /* unterminated class */
				if (matched != neg) {
					pi = j + 1; si++;
					continue;
				}
				/* class mismatch: fall through to the backtrack */
			} else if (pc == '\\' && pi + 1 < pn) {
				if (p[pi + 1] == s[si]) {
					pi += 2; si++;
					continue;
				}
			} else if (pc == s[si]) {
				pi++; si++;
				continue;
			}
		}
		if (star_p < 0)
			return 0;
		pi = star_p;                   /* the last '*' eats one more byte */
		si = ++star_s;
	}
	while (pi < pn && p[pi] == '*')
		pi++;
	return pi == pn;
}

/* ---- scan/keys walker callbacks ---------------------------------------- */

struct scan_ctx {
	struct pc_jw *w;
	const char *pat;
	int patlen;                        /* <0 = no pattern */
	int values;
	int emitted;
	int scanned;                       /* keys VISITED, pre-filter: the
	                                    * SCAN yield loop bounds on this,
	                                    * never on emitted - a rare MATCH
	                                    * must not walk the table in one
	                                    * call (the stall S40 forbids) */
	int limit;                         /* keys verb only */
	int stopped;                       /* keys: limit reached mid-walk */
	unsigned int now;
};

/* is a PRIM token the literal true? (bool params) */
static int pc_json_streq_prim(const char *line, const struct pc_jtok *t)
{
	return t->type == PC_J_PRIM && t->end - t->start == 4 &&
		!memcmp(line + t->start, "true", 4);
}

static long long rel_ttl(unsigned int expires, unsigned int now)
{
	if (!expires)
		return -1;
	return expires <= now ? 0 : (long long)expires - now;
}

/* scan: {"k":..,"k_enc"?,"v"?,"enc"?,"ttl":n} per live matching entry */
static int scan_cb(const str *key, const str *val, unsigned int exp,
		void *p)
{
	struct scan_ctx *sc = p;

	if (exp && exp <= sc->now)
		return 0;                      /* expired-as-absent */
	if (sc->pat && !pc_glob(sc->pat, sc->patlen, key->s, key->len))
		return 0;
	if (sc->w->overflow)
		return -1;                     /* stop early: reply already dead */
	if (sc->emitted++)
		pc_jw_lit(sc->w, ",");
	pc_jw_lit(sc->w, "{");
	pc_jw_value(sc->w, "k", "k_enc", key->s, (size_t)key->len);
	if (sc->values) {
		pc_jw_lit(sc->w, ",");
		pc_jw_value(sc->w, "v", "enc", val->s, (size_t)val->len);
	}
	pc_jw_lit(sc->w, ",\"ttl\":");
	pc_jw_i64(sc->w, rel_ttl(exp, sc->now));
	pc_jw_lit(sc->w, "}");
	return 0;
}

/* keys: flat array - clean-UTF-8 keys as strings, binary ones as
 * {"b64":".."} elements (an array slot has no room for a sibling) */
static int keys_cb(const str *key, const str *val, unsigned int exp,
		void *p)
{
	struct scan_ctx *sc = p;

	(void)val;
	if (exp && exp <= sc->now)
		return 0;
	if (sc->pat && !pc_glob(sc->pat, sc->patlen, key->s, key->len))
		return 0;
	if (sc->w->overflow)
		return -1;
	if (sc->emitted >= sc->limit) {
		sc->stopped = 1;
		return -1;
	}
	if (sc->emitted++)
		pc_jw_lit(sc->w, ",");
	if (pc_utf8_clean(key->s, (size_t)key->len)) {
		pc_jw_str(sc->w, key->s, (size_t)key->len);
	} else {
		pc_jw_lit(sc->w, "{");
		pc_jw_value(sc->w, "b64", "enc", key->s, (size_t)key->len);
		pc_jw_lit(sc->w, "}");
	}
	return 0;
}

/* the {found:false} / {found:true,value,enc?,ttl} shape shared by get */
/* ---- json path verbs (S26) --------------------------------------------
 * The document is read, edited as text and stored back, so concurrent
 * partial edits of the SAME key must not lose updates: a 64-way mutex
 * stripe (keyed on the key bytes) serializes the read-modify-write.
 * All writers live in this process - the daemon IS the serialization
 * point - so a local stripe is a complete answer, no CAS needed. */
static pthread_mutex_t jp_mu[64];
static pthread_once_t jp_mu_once = PTHREAD_ONCE_INIT;

static void jp_mu_init(void)
{
	int i;

	for (i = 0; i < 64; i++)
		pthread_mutex_init(&jp_mu[i], NULL);
}

static pthread_mutex_t *jp_lock(const char *key, int klen)
{
	unsigned int h = 2166136261u;
	int i;

	for (i = 0; i < klen; i++)
		h = (h ^ (unsigned char)key[i]) * 16777619u;
	pthread_once(&jp_mu_once, jp_mu_init);
	return &jp_mu[h & 63];
}

static const char *jp_strerror(int rc)
{
	switch (rc) {
	case PC_JP_E_DOC:     return "stored value is not JSON";
	case PC_JP_E_PATH:    return "bad path";
	case PC_JP_E_MISSING: return "intermediate path member missing";
	case PC_JP_E_TYPE:    return "wrong type along the path";
	case PC_JP_E_NOLEAF:  return "no value at path";
	case PC_JP_E_EXISTS:  return "value exists (nx)";
	case PC_JP_E_VAL:     return "new value is not valid JSON";
	case PC_JP_E_NUM:     return "path value is not an integer";
	case PC_JP_E_SIZE:    return "result too large";
	default:
		break;
	}
	return "json path error";
}

/* the striped JSON read-modify-write core: the verb layer and the
 * cluster forward plane both land HERE, from any thread - the stripe
 * is the serialization.  op = PC_JOP_*.  Returns 0 ok, 1 benign-absent
 * (jget miss / jdel absent), -1 error (*errmsg static).  GET fills
 * *frag_out (malloc'd) / *fraglen_out; INCR fills *newval; APPEND
 * fills *count_out. */
int pc_json_rmw(pcache_htable_t *ht, const char *key, int klen, int op,
		const char *path, int plen, const char *val, int vlen,
		long long by, int have_ttl, long long ttl, int nx, int xx,
		int mkpath, char **frag_out, int *fraglen_out,
		long long *newval, int *count_out, const char **errmsg)
{
	pthread_mutex_t *mu;
	str k, v, nvs;
	unsigned int exp = 0, newexp;
	char *doc = NULL, *nd = NULL;
	const char *fmsg = NULL;
	int rc, rc2 = 0, absent;

	k.s = (char *)key;
	k.len = klen;

	if (op == PC_JOP_GET) {
		rc = pcache_ht_fetch_ex(ht, &k, &v, &exp, NULL);
		if (rc == -2)
			return 1;
		if (rc != 0) {
			*errmsg = "get failed";
			return -1;
		}
		nd = malloc(VAL_MAX);
		if (!nd) {
			free(v.s);
			*errmsg = "out of memory";
			return -1;
		}
		rc2 = pc_jp_get(v.s, (size_t)v.len, path, (size_t)plen, nd,
			VAL_MAX);
		free(v.s);
		if (rc2 == PC_JP_E_NOLEAF) {
			free(nd);
			return 1;
		}
		if (rc2 < 0) {
			free(nd);
			*errmsg = jp_strerror(rc2);
			return -1;
		}
		*frag_out = nd;
		*fraglen_out = rc2;
		return 0;
	}

	mu = jp_lock(key, klen);
	pthread_mutex_lock(mu);
	rc = pcache_ht_fetch_ex(ht, &k, &v, &exp, NULL);
	absent = (rc == -2);
	if (rc != 0 && rc != -2) {
		fmsg = "get failed";
	} else if (absent && op == PC_JOP_SET && plen == 1) {
		doc = NULL;                    /* jset $ on a missing key CREATES */
		v.s = NULL;
		v.len = 0;
		exp = 0;
	} else if (absent && op == PC_JOP_DEL) {
		pthread_mutex_unlock(mu);
		return 1;
	} else if (absent) {
		fmsg = "no such key";
	}
	if (!fmsg) {
		doc = v.s;
		nd = malloc(VAL_MAX);
		if (!nd)
			fmsg = "out of memory";
	}
	if (!fmsg) {
		switch (op) {
		case PC_JOP_SET:
			rc2 = pc_jp_set(doc ? doc : "", doc ? (size_t)v.len
				: 0, path, (size_t)plen, val, (size_t)vlen,
				nx, xx, mkpath, nd, VAL_MAX);
			break;
		case PC_JOP_DEL:
			rc2 = pc_jp_del(doc, (size_t)v.len, path,
				(size_t)plen, nd, VAL_MAX);
			break;
		case PC_JOP_INCR:
			rc2 = pc_jp_incr(doc, (size_t)v.len, path,
				(size_t)plen, by, newval, nd, VAL_MAX);
			break;
		default:                       /* APPEND */
			rc2 = pc_jp_append(doc, (size_t)v.len, path,
				(size_t)plen, val, (size_t)vlen, count_out,
				nd, VAL_MAX);
		}
		if (op == PC_JOP_DEL && rc2 == PC_JP_E_NOLEAF) {
			pthread_mutex_unlock(mu);
			free(doc);
			free(nd);
			return 1;
		}
		if (rc2 < 0)
			fmsg = jp_strerror(rc2);
	}
	if (!fmsg) {
		const char *cn = col_name_of(ht);

		newexp = have_ttl ? ttl_to_abs(ttl) : exp;
		nvs.s = nd;
		nvs.len = rc2;
		if (pcache_ht_store(ht, &k, &nvs, newexp) == 0) {
			pc_wal_upsert(cn, k.s, k.len, nd, rc2, newexp,
				pcache_last_ver);
			if (pc_cluster_enabled())
				pc_neg_clear(cn, strlen(cn), k.s,
					(size_t)k.len);
		} else {
			fmsg = "store failed";
		}
	}
	pthread_mutex_unlock(mu);
	free(doc);
	free(nd);
	if (fmsg) {
		*errmsg = fmsg;
		return -1;
	}
	return 0;
}

static void write_hit(struct pc_jw *w, const char *v, size_t vn,
		unsigned int expires)
{
	long long ttl = expires ? (long long)expires - (long long)get_ticks()
		: -1;

	pc_jw_lit(w, "{\"found\":true,");
	pc_jw_value(w, "value", "enc", v, vn);
	pc_jw_lit(w, ",\"ttl\":");
	pc_jw_i64(w, ttl < -1 ? 0 : ttl);
	pc_jw_lit(w, "}");
}

/* A SHORT token, not pcache_mem_tier_str()'s sentence.  That string is
 * written for a human reading a startup log; this one is compared by
 * machines and scanned down a column of nodes, which is the whole point
 * of putting the tier on the fleet view - a node that differs should be
 * obvious at a glance, not at the end of a line of prose. */
static const char *mem_tier_token(int t)
{
	switch (t) {
	case PCACHE_MEM_HUGETLB:      return "hugetlb";
	case PCACHE_MEM_THP_ADVISE:   return "thp-advise";
	case PCACHE_MEM_THP_COLLAPSE: return "thp-collapse";
	case PCACHE_MEM_4K:           return "4k";
	case PCACHE_MEM_NO_ARENA:     return "no-arena";
	default:                      return "unknown";
	}
}

/* C7: a RECOVERING node holds data it cannot vouch for.  Its replay can
 * still carry a key that was DELETED while it was down, so serving from
 * it does not answer late - it answers WRONGLY, and a wrong answer is
 * the one failure a cache cannot let a client detect.
 *
 * STARTING IS NOT GATED, and that is the whole distinction.  A node that
 * has not joined yet holds NOTHING, so a miss from it is the truth; it
 * answers honestly, just emptily.  Gating it would refuse every data
 * verb for JOIN_WAIT_MS at every cold start of every clustered node -
 * measured here when the first cut of this gate refused its own test
 * fixture's 200-key fill - and would buy nothing, because there is no
 * wrong answer to prevent.  Truthfully empty is not the same as
 * untrustworthy.  Data verbs are therefore refused until READY.
 *
 * Observability is NOT gated.  A node stuck non-READY is exactly when
 * an operator needs to ask it what is wrong, so ping / stats / members
 * / probe answer throughout, as do the operator-initiated admin verbs.
 *
 * DRAINING SERVES.  It is being emptied, not doubted, and it still
 * holds everything it has not handed over.  That is deliberately NOT
 * the same rule as pc_clsel_eligible(), which gates SELECTION and
 * excludes DRAINING: do not DIAL a node that is going away, but do
 * answer on one you are already talking to.  Two different questions -
 * if you are here to unify them, they were separated on purpose.
 * (Today DRAINING is only entered on the way out, in the shutdown path,
 * so this arm is a forward-looking rule rather than live behaviour -
 * nothing exercises it yet, and waldroptest-style proof is not
 * available until a drain verb exists.)
 *
 * The client half is libperfd reading the `state` the members verb has
 * published since B1, so a cluster-aware client skips a non-READY node
 * rather than being refused by it.  This gate is what makes that
 * advisory rather than load-bearing. */
static int serving_denied(void)
{
	return pc_node_state() == PC_NST_RECOVERING;
}

/*
 * FAILED refuses WRITES only, which is the whole difference between it
 * and the gate above.  A node whose WAL is discarding acknowledged
 * writes still holds correct data and must go on answering for it -
 * what it must stop doing is accepting more it cannot honour.  Refusing
 * reads too would turn a durability fault into an availability one.
 *
 * The three dialects all funnel their mutations through op_set/op_del/
 * op_addsub, so the check lives there: one place per mutation rather
 * than one per dialect, and a new dialect inherits it.
 */
static int writes_denied(void)
{
	return pc_node_state() == PC_NST_FAILED;
}

#define PC_WRFAIL_MSG "node is FAILED (its WAL is losing acknowledged "\
	"writes) - reads still served here, send writes to another member"

/* one message, so all three dialects say the same thing */
#define PC_NOTREADY_MSG "node is not READY (recovering) - retry, or use "\
	"another member"

/* S37: the fleet as JSON, rendered from THIS node's vantage point.
 * Shared by the `members` verb and the status page's /members route,
 * so the two cannot drift - a page disagreeing with the verb would
 * be worse than no page. */
void pc_members_json(struct pc_jw *out)
{
	struct pc_member mem[PC_CL_MAXMEMBERS];
	int n = pc_cluster_members(mem, PC_CL_MAXMEMBERS), i;

	pc_jw_lit(out, "{\"members\":[");
	for (i = 0; i < n; i++) {
		char ab[24];
		const char *as = pc_inet_ntop4(mem[i].addr, ab,
			sizeof ab);

		if (i)
			pc_jw_lit(out, ",");
		pc_jw_lit(out, "{\"addr\":");
		pc_jw_str(out, as, strlen(as));
		pc_jw_lit(out, ",\"port\":");
		pc_jw_i64(out, mem[i].client_port);
		pc_jw_lit(out, ",\"node\":");
		pc_jw_i64(out, mem[i].node);
		pc_jw_lit(out, ",\"self\":");
		pc_jw_lit(out, mem[i].is_self ? "true" : "false");
		pc_jw_lit(out, ",\"master\":");
		pc_jw_lit(out, mem[i].is_master ? "true" : "false");
		/* who takes over.  The map has named a standby since
		 * C1 and nothing showed it, so "which node is the
		 * backup" was a question only the master could
		 * answer. */
		pc_jw_lit(out, ",\"backup\":");
		pc_jw_lit(out, mem[i].is_backup ? "true" : "false");
		pc_jw_lit(out, ",\"role\":\"");
		pc_jw_lit(out, mem[i].is_master ? "master" :
			mem[i].is_backup ? "backup" : "member");
		pc_jw_lit(out, "\"");
		/* B1: whether this member's data can be trusted yet -
		 * a separate question from whether it is the master */
		pc_jw_lit(out, ",\"state\":\"");
		pc_jw_lit(out, pc_node_state_name(mem[i].state));
		pc_jw_lit(out, "\",\"mem_tier\":\"");
		pc_jw_lit(out, mem_tier_token(mem[i].mem_tier));
		pc_jw_lit(out, "\",\"free_mb\":");
		pc_jw_i64(out, mem[i].free_mb);
		pc_jw_lit(out, ",\"total_mb\":");
		pc_jw_i64(out, mem[i].total_mb);
		/* identity is stable across that node's restarts,
		 * incarnation is not - a client watching both can
		 * tell a restart from a missed heartbeat */
		if (mem[i].has_ident) {
			char hx[33];
			int b;

			for (b = 0; b < 16; b++)
				snprintf(hx + (size_t)b * 2, 3, "%02x",
					mem[i].ident[b]);
			pc_jw_lit(out, ",\"identity\":");
			pc_jw_str(out, hx, 32);
			pc_jw_lit(out, ",\"incarnation\":");
			pc_jw_i64(out, (long long)mem[i].incarn);
		}
		pc_jw_lit(out, "}");
	}
	pc_jw_lit(out, "],\"routing\":{\"algo\":\"" PC_ROUTE_ALGO
		"\",\"mode\":");
	{
		int md = pc_cluster_mode();

		pc_jw_lit(out, md == PC_MODE_PROXY ? "\"proxy\""
			: md == PC_MODE_SHARD ? "\"shard\""
			: "\"store\"");
	}
	pc_jw_lit(out, ",\"authoritative\":");
	pc_jw_lit(out, pc_cluster_authoritative() ? "true" : "false");
	/* the peer-plane port every member shares: owner selection
	 * mixes (advertise ip, CLUSTER port), so a client that wants
	 * to compute an owner needs it - the client port would give
	 * a different hash and route everything wrong (harmlessly,
	 * but pointlessly) */
	pc_jw_lit(out, ",\"cport\":");
	pc_jw_i64(out, pc_cluster_port());
	pc_jw_lit(out, "}}");
}

int pc_verb_text(const char *line, const struct pc_jtok *toks, int ntok,
		int method_tok, int params_tok, struct pc_jw *out,
		const char **errmsg, unsigned int *park_req)
{
	const struct pc_jtok *m = &toks[method_tok];
	char key[KEY_MAX], *val = NULL;
	pcache_htable_t *ht;
	str k, v;
	long long by, ttl;
	int klen, vlen, rc, ret = 0;
	unsigned int vln, exp;
	int is_ctr;

#define NEEDKEY() do { \
	klen = get_str(line, toks, ntok, params_tok, "key", NULL, key, KEY_MAX); \
	if (klen < 0) ERR(E_PARAMS, "missing or bad key"); \
	k.s = key; k.len = klen; } while (0)

	/* ---- ping {echo?,enc?} : also the transport/codec diagnostic --- */
	if (pc_json_streq(line, m, "ping")) {
		int te = params_tok < 0 ? -1
			: pc_json_get(line, toks, ntok, params_tok, "echo");

		pc_jw_lit(out, "{\"pong\":true");
		if (te >= 0 && toks[te].type == PC_J_STR) {
			size_t span = (size_t)(toks[te].end - toks[te].start);
			char *e = malloc(span + 1);
			int el;

			if (!e)
				ERR(-32603, "out of memory");
			el = get_str(line, toks, ntok, params_tok, "echo", "enc",
				e, span + 1);
			if (el < 0) {
				free(e);
				ERR(E_PARAMS, "bad echo");
			}
			pc_jw_lit(out, ",");
			pc_jw_value(out, "echo", "enc", e, (size_t)el);
			free(e);
		}
		pc_jw_lit(out, "}");
		return 0;
	}

	/* ---- save : request an RDB snapshot ---------------------------- */
	if (pc_json_streq(line, m, "save")) {
		struct pc_rdb_stats rs;

		pc_rdb_get_stats(&rs);
		if (!rs.enabled)
			ERR(-32000, "persistence is not configured");
		pc_jw_lit(out, pc_rdb_request_save() == 0 ?
			"{\"started\":true}" : "{\"started\":false,"
			"\"reason\":\"already running\"}");
		return 0;
	}

	/* ---- sync {timeout_ms?} : WAL barrier - block until everything
	 * appended so far is on the platter ------------------------------ */
	if (pc_json_streq(line, m, "sync")) {
		long long tmo = 0;
		int rc2;

		if (params_tok >= 0)
			get_int(line, toks, ntok, params_tok, "timeout_ms",
				&tmo);
		rc2 = pc_wal_sync((int)tmo);
		if (rc2 == 1)
			ERR(-32000, "persistence is not configured");
		if (rc2 < 0)
			ERR(-32000, "sync timed out");
		{
			struct pc_wal_stats ws;

			pc_wal_get_stats(&ws);
			/* S58: the barrier covers every record that REACHED a
			 * ring, and a ring-full drop discards one that already
			 * holds a sequence number - the gap does not hold
			 * synced_seq back, so this reply used to say
			 * {"synced":true,"seq":N} while N included records the
			 * WAL never carried (measured: 20000 writes, seq 20000,
			 * 1223 dropped, 18777 replayed after a kill).  The
			 * count rides the receipt so a caller can tell a full
			 * barrier from a partial one; never-block-a-worker
			 * means drops stay possible by design. */
			pc_jw_lit(out, "{\"synced\":true,\"seq\":");
			pc_jw_i64(out, (long long)ws.synced_seq);
			pc_jw_lit(out, ",\"dropped\":");
			pc_jw_i64(out, (long long)ws.dropped);
			pc_jw_lit(out, "}");
		}
		return 0;
	}

	/* ---- load : import the current snapshot ADDITIVELY (existing
	 * keys win; expired records dropped) ----------------------------- */
	if (pc_json_streq(line, m, "load")) {
		long ld = 0, se = 0, sx = 0;

		if (!pc_rdb_dir()[0])
			ERR(-32000, "persistence is not configured");
		if (pc_rdb_import(pc_rdb_dir(), &ld, &se, &sx) != 0)
			ERR(-32000, "no readable snapshot");
		pc_jw_lit(out, "{\"loaded\":");
		pc_jw_i64(out, ld);
		pc_jw_lit(out, ",\"skipped_existing\":");
		pc_jw_i64(out, se);
		pc_jw_lit(out, ",\"skipped_expired\":");
		pc_jw_i64(out, sx);
		pc_jw_lit(out, "}");
		return 0;
	}

	/* ---- probe {secs?} : re-measure the WAL storage on demand ------
	 * Blocks THIS request (and the calling worker) for the probe's
	 * duration; the I/O shares the device with the live pump, so
	 * fsync latency is perturbed while it runs - inherent to
	 * re-measuring.  stats flips to cached:false afterwards. */
	if (pc_json_streq(line, m, "probe")) {
		struct pc_wal_policy pol;
		const struct pc_wal_probe *pr;
		long long secs = 0;

		if (params_tok >= 0)
			get_int(line, toks, ntok, params_tok, "secs", &secs);
		if (secs < 0)
			secs = 0;
		if (secs > 30)
			ERR(E_PARAMS, "secs is capped at 30");
		if (pc_wal_reprobe((int)secs, &pol) != 0)
			ERR(-32000, "wal is not configured or the probe failed");
		pr = pc_wal_probe_result();
		pc_jw_lit(out, "{\"cached\":false,\"sync_bs\":");
		pc_jw_i64(out, PC_WPROBE_SYNC_BS);
		pc_jw_lit(out, ",\"qd\":");
		pc_jw_i64(out, PC_WPROBE_QD);
		pc_jw_lit(out, ",\"seq_bs\":");
		pc_jw_i64(out, PC_WPROBE_SEQ_BS);
		pc_jw_lit(out, ",\"seq_mb_s\":");
		pc_jw_i64(out, (long long)(pr->seq_mb_s + 0.5));
		pc_jw_lit(out, ",\"fsync_p50_us\":");
		pc_jw_i64(out, pr->fsync_p50_us);
		pc_jw_lit(out, ",\"fsync_p99_us\":");
		pc_jw_i64(out, pr->fsync_p99_us);
		pc_jw_lit(out, ",\"sync_iops\":");
		pc_jw_i64(out, pr->sync_iops);
		pc_jw_lit(out, ",\"probed_secs\":");
		pc_jw_i64(out, pr->probed_secs);
		pc_jw_lit(out, ",\"recommend\":{\"fsync\":\"");
		pc_jw_lit(out, pol.fsync_recommend);
		pc_jw_lit(out, "\",\"max_durable_wps\":");
		pc_jw_i64(out, pol.max_durable_wps);
		/* say what that number is: a burst ceiling, with the volume
		 * it was measured over, so a consumer cannot read it as a
		 * sustained rate the probe never established */
		pc_jw_lit(out, ",\"max_durable_wps_is_upper_bound\":true");
		{
			/* and what the fsyncs are ACTUALLY costing - the only
			 * figure taken under real load rather than at start */
			struct pc_wal_fsync_obs ob;

			pc_wal_fsync_observed(&ob);
			pc_jw_lit(out, ",\"observed\":{\"fsync_n\":");
			pc_jw_i64(out, (long long)ob.n);
			pc_jw_lit(out, ",\"fsync_avg_us\":");
			pc_jw_i64(out, (long long)ob.avg_us);
			pc_jw_lit(out, ",\"fsync_max_us\":");
			pc_jw_i64(out, (long long)ob.max_us);
			pc_jw_lit(out, ",\"probe_underestimated\":");
			pc_jw_lit(out, ob.probe_underestimated ? "true" : "false");
			pc_jw_lit(out, "}");
		}
		pc_jw_lit(out, ",\"probe_sync_kb\":");
		pc_jw_i64(out, pr ? pr->sync_bytes >> 10 : 0);
		pc_jw_lit(out, ",\"segment_mb\":");
		pc_jw_i64(out, pol.segment_mb);
		{
			/* total-size recommendation from OBSERVED traffic:
			 * enough WAL to cover the worst-case gap between
			 * snapshots at the lifetime-average write rate,
			 * x3 safety - every input reported next to the
			 * number (a size without its basis is as naked as
			 * iops without bs/qd) */
			struct pc_wal_stats ws2;
			long long up = (long long)get_ticks();
			long long ckpt = pc_rdb_max_interval_s();
			long long avg, wps, need_mb, segs;

			pc_wal_get_stats(&ws2);
			if (ws2.appended > 0 && up > 0 && ckpt > 0) {
				avg = (long long)(ws2.bytes / ws2.appended);
				wps = (long long)ws2.appended / up;
				if (wps < 1)
					wps = 1;
				need_mb = wps * (avg + 32) * ckpt * 3
					/ (1024LL * 1024);
				segs = (need_mb + pol.segment_mb - 1)
					/ pol.segment_mb;
				if (segs < 4)
					segs = 4;
				pc_jw_lit(out, ",\"wal_total_mb\":");
				pc_jw_i64(out, segs * pol.segment_mb);
				pc_jw_lit(out, ",\"segments\":");
				pc_jw_i64(out, segs);
				pc_jw_lit(out,
					",\"basis\":{\"observed_wps\":");
				pc_jw_i64(out, wps);
				pc_jw_lit(out, ",\"avg_record_b\":");
				pc_jw_i64(out, avg);
				pc_jw_lit(out, ",\"checkpoint_s\":");
				pc_jw_i64(out, ckpt);
				pc_jw_lit(out, ",\"safety\":3,");
				pc_jw_lit(out, "\"uptime_s\":");
				pc_jw_i64(out, up);
				pc_jw_lit(out, "}");
			} else {
				pc_jw_lit(out, ",\"wal_total_mb\":null,"
					"\"basis\":\"no observed traffic"
					" or no snapshot rules yet\"");
			}
		}
		pc_jw_lit(out, "}}");
		return 0;
	}

	/* ---- stats {col?} ---------------------------------------------- */
	/* ---- members (S34): the fleet as a CLIENT needs it -------------
	 * Addresses + client ports so a library can pre-warm connections
	 * to the other nodes, load so it can weight them, and the ROUTING
	 * CONTRACT (mode + algorithm id) so a client may compute where a
	 * key belongs.  The algorithm carries a VERSION: a client that
	 * does not recognise it must fall back to plain spreading, and a
	 * mismatch then costs a forward, never correctness - the daemon
	 * re-checks ownership regardless of what the client believed. */
	if (pc_json_streq(line, m, "members")) {
		pc_members_json(out);
		return 0;
	}

	if (pc_json_streq(line, m, "stats")) {
		int tc = params_tok < 0 ? -1
			: pc_json_get(line, toks, ntok, params_tok, "col");
		int i;

		/* B1: the node's lifecycle state is a property of the NODE,
		 * not of the cluster - a node with no cluster still has one,
		 * and a readiness gate would have to answer for it too.  It
		 * sat inside the cluster block first, where an unclustered
		 * node could not report it at all. */
		pc_jw_lit(out, "{\"version\":\"" PC_VERSION "\",\"rev\":\""
			PC_BUILD_REV "\",\"state\":\"");
		pc_jw_lit(out, pc_node_state_name(pc_node_state()));
		pc_jw_lit(out, "\",\"memory\":");
		{
			unsigned long mt = 0, mu = 0, mf = 0;
			int mact = 0;

			pcache_arena_hugepage_capacity(&mact, &mt, &mu, &mf);
			pc_jw_lit(out, "{\"arena_total\":");
			pc_jw_i64(out, (long long)mt);
			pc_jw_lit(out, ",\"arena_used\":");
			pc_jw_i64(out, (long long)mu);
			pc_jw_lit(out, ",\"arena_free\":");
			pc_jw_i64(out, (long long)mf);
			pc_jw_lit(out, ",\"arena_live\":");
			pc_jw_i64(out, (long long)pcache_arena_live_bytes());
			/* arena_total/used/free above are HUGE-PAGE figures -
			 * they pin at the reservation while the process keeps
			 * growing, which is how a 64 MB arena reached 348 MB
			 * of RSS unnoticed.  These two are the whole node:
			 * what it holds from the host, and the ceiling it is
			 * allowed to hold. */
			pc_jw_lit(out, ",\"arena_held\":");
			pc_jw_i64(out, (long long)pcache_arena_held_bytes());
			pc_jw_lit(out, ",\"arena_max\":");
			pc_jw_i64(out, (long long)pcache_arena_max_bytes);
			/* S47: the pressure surface - what an operator alerts
			 * on BEFORE the arena starts refusing writes.  tier =
			 * the page backing actually in use; headroom_pct =
			 * how much of the ceiling is still free (0 at the
			 * cliff); nomem = writes already refused arena-full;
			 * reclaim = the give-back machinery's own counters. */
			{
				struct pcache_arena_pressure pr;
				unsigned long held = pcache_arena_held_bytes();
				unsigned long mx = pcache_arena_max_bytes;
				int hpct = mx ? (mx > held ?
					(int)(100ULL * (mx - held) / mx) : 0)
					: 100;

				pcache_arena_pressure(&pr);
				pc_jw_lit(out, ",\"tier\":");
				pc_jw_str(out, pr.tier, strlen(pr.tier));
				pc_jw_lit(out, ",\"headroom_pct\":");
				pc_jw_i64(out, hpct);
				pc_jw_lit(out, ",\"nomem\":");
				pc_jw_i64(out, (long long)pr.refused);
				pc_jw_lit(out, ",\"reclaim\":{\"retired\":");
				pc_jw_i64(out, (long long)pr.retired);
				pc_jw_lit(out, ",\"pages_freed\":");
				pc_jw_i64(out, (long long)pr.pages_freed);
				pc_jw_lit(out, ",\"released_bytes\":");
				pc_jw_i64(out, (long long)pr.released_bytes);
				pc_jw_lit(out, ",\"cold_bytes\":");
				pc_jw_i64(out, (long long)pr.cold_bytes);
				pc_jw_lit(out, ",\"flushes\":");
				pc_jw_i64(out, (long long)pr.flushes);
				pc_jw_lit(out, ",\"giveback_off\":");
				pc_jw_lit(out, pr.giveback_off ? "true" : "false");
				pc_jw_lit(out, "}");
			}
			pc_jw_lit(out, "}");
		}
		/* RESP listeners (S33) live OUTSIDE the cluster block: a
		 * RESP listener works on a standalone daemon, and burying
		 * its counters in "cluster" made them null exactly where a
		 * single-node operator would look for them. */
		pc_jw_lit(out, ",\"resp\":{\"conns\":");
		pc_jw_i64(out, (long long)PC_RESP_READ(pc_resp_conns));
		pc_jw_lit(out, ",\"rejected\":");
		pc_jw_i64(out, (long long)PC_RESP_READ(pc_resp_rejected));
		pc_jw_lit(out, ",\"authfail\":");
		pc_jw_i64(out, (long long)PC_RESP_READ(pc_resp_authfail));
		pc_jw_lit(out, "}");
		pc_jw_lit(out, ",\"collections\":[");
		for (i = 0; i < pc_store_count(); i++) {
			pcache_ht_totals_t tot;

			if (tc >= 0 && !pc_json_streq(line, &toks[tc], pc_store_name(i)))
				continue;
			pcache_ht_totals(pc_store_ht(i), &tot);
			if (out->len && out->buf[out->len - 1] == '}')
				pc_jw_lit(out, ",");
			pc_jw_lit(out, "{\"name\":");
			pc_jw_str(out, pc_store_name(i), strlen(pc_store_name(i)));
			/* the mode is the first thing anyone needs to know
			 * about a collection - and until now the only way to
			 * find it was to read the node's config file */
			pc_jw_lit(out, ",\"mode\":\"");
			pc_jw_lit(out, pc_store_shard_enabled(pc_store_ht(i))
				? "shard" : pc_store_proxy_enabled(pc_store_ht(i))
				? "proxy" : "store");
			pc_jw_lit(out, "\"");
			pc_jw_lit(out, ",\"entries\":");
			pc_jw_i64(out, (long long)tot.entries);
			pc_jw_lit(out, ",\"buckets\":");
			pc_jw_i64(out, pcache_ht_nbuckets(pc_store_ht(i)));
			pc_jw_lit(out, ",\"hits\":");
			pc_jw_i64(out, (long long)tot.hits);
			pc_jw_lit(out, ",\"misses\":");
			pc_jw_i64(out, (long long)tot.misses);
			pc_jw_lit(out, ",\"stores\":");
			pc_jw_i64(out, (long long)tot.stores);
			pc_jw_lit(out, ",\"removes\":");
			pc_jw_i64(out, (long long)tot.removes);
			pc_jw_lit(out, ",\"expired\":");
			pc_jw_i64(out, (long long)tot.expired);
			pc_jw_lit(out, "}");
		}
		pc_jw_lit(out, "],\"cluster\":");
		{
			struct pc_cl_stats cs;

			pc_cluster_get_stats(&cs);
			if (!cs.enabled) {
				pc_jw_lit(out, "null");
			} else {
				pc_jw_lit(out, "{\"node\":");
				pc_jw_i64(out, cs.node_id);
				/* what this node IS, as opposed to what it is
				 * currently called: identity survives its
				 * restarts, incarnation does not, and durable
				 * says whether the identity was persisted at
				 * all (it cannot be without a [wal] dir) */
				pc_jw_lit(out, ",\"identity\":");
				pc_jw_str(out, pc_cluster_identity(),
					strlen(pc_cluster_identity()));
				pc_jw_lit(out, ",\"incarnation\":");
				pc_jw_i64(out, (long long)
					pc_cluster_incarnation());
				pc_jw_lit(out, ",\"identity_durable\":");
				pc_jw_lit(out, pc_cluster_identity_durable()
					? "true" : "false");
				/* the Lamport clock: comparable across the
				 * fleet without any clock being in sync */
				pc_jw_lit(out, ",\"lamport\":");
				pc_jw_i64(out, (long long)pc_lamport_now());
				pc_jw_lit(out, ",\"lamport_rejected\":");
				pc_jw_i64(out, (long long)pc_lamport_rejected);
				/* two axes: the role is this node's authority
				 * in the membership; the state (top level,
				 * since an unclustered node has one too) is
				 * whether its data can be trusted yet.  A
				 * master can still be reconciling. */
				/* the term orders maps across mastership
				 * changes; rejected counts a peer advertising
				 * one we refused as implausible */
				/* the map this node holds.  Nothing places
				 * keys with it yet; these say whether the
				 * plumbing works. */
				pc_jw_lit(out, ",\"map\":{\"valid\":");
				pc_jw_lit(out, cs.map_valid ? "true" : "false");
				pc_jw_lit(out, ",\"term\":");
				pc_jw_i64(out, (long long)cs.map_term);
				pc_jw_lit(out, ",\"seq\":");
				pc_jw_i64(out, (long long)cs.map_seq);
				pc_jw_lit(out, ",\"nodes\":");
				pc_jw_i64(out, (long long)cs.map_nodes);
				pc_jw_lit(out, ",\"master\":");
				pc_jw_i64(out, (long long)cs.map_master);
				pc_jw_lit(out, ",\"published\":");
				pc_jw_i64(out, (long long)cs.map_pub);
				pc_jw_lit(out, ",\"received\":");
				pc_jw_i64(out, (long long)cs.map_rx);
				pc_jw_lit(out, ",\"stale\":");
				pc_jw_i64(out, (long long)cs.map_stale);
				pc_jw_lit(out, ",\"refused\":");
				pc_jw_i64(out, (long long)cs.map_bad);
				/* usable = the map's placeable set matches
				 * what this node sees as live.  place_hrw
				 * still climbing on a settled fleet means it
				 * never caught up. */
				pc_jw_lit(out, ",\"usable\":");
				pc_jw_lit(out, cs.map_usable ? "true" : "false");
				pc_jw_lit(out, ",\"place_map\":");
				pc_jw_i64(out, (long long)cs.place_map);
				pc_jw_lit(out, ",\"place_hrw\":");
				pc_jw_i64(out, (long long)cs.place_hrw);
				/* B4: keys dropped because the fleet no longer
				 * had them - deleted while this node was down
				 * and brought back by replay.  Not a map
				 * property, so not inside that object. */
				/* the designated standby, and how many
				 * identities this node remembers.  No standby
				 * means the cluster is one failure from
				 * losing its control plane. */
				pc_jw_lit(out, ",\"backup\":");
				pc_jw_i64(out, (long long)cs.backup_id);
				pc_jw_lit(out, "},\"sync\":{\"held\":");
				pc_jw_lit(out, cs.held_valid ? "true" : "false");
				pc_jw_lit(out, ",\"held_term\":");
				pc_jw_i64(out, (long long)cs.held_term);
				pc_jw_lit(out, ",\"held_seq\":");
				pc_jw_i64(out, (long long)cs.held_seq);
				pc_jw_lit(out, ",\"held_identities\":");
				pc_jw_i64(out, (long long)cs.held_hist_n);
				pc_jw_lit(out, ",\"sent\":");
				pc_jw_i64(out, (long long)cs.sync_sent);
				pc_jw_lit(out, ",\"received\":");
				pc_jw_i64(out, (long long)cs.sync_rx);
				pc_jw_lit(out, ",\"acked\":");
				pc_jw_i64(out, (long long)cs.sync_ack);
				pc_jw_lit(out, ",\"refused\":");
				pc_jw_i64(out, (long long)cs.sync_bad);
				pc_jw_lit(out, "},\"identities_seen\":");
				pc_jw_i64(out, (long long)cs.hist_n);
				pc_jw_lit(out, ",\"reconciled\":");
				pc_jw_i64(out, (long long)cs.reconciled);
				pc_jw_lit(out, ",\"reconcile_probed\":");
				pc_jw_i64(out, (long long)cs.reconcile_probed);
				pc_jw_lit(out, ",\"term\":");
				pc_jw_i64(out, (long long)pc_term_current());
				pc_jw_lit(out, ",\"term_rejected\":");
				pc_jw_i64(out, (long long)pc_term_rejected);
				pc_jw_lit(out, ",\"role\":\"");
				pc_jw_lit(out, cs.role == 2 ? "master" :
					cs.role == 1 ? "member" : "joining");
				pc_jw_lit(out, "\",\"master\":");
				pc_jw_i64(out, cs.master_id);
				pc_jw_lit(out, ",\"peers_up\":");
				pc_jw_i64(out, cs.peers_up);
				pc_jw_lit(out, ",\"pull_sent\":");
				pc_jw_i64(out, (long long)cs.pull_sent);
				pc_jw_lit(out, ",\"pull_hits\":");
				pc_jw_i64(out, (long long)cs.pull_hits);
				pc_jw_lit(out, ",\"pull_misses\":");
				pc_jw_i64(out, (long long)cs.pull_misses);
				pc_jw_lit(out, ",\"pull_timeouts\":");
				pc_jw_i64(out, (long long)cs.pull_timeouts);
				pc_jw_lit(out, ",\"pull_served\":");
				pc_jw_i64(out, (long long)cs.pull_served);
				pc_jw_lit(out, ",\"tomb_sent\":");
				pc_jw_i64(out, (long long)cs.tomb_sent);
				pc_jw_lit(out, ",\"tomb_applied\":");
				pc_jw_i64(out, (long long)cs.tomb_applied);
				pc_jw_lit(out, ",\"neg_hits\":");
				pc_jw_i64(out, (long long)cs.neg_hits);
				pc_jw_lit(out, ",\"bad_auth\":");
				pc_jw_i64(out, (long long)cs.bad_auth);
				{
					struct pc_proxy_stats px;

					pc_proxy_get_stats(&px);
					pc_jw_lit(out, ",\"placed_local\":");
					pc_jw_i64(out, (long long)px.placed_local);
					pc_jw_lit(out, ",\"placed_remote\":");
					pc_jw_i64(out, (long long)px.placed_remote);
					pc_jw_lit(out, ",\"fwd_sent\":");
					pc_jw_i64(out, (long long)px.fwd_sent);
					pc_jw_lit(out, ",\"fwd_served\":");
					pc_jw_i64(out, (long long)px.fwd_served);
					pc_jw_lit(out, ",\"migrated_out\":");
					pc_jw_i64(out, (long long)px.migrated_out);
					pc_jw_lit(out, ",\"migrated_in\":");
					pc_jw_i64(out, (long long)px.migrated_in);
					pc_jw_lit(out, ",\"migrate_lost\":");
					pc_jw_i64(out, (long long)px.migrate_lost);
					pc_jw_lit(out, ",\"migrate_dgrams\":");
					pc_jw_i64(out, (long long)px.migrate_dgrams);
					pc_jw_lit(out, ",\"repl_out\":");
				pc_jw_i64(out, (long long)px.repl_out);
					/* A2: copies refused as not-newer.  A
					 * number that only climbs means a
					 * sender is looping on records nobody
					 * will take. */
					pc_jw_lit(out, ",\"recv_older\":");
					pc_jw_i64(out, (long long)px.recv_older);
					/* the EFFECTIVE receive buffer: a kernel
					 * clamp here means dropped forwards and
					 * refused writes, so it belongs where it
					 * can be read at runtime, not only in the
					 * startup log */
					/* the parked-request table: peak
					 * against capacity, and what it
					 * refused when it was too small.  A
					 * peak at capacity with a nonzero
					 * refusal count is the signal to
					 * route clients or raise it. */
					pc_jw_lit(out, ",\"pend_peak\":");
					pc_jw_i64(out, (long long)cs.pend_peak);
					pc_jw_lit(out, ",\"pend_max\":");
					pc_jw_i64(out, (long long)cs.pend_max);
					pc_jw_lit(out, ",\"pend_used\":");
					pc_jw_i64(out, (long long)cs.pend_used);
					pc_jw_lit(out, ",\"pend_exhausted\":");
					pc_jw_i64(out,
						(long long)cs.pend_exhausted);
					/* the other two refusal causes, split:
					 * "forward failed" covering both is
					 * what sent an operator to look at a
					 * healthy network */
					pc_jw_lit(out, ",\"fwd_no_route\":");
					pc_jw_i64(out,
						(long long)cs.fwd_no_route);
					pc_jw_lit(out, ",\"fwd_send_fail\":");
					pc_jw_i64(out,
						(long long)cs.fwd_send_fail);
					pc_jw_lit(out, ",\"fwd_send_errno\":");
					pc_jw_i64(out,
						(long long)cs.fwd_send_errno);
					pc_jw_lit(out, ",\"rcvbuf\":");
					pc_jw_i64(out, (long long)pc_cluster_rcvbuf());
				pc_jw_lit(out, ",\"bulk_out\":");
					pc_jw_i64(out, (long long)px.bulk_out);
					pc_jw_lit(out, ",\"bulk_in\":");
					pc_jw_i64(out, (long long)px.bulk_in);
					pc_jw_lit(out, ",\"demotes_sent\":");
					pc_jw_i64(out, (long long)px.demotes_sent);
					pc_jw_lit(out, ",\"demotes_applied\":");
					pc_jw_i64(out, (long long)px.demotes_applied);
					pc_jw_lit(out, ",\"loc_hits\":");
					pc_jw_i64(out, (long long)px.loc_hits);
				}
				pc_jw_lit(out, "}");
			}
		}
		pc_jw_lit(out, ",\"rdb\":");
		{
			struct pc_rdb_stats rs;

			pc_rdb_get_stats(&rs);
			if (!rs.enabled) {
				pc_jw_lit(out, "null");
			} else {
				pc_jw_lit(out, "{\"saves\":");
				pc_jw_i64(out, (long long)rs.saves);
				pc_jw_lit(out, ",\"running\":");
				pc_jw_lit(out, rs.running ? "true" : "false");
				pc_jw_lit(out, ",\"last_bytes\":");
				pc_jw_i64(out, (long long)rs.last_bytes);
				pc_jw_lit(out, ",\"last_dur_ms\":");
				pc_jw_i64(out, rs.last_dur_ms);
				pc_jw_lit(out, ",\"last_marker\":");
				pc_jw_i64(out, (long long)rs.last_marker);
				pc_jw_lit(out, ",\"last_unix\":");
				pc_jw_i64(out, rs.last_unix);
				pc_jw_lit(out, "}");
			}
		}
		pc_jw_lit(out, ",\"wal\":");
		{
			struct pc_wal_stats ws;

			pc_wal_get_stats(&ws);
			if (!ws.enabled) {
				pc_jw_lit(out, "null");
			} else {
				/* the storage identity resolved at startup: the
				 * class/chain belong NEXT TO the wal counters,
				 * not only in a boot log line */
				const struct pc_st_id *sid = pc_wal_identity();

				pc_jw_lit(out, "{\"fsync\":\"");
				pc_jw_lit(out, ws.fsync_mode);
				pc_jw_lit(out, "\"");
				if (sid) {
					pc_jw_lit(out, ",\"storage_class\":");
					pc_jw_str(out, pc_st_class_str(sid->cls),
						strlen(pc_st_class_str(sid->cls)));
					pc_jw_lit(out, ",\"fstype\":");
					pc_jw_str(out, sid->fstype,
						strlen(sid->fstype));
					pc_jw_lit(out, ",\"chain\":");
					pc_jw_str(out, sid->chain,
						strlen(sid->chain));
				}
				{
					/* the startup measurement policy
					 * followed: cached from the wal dir's
					 * .pc-walprobe unless re-probed */
					const struct pc_wal_probe *pr =
						pc_wal_probe_result();

					if (pr) {
						pc_jw_lit(out,
						  ",\"probe\":{\"cached\":");
						/* nothing is cached any more
						 * (DESIGN 12am); the key
						 * stays for consumers */
						pc_jw_lit(out, "false");
						pc_jw_lit(out,
						  ",\"sync_bs\":");
						pc_jw_i64(out, PC_WPROBE_SYNC_BS);
						pc_jw_lit(out, ",\"qd\":");
						pc_jw_i64(out, PC_WPROBE_QD);
						pc_jw_lit(out,
						  ",\"seq_bs\":");
						pc_jw_i64(out, PC_WPROBE_SEQ_BS);
						pc_jw_lit(out,
						  ",\"seq_mb_s\":");
						pc_jw_i64(out, (long long)
							(pr->seq_mb_s + 0.5));
						pc_jw_lit(out,
						  ",\"fsync_p50_us\":");
						pc_jw_i64(out,
							pr->fsync_p50_us);
						pc_jw_lit(out,
						  ",\"fsync_p99_us\":");
						pc_jw_i64(out,
							pr->fsync_p99_us);
						pc_jw_lit(out,
						  ",\"sync_iops\":");
						pc_jw_i64(out, pr->sync_iops);
						pc_jw_lit(out, "}");
					}
					{
						/* what the fsyncs ACTUALLY
						 * cost, beside what the probe
						 * predicted - the probe cannot
						 * see past a write-back cache,
						 * this is measured under real
						 * load (DESIGN 12am) */
						struct pc_wal_fsync_obs ob;

						pc_wal_fsync_observed(&ob);
						pc_jw_lit(out,
						  ",\"observed\":{\"fsync_n\":");
						pc_jw_i64(out, (long long)ob.n);
						pc_jw_lit(out,
						  ",\"fsync_avg_us\":");
						pc_jw_i64(out,
							(long long)ob.avg_us);
						pc_jw_lit(out,
						  ",\"fsync_recent_us\":");
						pc_jw_i64(out,
							(long long)ob.recent_us);
						pc_jw_lit(out,
						  ",\"fsync_max_us\":");
						pc_jw_i64(out,
							(long long)ob.max_us);
						pc_jw_lit(out,
						  ",\"probe_p50_us\":");
						pc_jw_i64(out, (long long)
							ob.probe_p50_us);
						pc_jw_lit(out,
						  ",\"probe_underestimated\":");
						pc_jw_lit(out,
							ob.probe_underestimated
							? "true" : "false");
						pc_jw_lit(out, "}");
					}
				}
				pc_jw_lit(out, ",\"appended\":");
				pc_jw_i64(out, (long long)ws.appended);
				pc_jw_lit(out, ",\"bytes\":");
				pc_jw_i64(out, (long long)ws.bytes);
				pc_jw_lit(out, ",\"dropped\":");
				pc_jw_i64(out, (long long)ws.dropped);
				pc_jw_lit(out, ",\"late\":");
				pc_jw_i64(out, (long long)ws.late);
				pc_jw_lit(out, ",\"recycles\":");
				pc_jw_i64(out, (long long)ws.recycles);
				pc_jw_lit(out, ",\"overruns\":");
				pc_jw_i64(out, (long long)ws.overruns);
				pc_jw_lit(out, ",\"free_segments\":");
				pc_jw_i64(out, ws.free_segments);
				pc_jw_lit(out, ",\"last_seq\":");
				pc_jw_i64(out, (long long)ws.last_seq);
				pc_jw_lit(out, ",\"synced_seq\":");
				pc_jw_i64(out, (long long)ws.synced_seq);
				pc_jw_lit(out, "}");
			}
		}
		pc_jw_lit(out, "}");
		return 0;
	}

	/* everything below needs a collection - but an UNKNOWN method must
	 * report method-not-found, not a missing collection */
	{
		static const char *cv[] = { "get", "set", "del", "exists",
			"expire", "ttl", "add", "sub", "mget", "mset", "keys",
			"scan", "jget", "jset", "jdel", "jincr",
			"jarrappend" };
		size_t i;
		int known = 0;

		for (i = 0; i < sizeof cv / sizeof cv[0]; i++)
			if (pc_json_streq(line, m, cv[i])) {
				known = 1;
				break;
			}
		if (!known)
			ERR(-32601, "method not found");
	}
	/* C7: every verb below this line touches collection data */
	if (serving_denied())
		ERR(-32000, PC_NOTREADY_MSG);
	ht = get_col(line, toks, ntok, params_tok, errmsg);
	if (!ht)
		return E_PARAMS;

	/* ---- get {col,key} --------------------------------------------- */
	if (pc_json_streq(line, m, "get")) {
		NEEDKEY();
		{
			char *gb = get_buf();
			unsigned int gl = 0;

			if (!gb)
				ERR(-32603, "get failed");
			rc = op_get_buf(ht, &k, gb, VAL_MAX, &gl, &exp,
				park_req);
			if (rc == PC_OP_OK) {
				write_hit(out, gb, (size_t)gl, exp);
				return 0;
			}
		}
		if (rc == PC_OP_ERR_GET) {
			ERR(-32603, "get failed");
		} else {
			/* miss AND the parked case: the fallback shape below
			 * is used only if the proto layer cannot park - it
			 * must match THIS verb (a set once got a pull-miss
			 * reply at park pressure) */
			pc_jw_lit(out, "{\"found\":false}");
		}
		return 0;
	}

	/* ---- exists {col,key} ------------------------------------------ */
	if (pc_json_streq(line, m, "exists")) {
		NEEDKEY();
		{
			unsigned long long ver = 0;

			/* ver rides on exists rather than get: it is a
			 * property of the record, not of the value, and a
			 * caller comparing two copies should not have to
			 * move the bytes to do it */
			rc = pcache_ht_getver(ht, &k, &ver);
			if (rc != 0) {
				pc_jw_lit(out, "{\"exists\":false}");
				return 0;
			}
			pc_jw_lit(out, "{\"exists\":true,\"ver\":");
			pc_jw_i64(out, (long long)ver);
			pc_jw_lit(out, "}");
		}
		return 0;
	}

	/* ---- ttl {col,key} : -2 absent, -1 no expiry, else seconds ----- */
	if (pc_json_streq(line, m, "ttl")) {
		NEEDKEY();
		rc = pcache_ht_probe(ht, &k, &vln, &exp, &is_ctr);
		pc_jw_lit(out, "{\"ttl\":");
		if (rc == -2)
			pc_jw_i64(out, -2);
		else if (exp == 0)
			pc_jw_i64(out, -1);
		else
			pc_jw_i64(out, (long long)exp - (long long)get_ticks());
		pc_jw_lit(out, "}");
		return 0;
	}

	/* ---- del {col,key} --------------------------------------------- */
	if (pc_json_streq(line, m, "del")) {
		NEEDKEY();
		rc = op_del(ht, &k, park_req);
		pc_jw_lit(out, rc == PC_OP_OK ? "{\"deleted\":true}"
			: "{\"deleted\":false}");
		return 0;
	}

	/* ---- expire {col,key,ttl} : re-arm without rewriting ----------- */
	if (pc_json_streq(line, m, "expire")) {
		NEEDKEY();
		if (get_int(line, toks, ntok, params_tok, "ttl", &ttl))
			ERR(E_PARAMS, "missing ttl");
		pc_jw_lit(out, op_expire(ht, &k, ttl) == PC_OP_OK
			? "{\"updated\":true}" : "{\"updated\":false}");
		return 0;
	}

	/* ---- add/sub {col,key,by?,ttl?} -> {value:n} ------------------- */
	if (pc_json_streq(line, m, "add") || pc_json_streq(line, m, "sub")) {
		long long nv;

		NEEDKEY();
		if (get_int(line, toks, ntok, params_tok, "by", &by))
			by = 1;
		ttl = 0;
		get_int(line, toks, ntok, params_tok, "ttl", &ttl);
		if (pc_json_streq(line, m, "sub"))
			by = -by;
		rc = op_addsub(ht, &k, by, ttl, &nv, park_req, 0);
		if (rc == PC_OP_PARKED) {
			pc_jw_lit(out, "{\"error\":\"cluster busy\"}");
			return 0;
		}
		if (rc == PC_OP_ERR_BUSY)
			ERR(-32001, "parked-request table full - retry");
		if (rc == PC_OP_ERR_FWD)
			ERR(-32000, "forward failed");
		if (rc == PC_OP_ERR_NOTINT)
			ERR(E_PARAMS, "value is not an integer");
		pc_jw_lit(out, "{\"value\":");
		pc_jw_i64(out, nv);
		pc_jw_lit(out, "}");
		return 0;
	}

	/* ---- set {col,key,value,enc?,ttl?} ----------------------------- */
	if (pc_json_streq(line, m, "set")) {
		NEEDKEY();
		val = val_buf();
		if (!val)
			ERR(-32603, "out of memory");
		vlen = get_str(line, toks, ntok, params_tok, "value", "enc",
			val, VAL_WIRE_MAX);
		if (vlen < 0)
			ERR(E_PARAMS, "missing or oversized value");
		ttl = 0;
		get_int(line, toks, ntok, params_tok, "ttl", &ttl);
		v.s = val; v.len = vlen;

		rc = op_set(ht, &k, &v, ttl, park_req, 0);
		if (rc == PC_OP_OK)
			pc_jw_lit(out, "{\"stored\":true}");
		else if (rc == PC_OP_PARKED)
			pc_jw_lit(out, "{\"stored\":false}");
		else if (rc == PC_OP_ERR_BUSY)
			ERR(-32001, "parked-request table full - retry");
		else if (rc == PC_OP_ERR_FWD)
			ERR(-32000, "forward failed");
		else if (rc == PC_OP_ERR_WRFAIL)
			ERR(-32000, PC_WRFAIL_MSG);
		else if (rc == PC_OP_ERR_FULL)
			ERR(-32000, "cache full");
		else
			ERR(E_PARAMS, "value too large");
		return 0;
	}

	/* ---- json path verbs (S26 + the v1.1 extras): -------------------
	 * jget  {col,key,path?}            -> {"found":true,"value":<frag>}
	 * jset  {col,key,path?,val,nx?,xx?,mkpath?,ttl?} -> {"set":true}
	 * jdel  {col,key,path}             -> {"deleted":bool}
	 * jincr {col,key,path,by?}         -> {"value":N}
	 * jarrappend {col,key,path,val}    -> {"count":N}
	 * Documents are opaque JSON text in ordinary cells; edits are span
	 * splices (jsonpath.c) under the key's mutex stripe (pc_json_rmw -
	 * shared with the cluster forward plane).  TTL preserved unless ttl
	 * given.  On a proxy collection a non-holder FORWARDS the op to the
	 * holder (M_FWD_JSON); paths use plain names. */
	if (pc_json_streq(line, m, "jget") || pc_json_streq(line, m, "jset") ||
	        pc_json_streq(line, m, "jdel") ||
	        pc_json_streq(line, m, "jincr") ||
	        pc_json_streq(line, m, "jarrappend")) {
		char path[JP_NAME_PARAM];
		const char *fmsg = NULL, *sval = NULL;
		char *frag = NULL;
		size_t svlen = 0;
		long long by = 1, ttl2 = 0, nv = 0;
		int plen, rc2, fraglen = 0, cnt = 0, have_ttl, nx = 0, xx = 0;
		int mk = 0, t2, op;

		if (pc_json_streq(line, m, "jget"))
			op = PC_JOP_GET;
		else if (pc_json_streq(line, m, "jset"))
			op = PC_JOP_SET;
		else if (pc_json_streq(line, m, "jdel"))
			op = PC_JOP_DEL;
		else if (pc_json_streq(line, m, "jincr"))
			op = PC_JOP_INCR;
		else
			op = PC_JOP_APPEND;

		NEEDKEY();
		plen = get_str(line, toks, ntok, params_tok, "path", NULL,
			path, sizeof path);
		if (plen < 0) {
			if (pc_json_get(line, toks, ntok, params_tok,
			        "path") >= 0)
				ERR(E_PARAMS, "bad path");
			path[0] = '$';
			plen = 1;              /* default: the root */
		}
		if (op == PC_JOP_SET || op == PC_JOP_APPEND) {
			int tv = pc_json_get(line, toks, ntok, params_tok,
				"val");

			if (tv < 0)
				ERR(E_PARAMS, "missing val");
			/* the raw span IS the value (quotes restored for
			 * strings) - it arrived as parsed JSON */
			sval = line + (toks[tv].type == PC_J_STR ?
				toks[tv].start - 1 : toks[tv].start);
			svlen = (size_t)((toks[tv].type == PC_J_STR ?
				toks[tv].end + 1 : toks[tv].end) -
				(toks[tv].type == PC_J_STR ?
				toks[tv].start - 1 : toks[tv].start));
		}
		if (op == PC_JOP_INCR)
			get_int(line, toks, ntok, params_tok, "by", &by);
		have_ttl = get_int(line, toks, ntok, params_tok, "ttl",
			&ttl2) == 0;
		t2 = pc_json_get(line, toks, ntok, params_tok, "nx");
		nx = t2 >= 0 && pc_json_streq_prim(line, &toks[t2]);
		t2 = pc_json_get(line, toks, ntok, params_tok, "xx");
		xx = t2 >= 0 && pc_json_streq_prim(line, &toks[t2]);
		t2 = pc_json_get(line, toks, ntok, params_tok, "mkpath");
		mk = t2 >= 0 && pc_json_streq_prim(line, &toks[t2]);

		/* proxy non-holder (or shard non-owner): forward the whole
		 * op to the holder.  Shard needs no locator and no placement
		 * - the owner is deterministic, which closes the
		 * loc-miss-fork hole for shard collections by construction */
		if ((pc_store_proxy_enabled(ht) &&
		        pcache_ht_probe(ht, &k, NULL, NULL, NULL) != 0) ||
		        pc_store_shard_enabled(ht)) {
		    if (pc_cluster_enabled()) {
			const char *cn = col_name_of(ht);
			int shard = pc_store_shard_enabled(ht);
			int target;
			unsigned int req;

			if (shard) {
				target = pc_shard_owner(cn, strlen(cn), k.s,
					(size_t)k.len);
				if (!target)
					goto jlocal;   /* self-owned */
			} else {
				target = pc_loc_get(cn, strlen(cn), k.s,
					(size_t)k.len);
				if (!target)
					target = pc_place();
			}
			if (target) {
				/* remember the holder-to-be: every later op
				 * on this key forwards sticky instead of
				 * re-guessing placement */
				pc_loc_set(cn, strlen(cn), k.s,
					(size_t)k.len, target);
				req = pc_fwd_json_begin(target, op, cn,
					strlen(cn), k.s, (size_t)k.len,
					path, (size_t)plen, sval, svlen, by,
					have_ttl, ttl2, nx, xx, mk);
				if (req) {
					/* park-failure fallback per op */
					switch (op) {
					case PC_JOP_SET:
						pc_jw_lit(out,
							"{\"set\":false}");
						break;
					case PC_JOP_DEL:
						pc_jw_lit(out,
						    "{\"deleted\":false}");
						break;
					case PC_JOP_GET:
						pc_jw_lit(out,
						    "{\"found\":false}");
						break;
					default:
						pc_jw_lit(out, "{\"error\":"
						    "\"cluster busy\"}");
					}
					*park_req = req;
					return 0;
				}
			}
			if (op == PC_JOP_GET) {
				pc_jw_lit(out, "{\"found\":false}");
				return 0;
			}
			if (op == PC_JOP_DEL) {
				pc_jw_lit(out, "{\"deleted\":false}");
				return 0;
			}
			ERR(-32000, "no holder reachable");
		    }
		}

jlocal:
		rc2 = pc_json_rmw(ht, k.s, k.len, op, path, plen, sval,
			(int)svlen, by, have_ttl, ttl2, nx, xx, mk,
			&frag, &fraglen, &nv, &cnt, &fmsg);
		if (rc2 < 0)
			ERR(-32022, fmsg);
		switch (op) {
		case PC_JOP_GET:
			if (rc2 == 1) {
				pc_jw_lit(out, "{\"found\":false}");
			} else {
				pc_jw_lit(out, "{\"found\":true,\"value\":");
				pc_jw_raw(out, frag, (size_t)fraglen);
				pc_jw_lit(out, "}");
			}
			free(frag);
			break;
		case PC_JOP_SET:
			pc_jw_lit(out, "{\"set\":true}");
			break;
		case PC_JOP_DEL:
			pc_jw_lit(out, rc2 == 1 ? "{\"deleted\":false}"
				: "{\"deleted\":true}");
			break;
		case PC_JOP_INCR:
			pc_jw_lit(out, "{\"value\":");
			pc_jw_i64(out, nv);
			pc_jw_lit(out, "}");
			break;
		default:                       /* APPEND */
			pc_jw_lit(out, "{\"count\":");
			pc_jw_i64(out, cnt);
			pc_jw_lit(out, "}");
		}
		return 0;
	}

	/* ---- scan {col,cursor?,match?,count?,values?} ------------------ */
	/* ---- keys {col,match?,limit?} ---------------------------------- */
	if (pc_json_streq(line, m, "scan") || pc_json_streq(line, m, "keys")) {
		struct scan_ctx sc;
		char pat[256];
		long long cur = 0, count = 0, limit = 10000;
		unsigned int cursor;
		int is_keys = pc_json_streq(line, m, "keys");

		memset(&sc, 0, sizeof sc);
		sc.w = out;
		sc.patlen = -1;
		if (params_tok >= 0 &&
		        pc_json_get(line, toks, ntok, params_tok, "match") >= 0) {
			sc.patlen = get_str(line, toks, ntok, params_tok, "match",
				NULL, pat, sizeof pat);
			if (sc.patlen < 0)
				ERR(E_PARAMS, "bad match pattern");
		}
		sc.pat = sc.patlen >= 0 ? pat : NULL;
		sc.now = get_ticks();

		if (is_keys) {
			get_int(line, toks, ntok, params_tok, "limit", &limit);
			if (limit < 1 || limit > 100000)
				ERR(E_PARAMS, "limit out of range");
			sc.limit = (int)limit;
			pc_jw_lit(out, "{\"keys\":[");
			cursor = 0;
			do {
				rc = pcache_ht_scan_ex(ht, &cursor, 1024,
					PCACHE_SCAN_NOVAL, keys_cb, &sc);
			} while (rc == 0 && cursor && !out->overflow);
			pc_jw_lit(out, "],\"truncated\":");
			pc_jw_lit(out, sc.stopped ? "true" : "false");
			pc_jw_lit(out, "}");
			return 0;
		}

		get_int(line, toks, ntok, params_tok, "cursor", &cur);
		if (cur < 0 || cur > 0xFFFFFFFFLL)
			ERR(E_PARAMS, "bad cursor");
		get_int(line, toks, ntok, params_tok, "count", &count);
		if (count < 0 || count > 16384)
			ERR(E_PARAMS, "count out of range");
		{
			int tv = pc_json_get(line, toks, ntok, params_tok, "values");

			sc.values = tv >= 0 && pc_json_streq_prim(line, &toks[tv]);
		}
		cursor = (unsigned int)cur;
		pc_jw_lit(out, "{\"items\":[");
		/* values requested = copy them out; otherwise keys-only, which
		 * skips the value memcpy AND its cold cachelines (S40) */
		pcache_ht_scan_ex(ht, &cursor, (unsigned int)count,
			sc.values ? 0 : PCACHE_SCAN_NOVAL, scan_cb, &sc);
		pc_jw_lit(out, "],\"cursor\":");
		pc_jw_i64(out, cursor);
		pc_jw_lit(out, ",\"more\":");
		pc_jw_lit(out, cursor ? "true" : "false");
		pc_jw_lit(out, "}");
		return 0;
	}

	/* ---- mget {col,keys:[...]} ------------------------------------- */
	if (pc_json_streq(line, m, "mget")) {
		int tk = pc_json_get(line, toks, ntok, params_tok, "keys"), i, ki;

		if (tk < 0 || toks[tk].type != PC_J_ARR)
			ERR(E_PARAMS, "missing keys array");
		pc_jw_lit(out, "{\"values\":[");
		ki = 0;
		for (i = tk + 1; i < ntok; i++) {
			if (toks[i].parent != tk || toks[i].type != PC_J_STR)
				continue;
			klen = pc_json_unescape(line, &toks[i], key, KEY_MAX);
			if (ki++)
				pc_jw_lit(out, ",");
			if (klen < 0) {
				pc_jw_lit(out, "{\"found\":false}");
				continue;
			}
			k.s = key; k.len = klen;
			rc = pcache_ht_fetch_ex(ht, &k, &v, &exp, NULL);
			if (rc == 0) {
				write_hit(out, v.s, (size_t)v.len, exp);
				free(v.s);
			} else {
				pc_jw_lit(out, "{\"found\":false}");
			}
		}
		pc_jw_lit(out, "]}");
		return 0;
	}

	/* ---- mset {col,items:[{key,value,enc?,ttl?},...]} -------------- */
	if (pc_json_streq(line, m, "mset")) {
		int ti = pc_json_get(line, toks, ntok, params_tok, "items"), i;
		int stored = 0, dropped = 0;

		if (ti < 0 || toks[ti].type != PC_J_ARR)
			ERR(E_PARAMS, "missing items array");
		if (pc_store_shard_enabled(ht))
			ERR(E_PARAMS, "mset on a shard collection is not "
				"supported yet - use pipelined set");
		val = val_buf();
		if (!val)
			ERR(-32603, "out of memory");
		for (i = ti + 1; i < ntok; i++) {
			if (toks[i].parent != ti || toks[i].type != PC_J_OBJ)
				continue;
			klen = get_str(line, toks, ntok, i, "key", NULL, key, KEY_MAX);
			vlen = get_str(line, toks, ntok, i, "value", "enc", val,
				VAL_WIRE_MAX);
			if (klen < 0 || vlen < 0) {
				dropped++;
				continue;
			}
			ttl = 0;
			get_int(line, toks, ntok, i, "ttl", &ttl);
			k.s = key; k.len = klen;
			v.s = val; v.len = vlen;
			if (pcache_ht_store(ht, &k, &v, ttl_to_abs(ttl)) == 0) {
				pc_wal_upsert(col_name_of(ht), k.s, k.len, v.s, v.len,
					ttl_to_abs(ttl), pcache_last_ver);
				stored++;
			} else {
				dropped++;
			}
		}
		pc_jw_lit(out, "{\"stored\":");
		pc_jw_i64(out, stored);
		pc_jw_lit(out, ",\"dropped\":");
		pc_jw_i64(out, dropped);
		pc_jw_lit(out, "}");
		return 0;
	}

	(void)ret;
	ERR(-32601, "method not found");
}

/* ---- the binary codec (item 7: the libperfd hot path) -------------------
 * One frame = one data verb; layouts in proto.h.  col/key/value bytes
 * arrive RAW inside the frame and are used in place (zero copy, no
 * unescape leg).  @out is a plain byte sink here (pc_jw_raw only).
 * When parking, the codec pre-writes its own park-failure shape into
 * @out/@flags and reports col/key spans for the park table. */

static void bw_u8(struct pc_jw *w, unsigned int v)
{
	char b = (char)v;

	pc_jw_raw(w, &b, 1);
}

static void bw_u32(struct pc_jw *w, unsigned int v)
{
	unsigned char b[4];

	b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24;
	pc_jw_raw(w, (char *)b, 4);
}

static void bw_i64(struct pc_jw *w, long long sv)
{
	unsigned long long v = (unsigned long long)sv;
	unsigned char b[8];
	int i;

	for (i = 0; i < 8; i++)
		b[i] = (unsigned char)(v >> (8 * i));
	pc_jw_raw(w, (char *)b, 8);
}

static unsigned int br_u16(const char *p)
{
	return (unsigned char)p[0] | ((unsigned int)(unsigned char)p[1] << 8);
}

static long long br_i64(const char *p)
{
	unsigned long long v = 0;
	int i;

	for (i = 0; i < 8; i++)
		v |= (unsigned long long)(unsigned char)p[i] << (8 * i);
	return (long long)v;
}

#define ERRB(msg) do { *errmsg = (msg); return -1; } while (0)

int pc_verb_bin(const char *pl, size_t plen, struct pc_jw *out, int *flags,
		unsigned int *park_req, const char **colp, size_t *collenp,
		const char **keyp, size_t *klenp, const char **errmsg)
{
	unsigned int verb, cn;
	size_t klen, off;
	pcache_htable_t *ht;
	str k, v;
	long long by = 1, ttl = 0, nv;
	unsigned int exp, vln;
	int rc, is_ctr;

	if (!plen)
		ERRB("empty request");
	verb = (unsigned char)pl[0];
	if (verb == PC_VERB_PING) {
		pc_jw_raw(out, pl + 1, plen - 1);
		return 0;
	}
	if (verb < PC_VERB_GET || verb > PC_VERB_SUB)
		ERRB("unknown verb");
	if (serving_denied())                  /* C7 - PING above is exempt */
		ERRB(PC_NOTREADY_MSG);
	if (plen < 4)
		ERRB("short request");
	cn = (unsigned char)pl[1];
	klen = br_u16(pl + 2);
	off = 4;
	if (verb == PC_VERB_EXPIRE || verb == PC_VERB_SET)
		off += 8;                      /* ttl i64 */
	else if (verb == PC_VERB_ADD || verb == PC_VERB_SUB)
		off += 16;                     /* by i64, ttl i64 */
	if (plen < off + cn + klen)
		ERRB("short request");
	if (!klen || klen > KEY_MAX)
		ERRB("missing or bad key");
	ht = pc_store_find(pl + off, cn);
	if (!ht)
		ERRB("no such collection");
	k.s = (char *)pl + off + cn;
	k.len = (int)klen;
	*colp = pl + off;
	*collenp = cn;
	*keyp = k.s;
	*klenp = klen;

	switch (verb) {
	case PC_VERB_GET:
		{
			char *gb = get_buf();
			unsigned int gl = 0;

			if (!gb)
				ERRB("get failed");
			rc = op_get_buf(ht, &k, gb, VAL_MAX, &gl, &exp,
				park_req);
			if (rc == PC_OP_OK) {
				bw_u8(out, 1);
				bw_u32(out, exp ? exp - get_ticks() : 0);
				pc_jw_raw(out, gb, (size_t)gl);
				return 0;
			}
		}
		if (rc == PC_OP_ERR_GET) {
			ERRB("get failed");
		} else {
			bw_u8(out, 0);         /* miss; also the park fallback */
		}
		return 0;

	case PC_VERB_EXISTS:
		bw_u8(out, pcache_ht_probe(ht, &k, NULL, NULL, NULL) == 0);
		return 0;

	case PC_VERB_TTL:
		rc = pcache_ht_probe(ht, &k, &vln, &exp, &is_ctr);
		if (rc == -2)
			bw_i64(out, -2);
		else if (exp == 0)
			bw_i64(out, -1);
		else
			bw_i64(out, (long long)exp - (long long)get_ticks());
		return 0;

	case PC_VERB_DEL:
		rc = op_del(ht, &k, park_req);
		bw_u8(out, rc == PC_OP_OK);       /* parked falls back to 0 */
		return 0;

	case PC_VERB_EXPIRE:
		ttl = br_i64(pl + 4);
		bw_u8(out, op_expire(ht, &k, ttl) == PC_OP_OK);
		return 0;

	case PC_VERB_ADD:
	case PC_VERB_SUB:
		by = br_i64(pl + 4);
		ttl = br_i64(pl + 12);
		if (verb == PC_VERB_SUB)
			by = -by;
		rc = op_addsub(ht, &k, by, ttl, &nv, park_req, 0);
		if (rc == PC_OP_PARKED) {
			*flags = PC_BIN_F_ERR; /* the park-failure fallback */
			pc_jw_raw(out, "cluster busy", 12);
			return 0;
		}
		if (rc == PC_OP_ERR_BUSY)
			ERRB("parked-request table full - retry");
		if (rc == PC_OP_ERR_FWD)
			ERRB("forward failed");
		if (rc == PC_OP_ERR_NOTINT)
			ERRB("value is not an integer");
		bw_i64(out, nv);
		return 0;

	default:                               /* PC_VERB_SET */
		v.s = (char *)pl + off + cn + klen;
		v.len = (int)(plen - off - cn - klen);
		ttl = br_i64(pl + 4);
		rc = op_set(ht, &k, &v, ttl, park_req, 0);
		if (rc == PC_OP_OK)
			bw_u8(out, 1);
		else if (rc == PC_OP_PARKED)
			bw_u8(out, 0);         /* the park-failure fallback */
		else if (rc == PC_OP_ERR_BUSY)
			ERRB("parked-request table full - retry");
		else if (rc == PC_OP_ERR_FWD)
			ERRB("forward failed");
		else if (rc == PC_OP_ERR_WRFAIL)
			ERRB(PC_WRFAIL_MSG);
		else if (rc == PC_OP_ERR_FULL)
			ERRB("cache full");
		else
			ERRB("value too large");
		return 0;
	}
}

/* ---- the probe-resume replays (proto.c, PC_DONE_SET_RESUME) -------------
 * A probe confirmed the key absent fleet-wide; the deferred write runs
 * the normal core with the probe suppressed, so placement decides and
 * may still forward (re-park). */

int pc_op_set_resume(const char *col, size_t collen, const char *key,
		size_t klen, const char *val, int vlen, long long ttl,
		unsigned int *park)
{
	pcache_htable_t *ht = pc_store_find(col, collen);
	str k, v;

	if (!ht)
		return PC_OP_ERR_GET;
	k.s = (char *)key;
	k.len = (int)klen;
	v.s = (char *)val;
	v.len = vlen;
	return op_set(ht, &k, &v, ttl, park, 1);
}

int pc_op_add_resume(const char *col, size_t collen, const char *key,
		size_t klen, long long by, long long ttl, long long *nv,
		unsigned int *park)
{
	pcache_htable_t *ht = pc_store_find(col, collen);
	str k;

	if (!ht)
		return PC_OP_ERR_GET;
	k.s = (char *)key;
	k.len = (int)klen;
	return op_addsub(ht, &k, by, ttl, nv, park, 1);
}

/* ---- RESP compatibility dialect (task S29) ------------------------------
 * RESP2 requests from unmodified Redis clients (redis-cli, rtpengine,
 * hiredis apps) answer through the SAME op cores as the other two
 * dialects, so cluster semantics cannot drift.  The proto layer frames
 * (multibulk or inline) into an argv; this codec maps commands onto
 * verbs and writes the RESP reply - errors included - into @out.
 *
 * The Redis database-index model maps onto collections BY NAME: the
 * connection starts on the collection named "0", SELECT n moves to the
 * collection named "n" (a RESP-serving deployment names collections
 * "0".."15").  Unsupported commands and options refuse loudly with
 * -ERR; nothing is silently accepted.  Cut 1 command set is the
 * universal KV core - the rtpengine-specific list is settled by the
 * S29 wire capture, per the task spec. */

static void resp_err(struct pc_jw *w, const char *msg)
{
	pc_jw_lit(w, "-ERR ");
	pc_jw_lit(w, msg);
	pc_jw_lit(w, "\r\n");
}

/* RESP carries the error CODE in the first word and clients dispatch on
 * it: a retryable condition must answer -TRYAGAIN, not -ERR.  resp_err()
 * hardcodes ERR, so backpressure needs its own emitter - otherwise every
 * client sees a generic error and none of them retries, which is the whole
 * point of saying "busy" instead of "forward failed".  S38. */
static void resp_err_code(struct pc_jw *w, const char *code, const char *msg)
{
	pc_jw_lit(w, "-");
	pc_jw_lit(w, code);
	pc_jw_lit(w, " ");
	pc_jw_lit(w, msg);
	pc_jw_lit(w, "\r\n");
}

static void resp_simple(struct pc_jw *w, const char *s)
{
	pc_jw_lit(w, "+");
	pc_jw_lit(w, s);
	pc_jw_lit(w, "\r\n");
}

static void resp_int(struct pc_jw *w, long long v)
{
	pc_jw_lit(w, ":");
	pc_jw_i64(w, v);
	pc_jw_lit(w, "\r\n");
}

static void resp_nil(struct pc_jw *w)
{
	pc_jw_lit(w, "$-1\r\n");
}

static void resp_bulk(struct pc_jw *w, const char *p, size_t n)
{
	pc_jw_lit(w, "$");
	pc_jw_i64(w, (long long)n);
	pc_jw_lit(w, "\r\n");
	pc_jw_raw(w, p, n);
	pc_jw_lit(w, "\r\n");
}

static void resp_arr(struct pc_jw *w, int n)
{
	pc_jw_lit(w, "*");
	pc_jw_i64(w, n);
	pc_jw_lit(w, "\r\n");
}

/* case-insensitive command word compare (Redis commands are ci) */
static int resp_is(const char *a, size_t al, const char *b)
{
	size_t i;

	for (i = 0; i < al; i++) {
		char ca = a[i], cb = b[i];

		if (!cb)
			return 0;
		if (ca >= 'a' && ca <= 'z')
			ca -= 32;
		if (cb >= 'a' && cb <= 'z')
			cb -= 32;
		if (ca != cb)
			return 0;
	}
	return b[al] == 0;
}

static int resp_ll(const char *p, size_t n, long long *out)
{
	char num[24];

	if (!n || n >= sizeof num)
		return -1;
	memcpy(num, p, n);
	num[n] = 0;
	{
		char *end;
		long long v = strtoll(num, &end, 10);

		if (*end)
			return -1;
		*out = v;
	}
	return 0;
}

/* RESP walker callbacks: bulk keys into the element scratch */
static int resp_keys_cb(const str *key, const str *val, unsigned int exp,
		void *p)
{
	struct scan_ctx *sc = p;

	(void)val;
	sc->scanned++;
	if (exp && exp <= sc->now)
		return 0;
	if (sc->pat && !pc_glob(sc->pat, sc->patlen, key->s, key->len))
		return 0;
	if (sc->w->overflow)
		return -1;
	if (sc->emitted >= sc->limit) {
		sc->stopped = 1;
		return -1;
	}
	sc->emitted++;
	resp_bulk(sc->w, key->s, (size_t)key->len);
	return 0;
}

/* Is @name inside the RESP collection allow-list?  resp_collections is
 * a comma list; NULL means every collection is visible (S33).  This is
 * what stops a RESP client reaching collections meant for native
 * clients - SELECT maps a db index onto a collection NAME, so without
 * it any name is reachable. */
static int resp_col_allowed(const char *name, size_t n)
{
	const char *list = pc_resp_collections(), *p;

	if (!list)
		return 1;
	for (p = list; *p; ) {
		const char *e;
		size_t len;

		while (*p == ' ' || *p == ',' || *p == '\t')
			p++;
		e = p;
		while (*e && *e != ',')
			e++;
		len = (size_t)(e - p);
		while (len && (p[len - 1] == ' ' || p[len - 1] == '\t'))
			len--;
		if (len == n && !memcmp(p, name, n))
			return 1;
		p = e;
	}
	return 0;
}

/* the selected collection, or an -ERR written into @out */
static pcache_htable_t *resp_col(struct pc_jw *out, const char *cur_col,
		size_t cur_collen)
{
	pcache_htable_t *ht;

	if (!resp_col_allowed(cur_col, cur_collen)) {
		resp_err(out, "DB index is out of range");
		return NULL;
	}
	ht = pc_store_find(cur_col, cur_collen);
	if (!ht)
		resp_err(out, "no such collection (name one after the RESP "
			"db index, e.g. [collection 0])");
	return ht;
}

/* ---- CLUSTER: the topology a stock Redis client routes by (S44) ------
 * Placement runs rendezvous over the SLOT, so ownership is derivable
 * here and a Redis client in any language can route without an extra
 * hop.  What this is NOT is Redis Cluster: there are no MOVED
 * redirects, and none are needed - a request landing on the wrong node
 * is FORWARDED, so a client with a stale map stays correct and merely
 * pays the hop it would have paid anyway.  That is why CLUSTER INFO
 * reports state ok whenever the fleet is up: there is no slot that
 * nobody can answer for.
 *
 * The slot space here is FRAGMENTED.  Rendezvous decides each slot
 * independently, so runs of a single owner average ~1.5 slots and a
 * three-node fleet yields ~11k ranges where Redis would have 3.  It is
 * truthful and clients cope (they expand any reply into a 16384-entry
 * table), but it makes CLUSTER SLOTS a large reply - measured and
 * discussed under S44 in doc/DESIGN.md.  CLUSTER SHARDS is ~6x smaller
 * for the same information because it does not repeat the node block
 * per range; prefer it where the client supports it.
 */

/* a 40-hex node name, the only shape Redis clients accept.  Built from
 * the PERSISTED identity when the peer has one, so a name survives a
 * restart the way a Redis node name does; a peer whose build predates
 * identities falls back to its node number, stable while it holds it. */
static void resp_cl_id(char *out, const struct pc_member *m)
{
	static const char hx[] = "0123456789abcdef";
	int i;

	if (m->has_ident) {
		for (i = 0; i < 16; i++) {
			out[(size_t)i * 2]     = hx[(m->ident[i] >> 4) & 0xF];
			out[(size_t)i * 2 + 1] = hx[m->ident[i] & 0xF];
		}
		for (i = 0; i < 8; i++)
			out[32 + i] = hx[(m->incarn >> ((7 - i) * 4)) & 0xF];
	} else {
		for (i = 0; i < 40; i++)
			out[i] = '0';
		out[38] = hx[(m->node >> 4) & 0xF];
		out[39] = hx[m->node & 0xF];
	}
}

/* the port the CLUSTER family advertises for a member: its RESP door.
 * A Redis client routed at client_port would be dialling the native
 * door, which off-box speaks only Noise - the client cannot even say
 * hello.  Falls back to client_port for a peer whose build predates the
 * gossiped field, which is exactly the old (broken off-box, workable
 * on-box) behaviour rather than a made-up number. */
static int resp_cl_port(const struct pc_member *m)
{
	return m->resp_port ? m->resp_port : m->client_port;
}

/* one member as [ip, port, id], the triple CLUSTER SLOTS wants */
static void resp_cl_node(struct pc_jw *w, const struct pc_member *m)
{
	char id[40], ip[INET_ADDRSTRLEN];

	resp_cl_id(id, m);
	if (!inet_ntop(AF_INET, &m->addr, ip, sizeof(ip)))
		ip[0] = 0;
	resp_arr(w, 3);
	resp_bulk(w, ip, strlen(ip));
	resp_int(w, resp_cl_port(m));
	resp_bulk(w, id, 40);
}

/* index of the member owning @slot, or -1.  pc_shard_owner_slot()
 * answers 0 for "this node", which is a node NUMBER everywhere else,
 * so the self case is resolved through the member list, not assumed. */
static int resp_cl_owner(const struct pc_member *mem, int nm, unsigned slot)
{
	int want = pc_shard_owner_slot(slot), i;

	for (i = 0; i < nm; i++) {
		if (want == 0 ? mem[i].is_self : mem[i].node == want)
			return i;
	}
	return -1;
}

/* fill @ownv with the owning member INDEX for every slot (0xFF: none).
 * Computed once per reply: both CLUSTER SLOTS and CLUSTER SHARDS walk
 * the same array, so the two replies cannot disagree, and neither pays
 * for 16384 rendezvous rounds twice. */
static void resp_cl_ownmap(const struct pc_member *mem, int nm,
		unsigned char *ownv)
{
	int s;

	for (s = 0; s < PC_SLOTS; s++) {
		int o = resp_cl_owner(mem, nm, (unsigned)s);

		ownv[s] = o < 0 ? 0xFF : (unsigned char)o;
	}
}

/* number of maximal equal-owner runs, counting only @who when it is
 * >= 0 (SHARDS asks per member) or every owner when it is -1 */
static int resp_cl_runs(const unsigned char *ownv, int who)
{
	int s, n = 0;

	for (s = 0; s < PC_SLOTS; s++) {
		if (ownv[s] == 0xFF || (who >= 0 && ownv[s] != who))
			continue;
		if (!s || ownv[s - 1] != ownv[s])
			n++;
	}
	return n;
}

int pc_keys_chunk(void *htv, unsigned int *cursor, const char *pat,
		int patlen, unsigned int now, struct pc_jw *w, int *emitted,
		int limit, int *limit_hit)
{
	pcache_htable_t *ht = htv;
	struct scan_ctx sc;

	memset(&sc, 0, sizeof sc);
	sc.w = w;
	sc.pat = patlen >= 0 ? pat : NULL;
	sc.patlen = patlen;
	sc.now = now;
	sc.emitted = *emitted;
	sc.limit = limit;
	pcache_ht_scan_ex(ht, cursor, 1024, PCACHE_SCAN_NOVAL,
		resp_keys_cb, &sc);
	*emitted = sc.emitted;
	*limit_hit = sc.stopped;
	return *cursor != 0;
}

int pc_verb_resp(char *const *argv, const size_t *argl, int nargs,
		struct pc_jw *out, char *scratch, size_t scratch_cap,
		unsigned int *park_req, const char **colp, size_t *collenp,
		const char **keyp, size_t *klenp, char *cur_col,
		size_t *cur_collen, int *quit, int *authed,
		struct pc_enum_start *es, void *obs)
{
	pcache_htable_t *ht;
	const char *cmd;
	size_t cl;
	str k, v;
	long long ttl = 0, by, nv;
	unsigned int exp;
	int rc;

	if (nargs < 1)
		return 0;                      /* empty inline line: no reply */
	cmd = argv[0];
	cl = argl[0];

	/* An unauthenticated connection (RESP listener with a password
	 * configured, S33) may only greet, authenticate and hang up -
	 * exactly Redis's requirepass behaviour, PING included, so stock
	 * clients need no special-casing. */
	if (!*authed && !resp_is(cmd, cl, "AUTH") &&
	        !resp_is(cmd, cl, "HELLO") && !resp_is(cmd, cl, "QUIT")) {
		pc_jw_lit(out, "-NOAUTH Authentication required.\r\n");
		return 0;
	}

	/* ---- connection plane ---- */
	if (resp_is(cmd, cl, "PING")) {
		if (nargs > 1)
			resp_bulk(out, argv[1], argl[1]);
		else
			resp_simple(out, "PONG");
		return 0;
	}
	if (resp_is(cmd, cl, "ECHO")) {
		if (nargs != 2)
			resp_err(out, "wrong number of arguments for 'echo' command");
		else
			resp_bulk(out, argv[1], argl[1]);
		return 0;
	}
	if (resp_is(cmd, cl, "QUIT")) {
		resp_simple(out, "OK");
		*quit = 1;
		return 0;
	}
	if (resp_is(cmd, cl, "AUTH")) {
		/* On the native plaintext listener the handshake IS the
		 * authentication (5.1) and there is no password plane - same
		 * answer a requirepass-less Redis gives.  On a RESP listener
		 * (S33) there is no handshake, so an optional password is the
		 * strongest in-band control the dialect allows.  Accept both
		 * RESP2 forms: AUTH <pw> and the ACL-style AUTH <user> <pw>
		 * (any username - we have one principal). */
		const char *pw;
		size_t pwn;

		if (!pc_resp_password_set()) {
			resp_err(out, "Client sent AUTH, but no password is set");
			return 0;
		}
		if (nargs == 2) {
			pw = argv[1];
			pwn = argl[1];
		} else if (nargs == 3) {
			pw = argv[2];
			pwn = argl[2];
		} else {
			resp_err(out, "wrong number of arguments for 'auth' "
				"command");
			return 0;
		}
		if (pc_resp_password_ok(pw, pwn)) {
			*authed = 1;
			resp_simple(out, "OK");
		} else {
			PC_RESP_BUMP(pc_resp_authfail);
			pc_jw_lit(out, "-WRONGPASS invalid username-password "
				"pair or user is disabled.\r\n");
		}
		return 0;
	}
	if (resp_is(cmd, cl, "HELLO")) {
		long long ver = 2;

		if (nargs > 1 && resp_ll(argv[1], argl[1], &ver) < 0) {
			resp_err(out, "Protocol version is not an integer or "
				"out of range");
			return 0;
		}
		if (ver != 2) {
			pc_jw_lit(out, "-NOPROTO unsupported protocol version\r\n");
			return 0;
		}
		resp_arr(out, 14);
		resp_bulk(out, "server", 6);
		resp_bulk(out, "perfcached", 10);
		resp_bulk(out, "version", 7);
		resp_bulk(out, PC_VERSION, strlen(PC_VERSION));
		resp_bulk(out, "proto", 5);
		resp_int(out, 2);
		resp_bulk(out, "id", 2);
		resp_int(out, 0);
		resp_bulk(out, "mode", 4);
		resp_bulk(out, "standalone", 10);
		resp_bulk(out, "role", 4);
		resp_bulk(out, "master", 6);
		resp_bulk(out, "modules", 7);
		resp_arr(out, 0);
		return 0;
	}
	if (resp_is(cmd, cl, "COMMAND")) {
		resp_arr(out, 0);              /* redis-cli probes; empty is fine */
		return 0;
	}
	if (resp_is(cmd, cl, "CONFIG")) {
		if (nargs >= 2 && resp_is(argv[1], argl[1], "GET"))
			resp_arr(out, 0);      /* "no such parameter" */
		else
			resp_err(out, "unsupported CONFIG subcommand");
		return 0;
	}
	if (resp_is(cmd, cl, "CLUSTER")) {
		struct pc_member mem[PC_CL_MAXMEMBERS];
		int nm = pc_cluster_members(mem, PC_CL_MAXMEMBERS);
		const char *sub = nargs >= 2 ? argv[1] : "";
		size_t subl = nargs >= 2 ? argl[1] : 0;

		/* KEYSLOT is answerable with no cluster at all, and is how
		 * an operator checks our slot maths against real Redis. */
		if (resp_is(sub, subl, "KEYSLOT")) {
			if (nargs != 3) {
				resp_err(out, "wrong number of arguments for "
					"'cluster|keyslot' command");
				return 0;
			}
			resp_int(out, pc_key_slot(argv[2], argl[2]));
			return 0;
		}
		if (resp_is(sub, subl, "MYID")) {
			char id[40];
			int i;

			for (i = 0; i < nm && !mem[i].is_self; i++)
				;
			if (i == nm) {
				resp_err(out, "this node is not in a cluster");
				return 0;
			}
			resp_cl_id(id, &mem[i]);
			resp_bulk(out, id, 40);
			return 0;
		}
		if (resp_is(sub, subl, "INFO")) {
			struct pc_jw b;
			int on = nm > 0;

			pc_jw_init(&b, scratch, scratch_cap);
			pc_jw_lit(&b, "cluster_enabled:");
			pc_jw_i64(&b, on);
			/* ok whenever the fleet is up: every slot has an
			 * owner, and a miss is forwarded rather than
			 * refused, so there is no "slot not served" state */
			pc_jw_lit(&b, "\r\ncluster_state:");
			pc_jw_lit(&b, on ? "ok" : "fail");
			pc_jw_lit(&b, "\r\ncluster_slots_assigned:");
			pc_jw_i64(&b, on ? PC_SLOTS : 0);
			pc_jw_lit(&b, "\r\ncluster_slots_ok:");
			pc_jw_i64(&b, on ? PC_SLOTS : 0);
			pc_jw_lit(&b, "\r\ncluster_slots_pfail:0"
				"\r\ncluster_slots_fail:0"
				"\r\ncluster_known_nodes:");
			pc_jw_i64(&b, nm);
			pc_jw_lit(&b, "\r\ncluster_size:");
			pc_jw_i64(&b, nm);
			pc_jw_lit(&b, "\r\ncluster_current_epoch:0"
				"\r\ncluster_my_epoch:0\r\n");
			if (b.overflow)
				resp_err(out, "cluster info too large");
			else
				resp_bulk(out, b.buf, b.len);
			return 0;
		}

		if (!nm) {
			resp_err(out, "This instance has cluster support "
				"disabled");
			return 0;
		}

		if (resp_is(sub, subl, "NODES")) {
			/* The LEGACY text topology, and still the one the
			 * widest tooling reads: captured on the wire,
			 * redis-benchmark --cluster opens with exactly
			 * "CLUSTER NODES" (never INFO first), and Lettuce
			 * parses this format too.  Implemented S49, because
			 * without it the ecosystem's own load driver cannot
			 * route here and S44's payoff cannot even be measured.
			 *
			 * One line per node:
			 *   <id> <ip:port@cport> <flags> <master> <ping-sent>
			 *   <pong-recv> <config-epoch> <link-state> <slots...>
			 * There is no cluster bus, so cport repeats the client
			 * port - parsers split on the @ and use what precedes
			 * it.  No replicas exist, so flags are master (plus
			 * myself on the answering node) and the master field
			 * is "-". */
			struct pc_jw tw;
			unsigned char *ownv;
			int i, s;

			/* tw is a STACK local: pc_jw_init_heap() refuses a
			 * writer whose len is nonzero (its stranded-bytes
			 * guard), and uninitialised stack read as exactly
			 * that.  Found by the map test: NODES answered "out
			 * of memory" on any worker whose stack was dirty
			 * from earlier requests, and worked on a virgin
			 * daemon - which is why every smoke test passed. */
			memset(&tw, 0, sizeof tw);
			ownv = malloc(PC_SLOTS);
			if (!ownv) {
				resp_err(out, "out of memory building the "
					"slot map");
				return 0;
			}
			resp_cl_ownmap(mem, nm, ownv);
			/* worst case is one range per slot (~200KB of text);
			 * sized for the common case, grows if wrong */
			if (pc_jw_init_heap(&tw, (size_t)resp_cl_runs(ownv, -1)
			        * 13 + (size_t)nm * 128) != 0) {
				free(ownv);
				resp_err(out, "out of memory building the "
					"slot map");
				return 0;
			}
			for (i = 0; i < nm; i++) {
				char id[40], ip[INET_ADDRSTRLEN];

				resp_cl_id(id, &mem[i]);
				if (!inet_ntop(AF_INET, &mem[i].addr, ip,
				        sizeof(ip)))
					ip[0] = 0;
				pc_jw_raw(&tw, id, 40);
				pc_jw_lit(&tw, " ");
				pc_jw_lit(&tw, ip);
				pc_jw_lit(&tw, ":");
				pc_jw_i64(&tw, resp_cl_port(&mem[i]));
				pc_jw_lit(&tw, "@");
				pc_jw_i64(&tw, resp_cl_port(&mem[i]));
				pc_jw_lit(&tw, mem[i].is_self ?
					" myself,master" : " master");
				pc_jw_lit(&tw, " - 0 0 0 connected");
				for (s = 0; s < PC_SLOTS; s++) {
					int e;

					if (ownv[s] != i)
						continue;
					if (s && ownv[s - 1] == ownv[s])
						continue;
					for (e = s; e + 1 < PC_SLOTS &&
					        ownv[e + 1] == ownv[s]; e++)
						;
					pc_jw_lit(&tw, " ");
					pc_jw_i64(&tw, s);
					if (e > s) {
						pc_jw_lit(&tw, "-");
						pc_jw_i64(&tw, e);
					}
				}
				pc_jw_lit(&tw, "\n");
			}
			free(ownv);
			if (tw.overflow)
				resp_err(out, "cluster nodes reply too large");
			else
				resp_bulk(out, tw.buf, tw.len);
			pc_jw_free(&tw);
			return 0;
		}
		if (resp_is(sub, subl, "SLOTS") || resp_is(sub, subl, "SHARDS")) {
			int is_shards = resp_is(sub, subl, "SHARDS");
			unsigned char *ownv;
			int i, s, nr;

			/* 16KB, allocated per call rather than kept in a
			 * static: workers are THREADS, and a shared scratch
			 * buffer here would corrupt one reply with another's
			 * walk under any concurrency at all. */
			ownv = malloc(PC_SLOTS);
			if (!ownv) {
				resp_err(out, "out of memory building the "
					"slot map");
				return 0;
			}
			resp_cl_ownmap(mem, nm, ownv);

			if (!is_shards) {
				nr = resp_cl_runs(ownv, -1);
				/* SLOTS is the one reply whose size a request
				 * does not bound: it enumerates the slot
				 * space, and rendezvous scatters ownership, so
				 * a 4-node fleet already exceeds the fixed
				 * per-worker scratch and was refused as "reply
				 * too large".  Move it to a heap buffer sized
				 * for the ranges we are about to write - ~101
				 * bytes each at the widest (5-digit slots, a
				 * 15-char address, a 40-char name) - so the
				 * common case never reallocs. */
				if (pc_jw_init_heap(out,
				        (size_t)nr * 104 + 64) != 0) {
					free(ownv);
					resp_err(out, "could not build the "
						"slot map");
					return 0;
				}
				resp_arr(out, nr);
				for (s = 0; s < PC_SLOTS; s++) {
					int e;

					if (ownv[s] == 0xFF)
						continue;
					if (s && ownv[s - 1] == ownv[s])
						continue;
					for (e = s; e + 1 < PC_SLOTS &&
					        ownv[e + 1] == ownv[s]; e++)
						;
					resp_arr(out, 3);
					resp_int(out, s);
					resp_int(out, e);
					resp_cl_node(out, &mem[ownv[s]]);
				}
				free(ownv);
				return 0;
			}
			/* SHARDS groups the same ranges by owner, writing the
			 * node block once instead of once per range - the
			 * difference between a ~130KB and a ~770KB reply on a
			 * three-node fleet. */
			resp_arr(out, nm);
			for (i = 0; i < nm; i++) {
				char id[40], ip[INET_ADDRSTRLEN];

				nr = resp_cl_runs(ownv, i);
				resp_arr(out, 4);
				resp_bulk(out, "slots", 5);
				resp_arr(out, nr * 2);
				for (s = 0; s < PC_SLOTS; s++) {
					int e;

					if (ownv[s] != i)
						continue;
					if (s && ownv[s - 1] == ownv[s])
						continue;
					for (e = s; e + 1 < PC_SLOTS &&
					        ownv[e + 1] == ownv[s]; e++)
						;
					resp_int(out, s);
					resp_int(out, e);
				}
				resp_bulk(out, "nodes", 5);
				resp_arr(out, 1);
				resp_cl_id(id, &mem[i]);
				if (!inet_ntop(AF_INET, &mem[i].addr, ip,
				        sizeof(ip)))
					ip[0] = 0;
				resp_arr(out, 14);
				resp_bulk(out, "id", 2);
				resp_bulk(out, id, 40);
				resp_bulk(out, "port", 4);
				resp_int(out, resp_cl_port(&mem[i]));
				resp_bulk(out, "ip", 2);
				resp_bulk(out, ip, strlen(ip));
				resp_bulk(out, "endpoint", 8);
				resp_bulk(out, ip, strlen(ip));
				/* every node owns its slots outright - there
				 * are no replicas of a shard here - so the
				 * only honest role is master */
				resp_bulk(out, "role", 4);
				resp_bulk(out, "master", 6);
				resp_bulk(out, "replication-offset", 18);
				resp_int(out, 0);
				resp_bulk(out, "health", 6);
				resp_bulk(out, "online", 6);
			}
			free(ownv);
			return 0;
		}
		if (resp_is(sub, subl, "COUNTKEYSINSLOT")) {
			/* the count is not tracked per slot, and answering
			 * a made-up number would be worse than refusing */
			resp_err(out, "unsupported CLUSTER subcommand "
				"(keys are not counted per slot)");
			return 0;
		}
		resp_err(out, "unsupported CLUSTER subcommand");
		return 0;
	}
	if (resp_is(cmd, cl, "CLIENT")) {
		if (nargs == 3 && resp_is(argv[1], argl[1], "SETNAME")) {
			pc_obs_conn_name(obs, argv[2], argl[2]);
			resp_simple(out, "OK");
		} else if (nargs == 2 && resp_is(argv[1], argl[1], "LIST")) {
			/* the Grafana Redis datasource's client panel reads
			 * exactly this: one k=v line per connection */
			struct pc_jw b;

			pc_jw_init(&b, scratch, scratch_cap);
			pc_obs_client_list(&b, get_ticks());
			if (b.overflow)
				resp_err(out, "client list too large");
			else
				resp_bulk(out, b.buf, b.len);
		} else if (nargs == 2 && resp_is(argv[1], argl[1],
		        "GETNAME")) {
			resp_bulk(out, "", 0);   /* name echo: minimal */
		} else {
			resp_err(out, "unsupported CLIENT subcommand");
		}
		return 0;
	}

	if (resp_is(cmd, cl, "SLOWLOG")) {
		const char *sub = nargs >= 2 ? argv[1] : "";
		size_t subl = nargs >= 2 ? argl[1] : 0;

		if (resp_is(sub, subl, "GET")) {
			long long n = 10;

			if (nargs >= 3 &&
			        (resp_ll(argv[2], argl[2], &n) < 0 || n < -1))
				n = 10;
			pc_obs_slowlog_get(out, (int)(n < 0 ? 512 : n));
		} else if (resp_is(sub, subl, "LEN")) {
			resp_int(out, pc_obs_slowlog_len());
		} else if (resp_is(sub, subl, "RESET")) {
			pc_obs_slowlog_reset();
			resp_simple(out, "OK");
		} else {
			resp_err(out, "unsupported SLOWLOG subcommand");
		}
		return 0;
	}
	if (resp_is(cmd, cl, "SELECT")) {
		if (nargs != 2 || argl[1] == 0 || argl[1] >= 40) {
			resp_err(out, "DB index is out of range");
			return 0;
		}
		if (!resp_col_allowed(argv[1], argl[1])) {
			resp_err(out, "DB index is out of range");
			return 0;
		}
		if (!pc_store_find(argv[1], argl[1])) {
			resp_err(out, "DB index is out of range (no collection "
				"with that name)");
			return 0;
		}
		memcpy(cur_col, argv[1], argl[1]);
		*cur_collen = argl[1];
		resp_simple(out, "OK");
		return 0;
	}
	if (resp_is(cmd, cl, "INFO")) {
		struct pc_jw b;
		const char *sec = nargs >= 2 ? argv[1] : NULL;
		size_t secl = nargs >= 2 ? argl[1] : 0;
		int all;

		/* Redis semantics, which the Grafana Redis datasource RELIES
		 * on: INFO <section> returns ONLY that section (the plugin's
		 * commandstats panel got every section appended and rendered
		 * nothing); bare INFO returns the default set, which
		 * excludes commandstats; "all"/"everything" include it. */
		if (sec && (resp_is(sec, secl, "all") ||
		        resp_is(sec, secl, "everything") ||
		        resp_is(sec, secl, "default")))
			sec = NULL, all = 1;
		else
			all = 0;
#define INFO_WANT(_n) (!sec || resp_is(sec, secl, _n))
		pc_jw_init(&b, scratch, scratch_cap);
		if (INFO_WANT("server"))
			pc_jw_lit(&b, "# Server\r\nredis_version:7.0.0\r\n"
				"redis_mode:standalone\r\n"
				"perfcached_version:" PC_VERSION "\r\n"
				"perfcached_dialect:resp2-compat\r\n");
		/* The Replication section is NOT decoration: rtpengine asks
		 * INFO at startup purely to learn whether the server is a
		 * master, and REFUSES TO START without a role line - found by
		 * pointing a real rtpengine at us.  A perfcached node is
		 * always writable, so the honest answer is master with no
		 * replicas: redundancy lives in the collection modes. */
		if (INFO_WANT("replication"))
			pc_jw_lit(&b, "# Replication\r\nrole:master\r\n"
				"connected_slaves:0\r\n"
				"master_failover_state:no-failover\r\n"
				"master_repl_offset:0\r\n");
		/* S53: the fields the Grafana summary panels read.
		 * used_memory is the arena's HELD bytes - the figure sized
		 * against arena_mb, not a malloc guess. */
		if (INFO_WANT("clients")) {
			pc_jw_lit(&b, "# Clients\r\nconnected_clients:");
			pc_jw_i64(&b, pc_obs_conn_count());
			pc_jw_lit(&b, "\r\n");
		}
		if (INFO_WANT("memory")) {
			pc_jw_lit(&b, "# Memory\r\nused_memory:");
			pc_jw_i64(&b, (long long)pcache_arena_held_bytes());
			pc_jw_lit(&b, "\r\nused_memory_human:");
			pc_jw_i64(&b, (long long)(pcache_arena_held_bytes()
				>> 20));
			pc_jw_lit(&b, "M\r\n");
		}
		if (INFO_WANT("stats")) {
			unsigned long long kh = 0, km = 0;
			int ci;

			for (ci = 0; ci < pc_store_count(); ci++) {
				pcache_ht_totals_t ct;

				pcache_ht_totals(pc_store_ht(ci), &ct);
				kh += ct.hits;
				km += ct.misses;
			}
			pc_jw_lit(&b, "# Stats\r\n"
				"total_commands_processed:");
			pc_jw_i64(&b, (long long)pc_obs_total_calls());
			pc_jw_lit(&b, "\r\ninstantaneous_ops_per_sec:");
			pc_jw_i64(&b, (long long)pc_obs_inst_ops());
			pc_jw_lit(&b, "\r\nkeyspace_hits:");
			pc_jw_i64(&b, (long long)kh);
			pc_jw_lit(&b, "\r\nkeyspace_misses:");
			pc_jw_i64(&b, (long long)km);
			pc_jw_lit(&b, "\r\n");
		}
		if (all || (sec && resp_is(sec, secl, "commandstats"))) {
			pc_jw_lit(&b, "# Commandstats\r\n");
			pc_obs_cmdstats(&b);
		}
		if (INFO_WANT("keyspace")) {
			pc_jw_lit(&b, "# Keyspace\r\n");
			ht = pc_store_find(cur_col, *cur_collen);
			if (ht) {
				pcache_ht_totals_t tot;

				pcache_ht_totals(ht, &tot);
				pc_jw_lit(&b, "db");
				pc_jw_raw(&b, cur_col, *cur_collen);
				pc_jw_lit(&b, ":keys=");
				pc_jw_i64(&b, (long long)tot.entries);
				pc_jw_lit(&b, ",expires=0,avg_ttl=0\r\n");
			}
		}
#undef INFO_WANT
		if (b.overflow)
			resp_err(out, "info too large");
		else
			resp_bulk(out, b.buf, b.len);
		return 0;
	}
	/* C7.  RESP gets Redis's OWN error for this condition: -LOADING is
	 * what a real server sends while it reads its dataset, so redis-py,
	 * jedis, phpredis and hiredis-based clients already recognise it
	 * and retry rather than surfacing it as a hard failure.  Using our
	 * own error string here would be compatible in form and useless in
	 * practice. */
	if (serving_denied()) {
		resp_err(out, "LOADING " PC_NOTREADY_MSG);
		return 0;
	}

	if (resp_is(cmd, cl, "DBSIZE")) {
		pcache_ht_totals_t tot;

		ht = resp_col(out, cur_col, *cur_collen);
		if (!ht)
			return 0;
		pcache_ht_totals(ht, &tot);
		resp_int(out, (long long)tot.entries);
		return 0;
	}

	if (resp_is(cmd, cl, "FLUSHDB") || resp_is(cmd, cl, "FLUSHALL")) {
		resp_err(out, "FLUSHDB is not supported (delete keys "
			"explicitly)");
		return 0;
	}

	/* S48: TIME is collection-independent (17.2M calls/day measured on
	 * the realtime instance) - answer before the collection resolves */
	if (resp_is(cmd, cl, "TIME")) {
		struct timespec tw;
		char sb[24], ub[24];
		int sn, un;

		clock_gettime(CLOCK_REALTIME, &tw);
		sn = snprintf(sb, sizeof sb, "%lld", (long long)tw.tv_sec);
		un = snprintf(ub, sizeof ub, "%ld", tw.tv_nsec / 1000);
		resp_arr(out, 2);
		resp_bulk(out, sb, (size_t)sn);
		resp_bulk(out, ub, (size_t)un);
		return 0;
	}

	/* ---- data plane ----
	 * Recognize the command BEFORE resolving the collection: an
	 * unknown command must answer "unknown command" even on a daemon
	 * with no collection named "0" (found by prototest's XYZZY probe
	 * answering "no such collection"). */
	{
		static const char *const dc[] = { "GET", "SET", "SETEX",
			"PSETEX", "DEL", "UNLINK", "EXISTS", "TYPE", "EXPIRE",
			"PEXPIRE", "EXPIREAT", "PEXPIREAT", "TTL", "PTTL",
			"INCR", "DECR", "INCRBY", "DECRBY", "MGET", "KEYS",
			"SCAN", "MEMORY" };
		size_t i;
		int known = 0;

		for (i = 0; i < sizeof dc / sizeof dc[0]; i++)
			if (resp_is(cmd, cl, dc[i])) {
				known = 1;
				break;
			}
		if (!known) {
			char nb[64];
			size_t n = cl < sizeof nb - 1 ? cl : sizeof nb - 1;

			memcpy(nb, cmd, n);
			nb[n] = 0;
			pc_jw_lit(out, "-ERR unknown command '");
			pc_jw_raw(out, nb, n);
			pc_jw_lit(out, "'\r\n");
			return 0;
		}
	}
	ht = resp_col(out, cur_col, *cur_collen);
	if (!ht)
		return 0;
	*colp = cur_col;
	*collenp = *cur_collen;

	if (resp_is(cmd, cl, "GET")) {
		if (nargs != 2) {
			resp_err(out, "wrong number of arguments for 'get' command");
			return 0;
		}
		if (argl[1] == 0 || argl[1] > KEY_MAX) {
			resp_err(out, "key too long");
			return 0;
		}
		k.s = argv[1];
		k.len = (int)argl[1];
		*keyp = argv[1];
		*klenp = argl[1];
		{
			char *gb = get_buf();
			unsigned int gl = 0;

			if (!gb) {
				resp_err(out, "get failed");
				return 0;
			}
			rc = op_get_buf(ht, &k, gb, VAL_MAX, &gl, &exp,
				park_req);
			if (rc == PC_OP_OK) {
				resp_bulk(out, gb, (size_t)gl);
				return 0;
			}
		}
		if (rc == PC_OP_ERR_GET) {
			resp_err(out, "get failed");
		} else {
			resp_nil(out);         /* miss; also the park fallback */
		}
		return 0;
	}

	if (resp_is(cmd, cl, "SET") || resp_is(cmd, cl, "SETEX") ||
	        resp_is(cmd, cl, "PSETEX")) {
		int is_setex = resp_is(cmd, cl, "SETEX");
		int is_psetex = resp_is(cmd, cl, "PSETEX");
		int vi = (is_setex || is_psetex) ? 3 : 2, i;

		ttl = 0;
		if (is_setex || is_psetex) {
			if (nargs != 4) {
				resp_err(out, "wrong number of arguments");
				return 0;
			}
			if (resp_ll(argv[2], argl[2], &ttl) < 0 || ttl <= 0) {
				resp_err(out, "invalid expire time");
				return 0;
			}
			if (is_psetex)
				ttl = (ttl + 999) / 1000;
		} else {
			if (nargs < 3) {
				resp_err(out, "wrong number of arguments for "
					"'set' command");
				return 0;
			}
			for (i = 3; i < nargs; i++) {
				if (resp_is(argv[i], argl[i], "EX") ||
				        resp_is(argv[i], argl[i], "PX")) {
					int px = resp_is(argv[i], argl[i], "PX");

					if (i + 1 >= nargs ||
					        resp_ll(argv[i + 1],
					            argl[i + 1], &ttl) < 0 ||
					        ttl <= 0) {
						resp_err(out, "invalid expire "
							"time in 'set' command");
						return 0;
					}
					if (px)
						ttl = (ttl + 999) / 1000;
					i++;
				} else {
					/* NX/XX/KEEPTTL/GET/EXAT/PXAT: not in
					 * cut 1 - refuse, never half-honour */
					resp_err(out, "unsupported SET option");
					return 0;
				}
			}
		}
		if (argl[1] == 0 || argl[1] > KEY_MAX) {
			resp_err(out, "key too long");
			return 0;
		}
		k.s = argv[1];
		k.len = (int)argl[1];
		v.s = argv[vi];
		v.len = (int)argl[vi];
		*keyp = argv[1];
		*klenp = argl[1];
		rc = op_set(ht, &k, &v, ttl, park_req, 0);
		if (rc == PC_OP_OK)
			resp_simple(out, "OK");
		else if (rc == PC_OP_PARKED)
			resp_err(out, "busy");         /* park fallback only */
		else if (rc == PC_OP_ERR_BUSY)
			/* Redis Cluster's own retryable code: a stock client
			 * backs off and retries instead of aborting, which is
			 * what a full parked-request table deserves - the
			 * request has NOT happened and trying again is
			 * correct.  redis-benchmark aborts on -ERR. */
			resp_err_code(out, "TRYAGAIN", "cluster busy, retry");
		else if (rc == PC_OP_ERR_FWD)
			resp_err(out, "forward failed");
		else if (rc == PC_OP_ERR_WRFAIL)
			/* not TRYAGAIN: retrying here cannot help, the node
			 * is out of the map until an operator fixes it */
			resp_err(out, PC_WRFAIL_MSG);
		else if (rc == PC_OP_ERR_FULL)
			resp_err(out, "cache full");
		else
			resp_err(out, "value too large");
		return 0;
	}

	if (resp_is(cmd, cl, "DEL") || resp_is(cmd, cl, "UNLINK")) {
		long long cnt = 0;
		int i;

		if (nargs < 2) {
			resp_err(out, "wrong number of arguments for 'del' command");
			return 0;
		}
		if (nargs == 2) {
			/* single key: parks exactly (proxy/shard forward) */
			if (argl[1] == 0 || argl[1] > KEY_MAX) {
				resp_err(out, "key too long");
				return 0;
			}
			k.s = argv[1];
			k.len = (int)argl[1];
			*keyp = argv[1];
			*klenp = argl[1];
			rc = op_del(ht, &k, park_req);
			if (rc == PC_OP_PARKED)
				resp_int(out, 0);      /* park fallback only */
			else
				resp_int(out, rc == PC_OP_OK ? 1 : 0);
			return 0;
		}
		/* multi-key: each op runs; a forwarded delete still executes
		 * at its owner but its ack completes into nobody, so it
		 * counts optimistically - stated, not hidden */
		for (i = 1; i < nargs; i++) {
			unsigned int throwaway = 0;

			if (argl[i] == 0 || argl[i] > KEY_MAX)
				continue;
			k.s = argv[i];
			k.len = (int)argl[i];
			rc = op_del(ht, &k, &throwaway);
			if (rc == PC_OP_OK || rc == PC_OP_PARKED)
				cnt++;
		}
		resp_int(out, cnt);
		return 0;
	}

	if (resp_is(cmd, cl, "EXISTS")) {
		long long cnt = 0;
		int i;
		if (nargs < 2) {
			resp_err(out, "wrong number of arguments for 'exists' "
				"command");
			return 0;
		}
		for (i = 1; i < nargs; i++) {
			if (argl[i] == 0 || argl[i] > KEY_MAX)
				continue;
			k.s = argv[i];
			k.len = (int)argl[i];
			if (pcache_ht_probe(ht, &k, NULL, NULL, NULL) == 0)
				cnt++;
		}
		resp_int(out, cnt);
		return 0;
	}

	if (resp_is(cmd, cl, "TYPE")) {
		if (nargs != 2 || argl[1] == 0 || argl[1] > KEY_MAX) {
			resp_err(out, "wrong number of arguments for 'type' command");
			return 0;
		}
		k.s = argv[1];
		k.len = (int)argl[1];
		resp_simple(out, pcache_ht_probe(ht, &k, NULL, NULL, NULL) == 0 ?
			"string" : "none");
		return 0;
	}

	if (resp_is(cmd, cl, "EXPIRE") || resp_is(cmd, cl, "PEXPIRE") ||
	        resp_is(cmd, cl, "EXPIREAT") || resp_is(cmd, cl, "PEXPIREAT")) {
		if (nargs != 3 || argl[1] == 0 || argl[1] > KEY_MAX ||
		        resp_ll(argv[2], argl[2], &ttl) < 0) {
			resp_err(out, "wrong number of arguments for 'expire' "
				"command");
			return 0;
		}
		/* S48: the AT forms carry an absolute wall-clock deadline;
		 * convert to relative AT THE BOUNDARY - the TTL machinery is
		 * tick-based and never learns wall time (the WAL's absolute
		 * stamps are its own conversion).  A past deadline falls into
		 * the ttl<=0 delete branch below, which is Redis's own
		 * semantics for it. */
		if (resp_is(cmd, cl, "PEXPIREAT")) {
			struct timespec tw;

			clock_gettime(CLOCK_REALTIME, &tw);
			ttl -= (long long)tw.tv_sec * 1000 +
				tw.tv_nsec / 1000000;
		} else if (resp_is(cmd, cl, "EXPIREAT")) {
			ttl -= (long long)time(NULL);
		}
		if (resp_is(cmd, cl, "PEXPIRE") ||
		        resp_is(cmd, cl, "PEXPIREAT"))
			ttl = (ttl + 999) / 1000;
		if (ttl <= 0) {
			/* Redis deletes on a non-positive relative expire */
			k.s = argv[1];
			k.len = (int)argl[1];
			*keyp = argv[1];
			*klenp = argl[1];
			rc = op_del(ht, &k, park_req);
			if (rc == PC_OP_PARKED)
				resp_int(out, 0);
			else
				resp_int(out, rc == PC_OP_OK ? 1 : 0);
			return 0;
		}
		k.s = argv[1];
		k.len = (int)argl[1];
		resp_int(out, op_expire(ht, &k, ttl) == PC_OP_OK ? 1 : 0);
		return 0;
	}

	if (resp_is(cmd, cl, "TTL") || resp_is(cmd, cl, "PTTL")) {
		unsigned int vln;
		int is_ctr;
		if (nargs != 2 || argl[1] == 0 || argl[1] > KEY_MAX) {
			resp_err(out, "wrong number of arguments for 'ttl' command");
			return 0;
		}
		k.s = argv[1];
		k.len = (int)argl[1];
		rc = pcache_ht_probe(ht, &k, &vln, &exp, &is_ctr);
		if (rc == -2)
			nv = -2;
		else if (exp == 0)
			nv = -1;
		else
			nv = (long long)exp - (long long)get_ticks();
		if (nv > 0 && resp_is(cmd, cl, "PTTL"))
			nv *= 1000;
		resp_int(out, nv);
		return 0;
	}

	/* S48: MEMORY USAGE <key> [SAMPLES n] - their tooling's 76
	 * calls/day.  The answer is the record's own bytes: the 24-byte
	 * record header plus key plus value.  Slab-class rounding is NOT
	 * included - same estimate class as Redis's own answer.  SAMPLES
	 * is accepted and ignored (it concerns aggregate types). */
	if (resp_is(cmd, cl, "MEMORY")) {
		unsigned int vln;
		int is_ctr;

		if (nargs < 3 || !resp_is(argv[1], argl[1], "USAGE")) {
			resp_err(out, "MEMORY supports only MEMORY USAGE "
				"<key> [SAMPLES n]");
			return 0;
		}
		if (argl[2] == 0 || argl[2] > KEY_MAX) {
			resp_err(out, "key too long");
			return 0;
		}
		k.s = argv[2];
		k.len = (int)argl[2];
		if (pcache_ht_probe(ht, &k, &vln, &exp, &is_ctr) == -2) {
			resp_nil(out);
			return 0;
		}
		resp_int(out, 24 + (long long)k.len + vln);
		return 0;
	}

	if (resp_is(cmd, cl, "INCR") || resp_is(cmd, cl, "DECR") ||
	        resp_is(cmd, cl, "INCRBY") || resp_is(cmd, cl, "DECRBY")) {
		int has_by = resp_is(cmd, cl, "INCRBY") ||
			resp_is(cmd, cl, "DECRBY");
		int neg = resp_is(cmd, cl, "DECR") ||
			resp_is(cmd, cl, "DECRBY");

		by = 1;
		if (has_by && (nargs != 3 ||
		        resp_ll(argv[2], argl[2], &by) < 0)) {
			resp_err(out, "value is not an integer or out of range");
			return 0;
		}
		if (!has_by && nargs != 2) {
			resp_err(out, "wrong number of arguments");
			return 0;
		}
		if (argl[1] == 0 || argl[1] > KEY_MAX) {
			resp_err(out, "key too long");
			return 0;
		}
		if (neg)
			by = -by;
		k.s = argv[1];
		k.len = (int)argl[1];
		*keyp = argv[1];
		*klenp = argl[1];
		/* ttl -1 = PRESERVE: Redis INCR never touches the expiry */
		rc = op_addsub(ht, &k, by, -1, &nv, park_req, 0);
		if (rc == PC_OP_PARKED)
			resp_err(out, "busy");         /* park fallback only */
		else if (rc == PC_OP_ERR_BUSY)
			/* Redis Cluster's own retryable code: a stock client
			 * backs off and retries instead of aborting, which is
			 * what a full parked-request table deserves - the
			 * request has NOT happened and trying again is
			 * correct.  redis-benchmark aborts on -ERR. */
			resp_err_code(out, "TRYAGAIN", "cluster busy, retry");
		else if (rc == PC_OP_ERR_FWD)
			resp_err(out, "forward failed");
		else if (rc == PC_OP_ERR_NOTINT)
			resp_err(out, "value is not an integer or out of range");
		else if (rc != PC_OP_OK)
			resp_err(out, "cache full");
		else
			resp_int(out, nv);
		return 0;
	}

	if (resp_is(cmd, cl, "MGET")) {
		int i;
		if (nargs < 2) {
			resp_err(out, "wrong number of arguments for 'mget' "
				"command");
			return 0;
		}
		/* local fetches, misses stay nil - the exact text-mget
		 * semantics (batch verbs never pull) */
		resp_arr(out, nargs - 1);
		for (i = 1; i < nargs; i++) {
			if (argl[i] == 0 || argl[i] > KEY_MAX) {
				resp_nil(out);
				continue;
			}
			k.s = argv[i];
			k.len = (int)argl[i];
			if (pcache_ht_fetch_ex(ht, &k, &v, &exp, NULL) == 0) {
				resp_bulk(out, v.s, (size_t)v.len);
				free(v.s);
			} else {
				resp_nil(out);
			}
		}
		return 0;
	}

	if (resp_is(cmd, cl, "KEYS")) {
		if (nargs != 2 || argl[1] >= 256) {
			resp_err(out, "wrong number of arguments for 'keys' "
				"command");
			return 0;
		}
		/* S40: KEYS held this worker for the WHOLE walk - 12-33ms
		 * measured - and every other connection multiplexed here ate
		 * it as tail latency (7.9ms p99 photographed on a user
		 * host).  The verb now only DECLARES the walk; the proto
		 * layer runs it one bounded chunk per event-loop turn and
		 * the worker keeps serving between chunks. */
		es->start = 1;
		es->ht = ht;
		es->patlen = (int)argl[1];
		if (es->patlen == 1 && argv[1][0] == '*')
			es->patlen = -1;       /* full walk, skip the matcher */
		else
			memcpy(es->pat, argv[1], argl[1]);
		es->limit = 100000;            /* the keys-verb hard cap */
		return 0;
	}

	if (resp_is(cmd, cl, "SCAN")) {
		struct scan_ctx sc;
		struct pc_jw ew;
		unsigned int cursor;
		long long cur, count = 128;
		char curbuf[16];
		int i, n;

		if (nargs < 2 || resp_ll(argv[1], argl[1], &cur) < 0 ||
		        cur < 0 || cur > 0xFFFFFFFFLL) {
			resp_err(out, "invalid cursor");
			return 0;
		}
		memset(&sc, 0, sizeof sc);
		pc_jw_init(&ew, scratch, scratch_cap);
		sc.w = &ew;
		sc.patlen = -1;
		sc.now = get_ticks();
		sc.limit = 0x7FFFFFFF;
		for (i = 2; i < nargs; i++) {
			if (resp_is(argv[i], argl[i], "MATCH") &&
			        i + 1 < nargs && argl[i + 1] < 256) {
				sc.pat = argv[i + 1];
				sc.patlen = (int)argl[i + 1];
				i++;
			} else if (resp_is(argv[i], argl[i], "COUNT") &&
			        i + 1 < nargs) {
				if (resp_ll(argv[i + 1], argl[i + 1],
				        &count) < 0 || count < 1 ||
				        count > 16384) {
					resp_err(out, "invalid COUNT");
					return 0;
				}
				i++;
			} else if (resp_is(argv[i], argl[i], "TYPE") &&
			        i + 1 < nargs) {
				i++;           /* strings only: accept, ignore */
			} else {
				resp_err(out, "syntax error");
				return 0;
			}
		}
		if (sc.patlen < 0)
			sc.pat = NULL;
		cursor = (unsigned int)cur;
		/* Yield like Redis: ~COUNT KEYS per reply, not COUNT buckets.
		 * Measured against redis 8 on an identical dataset: it
		 * returns ~COUNT keys per call (exactly 1000.0 at COUNT
		 * 1000), while a single bucket-budget walk returns COUNT x
		 * density - 27% more round trips at density 0.76, which was
		 * the whole remaining sweep deficit.  The loop bounds on
		 * keys SCANNED, never on keys matched, and on a hard bucket
		 * cap, so neither a sparse table nor a rare MATCH can turn
		 * one call into the full-table stall S40 exists to prevent.
		 * (Redis behaves the same way: its MATCH calls can return
		 * empty.) */
		{
			unsigned int visited = 0, budget = (unsigned int)count;
			unsigned int chunk = (unsigned int)count;

			while (visited < budget * 16) {
				pcache_ht_scan_ex(ht, &cursor, chunk,
					PCACHE_SCAN_NOVAL, resp_keys_cb, &sc);
				visited += chunk;
				if (!cursor || ew.overflow ||
				        sc.scanned >= (int)budget)
					break;
				/* Size the next chunk from the density this
				 * call just measured, so the reply lands NEAR
				 * count instead of overshooting by up to a
				 * whole chunk (~1515 keys for COUNT 1000 at
				 * density 0.76, and the fatter replies cost
				 * more than the saved round trips on some
				 * hosts).  An empty region so far means no
				 * estimate: keep the full chunk and let the
				 * 16x cap bound the walk. */
				if (sc.scanned > 0) {
					unsigned long long want =
						(unsigned long long)
						(budget - (unsigned)sc.scanned)
						* visited / (unsigned)sc.scanned;

					chunk = want ? (want > budget ?
						budget : (unsigned int)want) : 1;
				}
			}
		}
		if (ew.overflow) {
			resp_err(out, "reply too large");
			return 0;
		}
		n = snprintf(curbuf, sizeof curbuf, "%u", cursor);
		resp_arr(out, 2);
		resp_bulk(out, curbuf, (size_t)n);
		resp_arr(out, sc.emitted);
		pc_jw_raw(out, ew.buf, ew.len);
		return 0;
	}

	/* unreachable: the data-plane pre-check answered unknown commands */
	resp_err(out, "protocol");
	return 0;
}
