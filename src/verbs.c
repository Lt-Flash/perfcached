/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* verbs.c - the verb set: PC_VERB_CMD methods (pc_verb_cmd), the fixed-layout binary verbs
 * (pc_verb_bin) and RESP (pc_verb_resp), all through the same dialect-neutral op cores. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <strings.h>                        /* strncasecmp */
#include <time.h>

#include <pthread.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "reload.h"
#include "compat/dprint.h"
#include "compat/timer.h"
#include "json.h"
#include "jsonpath.h"
#include "hashrec.h"
#include "sha1.h"
#include "store.h"
#include "colname.h"
#include "winctr_tw.h"
#include "events.h"
#include "config.h"        /* PC_MAX_COLLECTIONS */
#include "verbs.h"
#include "upstream.h"
#include "respout.h"                    /* the RESP reply writers */
#include "verbsint.h"                   /* the families' shared interface */
#include "pcglob.h"                     /* pc_glob */
#include "upgrade.h"
#include <sodium.h>
#include "mail.h"
#include "core/pcache_htable.h"
#include "core/pcache_arena.h"
#include "wal.h"
#include "rdb.h"
#include "recover.h"
#include "compat/compat.h"
#include "cluster.h"
#include "clcol.h"          /* CLCOL_OP_* for the DDL announce */
#include "pc_slot.h"                   /* the shared key->slot */
#include "obs.h"                       /* the Grafana surfaces */
#include "proto.h"                     /* PC_VERB_* / PC_BIN_F_ERR */
#include "../lib/perfd_push.h"         /* PFP_* */
#include "pubsub.h"
#include "daemon.h"                    /* pc_worker_id, pc_wal_reprobe */
#include "version.h"
#include "respmap.h"                   /* map, unmap, maps */
#include "mgetfan.h"
#include "presence.h"

/* a method's failure: the message becomes the error frame's payload (no code) */
#define ERR(msg) do { *errmsg = (msg); return -1; } while (0)
#define JW_REPLY_INIT (1u << 20)           /* dump's heap reply, grows to PT_HEAP_MAX */

/* a probe that also takes an upstream PRESENT marker as the key (EXISTS / TTL / TYPE behind a
 * miss); rc and outputs as pcache_ht_probe's, for a value-less record */
int probe_or_present(pcache_htable_t *ht, const str *k,
		unsigned int *vln, unsigned int *exp, int *is_ctr,
		unsigned char *tflags)
{
	int rc = pcache_ht_probe(ht, k, vln, exp, is_ctr);

	if (rc != -2)
		return rc;
	if (pcache_ht_probe_present(ht, k, exp, tflags) != 0)
		return -2;
	if (vln)
		*vln = 0;
	if (is_ctr)
		*is_ctr = 0;
	return 0;
}
/* per-thread READ scratch, never freed (workers live as long as the daemon); VAL_MAX-sized, so
 * PCACHE_E_TOOSMALL cannot happen for a stored value */
static __thread char *get_scratch_v;

char *get_buf(void)
{
	if (!get_scratch_v)
		get_scratch_v = malloc(VAL_MAX);
	return get_scratch_v;
}

/* the hash edit scratch: an edit reads get_buf() and writes this (the largest record plus hashrec's
 * room).  Per thread, any thread running hash commands, never freed; HRANDFIELD borrows it too. */
static __thread unsigned char *hr_scratch_v;

static unsigned char *hr_buf(void)
{
	if (!hr_scratch_v)
		hr_scratch_v = malloc(PC_HR_MAX + 64);
	return hr_scratch_v;
}

/* a hash entry is at least 7 bytes and pc_hr_valid caps every record hcmd_exec reads at
 * PC_HR_MAX, so HRANDFIELD's index array (one u32 per field) fits the scratch it borrows */
_Static_assert((PC_HR_MAX - PC_HR_HDR) / 7 * sizeof(unsigned int) <=
	PC_HR_MAX + 64, "HRANDFIELD's index array must fit the edit scratch");
/* and a forwarded hash command's encoding, capped at the forward ceiling
 * (op_hcmd), borrows it too */
_Static_assert(PC_MAX_FWD_VAL <= PC_HR_MAX + 64,
	"a forwarded hash command must fit the edit scratch");

/* a test's pause between a probe's run and the read of its result */
static long long probe_gap_ms(void)
{
	static long long ms = -1;

	if (ms < 0) {
		const char *e = getenv("PERFCACHED_TEST_PROBE_GAP_MS");

		ms = e ? atoll(e) : 0;
		if (ms < 0)
			ms = 0;
	}
	return ms;
}

/* a delete or expire that did not happen: set's words for it, NULL otherwise */
static const char *op_fail_text(int rc)
{
	if (rc == PC_OP_ERR_BUSY)
		return "parked-request table full - retry";
	if (rc == PC_OP_ERR_FWD)
		return "forward failed";
	if (rc == PC_OP_ERR_WRFAIL)
		return wr_refused_msg();
	return NULL;
}

/* the most elements one mget or mset takes; the refusal texts name it */
#define MULTI_MAX 65536u

/* relative seconds -> absolute ticks (0 stays 0 = never); the doors refuse past PC_TTL_MAX */
static unsigned int ttl_to_abs(long long ttl)
{
	if (ttl <= 0)
		return 0;
	if (ttl > PC_TTL_MAX)
		ttl = PC_TTL_MAX;
	return get_ticks() + (unsigned int)ttl;
}

/* a relative ttl as a forward carries it (0 = none): never past PC_TTL_MAX, so never
 * PCACHE_EXP_PRESERVE - the doors refuse longer ones, this holds behind them */
static unsigned int fwd_ttl(long long ttl)
{
	return ttl <= 0 ? 0 : ttl > PC_TTL_MAX ? (unsigned int)PC_TTL_MAX : (unsigned int)ttl;
}

/* a bytes parameter read IN PLACE (its span in the request tree, no copy).  -1 = absent, not a
 * 'b', or over @cap */
static int get_bytes(const struct pc_tv *v, int map, const char *key,
		const char **p, size_t *n, size_t cap)
{
	int k = pc_tv_get(v, map, key);

	if (k < 0 || v->n[k].type != 'b' || v->n[k].len > cap)
		return -1;
	*p = (const char *)v->n[k].p;
	*n = v->n[k].len;
	return 0;
}

static int m_is(const char *method, size_t mlen, const char *name)
{
	return strlen(name) == mlen && !memcmp(method, name, mlen);
}

/* reverse lookup: the WAL logs by collection NAME */
const char *col_name_of(pcache_htable_t *ht)
{
	int i;

	/* a resize's old table names the collection its writes now land in */
	ht = pcache_ht_write_target(ht);
	for (i = 0; i < pc_store_count(); i++)
		if (pc_store_live(i) && pc_store_ht(i) == ht)
			return pc_store_name(i);
	return "?";
}

static pcache_htable_t *get_col(const struct pc_tv *v, int params,
		const char **errmsg)
{
	int tc = pc_tv_get(v, params, "col");
	pcache_htable_t *ht;

	if (tc < 0 || v->n[tc].type != 'b') {
		*errmsg = "missing col";
		return NULL;
	}
	ht = pc_store_find((const char *)v->n[tc].p, v->n[tc].len);
	if (!ht)
		*errmsg = "no such collection";
	return ht;
}

/* ---- dialect-neutral data-verb cores: every codec answers through these, so the cluster
 * semantics cannot drift; a codec owns only the reply shape (its park-failure one too) */


const char pc_wrongtype_msg[] =
	"WRONGTYPE Operation against a key holding the wrong kind of value";
const char pc_json_wrongtype_msg[] = "Existing key has wrong Redis type";

/* only a full parked-request table (PC_CLFAIL_BUSY) is backpressure worth a client's retry; the
 * other reasons a pc_*_begin() returns 0 are "forward failed" */
static int fwd_fail_code(void)
{
	return pc_cluster_last_fail() == PC_CLFAIL_BUSY
		? PC_OP_ERR_BUSY : PC_OP_ERR_FWD;
}

/* a fleet probe not begun because the parked-request table is full: refused for a retry, not
 * read as "no live peer", which places the key here or answers absent */
static int probe_busy(void)
{
	return pc_cluster_last_fail() == PC_CLFAIL_BUSY;
}

/* defined beside serving_denied(), where the two gates are explained */
static int writes_denied(void);
/* each data-verb core is a hot-key sample point (store.c keeps 1 in pc_store_hot_rate()); a SET
 * or INCR resumed after a park (pc_op_*_resume) was counted when it came in */

static int op_refused(pcache_htable_t *ht);
static int op_refused_col(const char *cn, size_t cl);

/* The miss tail, shared by both get flavours: everything that happens
 * once the local table has said "absent". */
int op_get_miss(pcache_htable_t *ht, str *k, unsigned int *park)
{
	if (pc_store_shard_enabled(ht)) {
		/* ownership: unicast the owner; its miss is AUTHORITATIVE (no broadcast, no
		 * negative cache) except in the reshard grace, when one broadcast covers it */
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
	/* EAGER: every node holds every record, so a READY node's miss is final, no pull
	 * (pc_cluster_eager_miss_final).  A value over PC_MAX_FWD_VAL misses until a sweep. */
	if (pc_store_eager_enabled(ht) && !pc_cluster_replicas() &&
	        pc_cluster_eager_miss_final())
		return PC_OP_ABSENT;
	/* a local miss asks the cluster - broadcast (store), locator unicast first (proxy) -
	 * unless the negative cache already heard "no" */
	/* spread: only K of P nodes hold it, so unicast the best-ranked live holder; with none
	 * live the broadcast below still runs (mid membership change the set may be stale) */
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

			if (!holder && pc_store_eager_enabled(ht))
				holder = pc_cluster_eager_doubt_one();

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

/* as op_get, but the value lands in @buf (valid until this thread's next get), nothing is
 * allocated; @cap must be VAL_MAX so a stored value always fits */
int op_get_buf(pcache_htable_t *ht, str *k, char *buf, unsigned int cap,
		unsigned int *vlen, unsigned int *exp, unsigned int *park)
{
	unsigned int needed = 0;
	unsigned char fl = 0;
	int rc = pcache_ht_fetch_buf_fl(ht, k, buf, cap, vlen, &needed, exp,
		&fl);
	HOT(ht, k->s, k->len);

	if (rc == 0) {
		/* a document is not a string, nor is any typed record */
		if ((fl & PCACHE_F_TYPES) && pc_store_types_strict(ht)) {
			*vlen = 0;
			return PC_OP_ERR_TYPE;
		}
		return PC_OP_OK;
	}
	if (rc != -2)
		return PC_OP_ERR_GET;   /* TOOSMALL lands here too: cap is VAL_MAX */
	/* the core reports a past expiry on a miss: the event says expired vs never there */
	pc_ev_miss(ht, k, exp && *exp ? 1 : 0);
	return op_get_miss(ht, k, park);
}

/* may a local miss in @ht be looked up on another node - the test op_get_miss makes before it
 * asks anyone */
int mget_fans(pcache_htable_t *ht)
{
	if (!ht || !pc_cluster_enabled())
		return 0;
	if (pc_store_shard_enabled(ht))
		return 1;
	if (pc_store_eager_enabled(ht) && !pc_cluster_replicas() &&
	        pc_cluster_eager_miss_final())
		return 0;
	return pc_store_pull_enabled(ht);
}

/* an mget's @n keys; each not held here is looked up as op_get_miss would, all at once, one
 * deadline.  0: *@mp set (park it unless pc_mget_waiting() is 0); -1: *@code and *@why set */
int op_mget_fan(pcache_htable_t *ht, const char *const *keys,
		const size_t *klens, unsigned int n, int count, struct pc_mget **mp,
		const char **code, const char **why)
{
	const char *cn = col_name_of(ht);
	struct pc_mget *m = pc_mget_new(cn, strlen(cn), n);
	unsigned int i;

	*mp = NULL;
	*code = "ERR";
	if (!m) {
		*why = "out of memory";
		return -1;
	}
	if (count)
		pc_mget_set_count(m);
	for (i = 0; i < n; i++) {
		unsigned char fl = 0;
		unsigned int exp = 0, req = 0;
		str k, v;
		int rc;

		if (!keys[i] || klens[i] == 0 || klens[i] > KEY_MAX) {
			pc_mget_absent(m, i);
			continue;
		}
		k.s = (char *)keys[i];
		k.len = (int)klens[i];
		HOT(ht, k.s, k.len);           /* each key of the MGET */
		rc = count ? probe_or_present(ht, &k, NULL, NULL, NULL, NULL) :
			pcache_ht_fetch_ex(ht, &k, &v, &exp, &fl);
		if (rc == 0 && count) {
			pc_mget_hit(m, i, NULL, 0, 0);
			continue;
		}
		if (rc == 0) {
			/* under strict a document answers an mget as a miss - it is here, so
			 * nobody else is asked */
			if ((fl & PCACHE_F_TYPES) && pc_store_types_strict(ht)) {
				free(v.s);
				pc_mget_absent(m, i);
			} else {
				pc_mget_hit(m, i, v.s, v.len, exp);
				if (pc_mget_over(m)) {
					pc_mget_free(m);
					pc_mget_count_refused();
					*why = "reply too large - split the mget";
					return -1;
				}
			}
			continue;
		}
		if (rc != -2) {
			pc_mget_absent(m, i);
			continue;
		}
		pc_pull_cap_deadline(pc_mget_deadline(m));
		rc = op_get_miss(ht, &k, &req);
		pc_pull_cap_deadline(0);
		if (rc != PC_OP_PARKED) {
			pc_mget_absent(m, i);
			continue;
		}
		rc = pc_mget_wait(m, i, k.s, (size_t)k.len, req, why);
		if (rc < 0) {
			pc_pull_cancel(req);
			pc_mget_free(m);
			if (rc != PC_MGET_E_NOMEM)
				pc_mget_count_refused();
			if (rc == PC_MGET_E_BUSY)
				*code = "TRYAGAIN";
			return -1;
		}
	}
	*mp = m;
	return 0;
}

/* eager: a write goes to every live peer as it lands (fire-and-forget, the sweep repairs).  Only
 * records this node AUTHORED: a pushed-back copy would echo around the fleet. */
static void eager_push(pcache_htable_t *ht, const str *k, const char *val,
		size_t vlen, unsigned int exp, unsigned long long ver,
		unsigned char type)
{
	const char *cn;

	if (!pc_cluster_enabled() || !pc_store_eager_enabled(ht))
		return;
	cn = col_name_of(ht);
	/* a spread non-holder must not keep a copy, but not before a holder has it: the
	 * holder's ACK of the push group drops it (M_MIGRATE_ACK), whatever this returns */
	(void)pc_repl_push(cn, strlen(cn), k->s, (size_t)k->len, val, vlen,
		exp, ver, (unsigned char)(type & PCACHE_F_TYPES));  /* every type */
}

/* a TTL re-arm (client's or forwarded) that ADOPTS a passive copy: it goes to the WAL whole and
 * travels as authored, bytes, expiry and version from one read.  1 = re-armed, 0 = absent,
 * -2 = the arena is full */
static int op_expire_local(pcache_htable_t *ht, const str *k,
		unsigned int exp)
{
	const char *cn = col_name_of(ht);
	str val;
	unsigned int e = 0;
	unsigned char fl = 0;
	unsigned long long ver = 0, tver;
	int was_passive = 0, rc;

	rc = pcache_ht_touch_adopt(ht, k, exp, &was_passive);
	if (rc != 1)
		return rc == -2 ? -2 : 0;
	/* the touch's OWN version, stamped under the bucket lock: a SET landing before the unlocked
	 * fetch below must not lend the touch its version, or replay refuses the SET as older */
	tver = pcache_last_ver;
	/* logged before the read: an EXPIRE 1 can lapse by the fetch, which then returns early */
	if (!was_passive)
		pc_wal_touch(cn, k->s, k->len, exp, tver);
	if (pcache_ht_fetch_full(ht, k, &val, &e, &fl, &ver) != 0)
		return 1;                      /* re-armed, then gone */
	if (was_passive)
		pc_wal_upsert_fl(cn, k->s, k->len, val.s, val.len, e, ver, fl);
	eager_push(ht, k, val.s, (size_t)val.len, e, ver, fl);
	/* the fetch hands back a copy the caller owns */
	free(val.s);
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

/* a delete reaches the table a resize is filling too: a key the copier already carried
 * across would come back at the swap */
static int store_remove(pcache_htable_t *ht, const str *k)
{
	pcache_htable_t *o, *f;
	int rc, rs;

	pc_store_test_del_pause(k);
	f = pc_store_del_order(ht, &o);

	rs = pcache_ht_remove(f, k);                    /* the copier's source first */
	rc = (o ? pcache_ht_remove(o, k) : 0) == 1 || rs == 1;
	if (rc == 1)
		pc_ev_remove(ht, k);
	return rc;
}

/* as store_remove, with the delete's version stamped under the live table's bucket lock, for
 * the tombstone that follows */
static int store_remove_ver(pcache_htable_t *ht, const str *k,
		unsigned long long *ver)
{
	pcache_htable_t *o, *f;
	int rc, rs = 0;

	pc_store_test_del_pause(k);
	f = pc_store_del_order(ht, &o);

	if (f != ht)                                    /* the copier's source first */
		rs = pcache_ht_remove(f, k);
	rc = pcache_ht_remove_ver(ht, k, ver) == 1;
	if (f == ht && o)
		rs = pcache_ht_remove(o, k);
	rc = rc || rs == 1;
	if (rc == 1)
		pc_ev_remove(ht, k);
	return rc;
}

int op_del(pcache_htable_t *ht, str *k, unsigned int *park)
{
	const char *cn;
	unsigned long long dver = 0;
	int rc, dead;
	HOT(ht, k->s, k->len);

	if (op_refused(ht))
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
			return fwd_fail_code();   /* the owner may hold it: not "absent" */
		}
		/* self-owned: the local remove is the whole story - one
		 * owner means no tombstone plane */
		rc = store_remove(ht, k);
		dead = rc == 1 && pcache_last_del_dead;
		if (rc == 1)
			pc_wal_del(cn, k->s, k->len);
		return rc == 1 && !dead ? PC_OP_OK : PC_OP_ABSENT;
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
		/* unknown holder: ask the fleet - a tombstone under THIS node's clock can be older
		 * than the holder's record; a positive answer becomes the forward */
		{
			unsigned int req = pc_probe_fwd_begin(1, cn, strlen(cn),
				k->s, (size_t)k->len, NULL, 0, 0, 0);

			if (req) {
				*park = req;
				return PC_OP_PARKED;
			}
			if (probe_busy())
				return PC_OP_ERR_BUSY;
		}
		return PC_OP_ABSENT;
	}
	/* key not HERE: a write acked elsewhere may still be batched toward it, so the holder
	 * deletes under its own clock; only an UNVERSIONED negative entry skips the probe */
	if (pc_cluster_enabled() && !pc_store_proxy_enabled(ht) &&
	        pcache_ht_probe(ht, k, NULL, NULL, NULL) != 0) {
		unsigned long long tv = 0;

		cn = col_name_of(ht);
		if (pc_cluster_replicas()) {
			unsigned kslot = pc_key_slot(k->s, (size_t)k->len);
			int h = pc_spread_pick_peer(kslot, pc_cluster_replicas());
			int tries;

			/* the best-ranked holder can be one the send fails to (cut off,
			 * not yet declared gone): try the next live holder before "absent" */
			for (tries = 0; h && tries < pc_cluster_replicas();
			        tries++) {
				unsigned int req = pc_fwd_begin(h, 1, cn,
					strlen(cn), k->s, (size_t)k->len, NULL, 0,
					0, 0);

				if (req) {
					*park = req;
					return PC_OP_PARKED;
				}
				h = pc_spread_pick_peer_except(kslot,
					pc_cluster_replicas(), h);
			}
			return tries ? fwd_fail_code() : PC_OP_ABSENT;
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
			if (probe_busy())
				return PC_OP_ERR_BUSY;
		}
		return PC_OP_ABSENT;
	}
	rc = store_remove_ver(ht, k, &dver);
	dead = rc == 1 && pcache_last_del_dead;   /* past its deadline: removed, answered absent */
	if (rc == 1) {
		cn = col_name_of(ht);
		/* logged, and with a cluster numbered and sent */
		pc_delete_originate(cn, strlen(cn), k->s, (size_t)k->len,
			dver, 1);
	}
	return rc == 1 && !dead ? PC_OP_OK : PC_OP_ABSENT;
}

/* a re-arm takes the write's road: shard -> owner, proxy -> holder (locator, else probe; never
 * places), spread -> best live holder; store and eager have nobody to forward to */
int op_expire(pcache_htable_t *ht, str *k, long long ttl,
		unsigned int *park)
{
	unsigned int ttl_rel = fwd_ttl(ttl);
	const char *cn;
	int rc;
	HOT(ht, k->s, k->len);

	if (op_refused(ht))
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
			if (probe_busy())
				return PC_OP_ERR_BUSY;
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
	rc = op_expire_local(ht, k, ttl_to_abs(ttl));
	return rc == -2 ? PC_OP_ERR_FULL : rc ? PC_OP_OK : PC_OP_ABSENT;
}

/* add/sub (by pre-negated for sub): PC_OP_OK fills *nv */
int op_addsub(pcache_htable_t *ht, str *k, long long by, long long ttl,
		long long *nv, unsigned int *park, int probed)
{
	if (!probed)                         /* a resume was counted */
		HOT(ht, k->s, k->len);

	if (op_refused(ht))
		return PC_OP_ERR_WRFAIL;
	if (pc_store_shard_enabled(ht) && pc_cluster_enabled()) {
		const char *cn = col_name_of(ht);
		int owner = pc_shard_owner(cn, strlen(cn), k->s,
			(size_t)k->len);

		if (owner) {
			unsigned int req = pc_fwd_begin(owner, 2, cn,
				strlen(cn), k->s, (size_t)k->len, NULL, 0,
				ttl < 0 ? PCACHE_EXP_PRESERVE :
				fwd_ttl(ttl), by);

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
				fwd_ttl(ttl), by);

			if (req) {
				*park = req;
				return PC_OP_PARKED;
			}
			if (probe_busy())
				return PC_OP_ERR_BUSY;
		}
		if (!target)
			target = pc_place();
		if (target) {
			unsigned int req = pc_fwd_begin(target, 2, cn,
				strlen(cn), k->s, (size_t)k->len, NULL, 0,
				ttl < 0 ? PCACHE_EXP_PRESERVE :
				fwd_ttl(ttl), by);

			if (req) {
				*park = req;
				return PC_OP_PARKED;
			}
			return fwd_fail_code();
		}
	}
	{
		unsigned int eff = 0;
		unsigned char fl = 0;
		int r;

		/* INCR on a document is WRONGTYPE, not "not an integer" */
		if (pc_store_types_strict(ht) &&
		        pcache_ht_getflags(ht, k, &fl) == 0 &&
		        (fl & PCACHE_F_TYPES))
			return PC_OP_ERR_TYPE;
		r = pcache_ht_add_ex(ht, k, by,
			ttl < 0 ? PCACHE_EXP_PRESERVE : ttl_to_abs(ttl), nv, &eff);
		if (r == -3)
			return PC_OP_ERR_OVF;
		if (r < 0)
			return PC_OP_ERR_NOTINT;
		/* replay is idempotent: the WAL row is the resulting value and, under preserve
		 * (ttl < 0, Redis's INCR), the record's EFFECTIVE expiry */
		{
			char nvbuf[24];
			int nvl = snprintf(nvbuf, sizeof nvbuf, "%lld", *nv);

			pc_wal_upsert(col_name_of(ht), k->s, k->len, nvbuf,
				nvl, eff, pcache_last_ver);
			eager_push(ht, k, nvbuf, (size_t)nvl, eff,
				pcache_last_ver, 0);
		}
	}
	return PC_OP_OK;
}

static int set_local(pcache_htable_t *ht, const str *k, const str *v,
		long long ttl);

int op_set(pcache_htable_t *ht, str *k, str *v, long long ttl,
		unsigned int *park, int probed)
{
	if (op_refused(ht))
		return PC_OP_ERR_WRFAIL;
	if (!probed)                         /* a resume was counted */
		HOT(ht, k->s, k->len);

	if (pc_store_shard_enabled(ht) && pc_cluster_enabled()) {
		const char *cn = col_name_of(ht);
		int owner = pc_shard_owner(cn, strlen(cn), k->s,
			(size_t)k->len);

		if (owner) {
			unsigned int req;

			if (v->len > PC_MAX_FWD_VAL)
				return PC_OP_ERR_2BIG; /* the forward plane's ceiling: a
				                        * local store would break ownership */
			req = pc_fwd_begin(owner, 0, cn, strlen(cn), k->s,
				(size_t)k->len, v->s, (size_t)v->len,
				fwd_ttl(ttl), 0);
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
			/* too big to forward to a remote holder: only a key the fleet
			 * recently confirmed ABSENT may be born here (bulk plane migrates it) */
			if (target ||
			        !pc_neg_hit(cn, strlen(cn), k->s, (size_t)k->len))
				return PC_OP_ERR_2BIG;
			/* fleet-confirmed absent: fall through to the birth */
		} else {
			if (!target && !probed &&
			        !pc_neg_hit(cn, strlen(cn), k->s, (size_t)k->len)) {
				/* holder unknown, not confirmed absent: probe the fleet
				 * BEFORE placement can fork a key (the locator is a cache) */
				unsigned int req = pc_probe_fwd_begin(0, cn,
					strlen(cn), k->s, (size_t)k->len, v->s,
					(size_t)v->len,
					fwd_ttl(ttl), 0);

				if (req) {
					*park = req;
					return PC_OP_PARKED;
				}
				if (probe_busy())
					return PC_OP_ERR_BUSY;
				/* no peers: place it */
			}
			if (!target)
				target = pc_place();   /* a new key: place it */
			if (target) {
				unsigned int req = pc_fwd_begin(target, 0, cn,
					strlen(cn), k->s, (size_t)k->len, v->s,
					(size_t)v->len,
					fwd_ttl(ttl), 0);

				if (req) {
					*park = req;
					return PC_OP_PARKED;
				}
				return fwd_fail_code();
			}
			/* placement chose LOCAL: fall through to the store */
		}
	}
	return set_local(ht, k, v, ttl);
}

/* the write a SET makes on the node that stores it (table, WAL, event, eager push), shared by
 * op_set and the conditional set so the two cannot drift */
static int set_local(pcache_htable_t *ht, const str *k, const str *v,
		long long ttl)
{
	unsigned int exp = ttl_to_abs(ttl);
	int rc;

	pc_store_test_set_pause(k);
	rc = pcache_ht_store(ht, k, v, exp);

	if (rc == 0) {
		const char *cn = col_name_of(ht);

		pc_wal_upsert(cn, k->s, k->len, v->s, v->len, exp,
			pcache_last_ver);
		pc_ev_store(ht, k, (unsigned int)v->len, ttl);
		eager_push(ht, k, v->s, (size_t)v->len, exp,
			pcache_last_ver, 0);
		if (pc_cluster_enabled())
			pc_neg_clear(cn, strlen(cn), k->s, (size_t)k->len);
		pc_store_note_set(ht, (size_t)k->len, (size_t)v->len);
		return PC_OP_OK;
	}
	return rc == -2 ? PC_OP_ERR_FULL : PC_OP_ERR_2BIG;
}

/* SET NX/XX: checked and stored under the key's stripe, decided by the slot owner in every
 * copy-keeping mode; proxy and pull-only have no deciding node (PC_OP_ERR_MODE) */
static pthread_mutex_t *jp_lock(const char *key, int klen);

static int setcond_local(pcache_htable_t *ht, const str *k, const str *v,
		long long ttl, int cond)
{
	pthread_mutex_t *mu = jp_lock(k->s, k->len);
	int present, rc;

	pthread_mutex_lock(mu);
	present = pcache_ht_probe(ht, k, NULL, NULL, NULL) == 0;
	if (present != (cond == PC_SETCOND_XX)) {
		pthread_mutex_unlock(mu);
		return 0;
	}
	rc = set_local(ht, k, v, ttl);
	pthread_mutex_unlock(mu);
	return rc == PC_OP_OK ? 1 : rc;
}

int pc_op_setcond_apply(const char *col, size_t collen, const char *key,
		size_t klen, const char *val, size_t vlen, long long ttl,
		int cond)
{
	pcache_htable_t *ht = pc_store_find(col, collen);
	str k, v;

	if (!ht)
		return PC_OP_ERR_GET;
	if (op_refused_col(col, collen))
		return PC_OP_ERR_WRFAIL;
	k.s = (char *)key;
	k.len = (int)klen;
	v.s = (char *)val;
	v.len = (int)vlen;
	return setcond_local(ht, &k, &v, ttl, cond);
}

const char setcond_mode_msg[] = "SET NX/XX needs a shard or eager "
	"collection in a cluster: this collection has no node that decides it";

/* PC_OP_OK stored, PC_OP_ABSENT the condition did not hold, PARKED
 * forwarded to the deciding node, else PC_OP_ERR_* */
int op_setcond(pcache_htable_t *ht, str *k, str *v, long long ttl,
		int cond, unsigned int *park)
{
	int rc;
	HOT(ht, k->s, k->len);

	if (op_refused(ht))
		return PC_OP_ERR_WRFAIL;
	if (pc_cluster_enabled()) {
		const char *cn = col_name_of(ht);
		int owner;

		if (pc_store_proxy_enabled(ht) ||
		        (!pc_store_shard_enabled(ht) &&
		         !pc_store_eager_enabled(ht)))
			return PC_OP_ERR_MODE;
		owner = pc_shard_owner(cn, strlen(cn), k->s, (size_t)k->len);
		if (owner) {
			unsigned int req;

			if (v->len > PC_MAX_FWD_VAL)
				return PC_OP_ERR_2BIG;
			req = pc_fwd_begin(owner, cond == PC_SETCOND_NX ? 4 : 5,
				cn, strlen(cn), k->s, (size_t)k->len, v->s,
				(size_t)v->len, fwd_ttl(ttl), 0);
			if (req) {
				*park = req;
				return PC_OP_PARKED;
			}
			return fwd_fail_code();
		}
		/* this node decides: fall through */
	}
	rc = setcond_local(ht, k, v, ttl, cond);
	return rc == 1 ? PC_OP_OK : rc == 0 ? PC_OP_ABSENT : rc;
}

/* SET IFEQ/IFNE and DELEX IFEQ/IFNE (Redis 8.4), decided as SET NX is; a hash, document or
 * rate-limit record is a type error, a counter compares as its decimal text */
_Static_assert(PC_OP_ERR_TYPE == -11 && PC_FWD_E_TYPE == -11,
	"cluster.c's forward handler spells the type refusal as -11");

static void hash_drop_local(pcache_htable_t *ht, const str *k);

static int cmp_local(pcache_htable_t *ht, const str *k, const char *cmp,
		size_t cmplen, const str *v, long long ttl, int kind)
{
	pthread_mutex_t *mu = jp_lock(k->s, k->len);
	char *cur = get_buf();
	unsigned int vl = 0, exp = 0;
	unsigned char fl = 0;
	int rc, absent, eq, go;

	if (!cur)
		return PC_OP_ERR_GET;
	pthread_mutex_lock(mu);
	rc = pcache_ht_fetch_buf_fl(ht, k, cur, VAL_MAX, &vl, NULL, &exp, &fl);
	absent = rc == -2;
	if (rc != 0 && !absent) {
		pthread_mutex_unlock(mu);
		return PC_OP_ERR_GET;
	}
	if (!absent && (fl & PCACHE_F_TYPES)) {
		pthread_mutex_unlock(mu);
		return PC_OP_ERR_TYPE;
	}
	eq = !absent && vl == cmplen && (cmplen == 0 || !memcmp(cur, cmp, cmplen));
	go = kind == PC_CMP_SET_IFEQ ? eq :
		kind == PC_CMP_SET_IFNE ? !eq :
		kind == PC_CMP_DEL_IFEQ ? eq :
		!absent && !eq;                /* PC_CMP_DEL_IFNE */
	if (!go) {
		pthread_mutex_unlock(mu);
		return 0;
	}
	if (kind == PC_CMP_SET_IFEQ || kind == PC_CMP_SET_IFNE) {
		rc = set_local(ht, k, v, ttl);
		pthread_mutex_unlock(mu);
		return rc == PC_OP_OK ? 1 : rc;
	}
	hash_drop_local(ht, k);        /* the decided-node delete: WAL + tombstones */
	pthread_mutex_unlock(mu);
	return 1;
}

int pc_op_cmp_apply(const char *col, size_t collen, const char *key,
		size_t klen, const char *cmp, size_t cmplen, const char *val,
		size_t vlen, long long ttl, int kind)
{
	pcache_htable_t *ht = pc_store_find(col, collen);
	str k, v;

	if (!ht)
		return PC_OP_ERR_GET;
	if (op_refused_col(col, collen))
		return PC_OP_ERR_WRFAIL;
	if (kind < PC_CMP_SET_IFEQ || kind > PC_CMP_DEL_IFNE)
		return PC_OP_ERR_GET;
	k.s = (char *)key;
	k.len = (int)klen;
	v.s = (char *)val;
	v.len = (int)vlen;
	return cmp_local(ht, &k, cmp, cmplen, &v, ttl, kind);
}

const char cmp_mode_msg[] = "IFEQ/IFNE need a shard or eager "
	"collection in a cluster: this collection has no node that decides it";
/* DELEX with a condition on a key that is not a string - Redis's words */
const char delex_type_msg[] = "Key should be of string type if "
	"conditions are specified";

/* PC_OP_OK applied, PC_OP_ABSENT the condition did not hold, PARKED
 * forwarded to the deciding node, PC_OP_ERR_TYPE, else PC_OP_ERR_* */
