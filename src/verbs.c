/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * verbs.c — the verb set behind the native door (task S8; S317 made the
 * door binary-only).  See verbs.h.  pc_verb_cmd answers every method
 * that arrives in a PC_VERB_CMD frame - parameters read off a tree, the
 * reply written as one - pc_verb_bin the fixed-layout data verbs, and
 * pc_verb_resp the RESP door; all three answer through the same
 * dialect-neutral op cores.  TTLs on the wire are RELATIVE seconds; the
 * store keeps absolute ticks (0 = never), converted here at the boundary.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>                          /* S313 */
#include <strings.h>                        /* S313: strncasecmp */
#include <time.h>

#include <pthread.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "compat/dprint.h"
#include "quiesce.h"
#include "compat/timer.h"
#include "json.h"
#include "jsonpath.h"
#include "hashrec.h"                       /* S313 */
#include "sha1.h"                          /* S170a */
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
#include "clane.h"                   /* S36b: lane figures */
#include "clretain.h"                       /* S238 */
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
#define KEY_MAX     4096
#define JP_NAME_PARAM 512                    /* longest path string */
/* a method's failure: the message becomes the error frame's payload (S317
 * - no code; nothing ever read the JSON-RPC ones) */
#define ERR(msg) do { *errmsg = (msg); return -1; } while (0)
#define JW_REPLY_INIT (1u << 20)           /* S112: dump's heap reply, grows to PT_HEAP_MAX */

/* Per-thread READ scratch, never freed: workers live as long as the
 * daemon.  (The matching WRITE scratch went with the JSON dialect, S317:
 * a value now arrives as bytes inside the request and is stored from
 * where it lies.)  A GET used to cost a malloc + memcpy + free on top of
 * the copy-out the seqlock already requires: pcache_ht_fetch_ex() copies
 * the record into its own
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

/* S318: the hash edit scratch.  A hash write is a read-modify-write on
 * one record: the copy-out lands in get_buf() (a read is a read), and
 * each edit reads one buffer and writes the other, so a second one is
 * all a write needs - PC_HR_MAX + 64, the largest record an edit can
 * produce plus the room hashrec asks for.  It used to be two mallocs and
 * two frees per HSET on top of the fetch's own: invisible under glibc's
 * caching allocator, an mmap+munmap+fault cycle each under a non-caching
 * one (the reply scratch in proto.c measured that at ~50x).  Per thread,
 * taken on the first hash write - or the first HRANDFIELD with a count,
 * which borrows it for its index array, it being idle during a read - on
 * any thread that runs hash commands (workers, and the cluster thread
 * through hcmd_remote), and never freed: threads live as long as the
 * daemon. */
static __thread unsigned char *hr_scratch_v;

static unsigned char *hr_buf(void)
{
	if (!hr_scratch_v)
		hr_scratch_v = malloc(PC_HR_MAX + 64);
	return hr_scratch_v;
}

/* the smallest entry a hash record can hold is 7 bytes (a 6-byte entry
 * header, a one-byte name, an empty value), so the field count - and with
 * it HRANDFIELD's index array, one u32 per field - is bounded by the
 * record, and the array fits the scratch it borrows.  The premise is
 * that a record is at most PC_HR_MAX bytes: pc_hr_valid enforces it on
 * every record hcmd_exec reads (a record from recover or a cluster apply
 * is stamped F_HASH at any cell size, so the writers' cap alone is not
 * the bound) */
_Static_assert((PC_HR_MAX - PC_HR_HDR) / 7 * sizeof(unsigned int) <=
	PC_HR_MAX + 64, "HRANDFIELD's index array must fit the edit scratch");
/* and a forwarded hash command's encoding, capped at the forward ceiling
 * (op_hcmd), borrows it too */
_Static_assert(PC_MAX_FWD_VAL <= PC_HR_MAX + 64,
	"a forwarded hash command must fit the edit scratch");

/* relative seconds -> absolute ticks (0 stays 0 = never) */
static unsigned int ttl_to_abs(long long ttl)
{
	if (ttl <= 0)
		return 0;
	return get_ticks() + (unsigned int)ttl;
}

/* S317: a bytes parameter read IN PLACE - the node's span inside the
 * request tree, so a value, a payload or a key is stored from where it
 * arrived (no copy, no decode leg).  -1 = absent or not a 'b'; the cap
 * refuses what the caller's limit would have. */
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

/* the method name, compared on its bytes */
static int m_is(const char *method, size_t mlen, const char *name)
{
	return strlen(name) == mlen && !memcmp(method, name, mlen);
}

/* IPv4 dotted-quad without pulling inet_ntoa's static buffer into a
 * multithreaded reply path */
static const char *pc_inet_ntop4(struct in_addr a, char *buf, size_t cap)
{
	/* by hand: `members` formats one per member per call, and the
	 * snprintf family was 31% of its worker time */
	unsigned int v = ntohl(a.s_addr);
	char *p = buf;
	int sh;

	if (cap < 16) {
		if (cap)
			buf[0] = 0;
		return buf;
	}
	for (sh = 24; sh >= 0; sh -= 8) {
		unsigned int o = (v >> sh) & 0xff;

		if (o >= 100)
			*p++ = (char)('0' + o / 100);
		if (o >= 10)
			*p++ = (char)('0' + o / 10 % 10);
		*p++ = (char)('0' + o % 10);
		if (sh)
			*p++ = '.';
	}
	*p = 0;
	return buf;
}

/* lowercase hex of @n bytes into @out (2n chars, no terminator) */
static void hex_bytes(char *out, const unsigned char *b, size_t n)
{
	static const char hx[] = "0123456789abcdef";
	size_t i;

	for (i = 0; i < n; i++) {
		out[2 * i] = hx[b[i] >> 4];
		out[2 * i + 1] = hx[b[i] & 15];
	}
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

/* ---- dialect-neutral data-verb cores ------------------------------------
 * Every codec (the CMD methods, the fixed-layout binary verbs, RESP)
 * answers through these, so the cluster semantics - pull-on-miss, holder
 * forwarding, placement, WAL, tombstones - cannot drift between them.
 * The codec owns only the reply SHAPE (including its own park-failure
 * fallback, pre-written before returning PC_OP_PARKED). */


/* pc_*_begin() returns 0 for three unrelated reasons and this used to
 * flatten all of them into "forward failed".  Only a full parked-request
 * table is the daemon applying backpressure, and only that is worth a
 * client retrying.  S38. */
const char pc_wrongtype_msg[] =
	"WRONGTYPE Operation against a key holding the wrong kind of value";
const char pc_json_wrongtype_msg[] = "Existing key has wrong Redis type";

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
	/* S303: EAGER - every node holds every record, so once this node is
	 * READY its own miss is the fleet's answer: no pull, no parked
	 * client.  Asking cost every miss a network round trip and, when a
	 * peer was slow, the 80 ms pull timeout - on 245-247 0 of 67,000
	 * such pulls ever found the key.  A write acknowledged elsewhere
	 * and still in its push batch reads as a miss here; the read raced
	 * the write either way.  A node that is not READY (recovering,
	 * joining) still asks: that is when a peer knows more than it does.
	 * So does one inside 30 s of starting or of a peer coming back (the
	 * sweep has not re-sent what it lacks yet - outagetest), and one
	 * that has refused a replica copy for memory (at its ceiling it does
	 * not hold every record - ceilingreplicatest):
	 * pc_cluster_eager_miss_final().
	 * Spread is eager-flagged but holds K copies, so it keeps asking.
	 * A value over PC_MAX_FWD_VAL is not pushed - the sweep carries it
	 * over the bulk plane (S174) - so it reads as a miss on the other
	 * nodes until the next sweep (~10-20 s); accepted by the operator
	 * (2026-10-02), a write-time bulk push is a later task.
	 * Deletes still probe (RV-11): a delete that missed a record in its
	 * push window would be LOST, which a read cannot do. */
	if (pc_store_eager_enabled(ht) && !pc_cluster_replicas() &&
	        pc_cluster_eager_miss_final())
		return PC_OP_ABSENT;
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
	unsigned char fl = 0;
	int rc = pcache_ht_fetch_buf_fl(ht, k, buf, cap, vlen, &needed, exp,
		&fl);

	if (rc == 0) {
		/* S280: a document is not a string (S314: nor any typed record) */
		if ((fl & PCACHE_F_TYPES) && pc_store_types_strict(ht)) {
			*vlen = 0;
			return PC_OP_ERR_TYPE;
		}
		return PC_OP_OK;
	}
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
		size_t vlen, unsigned int exp, unsigned long long ver,
		unsigned char type)
{
	const char *cn;

	if (!pc_cluster_enabled() || !pc_store_eager_enabled(ht))
		return;
	cn = col_name_of(ht);
	/* S127: a 0 means this node is NOT one of the record's K holders.
	 * It accepted the write and forwarded it to the set - "any member
	 * may write" is about ADMISSION, not placement - and it must not keep
	 * a long-lived copy, or spread quietly becomes K+1 for every writing
	 * node.
	 *
	 * S247: BUT NOT BEFORE A HOLDER HAS IT.  The push is fire-and-forget
	 * and goes only to holders this node counts as live; the copy used to
	 * be removed right here, so a write acknowledged during a cut existed
	 * nowhere after the heal (35 of them, measured).  S253: the copy is
	 * dropped by the holder's ACK of the push group it travelled in
	 * (clwork.c pc_repl_push, cluster.c M_MIGRATE_ACK) - nothing to do
	 * here, whatever pc_repl_push says. */
	(void)pc_repl_push(cn, strlen(cn), k->s, (size_t)k->len, val, vlen,
		exp, ver, (unsigned char)(type & PCACHE_F_TYPES));  /* S314: every type */
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
	unsigned long long ver = 0, tver;
	int was_passive = 0;

	if (pcache_ht_touch_adopt(ht, k, exp, &was_passive) != 1)
		return 0;
	/* The touch's OWN version: stamped under the bucket lock and left
	 * in pcache_last_ver by this thread.  The fetch below is a second,
	 * unlocked read, and a SET from another worker can land between the
	 * two; the touch record then carried THAT write's version with the
	 * touch's expiry, and on replay - by version, S240 - it met an older
	 * record first, gave it the SET's version, and the SET itself was
	 * refused as older than what it met: an acknowledged write replaced
	 * by an earlier one (walordertest's expirer round, 5 of 20,000 on a
	 * loaded runner, 0.4.6).  The push below still pairs bytes, expiry
	 * and version out of the one read, as S213 wants. */
	tver = pcache_last_ver;
	if (pcache_ht_fetch_full(ht, k, &val, &e, &fl, &ver) != 0)
		return 1;                      /* re-armed, then gone */
	if (was_passive)
		pc_wal_upsert_fl(cn, k->s, k->len, val.s, val.len, e, ver, fl);
	else
		pc_wal_touch(cn, k->s, k->len, exp, tver);
	eager_push(ht, k, val.s, (size_t)val.len, e, ver, fl);
	/* S254: the fetch hands back a copy the caller owns.  This line was
	 * missing from S213 on - every EXPIRE that hit leaked the value
	 * (expireleaktest: 203 MB for 200,000 re-arms of 1 KB), and at 12
	 * workers glibc growing the thread heaps for it serialised them on
	 * the mm lock in mprotect: 58% of the CPU. */
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

/* as store_remove, with the delete's version stamped under the live
 * table's bucket lock - for a remove a tombstone follows (S234) */
static int store_remove_ver(pcache_htable_t *ht, const str *k,
		unsigned long long *ver)
{
	pcache_htable_t *sh = pc_store_resize_shadow(ht);
	int rc;

	if (sh)
		(void)pcache_ht_remove(sh, k);
	rc = pcache_ht_remove_ver(ht, k, ver);
	if (rc == 1)
		pc_ev_remove(ht, k);                              /* S153 */
	return rc;
}

static int op_del(pcache_htable_t *ht, str *k, unsigned int *park)
{
	const char *cn;
	unsigned long long dver = 0;
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
		/* S249: unknown holder - ask the fleet, as expire does and
		 * as RV-11 made store/eager/spread do.  This was a tombstone
		 * broadcast under THIS node's clock, which is below the
		 * holder's record version whenever this node never saw the
		 * write (a proxy ingress holds no copies), so the holder
		 * refused it as older: the key lived on, and the client read
		 * {"deleted": false} as "there was no such key".  Measured
		 * (resizefleettest, then by hand): a delete through a node
		 * that had not read the key first deleted nothing.  A positive
		 * answer becomes the forward - the holder deletes and
		 * tombstones under its own clock; all-negative is absent. */
		{
			unsigned int req = pc_probe_fwd_begin(1, cn, strlen(cn),
				k->s, (size_t)k->len, NULL, 0, 0, 0);

			if (req) {
				*park = req;
				return PC_OP_PARKED;
			}
		}
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
			unsigned kslot = pc_key_slot(k->s, (size_t)k->len);
			int h = pc_spread_pick_peer(kslot, pc_cluster_replicas());
			int tries;

			/* S247: the best-ranked holder can be one the send
			 * fails to (a peer cut off, not yet declared gone):
			 * try the next live holder.  Answering "absent" after
			 * one failed send told the client there was no such
			 * key while another holder still had it - measured in
			 * spreadparttest, the delete of a118 went to the
			 * unreachable node only, and the key outlived it. */
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
	rc = store_remove_ver(ht, k, &dver);
	if (rc == 1) {
		cn = col_name_of(ht);
		pc_wal_del(cn, k->s, k->len);
		if (pc_cluster_enabled())
			pc_tombstone_send_ver(cn, strlen(cn), k->s,
				(size_t)k->len, dver);
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
		unsigned char fl = 0;

		/* S280: INCR on a document is WRONGTYPE, not "not an integer" */
		if (pc_store_types_strict(ht) &&
		        pcache_ht_getflags(ht, k, &fl) == 0 &&
		        (fl & PCACHE_F_TYPES))
			return PC_OP_ERR_TYPE;
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
				pcache_last_ver, 0);
		}
	}
	return PC_OP_OK;
}

static int set_local(pcache_htable_t *ht, const str *k, const str *v,
		long long ttl);

static int op_set(pcache_htable_t *ht, str *k, str *v, long long ttl,
		unsigned int *park, int probed)
{
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
	return set_local(ht, k, v, ttl);
}

/* the write a SET makes on the node that stores it: the table, the WAL,
 * the event, the eager push - shared by op_set and S279's conditional
 * set, so the two cannot drift */
static int set_local(pcache_htable_t *ht, const str *k, const str *v,
		long long ttl)
{
	int rc = pcache_ht_store(ht, k, v, ttl_to_abs(ttl));

	if (rc == 0) {
		const char *cn = col_name_of(ht);

		pc_wal_upsert(cn, k->s, k->len, v->s, v->len, ttl_to_abs(ttl),
			pcache_last_ver);
		pc_ev_store(ht, k, (unsigned int)v->len, ttl);      /* S153 */
		eager_push(ht, k, v->s, (size_t)v->len, ttl_to_abs(ttl),
			pcache_last_ver, 0);
		if (pc_cluster_enabled())
			pc_neg_clear(cn, strlen(cn), k->s, (size_t)k->len);
		pc_store_note_set(ht, (size_t)k->len, (size_t)v->len);   /* S67 */
		return PC_OP_OK;
	}
	return rc == -2 ? PC_OP_ERR_FULL : PC_OP_ERR_2BIG;
}

/*
 * S279: SET NX / SET XX.
 *
 * The condition and the store happen under the key's stripe - the one
 * JSON's read-modify-write already serialises on - so two conditional
 * sets of one key on one node cannot both see it absent.  A PLAIN set
 * does not take the stripe and can land between the check and the
 * store; the idiom this serves (a lock: SET k token EX n NX, released by
 * DEL or expiry) never mixes the two on one key.
 *
 * Which node decides: every copy-keeping mode sends the key to ONE node,
 * the slot's owner (pc_shard_owner - map first, rendezvous over live
 * peers as the fallback), so every NX for a key is decided in one table.
 * In shard that is simply the owner.  In eager and spread every node
 * holds the key and each would otherwise decide alone: two clients on
 * two nodes both got OK within one replication window - worse than
 * refusing.  The decider stores and pushes like any authored write.
 * What stays open: a decider that dies with a grant not yet pushed, and
 * a new owner that has not received it, can grant it again - as a Redis
 * replica promoted before replication can.
 *
 * Proxy and pull-only clusters have no such node (a key lives wherever
 * it was placed or written): refused with PC_OP_ERR_MODE.
 */
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
	if (writes_denied())
		return PC_OP_ERR_WRFAIL;
	k.s = (char *)key;
	k.len = (int)klen;
	v.s = (char *)val;
	v.len = (int)vlen;
	return setcond_local(ht, &k, &v, ttl, cond);
}

static const char setcond_mode_msg[] = "SET NX/XX needs a shard or eager "
	"collection in a cluster: this collection has no node that decides it";

/* PC_OP_OK stored, PC_OP_ABSENT the condition did not hold, PARKED
 * forwarded to the deciding node, else PC_OP_ERR_* */
static int op_setcond(pcache_htable_t *ht, str *k, str *v, long long ttl,
		int cond, unsigned int *park)
{
	int rc;

	if (writes_denied())
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
				(size_t)v->len, ttl > 0 ? (unsigned int)ttl : 0, 0);
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

/*
 * S297: compare-and-set and compare-and-delete, in Redis 8.4's spelling -
 * SET key value IFEQ cmp / IFNE cmp, DELEX key IFEQ cmp / IFNE cmp (both
 * probed against redis 8.10.2, 2026-10-05).  A token lock needs them: its
 * refresh and release are "only if the value is still MY token", and as
 * GET-then-DEL that races - a lock that expired between the two and was
 * taken by another client is deleted by its old holder.
 *
 *   SET IFEQ   stores when the key holds a string equal to cmp
 *   SET IFNE   stores when the key is absent or holds a different string
 *   DELEX IFEQ deletes when the key holds a string equal to cmp
 *   DELEX IFNE deletes when the key holds a different string
 *
 * A key holding a hash, a document or a rate-limit record is a type
 * error (SET: WRONGTYPE; DELEX: Redis's "Key should be of string type if
 * conditions are specified").  A counter compares as its decimal text, as
 * GET reads it.  Decided where SET NX is - under the key's stripe, on the
 * key's deciding node (op_setcond's rule and its comment) - and stored or
 * deleted there like any authored write: WAL, push, tombstones.
 */
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
	if (writes_denied())
		return PC_OP_ERR_WRFAIL;
	if (kind < PC_CMP_SET_IFEQ || kind > PC_CMP_DEL_IFNE)
		return PC_OP_ERR_GET;
	k.s = (char *)key;
	k.len = (int)klen;
	v.s = (char *)val;
	v.len = (int)vlen;
	return cmp_local(ht, &k, cmp, cmplen, &v, ttl, kind);
}

static const char cmp_mode_msg[] = "IFEQ/IFNE need a shard or eager "
	"collection in a cluster: this collection has no node that decides it";
/* DELEX with a condition on a key that is not a string - Redis's words */
static const char delex_type_msg[] = "Key should be of string type if "
	"conditions are specified";

/* PC_OP_OK applied, PC_OP_ABSENT the condition did not hold, PARKED
 * forwarded to the deciding node, PC_OP_ERR_TYPE, else PC_OP_ERR_* */
static int op_cmp(pcache_htable_t *ht, str *k, const char *cmp,
		size_t cmplen, str *v, long long ttl, int kind,
		unsigned int *park)
{
	str none = { NULL, 0 };
	int rc;

	if (!v)
		v = &none;
	if (writes_denied())
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
			/* the edit scratch: pc_fwd_begin builds its datagram
			 * from it before returning (sized by the assert
			 * beside hr_buf) */
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
				need, ttl > 0 ? (unsigned int)ttl : 0, 0);
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
	if (sc->emitted >= sc->limit) {
		sc->stopped = 1;
		return -1;
	}
	sc->emitted++;
	pc_tw_bulk(sc->w, key->s, (size_t)key->len);
	return 0;
}

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
/* ---- S314: an atomic sliding-window rate-limit record ------------------
 * What a Redis client does as MULTI { ZADD k now now; ZREMRANGEBYSCORE k
 * -inf now-window; ZCOUNT k -inf +inf; EXPIRE k window } (DESIGN 12aa: the
 * SBC/RGS limiter, ~12.9M ZADD a day) in ONE command and one round trip:
 * record this hit, drop what fell out of the window, answer the count and
 * whether it is within the limit.  Every hit is recorded, an over-limit one
 * too, as the ZADD-first idiom does.
 *
 * The record: [ver 1][0][0][0][n u32 LE][n x ts u64 LE], hit times in ms,
 * oldest first, type PCACHE_F_RL.  Edited under the key's stripe - the
 * JSON read-modify-write's lock - on the key's DECIDING node, as SET NX is
 * (op_rlhit), so every hit on a key is counted in one table.  Times come
 * from that node's wall clock and are kept monotonic per key (a clock
 * stepped back never reorders the array).  At PC_RL_MAX hits the oldest
 * goes - the count then stays at the cap, so a limit must be below it. */
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

