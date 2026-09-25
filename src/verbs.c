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
#include <ctype.h>
#include <time.h>

#include <pthread.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "compat/dprint.h"
#include "quiesce.h"
#include "compat/timer.h"
#include "json.h"
#include "jsonpath.h"
#include "store.h"
#include "events.h"
#include "config.h"        /* PC_MAX_COLLECTIONS */
#include "verbs.h"
#include "core/pcache_htable.h"
#include "core/pcache_arena.h"
#include "wal.h"
#include "rdb.h"
#include "recover.h"
#include "compat/compat.h"
#include "cluster.h"
#include "clcol.h"          /* CLCOL_OP_* for the DDL announce */
#include "pc_slot.h"                   /* the shared key->slot */
#include "obs.h"                       /* S53: the Grafana surfaces */
#include "core/pcache_mem.h"
#include "clterm.h"
#include "fnv1a.h"                     /* the one FNV-1a */
#include "clunk.h"                     /* S165 */
#include "metrics.h"                   /* the daemon's own start stamp */
#include "proto.h"                     /* PC_VERB_* / PC_BIN_F_ERR */
#include "../lib/perfd_push.h"         /* PS5: PFP_* */
#include "pubsub.h"                    /* PS1/PS2 */
#include "daemon.h"                    /* pc_worker_id */
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
#define JW_REPLY_INIT (1u << 20)           /* S112: dump's heap reply, grows to JW_HEAP_MAX */

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
		if (pc_store_live(i) && pc_store_ht(i) == ht)
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
	/* S127: under spread only K of P nodes hold the record, so a
	 * broadcast asks P-1 peers a question K of them can answer.
	 * Unicast the best-ranked holder instead.  If no holder is live the
	 * fall-through below still broadcasts, which is the honest answer
	 * during a membership change: the set we computed may be stale and
	 * somebody out there may still have the record. */
	{
		int kk = pc_cluster_replicas();

		if (kk && pc_cluster_enabled() && pc_store_pull_enabled(ht)) {
			const char *cn = col_name_of(ht);

			if (!pc_neg_hit(cn, strlen(cn), k->s, (size_t)k->len)) {
				int h = pc_spread_pick_peer(
					pc_key_slot(k->s, (size_t)k->len), kk);

				if (h) {
					unsigned int req = pc_pull_begin_at(cn,
						strlen(cn), k->s,
						(size_t)k->len, h);

					if (req) {
						*park = req;
						return PC_OP_PARKED;
					}
				}
			}
		}
	}
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
	/* S153: with the core reporting a past expiry on a miss, the line can
	 * say WHY - a TTL question and a keyspace question are different. */
	pc_ev_miss(ht, k, exp && *exp ? 1 : 0);
	return op_get_miss(ht, k, park);
}

/* S73: an eager collection replicates a write to every live peer the
 * moment it lands - on the write path, fire-and-forget, no TTL filter.
 * The sweep is repair behind this.  Only records this node AUTHORED:
 * a copy pushed back would echo the keyspace around the fleet, the
 * same rule the sweep keeps. */
static void eager_push(pcache_htable_t *ht, const str *k, const char *val,
		size_t vlen, unsigned int exp, unsigned long long ver)
{
	const char *cn;

	if (!pc_cluster_enabled() || !pc_store_eager_enabled(ht))
		return;
	cn = col_name_of(ht);
	if (pc_repl_push(cn, strlen(cn), k->s, (size_t)k->len, val, vlen,
	        exp, ver))
		return;
	/* S127: this node is NOT one of the record's K holders.  It accepted
	 * the write and forwarded it to the set - "any member may write" is
	 * about ADMISSION, not placement - and now it must not keep a
	 * long-lived copy, or spread quietly becomes K+1 for every writing
	 * node and the extra copies are orphans the sweep never repairs or
	 * reclaims.  A plain remove, NOT store_remove(): a tombstone here
	 * would delete the record from the holders we just sent it to. */
	pcache_ht_remove(ht, (str *)k);
	pc_wal_del(cn, k->s, k->len);
}

/* S213: a TTL re-arm on the record this node holds, whichever way it
 * arrived - the client's expire, or one forwarded by the node that took
 * it.  The touch ADOPTS a passive copy: the node now holds the latest
 * version, so the record is its own from here.  A copy that just became
 * ours goes to the WAL whole - copies are off the WAL, and a bare touch
 * entry replays to nothing after a restart.  Then it travels as
 * authored: bytes, expiry and version out of ONE read, so the copy pairs
 * the right value with the right version.  Before this a passive copy
 * was re-armed and left passive: the author's clock ran out while this
 * node's copy lived on, which is how a re-registered device sat on one
 * node of the fleet.  1 = re-armed, 0 = absent. */
static int op_expire_local(pcache_htable_t *ht, const str *k,
		unsigned int exp)
{
	const char *cn = col_name_of(ht);
	str val;
	unsigned int e = 0;
	unsigned char fl = 0;
	unsigned long long ver = 0;
	int was_passive = 0;

	if (pcache_ht_touch_adopt(ht, k, exp, &was_passive) != 1)
		return 0;
	if (pcache_ht_fetch_full(ht, k, &val, &e, &fl, &ver) != 0)
		return 1;                      /* re-armed, then gone */
	if (was_passive)
		pc_wal_upsert(cn, k->s, k->len, val.s, val.len, e, ver);
	else
		pc_wal_touch(cn, k->s, k->len, e, ver);
	eager_push(ht, k, val.s, (size_t)val.len, e, ver);
	return 1;
}

int pc_op_expire_apply(const char *col, size_t collen, const char *key,
		size_t klen, unsigned int exp)
{
	pcache_htable_t *ht = pc_store_find(col, collen);
	str k;

	if (!ht)
		return 0;
	k.s = (char *)key;
	k.len = (int)klen;
	return op_expire_local(ht, &k, exp);
}

/* S69: a delete reaches the table a resize is filling as well as the live
 * one - the copier has already carried some keys across, and a key deleted
 * after it passed would come back at the swap. */
static int store_remove(pcache_htable_t *ht, const str *k)
{
	pcache_htable_t *sh = pc_store_resize_shadow(ht);

	int rc;

	if (sh)
		(void)pcache_ht_remove(sh, k);
	rc = pcache_ht_remove(ht, k);
	if (rc == 1)
		pc_ev_remove(ht, k);                              /* S153 */
	return rc;
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
		rc = store_remove(ht, k);
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
	/* RV-11: store, eager, spread - and the key is not HERE.  That used
	 * to be the whole answer: "absent", no tombstone.  But a write taken
	 * by another node is applied and acknowledged there while its push
	 * to this one batches for up to REPL_FLUSH_MS, so a delete that a
	 * balancer lands here inside that window deleted nothing, and the
	 * push then put the record on every member.  A client cannot carry
	 * the ordering across - RESP has nowhere to put it - so the daemon
	 * does what it does for a read: it looks where the record IS.
	 *
	 * The node that HOLDS the record deletes it and tombstones under its
	 * OWN clock, and that clock has observed the record's version (every
	 * apply folds the sender's version in), so its tombstone is newer by
	 * construction and S209's compare orders it on every member - this
	 * node included, where the late push is then refused.  No cross-node
	 * clock comparison anywhere, which is what sank "tombstone on an
	 * absent key".
	 *
	 * Spread knows its holders: unicast the best-ranked live one.  Store
	 * and eager ask the fleet with the probe set/add/touch use - a
	 * positive answer becomes the forward, all-negative is "absent" and
	 * is remembered, so a repeated miss costs nothing.  A delete looks
	 * where a read would look: `pull = 0` keeps both local.  Only an
	 * UNVERSIONED negative entry skips the probe - a versioned one is a
	 * tombstone, and the key may have been written again since. */
	if (pc_cluster_enabled() && !pc_store_proxy_enabled(ht) &&
	        pcache_ht_probe(ht, k, NULL, NULL, NULL) != 0) {
		unsigned long long tv = 0;

		cn = col_name_of(ht);
		if (pc_cluster_replicas()) {
			int h = pc_spread_pick_peer(
				pc_key_slot(k->s, (size_t)k->len),
				pc_cluster_replicas());

			if (h) {
				unsigned int req = pc_fwd_begin(h, 1, cn,
					strlen(cn), k->s, (size_t)k->len, NULL, 0,
					0, 0);

				if (req) {
					*park = req;
					return PC_OP_PARKED;
				}
			}
			return PC_OP_ABSENT;
		}
		if (pc_store_pull_enabled(ht) &&
		        !(pc_neg_ver(cn, strlen(cn), k->s, (size_t)k->len, &tv)
		          && !tv)) {
			unsigned int req = pc_probe_fwd_begin(1, cn, strlen(cn),
				k->s, (size_t)k->len, NULL, 0, 0, 0);

			if (req) {
				*park = req;
				return PC_OP_PARKED;
			}
		}
		return PC_OP_ABSENT;
	}
	rc = store_remove(ht, k);
	if (rc == 1) {
		cn = col_name_of(ht);
		pc_wal_del(cn, k->s, k->len);
		if (pc_cluster_enabled())
			pc_tombstone_send(cn, strlen(cn), k->s, (size_t)k->len);
	}
	return rc == 1 ? PC_OP_OK : PC_OP_ABSENT;
}

/* S213: a re-arm is a write and takes the write's road.  It used to
 * touch whatever the local table held and answer "absent" for a key the
 * fleet holds - the one mutating verb without a mode branch, while set,
 * del and add/sub all forward to the owner.  Shard: the owner.  Proxy:
 * the locator's holder, else the same probe set/add use (a positive
 * answer becomes the forward; all-negative is "absent" - a re-arm never
 * places).  Spread: the best-ranked live holder, which adopts and pushes
 * to the rest.  Store and eager have nobody to forward to: absent here
 * is absent. */