int op_cmp(pcache_htable_t *ht, str *k, const char *cmp,
		size_t cmplen, str *v, long long ttl, int kind,
		unsigned int *park)
{
	str none = { NULL, 0 };
	int rc;
	HOT(ht, k->s, k->len);

	if (!v)
		v = &none;
	if (op_refused(ht))
		return PC_OP_ERR_WRFAIL;
	if (pc_cluster_enabled()) {
		const char *cn = col_name_of(ht);
		int owner;

		if (pc_store_proxy_enabled(ht) ||
		        (!pc_store_shard_enabled(ht) &&
		         !pc_store_eager_enabled(ht)))
			return PC_OP_ERR_MODE;
		owner = pc_shard_owner(cn, strlen(cn), k->s, (size_t)k->len);
		if (owner) {
			size_t need = 4 + cmplen + (size_t)v->len;
			unsigned char *enc;
			unsigned int req;

			if (need > PC_MAX_FWD_VAL)
				return PC_OP_ERR_2BIG;
			/* the edit scratch (sized by hr_buf's assert): pc_fwd_begin builds its
			 * datagram from it before returning */
			enc = hr_buf();
			if (!enc)
				return PC_OP_ERR_FWD;
			enc[0] = (unsigned char)cmplen;
			enc[1] = (unsigned char)(cmplen >> 8);
			enc[2] = (unsigned char)(cmplen >> 16);
			enc[3] = (unsigned char)(cmplen >> 24);
			memcpy(enc + 4, cmp, cmplen);
			if (v->len)
				memcpy(enc + 4 + cmplen, v->s, (size_t)v->len);
			req = pc_fwd_begin(owner, PC_CMP_FWD_OP(kind), cn,
				strlen(cn), k->s, (size_t)k->len, (const char *)enc,
				need, fwd_ttl(ttl), 0);
			if (req) {
				*park = req;
				return PC_OP_PARKED;
			}
			return fwd_fail_code();
		}
		/* this node decides: fall through */
	}
	rc = cmp_local(ht, k, cmp, cmplen, v, ttl, kind);
	return rc == 1 ? PC_OP_OK : rc == 0 ? PC_OP_ABSENT : rc;
}

/* ---- scan/keys walker callbacks ---------------------------------------- */

struct scan_ctx {
	struct pc_tw *w;
	const char *pat;
	int patlen;                        /* <0 = no pattern */
	int values;
	int emitted;                       /* keys: against the limit */
	int limit;                         /* keys verb only */
	int stopped;                       /* keys: limit reached mid-walk */
	unsigned int now;
};

static long long rel_ttl(unsigned int expires, unsigned int now)
{
	if (!expires)
		return -1;
	return expires <= now ? 0 : (long long)expires - now;
}

/* dump's first pass: a scan chunk's keys as u32 length + bytes, by Redis slot; keys that overflow
 * one reply refuse the chunk ("halve count") rather than cut mid-bucket and repeat */
struct dump_keys {
	char *buf;
	size_t len, cap;
	/* the old table's keys, first in buf, end here */
	size_t nold;
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

/* scan: {k, v?, ttl} per live matching entry - keys and values are bytes */
static int scan_cb(const str *key, const str *val, unsigned int exp,
		void *p)
{
	struct scan_ctx *sc = p;

	if (exp && exp <= sc->now)
		return 0;                      /* expired-as-absent */
	if (sc->pat && !pc_glob(sc->pat, sc->patlen, key->s, key->len))
		return 0;
	if (sc->w->over)
		return -1;                     /* stop early: reply already dead */
	pc_tw_map(sc->w);
	pc_tw_key(sc->w, "k");
	pc_tw_bulk(sc->w, key->s, (size_t)key->len);
	if (sc->values) {
		pc_tw_key(sc->w, "v");
		pc_tw_bulk(sc->w, val->s, (size_t)val->len);
	}
	pc_tw_key(sc->w, "ttl");
	pc_tw_i64(sc->w, rel_ttl(exp, sc->now));
	pc_tw_end(sc->w);
	return 0;
}

/* what keys writes after its last key: the closing fields and, for a collection glob, a
 * header and a truncated entry for every other collection */
#define KEYS_TAIL (256 + PC_MAX_COLLECTIONS * 2 * (PC_COL_NAME_MAX + 16))

/* keys: a flat array of keys, each as its bytes */
static int keys_cb(const str *key, const str *val, unsigned int exp,
		void *p)
{
	struct scan_ctx *sc = p;

	(void)val;
	if (exp && exp <= sc->now)
		return 0;
	if (sc->pat && !pc_glob(sc->pat, sc->patlen, key->s, key->len))
		return 0;
	if (sc->w->over)
		return -1;
	/* a key that would leave no room for the reply's tail stops the walk: truncated, not
	 * "reply too large" */
	if (sc->emitted >= sc->limit || sc->w->n + (size_t)key->len + 16 +
	        KEYS_TAIL > (sc->w->owned ? PT_HEAP_MAX : sc->w->cap)) {
		sc->stopped = 1;
		return -1;
	}
	sc->emitted++;
	pc_tw_bulk(sc->w, key->s, (size_t)key->len);
	return 0;
}

/* ---- json path verbs: a document is read, edited as text and stored back, so a 64-way
 * stripe on the key bytes serializes the read-modify-write (all writers are in-process) */
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

const char *jp_strerror(int rc)
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

/* ---- RL.HIT: one sliding-window hit, decided as SET NX is, under the key's stripe.  Record:
 * [ver 1][0][0][0][n u32 LE][n x ms u64 LE], oldest first; at PC_RL_MAX the oldest goes */
#define RL_HDR 8

static void rl_w64(unsigned char *p, unsigned long long v)
{
	int i;

	for (i = 0; i < 8; i++)
		p[i] = (unsigned char)(v >> (8 * i));
}

static unsigned long long rl_r64(const unsigned char *p)
{
	unsigned long long v = 0;
	int i;

	for (i = 7; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}

static long long rl_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 0 ok (*count, *allowed set), -1 refused (*errmsg) */
static int rl_hit_local(pcache_htable_t *ht, const char *key, int klen,
		long long window_ms, long long limit, long long *count,
		int *allowed, const char **errmsg)
{
	pthread_mutex_t *mu = jp_lock(key, klen);
	unsigned char *nd, fl = 0;
	const unsigned char *old;
	unsigned int exp = 0, newexp, n = 0, first = 0, keep = 0;
	long long now;
	str k, v, nvs;
	int rc;

	nd = malloc(RL_HDR + 8 * (PC_RL_MAX + 1));
	if (!nd) {
		*errmsg = "out of memory";
		return -1;
	}
	k.s = (char *)key;
	k.len = klen;
	pthread_mutex_lock(mu);
	rc = pcache_ht_fetch_ex(ht, &k, &v, &exp, &fl);
	if (rc == 0 && !(fl & PCACHE_F_RL)) {
		free(v.s);
		if (pc_store_types_strict(ht)) {
			pthread_mutex_unlock(mu);
			free(nd);
			*errmsg = pc_json_wrongtype_msg;   /* routes as type */
			return -1;
		}
		rc = -2;                       /* loose: a string is replaced */
	}
	if (rc != 0 && rc != -2) {
		pthread_mutex_unlock(mu);
		free(nd);
		*errmsg = "get failed";
		return -1;
	}
	now = rl_now_ms();
	if (rc == 0) {
		old = (const unsigned char *)v.s;
		if (v.len >= RL_HDR && old[0] == 1) {
			n = (unsigned int)old[4] | (unsigned int)old[5] << 8 |
				(unsigned int)old[6] << 16 |
				(unsigned int)old[7] << 24;
			if (n > (unsigned int)(v.len - RL_HDR) / 8)
				n = (unsigned int)(v.len - RL_HDR) / 8;
		}
		if (n) {
			long long last = (long long)rl_r64(old + RL_HDR +
				8 * (n - 1));

			if (now < last)
				now = last;    /* monotonic per key */
		}
		while (first < n && (long long)rl_r64(old + RL_HDR +
		        8 * first) <= now - window_ms)
			first++;
		keep = n - first;
		if (keep > PC_RL_MAX - 1) {
			first += keep - (PC_RL_MAX - 1);
			keep = PC_RL_MAX - 1;
		}
		memcpy(nd + RL_HDR, old + RL_HDR + 8 * first, 8 * (size_t)keep);
		free(v.s);
	}
	rl_w64(nd + RL_HDR + 8 * keep, (unsigned long long)now);
	keep++;
	memset(nd, 0, RL_HDR);
	nd[0] = 1;
	nd[4] = (unsigned char)keep;
	nd[5] = (unsigned char)(keep >> 8);
	nd[6] = (unsigned char)(keep >> 16);
	nd[7] = (unsigned char)(keep >> 24);
	nvs.s = (char *)nd;
	nvs.len = (int)(RL_HDR + 8 * keep);
	newexp = ttl_to_abs(window_ms / 1000 + 2);  /* outlives the window */
	if (pcache_ht_store_ex(ht, &k, &nvs, newexp, PCACHE_F_RL) == 0) {
		const char *cn = col_name_of(ht);

		pc_ev_store(ht, &k, (unsigned int)nvs.len, 0);
		pc_wal_upsert_fl(cn, k.s, k.len, nvs.s, nvs.len, newexp,
			pcache_last_ver, PCACHE_F_RL);
		eager_push(ht, &k, nvs.s, (size_t)nvs.len, newexp,
			pcache_last_ver, PCACHE_F_RL);
		if (pc_cluster_enabled())
			pc_neg_clear(cn, strlen(cn), k.s, (size_t)k.len);
	} else {
		pthread_mutex_unlock(mu);
		free(nd);
		*errmsg = "store failed";
		return -1;
	}
	pthread_mutex_unlock(mu);
	free(nd);
	*count = keep;
	*allowed = limit <= 0 || (long long)keep <= limit;
	return 0;
}

/* the gate's refusal, with the member that holds it back */
static __thread char rl_gate_msg[160];

const char *rl_gate_text(int lacking)
{
	snprintf(rl_gate_msg, sizeof rl_gate_msg, "RL.HIT needs every node "
		"in the fleet on 0.4.4 or later - node %d does not carry the "
		"rate-limit type yet", lacking);
	return rl_gate_msg;
}

const char rl_mode_msg[] = "RL.HIT needs a shard or eager "
	"collection in a cluster: this collection has no node that decides it";

/* PC_OP_OK (*count, *allowed), PARKED forwarded to the deciding node (SET NX's rule), or
 * PC_OP_ERR_* */
int op_rlhit(pcache_htable_t *ht, str *k, long long window_ms,
		long long limit, long long *count, int *allowed,
		unsigned int *park, int *lacking)
{
	const char *emsg = NULL;
	HOT(ht, k->s, k->len);

	if (op_refused(ht))
		return PC_OP_ERR_WRFAIL;
	if (!pc_cluster_fleet_has(PC_FEAT_TYPES2, lacking))
		return PC_OP_ERR_GATE;
	if (pc_cluster_enabled()) {
		const char *cn = col_name_of(ht);
		int owner;

		if (pc_store_proxy_enabled(ht) ||
		        (!pc_store_shard_enabled(ht) &&
		         !pc_store_eager_enabled(ht)))
			return PC_OP_ERR_MODE;
		owner = pc_shard_owner(cn, strlen(cn), k->s, (size_t)k->len);
		if (owner) {
			unsigned int req = pc_fwd_json_begin(owner,
				PC_JOP_RLHIT, cn, strlen(cn), k->s,
				(size_t)k->len, "", 0, "", 0, window_ms,
				limit > 0, limit > 0 ? limit : 0, 0, 0, 0);

			if (req) {
				*park = req;
				return PC_OP_PARKED;
			}
			return fwd_fail_code();
		}
	}
	if (rl_hit_local(ht, k->s, k->len, window_ms, limit, count, allowed,
	        &emsg) != 0)
		return emsg == pc_json_wrongtype_msg ? PC_OP_ERR_TYPE :
			PC_OP_ERR_GET;
	return PC_OP_OK;
}

/* ---- Symfony Lock's (RedisStore) fixed Lua scripts, approved by body SHA1 and run as C.  Record:
 * [ver 1][0][0][0][n u32 LE] then n x [exp_ms u64 LE][len u16 LE][member]; decided as RL.HIT */
#define LK_HDR      8
#define LK_MAX      1024               /* members a lock record holds */
#define LK_TOK_MAX  4096               /* bytes in one token */

struct lk_m {
	unsigned long long exp;
	const unsigned char *tok;
	unsigned int len;
};

static __thread struct lk_m lk_scratch[LK_MAX + 2];

/* the members, or -1 when the record is malformed */
static int lk_parse(const unsigned char *r, size_t n, struct lk_m *m)
{
	unsigned int cnt, i;
	size_t o = LK_HDR;

	if (n < LK_HDR || r[0] != 1)
		return -1;
	cnt = (unsigned int)r[4] | (unsigned int)r[5] << 8 |
		(unsigned int)r[6] << 16 | (unsigned int)r[7] << 24;
	if (cnt > LK_MAX)
		return -1;
	for (i = 0; i < cnt; i++) {
		unsigned int l;

		if (n - o < 10)
			return -1;
		m[i].exp = rl_r64(r + o);
		l = (unsigned int)r[o + 8] | (unsigned int)r[o + 9] << 8;
		if (n - o - 10 < l)
			return -1;
		m[i].tok = r + o + 10;
		m[i].len = l;
		o += 10 + l;
	}
	return o == n ? (int)cnt : -1;
}

static int lk_find(const struct lk_m *m, int n, const void *t, size_t tl)
{
	int i;

	for (i = 0; i < n; i++)
		if (m[i].len == tl && !memcmp(m[i].tok, t, tl))
			return i;
	return -1;
}

/* ZADD: set the member's expiry, adding it if absent; -1 when full */
static int lk_set(struct lk_m *m, int *n, const void *t, size_t tl,
		unsigned long long exp)
{
	int i = lk_find(m, *n, t, tl);

	if (i < 0) {
		if (*n >= LK_MAX)
			return -1;
		i = (*n)++;
		m[i].tok = t;
		m[i].len = (unsigned int)tl;
	}
	m[i].exp = exp;
	return 0;
}

/* ZREM */
static void lk_rem(struct lk_m *m, int *n, const void *t, size_t tl)
{
	int i = lk_find(m, *n, t, tl);

	if (i >= 0) {
		m[i] = m[*n - 1];
		(*n)--;
	}
}

/* ZREMRANGEBYSCORE key -inf now; 1 when anything went */
static int lk_trim(struct lk_m *m, int *n, unsigned long long now)
{
	int i, gone = 0;

	for (i = 0; i < *n; )
		if (m[i].exp <= now) {
			m[i] = m[--(*n)];
			gone = 1;
		} else {
			i++;
		}
	return gone;
}

static const char lk_write[] = "__write__";

/* run one script on THIS node's table: 0 with *result (1 true, 0 false),
 * -1 with *errmsg (pc_json_wrongtype_msg for a key of another type) */
static int slock_local(pcache_htable_t *ht, const char *key, int klen,
		int script, long long now_arg, long long ttl_ms,
		const char *tok, size_t toklen, int *result,
		const char **errmsg)
{
	pthread_mutex_t *mu = jp_lock(key, klen);
	struct lk_m *m = lk_scratch;
	char *cur = get_buf();
	unsigned char *nb = hr_buf();
	unsigned int vl = 0, exp = 0;
	unsigned char fl = 0;
	unsigned long long now, wall = (unsigned long long)rl_now_ms();
	int rc, n = 0, ti, wi, changed = 0, res = 0;
	str k;

	*result = 0;
	if (script == PC_SLOCK_PROBE) {
		/* TIME works here, which is all the probe asks; its SET of a
		 * 1 ms key is not kept - it would only expire */
		*result = 1;
		return 0;
	}
	if (!cur || !nb) {
		*errmsg = "out of memory";
		return -1;
	}
	if (toklen > LK_TOK_MAX) {
		*errmsg = "lock token too long";
		return -1;
	}
	now = now_arg >= 0 ? (unsigned long long)now_arg : wall;
	k.s = (char *)key;
	k.len = klen;
	pthread_mutex_lock(mu);
	rc = pcache_ht_fetch_buf_fl(ht, &k, cur, VAL_MAX, &vl, NULL, &exp, &fl);
	if (rc != 0 && rc != -2) {
		pthread_mutex_unlock(mu);
		*errmsg = "get failed";
		return -1;
	}
	if (rc == 0 && !(fl & PCACHE_F_TYPES)) {
		/* the scripts' first line: a string key is an old-format
		 * lock (Symfony < 5.2) - answer false, touch nothing */
		pthread_mutex_unlock(mu);
		return 0;
	}
	if (rc == 0 && !(fl & PCACHE_F_LOCK)) {
		/* Redis: the first Z command on a hash or a document fails */
		pthread_mutex_unlock(mu);
		*errmsg = pc_json_wrongtype_msg;
		return -1;
	}
	if (rc == 0 && (n = lk_parse((const unsigned char *)cur, vl, m)) < 0) {
		pthread_mutex_unlock(mu);
		*errmsg = "corrupt lock record";
		return -1;
	}
	if (script == PC_SLOCK_SAVE || script == PC_SLOCK_READ ||
	        script == PC_SLOCK_EXISTS)
		changed = lk_trim(m, &n, now);
	ti = lk_find(m, n, tok, toklen);
	wi = lk_find(m, n, lk_write, sizeof lk_write - 1);
	switch (script) {
	case PC_SLOCK_SAVE:
		/* held by us: only a write lock, or a read lock we hold alone,
		 * may be (re)taken; held by anyone else: refused */
		if (ti >= 0 ? (wi < 0 && n > 1) : n > 0)
			break;
		if (lk_set(m, &n, tok, toklen, now + (unsigned long long)ttl_ms) ||
		    lk_set(m, &n, lk_write, sizeof lk_write - 1,
		        now + (unsigned long long)ttl_ms))
			break;
		res = changed = 1;
		break;
	case PC_SLOCK_READ:
		if (ti < 0 && wi >= 0)
			break;
		if (lk_set(m, &n, tok, toklen, now + (unsigned long long)ttl_ms))
			break;
		lk_rem(m, &n, lk_write, sizeof lk_write - 1);
		res = changed = 1;
		break;
	case PC_SLOCK_REFRESH:
		if (ti < 0)
			break;
		m[ti].exp = now + (unsigned long long)ttl_ms;
		if (wi >= 0)
			m[wi].exp = now + (unsigned long long)ttl_ms;
		res = changed = 1;
		break;
	case PC_SLOCK_RELEASE:
		if (ti < 0)
			break;
		lk_rem(m, &n, tok, toklen);
		lk_rem(m, &n, lk_write, sizeof lk_write - 1);
		res = changed = 1;
		break;
	case PC_SLOCK_EXISTS:
		res = ti >= 0;
		break;
	default:
		pthread_mutex_unlock(mu);
		*errmsg = "unknown lock script";
		return -1;
	}
	if (changed && rc == 0 && n == 0) {
		/* an emptied zset is no key at all */
		hash_drop_local(ht, &k);
	} else if (changed && n > 0) {
		unsigned long long max = 0;
		size_t o = LK_HDR;
		int i;

		for (i = 0; i < n; i++)
			if (m[i].exp > max)
				max = m[i].exp;
		if (max <= wall) {
			/* PEXPIREAT in the past: the key goes */
			if (rc == 0)
				hash_drop_local(ht, &k);
		} else {
			const char *cn = col_name_of(ht);
			unsigned int newexp = ttl_to_abs((long long)
				((max - wall + 999) / 1000));
			size_t need = LK_HDR;
			str nvs;

			for (i = 0; i < n; i++)
				need += 10 + m[i].len;
			if (need > PC_HR_MAX) {
				/* the edit buffer's and a replicated record's
				 * ceiling (PC_MAX_FWD_VAL) */
				pthread_mutex_unlock(mu);
				*errmsg = "lock record full";
				return -1;
			}
			memset(nb, 0, LK_HDR);
			nb[0] = 1;
			nb[4] = (unsigned char)n;
			nb[5] = (unsigned char)(n >> 8);
			for (i = 0; i < n; i++) {
				rl_w64(nb + o, m[i].exp);
				nb[o + 8] = (unsigned char)m[i].len;
				nb[o + 9] = (unsigned char)(m[i].len >> 8);
				/* memmove: a member may point into cur, never nb */
				memmove(nb + o + 10, m[i].tok, m[i].len);
				o += 10 + m[i].len;
			}
			nvs.s = (char *)nb;
			nvs.len = (int)o;
			if (pcache_ht_store_ex(ht, &k, &nvs, newexp,
			        PCACHE_F_LOCK) != 0) {
				pthread_mutex_unlock(mu);
				*errmsg = "store failed";
				return -1;
			}
			pc_ev_store(ht, &k, (unsigned int)nvs.len, 0);
			pc_wal_upsert_fl(cn, k.s, k.len, nvs.s, nvs.len, newexp,
				pcache_last_ver, PCACHE_F_LOCK);
			eager_push(ht, &k, nvs.s, (size_t)nvs.len, newexp,
				pcache_last_ver, PCACHE_F_LOCK);
			if (pc_cluster_enabled())
				pc_neg_clear(cn, strlen(cn), k.s, (size_t)k.len);
		}
	}
	pthread_mutex_unlock(mu);
	*result = res;
	return 0;
}

const char slock_gate_fmt[] = "the Symfony lock scripts need every "
	"node in the fleet on 0.5.0 or later - node %d is older";
const char slock_mode_msg[] = "the Symfony lock scripts need a shard "
	"or eager collection in a cluster: this collection has no node that "
	"decides a key";

/* PC_OP_OK with *result, PARKED forwarded to the deciding node,
 * PC_OP_ERR_TYPE, PC_OP_ERR_GATE (*lacking), else PC_OP_ERR_* */
int op_slock(pcache_htable_t *ht, str *k, int script,
		long long now_arg, long long ttl_ms, const char *tok,
		size_t toklen, int *result, unsigned int *park, int *lacking)
{
	const char *emsg = NULL;
	HOT(ht, k->s, k->len);

	*result = 0;
	if (script == PC_SLOCK_PROBE) {
		*result = 1;
		return PC_OP_OK;
	}
	if (op_refused(ht))
		return PC_OP_ERR_WRFAIL;
	if (!pc_cluster_fleet_has(PC_FEAT_SLOCK, lacking))
		return PC_OP_ERR_GATE;
	if (toklen > LK_TOK_MAX)
		return PC_OP_ERR_2BIG;
	if (pc_cluster_enabled()) {
		const char *cn = col_name_of(ht);
		int owner;

		if (pc_store_proxy_enabled(ht) ||
		        (!pc_store_shard_enabled(ht) &&
		         !pc_store_eager_enabled(ht)))
			return PC_OP_ERR_MODE;
		owner = pc_shard_owner(cn, strlen(cn), k->s, (size_t)k->len);
		if (owner) {
			unsigned char enc[17 + LK_TOK_MAX];
			unsigned int req;

			enc[0] = (unsigned char)script;
			rl_w64(enc + 1, (unsigned long long)now_arg);
			rl_w64(enc + 9, (unsigned long long)ttl_ms);
			memcpy(enc + 17, tok, toklen);
			req = pc_fwd_json_begin(owner, PC_JOP_SLOCK, cn,
				strlen(cn), k->s, (size_t)k->len, "", 0,
				(const char *)enc, (int)(17 + toklen), 0, 0, 0, 0,
				0, 0);
			if (req) {
				*park = req;
				return PC_OP_PARKED;
			}
			return fwd_fail_code();
		}
	}
	if (slock_local(ht, k->s, k->len, script, now_arg, ttl_ms, tok, toklen,
	        result, &emsg) != 0)
		return emsg == pc_json_wrongtype_msg ? PC_OP_ERR_TYPE :
			PC_OP_ERR_GET;
	return PC_OP_OK;
}

/* ---- hash commands: a hash is ONE record (hashrec.c); hcmd_exec runs a write on the deciding
 * node, so field writes on two nodes both land.  The reply is a ptree that forwards unchanged. */

/* the little-endian u32 the two argument decoders read: a forwarded
 * command's [argc u16][(len u32)(bytes)]* and the HCMD frame's */
static unsigned int rd_u32(const unsigned char *p)
{
	return (unsigned int)p[0] | (unsigned int)p[1] << 8 |
		(unsigned int)p[2] << 16 | (unsigned int)p[3] << 24;
}

enum { HC_HSET, HC_HSETNX, HC_HMSET, HC_HGET, HC_HMGET, HC_HDEL,
	HC_HEXISTS, HC_HLEN, HC_HSTRLEN, HC_HKEYS, HC_HVALS, HC_HGETALL,
	HC_HINCRBY, HC_HINCRBYFLOAT, HC_HRANDFIELD, HC_HSCAN };

static const struct {
	const char *name, *lower;
	int min, max, write;           /* argc bounds incl. name and key; -1 = any */
} hcmds[] = {
	{ "HSET", "hset", 4, -1, 1 }, { "HSETNX", "hsetnx", 4, 4, 1 },
	{ "HMSET", "hmset", 4, -1, 1 }, { "HGET", "hget", 3, 3, 0 },
	{ "HMGET", "hmget", 3, -1, 0 }, { "HDEL", "hdel", 3, -1, 1 },
	{ "HEXISTS", "hexists", 3, 3, 0 }, { "HLEN", "hlen", 2, 2, 0 },
	{ "HSTRLEN", "hstrlen", 3, 3, 0 }, { "HKEYS", "hkeys", 2, 2, 0 },
	{ "HVALS", "hvals", 2, 2, 0 }, { "HGETALL", "hgetall", 2, 2, 0 },
	{ "HINCRBY", "hincrby", 4, 4, 1 },
	{ "HINCRBYFLOAT", "hincrbyfloat", 4, 4, 1 },
	{ "HRANDFIELD", "hrandfield", 2, 4, 0 }, { "HSCAN", "hscan", 3, -1, 0 },
};
#define HC_N ((int)(sizeof hcmds / sizeof hcmds[0]))

static int hcmd_find(const char *name, size_t n)
{
	int i;

	for (i = 0; i < HC_N; i++)
		if (strlen(hcmds[i].name) == n &&
		        !strncasecmp(hcmds[i].name, name, n))
			return i;
	return -1;
}

static int hcmd_arity_ok(int ci, int argc)
{
	if (argc < hcmds[ci].min || (hcmds[ci].max > 0 && argc > hcmds[ci].max))
		return 0;
	if ((ci == HC_HSET || ci == HC_HMSET) && argc % 2)
		return 0;
	return 1;
}

static const char hc_wrongtype[] =
	"WRONGTYPE Operation against a key holding the wrong kind of value";
static const char hc_toolarge[] = "ERR hash too large - this is a cache";

/* the empty hash deletes its key, as Redis does, by the delete's road: a shard owner's plain
 * remove, or a tombstone so every eager node drops it too */
static void hash_drop_local(pcache_htable_t *ht, const str *k)
{
	const char *cn = col_name_of(ht);
	unsigned long long dver = 0;

	if (pc_store_shard_enabled(ht) && pc_cluster_enabled()) {
		if (store_remove(ht, k) == 1)
			pc_wal_del(cn, k->s, k->len);
		return;
	}
	if (store_remove_ver(ht, k, &dver) == 1)
		pc_delete_originate(cn, strlen(cn), k->s, (size_t)k->len,
			dver, 1);
}

/* a key removed as a client delete would be, tombstone included even when not held here: Redis
 * is the record, and a peer's eager push of it may still be on its way */
void pc_verb_remove_local(pcache_htable_t *ht, const str *k)
{
	const char *cn = col_name_of(ht);
	unsigned long long dver = 0;

	if (pc_store_shard_enabled(ht) && pc_cluster_enabled()) {
		hash_drop_local(ht, k);        /* one owner: absent is absent */
		return;
	}
	if (store_remove_ver(ht, k, &dver) == 1)
		pc_delete_originate(cn, strlen(cn), k->s, (size_t)k->len,
			dver, 1);
	else if (pc_cluster_enabled() && pc_store_eager_enabled(ht))
		pc_delete_originate(cn, strlen(cn), k->s, (size_t)k->len,
			0, 0);                     /* a fresh tick, numbered too */
}

/* a record dropped because Redis said the key changed (every node hears that itself): table and
 * WAL, no push; a local mark refuses the same version from a peer, never replayed as a delete */
void pc_verb_forget_local(pcache_htable_t *ht, const str *k)
{
	const char *cn = col_name_of(ht);
	unsigned long long dver = 0;

	if (store_remove_ver(ht, k, &dver) == 1) {
		pc_wal_del(cn, k->s, k->len);
		if (pc_cluster_enabled() && dver)
			pc_neg_set_ver_local(cn, strlen(cn), k->s, (size_t)k->len,
				2000, dver);
	}
}

void pc_verb_forget_unguarded(pcache_htable_t *ht, const str *k)
{
	const char *cn = col_name_of(ht);
	unsigned long long dver = 0;

	if (store_remove_ver(ht, k, &dver) == 1)
		pc_wal_del(cn, k->s, k->len);
}

/* pc_verb_forget_local of the record at @ver or older only: one stored since then stays */
void pc_verb_forget_upto(pcache_htable_t *ht, const str *k,
		unsigned long long ver)
{
	const char *cn = col_name_of(ht);
	pcache_htable_t *sh = pc_store_resize_shadow(ht);
	int rs = sh ? pcache_ht_remove_upto(sh, k, ver) : 0;

	if (pcache_ht_remove_upto(ht, k, ver) == 1 || rs == 1) {
		pc_ev_remove(ht, k);
		pc_wal_del(cn, k->s, k->len);
		if (pc_cluster_enabled())
			pc_neg_set_ver_local(cn, strlen(cn), k->s, (size_t)k->len,
				2000, ver);
	}
}

/* a record written here as a client write would be (table, WAL, eager push), for read-through's
 * promoted copies and no-fall entries; @fl its TYPE bits */
int pc_verb_store_local(pcache_htable_t *ht, const str *k, const char *v,
		size_t n, unsigned int exp, unsigned char fl)
{
	const char *cn = col_name_of(ht);
	str nv;

	nv.s = (char *)v;
	nv.len = (int)n;
	fl &= PCACHE_F_TYPES;
	if (pcache_ht_store_ex(ht, k, &nv, exp, fl) != 0)
		return -1;
	pc_ev_store(ht, k, (unsigned int)n, 0);
	pc_wal_upsert_fl(cn, k->s, k->len, nv.s, nv.len, exp,
		pcache_last_ver, fl);
	eager_push(ht, k, nv.s, n, exp, pcache_last_ver, fl);
	if (pc_cluster_enabled())
		pc_neg_clear(cn, strlen(cn), k->s, (size_t)k->len);
	return 0;
}

/* the record goes in via the store's own copy and the WAL/push read the scratch after: the cell's
 * version is stamped only at publish, and a published cell may be replaced and freed at once */
static int hash_store_local(pcache_htable_t *ht, const str *k,
		const unsigned char *rec, size_t n, unsigned int exp)
{
	const char *cn = col_name_of(ht);
	str nv;

	nv.s = (char *)rec;
	nv.len = (int)n;
	if (pcache_ht_store_ex(ht, k, &nv, exp, PCACHE_F_HASH) != 0)
		return -1;
	pc_ev_store(ht, k, (unsigned int)n, 0);
	pc_wal_upsert_fl(cn, k->s, k->len, nv.s, nv.len, exp,
		pcache_last_ver, PCACHE_F_HASH);
	eager_push(ht, k, nv.s, n, exp, pcache_last_ver, PCACHE_F_HASH);
	if (pc_cluster_enabled())
		pc_neg_clear(cn, strlen(cn), k->s, (size_t)k->len);
	return 0;
}

static unsigned long long hc_rand(void)
{
	static __thread unsigned long long s;

	if (!s)
		s = (unsigned long long)time(NULL) ^
			((unsigned long long)(size_t)&s << 16) ^ 0x9e3779b97f4a7c15ULL;
	s ^= s << 13;
	s ^= s >> 7;
	s ^= s << 17;
	return s;
}

/* strict decimal: an optional '-', digits, inside 64 bits */
static int hc_ll(const char *p, size_t n, long long *out)
{
	char buf[32], *end;

	if (!n || n >= sizeof buf)
		return 0;
	memcpy(buf, p, n);
	buf[n] = 0;
	if (buf[0] == '+' || buf[0] == ' ')
		return 0;
	errno = 0;
	*out = strtoll(buf, &end, 10);
	return end == buf + n && errno == 0;
}

/* the field names a hash command takes: 1..PC_HR_FIELD_MAX bytes each.
 * -1 = refused, with the error written to @t */
static int hcmd_check_fields(int ci, int argc, const size_t *argl,
		struct pc_tw *t)
{
	int i;

	for (i = 2; i < argc; i++) {
		int is_field = (ci == HC_HSET || ci == HC_HMSET) ? !(i % 2) :
			(ci == HC_HMGET || ci == HC_HDEL) ? 1 :
			(ci == HC_HSETNX || ci == HC_HGET || ci == HC_HEXISTS ||
			 ci == HC_HSTRLEN || ci == HC_HINCRBY ||
			 ci == HC_HINCRBYFLOAT) ? i == 2 : 0;

		if (is_field && (!argl[i] || argl[i] > PC_HR_FIELD_MAX)) {
			pc_tw_err(t, argl[i] ? "ERR field too long" :
				"ERR empty field name");
			return -1;
		}
	}
	return 0;
}

/* render a READ command's reply over one hash record (NULL/0/0 = absent), for hcmd_exec and for
 * the asker of a forwarded read, so the reply is the same whichever node answers */
static void hcmd_render(int ci, int argc, const char *const *argv,
		const size_t *argl, const unsigned char *r, size_t n,
		unsigned int cnt, struct pc_tw *t)
{
	int i;

	switch (ci) {
	case HC_HGET:
	case HC_HSTRLEN:
	case HC_HEXISTS: {
		const unsigned char *fv;
		size_t fvl;
		int got = pc_hr_get(r, n, argv[2], argl[2], &fv, &fvl);

		if (ci == HC_HGET) {
			if (got)
				pc_tw_bulk(t, fv, fvl);
			else
				pc_tw_nil(t);
		} else {
			pc_tw_i64(t, ci == HC_HEXISTS ? got : got ? (long long)fvl : 0);
		}
		break;
	}
	case HC_HMGET:
		pc_tw_arr(t);
		for (i = 2; i < argc; i++) {
			const unsigned char *fv;
			size_t fvl;

			if (pc_hr_get(r, n, argv[i], argl[i], &fv, &fvl))
				pc_tw_bulk(t, fv, fvl);
			else
				pc_tw_nil(t);
		}
		pc_tw_end(t);
		break;
	case HC_HLEN:
		pc_tw_i64(t, cnt);
		break;
	case HC_HKEYS:
	case HC_HVALS:
	case HC_HGETALL: {
		size_t off = 0, flen, vlen;
		const unsigned char *f, *fv;

		pc_tw_arr(t);
		while (r && pc_hr_next(r, n, &off, &f, &flen, &fv, &vlen)) {
			if (ci != HC_HVALS)
				pc_tw_bulk(t, f, flen);
			if (ci != HC_HKEYS)
				pc_tw_bulk(t, fv, vlen);
		}
		pc_tw_end(t);
		break;
	}
	case HC_HRANDFIELD: {
		long long want = 0;
		int withv = 0, single = argc == 2;
		unsigned int j, pick, m, *idx = NULL;
		size_t off, flen, vl;
		const unsigned char *f, *fv;

		if (argc >= 3 && !hc_ll(argv[2], argl[2], &want)) {
			pc_tw_err(t, "ERR value is not an integer or out of range");
			break;
		}
		if (argc == 4) {
			if (argl[3] != 10 || strncasecmp(argv[3], "WITHVALUES", 10)) {
				pc_tw_err(t, "ERR syntax error");
				break;
			}
			withv = 1;
		}
		if (single) {
			if (!cnt) {
				pc_tw_nil(t);
				break;
			}
			pick = (unsigned int)(hc_rand() % cnt);
			off = 0;
			for (j = 0; j <= pick; j++)
				pc_hr_next(r, n, &off, &f, &flen, &fv, &vl);
			pc_tw_bulk(t, f, flen);
			break;
		}
		if (!cnt || !want) {
			pc_tw_arr(t);
			pc_tw_end(t);
			break;
		}
		/* every field's offset once, in the edit scratch (idle in a read): a pick indexes it;
		 * positive count: distinct fields, a partial shuffle of it; negative: repeats */
		m = want > 0 ? (want > cnt ? cnt : (unsigned int)want) :
			(unsigned int)(want < -1000000 ? 1000000 : -want);
		idx = (unsigned int *)hr_buf();
		if (!idx) {
			pc_tw_err(t, "ERR out of memory");
			break;
		}
		off = 0;
		for (j = 0; j < cnt; j++) {
			idx[j] = (unsigned int)(off ? off : PC_HR_HDR);
			if (!pc_hr_next(r, n, &off, &f, &flen, &fv, &vl))
				break;
		}
		cnt = j;
		if (!cnt) {
			pc_tw_arr(t);
			pc_tw_end(t);
			break;
		}
		if (m > cnt && want > 0)
			m = cnt;
		for (j = 0; want > 0 && j < m; j++) {
			unsigned int s = j + (unsigned int)(hc_rand() %
				(cnt - j)), tmp2 = idx[j];

			idx[j] = idx[s];
			idx[s] = tmp2;
		}
		pc_tw_arr(t);
		for (j = 0; j < m && !t->over; j++) {
			pick = want > 0 ? j : (unsigned int)(hc_rand() % cnt);
			off = idx[pick];
			pc_hr_next(r, n, &off, &f, &flen, &fv, &vl);
			pc_tw_bulk(t, f, flen);
			if (withv)
				pc_tw_bulk(t, fv, vl);
		}
		pc_tw_end(t);
		break;
	}
	case HC_HSCAN: {
		/* a hash is one small record, so - as Redis does for its
		 * listpack hashes - one call returns everything, cursor 0 */
		const char *pat = NULL;
		size_t patl = 0, off = 0, flen, vlen;
		const unsigned char *f, *fv;
		long long cur2, count;
		int novals = 0, bad = 0;

		if (!hc_ll(argv[2], argl[2], &cur2)) {
			pc_tw_err(t, "ERR invalid cursor");
			break;
		}
		for (i = 3; i < argc && !bad; i++) {
			if (argl[i] == 5 && !strncasecmp(argv[i], "MATCH", 5) &&
			        i + 1 < argc) {
				pat = argv[i + 1];
				patl = argl[i + 1];
				i++;
			} else if (argl[i] == 5 && !strncasecmp(argv[i], "COUNT", 5) &&
			        i + 1 < argc) {
				if (!hc_ll(argv[i + 1], argl[i + 1], &count) ||
				        count < 1) {
					pc_tw_err(t, "ERR value is not an integer or out of range");
					bad = 1;
				}
				i++;
			} else if (argl[i] == 8 && !strncasecmp(argv[i], "NOVALUES", 8)) {
				novals = 1;
			} else {
				pc_tw_err(t, "ERR syntax error");
				bad = 1;
			}
		}
		if (bad)
			break;
		/* the hits go straight in: the writer back-patches the count
		 * when the array closes */
		pc_tw_arr(t);
		pc_tw_bulk(t, "0", 1);
		pc_tw_arr(t);
		while (r && pc_hr_next(r, n, &off, &f, &flen, &fv, &vlen)) {
			if (pat && !pc_glob(pat, (int)patl, (const char *)f,
			        (int)flen))
				continue;
			pc_tw_bulk(t, f, flen);
			if (!novals)
				pc_tw_bulk(t, fv, vlen);
		}
		pc_tw_end(t);
		pc_tw_end(t);
		break;
	}
	default:
		pc_tw_err(t, "ERR not a read command");
		break;
	}
}

/* decode a forwarded hash command: [argc u16][(len u32)(bytes)] x argc.
 * -1 = malformed, or more arguments than any door accepts */
static int hcmd_decode(const unsigned char *p, size_t vlen,
		const char **argv, size_t *argl, int *argc_out)
{
	size_t o = 2;
	int argc, i;

	if (vlen < 2)
		return -1;
	argc = p[0] | p[1] << 8;
	if (argc < 2 || argc > PC_HCMD_MAXARGS)
		return -1;
	for (i = 0; i < argc; i++) {
		size_t l;

		if (vlen - o < 4)
			return -1;
		l = (size_t)rd_u32(p + o);
		if (vlen - o - 4 < l)
			return -1;
		argv[i] = (const char *)p + o + 4;
		argl[i] = l;
		o += 4 + l;
	}
	*argc_out = argc;
	return 0;
}

int pc_hcmd_render_read(const unsigned char *args, size_t argsl,
		const unsigned char *rec, size_t rlen, int st, struct pc_tw *t)
{
	const char *argv[PC_HCMD_MAXARGS];
	size_t argl[PC_HCMD_MAXARGS];
	unsigned int cnt = 0;
	int argc, ci;

	if (!args || hcmd_decode(args, argsl, argv, argl, &argc) != 0)
		return -1;
	ci = hcmd_find(argv[0], argl[0]);
	if (ci < 0 || !hcmd_arity_ok(ci, argc) || hcmds[ci].write)
		return -1;
	/* the owner's answer, in the order hcmd_exec would have given it:
	 * the key's type first, then the record, then the arguments */
	if (st == 3) {
		pc_tw_err(t, hc_wrongtype);
		return 0;
	}
	if (st != 0) {
		rec = NULL;
		rlen = 0;
	} else if (!pc_hr_valid(rec, rlen, &cnt)) {
		pc_tw_err(t, "ERR corrupt hash record");
		return 0;
	}
	if (hcmd_check_fields(ci, argc, argl, t) != 0)
		return 0;
	hcmd_render(ci, argc, argv, argl, rec, rlen, cnt, t);
	return 0;
}

/* run one hash command on THIS node's table, reply in @t.  A read takes GET's lock-free copy-out;
 * a write holds the key's stripe from copy-out to store, editing between get_buf() and hr_buf() */
static void hcmd_exec(pcache_htable_t *ht, int ci, int argc,
		const char *const *argv, const size_t *argl, struct pc_tw *t)
{
	pthread_mutex_t *mu = NULL;
	const unsigned char *r = NULL;
	unsigned char *a, *b = NULL, fl = 0;
	unsigned int exp = 0, cnt = 0, rlen = 0;
	size_t n = 0;
	long cur;
	str k;
	int rc, i;

	k.s = (char *)argv[1];
	k.len = (int)argl[1];
	a = (unsigned char *)get_buf();
	if (hcmds[ci].write)
		b = hr_buf();
	if (!a || (hcmds[ci].write && !b)) {
		pc_tw_err(t, "ERR out of memory");
		return;
	}
	if (hcmds[ci].write) {
		mu = jp_lock(k.s, k.len);
		pthread_mutex_lock(mu);
	}
	/* VAL_MAX-sized: any record fits (no TOOSMALL), so a string where a hash was expected still
	 * reads whole enough to be refused by its flags */
	rc = pcache_ht_fetch_buf_fl(ht, &k, (char *)a, VAL_MAX, &rlen, NULL,
		&exp, &fl);
	if (rc == 0 && !(fl & PCACHE_F_HASH)) {
		pc_tw_err(t, hc_wrongtype);
		goto out;
	}
	if (rc != 0 && rc != -2) {
		pc_tw_err(t, "ERR get failed");
		goto out;
	}
	if (rc == 0) {
		r = a;
		n = rlen;
		if (!pc_hr_valid(r, n, &cnt)) {
			pc_tw_err(t, "ERR corrupt hash record");
			goto out;
		}
	} else {
		/* an absent key has no TTL: an expired miss reports the PAST expiry,
		 * which a new hash must not inherit (pc_json_rmw resets it too) */
		exp = 0;
	}
	if (hcmd_check_fields(ci, argc, argl, t) != 0)
		goto out;
	if (!hcmds[ci].write) {
		/* the same rendering the asking node of a forwarded read
		 * runs over the record the owner sent it */
		hcmd_render(ci, argc, argv, argl, r, n, cnt, t);
		goto out;
	}
	switch (ci) {
	case HC_HSET:
	case HC_HMSET:
	case HC_HSETNX: {
		const unsigned char *src = r;
		size_t sn = n;
		long long added = 0;
		int ad;

		cur = (long)n;
		for (i = 2; i + 1 < argc; i += 2) {
			/* the copy-out is in a, so the first edit lands in b */
			unsigned char *dst = (src == b) ? a : b;

			cur = pc_hr_set(src, sn, argv[i], argl[i], argv[i + 1],
				argl[i + 1], ci == HC_HSETNX, dst,
				PC_HR_MAX + 64, &ad);
			if (cur < 0)
				break;
			added += ad;
			src = dst;
			sn = (size_t)cur;
		}
		if (cur < 0) {
			pc_tw_err(t, cur == PC_HR_E_SIZE ? hc_toolarge :
				"ERR bad field");
		} else if (added || ci != HC_HSETNX) {
			if (hash_store_local(ht, &k, src, sn, exp) != 0)
				pc_tw_err(t, "ERR cache full");
			else if (ci == HC_HMSET)
				pc_tw_ok(t);
			else
				pc_tw_i64(t, added);
		} else {
			pc_tw_i64(t, 0);              /* HSETNX declined */
		}
		break;
	}
	case HC_HDEL: {
		const unsigned char *src = r;
		size_t sn = n;
		long long removed = 0;
		int rm;

		if (!r) {
			pc_tw_i64(t, 0);
			break;
		}
		for (i = 2; i < argc; i++) {
			unsigned char *dst = (src == b) ? a : b;

			cur = pc_hr_del(src, sn, argv[i], argl[i], dst,
				PC_HR_MAX + 64, &rm);
			if (cur < 0)
				break;
			removed += rm;
			src = dst;
			sn = (size_t)cur;
		}
		if (removed) {
			if (sn == PC_HR_HDR)
				hash_drop_local(ht, &k);
			else if (hash_store_local(ht, &k, src, sn, exp) != 0) {
				pc_tw_err(t, "ERR cache full");
				break;
			}
		}
		pc_tw_i64(t, removed);
		break;
	}
	case HC_HINCRBY: {
		long long by, res = 0;

		if (!hc_ll(argv[3], argl[3], &by)) {
			pc_tw_err(t, "ERR value is not an integer or out of range");
			break;
		}
		cur = pc_hr_incrby(r, n, argv[2], argl[2], by, &res, b,
			PC_HR_MAX + 64);
		if (cur == PC_HR_E_NUM)
			pc_tw_err(t, "ERR hash value is not an integer");
		else if (cur == PC_HR_E_OVF)
			pc_tw_err(t, "ERR increment or decrement would overflow");
		else if (cur < 0)
			pc_tw_err(t, hc_toolarge);
		else if (hash_store_local(ht, &k, b, (size_t)cur, exp) != 0)
			pc_tw_err(t, "ERR cache full");
		else
			pc_tw_i64(t, res);
		break;
	}
	case HC_HINCRBYFLOAT: {
		char res[64], tmp[64];
		const unsigned char *fv;
		size_t fvl;

		/* which refusal (pc_hr_incrbyfloat says "not a number" for all three):
		 * probes write a one-field record into b, the real edit overwrites it */
		if (pc_hr_incrbyfloat(NULL, 0, "x", 1, argv[3], argl[3], tmp, b,
		        PC_HR_MAX + 64) < 0) {
			pc_tw_err(t, "ERR value is not a valid float");
			break;
		}
		cur = pc_hr_incrbyfloat(r, n, argv[2], argl[2], argv[3],
			argl[3], res, b, PC_HR_MAX + 64);
		if (cur == PC_HR_E_NUM) {
			if (pc_hr_get(r, n, argv[2], argl[2], &fv, &fvl) &&
			        pc_hr_incrbyfloat(NULL, 0, "x", 1,
			        (const char *)fv, fvl, tmp, b, PC_HR_MAX + 64) < 0)
				pc_tw_err(t, "ERR hash value is not a float");
			else
				pc_tw_err(t, "ERR increment would produce NaN or "
					"Infinity");
		} else if (cur < 0) {
			pc_tw_err(t, hc_toolarge);
		} else if (hash_store_local(ht, &k, b, (size_t)cur, exp) != 0) {
			pc_tw_err(t, "ERR cache full");
		} else {
			pc_tw_bulk(t, res, strlen(res));
		}
		break;
	}
	default:
		pc_tw_err(t, "ERR not a write command");
		break;
	}
out:
	if (mu)
		pthread_mutex_unlock(mu);
}

static const char hc_mode_msg[] = "ERR hash commands need a shard or eager "
	"collection in a cluster: this collection has no node that decides a key";

static __thread char hc_gate_msg[160];

static const char *hc_gate_text(int lacking)
{
	snprintf(hc_gate_msg, sizeof hc_gate_msg, "ERR hash commands need every "
		"node in the fleet on 0.4.4 or later - node %d does not carry "
		"the hash type yet", lacking);
	return hc_gate_msg;
}

/* a forwarded read's encoded arguments, left for the door's park entry (same thread, the door
 * parks right after PARKED); taking clears it */
static __thread unsigned char *hcmd_park_args_v;
static __thread size_t hcmd_park_argl_v;

/* the arguments encoded as a forward carries them, on the heap for the park entry */
static unsigned char *hcmd_enc_heap(int argc, const char *const *argv,
		const size_t *argl, size_t *need_out)
{
	size_t need = 2, o = 2;
	unsigned char *enc;
	int i;

	for (i = 0; i < argc; i++)
		need += 4 + argl[i];
	enc = malloc(need);
	if (!enc)
		return NULL;
	enc[0] = (unsigned char)argc;
	enc[1] = (unsigned char)(argc >> 8);
	for (i = 0; i < argc; i++) {
		enc[o] = (unsigned char)argl[i];
		enc[o + 1] = (unsigned char)(argl[i] >> 8);
		enc[o + 2] = (unsigned char)(argl[i] >> 16);
		enc[o + 3] = (unsigned char)(argl[i] >> 24);
		memcpy(enc + o + 4, argv[i], argl[i]);
		o += 4 + argl[i];
	}
	*need_out = need;
	return enc;
}

/* the node a read pulls the record from to render here, or 0: shard, the owner of a read-through
 * key (a plain key is forwarded); spread, a holder when this node is not one and lacks the key */
static int read_pull_peer(pcache_htable_t *ht, const char *cn, int shard,
		int owner, const char *key, size_t klen)
{
	size_t cl = strlen(cn);
	unsigned long long v;
	int kk = pc_cluster_replicas(), slot;
	str k;

	if (shard)
		return owner && pc_upstream_allowed(cn, cl, key, klen) ? owner : 0;
	slot = pc_key_slot(key, klen);
	if (kk <= 0 || !pc_store_pull_enabled(ht) ||
	        pc_spread_in_set(slot, 0, kk))
		return 0;
	k.s = (char *)key;
	k.len = (int)klen;
	if (pcache_ht_getmeta(ht, &k, &v, NULL) == 0 ||
	        pc_neg_hit(cn, cl, key, klen))
		return 0;
	return pc_spread_pick_peer(slot, kk);
}

int pc_jget_park(pcache_htable_t *ht, const char *key, size_t klen,
		const char *path, size_t plen, int shape, unsigned int *park)
{
	const char *cn = col_name_of(ht);
	int sh = pc_store_shard_enabled(ht), at;
	unsigned char sb = (unsigned char)shape;
	const char *ja[4];
	size_t jl[4], need;
	unsigned char *enc;
	unsigned int req;

	if (!pc_cluster_enabled() || pc_store_proxy_enabled(ht))
		return 0;
	at = read_pull_peer(ht, cn, sh, sh ? pc_shard_owner(cn, strlen(cn),
		key, klen) : 0, key, klen);
	if (at <= 0)
		return 0;
	ja[0] = PC_JGET_PARK;
	jl[0] = sizeof PC_JGET_PARK - 1;
	ja[1] = key;
	jl[1] = klen;
	ja[2] = path;
	jl[2] = plen;
	ja[3] = (const char *)&sb;
	jl[3] = 1;
	if (!(enc = hcmd_enc_heap(4, ja, jl, &need)))
		return 0;
	req = pc_pull_begin_at(cn, strlen(cn), key, klen, at);
	if (!req) {
		free(enc);
		return 0;
	}
	free(hcmd_park_args_v);
	hcmd_park_args_v = enc;
	hcmd_park_argl_v = need;
	*park = req;
	return 1;
}

int pc_jget_park_shape(const unsigned char *args, size_t argsl)
{
	const char *argv[PC_HCMD_MAXARGS];
	size_t argl[PC_HCMD_MAXARGS];
	int argc;

	if (!args || hcmd_decode(args, argsl, argv, argl, &argc) != 0 ||
	        argc != 4 || argl[3] != 1)
		return -1;
	return (unsigned char)argv[3][0];
}

/* PC_OP_OK (the reply in @t), PARKED (forwarded to the deciding node), or
 * PC_OP_ERR_* (GATE / MODE / WRFAIL / 2BIG / BUSY / FWD) */
static int op_hcmd(pcache_htable_t *ht, int ci, int argc,
		const char *const *argv, const size_t *argl, struct pc_tw *t,
		unsigned int *park, int *lacking)
{
	if (argc > 1)                        /* argv[1] is the key */
		HOT(ht, argv[1], argl[1]);
	if (hcmds[ci].write && op_refused(ht))
		return PC_OP_ERR_WRFAIL;
	if (!pc_cluster_fleet_has(PC_FEAT_TYPES2, lacking))
		return PC_OP_ERR_GATE;
	{
		/* one ceiling for the arguments wherever the command runs (the forward
		 * frame's), so a command cannot work on the owner yet fail via a forwarder */
		size_t need = 2;
		int i;

		for (i = 0; i < argc; i++)
			need += 4 + argl[i];
		if (need > PC_MAX_FWD_VAL)
			return PC_OP_ERR_2BIG;
	}
	if (pc_cluster_enabled()) {
		const char *cn = col_name_of(ht);
		int shard = pc_store_shard_enabled(ht), owner;

		if (pc_store_proxy_enabled(ht) ||
		        (!shard && !pc_store_eager_enabled(ht)))
			return PC_OP_ERR_MODE;
		owner = (shard || hcmds[ci].write) ? pc_shard_owner(cn,
			strlen(cn), argv[1], argl[1]) : 0;
		if (!hcmds[ci].write) {
			int at = read_pull_peer(ht, cn, shard, owner, argv[1],
				argl[1]);
			size_t need;
			unsigned char *enc;
			unsigned int req;

			if (at > 0 && (enc = hcmd_enc_heap(argc, argv, argl,
			        &need)) != NULL) {
				req = pc_pull_begin_at(cn, strlen(cn), argv[1],
					argl[1], at);
				if (req) {
					free(hcmd_park_args_v);
					hcmd_park_args_v = enc;
					hcmd_park_argl_v = need;
					*park = req;
					return PC_OP_PARKED;
				}
				free(enc);
			}
		}
		if (owner) {
			size_t need = 2, o = 2;
			unsigned char *enc;
			unsigned int req;
			int i, rd;

			for (i = 0; i < argc; i++)
				need += 4 + argl[i];
			/* a READ asks the owner for the record and renders it here, once
			 * every node speaks it (an older owner runs the old plane) */
			rd = !hcmds[ci].write &&
				pc_cluster_fleet_has(PC_FEAT_HREAD, NULL);
			/* a write's encoding goes in the edit scratch (the forward plane copies it
			 * before returning); a read's outlives the call in the park entry: heap */
			enc = rd ? malloc(need) : hr_buf();
			if (!enc)
				return PC_OP_ERR_FWD;
			enc[0] = (unsigned char)argc;
			enc[1] = (unsigned char)(argc >> 8);
			for (i = 0; i < argc; i++) {
				enc[o] = (unsigned char)argl[i];
				enc[o + 1] = (unsigned char)(argl[i] >> 8);
				enc[o + 2] = (unsigned char)(argl[i] >> 16);
				enc[o + 3] = (unsigned char)(argl[i] >> 24);
				memcpy(enc + o + 4, argv[i], argl[i]);
				o += 4 + argl[i];
			}
			req = pc_fwd_json_begin(owner,
				rd ? PC_JOP_HCMD_READ : PC_JOP_HCMD, cn,
				strlen(cn), argv[1], argl[1], "", 0,
				rd ? "" : (const char *)enc, rd ? 0 : (int)need,
				0, 0, 0, 0, 0, 0);
			if (req) {
				if (rd) {
					/* the door's park entry takes them; the
					 * reply is rendered from them on completion */
					free(hcmd_park_args_v);
					hcmd_park_args_v = enc;
					hcmd_park_argl_v = need;
				}
				*park = req;
				return PC_OP_PARKED;
			}
			if (rd)
				free(enc);
			return fwd_fail_code();
		}
	}
	hcmd_exec(ht, ci, argc, argv, argl, t);
	return t->over ? PC_OP_ERR_FULL : PC_OP_OK;
}

unsigned char *pc_hcmd_take_park_args(size_t *n)
{
	unsigned char *p = hcmd_park_args_v;

	if (n)
		*n = p ? hcmd_park_argl_v : 0;
	hcmd_park_args_v = NULL;
	hcmd_park_argl_v = 0;
	return p;
}

/* the JSON read-modify-write's two per-thread VAL_MAX buffers (record, result), kept for the
 * thread's life; a fragment pc_json_rmw hands back is borrowed until its next call */
static __thread char *jr_doc, *jr_out;

static int jr_scratch(void)
{
	if (!jr_doc)
		jr_doc = malloc(VAL_MAX);
	if (!jr_out)
		jr_out = malloc(VAL_MAX);
	return jr_doc && jr_out ? 0 : -1;
}

/* the deciding node's side of a forwarded hash command: decode, run,
 * ship the tree back as the ack's fragment.  0 ok, -1 a bad frame. */
static int hcmd_remote(pcache_htable_t *ht, const char *val, int vlen,
		char **frag_out, int *fraglen_out)
{
	const char *argv[PC_HCMD_MAXARGS];
	size_t argl[PC_HCMD_MAXARGS];
	struct pc_tw t;
	int argc, ci;

	if (vlen < 0 || hcmd_decode((const unsigned char *)val, (size_t)vlen,
	        argv, argl, &argc) != 0)
		return -1;
	ci = hcmd_find(argv[0], argl[0]);
	if (ci < 0 || !hcmd_arity_ok(ci, argc))
		return -1;
	/* into this thread's result buffer, borrowed by the caller */
	pc_tw_init(&t, (unsigned char *)jr_out, VAL_MAX);
	hcmd_exec(ht, ci, argc, argv, argl, &t);
	*frag_out = jr_out;
	if (t.over) {
		/* larger than the buffer, so far past the forward ack's ceiling: report
		 * too big by length, which the caller refuses (fwd_reply_too_big) */
		*fraglen_out = VAL_MAX + 1;
		return 0;
	}
	if (pc_tw_done(&t) != 0)
		return -1;
	*fraglen_out = (int)t.n;
	return 0;
}

int pc_jget_render_pull(const unsigned char *args, size_t argsl,
		const char *col, size_t collen, const char *rec, size_t rlen,
		int found, long long type, char **frag, int *fraglen)
{
	const char *argv[PC_HCMD_MAXARGS];
	size_t argl[PC_HCMD_MAXARGS];
	pcache_htable_t *ht;
	int argc, rc;

	if (!args || hcmd_decode(args, argsl, argv, argl, &argc) != 0 ||
	        argc < 3 || argl[0] != sizeof PC_JGET_PARK - 1 ||
	        memcmp(argv[0], PC_JGET_PARK, argl[0]) != 0)
		return -1;
	if (!found)
		return 1;
	ht = pc_store_find(col, collen);
	/* as the owner's jget - a string is not a document */
	if (!(type & PCACHE_F_JSON) && (!ht || pc_store_types_strict(ht)))
		return 3;
	if (jr_scratch() != 0)
		return 2;
	rc = pc_jp_get(rec, rlen, argv[2], argl[2], jr_out, VAL_MAX);
	if (rc == PC_JP_E_NOLEAF)
		return 1;
	if (rc < 0)
		return 2;
	*frag = jr_out;
	*fraglen = rc;
	return 0;
}

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
	HOT(ht, key, klen);

	k.s = (char *)key;
	k.len = klen;
	if (op != PC_JOP_RLHIT && jr_scratch() != 0) {
		*errmsg = "out of memory";
		return -1;
	}

	/* not document ops (hash, lock, rate limit): the same stripe and forward plane,
	 * so a forwarded one runs here on the deciding node unchanged */
	if (op == PC_JOP_HCMD) {
		(void)klen; (void)nx; (void)xx; (void)mkpath; (void)path;
		(void)plen; (void)by; (void)have_ttl; (void)ttl;
		if (hcmd_remote(ht, val, vlen, frag_out, fraglen_out) != 0) {
			*errmsg = "bad hash command";
			return -1;
		}
		return 0;
	}
	if (op == PC_JOP_HCMD_READ) {
		unsigned char fl = 0;
		unsigned int vl = 0;

		(void)val; (void)vlen;
		rc = pcache_ht_fetch_buf_fl(ht, &k, jr_doc, VAL_MAX, &vl, NULL,
			&exp, &fl);
		if (rc == -2)
			return 1;                      /* absent: the asker renders the miss */
		if (rc != 0) {
			*errmsg = "get failed";
			return -1;
		}
		if (!(fl & PCACHE_F_HASH)) {
			*errmsg = pc_json_wrongtype_msg;   /* st 3: the asker says WRONGTYPE */
			return -1;
		}
		/* the record itself is the fragment (borrowed; at most PC_HR_MAX, so it
		 * always fits the ack) - the asker renders HGETALL and the rest over it */
		*frag_out = jr_doc;
		*fraglen_out = (int)vl;
		return 0;
	}
	if (op == PC_JOP_SLOCK) {
		const unsigned char *e = (const unsigned char *)val;
		int res = 0, r;

		(void)nx; (void)xx; (void)mkpath; (void)path; (void)plen;
		(void)by; (void)have_ttl; (void)ttl; (void)frag_out;
		(void)fraglen_out; (void)count_out;
		if (vlen < 17) {
			*errmsg = "bad lock request";
			return -1;
		}
		r = slock_local(ht, key, klen, e[0], (long long)rl_r64(e + 1),
			(long long)rl_r64(e + 9), (const char *)e + 17,
			(size_t)vlen - 17, &res, errmsg);
		if (r == 0 && newval)
			*newval = res;
		return r;
	}
	/* a write asks as the op_ cores do, here so a forwarded apply and RESP ask too */
	if ((op == PC_JOP_RLHIT || op == PC_JOP_SET || op == PC_JOP_DEL ||
	        op == PC_JOP_INCR || op == PC_JOP_APPEND) && op_refused(ht)) {
		*errmsg = wr_refused_msg();
		return -1;
	}
	if (op == PC_JOP_RLHIT) {
		long long cnt = 0;
		int al = 0, r;

		(void)nx; (void)xx; (void)mkpath; (void)path; (void)plen;
		(void)val; (void)vlen; (void)frag_out; (void)fraglen_out;
		r = rl_hit_local(ht, key, klen, by, have_ttl ? ttl : 0, &cnt,
			&al, errmsg);
		if (r == 0) {
			if (newval)
				*newval = cnt;
			if (count_out)
				*count_out = al;
		}
		return r;
	}

	if (op == PC_JOP_GET) {
		unsigned char fl = 0;
		unsigned int vl = 0;

		rc = pcache_ht_fetch_buf_fl(ht, &k, jr_doc, VAL_MAX, &vl, NULL,
			&exp, &fl);
		if (rc == -2)
			return 1;
		if (rc != 0) {
			*errmsg = "get failed";
			return -1;
		}
		/* a string is not a document, whatever its bytes */
		if (!(fl & PCACHE_F_JSON) && pc_store_types_strict(ht)) {
			*errmsg = pc_json_wrongtype_msg;
			return -1;
		}
		rc2 = pc_jp_get(jr_doc, (size_t)vl, path, (size_t)plen, jr_out,
			VAL_MAX);
		if (rc2 == PC_JP_E_NOLEAF)
			return 1;
		if (rc2 < 0) {
			*errmsg = jp_strerror(rc2);
			return -1;
		}
		*frag_out = jr_out;                    /* borrowed */
		*fraglen_out = rc2;
		return 0;
	}

	mu = jp_lock(key, klen);
	pthread_mutex_lock(mu);
	{
		unsigned char fl = 0;
		unsigned int vl = 0;

		rc = pcache_ht_fetch_buf_fl(ht, &k, jr_doc, VAL_MAX, &vl, NULL,
			&exp, &fl);
		v.s = jr_doc;
		v.len = (int)vl;
		/* every JSON write - $ or a path, set, del, incr,
		 * append - refuses a key that holds a string */
		if (rc == 0 && !(fl & PCACHE_F_JSON) &&
		        pc_store_types_strict(ht)) {
			pthread_mutex_unlock(mu);
			*errmsg = pc_json_wrongtype_msg;
			return -1;
		}
	}
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
		nd = jr_out;
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
			/* @by: how many values @val joins (RESP's several); 0 or 1 = one */
			rc2 = pc_jp_append(doc, (size_t)v.len, path,
				(size_t)plen, val, (size_t)vlen,
				by > 1 && by <= vlen ? (int)by : 1, count_out,
				nd, VAL_MAX);
		}
		if (op == PC_JOP_DEL && rc2 == PC_JP_E_NOLEAF) {
			pthread_mutex_unlock(mu);
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
		/* what a JSON write stores is a document */
		if (pcache_ht_store_ex(ht, &k, &nvs, newexp, PCACHE_F_JSON) == 0) {
			pc_ev_store(ht, &k, (unsigned int)nvs.len, 0);
			pc_wal_upsert_fl(cn, k.s, k.len, nd, rc2, newexp,
				pcache_last_ver, PCACHE_F_JSON);
			eager_push(ht, &k, nd, (size_t)rc2, newexp,
				pcache_last_ver, PCACHE_F_JSON);
			if (pc_cluster_enabled())
				pc_neg_clear(cn, strlen(cn), k.s,
					(size_t)k.len);
		} else {
			fmsg = "store failed";
		}
	}
	pthread_mutex_unlock(mu);
	if (fmsg) {
		*errmsg = fmsg;
		return -1;
	}
	return 0;
}