/* S314: the gate's refusal, with the member that holds it back */
static char rl_gate_msg[160];

static const char *rl_gate_text(int lacking)
{
	snprintf(rl_gate_msg, sizeof rl_gate_msg, "RL.HIT needs every node "
		"in the fleet on 0.4.4 or later - node %d does not carry the "
		"rate-limit type yet", lacking);
	return rl_gate_msg;
}

static const char rl_mode_msg[] = "RL.HIT needs a shard or eager "
	"collection in a cluster: this collection has no node that decides it";

/* PC_OP_OK (*count, *allowed), PARKED forwarded to the deciding node, or
 * PC_OP_ERR_*.  Which node decides: SET NX's rule (op_setcond) - the
 * slot's owner in every copy-keeping mode, so every hit on a key lands in
 * one table; proxy and pull-only clusters have no such node. */
static int op_rlhit(pcache_htable_t *ht, str *k, long long window_ms,
		long long limit, long long *count, int *allowed,
		unsigned int *park, int *lacking)
{
	const char *emsg = NULL;

	if (writes_denied())
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

/* ---- S170a: Symfony's lock scripts, run natively ----------------------
 * Symfony's Lock component (RedisStore) keeps a lock in a sorted set: the
 * members are the holders' tokens and "__write__", each scored with its
 * expiry in ms, and it drives the set with five fixed Lua scripts (six
 * with the read lock) - the only scripting any client of ours sends
 * (DESIGN 12hp).  They are approved here by the SHA1 of their bodies
 * (identical in every Symfony line 5.4-7.4, DESIGN 12il) and run as C,
 * step for step after the Lua: no interpreter, no client code.
 *
 * The record (type PCACHE_F_LOCK): [ver 1][0][0][0][n u32 LE] then n x
 * [exp_ms u64 LE][len u16 LE][member].  Edited under the key's stripe on
 * the key's deciding node, as RL.HIT's record is, and stored like any
 * authored write.  The key's expiry is the latest member's - in whole
 * seconds here, so a key can outlive its last member by under a second;
 * the scripts trim by the members' own ms expiries, so no lock outlives
 * its TTL. */
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

static const char slock_gate_fmt[] = "the Symfony lock scripts need every "
	"node in the fleet on 0.5.0 or later - node %d is older";
static const char slock_mode_msg[] = "the Symfony lock scripts need a shard "
	"or eager collection in a cluster: this collection has no node that "
	"decides a key";

/* PC_OP_OK with *result, PARKED forwarded to the deciding node,
 * PC_OP_ERR_TYPE, PC_OP_ERR_GATE (*lacking), else PC_OP_ERR_* */
static int op_slock(pcache_htable_t *ht, str *k, int script,
		long long now_arg, long long ttl_ms, const char *tok,
		size_t toklen, int *result, unsigned int *park, int *lacking)
{
	const char *emsg = NULL;

	*result = 0;
	if (script == PC_SLOCK_PROBE) {
		*result = 1;
		return PC_OP_OK;
	}
	if (writes_denied())
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

/* ---- S313: the hash commands -------------------------------------------
 * A hash is ONE record, typed PCACHE_F_HASH (src/hashrec.c holds the
 * format and the edits).  Every H command runs in hcmd_exec() on ONE
 * node: the key's deciding node for a write (SET NX's rule, op_setcond)
 * and for every command in shard mode, this node for an eager read
 * (every eager node holds the record).  So two writes on two nodes to
 * different fields of one hash both land - whole-value replication alone
 * would let the later one erase the other.  On that node a write holds
 * the key's stripe from its copy-out to its store; a read holds nothing
 * (S318: the lock-free fetch GET takes, see hcmd_exec).
 *
 * The reply is built once, dialect-neutral (a small typed tree - the
 * i b n a o e subset of src/ptree.h, which S317 made the native door's
 * whole codec), so a forwarded command needs no state on the asking
 * node: the decider runs the command and ships the tree back in the JSON
 * forward plane's ack, and the asker renders it as RESP or sends it as
 * the CMD reply, bytes unchanged. */

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

/* the empty hash deletes its key, as Redis does - and the delete takes the
 * delete's road (op_del): a shard owner's plain remove, or a tombstone so
 * every eager node drops it too */
static void hash_drop_local(pcache_htable_t *ht, const str *k)
{
	const char *cn = col_name_of(ht);
	unsigned long long dver = 0;

	if (pc_store_shard_enabled(ht) && pc_cluster_enabled()) {
		if (store_remove(ht, k) == 1)
			pc_wal_del(cn, k->s, k->len);
		return;
	}
	if (store_remove_ver(ht, k, &dver) == 1) {
		pc_wal_del(cn, k->s, k->len);
		if (pc_cluster_enabled())
			pc_tombstone_send_ver(cn, strlen(cn), k->s,
				(size_t)k->len, dver);
	}
}

/* The rebuilt record goes in through the store's own copy, and the WAL
 * append and the push read it from the scratch afterwards.  S318 weighed
 * building it in the destination cell instead (one copy fewer) and left
 * it: the version the WAL and the push must carry is stamped only as the
 * cell is published (S234, under the bucket lock), and once published
 * the cell is the table's - a SET, a DEL, the expiry sweep, a cluster
 * apply or the evacuator may replace it and free it before the append
 * reads a byte.  So the record has to exist outside the arena anyway,
 * and that is the scratch the store copies from. */
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

/* S319: render a READ command's reply over one hash record (@r of @n
 * bytes holding @cnt fields; NULL/0/0 = the key is absent).  Shared by
 * hcmd_exec (the record from this node's table) and the asking node of a
 * forwarded read (the record the owner sent) - so a reply is the same
 * whichever node answers it. */
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
		/* a positive count is distinct fields: a partial shuffle of
		 * every index, in the edit scratch (idle during a read, and
		 * sized for it - the assert beside hr_buf).  A negative count
		 * allows repeats, so each pick stands alone and needs no array. */
		m = want > 0 ? (want > cnt ? cnt : (unsigned int)want) :
			(unsigned int)(want < -1000000 ? 1000000 : -want);
		if (want > 0) {
			idx = (unsigned int *)hr_buf();
			if (!idx) {
				pc_tw_err(t, "ERR out of memory");
				break;
			}
			for (j = 0; j < cnt; j++)
				idx[j] = j;
			for (j = 0; j < m; j++) {
				unsigned int s = j + (unsigned int)(hc_rand() %
					(cnt - j)), tmp2 = idx[j];

				idx[j] = idx[s];
				idx[s] = tmp2;
			}
		}
		pc_tw_arr(t);
		for (j = 0; j < m; j++) {
			unsigned int q;

			pick = idx ? idx[j] : (unsigned int)(hc_rand() % cnt);
			off = 0;
			for (q = 0; q <= pick; q++)
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

/* Run one hash command on THIS node's table; the reply goes in @t.
 *
 * S318: a read takes the road GET takes - the seqlock copy-out into this
 * thread's get_buf(), no stripe, nothing allocated.  The optimistic
 * section retries a torn snapshot and sleeps on the bucket lock behind
 * a stalled writer, so what arrives here is a whole record or a miss,
 * exactly what the stripe used to guarantee; the flags come with it, so
 * WRONGTYPE is answered as before.  A write holds the key's stripe from
 * that same copy-out to its store: two writers on one key would
 * otherwise each edit the copy they took, and the second store erase the
 * first's field.  Its edits ping-pong between get_buf() (the copy-out)
 * and hr_buf(), so a write allocates nothing either - the reply writer's
 * own buffer is the one allocation left on the path. */
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
	/* VAL_MAX-sized, so a record of any type fits (TOOSMALL cannot
	 * happen) and a string where a hash was expected still reads whole
	 * enough to be refused by its flags */
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
		/* an absent key has no TTL.  On an expired miss the fetch
		 * reports the PAST expiry (S153: so a GET can say why), and a
		 * hash created over it must not inherit it - stored with that
		 * stamp it was expired-as-absent on its first read.  The JSON
		 * path resets it the same way (pc_json_rmw). */
		exp = 0;
	}
	if (hcmd_check_fields(ci, argc, argl, t) != 0)
		goto out;
	if (!hcmds[ci].write) {
		/* S319: the same rendering the asking node of a forwarded read
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

		/* which refusal: the increment, the field's value, or the
		 * result - pc_hr_incrbyfloat says "not a number" for all three.
		 * The probes write a one-field record into b, which the real
		 * edit then overwrites; r stays whole in a. */
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

static char hc_gate_msg[160];

static const char *hc_gate_text(int lacking)
{
	snprintf(hc_gate_msg, sizeof hc_gate_msg, "ERR hash commands need every "
		"node in the fleet on 0.4.4 or later - node %d does not carry "
		"the hash type yet", lacking);
	return hc_gate_msg;
}

/* PC_OP_OK (the reply in @t), PARKED (forwarded to the deciding node), or
 * PC_OP_ERR_* (GATE / MODE / WRFAIL / 2BIG / BUSY / FWD) */
/* S319: a forwarded read's encoded arguments, left for the door's park
 * entry to take - same thread, same call stack (the door parks right
 * after the verb returns PARKED); taking clears it */
static __thread unsigned char *hcmd_park_args_v;
static __thread size_t hcmd_park_argl_v;

static int op_hcmd(pcache_htable_t *ht, int ci, int argc,
		const char *const *argv, const size_t *argl, struct pc_tw *t,
		unsigned int *park, int *lacking)
{
	if (hcmds[ci].write && writes_denied())
		return PC_OP_ERR_WRFAIL;
	if (!pc_cluster_fleet_has(PC_FEAT_TYPES2, lacking))
		return PC_OP_ERR_GATE;
	{
		/* S319: one ceiling for the arguments wherever the command runs
		 * - the forward frame's - so a command does not work on the
		 * owner and fail through a forwarder */
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
		if (owner) {
			size_t need = 2, o = 2;
			unsigned char *enc;
			unsigned int req;
			int i, rd;

			for (i = 0; i < argc; i++)
				need += 4 + argl[i];
			/* S319: a READ asks the owner for the record and renders
			 * here - once every node speaks it (a rolling restart
			 * meets older owners, which run the old plane as before) */
			rd = !hcmds[ci].write &&
				pc_cluster_fleet_has(PC_FEAT_HREAD, NULL);
			/* S318: a write's encoding goes in the edit scratch, not
			 * a malloc - the forward plane builds its datagram from
			 * it before returning, and the ceiling checked above
			 * keeps it inside (the assert beside hr_buf).  A read's
			 * arguments outlive this call in the door's park entry,
			 * so they are heap (S319). */
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

/*
 * S283: the JSON read-modify-write (and the forward plane's hash ops that
 * share its entry) works in two per-thread buffers - the stored record and
 * the result - each VAL_MAX, taken on a thread's first call and kept for
 * its life.  It used to malloc a copy of the document and a VAL_MAX result
 * per request: three allocations per JSON command (counted on 0.4.6 with
 * test/allocshim.so), and 256 KB is above musl's mmap threshold, so on
 * musl every JSON command paid an mmap, page faults and an munmap - the
 * JSON collapse of DESIGN 12hd.  Every fragment pc_json_rmw hands back
 * (GET, PC_JOP_HCMD, PC_JOP_HCMD_READ) is BORROWED from these buffers:
 * valid until this thread's next pc_json_rmw, never freed by the caller.
 */
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
	/* S283: into this thread's result buffer, borrowed by the caller */
	pc_tw_init(&t, (unsigned char *)jr_out, VAL_MAX);
	hcmd_exec(ht, ci, argc, argv, argl, &t);
	*frag_out = jr_out;
	if (t.over) {
		/* larger than the buffer, so far past the forward ack's
		 * ceiling: say "too big" - the caller counts and refuses
		 * that by its length (fwd_reply_too_big), as it did when
		 * the tree was a heap buffer that grew */
		*fraglen_out = VAL_MAX + 1;
		return 0;
	}
	if (pc_tw_done(&t) != 0)
		return -1;
	*fraglen_out = (int)t.n;
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

	k.s = (char *)key;
	k.len = klen;
	if (op != PC_JOP_RLHIT && jr_scratch() != 0) {
		*errmsg = "out of memory";
		return -1;
	}

	/* S314: not a document op - the same stripe, the same forward plane,
	 * so a forwarded RL.HIT runs here on the deciding node unchanged */
	if (op == PC_JOP_HCMD) {                       /* S313 */
		(void)klen; (void)nx; (void)xx; (void)mkpath; (void)path;
		(void)plen; (void)by; (void)have_ttl; (void)ttl;
		if (hcmd_remote(ht, val, vlen, frag_out, fraglen_out) != 0) {
			*errmsg = "bad hash command";
			return -1;
		}
		return 0;
	}
	if (op == PC_JOP_HCMD_READ) {                  /* S319 */
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
		/* the record itself is the fragment - at most PC_HR_MAX bytes
		 * by its own invariant, so it always fits the ack; the asker
		 * renders HGETALL and the rest over it.  Borrowed (S283). */
		*frag_out = jr_doc;
		*fraglen_out = (int)vl;
		return 0;
	}
	if (op == PC_JOP_SLOCK) {                      /* S170a */
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
		/* S280: a string is not a document, whatever its bytes */
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
		*frag_out = jr_out;                    /* borrowed (S283) */
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
		/* S280: every JSON write - $ or a path, set, del, incr,
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
			rc2 = pc_jp_append(doc, (size_t)v.len, path,
				(size_t)plen, val, (size_t)vlen, count_out,
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
		/* S280: what a JSON write stores is a document */
		if (pcache_ht_store_ex(ht, &k, &nvs, newexp, PCACHE_F_JSON) == 0) {
			pc_ev_store(ht, &k, (unsigned int)nvs.len, 0);   /* S153 */
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

/* the {found:true, value, ttl} shape shared by get and mget */
static void write_hit(struct pc_tw *w, const char *v, size_t vn,
		unsigned int expires)
{
	long long ttl = expires ? (long long)expires - (long long)get_ticks()
		: -1;

	pc_tw_map(w);
	pc_tw_key(w, "found");
	pc_tw_bool(w, 1);
	pc_tw_key(w, "value");
	pc_tw_bulk(w, v, vn);
	pc_tw_key(w, "ttl");
	pc_tw_i64(w, ttl < -1 ? 0 : ttl);
	pc_tw_end(w);
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
	int st = pc_node_state();

	/* S229: HEALING refuses writes too - more of what it is still making
	 * durable - and serves reads, for the same reason FAILED does */
	return st == PC_NST_FAILED || st == PC_NST_HEALING;
}

#define PC_WRFAIL_MSG "node is FAILED (its WAL cannot honour the writes it "\
	"acknowledges) - reads still served here, send writes to another member"
#define PC_WRHEAL_MSG "node is HEALING (its WAL lost acknowledged writes and "\
	"a snapshot is making them durable) - reads still served here, send "\
	"writes to another member or retry shortly"

static const char *wr_refused_msg(void)
{
	return pc_node_state() == PC_NST_HEALING ? PC_WRHEAL_MSG : PC_WRFAIL_MSG;
}

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
static void unknown_tree(struct pc_tw *out)
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
	pc_tw_key(out, "unknown_commands");
	pc_tw_map(out);
	pc_tw_key(out, "members");
	pc_tw_i64(out, members);
	pc_tw_key(out, "reporting");
	pc_tw_i64(out, reporting);
	pc_tw_key(out, "other");
	pc_tw_i64(out, (long long)fl->other);
	pc_tw_key(out, "preauth");
	pc_tw_i64(out, (long long)fl->preauth);
	{
		/* peers' tables heard, and frames refused whole (clunk_parse) */
		unsigned long long rcv = 0, bad = 0;

		pc_cluster_unknown_figures(&rcv, &bad);
		pc_tw_key(out, "frames_received");
		pc_tw_i64(out, (long long)rcv);
		pc_tw_key(out, "frames_refused");
		pc_tw_i64(out, (long long)bad);
	}
	pc_tw_key(out, "rows");
	pc_tw_arr(out);
	for (i = 0; i < fl->n; i++) {
		const struct clunk_row *r = &fl->rows[i];

		pc_tw_map(out);
		pc_tw_key(out, "dialect");
		pc_tw_str(out, clunk_dialect_name(r->dialect));
		pc_tw_key(out, "name");
		pc_tw_bulk(out, r->name, r->nlen);
		pc_tw_key(out, "count");
		pc_tw_i64(out, (long long)r->count);
		pc_tw_key(out, "here");
		pc_tw_i64(out, (long long)r->here);
		pc_tw_key(out, "first_s");
		pc_tw_i64(out, (long long)r->first_s);
		pc_tw_key(out, "last_s");
		pc_tw_i64(out, (long long)r->last_s);
		pc_tw_key(out, "addr");
		pc_tw_bulk(out, r->addr, strlen(r->addr));
		pc_tw_key(out, "client");
		pc_tw_bulk(out, r->client, strlen(r->client));
		pc_tw_key(out, "node");
		pc_tw_i64(out, r->node);
		pc_tw_end(out);
	}
	pc_tw_end(out);
	pc_tw_end(out);
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
	s->fwd_reply_too_big -= b.fwd_reply_too_big;   /* S319 */
	s->tomb_sent -= b.tomb_sent;
	s->tomb_applied -= b.tomb_applied;
	s->neg_hits -= b.neg_hits;
	s->neg_displaced -= b.neg_displaced;                     /* RV-15 */
	s->tomb_displaced -= b.tomb_displaced;
	s->joins_rejected -= b.joins_rejected;
	s->reconciled -= b.reconciled;
	s->reconcile_probed -= b.reconcile_probed;
	s->repl_held_recovered -= b.repl_held_recovered;
	s->pull_held_recovered -= b.pull_held_recovered;
	s->reconcile_dig_asked -= b.reconcile_dig_asked;         /* S261 */
	s->reconcile_dig_diff -= b.reconcile_dig_diff;
	s->reconcile_dig_skipped -= b.reconcile_dig_skipped;
	s->reconcile_dig_fallback -= b.reconcile_dig_fallback;
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
	s->recv_nomem -= b.recv_nomem;
	s->recv_stale_dropped -= b.recv_stale_dropped;
	s->recv_below_dropped -= b.recv_below_dropped;
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

/* S37: the fleet as a tree, rendered from THIS node's vantage point.
 * Shared by the `members` verb and the status page's /members route
 * (which renders the same tree as JSON), so the two cannot drift - a
 * page disagreeing with the verb would be worse than no page. */
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
/* S258: the /proc readings, at most once a second PER WORKER.  Opening,
 * parsing and closing /proc/self/stat and /proc/self/statm was ~25% of a
 * stats call (~160 us, 6.6 KB of JSON).  The figures are for display -
 * CPU time and RSS on the page, gate.sh - so a reading up to a second old
 * is exact enough, and a thread-local copy needs no lock: a connection
 * stays on its worker, so the page's poll reuses the one it made. */
static __thread unsigned int proc_tick;
static __thread int proc_have;
static __thread unsigned long long proc_ut, proc_st, proc_rss;
static __thread long long proc_thr;

static void stats_process(struct pc_tw *out)
{
	long tck = sysconf(_SC_CLK_TCK), pg = sysconf(_SC_PAGESIZE);
	unsigned long long ut = 0, st = 0, rss_pages = 0;
	long long thr = 0;
	unsigned int now = get_ticks();
	FILE *f;
	char buf[512], *p;

	if (tck <= 0)
		tck = 100;
	if (pg <= 0)
		pg = 4096;
	if (proc_have && proc_tick == now) {
		ut = proc_ut;
		st = proc_st;
		rss_pages = proc_rss;
		thr = proc_thr;
		goto have;
	}

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
	proc_ut = ut;
	proc_st = st;
	proc_rss = rss_pages;
	proc_thr = thr;
	proc_tick = now;
	proc_have = 1;
have:
	pc_tw_key(out, "process");
	pc_tw_map(out);
	pc_tw_key(out, "allocator");               /* S282 */
	pc_tw_str(out, PC_ALLOC_NAME);
	pc_tw_key(out, "uptime_s");
	pc_tw_i64(out, (long long)pc_metrics_uptime());
	pc_tw_key(out, "pid");
	pc_tw_i64(out, (long long)getpid());
	pc_tw_key(out, "threads");
	pc_tw_i64(out, thr);
	pc_tw_key(out, "cpu_user_ms");
	pc_tw_i64(out, (long long)(ut * 1000ULL / (unsigned long long)tck));
	pc_tw_key(out, "cpu_sys_ms");
	pc_tw_i64(out, (long long)(st * 1000ULL / (unsigned long long)tck));
	pc_tw_key(out, "rss_bytes");
	pc_tw_i64(out, (long long)(rss_pages * (unsigned long long)pg));
	pc_tw_end(out);
	/* S123: what the running totals below count from - the start, or
	 * the last reset */
	{
		long long ra;

		pthread_mutex_lock(&reset_mx);
		ra = reset_at_s;
		pthread_mutex_unlock(&reset_mx);
		pc_tw_key(out, "since");
		pc_tw_map(out);
		pc_tw_key(out, "reset_at");
		pc_tw_i64(out, ra);
		pc_tw_key(out, "s");
		pc_tw_i64(out, ra ? (long long)time(NULL) - ra
			: (long long)pc_metrics_uptime());
		pc_tw_end(out);
	}
	/* S89: the doors this daemon is serving, from the config it runs
	 * with.  The startup log says this once and is gone; "which ports is
	 * this thing actually serving" is a stats question a week later. */
	pc_tw_key(out, "listeners");
	pc_tw_arr(out);
	{
		int i, n = pc_listener_count();

		for (i = 0; i < n; i++) {
			const struct pc_listener *l = pc_listener_at(i);
			int plain = pc_listener_plaintext(l);
			int secret = l->http ? pc_http_token() != NULL
				: l->resp ? pc_resp_password_set() : !plain;

			pc_tw_map(out);
			pc_tw_key(out, "kind");
			pc_tw_str(out, l->type == PC_LISTEN_UNIX ? "unix"
				: l->http ? "http" : l->resp ? "resp" : "native");
			pc_tw_key(out, "addr");
			pc_tw_bulk(out, l->addr, strlen(l->addr));
			pc_tw_key(out, "port");
			pc_tw_i64(out, l->port);
			pc_tw_key(out, "plaintext");
			pc_tw_bool(out, (plain));
			pc_tw_key(out, "allow");
			pc_tw_i64(out, pc_listener_allow(l));
			pc_tw_key(out, "secret");
			pc_tw_bool(out, (secret));
			pc_tw_end(out);
		}
	}
	pc_tw_end(out);
}

void pc_stats_tree(struct pc_tw *out, const char *only_col)
{
	int i;

	/* B1: the node's lifecycle state is a property of the NODE,
	 * not of the cluster - a node with no cluster still has one,
	 * and a readiness gate would have to answer for it too.  It
	 * sat inside the cluster block first, where an unclustered
	 * node could not report it at all. */
	/* S250: which build answered - standalone has no cluster code */
#ifdef PC_EDITION_STANDALONE
#define PC_EDITION_NAME "standalone"
#else
#define PC_EDITION_NAME "clustered"
#endif
	pc_tw_map(out);
	pc_tw_key(out, "version");
	pc_tw_str(out, PC_VERSION);
	pc_tw_key(out, "rev");
	pc_tw_str(out, PC_BUILD_REV);
	pc_tw_key(out, "edition");
	pc_tw_str(out, PC_EDITION_NAME);
	pc_tw_key(out, "state");
	pc_tw_str(out, pc_node_state_name(pc_node_state()));
	pc_tw_key(out, "state_reason");                              /* S186 */
	pc_tw_str(out, pc_node_reason_text(pc_node_reason()));
	stats_process(out);
	pc_tw_key(out, "memory");
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
		pc_tw_map(out);
		pc_tw_key(out, "arena_capacity_valid");
		pc_tw_bool(out, (mact));
		pc_tw_key(out, "arena_total");
		if (mact)
			pc_tw_i64(out, (long long)mt);
		else
			pc_tw_nil(out);
		pc_tw_key(out, "arena_used");
		if (mact)
			pc_tw_i64(out, (long long)mu);
		else
			pc_tw_nil(out);
		pc_tw_key(out, "arena_free");
		if (mact)
			pc_tw_i64(out, (long long)mf);
		else
			pc_tw_nil(out);
		pc_tw_key(out, "arena_live");
		pc_tw_i64(out, (long long)pcache_arena_live_bytes());
		/* arena_total/used/free above are HUGE-PAGE figures -
		 * they pin at the reservation while the process keeps
		 * growing, which is how a 64 MB arena reached 348 MB
		 * of RSS unnoticed.  These two are the whole node:
		 * what it holds from the host, and the ceiling it is
		 * allowed to hold. */
		pc_tw_key(out, "arena_held");
		pc_tw_i64(out, (long long)pcache_arena_held_bytes());
		pc_tw_key(out, "arena_max");
		pc_tw_i64(out, (long long)pcache_arena_max_bytes);
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
			pc_tw_key(out, "tier");
			pc_tw_bulk(out, pr.tier, strlen(pr.tier));
			pc_tw_key(out, "headroom_pct");
			pc_tw_i64(out, hpct);
			pc_tw_key(out, "nomem");
			pc_tw_i64(out, (long long)pr.refused);
			/* S244: of nomem, the overwrites that fit the cell
			 * they replaced and landed anyway; and older copies
			 * dropped because the newer one could not land */
			pc_tw_key(out, "nomem_inplace");
			pc_tw_i64(out, (long long)__atomic_load_n(
				&pcache_nomem_inplace, __ATOMIC_RELAXED));
			pc_tw_key(out, "nomem_stale_dropped");
			pc_tw_i64(out, (long long)__atomic_load_n(
				&pcache_nomem_dropped, __ATOMIC_RELAXED));
			pc_tw_key(out, "pool_empty");
			pc_tw_i64(out, (long long)pr.pool_empty);
			/* S166: the groups committed ahead of the frontier, and
			 * the ones a carve had to commit on the write path */
			pc_tw_key(out, "commits_ahead");
			pc_tw_i64(out, (long long)pr.commits_ahead);
			pc_tw_key(out, "commits_inline");
			pc_tw_i64(out, (long long)pr.commits_inline);
			pc_tw_key(out, "commits_index");
			pc_tw_i64(out, (long long)pr.commits_index);
			/* S167: what is really locked, and whether a group
			 * could not be locked after a pinned start */
			pc_tw_key(out, "locked_bytes");
			pc_tw_i64(out, (long long)pr.locked_bytes);
			pc_tw_key(out, "pin_lost");
			pc_tw_i64(out, (long long)pr.pin_lost);
			/* S114: what held is made of - the index regions (never
			 * freed) and the free slots kept resident */
			pc_tw_key(out, "arena_regions");
			pc_tw_i64(out, (long long)pr.regions_bytes);
			pc_tw_key(out, "arena_warm_free");
			pc_tw_i64(out, (long long)pr.warm_free_bytes);
			/* S150 C: retired index slots, resident and punched, and
			 * how the carve has been served */
			pc_tw_key(out, "arena_regions_free_warm");
			pc_tw_i64(out, (long long)pr.regions_free_warm_bytes);
			pc_tw_key(out, "arena_regions_free_cold");
			pc_tw_i64(out, (long long)pr.regions_free_cold_bytes);
			pc_tw_key(out, "arena_regions_retired");
			pc_tw_i64(out, (long long)pr.regions_retired);
			pc_tw_key(out, "arena_region_reuse");
			pc_tw_i64(out, (long long)pr.region_reuse);
			/* S118: the chunks the size classes own and the shm pages'
			 * alignment slots - with the two above, held exactly */
			pc_tw_key(out, "arena_sparse_chunks");    /* S190 */
			pc_tw_i64(out, (long long)pr.sparse_chunks);
			pc_tw_key(out, "arena_evacuated");
			pc_tw_i64(out, (long long)pr.evacuated);
			pc_tw_key(out, "arena_class_chunks");
			pc_tw_i64(out, (long long)pr.class_chunk_bytes);
			pc_tw_key(out, "arena_page_slack");
			pc_tw_i64(out, (long long)pr.page_slack_bytes);
			pc_tw_key(out, "arena_committed");
			pc_tw_i64(out, (long long)pr.committed_bytes);
			pc_tw_key(out, "arena_reserved");
			pc_tw_i64(out, (long long)pr.reserved_bytes);
			pc_tw_key(out, "at_ceiling");
			pc_tw_bool(out, (pr.at_ceiling_since));
			pc_tw_key(out, "at_ceiling_since");
			pc_tw_i64(out, (long long)pr.at_ceiling_since);
			pc_tw_key(out, "reclaim");
			pc_tw_map(out);
			pc_tw_key(out, "retired");
			pc_tw_i64(out, (long long)pr.retired);
			pc_tw_key(out, "pages_freed");
			pc_tw_i64(out, (long long)pr.pages_freed);
			pc_tw_key(out, "released_bytes");
			pc_tw_i64(out, (long long)pr.released_bytes);
			/* S119: how much of that was the never-carved tail */
			pc_tw_key(out, "tail_released");
			pc_tw_i64(out, (long long)pr.tail_released_bytes);
			pc_tw_key(out, "cold_bytes");
			pc_tw_i64(out, (long long)pr.cold_bytes);
			pc_tw_key(out, "punch_calls");
			pc_tw_i64(out, (long long)pr.punch_calls);
			pc_tw_key(out, "punch_groups");
			pc_tw_i64(out, (long long)pr.punch_groups);
			pc_tw_key(out, "shrink_step_bytes");
			pc_tw_i64(out, (long long)pr.shrink_step);
			pc_tw_key(out, "flushes");
			pc_tw_i64(out, (long long)pr.flushes);
			pc_tw_key(out, "giveback_off");
			pc_tw_bool(out, (pr.giveback_off));
			pc_tw_end(out);
		}
		pc_tw_end(out);
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
		pc_tw_key(out, "budget");
		pc_tw_map(out);
		pc_tw_key(out, "index");
		pc_tw_i64(out, (long long)bi);
		pc_tw_key(out, "records");
		pc_tw_i64(out, (long long)br);
		pc_tw_key(out, "held");
		pc_tw_i64(out, (long long)pcache_arena_held_bytes());
		pc_tw_key(out, "ceiling");
		pc_tw_i64(out, (long long)pcache_arena_max_bytes);
		pc_tw_key(out, "reservation");
		pc_tw_i64(out, (long long)bt);
		pc_tw_end(out);
	}
	/* RESP listeners (S33) live OUTSIDE the cluster block: a
	 * RESP listener works on a standalone daemon, and burying
	 * its counters in "cluster" made them null exactly where a
	 * single-node operator would look for them. */
	/* S161: the client limit and what it refused; HTTP is outside it */
	pc_tw_key(out, "clients");
	pc_tw_map(out);
	pc_tw_key(out, "open");
	pc_tw_i64(out, (long long)PC_RESP_READ(pc_clients_open));
	pc_tw_key(out, "max");
	pc_tw_i64(out, pc_max_clients);
	pc_tw_key(out, "refused");
	pc_tw_i64(out, (long long)__atomic_load_n(&pc_clients_refused,
		__ATOMIC_RELAXED));
	pc_tw_end(out);
	{
		struct pc_pubsub_stats ps;   /* PS1/PS3 */

		pc_pubsub_stats(&ps);
		pc_tw_key(out, "pubsub");
		pc_tw_map(out);
		pc_tw_key(out, "channels");
		pc_tw_i64(out, ps.channels);
		pc_tw_key(out, "patterns");
		pc_tw_i64(out, ps.patterns);
		pc_tw_key(out, "subscribers");
		pc_tw_i64(out, ps.subscribers);
		pc_tw_key(out, "published");
		pc_tw_i64(out, (long long)ps.published);
		pc_tw_key(out, "delivered");
		pc_tw_i64(out, (long long)ps.delivered);
		pc_tw_key(out, "slow_kills");
		pc_tw_i64(out, (long long)ps.slow_kills);
		pc_tw_key(out, "relay_sent");
		pc_tw_i64(out, (long long)ps.relay_sent);
		pc_tw_key(out, "relay_recv");
		pc_tw_i64(out, (long long)ps.relay_recv);
		pc_tw_key(out, "relay_lost");
		pc_tw_i64(out, (long long)ps.relay_lost);
		pc_tw_key(out, "relay_dropped");
		pc_tw_i64(out, (long long)ps.relay_dropped);
		pc_tw_key(out, "alloc_failed");
		pc_tw_i64(out, (long long)ps.alloc_failed);
		pc_tw_key(out, "relay_duplicates");
		pc_tw_i64(out, (long long)ps.relay_duplicates);
		pc_tw_key(out, "queue_dropped");
		pc_tw_i64(out, (long long)ps.queue_dropped);
		pc_tw_key(out, "queue_bytes");
		pc_tw_i64(out, (long long)ps.queue_bytes);
		pc_tw_key(out, "publish_paused");
		pc_tw_i64(out, (long long)ps.publish_paused);
		{
			/* PS5: pushes over UDP */
			struct pc_udp_figures uf;

			pc_conn_udp_figures(&uf);
			pc_tw_key(out, "udp");
			pc_tw_map(out);
			pc_tw_key(out, "streams");
			pc_tw_i64(out, uf.streams);
			pc_tw_key(out, "probes");
			pc_tw_i64(out, (long long)uf.probes);
			pc_tw_key(out, "confirmed");
			pc_tw_i64(out, (long long)uf.confirmed);
			pc_tw_key(out, "expired");
			pc_tw_i64(out, (long long)uf.expired);
			pc_tw_key(out, "pushed");
			pc_tw_i64(out, (long long)uf.pushed);
			pc_tw_key(out, "oversized_to_tcp");
			pc_tw_i64(out, (long long)uf.oversized);
			pc_tw_key(out, "send_errors");
			pc_tw_i64(out, (long long)uf.send_errors);
			pc_tw_key(out, "acks");
			pc_tw_i64(out, (long long)uf.acks);
			pc_tw_key(out, "pruned_no_ack");
			pc_tw_i64(out, (long long)uf.pruned_no_ack);
			pc_tw_key(out, "pruned_no_progress");
			pc_tw_i64(out, (long long)uf.pruned_no_progress);
			pc_tw_end(out);
		}
		{
			/* PS11: the dedicated relay plane */
			int rport, rthreads, rdirect;
			unsigned long long sd, sc, rx;

			pc_cluster_pubsub_relay_figures(&rport, &rthreads, &rdirect,
				&sd, &sc, &rx);
			pc_tw_key(out, "relay_port");
			pc_tw_i64(out, rport);
			pc_tw_key(out, "relay_rx_threads");
			pc_tw_i64(out, rthreads);
			pc_tw_key(out, "relay_peers_direct");
			pc_tw_i64(out, rdirect);
			pc_tw_key(out, "relay_sent_direct");
			pc_tw_i64(out, (long long)sd);
			pc_tw_key(out, "relay_sent_cluster");
			pc_tw_i64(out, (long long)sc);
			pc_tw_key(out, "relay_rx_datagrams");
			pc_tw_i64(out, (long long)rx);
		}
		{
			/* PS12: relaying only what a peer wants */
			struct pc_psint_figures fi;

			pc_cluster_pubsub_interest_figures(&fi);
			if (fi.version) { pc_tw_key(out, "relay_mode"); pc_tw_str(out, "interested"); } else { pc_tw_key(out, "relay_mode"); pc_tw_str(out, "all"); }
			{
				/* hex: [epoch 4][rebuild 4][adds 8] read off
				 * directly, and no sign to lose */
				char hx[20];
				int hn = snprintf(hx, sizeof hx, "%016llx",
					(unsigned long long)fi.version);

				pc_tw_key(out, "interest_version");
				pc_tw_bulk(out, hx, (size_t)hn);
			}
			pc_tw_key(out, "relay_skipped");
			pc_tw_i64(out, (long long)fi.skipped);
			pc_tw_key(out, "relay_peers_filtered");
			pc_tw_i64(out, fi.peers_filtered);
			pc_tw_key(out, "relay_peers_broadcast");
			pc_tw_i64(out, fi.peers_broadcast);
			pc_tw_key(out, "interest_updates_sent");
			pc_tw_i64(out, (long long)fi.updates_sent);
			pc_tw_key(out, "interest_updates_received");
			pc_tw_i64(out, (long long)fi.updates_received);
			pc_tw_key(out, "interest_resyncs_sent");
			pc_tw_i64(out, (long long)fi.resyncs_sent);
			pc_tw_key(out, "interest_resyncs_received");
			pc_tw_i64(out, (long long)fi.resyncs_received);
			pc_tw_key(out, "interest_requests_sent");
			pc_tw_i64(out, (long long)fi.requests_sent);
		}
		pc_tw_key(out, "keyspace_events");
		pc_tw_i64(out, (long long)ps.keyspace);
		pc_tw_end(out);
	}
	/* S159: the command rows and the slow log, off the RESP door - what
	 * INFO commandstats / latencystats and SLOWLOG GET answer there */
	pc_tw_key(out, "commands");
	pc_obs_commands_tree(out);
	pc_tw_key(out, "slowlog");
	pc_obs_slowlog_tree(out, 32);
	unknown_tree(out);                     /* S165 */
	pc_tw_key(out, "resp");
	pc_tw_map(out);
	pc_tw_key(out, "conns");
	pc_tw_i64(out, PC_DOOR_SINCE(resp_conns));
	pc_tw_key(out, "open");
	pc_tw_i64(out, (long long)PC_RESP_READ(pc_resp_open));
	pc_tw_key(out, "rejected");
	pc_tw_i64(out, PC_DOOR_SINCE(resp_rejected));
	pc_tw_key(out, "authfail");
	pc_tw_i64(out, PC_DOOR_SINCE(resp_authfail));
	/* S129: failed `enable` attempts.  Beside authfail because it is
	 * the same question one layer up - somebody offering a secret that
	 * did not match - and an operator watching for a privilege secret
	 * being guessed looks here. */
	pc_tw_key(out, "enable_fails");
	pc_tw_i64(out, (long long)pc_verb_enable_fails());
	pc_tw_key(out, "requests");
	pc_tw_i64(out, PC_DOOR_SINCE(resp_reqs));
	pc_tw_key(out, "slots_hits");
	pc_tw_i64(out, PC_DOOR_SINCE(resp_slots_hits));
	pc_tw_key(out, "slots_builds");
	pc_tw_i64(out, PC_DOOR_SINCE(resp_slots_builds));
	/* S292: cooperative KEYS walks finished, and the turns they took -
	 * turns per walk is what a KEYS caller waits on under load */
	{
		unsigned long long kw, kt;

		pc_enum_figures(&kw, &kt);
		pc_tw_key(out, "keys_walks");
		pc_tw_i64(out, (long long)kw);
		pc_tw_key(out, "keys_turns");
		pc_tw_i64(out, (long long)kt);
	}
	pc_tw_end(out);
	/* S76: the native door, per dialect.  A connection is counted once
	 * when its first byte settles the dialect; every consumed request
	 * after that counts against it.  resp here is RESP spoken to the
	 * native port - the dedicated RESP door is the block above.  (The
	 * json block left with the JSON-RPC dialect, S317.) */
	pc_tw_key(out, "native");
	pc_tw_map(out);
	pc_tw_key(out, "conns");
	pc_tw_i64(out, PC_DOOR_SINCE(nat_conns));
	pc_tw_key(out, "binary");
	pc_tw_map(out);
	pc_tw_key(out, "conns");
	pc_tw_i64(out, PC_DOOR_SINCE(nat_bin_conns));
	pc_tw_key(out, "open");
	pc_tw_i64(out, (long long)PC_RESP_READ(pc_nat_bin_open));
	pc_tw_key(out, "requests");
	pc_tw_i64(out, PC_DOOR_SINCE(nat_bin_reqs));
	pc_tw_end(out);
	pc_tw_key(out, "resp");
	pc_tw_map(out);
	pc_tw_key(out, "conns");
	pc_tw_i64(out, PC_DOOR_SINCE(nat_resp_conns));
	pc_tw_key(out, "open");
	pc_tw_i64(out, (long long)PC_RESP_READ(pc_nat_resp_open));
	pc_tw_key(out, "requests");
	pc_tw_i64(out, PC_DOOR_SINCE(nat_resp_reqs));
	pc_tw_end(out);
	pc_tw_end(out);
	pc_tw_key(out, "collections");
	pc_tw_arr(out);
	for (i = 0; i < pc_store_count(); i++) {
		if (!pc_store_live(i))
			continue;   /* S69: a dropped collection */
		pcache_ht_totals_t tot;

		if (only_col && strcmp(only_col, pc_store_name(i)))
			continue;
		pcache_ht_totals(pc_store_ht(i), &tot);
		pc_tw_map(out);
		pc_tw_key(out, "name");
		pc_tw_bulk(out, pc_store_name(i), strlen(pc_store_name(i)));
		{
			/* S163: the name's hash, which is how the map's per-member
			 * collection figures are joined to this row */
			char hx[24];

			snprintf(hx, sizeof hx, "%016llx", (unsigned long long)
				clmemb_col_hash(pc_store_name(i)));
			pc_tw_key(out, "hash");
			pc_tw_str(out, hx);
		}
		/* the mode is the first thing anyone needs to know
		 * about a collection - and until now the only way to
		 * find it was to read the node's config file */
		/* eager is legal only alongside store, so it is not a
		 * property hanging off the mode - it IS the mode, and
		 * reporting "store" for an eager collection drops the
		 * half an operator most needs. */
		pc_tw_key(out, "mode");
		/* S127: spread SETS the eager flag, because it reuses eager's
		 * push machinery aimed at K holders - so a spread collection
		 * reported "eager" here and the dashboard, which derives its
		 * label from this, told an operator "all in eager mode" about
		 * a fleet that is not.  Ask the cluster first. */
		pc_tw_str(out, pc_cluster_replicas() ? "spread"
			: pc_store_shard_enabled(pc_store_ht(i))
			? "shard" : pc_store_proxy_enabled(pc_store_ht(i))
			? "proxy" : pc_store_eager_enabled(pc_store_ht(i))
			? "eager" : "store");
		pc_tw_key(out, "entries");
		pc_tw_i64(out, (long long)tot.entries);
		pc_tw_key(out, "buckets");
		pc_tw_i64(out, pcache_ht_nbuckets(pc_store_ht(i)));
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
		pc_tw_key(out, "slots");
		pc_tw_i64(out, PCACHE_SLOTS);
		pc_tw_key(out, "overflow");
		pc_tw_i64(out, pcache_ht_overflow(pc_store_ht(i)));
		/* S239: how it is sized, and what AUTO would size it to (log2
		 * buckets, 0 = leave it) - the page shows amber for a WARN
		 * collection auto would shrink, red for keys in the leg while
		 * growth is blocked */
		pc_tw_key(out, "autoscale");
		pc_tw_str(out, pc_autoscale_name(pc_store_autoscale(i)));
		pc_tw_key(out, "autoscale_target_log2");
		pc_tw_i64(out, pc_store_autoscale_target(i));
		/* S172: what the maintenance tick has put BACK into the
		 * table - a leg that stays flat while this climbs is a
		 * table still filling it faster than the drain empties it */
		pc_tw_key(out, "leg_drained");
		pc_tw_i64(out, (long long)pcache_ht_drained(pc_store_ht(i)));
		/* S175: what the last COMPLETE drain pass could not put back
		 * - their buckets are full, so only a wider table can take
		 * them - and the splits that observation has asked for.
		 * leg_stuck falling to nothing while leg_splits climbs is
		 * the signal doing its job; leg_stuck holding steady with
		 * leg_splits flat means the floor has been reached and the
		 * remainder is the load factor the operator chose. */
		pc_tw_key(out, "leg_stuck");
		pc_tw_i64(out, pcache_ht_leg_stuck(pc_store_ht(i)));
		pc_tw_key(out, "leg_splits");
		pc_tw_i64(out, (long long)pcache_ht_leg_splits(pc_store_ht(i)));
		pc_tw_key(out, "held_walk_us");
		pc_tw_i64(out, (long long)pc_store_held_walk_us(i));
		{
			int rs_t = 0;
			unsigned long long rs_m = 0;

			if (pc_store_resizing(i, &rs_t, &rs_m)) {   /* S69 */
				pc_tw_key(out, "resizing_to");
				pc_tw_i64(out, 1LL << rs_t);
				pc_tw_key(out, "resize_moved");
				pc_tw_i64(out, (long long)rs_m);
			}
		}
		pc_tw_key(out, "hits");
		pc_tw_i64(out, (long long)tot.hits);
		pc_tw_key(out, "misses");
		pc_tw_i64(out, (long long)tot.misses);
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

			pc_tw_key(out, "reach");
			pc_tw_i64(out, (long long)rk);
			pc_tw_key(out, "reach_window_s");
			pc_tw_i64(out, (long long)rw);
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
			pc_tw_key(out, "hits_client");
			pc_tw_i64(out, (long long)ct.hits);
			pc_tw_key(out, "misses_client");
			pc_tw_i64(out, (long long)ct.misses);
			pc_tw_key(out, "reach_client");
			pc_tw_i64(out, (long long)rc2);
			/* S164: a replicated apply, a kept pull and a recovery
			 * replay all store, so `stores` puts one eager client
			 * write on every member.  These are the writes clients
			 * made HERE - this node's share of the fleet figure. */
			pc_tw_key(out, "stores_client");
			pc_tw_i64(out, (long long)ct.stores);
			pc_tw_key(out, "removes_client");
			pc_tw_i64(out, (long long)ct.removes);
		}
		pc_tw_key(out, "stores");
		pc_tw_i64(out, (long long)tot.stores);
		pc_tw_key(out, "removes");
		pc_tw_i64(out, (long long)tot.removes);
		pc_tw_key(out, "expired");
		pc_tw_i64(out, (long long)tot.expired);
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
				pc_tw_key(out, "fleet");
				pc_tw_map(out);
				pc_tw_key(out, "basis");
				pc_tw_str(out, pc_fleet_basis_name(fl.basis));
				pc_tw_key(out, "members");
				pc_tw_i64(out, fl.members);
				pc_tw_key(out, "reporting");
				pc_tw_i64(out, fl.reporting);
				pc_tw_key(out, "entries");
				pc_tw_i64(out, (long long)fl.entries);
				pc_tw_key(out, "copies");
				pc_tw_i64(out, (long long)fl.copies);
				pc_tw_key(out, "expired");
				pc_tw_i64(out, (long long)fl.expired);
				pc_tw_key(out, "hits");
				pc_tw_i64(out, (long long)fl.hits);
				pc_tw_key(out, "misses");
				pc_tw_i64(out, (long long)fl.misses);
				pc_tw_key(out, "stores");
				pc_tw_i64(out, (long long)fl.stores);
				pc_tw_key(out, "removes");
				pc_tw_i64(out, (long long)fl.removes);
				pc_tw_end(out);
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

			pc_tw_key(out, "held_bytes");
			pc_tw_i64(out, (long long)hb);
			pc_tw_key(out, "held_age_s");
			pc_tw_i64(out, age == (unsigned int)-1 ? -1 : (long long)age);
		}
		/* S116: the value sizes HELD, from the same walk - the same on
		 * every member whatever path stored the record, unlike size_hist
		 * below, which counts this node's own client writes.  Zeros
		 * before the first walk; the page keys on held_age_s. */
		{
			unsigned long long hh[8] = { 0 };
			int c;

			(void)pc_store_held_hist(i, hh);
			pc_tw_key(out, "held_hist");
			pc_tw_arr(out);
			for (c = 0; c < 8; c++)
				pc_tw_i64(out, (long long)hh[c]);
			pc_tw_end(out);
		}
		/* S120: what this collection costs - its index regions, exact,
		 * and its records as the cells they occupy, from the walk */
		pc_tw_key(out, "index_bytes");
		pc_tw_i64(out, (long long)pc_store_index_bytes(i));
		pc_tw_key(out, "held_cells");
		pc_tw_i64(out, (long long)pc_store_held_cells(i));
		/* S67: the write stream's sizes - a mean the page turns into an
		 * estimate of memory held, and a log2 distribution */
		{
			unsigned long long sb = 0, sn = 0, sh[8] = { 0 };
			int c;

			pc_store_size_stats(i, &sb, &sn, sh);
			pc_tw_key(out, "stored_bytes");
			pc_tw_i64(out, (long long)sb);
			pc_tw_key(out, "stored_n");
			pc_tw_i64(out, (long long)sn);
			pc_tw_key(out, "size_hist");
			pc_tw_arr(out);
			for (c = 0; c < 8; c++)
				pc_tw_i64(out, (long long)sh[c]);
			pc_tw_end(out);
		}
		pc_tw_end(out);
	}
	pc_tw_end(out);
	pc_tw_key(out, "cluster");
	{
		struct pc_cl_stats cs;

		pc_cluster_get_stats(&cs);
		since_cl(&cs);
		if (!cs.enabled) {
			pc_tw_nil(out);
		} else {
			pc_tw_map(out);
			pc_tw_key(out, "node");
			pc_tw_i64(out, cs.node_id);
			/* what this node IS, as opposed to what it is
			 * currently called: identity survives its
			 * restarts, incarnation does not, and durable
			 * says whether the identity was persisted at
			 * all (it cannot be without a [wal] dir) */
			pc_tw_key(out, "identity");
			pc_tw_bulk(out, pc_cluster_identity(), strlen(pc_cluster_identity()));
			pc_tw_key(out, "incarnation");
			pc_tw_i64(out, (long long)
				pc_cluster_incarnation());
			pc_tw_key(out, "identity_durable");
			pc_tw_bool(out, (pc_cluster_identity_durable()));
			/* S80: where that identity lives, and whether that
			 * place survives a reboot */
			pc_tw_key(out, "state_dir");
			if (pc_cluster_state_dir())
				pc_tw_bulk(out, pc_cluster_state_dir(), strlen(pc_cluster_state_dir()));
			else
				pc_tw_nil(out);
			pc_tw_key(out, "state_on_tmpfs");
			pc_tw_bool(out, (pc_cluster_state_tmpfs()));
			/* the Lamport clock: comparable across the
			 * fleet without any clock being in sync */
			pc_tw_key(out, "lamport");
			pc_tw_i64(out, (long long)pc_lamport_now());
			pc_tw_key(out, "lamport_rejected");
			pc_tw_i64(out, (long long)pc_lamport_rejected);
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
			{                                                /* S302 */
				char cid[37];
				const char *cst;
				unsigned long long fs;

				pc_cluster_cid_info(cid, &cst, &fs);
				pc_tw_key(out, "cluster_id");
				pc_tw_bulk(out, cid, strlen(cid));
				pc_tw_key(out, "cluster_id_state");
				pc_tw_str(out, cst);
				pc_tw_key(out, "foreign_seen");
				pc_tw_i64(out, (long long)fs);
				{                                        /* S36d */
					char fid[16 * 37], fad[16][24];
					long long fago[16];
					unsigned long long ffr[16];
					int fn = pc_cluster_foreign_list(fid, 16, fad,
						fago, ffr, 16), fi;

					pc_tw_key(out, "self_late");     /* S36e1 */
				pc_tw_i64(out, (long long)pc_cluster_self_late());
				pc_tw_key(out, "election_holds");   /* S36e3 */
				pc_tw_i64(out, (long long)pc_cluster_mprobe_holds());
				pc_tw_key(out, "foreign_clusters");
				pc_tw_arr(out);
					for (fi = 0; fi < fn; fi++) {
						pc_tw_map(out);
						pc_tw_key(out, "id");
						pc_tw_bulk(out, fid + fi * 37, strlen(fid + fi * 37));
						pc_tw_key(out, "from");
						pc_tw_bulk(out, fad[fi], strlen(fad[fi]));
						pc_tw_key(out, "last_seen_s");
						pc_tw_i64(out, fago[fi] / 1000);
						pc_tw_key(out, "frames");
						pc_tw_i64(out, (long long)ffr[fi]);
						pc_tw_end(out);
					}
					pc_tw_end(out);
				}
			}
			/* S303: whether an eager miss here is final, or
			 * asks the fleet (not READY, or a copy was refused
			 * for memory since start) */
			pc_tw_key(out, "eager_miss_final");
			pc_tw_bool(out, (pc_cluster_eager_miss_final()));
			{                                                /* S36 */
				const char *dm;
				int sn, sr, kn;
				unsigned long long rf, hs, hr;

				pc_cluster_discovery_info(&dm, &sn, &sr, &kn, &rf,
					&hs, &hr);
				pc_tw_key(out, "discovery");
				pc_tw_map(out);
				pc_tw_key(out, "mode");
				pc_tw_str(out, dm);
				pc_tw_key(out, "seeds");
				pc_tw_i64(out, sn);
				pc_tw_key(out, "seeds_resolved");
				pc_tw_i64(out, sr);
				pc_tw_key(out, "known");
				pc_tw_i64(out, kn);
				pc_tw_key(out, "resolve_fail");
				pc_tw_i64(out, (long long)rf);
				pc_tw_key(out, "hints_sent");
				pc_tw_i64(out, (long long)hs);
				pc_tw_key(out, "hints_recv");
				pc_tw_i64(out, (long long)hr);
				pc_tw_end(out);
			}
			{                                                /* S36b */
				struct clane_figs lf;
				int md, lp;

				memset(&lf, 0, sizeof lf);
				pc_cluster_lane_info(&lf, &md, &lp);
				pc_tw_key(out, "lane");
				pc_tw_map(out);
				pc_tw_key(out, "max_datagram");
				pc_tw_i64(out, md);
				pc_tw_key(out, "port");
				pc_tw_i64(out, lp);
				pc_tw_key(out, "peers_up");
				pc_tw_i64(out, lf.peers_up);
				pc_tw_key(out, "inbound");
				pc_tw_i64(out, lf.inbound);
				pc_tw_key(out, "sent_frames");
				pc_tw_i64(out, (long long)lf.sent_frames);
				pc_tw_key(out, "sent_bytes");
				pc_tw_i64(out, (long long)lf.sent_bytes);
				pc_tw_key(out, "recv_frames");
				pc_tw_i64(out, (long long)lf.recv_frames);
				pc_tw_key(out, "recv_bytes");
				pc_tw_i64(out, (long long)lf.recv_bytes);
				pc_tw_key(out, "dropped_full");
				pc_tw_i64(out, (long long)lf.dropped_full);
				pc_tw_key(out, "dropped_stale");
				pc_tw_i64(out, (long long)lf.dropped_stale);
				pc_tw_key(out, "dropped_rx");
				pc_tw_i64(out, (long long)lf.dropped_rx);
				pc_tw_key(out, "connects");
				pc_tw_i64(out, (long long)lf.connects);
				pc_tw_key(out, "connect_fail");
				pc_tw_i64(out, (long long)lf.connect_fail);
				pc_tw_key(out, "refused");
				pc_tw_i64(out, (long long)lf.refused);
				pc_tw_key(out, "forced");            /* S36 e2 */
				pc_tw_i64(out, lf.forced);
				pc_tw_key(out, "udp_blocked_saves");
				pc_tw_i64(out, (long long)pc_cluster_lane_saves());
				pc_tw_end(out);
			}
			pc_tw_key(out, "multicast_ttl");      /* S291 */
			pc_tw_i64(out, (long long)pc_cluster_multicast_ttl());
			pc_tw_key(out, "map");
			pc_tw_map(out);
			pc_tw_key(out, "valid");
			pc_tw_bool(out, (cs.map_valid));
			pc_tw_key(out, "term");
			pc_tw_i64(out, (long long)cs.map_term);
			pc_tw_key(out, "seq");
			pc_tw_i64(out, (long long)cs.map_seq);
			pc_tw_key(out, "nodes");
			pc_tw_i64(out, (long long)cs.map_nodes);
			pc_tw_key(out, "master");
			pc_tw_i64(out, (long long)cs.map_master);
			pc_tw_key(out, "published");
			pc_tw_i64(out, (long long)cs.map_pub);
			pc_tw_key(out, "received");
			pc_tw_i64(out, (long long)cs.map_rx);
			pc_tw_key(out, "stale");
			pc_tw_i64(out, (long long)cs.map_stale);
			pc_tw_key(out, "refused");
			pc_tw_i64(out, (long long)cs.map_bad);
			pc_tw_key(out, "unadmitted");   /* S241 */
			pc_tw_i64(out, (long long)cs.map_unadmitted);
			/* usable = the map's placeable set matches
			 * what this node sees as live.  place_hrw
			 * still climbing on a settled fleet means it
			 * never caught up. */
			pc_tw_key(out, "usable");
			pc_tw_bool(out, (cs.map_usable));
			pc_tw_key(out, "place_map");
			pc_tw_i64(out, (long long)cs.place_map);
			pc_tw_key(out, "place_hrw");
			pc_tw_i64(out, (long long)cs.place_hrw);
			/* B4: keys dropped because the fleet no longer
			 * had them - deleted while this node was down
			 * and brought back by replay.  Not a map
			 * property, so not inside that object. */
			/* the designated standby, and how many
			 * identities this node remembers.  No standby
			 * means the cluster is one failure from
			 * losing its control plane. */
			pc_tw_key(out, "backup");
			pc_tw_i64(out, (long long)cs.backup_id);
			pc_tw_end(out);
			pc_tw_key(out, "sync");
			pc_tw_map(out);
			pc_tw_key(out, "held");
			pc_tw_bool(out, (cs.held_valid));
			pc_tw_key(out, "held_term");
			pc_tw_i64(out, (long long)cs.held_term);
			pc_tw_key(out, "held_seq");
			pc_tw_i64(out, (long long)cs.held_seq);
			pc_tw_key(out, "held_identities");
			pc_tw_i64(out, (long long)cs.held_hist_n);
			pc_tw_key(out, "sent");
			pc_tw_i64(out, (long long)cs.sync_sent);
			pc_tw_key(out, "received");
			pc_tw_i64(out, (long long)cs.sync_rx);
			pc_tw_key(out, "acked");
			pc_tw_i64(out, (long long)cs.sync_ack);
			pc_tw_key(out, "refused");
			pc_tw_i64(out, (long long)cs.sync_bad);
			pc_tw_key(out, "timeouts");
			pc_tw_i64(out, (long long)cs.sync_timeouts);
			pc_tw_end(out);
			pc_tw_key(out, "identities_seen");
			pc_tw_i64(out, (long long)cs.hist_n);
			pc_tw_key(out, "reconciled");
			pc_tw_i64(out, (long long)cs.reconciled);
			pc_tw_key(out, "reconcile_probed");
			pc_tw_i64(out, (long long)cs.reconcile_probed);
			pc_tw_key(out, "repl_held_recovered");   /* S245 */
			pc_tw_i64(out, (long long)cs.repl_held_recovered);
			pc_tw_key(out, "pull_held_recovered");
			pc_tw_i64(out, (long long)cs.pull_held_recovered);
			pc_tw_key(out, "reconcile_digests");
			pc_tw_map(out);
			pc_tw_key(out, "asked");  /* S261 */
			pc_tw_i64(out, (long long)cs.reconcile_dig_asked);
			pc_tw_key(out, "slots_differ");
			pc_tw_i64(out, (long long)cs.reconcile_dig_diff);
			pc_tw_key(out, "keys_skipped");
			pc_tw_i64(out, (long long)cs.reconcile_dig_skipped);
			pc_tw_key(out, "unanswered");
			pc_tw_i64(out, (long long)cs.reconcile_dig_fallback);
			pc_tw_end(out);
			/* S212: the clocks - durations of the last completed
			 * pass, not rates */
			pc_tw_key(out, "reconcile_ms");
			pc_tw_i64(out, (long long)cs.reconcile_ms);
			pc_tw_key(out, "reconcile_pending");
			pc_tw_i64(out, (long long)cs.reconcile_pending);
			pc_tw_key(out, "repl_sweep_ms");
			pc_tw_i64(out, (long long)cs.repl_sweep_ms);
			pc_tw_key(out, "repl_sweep_scanned");
			pc_tw_i64(out, (long long)cs.repl_sweep_scanned);
			pc_tw_key(out, "repl_sweep_sent");
			pc_tw_i64(out, (long long)cs.repl_sweep_sent);
			pc_tw_key(out, "repl_sweep_cycles");
			pc_tw_i64(out, (long long)cs.repl_sweep_cycles);
			pc_tw_key(out, "reconcile_deferred");
			pc_tw_i64(out, (long long)cs.reconcile_deferred);
			pc_tw_key(out, "reconcile_witness");   /* S223 */
			pc_tw_i64(out, cs.reconcile_witness);
			pc_tw_key(out, "reconcile_skipped");
			pc_tw_bool(out, (cs.reconcile_skipped));
			/* S274: would a restart of this node NOW be witnessed */
			pc_tw_key(out, "restart");
			pc_tw_map(out);
			pc_tw_key(out, "safe");
			pc_tw_bool(out, (cs.restart_safe));
			pc_tw_key(out, "witnesses");
			pc_tw_i64(out, cs.restart_witnesses);
			pc_tw_key(out, "safe_in_ms");
			pc_tw_i64(out, cs.restart_safe_in_ms);
			pc_tw_end(out);
			pc_tw_key(out, "term");
			pc_tw_i64(out, (long long)pc_term_current());
			pc_tw_key(out, "term_rejected");
			pc_tw_i64(out, (long long)pc_term_rejected);
			pc_tw_key(out, "eager");
			pc_tw_bool(out, (pc_cluster_eager()));
			pc_tw_key(out, "mode");
			pc_tw_str(out, pc_cluster_mode_name());
			pc_tw_key(out, "role");
			pc_tw_str(out, cs.role == 2 ? "master" :
				cs.role == 1 ? "member" : "joining");
			pc_tw_key(out, "master");
			pc_tw_i64(out, cs.master_id);
			pc_tw_key(out, "peers_up");
			pc_tw_i64(out, cs.peers_up);
			pc_tw_key(out, "replicas_short");      /* S157 */
			pc_tw_i64(out, cs.replicas_short);
			/* S218: counted since S26 and shown nowhere - a master
			 * change is the event an operator most wants dated */
			pc_tw_key(out, "elections");
			pc_tw_i64(out, (long long)cs.elections);
			pc_tw_key(out, "demotions");
			pc_tw_i64(out, (long long)cs.demotions);
			pc_tw_key(out, "wire_epoch");          /* S221 */
			pc_tw_i64(out, PC_CL_EPOCH);
			pc_tw_key(out, "epoch_refused");
			pc_tw_i64(out, (long long)cs.epoch_refused);
			pc_tw_key(out, "apply_threads");       /* S217 */
			pc_tw_i64(out, cs.apply_threads ? cs.apply_threads : 1);
			pc_tw_key(out, "apply_dispatched");
			pc_tw_i64(out, (long long)cs.apply_dispatched);
			pc_tw_key(out, "apply_blocks");
			pc_tw_i64(out, (long long)cs.apply_blocks);
			pc_tw_key(out, "apply_backlog");
			pc_tw_i64(out, (long long)cs.apply_backlog);
			if (cs.apply_threads > 1) {    /* per ring: the stall's post-mortem */
				int ri;

				pc_tw_key(out, "apply_rings");
				pc_tw_arr(out);
				for (ri = 0; ri < cs.apply_threads && ri < 16; ri++) {
					pc_tw_map(out);
					pc_tw_key(out, "backlog");
					pc_tw_i64(out, (long long)cs.apply_ring[ri].backlog);
					pc_tw_key(out, "busy_ms");
					pc_tw_i64(out, cs.apply_ring[ri].busy_ms);
					pc_tw_key(out, "wait_ms");
					pc_tw_i64(out, cs.apply_ring[ri].wait_ms);
					pc_tw_end(out);
				}
				pc_tw_end(out);
			}
			pc_tw_key(out, "stalled");             /* S216 */
			pc_tw_bool(out, (cs.stalled));
			pc_tw_key(out, "stalled_for_ms");
			pc_tw_i64(out, cs.stalled_for_ms);
			pc_tw_key(out, "apply_stalls");
			pc_tw_i64(out, (long long)cs.apply_stalls);
			pc_tw_key(out, "apply_stall_ms_max");
			pc_tw_i64(out, (long long)cs.apply_stall_ms_max);
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
				pc_tw_key(out, "identity_uuid");
				pc_tw_str(out, uu);
				pc_tw_key(out, "identity_ver");
				pc_tw_i64(out, uv);
			}
			/* every peer's identity, so a dashboard can show which
			 * machine holds each id */
			{
				struct pc_cl_peer_info pi[PC_CL_MAXPEER];
				int np = pc_cluster_peers(pi, PC_CL_MAXPEER), k;

				pc_tw_key(out, "peers");
				pc_tw_arr(out);
				for (k = 0; k < np; k++) {
					pc_tw_map(out);
					pc_tw_key(out, "node");
					pc_tw_i64(out, pi[k].node);
					pc_tw_key(out, "up");
					pc_tw_bool(out, (pi[k].up));
					pc_tw_key(out, "identity");
					pc_tw_str(out, pi[k].ident);
					pc_tw_key(out, "identity_ver");
					pc_tw_i64(out, pi[k].ident_ver);
					pc_tw_key(out, "free_mb");
					pc_tw_i64(out, pi[k].free_mb);
					pc_tw_end(out);
				}
				pc_tw_end(out);
			}
			pc_tw_key(out, "reserved_ids");
			pc_tw_i64(out, cs.reserved_ids);
			pc_tw_key(out, "pull_sent");
			pc_tw_i64(out, (long long)cs.pull_sent);
			pc_tw_key(out, "pull_hits");
			pc_tw_i64(out, (long long)cs.pull_hits);
			pc_tw_key(out, "pull_misses");
			pc_tw_i64(out, (long long)cs.pull_misses);
			pc_tw_key(out, "pull_timeouts");
			pc_tw_i64(out, (long long)cs.pull_timeouts);
			pc_tw_key(out, "pull_served");
			pc_tw_i64(out, (long long)cs.pull_served);
			pc_tw_key(out, "tomb_sent");
			pc_tw_i64(out, (long long)cs.tomb_sent);
			pc_tw_key(out, "tomb_applied");
			pc_tw_i64(out, (long long)cs.tomb_applied);
			pc_tw_key(out, "neg_hits");
			pc_tw_i64(out, (long long)cs.neg_hits);
			pc_tw_key(out, "neg_displaced");   /* RV-15 */
			pc_tw_i64(out, (long long)cs.neg_displaced);
			pc_tw_key(out, "tomb_displaced");
			pc_tw_i64(out, (long long)cs.tomb_displaced);
			/* S238: the retained deletes - how many, what they
			 * cost (outside the arena), the cap's early drops (a
			 * partition longer than what is remembered may
			 * resurrect) and what was replayed to returning peers */
			{
				struct clretain_stats rt;
				struct timespec ts;

				clock_gettime(CLOCK_MONOTONIC, &ts);
				clretain_get_stats(&rt, (unsigned int)ts.tv_sec);
				pc_tw_key(out, "tombstones_retained");
				pc_tw_map(out);
				pc_tw_key(out, "entries");
				pc_tw_i64(out, (long long)rt.entries);
				pc_tw_key(out, "bytes");
				pc_tw_i64(out, (long long)rt.bytes);
				pc_tw_key(out, "max");
				pc_tw_i64(out, rt.max);
				pc_tw_key(out, "retain_s");
				pc_tw_i64(out, rt.retain_s);
				pc_tw_key(out, "oldest_age_s");
				pc_tw_i64(out, rt.oldest_age_s);
				pc_tw_key(out, "dropped_early");
				pc_tw_i64(out, (long long)rt.dropped_early);
				pc_tw_key(out, "replayed");
				pc_tw_i64(out, (long long)pc_cluster_tomb_catchup_sent());
				pc_tw_end(out);
			}
			pc_tw_key(out, "bad_auth");
			pc_tw_i64(out, (long long)cs.bad_auth);
			{
				struct pc_proxy_stats px;

				pc_proxy_get_stats(&px);
				since_px(&px);
				pc_tw_key(out, "placed_local");
				pc_tw_i64(out, (long long)px.placed_local);
				pc_tw_key(out, "placed_remote");
				pc_tw_i64(out, (long long)px.placed_remote);
				pc_tw_key(out, "fwd_sent");
				pc_tw_i64(out, (long long)px.fwd_sent);
				pc_tw_key(out, "fwd_served");
				pc_tw_i64(out, (long long)px.fwd_served);
				{
					/* S295: proxy holder hints */
					unsigned long long hs = 0, hl = 0;

					pc_proto_hint_stats(&hs, &hl);
					/* not "hints_sent": discovery's
					 * peer-address hints have that name */
					pc_tw_key(out, "holder_hints_sent");
					pc_tw_i64(out, (long long)hs);
					pc_tw_key(out, "holder_hint_local");
					pc_tw_i64(out, (long long)hl);
				}
				pc_tw_key(out, "migrated_out");
				pc_tw_i64(out, (long long)px.migrated_out);
				pc_tw_key(out, "migrated_in");
				pc_tw_i64(out, (long long)px.migrated_in);
				pc_tw_key(out, "migrate_lost");
				pc_tw_i64(out, (long long)px.migrate_lost);
				pc_tw_key(out, "migrate_retx");
				pc_tw_i64(out, (long long)px.migrate_retx);
				/* S296: whether the rebalancer has anything to
				 * do, and why */
				{
					struct pc_rebalance rbs;

					pc_cluster_rebalance(&rbs);
					pc_tw_key(out, "rebalance");
					pc_tw_map(out);
					pc_tw_key(out, "state");
					pc_tw_str(out, rbs.settled ? "settled" : "leveling");
					pc_tw_key(out, "reason");
					pc_tw_bulk(out, rbs.reason, strlen(rbs.reason));
					pc_tw_key(out, "settled_since");
					pc_tw_i64(out, rbs.settled_since);
					pc_tw_key(out, "last_move");
					pc_tw_i64(out, rbs.last_move);
					pc_tw_key(out, "moved_records");
					pc_tw_i64(out, (long long)rbs.moved_records);
					pc_tw_key(out, "ticks_shed");
					pc_tw_i64(out, (long long)rbs.ticks_shed);
					pc_tw_end(out);
				}
				pc_tw_key(out, "migrate_dgrams");
				pc_tw_i64(out, (long long)px.migrate_dgrams);
				pc_tw_key(out, "repl_out");
			pc_tw_i64(out, (long long)px.repl_out);
			/* S127: what spread is doing, in two numbers an
			 * operator can act on.  not_held = writes accepted,
			 * forwarded and NOT kept, because placement leaves
			 * this node out of the set; repaired = times the
			 * holder set changed and a re-placement was armed
			 * (one per membership change, not per record). */
			pc_tw_key(out, "spread_not_held");
			pc_tw_i64(out, (long long)px.spread_not_held);
			pc_tw_key(out, "spread_repaired");
			pc_tw_i64(out, (long long)px.spread_repaired);
			pc_tw_key(out, "spread_reclaimed");
			pc_tw_i64(out, (long long)px.spread_reclaimed);
			pc_tw_key(out, "spread_possess_sent");
			pc_tw_i64(out, (long long)px.spread_possess_sent);
			pc_tw_key(out, "spread_possess_kept");
			pc_tw_i64(out, (long long)px.spread_possess_kept);
			pc_tw_key(out, "spread_ack_parked");  /* S253 */
			pc_tw_i64(out, (long long)px.spread_ack_parked);
			pc_tw_key(out, "spread_ack_kept");
			pc_tw_i64(out, (long long)px.spread_ack_kept);
			pc_tw_key(out, "spread_ack_dropped");
			pc_tw_i64(out, (long long)px.spread_ack_dropped);
			pc_tw_key(out, "spread_ack_short");
			pc_tw_i64(out, (long long)px.spread_ack_short);
			pc_tw_key(out, "spread_ack_lost");
			pc_tw_i64(out, (long long)px.spread_ack_lost);
			/* S260 */
			pc_tw_key(out, "sweep_gate");
			pc_tw_map(out);
			pc_tw_key(out, "asked");
			pc_tw_i64(out, (long long)px.sweep_gate_asked);
			pc_tw_key(out, "answered");
			pc_tw_i64(out, (long long)px.sweep_gate_answered);
			pc_tw_key(out, "unanswered");
			pc_tw_i64(out, (long long)px.sweep_gate_unanswered);
			pc_tw_key(out, "clean");
			pc_tw_i64(out, (long long)px.sweep_gate_clean);
			pc_tw_key(out, "idle");
			pc_tw_i64(out, (long long)px.sweep_gate_idle);
			pc_tw_key(out, "slots_asked");
			pc_tw_i64(out, (long long)px.sweep_gate_slots_asked);
			pc_tw_key(out, "slots_diff");
			pc_tw_i64(out, (long long)px.sweep_gate_slots_diff);
			pc_tw_key(out, "digested");
			pc_tw_i64(out, (long long)px.sweep_gate_digested);
			pc_tw_key(out, "walks");
			pc_tw_i64(out, (long long)px.sweep_gate_walks);
			pc_tw_end(out);
			pc_tw_key(out, "spread_gap_repaired");    /* S248 */
			pc_tw_i64(out, (long long)px.spread_gap_repaired);
			/* Two counters that were maintained and reset-baselined
			 * but NEVER emitted - found by statlint, not by anyone
			 * looking.  fwd_fails is a FAILURE counter: forwards
			 * that could not be sent have been invisible to every
			 * operator for as long as it has existed. */
			pc_tw_key(out, "fwd_fails");
			pc_tw_i64(out, (long long)px.fwd_fails);
			pc_tw_key(out, "migrate_skipped_big");
			pc_tw_i64(out, (long long)px.migrate_skipped_big);
			pc_tw_key(out, "repl_pushed");
			pc_tw_i64(out, (long long)px.repl_pushed);
			pc_tw_key(out, "repl_groups");   /* S105 */
			pc_tw_i64(out, (long long)px.repl_groups);
			pc_tw_key(out, "repl_skipped_dying");
			pc_tw_i64(out, (long long)px.repl_skipped_dying);
				/* A2: copies refused as not-newer.  A
				 * number that only climbs means a
				 * sender is looping on records nobody
				 * will take. */
				pc_tw_key(out, "recv_older");
				pc_tw_i64(out, (long long)px.recv_older);
				/* S209: a copy refused because a tombstone for
				 * the key carries a newer version - the set the
				 * delete superseded, arriving after it.  Each is a
				 * resurrection that did not happen. */
				pc_tw_key(out, "recv_tombstoned");
				pc_tw_i64(out, (long long)px.recv_tombstoned);
				/* S244: refused at the arena ceiling; stale
				 * copies dropped for them */
				pc_tw_key(out, "recv_nomem");
				pc_tw_i64(out, (long long)px.recv_nomem);
				pc_tw_key(out, "recv_stale_dropped");
				pc_tw_i64(out, (long long)px.recv_stale_dropped);
				pc_tw_key(out, "recv_below_dropped");
				pc_tw_i64(out, (long long)px.recv_below_dropped);
				/* S174: oversized copies ride the bulk
				 * plane; a batch whose outcome did not
				 * confirm holds this peer's sweep mark
				 * where it is, so the next cycle offers
				 * them again.  Climbing means the plane
				 * is failing, not that records are lost. */
				pc_tw_key(out, "repl_bulk_lost");
				pc_tw_i64(out, (long long)px.repl_bulk_lost);
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
				pc_tw_key(out, "rx_applied");
				pc_tw_i64(out, (long long)cs.rx_applied);
				pc_tw_key(out, "rx_applied_ps");
				pc_tw_i64(out, (long long)cs.rx_applied_ps);
				pc_tw_key(out, "rx_older_ps");
				pc_tw_i64(out, (long long)cs.rx_older_ps);
				pc_tw_key(out, "rx_drops");
				pc_tw_i64(out, (long long)cs.rx_drops);
				pc_tw_key(out, "rx_drops_ps");
				pc_tw_i64(out, (long long)cs.rx_drops_ps);
				pc_tw_key(out, "rx_queue");
				pc_tw_i64(out, (long long)cs.rx_queue);
				pc_tw_key(out, "rx_rcvbuf");
				pc_tw_i64(out, (long long)cs.rx_rcvbuf);
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
				pc_tw_key(out, "pend_peak");
				pc_tw_i64(out, (long long)cs.pend_peak);
				pc_tw_key(out, "pend_max");
				pc_tw_i64(out, (long long)cs.pend_max);
				pc_tw_key(out, "pend_used");
				pc_tw_i64(out, (long long)cs.pend_used);
				pc_tw_key(out, "pend_exhausted");
				pc_tw_i64(out, (long long)cs.pend_exhausted);
				/* the other two refusal causes, split:
				 * "forward failed" covering both is
				 * what sent an operator to look at a
				 * healthy network */
				pc_tw_key(out, "fwd_no_route");
				pc_tw_i64(out, (long long)cs.fwd_no_route);
				pc_tw_key(out, "fwd_send_fail");
				pc_tw_i64(out, (long long)cs.fwd_send_fail);
				pc_tw_key(out, "fwd_send_errno");
				pc_tw_i64(out, (long long)cs.fwd_send_errno);
				/* S319: replies this node, as an OWNER,
				 * could not ship - the ask was answered
				 * "holder rejected" and the log says why */
				pc_tw_key(out, "fwd_reply_too_big");
				pc_tw_i64(out, (long long)cs.fwd_reply_too_big);
				pc_tw_key(out, "rcvbuf");
				pc_tw_i64(out, (long long)pc_cluster_rcvbuf());
			pc_tw_key(out, "bulk_out");
				pc_tw_i64(out, (long long)px.bulk_out);
				pc_tw_key(out, "bulk_in");
				pc_tw_i64(out, (long long)px.bulk_in);
				/* S83: the bootstrap pull */
				pc_tw_key(out, "boot_out");
				pc_tw_i64(out, (long long)px.boot_out);
				pc_tw_key(out, "boot_in");
				pc_tw_i64(out, (long long)px.boot_in);
				pc_tw_key(out, "boot_failed");
				pc_tw_i64(out, (long long)px.boot_failed);
				pc_tw_key(out, "demotes_sent");
				pc_tw_i64(out, (long long)px.demotes_sent);
				pc_tw_key(out, "demotes_applied");
				pc_tw_i64(out, (long long)px.demotes_applied);
				pc_tw_key(out, "loc_hits");
				pc_tw_i64(out, (long long)px.loc_hits);
			}
			pc_tw_end(out);
		}
	}
	pc_tw_key(out, "rdb");
	{
		struct pc_rdb_stats rs;

		pc_rdb_get_stats(&rs);
		if (!rs.enabled) {
			pc_tw_nil(out);
		} else {
			pc_tw_map(out);
			pc_tw_key(out, "saves");
			pc_tw_i64(out, (long long)rs.saves);
			pc_tw_key(out, "running");
			pc_tw_bool(out, (rs.running));
			pc_tw_key(out, "last_bytes");
			pc_tw_i64(out, (long long)rs.last_bytes);
			pc_tw_key(out, "last_dur_ms");
			pc_tw_i64(out, rs.last_dur_ms);
			pc_tw_key(out, "last_marker");
			pc_tw_i64(out, (long long)rs.last_marker);
			pc_tw_key(out, "safe_marker");      /* S215 */
			pc_tw_i64(out, (long long)rs.safe_marker);
			pc_tw_key(out, "save_errors");
			pc_tw_i64(out, (long long)rs.save_errors);
			pc_tw_key(out, "last_unix");
			pc_tw_i64(out, rs.last_unix);
			pc_tw_key(out, "last_age_s");    /* RV-12 */
			pc_tw_i64(out, rs.last_age_s);
			pc_tw_end(out);
		}
	}
	pc_tw_key(out, "wal");
	{
		struct pc_wal_stats ws;

		pc_wal_get_stats(&ws);
		since_wal(&ws);
		if (!ws.enabled) {
			pc_tw_nil(out);
		} else {
			/* the storage identity resolved at startup: the
			 * class/chain belong NEXT TO the wal counters,
			 * not only in a boot log line */
			const struct pc_st_id *sid = pc_wal_identity();

			pc_tw_map(out);
			pc_tw_key(out, "fsync");
			pc_tw_str(out, ws.fsync_mode);
			if (sid) {
				pc_tw_key(out, "storage_class");
				pc_tw_bulk(out, pc_st_class_str(sid->cls), strlen(pc_st_class_str(sid->cls)));
				pc_tw_key(out, "fstype");
				pc_tw_bulk(out, sid->fstype, strlen(sid->fstype));
				pc_tw_key(out, "chain");
				pc_tw_bulk(out, sid->chain, strlen(sid->chain));
			}
			{
				/* the startup measurement policy
				 * followed: cached from the wal dir's
				 * .pc-walprobe unless re-probed */
				const struct pc_wal_probe *pr =
					pc_wal_probe_result();

				if (pr) {
					pc_tw_key(out, "probe");
					pc_tw_map(out);
					pc_tw_key(out, "cached");
					/* nothing is cached any more
					 * (DESIGN 12am); the key
					 * stays for consumers */
					pc_tw_bool(out, 0);
					pc_tw_key(out, "sync_bs");
					pc_tw_i64(out, PC_WPROBE_SYNC_BS);
					pc_tw_key(out, "qd");
					pc_tw_i64(out, PC_WPROBE_QD);
					pc_tw_key(out, "seq_bs");
					pc_tw_i64(out, PC_WPROBE_SEQ_BS);
					pc_tw_key(out, "seq_mb_s");
					pc_tw_i64(out, (long long)
						(pr->seq_mb_s + 0.5));
					pc_tw_key(out, "fsync_p50_us");
					pc_tw_i64(out, pr->fsync_p50_us);
					pc_tw_key(out, "fsync_p99_us");
					pc_tw_i64(out, pr->fsync_p99_us);
					pc_tw_key(out, "sync_iops");
					pc_tw_i64(out, pr->sync_iops);
					pc_tw_end(out);
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
					pc_tw_key(out, "observed");
					pc_tw_map(out);
					pc_tw_key(out, "fsync_n");
					pc_tw_i64(out, (long long)ob.fsync_n);
					pc_tw_key(out, "fsync_avg_us");
					pc_tw_i64(out, (long long)ob.avg_us);
					pc_tw_key(out, "fsync_recent_us");
					pc_tw_i64(out, (long long)ob.recent_us);
					pc_tw_key(out, "fsync_max_us");
					pc_tw_i64(out, (long long)ob.max_us);
					pc_tw_key(out, "probe_p50_us");
					pc_tw_i64(out, (long long)
						ob.probe_p50_us);
					pc_tw_key(out, "probe_underestimated");
					pc_tw_bool(out, (ob.probe_underestimated));
					pc_tw_end(out);
				}
			}
			pc_tw_key(out, "appended");
			pc_tw_i64(out, (long long)ws.appended);
			pc_tw_key(out, "bytes");
			pc_tw_i64(out, (long long)ws.bytes);
			pc_tw_key(out, "dropped");
			pc_tw_i64(out, (long long)ws.dropped);
			pc_tw_key(out, "late");
			pc_tw_i64(out, (long long)ws.late);
			pc_tw_key(out, "recycles");
			pc_tw_i64(out, (long long)ws.recycles);
			pc_tw_key(out, "storage_failed");   /* S215 */
			pc_tw_bool(out, (ws.failed));
			pc_tw_key(out, "buffer_bytes");     /* S265 */
			pc_tw_i64(out, (long long)pc_wal_buffer_bytes());
			pc_tw_key(out, "buffers_locked");
			pc_tw_bool(out, (pc_wal_locked_bytes(NULL)));
			pc_tw_key(out, "ctrl_errors");
			pc_tw_i64(out, (long long)ws.ctrl_errors);
			pc_tw_key(out, "overruns");
			pc_tw_i64(out, (long long)ws.overruns);
			pc_tw_key(out, "heals");          /* S229 */
			pc_tw_i64(out, (long long)ws.heals);
			if (ws.healing) { pc_tw_key(out, "healing"); pc_tw_bool(out, 1); } else { pc_tw_key(out, "healing"); pc_tw_bool(out, 0); }
			pc_tw_key(out, "last_heal_ms");
			pc_tw_i64(out, ws.last_heal_ms);
			/* S306: heals in the window, and whether that many
			 * means healing is routine (the log is undersized) */
			pc_tw_key(out, "heals_recent");
			pc_tw_i64(out, ws.heals_recent);
			if (ws.heal_routine) { pc_tw_key(out, "heal_routine"); pc_tw_bool(out, 1); } else { pc_tw_key(out, "heal_routine"); pc_tw_bool(out, 0); }
			pc_tw_key(out, "unhealed");
			pc_tw_i64(out, (long long)ws.unhealed);
			pc_tw_key(out, "free_segments");
			pc_tw_i64(out, ws.free_segments);
			{
				/* S224: tenths are plenty - each goes as a number's
				 * text ('d'), which the tree has for exactly this
				 * (RV-13: three figures now) */
				static const char *const fk[3] = { "fill_mb_s",
					"full_in_s", "ring_fill_s" };
				double fv[3];
				char fb[48];
				int fi, fl;

				fv[0] = ws.fill_mb_s;
				fv[1] = ws.full_in_s;
				fv[2] = ws.ring_fill_s;
				for (fi = 0; fi < 3; fi++) {
					fl = snprintf(fb, sizeof fb, "%.1f", fv[fi]);
					if (fl <= 0 || (size_t)fl >= sizeof fb)
						continue;
					pc_tw_key(out, fk[fi]);
					pc_tw_num(out, fb, (size_t)fl);
				}
			}
			pc_tw_key(out, "last_seq");
			pc_tw_i64(out, (long long)ws.last_seq);
			pc_tw_key(out, "synced_seq");
			pc_tw_i64(out, (long long)ws.synced_seq);
			/* S145: the two states one "unsynced" figure used to span */
			pc_tw_key(out, "appended_seq");
			pc_tw_i64(out, (long long)ws.pending_hi);
			pc_tw_key(out, "staged");
			pc_tw_i64(out, (long long)(ws.last_seq > ws.pending_hi ? ws.last_seq - ws.pending_hi : 0));
			pc_tw_key(out, "unsynced");
			pc_tw_i64(out, (long long)(ws.pending_hi > ws.synced_seq ? ws.pending_hi - ws.synced_seq : 0));
			pc_tw_end(out);
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
		pc_tw_key(out, "retire");
		pc_tw_map(out);
		pc_tw_key(out, "pending");
		pc_tw_i64(out, (long long)rp);
		pc_tw_key(out, "cleared");
		pc_tw_i64(out, (long long)rc);
		pc_tw_key(out, "returned_bytes");
		pc_tw_i64(out, (long long)rb);
		pc_tw_key(out, "keys_ended_on_swap");
		pc_tw_i64(out, (long long)pc_enum_ended_on_swap());
		pc_tw_key(out, "lines");
		pc_tw_i64(out, (long long)lines);
		pc_tw_key(out, "inside");
		pc_tw_i64(out, (long long)inside);
		pc_tw_end(out);
	}
	pc_tw_end(out);
}

/* S299: the member list is PC_CL_MAXMEMBERS entries of ~1 KB - half a
 * megabyte, which three request handlers used to put on their stacks
 * (68 KB each while the bound was 64).  One buffer per thread, made on
 * first use and never freed: only the entries a call fills are touched,
 * so a small fleet's RSS does not pay for the bound. */
static struct pc_member *members_scratch(void)
{
	static __thread struct pc_member *buf;

	if (!buf)
		buf = malloc(sizeof *buf * PC_CL_MAXMEMBERS);
	return buf;
}

void pc_members_tree(struct pc_tw *out)
{
	struct pc_member *mem = members_scratch();
	int n = mem ? pc_cluster_members(mem, PC_CL_MAXMEMBERS) : 0, i;

	pc_tw_map(out);
	pc_tw_key(out, "members");
	pc_tw_arr(out);
	for (i = 0; i < n; i++) {
		char ab[24];
		const char *as = pc_inet_ntop4(mem[i].addr, ab,
			sizeof ab);

		pc_tw_map(out);
		pc_tw_key(out, "addr");
		pc_tw_bulk(out, as, strlen(as));
		pc_tw_key(out, "port");
		pc_tw_i64(out, mem[i].client_port);
		pc_tw_key(out, "node");
		pc_tw_i64(out, mem[i].node);
		pc_tw_key(out, "self");
		pc_tw_bool(out, (mem[i].is_self));
		pc_tw_key(out, "master");
		pc_tw_bool(out, (mem[i].is_master));
		/* who takes over.  The map has named a standby since
		 * C1 and nothing showed it, so "which node is the
		 * backup" was a question only the master could
		 * answer. */
		pc_tw_key(out, "backup");
		pc_tw_bool(out, (mem[i].is_backup));
		pc_tw_key(out, "role");
		pc_tw_str(out, mem[i].is_master ? "master" :
			mem[i].is_backup ? "backup" : "member");
		/* B1: whether this member's data can be trusted yet -
		 * a separate question from whether it is the master */
		pc_tw_key(out, "gone_s");
		pc_tw_i64(out, mem[i].gone_s);
		pc_tw_key(out, "held_s");
		pc_tw_i64(out, mem[i].held_s);
		/* S293: and, for a member that has left, WHY - goodbye,
		 * silent (purged) or forgotten (refused) - and for a silent
		 * one how long it had been quiet */
		if (mem[i].gone_s >= 0) {
			pc_tw_key(out, "gone_reason");
			pc_tw_str(out, pc_cluster_gone_reason(mem[i].gone_why));
			pc_tw_key(out, "gone_silent_s");
			pc_tw_i64(out, mem[i].gone_silent_s);
		}
		pc_tw_key(out, "state");
		/* S192: a member that has LEFT reports no state of its own -
		 * it is not answering.  Say "gone" rather than borrow a
		 * lifecycle word that means something else. */
		pc_tw_str(out, mem[i].gone_s >= 0 ? "gone"
			: pc_node_state_name(mem[i].state));
		/* S186: and WHY, when there is a reason to give.  The card
		 * showed FAILED with nothing beside it, which sends an
		 * operator to the logs of a node that may not be the one
		 * they are looking at - this is every node's view of every
		 * other. */
		pc_tw_key(out, "reason");
		pc_tw_str(out, pc_node_reason_text(mem[i].reason));
		pc_tw_key(out, "mem_tier");
		pc_tw_str(out, mem_tier_token(mem[i].mem_tier));
		pc_tw_key(out, "free_mb");
		pc_tw_i64(out, mem[i].free_mb);
		pc_tw_key(out, "total_mb");
		pc_tw_i64(out, mem[i].total_mb);
		/* S78: gossiped, so the grid links to the node's OWN door and
		 * shows its uptime; -1 = a build before the fields */
		pc_tw_key(out, "http");
		pc_tw_i64(out, mem[i].http_port);
		pc_tw_key(out, "uptime_s");
		pc_tw_i64(out, mem[i].uptime_s);
		/* S103: this node's VIEW of the member - what the sender election
		 * decides from */
		pc_tw_key(out, "entries");
		pc_tw_i64(out, mem[i].entries);
		pc_tw_key(out, "start");
		pc_tw_str(out, mem[i].start ? mem[i].start : "unknown");
		/* S160: its open clients as gossiped, absent for a build before
		 * the field - the page sums what is present and says how many
		 * members reported (the json gauge left with the dialect, S317) */
		if (mem[i].cl_reported) {
			pc_tw_key(out, "clients");
			pc_tw_map(out);
			pc_tw_key(out, "open");
			pc_tw_i64(out, mem[i].cl_open);
			pc_tw_key(out, "resp");
			pc_tw_i64(out, mem[i].cl_resp);
			pc_tw_key(out, "binary");
			pc_tw_i64(out, mem[i].cl_bin);
			pc_tw_key(out, "native_resp");
			pc_tw_i64(out, mem[i].cl_nresp);
			pc_tw_end(out);
		}
		/* PS11: its relay port and whether relays to it use it */
		pc_tw_key(out, "relay");
		pc_tw_map(out);
		pc_tw_key(out, "port");
		pc_tw_i64(out, mem[i].relay_port);
		pc_tw_key(out, "direct");
		pc_tw_bool(out, mem[i].relay_direct);
		/* PS12: what relays to it are: all, filtered by its interest, or
		 * everything because our copy of its interest is not current */
		pc_tw_key(out, "interest");
		pc_tw_str(out, mem[i].relay_interest == 1 ? "filtered"
			: mem[i].relay_interest == 2 ? "broadcast" : "all");
		pc_tw_end(out);
		/* S163: its per-collection figures, joined to /stats by hash */
		if (mem[i].cols_reported) {
			int k;

			pc_tw_key(out, "collections_total");
			pc_tw_i64(out, mem[i].ncols_total);
			pc_tw_key(out, "collections");
			pc_tw_arr(out);
			for (k = 0; k < mem[i].ncols; k++) {
				char hx[24];
				const struct clmemb_col *cl = &mem[i].cols[k];
				unsigned char hb[8];
				int q;

				/* %016llx by hand (see pc_inet_ntop4) */
				for (q = 0; q < 8; q++)
					hb[q] = (unsigned char)(cl->hash >> (56 - 8 * q));
				hex_bytes(hx, hb, 8);
				pc_tw_map(out);
				pc_tw_key(out, "hash");
				pc_tw_bulk(out, hx, 16);
				pc_tw_key(out, "entries");
				pc_tw_i64(out, (long long)cl->entries);
				pc_tw_key(out, "hits");
				pc_tw_i64(out, (long long)cl->hits);
				pc_tw_key(out, "misses");
				pc_tw_i64(out, (long long)cl->misses);
				pc_tw_key(out, "stores");
				pc_tw_i64(out, (long long)cl->stores);
				pc_tw_key(out, "removes");
				pc_tw_i64(out, (long long)cl->removes);
				pc_tw_key(out, "expired");
				pc_tw_i64(out, (long long)cl->expired);
				pc_tw_end(out);
			}
			pc_tw_end(out);
		}
		/* identity is stable across that node's restarts,
		 * incarnation is not - a client watching both can
		 * tell a restart from a missed heartbeat */
		if (mem[i].has_ident) {
			char hx[33];

			hex_bytes(hx, mem[i].ident, 16);   /* was 16 snprintf */
			hx[32] = 0;
			pc_tw_key(out, "identity");
			pc_tw_bulk(out, hx, 32);
			pc_tw_key(out, "incarnation");
			pc_tw_i64(out, (long long)mem[i].incarn);
		}
		pc_tw_end(out);
	}
	/* S221: the cluster wire epoch, advertised beside the routing algo
	 * and for the same reason - a client that does not recognise it
	 * stops ROUTING and lets the daemon forward, which costs a hop and
	 * never correctness. */
	pc_tw_end(out);
	pc_tw_key(out, "routing");
	pc_tw_map(out);
	pc_tw_key(out, "epoch");
	pc_tw_i64(out, PC_CL_EPOCH);
	pc_tw_key(out, "algo");
	pc_tw_str(out, PC_ROUTE_ALGO);
	pc_tw_key(out, "mode");
	{
		int md = pc_cluster_mode();

		pc_tw_str(out, md == PC_MODE_PROXY ? "proxy"
			: md == PC_MODE_SPREAD ? "spread"
			: md == PC_MODE_SHARD ? "shard" : "store");
	}
	pc_tw_key(out, "eager");
	pc_tw_bool(out, (pc_cluster_eager()));
	/* the single operational name; "mode" above stays as it was so
	 * an existing reader keeps working */
	pc_tw_key(out, "mode_name");
	pc_tw_str(out, pc_cluster_mode_name());
	/* S127: K under spread, 0 otherwise.  An operator confirming a
	 * spread fleet needs the number the WRITE PATH is using, not
	 * the one in the file - they differ if the config never
	 * reached the cluster. */
	pc_tw_key(out, "replicas");
	pc_tw_i64(out, pc_cluster_replicas());
	pc_tw_key(out, "authoritative");
	pc_tw_bool(out, (pc_cluster_authoritative()));
	/* the peer-plane port every member shares: owner selection
	 * mixes (advertise ip, CLUSTER port), so a client that wants
	 * to compute an owner needs it - the client port would give
	 * a different hash and route everything wrong (harmlessly,
	 * but pointlessly) */
	pc_tw_key(out, "cport");
	pc_tw_i64(out, pc_cluster_port());
	/* RV-10: the stamp of the map this list was read under.  A reply
	 * the daemon had to forward carries the stamp it routed by, and a
	 * client holding a different one knows it is behind. */
	{
		unsigned int mt = 0, ms = 0;

		if (pc_cluster_map_stamp(&mt, &ms)) {
			pc_tw_key(out, "map");
			pc_tw_map(out);
			pc_tw_key(out, "term");
			pc_tw_i64(out, mt);
			pc_tw_key(out, "seq");
			pc_tw_i64(out, ms);
			pc_tw_end(out);
		}
	}
	pc_tw_end(out);
	/* S70: the one thing a client cannot otherwise learn about the
	 * daemon it dialled, and the first question in a mixed fleet */
	pc_tw_key(out, "version");
	pc_tw_str(out, PC_VERSION);
	pc_tw_key(out, "rev");
	pc_tw_str(out, PC_BUILD_REV);
	pc_tw_end(out);
}

const char pc_unknown_method[] = "method not found";

int pc_verb_cmd(const char *method, size_t mlen, const struct pc_tv *v,
		int params, struct pc_tw *out, const char **errmsg,
		unsigned int *park_req, int *privileged, int *outcome,
		void *conn)
{
	pcache_htable_t *ht;
	str k, v_;
	long long by, ttl;
	int rc;
	unsigned int vln, exp;
	int is_ctr;

	*outcome = 0;
	/* every accessor answers "absent" for params -1, so a method that
	 * takes no parameters and one called without them read alike */
	/* the key is read where it lies in the request tree: the op cores
	 * take a span, and nothing below outlives the request */
#define NEEDKEY() do { \
	const char *kp_; size_t kn_; \
	if (get_bytes(v, params, "key", &kp_, &kn_, KEY_MAX) != 0) \
		ERR("missing or bad key"); \
	k.s = (char *)kp_; k.len = (int)kn_; } while (0)

	/* ---- ping {echo?} : also the transport/codec diagnostic -------- */
	if (m_is(method, mlen, "ping")) {
		int te = pc_tv_get(v, params, "echo");

		pc_tw_map(out);
		pc_tw_key(out, "pong");
		pc_tw_bool(out, 1);
		/* an echo that is not bytes is left out, as a non-string was */
		if (te >= 0 && v->n[te].type == 'b') {
			pc_tw_key(out, "echo");
			pc_tw_bulk(out, v->n[te].p, v->n[te].len);
		}
		pc_tw_end(out);
		return 0;
	}

	/* ---- PS4: pub/sub on the native door ----------------------------
	 * subscribe {channel} / psubscribe {pattern} / unsubscribe {channel?}
	 * / punsubscribe {pattern?} answer {subscribed: N}; publish {channel,
	 * payload} answers {receivers: N}.  Deliveries are NOTIFY frames
	 * carrying {method: "message", params: {channel, payload}} and
	 * "pmessage" with pattern first (proto.c builds them) - the S107
	 * shape, so libperfd's notify hook receives them and a blocking get
	 * keeps waiting for its own id: the connection stays multiplexed,
	 * which is the whole reason a frame carries an id. */
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
		char chan[4096];
		const char *pay;
		size_t plen;
		int clen;

		if (params < 0)
			ERR("missing params");
		clen = pc_tv_get_str(v, params, "channel", chan, sizeof chan);
		if (clen <= 0)
			ERR("missing or bad channel");
		if ((size_t)clen >= sizeof PC_PUBSUB_RESERVED - 1 &&
		    !memcmp(chan, PC_PUBSUB_RESERVED, sizeof PC_PUBSUB_RESERVED - 1))
			ERR("channel prefix __pc. is reserved");
		/* the payload is published from where it lies in the request */
		if (get_bytes(v, params, "payload", &pay, &plen, VAL_MAX) != 0)
			ERR("missing or bad payload");
		pc_tw_map(out);
		pc_tw_key(out, "receivers");
		pc_tw_i64(out, pc_pubsub_publish(chan, (size_t)clen, pay, plen,
			0));
		pc_tw_end(out);
		return 0;
	}
	/* PS5: pushes over UDP to this connection's own address
	 * (lib/perfd_push.h):
	 *   pubsub_udp {port: N}          -> {stream, key, ...}; port 0 stops
	 *   pubsub_udp_confirm {cookie}   -> {udp: true, seq: N}; a probe's
	 *                                    cookie; messages follow N
	 *   pubsub_udp_ack {seq: N}       -> {acked: N}
	 * stream, key and the cookie stay HEX TEXT, as lib/perfd_push.h's
	 * consumers read them. */
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
	/* ---- fleetstop {timeout_ms?} : S222 - stop the whole fleet -----
	 * PRIVILEGED, by S129's own rule: the set is chosen by blast radius,
	 * and nothing a client can say has a larger one - every node in the
	 * fleet goes down.  The client secret cannot gate it: applications
	 * hold that.  Each peer stops the way SIGTERM stops it (drain, a
	 * snapshot, goodbye); this node waits for them, reports who went and
	 * who did not, and then stops itself, last. */
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
		/* S233: left the live set without a goodbye */
		pc_tw_key(out, "lost");
		pc_tw_arr(out);
		for (j = 0; j < nl; j++)
			pc_tw_i64(out, lost[j]);
		pc_tw_end(out);
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
		rc2 = pc_wal_sync((int)tmo);
		if (rc2 == 1)
			ERR("persistence is not configured");
		/* S215: before this, a refused fdatasync moved the watermark
		 * anyway and this verb answered {synced: true} over it */
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
	 * keys win; expired records dropped) ----------------------------- */
	if (m_is(method, mlen, "load")) {
		long ld = 0, se = 0, sx = 0;

		if (!pc_rdb_dir()[0])
			ERR("persistence is not configured");
		if (pc_rdb_import(pc_rdb_dir(), &ld, &se, &sx) != 0)
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
	if (m_is(method, mlen, "create")) {
		char nm[PC_COL_NAME_MAX];
		long long bl = PC_COL_DEFAULT_LOG2;
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
		pc_tv_get_int(v, params, "buckets_log2", &bl);
		if (bl < 4 || bl > 24)
			ERR("buckets_log2 is 4..24");
		rc2 = pc_store_create(nm, (size_t)nl, (int)bl);
		if (rc2 == -2)
			ERR("the collection limit is reached");
		if (rc2 == -3) {
			const char *why = index_ceiling_msg((int)bl);   /* S128 */

			ERR(why ? why : "the table could not be created "
				"- the arena has no room for its index");
		}
		if (rc2 != 0)
			ERR("a collection of that name already exists");
		/* the WAL first, then the file: both are read at startup and
		 * the WAL's ordering settles a disagreement between them */
		pc_wal_col_create(nm, (int)bl);
		pc_store_persist();
		pc_cluster_col_announce(nm, (size_t)nl, (int)bl, CLCOL_OP_SET);
		LM_NOTICE("collection '%s' created by a client: 2^%d buckets, "
			"%s mode\n", nm, (int)bl, pc_cluster_mode_name());
		pc_tw_map(out);
		pc_tw_key(out, "created");
		pc_tw_bool(out, 1);
		pc_tw_key(out, "col");
		pc_tw_bulk(out, nm, (size_t)nl);
		pc_tw_key(out, "buckets");
		pc_tw_i64(out, 1LL << bl);
		pc_tw_end(out);
		return 0;
	}
	if (m_is(method, mlen, "drop")) {
		char nm[PC_COL_NAME_MAX];
		pcache_ht_totals_t tot;
		long long n = 0;
		int nl, force;

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
		n = pc_store_drop(nm, (size_t)nl);
		if (n < 0)
			ERR("no such collection");
		pc_wal_col_drop(nm);
		pc_store_persist();
		pc_cluster_col_announce(nm, (size_t)nl, 0, CLCOL_OP_DROP);
		LM_NOTICE("collection '%s' dropped by a client: %lld record(s) "
			"freed\n", nm, n);
		pc_tw_map(out);
		pc_tw_key(out, "dropped");
		pc_tw_bool(out, 1);
		pc_tw_key(out, "col");
		pc_tw_bulk(out, nm, (size_t)nl);
		pc_tw_key(out, "records");
		pc_tw_i64(out, n);
		pc_tw_end(out);
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
	if (m_is(method, mlen, "rename")) {
		char nm[PC_COL_NAME_MAX], to[PC_COL_NAME_MAX];
		int nl, tl, rc2;

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
		rc2 = pc_store_rename(nm, (size_t)nl, to, (size_t)tl,
			pc_lamport_tick());
		if (rc2 == -1)
			ERR("no such collection");
		if (rc2 == -2)
			ERR("a collection of the new name already exists");
		if (rc2 != 0)
			ERR("the rename failed");
		pc_wal_col_rename(nm, to);
		if (!pc_wal_enabled())
			pc_store_persist();
		pc_cluster_col_announce2(nm, (size_t)nl, to, (size_t)tl);
		LM_NOTICE("collection '%s' renamed to '%s' by a client\n", nm, to);
		pc_tw_map(out);
		pc_tw_key(out, "renamed");
		pc_tw_bool(out, 1);
		pc_tw_key(out, "col");
		pc_tw_bulk(out, nm, (size_t)nl);
		pc_tw_key(out, "to");
		pc_tw_bulk(out, to, (size_t)tl);
		pc_tw_end(out);
		return 0;
	}

	/* ---- S69: resize {col, buckets_log2} ---------------------------
	 * Either direction, one mechanism, while the collection serves.  The
	 * work runs on the maintenance thread in bounded passes, so this
	 * returns as soon as it has started rather than holding a worker for
	 * the length of a migration; `stats` carries the progress and the
	 * daemon logs the swap and the completion. */
	/* ---- autoscale {col, mode} : S239 - how a collection is sized -----
	 * auto (grow and shrink by key count, the default), warn (change
	 * nothing, say what auto would do), off (change nothing, say
	 * nothing).  Privileged, as resize is: it decides the table's size.
	 * Persisted (the collections file), outranking the config from
	 * then on; per node, as the sizing itself is. */
	if (m_is(method, mlen, "autoscale")) {
		char nm[PC_COL_NAME_MAX], md[16];
		int nl, ml, mode;

		if (!privileged || !*privileged)
			ERR(pc_enable_configured() ? PC_NOPRIV_MSG
				: PC_NOENABLE_MSG);
		nl = pc_tv_get_str(v, params, "col", nm, sizeof nm - 1);
		if (nl <= 0)
			ERR("missing col");
		nm[nl] = '\0';
		ml = pc_tv_get_str(v, params, "mode", md, sizeof md - 1);
		if (ml <= 0)
			ERR("missing mode (auto|warn|off)");
		md[ml] = '\0';
		if ((mode = pc_autoscale_parse(md)) < 0)
			ERR("mode is auto, warn or off");
		if (pc_store_autoscale_set(nm, (size_t)nl, mode) != 0)
			ERR("no such collection");
		LM_NOTICE("collection '%s': autoscale = %s, set by a client\n",
			nm, md);
		pc_tw_map(out);
		pc_tw_key(out, "col");
		pc_tw_bulk(out, nm, (size_t)nl);
		pc_tw_key(out, "autoscale");
		pc_tw_str(out, pc_autoscale_name(mode));
		pc_tw_end(out);
		return 0;
	}
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
			const char *why = index_ceiling_msg((int)bl);   /* S128 */

			ERR(why ? why : "the new table could not be "
				"created - the arena has no room for its index");
		}
		if (rc2 == -5)
			ERR("the collection is already that size");
		/* S255: this node's own resize is the newest it has seen */
		pc_store_resize_gen_note(nm, (size_t)nl, pc_cluster_col_announce(
			nm, (size_t)nl, (int)bl, CLCOL_OP_RESIZE));
		LM_NOTICE("collection '%s': resize to 2^%d buckets started by a "
			"client\n", nm, (int)bl);
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

	/* ---- S123: reset_stats : the running totals start again --------
	 * Admin scope like load: the node's counters, not the fleet's; live
	 * gauges are untouched.  The RESP door's CONFIG RESETSTAT and the
	 * page's POST /reset-stats are the same call. */
	/* ---- S129: enable / disable -----------------------------------
	 * NATIVE DOOR ONLY, and that is the whole point of where this lives.
	 * The brief put ENABLE/DISABLE on the RESP door too; refused,
	 * because that door may run plaintext OFF-BOX under an allow-list,
	 * which would put the highest-value secret in the fleet on the
	 * wire in the clear - a worse exposure than the AUTH password it
	 * would sit beside, since this one grants keyspace deletion.  The
	 * native door is Noise-encrypted off the loopback.  Redis clients
	 * have no reason to issue DDL.
	 *
	 * @privileged NULL means a door with no privilege concept (RESP):
	 * enable is refused there rather than silently succeeding. */
	if (m_is(method, mlen, "enable")) {
		char sec[256];
		int sl;

		if (!privileged)
			ERR("enable is available on the native door only");
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
		if (!pc_enable_secret_ok(sec, (size_t)sl)) {
			enable_fail++;
			/* counted and said, the way resp_authfail is: a
			 * privilege secret being guessed should be visible */
			LM_NOTICE("a client offered a wrong enable secret "
				"(%llu failed so far)\n",
				(unsigned long long)enable_fail);
			ERR("wrong enable secret");
		}
		*privileged = 1;
		LM_NOTICE("a client raised privilege with enable\n");
		pc_tw_map(out);
		pc_tw_key(out, "privileged");
		pc_tw_bool(out, 1);
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
		pc_tw_map(out);
		pc_tw_key(out, "reset");
		pc_tw_bool(out, 1);
		pc_tw_key(out, "at");
		pc_tw_i64(out, pc_stats_reset());
		pc_tw_end(out);
		return 0;
	}

	/* ---- probe {secs?} : re-measure the WAL storage on demand ------
	 * Blocks THIS request (and the calling worker) for the probe's
	 * duration; the I/O shares the device with the live pump, so
	 * fsync latency is perturbed while it runs - inherent to
	 * re-measuring.  stats flips to cached:false afterwards. */
	if (m_is(method, mlen, "probe")) {
		struct pc_wal_policy pol;
		const struct pc_wal_probe *pr;
		long long secs = 0;

		pc_tv_get_int(v, params, "secs", &secs);
		if (secs < 0)
			secs = 0;
		if (secs > 30)
			ERR("secs is capped at 30");
		if (pc_wal_reprobe((int)secs, &pol) != 0)
			ERR("wal is not configured or the probe failed");
		pr = pc_wal_probe_result();
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
		/* say what that number is: a burst ceiling, with the volume
		 * it was measured over, so a consumer cannot read it as a
		 * sustained rate the probe never established */
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
		pc_tw_i64(out, pr ? pr->sync_bytes >> 10 : 0);
		pc_tw_key(out, "segment_mb");
		pc_tw_i64(out, pol.segment_mb);
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

	/* ---- stats {col?} ---------------------------------------------- */
	/* ---- members (S34): the fleet as a CLIENT needs it -------------
	 * Addresses + client ports so a library can pre-warm connections
	 * to the other nodes, load so it can weight them, and the ROUTING
	 * CONTRACT (mode + algorithm id) so a client may compute where a
	 * key belongs.  The algorithm carries a VERSION: a client that
	 * does not recognise it must fall back to plain spreading, and a
	 * mismatch then costs a forward, never correctness - the daemon
	 * re-checks ownership regardless of what the client believed. */
	if (m_is(method, mlen, "members")) {
		pc_members_tree(out);
		return 0;
	}

	/* ---- S88: collections -> [{name, mode, entries, buckets}] -----------
	 * The second question every operator asks after connecting, and the
	 * one `stats` answered only as a large document.  (The array's count
	 * is back-patched, so a dropped slot 0 no longer leaves a separator
	 * in front of the first row, as the streamed JSON did.) */
	if (m_is(method, mlen, "collections")) {
		int i;

		pc_tw_map(out);
		pc_tw_key(out, "collections");
		pc_tw_arr(out);
		for (i = 0; i < pc_store_count(); i++) {
			if (!pc_store_live(i))
				continue;   /* S69: a dropped collection */
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

		/* pc_tv_get_str returns a LENGTH and does not terminate: every
		 * other caller carries the length alongside the buffer.
		 * pc_stats_tree takes a C string, so terminate here or the
		 * compare runs off the end and the filter matches nothing -
		 * which is exactly what verbtest caught. */
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
			"jarrappend", "dump", "restore", "rlhit",   /* S314 */
			"hcmd",                                    /* S313 */
			"setifeq", "setifne", "delifeq", "delifne" };   /* S297 */
		size_t i;
		int known = 0;

		for (i = 0; i < sizeof cv / sizeof cv[0]; i++)
			if (m_is(method, mlen, cv[i])) {
				known = 1;
				break;
			}
		if (!known)
			ERR(pc_unknown_method);    /* S165: proto counts it by this pointer */
	}
	/* C7: every verb below this line touches collection data */
	if (serving_denied())
		ERR(PC_NOTREADY_MSG);
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
	if (m_is(method, mlen, "dump")) {
		struct dump_keys dk;
		long long cur = 0, count = 1024, slo = 0, shi = PC_SLOTS - 1, endb = -1;
		unsigned int cursor;
		size_t off;
		struct pc_tw_mark mark;
		int n = 0;
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
		/* the dump bug: a cursor is a BUCKET index, and a resize swap
		 * between two chunks puts it into a table of another shape -
		 * after a grow the records of buckets already walked reappear
		 * past the cursor (duplicates), after a shrink those not yet
		 * walked fold in behind it (MISSED, silently).  The reply names
		 * the table it walked; a caller that hands it back is refused
		 * the moment the collection has been republished, and restarts
		 * the collection.  A caller that does not is served as before. */
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
		/* S112: `end` = the first bucket this walker does NOT own - a
		 * bucket-range split for several connections on one node, each
		 * walking its own buckets once (a slot range makes every
		 * connection scan every bucket and discard the rest: measured
		 * 1.7x on eight connections).  The tail belongs to the walker
		 * whose range holds the last bucket; a walker whose `end` is
		 * short of it never enters the tail. */
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
		/* S121: one cursor space, the leg included.  A chunk is `count`
		 * buckets, or about `count` buckets' worth of records from the
		 * overflow leg - the walk budgets the leg itself (a cursor with
		 * PCACHE_CURSOR_OVF names a chain in it).  S281: the leg comes
		 * FIRST, at cursor 0, so the walker whose range starts at bucket
		 * 0 carries it and every other walker starts past it; a bounded
		 * range still ends at its `end`. */
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
			rc = pcache_ht_scan_ex(ht, &cursor, take, PCACHE_SCAN_NOVAL,
				dump_key_cb, &dk);
			if (rc < 0 || dk.full) {
				free(dk.buf);
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
		for (off = 0; off < dk.len && !out->over; ) {
			str rk, rv;
			unsigned int rexp = 0;
			unsigned long long ver = 0;
			unsigned char fl = 0;
			uint32_t kl;

			memcpy(&kl, dk.buf + off, 4);
			rk.s = dk.buf + off + 4;
			rk.len = (int)kl;
			off += 4 + kl;
			if (pcache_ht_fetch_full(ht, &rk, &rv, &rexp, &fl, &ver) != 0)
				continue;              /* gone since the scan */
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
			if (fl & PCACHE_F_JSON) {          /* S280: the type */
				pc_tw_key(out, "t");
				pc_tw_str(out, "json");
			} else if (fl & PCACHE_F_RL) {     /* S314 */
				pc_tw_key(out, "t");
				pc_tw_str(out, "rl");
			} else if (fl & PCACHE_F_HASH) {   /* S313 */
				pc_tw_key(out, "t");
				pc_tw_str(out, "hash");
			} else if (fl & PCACHE_F_LOCK) {   /* S170a */
				pc_tw_key(out, "t");
				pc_tw_str(out, "lock");
			}
			pc_tw_end(out);
			free(rv.s);
		}
		free(dk.buf);
		if (out->over) {
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
	/* ---- S113: restore {col, policy?, records: [{k, v, exp?, ver, t?}]}
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
	if (m_is(method, mlen, "restore")) {
		const char *em = "missing col";
		/* S129: privileged by BLAST RADIUS.  restore with
		 * policy:overwrite replaces every record it is handed at a
		 * fresh version - perfload rolls a fleet back with it - which
		 * is a larger blast radius than the resize beside it in this
		 * set.  It is NOT gated on allow_create: that switch is about
		 * originating DDL, and a restore creates no collection. */
		if (!privileged || !*privileged)
			ERR(pc_enable_configured() ? PC_NOPRIV_MSG
				: PC_NOENABLE_MSG);
		int trec, tpol, policy = 0;      /* 0 newer, 1 skip, 2 overwrite */
		unsigned int i, nrec;
		long long stored = 0, older = 0, existing = 0, expired = 0, refused = 0, bad = 0;
		unsigned long long now_ms;
		unsigned int now_t = get_ticks();
		struct timespec tw;

		ht = get_col(v, params, &em);
		if (!ht)
			ERR(em);
		if (writes_denied())
			ERR(PC_NOTREADY_MSG);
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
		for (i = 0; i < nrec; i++) {
			/* each record's key and value are stored from where they
			 * lie in the request tree */
			int r = pc_tv_at(v, trec, i), tt, rc2;
			const char *kp, *vp;
			size_t kl, vl;
			unsigned char rtype;           /* S280: the record's type */
			str rk, rv;
			long long expms = 0, ver = 0;
			unsigned int exp;
			unsigned long long ver_used, held_ver = 0;

			if (r < 0 || v->n[r].type != 'm') {
				bad++;
				continue;
			}
			if (get_bytes(v, r, "k", &kp, &kl, PCACHE_CELL_MAX) != 0 ||
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
				pc_tv_streq(v, tt, "lock") ? PCACHE_F_LOCK : 0;   /* S170a */
			/* a hash record is stored as the hash commands will read
			 * it: a corrupt or over-ceiling one is refused here, not
			 * discovered by the first HGET (S318 review, 10-04) */
			if (rtype == PCACHE_F_HASH &&
			        !pc_hr_valid((const unsigned char *)vp, vl, NULL)) {
				bad++;
				continue;
			}
			if (expms > 0 && (unsigned long long)expms <= now_ms) {
				expired++;
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
			 * overflow leg as absent (S122) */
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
					continue;   /* S69: a dropped collection */
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
					rc2 = pcache_ht_scan_ex(pc_store_ht(i), &cursor,
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

	/* ---- get {col,key} --------------------------------------------- */
	if (m_is(method, mlen, "get")) {
		NEEDKEY();
		{
			char *gb = get_buf();
			unsigned int gl = 0;

			if (!gb)
				ERR("get failed");
			rc = op_get_buf(ht, &k, gb, VAL_MAX, &gl, &exp,
				park_req);
			if (rc == PC_OP_OK) {
				write_hit(out, gb, (size_t)gl, exp);
				*outcome = PC_OUT_HIT;
				return 0;
			}
		}
		if (rc == PC_OP_ERR_TYPE)                  /* S280 */
			ERR(pc_wrongtype_msg);
		if (rc == PC_OP_ERR_GET) {
			ERR("get failed");
		} else {
			/* miss AND the parked case: the fallback shape below
			 * is used only if the proto layer cannot park - it
			 * must match THIS verb (a set once got a pull-miss
			 * reply at park pressure) */
			pc_tw_map(out);
			pc_tw_key(out, "found");
			pc_tw_bool(out, 0);
			pc_tw_end(out);
			*outcome = PC_OUT_MISS;
		}
		return 0;
	}

	/* ---- exists {col,key} ------------------------------------------ */
	if (m_is(method, mlen, "exists")) {
		NEEDKEY();
		{
			unsigned long long ver = 0;

			/* ver rides on exists rather than get: it is a
			 * property of the record, not of the value, and a
			 * caller comparing two copies should not have to
			 * move the bytes to do it */
			rc = pcache_ht_getver(ht, &k, &ver);
			if (rc != 0) {
				pc_tw_map(out);
				pc_tw_key(out, "exists");
				pc_tw_bool(out, 0);
				pc_tw_end(out);
				return 0;
			}
			pc_tw_map(out);
			pc_tw_key(out, "exists");
			pc_tw_bool(out, 1);
			pc_tw_key(out, "ver");
			pc_tw_i64(out, (long long)ver);
			pc_tw_end(out);
		}
		return 0;
	}

	/* ---- ttl {col,key} : -2 absent, -1 no expiry, else seconds ----- */
	if (m_is(method, mlen, "ttl")) {
		NEEDKEY();
		rc = pcache_ht_probe(ht, &k, &vln, &exp, &is_ctr);
		pc_tw_map(out);
		pc_tw_key(out, "ttl");
		if (rc == -2)
			pc_tw_i64(out, -2);
		else if (exp == 0)
			pc_tw_i64(out, -1);
		else
			pc_tw_i64(out, (long long)exp - (long long)get_ticks());
		pc_tw_end(out);
		return 0;
	}

	/* ---- del {col,key} --------------------------------------------- */
	if (m_is(method, mlen, "del")) {
		NEEDKEY();
		rc = op_del(ht, &k, park_req);
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
		if (method[0] == 's')
			by = -by;
		rc = op_addsub(ht, &k, by, ttl, &nv, park_req, 0);
		if (rc == PC_OP_PARKED) {
			/* the park fallback is a RESULT, not a failure (D2) */
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
		if (rc == PC_OP_ERR_TYPE)                  /* S280 */
			ERR(pc_wrongtype_msg);
		if (rc == PC_OP_ERR_NOTINT)
			ERR("value is not an integer");
		if (rc == PC_OP_ERR_WRFAIL)    /* refused: nv was never written */
			ERR(wr_refused_msg());
		pc_tw_map(out);
		pc_tw_key(out, "value");
		pc_tw_i64(out, nv);
		pc_tw_end(out);
		return 0;
	}

	/* ---- S297: setifeq / setifne {col,key,value,cmp,ttl?} and
	 * delifeq / delifne {col,key,cmp} ---------------------------------- *
	 * Redis 8.4's SET IFEQ/IFNE and DELEX IFEQ/IFNE.  Methods, not
	 * params on set and del, for S279b's reason: an older daemon would
	 * ignore an unknown param and act unconditionally; it refuses an
	 * unknown method.  {stored|deleted: bool}; a key holding a hash, a
	 * document or a rate-limit record is an error. */
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

	/* ---- set {col,key,value,ttl?} ---------------------------------- *
	 * S279b: setnx / setxx take the same params and store only when the
	 * key is absent / present - {stored: false} when declined.  Methods,
	 * not params on set: an older daemon ignores an unknown param and
	 * would store unconditionally, and a client library replays set
	 * after a failover, which would turn a won lock into "declined". */
	if (m_is(method, mlen, "set") || m_is(method, mlen, "setnx") ||
	        m_is(method, mlen, "setxx")) {
		int cond = m_is(method, mlen, "setnx") ? PC_SETCOND_NX :
			m_is(method, mlen, "setxx") ? PC_SETCOND_XX : 0;
		const char *vp;
		size_t vl;

		NEEDKEY();
		/* the value is stored from where it lies in the request: the
		 * copy-and-decode leg went with the JSON dialect (S317) */
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

	/* S313: hcmd {col, key, cmd, args?: [..]} -> the command's reply tree
	 * ITSELF (D9) - any H command, e.g. {cmd: "HSET", args: ["f", "v"]};
	 * the reply is the command's Redis reply: an integer, bytes, nil, an
	 * array, OK or an error line, as the hash core built it */
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
			/* the arguments are used where they lie in the request */
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
	/* S314: rlhit {col,key,window_ms,limit?} -> {count: N, allowed: b} */
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
	/* ---- json path verbs (S26 + the v1.1 extras): -------------------
	 * jget  {col,key,path?}            -> {found: true, value: <frag>}
	 * jset  {col,key,path?,val,nx?,xx?,mkpath?,ttl?} -> {set: true}
	 * jdel  {col,key,path}             -> {deleted: bool}
	 * jincr {col,key,path,by?}         -> {value: N}
	 * jarrappend {col,key,path,val}    -> {count: N}
	 * Documents are opaque JSON text in ordinary cells; edits are span
	 * splices (jsonpath.c) under the key's mutex stripe (pc_json_rmw -
	 * shared with the cluster forward plane).  A document travels as
	 * DATA: `val` is one bulk holding the value's JSON text (a string
	 * value arrives quoted - the client encodes), and jget's `value` is
	 * a bulk holding the fragment's JSON text (D3).  TTL preserved
	 * unless ttl given.  On a proxy collection a non-holder FORWARDS
	 * the op to the holder (M_FWD_JSON); paths use plain names. */
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
			/* the document's JSON text, used where it lies in the
			 * request; the path layer validates it */
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
			unsigned char fl = 0;

			/* S280: a string is refused, not deleted - Redis 8's
			 * JSON.DEL on one is the wrong-type error and the key
			 * survives (probed); the RESP door does the same.  Only
			 * a record held HERE can be checked: a shard non-owner
			 * forwards the delete as before. */
			if (pc_store_types_strict(ht) &&
			        pcache_ht_getflags(ht, &k, &fl) == 0 &&
			        !(fl & PCACHE_F_JSON))
				ERR(pc_json_wrongtype_msg);
			rc = op_del(ht, &k, park_req);
			pc_tw_map(out);
			pc_tw_key(out, "deleted");
			pc_tw_bool(out, rc == PC_OP_OK);
			pc_tw_end(out);
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
			pc_tw_end(out);                /* frag is borrowed (S283) */
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

	/* ---- scan {col,cursor?,match?,count?,values?} ------------------ */
	/* ---- keys {col,match?,limit?} ---------------------------------- */
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
			/* S88: say which collection, so a listed key pastes straight
			 * back into get, and say when this is ONE node's share: a
			 * placement-spread collection is a third of the truth on a
			 * three-node fleet, and an operator read it as "two nodes are
			 * not saving their keys" */
			{
				struct pc_cl_stats cs;
				const char *cn = col_name_of(ht);

				pc_cluster_get_stats(&cs);
				pc_tw_map(out);
				pc_tw_key(out, "collection");
				pc_tw_bulk(out, cn, strlen(cn));
				pc_tw_key(out, "complete");
				pc_tw_bool(out, !(pc_cluster_enabled() && cs.peers_up > 0 &&
					!pc_store_eager_enabled(ht)));
				pc_tw_key(out, "scope");
				pc_tw_str(out, "node");
				pc_tw_key(out, "keys");
				pc_tw_arr(out);
			}
			cursor = 0;
			do {
				rc = pc_store_scan(ht, &cursor, 1024,   /* S257 */
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
		 * skips the value memcpy AND its cold cachelines (S40) */
		pc_store_scan(ht, &cursor, (unsigned int)count,  /* S257 */
			sc.values ? 0 : PCACHE_SCAN_NOVAL, scan_cb, &sc);
		pc_tw_end(out);
		pc_tw_key(out, "cursor");
		pc_tw_i64(out, cursor);
		pc_tw_key(out, "more");
		pc_tw_bool(out, (cursor));
		pc_tw_end(out);
		return 0;
	}

	/* ---- mget {col,keys:[...]} ------------------------------------- */
	if (m_is(method, mlen, "mget")) {
		int tk = pc_tv_get(v, params, "keys");
		unsigned int i;

		if (tk < 0 || v->n[tk].type != 'a')
			ERR("missing keys array");
		pc_tw_map(out);
		pc_tw_key(out, "values");
		pc_tw_arr(out);
		for (i = 0; i < v->n[tk].len; i++) {
			int kn = pc_tv_at(v, tk, i);

			/* an element that is not bytes is skipped, not counted,
			 * as the JSON door skipped a non-string */
			if (kn < 0 || v->n[kn].type != 'b')
				continue;
			if (v->n[kn].len > KEY_MAX) {
				pc_tw_map(out);
				pc_tw_key(out, "found");
				pc_tw_bool(out, 0);
				pc_tw_end(out);
				continue;
			}
			k.s = (char *)v->n[kn].p; k.len = (int)v->n[kn].len;
			{
				unsigned char fl = 0;

				rc = pcache_ht_fetch_ex(ht, &k, &v_, &exp, &fl);
				/* S280: as RESP MGET - a document answers as
				 * a miss under strict, not as an error */
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

	/* ---- mset {col,items:[{key,value,ttl?},...]} ------------------- */
	if (m_is(method, mlen, "mset")) {
		int ti = pc_tv_get(v, params, "items");
		int stored = 0, dropped = 0;
		unsigned int i;

		if (ti < 0 || v->n[ti].type != 'a')
			ERR("missing items array");
		if (pc_store_shard_enabled(ht))
			ERR("mset on a shard collection is not "
				"supported yet - use pipelined set");
		for (i = 0; i < v->n[ti].len; i++) {
			int it = pc_tv_at(v, ti, i);
			const char *kp, *vp;
			size_t kl, vl;

			if (it < 0 || v->n[it].type != 'm')
				continue;
			/* key and value are stored from where they lie */
			if (get_bytes(v, it, "key", &kp, &kl, KEY_MAX) != 0 ||
			        get_bytes(v, it, "value", &vp, &vl, VAL_MAX) != 0) {
				dropped++;
				continue;
			}
			ttl = 0;
			pc_tv_get_int(v, it, "ttl", &ttl);
			k.s = (char *)kp; k.len = (int)kl;
			v_.s = (char *)vp; v_.len = (int)vl;
			/* S303: the same local write a set makes - WAL, event,
			 * eager push.  This stored and logged only, so on an
			 * eager fleet an mset reached the peers at the next
			 * sweep (~10 s) and a read there pulled it meanwhile;
			 * an eager miss no longer pulls. */
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

/* ---- the binary codec (item 7: the libperfd hot path) -------------------
 * One frame = one data verb; layouts in proto.h.  col/key/value bytes
 * arrive RAW inside the frame and are used in place (zero copy).  @out
 * is a plain byte sink here (pc_jw_raw only).  When parking, the codec
 * pre-writes its own park-failure shape into @out/@flags and reports
 * col/key spans for the park table.  The pub/sub verbs 10-14 are gone
 * (S317): publish and the subscriptions are CMD methods. */

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

	(void)conn;                            /* the pub/sub verbs took it */
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
		ERRB(pc_bin_unknown_verb);     /* S165: proto.c counts it by this pointer */
	if (serving_denied())                  /* C7 - PING above is exempt */
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
		if (rc == PC_OP_ERR_TYPE)                  /* S280 */
			ERRB(pc_wrongtype_msg);
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
		if (rc == PC_OP_ERR_TYPE)                  /* S280 */
			ERRB(pc_wrongtype_msg);
		if (rc == PC_OP_ERR_NOTINT)
			ERRB("value is not an integer");
		if (rc == PC_OP_ERR_WRFAIL)    /* refused: nv was never written */
			ERRB(wr_refused_msg());
		bw_i64(out, nv);
		return 0;

	case PC_VERB_HCMD: {                   /* S313: any H command */
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
		if (rc == PC_OP_OK)
			pc_jw_raw(out, (const char *)t.b, t.n);
		else if (rc == PC_OP_PARKED)
			pc_jw_raw(out, "cluster busy", 12);  /* park fallback */
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

	case PC_VERB_RLHIT: {                  /* S314 */
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
			pc_jw_raw(out, "cluster busy", 12);   /* park fallback */
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

	case PC_VERB_SETNX:                    /* S279b */
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

/* S280: an error line in Redis's own words, code and all - its WRONGTYPE,
 * and RedisJSON's wrong-type error, which has no code word */
static void resp_err_line(struct pc_jw *w, const char *line)
{
	pc_jw_lit(w, "-");
	pc_jw_lit(w, line);
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

/* RESP walker callbacks: bulk keys into the element scratch.  The RESP
 * door's own walk state - its writer is the RESP text writer, where the
 * native door's (scan_ctx) is the tree's. */
struct resp_scan_ctx {
	struct pc_jw *w;
	const char *pat;
	int patlen;                        /* <0 = no pattern */
	int emitted;
	int scanned;                       /* keys VISITED, pre-filter: the
	                                    * SCAN yield loop bounds on this,
	                                    * never on emitted - a rare MATCH
	                                    * must not walk the table in one
	                                    * call (the stall S40 forbids) */
	int limit;
	int stopped;                       /* limit reached mid-walk */
	unsigned int now;
};

static int resp_keys_cb(const str *key, const str *val, unsigned int exp,
		void *p)
{
	struct resp_scan_ctx *sc = p;

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

static long long keys_mono_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* S292: a turn walks by TIME, not by a bucket count.  1,024 buckets a
 * turn made a walk's turn count scale with the table's SIZE: a sparse
 * 131,072-bucket table took 128 turns, each as long as everything else
 * the worker served in it, so the KEYS caller waited 128 busy turns for
 * a walk that costs well under a millisecond.  Now a turn walks its
 * floor of 1,024 buckets and then goes on in steps of PC_KEYS_STEP while
 * the turn has spent less than [daemon] keys_turn_us - a sparse table ends
 * in a turn or two, and a dense one still yields after about the budget
 * (or the floor, whichever costs more), which is the stall bound the
 * cooperative walk exists for.  The step is 256 rather than 64 because
 * each pc_store_scan call also looks the table up in the registry. */
#define PC_KEYS_FLOOR      1024
#define PC_KEYS_STEP       256
static long long keys_budget_ns = PC_KEYS_TURN_US_DEFAULT * 1000LL;

void pc_keys_set_turn_us(int us)
{
	keys_budget_ns = us > 0 ? us * 1000LL : 0;
}

int pc_keys_chunk(void *htv, unsigned int *cursor, const char *pat,
		int patlen, unsigned int now, struct pc_jw *w, int *emitted,
		int limit, int *limit_hit)
{
	pcache_htable_t *ht = htv;
	struct resp_scan_ctx sc;
	long long t0 = keys_mono_ns();
	unsigned int step = PC_KEYS_FLOOR;

	memset(&sc, 0, sizeof sc);
	sc.w = w;
	sc.pat = patlen >= 0 ? pat : NULL;
	sc.patlen = patlen;
	sc.now = now;
	sc.emitted = *emitted;
	sc.limit = limit;
	for (;;) {
		pc_store_scan(ht, cursor, step, PCACHE_SCAN_NOVAL,  /* S257 */
			resp_keys_cb, &sc);
		if (!*cursor || sc.stopped || w->overflow ||
		        keys_mono_ns() - t0 >= keys_budget_ns)
			break;
		step = PC_KEYS_STEP;
	}
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
	{
		/* S280: the recorded type, whichever redis_types - loose
		 * only relaxes what the commands accept */
		unsigned char fl = 0;

		resp_simple(c->out, pcache_ht_getflags(c->ht, &k, &fl) != 0 ?
			"none" : (fl & PCACHE_F_JSON) ? "ReJSON-RL" :
			(fl & PCACHE_F_RL) ? "ratelimit" :             /* S314 */
			(fl & PCACHE_F_HASH) ? "hash" :                /* S313 */
			(fl & PCACHE_F_LOCK) ? "zset" : "string");     /* S170a */
	}
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
		{
			unsigned char fl = 0;

			if (pcache_ht_fetch_ex(c->ht, &k, &v, &exp, &fl) == 0) {
				/* S280: Redis answers nil for a document
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
	if (rc == PC_OP_ERR_TYPE)                      /* S280 */
		resp_err_line(c->out, pc_wrongtype_msg);
	else if (rc == PC_OP_ERR_GET) {
		resp_err(c->out, "get failed");
	} else {
		resp_nil(c->out);              /* miss; also the park fallback */
	}
	return 0;
}

/* S297: DELEX key [IFEQ v | IFNE v] - Redis 8.4.  Without a condition it
 * is DEL of one key.  IFDEQ/IFDNE compare a DIGEST, which perfcached does
 * not have: refused by name.  Replies as Redis 8.10.2 gave them: 1 / 0,
 * and a condition on a non-string key is an error. */
static int rh_delex(struct resp_ctx *c)
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
				/* S279 */
				int want = resp_is(c->argv[i], c->argl[i], "NX")
					? PC_SETCOND_NX : PC_SETCOND_XX;

				if ((cond && cond != want) || ckind) {
					resp_err(c->out, "syntax error");
					return 0;
				}
				cond = want;
			} else if (resp_is(c->argv[i], c->argl[i], "IFEQ") ||
			        resp_is(c->argv[i], c->argl[i], "IFNE")) {
				/* S297: Redis 8.4's compare-and-set; one
				 * condition, never beside NX/XX (Redis: syntax
				 * error) */
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
		resp_nil(c->out);         /* S279: NX/XX (S297: IFEQ/IFNE) did not hold */
	else if (rc == PC_OP_ERR_TYPE)
		resp_err_line(c->out, pc_wrongtype_msg);     /* S297 */
	else if (rc == PC_OP_ERR_MODE)
		resp_err(c->out, ckind ? cmp_mode_msg : setcond_mode_msg);
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
		resp_err(c->out, wr_refused_msg());
	else if (rc == PC_OP_ERR_FULL)
		resp_err(c->out, "cache full");
	else
		resp_err(c->out, "value too large");
	return 0;
}

/* S314: RL.HIT key window_ms [limit] -> *2 :count :allowed (1 when no
 * limit is given).  A perfcached extension, not a Redis command. */
static int rh_rlhit(struct resp_ctx *c)
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
static int rh_eval(struct resp_ctx *c)
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
static int rh_script(struct resp_ctx *c)
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

/* S313: every H command - the reply tree rendered as RESP */
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
	else if (rc == PC_OP_ERR_TYPE)                 /* S280 */
		resp_err_line(c->out, pc_wrongtype_msg);
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
	struct pc_member *mem = members_scratch();
	int nm = mem ? pc_cluster_members(mem, PC_CL_MAXMEMBERS) : 0;
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
		unsigned char fl = 0;

		/* S280: JSON.DEL of a whole string is RedisJSON's wrong-type
		 * refusal, as every other JSON verb on it is */
		if (pc_store_types_strict(c->ht) &&
		        pcache_ht_getflags(c->ht, &k, &fl) == 0 &&
		        !(fl & PCACHE_F_JSON)) {
			resp_err_line(c->out, pc_json_wrongtype_msg);
			return 0;
		}
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
		if (fmsg == pc_json_wrongtype_msg)       /* S280 */
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
	}
	return 0;
}

static int rh_scan(struct resp_ctx *c)
{
	struct resp_scan_ctx sc;
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
			pc_store_scan(c->ht, &cursor, chunk,       /* S257 */
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
	{ "DELEX", RESP_DATA | RESP_KEYED, rh_delex },     /* S297 */
	{ "EVAL", RESP_DATA, rh_eval }, { "EVALSHA", RESP_DATA, rh_eval },   /* S170a */
	{ "SCRIPT", RESP_DATA, rh_script },
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
	{ "RL.HIT", RESP_DATA | RESP_KEYED, rh_rlhit },                    /* S314 */
	{ "HSET", RESP_DATA | RESP_KEYED, rh_hash }, { "HSETNX", RESP_DATA | RESP_KEYED, rh_hash },  /* S313 */
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

/* RV-10: -MOVED <slot> <ip>:<port> for a key @node owns.  0 when the
 * member cannot be named - the caller then forwards, as it always has:
 * a redirect must never turn a request the fleet can serve into an error */
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

/* The command table as an open-addressed index over the case-folded
 * name, so a lookup is one hash and usually one compare - GET is the
 * 24th row, and the linear scan compared it against the 23 before it.
 * Built on first use by whichever worker claims it (CAS), published with
 * a release store; a caller that finds it not ready yet scans the table,
 * which is always right.  A name longer than any command cannot match. */
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
	 * the three planes encode.
	 *
	 * The row is looked up ONCE here and read by all four sites below.
	 * Each used to look it up again: four linear scans of the table
	 * per command, ~96 case-folding compares for a GET (the 24th row),
	 * measured at 14% of a SET's worker CPU and 18% of a GET's. */
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