static int op_expire(pcache_htable_t *ht, str *k, long long ttl,
		unsigned int *park)
{
	unsigned int ttl_rel = ttl > 0 ? (unsigned int)ttl : 0;
	const char *cn;

	if (writes_denied())
		return PC_OP_ERR_WRFAIL;
	if (pc_store_shard_enabled(ht) && pc_cluster_enabled()) {
		int owner;

		cn = col_name_of(ht);
		owner = pc_shard_owner(cn, strlen(cn), k->s, (size_t)k->len);
		if (owner) {
			unsigned int req = pc_fwd_begin(owner, 3, cn,
				strlen(cn), k->s, (size_t)k->len, NULL, 0,
				ttl_rel, 0);

			if (req) {
				*park = req;
				return PC_OP_PARKED;
			}
			return fwd_fail_code();
		}
		/* self-owned: fall through to the local re-arm */
	} else if (pc_store_proxy_enabled(ht) && pc_cluster_enabled() &&
	        pcache_ht_probe(ht, k, NULL, NULL, NULL) != 0) {
		int target;

		cn = col_name_of(ht);
		target = pc_loc_get(cn, strlen(cn), k->s, (size_t)k->len);
		if (target) {
			unsigned int req = pc_fwd_begin(target, 3, cn,
				strlen(cn), k->s, (size_t)k->len, NULL, 0,
				ttl_rel, 0);

			if (req) {
				*park = req;
				return PC_OP_PARKED;
			}
		} else if (!pc_neg_hit(cn, strlen(cn), k->s, (size_t)k->len)) {
			unsigned int req = pc_probe_fwd_begin(3, cn, strlen(cn),
				k->s, (size_t)k->len, NULL, 0, ttl_rel, 0);

			if (req) {
				*park = req;
				return PC_OP_PARKED;
			}
		}
		return PC_OP_ABSENT;
	} else if (pc_cluster_replicas() && pc_cluster_enabled() &&
	        pcache_ht_probe(ht, k, NULL, NULL, NULL) != 0) {
		int h = pc_spread_pick_peer(pc_key_slot(k->s, (size_t)k->len),
			pc_cluster_replicas());

		cn = col_name_of(ht);
		if (h) {
			unsigned int req = pc_fwd_begin(h, 3, cn, strlen(cn),
				k->s, (size_t)k->len, NULL, 0, ttl_rel, 0);

			if (req) {
				*park = req;
				return PC_OP_PARKED;
			}
		}
		return PC_OP_ABSENT;
	}
	return op_expire_local(ht, k, ttl_to_abs(ttl)) ? PC_OP_OK
		: PC_OP_ABSENT;
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
			eager_push(ht, k, nvbuf, (size_t)nvl, eff,
				pcache_last_ver);
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
		pc_ev_store(ht, k, (unsigned int)v->len, ttl);      /* S153 */
		eager_push(ht, k, v->s, (size_t)v->len, ttl_to_abs(ttl),
			pcache_last_ver);
		if (pc_cluster_enabled())
			pc_neg_clear(cn, strlen(cn), k->s, (size_t)k->len);
		pc_store_note_set(ht, (size_t)k->len, (size_t)v->len);   /* S67 */
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

/* S112: the first pass of dump - the keys of a bucket-bounded scan, packed
 * as u32 length + bytes, filtered by the Redis slot of the key.  The buffer
 * is one reply's worth: a chunk whose keys alone overflow it is refused
 * with "halve count" rather than cut mid-bucket, which would repeat. */
struct dump_keys {
	char *buf;
	size_t len, cap;
	unsigned int slo, shi, now;
	int full;
};
static int dump_key_cb(const str *key, const str *val, unsigned int exp,
		void *p)
{
	struct dump_keys *dk = p;
	uint32_t kl = (uint32_t)key->len;
	unsigned int slot;

	(void)val;
	if (exp && exp <= dk->now)
		return 0;                      /* expired-as-absent */
	slot = pc_key_slot(key->s, (size_t)key->len);
	if (slot < dk->slo || slot > dk->shi)
		return 0;
	if (dk->len + 4 + kl > dk->cap) {
		dk->full = 1;
		return -1;
	}
	memcpy(dk->buf + dk->len, &kl, 4);
	memcpy(dk->buf + dk->len + 4, key->s, kl);
	dk->len += 4 + kl;
	return 0;
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
			pc_ev_store(ht, &k, (unsigned int)nvs.len, 0);   /* S153 */
			pc_wal_upsert(cn, k.s, k.len, nd, rc2, newexp,
				pcache_last_ver);
			eager_push(ht, &k, nd, (size_t)rc2, newexp,
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

#define PC_WRFAIL_MSG "node is FAILED (its WAL cannot honour the writes it "\
	"acknowledges) - reads still served here, send writes to another member"

/* one message, so all three dialects say the same thing */
/* S69 */
#define PC_COL_DEFAULT_LOG2 12         /* 4,096 buckets: ~1.5 MB of index,
                                        * and the splitter grows it */
#define PC_NOCREATE_MSG "creating and dropping collections is not enabled "\
	"here - set [daemon] allow_create = yes on this node"
static int allow_create;

/* S129.  Two DIFFERENT questions, and collapsing them is the bug this
 * exists to prevent: allow_create answers "may this NODE originate DDL
 * at all", the connection's privilege bit answers "may THIS LINK".  The
 * client secret cannot answer the second - apps, ops tooling and
 * perfcli all present the same value, so gating on it hands drop and
 * resize to every application connection.
 *
 * The privileged set is chosen by BLAST RADIUS, not by the word DDL:
 * create/drop/resize/rename, and `restore` with them, because a restore
 * replaces every record it is given at a fresh version - a larger blast
 * radius than a resize.  `probe` is the next candidate (it stalls a
 * worker for up to thirty seconds) and is left out of v1 deliberately.
 */
#define PC_NOPRIV_MSG "this operation needs a privileged connection - "\
	"call enable with the [secrets] enable value first"
#define PC_NOENABLE_MSG "no [secrets] enable is configured on this node, "\
	"so privilege cannot be raised and this operation is unreachable"
static unsigned long long enable_fail;   /* failed enable attempts */

unsigned long long pc_verb_enable_fails(void)
{
	return enable_fail;
}

void pc_verb_set_allow_create(int on)
{
	allow_create = on;
}

/* RV-10: [listen] resp_redirect - see pc_verb_resp */
static int resp_redirect_moved;

void pc_verb_set_resp_redirect(int moved)
{
	resp_redirect_moved = moved;
}

/* S128: why a table could not be built.  A create or a resize is the one
 * client word that carves a multi-gigabyte structure in one go - 2^24
 * buckets is 3.0 GB of index - and until now the REGION carve had no
 * ceiling test at all, so one verb could put a node 21% past its
 * configured arena_mb.
 *
 * The CORE is what refuses now, before it carves the first region, which
 * is what makes the bound honest and is the only place that can be sure:
 * a resize to the size a collection already is builds nothing, and a
 * create of a name that exists builds nothing, so a test made before the
 * store has decided would refuse operations that never carve.  This runs
 * on the refusal the store hands back, and only names the three figures
 * an operator needs - what it would take, what is held, what the ceiling
 * is - when the ceiling really is the reason.  Anything else keeps the
 * generic message, because "the arena has no room" is a claim.
 *
 * A resize needs no special arithmetic for holding both tables at once:
 * the OLD table is already inside `held` and the NEW one is what was
 * asked for, so the single test IS the both-at-once question.  Returns
 * the detailed message, or NULL when the ceiling is not the cause. */
static __thread char index_fit_msg[224];

static const char *index_ceiling_msg(int bl)
{
	unsigned long want = pcache_htable_index_bytes((unsigned int)bl);
	unsigned long held = pcache_arena_held_bytes();
	unsigned long mx = pcache_arena_max_bytes;

	if (!mx || !want || held + want <= mx)
		return NULL;
	snprintf(index_fit_msg, sizeof index_fit_msg,
		"a table of 2^%d buckets needs %lu MB of index and the arena "
		"holds %lu MB of its %lu MB ceiling - raise [memory] arena_mb "
		"(or arena_cap_mb), or ask for fewer buckets",
		bl, want >> 20, held >> 20, mx >> 20);
	return index_fit_msg;
}

#define PC_NOTREADY_MSG "node is not READY (recovering) - retry, or use "\
	"another member"

/* S123: the running totals start again.  The door counters are
 * re-baselined in proto.c; the cluster plane's, the proxy plane's and the
 * WAL's structs are snapshotted here and the block below reports the
 * difference; the table's own counters go through the core's
 * pcache_ht_stats_reset(); the store's size tallies are zeroed.  Live
 * gauges - entries, buckets, open connections, memory, sequence numbers,
 * roles, pending peaks - are untouched. */
/* S165: the commands clients sent that no door implements, for the
 * whole fleet - this node's table folded with every live peer's latest
 * (cluster.c), newest first.  `count` is fleet-wide, `here` this node's
 * share, `node` the member that saw it last; `members` / `reporting` say
 * how much of the fleet the list covers.  Names only, never arguments. */
static void unknown_json(struct pc_jw *out)
{
	struct clunk_table *self = malloc(sizeof *self);
	struct clunk_table *fl = malloc(sizeof *fl);
	int members = 1, reporting = 1, i;

	if (!self || !fl) {
		free(self);
		free(fl);
		return;
	}
	pc_obs_unknown_local(self);
	pc_cluster_unknown_fleet(self, fl, &members, &reporting);
	pc_jw_lit(out, ",\"unknown_commands\":{\"members\":");
	pc_jw_i64(out, members);
	pc_jw_lit(out, ",\"reporting\":");
	pc_jw_i64(out, reporting);
	pc_jw_lit(out, ",\"other\":");
	pc_jw_i64(out, (long long)fl->other);
	pc_jw_lit(out, ",\"preauth\":");
	pc_jw_i64(out, (long long)fl->preauth);
	{
		/* peers' tables heard, and frames refused whole (clunk_parse) */
		unsigned long long rcv = 0, bad = 0;

		pc_cluster_unknown_figures(&rcv, &bad);
		pc_jw_lit(out, ",\"frames_received\":");
		pc_jw_i64(out, (long long)rcv);
		pc_jw_lit(out, ",\"frames_refused\":");
		pc_jw_i64(out, (long long)bad);
	}
	pc_jw_lit(out, ",\"rows\":[");
	for (i = 0; i < fl->n; i++) {
		const struct clunk_row *r = &fl->rows[i];

		pc_jw_lit(out, i ? ",{\"dialect\":\"" : "{\"dialect\":\"");
		pc_jw_lit(out, clunk_dialect_name(r->dialect));
		pc_jw_lit(out, "\",\"name\":");
		pc_jw_str(out, r->name, r->nlen);
		pc_jw_lit(out, ",\"count\":");
		pc_jw_i64(out, (long long)r->count);
		pc_jw_lit(out, ",\"here\":");
		pc_jw_i64(out, (long long)r->here);
		pc_jw_lit(out, ",\"first_s\":");
		pc_jw_i64(out, (long long)r->first_s);
		pc_jw_lit(out, ",\"last_s\":");
		pc_jw_i64(out, (long long)r->last_s);
		pc_jw_lit(out, ",\"addr\":");
		pc_jw_str(out, r->addr, strlen(r->addr));
		pc_jw_lit(out, ",\"client\":");
		pc_jw_str(out, r->client, strlen(r->client));
		pc_jw_lit(out, ",\"node\":");
		pc_jw_i64(out, r->node);
		pc_jw_lit(out, "}");
	}
	pc_jw_lit(out, "]}");
	free(self);
	free(fl);
}

static pthread_mutex_t reset_mx = PTHREAD_MUTEX_INITIALIZER;
static struct pc_cl_stats cl_base;
static struct pc_proxy_stats px_base;
static struct pc_wal_stats wal_base;
static long long reset_at_s;

long long pc_stats_reset(void)
{
	struct pc_cl_stats cs;
	struct pc_proxy_stats px;
	struct pc_wal_stats ws;
	int i, n = pc_store_count();

	pc_doors_reset();
	pc_obs_unknown_reset();                /* S165 */
	for (i = 0; i < n; i++)
		if (pc_store_live(i))          /* S150 C: a retired slot holds no table */
			pcache_ht_stats_reset(pc_store_ht(i));
	pc_store_size_reset();
	memset(&cs, 0, sizeof cs);
	memset(&px, 0, sizeof px);
	memset(&ws, 0, sizeof ws);
	if (pc_cluster_enabled()) {
		pc_cluster_get_stats(&cs);
		pc_proxy_get_stats(&px);
	}
	pc_wal_get_stats(&ws);
	pthread_mutex_lock(&reset_mx);
	cl_base = cs;
	px_base = px;
	wal_base = ws;
	reset_at_s = (long long)time(NULL);
	pthread_mutex_unlock(&reset_mx);
	return reset_at_s;
}

static void since_cl(struct pc_cl_stats *s)
{
	struct pc_cl_stats b;

	pthread_mutex_lock(&reset_mx);
	b = cl_base;
	pthread_mutex_unlock(&reset_mx);
	s->hb_sent -= b.hb_sent;
	s->hb_seen -= b.hb_seen;
	s->bad_auth -= b.bad_auth;
	s->hb_watchdog -= b.hb_watchdog;
	s->apply_stalls -= b.apply_stalls;      /* S216; the max is a gauge */
	s->epoch_refused -= b.epoch_refused;    /* S221 */
	s->joins -= b.joins;
	s->assigns -= b.assigns;
	s->elections -= b.elections;
	s->demotions -= b.demotions;
	s->pull_sent -= b.pull_sent;
	s->pull_served -= b.pull_served;
	s->pull_hits -= b.pull_hits;
	s->pull_misses -= b.pull_misses;
	s->pull_timeouts -= b.pull_timeouts;
	s->pend_exhausted -= b.pend_exhausted;
	s->fwd_no_route -= b.fwd_no_route;
	s->fwd_send_fail -= b.fwd_send_fail;
	s->tomb_sent -= b.tomb_sent;
	s->tomb_applied -= b.tomb_applied;
	s->neg_hits -= b.neg_hits;
	s->joins_rejected -= b.joins_rejected;
	s->reconciled -= b.reconciled;
	s->reconcile_probed -= b.reconcile_probed;
	s->repl_sweep_cycles -= b.repl_sweep_cycles;   /* S212; the clocks are gauges */
	s->reconcile_deferred -= b.reconcile_deferred;
	s->sync_sent -= b.sync_sent;
	s->sync_rx -= b.sync_rx;
	s->sync_ack -= b.sync_ack;
	s->sync_bad -= b.sync_bad;
	s->map_pub -= b.map_pub;
	s->map_rx -= b.map_rx;
	s->map_stale -= b.map_stale;
	s->map_bad -= b.map_bad;
	s->place_map -= b.place_map;
	s->place_hrw -= b.place_hrw;
	/* S125: the two receive-plane TOTALS are counters and re-baseline
	 * with the rest.  The three per-second figures and the two queue
	 * gauges are live readings, not running totals, so they do not -
	 * the same rule the memory and connection gauges follow. */
	s->rx_applied -= b.rx_applied;
	s->rx_drops -= b.rx_drops;
}

static void since_px(struct pc_proxy_stats *s)
{
	struct pc_proxy_stats b;

	pthread_mutex_lock(&reset_mx);
	b = px_base;
	pthread_mutex_unlock(&reset_mx);
	s->placed_local -= b.placed_local;
	s->placed_remote -= b.placed_remote;
	s->fwd_sent -= b.fwd_sent;
	s->fwd_served -= b.fwd_served;
	s->fwd_fails -= b.fwd_fails;
	s->migrated_out -= b.migrated_out;
	s->migrated_in -= b.migrated_in;
	s->migrate_skipped_big -= b.migrate_skipped_big;
	s->migrate_lost -= b.migrate_lost;
	s->migrate_retx -= b.migrate_retx;
	s->migrate_dgrams -= b.migrate_dgrams;
	s->bulk_out -= b.bulk_out;
	s->bulk_in -= b.bulk_in;
	s->repl_out -= b.repl_out;
	s->demotes_sent -= b.demotes_sent;
	s->demotes_applied -= b.demotes_applied;
	s->loc_hits -= b.loc_hits;
	s->loc_clears -= b.loc_clears;
	s->recv_older -= b.recv_older;
	s->recv_tombstoned -= b.recv_tombstoned;
	s->repl_bulk_lost -= b.repl_bulk_lost;
	s->repl_pushed -= b.repl_pushed;
	s->repl_skipped_dying -= b.repl_skipped_dying;
	s->boot_out -= b.boot_out;
	s->boot_in -= b.boot_in;
	s->boot_failed -= b.boot_failed;
	s->repl_groups -= b.repl_groups;
}

static void since_wal(struct pc_wal_stats *s)
{
	struct pc_wal_stats b;

	pthread_mutex_lock(&reset_mx);
	b = wal_base;
	pthread_mutex_unlock(&reset_mx);
	s->appended -= b.appended;
	s->bytes -= b.bytes;
	s->dropped -= b.dropped;
	s->late -= b.late;
	s->recycles -= b.recycles;
	s->overruns -= b.overruns;
}

/* S37: the fleet as JSON, rendered from THIS node's vantage point.
 * Shared by the `members` verb and the status page's /members route,
 * so the two cannot drift - a page disagreeing with the verb would
 * be worse than no page. */
/*
 * S67: the `stats` body, callable from the verb AND from the HTTP
 * door.  The dashboard needs exactly these numbers, and having no way
 * to reach them is why the page could only ever show what /members
 * carries - which is how it ended up reporting the RESP door's
 * connection count and nothing about the native one every client uses.
 *
 * @only_col NULL = every collection, else just that one.
 */
/* The facts about the PROCESS that the store cannot supply: how long it
 * has been up, how much CPU it has burned, and how much memory the
 * kernel believes it holds.  A dashboard had none of these, so its CPU
 * panel could not be written at all, and RSS - the only figure that
 * predicts an OOM kill - was invisible next to an arena number that
 * deliberately does not track it.
 *
 * perfcached is threaded, not forked, so /proc/self is the whole daemon:
 * Linux aggregates every thread's CPU into the process entry.
 *
 * CPU is CUMULATIVE milliseconds, never a percentage.  A percentage
 * needs two samples and a window, and only the caller knows its own
 * polling interval; a rate computed here could only be the mean since
 * startup, which flattens with age and hides exactly the recent spike
 * someone opened the page to find.
 */
static void stats_process(struct pc_jw *out)
{
	long tck = sysconf(_SC_CLK_TCK), pg = sysconf(_SC_PAGESIZE);
	unsigned long long ut = 0, st = 0, rss_pages = 0;
	long long thr = 0;
	FILE *f;
	char buf[512], *p;

	if (tck <= 0)
		tck = 100;
	if (pg <= 0)
		pg = 4096;

	f = fopen("/proc/self/statm", "r");
	if (f) {
		if (fscanf(f, "%*s %llu", &rss_pages) != 1)
			rss_pages = 0;
		fclose(f);
	}
	f = fopen("/proc/self/stat", "r");
	if (f) {
		/* comm sits in parentheses and may contain them itself,
		 * so the numeric fields start after the LAST ')'. */
		if (fgets(buf, sizeof buf, f)) {
			p = strrchr(buf, ')');
			if (p && sscanf(p + 1, " %*c %*d %*d %*d %*d %*d %*u"
					" %*u %*u %*u %*u %llu %llu %*d %*d"
					" %*d %*d %lld",
					&ut, &st, &thr) != 3) {
				ut = st = 0;
				thr = 0;
			}
		}
		fclose(f);
	}

	pc_jw_lit(out, ",\"process\":{\"uptime_s\":");
	pc_jw_i64(out, (long long)pc_metrics_uptime());
	pc_jw_lit(out, ",\"pid\":");
	pc_jw_i64(out, (long long)getpid());
	pc_jw_lit(out, ",\"threads\":");
	pc_jw_i64(out, thr);
	pc_jw_lit(out, ",\"cpu_user_ms\":");
	pc_jw_i64(out, (long long)(ut * 1000ULL / (unsigned long long)tck));
	pc_jw_lit(out, ",\"cpu_sys_ms\":");
	pc_jw_i64(out, (long long)(st * 1000ULL / (unsigned long long)tck));
	pc_jw_lit(out, ",\"rss_bytes\":");
	pc_jw_i64(out, (long long)(rss_pages * (unsigned long long)pg));
	pc_jw_lit(out, "}");
	/* S123: what the running totals below count from - the start, or
	 * the last reset */
	{
		long long ra;

		pthread_mutex_lock(&reset_mx);
		ra = reset_at_s;
		pthread_mutex_unlock(&reset_mx);
		pc_jw_lit(out, ",\"since\":{\"reset_at\":");
		pc_jw_i64(out, ra);
		pc_jw_lit(out, ",\"s\":");
		pc_jw_i64(out, ra ? (long long)time(NULL) - ra
			: (long long)pc_metrics_uptime());
		pc_jw_lit(out, "}");
	}
	/* S89: the doors this daemon is serving, from the config it runs
	 * with.  The startup log says this once and is gone; "which ports is
	 * this thing actually serving" is a stats question a week later. */
	pc_jw_lit(out, ",\"listeners\":[");
	{
		int i, n = pc_listener_count();

		for (i = 0; i < n; i++) {
			const struct pc_listener *l = pc_listener_at(i);
			int plain = pc_listener_plaintext(l);
			int secret = l->http ? pc_http_token() != NULL
				: l->resp ? pc_resp_password_set() : !plain;

			if (i)
				pc_jw_lit(out, ",");
			pc_jw_lit(out, "{\"kind\":\"");
			pc_jw_lit(out, l->type == PC_LISTEN_UNIX ? "unix"
				: l->http ? "http" : l->resp ? "resp" : "native");
			pc_jw_lit(out, "\",\"addr\":");
			pc_jw_str(out, l->addr, strlen(l->addr));
			pc_jw_lit(out, ",\"port\":");
			pc_jw_i64(out, l->port);
			pc_jw_lit(out, ",\"plaintext\":");
			pc_jw_lit(out, plain ? "true" : "false");
			pc_jw_lit(out, ",\"allow\":");
			pc_jw_i64(out, pc_listener_allow(l));
			pc_jw_lit(out, ",\"secret\":");
			pc_jw_lit(out, secret ? "true" : "false");
			pc_jw_lit(out, "}");
		}
	}
	pc_jw_lit(out, "]");
}

void pc_stats_json(struct pc_jw *out, const char *only_col)
{
	int i;


	/* B1: the node's lifecycle state is a property of the NODE,
	 * not of the cluster - a node with no cluster still has one,
	 * and a readiness gate would have to answer for it too.  It
	 * sat inside the cluster block first, where an unclustered
	 * node could not report it at all. */
	pc_jw_lit(out, "{\"version\":\"" PC_VERSION "\",\"rev\":\""
		PC_BUILD_REV "\",\"state\":\"");
	pc_jw_lit(out, pc_node_state_name(pc_node_state()));
	pc_jw_lit(out, "\",\"state_reason\":\"");        /* S186 */
	pc_jw_lit(out, pc_node_reason_text(pc_node_reason()));
	pc_jw_lit(out, "\"");
	stats_process(out);
	pc_jw_lit(out, ",\"memory\":");
	{
		unsigned long mt = 0, mu = 0, mf = 0;
		int mact = 0;

		pcache_arena_hugepage_capacity(&mact, &mt, &mu, &mf);
		/* These three describe the HUGE-PAGE arena, and the
		 * accessor zeroes all of them when that arena is not
		 * the backing in use - its header says @active must be
		 * checked before any of them is trusted.  Published as
		 * plain zeros they read as "no memory left" on a node
		 * that simply never measured, which is the opposite of
		 * the truth, so they go out as null instead: a consumer
		 * can forget to check a flag, but it cannot mistake
		 * null for a quantity.  arena_held, arena_max,
		 * arena_live and headroom_pct below are backing-
		 * independent and stay meaningful either way. */
		pc_jw_lit(out, "{\"arena_capacity_valid\":");
		pc_jw_lit(out, mact ? "true" : "false");
		pc_jw_lit(out, ",\"arena_total\":");
		if (mact)
			pc_jw_i64(out, (long long)mt);
		else
			pc_jw_lit(out, "null");
		pc_jw_lit(out, ",\"arena_used\":");
		if (mact)
			pc_jw_i64(out, (long long)mu);
		else
			pc_jw_lit(out, "null");
		pc_jw_lit(out, ",\"arena_free\":");
		if (mact)
			pc_jw_i64(out, (long long)mf);
		else
			pc_jw_lit(out, "null");
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
			pc_jw_lit(out, ",\"pool_empty\":");
			pc_jw_i64(out, (long long)pr.pool_empty);
			/* S166: the groups committed ahead of the frontier, and
			 * the ones a carve had to commit on the write path */
			pc_jw_lit(out, ",\"commits_ahead\":");
			pc_jw_i64(out, (long long)pr.commits_ahead);
			pc_jw_lit(out, ",\"commits_inline\":");
			pc_jw_i64(out, (long long)pr.commits_inline);
			pc_jw_lit(out, ",\"commits_index\":");
			pc_jw_i64(out, (long long)pr.commits_index);
			/* S167: what is really locked, and whether a group
			 * could not be locked after a pinned start */
			pc_jw_lit(out, ",\"locked_bytes\":");
			pc_jw_i64(out, (long long)pr.locked_bytes);
			pc_jw_lit(out, ",\"pin_lost\":");
			pc_jw_i64(out, (long long)pr.pin_lost);
			/* S114: what held is made of - the index regions (never
			 * freed) and the free slots kept resident */
			pc_jw_lit(out, ",\"arena_regions\":");
			pc_jw_i64(out, (long long)pr.regions_bytes);
			pc_jw_lit(out, ",\"arena_warm_free\":");
			pc_jw_i64(out, (long long)pr.warm_free_bytes);
			/* S150 C: retired index slots, resident and punched, and
			 * how the carve has been served */
			pc_jw_lit(out, ",\"arena_regions_free_warm\":");
			pc_jw_i64(out, (long long)pr.regions_free_warm_bytes);
			pc_jw_lit(out, ",\"arena_regions_free_cold\":");
			pc_jw_i64(out, (long long)pr.regions_free_cold_bytes);
			pc_jw_lit(out, ",\"arena_regions_retired\":");
			pc_jw_i64(out, (long long)pr.regions_retired);
			pc_jw_lit(out, ",\"arena_region_reuse\":");
			pc_jw_i64(out, (long long)pr.region_reuse);
			/* S118: the chunks the size classes own and the shm pages'
			 * alignment slots - with the two above, held exactly */
			pc_jw_lit(out, ",\"arena_sparse_chunks\":");    /* S190 */
			pc_jw_i64(out, (long long)pr.sparse_chunks);
			pc_jw_lit(out, ",\"arena_evacuated\":");
			pc_jw_i64(out, (long long)pr.evacuated);
			pc_jw_lit(out, ",\"arena_class_chunks\":");
			pc_jw_i64(out, (long long)pr.class_chunk_bytes);
			pc_jw_lit(out, ",\"arena_page_slack\":");
			pc_jw_i64(out, (long long)pr.page_slack_bytes);
			pc_jw_lit(out, ",\"arena_committed\":");
			pc_jw_i64(out, (long long)pr.committed_bytes);
			pc_jw_lit(out, ",\"arena_reserved\":");
			pc_jw_i64(out, (long long)pr.reserved_bytes);
			pc_jw_lit(out, ",\"at_ceiling\":");
			pc_jw_lit(out, pr.at_ceiling_since ? "true" : "false");
			pc_jw_lit(out, ",\"at_ceiling_since\":");
			pc_jw_i64(out, (long long)pr.at_ceiling_since);
			pc_jw_lit(out, ",\"reclaim\":{\"retired\":");
			pc_jw_i64(out, (long long)pr.retired);
			pc_jw_lit(out, ",\"pages_freed\":");
			pc_jw_i64(out, (long long)pr.pages_freed);
			pc_jw_lit(out, ",\"released_bytes\":");
			pc_jw_i64(out, (long long)pr.released_bytes);
			/* S119: how much of that was the never-carved tail */
			pc_jw_lit(out, ",\"tail_released\":");
			pc_jw_i64(out, (long long)pr.tail_released_bytes);
			pc_jw_lit(out, ",\"cold_bytes\":");
			pc_jw_i64(out, (long long)pr.cold_bytes);
			pc_jw_lit(out, ",\"punch_calls\":");
			pc_jw_i64(out, (long long)pr.punch_calls);
			pc_jw_lit(out, ",\"punch_groups\":");
			pc_jw_i64(out, (long long)pr.punch_groups);
			pc_jw_lit(out, ",\"shrink_step_bytes\":");
			pc_jw_i64(out, (long long)pr.shrink_step);
			pc_jw_lit(out, ",\"flushes\":");
			pc_jw_i64(out, (long long)pr.flushes);
			pc_jw_lit(out, ",\"giveback_off\":");
			pc_jw_lit(out, pr.giveback_off ? "true" : "false");
			pc_jw_lit(out, "}");
		}
		pc_jw_lit(out, "}");
	}
	/* S120: the budget - what the collections cost, from what the daemon
	 * knows exactly: the index regions each table was carved (noted at
	 * creation and growth) and the records as the cells they occupy (the
	 * walk rounds every record to its class), beside the ceiling and the
	 * reservation.  The README's sizing recipe is the same arithmetic. */
	{
		unsigned long long bi = 0, br = 0;
		unsigned long bt = 0, bu = 0, bf = 0;
		int c, nc = pc_store_count(), ba = 0;

		for (c = 0; c < nc; c++) {
			bi += pc_store_index_bytes(c);
			br += pc_store_held_cells(c);
		}
		pcache_arena_hugepage_capacity(&ba, &bt, &bu, &bf);
		pc_jw_lit(out, ",\"budget\":{\"index\":");
		pc_jw_i64(out, (long long)bi);
		pc_jw_lit(out, ",\"records\":");
		pc_jw_i64(out, (long long)br);
		pc_jw_lit(out, ",\"held\":");
		pc_jw_i64(out, (long long)pcache_arena_held_bytes());
		pc_jw_lit(out, ",\"ceiling\":");
		pc_jw_i64(out, (long long)pcache_arena_max_bytes);
		pc_jw_lit(out, ",\"reservation\":");
		pc_jw_i64(out, (long long)bt);
		pc_jw_lit(out, "}");
	}
	/* RESP listeners (S33) live OUTSIDE the cluster block: a
	 * RESP listener works on a standalone daemon, and burying
	 * its counters in "cluster" made them null exactly where a
	 * single-node operator would look for them. */
	/* S161: the client limit and what it refused; HTTP is outside it */
	pc_jw_lit(out, ",\"clients\":{\"open\":");
	pc_jw_i64(out, (long long)PC_RESP_READ(pc_clients_open));
	pc_jw_lit(out, ",\"max\":");
	pc_jw_i64(out, pc_max_clients);
	pc_jw_lit(out, ",\"refused\":");
	pc_jw_i64(out, (long long)__atomic_load_n(&pc_clients_refused,
		__ATOMIC_RELAXED));
	pc_jw_lit(out, "}");
	{
		struct pc_pubsub_stats ps;   /* PS1/PS3 */

		pc_pubsub_stats(&ps);
		pc_jw_lit(out, ",\"pubsub\":{\"channels\":");
		pc_jw_i64(out, ps.channels);
		pc_jw_lit(out, ",\"patterns\":");
		pc_jw_i64(out, ps.patterns);
		pc_jw_lit(out, ",\"subscribers\":");
		pc_jw_i64(out, ps.subscribers);
		pc_jw_lit(out, ",\"published\":");
		pc_jw_i64(out, (long long)ps.published);
		pc_jw_lit(out, ",\"delivered\":");
		pc_jw_i64(out, (long long)ps.delivered);
		pc_jw_lit(out, ",\"slow_kills\":");
		pc_jw_i64(out, (long long)ps.slow_kills);
		pc_jw_lit(out, ",\"relay_sent\":");
		pc_jw_i64(out, (long long)ps.relay_sent);
		pc_jw_lit(out, ",\"relay_recv\":");
		pc_jw_i64(out, (long long)ps.relay_recv);
		pc_jw_lit(out, ",\"relay_lost\":");
		pc_jw_i64(out, (long long)ps.relay_lost);
		pc_jw_lit(out, ",\"relay_dropped\":");
		pc_jw_i64(out, (long long)ps.relay_dropped);
		pc_jw_lit(out, ",\"alloc_failed\":");
		pc_jw_i64(out, (long long)ps.alloc_failed);
		pc_jw_lit(out, ",\"relay_duplicates\":");
		pc_jw_i64(out, (long long)ps.relay_duplicates);
		pc_jw_lit(out, ",\"queue_dropped\":");
		pc_jw_i64(out, (long long)ps.queue_dropped);
		pc_jw_lit(out, ",\"queue_bytes\":");
		pc_jw_i64(out, (long long)ps.queue_bytes);
		pc_jw_lit(out, ",\"publish_paused\":");
		pc_jw_i64(out, (long long)ps.publish_paused);
		{
			/* PS5: pushes over UDP */
			struct pc_udp_figures uf;

			pc_conn_udp_figures(&uf);
			pc_jw_lit(out, ",\"udp\":{\"streams\":");
			pc_jw_i64(out, uf.streams);
			pc_jw_lit(out, ",\"probes\":");
			pc_jw_i64(out, (long long)uf.probes);
			pc_jw_lit(out, ",\"confirmed\":");
			pc_jw_i64(out, (long long)uf.confirmed);
			pc_jw_lit(out, ",\"expired\":");
			pc_jw_i64(out, (long long)uf.expired);
			pc_jw_lit(out, ",\"pushed\":");
			pc_jw_i64(out, (long long)uf.pushed);
			pc_jw_lit(out, ",\"oversized_to_tcp\":");
			pc_jw_i64(out, (long long)uf.oversized);
			pc_jw_lit(out, ",\"send_errors\":");
			pc_jw_i64(out, (long long)uf.send_errors);
			pc_jw_lit(out, ",\"acks\":");
			pc_jw_i64(out, (long long)uf.acks);
			pc_jw_lit(out, ",\"pruned_no_ack\":");
			pc_jw_i64(out, (long long)uf.pruned_no_ack);
			pc_jw_lit(out, ",\"pruned_no_progress\":");
			pc_jw_i64(out, (long long)uf.pruned_no_progress);
			pc_jw_lit(out, "}");
		}
		{
			/* PS11: the dedicated relay plane */
			int rport, rthreads, rdirect;
			unsigned long long sd, sc, rx;

			pc_cluster_pubsub_relay_figures(&rport, &rthreads, &rdirect,
				&sd, &sc, &rx);
			pc_jw_lit(out, ",\"relay_port\":");
			pc_jw_i64(out, rport);
			pc_jw_lit(out, ",\"relay_rx_threads\":");
			pc_jw_i64(out, rthreads);
			pc_jw_lit(out, ",\"relay_peers_direct\":");
			pc_jw_i64(out, rdirect);
			pc_jw_lit(out, ",\"relay_sent_direct\":");
			pc_jw_i64(out, (long long)sd);
			pc_jw_lit(out, ",\"relay_sent_cluster\":");
			pc_jw_i64(out, (long long)sc);
			pc_jw_lit(out, ",\"relay_rx_datagrams\":");
			pc_jw_i64(out, (long long)rx);
		}
		{
			/* PS12: relaying only what a peer wants */
			struct pc_psint_figures fi;

			pc_cluster_pubsub_interest_figures(&fi);
			pc_jw_lit(out, fi.version ? ",\"relay_mode\":\"interested\""
				: ",\"relay_mode\":\"all\"");
			{
				/* hex: [epoch 4][rebuild 4][adds 8] read off
				 * directly, and no sign to lose */
				char hx[20];
				int hn = snprintf(hx, sizeof hx, "%016llx",
					(unsigned long long)fi.version);

				pc_jw_lit(out, ",\"interest_version\":");
				pc_jw_str(out, hx, (size_t)hn);
			}
			pc_jw_lit(out, ",\"relay_skipped\":");
			pc_jw_i64(out, (long long)fi.skipped);
			pc_jw_lit(out, ",\"relay_peers_filtered\":");
			pc_jw_i64(out, fi.peers_filtered);
			pc_jw_lit(out, ",\"relay_peers_broadcast\":");
			pc_jw_i64(out, fi.peers_broadcast);
			pc_jw_lit(out, ",\"interest_updates_sent\":");
			pc_jw_i64(out, (long long)fi.updates_sent);
			pc_jw_lit(out, ",\"interest_updates_received\":");
			pc_jw_i64(out, (long long)fi.updates_received);
			pc_jw_lit(out, ",\"interest_resyncs_sent\":");
			pc_jw_i64(out, (long long)fi.resyncs_sent);
			pc_jw_lit(out, ",\"interest_resyncs_received\":");
			pc_jw_i64(out, (long long)fi.resyncs_received);
			pc_jw_lit(out, ",\"interest_requests_sent\":");
			pc_jw_i64(out, (long long)fi.requests_sent);
		}
		pc_jw_lit(out, ",\"keyspace_events\":");
		pc_jw_i64(out, (long long)ps.keyspace);
		pc_jw_lit(out, "}");
	}
	/* S159: the command rows and the slow log, off the RESP door - what
	 * INFO commandstats / latencystats and SLOWLOG GET answer there */
	pc_jw_lit(out, ",\"commands\":");
	pc_obs_commands_json(out);
	pc_jw_lit(out, ",\"slowlog\":");
	pc_obs_slowlog_json(out, 32);
	unknown_json(out);                     /* S165 */
	pc_jw_lit(out, ",\"resp\":{\"conns\":");
	pc_jw_i64(out, PC_DOOR_SINCE(resp_conns));
	pc_jw_lit(out, ",\"open\":");
	pc_jw_i64(out, (long long)PC_RESP_READ(pc_resp_open));
	pc_jw_lit(out, ",\"rejected\":");
	pc_jw_i64(out, PC_DOOR_SINCE(resp_rejected));
	pc_jw_lit(out, ",\"authfail\":");
	pc_jw_i64(out, PC_DOOR_SINCE(resp_authfail));
	/* S129: failed `enable` attempts.  Beside authfail because it is
	 * the same question one layer up - somebody offering a secret that
	 * did not match - and an operator watching for a privilege secret
	 * being guessed looks here. */
	pc_jw_lit(out, ",\"enable_fails\":");
	pc_jw_i64(out, (long long)pc_verb_enable_fails());
	pc_jw_lit(out, ",\"requests\":");
	pc_jw_i64(out, PC_DOOR_SINCE(resp_reqs));
	pc_jw_lit(out, ",\"slots_hits\":");
	pc_jw_i64(out, PC_DOOR_SINCE(resp_slots_hits));
	pc_jw_lit(out, ",\"slots_builds\":");
	pc_jw_i64(out, PC_DOOR_SINCE(resp_slots_builds));
	pc_jw_lit(out, "}");
	/* S76: the native door, per dialect.  A connection is counted once
	 * when its first byte settles the dialect; every consumed request
	 * after that counts against it.  resp here is RESP spoken to the
	 * native port - the dedicated RESP door is the block above. */
	pc_jw_lit(out, ",\"native\":{\"conns\":");
	pc_jw_i64(out, PC_DOOR_SINCE(nat_conns));
	pc_jw_lit(out, ",\"json\":{\"conns\":");
	pc_jw_i64(out, PC_DOOR_SINCE(nat_text_conns));
	pc_jw_lit(out, ",\"open\":");
	pc_jw_i64(out, (long long)PC_RESP_READ(pc_nat_text_open));
	pc_jw_lit(out, ",\"requests\":");
	pc_jw_i64(out, PC_DOOR_SINCE(nat_text_reqs));
	pc_jw_lit(out, "},\"binary\":{\"conns\":");
	pc_jw_i64(out, PC_DOOR_SINCE(nat_bin_conns));
	pc_jw_lit(out, ",\"open\":");
	pc_jw_i64(out, (long long)PC_RESP_READ(pc_nat_bin_open));
	pc_jw_lit(out, ",\"requests\":");
	pc_jw_i64(out, PC_DOOR_SINCE(nat_bin_reqs));
	pc_jw_lit(out, "},\"resp\":{\"conns\":");
	pc_jw_i64(out, PC_DOOR_SINCE(nat_resp_conns));
	pc_jw_lit(out, ",\"open\":");
	pc_jw_i64(out, (long long)PC_RESP_READ(pc_nat_resp_open));
	pc_jw_lit(out, ",\"requests\":");
	pc_jw_i64(out, PC_DOOR_SINCE(nat_resp_reqs));
	pc_jw_lit(out, "}}");
	pc_jw_lit(out, ",\"collections\":[");
	for (i = 0; i < pc_store_count(); i++) {
		if (!pc_store_live(i))
			continue;   /* S69: a dropped collection */
		pcache_ht_totals_t tot;

		if (only_col && strcmp(only_col, pc_store_name(i)))
			continue;
		pcache_ht_totals(pc_store_ht(i), &tot);
		if (out->len && out->buf[out->len - 1] == '}')
			pc_jw_lit(out, ",");
		pc_jw_lit(out, "{\"name\":");
		pc_jw_str(out, pc_store_name(i), strlen(pc_store_name(i)));
		{
			/* S163: the name's hash, which is how the map's per-member
			 * collection figures are joined to this row */
			char hx[24];

			snprintf(hx, sizeof hx, "%016llx", (unsigned long long)
				clmemb_col_hash(pc_store_name(i)));
			pc_jw_lit(out, ",\"hash\":\"");
			pc_jw_lit(out, hx);
			pc_jw_lit(out, "\"");
		}
		/* the mode is the first thing anyone needs to know
		 * about a collection - and until now the only way to
		 * find it was to read the node's config file */
		/* eager is legal only alongside store, so it is not a
		 * property hanging off the mode - it IS the mode, and
		 * reporting "store" for an eager collection drops the
		 * half an operator most needs. */
		pc_jw_lit(out, ",\"mode\":\"");
		/* S127: spread SETS the eager flag, because it reuses eager's
		 * push machinery aimed at K holders - so a spread collection
		 * reported "eager" here and the dashboard, which derives its
		 * label from this, told an operator "all in eager mode" about
		 * a fleet that is not.  Ask the cluster first. */
		pc_jw_lit(out, pc_cluster_replicas() ? "spread"
			: pc_store_shard_enabled(pc_store_ht(i))
			? "shard" : pc_store_proxy_enabled(pc_store_ht(i))
			? "proxy" : pc_store_eager_enabled(pc_store_ht(i))
			? "eager" : "store");
		pc_jw_lit(out, "\"");
		pc_jw_lit(out, ",\"entries\":");
		pc_jw_i64(out, (long long)tot.entries);
		pc_jw_lit(out, ",\"buckets\":");
		pc_jw_i64(out, pcache_ht_nbuckets(pc_store_ht(i)));
		/* S131: records in the overflow leg, and what the last held
		 * walk cost.  A table at its target load factor keeps the leg
		 * near empty; one that has stopped growing puts everything
		 * there, on a chain per hash bucket under ONE lock - and the
		 * walk that crosses it blocks every other maintenance duty
		 * for its whole duration, the splitter included.  Neither
		 * figure was reported anywhere, so a table at 81 entries per
		 * bucket looked like a table at 4. */
		/* S180: slots per bucket, so a reader can work out the LOAD
		 * FACTOR - entries against buckets x slots.  The status page
		 * divided by buckets alone and reported a table at half its
		 * capacity as "300%", which is the opposite of the signal an
		 * operator needs from that column. */
		pc_jw_lit(out, ",\"slots\":");
		pc_jw_i64(out, PCACHE_SLOTS);
		pc_jw_lit(out, ",\"overflow\":");
		pc_jw_i64(out, pcache_ht_overflow(pc_store_ht(i)));
		/* S172: what the maintenance tick has put BACK into the
		 * table - a leg that stays flat while this climbs is a
		 * table still filling it faster than the drain empties it */
		pc_jw_lit(out, ",\"leg_drained\":");
		pc_jw_i64(out, (long long)pcache_ht_drained(pc_store_ht(i)));
		/* S175: what the last COMPLETE drain pass could not put back
		 * - their buckets are full, so only a wider table can take
		 * them - and the splits that observation has asked for.
		 * leg_stuck falling to nothing while leg_splits climbs is
		 * the signal doing its job; leg_stuck holding steady with
		 * leg_splits flat means the floor has been reached and the
		 * remainder is the load factor the operator chose. */
		pc_jw_lit(out, ",\"leg_stuck\":");
		pc_jw_i64(out, pcache_ht_leg_stuck(pc_store_ht(i)));
		pc_jw_lit(out, ",\"leg_splits\":");
		pc_jw_i64(out, (long long)pcache_ht_leg_splits(pc_store_ht(i)));
		pc_jw_lit(out, ",\"held_walk_us\":");
		pc_jw_i64(out, (long long)pc_store_held_walk_us(i));
		{
			int rs_t = 0;
			unsigned long long rs_m = 0;

			if (pc_store_resizing(i, &rs_t, &rs_m)) {   /* S69 */
				pc_jw_lit(out, ",\"resizing_to\":");
				pc_jw_i64(out, 1LL << rs_t);
				pc_jw_lit(out, ",\"resize_moved\":");
				pc_jw_i64(out, (long long)rs_m);
			}
		}
		pc_jw_lit(out, ",\"hits\":");
		pc_jw_i64(out, (long long)tot.hits);
		pc_jw_lit(out, ",\"misses\":");
		pc_jw_i64(out, (long long)tot.misses);
		/* S148: hits/misses count LOOKUPS, so one hot key pulled hard
		 * reads as a perfect cache beside thousands of records nobody
		 * wants.  `reach` is how many DISTINCT keys were actually
		 * served in the last closed window - read against `entries`,
		 * it is the half the ratio cannot express.  Approximate by
		 * design (a 64-register HLL, ~13%); the window is the
		 * daemon's, because a sketch cannot be differenced the way
		 * the page differences counters. */
		{
			unsigned int rw = 0;
			unsigned long rk = pcache_ht_reach(pc_store_ht(i), &rw);

			pc_jw_lit(out, ",\"reach\":");
			pc_jw_i64(out, (long long)rk);
			pc_jw_lit(out, ",\"reach_window_s\":");
			pc_jw_i64(out, (long long)rw);
		}
		/* S151: the same three over CLIENT worker slots only.  hits,
		 * misses and reach above count every origin: a peer serving a
		 * pull lands there (12du), and so does recovery's probe of each
		 * replayed key.  These are what an operator means by the words,
		 * and what the status page now reads. */
		{
			pcache_ht_totals_t ct;
			unsigned int rw2 = 0;
			unsigned long rc2 = pcache_ht_reach_client(pc_store_ht(i), &rw2);

			pcache_ht_totals_client(pc_store_ht(i), &ct);
			pc_jw_lit(out, ",\"hits_client\":");
			pc_jw_i64(out, (long long)ct.hits);
			pc_jw_lit(out, ",\"misses_client\":");
			pc_jw_i64(out, (long long)ct.misses);
			pc_jw_lit(out, ",\"reach_client\":");
			pc_jw_i64(out, (long long)rc2);
			/* S164: a replicated apply, a kept pull and a recovery
			 * replay all store, so `stores` puts one eager client
			 * write on every member.  These are the writes clients
			 * made HERE - this node's share of the fleet figure. */
			pc_jw_lit(out, ",\"stores_client\":");
			pc_jw_i64(out, (long long)ct.stores);
			pc_jw_lit(out, ",\"removes_client\":");
			pc_jw_i64(out, (long long)ct.removes);
		}
		pc_jw_lit(out, ",\"stores\":");
		pc_jw_i64(out, (long long)tot.stores);
		pc_jw_lit(out, ",\"removes\":");
		pc_jw_i64(out, (long long)tot.removes);
		pc_jw_lit(out, ",\"expired\":");
		pc_jw_i64(out, (long long)tot.expired);
		/* S164: the collection for the whole fleet, computed here so the
		 * page, perfcli and any scraper read one answer.  Held copies
		 * follow the mode (eager: the fullest member; spread: the copies
		 * over K; store: the fullest member as a lower bound; shard and
		 * proxy: the sum); client events sum.  Absent unclustered. */
		{
			struct pc_col_fleet fl;
			pcache_htable_t *fh = pc_store_ht(i);
			int basis = pc_cluster_replicas() ? PC_FLEET_PER_K
				: pc_store_shard_enabled(fh) || pc_store_proxy_enabled(fh)
				? PC_FLEET_SUM
				: pc_store_eager_enabled(fh) ? PC_FLEET_FULLEST
				: PC_FLEET_AT_LEAST;

			if (pc_cluster_col_fleet(i, basis, &fl)) {
				pc_jw_lit(out, ",\"fleet\":{\"basis\":\"");
				pc_jw_lit(out, pc_fleet_basis_name(fl.basis));
				pc_jw_lit(out, "\",\"members\":");
				pc_jw_i64(out, fl.members);
				pc_jw_lit(out, ",\"reporting\":");
				pc_jw_i64(out, fl.reporting);
				pc_jw_lit(out, ",\"entries\":");
				pc_jw_i64(out, (long long)fl.entries);
				pc_jw_lit(out, ",\"copies\":");
				pc_jw_i64(out, (long long)fl.copies);
				pc_jw_lit(out, ",\"expired\":");
				pc_jw_i64(out, (long long)fl.expired);
				pc_jw_lit(out, ",\"hits\":");
				pc_jw_i64(out, (long long)fl.hits);
				pc_jw_lit(out, ",\"misses\":");
				pc_jw_i64(out, (long long)fl.misses);
				pc_jw_lit(out, ",\"stores\":");
				pc_jw_i64(out, (long long)fl.stores);
				pc_jw_lit(out, ",\"removes\":");
				pc_jw_i64(out, (long long)fl.removes);
				pc_jw_lit(out, "}");
			}
		}
		/* S109: what the collection holds - key + value bytes of every
		 * record, from the maintenance thread's paced walk (store.c), so
		 * a node whose records all arrived by replication shows its real
		 * size.  held_age_s = how old the figure is; -1 before the first
		 * walk (a page shows a dash then, not a zero). */
		{
			unsigned int age = 0;
			unsigned long long hb = pc_store_held(i, &age);

			pc_jw_lit(out, ",\"held_bytes\":");
			pc_jw_i64(out, (long long)hb);
			pc_jw_lit(out, ",\"held_age_s\":");
			pc_jw_i64(out, age == (unsigned int)-1 ? -1 : (long long)age);
		}
		/* S116: the value sizes HELD, from the same walk - the same on
		 * every member whatever path stored the record, unlike size_hist
		 * below, which counts this node's own client writes.  Zeros
		 * before the first walk; the page keys on held_age_s. */
		{
			unsigned long long hh[8] = { 0 };
			int c;

			(void)pc_store_held_hist(i, hh);
			pc_jw_lit(out, ",\"held_hist\":[");
			for (c = 0; c < 8; c++) {
				if (c)
					pc_jw_lit(out, ",");
				pc_jw_i64(out, (long long)hh[c]);
			}
			pc_jw_lit(out, "]");
		}
		/* S120: what this collection costs - its index regions, exact,
		 * and its records as the cells they occupy, from the walk */
		pc_jw_lit(out, ",\"index_bytes\":");
		pc_jw_i64(out, (long long)pc_store_index_bytes(i));
		pc_jw_lit(out, ",\"held_cells\":");
		pc_jw_i64(out, (long long)pc_store_held_cells(i));
		/* S67: the write stream's sizes - a mean the page turns into an
		 * estimate of memory held, and a log2 distribution */
		{
			unsigned long long sb = 0, sn = 0, sh[8] = { 0 };
			int c;

			pc_store_size_stats(i, &sb, &sn, sh);
			pc_jw_lit(out, ",\"stored_bytes\":");
			pc_jw_i64(out, (long long)sb);
			pc_jw_lit(out, ",\"stored_n\":");
			pc_jw_i64(out, (long long)sn);
			pc_jw_lit(out, ",\"size_hist\":[");
			for (c = 0; c < 8; c++) {
				if (c)
					pc_jw_lit(out, ",");
				pc_jw_i64(out, (long long)sh[c]);
			}
			pc_jw_lit(out, "]");
		}
		pc_jw_lit(out, "}");
	}
	pc_jw_lit(out, "],\"cluster\":");
	{
		struct pc_cl_stats cs;

		pc_cluster_get_stats(&cs);
		since_cl(&cs);
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
			/* S80: where that identity lives, and whether that
			 * place survives a reboot */
			pc_jw_lit(out, ",\"state_dir\":");
			if (pc_cluster_state_dir())
				pc_jw_str(out, pc_cluster_state_dir(),
					strlen(pc_cluster_state_dir()));
			else
				pc_jw_lit(out, "null");
			pc_jw_lit(out, ",\"state_on_tmpfs\":");
			pc_jw_lit(out, pc_cluster_state_tmpfs()
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
			/* S212: the clocks - durations of the last completed
			 * pass, not rates */
			pc_jw_lit(out, ",\"reconcile_ms\":");
			pc_jw_i64(out, (long long)cs.reconcile_ms);
			pc_jw_lit(out, ",\"reconcile_pending\":");
			pc_jw_i64(out, (long long)cs.reconcile_pending);
			pc_jw_lit(out, ",\"repl_sweep_ms\":");
			pc_jw_i64(out, (long long)cs.repl_sweep_ms);
			pc_jw_lit(out, ",\"repl_sweep_scanned\":");
			pc_jw_i64(out, (long long)cs.repl_sweep_scanned);
			pc_jw_lit(out, ",\"repl_sweep_sent\":");
			pc_jw_i64(out, (long long)cs.repl_sweep_sent);
			pc_jw_lit(out, ",\"repl_sweep_cycles\":");
			pc_jw_i64(out, (long long)cs.repl_sweep_cycles);
			pc_jw_lit(out, ",\"reconcile_deferred\":");
			pc_jw_i64(out, (long long)cs.reconcile_deferred);
			pc_jw_lit(out, ",\"reconcile_witness\":");   /* S223 */
			pc_jw_i64(out, cs.reconcile_witness);
			pc_jw_lit(out, ",\"reconcile_skipped\":");
			pc_jw_lit(out, cs.reconcile_skipped ? "true" : "false");
			pc_jw_lit(out, ",\"term\":");
			pc_jw_i64(out, (long long)pc_term_current());
			pc_jw_lit(out, ",\"term_rejected\":");
			pc_jw_i64(out, (long long)pc_term_rejected);
			pc_jw_lit(out, ",\"eager\":");
			pc_jw_lit(out, pc_cluster_eager() ? "true" : "false");
			pc_jw_lit(out, ",\"mode\":\"");
			pc_jw_lit(out, pc_cluster_mode_name());
			pc_jw_lit(out, "\"");
			pc_jw_lit(out, ",\"role\":\"");
			pc_jw_lit(out, cs.role == 2 ? "master" :
				cs.role == 1 ? "member" : "joining");
			pc_jw_lit(out, "\",\"master\":");
			pc_jw_i64(out, cs.master_id);
			pc_jw_lit(out, ",\"peers_up\":");
			pc_jw_i64(out, cs.peers_up);
			pc_jw_lit(out, ",\"replicas_short\":");      /* S157 */
			pc_jw_i64(out, cs.replicas_short);
			/* S218: counted since S26 and shown nowhere - a master
			 * change is the event an operator most wants dated */
			pc_jw_lit(out, ",\"elections\":");
			pc_jw_i64(out, (long long)cs.elections);
			pc_jw_lit(out, ",\"demotions\":");
			pc_jw_i64(out, (long long)cs.demotions);
			pc_jw_lit(out, ",\"wire_epoch\":");          /* S221 */
			pc_jw_i64(out, PC_CL_EPOCH);
			pc_jw_lit(out, ",\"epoch_refused\":");
			pc_jw_i64(out, (long long)cs.epoch_refused);
			pc_jw_lit(out, ",\"apply_threads\":");       /* S217 */
			pc_jw_i64(out, cs.apply_threads ? cs.apply_threads : 1);
			pc_jw_lit(out, ",\"apply_dispatched\":");
			pc_jw_i64(out, (long long)cs.apply_dispatched);
			pc_jw_lit(out, ",\"apply_blocks\":");
			pc_jw_i64(out, (long long)cs.apply_blocks);
			pc_jw_lit(out, ",\"apply_backlog\":");
			pc_jw_i64(out, (long long)cs.apply_backlog);
			pc_jw_lit(out, ",\"stalled\":");             /* S216 */
			pc_jw_lit(out, cs.stalled ? "true" : "false");
			pc_jw_lit(out, ",\"stalled_for_ms\":");
			pc_jw_i64(out, cs.stalled_for_ms);
			pc_jw_lit(out, ",\"apply_stalls\":");
			pc_jw_i64(out, (long long)cs.apply_stalls);
			pc_jw_lit(out, ",\"apply_stall_ms_max\":");
			pc_jw_i64(out, (long long)cs.apply_stall_ms_max);
			/* WHICH MACHINE this is.  The node id is a per-fleet
			 * handle and gets reused; the identity is the thing
			 * that tells two claimants apart, and its UUID version
			 * says whether it was derived from the platform (8) or
			 * randomly minted (7). */
			{
				char uu[37];
				int uv = 0;

				pc_cluster_identity_uuid(uu, &uv);
				/* identity_UUID, not identity: "identity" is
				 * already published above as 32 hex and things
				 * read it (statedirtest among them).  Emitting
				 * a second key of the same name gave the object
				 * a duplicate, and a parser takes the LAST -
				 * silently changing the format under every
				 * existing consumer. */
				pc_jw_lit(out, ",\"identity_uuid\":\"");
				pc_jw_lit(out, uu);
				pc_jw_lit(out, "\",\"identity_ver\":");
				pc_jw_i64(out, uv);
			}
			/* every peer's identity, so a dashboard can show which
			 * machine holds each id */
			{
				struct pc_cl_peer_info pi[PC_CL_MAXPEER];
				int np = pc_cluster_peers(pi, PC_CL_MAXPEER), k;

				pc_jw_lit(out, ",\"peers\":[");
				for (k = 0; k < np; k++) {
					if (k)
						pc_jw_lit(out, ",");
					pc_jw_lit(out, "{\"node\":");
					pc_jw_i64(out, pi[k].node);
					pc_jw_lit(out, ",\"up\":");
					pc_jw_lit(out, pi[k].up ? "true" : "false");
					pc_jw_lit(out, ",\"identity\":\"");
					pc_jw_lit(out, pi[k].ident);
					pc_jw_lit(out, "\",\"identity_ver\":");
					pc_jw_i64(out, pi[k].ident_ver);
					pc_jw_lit(out, ",\"free_mb\":");
					pc_jw_i64(out, pi[k].free_mb);
					pc_jw_lit(out, "}");
				}
				pc_jw_lit(out, "]");
			}
			pc_jw_lit(out, ",\"reserved_ids\":");
			pc_jw_i64(out, cs.reserved_ids);
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
				since_px(&px);
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
				pc_jw_lit(out, ",\"migrate_retx\":");
				pc_jw_i64(out, (long long)px.migrate_retx);
				pc_jw_lit(out, ",\"migrate_dgrams\":");
				pc_jw_i64(out, (long long)px.migrate_dgrams);
				pc_jw_lit(out, ",\"repl_out\":");
			pc_jw_i64(out, (long long)px.repl_out);
			/* S127: what spread is doing, in two numbers an
			 * operator can act on.  not_held = writes accepted,
			 * forwarded and NOT kept, because placement leaves
			 * this node out of the set; repaired = times the
			 * holder set changed and a re-placement was armed
			 * (one per membership change, not per record). */
			pc_jw_lit(out, ",\"spread_not_held\":");
			pc_jw_i64(out, (long long)px.spread_not_held);
			pc_jw_lit(out, ",\"spread_repaired\":");
			pc_jw_i64(out, (long long)px.spread_repaired);
			pc_jw_lit(out, ",\"spread_reclaimed\":");
			pc_jw_i64(out, (long long)px.spread_reclaimed);
			pc_jw_lit(out, ",\"spread_possess_sent\":");
			pc_jw_i64(out, (long long)px.spread_possess_sent);
			pc_jw_lit(out, ",\"spread_possess_kept\":");
			pc_jw_i64(out, (long long)px.spread_possess_kept);
			/* Two counters that were maintained and reset-baselined
			 * but NEVER emitted - found by statlint, not by anyone
			 * looking.  fwd_fails is a FAILURE counter: forwards
			 * that could not be sent have been invisible to every
			 * operator for as long as it has existed. */
			pc_jw_lit(out, ",\"fwd_fails\":");
			pc_jw_i64(out, (long long)px.fwd_fails);
			pc_jw_lit(out, ",\"migrate_skipped_big\":");
			pc_jw_i64(out, (long long)px.migrate_skipped_big);
			pc_jw_lit(out, ",\"repl_pushed\":");
			pc_jw_i64(out, (long long)px.repl_pushed);
			pc_jw_lit(out, ",\"repl_groups\":");   /* S105 */
			pc_jw_i64(out, (long long)px.repl_groups);
			pc_jw_lit(out, ",\"repl_skipped_dying\":");
			pc_jw_i64(out, (long long)px.repl_skipped_dying);
				/* A2: copies refused as not-newer.  A
				 * number that only climbs means a
				 * sender is looping on records nobody
				 * will take. */
				pc_jw_lit(out, ",\"recv_older\":");
				pc_jw_i64(out, (long long)px.recv_older);
				/* S209: a copy refused because a tombstone for
				 * the key carries a newer version - the set the
				 * delete superseded, arriving after it.  Each is a
				 * resurrection that did not happen. */
				pc_jw_lit(out, ",\"recv_tombstoned\":");
				pc_jw_i64(out, (long long)px.recv_tombstoned);
				/* S174: oversized copies ride the bulk
				 * plane; a batch whose outcome did not
				 * confirm holds this peer's sweep mark
				 * where it is, so the next cycle offers
				 * them again.  Climbing means the plane
				 * is failing, not that records are lost. */
				pc_jw_lit(out, ",\"repl_bulk_lost\":");
				pc_jw_i64(out, (long long)px.repl_bulk_lost);
				/* S125: how far behind this node's applier
				 * is.  A node that cannot keep up still
				 * answers its heartbeat and still serves
				 * its client door quickly, so nothing else
				 * here says it - the only other symptom is
				 * a read of a key that exists solely as an
				 * unapplied replica.  rx_queue against
				 * rx_rcvbuf is the buffer filling; a
				 * nonzero rx_drops_ps is data already lost
				 * and now waiting on the repair sweep. */
				pc_jw_lit(out, ",\"rx_applied\":");
				pc_jw_i64(out, (long long)cs.rx_applied);
				pc_jw_lit(out, ",\"rx_applied_ps\":");
				pc_jw_i64(out, (long long)cs.rx_applied_ps);
				pc_jw_lit(out, ",\"rx_older_ps\":");
				pc_jw_i64(out, (long long)cs.rx_older_ps);
				pc_jw_lit(out, ",\"rx_drops\":");
				pc_jw_i64(out, (long long)cs.rx_drops);
				pc_jw_lit(out, ",\"rx_drops_ps\":");
				pc_jw_i64(out, (long long)cs.rx_drops_ps);
				pc_jw_lit(out, ",\"rx_queue\":");
				pc_jw_i64(out, (long long)cs.rx_queue);
				pc_jw_lit(out, ",\"rx_rcvbuf\":");
				pc_jw_i64(out, (long long)cs.rx_rcvbuf);
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
				/* S83: the bootstrap pull */
				pc_jw_lit(out, ",\"boot_out\":");
				pc_jw_i64(out, (long long)px.boot_out);
				pc_jw_lit(out, ",\"boot_in\":");
				pc_jw_i64(out, (long long)px.boot_in);
				pc_jw_lit(out, ",\"boot_failed\":");
				pc_jw_i64(out, (long long)px.boot_failed);
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
			pc_jw_lit(out, ",\"safe_marker\":");      /* S215 */
			pc_jw_i64(out, (long long)rs.safe_marker);
			pc_jw_lit(out, ",\"save_errors\":");
			pc_jw_i64(out, (long long)rs.save_errors);
			pc_jw_lit(out, ",\"last_unix\":");
			pc_jw_i64(out, rs.last_unix);
			pc_jw_lit(out, ",\"last_age_s\":");    /* RV-12 */
			pc_jw_i64(out, rs.last_age_s);
			pc_jw_lit(out, "}");
		}
	}
	pc_jw_lit(out, ",\"wal\":");
	{
		struct pc_wal_stats ws;

		pc_wal_get_stats(&ws);
		since_wal(&ws);
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
					pc_jw_i64(out, (long long)ob.fsync_n);
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
			pc_jw_lit(out, ",\"storage_failed\":");   /* S215 */
			pc_jw_lit(out, ws.failed ? "true" : "false");
			pc_jw_lit(out, ",\"ctrl_errors\":");
			pc_jw_i64(out, (long long)ws.ctrl_errors);
			pc_jw_lit(out, ",\"overruns\":");
			pc_jw_i64(out, (long long)ws.overruns);
			pc_jw_lit(out, ",\"free_segments\":");
			pc_jw_i64(out, ws.free_segments);
			{
				/* S224: tenths are plenty, and the JSON writer
				 * has no float - print it once, raw */
				char fb[64];
				int fl = snprintf(fb, sizeof fb,
					",\"fill_mb_s\":%.1f,\"full_in_s\":%.1f",
					ws.fill_mb_s, ws.full_in_s);

				if (fl > 0 && (size_t)fl < sizeof fb)
					pc_jw_raw(out, fb, (size_t)fl);
			}
			pc_jw_lit(out, ",\"last_seq\":");
			pc_jw_i64(out, (long long)ws.last_seq);
			pc_jw_lit(out, ",\"synced_seq\":");
			pc_jw_i64(out, (long long)ws.synced_seq);
			/* S145: the two states one "unsynced" figure used to span */
			pc_jw_lit(out, ",\"appended_seq\":");
			pc_jw_i64(out, (long long)ws.pending_hi);
			pc_jw_lit(out, ",\"staged\":");
			pc_jw_i64(out, (long long)(ws.last_seq > ws.pending_hi ? ws.last_seq - ws.pending_hi : 0));
			pc_jw_lit(out, ",\"unsynced\":");
			pc_jw_i64(out, (long long)(ws.pending_hi > ws.synced_seq ? ws.pending_hi - ws.synced_seq : 0));
			pc_jw_lit(out, "}");
		}
	}
	/* S150 B: tables a resize left behind, waiting for every thread
	 * that could hold a pointer into them to park; the walks that ended
	 * because their table moved; and the quiescence lines themselves */
	{
		unsigned int rp, lines, inside;
		unsigned long long rc, rb;

		pc_store_retire_figures(&rp, &rc, &rb);
		pc_qs_figures(&lines, &inside);
		pc_jw_lit(out, ",\"retire\":{\"pending\":");
		pc_jw_i64(out, (long long)rp);
		pc_jw_lit(out, ",\"cleared\":");
		pc_jw_i64(out, (long long)rc);
		pc_jw_lit(out, ",\"returned_bytes\":");
		pc_jw_i64(out, (long long)rb);
		pc_jw_lit(out, ",\"keys_ended_on_swap\":");
		pc_jw_i64(out, (long long)pc_enum_ended_on_swap());
		pc_jw_lit(out, ",\"lines\":");
		pc_jw_i64(out, (long long)lines);
		pc_jw_lit(out, ",\"inside\":");
		pc_jw_i64(out, (long long)inside);
		pc_jw_lit(out, "}");
	}
	pc_jw_lit(out, "}");
}

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
		pc_jw_lit(out, ",\"gone_s\":");
		pc_jw_i64(out, mem[i].gone_s);
		pc_jw_lit(out, ",\"held_s\":");
		pc_jw_i64(out, mem[i].held_s);
		pc_jw_lit(out, ",\"state\":\"");
		/* S192: a member that has LEFT reports no state of its own -
		 * it is not answering.  Say "gone" rather than borrow a
		 * lifecycle word that means something else. */
		pc_jw_lit(out, mem[i].gone_s >= 0 ? "gone"
			: pc_node_state_name(mem[i].state));
		/* S186: and WHY, when there is a reason to give.  The card
		 * showed FAILED with nothing beside it, which sends an
		 * operator to the logs of a node that may not be the one
		 * they are looking at - this is every node's view of every
		 * other. */
		pc_jw_lit(out, "\",\"reason\":\"");
		pc_jw_lit(out, pc_node_reason_text(mem[i].reason));
		pc_jw_lit(out, "\",\"mem_tier\":\"");
		pc_jw_lit(out, mem_tier_token(mem[i].mem_tier));
		pc_jw_lit(out, "\",\"free_mb\":");
		pc_jw_i64(out, mem[i].free_mb);
		pc_jw_lit(out, ",\"total_mb\":");
		pc_jw_i64(out, mem[i].total_mb);
		/* S78: gossiped, so the grid links to the node's OWN door and
		 * shows its uptime; -1 = a build before the fields */
		pc_jw_lit(out, ",\"http\":");
		pc_jw_i64(out, mem[i].http_port);
		pc_jw_lit(out, ",\"uptime_s\":");
		pc_jw_i64(out, mem[i].uptime_s);
		/* S103: this node's VIEW of the member - what the sender election
		 * decides from */
		pc_jw_lit(out, ",\"entries\":");
		pc_jw_i64(out, mem[i].entries);
		pc_jw_lit(out, ",\"start\":\"");
		pc_jw_lit(out, mem[i].start ? mem[i].start : "unknown");
		pc_jw_lit(out, "\"");
		/* S160: its open clients as gossiped, absent for a build before
		 * the field - the page sums what is present and says how many
		 * members reported */
		if (mem[i].cl_reported) {
			pc_jw_lit(out, ",\"clients\":{\"open\":");
			pc_jw_i64(out, mem[i].cl_open);
			pc_jw_lit(out, ",\"resp\":");
			pc_jw_i64(out, mem[i].cl_resp);
			pc_jw_lit(out, ",\"binary\":");
			pc_jw_i64(out, mem[i].cl_bin);
			pc_jw_lit(out, ",\"json\":");
			pc_jw_i64(out, mem[i].cl_json);
			pc_jw_lit(out, ",\"native_resp\":");
			pc_jw_i64(out, mem[i].cl_nresp);
			pc_jw_lit(out, "}");
		}
		/* PS11: its relay port and whether relays to it use it */
		pc_jw_lit(out, ",\"relay\":{\"port\":");
		pc_jw_i64(out, mem[i].relay_port);
		pc_jw_lit(out, mem[i].relay_direct ? ",\"direct\":true" : ",\"direct\":false");
		/* PS12: what relays to it are: all, filtered by its interest, or
		 * everything because our copy of its interest is not current */
		pc_jw_lit(out, mem[i].relay_interest == 1 ? ",\"interest\":\"filtered\"}"
			: mem[i].relay_interest == 2 ? ",\"interest\":\"broadcast\"}"
			: ",\"interest\":\"all\"}");
		/* S163: its per-collection figures, joined to /stats by hash */
		if (mem[i].cols_reported) {
			int k;

			pc_jw_lit(out, ",\"collections_total\":");
			pc_jw_i64(out, mem[i].ncols_total);
			pc_jw_lit(out, ",\"collections\":[");
			for (k = 0; k < mem[i].ncols; k++) {
				char hx[24];
				const struct clmemb_col *cl = &mem[i].cols[k];

				snprintf(hx, sizeof hx, "%016llx",
					(unsigned long long)cl->hash);
				pc_jw_lit(out, k ? ",{\"hash\":\"" : "{\"hash\":\"");
				pc_jw_lit(out, hx);
				pc_jw_lit(out, "\",\"entries\":");
				pc_jw_i64(out, (long long)cl->entries);
				pc_jw_lit(out, ",\"hits\":");
				pc_jw_i64(out, (long long)cl->hits);
				pc_jw_lit(out, ",\"misses\":");
				pc_jw_i64(out, (long long)cl->misses);
				pc_jw_lit(out, ",\"stores\":");
				pc_jw_i64(out, (long long)cl->stores);
				pc_jw_lit(out, ",\"removes\":");
				pc_jw_i64(out, (long long)cl->removes);
				pc_jw_lit(out, ",\"expired\":");
				pc_jw_i64(out, (long long)cl->expired);
				pc_jw_lit(out, "}");
			}
			pc_jw_lit(out, "]");
		}
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
	/* S221: the cluster wire epoch, advertised beside the routing algo
	 * and for the same reason - a client that does not recognise it
	 * stops ROUTING and lets the daemon forward, which costs a hop and
	 * never correctness. */
	pc_jw_lit(out, "],\"routing\":{\"epoch\":");
	pc_jw_i64(out, PC_CL_EPOCH);
	pc_jw_lit(out, ",\"algo\":\"" PC_ROUTE_ALGO
		"\",\"mode\":");
	{
		int md = pc_cluster_mode();

		pc_jw_lit(out, md == PC_MODE_PROXY ? "\"proxy\""
			: md == PC_MODE_SPREAD ? "\"spread\""
			: md == PC_MODE_SHARD ? "\"shard\""
			: "\"store\"");
	}
	pc_jw_lit(out, ",\"eager\":");
	pc_jw_lit(out, pc_cluster_eager() ? "true" : "false");
	/* the single operational name; "mode" above stays as it was so
	 * an existing reader keeps working */
	pc_jw_lit(out, ",\"mode_name\":\"");
	pc_jw_lit(out, pc_cluster_mode_name());
	pc_jw_lit(out, "\"");
	/* S127: K under spread, 0 otherwise.  An operator confirming a
	 * spread fleet needs the number the WRITE PATH is using, not
	 * the one in the file - they differ if the config never
	 * reached the cluster. */
	pc_jw_lit(out, ",\"replicas\":");
	pc_jw_i64(out, pc_cluster_replicas());
	pc_jw_lit(out, ",\"authoritative\":");
	pc_jw_lit(out, pc_cluster_authoritative() ? "true" : "false");
	/* the peer-plane port every member shares: owner selection
	 * mixes (advertise ip, CLUSTER port), so a client that wants
	 * to compute an owner needs it - the client port would give
	 * a different hash and route everything wrong (harmlessly,
	 * but pointlessly) */
	pc_jw_lit(out, ",\"cport\":");
	pc_jw_i64(out, pc_cluster_port());
	/* RV-10: the stamp of the map this list was read under.  A reply
	 * the daemon had to forward carries the stamp it routed by, and a
	 * client holding a different one knows it is behind. */
	{
		unsigned int mt = 0, ms = 0;

		if (pc_cluster_map_stamp(&mt, &ms)) {
			pc_jw_lit(out, ",\"map\":{\"term\":");
			pc_jw_i64(out, mt);
			pc_jw_lit(out, ",\"seq\":");
			pc_jw_i64(out, ms);
			pc_jw_lit(out, "}");
		}
	}
	/* S70: the one thing a client cannot otherwise learn about the
	 * daemon it dialled, and the first question in a mixed fleet */
	pc_jw_lit(out, "},\"version\":\"" PC_VERSION "\",\"rev\":\"" PC_BUILD_REV "\"}");
}