/* a collection change written to the collections file: 0 when it could not be - logged, and the
 * verb's reply says persisted false */
static int col_persist(const char *nm, const char *what)
{
	if (pc_store_persist() == 0)
		return 1;
	LM_ERR("collection '%s' %s, but the collections file could not be written to the state "
		"dir - a restart may not keep the change\n", nm, what);
	return 0;
}

/* the {found:true, value, ttl} shape shared by get and mget */
static void write_hit(struct pc_tw *w, const char *v, size_t vn,
		unsigned int expires)
{
	long long ttl = rel_ttl(expires, get_ticks());

	pc_tw_map(w);
	pc_tw_key(w, "found");
	pc_tw_bool(w, 1);
	pc_tw_key(w, "value");
	pc_tw_bulk(w, v, vn);
	pc_tw_key(w, "ttl");
	pc_tw_i64(w, ttl);
	pc_tw_end(w);
}

/* a RECOVERING node may serve a key deleted while it was down - a WRONG answer - so it refuses
 * data verbs; STARTING (empty: truthful) and DRAINING serve, unlike pc_clsel_eligible on purpose */
static int serving_denied(void)
{
	return pc_nst_refuses(pc_node_state());   /* CONNECTING too */
}

/* FAILED refuses WRITES only: its data is still correct, so refusing reads would turn a durability
 * fault into an availability one.  Every dialect's mutation funnels through the op_ cores. */
static int writes_denied(void)
{
	int st = pc_node_state();

	/* HEALING refuses writes too and serves reads, for the same reason FAILED does */
	return st == PC_NST_FAILED || st == PC_NST_HEALING;
}

#define PC_WRFAIL_MSG "node is FAILED (its WAL cannot honour the writes it "\
	"acknowledges) - reads still served here, send writes to another member"
#define PC_WRHEAL_MSG "node is HEALING (its WAL lost acknowledged writes and "\
	"a snapshot is making them durable) - reads still served here, send "\
	"writes to another member or retry shortly"

/* why this thread's last mutation was refused when the reason is the collection (upstream_writes
 * = refuse), not the node; every dialect says it through wr_refused_msg() */
static __thread const char *wr_reason;

const char *wr_refused_msg(void)
{
	const char *m = wr_reason;

	if (m) {
		wr_reason = NULL;
		return m;
	}
	return pc_node_state() == PC_NST_HEALING ? PC_WRHEAL_MSG : PC_WRFAIL_MSG;
}

/* every mutation asks this: the node refusing (FAILED, HEALING), or the collection - by name,
 * so a forwarded apply asks too */
static int op_refused_col(const char *cn, size_t cl)
{
	wr_reason = NULL;
	if (writes_denied())
		return 1;
	if (pc_upstream_any() && cn &&
	    (wr_reason = pc_upstream_write_refusal(cn, cl)) != NULL)
		return 1;
	return 0;
}

static int op_refused(pcache_htable_t *ht)
{
	const char *cn = col_name_of(ht);

	return op_refused_col(cn, cn ? strlen(cn) : 0);
}

#define PC_COL_DEFAULT_LOG2 12         /* 4,096 buckets: ~1.5 MB of index,
                                        * and the splitter grows it */
#define PC_NOCREATE_MSG "creating and dropping collections is not enabled "\
	"here - set [daemon] allow_create = yes on this node"
#define PC_RENAME_BOUND_MSG "a read-through collection and its .nofall "\
	"companion are not renamed live - their upstream is bound to the name "\
	"in the config: rename it there and restart every node at once"
#define PC_RENAME_INTO_BOUND_MSG "the new name is a read-through "\
	"collection's (or its .nofall companion's) in the config - a rename "\
	"into it would bind this collection to that upstream"
static int allow_create;

/* allow_create asks may this NODE originate DDL, the link's privilege bit may THIS LINK (the
 * shared client secret cannot); privileged by blast radius: create/drop/resize/rename, restore */
#define PC_NOPRIV_MSG "this operation needs a privileged connection - "\
	"call enable with the [secrets] enable value first"
#define PC_NOENABLE_MSG "no [secrets] enable is configured on this node, "\
	"so privilege cannot be raised and this operation is unreachable"
static unsigned long long enable_fail;   /* failed enable attempts */

/* wrong enable secrets count per source address: pc_enable_lockout_after within
 * pc_enable_lockout_s seconds locks the address out that long; a right one clears it.  0 = off */
int pc_enable_lockout_after = 5;
int pc_enable_lockout_s = 60;
#define EN_LOCK_MAX 64
static struct en_lock {
	char ip[48];
	int fails;
	long long first_ms, until_ms, last_ms;
} en_lock[EN_LOCK_MAX];
static pthread_mutex_t en_lock_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned long long enable_lockouts;   /* lockouts applied */
static unsigned long long enable_refused;    /* enables refused while locked */

/* the peer's ip without the port ("ip:port" -> "ip"; a v6 "[ip]:port" keeps
 * its brackets, which is fine for a key); 0 when there is none */
static int en_peer_ip(void *conn, char *ip, size_t cap)
{
	const char *t = conn ? pc_conn_peer_text(conn) : NULL;
	const char *colon;
	size_t n;

	if (!t || !*t)
		return 0;
	colon = strrchr(t, ':');
	n = colon ? (size_t)(colon - t) : strlen(t);
	if (n == 0 || n >= cap)
		return 0;
	memcpy(ip, t, n);
	ip[n] = 0;
	return 1;
}

static struct en_lock *en_slot(const char *ip)
{
	int i, oldest = 0;

	for (i = 0; i < EN_LOCK_MAX; i++)
		if (!strcmp(en_lock[i].ip, ip))
			return &en_lock[i];
	for (i = 1; i < EN_LOCK_MAX; i++)
		if (en_lock[i].last_ms < en_lock[oldest].last_ms)
			oldest = i;
	memset(&en_lock[oldest], 0, sizeof en_lock[oldest]);
	snprintf(en_lock[oldest].ip, sizeof en_lock[oldest].ip, "%s", ip);
	return &en_lock[oldest];
}

/* 1 when @conn's address is locked out now */
static int en_locked(void *conn)
{
	char ip[48];
	struct en_lock *l;
	long long now = rl_now_ms();
	int locked;

	if (pc_enable_lockout_after <= 0 || !en_peer_ip(conn, ip, sizeof ip))
		return 0;
	pthread_mutex_lock(&en_lock_mu);
	l = en_slot(ip);
	l->last_ms = now;
	locked = l->until_ms > now;
	if (locked)
		enable_refused++;
	pthread_mutex_unlock(&en_lock_mu);
	return locked;
}

static void en_wrong(void *conn)
{
	char ip[48];
	struct en_lock *l;
	long long now = rl_now_ms(), win = (long long)pc_enable_lockout_s * 1000;

	if (pc_enable_lockout_after <= 0 || !en_peer_ip(conn, ip, sizeof ip))
		return;
	pthread_mutex_lock(&en_lock_mu);
	l = en_slot(ip);
	l->last_ms = now;
	if (!l->fails || now - l->first_ms > win) {
		l->first_ms = now;
		l->fails = 0;
	}
	if (++l->fails >= pc_enable_lockout_after) {
		l->until_ms = now + win;
		l->fails = 0;
		enable_lockouts++;
		LM_WARN("enable: %s offered %d wrong secrets within %d s - its "
			"enable is refused for %d s (%llu lockouts so far)\n", ip,
			pc_enable_lockout_after, pc_enable_lockout_s,
			pc_enable_lockout_s, enable_lockouts);
	}
	pthread_mutex_unlock(&en_lock_mu);
}

static void en_right(void *conn)
{
	char ip[48];
	struct en_lock *l;

	if (pc_enable_lockout_after <= 0 || !en_peer_ip(conn, ip, sizeof ip))
		return;
	pthread_mutex_lock(&en_lock_mu);
	l = en_slot(ip);
	l->fails = 0;
	l->until_ms = 0;
	pthread_mutex_unlock(&en_lock_mu);
}

unsigned long long pc_verb_enable_fails(void)
{
	return enable_fail;
}

unsigned long long pc_verb_enable_lockouts(void)
{
	return __atomic_load_n(&enable_lockouts, __ATOMIC_RELAXED);
}

unsigned long long pc_verb_enable_refused(void)
{
	return __atomic_load_n(&enable_refused, __ATOMIC_RELAXED);
}

void pc_verb_set_allow_create(int on)
{
	allow_create = on;
}

/* [listen] resp_redirect - see pc_verb_resp */
static int resp_redirect_moved;

void pc_verb_set_resp_redirect(int moved)
{
	resp_redirect_moved = moved;
}

/* why a table could not be built: the detailed message when the arena ceiling (checked by the
 * core before it carves) is the cause, else NULL; a resize's old table is already in `held` */
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

#define PC_NOTREADY_MSG "node is not READY (recovering or connecting) - retry, or use "\
	"another member"

/* the member list (PC_CL_MAXMEMBERS x ~1 KB) is too big for a stack: one buffer per thread,
 * made on first use, never freed; only the entries a call fills are touched */
struct pc_member *members_scratch(void)
{
	static __thread struct pc_member *buf;

	if (!buf)
		buf = malloc(sizeof *buf * PC_CL_MAXMEMBERS);
	return buf;
}

const char pc_unknown_method[] = "method not found";

/* what a native verb written through answers with */
struct up_ans {
	long long ival;                /* Redis's number: OK 1, an integer, nil 0 */
	int stored, dropped;           /* mset: the items sent, the ones refused */
	int want_tree;                 /* hcmd: the reply is a tree (UPG_TREE) */
	struct pc_tw tree;
};
static int up_cmd_gate(pcache_htable_t *ht, const char *method, size_t mlen,
		const struct pc_tv *v, int params, const char **errmsg,
		struct up_ans *ans);
static int up_get_lost(pcache_htable_t *ht, const str *k, int *tries);
/* the gate answered a write-through write with Redis's success; the
 * door renders the verb's own reply from ans->ival */
#define UPG_REPLY 3
/* the gate built the verb's reply in ans->tree (hcmd: Redis's reply,
 * an error too, as the tree the hash core builds) */
#define UPG_TREE 4