int pc_verb_text(const char *line, const struct pc_jtok *toks, int ntok,
		int method_tok, int params_tok, struct pc_jw *out,
		const char **errmsg, unsigned int *park_req, int *privileged, void *conn)
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

	/* ---- PS4: pub/sub on the native doors ---------------------------
	 * subscribe {channel} / psubscribe {pattern} / unsubscribe {channel?}
	 * / punsubscribe {pattern?} answer {"subscribed":N}; publish
	 * {channel, payload[, enc]} answers {"receivers":N}.  Deliveries are
	 * id-less notifications, {"method":"message","params":{"channel",
	 * "payload"[,"enc"]}} and "pmessage" with "pattern" - the S107
	 * shape, so libperfd's notify hook receives them and a blocking get
	 * keeps waiting for its own id: the connection stays multiplexed,
	 * which is the whole reason the binary dialect carries an id. */
	if (pc_json_streq(line, m, "subscribe") ||
	    pc_json_streq(line, m, "psubscribe") ||
	    pc_json_streq(line, m, "unsubscribe") ||
	    pc_json_streq(line, m, "punsubscribe")) {
		int pat = line[m->start] == 'p';
		int un = line[m->start + (pat ? 1 : 0)] == 'u';
		char name[4096];
		int nlen = params_tok < 0 ? -1 : get_str(line, toks, ntok,
			params_tok, pat ? "pattern" : "channel", NULL, name,
			sizeof name);
		int n;

		if (!un) {
			if (nlen <= 0)
				ERR(E_PARAMS, pat ? "missing or bad pattern"
					: "missing or bad channel");
			n = pc_pubsub_subscribe(conn, pc_worker_id(), name,
				(size_t)nlen, pat);
			if (n < 0)
				ERR(-32603, "cannot subscribe");
		} else if (nlen > 0) {
			n = pc_pubsub_unsubscribe(conn, name, (size_t)nlen, pat);
		} else {
			size_t l;
			int left;

			while (pc_pubsub_pop(conn, pat, name, sizeof name, &l, &left))
				;
			n = pc_pubsub_count(conn);
		}
		pc_jw_lit(out, "{\"subscribed\":");
		pc_jw_i64(out, n);
		pc_jw_lit(out, "}");
		return 0;
	}
	if (pc_json_streq(line, m, "publish")) {
		char chan[4096], *pay;
		int clen, plen;

		if (params_tok < 0)
			ERR(E_PARAMS, "missing params");
		clen = get_str(line, toks, ntok, params_tok, "channel", NULL,
			chan, sizeof chan);
		if (clen <= 0)
			ERR(E_PARAMS, "missing or bad channel");
		if ((size_t)clen >= sizeof PC_PUBSUB_RESERVED - 1 &&
		    !memcmp(chan, PC_PUBSUB_RESERVED, sizeof PC_PUBSUB_RESERVED - 1))
			ERR(E_PARAMS, "channel prefix __pc. is reserved");
		pay = get_buf();
		if (!pay)
			ERR(-32603, "out of memory");
		plen = get_str(line, toks, ntok, params_tok, "payload", "enc",
			pay, VAL_MAX);
		if (plen < 0)
			ERR(E_PARAMS, "missing or bad payload");
		pc_jw_lit(out, "{\"receivers\":");
		pc_jw_i64(out, pc_pubsub_publish(chan, (size_t)clen, pay,
			(size_t)plen, 0));
		pc_jw_lit(out, "}");
		return 0;
	}
	/* PS5: pushes over UDP to this connection's own address
	 * (lib/perfd_push.h):
	 *   pubsub_udp {"port":N}          -> {"stream","key",...}; port 0 stops
	 *   pubsub_udp_confirm {"cookie"}  -> {"udp":true,"seq":N}; a probe's
	 *                                     cookie; messages follow N
	 *   pubsub_udp_ack {"seq":N}       -> {"acked":N} */
	if (pc_json_streq(line, m, "pubsub_udp")) {
		long long port;
		uint64_t stream;
		char key[2 * PFP_KEY + 1], sid[20];
		int rc;

		if (params_tok < 0 || get_int(line, toks, ntok, params_tok, "port",
		        &port) != 0 || port < 0 || port > 65535)
			ERR(E_PARAMS, "missing or bad port (1..65535, or 0 to stop)");
		rc = pc_conn_udp_on(conn, (int)port, &stream, key, errmsg);
		if (rc < 0)
			return -32000;
		if (rc == 1) {
			pc_jw_lit(out, "{\"udp\":false}");
			return 0;
		}
		snprintf(sid, sizeof sid, "%016llx", (unsigned long long)stream);
		pc_jw_lit(out, "{\"stream\":\"");
		pc_jw_raw(out, sid, 16);
		pc_jw_lit(out, "\",\"key\":\"");
		pc_jw_raw(out, key, 2 * PFP_KEY);
		pc_jw_lit(out, "\",\"max_datagram\":");
		pc_jw_i64(out, PFP_MAX);
		pc_jw_lit(out, ",\"ack_every\":");
		pc_jw_i64(out, PFP_ACK_EVERY);
		pc_jw_lit(out, ",\"ack_ms\":");
		pc_jw_i64(out, PFP_ACK_MS);
		pc_jw_lit(out, ",\"prune_ms\":");
		pc_jw_i64(out, PFP_PRUNE_MS);
		pc_jw_lit(out, "}");
		return 0;
	}
	if (pc_json_streq(line, m, "pubsub_udp_confirm")) {
		char ck[64];
		int n = params_tok < 0 ? -1 : get_str(line, toks, ntok, params_tok,
			"cookie", NULL, ck, sizeof ck);

		if (n != 2 * PFP_COOKIE)
			ERR(E_PARAMS, "missing or bad cookie (32 hex digits)");
		uint64_t seq;

		if (pc_conn_udp_confirm(conn, ck, (size_t)n, &seq, errmsg) != 0)
			return -32000;
		pc_jw_lit(out, "{\"udp\":true,\"seq\":");
		pc_jw_i64(out, (long long)seq);
		pc_jw_lit(out, "}");
		return 0;
	}
	if (pc_json_streq(line, m, "pubsub_udp_ack")) {
		long long seq;

		if (params_tok < 0 || get_int(line, toks, ntok, params_tok, "seq",
		        &seq) != 0 || seq < 0)
			ERR(E_PARAMS, "missing or bad seq");
		if (pc_conn_udp_ack(conn, (uint64_t)seq, errmsg) != 0)
			return -32000;
		pc_jw_lit(out, "{\"acked\":");
		pc_jw_i64(out, seq);
		pc_jw_lit(out, "}");
		return 0;
	}
	/* ---- fleetstop {timeout_ms?} : S222 - stop the whole fleet -----
	 * PRIVILEGED, by S129's own rule: the set is chosen by blast radius,
	 * and nothing a client can say has a larger one - every node in the
	 * fleet goes down.  The client secret cannot gate it: applications
	 * hold that.  Each peer stops the way SIGTERM stops it (drain, a
	 * snapshot, goodbye); this node waits for them, reports who went and
	 * who did not, and then stops itself, last. */
	if (pc_json_streq(line, m, "fleetstop")) {
		int gone[64], still[64], ng = 0, ns = 0, nt, j;
		long long tmo = 30000;

		if (!privileged || !*privileged)
			ERR(-32000, pc_enable_configured() ? PC_NOPRIV_MSG
				: PC_NOENABLE_MSG);
		if (!pc_cluster_enabled())
			ERR(-32000, "this node is not clustered - stop it with "
				"SIGTERM, which takes a snapshot on the way down");
		if (params_tok >= 0)
			get_int(line, toks, ntok, params_tok, "timeout_ms", &tmo);
		if (tmo < 1000)
			tmo = 1000;
		if (tmo > 120000)
			tmo = 120000;
		nt = pc_cluster_fleet_stop(tmo, gone, &ng, still, &ns, 64);
		if (nt < 0)
			ERR(-32000, "this node is not clustered");
		pc_jw_lit(out, "{\"stopping\":true,\"asked\":");
		pc_jw_i64(out, nt);
		pc_jw_lit(out, ",\"stopped\":[");
		for (j = 0; j < ng; j++) {
			if (j)
				pc_jw_lit(out, ",");
			pc_jw_i64(out, gone[j]);
		}
		pc_jw_lit(out, "],\"still_up\":[");
		for (j = 0; j < ns; j++) {
			if (j)
				pc_jw_lit(out, ",");
			pc_jw_i64(out, still[j]);
		}
		pc_jw_lit(out, "]}");
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
		/* S215: before this, a refused fdatasync moved the watermark
		 * anyway and this verb answered {"synced":true} over it */
		if (rc2 == -2)
			ERR(-32000, "the WAL's storage failed - what was "
				"written after the last good sync is not on "
				"the device, and will not be synced by this "
				"process");
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

	/* ---- S69: create {col, buckets_log2?} / drop {col, force?} -----
	 * A cache should not need a config edit and a fleet restart to hold
	 * a new keyspace.  Gated by [daemon] allow_create, which is OFF by
	 * default: a driver that creates on miss turns a typo into a second
	 * empty collection and an operator into someone whose cache "lost
	 * everything", so this is opt-in on the DAEMON.  The gate governs
	 * ORIGINATION only - a create made anywhere reaches every member
	 * whatever its own setting, so the collection set stays uniform and
	 * turning the gate on does not need a fleet restart.
	 *
	 * A created collection takes the CLUSTER's mode: a fleet is one mode
	 * over one collection set, and a per-collection variant nobody
	 * declared is how a keyspace goes quietly unreplicated. */
	if (pc_json_streq(line, m, "create")) {
		char nm[PC_COL_NAME_MAX];
		long long bl = PC_COL_DEFAULT_LOG2;
		int nl, rc2;

		if (!allow_create)
			ERR(-32000, PC_NOCREATE_MSG);
		if (!privileged || !*privileged)
			ERR(-32000, pc_enable_configured() ? PC_NOPRIV_MSG
				: PC_NOENABLE_MSG);
		if (writes_denied())
			ERR(-32000, PC_NOTREADY_MSG);
		nl = params_tok < 0 ? -1 : get_str(line, toks, ntok, params_tok,
			"col", NULL, nm, sizeof nm - 1);
		if (nl <= 0)
			ERR(E_PARAMS, "missing col");
		nm[nl] = '\0';
		if (params_tok >= 0)
			get_int(line, toks, ntok, params_tok, "buckets_log2", &bl);
		if (bl < 4 || bl > 24)
			ERR(E_PARAMS, "buckets_log2 is 4..24");
		rc2 = pc_store_create(nm, (size_t)nl, (int)bl);
		if (rc2 == -2)
			ERR(E_PARAMS, "the collection limit is reached");
		if (rc2 == -3) {
			const char *why = index_ceiling_msg((int)bl);   /* S128 */

			ERR(-32000, why ? why : "the table could not be created "
				"- the arena has no room for its index");
		}
		if (rc2 != 0)
			ERR(E_PARAMS, "a collection of that name already exists");
		/* the WAL first, then the file: both are read at startup and
		 * the WAL's ordering settles a disagreement between them */
		pc_wal_col_create(nm, (int)bl);
		pc_store_persist();
		pc_cluster_col_announce(nm, (size_t)nl, (int)bl, CLCOL_OP_SET);
		LM_NOTICE("collection '%s' created by a client: 2^%d buckets, "
			"%s mode\n", nm, (int)bl, pc_cluster_mode_name());
		pc_jw_lit(out, "{\"created\":true,\"col\":");
		pc_jw_str(out, nm, (size_t)nl);
		pc_jw_lit(out, ",\"buckets\":");
		pc_jw_i64(out, 1LL << bl);
		pc_jw_lit(out, "}");
		return 0;
	}
	if (pc_json_streq(line, m, "drop")) {
		char nm[PC_COL_NAME_MAX];
		pcache_ht_totals_t tot;
		long long n = 0;
		int nl, force = 0, tf;

		if (!allow_create)
			ERR(-32000, PC_NOCREATE_MSG);
		if (!privileged || !*privileged)
			ERR(-32000, pc_enable_configured() ? PC_NOPRIV_MSG
				: PC_NOENABLE_MSG);
		if (writes_denied())
			ERR(-32000, PC_NOTREADY_MSG);
		nl = params_tok < 0 ? -1 : get_str(line, toks, ntok, params_tok,
			"col", NULL, nm, sizeof nm - 1);
		if (nl <= 0)
			ERR(E_PARAMS, "missing col");
		nm[nl] = '\0';
		tf = params_tok < 0 ? -1 : pc_json_get(line, toks, ntok,
			params_tok, "force");
		force = tf >= 0 && pc_json_streq_prim(line, &toks[tf]);
		ht = pc_store_find(nm, (size_t)nl);
		if (!ht)
			ERR(E_PARAMS, "no such collection");
		pcache_ht_totals(ht, &tot);
		if (tot.entries && !force) {
			pc_jw_lit(out, "{\"dropped\":false,\"entries\":");
			pc_jw_i64(out, (long long)tot.entries);
			pc_jw_lit(out, ",\"why\":\"the collection is not empty - "
				"pass force to drop it with its records\"}");
			return 0;
		}
		n = pc_store_drop(nm, (size_t)nl);
		if (n < 0)
			ERR(E_PARAMS, "no such collection");
		pc_wal_col_drop(nm);
		pc_store_persist();
		pc_cluster_col_announce(nm, (size_t)nl, 0, CLCOL_OP_DROP);
		LM_NOTICE("collection '%s' dropped by a client: %lld record(s) "
			"freed\n", nm, n);
		pc_jw_lit(out, "{\"dropped\":true,\"col\":");
		pc_jw_str(out, nm, (size_t)nl);
		pc_jw_lit(out, ",\"records\":");
		pc_jw_i64(out, n);
		pc_jw_lit(out, "}");
		return 0;
	}

	/* ---- S69: rename {col, to} -------------------------------------
	 * The atomic cutover perfload's restore-into-a-second-collection
	 * always wanted: restore, verify, swap, drop.  One pointer swap, so
	 * a lookup sees one name or the other and never a half-written one.
	 *
	 * The collections file is NOT brought forward here: records written
	 * before this are in the WAL under the old name, and a replay has to
	 * meet that name before the rename record moves it.  A checkpoint
	 * retires that window and persists the new name; with no WAL running
	 * there is no window and the file is written at once. */
	if (pc_json_streq(line, m, "rename")) {
		char nm[PC_COL_NAME_MAX], to[PC_COL_NAME_MAX];
		int nl, tl, rc2;

		if (!allow_create)
			ERR(-32000, PC_NOCREATE_MSG);
		if (!privileged || !*privileged)
			ERR(-32000, pc_enable_configured() ? PC_NOPRIV_MSG
				: PC_NOENABLE_MSG);
		if (writes_denied())
			ERR(-32000, PC_NOTREADY_MSG);
		nl = params_tok < 0 ? -1 : get_str(line, toks, ntok, params_tok,
			"col", NULL, nm, sizeof nm - 1);
		tl = params_tok < 0 ? -1 : get_str(line, toks, ntok, params_tok,
			"to", NULL, to, sizeof to - 1);
		if (nl <= 0 || tl <= 0)
			ERR(E_PARAMS, "rename takes col and to");
		nm[nl] = '\0';
		to[tl] = '\0';
		rc2 = pc_store_rename(nm, (size_t)nl, to, (size_t)tl,
			pc_lamport_tick());
		if (rc2 == -1)
			ERR(E_PARAMS, "no such collection");
		if (rc2 == -2)
			ERR(E_PARAMS, "a collection of the new name already exists");
		if (rc2 != 0)
			ERR(-32603, "the rename failed");
		pc_wal_col_rename(nm, to);
		if (!pc_wal_enabled())
			pc_store_persist();
		pc_cluster_col_announce2(nm, (size_t)nl, to, (size_t)tl);
		LM_NOTICE("collection '%s' renamed to '%s' by a client\n", nm, to);
		pc_jw_lit(out, "{\"renamed\":true,\"col\":");
		pc_jw_str(out, nm, (size_t)nl);
		pc_jw_lit(out, ",\"to\":");
		pc_jw_str(out, to, (size_t)tl);
		pc_jw_lit(out, "}");
		return 0;
	}

	/* ---- S69: resize {col, buckets_log2} ---------------------------
	 * Either direction, one mechanism, while the collection serves.  The
	 * work runs on the maintenance thread in bounded passes, so this
	 * returns as soon as it has started rather than holding a worker for
	 * the length of a migration; `stats` carries the progress and the
	 * daemon logs the swap and the completion. */
	if (pc_json_streq(line, m, "resize")) {
		char nm[PC_COL_NAME_MAX];
		long long bl = 0;
		int nl, rc2;

		if (!allow_create)
			ERR(-32000, PC_NOCREATE_MSG);
		if (!privileged || !*privileged)
			ERR(-32000, pc_enable_configured() ? PC_NOPRIV_MSG
				: PC_NOENABLE_MSG);
		if (writes_denied())
			ERR(-32000, PC_NOTREADY_MSG);
		nl = params_tok < 0 ? -1 : get_str(line, toks, ntok, params_tok,
			"col", NULL, nm, sizeof nm - 1);
		if (nl <= 0)
			ERR(E_PARAMS, "missing col");
		nm[nl] = '\0';
		if (params_tok < 0 ||
		        get_int(line, toks, ntok, params_tok, "buckets_log2", &bl))
			ERR(E_PARAMS, "missing buckets_log2");
		rc2 = pc_store_resize_start(nm, (size_t)nl, (int)bl);
		if (rc2 == -1)
			ERR(E_PARAMS, "no such collection");
		if (rc2 == -2)
			ERR(E_PARAMS, "a resize of that collection is already running");
		if (rc2 == -3)
			ERR(E_PARAMS, "buckets_log2 is 4..24, and not below what "
				"the collection already holds - the splitter would "
				"grow it straight back");
		if (rc2 == -4) {
			const char *why = index_ceiling_msg((int)bl);   /* S128 */

			ERR(-32000, why ? why : "the new table could not be "
				"created - the arena has no room for its index");
		}
		if (rc2 == -5)
			ERR(E_PARAMS, "the collection is already that size");
		pc_cluster_col_announce(nm, (size_t)nl, (int)bl, CLCOL_OP_RESIZE);
		LM_NOTICE("collection '%s': resize to 2^%d buckets started by a "
			"client\n", nm, (int)bl);
		pc_jw_lit(out, "{\"resizing\":true,\"col\":");
		pc_jw_str(out, nm, (size_t)nl);
		pc_jw_lit(out, ",\"buckets\":");
		pc_jw_i64(out, 1LL << bl);
		pc_jw_lit(out, "}");
		return 0;
	}

	/* ---- S123: reset_stats : the running totals start again --------
	 * Admin scope like load: the node's counters, not the fleet's; live
	 * gauges are untouched.  The RESP door's CONFIG RESETSTAT and the
	 * page's POST /reset-stats are the same call. */
	/* ---- S129: enable / disable -----------------------------------
	 * JSON DOOR ONLY, and that is the whole point of where this lives.
	 * The brief put ENABLE/DISABLE on the RESP door too; refused,
	 * because that door may run plaintext OFF-BOX under an allow-list,
	 * which would put the highest-value secret in the fleet on the
	 * wire in the clear - a worse exposure than the AUTH password it
	 * would sit beside, since this one grants keyspace deletion.
	 * Redis clients have no reason to issue DDL.
	 *
	 * @privileged NULL means a door with no privilege concept (binary,
	 * RESP): enable is refused there rather than silently succeeding. */
	if (pc_json_streq(line, m, "enable")) {
		char sec[256];
		int sl;

		if (!privileged)
			ERR(-32000, "enable is available on the JSON door only");
		if (!pc_enable_configured()) {
			enable_fail++;
			ERR(-32000, PC_NOENABLE_MSG);
		}
		sl = params_tok < 0 ? -1 : get_str(line, toks, ntok, params_tok,
			"secret", NULL, sec, sizeof sec - 1);
		if (sl <= 0) {
			enable_fail++;
			ERR(E_PARAMS, "missing secret");
		}
		sec[sl] = 0;
		if (!pc_enable_secret_ok(sec, (size_t)sl)) {
			enable_fail++;
			/* counted and said, the way resp_authfail is: a
			 * privilege secret being guessed should be visible */
			LM_NOTICE("a client offered a wrong enable secret "
				"(%llu failed so far)\n",
				(unsigned long long)enable_fail);
			ERR(-32000, "wrong enable secret");
		}
		*privileged = 1;
		LM_NOTICE("a client raised privilege with enable\n");
		pc_jw_lit(out, "{\"privileged\":true}");
		return 0;
	}
	if (pc_json_streq(line, m, "disable")) {
		if (!privileged)
			ERR(-32000, "disable is available on the JSON door only");
		*privileged = 0;
		pc_jw_lit(out, "{\"privileged\":false}");
		return 0;
	}

	if (pc_json_streq(line, m, "reset_stats")) {
		pc_jw_lit(out, "{\"reset\":true,\"at\":");
		pc_jw_i64(out, pc_stats_reset());
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
			pc_jw_i64(out, (long long)ob.fsync_n);
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

	/* ---- S88: collections -> [{name, mode, entries, buckets}] -----------
	 * The second question every operator asks after connecting, and the
	 * one `stats` answered only as a large document. */
	if (pc_json_streq(line, m, "collections")) {
		int i;

		pc_jw_lit(out, "{\"collections\":[");
		for (i = 0; i < pc_store_count(); i++) {
			if (!pc_store_live(i))
				continue;   /* S69: a dropped collection */
			pcache_htable_t *cht = pc_store_ht(i);
			pcache_ht_totals_t tot;

			pcache_ht_totals(cht, &tot);
			if (i)
				pc_jw_lit(out, ",");
			pc_jw_lit(out, "{\"name\":");
			pc_jw_str(out, pc_store_name(i), strlen(pc_store_name(i)));
			pc_jw_lit(out, ",\"mode\":\"");
			pc_jw_lit(out, pc_cluster_replicas() ? "spread"
				: pc_store_shard_enabled(cht) ? "shard"
				: pc_store_proxy_enabled(cht) ? "proxy"
				: pc_store_eager_enabled(cht) ? "eager" : "store");
			pc_jw_lit(out, "\",\"entries\":");
			pc_jw_i64(out, (long long)tot.entries);
			pc_jw_lit(out, ",\"buckets\":");
			pc_jw_i64(out, pcache_ht_nbuckets(cht));
			pc_jw_lit(out, "}");
		}
		pc_jw_lit(out, "]}");
		return 0;
	}
	if (pc_json_streq(line, m, "stats")) {
		char colbuf[128];
		const char *only = NULL;
		int cl;

		/* get_str returns a LENGTH and does not terminate: every
		 * other caller carries the length alongside the buffer.
		 * pc_stats_json takes a C string, so terminate here or the
		 * compare runs off the end and the filter matches nothing -
		 * which is exactly what verbtest caught. */
		cl = params_tok >= 0
			? get_str(line, toks, ntok, params_tok, "col", NULL,
				colbuf, sizeof colbuf - 1) : -1;
		if (cl >= 0) {
			colbuf[cl] = '\0';
			only = colbuf;
		}
		pc_stats_json(out, only);
		return 0;
	}

	/* everything below needs a collection - but an UNKNOWN method must
	 * report method-not-found, not a missing collection */
	{
		static const char *cv[] = { "get", "set", "del", "exists",
			"expire", "ttl", "add", "sub", "mget", "mset", "keys",
			"scan", "jget", "jset", "jdel", "jincr",
			"jarrappend", "dump", "restore" };
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
	/* ---- S112: dump {col, cursor?, count?, slot_lo?, slot_hi?, table?} --
	 * A chunk of whole records for perfdump: key, value, ttl, version.
	 * Two passes on purpose.  The bucket-budgeted key scan stops exactly
	 * on a bucket boundary, so a chunk never repeats a record (the
	 * metadata walk resumes mid-bucket and re-emits, right for its
	 * replication consumers, wrong for a file that promises each record
	 * once); then one atomic fetch per key pairs value, expiry and
	 * version out of the same read.  `count` is BUCKETS (default 1024,
	 * S40's chunk), never records: records per chunk follow the load
	 * factor.  A slot range filters by the key's Redis slot (hashtag-
	 * aware, PC_SLOTS), so several connections split a keyspace
	 * deterministically.  Expired records are not emitted.  A chunk
	 * that does not fit the reply is refused whole ("halve count"), the
	 * partial reply rolled back, so the caller never sees a cut. */
	if (pc_json_streq(line, m, "dump")) {
		struct dump_keys dk;
		long long cur = 0, count = 1024, slo = 0, shi = PC_SLOTS - 1, endb = -1;
		unsigned int cursor;
		size_t off, mark;
		int n = 0;
		unsigned long long tpub = 0;
		long long want = -1;
		int tcol = params_tok < 0 ? -1
			: pc_json_get(line, toks, ntok, params_tok, "col");

		if (tcol < 0 || toks[tcol].type != PC_J_STR)
			ERR(E_PARAMS, "missing col");
		/* the table AND its generation, as one pair (see store.c) */
		ht = pc_store_find_pub(line + toks[tcol].start,
			(size_t)(toks[tcol].end - toks[tcol].start), &tpub);
		if (!ht)
			ERR(E_PARAMS, "no such collection");
		/* the dump bug: a cursor is a BUCKET index, and a resize swap
		 * between two chunks puts it into a table of another shape -
		 * after a grow the records of buckets already walked reappear
		 * past the cursor (duplicates), after a shrink those not yet
		 * walked fold in behind it (MISSED, silently).  The reply names
		 * the table it walked; a caller that hands it back is refused
		 * the moment the collection has been republished, and restarts
		 * the collection.  A caller that does not is served as before. */
		if (!get_int(line, toks, ntok, params_tok, "table", &want) &&
		        (want < 0 || (unsigned long long)want != tpub))
			ERR(E_PARAMS, "table resized during the dump: restart this "
				"collection from cursor 0");
		get_int(line, toks, ntok, params_tok, "cursor", &cur);
		if (cur < 0 || cur > 0xFFFFFFFFLL)
			ERR(E_PARAMS, "bad cursor");
		get_int(line, toks, ntok, params_tok, "count", &count);
		if (count < 1 || count > 16384)
			ERR(E_PARAMS, "count out of range");
		/* S112: `end` = the first bucket this walker does NOT own - a
		 * bucket-range split for several connections on one node, each
		 * walking its own buckets once (a slot range makes every
		 * connection scan every bucket and discard the rest: measured
		 * 1.7x on eight connections).  The tail belongs to the walker
		 * whose range holds the last bucket; a walker whose `end` is
		 * short of it never enters the tail. */
		get_int(line, toks, ntok, params_tok, "end", &endb);
		if (endb < -1 || endb > 0xFFFFFFFFLL)
			ERR(E_PARAMS, "bad end");
		get_int(line, toks, ntok, params_tok, "slot_lo", &slo);
		get_int(line, toks, ntok, params_tok, "slot_hi", &shi);
		if (slo < 0 || shi >= PC_SLOTS || slo > shi)
			ERR(E_PARAMS, "bad slot range");
		memset(&dk, 0, sizeof dk);
		dk.slo = (unsigned int)slo;
		dk.shi = (unsigned int)shi;
		dk.now = get_ticks();
		dk.cap = PC_MAX_REQ;
		dk.buf = malloc(dk.cap);
		if (!dk.buf)
			ERR(-32603, "out of memory");
		/* S121: one cursor space, the leg included.  A chunk is `count`
		 * buckets, or about `count` buckets' worth of records from the
		 * overflow leg once the buckets are done - the walk budgets the
		 * leg itself now (a cursor with PCACHE_CURSOR_OVF names a chain
		 * in it).  A walker with an `end` short of the last bucket stops
		 * there and never enters the leg; the walker whose range holds
		 * the last bucket carries the leg. */
		{
			unsigned int nb = pcache_ht_nbuckets(ht);
			unsigned int take = (unsigned int)count;
			int bounded = endb >= 0 && (unsigned long long)endb < (unsigned long long)nb;

			cursor = (unsigned int)cur;
			if (nb == 0) {
				free(dk.buf);
				ERR(E_PARAMS, "empty table");
			}
			if (bounded && (cursor & PCACHE_CURSOR_OVF)) {
				free(dk.buf);
				ERR(E_PARAMS, "a leg cursor on a bounded range");
			}
			if (bounded && cursor >= (unsigned int)endb) {
				free(dk.buf);
				ERR(E_PARAMS, "cursor at or past end");
			}
			if (bounded && take > (unsigned int)endb - cursor)
				take = (unsigned int)endb - cursor;
			rc = pcache_ht_scan_ex(ht, &cursor, take, PCACHE_SCAN_NOVAL,
				dump_key_cb, &dk);
			if (rc < 0 || dk.full) {
				free(dk.buf);
				ERR(E_PARAMS, "count too large for this table: halve it");
			}
			if (bounded && (cursor == 0 || cursor >= (unsigned int)endb))
				cursor = 0;            /* a bounded range: done at its end */
		}
		/* the reply may grow: a chunk is bounded by buckets, not bytes,
		 * and the dispatcher frees a heap buffer a verb swapped in */
		if (out->len == 0 && !out->owned)
			(void)pc_jw_init_heap(out, JW_REPLY_INIT);
		mark = out->len;
		pc_jw_lit(out, "{\"records\":[");
		for (off = 0; off < dk.len && !out->overflow; ) {
			str k, v;
			unsigned int exp = 0;
			unsigned long long ver = 0;
			uint32_t kl;

			memcpy(&kl, dk.buf + off, 4);
			k.s = dk.buf + off + 4;
			k.len = (int)kl;
			off += 4 + kl;
			if (pcache_ht_fetch_ver(ht, &k, &v, &exp, &ver) != 0)
				continue;              /* gone since the scan */
			if (exp && exp <= dk.now) {
				free(v.s);
				continue;
			}
			if (n++)
				pc_jw_lit(out, ",");
			pc_jw_lit(out, "{");
			pc_jw_value(out, "k", "k_enc", k.s, (size_t)k.len);
			pc_jw_lit(out, ",");
			pc_jw_value(out, "v", "enc", v.s, (size_t)v.len);
			pc_jw_lit(out, ",\"ttl\":");
			pc_jw_i64(out, rel_ttl(exp, dk.now));
			pc_jw_lit(out, ",\"ver\":");
			pc_jw_i64(out, (long long)ver);
			pc_jw_lit(out, "}");
			free(v.s);
		}
		free(dk.buf);
		if (out->overflow) {
			out->len = mark;           /* roll the partial reply back */
			out->overflow = 0;
			ERR(E_PARAMS, "chunk exceeds the reply: halve count");
		}
		pc_jw_lit(out, "],\"cursor\":");
		pc_jw_i64(out, cursor);
		pc_jw_lit(out, ",\"more\":");
		pc_jw_lit(out, cursor ? "true" : "false");
		pc_jw_lit(out, ",\"n\":");
		pc_jw_i64(out, n);
		/* the table this chunk walked: hand it back as `table` */
		pc_jw_lit(out, ",\"table\":");
		pc_jw_i64(out, (long long)tpub);
		pc_jw_lit(out, ",\"buckets\":");
		pc_jw_i64(out, (long long)pcache_ht_nbuckets(ht));
		pc_jw_lit(out, "}");
		return 0;
	}
	/* ---- S113: restore {col, policy?, records:[{k,k_enc?,v,enc?,exp,ver}]}
	 * perfload's batch: each record installed with ITS version and ITS
	 * absolute expiry, through the write path (WAL, the eager push), not
	 * as a fresh client write.  policy: "newer" (the store's own rule -
	 * a copy older than the one held loses, counted), "skip" (an existing
	 * key is left alone, counted), "overwrite" (the loaded VALUE wins
	 * under a fresh version, so every replica takes it - a fleet rolled
	 * back to a dump).  A record whose expiry has passed is skipped and
	 * counted.  A key this node does not own (shard/proxy) is refused and
	 * counted rather than forwarded: the forward frame carries no
	 * version, and a batch cannot park; the loader routes to owners. */
	if (pc_json_streq(line, m, "restore")) {
		const char *em = "missing col";
		/* S129: privileged by BLAST RADIUS.  restore with
		 * policy:overwrite replaces every record it is handed at a
		 * fresh version - perfload rolls a fleet back with it - which
		 * is a larger blast radius than the resize beside it in this
		 * set.  It is NOT gated on allow_create: that switch is about
		 * originating DDL, and a restore creates no collection. */
		if (!privileged || !*privileged)
			ERR(-32000, pc_enable_configured() ? PC_NOPRIV_MSG
				: PC_NOENABLE_MSG);
		int trec, tpol, i, policy = 0;   /* 0 newer, 1 skip, 2 overwrite */
		long long stored = 0, older = 0, existing = 0, expired = 0, refused = 0, bad = 0;
		unsigned long long now_ms;
		unsigned int now_t = get_ticks();
		struct timespec tw;
		char *kbuf, *vbuf, *raw;
		int nmis = 0, ridx = -1;

		ht = get_col(line, toks, ntok, params_tok, &em);
		if (!ht)
			ERR(E_PARAMS, em);
		if (writes_denied())
			ERR(-32000, PC_NOTREADY_MSG);
		tpol = params_tok < 0 ? -1 : pc_json_get(line, toks, ntok, params_tok, "policy");
		if (tpol >= 0) {
			if (pc_json_streq(line, &toks[tpol], "newer"))
				policy = 0;
			else if (pc_json_streq(line, &toks[tpol], "skip"))
				policy = 1;
			else if (pc_json_streq(line, &toks[tpol], "overwrite"))
				policy = 2;
			else
				ERR(E_PARAMS, "policy: newer, skip or overwrite");
		}
		trec = params_tok < 0 ? -1 : pc_json_get(line, toks, ntok, params_tok, "records");
		if (trec < 0 || toks[trec].type != PC_J_ARR)
			ERR(E_PARAMS, "missing records");
		clock_gettime(CLOCK_REALTIME, &tw);
		now_ms = (unsigned long long)tw.tv_sec * 1000ULL + (unsigned long long)tw.tv_nsec / 1000000ULL;
		kbuf = malloc(PCACHE_CELL_MAX);
		vbuf = malloc(PCACHE_CELL_MAX);
		raw = malloc(PCACHE_CELL_MAX);
		if (!kbuf || !vbuf || !raw) {
			free(kbuf); free(vbuf); free(raw);
			ERR(-32603, "out of memory");
		}
		/* the reply opens with the misrouted list: [index, node] pairs
		 * the loader re-sends to the node named; the counts follow */
		pc_jw_lit(out, "{\"misrouted\":[");
		for (i = trec + 1; i < ntok; i++) {
			int tk, tv_, te, tx, tver, kl, vl, rc2, child = 0, j;
			str k, v;
			long long expms = 0, ver = 0;
			unsigned int exp;
			unsigned long long ver_used, held_ver = 0;

			if (toks[i].parent != trec)
				continue;
			ridx++;
			if (toks[i].type != PC_J_OBJ) {
				bad++;
				continue;
			}
			/* the record's fields: direct children alternate key, value */
			tk = tv_ = te = tx = tver = -1;
			{
				int tke = -1;

				for (j = i + 1; j < ntok && child < toks[i].size * 2; j++) {
					int len;

					if (toks[j].parent != i)
						continue;
					if ((child++ & 1) != 0 || toks[j].type != PC_J_STR || j + 1 >= ntok)
						continue;
					len = toks[j].end - toks[j].start;
					if (len == 1 && line[toks[j].start] == 'k')
						tk = j + 1;
					else if (len == 1 && line[toks[j].start] == 'v')
						tv_ = j + 1;
					else if (len == 3 && !memcmp(line + toks[j].start, "exp", 3))
						tx = j + 1;
					else if (len == 3 && !memcmp(line + toks[j].start, "ver", 3))
						tver = j + 1;
					else if (len == 3 && !memcmp(line + toks[j].start, "enc", 3))
						te = j + 1;
					else if (len == 5 && !memcmp(line + toks[j].start, "k_enc", 5))
						tke = j + 1;
				}
				if (tk < 0 || tv_ < 0 || tver < 0) {
					bad++;
					continue;
				}
				kl = pc_json_unescape(line, &toks[tk], raw, PCACHE_CELL_MAX);
				if (kl < 0) {
					bad++;
					continue;
				}
				if (tke >= 0 && pc_json_streq(line, &toks[tke], "b64"))
					kl = pc_b64_dec(raw, (size_t)kl, kbuf, PCACHE_CELL_MAX);
				else
					memcpy(kbuf, raw, (size_t)kl);
				if (kl <= 0) {
					bad++;
					continue;
				}
			}
			vl = pc_json_unescape(line, &toks[tv_], raw, PCACHE_CELL_MAX);
			if (vl < 0) {
				bad++;
				continue;
			}
			if (te >= 0 && pc_json_streq(line, &toks[te], "b64"))
				vl = pc_b64_dec(raw, (size_t)vl, vbuf, PCACHE_CELL_MAX);
			else
				memcpy(vbuf, raw, (size_t)vl);
			if (vl < 0) {
				bad++;
				continue;
			}
			if (tx >= 0)
				expms = strtoll(line + toks[tx].start, NULL, 10);
			ver = strtoll(line + toks[tver].start, NULL, 10);
			if (expms > 0 && (unsigned long long)expms <= now_ms) {
				expired++;
				continue;
			}
			exp = expms > 0 ? now_t + (unsigned int)(((unsigned long long)expms - now_ms + 999ULL) / 1000ULL) : 0;
			k.s = kbuf; k.len = kl;
			v.s = vbuf; v.len = vl;
			/* a key another node owns (shard) or is known to hold
			 * (proxy): refused and NAMED, never forwarded */
			if (pc_cluster_enabled()) {
				const char *cn = col_name_of(ht);
				int node = 0;

				if (pc_store_shard_enabled(ht))
					node = pc_shard_owner(cn, strlen(cn), k.s, (size_t)k.len);
				else if (pc_store_proxy_enabled(ht) &&
				         pcache_ht_getver(ht, &k, &held_ver) != 0)
					node = pc_loc_get(cn, strlen(cn), k.s, (size_t)k.len);
				if (node) {
					if (nmis++)
						pc_jw_lit(out, ",");
					pc_jw_lit(out, "[");
					pc_jw_i64(out, ridx);
					pc_jw_lit(out, ",");
					pc_jw_i64(out, node);
					pc_jw_lit(out, "]");
					refused++;
					continue;
				}
			}
			/* getver, not probe: the probe misreports a record in the
			 * overflow leg as absent (S122) */
			if (policy == 1 && pcache_ht_getver(ht, &k, &held_ver) == 0) {
				existing++;
				continue;
			}
			if (policy == 2) {
				rc2 = pcache_ht_store(ht, &k, &v, exp);
				ver_used = pcache_last_ver;
			} else {
				rc2 = pcache_ht_store_ver(ht, &k, &v, exp, 0, (unsigned long long)ver);
				ver_used = (unsigned long long)ver;
			}
			if (rc2 == PCACHE_E_OLDER) {
				older++;
				continue;
			}
			if (rc2 != 0) {
				refused++;                 /* full, or an oversized record */
				continue;
			}
			{
				const char *cn = col_name_of(ht);

				pc_wal_upsert(cn, k.s, k.len, v.s, v.len, exp, ver_used);
				eager_push(ht, &k, v.s, (size_t)v.len, exp, ver_used);
				if (pc_cluster_enabled())
					pc_neg_clear(cn, strlen(cn), k.s, (size_t)k.len);
				pc_store_note_set(ht, (size_t)k.len, (size_t)v.len);
			}
			stored++;
		}
		free(kbuf); free(vbuf); free(raw);
		pc_jw_lit(out, "],\"stored\":");
		pc_jw_i64(out, stored);
		pc_jw_lit(out, ",\"older\":");
		pc_jw_i64(out, older);
		pc_jw_lit(out, ",\"existing\":");
		pc_jw_i64(out, existing);
		pc_jw_lit(out, ",\"expired\":");
		pc_jw_i64(out, expired);
		pc_jw_lit(out, ",\"refused\":");
		pc_jw_i64(out, refused);
		pc_jw_lit(out, ",\"bad\":");
		pc_jw_i64(out, bad);
		pc_jw_lit(out, "}");
		return 0;
	}
	/* ---- S88: keys {col: <glob>|absent, match?, limit?} -----------------
	 * "keys *" means every collection, not a collection named star: the
	 * collection argument is a glob and the answer is grouped by
	 * collection.
	 *
	 * THE LIMIT IS PER COLLECTION (default 100), not a budget shared
	 * across the result.  It was shared once, drawn down in collection
	 * order, and the comment here claimed that stopped "one large
	 * collection starving the rest" - it did the opposite.  The FIRST
	 * collection spent the budget, the outer loop stopped, and every
	 * later collection was omitted from the reply without even being
	 * named; `truncated` was the only hint and it does not distinguish
	 * "some keys missing" from "whole collections missing".  Reported
	 * from the field 2026-09-14.
	 *
	 * So: every matched collection is listed, each up to `limit`, and
	 * each one that hit its cap is named in `truncated_collections` -
	 * additive, so nothing that reads `collections` today breaks.
	 * Enumeration is still the most expensive thing this daemon does,
	 * and the bound that matters is the RESPONSE SIZE, so the writer's
	 * overflow now stops the outer loop too - it used to keep walking
	 * collections it could no longer emit. */
	if (pc_json_streq(line, m, "keys")) {
		int tc = params_tok < 0 ? -1
			: pc_json_get(line, toks, ntok, params_tok, "col");
		const char *cg = "*";
		size_t cgl = 1;

		if (tc >= 0 && toks[tc].type == PC_J_STR) {
			cg = line + toks[tc].start;
			cgl = (size_t)(toks[tc].end - toks[tc].start);
		}
		if (tc < 0 || memchr(cg, '*', cgl) || memchr(cg, '?', cgl) ||
		    memchr(cg, '[', cgl)) {
			struct scan_ctx sc;
			char pat[256];
			long long limit = 100;
			int trunc[PC_MAX_COLLECTIONS];
			int i, first = 1, matched = 0, ntrunc = 0;

			memset(&sc, 0, sizeof sc);
			sc.w = out;
			sc.patlen = -1;
			if (params_tok >= 0 && pc_json_get(line, toks, ntok,
			        params_tok, "match") >= 0) {
				sc.patlen = get_str(line, toks, ntok, params_tok,
					"match", NULL, pat, sizeof pat);
				if (sc.patlen < 0)
					ERR(E_PARAMS, "bad match pattern");
			}
			sc.pat = sc.patlen >= 0 ? pat : NULL;
			sc.now = get_ticks();
			get_int(line, toks, ntok, params_tok, "limit", &limit);
			if (limit < 1 || limit > 100000)
				ERR(E_PARAMS, "limit out of range");
			pc_jw_lit(out, "{\"collections\":{");
			for (i = 0; i < pc_store_count() && !out->overflow; i++) {
				if (!pc_store_live(i))
					continue;   /* S69: a dropped collection */
				const char *nm = pc_store_name(i);
				unsigned int cursor = 0;
				int rc2;

				if (!pc_glob(cg, (int)cgl, nm, (int)strlen(nm)))
					continue;
				matched++;
				if (!first)
					pc_jw_lit(out, ",");
				first = 0;
				pc_jw_str(out, nm, strlen(nm));
				pc_jw_lit(out, ":[");
				sc.emitted = 0;
				sc.stopped = 0;
				sc.limit = (int)limit;
				do {
					rc2 = pcache_ht_scan_ex(pc_store_ht(i), &cursor,
						1024, PCACHE_SCAN_NOVAL, keys_cb, &sc);
				} while (rc2 == 0 && cursor && !out->overflow);
				pc_jw_lit(out, "]");
				if (sc.stopped && ntrunc < PC_MAX_COLLECTIONS)
					trunc[ntrunc++] = i;
			}
			pc_jw_lit(out, "},\"truncated\":");
			pc_jw_lit(out, (ntrunc || out->overflow) ? "true" : "false");
			pc_jw_lit(out, ",\"truncated_collections\":[");
			for (i = 0; i < ntrunc; i++) {
				const char *tn = pc_store_name(trunc[i]);

				if (i)
					pc_jw_lit(out, ",");
				pc_jw_str(out, tn, strlen(tn));
			}
			pc_jw_lit(out, "],\"matched\":");
			pc_jw_i64(out, matched);
			pc_jw_lit(out, "}");
			return 0;
		}
	}
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
		rc = op_expire(ht, &k, ttl, park_req);
		pc_jw_lit(out, rc == PC_OP_OK ? "{\"updated\":true}"
			: "{\"updated\":false}");
		return 0;
	}

	/* ---- add/sub {col,key,by?,ttl?} -> {value:n} ------------------- */
	if (pc_json_streq(line, m, "add") || pc_json_streq(line, m, "sub")) {
		long long nv = 0;

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
		if (rc == PC_OP_ERR_WRFAIL)    /* refused: nv was never written */
			ERR(-32000, PC_WRFAIL_MSG);
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

		/* S201: jdel at the ROOT is a key removal, not a document
		 * edit - what the RESP door has done for JSON.DEL key [$]
		 * all along (rh_json, rootpath).  This door sent it through
		 * pc_json_rmw -> pc_jp_del, which cannot delete the root: every
		 * root jdel answered -32022 "bad path" and left the key, on a
		 * fresh document, an edited one, an implicit path and a plain
		 * value alike.  Found gating cachedb_perfd for the staging
		 * RGSs, whose de-registration is JSON.DEL <key>: the record
		 * survived and the device kept reading as registered.  Same
		 * call as the native del verb, so a proxy or shard collection
		 * forwards and parks exactly as del does. */
		if (op == PC_JOP_DEL && plen == 1 && path[0] == '$') {
			rc = op_del(ht, &k, park_req);
			pc_jw_lit(out, rc == PC_OP_OK ? "{\"deleted\":true}"
				: "{\"deleted\":false}");
			return 0;
		}

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
			/* S88: say which collection, so a listed key pastes straight
			 * back into get, and say when this is ONE node's share: a
			 * placement-spread collection is a third of the truth on a
			 * three-node fleet, and an operator read it as "two nodes are
			 * not saving their keys" */
			{
				struct pc_cl_stats cs;
				const char *cn = col_name_of(ht);

				pc_cluster_get_stats(&cs);
				pc_jw_lit(out, "{\"collection\":");
				pc_jw_str(out, cn, strlen(cn));
				pc_jw_lit(out, ",\"complete\":");
				pc_jw_lit(out, pc_cluster_enabled() && cs.peers_up > 0 &&
					!pc_store_eager_enabled(ht) ? "false" : "true");
				pc_jw_lit(out, ",\"scope\":\"node\",\"keys\":[");
			}
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
const char pc_bin_unknown_verb[] = "unknown verb";

int pc_verb_bin(const char *pl, size_t plen, struct pc_jw *out, int *flags,
		unsigned int *park_req, const char **colp, size_t *collenp,
		const char **keyp, size_t *klenp, const char **errmsg, void *conn)
{
	unsigned int verb, cn;
	size_t klen, off;
	pcache_htable_t *ht;
	str k, v;
	long long by = 1, ttl = 0, nv = 0;
	unsigned int exp, vln;
	int rc, is_ctr;

	if (!plen)
		ERRB("empty request");
	verb = (unsigned char)pl[0];
	if (verb == PC_VERB_PING) {
		pc_jw_raw(out, pl + 1, plen - 1);
		return 0;
	}
	if (verb >= PC_VERB_PUBLISH && verb <= PC_VERB_PUNSUBSCRIBE) {
		/* PS4: the pub/sub verbs; see proto.h for the layouts */
		unsigned int cl;

		if (plen < 3)
			ERRB("short request");
		cl = br_u16(pl + 1);
		if (plen < 3 + cl)
			ERRB("short request");
		if (verb == PC_VERB_PUBLISH) {
			if (!cl)
				ERRB("missing channel");
			if (cl >= sizeof PC_PUBSUB_RESERVED - 1 &&
			    !memcmp(pl + 3, PC_PUBSUB_RESERVED,
			            sizeof PC_PUBSUB_RESERVED - 1))
				ERRB("channel prefix __pc. is reserved");
			pc_jw_lit(out, "{\"receivers\":");
			pc_jw_i64(out, pc_pubsub_publish(pl + 3, cl, pl + 3 + cl,
				plen - 3 - cl, 0));
			pc_jw_lit(out, "}");
			return 0;
		}
		{
			int pat = verb == PC_VERB_PSUBSCRIBE ||
				verb == PC_VERB_PUNSUBSCRIBE;
			int un = verb == PC_VERB_UNSUBSCRIBE ||
				verb == PC_VERB_PUNSUBSCRIBE;
			int n;

			if (!un) {
				if (!cl)
					ERRB("missing channel");
				n = pc_pubsub_subscribe(conn, pc_worker_id(), pl + 3,
					cl, pat);
				if (n < 0)
					ERRB("cannot subscribe");
			} else if (cl) {
				n = pc_pubsub_unsubscribe(conn, pl + 3, cl, pat);
			} else {
				char nm[4096];
				size_t l;
				int left;

				while (pc_pubsub_pop(conn, pat, nm, sizeof nm, &l, &left))
					;
				n = pc_pubsub_count(conn);
			}
			pc_jw_lit(out, "{\"subscribed\":");
			pc_jw_i64(out, n);
			pc_jw_lit(out, "}");
			return 0;
		}
	}
	if (verb < PC_VERB_GET || verb > PC_VERB_SUB)
		ERRB(pc_bin_unknown_verb);     /* S165: proto.c counts it by this pointer */
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
		rc = op_expire(ht, &k, ttl, park_req);
		bw_u8(out, rc == PC_OP_OK);       /* parked falls back to 0 */
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
		if (rc == PC_OP_ERR_WRFAIL)    /* refused: nv was never written */
			ERRB(PC_WRFAIL_MSG);
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

/* a string literal's length is the compiler's to count: JSON.DEBUG HELP
 * once sent 55 bytes of a 53-byte literal, two bytes of whatever
 * followed it in rodata - silent in a plain build, an abort under the
 * sanitizer (the rc18 tag pipeline).  Literals go through this. */
#define resp_bulk_lit(w, s) resp_bulk((w), (s), sizeof(s) - 1)

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

/* S69: the RESP door addresses collections by Redis DB INDEX, so until
 * now a collection had to be NAMED "0" to be reachable at all - and a
 * Redis-native monitor pointed at a node holding live data under real
 * names reported one empty database, which is a correctness problem for
 * the audience the door exists to serve.  An entry of resp_collections
 * may now be `INDEX:NAME`, which makes NAME the collection SELECT INDEX
 * lands on; a bare NAME keeps the old meaning (allowed, addressed by
 * itself).  Resolution happens once per command, so everything
 * downstream still works on a collection name. */
static size_t resp_map_index(const char *sel, size_t sl, char *out, size_t cap)
{
	const char *list = pc_resp_collections(), *p;

	if (list)
		for (p = list; *p; ) {
			const char *e, *colon;
			size_t len;

			while (*p == ' ' || *p == ',' || *p == '\t')
				p++;
			e = p;
			while (*e && *e != ',')
				e++;
			len = (size_t)(e - p);
			while (len && (p[len - 1] == ' ' || p[len - 1] == '\t'))
				len--;
			colon = memchr(p, ':', len);
			if (colon && (size_t)(colon - p) == sl &&
			        !memcmp(p, sel, sl)) {
				size_t nl = len - (size_t)(colon - p) - 1;

				if (nl && nl < cap) {
					memcpy(out, colon + 1, nl);
					out[nl] = 0;
					return nl;
				}
			}
			p = e;
		}
	if (sl >= cap)
		sl = cap - 1;
	memcpy(out, sel, sl);
	out[sl] = 0;
	return sl;
}

/* S69: the index a RESP client selects to reach @name - the reverse, for
 * INFO keyspace.  A collection with no index entry is reachable by its
 * own name and reported under it. */
static size_t resp_index_for(const char *name, size_t nl, char *out, size_t cap)
{
	const char *list = pc_resp_collections(), *p;

	if (list)
		for (p = list; *p; ) {
			const char *e, *colon;
			size_t len;

			while (*p == ' ' || *p == ',' || *p == '\t')
				p++;
			e = p;
			while (*e && *e != ',')
				e++;
			len = (size_t)(e - p);
			while (len && (p[len - 1] == ' ' || p[len - 1] == '\t'))
				len--;
			colon = memchr(p, ':', len);
			if (colon) {
				size_t off = (size_t)(colon - p) + 1;

				if (len - off == nl &&
				        !memcmp(colon + 1, name, nl)) {
					size_t il = (size_t)(colon - p);

					if (il && il < cap) {
						memcpy(out, p, il);
						out[il] = 0;
						return il;
					}
				}
			}
			p = e;
		}
	if (nl >= cap)
		nl = cap - 1;
	memcpy(out, name, nl);
	out[nl] = 0;
	return nl;
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
		{
			const char *colon = memchr(p, ':', len);

			/* S69: `INDEX:NAME` allows NAME - the index is how a
			 * RESP client asks for it, the name is what the rest
			 * of the daemon calls it */
			if (colon) {
				size_t off = (size_t)(colon - p) + 1;

				if (len - off == n &&
				    !memcmp(colon + 1, name, n))
					return 1;
			} else if (len == n && !memcmp(p, name, n)) {
				return 1;
			}
		}
		p = e;
	}
	return 0;
}

/* the selected collection, or an -ERR written into @out */
static pcache_htable_t *resp_col(struct pc_jw *out, const char *cur_col,
		size_t cur_collen)
{
	pcache_htable_t *ht;
	char nm[64];
	size_t nl = resp_map_index(cur_col, cur_collen, nm, sizeof nm);   /* S69 */

	cur_col = nm;
	cur_collen = nl;
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

/* S74: the ONE answer to "is this a cluster" that INFO server, INFO
 * cluster and CLUSTER INFO all derive from.  They drifted apart once -
 * INFO said standalone while CLUSTER INFO said enabled:1 - and every
 * cluster-aware client trusts INFO, so none of them ever fetched the map
 * the slot work exists to serve.  pc_cluster_members() returns 0 exactly
 * when the plane is off, so "nm > 0" and this are the same fact. */
static int resp_cluster_on(void)
{
	return pc_cluster_enabled();
}

/* ---- S75: the serialised slot map, cached per membership snapshot ----
 * CLUSTER SLOTS is a pure function of who the members are: building it
 * walks 16384 rendezvous rounds and writes ~750KB on a three-node fleet
 * (rendezvous scatters ownership, so ~11000 ranges).  Once S74 tells
 * clients the truth they all fetch it, on connect and on every topology
 * refresh, so the build is paid once per membership change instead of
 * once per client.  The client's own parsing cost is untouched - that is
 * the placement question (option 1 in the filing), measured, not
 * guessed at, before it is reopened.  Workers are threads: the copy out
 * happens under the lock. */
static pthread_mutex_t clmap_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { unsigned long long key; char *buf; size_t len; } clmap_cache[2];

/* FNV-1a over everything the reply depends on: the members, their
 * doors, their state, in snapshot order (self first - the reply differs
 * per node, so does the cache).  Placement reads the same peer table, so
 * a change that moves a slot changes this key. */
static unsigned long long resp_clmap_key(const struct pc_member *mem,
		int nm, int shards)
{
	unsigned long long h = FNV1A64_BASIS;
	int i;
#define CLMAP_MIX(_x) do { h = fnv1a64_u64(h, (uint64_t)(_x)); } while (0)
	CLMAP_MIX(nm);
	CLMAP_MIX(shards);
	for (i = 0; i < nm; i++) {
		CLMAP_MIX(mem[i].addr.s_addr);
		CLMAP_MIX(mem[i].node);
		CLMAP_MIX(mem[i].state);
		CLMAP_MIX(mem[i].client_port);
		CLMAP_MIX(mem[i].resp_port);
		CLMAP_MIX(mem[i].is_master);
	}
#undef CLMAP_MIX
	return h;
}

static int resp_clmap_cached(int shards, unsigned long long key,
		struct pc_jw *out)
{
	int hit = 0;

	pthread_mutex_lock(&clmap_mu);
	if (clmap_cache[shards].buf && clmap_cache[shards].key == key &&
	    pc_jw_init_heap(out, clmap_cache[shards].len + 16) == 0) {
		pc_jw_raw(out, clmap_cache[shards].buf, clmap_cache[shards].len);
		hit = 1;
	}
	pthread_mutex_unlock(&clmap_mu);
	return hit;
}

static void resp_clmap_store(int shards, unsigned long long key,
		const char *buf, size_t len)
{
	char *copy = malloc(len);

	if (!copy)
		return;                        /* answered, just not cached */
	memcpy(copy, buf, len);
	pthread_mutex_lock(&clmap_mu);
	free(clmap_cache[shards].buf);
	clmap_cache[shards].buf = copy;
	clmap_cache[shards].len = len;
	clmap_cache[shards].key = key;
	pthread_mutex_unlock(&clmap_mu);
}

/* ---- the command table (S140) -----------------------------------------
 * Two lists used to say which RESP commands exist: the `dc[]` array the
 * data-plane pre-check carried, and the if-chain below that dispatches
 * them.  A command in one and not the other answers "unknown command"
 * before reaching its handler, or falls through to the tail and answers
 * "protocol".  This is that one list.  Matching is `resp_is`, so it is
 * exactly what the chain does.
 *
 * The `fn` column arrives task by task as handlers come out of the
 * chain; until a row has one, the chain below still dispatches it, which
 * is why this lands first and alone. */
/* Which of the three positions in pc_verb_resp answers a command.  They
 * are NOT interchangeable: two gates cut across the old chain - the
 * readiness gate and the collection resolution - and a command answered
 * on the wrong side of one changes behaviour even though name matching
 * keeps the commands mutually exclusive.  S140 moved DBSIZE, FLUSHDB and
 * FLUSHALL across the readiness gate before that was noticed: a
 * RECOVERING node reported its keyspace and accepted a flush where it
 * owes the client -LOADING (readygatetest covers all three now). */
#define RESP_CONN   0x01               /* before the readiness gate */
#define RESP_READY  0x02               /* past it, before the collection */
#define RESP_DATA   0x04               /* past both: needs the collection */
#define RESP_KEYED  0x10               /* RV-10: argv[1] is THE key (one-key
                                        * commands; of several, the first) */

/* Everything a handler may need from the connection, filled once at the
 * top of pc_verb_resp: a command's code takes this and nothing else,
 * instead of seventeen parameters.  (S140) */
struct resp_ctx {
	char *const *argv;
	const size_t *argl;
	int nargs;
	struct pc_jw *out;
	char *scratch;
	size_t scratch_cap;
	unsigned int *park_req;
	const char **colp;
	size_t *collenp;
	const char **keyp;
	size_t *klenp;
	char *cur_col;
	size_t *cur_collen;
	int *quit;
	int *authed;
	struct pc_enum_start *es;
	void *obs;
	pcache_htable_t *ht;           /* NULL until the collection resolves */
	const char *cmd;               /* the name as the client spelled it, so */
	size_t cl;                     /* a shared handler can tell them apart */
	void *conn;                    /* PS1: the connection, opaque */
};

struct resp_cmd {
	const char *name;
	unsigned flags;
	int (*fn)(struct resp_ctx *);  /* NULL: the chain still dispatches it */
};

static int rh_ping(struct resp_ctx *c)
{
	if (pc_pubsub_count(c->conn) > 0) {
		/* PS2: in subscribed mode a PING answers as an array, the
		 * shape predis's pubSubLoop reads */
		resp_arr(c->out, 2);
		resp_bulk(c->out, "pong", 4);
		if (c->nargs > 1)
			resp_bulk(c->out, c->argv[1], c->argl[1]);
		else
			resp_bulk(c->out, "", 0);
		return 0;
	}
	if (c->nargs > 1)
		resp_bulk(c->out, c->argv[1], c->argl[1]);
	else
		resp_simple(c->out, "PONG");
	return 0;
}

/* ---- PS2: pub/sub over RESP2 ------------------------------------------- */

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
	int i;

	if (c->nargs < 2) {
		resp_err(c->out, pattern
			? "wrong number of arguments for 'psubscribe' command"
			: "wrong number of arguments for 'subscribe' command");
		return 0;
	}
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
		char name[4096];
		size_t nlen;
		int left, any = 0;

		while (pc_pubsub_pop(c->conn, pattern, name, sizeof name,
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

static int rh_subscribe(struct resp_ctx *c)    { return ps_subscribe(c, 0); }
static int rh_psubscribe(struct resp_ctx *c)   { return ps_subscribe(c, 1); }
static int rh_unsubscribe(struct resp_ctx *c)  { return ps_unsubscribe(c, 0); }
static int rh_punsubscribe(struct resp_ctx *c) { return ps_unsubscribe(c, 1); }

static int rh_publish(struct resp_ctx *c)
{
	if (c->nargs != 3) {
		resp_err(c->out, "wrong number of arguments for 'publish' command");
		return 0;
	}
	if (c->argl[1] >= sizeof PC_PUBSUB_RESERVED - 1 &&
	    !memcmp(c->argv[1], PC_PUBSUB_RESERVED, sizeof PC_PUBSUB_RESERVED - 1)) {
		resp_err(c->out, "channel prefix " PC_PUBSUB_RESERVED
			" is reserved for the daemon's own events");
		return 0;
	}
	resp_int(c->out, pc_pubsub_publish(c->argv[1], c->argl[1],
		c->argv[2], c->argl[2], 0));
	return 0;
}

struct ps_names { char **v; size_t *l; int n, cap; };

static int ps_collect(const char *name, size_t nlen, int nsubs, void *arg)
{
	struct ps_names *ns = arg;
	char *cp;

	(void)nsubs;
	if (ns->n == ns->cap) {
		int ncap = ns->cap ? ns->cap * 2 : 32;
		char **nv = realloc(ns->v, sizeof *nv * (size_t)ncap);
		size_t *nl = nv ? realloc(ns->l, sizeof *nl * (size_t)ncap) : NULL;

		if (!nv || !nl) {
			free(nv);
			return -1;
		}
		ns->v = nv; ns->l = nl; ns->cap = ncap;
	}
	cp = malloc(nlen ? nlen : 1);
	if (!cp)
		return -1;
	memcpy(cp, name, nlen);
	ns->v[ns->n] = cp;
	ns->l[ns->n] = nlen;
	ns->n++;
	return 0;
}

/* S165: a subcommand this door does not implement, counted by name -
 * the command and the subcommand, never an argument after them */
static void unk_sub(const struct resp_ctx *c)
{
	if (c->nargs >= 2)
		pc_obs_unknown(CLUNK_RESP, c->argv[0], c->argl[0], c->argv[1],
			c->argl[1], c->obs);
}

static int rh_pubsub(struct resp_ctx *c)
{
	const char *sub = c->nargs > 1 ? c->argv[1] : "";
	size_t subl = c->nargs > 1 ? c->argl[1] : 0;
	int i;

	if (resp_is(sub, subl, "CHANNELS")) {
		struct ps_names ns = { NULL, NULL, 0, 0 };

		pc_pubsub_channels(c->nargs > 2 ? c->argv[2] : NULL,
			c->nargs > 2 ? c->argl[2] : 0, ps_collect, &ns);
		resp_arr(c->out, ns.n);
		for (i = 0; i < ns.n; i++) {
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
		unk_sub(c);                    /* S165 */
		resp_err(c->out, "unsupported PUBSUB subcommand (CHANNELS, "
			"NUMSUB, NUMPAT)");
	}
	return 0;
}

static int rh_reset(struct resp_ctx *c)
{
	char name[4096];
	size_t nlen;
	int left;

	/* PS2: back to a fresh connection's shape - out of subscribed mode
	 * and on db 0.  node-redis v4 uses it on its return paths. */
	while (pc_pubsub_pop(c->conn, 0, name, sizeof name, &nlen, &left))
		;
	while (pc_pubsub_pop(c->conn, 1, name, sizeof name, &nlen, &left))
		;
	c->cur_col[0] = '0';
	*c->cur_collen = 1;
	resp_simple(c->out, "RESET");
	return 0;
}

static int rh_echo(struct resp_ctx *c)
{
	if (c->nargs != 2)
		resp_err(c->out, "wrong number of arguments for 'echo' command");
	else
		resp_bulk(c->out, c->argv[1], c->argl[1]);
	return 0;
}

static int rh_quit(struct resp_ctx *c)
{
	resp_simple(c->out, "OK");
	*c->quit = 1;
	return 0;
}

static int rh_command(struct resp_ctx *c)
{
	resp_arr(c->out, 0);                   /* redis-cli probes; empty is fine */
	return 0;
}

static int rh_config(struct resp_ctx *c)
{
	if (c->nargs >= 2 && resp_is(c->argv[1], c->argl[1], "GET"))
		resp_arr(c->out, 0);           /* "no such parameter" */
	else if (c->nargs >= 2 && resp_is(c->argv[1], c->argl[1], "RESETSTAT")) {
		pc_stats_reset();              /* S123: what redis-cli expects */
		resp_simple(c->out, "OK");
	} else {
		unk_sub(c);                    /* S165 */
		resp_err(c->out, "unsupported CONFIG subcommand");
	}
	return 0;
}

static int rh_dbsize(struct resp_ctx *c)
{
	pcache_ht_totals_t tot;
	pcache_htable_t *ht = resp_col(c->out, c->cur_col, *c->cur_collen);

	if (!ht)
		return 0;
	pcache_ht_totals(ht, &tot);
	resp_int(c->out, (long long)tot.entries);
	return 0;
}

static int rh_flush(struct resp_ctx *c)
{
	resp_err(c->out, "FLUSHDB is not supported (delete keys "
		"explicitly)");
	return 0;
}

/* CLIENT SETINFO LIB-NAME|LIB-VER <value>: what a client library sends
 * on connect to say what it is (redis-py, go-redis, ...).  The first
 * thing S165's unknown-command card caught on the staging fleet, 66
 * times in five minutes from behind the HAProxy - answered "unsupported"
 * every time, and with it the only clue to which client that was.  The
 * value is kept on the connection and shown in CLIENT LIST and /clients.
 * As Redis answers it: exactly one attribute and one value, the two
 * attribute names only, and no spaces, newlines or other unprintable
 * bytes in the value, which is written into a space-separated list. */
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

static int rh_client(struct resp_ctx *c)
{
	if (c->nargs == 3 && resp_is(c->argv[1], c->argl[1], "SETNAME")) {
		pc_obs_conn_name(c->obs, c->argv[2], c->argl[2]);
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
		/* S165: a known subcommand with the wrong arguments is the
		 * client's bug, answered correctly - only a name we do not
		 * implement is a gap */
		if (c->nargs >= 2 &&
		    !resp_is(c->argv[1], c->argl[1], "SETNAME") &&
		    !resp_is(c->argv[1], c->argl[1], "LIST") &&
		    !resp_is(c->argv[1], c->argl[1], "GETNAME"))
			unk_sub(c);
		resp_err(c->out, "unsupported CLIENT subcommand");
	}
	return 0;
}

static int rh_slowlog(struct resp_ctx *c)
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
		unk_sub(c);                    /* S165 */
		resp_err(c->out, "unsupported SLOWLOG subcommand");
	}
	return 0;
}

static int rh_select(struct resp_ctx *c)
{
	if (c->nargs != 2 || c->argl[1] == 0 || c->argl[1] >= 40) {
		resp_err(c->out, "DB index is out of range");
		return 0;
	}
	{
		char nm[64];
		size_t nl = resp_map_index(c->argv[1], c->argl[1], nm,
			sizeof nm);   /* S69 */

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

/* S48: MEMORY USAGE <key> [SAMPLES n] - their tooling's 76 calls/day.
 * The answer is the record's own bytes: the 24-byte record header plus
 * key plus value.  Slab-class rounding is NOT included - same estimate
 * class as Redis's own answer.  SAMPLES is accepted and ignored (it
 * concerns aggregate types). */
static int rh_memory(struct resp_ctx *c)
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

static int rh_keys(struct resp_ctx *c)
{
	if (c->nargs != 2 || c->argl[1] >= 256) {
		resp_err(c->out, "wrong number of arguments for 'keys' "
			"command");
		return 0;
	}
	/* S40: KEYS held this worker for the WHOLE walk - 12-33ms
	 * measured - and every other connection multiplexed here ate
	 * it as tail latency (7.9ms p99 photographed on a user
	 * host).  The verb now only DECLARES the walk; the proto
	 * layer runs it one bounded chunk per event-loop turn and
	 * the worker keeps serving between chunks. */
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

static int rh_exists(struct resp_ctx *c)
{
	long long cnt = 0;
	str k;
	int i;

	if (c->nargs < 2) {
		resp_err(c->out, "wrong number of arguments for 'exists' "
			"command");
		return 0;
	}
	for (i = 1; i < c->nargs; i++) {
		if (c->argl[i] == 0 || c->argl[i] > KEY_MAX)
			continue;
		k.s = c->argv[i];
		k.len = (int)c->argl[i];
		if (pcache_ht_probe(c->ht, &k, NULL, NULL, NULL) == 0)
			cnt++;
	}
	resp_int(c->out, cnt);
	return 0;
}

static int rh_type(struct resp_ctx *c)
{
	str k;

	if (c->nargs != 2 || c->argl[1] == 0 || c->argl[1] > KEY_MAX) {
		resp_err(c->out, "wrong number of arguments for 'type' command");
		return 0;
	}
	k.s = c->argv[1];
	k.len = (int)c->argl[1];
	resp_simple(c->out, pcache_ht_probe(c->ht, &k, NULL, NULL, NULL) == 0 ?
		"string" : "none");
	return 0;
}

static int rh_ttl(struct resp_ctx *c)
{
	unsigned int vln, exp;
	int is_ctr, rc;
	long long nv;
	str k;

	if (c->nargs != 2 || c->argl[1] == 0 || c->argl[1] > KEY_MAX) {
		resp_err(c->out, "wrong number of arguments for 'ttl' command");
		return 0;
	}
	k.s = c->argv[1];
	k.len = (int)c->argl[1];
	rc = pcache_ht_probe(c->ht, &k, &vln, &exp, &is_ctr);
	if (rc == -2)
		nv = -2;
	else if (exp == 0)
		nv = -1;
	else
		nv = (long long)exp - (long long)get_ticks();
	if (nv > 0 && resp_is(c->cmd, c->cl, "PTTL"))
		nv *= 1000;
	resp_int(c->out, nv);
	return 0;
}

static int rh_mget(struct resp_ctx *c)
{
	unsigned int exp;
	str k, v;
	int i;

	if (c->nargs < 2) {
		resp_err(c->out, "wrong number of arguments for 'mget' "
			"command");
		return 0;
	}
	/* local fetches, misses stay nil - the exact text-mget
	 * semantics (batch verbs never pull) */
	resp_arr(c->out, c->nargs - 1);
	for (i = 1; i < c->nargs; i++) {
		if (c->argl[i] == 0 || c->argl[i] > KEY_MAX) {
			resp_nil(c->out);
			continue;
		}
		k.s = c->argv[i];
		k.len = (int)c->argl[i];
		if (pcache_ht_fetch_ex(c->ht, &k, &v, &exp, NULL) == 0) {
			resp_bulk(c->out, v.s, (size_t)v.len);
			free(v.s);
		} else {
			resp_nil(c->out);
		}
	}
	return 0;
}

static int rh_get(struct resp_ctx *c)
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
	if (rc == PC_OP_ERR_GET) {
		resp_err(c->out, "get failed");
	} else {
		resp_nil(c->out);              /* miss; also the park fallback */
	}
	return 0;
}

static int rh_del(struct resp_ctx *c)
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
	/* multi-key: each op runs; a forwarded delete still executes
	 * at its owner but its ack completes into nobody, so it
	 * counts optimistically - stated, not hidden */
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

static int rh_expire(struct resp_ctx *c)
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
	/* S48: the AT forms carry an absolute wall-clock deadline;
	 * convert to relative AT THE BOUNDARY - the TTL machinery is
	 * tick-based and never learns wall time (the WAL's absolute
	 * stamps are its own conversion).  A past deadline falls into
	 * the ttl<=0 delete branch below, which is Redis's own
	 * semantics for it. */
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

static int rh_set(struct resp_ctx *c)
{
	int is_setex = resp_is(c->cmd, c->cl, "SETEX");
	int is_psetex = resp_is(c->cmd, c->cl, "PSETEX");
	int vi = (is_setex || is_psetex) ? 3 : 2, i;
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
			} else {
				/* NX/XX/KEEPTTL/GET/EXAT/PXAT: not in
				 * cut 1 - refuse, never half-honour */
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
	rc = op_set(c->ht, &k, &v, ttl, c->park_req, 0);
	if (rc == PC_OP_OK)
		resp_simple(c->out, "OK");
	else if (rc == PC_OP_PARKED)
		resp_err(c->out, "busy");         /* park fallback only */
	else if (rc == PC_OP_ERR_BUSY)
		/* Redis Cluster's own retryable code: a stock client
		 * backs off and retries instead of aborting, which is
		 * what a full parked-request table deserves - the
		 * request has NOT happened and trying again is
		 * correct.  redis-benchmark aborts on -ERR. */
		resp_err_code(c->out, "TRYAGAIN", "cluster busy, retry");
	else if (rc == PC_OP_ERR_FWD)
		resp_err(c->out, "forward failed");
	else if (rc == PC_OP_ERR_WRFAIL)
		/* not TRYAGAIN: retrying here cannot help, the node
		 * is out of the map until an operator fixes it */
		resp_err(c->out, PC_WRFAIL_MSG);
	else if (rc == PC_OP_ERR_FULL)
		resp_err(c->out, "cache full");
	else
		resp_err(c->out, "value too large");
	return 0;
}

static int rh_incr(struct resp_ctx *c)
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
		/* Redis Cluster's own retryable code: a stock client
		 * backs off and retries instead of aborting, which is
		 * what a full parked-request table deserves - the
		 * request has NOT happened and trying again is
		 * correct.  redis-benchmark aborts on -ERR. */
		resp_err_code(c->out, "TRYAGAIN", "cluster busy, retry");
	else if (rc == PC_OP_ERR_FWD)
		resp_err(c->out, "forward failed");
	else if (rc == PC_OP_ERR_NOTINT)
		resp_err(c->out, "value is not an integer or out of range");
	else if (rc != PC_OP_OK)
		resp_err(c->out, "cache full");
	else
		resp_int(c->out, nv);
	return 0;
}

static int rh_auth(struct resp_ctx *c)
{
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
		resp_err(c->out, "Client sent AUTH, but no password is set");
		return 0;
	}
	if (c->nargs == 2) {
		pw = c->argv[1];
		pwn = c->argl[1];
	} else if (c->nargs == 3) {
		pw = c->argv[2];
		pwn = c->argl[2];
	} else {
		resp_err(c->out, "wrong number of arguments for 'auth' "
			"command");
		return 0;
	}
	if (pc_resp_password_ok(pw, pwn)) {
		*c->authed = 1;
		resp_simple(c->out, "OK");
	} else {
		PC_RESP_BUMP(pc_resp_authfail);
		pc_jw_lit(c->out, "-WRONGPASS invalid username-password "
			"pair or user is disabled.\r\n");
	}
	return 0;
}

static int rh_hello(struct resp_ctx *c)
{
	long long ver = 2;

	if (c->nargs > 1 && resp_ll(c->argv[1], c->argl[1], &ver) < 0) {
		resp_err(c->out, "Protocol version is not an integer or "
			"out of range");
		return 0;
	}
	if (ver != 2) {
		pc_jw_lit(c->out, "-NOPROTO unsupported protocol version\r\n");
		return 0;
	}
	resp_arr(c->out, 14);
	resp_bulk_lit(c->out, "server");
	resp_bulk_lit(c->out, "perfcached");
	resp_bulk_lit(c->out, "version");
	resp_bulk(c->out, PC_VERSION, strlen(PC_VERSION));
	resp_bulk_lit(c->out, "proto");
	resp_int(c->out, 2);
	resp_bulk_lit(c->out, "id");
	resp_int(c->out, 0);
	resp_bulk_lit(c->out, "mode");
	resp_bulk_lit(c->out, "standalone");
	resp_bulk_lit(c->out, "role");
	resp_bulk_lit(c->out, "master");
	resp_bulk_lit(c->out, "modules");
	resp_arr(c->out, 0);
	return 0;
}

static int rh_cluster(struct resp_ctx *c)
{
	struct pc_member mem[PC_CL_MAXMEMBERS];
	int nm = pc_cluster_members(mem, PC_CL_MAXMEMBERS);
	const char *sub = c->nargs >= 2 ? c->argv[1] : "";
	size_t subl = c->nargs >= 2 ? c->argl[1] : 0;

	/* KEYSLOT is answerable with no cluster at all, and is how
	 * an operator checks our slot maths against real Redis. */
	if (resp_is(sub, subl, "KEYSLOT")) {
		if (c->nargs != 3) {
			resp_err(c->out, "wrong number of arguments for "
				"'cluster|keyslot' command");
			return 0;
		}
		resp_int(c->out, pc_key_slot(c->argv[2], c->argl[2]));
		return 0;
	}
	if (resp_is(sub, subl, "MYID")) {
		char id[40];
		int i;

		for (i = 0; i < nm && !mem[i].is_self; i++)
			;
		if (i == nm) {
			resp_err(c->out, "this node is not in a cluster");
			return 0;
		}
		resp_cl_id(id, &mem[i]);
		resp_bulk(c->out, id, 40);
		return 0;
	}
	if (resp_is(sub, subl, "INFO")) {
		struct pc_jw b;
		int on = resp_cluster_on();   /* S74: one truth, INFO reads it too */

		pc_jw_init(&b, c->scratch, c->scratch_cap);
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
			resp_err(c->out, "cluster info too large");
		else
			resp_bulk(c->out, b.buf, b.len);
		return 0;
	}

	if (!nm) {
		resp_err(c->out, "This instance has cluster support "
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
			resp_err(c->out, "out of memory building the "
				"slot map");
			return 0;
		}
		resp_cl_ownmap(mem, nm, ownv);
		/* worst case is one range per slot (~200KB of text);
		 * sized for the common case, grows if wrong */
		if (pc_jw_init_heap(&tw, (size_t)resp_cl_runs(ownv, -1)
		        * 13 + (size_t)nm * 128) != 0) {
			free(ownv);
			resp_err(c->out, "out of memory building the "
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
			resp_err(c->out, "cluster nodes reply too large");
		else
			resp_bulk(c->out, tw.buf, tw.len);
		pc_jw_free(&tw);
		return 0;
	}
	if (resp_is(sub, subl, "SLOTS") || resp_is(sub, subl, "SHARDS")) {
		int is_shards = resp_is(sub, subl, "SHARDS");
		unsigned long long key = resp_clmap_key(mem, nm, is_shards);
		struct pc_jw tw;
		unsigned char *ownv;
		int i, s, nr;

		/* S75: serve the last build while the membership
		 * snapshot's key still matches */
		if (resp_clmap_cached(is_shards, key, c->out)) {
			PC_RESP_BUMP(pc_resp_slots_hits);
			return 0;
		}
		/* tw is a STACK local: pc_jw_init_heap() refuses a
		 * writer whose len is nonzero, and dirty stack reads
		 * as exactly that (see CLUSTER NODES) */
		memset(&tw, 0, sizeof tw);
		/* 16KB, allocated per call rather than kept in a
		 * static: workers are THREADS, and a shared scratch
		 * buffer here would corrupt one reply with another's
		 * walk under any concurrency at all. */
		ownv = malloc(PC_SLOTS);
		if (!ownv) {
			resp_err(c->out, "out of memory building the "
				"slot map");
			return 0;
		}
		resp_cl_ownmap(mem, nm, ownv);
		nr = resp_cl_runs(ownv, -1);
		/* SLOTS is the one reply whose size a request does
		 * not bound: it enumerates the slot space, and
		 * rendezvous scatters ownership, so a 4-node fleet
		 * already exceeds the fixed per-worker scratch.  A
		 * heap buffer sized for the ranges about to be
		 * written - ~101 bytes each at the widest (5-digit
		 * slots, a 15-char address, a 40-char name) - so the
		 * common case never reallocs. */
		if (pc_jw_init_heap(&tw, (size_t)nr * 104 +
		        (size_t)nm * 256 + 64) != 0) {
			free(ownv);
			resp_err(c->out, "could not build the slot map");
			return 0;
		}
		if (!is_shards) {
			resp_arr(&tw, nr);
			for (s = 0; s < PC_SLOTS; s++) {
				int e;

				if (ownv[s] == 0xFF)
					continue;
				if (s && ownv[s - 1] == ownv[s])
					continue;
				for (e = s; e + 1 < PC_SLOTS &&
				        ownv[e + 1] == ownv[s]; e++)
					;
				resp_arr(&tw, 3);
				resp_int(&tw, s);
				resp_int(&tw, e);
				resp_cl_node(&tw, &mem[ownv[s]]);
			}
		} else {
			/* SHARDS groups the same ranges by owner,
			 * writing the node block once instead of once
			 * per range - the difference between a ~130KB
			 * and a ~770KB reply on a three-node fleet. */
			resp_arr(&tw, nm);
			for (i = 0; i < nm; i++) {
				char id[40], ip[INET_ADDRSTRLEN];

				nr = resp_cl_runs(ownv, i);
				resp_arr(&tw, 4);
				resp_bulk(&tw, "slots", 5);
				resp_arr(&tw, nr * 2);
				for (s = 0; s < PC_SLOTS; s++) {
					int e;

					if (ownv[s] != i)
						continue;
					if (s && ownv[s - 1] == ownv[s])
						continue;
					for (e = s; e + 1 < PC_SLOTS &&
					        ownv[e + 1] == ownv[s]; e++)
						;
					resp_int(&tw, s);
					resp_int(&tw, e);
				}
				resp_bulk(&tw, "nodes", 5);
				resp_arr(&tw, 1);
				resp_cl_id(id, &mem[i]);
				if (!inet_ntop(AF_INET, &mem[i].addr, ip,
				        sizeof(ip)))
					ip[0] = 0;
				resp_arr(&tw, 14);
				resp_bulk(&tw, "id", 2);
				resp_bulk(&tw, id, 40);
				resp_bulk(&tw, "port", 4);
				resp_int(&tw, resp_cl_port(&mem[i]));
				resp_bulk(&tw, "ip", 2);
				resp_bulk(&tw, ip, strlen(ip));
				resp_bulk(&tw, "endpoint", 8);
				resp_bulk(&tw, ip, strlen(ip));
				/* every node owns its slots outright -
				 * there are no replicas of a shard here -
				 * so the only honest role is master */
				resp_bulk(&tw, "role", 4);
				resp_bulk(&tw, "master", 6);
				resp_bulk(&tw, "replication-offset", 18);
				resp_int(&tw, 0);
				resp_bulk(&tw, "health", 6);
				resp_bulk(&tw, "online", 6);
			}
		}
		free(ownv);
		PC_RESP_BUMP(pc_resp_slots_builds);
		if (tw.overflow) {
			resp_err(c->out, "could not build the slot map");
		} else {
			resp_clmap_store(is_shards, key, tw.buf, tw.len);
			if (pc_jw_init_heap(c->out, tw.len + 16) != 0)
				resp_err(c->out, "could not build the "
					"slot map");
			else
				pc_jw_raw(c->out, tw.buf, tw.len);
		}
		pc_jw_free(&tw);
		return 0;
	}
	if (resp_is(sub, subl, "COUNTKEYSINSLOT")) {
		/* the count is not tracked per slot, and answering
		 * a made-up number would be worse than refusing */
		unk_sub(c);                    /* S165: refused on purpose, still a gap */
		resp_err(c->out, "unsupported CLUSTER subcommand "
			"(keys are not counted per slot)");
		return 0;
	}
	unk_sub(c);                            /* S165 */
	resp_err(c->out, "unsupported CLUSTER subcommand");
	return 0;
}

static int rh_info(struct resp_ctx *c)
{
	struct pc_jw b;
	const char *sec = c->nargs >= 2 ? c->argv[1] : NULL;
	size_t secl = c->nargs >= 2 ? c->argl[1] : 0;
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
	pc_jw_init(&b, c->scratch, c->scratch_cap);
	if (INFO_WANT("server")) {
		/* S74: redis_mode is where a cluster-aware client
		 * decides whether to fetch the slot map at all */
		pc_jw_lit(&b, "# Server\r\nredis_version:7.0.0\r\n"
			"redis_mode:");
		pc_jw_lit(&b, resp_cluster_on() ? "cluster" : "standalone");
		pc_jw_lit(&b, "\r\nperfcached_version:" PC_VERSION "\r\n"
			"perfcached_dialect:resp2-compat\r\n");
	}
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
	/* S74: the other field clients decide from.  A section that
	 * is absent or empty reads as "no cluster" to anything that
	 * parses it - which it was. */
	if (INFO_WANT("cluster")) {
		pc_jw_lit(&b, "# Cluster\r\ncluster_enabled:");
		pc_jw_i64(&b, resp_cluster_on());
		pc_jw_lit(&b, "\r\n");
	}
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
			if (!pc_store_live(ci))
				continue;   /* S69: a dropped collection */
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
	if (all || (sec && resp_is(sec, secl, "latencystats"))) {
		pc_jw_lit(&b, "# Latencystats\r\n");   /* S159 */
		pc_obs_latencystats(&b);
	}
	if (INFO_WANT("keyspace")) {
		int ci;

		/* S69: EVERY collection this door can reach, each under
		 * the index that reaches it.  Reporting only the
		 * selected one made a node holding live data read as
		 * one empty database to every Redis-native monitor - a
		 * flat zero line through an outage and a normal day
		 * alike. */
		pc_jw_lit(&b, "# Keyspace\r\n");
		for (ci = 0; ci < pc_store_count(); ci++) {
			pcache_ht_totals_t tot;
			const char *nm;
			char idx[64];
			size_t il;

			if (!pc_store_live(ci))
				continue;
			nm = pc_store_name(ci);
			if (!resp_col_allowed(nm, strlen(nm)))
				continue;
			pcache_ht_totals(pc_store_ht(ci), &tot);
			if (!tot.entries)
				continue;   /* Redis omits empty dbs */
			il = resp_index_for(nm, strlen(nm), idx,
				sizeof idx);
			pc_jw_lit(&b, "db");
			pc_jw_raw(&b, idx, il);
			pc_jw_lit(&b, ":keys=");
			pc_jw_i64(&b, (long long)tot.entries);
			pc_jw_lit(&b, ",expires=0,avg_ttl=0\r\n");
		}
	}
#undef INFO_WANT
	if (b.overflow)
		resp_err(c->out, "info too large");
	else
		resp_bulk(c->out, b.buf, b.len);
	return 0;
}

/* S48: TIME is collection-independent (17.2M calls/day measured on
 * the realtime instance) - answer before the collection resolves */
static int rh_time(struct resp_ctx *c)
{
	struct timespec tw;
	char sb[24], ub[24];
	int sn, un;

	clock_gettime(CLOCK_REALTIME, &tw);
	sn = snprintf(sb, sizeof sb, "%lld", (long long)tw.tv_sec);
	un = snprintf(ub, sizeof ub, "%ld", tw.tv_nsec / 1000);
	resp_arr(c->out, 2);
	resp_bulk(c->out, sb, (size_t)sn);
	resp_bulk(c->out, ub, (size_t)un);
	return 0;
}

/* ---- S86: RedisJSON on the RESP door ---------------------------
 * JSON.SET key path value [NX|XX] [EX seconds], JSON.GET key [path],
 * JSON.DEL key [path], JSON.NUMINCRBY key path n, JSON.ARRAPPEND key
 * path value..., JSON.DEBUG HELP | MEMORY key [path] - mapped onto
 * the native path operations (pc_json_rmw: a read-modify-write
 * under the key's mutex stripe).  Paths are the engine's pinned
 * subset: `$`, `.name`, `[index]`; RedisJSON's two spellings are
 * accepted - a `$` path answers v2 style (a JSON array of the
 * matches), a legacy `.path` or bare `path` answers the bare value.
 * EX is an extension: the native set takes a TTL, so JSON.SET +
 * EXPIRE collapse into one round trip; without it the key's expiry
 * is preserved, as RedisJSON does.  Served where the key is local
 * (store and eager collections; a shard owner or proxy holder); a
 * key this node would have to forward is refused by name rather
 * than half-served - the native door forwards it. */
static int rh_json_debug(struct resp_ctx *c)
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
			free(frag);
			resp_err_code(c->out, "WRONGTYPE", "the value at this key "
				"is not a JSON document");
			return 0;
		}
		resp_int(c->out, rc2 == 1 ? 0 : fraglen);
		free(frag);
		return 0;
	}
	resp_err(c->out, "JSON.DEBUG: use HELP or MEMORY <key> [path]");
	return 0;
}

static int rh_json(struct resp_ctx *c)
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
	if (c->nargs >= 3 && !(op == PC_JOP_GET && c->nargs == 2)) {
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
				        jttl <= 0) {
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
		rc = op_del(c->ht, &k, c->park_req);
		resp_int(c->out, rc == PC_OP_OK ? 1 : 0);
		return 0;
	}
	if (op == PC_JOP_APPEND) {
		/* one value per call in the engine; RedisJSON takes many */
		int total = 0;

		for (i = 3; i < c->nargs; i++) {
			rc2 = pc_json_rmw(c->ht, k.s, k.len, PC_JOP_APPEND,
				jpath, jplen, c->argv[i], (int)c->argl[i], 0, 0, 0,
				0, 0, 0, &frag, &fraglen, &nv, &cnt, &fmsg);
			if (rc2 < 0) {
				resp_err(c->out, fmsg);
				return 0;
			}
			total = cnt;
		}
		if (v2) {
			resp_arr(c->out, 1);
			resp_int(c->out, total);
		} else
			resp_int(c->out, total);
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
		if (fmsg == jp_strerror(PC_JP_E_DOC))
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
		if (rc2 == 1) {
			/* a missing key is nil; a missing path is [] on a
			 * $ path and an error on a legacy one, as RedisJSON */
			if (rootpath)
				resp_nil(c->out);
			else if (v2)
				resp_bulk_lit(c->out, "[]");
			else
				resp_err(c->out, "path does not exist");
		} else if (rootpath &&
		        !pc_jp_valid_value(frag, (size_t)fraglen)) {
			/* the whole value came back and it is not JSON: a
			 * plain SET lives here, not a document */
			resp_err_code(c->out, "WRONGTYPE", "the value at this key "
				"is not a JSON document");
		} else if (v2) {
			pc_jw_lit(c->out, "$");
			pc_jw_i64(c->out, (long long)fraglen + 2);
			pc_jw_lit(c->out, "\r\n[");
			pc_jw_raw(c->out, frag, (size_t)fraglen);
			pc_jw_lit(c->out, "]\r\n");
		} else
			resp_bulk(c->out, frag, (size_t)fraglen);
		free(frag);
	}
	return 0;
}

static int rh_scan(struct resp_ctx *c)
{
	struct scan_ctx sc;
	struct pc_jw ew;
	unsigned int cursor;
	long long cur, count = 128;
	char curbuf[16];
	int i, n;

	if (c->nargs < 2 || resp_ll(c->argv[1], c->argl[1], &cur) < 0 ||
	        cur < 0 || cur > 0xFFFFFFFFLL) {
		resp_err(c->out, "invalid cursor");
		return 0;
	}
	memset(&sc, 0, sizeof sc);
	pc_jw_init(&ew, c->scratch, c->scratch_cap);
	sc.w = &ew;
	sc.patlen = -1;
	sc.now = get_ticks();
	sc.limit = 0x7FFFFFFF;
	for (i = 2; i < c->nargs; i++) {
		if (resp_is(c->argv[i], c->argl[i], "MATCH") &&
		        i + 1 < c->nargs && c->argl[i + 1] < 256) {
			sc.pat = c->argv[i + 1];
			sc.patlen = (int)c->argl[i + 1];
			i++;
		} else if (resp_is(c->argv[i], c->argl[i], "COUNT") &&
		        i + 1 < c->nargs) {
			if (resp_ll(c->argv[i + 1], c->argl[i + 1],
			        &count) < 0 || count < 1 ||
			        count > 16384) {
				resp_err(c->out, "invalid COUNT");
				return 0;
			}
			i++;
		} else if (resp_is(c->argv[i], c->argl[i], "TYPE") &&
		        i + 1 < c->nargs) {
			i++;           /* strings only: accept, ignore */
		} else {
			resp_err(c->out, "syntax error");
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
			pcache_ht_scan_ex(c->ht, &cursor, chunk,
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
		resp_err(c->out, "reply too large");
		return 0;
	}
	n = snprintf(curbuf, sizeof curbuf, "%u", cursor);
	resp_arr(c->out, 2);
	resp_bulk(c->out, curbuf, (size_t)n);
	resp_arr(c->out, sc.emitted);
	pc_jw_raw(c->out, ew.buf, ew.len);
	return 0;
}

static const struct resp_cmd resp_cmds[] = {
	/* connection plane, in the order the chain tries them */
	{ "PING", RESP_CONN, rh_ping }, { "ECHO", RESP_CONN, rh_echo },
	{ "QUIT", RESP_CONN, rh_quit },
	{ "AUTH", RESP_CONN, rh_auth }, { "HELLO", RESP_CONN, rh_hello },
	/* PS2: pub/sub lives on the connection plane - a subscriber is a
	 * socket, and a publish touches no collection */
	{ "SUBSCRIBE", RESP_CONN, rh_subscribe },
	{ "UNSUBSCRIBE", RESP_CONN, rh_unsubscribe },
	{ "PSUBSCRIBE", RESP_CONN, rh_psubscribe },
	{ "PUNSUBSCRIBE", RESP_CONN, rh_punsubscribe },
	{ "PUBLISH", RESP_CONN, rh_publish },
	{ "PUBSUB", RESP_CONN, rh_pubsub },
	{ "RESET", RESP_CONN, rh_reset },
	{ "COMMAND", RESP_CONN, rh_command },
	{ "CONFIG", RESP_CONN, rh_config },
	{ "CLUSTER", RESP_CONN, rh_cluster },
	{ "CLIENT", RESP_CONN, rh_client },
	{ "SLOWLOG", RESP_CONN, rh_slowlog },
	{ "SELECT", RESP_CONN, rh_select }, { "INFO", RESP_CONN, rh_info },
	/* ready plane: a serving node, but no collection of their own */
	{ "DBSIZE", RESP_READY, rh_dbsize }, { "TIME", RESP_READY, rh_time },
	{ "FLUSHDB", RESP_READY, rh_flush },
	{ "FLUSHALL", RESP_READY, rh_flush },
	/* data plane: this was dc[] */
	{ "GET", RESP_DATA | RESP_KEYED, rh_get }, { "SET", RESP_DATA | RESP_KEYED, rh_set },
	{ "SETEX", RESP_DATA | RESP_KEYED, rh_set }, { "PSETEX", RESP_DATA | RESP_KEYED, rh_set },
	{ "DEL", RESP_DATA | RESP_KEYED, rh_del }, { "UNLINK", RESP_DATA | RESP_KEYED, rh_del },
	{ "EXISTS", RESP_DATA | RESP_KEYED, rh_exists }, { "TYPE", RESP_DATA | RESP_KEYED, rh_type },
	{ "EXPIRE", RESP_DATA | RESP_KEYED, rh_expire }, { "PEXPIRE", RESP_DATA | RESP_KEYED, rh_expire },
	{ "EXPIREAT", RESP_DATA | RESP_KEYED, rh_expire }, { "PEXPIREAT", RESP_DATA | RESP_KEYED, rh_expire },
	{ "TTL", RESP_DATA | RESP_KEYED, rh_ttl }, { "PTTL", RESP_DATA | RESP_KEYED, rh_ttl },
	{ "INCR", RESP_DATA | RESP_KEYED, rh_incr }, { "DECR", RESP_DATA | RESP_KEYED, rh_incr },
	{ "INCRBY", RESP_DATA | RESP_KEYED, rh_incr }, { "DECRBY", RESP_DATA | RESP_KEYED, rh_incr },
	{ "MGET", RESP_DATA, rh_mget }, { "KEYS", RESP_DATA, rh_keys },
	{ "SCAN", RESP_DATA, rh_scan }, { "MEMORY", RESP_DATA, rh_memory },
	/* S86: RedisJSON's surface over the native path ops */
	{ "JSON.SET", RESP_DATA | RESP_KEYED, rh_json }, { "JSON.GET", RESP_DATA | RESP_KEYED, rh_json },
	{ "JSON.DEL", RESP_DATA | RESP_KEYED, rh_json },
	{ "JSON.NUMINCRBY", RESP_DATA | RESP_KEYED, rh_json },
	{ "JSON.ARRAPPEND", RESP_DATA | RESP_KEYED, rh_json },
	{ "JSON.DEBUG", RESP_DATA, rh_json_debug }
};

/* RV-10: -MOVED <slot> <ip>:<port> for a key @node owns.  0 when the
 * member cannot be named - the caller then forwards, as it always has:
 * a redirect must never turn a request the fleet can serve into an error */
static int resp_moved(struct pc_jw *out, int node, unsigned int slot)
{
	struct pc_member mem[PC_CL_MAXMEMBERS];
	char ip[INET_ADDRSTRLEN];
	int n = pc_cluster_members(mem, PC_CL_MAXMEMBERS), i;

	for (i = 0; i < n; i++) {
		int port;

		if (mem[i].node != node)
			continue;
		port = mem[i].resp_port ? mem[i].resp_port : mem[i].client_port;
		if (port <= 0 || !inet_ntop(AF_INET, &mem[i].addr, ip, sizeof ip))
			return 0;
		pc_jw_lit(out, "-MOVED ");
		pc_jw_i64(out, (long long)slot);
		pc_jw_lit(out, " ");
		pc_jw_raw(out, ip, strlen(ip));
		pc_jw_lit(out, ":");
		pc_jw_i64(out, port);
		pc_jw_lit(out, "\r\n");
		return 1;
	}
	return 0;
}

static const struct resp_cmd *resp_cmd_find(const char *cmd, size_t cl)
{
	size_t i;

	for (i = 0; i < sizeof resp_cmds / sizeof resp_cmds[0]; i++)
		if (resp_is(cmd, cl, resp_cmds[i].name))
			return &resp_cmds[i];
	return NULL;
}

int pc_verb_resp(char *const *argv, const size_t *argl, int nargs,
		struct pc_jw *out, char *scratch, size_t scratch_cap,
		unsigned int *park_req, const char **colp, size_t *collenp,
		const char **keyp, size_t *klenp, char *cur_col,
		size_t *cur_collen, int *quit, int *authed,
		struct pc_enum_start *es, void *obs, void *conn)
{
	pcache_htable_t *ht;
	const char *cmd;
	size_t cl;

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
		/* S165: an unknown command before AUTH is counted, never
		 * named - that is where port scanners live */
		if (!resp_cmd_find(cmd, cl))
			pc_obs_unknown_preauth(CLUNK_RESP);
		pc_jw_lit(out, "-NOAUTH Authentication required.\r\n");
		return 0;
	}
	/* PS2: SUBSCRIBED MODE, exactly redis's gate - predis's pubSubLoop
	 * breaks on anything else answering on a subscribed connection */
	if (pc_pubsub_count(conn) > 0 &&
	    !resp_is(cmd, cl, "SUBSCRIBE") && !resp_is(cmd, cl, "UNSUBSCRIBE") &&
	    !resp_is(cmd, cl, "PSUBSCRIBE") && !resp_is(cmd, cl, "PUNSUBSCRIBE") &&
	    !resp_is(cmd, cl, "PING") && !resp_is(cmd, cl, "QUIT") &&
	    !resp_is(cmd, cl, "RESET")) {
		char lc[64];
		size_t i, n = cl < sizeof lc - 1 ? cl : sizeof lc - 1;

		for (i = 0; i < n; i++)
			lc[i] = (char)tolower((unsigned char)cmd[i]);
		lc[n] = 0;
		pc_jw_lit(out, "-ERR Can't execute '");
		pc_jw_lit(out, lc);
		pc_jw_lit(out, "': only (P|S)SUBSCRIBE / (P|S)UNSUBSCRIBE / "
			"PING / QUIT / RESET are allowed in this context\r\n");
		return 0;
	}

	/* ---- connection plane ----
	 * Every command is answered from its row's plane now - the chain of
	 * if (resp_is(...)) blocks this replaced is gone.  Name matching
	 * makes the commands mutually exclusive, but that alone does NOT
	 * make a plane safe to change: a command may only be answered from
	 * the same side of BOTH gates - the readiness gate and the
	 * collection resolution - as the block it replaced, which is what
	 * the three planes encode. */
	{
		const struct resp_cmd *row = resp_cmd_find(cmd, cl);

		if (row && row->fn && (row->flags & RESP_CONN)) {
			struct resp_ctx c = { argv, argl, nargs, out, scratch,
				scratch_cap, park_req, colp, collenp, keyp,
				klenp, cur_col, cur_collen, quit, authed, es,
				obs, NULL, cmd, cl, conn };

			return row->fn(&c);
		}
	}

	/* C7.  RESP gets Redis's OWN error for this condition: -LOADING is
	 * what a real server sends while it reads its dataset, so redis-py,
	 * jedis, phpredis and hiredis-based clients already recognise it
	 * and retry rather than surfacing it as a hard failure.  Using our
	 * own error string here would be compatible in form and useless in
	 * practice.
	 *
	 * S138: it has to go out through resp_err_code().  resp_err()
	 * hardcodes -ERR, so the condition arrived as `-ERR LOADING ...`,
	 * where the code a client dispatches on is ERR and LOADING is just
	 * the first word of a message nobody parses - the same reason S38
	 * gave this emitter to TRYAGAIN. */
	if (serving_denied()) {
		resp_err_code(out, "LOADING", PC_NOTREADY_MSG);
		return 0;
	}

	/* ---- ready plane ----
	 * Past the readiness gate, before any collection resolves.  These
	 * need a node that is serving but no collection of their own:
	 * DBSIZE reports the keyspace, FLUSHDB would destroy it, and TIME
	 * is collection-independent (S48) but still owed the gate. */
	{
		const struct resp_cmd *row = resp_cmd_find(cmd, cl);

		if (row && row->fn && (row->flags & RESP_READY)) {
			struct resp_ctx c = { argv, argl, nargs, out, scratch,
				scratch_cap, park_req, colp, collenp, keyp,
				klenp, cur_col, cur_collen, quit, authed, es,
				obs, NULL, cmd, cl, conn };

			return row->fn(&c);
		}
	}

	/* ---- data plane ----
	 * Recognize the command BEFORE resolving the collection: an
	 * unknown command must answer "unknown command" even on a daemon
	 * with no collection named "0" (found by prototest's XYZZY probe
	 * answering "no such collection"). */
	{
		const struct resp_cmd *row = resp_cmd_find(cmd, cl);

		if (!row || !(row->flags & RESP_DATA)) {
			char nb[64];
			size_t n = cl < sizeof nb - 1 ? cl : sizeof nb - 1;

			memcpy(nb, cmd, n);
			nb[n] = 0;
			pc_obs_unknown(CLUNK_RESP, cmd, cl, NULL, 0, obs);   /* S165 */
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

	/* the data plane's own dispatch: these handlers need the collection,
	 * so they cannot be answered at the site above (S140) */
	{
		const struct resp_cmd *row = resp_cmd_find(cmd, cl);

		/* RV-10: resp_redirect = moved.  Under shard and spread a
		 * key's home is computable, and a cluster-aware Redis client
		 * computed one from CLUSTER SLOTS - it refreshes that table
		 * on -MOVED and on nothing else, so a forward (S44's
		 * default) leaves it stale for ever after a rebalance.
		 * With the policy on, a key this node does not own is
		 * answered with the redirect and NOT forwarded.  Off, or
		 * when the owner cannot be named, the forward stands. */
		if (resp_redirect_moved && row && (row->flags & RESP_KEYED) &&
		        nargs >= 2 && argl[1] && pc_cluster_enabled()) {
			unsigned int slot = pc_key_slot(argv[1], argl[1]);
			int target = 0;

			if (pc_store_shard_enabled(ht))
				target = pc_shard_owner(cur_col, *cur_collen,
					argv[1], argl[1]);
			else if (pc_cluster_replicas() &&
			         !pc_spread_in_set(slot, 0, pc_cluster_replicas()))
				target = pc_spread_pick_peer(slot,
					pc_cluster_replicas());
			if (target && resp_moved(out, target, slot))
				return 0;
		}
		if (row && row->fn && (row->flags & RESP_DATA)) {
			struct resp_ctx c = { argv, argl, nargs, out, scratch,
				scratch_cap, park_req, colp, collenp, keyp,
				klenp, cur_col, cur_collen, quit, authed, es,
				obs, ht, cmd, cl, conn };

			return row->fn(&c);
		}
	}

	/* unreachable: the data-plane pre-check answered unknown commands */
	resp_err(out, "protocol");
	return 0;
}