int pc_verb_cmd(const char *method, size_t mlen, const struct pc_tv *v,
		int params, struct pc_tw *out, const char **errmsg,
		unsigned int *park_req, int *privileged, int *outcome,
		void *conn)
{
	pcache_htable_t *ht;
	str k, v_;
	long long by, ttl;
	int rc, lost_tries = 0;
	unsigned int exp;

	*outcome = 0;
	/* every accessor answers "absent" for params -1, so a method that
	 * takes no parameters and one called without them read alike */
	/* keys, values and payloads are used where they lie in the request tree: the op
	 * cores take spans, and nothing below outlives the request */
#define NEEDKEY() do { \
	const char *kp_; size_t kn_; \
	if (get_bytes(v, params, "key", &kp_, &kn_, KEY_MAX) != 0 || !kn_) \
		ERR("missing or bad key"); \
	k.s = (char *)kp_; k.len = (int)kn_; } while (0)

	/* ---- conninfo: the worker this connection is on and every worker's live native
	 * connections, so a client can move off a crowded one */
	if (m_is(method, mlen, "conninfo")) {
		int c[256], n = pc_obs_worker_conns(c, 256), me = pc_worker_id(), i, least = -1;

		for (i = 0; i < n; i++)
			if (least < 0 || c[i] < least)
				least = c[i];
		pc_tw_map(out);
		pc_tw_key(out, "worker");
		pc_tw_i64(out, me - 1);
		pc_tw_key(out, "workers");
		pc_tw_i64(out, n);
		pc_tw_key(out, "mine");
		pc_tw_i64(out, me >= 1 && me <= n ? c[me - 1] : 0);
		pc_tw_key(out, "least");
		pc_tw_i64(out, least < 0 ? 0 : least);
		/* a client's standby dial asks whether this node serves before it counts it as come up */
		pc_tw_key(out, "state");
		pc_tw_str(out, pc_node_state_name(pc_node_state()));
		pc_tw_key(out, "conns");
		pc_tw_arr(out);
		for (i = 0; i < n; i++)
			pc_tw_i64(out, c[i]);
		pc_tw_end(out);
		pc_tw_end(out);
		return 0;
	}

	/* ---- ping {echo?} : also the transport/codec diagnostic -------- */
	if (m_is(method, mlen, "ping")) {
		int te = pc_tv_get(v, params, "echo");

		pc_tw_map(out);
		pc_tw_key(out, "pong");
		pc_tw_bool(out, 1);
		if (te >= 0 && v->n[te].type == 'b') {
			pc_tw_key(out, "echo");
			pc_tw_bulk(out, v->n[te].p, v->n[te].len);
		}
		pc_tw_end(out);
		return 0;
	}

	/* ---- pub/sub: deliveries are NOTIFY frames (proto.c builds them), so a blocking get
	 * keeps waiting for its own id - the connection stays multiplexed */
	if (m_is(method, mlen, "subscribe") ||
	    m_is(method, mlen, "psubscribe") ||
	    m_is(method, mlen, "unsubscribe") ||
	    m_is(method, mlen, "punsubscribe")) {
		int pat = method[0] == 'p';
		int un = method[pat ? 1 : 0] == 'u';
		char name[4096];
		int nlen = pc_tv_get_str(v, params, pat ? "pattern" : "channel",
			name, sizeof name);
		int n;

		if (!un) {
			if (nlen <= 0)
				ERR(pat ? "missing or bad pattern"
					: "missing or bad channel");
			n = pc_pubsub_subscribe(conn, pc_worker_id(), name,
				(size_t)nlen, pat);
			if (n < 0)
				ERR("cannot subscribe");
		} else if (nlen > 0) {
			n = pc_pubsub_unsubscribe(conn, name, (size_t)nlen, pat);
		} else {
			size_t l;
			int left;

			while (pc_pubsub_pop(conn, pat, name, sizeof name, &l, &left))
				;
			n = pc_pubsub_count(conn);
		}
		pc_tw_map(out);
		pc_tw_key(out, "subscribed");
		pc_tw_i64(out, n);
		pc_tw_end(out);
		return 0;
	}
	if (m_is(method, mlen, "publish")) {
		static __thread char why[64];
		char chan[4096];
		const char *pay;
		size_t plen;
		int clen;

		if (params < 0)
			ERR("missing params");
		clen = pc_tv_get_str(v, params, "channel", chan, sizeof chan);
		if (clen <= 0)
			ERR("missing or bad channel");
		if (pc_pubsub_reserved(chan, (size_t)clen)) {
			snprintf(why, sizeof why, "channel prefix %s is reserved",
				pc_pubsub_reserved(chan, (size_t)clen));
			ERR(why);
		}
		if (get_bytes(v, params, "payload", &pay, &plen, VAL_MAX) != 0)
			ERR("missing or bad payload");
		pc_tw_map(out);
		pc_tw_key(out, "receivers");
		pc_tw_i64(out, pc_pubsub_publish(chan, (size_t)clen, pay, plen,
			0));
		pc_tw_end(out);
		return 0;
	}
	/* pushes over UDP to this connection's own address (lib/perfd_push.h); stream, key
	 * and the cookie stay HEX TEXT, as that header's consumers read them */
	if (m_is(method, mlen, "pubsub_udp")) {
		long long port;
		uint64_t stream;
		char key[2 * PFP_KEY + 1], sid[20];
		int rc;

		if (pc_tv_get_int(v, params, "port", &port) != 0 || port < 0 ||
		        port > 65535)
			ERR("missing or bad port (1..65535, or 0 to stop)");
		rc = pc_conn_udp_on(conn, (int)port, &stream, key, errmsg);
		if (rc < 0)
			return -1;
		if (rc == 1) {
			pc_tw_map(out);
			pc_tw_key(out, "udp");
			pc_tw_bool(out, 0);
			pc_tw_end(out);
			return 0;
		}
		snprintf(sid, sizeof sid, "%016llx", (unsigned long long)stream);
		pc_tw_map(out);
		pc_tw_key(out, "stream");
		pc_tw_bulk(out, sid, 16);
		pc_tw_key(out, "key");
		pc_tw_bulk(out, key, 2 * PFP_KEY);
		pc_tw_key(out, "max_datagram");
		pc_tw_i64(out, PFP_MAX);
		pc_tw_key(out, "ack_every");
		pc_tw_i64(out, PFP_ACK_EVERY);
		pc_tw_key(out, "ack_ms");
		pc_tw_i64(out, PFP_ACK_MS);
		pc_tw_key(out, "prune_ms");
		pc_tw_i64(out, PFP_PRUNE_MS);
		pc_tw_end(out);
		return 0;
	}
	if (m_is(method, mlen, "pubsub_udp_confirm")) {
		char ck[64];
		int n = pc_tv_get_str(v, params, "cookie", ck, sizeof ck);
		uint64_t seq;

		if (n != 2 * PFP_COOKIE)
			ERR("missing or bad cookie (32 hex digits)");
		if (pc_conn_udp_confirm(conn, ck, (size_t)n, &seq, errmsg) != 0)
			return -1;
		pc_tw_map(out);
		pc_tw_key(out, "udp");
		pc_tw_bool(out, 1);
		pc_tw_key(out, "seq");
		pc_tw_i64(out, (long long)seq);
		pc_tw_end(out);
		return 0;
	}
	if (m_is(method, mlen, "pubsub_udp_ack")) {
		long long seq;

		if (pc_tv_get_int(v, params, "seq", &seq) != 0 || seq < 0)
			ERR("missing or bad seq");
		if (pc_conn_udp_ack(conn, (uint64_t)seq, errmsg) != 0)
			return -1;
		pc_tw_map(out);
		pc_tw_key(out, "acked");
		pc_tw_i64(out, seq);
		pc_tw_end(out);
		return 0;
	}
	/* sketch: a collection's distinct-key HyperLogLog from its last held walk (base64 of
	 * PC_HLL_M one-byte registers, fnv1a64 + fmix64), for the dashboard to merge */
	if (m_is(method, mlen, "sketch")) {
		const char *em = NULL;
		pcache_htable_t *sht = get_col(v, params, &em);
		unsigned char regs[PC_HLL_M];
		char b64[sodium_base64_ENCODED_LEN(PC_HLL_M, sodium_base64_VARIANT_ORIGINAL)];
		unsigned int at = 0;
		int si, sidx = -1;

		if (!sht)
			ERR(em);
		for (si = 0; si < pc_store_count(); si++)
			if (pc_store_ht(si) == sht)
				sidx = si;
		if (sidx < 0 || pc_store_sketch(sidx, regs, &at) != 0)
			ERR("no held walk of this collection yet (since a start, a create or a stats reset) - one comes within a minute");
		sodium_bin2base64(b64, sizeof b64, regs, sizeof regs,
			sodium_base64_VARIANT_ORIGINAL);
		pc_tw_map(out);
		pc_tw_key(out, "p");
		pc_tw_i64(out, PC_HLL_P);
		pc_tw_key(out, "hash");
		pc_tw_str(out, "fnv1a64+fmix64");
		pc_tw_key(out, "walked_at");
		pc_tw_i64(out, (long long)at);
		pc_tw_key(out, "registers");
		pc_tw_str(out, b64);
		pc_tw_end(out);
		return 0;
	}
	/* reload this node's configuration file now, answered when it is
	 * done - the same run SIGHUP and `systemctl reload` make */
	if (m_is(method, mlen, "reload")) {
		char rm[512];
		int rrc;

		if (!privileged || !*privileged)
			ERR(pc_enable_configured() ? PC_NOPRIV_MSG
				: PC_NOENABLE_MSG);
		rrc = pc_reload_run(rm, sizeof rm);
		pc_tw_map(out);
		pc_tw_key(out, "applied");
		pc_tw_bool(out, rrc == 0);
		pc_tw_key(out, "message");
		pc_tw_str(out, rm);
		pc_tw_end(out);
		return 0;
	}
	/* ---- fleetstop {timeout_ms?}: privileged - nothing has a larger blast radius.  Each peer
	 * stops as SIGTERM stops it; this node waits, reports who went, then stops itself last */
	if (m_is(method, mlen, "fleetstop")) {
		int gone[64], still[64], lost[64], ng = 0, ns = 0, nl = 0,
			nt, j;
		long long tmo = 30000;

		if (!privileged || !*privileged)
			ERR(pc_enable_configured() ? PC_NOPRIV_MSG
				: PC_NOENABLE_MSG);
		if (!pc_cluster_enabled())
			ERR("this node is not clustered - stop it with "
				"SIGTERM, which takes a snapshot on the way down");
		pc_tv_get_int(v, params, "timeout_ms", &tmo);
		if (tmo < 1000)
			tmo = 1000;
		if (tmo > 120000)
			tmo = 120000;
		nt = pc_cluster_fleet_stop(tmo, gone, &ng, still, &ns, lost,
			&nl, 64);
		if (nt < 0)
			ERR("this node is not clustered");
		pc_tw_map(out);
		pc_tw_key(out, "stopping");
		pc_tw_bool(out, 1);
		pc_tw_key(out, "asked");
		pc_tw_i64(out, nt);
		pc_tw_key(out, "stopped");
		pc_tw_arr(out);
		for (j = 0; j < ng; j++)
			pc_tw_i64(out, gone[j]);
		pc_tw_end(out);
		pc_tw_key(out, "still_up");
		pc_tw_arr(out);
		for (j = 0; j < ns; j++)
			pc_tw_i64(out, still[j]);
		pc_tw_end(out);
		/* left the live set without a goodbye */
		pc_tw_key(out, "lost");
		pc_tw_arr(out);
		for (j = 0; j < nl; j++)
			pc_tw_i64(out, lost[j]);
		pc_tw_end(out);
		pc_tw_end(out);
		return 0;
	}
	/* forget {node}: a member removed for good - its reserved id released here, its card and
	 * fleet.node warning gone; each node holds its own reservations */
	if (m_is(method, mlen, "forget")) {
		long long node = 0;
		const char *why;

		if (!privileged || !*privileged)
			ERR(pc_enable_configured() ? PC_NOPRIV_MSG
				: PC_NOENABLE_MSG);
		if (pc_tv_get_int(v, params, "node", &node) != 0)
			ERR("forget wants {\"node\": <a departed member's node id>}");
		if ((why = pc_cluster_forget(node < 1 || node > PC_CL_NODE_MAX ?
		        -1 : (int)node)))
			ERR(why);
		pc_tw_map(out);
		pc_tw_key(out, "forgotten");
		pc_tw_i64(out, node);
		pc_tw_end(out);
		return 0;
	}
	/* master_prefer {node} on the master hands mastership to that
	 * member once it is the synced standby; master_auto (node 0) cancels */
	if (m_is(method, mlen, "master_prefer") || m_is(method, mlen, "master_auto")) {
		long long node = 0;
		const char *why;

		if (!privileged || !*privileged)
			ERR(pc_enable_configured() ? PC_NOPRIV_MSG
				: PC_NOENABLE_MSG);
		if (m_is(method, mlen, "master_prefer") &&
		    (pc_tv_get_int(v, params, "node", &node) != 0 || node < 1 ||
		     node > PC_CL_NODE_MAX))
			ERR("master_prefer wants {\"node\": <a member's node id>}");
		if ((why = pc_cluster_master_prefer((int)node)))
			ERR(why);
		pc_tw_map(out);
		pc_tw_key(out, "handover_to");
		pc_tw_i64(out, node);
		pc_tw_end(out);
		return 0;
	}

	/* ---- save : request an RDB snapshot ---------------------------- */
	if (m_is(method, mlen, "save")) {
		struct pc_rdb_stats rs;

		pc_rdb_get_stats(&rs);
		if (!rs.enabled)
			ERR("persistence is not configured");
		pc_tw_map(out);
		pc_tw_key(out, "started");
		if (pc_rdb_request_save() == 0) {
			pc_tw_bool(out, 1);
		} else {
			pc_tw_bool(out, 0);
			pc_tw_key(out, "reason");
			pc_tw_str(out, "already running");
		}
		pc_tw_end(out);
		return 0;
	}

	/* ---- sync {timeout_ms?} : WAL barrier - block until everything
	 * appended so far is on the platter ------------------------------ */
	if (m_is(method, mlen, "sync")) {
		long long tmo = 0;
		int rc2;

		pc_tv_get_int(v, params, "timeout_ms", &tmo);
		/* the wait holds this worker: a stalled device never holds it longer than 10 s */
		if (tmo < 0 || tmo > 10000)
			tmo = tmo < 0 ? 0 : 10000;
		rc2 = pc_wal_sync((int)tmo);
		if (rc2 == 1)
			ERR("persistence is not configured");
		if (rc2 == -2)
			ERR("the WAL's storage failed - what was "
				"written after the last good sync is not on "
				"the device, and will not be synced by this "
				"process");
		if (rc2 < 0)
			ERR("sync timed out");
		{
			struct pc_wal_stats ws;

			pc_wal_get_stats(&ws);
			/* a ring-full drop discards a record that already holds a
			 * sequence number: `dropped` tells a partial barrier from a full one */
			pc_tw_map(out);
			pc_tw_key(out, "synced");
			pc_tw_bool(out, 1);
			pc_tw_key(out, "seq");
			pc_tw_i64(out, (long long)ws.synced_seq);
			pc_tw_key(out, "dropped");
			pc_tw_i64(out, (long long)ws.dropped);
			pc_tw_end(out);
		}
		return 0;
	}

	/* ---- load : import the current snapshot ADDITIVELY (existing
	 * keys win; expired records dropped); privileged ------------------ */
	if (m_is(method, mlen, "load")) {
		long ld = 0, se = 0, sx = 0;
		int rc2;

		if (!pc_rdb_dir()[0])
			ERR("persistence is not configured");
		if (!privileged || !*privileged)
			ERR(pc_enable_configured() ? PC_NOPRIV_MSG
				: PC_NOENABLE_MSG);
		rc2 = pc_rdb_import(pc_rdb_dir(), &ld, &se, &sx);
		if (rc2 == -2)
			ERR("a load is already running");
		if (rc2 != 0)
			ERR("no readable snapshot");
		pc_tw_map(out);
		pc_tw_key(out, "loaded");
		pc_tw_i64(out, ld);
		pc_tw_key(out, "skipped_existing");
		pc_tw_i64(out, se);
		pc_tw_key(out, "skipped_expired");
		pc_tw_i64(out, sx);
		pc_tw_end(out);
		return 0;
	}

	/* ---- create {col, buckets_log2?} / drop {col, force?}: [daemon] allow_create gates
	 * only ORIGINATION - a create reaches every member, and takes the cluster's mode */
	if (m_is(method, mlen, "create")) {
		char nm[PC_COL_NAME_MAX];
		long long bl = PC_COL_DEFAULT_LOG2;
		unsigned long long gen;
		int nl, rc2, saved;

		if (!allow_create)
			ERR(PC_NOCREATE_MSG);
		if (!privileged || !*privileged)
			ERR(pc_enable_configured() ? PC_NOPRIV_MSG
				: PC_NOENABLE_MSG);
		if (writes_denied())
			ERR(PC_NOTREADY_MSG);
		nl = pc_tv_get_str(v, params, "col", nm, sizeof nm - 1);
		if (nl <= 0)
			ERR("missing col");
		nm[nl] = '\0';
		/* a client names a collection as the config does */
		if (!pc_col_name_ok(nm, (size_t)nl))
			ERR(PC_COL_NAME_RULE);
		pc_tv_get_int(v, params, "buckets_log2", &bl);
		if (bl < 4 || bl > 24)
			ERR("buckets_log2 is 4..24");
		gen = pc_lamport_tick();
		rc2 = pc_store_create_gen(nm, (size_t)nl, (int)bl, gen);
		if (rc2 == -2)
			ERR("the collection limit is reached");
		if (rc2 == -3) {
			const char *why = index_ceiling_msg((int)bl);

			ERR(why ? why : "the table could not be created "
				"- the arena has no room for its index");
		}
		if (rc2 != 0)
			ERR("a collection of that name already exists");
		/* the WAL first, then the file: both are read at startup and
		 * the WAL's ordering settles a disagreement between them */
		pc_wal_col_create(nm, (int)bl);
		saved = col_persist(nm, "created");
		pc_cluster_col_announce_gen(nm, (size_t)nl, (int)bl, CLCOL_OP_SET, gen);
		LM_NOTICE("collection '%s' created by a client: 2^%d buckets, "
			"%s mode\n", nm, (int)bl, pc_cluster_mode_name());
		pc_tw_map(out);
		pc_tw_key(out, "created");
		pc_tw_bool(out, 1);
		pc_tw_key(out, "col");
		pc_tw_bulk(out, nm, (size_t)nl);
		pc_tw_key(out, "buckets");
		pc_tw_i64(out, 1LL << bl);
		if (!saved) {
			pc_tw_key(out, "persisted");
			pc_tw_bool(out, 0);
		}
		pc_tw_end(out);
		return 0;
	}
	if (m_is(method, mlen, "drop")) {
		char nm[PC_COL_NAME_MAX];
		pcache_ht_totals_t tot;
		unsigned long long gen;
		long long n = 0;
		int nl, force, saved;

		if (!allow_create)
			ERR(PC_NOCREATE_MSG);
		if (!privileged || !*privileged)
			ERR(pc_enable_configured() ? PC_NOPRIV_MSG
				: PC_NOENABLE_MSG);
		if (writes_denied())
			ERR(PC_NOTREADY_MSG);
		nl = pc_tv_get_str(v, params, "col", nm, sizeof nm - 1);
		if (nl <= 0)
			ERR("missing col");
		nm[nl] = '\0';
		force = pc_tv_get_bool(v, params, "force");
		ht = pc_store_find(nm, (size_t)nl);
		if (!ht)
			ERR("no such collection");
		pcache_ht_totals(ht, &tot);
		if (tot.entries && !force) {
			pc_tw_map(out);
			pc_tw_key(out, "dropped");
			pc_tw_bool(out, 0);
			pc_tw_key(out, "entries");
			pc_tw_i64(out, (long long)tot.entries);
			pc_tw_key(out, "why");
			pc_tw_str(out, "the collection is not empty - pass force to drop it with its records");
			pc_tw_end(out);
			return 0;
		}
		gen = pc_lamport_tick();
		n = pc_store_drop_gen(nm, (size_t)nl, gen);
		if (n < 0)
			ERR("no such collection");
		pc_wal_col_drop(nm);
		saved = col_persist(nm, "dropped");
		pc_cluster_col_announce_gen(nm, (size_t)nl, 0, CLCOL_OP_DROP, gen);
		LM_NOTICE("collection '%s' dropped by a client: %lld record(s) "
			"freed\n", nm, n);
		pc_tw_map(out);
		pc_tw_key(out, "dropped");
		pc_tw_bool(out, 1);
		pc_tw_key(out, "col");
		pc_tw_bulk(out, nm, (size_t)nl);
		pc_tw_key(out, "records");
		pc_tw_i64(out, n);
		if (!saved) {
			pc_tw_key(out, "persisted");
			pc_tw_bool(out, 0);
		}
		pc_tw_end(out);
		return 0;
	}

	/* ---- rename {col, to}: one pointer swap, persisted at once.  A load maps the snapshot's and
	 * the WAL's older name through the WAL's rename records (recover.c) */
	if (m_is(method, mlen, "rename")) {
		char nm[PC_COL_NAME_MAX], to[PC_COL_NAME_MAX];
		int nl, tl, rc2, saved;

		if (!allow_create)
			ERR(PC_NOCREATE_MSG);
		if (!privileged || !*privileged)
			ERR(pc_enable_configured() ? PC_NOPRIV_MSG
				: PC_NOENABLE_MSG);
		if (writes_denied())
			ERR(PC_NOTREADY_MSG);
		nl = pc_tv_get_str(v, params, "col", nm, sizeof nm - 1);
		tl = pc_tv_get_str(v, params, "to", to, sizeof to - 1);
		if (nl <= 0 || tl <= 0)
			ERR("rename takes col and to");
		nm[nl] = '\0';
		to[tl] = '\0';
		/* only the new name must follow the rule: a rename is how an
		 * older name gets one */
		if (!pc_col_name_ok(to, (size_t)tl))
			ERR(PC_COL_NAME_RULE);
		/* an upstream binds a collection (and its .nofall companion) by its config
		 * name: a bound name moves only by config and a restart of every node */
		if (pc_upstream_binds(nm, (size_t)nl))
			ERR(PC_RENAME_BOUND_MSG);
		if (pc_upstream_binds(to, (size_t)tl))
			ERR(PC_RENAME_INTO_BOUND_MSG);
		rc2 = pc_store_rename(nm, (size_t)nl, to, (size_t)tl,
			pc_lamport_tick());
		if (rc2 == -1)
			ERR("no such collection");
		if (rc2 == -2)
			ERR("a collection of the new name already exists");
		if (rc2 != 0)
			ERR("the rename failed");
		pc_wal_col_rename(nm, to);
		saved = col_persist(to, "renamed");
		pc_cluster_col_announce2(nm, (size_t)nl, to, (size_t)tl);
		LM_NOTICE("collection '%s' renamed to '%s' by a client\n", nm, to);
		pc_tw_map(out);
		pc_tw_key(out, "renamed");
		pc_tw_bool(out, 1);
		pc_tw_key(out, "col");
		pc_tw_bulk(out, nm, (size_t)nl);
		pc_tw_key(out, "to");
		pc_tw_bulk(out, to, (size_t)tl);
		if (!saved) {
			pc_tw_key(out, "persisted");
			pc_tw_bool(out, 0);
		}
		pc_tw_end(out);
		return 0;
	}

	/* ---- map {index, col} / unmap {index}: the RESP door's DB-index map, fleet state by
	 * Lamport generation - refused while a live member predates it; gated as rename is */
	if (m_is(method, mlen, "map") || m_is(method, mlen, "unmap")) {
		static __thread char why[192];
		char col[PC_COL_NAME_MAX] = "", was[PC_COL_NAME_MAX];
		int set = m_is(method, mlen, "map"), cl = 0, lacking = 0, r;
		long long idx = -1;
		uint64_t gen;
		const char *who = conn ? pc_conn_peer_text(conn) : NULL;

		if (!allow_create)
			ERR("changing the RESP DB-index map is not enabled here - set "
				"[daemon] allow_create = yes on this node (the gate of "
				"every collection change)");
		if (!privileged || !*privileged)
			ERR(pc_enable_configured() ? PC_NOPRIV_MSG
				: PC_NOENABLE_MSG);
		if (writes_denied())
			ERR(PC_NOTREADY_MSG);
		if (pc_tv_get_int(v, params, "index", &idx) != 0 ||
		        idx < 0 || idx >= PC_RESPMAP_MAX) {
			snprintf(why, sizeof why, "%s takes index (0..%d)%s",
				set ? "map" : "unmap", PC_RESPMAP_MAX - 1,
				set ? " and col" : "");
			ERR(why);
		}
		if (set) {
			cl = pc_tv_get_str(v, params, "col", col, sizeof col - 1);
			if (cl <= 0)
				ERR("map takes index and col");
			col[cl] = '\0';
			if (!pc_store_find(col, (size_t)cl))
				ERR("no such collection");
		} else if (pc_respmap_resolve((int)idx, was, sizeof was) !=
		        PC_RESPMAP_RUNTIME) {
			snprintf(why, sizeof why, "DB index %lld is not mapped at "
				"runtime - it answers as resp_collections maps it",
				idx);
			ERR(why);
		}
		if (!pc_cluster_fleet_has(PC_FEAT_RESPMAP, &lacking)) {
			snprintf(why, sizeof why, "every member must run 0.7.1 or "
				"later to map a RESP index at runtime - node %d is "
				"older", lacking);
			ERR(why);
		}
		gen = pc_lamport_tick();
		r = pc_respmap_apply((int)idx, col, (size_t)cl, gen, set, was,
			sizeof was);
		if (r == -2)
			ERR("out of memory - the map is unchanged");
		if (r < 0)
			ERR("a collection name the RESP map cannot hold (',', ':', "
				"'*' or a space)");
		if (r == 0)
			ERR("a newer change to this index is already held - read "
				"maps and retry");
		if (pc_respmap_persist() != 0)
			LM_ERR("resp: the runtime map could not be written to the "
				"state dir - it holds until a restart\n");
		pc_cluster_respmap_announce((int)idx, col, gen, set);
		if (set)
			LM_NOTICE("resp: DB index %lld mapped to collection '%s' by "
				"%s (was '%s')\n", idx, col, who && *who ? who : "a client",
				was[0] ? was : "-");
		else
			LM_NOTICE("resp: DB index %lld unmapped by %s (was '%s'): it "
				"answers as resp_collections maps it\n", idx,
				who && *who ? who : "a client", was[0] ? was : "-");
		pc_tw_map(out);
		pc_tw_key(out, set ? "mapped" : "unmapped");
		pc_tw_bool(out, 1);
		pc_tw_key(out, "index");
		pc_tw_i64(out, idx);
		pc_tw_key(out, "col");
		{
			char now[PC_COL_NAME_MAX];

			pc_respmap_resolve((int)idx, now, sizeof now);
			pc_tw_str(out, now);
		}
		pc_tw_key(out, "was");
		pc_tw_str(out, was);
		pc_tw_end(out);
		return 0;
	}
	/* maps: every index the door answers, what it reaches and who put it there; ungated */
	if (m_is(method, mlen, "maps")) {
		pc_tw_map(out);
		pc_tw_key(out, "config");
		if (pc_respmap_config())
			pc_tw_str(out, pc_respmap_config());
		else
			pc_tw_nil(out);             /* no list: every collection */
		pc_tw_key(out, "overrides");
		pc_tw_i64(out, pc_respmap_overrides());
		pc_tw_key(out, "maps");
		respmap_rows(out);
		pc_tw_end(out);
		return 0;
	}

	/* ---- mailtest: one mail now, holding this worker for the SMTP answer; a refused mail
	 * is a RESULT ({sent: false, reply}), not an error - the reply is what was asked for */
	if (m_is(method, mlen, "mailtest")) {
		static __thread char mail_err[PC_MAIL_REPLY_MAX];
		char reply[PC_MAIL_REPLY_MAX];
		int r;

		if (!privileged || !*privileged)
			ERR(pc_enable_configured() ? PC_NOPRIV_MSG
				: PC_NOENABLE_MSG);
		r = pc_mail_test(reply, sizeof reply);
		if (r < 0) {
			snprintf(mail_err, sizeof mail_err, "%s", reply);
			ERR(mail_err);
		}
		pc_tw_map(out);
		pc_tw_key(out, "sent");
		pc_tw_bool(out, r == 1);
		pc_tw_key(out, "reply");
		pc_tw_str(out, reply);
		pc_tw_end(out);
		return 0;
	}

	if (m_is(method, mlen, "cluster_epoch_commit")) {
		/* the refusal's words outlive this frame: the door strlen()s
		 * errmsg after the verb returns */
		static __thread char why[200];
		char who[160];
		long long epoch = 0;

		if (!privileged || !*privileged)
			ERR(pc_enable_configured() ? PC_NOPRIV_MSG
				: PC_NOENABLE_MSG);
		if (pc_tv_get_int(v, params, "epoch", &epoch) != 0 || epoch <= 0 ||
		    epoch > 255)
			ERR("missing epoch");
		if (pc_cluster_epoch_commit((int)epoch, why, sizeof why) != 0)
			ERR(why);
		LM_NOTICE("cluster: wire epoch %lld committed - by %s\n", epoch,
			pc_conn_who(conn, who, sizeof who));
		pc_tw_map(out);
		pc_tw_key(out, "committed");
		pc_tw_i64(out, epoch);
		pc_tw_end(out);
		return 0;
	}
	/* ---- updrain {col, stop?}: take over every key the upstream holds that the fleet lacks;
	 * privileged - it reads a whole keyspace from what may be a production server */
	if (m_is(method, mlen, "updrain")) {
		char nm[PC_COL_NAME_MAX];
		const char *msg = "";
		int nl, st;

		if (!privileged || !*privileged)
			ERR(pc_enable_configured() ? PC_NOPRIV_MSG
				: PC_NOENABLE_MSG);
		nl = pc_tv_get_str(v, params, "col", nm, sizeof nm - 1);
		if (nl <= 0)
			ERR("missing col");
		nm[nl] = '\0';
		st = pc_tv_get_bool(v, params, "stop") > 0;
		if (pc_upstream_drain_ctl(nm, (size_t)nl, st, &msg) != 0)
			ERR(msg);
		{
			char who[160];

			LM_NOTICE("collection '%.*s': updrain%s %s - by %s\n", nl, nm,
				st ? " stop" : "", msg, pc_conn_who(conn, who, sizeof who));
		}
		pc_tw_map(out);
		pc_tw_key(out, "col");
		pc_tw_bulk(out, nm, (size_t)nl);
		pc_tw_key(out, "drain");
		pc_tw_str(out, msg);
		pc_tw_end(out);
		return 0;
	}

	/* ---- upfill {col, stop?, check?}: keys the fleet owned before upstream_writes = through,
	 * written with NX to the master - privileged: it may be a production server */
	if (m_is(method, mlen, "upfill")) {
		char nm[PC_COL_NAME_MAX];
		const char *msg = "";
		int nl, st;

		if (!privileged || !*privileged)
			ERR(pc_enable_configured() ? PC_NOPRIV_MSG
				: PC_NOENABLE_MSG);
		nl = pc_tv_get_str(v, params, "col", nm, sizeof nm - 1);
		if (nl <= 0)
			ERR("missing col");
		nm[nl] = '\0';
		st = pc_tv_get_bool(v, params, "stop") > 0;
		/* check compares with Redis, writing nothing */
		if (pc_upstream_upfill_ctl(nm, (size_t)nl, st,
		        pc_tv_get_bool(v, params, "check") > 0, &msg) != 0)
			ERR(msg);
		{
			char who[160];

			LM_NOTICE("collection '%.*s': upfill%s %s - by %s\n", nl, nm,
				pc_tv_get_bool(v, params, "check") > 0 ? " check" : "",
				msg, pc_conn_who(conn, who, sizeof who));
		}
		pc_tw_map(out);
		pc_tw_key(out, "col");
		pc_tw_bulk(out, nm, (size_t)nl);
		pc_tw_key(out, "upfill");
		pc_tw_str(out, msg);
		pc_tw_end(out);
		return 0;
	}

	/* ---- upgrade_stage {op: begin|chunk|end|abort, ...}: a build staged on this node.
	 * Chunks are written in order as they arrive; end verifies the whole file first */
	if (m_is(method, mlen, "upgrade_stage")) {
		char op[8], id[33], sha[65], sx[2 * PC_UPG_SIG_LEN + 1], ver[48];
		char who[96];
		unsigned char sig[PC_UPG_SIG_LEN];
		const char *e = "";
		long long n = 0;
		int l;

		if (!privileged || !*privileged)
			ERR(pc_enable_configured() ? PC_NOPRIV_MSG
				: PC_NOENABLE_MSG);
		l = pc_tv_get_str(v, params, "op", op, sizeof op - 1);
		if (l <= 0)
			ERR("missing op: begin, chunk, end or abort");
		op[l] = '\0';
		if (!strcmp(op, "begin")) {
			size_t bl = 0;
			int hs;

			if (pc_tv_get_int(v, params, "size", &n) != 0)
				ERR("missing size");
			l = pc_tv_get_str(v, params, "sha256", sha, sizeof sha - 1);
			sha[l > 0 ? l : 0] = '\0';
			l = pc_tv_get_str(v, params, "version", ver, sizeof ver - 1);
			ver[l > 0 ? l : 0] = '\0';
			l = pc_tv_get_str(v, params, "sig", sx, sizeof sx - 1);
			hs = l > 0;
			if (hs && (l != 2 * PC_UPG_SIG_LEN || sodium_hex2bin(sig,
			        sizeof sig, sx, (size_t)l, NULL, &bl, NULL) != 0 ||
			        bl != PC_UPG_SIG_LEN))
				ERR("sig must be the signature as 128 hex characters");
			if (pc_upg_stage_begin(n, sha, hs ? sig : NULL, ver, id,
			        sizeof id, &e) != 0)
				ERR(e);
			pc_tw_map(out);
			pc_tw_key(out, "id");
			pc_tw_str(out, id);
			pc_tw_key(out, "chunk_max");
			pc_tw_i64(out, PC_UPG_CHUNK_MAX);
			pc_tw_end(out);
			return 0;
		}
		l = pc_tv_get_str(v, params, "id", id, sizeof id - 1);
		if (l <= 0)
			ERR("missing id");
		id[l] = '\0';
		if (!strcmp(op, "chunk")) {
			long long off = -1, got = 0;
			unsigned char *raw;
			size_t rl = 0;
			int k = pc_tv_get(v, params, "data"), r;

			if (pc_tv_get_int(v, params, "off", &off) != 0)
				ERR("missing off");
			if (k < 0 || v->n[k].type != 'b' || !v->n[k].len)
				ERR("missing data (base64)");
			raw = malloc(PC_UPG_CHUNK_MAX);
			if (!raw)
				ERR("out of memory");
			if (sodium_base642bin(raw, PC_UPG_CHUNK_MAX,
			        (const char *)v->n[k].p, v->n[k].len, "\r\n", &rl,
			        NULL, sodium_base64_VARIANT_ORIGINAL) != 0) {
				free(raw);
				ERR("data is not base64, or decodes past 256 KB");
			}
			r = pc_upg_stage_chunk(id, off, raw, rl, &got, &e);
			free(raw);
			if (r != 0)
				ERR(e);
			pc_tw_map(out);
			pc_tw_key(out, "got");
			pc_tw_i64(out, got);
			pc_tw_end(out);
			return 0;
		}
		if (!strcmp(op, "end")) {
			int rl, older = 0;

			if (pc_upg_stage_end(id, sha, sizeof sha, who, sizeof who,
			        &e) != 0)
				ERR(e);
			/* then to every member that takes one; each verifies it
			 * itself, and answers in upgrade_status's relay */
			rl = pc_upg_relay_staged(sha, &older);
			pc_tw_map(out);
			pc_tw_key(out, "sha256");
			pc_tw_str(out, sha);
			pc_tw_key(out, "signer");
			pc_tw_str(out, who);
			pc_tw_key(out, "relayed_to");
			pc_tw_i64(out, rl);
			pc_tw_key(out, "older_members");
			pc_tw_i64(out, older);
			pc_tw_end(out);
			return 0;
		}
		if (!strcmp(op, "abort")) {
			if (pc_upg_stage_abort(id, &e) != 0)
				ERR(e);
			pc_tw_map(out);
			pc_tw_key(out, "aborted");
			pc_tw_bool(out, 1);
			pc_tw_end(out);
			return 0;
		}
		ERR("op: begin, chunk, end or abort");
	}
	/* ---- upgrade_switch {sha256, force?}: the host's root helper installs a build staged
	 * here, restarting this node into it; its word is upgrade_status.requested.result */
	if (m_is(method, mlen, "upgrade_switch")) {
		char sha[65], id[33], ver[48];
		const char *e = "";
		int l;

		if (!privileged || !*privileged)
			ERR(pc_enable_configured() ? PC_NOPRIV_MSG
				: PC_NOENABLE_MSG);
		l = pc_tv_get_str(v, params, "sha256", sha, sizeof sha - 1);
		if (l <= 0)
			ERR("missing sha256");
		sha[l] = '\0';
		{
			/* {member: "a.b.c.d"} asks that member instead
			 * (M_UPG_SWITCH); its answer is upgrade_status.asked */
			char mb[INET_ADDRSTRLEN];
			struct in_addr ma;

			l = pc_tv_get_str(v, params, "member", mb, sizeof mb - 1);
			if (l > 0) {
				mb[l] = '\0';
				if (inet_pton(AF_INET, mb, &ma) != 1)
					ERR("member must be an IPv4 address");
				if (pc_upg_ask_member(ma.s_addr, sha, &e) != 0)
					ERR(e);
				pc_tw_map(out);
				pc_tw_key(out, "asked");
				pc_tw_str(out, mb);
				pc_tw_end(out);
				return 0;
			}
		}
		if (pc_upg_switch(sha, pc_tv_get_bool(v, params, "force") > 0, id,
		        sizeof id, ver, sizeof ver, &e) != 0)
			ERR(e);
		pc_tw_map(out);
		pc_tw_key(out, "request");
		pc_tw_str(out, id);
		pc_tw_key(out, "version");
		pc_tw_str(out, ver);
		pc_tw_key(out, "helper");
		pc_tw_str(out, pc_upg_helper_state());
		pc_tw_end(out);
		return 0;
	}
	/* ---- upgrade_revert {force?}: the helper switches this node back to the prefix it
	 * kept before its last switch - a failed gate's revert, the driver's rollback */
	if (m_is(method, mlen, "upgrade_revert")) {
		char id[33], to[200], who[160];
		const char *e = "";
		int force = pc_tv_get_bool(v, params, "force") > 0;

		if (!privileged || !*privileged)
			ERR(pc_enable_configured() ? PC_NOPRIV_MSG
				: PC_NOENABLE_MSG);
		if (pc_upg_revert(force, id, sizeof id, to, sizeof to, &e) != 0)
			ERR(e);
		LM_NOTICE("upgrade: revert to %s requested (%s) by %s\n", to, id,
			pc_conn_who(conn, who, sizeof who));
		pc_tw_map(out);
		pc_tw_key(out, "request");
		pc_tw_str(out, id);
		pc_tw_key(out, "to");
		pc_tw_str(out, to);
		pc_tw_key(out, "helper");
		pc_tw_str(out, pc_upg_helper_state());
		pc_tw_end(out);
		return 0;
	}

	/* ---- upgrade_status: what is staged here and the upload in progress; unprivileged */
	if (m_is(method, mlen, "upgrade_status")) {
		pc_upg_status_tree(out);
		return 0;
	}

	/* ---- autoscale {col, mode}: persisted, outranking the config; fleet state ordered by
	 * generation, refused while a member predates it - auto sizes each member's share */
	if (m_is(method, mlen, "autoscale")) {
		static __thread char why[96];
		char nm[PC_COL_NAME_MAX], md[16];
		int nl, ml, mode, lacking = 0, rc, was = -1;
		unsigned long long gen;

		if (!privileged || !*privileged)
			ERR(pc_enable_configured() ? PC_NOPRIV_MSG
				: PC_NOENABLE_MSG);
		nl = pc_tv_get_str(v, params, "col", nm, sizeof nm - 1);
		if (nl <= 0)
			ERR("missing col");
		nm[nl] = '\0';
		ml = pc_tv_get_str(v, params, "mode", md, sizeof md - 1);
		if (ml <= 0)
			ERR("missing mode (auto|floor|warn|off)");
		md[ml] = '\0';
		if ((mode = pc_autoscale_parse(md)) < 0)
			ERR("mode is auto, floor, warn or off");
		/* a member that cannot take it would keep its own: one collection sized two ways */
		if (!pc_cluster_fleet_has(PC_FEAT_AUTOSCALE, &lacking)) {
			snprintf(why, sizeof why, "every member must run 0.7.2 or "
				"later to set autoscale - node %d is older", lacking);
			ERR(why);
		}
		gen = pc_lamport_tick();
		rc = pc_store_autoscale_apply(nm, (size_t)nl, mode, gen, &was);
		if (rc < 0)
			ERR("no such collection");
		if (rc == 0)
			ERR("a newer autoscale change to this collection is already "
				"held - read stats and retry");
		(void)pc_store_persist();
		pc_cluster_autoscale_announce(nm, (size_t)nl, mode, gen);
		LM_NOTICE("collection '%s': autoscale = %s, set by a client "
			"(was %s)\n", nm, md, pc_autoscale_name(was));
		pc_tw_map(out);
		pc_tw_key(out, "col");
		pc_tw_bulk(out, nm, (size_t)nl);
		pc_tw_key(out, "autoscale");
		pc_tw_str(out, pc_autoscale_name(mode));
		pc_tw_end(out);
		return 0;
	}
	/* ---- resize {col, buckets_log2}: runs on the maintenance thread in bounded passes and
	 * returns once started; `stats` carries the progress */
	if (m_is(method, mlen, "resize")) {
		char nm[PC_COL_NAME_MAX];
		long long bl = 0;
		int nl, rc2;

		if (!allow_create)
			ERR(PC_NOCREATE_MSG);
		if (!privileged || !*privileged)
			ERR(pc_enable_configured() ? PC_NOPRIV_MSG
				: PC_NOENABLE_MSG);
		if (writes_denied())
			ERR(PC_NOTREADY_MSG);
		nl = pc_tv_get_str(v, params, "col", nm, sizeof nm - 1);
		if (nl <= 0)
			ERR("missing col");
		nm[nl] = '\0';
		if (pc_tv_get_int(v, params, "buckets_log2", &bl) != 0)
			ERR("missing buckets_log2");
		rc2 = pc_store_resize_start(nm, (size_t)nl, (int)bl);
		if (rc2 == -1)
			ERR("no such collection");
		if (rc2 == -2)
			ERR("a resize of that collection is already running");
		if (rc2 == -3)
			ERR("buckets_log2 is 4..24, and not below what "
				"the collection already holds - the splitter would "
				"grow it straight back");
		if (rc2 == -4) {
			const char *why = index_ceiling_msg((int)bl);

			ERR(why ? why : "the new table could not be "
				"created - the arena has no room for its index");
		}
		if (rc2 == -5)
			ERR("the collection is already that size");
		/* this node's own resize is the newest it has seen */
		pc_store_resize_gen_note(nm, (size_t)nl, pc_cluster_col_announce(
			nm, (size_t)nl, (int)bl, CLCOL_OP_RESIZE));
		LM_NOTICE("collection '%s': resize to 2^%d buckets started by a "
			"client\n", nm, (int)bl);
		pc_store_set_floor(nm, (size_t)nl, (int)bl);
		pc_tw_map(out);
		pc_tw_key(out, "resizing");
		pc_tw_bool(out, 1);
		pc_tw_key(out, "col");
		pc_tw_bulk(out, nm, (size_t)nl);
		pc_tw_key(out, "buckets");
		pc_tw_i64(out, 1LL << bl);
		pc_tw_end(out);
		return 0;
	}

	/* ---- enable / disable: native door only (@privileged NULL elsewhere) - the RESP door
	 * may run plaintext off-box, and this secret grants keyspace deletion */
	if (m_is(method, mlen, "enable")) {
		char sec[256], who[160];
		int sl;

		if (!privileged)
			ERR("enable is available on the native door only");
		/* the privilege secret never crosses the network in the clear */
		if (pc_conn_plain_auth(conn))
			ERR("enable needs an encrypted connection - this one is "
				"authenticated but plaintext (libperfd: perfd_opts.encrypt "
				"= 1; perfcli: --encrypt, which -E and -e imply)");
		if (!pc_enable_configured()) {
			enable_fail++;
			ERR(PC_NOENABLE_MSG);
		}
		sl = pc_tv_get_str(v, params, "secret", sec, sizeof sec - 1);
		if (sl <= 0) {
			enable_fail++;
			ERR("missing secret");
		}
		sec[sl] = 0;
		if (en_locked(conn))
			ERR("too many wrong enable secrets from this address - "
				"try again later");
		if (!pc_enable_secret_ok(sec, (size_t)sl)) {
			enable_fail++;
			/* counted and said, the way resp_authfail is: a
			 * privilege secret being guessed should be visible */
			LM_NOTICE("%s offered a wrong enable secret "
				"(%llu failed so far)\n", pc_conn_who(conn, who, sizeof who),
				(unsigned long long)enable_fail);
			en_wrong(conn);
			ERR("wrong enable secret");
		}
		en_right(conn);
		*privileged = 1;
		LM_NOTICE("%s raised privilege with enable\n",
			pc_conn_who(conn, who, sizeof who));
		pc_tw_map(out);
		pc_tw_key(out, "privileged");
		pc_tw_bool(out, 1);
		pc_tw_end(out);
		return 0;
	}
	/* name this connection - CLIENT SETNAME's native form, shown in `clients` and
	 * carried by the enable, sweep and reset log lines */
	if (m_is(method, mlen, "name")) {
		char nm[PC_OBS_NAME + 32];       /* room to see a too-long one */
		int nl, i;

		nl = pc_tv_get_str(v, params, "name", nm, sizeof nm - 1);
		if (nl <= 0)
			ERR("missing name");
		if (nl >= PC_OBS_NAME)
			ERR("name: at most 47 characters");
		for (i = 0; i < nl; i++) {
			unsigned char ch = (unsigned char)nm[i];

			if (ch <= ' ' || ch >= 0x7f || ch == '"' || ch == '\\')
				ERR("name: printable ASCII without spaces or quotes, under "
					"the row's length");
		}
		nm[nl] = 0;
		pc_conn_set_name(conn, nm, (size_t)nl);
		pc_tw_map(out);
		pc_tw_key(out, "named");
		pc_tw_str(out, nm);
		pc_tw_end(out);
		return 0;
	}

	if (m_is(method, mlen, "disable")) {
		if (!privileged)
			ERR("disable is available on the native door only");
		*privileged = 0;
		pc_tw_map(out);
		pc_tw_key(out, "privileged");
		pc_tw_bool(out, 0);
		pc_tw_end(out);
		return 0;
	}

	if (m_is(method, mlen, "reset_stats")) {
		char who[160];
		/* this node's running totals, live gauges untouched; {fleet: true} resets
		 * every live peer too.  CONFIG RESETSTAT on the RESP door is the same call */
		int fleet = pc_tv_get_bool(v, params, "fleet") > 0;

		LM_NOTICE("stats reset%s by %s\n", fleet ? " (the fleet)" : "",
			pc_conn_who(conn, who, sizeof who));
		pc_tw_map(out);
		pc_tw_key(out, "reset");
		pc_tw_bool(out, 1);
		pc_tw_key(out, "at");
		pc_tw_i64(out, fleet ? pc_stats_reset_fleet() : pc_stats_reset());
		pc_tw_end(out);
		return 0;
	}

	/* ---- probe {secs?}: re-measure the WAL storage, blocking this worker for the probe;
	 * its I/O shares the device with the live pump, so fsync latency is perturbed */
	if (m_is(method, mlen, "probe")) {
		static pthread_mutex_t probe_mu = PTHREAD_MUTEX_INITIALIZER;
		struct pc_wal_policy pol;
		struct pc_wal_probe got;
		const struct pc_wal_probe *pr = &got;
		long long secs = 0;
		int prc;

		pc_tv_get_int(v, params, "secs", &secs);
		if (secs < 0)
			secs = 0;
		if (secs > 30)
			ERR("secs is capped at 30");
		/* one probe at a time; its result is copied before another can start */
		if (pthread_mutex_trylock(&probe_mu) != 0)
			ERR("a probe is already running - retry when it ends");
		prc = pc_wal_reprobe((int)secs, &pol);
		if (prc == 0 && probe_gap_ms())
			usleep((useconds_t)probe_gap_ms() * 1000);
		if (prc == 0 && pc_wal_probe_result())
			got = *pc_wal_probe_result();
		else
			prc = -1;
		pthread_mutex_unlock(&probe_mu);
		if (prc != 0)
			ERR("wal is not configured or the probe failed");
		pc_tw_map(out);
		pc_tw_key(out, "cached");
		pc_tw_bool(out, 0);
		pc_tw_key(out, "sync_bs");
		pc_tw_i64(out, PC_WPROBE_SYNC_BS);
		pc_tw_key(out, "qd");
		pc_tw_i64(out, PC_WPROBE_QD);
		pc_tw_key(out, "seq_bs");
		pc_tw_i64(out, PC_WPROBE_SEQ_BS);
		pc_tw_key(out, "seq_mb_s");
		pc_tw_i64(out, (long long)(pr->seq_mb_s + 0.5));
		pc_tw_key(out, "fsync_p50_us");
		pc_tw_i64(out, pr->fsync_p50_us);
		pc_tw_key(out, "fsync_p99_us");
		pc_tw_i64(out, pr->fsync_p99_us);
		pc_tw_key(out, "sync_iops");
		pc_tw_i64(out, pr->sync_iops);
		pc_tw_key(out, "probed_secs");
		pc_tw_i64(out, pr->probed_secs);
		pc_tw_key(out, "recommend");
		pc_tw_map(out);
		pc_tw_key(out, "fsync");
		pc_tw_str(out, pol.fsync_recommend);
		pc_tw_key(out, "max_durable_wps");
		pc_tw_i64(out, pol.max_durable_wps);
		/* a burst ceiling over the probe's volume, not a sustained rate */
		pc_tw_key(out, "max_durable_wps_is_upper_bound");
		pc_tw_bool(out, 1);
		{
			/* and what the fsyncs are ACTUALLY costing - the only
			 * figure taken under real load rather than at start */
			struct pc_wal_fsync_obs ob;

			pc_wal_fsync_observed(&ob);
			pc_tw_key(out, "observed");
			pc_tw_map(out);
			pc_tw_key(out, "fsync_n");
			pc_tw_i64(out, (long long)ob.fsync_n);
			pc_tw_key(out, "fsync_avg_us");
			pc_tw_i64(out, (long long)ob.avg_us);
			pc_tw_key(out, "fsync_max_us");
			pc_tw_i64(out, (long long)ob.max_us);
			pc_tw_key(out, "probe_underestimated");
			pc_tw_bool(out, (ob.probe_underestimated));
			pc_tw_end(out);
		}
		pc_tw_key(out, "probe_sync_kb");
		pc_tw_i64(out, pr->sync_bytes >> 10);
		pc_tw_key(out, "segment_mb");
		pc_tw_i64(out, pol.segment_mb);
		{
			/* size: the worst snapshot gap at the lifetime-average write rate, x3;
			 * every input is reported beside the number */
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
				pc_tw_key(out, "wal_total_mb");
				pc_tw_i64(out, segs * pol.segment_mb);
				pc_tw_key(out, "segments");
				pc_tw_i64(out, segs);
				pc_tw_key(out, "basis");
				pc_tw_map(out);
				pc_tw_key(out, "observed_wps");
				pc_tw_i64(out, wps);
				pc_tw_key(out, "avg_record_b");
				pc_tw_i64(out, avg);
				pc_tw_key(out, "checkpoint_s");
				pc_tw_i64(out, ckpt);
				pc_tw_key(out, "safety");
				pc_tw_i64(out, 3);
				pc_tw_key(out, "uptime_s");
				pc_tw_i64(out, up);
				pc_tw_end(out);
			} else {
				pc_tw_key(out, "wal_total_mb");
				pc_tw_nil(out);
				pc_tw_key(out, "basis");
				pc_tw_str(out, "no observed traffic or no snapshot rules yet");
			}
		}
		pc_tw_end(out);
		pc_tw_end(out);
		return 0;
	}

	/* ---- members: addresses, client ports, load and the routing contract; a client that
	 * does not know the algorithm version spreads plainly - ownership is re-checked here */
	if (m_is(method, mlen, "members")) {
		pc_members_tree(out);
		return 0;
	}

	/* ---- clients {cap?}: what /clients renders, for a dashboard on the native door */
	if (m_is(method, mlen, "clients")) {
		long long cap = 0;

		pc_tv_get_int(v, params, "cap", &cap);
		pc_obs_clients_tree(out, get_ticks(),
			cap <= 0 ? 200 : cap > 4096 ? 4096 : (int)cap);
		return 0;
	}

	/* ---- collections -> [{name, mode, entries, buckets}] */
	if (m_is(method, mlen, "collections")) {
		int i;

		pc_tw_map(out);
		pc_tw_key(out, "collections");
		pc_tw_arr(out);
		for (i = 0; i < pc_store_count(); i++) {
			if (!pc_store_live(i))
				continue;
			pcache_htable_t *cht = pc_store_ht(i);
			pcache_ht_totals_t tot;

			pcache_ht_totals(cht, &tot);
			pc_tw_map(out);
			pc_tw_key(out, "name");
			pc_tw_bulk(out, pc_store_name(i), strlen(pc_store_name(i)));
			pc_tw_key(out, "mode");
			pc_tw_str(out, pc_cluster_replicas() ? "spread"
				: pc_store_shard_enabled(cht) ? "shard"
				: pc_store_proxy_enabled(cht) ? "proxy"
				: pc_store_eager_enabled(cht) ? "eager" : "store");
			pc_tw_key(out, "entries");
			pc_tw_i64(out, (long long)tot.entries);
			pc_tw_key(out, "buckets");
			pc_tw_i64(out, pcache_ht_nbuckets(cht));
			pc_tw_end(out);
		}
		pc_tw_end(out);
		pc_tw_end(out);
		return 0;
	}
	if (m_is(method, mlen, "stats")) {
		char colbuf[128];
		const char *only = NULL;
		int cl;

		/* pc_tv_get_str does not terminate, and pc_stats_tree takes a C string */
		cl = pc_tv_get_str(v, params, "col", colbuf, sizeof colbuf - 1);
		if (cl >= 0) {
			colbuf[cl] = '\0';
			only = colbuf;
		}
		pc_stats_tree(out, only);
		return 0;
	}

	/* everything below needs a collection - but an UNKNOWN method must
	 * report method-not-found, not a missing collection */
	{
		static const char *cv[] = { "get", "set", "setnx", "setxx",
			"del", "exists",
			"expire", "ttl", "add", "sub", "mget", "mset", "keys",
			"scan", "jget", "jset", "jdel", "jincr",
			"jarrappend", "dump", "restore", "rlhit",
			"hcmd",
			"setifeq", "setifne", "delifeq", "delifne" };
		size_t i;
		int known = 0;

		for (i = 0; i < sizeof cv / sizeof cv[0]; i++)
			if (m_is(method, mlen, cv[i])) {
				known = 1;
				break;
			}
		if (!known)
			ERR(pc_unknown_method);    /* proto counts it by this pointer */
	}
	/* every verb below this line touches collection data */
	if (serving_denied())
		ERR(PC_NOTREADY_MSG);
	/* ---- dump {col, cursor?, count?, end?, slot_lo?, slot_hi?, table?}: records for
	 * perfdump.  `count` is BUCKETS: a bucket-bounded key scan never repeats a record */
	if (m_is(method, mlen, "dump")) {
		struct dump_keys dk;
		pcache_htable_t *old = NULL;
		long long cur = 0, count = 1024, slo = 0, shi = PC_SLOTS - 1, endb = -1;
		unsigned int cursor;
		size_t off;
		struct pc_tw_mark mark;
		int n = 0, big = 0;
		unsigned long long tpub = 0;
		long long want = -1;
		int tcol = pc_tv_get(v, params, "col");

		if (tcol < 0 || v->n[tcol].type != 'b')
			ERR("missing col");
		/* the table AND its generation, as one pair (see store.c) */
		ht = pc_store_find_pub((const char *)v->n[tcol].p,
			v->n[tcol].len, &tpub);
		if (!ht)
			ERR("no such collection");
		/* a cursor is a BUCKET index: after a resize it walks another shape (duplicates,
		 * silent misses), so a caller handing `table` back is refused once republished */
		if (!pc_tv_get_int(v, params, "table", &want) &&
		        (want < 0 || (unsigned long long)want != tpub))
			ERR("table resized during the dump: restart this "
				"collection from cursor 0");
		pc_tv_get_int(v, params, "cursor", &cur);
		if (cur < 0 || cur > 0xFFFFFFFFLL)
			ERR("bad cursor");
		pc_tv_get_int(v, params, "count", &count);
		if (count < 1 || count > 16384)
			ERR("count out of range");
		/* `end`: the first bucket this walker does NOT own - several connections on one
		 * node each walk their own buckets (a slot range scans every bucket) */
		pc_tv_get_int(v, params, "end", &endb);
		if (endb < -1 || endb > 0xFFFFFFFFLL)
			ERR("bad end");
		pc_tv_get_int(v, params, "slot_lo", &slo);
		pc_tv_get_int(v, params, "slot_hi", &shi);
		if (slo < 0 || shi >= PC_SLOTS || slo > shi)
			ERR("bad slot range");
		memset(&dk, 0, sizeof dk);
		dk.slo = (unsigned int)slo;
		dk.shi = (unsigned int)shi;
		dk.now = get_ticks();
		dk.cap = PC_MAX_REQ;
		dk.buf = malloc(dk.cap);
		if (!dk.buf)
			ERR("out of memory");
		/* one cursor space: the overflow leg (PCACHE_CURSOR_OVF) comes FIRST, at cursor 0,
		 * so the walker whose range starts at bucket 0 carries it */
		{
			unsigned int nb = pcache_ht_nbuckets(ht);
			unsigned int take = (unsigned int)count;
			int bounded = endb >= 0 && (unsigned long long)endb < (unsigned long long)nb;

			cursor = (unsigned int)cur;
			if (nb == 0) {
				free(dk.buf);
				ERR("empty table");
			}
			if (bounded && !(cursor & PCACHE_CURSOR_OVF) &&
			        cursor >= (unsigned int)endb) {
				free(dk.buf);
				ERR("cursor at or past end");
			}
			if (bounded && !(cursor & PCACHE_CURSOR_OVF) &&
			        take > (unsigned int)endb - cursor)
				take = (unsigned int)endb - cursor;
			/* the walker at bucket 0 first takes what only a draining resize's old table
			 * holds; a key the live table has by then is left to the live walk */
			old = cur == 0 ? pc_store_draining(ht) : NULL;
			if (old) {
				unsigned int oc = 0;
				size_t ro, wo = 0;

				do {
					rc = pcache_ht_scan_ex(old, &oc, 4096,
						PCACHE_SCAN_NOVAL, dump_key_cb, &dk);
				} while (rc == 0 && oc && !dk.full);
				if (rc < 0 || dk.full) {
					free(dk.buf);
					ERR("a draining resize holds too much for one "
						"chunk: retry the collection in a moment");
				}
				for (ro = 0; ro < dk.len; ) {
					unsigned long long hv;
					uint32_t kl;
					str rk;

					memcpy(&kl, dk.buf + ro, 4);
					rk.s = dk.buf + ro + 4;
					rk.len = (int)kl;
					if (pcache_ht_getver_here(ht, &rk, &hv) != 0) {
						memmove(dk.buf + wo, dk.buf + ro, 4 + kl);
						wo += 4 + kl;
					}
					ro += 4 + kl;
				}
				dk.len = dk.nold = wo;
			}
			rc = pcache_ht_scan_ex(ht, &cursor, take, PCACHE_SCAN_NOVAL,
				dump_key_cb, &dk);
			if (rc < 0 || dk.full) {
				free(dk.buf);
				if (old)
					ERR("a draining resize holds too much for one "
						"chunk: retry the collection in a moment");
				ERR("count too large for this table: halve it");
			}
			if (bounded && (cursor == 0 ||
			        (!(cursor & PCACHE_CURSOR_OVF) &&
			         cursor >= (unsigned int)endb)))
				cursor = 0;            /* a bounded range: done at its end */
		}
		/* the reply may grow: a chunk is bounded by buckets, not bytes,
		 * and the dispatcher frees a heap buffer a verb swapped in */
		if (out->n == 0 && !out->owned)
			(void)pc_tw_to_heap(out, JW_REPLY_INIT);
		mark = pc_tw_mark(out);
		pc_tw_map(out);
		pc_tw_key(out, "records");
		pc_tw_arr(out);
		for (off = 0; off < dk.len && !out->over && !big; ) {
			str rk, rv;
			unsigned int rexp = 0;
			unsigned long long ver = 0;
			unsigned char fl = 0;
			uint32_t kl;
			int frc;

			memcpy(&kl, dk.buf + off, 4);
			rk.s = dk.buf + off + 4;
			rk.len = (int)kl;
			off += 4 + kl;
			/* through the live table: it reads the drained one's copy too, the newer */
			frc = pcache_ht_fetch_full(ht, &rk, &rv, &rexp, &fl, &ver);
			/* the closing drain unhooks the old table before it moves its records */
			if (frc == -2 && off - 4 - kl < dk.nold)
				frc = pcache_ht_fetch_full(old, &rk, &rv, &rexp, &fl, &ver);
			if (frc == -2)
				continue;              /* gone since the scan */
			if (frc != 0) {
				/* a record that could not be read is never left out silently */
				free(dk.buf);
				pc_tw_rollback(out, mark);
				ERR("out of memory reading a record: retry the chunk");
			}
			if (rexp && rexp <= dk.now) {
				free(rv.s);
				continue;
			}
			n++;
			pc_tw_map(out);
			pc_tw_key(out, "k");
			pc_tw_bulk(out, rk.s, (size_t)rk.len);
			pc_tw_key(out, "v");
			pc_tw_bulk(out, rv.s, (size_t)rv.len);
			pc_tw_key(out, "ttl");
			pc_tw_i64(out, rel_ttl(rexp, dk.now));
			pc_tw_key(out, "ver");
			pc_tw_i64(out, (long long)ver);
			if (fl & PCACHE_F_JSON) {
				pc_tw_key(out, "t");
				pc_tw_str(out, "json");
			} else if (fl & PCACHE_F_RL) {
				pc_tw_key(out, "t");
				pc_tw_str(out, "rl");
			} else if (fl & PCACHE_F_HASH) {
				pc_tw_key(out, "t");
				pc_tw_str(out, "hash");
			} else if (fl & PCACHE_F_LOCK) {
				pc_tw_key(out, "t");
				pc_tw_str(out, "lock");
			}
			pc_tw_end(out);
			free(rv.s);
			/* the door queues at most PC_MAX_OUTQ, header and trailer included */
			big = out->n > PC_MAX_OUTQ - 65536;
		}
		free(dk.buf);
		if (out->over || big) {
			/* roll the partial reply back to its last whole part -
			 * counts included - before the error frame goes */
			pc_tw_rollback(out, mark);
			ERR("chunk exceeds the reply: halve count");
		}
		pc_tw_end(out);
		pc_tw_key(out, "cursor");
		pc_tw_i64(out, cursor);
		pc_tw_key(out, "more");
		pc_tw_bool(out, (cursor));
		pc_tw_key(out, "n");
		pc_tw_i64(out, n);
		/* the table this chunk walked: hand it back as `table` */
		pc_tw_key(out, "table");
		pc_tw_i64(out, (long long)tpub);
		pc_tw_key(out, "buckets");
		pc_tw_i64(out, (long long)pcache_ht_nbuckets(ht));
		pc_tw_end(out);
		return 0;
	}
	/* ---- restore {col, policy?, records: [{k, v, exp?, ver, t?}]}: perfload's batch - each
	 * record keeps ITS version and expiry and goes through the write path */
	if (m_is(method, mlen, "restore")) {
		const char *em = "missing col";
		/* privileged by blast radius (policy:overwrite rolls a fleet back), but not
		 * gated on allow_create: that is about originating DDL */
		if (!privileged || !*privileged)
			ERR(pc_enable_configured() ? PC_NOPRIV_MSG
				: PC_NOENABLE_MSG);
		int trec, tpol, policy = 0;      /* 0 newer, 1 skip, 2 overwrite */
		unsigned int i, nrec;
		int r;
		long long stored = 0, older = 0, existing = 0, expired = 0, refused = 0, bad = 0;
		unsigned long long now_ms;
		unsigned int now_t = get_ticks();
		struct timespec tw;

		ht = get_col(v, params, &em);
		if (!ht)
			ERR(em);
		if (writes_denied())
			ERR(PC_NOTREADY_MSG);
		/* a restore writes every record it carries */
		if (op_refused(ht))
			ERR(wr_refused_msg());
		tpol = pc_tv_get(v, params, "policy");
		if (tpol >= 0) {
			if (pc_tv_streq(v, tpol, "newer"))
				policy = 0;
			else if (pc_tv_streq(v, tpol, "skip"))
				policy = 1;
			else if (pc_tv_streq(v, tpol, "overwrite"))
				policy = 2;
			else
				ERR("policy: newer, skip or overwrite");
		}
		trec = pc_tv_get(v, params, "records");
		if (trec < 0 || v->n[trec].type != 'a')
			ERR("missing records");
		nrec = v->n[trec].len;
		clock_gettime(CLOCK_REALTIME, &tw);
		now_ms = (unsigned long long)tw.tv_sec * 1000ULL + (unsigned long long)tw.tv_nsec / 1000000ULL;
		/* the reply opens with the misrouted list: [index, node] pairs
		 * the loader re-sends to the node named; the counts follow */
		pc_tw_map(out);
		pc_tw_key(out, "misrouted");
		pc_tw_arr(out);
		for (i = 0, r = trec + 1; i < nrec; i++, r = v->n[r].next) {
			int tt, rc2;
			const char *kp, *vp;
			size_t kl, vl;
			unsigned char rtype;
			str rk, rv;
			long long expms = 0, ver = 0;
			unsigned int exp;
			unsigned long long ver_used, held_ver = 0;

			if (r < 0 || v->n[r].type != 'm') {
				bad++;
				continue;
			}
			if (get_bytes(v, r, "k", &kp, &kl, KEY_MAX) != 0 ||
			        !kl ||
			        get_bytes(v, r, "v", &vp, &vl, PCACHE_CELL_MAX) != 0 ||
			        pc_tv_get_int(v, r, "ver", &ver) != 0) {
				bad++;
				continue;
			}
			pc_tv_get_int(v, r, "exp", &expms);
			tt = pc_tv_get(v, r, "t");
			rtype = tt < 0 ? 0 :
				pc_tv_streq(v, tt, "json") ? PCACHE_F_JSON :
				pc_tv_streq(v, tt, "rl") ? PCACHE_F_RL :
				pc_tv_streq(v, tt, "hash") ? PCACHE_F_HASH :
				pc_tv_streq(v, tt, "lock") ? PCACHE_F_LOCK : 0;
			/* a hash record is stored as the hash commands will read it: a corrupt or
			 * over-ceiling one is refused here, not discovered by the first HGET */
			if (rtype == PCACHE_F_HASH &&
			        !pc_hr_valid((const unsigned char *)vp, vl, NULL)) {
				bad++;
				continue;
			}
			if (expms > 0 && (unsigned long long)expms <= now_ms) {
				expired++;
				continue;
			}
			/* past what the tick expiry holds: refused, as a ttl past PC_TTL_MAX is */
			if (expms > 0 && ((unsigned long long)expms - now_ms + 999ULL) / 1000ULL >
			        (unsigned long long)PC_TTL_MAX) {
				bad++;
				continue;
			}
			exp = expms > 0 ? now_t + (unsigned int)(((unsigned long long)expms - now_ms + 999ULL) / 1000ULL) : 0;
			rk.s = (char *)kp; rk.len = (int)kl;
			rv.s = (char *)vp; rv.len = (int)vl;
			/* a key another node owns (shard) or is known to hold
			 * (proxy): refused and NAMED, never forwarded */
			if (pc_cluster_enabled()) {
				const char *cn = col_name_of(ht);
				int node = 0;

				if (pc_store_shard_enabled(ht))
					node = pc_shard_owner(cn, strlen(cn), rk.s, (size_t)rk.len);
				else if (pc_store_proxy_enabled(ht) &&
				         pcache_ht_getver(ht, &rk, &held_ver) != 0)
					node = pc_loc_get(cn, strlen(cn), rk.s, (size_t)rk.len);
				if (node) {
					pc_tw_arr(out);
					pc_tw_i64(out, (long long)i);
					pc_tw_i64(out, node);
					pc_tw_end(out);
					refused++;
					continue;
				}
			}
			/* getver, not probe: the probe misreports a record in the
			 * overflow leg as absent */
			if (policy == 1 && pcache_ht_getver(ht, &rk, &held_ver) == 0) {
				existing++;
				continue;
			}
			if (policy == 2) {
				rc2 = pcache_ht_store_ex(ht, &rk, &rv, exp, rtype);
				ver_used = pcache_last_ver;
			} else {
				rc2 = pcache_ht_store_ver(ht, &rk, &rv, exp, rtype, (unsigned long long)ver);
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

				pc_wal_upsert_fl(cn, rk.s, rk.len, rv.s, rv.len, exp, ver_used,
					rtype);
				eager_push(ht, &rk, rv.s, (size_t)rv.len, exp, ver_used, rtype);
				if (pc_cluster_enabled())
					pc_neg_clear(cn, strlen(cn), rk.s, (size_t)rk.len);
				pc_store_note_set(ht, (size_t)rk.len, (size_t)rv.len);
			}
			stored++;
		}
		pc_tw_end(out);
		pc_tw_key(out, "stored");
		pc_tw_i64(out, stored);
		pc_tw_key(out, "older");
		pc_tw_i64(out, older);
		pc_tw_key(out, "existing");
		pc_tw_i64(out, existing);
		pc_tw_key(out, "expired");
		pc_tw_i64(out, expired);
		pc_tw_key(out, "refused");
		pc_tw_i64(out, refused);
		pc_tw_key(out, "bad");
		pc_tw_i64(out, bad);
		pc_tw_end(out);
		return 0;
	}
	/* ---- keys {col: <glob>|absent, match?, limit?}: grouped by collection, `limit` PER
	 * collection; each that hit it is named in truncated_collections */
	if (m_is(method, mlen, "keys")) {
		int tc = pc_tv_get(v, params, "col");
		const char *cg = "*";
		size_t cgl = 1;

		if (tc >= 0 && v->n[tc].type == 'b') {
			cg = (const char *)v->n[tc].p;
			cgl = v->n[tc].len;
		}
		if (tc < 0 || memchr(cg, '*', cgl) || memchr(cg, '?', cgl) ||
		    memchr(cg, '[', cgl)) {
			struct scan_ctx sc;
			char pat[256];
			long long limit = 100;
			int trunc[PC_MAX_COLLECTIONS];
			int i, matched = 0, ntrunc = 0;

			memset(&sc, 0, sizeof sc);
			sc.w = out;
			sc.patlen = -1;
			if (pc_tv_get(v, params, "match") >= 0) {
				sc.patlen = pc_tv_get_str(v, params, "match", pat,
					sizeof pat);
				if (sc.patlen < 0)
					ERR("bad match pattern");
			}
			sc.pat = sc.patlen >= 0 ? pat : NULL;
			sc.now = get_ticks();
			pc_tv_get_int(v, params, "limit", &limit);
			if (limit < 1 || limit > 100000)
				ERR("limit out of range");
			pc_tw_map(out);
			pc_tw_key(out, "collections");
			pc_tw_map(out);
			for (i = 0; i < pc_store_count() && !out->over; i++) {
				if (!pc_store_live(i))
					continue;
				const char *nm = pc_store_name(i);
				unsigned int cursor = 0;
				int rc2;

				if (!pc_glob(cg, (int)cgl, nm, (int)strlen(nm)))
					continue;
				matched++;
				pc_tw_keyn(out, nm, strlen(nm));
				pc_tw_arr(out);
				sc.emitted = 0;
				sc.stopped = 0;
				sc.limit = (int)limit;
				do {
					rc2 = pc_store_scan(pc_store_ht(i), &cursor,
						1024, PCACHE_SCAN_NOVAL, keys_cb, &sc);
				} while (rc2 == 0 && cursor && !out->over);
				pc_tw_end(out);
				if (sc.stopped && ntrunc < PC_MAX_COLLECTIONS)
					trunc[ntrunc++] = i;
			}
			pc_tw_end(out);
			pc_tw_key(out, "truncated");
			pc_tw_bool(out, ((ntrunc || out->over)));
			pc_tw_key(out, "truncated_collections");
			pc_tw_arr(out);
			for (i = 0; i < ntrunc; i++) {
				const char *tn = pc_store_name(trunc[i]);

				pc_tw_bulk(out, tn, strlen(tn));
			}
			pc_tw_end(out);
			pc_tw_key(out, "matched");
			pc_tw_i64(out, matched);
			pc_tw_end(out);
			return 0;
		}
	}
	ht = get_col(v, params, errmsg);
	if (!ht)
		return -1;
	ttl = 0;
	if (pc_tv_get_int(v, params, "ttl", &ttl) == 0 && ttl > PC_TTL_MAX)
		ERR(PC_TTL_MSG);
	/* an upstream-backed collection: the gate may park the frame, refuse it, or answer
	 * with Redis's reply */
regate:
	if (ht->shadow && conn) {
		struct up_ans ans;
		int g;

		memset(&ans, 0, sizeof ans);
		pc_tw_init(&ans.tree, NULL, 0);
		g = up_cmd_gate(ht, method, mlen, v, params, errmsg, &ans);
		if (g == UPG_TREE) {
			/* hcmd written through - the tree goes as the reply */
			if (pc_tw_done(&ans.tree) != 0 || !ans.tree.n) {
				pc_tw_free(&ans.tree);
				ERR("internal reply error");
			}
			pc_tw_raw(out, ans.tree.b, ans.tree.n);
			pc_tw_free(&ans.tree);
			return 0;
		}
		pc_tw_free(&ans.tree);
		if (g == PC_UPG_HELD)
			return 0;
		if (g == PC_UPG_ANSWERED)
			return -1;
		if (g == UPG_REPLY && (m_is(method, mlen, "add") ||
		    m_is(method, mlen, "sub"))) {
			pc_tw_map(out);                /* Redis's new value */
			pc_tw_key(out, "value");
			pc_tw_i64(out, ans.ival);
			pc_tw_end(out);
			return 0;
		}
		if (g == UPG_REPLY && m_is(method, mlen, "mset")) {
			pc_tw_map(out);                /* one MSET, applied */
			pc_tw_key(out, "stored");
			pc_tw_i64(out, ans.ival > 0 ? ans.stored : 0);
			pc_tw_key(out, "dropped");
			pc_tw_i64(out, ans.dropped);
			pc_tw_end(out);
			return 0;
		}
		if (g == UPG_REPLY) {
			/* written through - the verb's reply from Redis's */
			const char *rk = m_is(method, mlen, "del") ||
				m_is(method, mlen, "jdel") ? "deleted" :
				m_is(method, mlen, "expire") ? "updated" :
				m_is(method, mlen, "jset") ? "set" : "stored";

			pc_tw_map(out);
			pc_tw_key(out, rk);
			pc_tw_bool(out, ans.ival > 0);
			pc_tw_end(out);
			return 0;
		}
	}

	if (m_is(method, mlen, "get")) {
		NEEDKEY();
		{
			char *gb = get_buf();
			unsigned int gl = 0;

			if (!gb)
				ERR("get failed");
			if (ht->shadow && conn)
				pc_upstream_test_pause();
			rc = op_get_buf(ht, &k, gb, VAL_MAX, &gl, &exp,
				park_req);
			if (rc == PC_OP_OK) {
				write_hit(out, gb, (size_t)gl, exp);
				*outcome = PC_OUT_HIT;
				return 0;
			}
		}
		if (rc == PC_OP_ERR_TYPE)
			ERR(pc_wrongtype_msg);
		if (rc != PC_OP_ERR_GET && ht->shadow && conn &&
		    !(park_req && *park_req) && up_get_lost(ht, &k, &lost_tries))
			goto regate;
		if (rc == PC_OP_ERR_GET) {
			ERR("get failed");
		} else {
			/* a miss, and the fallback if the proto layer cannot park: it must be
			 * THIS verb's shape */
			pc_tw_map(out);
			pc_tw_key(out, "found");
			pc_tw_bool(out, 0);
			pc_tw_end(out);
			*outcome = PC_OUT_MISS;
		}
		return 0;
	}

	/* ---- exists {col,key} : {exists, ver} ; ttl {col,key} : -2 absent,
	 * -1 no expiry, else seconds - a key elsewhere asked of its node */
	if (m_is(method, mlen, "exists") || m_is(method, mlen, "ttl")) {
		NEEDKEY();
		pc_pres_cmd(ht, &k, method[0] == 'e' ? PC_PRES_EXISTS : PC_PRES_TTL,
			out, park_req);
		return 0;
	}

	if (m_is(method, mlen, "del")) {
		NEEDKEY();
		rc = op_del(ht, &k, park_req);
		if (op_fail_text(rc))
			ERR(op_fail_text(rc));
		pc_tw_map(out);
		pc_tw_key(out, "deleted");
		pc_tw_bool(out, rc == PC_OP_OK);   /* parked falls back to false */
		pc_tw_end(out);
		return 0;
	}

	/* ---- expire {col,key,ttl} : re-arm without rewriting ----------- */
	if (m_is(method, mlen, "expire")) {
		NEEDKEY();
		if (pc_tv_get_int(v, params, "ttl", &ttl) != 0)
			ERR("missing ttl");
		rc = op_expire(ht, &k, ttl, park_req);
		if (op_fail_text(rc))
			ERR(op_fail_text(rc));
		if (rc == PC_OP_ERR_FULL)
			ERR("cache full");
		pc_tw_map(out);
		pc_tw_key(out, "updated");
		pc_tw_bool(out, rc == PC_OP_OK);   /* parked falls back to false */
		pc_tw_end(out);
		return 0;
	}

	/* ---- add/sub {col,key,by?,ttl?} -> {value:n} ------------------- */
	if (m_is(method, mlen, "add") || m_is(method, mlen, "sub")) {
		long long nv = 0;

		NEEDKEY();
		if (pc_tv_get_int(v, params, "by", &by) != 0)
			by = 1;
		ttl = 0;
		pc_tv_get_int(v, params, "ttl", &ttl);
		if (method[0] == 's' && by == LLONG_MIN)
			ERR("sub: by out of range");
		if (method[0] == 's')
			by = -by;
		rc = op_addsub(ht, &k, by, ttl, &nv, park_req, 0);
		if (rc == PC_OP_PARKED) {
			/* the park fallback is a RESULT, not a failure */
			pc_tw_map(out);
			pc_tw_key(out, "error");
			pc_tw_str(out, "cluster busy");
			pc_tw_end(out);
			return 0;
		}
		if (rc == PC_OP_ERR_BUSY)
			ERR("parked-request table full - retry");
		if (rc == PC_OP_ERR_FWD)
			ERR("forward failed");
		if (rc == PC_OP_ERR_TYPE)
			ERR(pc_wrongtype_msg);
		if (rc == PC_OP_ERR_NOTINT)
			ERR("value is not an integer");
		if (rc == PC_OP_ERR_OVF)
			ERR(PC_OVF_MSG);
		if (rc == PC_OP_ERR_WRFAIL)    /* refused: nv was never written */
			ERR(wr_refused_msg());
		pc_tw_map(out);
		pc_tw_key(out, "value");
		pc_tw_i64(out, nv);
		pc_tw_end(out);
		return 0;
	}

	/* ---- setifeq / setifne {col,key,value,cmp,ttl?}, delifeq / delifne {col,key,cmp}: Redis
	 * 8.4's SET/DELEX IFEQ/IFNE - methods, as an older daemon ignores an unknown param */
	if (m_is(method, mlen, "setifeq") || m_is(method, mlen, "setifne") ||
	        m_is(method, mlen, "delifeq") || m_is(method, mlen, "delifne")) {
		int kind = m_is(method, mlen, "setifeq") ? PC_CMP_SET_IFEQ :
			m_is(method, mlen, "setifne") ? PC_CMP_SET_IFNE :
			m_is(method, mlen, "delifeq") ? PC_CMP_DEL_IFEQ :
			PC_CMP_DEL_IFNE;
		int isset = kind == PC_CMP_SET_IFEQ || kind == PC_CMP_SET_IFNE;
		const char *cp, *vp = NULL;
		size_t cl, vl = 0;

		NEEDKEY();
		if (get_bytes(v, params, "cmp", &cp, &cl, (size_t)-1) != 0)
			ERR("missing cmp");
		ttl = 0;
		if (isset) {
			if (get_bytes(v, params, "value", &vp, &vl, (size_t)-1) != 0)
				ERR("missing or oversized value");
			pc_tv_get_int(v, params, "ttl", &ttl);
		}
		v_.s = (char *)vp;
		v_.len = (int)vl;
		rc = op_cmp(ht, &k, cp, cl, isset ? &v_ : NULL, ttl, kind,
			park_req);
		if (rc == PC_OP_OK || rc == PC_OP_ABSENT || rc == PC_OP_PARKED) {
			pc_tw_map(out);
			pc_tw_key(out, isset ? "stored" : "deleted");
			pc_tw_bool(out, rc == PC_OP_OK);   /* parked: the fallback */
			pc_tw_end(out);
		} else if (rc == PC_OP_ERR_TYPE)
			ERR(isset ? pc_wrongtype_msg : delex_type_msg);
		else if (rc == PC_OP_ERR_MODE)
			ERR(cmp_mode_msg);
		else if (rc == PC_OP_ERR_BUSY)
			ERR("parked-request table full - retry");
		else if (rc == PC_OP_ERR_WRFAIL)
			ERR(wr_refused_msg());
		else if (rc == PC_OP_ERR_FULL)
			ERR("cache full");
		else if (rc == PC_OP_ERR_2BIG)
			ERR("value too large");
		else
			ERR("forward failed");
		return 0;
	}

	/* ---- set / setnx / setxx {col,key,value,ttl?}: methods, not params - an older daemon
	 * would ignore an unknown param and store unconditionally */
	if (m_is(method, mlen, "set") || m_is(method, mlen, "setnx") ||
	        m_is(method, mlen, "setxx")) {
		int cond = m_is(method, mlen, "setnx") ? PC_SETCOND_NX :
			m_is(method, mlen, "setxx") ? PC_SETCOND_XX : 0;
		const char *vp;
		size_t vl;

		NEEDKEY();
		if (get_bytes(v, params, "value", &vp, &vl, (size_t)-1) != 0)
			ERR("missing or oversized value");
		ttl = 0;
		pc_tv_get_int(v, params, "ttl", &ttl);
		v_.s = (char *)vp; v_.len = (int)vl;
		if (cond) {
			rc = op_setcond(ht, &k, &v_, ttl, cond, park_req);
			if (rc == PC_OP_ABSENT) {
				pc_tw_map(out);
				pc_tw_key(out, "stored");
				pc_tw_bool(out, 0);
				pc_tw_end(out);
				return 0;
			}
			if (rc == PC_OP_ERR_MODE)
				ERR(setcond_mode_msg);
		} else
			rc = op_set(ht, &k, &v_, ttl, park_req, 0);
		if (rc == PC_OP_OK || rc == PC_OP_PARKED) {
			pc_tw_map(out);
			pc_tw_key(out, "stored");
			pc_tw_bool(out, rc == PC_OP_OK);   /* parked: the fallback */
			pc_tw_end(out);
		} else if (rc == PC_OP_ERR_BUSY)
			ERR("parked-request table full - retry");
		else if (rc == PC_OP_ERR_FWD)
			ERR("forward failed");
		else if (rc == PC_OP_ERR_WRFAIL)
			ERR(wr_refused_msg());
		else if (rc == PC_OP_ERR_FULL)
			ERR("cache full");
		else
			ERR("value too large");
		return 0;
	}

	/* ---- hcmd {col, key, cmd, args?: [..]}: any H command, e.g. {cmd: "HSET", args:
	 * ["f", "v"]}; the reply is its Redis reply as the hash core's tree */
	if (m_is(method, mlen, "hcmd")) {
		const char *argv[PC_HCMD_MAXARGS];
		size_t argl[PC_HCMD_MAXARGS];
		char cmd[16];
		int argc, ci, ta, lacking = 0, cl;
		struct pc_tw t;

		NEEDKEY();
		cl = pc_tv_get_str(v, params, "cmd", cmd, sizeof cmd - 1);
		if (cl < 0)
			ERR("missing cmd");
		ci = hcmd_find(cmd, (size_t)cl);
		if (ci < 0)
			ERR("cmd is not a hash command");
		argv[0] = hcmds[ci].name;
		argl[0] = strlen(hcmds[ci].name);
		argv[1] = k.s;
		argl[1] = (size_t)k.len;
		argc = 2;
		ta = pc_tv_get(v, params, "args");
		if (ta >= 0) {
			unsigned int ai;

			if (v->n[ta].type != 'a')
				ERR("args must be an array of strings");
			for (ai = 0; ai < v->n[ta].len; ai++) {
				int an = pc_tv_at(v, ta, ai);

				if (an < 0 || v->n[an].type != 'b' || argc >= PC_HCMD_MAXARGS)
					ERR("args must be an array of strings");
				argv[argc] = (const char *)v->n[an].p;
				argl[argc++] = v->n[an].len;
			}
		}
		if (!hcmd_arity_ok(ci, argc))
			ERR("wrong number of args for this cmd");
		pc_tw_init(&t, NULL, 0);
		rc = op_hcmd(ht, ci, argc, argv, argl, &t, park_req, &lacking);
		if (rc == PC_OP_OK) {
			/* the hash core's tree is a ptree item: it goes as the
			 * reply, bytes unchanged */
			if (pc_tw_done(&t) != 0 || !t.n) {
				pc_tw_free(&t);
				ERR("internal reply error");
			}
			pc_tw_raw(out, t.b, t.n);
			pc_tw_free(&t);
			return 0;
		}
		pc_tw_free(&t);
		if (rc == PC_OP_PARKED) {
			pc_tw_map(out);
			pc_tw_key(out, "error");
			pc_tw_str(out, "cluster busy");
			pc_tw_end(out);
			return 0;
		}
		if (rc == PC_OP_ERR_GATE)
			ERR(hc_gate_text(lacking));
		if (rc == PC_OP_ERR_MODE)
			ERR(hc_mode_msg);
		if (rc == PC_OP_ERR_2BIG)
			ERR("hash command too large: its arguments exceed 58000 bytes");
		if (rc == PC_OP_ERR_BUSY)
			ERR("parked-request table full - retry");
		if (rc == PC_OP_ERR_WRFAIL)
			ERR(wr_refused_msg());
		ERR("hash command failed");
	}
	/* rlhit {col,key,window_ms,limit?} -> {count: N, allowed: b} */
	if (m_is(method, mlen, "rlhit")) {
		long long win = 0, lim = 0, cnt = 0;
		int al = 0, lacking = 0;

		NEEDKEY();
		if (pc_tv_get_int(v, params, "window_ms", &win) != 0 ||
		        win < 1 || win > 86400000LL)
			ERR("window_ms: 1..86400000");
		pc_tv_get_int(v, params, "limit", &lim);
		if (lim < 0 || lim >= PC_RL_MAX)
			ERR("limit: 0 (none) .. 5999");
		rc = op_rlhit(ht, &k, win, lim, &cnt, &al, park_req, &lacking);
		if (rc == PC_OP_OK) {
			pc_tw_map(out);
			pc_tw_key(out, "count");
			pc_tw_i64(out, cnt);
			pc_tw_key(out, "allowed");
			pc_tw_bool(out, al);
			pc_tw_end(out);
			return 0;
		}
		if (rc == PC_OP_PARKED) {
			pc_tw_map(out);
			pc_tw_key(out, "error");
			pc_tw_str(out, "cluster busy");
			pc_tw_end(out);
			return 0;
		}
		if (rc == PC_OP_ERR_GATE)
			ERR(rl_gate_text(lacking));
		if (rc == PC_OP_ERR_MODE)
			ERR(rl_mode_msg);
		if (rc == PC_OP_ERR_TYPE)
			ERR(pc_wrongtype_msg);
		if (rc == PC_OP_ERR_BUSY)
			ERR("parked-request table full - retry");
		if (rc == PC_OP_ERR_WRFAIL)
			ERR(wr_refused_msg());
		ERR("rate-limit hit failed");
	}
	/* ---- jget/jset/jdel/jincr/jarrappend: documents are JSON text spliced under the key's
	 * mutex stripe (pc_json_rmw); `val` and jget's `value` are JSON text, strings quoted */
	if (m_is(method, mlen, "jget") || m_is(method, mlen, "jset") ||
	        m_is(method, mlen, "jdel") ||
	        m_is(method, mlen, "jincr") ||
	        m_is(method, mlen, "jarrappend")) {
		char path[JP_NAME_PARAM];
		const char *fmsg = NULL, *sval = NULL;
		char *frag = NULL;
		size_t svlen = 0;
		long long by = 1, ttl2 = 0, nv = 0;
		int plen, rc2, fraglen = 0, cnt = 0, have_ttl, nx = 0, xx = 0;
		int mk = 0, op;

		if (m_is(method, mlen, "jget"))
			op = PC_JOP_GET;
		else if (m_is(method, mlen, "jset"))
			op = PC_JOP_SET;
		else if (m_is(method, mlen, "jdel"))
			op = PC_JOP_DEL;
		else if (m_is(method, mlen, "jincr"))
			op = PC_JOP_INCR;
		else
			op = PC_JOP_APPEND;

		NEEDKEY();
		plen = pc_tv_get_str(v, params, "path", path, sizeof path);
		if (plen < 0) {
			if (pc_tv_get(v, params, "path") >= 0)
				ERR("bad path");
			path[0] = '$';
			plen = 1;              /* default: the root */
		}
		if (op == PC_JOP_SET || op == PC_JOP_APPEND) {
			/* the path layer validates the document's JSON text */
			if (get_bytes(v, params, "val", &sval, &svlen,
			        (size_t)-1) != 0)
				ERR("missing val");
		}
		if (op == PC_JOP_INCR)
			pc_tv_get_int(v, params, "by", &by);
		have_ttl = pc_tv_get_int(v, params, "ttl", &ttl2) == 0;
		nx = pc_tv_get_bool(v, params, "nx");
		xx = pc_tv_get_bool(v, params, "xx");
		mk = pc_tv_get_bool(v, params, "mkpath");

		/* jdel at the ROOT removes the key, as RESP JSON.DEL key does (pc_jp_del cannot
		 * delete the root); del's own call, so it forwards and parks as del does */
		if (op == PC_JOP_DEL && plen == 1 && path[0] == '$') {
			unsigned char fl = 0;

			/* a string is refused (Redis 8's JSON.DEL wrong-type error); only a record
			 * held HERE can be checked - a shard non-owner forwards the delete */
			if (pc_store_types_strict(ht) &&
			        pcache_ht_getflags(ht, &k, &fl) == 0 &&
			        !(fl & PCACHE_F_JSON))
				ERR(pc_json_wrongtype_msg);
			rc = op_del(ht, &k, park_req);
			if (op_fail_text(rc))
				ERR(op_fail_text(rc));
			pc_tw_map(out);
			pc_tw_key(out, "deleted");
			pc_tw_bool(out, rc == PC_OP_OK);
			pc_tw_end(out);
			return 0;
		}

		/* a read-through key's owner, or a spread holder, is
		 * pulled from as a hash read is; the path is taken here */
		if (op == PC_JOP_GET && pc_jget_park(ht, k.s, (size_t)k.len, path,
		        (size_t)plen, 0, park_req)) {
			pc_tw_map(out);                /* the park's fallback */
			pc_tw_key(out, "found");
			pc_tw_bool(out, 0);
			pc_tw_end(out);
			*outcome = PC_OUT_MISS;
			return 0;
		}

		/* a proxy non-holder or shard non-owner forwards the whole op; a shard owner is
		 * deterministic, so it needs no locator and no placement */
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
				/* remember the holder-to-be: later ops on this
				 * key forward sticky instead of re-guessing placement */
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
						pc_tw_map(out);
						pc_tw_key(out, "set");
						pc_tw_bool(out, 0);
						pc_tw_end(out);
						break;
					case PC_JOP_DEL:
						pc_tw_map(out);
						pc_tw_key(out, "deleted");
						pc_tw_bool(out, 0);
						pc_tw_end(out);
						break;
					case PC_JOP_GET:
						pc_tw_map(out);
						pc_tw_key(out, "found");
						pc_tw_bool(out, 0);
						pc_tw_end(out);
						*outcome = PC_OUT_MISS;
						break;
					default:
						pc_tw_map(out);
						pc_tw_key(out, "error");
						pc_tw_str(out, "cluster busy");
						pc_tw_end(out);
					}
					*park_req = req;
					return 0;
				}
			}
			if (op == PC_JOP_GET) {
				pc_tw_map(out);
				pc_tw_key(out, "found");
				pc_tw_bool(out, 0);
				pc_tw_end(out);
				*outcome = PC_OUT_MISS;
				return 0;
			}
			if (op == PC_JOP_DEL) {
				pc_tw_map(out);
				pc_tw_key(out, "deleted");
				pc_tw_bool(out, 0);
				pc_tw_end(out);
				return 0;
			}
			ERR("no holder reachable");
		    }
		}

jlocal:
		rc2 = pc_json_rmw(ht, k.s, k.len, op, path, plen, sval,
			(int)svlen, by, have_ttl, ttl2, nx, xx, mk,
			&frag, &fraglen, &nv, &cnt, &fmsg);
		if (rc2 < 0)
			ERR(fmsg);
		switch (op) {
		case PC_JOP_GET:
			pc_tw_map(out);
			pc_tw_key(out, "found");
			if (rc2 == 1) {
				pc_tw_bool(out, 0);
				*outcome = PC_OUT_MISS;
			} else {
				pc_tw_bool(out, 1);
				pc_tw_key(out, "value");
				pc_tw_bulk(out, frag, (size_t)fraglen);
				*outcome = PC_OUT_HIT;
			}
			pc_tw_end(out);                /* frag is borrowed */
			break;
		case PC_JOP_SET:
			pc_tw_map(out);
			pc_tw_key(out, "set");
			pc_tw_bool(out, 1);
			pc_tw_end(out);
			break;
		case PC_JOP_DEL:
			pc_tw_map(out);
			pc_tw_key(out, "deleted");
			pc_tw_bool(out, rc2 != 1);
			pc_tw_end(out);
			break;
		case PC_JOP_INCR:
			pc_tw_map(out);
			pc_tw_key(out, "value");
			pc_tw_i64(out, nv);
			pc_tw_end(out);
			break;
		default:                       /* APPEND */
			pc_tw_map(out);
			pc_tw_key(out, "count");
			pc_tw_i64(out, cnt);
			pc_tw_end(out);
		}
		return 0;
	}

	/* ---- scan {col,cursor?,match?,count?,values?} / keys {col,match?,limit?} */
	if (m_is(method, mlen, "scan") || m_is(method, mlen, "keys")) {
		struct scan_ctx sc;
		char pat[256];
		long long cur = 0, count = 0, limit = 10000;
		unsigned int cursor;
		int is_keys = m_is(method, mlen, "keys");

		memset(&sc, 0, sizeof sc);
		sc.w = out;
		sc.patlen = -1;
		if (pc_tv_get(v, params, "match") >= 0) {
			sc.patlen = pc_tv_get_str(v, params, "match", pat,
				sizeof pat);
			if (sc.patlen < 0)
				ERR("bad match pattern");
		}
		sc.pat = sc.patlen >= 0 ? pat : NULL;
		sc.now = get_ticks();

		if (is_keys) {
			pc_tv_get_int(v, params, "limit", &limit);
			if (limit < 1 || limit > 100000)
				ERR("limit out of range");
			sc.limit = (int)limit;
			/* name the collection, so a key pastes back into get, and flag ONE node's
			 * share of a placement-spread collection */
			{
				const char *cn = col_name_of(ht);

				pc_tw_map(out);
				pc_tw_key(out, "collection");
				pc_tw_bulk(out, cn, strlen(cn));
				pc_tw_key(out, "complete");
				/* a member that is down still holds its share */
				pc_tw_bool(out, !(pc_cluster_enabled() &&
					pc_cluster_peers_known() > 0 &&
					!pc_store_eager_enabled(ht)));
				pc_tw_key(out, "scope");
				pc_tw_str(out, "node");
				pc_tw_key(out, "keys");
				pc_tw_arr(out);
			}
			cursor = 0;
			do {
				rc = pc_store_scan(ht, &cursor, 1024,
					PCACHE_SCAN_NOVAL, keys_cb, &sc);
			} while (rc == 0 && cursor && !out->over);
			pc_tw_end(out);
			pc_tw_key(out, "truncated");
			pc_tw_bool(out, (sc.stopped));
			pc_tw_end(out);
			return 0;
		}

		pc_tv_get_int(v, params, "cursor", &cur);
		if (cur < 0 || cur > 0xFFFFFFFFLL)
			ERR("bad cursor");
		pc_tv_get_int(v, params, "count", &count);
		if (count < 0 || count > 16384)
			ERR("count out of range");
		sc.values = pc_tv_get_bool(v, params, "values");
		cursor = (unsigned int)cur;
		pc_tw_map(out);
		pc_tw_key(out, "items");
		pc_tw_arr(out);
		/* values requested = copy them out; otherwise keys-only, which
		 * skips the value memcpy AND its cold cachelines */
		pc_store_scan(ht, &cursor, (unsigned int)count,
			sc.values ? 0 : PCACHE_SCAN_NOVAL, scan_cb, &sc);
		pc_tw_end(out);
		pc_tw_key(out, "cursor");
		pc_tw_i64(out, cursor);
		pc_tw_key(out, "more");
		pc_tw_bool(out, (cursor));
		pc_tw_end(out);
		return 0;
	}

	if (m_is(method, mlen, "mget")) {
		int tk = pc_tv_get(v, params, "keys"), kn;
		unsigned int i;

		if (tk < 0 || v->n[tk].type != 'a')
			ERR("missing keys array");
		if (v->n[tk].len > MULTI_MAX)
			ERR("too many keys - mget takes at most 65536, split it");
		if (mget_fans(ht)) {
			/* a key on another node is looked up, never a silent miss; the keys as
			 * the loop below takes them: non-bytes skipped, a long one absent */
			const char **ks;
			size_t *kl;
			struct pc_mget *mg;
			const char *code, *why = "out of memory";
			unsigned int nk = 0;
			int frc = -1;

			ks = malloc((v->n[tk].len + 1) * sizeof *ks);
			kl = malloc((v->n[tk].len + 1) * sizeof *kl);
			if (ks && kl) {
				for (i = 0, kn = tk + 1; i < v->n[tk].len;
				        i++, kn = v->n[kn].next) {
					if (v->n[kn].type != 'b')
						continue;
					ks[nk] = (const char *)v->n[kn].p;
					kl[nk++] = v->n[kn].len;
				}
				frc = op_mget_fan(ht, ks, kl, nk, 0, &mg, &code, &why);
			}
			free(ks);
			free(kl);
			if (frc < 0)
				ERR(why);
			if (pc_mget_waiting(mg)) {
				pc_mget_park(mg);  /* the door attaches it */
				return 0;
			}
			pc_mget_tree(mg, out);
			pc_mget_free(mg);
			return 0;
		}
		pc_tw_map(out);
		pc_tw_key(out, "values");
		pc_tw_arr(out);
		for (i = 0, kn = tk + 1; i < v->n[tk].len && !out->over;
		        i++, kn = v->n[kn].next) {
			/* an element that is not bytes is skipped, not counted */
			if (v->n[kn].type != 'b')
				continue;
			if (v->n[kn].len > KEY_MAX) {
				pc_tw_map(out);
				pc_tw_key(out, "found");
				pc_tw_bool(out, 0);
				pc_tw_end(out);
				continue;
			}
			k.s = (char *)v->n[kn].p; k.len = (int)v->n[kn].len;
			HOT(ht, k.s, k.len);
			{
				unsigned char fl = 0;

				rc = pcache_ht_fetch_ex(ht, &k, &v_, &exp, &fl);
				/* as RESP MGET: a document answers as a
				 * miss under strict, not as an error */
				if (rc == 0 && (fl & PCACHE_F_TYPES) &&
				        pc_store_types_strict(ht)) {
					free(v_.s);
					rc = -2;
				}
			}
			if (rc == 0) {
				write_hit(out, v_.s, (size_t)v_.len, exp);
				free(v_.s);
			} else {
				pc_tw_map(out);
				pc_tw_key(out, "found");
				pc_tw_bool(out, 0);
				pc_tw_end(out);
			}
		}
		pc_tw_end(out);
		pc_tw_end(out);
		return 0;
	}

	if (m_is(method, mlen, "mset")) {
		int ti = pc_tv_get(v, params, "items");
		int stored = 0, dropped = 0, it;
		unsigned int i;

		if (ti < 0 || v->n[ti].type != 'a')
			ERR("missing items array");
		if (v->n[ti].len > MULTI_MAX)
			ERR("too many items - mset takes at most 65536, split it");
		if (pc_store_shard_enabled(ht))
			ERR("mset on a shard collection is not "
				"supported yet - use pipelined set");
		if (op_refused(ht))
			ERR(wr_refused_msg());
		for (i = 0, it = ti + 1; i < v->n[ti].len;
		        i++, it = v->n[it].next) {
			const char *kp, *vp;
			size_t kl, vl;

			if (v->n[it].type != 'm')
				continue;
			if (get_bytes(v, it, "key", &kp, &kl, KEY_MAX) != 0 || !kl ||
			        get_bytes(v, it, "value", &vp, &vl, VAL_MAX) != 0) {
				dropped++;
				continue;
			}
			ttl = 0;
			if (pc_tv_get_int(v, it, "ttl", &ttl) == 0 &&
			        ttl > PC_TTL_MAX) {
				dropped++;
				continue;
			}
			k.s = (char *)kp; k.len = (int)kl;
			v_.s = (char *)vp; v_.len = (int)vl;
			/* the same local write a set makes - WAL, event, eager push */
			if (set_local(ht, &k, &v_, ttl) == PC_OP_OK)
				stored++;
			else
				dropped++;
		}
		pc_tw_map(out);
		pc_tw_key(out, "stored");
		pc_tw_i64(out, stored);
		pc_tw_key(out, "dropped");
		pc_tw_i64(out, dropped);
		pc_tw_end(out);
		return 0;
	}

	/* a name the gate above admitted that no branch took: unreachable,
	 * and said the same way as an unknown method if it ever is */
	ERR(pc_unknown_method);
#undef NEEDKEY
}

/* ---- the binary codec: one frame = one data verb (layouts in proto.h), its bytes used
 * in place.  When parking, a verb pre-writes its own park-failure shape into @out/@flags */

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

/* Redis's reply to a forwarded H command as the tree hcmd_exec builds: an integer, OK,
 * a bulk (HINCRBYFLOAT), nil, or Redis's error as the error item */
static void up_reply_tree(struct pc_tw *t, const struct pc_upg *g)
{
	const char *r = g->raw, *e;
	size_t n = g->rawlen;
	long long l;

	if (g->rerr) {
		pc_tw_err(t, g->rerr);
		return;
	}
	if (!r || n < 3) {
		pc_tw_err(t, "ERR upstream: no reply");
		return;
	}
	switch (r[0]) {
	case ':':
		pc_tw_i64(t, g->ival);
		return;
	case '+':
		if (n == 5 && !memcmp(r + 1, "OK", 2))
			pc_tw_ok(t);
		else
			pc_tw_bulk(t, r + 1, n - 3);
		return;
	case '_':
		pc_tw_nil(t);
		return;
	case '$':
		/* the reply ends in CRLF: the number stops there */
		l = strtoll(r + 1, NULL, 10);
		e = memchr(r, '\n', n);
		if (l < 0) {
			pc_tw_nil(t);
			return;
		}
		if (e && (size_t)(e + 1 - r) + (size_t)l <= n) {
			pc_tw_bulk(t, e + 1, (size_t)l);
			return;
		}
		break;
	}
	pc_tw_err(t, "ERR upstream: an unexpected reply");
}

/* ---- read-through on the native doors (the RESP door's gate is in pc_verb_resp).  HELD:
 * proto parks a copy of the FRAME under its id and dispatches it again on completion */
static int up_native(pcache_htable_t *ht, int cls, int del,
		const char *const *keys, const size_t *kl, int n,
		const char **errmsg, const struct pc_upfw *fw, struct up_ans *ans)
{
	struct pc_upg g;
	const char *cn = col_name_of(ht);
	size_t cl = strlen(cn);
	int rc, verdict = pc_conn_upstream_frame_verdict();
	int rd = (cls == PC_UPC_READ || cls == PC_UPC_PROBE) && !del;
	unsigned long long end0 = rd ? pc_upstream_rm_mark(cn, cl) : 0;

	rc = pc_upstream_gate_fw(ht, cn, cl, cls, del, keys, kl, n, verdict, &g,
		fw);
	if (rc == PC_UPG_HELD) {
		pc_conn_upstream_park_frame(g.req);
		pc_conn_upstream_park_frame_mark(end0);
	} else if (rc == PC_UPG_PROCEED && rd && verdict != PC_UPV_ERROR)
		/* for pc_upstream_lost: on the run after a fetch, what went since it parked */
		pc_upstream_lost_arm(cn, cl, verdict == PC_UPV_OK ?
			pc_conn_upstream_frame_mark() : end0);
	else if (rc == PC_UPG_ANSWERED && g.done && ans && ans->want_tree) {
		up_reply_tree(&ans->tree, &g);
		return UPG_TREE;
	} else if (rc == PC_UPG_ANSWERED && g.done && !g.rerr) {
		if (ans)
			ans->ival = g.ival;        /* Redis applied it */
		return UPG_REPLY;
	} else if (rc == PC_UPG_ANSWERED)
		*errmsg = g.rerr ? g.rerr : g.wrongtype ? pc_wrongtype_msg : g.err;
	return rc;
}

/* a native get that missed: 1 its key went between the gate and the read - gate it again
 * (twice at most), the verdict cleared so it fetches */
static int up_get_lost(pcache_htable_t *ht, const str *k, int *tries)
{
	const char *cn = col_name_of(ht), *kp = k->s;
	size_t kn = (size_t)k->len;

	if (!pc_upstream_lost(ht, cn, strlen(cn), &kp, &kn, 1))
		return 0;
	if (*tries >= 2) {
		pc_upstream_lost_spent();
		return 0;
	}
	(*tries)++;
	pc_conn_upstream_frame_regate();
	return 1;
}

/* the verbs upstream_writes = through forwards; the rest (conditional sets and
 * deletes, jincr, jarrappend, rlhit) are refused there, not guessed at */
static const char up_fw_refused[] = "upstream_writes = through does not "
	"forward this verb (set, setnx, setxx, add, sub, del, expire, hcmd, "
	"mset, jset and jdel are)";

static void fw_one(struct pc_upfw *fw, const char **av, size_t *al, int ac,
		int guess)
{
	static const int k1[1] = { 1 };

	memset(fw, 0, sizeof *fw);
	fw->argc = ac;
	fw->argv = av;
	fw->argl = al;
	fw->nk = 1;
	fw->kidx = k1;
	fw->guess = guess;
}

/* an add/sub's TTL as op_addsub means it: < 0 keeps the key's, 0 removes it, > 0 sets
 * it - the command after INCRBY/DECRBY, or none (0 returned) */
static int up_ctr_ttl(const char *kp, size_t kn, long long ttl,
		const char **a2, size_t *l2, char *tb, size_t tbn)
{
	if (ttl < 0)
		return 0;
	a2[0] = ttl > 0 ? "EXPIRE" : "PERSIST";
	l2[0] = ttl > 0 ? 6 : 7;
	a2[1] = kp;
	l2[1] = kn;
	if (!ttl)
		return 2;
	a2[2] = tb;
	l2[2] = (size_t)snprintf(tb, tbn, "%lld", ttl);
	return 3;
}

/* an H write forwarded as itself - argv [name, key, args...], Redis's reply as the hash
 * core's tree; arguments the handler would refuse are left to it (PROCEED) */
static int up_hcmd_fw(pcache_htable_t *ht, int cls, int del,
		const char **hv, size_t *hl, int argc, const char **errmsg,
		struct up_ans *ans)
{
	struct pc_upfw fw;
	int ci = hcmd_find(hv[0], hl[0]);

	if (ci < 0 || !hcmds[ci].write || !hcmd_arity_ok(ci, argc))
		return PC_UPG_PROCEED;
	hv[0] = hcmds[ci].name;
	hl[0] = strlen(hcmds[ci].name);
	fw_one(&fw, hv, hl, argc, PC_UPFW_HASH);
	ans->want_tree = 1;
	return up_native(ht, cls, del, &hv[1], &hl[1], 1, errmsg, &fw, ans);
}

static int up_bin_gate(pcache_htable_t *ht, unsigned int verb,
		const char *pl, size_t plen, size_t koff, size_t klen,
		const char **errmsg, struct up_ans *ans)
{
	const char *kp = pl + koff;
	int cls = PC_UPC_NONE, del = 0, all = 0;

	switch (verb) {
	case PC_VERB_GET:
		cls = PC_UPC_READ;
		break;
	case PC_VERB_EXISTS: case PC_VERB_TTL:
		cls = PC_UPC_PROBE;
		break;
	case PC_VERB_SET:
		cls = PC_UPC_PLAIN;
		break;
	case PC_VERB_DEL: case PC_VERB_EXPIRE:
		cls = PC_UPC_MUTATE;
		del = 1;
		break;
	case PC_VERB_ADD: case PC_VERB_SUB: case PC_VERB_SETNX:
	case PC_VERB_SETXX:
		cls = PC_UPC_MUTATE;
		break;
	case PC_VERB_HCMD: {
		/* [argc u16] then [len u32][bytes]...; the first is the name */
		size_t e = koff + klen;

		if (plen < e + 6)
			return PC_UPG_PROCEED;  /* the codec refuses it below */
		{
			size_t nl = (size_t)rd_u32((const unsigned char *)pl + e + 2);

			if (plen - e - 6 < nl)
				return PC_UPG_PROCEED;
			cls = pc_upstream_cmd_class(pl + e + 6, nl, NULL, NULL, 0,
				&del, &all);
		}
		break;
	}
	default:
		if (verb == PC_VERB_RLHIT &&
		    pc_upstream_through(col_name_of(ht), strlen(col_name_of(ht)))) {
			pc_upstream_wt_refused(col_name_of(ht),
				strlen(col_name_of(ht)), "bin:rlhit", 9, up_fw_refused);
			*errmsg = up_fw_refused;
			return PC_UPG_ANSWERED;
		}
		return PC_UPG_PROCEED;         /* RLHIT and the rest */
	}
	if (verb == PC_VERB_HCMD && cls == PC_UPC_MUTATE &&
	    pc_upstream_through(col_name_of(ht), strlen(col_name_of(ht)))) {
		/* the H write as itself - [argc u16][(len u32)(bytes)]* after
		 * the key, the name first (checked above: 6 bytes) */
		const unsigned char *e = (const unsigned char *)kp + klen;
		size_t el = plen - koff - klen, o = 2;
		const char *hv[PC_HCMD_MAXARGS];
		size_t hl[PC_HCMD_MAXARGS];
		int hc = e[0] | e[1] << 8, i;

		if (hc < 1 || hc + 1 > PC_HCMD_MAXARGS)
			return PC_UPG_PROCEED;     /* the handler refuses it */
		for (i = 0; i < hc; i++) {
			size_t l;

			if (el - o < 4)
				return PC_UPG_PROCEED;
			l = (size_t)rd_u32(e + o);
			if (el - o - 4 < l)
				return PC_UPG_PROCEED;
			hv[i ? i + 1 : 0] = (const char *)e + o + 4;
			hl[i ? i + 1 : 0] = l;
			o += 4 + l;
		}
		hv[1] = kp;
		hl[1] = klen;
		return up_hcmd_fw(ht, cls, del, hv, hl, hc + 1, errmsg, ans);
	}
	if ((cls == PC_UPC_PLAIN || cls == PC_UPC_MUTATE || del) &&
	    pc_upstream_through(col_name_of(ht), strlen(col_name_of(ht)))) {
		/* the Redis command the verb means (TTLs are seconds) */
		const char *av[7], *a2[3];
		size_t al[7], l2[3];
		char tb[24], bb[24];
		struct pc_upfw fw;
		long long ttl = 0;
		int ac = 0, guess = PC_UPFW_STRING, n2 = 0;

		switch (verb) {
		case PC_VERB_SET: case PC_VERB_SETNX: case PC_VERB_SETXX:
			ttl = br_i64(pl + 4);
			av[ac] = "SET"; al[ac++] = 3;
			av[ac] = kp; al[ac++] = klen;
			av[ac] = pl + koff + klen; al[ac++] = plen - koff - klen;
			if (verb != PC_VERB_SET) {
				av[ac] = verb == PC_VERB_SETNX ? "NX" : "XX";
				al[ac++] = 2;
			}
			if (ttl > 0) {
				av[ac] = "EX"; al[ac++] = 2;
				av[ac] = tb;
				al[ac++] = (size_t)snprintf(tb, sizeof tb, "%lld", ttl);
			}
			break;
		case PC_VERB_DEL:
			av[ac] = "DEL"; al[ac++] = 3;
			av[ac] = kp; al[ac++] = klen;
			guess = PC_UPFW_GONE;
			break;
		case PC_VERB_EXPIRE:
			ttl = br_i64(pl + 4);
			if (ttl > 0) {
				av[ac] = "EXPIRE"; al[ac++] = 6;
				av[ac] = kp; al[ac++] = klen;
				av[ac] = tb;
				al[ac++] = (size_t)snprintf(tb, sizeof tb, "%lld", ttl);
			} else {
				av[ac] = "PERSIST"; al[ac++] = 7;  /* 0: no expiry */
				av[ac] = kp; al[ac++] = klen;
			}
			break;
		case PC_VERB_ADD: case PC_VERB_SUB:
			/* INCRBY/DECRBY, read back as the string Redis
			 * holds; the reply is Redis's new value */
			av[ac] = verb == PC_VERB_ADD ? "INCRBY" : "DECRBY";
			al[ac++] = 6;
			av[ac] = kp; al[ac++] = klen;
			av[ac] = bb;
			al[ac++] = (size_t)snprintf(bb, sizeof bb, "%lld",
				br_i64(pl + 4));
			n2 = up_ctr_ttl(kp, klen, br_i64(pl + 12), a2, l2, tb,
				sizeof tb);
			break;
		default:
			memset(&fw, 0, sizeof fw);
			fw.refuse = up_fw_refused;
			fw.what = verb == PC_VERB_HCMD ? "bin:hcmd" : "bin:?";
			fw.whatlen = strlen(fw.what);
			return up_native(ht, cls, del, &kp, &klen, 1, errmsg, &fw,
				ans);
		}
		fw_one(&fw, av, al, ac, guess);
		fw.argc2 = n2;
		fw.argv2 = a2;
		fw.argl2 = l2;
		return up_native(ht, cls, del, &kp, &klen, 1, errmsg, &fw, ans);
	}
	return up_native(ht, cls, del, &kp, &klen, 1, errmsg, NULL, ans);
}

/* a native mset on a write-through collection: one MSET, an EXPIRE per item with a TTL;
 * an mset whose keys are all the fleet's own (outside upstream_prefixes) is the handler's */
static int up_mset_fw(pcache_htable_t *ht, const struct pc_tv *v, int ta,
		const char *const *ks, const size_t *kl, int n, const char **errmsg,
		struct up_ans *ans)
{
	const char *cn = col_name_of(ht), **av = NULL, **a2 = NULL;
	size_t cl = strlen(cn), *al = NULL, *l2 = NULL;
	char *tb = NULL;
	int *kx = NULL, ac = 0, nv = 0, nx = 0, outside = 0, rc, e;
	unsigned int it;
	struct pc_upfw fw;
	static const char mixed[] = "upstream_writes = through: this mset "
		"mixes keys inside and outside upstream_prefixes - send them in two";

	av = malloc((size_t)(2 * n + 1) * sizeof *av);
	al = malloc((size_t)(2 * n + 1) * sizeof *al);
	kx = malloc((size_t)n * sizeof *kx);
	a2 = malloc((size_t)(3 * n) * sizeof *a2);
	l2 = malloc((size_t)(3 * n) * sizeof *l2);
	tb = malloc((size_t)n * 24);
	if (!av || !al || !kx || !a2 || !l2 || !tb) {
		rc = PC_UPG_ANSWERED;
		*errmsg = "upstream: out of memory forwarding the mset";
		goto out;
	}
	av[ac] = "MSET";
	al[ac++] = 4;
	for (it = 0, e = ta + 1; it < v->n[ta].len; it++, e = v->n[e].next) {
		const char *kp, *vp;
		size_t kn, vn;
		long long ttl = 0;

		if (v->n[e].type != 'm')
			continue;
		if (get_bytes(v, e, "key", &kp, &kn, KEY_MAX) != 0 || !kn ||
		        get_bytes(v, e, "value", &vp, &vn, VAL_MAX) != 0 ||
		        (pc_tv_get_int(v, e, "ttl", &ttl) == 0 && ttl > PC_TTL_MAX)) {
			ans->dropped++;            /* as the handler drops it */
			continue;
		}
		if (!pc_upstream_allowed(cn, cl, kp, kn))
			outside++;
		kx[nv++] = ac;
		av[ac] = kp; al[ac++] = kn;
		av[ac] = vp; al[ac++] = vn;
		pc_tv_get_int(v, e, "ttl", &ttl);
		if (ttl > 0) {                 /* MSET leaves none: set_local's 0 */
			a2[3 * nx] = "EXPIRE";
			l2[3 * nx] = 6;
			a2[3 * nx + 1] = kp;
			l2[3 * nx + 1] = kn;
			a2[3 * nx + 2] = tb + 24 * nx;
			l2[3 * nx + 2] = (size_t)snprintf(tb + 24 * nx, 24, "%lld",
				ttl);
			nx++;
		}
	}
	if (!nv || outside == nv) {
		rc = PC_UPG_PROCEED;           /* the fleet's own keys: here */
		goto out;
	}
	if (outside) {
		pc_upstream_wt_refused(cn, cl, "cmd:mset", 8, mixed);
		*errmsg = mixed;
		rc = PC_UPG_ANSWERED;
		goto out;
	}
	memset(&fw, 0, sizeof fw);
	fw.argc = ac;
	fw.argv = av;
	fw.argl = al;
	fw.nk = nv;
	fw.kidx = kx;
	fw.guess = PC_UPFW_STRING;
	if (nx) {
		fw.argc2 = 3;
		fw.n2 = nx;
		fw.argv2 = a2;
		fw.argl2 = l2;
	}
	ans->stored = nv;
	rc = up_native(ht, PC_UPC_PLAIN, 0, ks, kl, n, errmsg, &fw, ans);
out:
	free(av);
	free(al);
	free(kx);
	free(a2);
	free(l2);
	free(tb);
	return rc;
}

/* a j* verb's path as its handler reads it: absent is the root; -1 for one present but not
 * readable, or empty - the gate then proceeds and the handler refuses it */
static int gate_jpath(const struct pc_tv *v, int params, char *path)
{
	int pl = pc_tv_get_str(v, params, "path", path, JP_NAME_PARAM);

	if (pl < 0 && pc_tv_get(v, params, "path") < 0) {
		path[0] = '$';
		return 1;
	}
	return pl > 0 ? pl : -1;
}

static int up_cmd_gate(pcache_htable_t *ht, const char *method, size_t mlen,
		const struct pc_tv *v, int params, const char **errmsg,
		struct up_ans *ans)
{
	int cls = PC_UPC_NONE, del = 0, all = 0, rc;
	const char *kp;
	size_t kn;

	if (m_is(method, mlen, "get") || m_is(method, mlen, "probe") ||
	    m_is(method, mlen, "jget"))
		cls = PC_UPC_READ;
	else if (m_is(method, mlen, "exists") || m_is(method, mlen, "ttl"))
		cls = PC_UPC_PROBE;
	else if (m_is(method, mlen, "set"))
		cls = PC_UPC_PLAIN;
	else if (m_is(method, mlen, "jset")) {
		char path[JP_NAME_PARAM];
		int pl = gate_jpath(v, params, path);
		int tn = pc_tv_get(v, params, "nx"), tx = pc_tv_get(v, params, "xx");

		if (pl < 0)
			return PC_UPG_PROCEED;
		cls = pl == 1 && (path[0] == '$' || path[0] == '.') &&
			tn < 0 && tx < 0 ? PC_UPC_PLAIN : PC_UPC_MUTATE;
	} else if (m_is(method, mlen, "del") || m_is(method, mlen, "delifeq") ||
	    m_is(method, mlen, "delifne") || m_is(method, mlen, "expire") ||
	    m_is(method, mlen, "jdel")) {
		cls = PC_UPC_MUTATE;
		del = 1;
	} else if (m_is(method, mlen, "setnx") || m_is(method, mlen, "setxx") ||
	    m_is(method, mlen, "setifeq") || m_is(method, mlen, "setifne") ||
	    m_is(method, mlen, "add") || m_is(method, mlen, "sub") ||
	    m_is(method, mlen, "jincr") || m_is(method, mlen, "jarrappend"))
		cls = PC_UPC_MUTATE;
	else if (m_is(method, mlen, "hcmd")) {
		char cmd[16];
		int cl = pc_tv_get_str(v, params, "cmd", cmd, sizeof cmd - 1);

		if (cl <= 0)
			return PC_UPG_PROCEED;
		cls = pc_upstream_cmd_class(cmd, (size_t)cl, NULL, NULL, 0, &del,
			&all);
	} else if (m_is(method, mlen, "rlhit")) {
		/* the rate limiter writes perfcached only - refused on a
		 * write-through collection, as the binary verb and RL.HIT are */
		if (!pc_upstream_through(col_name_of(ht), strlen(col_name_of(ht))))
			return PC_UPG_PROCEED;
		pc_upstream_wt_refused(col_name_of(ht), strlen(col_name_of(ht)),
			"cmd:rlhit", 9, up_fw_refused);
		*errmsg = up_fw_refused;
		return PC_UPG_ANSWERED;
	} else if (m_is(method, mlen, "mget") || m_is(method, mlen, "mset")) {
		/* many keys: mget's "keys", mset's "items"[].key */
		int ism = method[1] == 's';
		int ta = pc_tv_get(v, params, ism ? "items" : "keys");
		const char **ks;
		size_t *kl;
		unsigned int i;
		int n = 0, e;

		/* past the handler's element cap: the handler refuses it */
		if (ta < 0 || v->n[ta].type != 'a' || !v->n[ta].len ||
		        v->n[ta].len > MULTI_MAX)
			return PC_UPG_PROCEED;
		ks = malloc(v->n[ta].len * sizeof *ks);
		kl = malloc(v->n[ta].len * sizeof *kl);
		if (!ks || !kl) {
			free(ks);
			free(kl);
			*errmsg = "upstream: out of memory";
			return PC_UPG_ANSWERED;
		}
		for (i = 0, e = ta + 1; i < v->n[ta].len; i++, e = v->n[e].next) {
			if (ism) {
				if (get_bytes(v, e, "key", &kp, &kn, KEY_MAX) != 0 ||
				        !kn)
					continue;
			} else {
				if (v->n[e].type != 'b' || v->n[e].len > KEY_MAX)
					continue;
				kp = (const char *)v->n[e].p;
				kn = v->n[e].len;
			}
			ks[n] = kp;
			kl[n] = kn;
			n++;
		}
		if (ism && n && pc_upstream_through(col_name_of(ht),
		        strlen(col_name_of(ht)))) {
			rc = up_mset_fw(ht, v, ta, ks, kl, n, errmsg, ans);
			free(ks);
			free(kl);
			return rc;
		}
		rc = n ? up_native(ht, ism ? PC_UPC_PLAIN : PC_UPC_READ, 0, ks,
			kl, n, errmsg, NULL, ans) : PC_UPG_PROCEED;
		free(ks);
		free(kl);
		return rc;
	}
	if (cls == PC_UPC_NONE)
		return PC_UPG_PROCEED;
	if (get_bytes(v, params, "key", &kp, &kn, KEY_MAX) != 0 || !kn)
		return PC_UPG_PROCEED;          /* the method refuses it below */
	if ((cls == PC_UPC_PLAIN || cls == PC_UPC_MUTATE || del) &&
	    pc_upstream_through(col_name_of(ht), strlen(col_name_of(ht)))) {
		/* the Redis command the method means */
		const char *av[7], *a2[3];
		size_t al[7], l2[3];
		char tb[24], bb[24], path[JP_NAME_PARAM];
		struct pc_upfw fw;
		long long ttl = 0;
		int ac = 0, guess = PC_UPFW_STRING, pl;

		memset(&fw, 0, sizeof fw);
		if (m_is(method, mlen, "set") || m_is(method, mlen, "setnx") ||
		    m_is(method, mlen, "setxx")) {
			const char *vp;
			size_t vl;

			if (get_bytes(v, params, "value", &vp, &vl, (size_t)-1) != 0)
				return PC_UPG_PROCEED;     /* refused below */
			pc_tv_get_int(v, params, "ttl", &ttl);
			av[ac] = "SET"; al[ac++] = 3;
			av[ac] = kp; al[ac++] = kn;
			av[ac] = vp; al[ac++] = vl;
			if (!m_is(method, mlen, "set")) {
				av[ac] = m_is(method, mlen, "setnx") ? "NX" : "XX";
				al[ac++] = 2;
			}
			if (ttl > 0) {
				av[ac] = "EX"; al[ac++] = 2;
				av[ac] = tb;
				al[ac++] = (size_t)snprintf(tb, sizeof tb, "%lld", ttl);
			}
		} else if (m_is(method, mlen, "del")) {
			av[ac] = "DEL"; al[ac++] = 3;
			av[ac] = kp; al[ac++] = kn;
			guess = PC_UPFW_GONE;
		} else if (m_is(method, mlen, "expire")) {
			if (pc_tv_get_int(v, params, "ttl", &ttl) != 0)
				return PC_UPG_PROCEED;     /* refused below */
			if (ttl > 0) {
				av[ac] = "EXPIRE"; al[ac++] = 6;
				av[ac] = kp; al[ac++] = kn;
				av[ac] = tb;
				al[ac++] = (size_t)snprintf(tb, sizeof tb, "%lld", ttl);
			} else {
				av[ac] = "PERSIST"; al[ac++] = 7;
				av[ac] = kp; al[ac++] = kn;
			}
		} else if ((m_is(method, mlen, "jset") || m_is(method, mlen, "jdel")) &&
		    !pc_tv_get_bool(v, params, "mkpath")) {
			const char *vp = NULL;
			size_t vl = 0;

			pl = gate_jpath(v, params, path);
			if (pl < 0)
				return PC_UPG_PROCEED;     /* refused below */
			fw.json = 1;
			guess = PC_UPFW_JSON;
			if (m_is(method, mlen, "jset")) {
				if (get_bytes(v, params, "val", &vp, &vl, (size_t)-1) != 0)
					return PC_UPG_PROCEED; /* refused below */
				av[ac] = "JSON.SET"; al[ac++] = 8;
				av[ac] = kp; al[ac++] = kn;
				av[ac] = path; al[ac++] = (size_t)pl;
				av[ac] = vp; al[ac++] = vl;
				if (pc_tv_get_bool(v, params, "nx")) {
					av[ac] = "NX"; al[ac++] = 2;
				} else if (pc_tv_get_bool(v, params, "xx")) {
					av[ac] = "XX"; al[ac++] = 2;
				}
				if (pc_tv_get_int(v, params, "ttl", &ttl) == 0) {
					/* RedisJSON has no EX: an EXPIRE after it */
					a2[0] = ttl > 0 ? "EXPIRE" : "PERSIST";
					l2[0] = ttl > 0 ? 6 : 7;
					a2[1] = kp;
					l2[1] = kn;
					a2[2] = tb;
					l2[2] = (size_t)snprintf(tb, sizeof tb, "%lld", ttl);
					fw.argc2 = ttl > 0 ? 3 : 2;
					fw.argv2 = a2;
					fw.argl2 = l2;
				}
			} else {
				av[ac] = "JSON.DEL"; al[ac++] = 8;
				av[ac] = kp; al[ac++] = kn;
				av[ac] = path; al[ac++] = (size_t)pl;
			}
		} else if (m_is(method, mlen, "add") || m_is(method, mlen, "sub")) {
			/* INCRBY/DECRBY, the handler's defaults (by 1, ttl 0) */
			long long by = 1;

			if (pc_tv_get_int(v, params, "by", &by) != 0)
				by = 1;
			pc_tv_get_int(v, params, "ttl", &ttl);
			av[ac] = method[0] == 'a' ? "INCRBY" : "DECRBY"; al[ac++] = 6;
			av[ac] = kp; al[ac++] = kn;
			av[ac] = bb;
			al[ac++] = (size_t)snprintf(bb, sizeof bb, "%lld", by);
			fw.argc2 = up_ctr_ttl(kp, kn, ttl, a2, l2, tb, sizeof tb);
			fw.argv2 = a2;
			fw.argl2 = l2;
		} else if (m_is(method, mlen, "hcmd")) {
			/* the H write as itself, its args after the key */
			const char *hv[PC_HCMD_MAXARGS];
			size_t hl[PC_HCMD_MAXARGS];
			char cmd[16];
			int cl = pc_tv_get_str(v, params, "cmd", cmd, sizeof cmd - 1);
			int ta = pc_tv_get(v, params, "args"), hc = 2;

			if (cl <= 0)
				return PC_UPG_PROCEED;     /* the handler refuses it */
			hv[0] = cmd;
			hl[0] = (size_t)cl;
			hv[1] = kp;
			hl[1] = kn;
			if (ta >= 0) {
				unsigned int ai;

				if (v->n[ta].type != 'a')
					return PC_UPG_PROCEED;
				for (ai = 0; ai < v->n[ta].len; ai++) {
					int an = pc_tv_at(v, ta, ai);

					if (an < 0 || v->n[an].type != 'b' ||
					    hc >= PC_HCMD_MAXARGS)
						return PC_UPG_PROCEED;
					hv[hc] = (const char *)v->n[an].p;
					hl[hc++] = v->n[an].len;
				}
			}
			return up_hcmd_fw(ht, cls, del, hv, hl, hc, errmsg, ans);
		} else {
			char wn[40];

			snprintf(wn, sizeof wn, "cmd:%.*s", (int)(mlen < 32 ? mlen : 32),
				method);
			fw.refuse = up_fw_refused;
			fw.what = wn;
			fw.whatlen = strlen(wn);
			return up_native(ht, cls, del, &kp, &kn, 1, errmsg, &fw, ans);
		}
		{
			int argc2 = fw.argc2, json = fw.json;
			const char *const *v2 = fw.argv2;
			const size_t *l2p = fw.argl2;

			fw_one(&fw, av, al, ac, guess);
			fw.argc2 = argc2;
			fw.argv2 = v2;
			fw.argl2 = l2p;
			fw.json = json;
		}
		return up_native(ht, cls, del, &kp, &kn, 1, errmsg, &fw, ans);
	}
	return up_native(ht, cls, del, &kp, &kn, 1, errmsg, NULL, ans);
}

int pc_verb_bin(const char *pl, size_t plen, struct pc_jw *out, int *flags,
		unsigned int *park_req, const char **colp, size_t *collenp,
		const char **keyp, size_t *klenp, const char **errmsg, void *conn)
{
	unsigned int verb, cn;
	size_t klen, off;
	pcache_htable_t *ht;
	str k, v;
	long long by = 1, ttl = 0, nv = 0;
	unsigned int exp;
	int rc, lost_tries = 0;

	(void)conn;
	if (!plen)
		ERRB("empty request");
	verb = (unsigned char)pl[0];
	if (verb == PC_VERB_PING) {
		pc_jw_raw(out, pl + 1, plen - 1);
		return 0;
	}
	if ((verb < PC_VERB_GET || verb > PC_VERB_SUB) &&
	        verb != PC_VERB_SETNX && verb != PC_VERB_SETXX &&
	        verb != PC_VERB_RLHIT && verb != PC_VERB_HCMD)
		ERRB(pc_bin_unknown_verb);     /* proto.c counts it by this pointer */
	if (serving_denied())                  /* PING above is exempt */
		ERRB(PC_NOTREADY_MSG);
	if (plen < 4)
		ERRB("short request");
	cn = (unsigned char)pl[1];
	klen = br_u16(pl + 2);
	off = 4;
	if (verb == PC_VERB_EXPIRE || verb == PC_VERB_SET ||
	        verb == PC_VERB_SETNX || verb == PC_VERB_SETXX)
		off += 8;                      /* ttl i64 */
	else if (verb == PC_VERB_ADD || verb == PC_VERB_SUB ||
	        verb == PC_VERB_RLHIT)
		off += 16;                     /* by i64, ttl i64 */
	if (plen < off + cn + klen)
		ERRB("short request");
	if (!klen || klen > KEY_MAX)
		ERRB("missing or bad key");
	if (((verb == PC_VERB_EXPIRE || verb == PC_VERB_SET || verb == PC_VERB_SETNX ||
	        verb == PC_VERB_SETXX) && br_i64(pl + 4) > PC_TTL_MAX) ||
	        ((verb == PC_VERB_ADD || verb == PC_VERB_SUB) &&
	        br_i64(pl + 12) > PC_TTL_MAX))
		ERRB(PC_TTL_MSG);
	ht = pc_store_find(pl + off, cn);
	if (!ht)
		ERRB("no such collection");
	k.s = (char *)pl + off + cn;
	k.len = (int)klen;
	*colp = pl + off;
	*collenp = cn;
	*keyp = k.s;
	*klenp = klen;

regate:
	if (ht->shadow && conn) {
		struct up_ans ans;
		int g;

		memset(&ans, 0, sizeof ans);
		pc_tw_init(&ans.tree, NULL, 0);
		g = up_bin_gate(ht, verb, pl, plen, off + cn, klen, errmsg, &ans);
		if (g == UPG_TREE) {
			if (pc_tw_done(&ans.tree) != 0 || !ans.tree.n) {
				pc_tw_free(&ans.tree);
				ERRB("internal reply error");
			}
			pc_jw_raw(out, (const char *)ans.tree.b, ans.tree.n);
			pc_tw_free(&ans.tree);
			return 0;
		}
		pc_tw_free(&ans.tree);
		if (g == PC_UPG_HELD)
			return 0;
		if (g == PC_UPG_ANSWERED)
			return -1;
		if (g == UPG_REPLY && (verb == PC_VERB_ADD || verb == PC_VERB_SUB)) {
			bw_i64(out, ans.ival);
			return 0;
		}
		if (g == UPG_REPLY) {
			bw_u8(out, ans.ival > 0);
			return 0;
		}
	}

	switch (verb) {
	case PC_VERB_GET:
		{
			char *gb = get_buf();
			unsigned int gl = 0;

			if (!gb)
				ERRB("get failed");
			if (ht->shadow && conn)
				pc_upstream_test_pause();
			rc = op_get_buf(ht, &k, gb, VAL_MAX, &gl, &exp,
				park_req);
			if (rc == PC_OP_OK) {
				unsigned int now = get_ticks();

				/* 0 is "no expiry" on this wire: a record past its
				 * expiry since the fetch has 1 s left, never 0 */
				bw_u8(out, 1);
				bw_u32(out, !exp ? 0 : exp > now ? exp - now : 1);
				pc_jw_raw(out, gb, (size_t)gl);
				return 0;
			}
		}
		if (rc == PC_OP_ERR_TYPE)
			ERRB(pc_wrongtype_msg);
		if (rc != PC_OP_ERR_GET && ht->shadow && conn &&
		    !(park_req && *park_req) && up_get_lost(ht, &k, &lost_tries))
			goto regate;
		if (rc == PC_OP_ERR_GET) {
			ERRB("get failed");
		} else {
			bw_u8(out, 0);         /* miss; also the park fallback */
		}
		return 0;

	case PC_VERB_EXISTS:
	case PC_VERB_TTL:
		pc_pres_verb(ht, &k, verb == PC_VERB_EXISTS ? PC_PRES_EXISTS :
			PC_PRES_TTL, out, park_req);
		return 0;

	case PC_VERB_DEL:
		rc = op_del(ht, &k, park_req);
		if (op_fail_text(rc))
			ERRB(op_fail_text(rc));
		bw_u8(out, rc == PC_OP_OK);       /* parked falls back to 0 */
		return 0;

	case PC_VERB_EXPIRE:
		ttl = br_i64(pl + 4);
		rc = op_expire(ht, &k, ttl, park_req);
		if (op_fail_text(rc))
			ERRB(op_fail_text(rc));
		if (rc == PC_OP_ERR_FULL)
			ERRB("cache full");
		bw_u8(out, rc == PC_OP_OK);       /* parked falls back to 0 */
		return 0;

	case PC_VERB_ADD:
	case PC_VERB_SUB:
		by = br_i64(pl + 4);
		ttl = br_i64(pl + 12);
		if (verb == PC_VERB_SUB && by == LLONG_MIN)
			ERRB("sub: by out of range");
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
		if (rc == PC_OP_ERR_TYPE)
			ERRB(pc_wrongtype_msg);
		if (rc == PC_OP_ERR_NOTINT)
			ERRB("value is not an integer");
		if (rc == PC_OP_ERR_OVF)
			ERRB(PC_OVF_MSG);
		if (rc == PC_OP_ERR_WRFAIL)    /* refused: nv was never written */
			ERRB(wr_refused_msg());
		bw_i64(out, nv);
		return 0;

	case PC_VERB_HCMD: {
		const unsigned char *e = (const unsigned char *)pl + off + cn + klen;
		size_t el = plen - off - cn - klen, o = 2;
		const char *hargv[PC_HCMD_MAXARGS];
		size_t hargl[PC_HCMD_MAXARGS];
		struct pc_tw t;
		int hargc, i, ci, lacking = 0;

		if (el < 2)
			ERRB("short request");
		hargc = e[0] | e[1] << 8;      /* the name + the args after the key */
		if (hargc < 1 || hargc + 1 > PC_HCMD_MAXARGS)
			ERRB("bad argument count");
		for (i = 0; i < hargc; i++) {
			size_t l;

			if (el - o < 4)
				ERRB("short request");
			l = (size_t)rd_u32(e + o);
			if (el - o - 4 < l)
				ERRB("short request");
			hargv[i ? i + 1 : 0] = (const char *)e + o + 4;
			hargl[i ? i + 1 : 0] = l;
			o += 4 + l;
		}
		hargv[1] = k.s;
		hargl[1] = (size_t)k.len;
		ci = hcmd_find(hargv[0], hargl[0]);
		if (ci < 0)
			ERRB("not a hash command");
		if (!hcmd_arity_ok(ci, hargc + 1))
			ERRB("wrong number of arguments");
		pc_tw_init(&t, NULL, 0);
		rc = op_hcmd(ht, ci, hargc + 1, hargv, hargl, &t, park_req,
			&lacking);
		if (rc == PC_OP_OK && (pc_tw_done(&t) != 0 || !t.n)) {
			pc_tw_free(&t);
			ERRB("internal reply error");
		}
		if (rc == PC_OP_OK) {
			pc_jw_raw(out, (const char *)t.b, t.n);
		} else if (rc == PC_OP_PARKED) {
			/* the park-failure fallback is an error frame */
			*flags = PC_BIN_F_ERR;
			pc_jw_raw(out, "cluster busy", 12);
		}
		pc_tw_free(&t);
		if (rc == PC_OP_OK || rc == PC_OP_PARKED)
			return 0;
		if (rc == PC_OP_ERR_GATE)
			ERRB(hc_gate_text(lacking));
		if (rc == PC_OP_ERR_MODE)
			ERRB(hc_mode_msg);
		if (rc == PC_OP_ERR_2BIG)
			ERRB("hash command too large: its arguments exceed 58000 bytes");
		if (rc == PC_OP_ERR_BUSY)
			ERRB("parked-request table full - retry");
		if (rc == PC_OP_ERR_FWD)
			ERRB("forward failed");
		if (rc == PC_OP_ERR_WRFAIL)
			ERRB(wr_refused_msg());
		ERRB("hash command failed");
	}

	case PC_VERB_RLHIT: {
		long long win = br_i64(pl + 4), lim = br_i64(pl + 12), cnt = 0;
		int al = 0, lacking = 0;

		if (win < 1 || win > 86400000LL)
			ERRB("window_ms: 1..86400000");
		if (lim < 0 || lim >= PC_RL_MAX)
			ERRB("limit: 0 (none) .. 5999");
		rc = op_rlhit(ht, &k, win, lim, &cnt, &al, park_req, &lacking);
		if (rc == PC_OP_OK) {
			bw_i64(out, cnt);
			bw_u8(out, al ? 1 : 0);
		} else if (rc == PC_OP_PARKED) {
			/* the park-failure fallback is an error frame */
			*flags = PC_BIN_F_ERR;
			pc_jw_raw(out, "cluster busy", 12);
		} else if (rc == PC_OP_ERR_GATE) {
			ERRB(rl_gate_text(lacking));
		} else if (rc == PC_OP_ERR_MODE) {
			ERRB(rl_mode_msg);
		} else if (rc == PC_OP_ERR_TYPE) {
			ERRB(pc_wrongtype_msg);
		} else if (rc == PC_OP_ERR_BUSY) {
			ERRB("parked-request table full - retry");
		} else if (rc == PC_OP_ERR_FWD) {
			ERRB("forward failed");
		} else if (rc == PC_OP_ERR_WRFAIL) {
			ERRB(wr_refused_msg());
		} else {
			ERRB("rate-limit hit failed");
		}
		return 0;
	}

	case PC_VERB_SETNX:
	case PC_VERB_SETXX:
		v.s = (char *)pl + off + cn + klen;
		v.len = (int)(plen - off - cn - klen);
		ttl = br_i64(pl + 4);
		rc = op_setcond(ht, &k, &v, ttl, verb == PC_VERB_SETNX ?
			PC_SETCOND_NX : PC_SETCOND_XX, park_req);
		if (rc == PC_OP_OK)
			bw_u8(out, 1);
		else if (rc == PC_OP_ABSENT || rc == PC_OP_PARKED)
			bw_u8(out, 0);         /* declined; park fallback */
		else if (rc == PC_OP_ERR_MODE)
			ERRB(setcond_mode_msg);
		else if (rc == PC_OP_ERR_BUSY)
			ERRB("parked-request table full - retry");
		else if (rc == PC_OP_ERR_FWD)
			ERRB("forward failed");
		else if (rc == PC_OP_ERR_WRFAIL)
			ERRB(wr_refused_msg());
		else if (rc == PC_OP_ERR_FULL)
			ERRB("cache full");
		else
			ERRB("value too large");
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
			ERRB(wr_refused_msg());
		else if (rc == PC_OP_ERR_FULL)
			ERRB("cache full");
		else
			ERRB("value too large");
		return 0;
	}
}

/* ---- probe-resume replays (proto.c, PC_DONE_SET_RESUME): the key is absent fleet-wide,
 * so the deferred write runs the normal core with the probe off - it may still forward */

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

/* ---- RESP2 dialect: Redis clients answer through the SAME op cores as the native door,
 * so cluster semantics cannot drift; unsupported commands and options refuse with -ERR */

/* a resp_collections entry `INDEX:NAME` makes NAME the collection SELECT INDEX lands on;
 * a bare NAME is allowed and addressed by itself.  Resolved once per command */
size_t resp_map_index(const char *sel, size_t sl, char *out, size_t cap)
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

/* the index a RESP client selects to reach @name, for INFO keyspace; a collection with
 * no index entry is reported under its own name */
size_t resp_index_for(const char *name, size_t nl, char *out, size_t cap)
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

/* is @name inside the RESP allow-list (resp_collections, NULL = all)?  SELECT maps a db
 * index onto a collection NAME, so without it any name is reachable */
int resp_col_allowed(const char *name, size_t n)
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

			/* `INDEX:NAME` allows NAME: the index is how a RESP client asks */
			if (colon) {
				size_t off = (size_t)(colon - p) + 1;

				if (len - off == n &&
				    !memcmp(colon + 1, name, n))
					return 1;
			} else if (len == 1 && *p == '*') {
				return 1;      /* no list in the file: every name visible */
			} else if (len == n && !memcmp(p, name, n)) {
				return 1;
			}
		}
		p = e;
	}
	return 0;
}

/* the selected collection, or an -ERR written into @out */
pcache_htable_t *resp_col(struct pc_jw *out, const char *cur_col,
		size_t cur_collen)
{
	pcache_htable_t *ht;
	char nm[64];
	size_t nl = resp_map_index(cur_col, cur_collen, nm, sizeof nm);

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

/* the ONE answer to "is this a cluster" that INFO server, INFO cluster and CLUSTER INFO
 * all derive from: cluster-aware clients trust INFO, so they must never disagree */
int resp_cluster_on(void)
{
	return pc_cluster_enabled();
}

/* ---- the command table: the one list of the RESP commands this door answers */
/* the plane: which side of the readiness gate and the collection resolution answers a
 * command - not interchangeable (a RECOVERING node owes FLUSHDB -LOADING) */
#define RESP_CONN   0x01               /* before the readiness gate */
#define RESP_READY  0x02               /* past it, before the collection */
#define RESP_DATA   0x04               /* past both: needs the collection */
#define RESP_KEYED  0x10               /* argv[1] is THE key (of several, the first) */

struct resp_cmd {
	const char *name;
	unsigned flags;
	int (*fn)(struct resp_ctx *);
};

/* a subcommand this door does not implement, counted by name -
 * the command and the subcommand, never an argument after them */
void unk_sub(const struct resp_ctx *c)
{
	if (c->nargs >= 2)
		pc_obs_unknown(CLUNK_RESP, c->argv[0], c->argl[0], c->argv[1],
			c->argl[1], c->obs);
}


/* every H command - the reply tree rendered as RESP */
static int rh_hash(struct resp_ctx *c)
{
	int ci = hcmd_find(c->cmd, c->cl), rc, lacking = 0;
	struct pc_tw t;
	char msg[96];

	if (ci < 0 || !hcmd_arity_ok(ci, c->nargs)) {
		snprintf(msg, sizeof msg, "wrong number of arguments for '%s' "
			"command", ci < 0 ? "?" : hcmds[ci].lower);
		resp_err(c->out, msg);
		return 0;
	}
	if (c->argl[1] == 0 || c->argl[1] > KEY_MAX) {
		resp_err(c->out, "key too long");
		return 0;
	}
	*c->keyp = c->argv[1];
	*c->klenp = c->argl[1];
	pc_tw_init(&t, NULL, 0);
	rc = op_hcmd(c->ht, ci, c->nargs, (const char *const *)c->argv,
		c->argl, &t, c->park_req, &lacking);
	if (rc == PC_OP_OK) {
		if (!t.n || pc_tree_resp(c->out, t.b, t.n) != t.n)
			resp_err(c->out, "internal reply error");
	} else if (rc == PC_OP_PARKED)
		resp_err(c->out, "busy");         /* park fallback only */
	else if (rc == PC_OP_ERR_GATE)
		resp_err_line(c->out, hc_gate_text(lacking));
	else if (rc == PC_OP_ERR_MODE)
		resp_err_line(c->out, hc_mode_msg);
	else if (rc == PC_OP_ERR_2BIG)
		resp_err(c->out, "hash command too large: its arguments exceed "
			"58000 bytes");
	else if (rc == PC_OP_ERR_BUSY)
		resp_err_code(c->out, "TRYAGAIN", "cluster busy, retry");
	else if (rc == PC_OP_ERR_FWD)
		resp_err(c->out, "forward failed");
	else if (rc == PC_OP_ERR_WRFAIL)
		resp_err(c->out, wr_refused_msg());
	else
		resp_err(c->out, "out of memory");
	pc_tw_free(&t);
	return 0;
}

static const struct resp_cmd resp_cmds[] = {
	{ "PING", RESP_CONN, rh_ping }, { "ECHO", RESP_CONN, rh_echo },
	{ "QUIT", RESP_CONN, rh_quit },
	{ "AUTH", RESP_CONN, rh_auth }, { "HELLO", RESP_CONN, rh_hello },
	/* pub/sub lives on the connection plane - a subscriber is a
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
	{ "DBSIZE", RESP_READY, rh_dbsize }, { "TIME", RESP_READY, rh_time },
	{ "FLUSHDB", RESP_READY, rh_flush },
	{ "FLUSHALL", RESP_READY, rh_flush },
	{ "GET", RESP_DATA | RESP_KEYED, rh_get }, { "SET", RESP_DATA | RESP_KEYED, rh_set },
	{ "SETEX", RESP_DATA | RESP_KEYED, rh_set }, { "PSETEX", RESP_DATA | RESP_KEYED, rh_set },
	{ "DEL", RESP_DATA | RESP_KEYED, rh_del }, { "UNLINK", RESP_DATA | RESP_KEYED, rh_del },
	{ "DELEX", RESP_DATA | RESP_KEYED, rh_delex },
	{ "EVAL", RESP_DATA, rh_eval }, { "EVALSHA", RESP_DATA, rh_eval },
	{ "SCRIPT", RESP_DATA, rh_script },
	{ "EXISTS", RESP_DATA | RESP_KEYED, rh_exists }, { "TYPE", RESP_DATA | RESP_KEYED, rh_type },
	{ "EXPIRE", RESP_DATA | RESP_KEYED, rh_expire }, { "PEXPIRE", RESP_DATA | RESP_KEYED, rh_expire },
	{ "EXPIREAT", RESP_DATA | RESP_KEYED, rh_expire }, { "PEXPIREAT", RESP_DATA | RESP_KEYED, rh_expire },
	{ "TTL", RESP_DATA | RESP_KEYED, rh_ttl }, { "PTTL", RESP_DATA | RESP_KEYED, rh_ttl },
	{ "INCR", RESP_DATA | RESP_KEYED, rh_incr }, { "DECR", RESP_DATA | RESP_KEYED, rh_incr },
	{ "INCRBY", RESP_DATA | RESP_KEYED, rh_incr }, { "DECRBY", RESP_DATA | RESP_KEYED, rh_incr },
	{ "MGET", RESP_DATA, rh_mget }, { "KEYS", RESP_DATA, rh_keys },
	{ "SCAN", RESP_DATA, rh_scan }, { "MEMORY", RESP_DATA, rh_memory },
	{ "JSON.SET", RESP_DATA | RESP_KEYED, rh_json }, { "JSON.GET", RESP_DATA | RESP_KEYED, rh_json },
	{ "RL.HIT", RESP_DATA | RESP_KEYED, rh_rlhit },
	{ "HSET", RESP_DATA | RESP_KEYED, rh_hash }, { "HSETNX", RESP_DATA | RESP_KEYED, rh_hash },
	{ "HMSET", RESP_DATA | RESP_KEYED, rh_hash }, { "HGET", RESP_DATA | RESP_KEYED, rh_hash },
	{ "HMGET", RESP_DATA | RESP_KEYED, rh_hash }, { "HDEL", RESP_DATA | RESP_KEYED, rh_hash },
	{ "HEXISTS", RESP_DATA | RESP_KEYED, rh_hash }, { "HLEN", RESP_DATA | RESP_KEYED, rh_hash },
	{ "HSTRLEN", RESP_DATA | RESP_KEYED, rh_hash }, { "HKEYS", RESP_DATA | RESP_KEYED, rh_hash },
	{ "HVALS", RESP_DATA | RESP_KEYED, rh_hash }, { "HGETALL", RESP_DATA | RESP_KEYED, rh_hash },
	{ "HINCRBY", RESP_DATA | RESP_KEYED, rh_hash }, { "HINCRBYFLOAT", RESP_DATA | RESP_KEYED, rh_hash },
	{ "HRANDFIELD", RESP_DATA | RESP_KEYED, rh_hash }, { "HSCAN", RESP_DATA | RESP_KEYED, rh_hash },
	{ "JSON.DEL", RESP_DATA | RESP_KEYED, rh_json },
	{ "JSON.NUMINCRBY", RESP_DATA | RESP_KEYED, rh_json },
	{ "JSON.ARRAPPEND", RESP_DATA | RESP_KEYED, rh_json },
	{ "JSON.DEBUG", RESP_DATA, rh_json_debug }
};

/* -MOVED <slot> <ip>:<port> for a key @node owns; 0 when the member cannot be named and
 * the caller forwards - a redirect must never turn a servable request into an error */
static int resp_moved(struct pc_jw *out, int node, unsigned int slot)
{
	struct pc_member *mem = members_scratch();
	char ip[INET_ADDRSTRLEN];
	int n = mem ? pc_cluster_members(mem, PC_CL_MAXMEMBERS) : 0, i;

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

/* the command table as an open-addressed index over the case-folded name, built once by
 * the worker that claims it (CAS, release store); until then a caller scans the table */
#define RESP_IDX_SLOTS 256                      /* power of two, > 2x the rows */
#define RESP_NAME_MAX  32
static unsigned short resp_idx[RESP_IDX_SLOTS]; /* row + 1; 0 = empty */
static int resp_idx_state;                      /* 0 none, 1 building, 2 ready */

static unsigned int resp_name_hash(const char *s, size_t n)
{
	unsigned int h = 2166136261u;
	size_t i;

	for (i = 0; i < n; i++) {
		unsigned char ch = (unsigned char)s[i];

		if (ch >= 'a' && ch <= 'z')
			ch -= 32;
		h = (h ^ ch) * 16777619u;
	}
	return h;
}

static void resp_idx_build(void)
{
	size_t i, nrows = sizeof resp_cmds / sizeof resp_cmds[0];

	for (i = 0; i < nrows; i++) {
		unsigned int j = resp_name_hash(resp_cmds[i].name,
			strlen(resp_cmds[i].name)) & (RESP_IDX_SLOTS - 1);

		while (resp_idx[j])
			j = (j + 1) & (RESP_IDX_SLOTS - 1);
		resp_idx[j] = (unsigned short)(i + 1);
	}
}

static const struct resp_cmd *resp_cmd_find(const char *cmd, size_t cl)
{
	size_t i;

	if (__atomic_load_n(&resp_idx_state, __ATOMIC_ACQUIRE) == 2) {
		unsigned int j;

		if (cl == 0 || cl > RESP_NAME_MAX)
			return NULL;
		j = resp_name_hash(cmd, cl) & (RESP_IDX_SLOTS - 1);
		while (resp_idx[j]) {
			const struct resp_cmd *r = &resp_cmds[resp_idx[j] - 1];

			if (resp_is(cmd, cl, r->name))
				return r;
			j = (j + 1) & (RESP_IDX_SLOTS - 1);
		}
		return NULL;
	} else {
		int none = 0;

		if (__atomic_compare_exchange_n(&resp_idx_state, &none, 1, 0,
		        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
			resp_idx_build();
			__atomic_store_n(&resp_idx_state, 2, __ATOMIC_RELEASE);
		}
	}
	for (i = 0; i < sizeof resp_cmds / sizeof resp_cmds[0]; i++)
		if (resp_is(cmd, cl, resp_cmds[i].name))
			return &resp_cmds[i];
	return NULL;
}

int pc_verb_resp_known(const char *cmd, size_t cl)
{
	return resp_cmd_find(cmd, cl) != NULL;
}

int pc_verb_resp(char *const *argv, const size_t *argl, int nargs,
		struct pc_jw *out, char *scratch, size_t scratch_cap,
		unsigned int *park_req, const char **colp, size_t *collenp,
		const char **keyp, size_t *klenp, char *cur_col,
		size_t *cur_collen, int *quit, int *authed,
		struct pc_enum_start *es, void *obs, void *conn)
{
	pcache_htable_t *ht;
	const struct resp_cmd *row;
	const char *cmd;
	size_t cl;

	if (nargs < 1)
		return 0;                      /* empty inline line: no reply */
	cmd = argv[0];
	cl = argl[0];

	/* unauthenticated: only greet, authenticate and hang up - Redis's requirepass
	 * behaviour, PING included, so stock clients need no special-casing */
	if (!*authed && !resp_is(cmd, cl, "AUTH") &&
	        !resp_is(cmd, cl, "HELLO") && !resp_is(cmd, cl, "QUIT")) {
		/* an unknown command before AUTH is counted, never
		 * named - that is where port scanners live */
		if (!resp_cmd_find(cmd, cl))
			pc_obs_unknown_preauth(CLUNK_RESP);
		pc_jw_lit(out, "-NOAUTH Authentication required.\r\n");
		return 0;
	}
	/* SUBSCRIBED MODE, exactly redis's gate - predis's pubSubLoop
	 * breaks on anything else answering on a subscribed connection */
	if (pc_pubsub_count(conn) > 0 &&
	    !resp_is(cmd, cl, "SUBSCRIBE") && !resp_is(cmd, cl, "UNSUBSCRIBE") &&
	    !resp_is(cmd, cl, "PSUBSCRIBE") && !resp_is(cmd, cl, "PUNSUBSCRIBE") &&
	    !resp_is(cmd, cl, "PING") && !resp_is(cmd, cl, "QUIT") &&
	    !resp_is(cmd, cl, "RESET")) {
		char lc[64];
		size_t i, n = cl < sizeof lc - 1 ? cl : sizeof lc - 1;

		/* a control byte in the name would split the one-line error */
		for (i = 0; i < n; i++)
			lc[i] = (unsigned char)cmd[i] < 0x20 || cmd[i] == 0x7f ? ' '
				: (char)tolower((unsigned char)cmd[i]);
		lc[n] = 0;
		pc_jw_lit(out, "-ERR Can't execute '");
		pc_jw_lit(out, lc);
		pc_jw_lit(out, "': only (P|S)SUBSCRIBE / (P|S)UNSUBSCRIBE / "
			"PING / QUIT / RESET are allowed in this context\r\n");
		return 0;
	}

	/* ---- connection plane: a command is answered only from its row's side of BOTH gates
	 * (readiness, collection); the row is looked up once and read by every site below */
	row = resp_cmd_find(cmd, cl);
	{
		if (row && row->fn && (row->flags & RESP_CONN)) {
			struct resp_ctx c = { argv, argl, nargs, out, scratch,
				scratch_cap, park_req, colp, collenp, keyp,
				klenp, cur_col, cur_collen, quit, authed, es,
				obs, NULL, cmd, cl, conn };

			return row->fn(&c);
		}
	}

	/* Redis's own -LOADING, via resp_err_code (resp_err would send -ERR LOADING):
	 * clients already recognise it and retry rather than failing hard */
	if (serving_denied()) {
		resp_err_code(out, "LOADING", PC_NOTREADY_MSG);
		return 0;
	}

	/* ---- ready plane: past the readiness gate, before any collection - DBSIZE reports
	 * the keyspace, FLUSHDB would destroy it, TIME is still owed the gate */
	{
		if (row && row->fn && (row->flags & RESP_READY)) {
			struct resp_ctx c = { argv, argl, nargs, out, scratch,
				scratch_cap, park_req, colp, collenp, keyp,
				klenp, cur_col, cur_collen, quit, authed, es,
				obs, NULL, cmd, cl, conn };

			return row->fn(&c);
		}
	}

	/* ---- data plane: recognise the command BEFORE resolving the collection - an unknown
	 * command answers "unknown command" even with no collection named "0" */
	{
		if (!row || !(row->flags & RESP_DATA)) {
			char nb[64];
			size_t n = cl < sizeof nb - 1 ? cl : sizeof nb - 1;

			/* upstream_unknown = pass: an unknown command on a write-through collection
			 * goes to the upstream as Redis's COMMAND INFO classes it */
			if (!row && conn && pc_upstream_any()) {
				char upnm[64];
				size_t upl = resp_map_index(cur_col, *cur_collen, upnm,
					sizeof upnm);

				if (pc_upstream_resp_pass(upnm, upl, argv, argl, nargs,
				        out, conn) != PC_UPG_PROCEED)
					return 0;
			}
			{
				size_t i;

				/* a control byte in the name would split the one-line error */
				for (i = 0; i < n; i++)
					nb[i] = (unsigned char)cmd[i] < 0x20 || cmd[i] == 0x7f
						? ' ' : cmd[i];
			}
			nb[n] = 0;
			pc_obs_unknown(CLUNK_RESP, cmd, cl, NULL, 0, obs);
			pc_jw_lit(out, "-ERR unknown command '");
			pc_jw_raw(out, nb, n);
			pc_jw_lit(out, "'\r\n");
			return 0;
		}
	}
	ht = resp_col(out, cur_col, *cur_collen);
	if (!ht)
		return 0;
	/* the table's own name: cur_col may be a SELECT index the door would park with */
	*colp = col_name_of(ht);
	*collenp = strlen(*colp);

	{
		/* resp_redirect = moved: cluster clients refresh their slot map only on -MOVED,
		 * so a key owned elsewhere is redirected, not forwarded, when the owner is known */
		if (resp_redirect_moved && row && (row->flags & RESP_KEYED) &&
		        nargs >= 2 && argl[1] && pc_cluster_enabled()) {
			unsigned int slot = pc_key_slot(argv[1], argl[1]);
			int target = 0;

			if (pc_store_shard_enabled(ht))
				target = pc_shard_owner(*colp, *collenp,
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

			/* read-through: a key to fetch first parks the command; the gate takes the
			 * COLLECTION's name - cur_col may be a SELECT index (resp_collections) */
			/* never for another perfcached's fetcher: a daisy-chain cut, counted for
			 * this node's alert and page */
			if (ht->shadow && conn) {
				char upnm[64];
				size_t upl = resp_map_index(cur_col, *cur_collen,
					upnm, sizeof upnm);
				size_t at = out->len;
				int rc;

				if (pc_conn_rt_fetcher(conn)) {
					pc_upstream_chain_note(upnm, upl);
					return row->fn(&c);
				}
				/* a pure read whose key went between the gate and the handler
				 * answered nil: dropped and gated again (fetches), twice at most */
				for (;;) {
					if (pc_upstream_resp_gate(ht, upnm, upl, argv,
					        argl, nargs, out, conn) != PC_UPG_PROCEED)
						return 0;
					pc_upstream_test_pause();
					rc = row->fn(&c);
					if (rc < 0 || (park_req && *park_req) ||
					    pc_mget_parked() ||
					    (es && es->start) || out->owned ||
					    !pc_upstream_resp_lost(ht, upnm, upl, argv,
					        argl, nargs))
						return rc;
					if (!pc_conn_upstream_retry(conn)) {
						pc_upstream_lost_spent();
						return rc;
					}
					out->len = at;
					out->overflow = 0;
				}
			}
			return row->fn(&c);
		}
	}

	/* unreachable: the data-plane pre-check answered unknown commands */
	resp_err(out, "protocol");
	return 0;
}
