/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * verbs.h — the verb set behind the native door (task S8; binary-only
 * since S317, DESIGN 12if).
 *
 * pc_verb_cmd() answers a PC_VERB_CMD frame: a method name and its
 * parameters as a tree (src/ptree.h), the reply written as ONE tree item
 * into @out.  On a method-level failure it returns -1 and points *errmsg
 * at a static message, which proto sends as an error frame.
 *
 * pc_verb_bin() is the codec for the fixed-layout DATA verbs (layouts in
 * proto.h) and pc_verb_resp() the RESP door's; all three answer through
 * the same dialect-neutral op cores, so cluster semantics cannot drift
 * between them.
 */
#ifndef PC_VERBS_H
#define PC_VERBS_H

#include "json.h"
#include "ptree.h"                     /* S317 */
#include "core/pcache_htable.h"

/* dialect-neutral op-core outcomes - shared with proto.c's
 * probe-resume path (PC_DONE_SET_RESUME) */
#define PC_OP_OK          0            /* done (hit/stored/deleted) */
#define PC_OP_ABSENT      1            /* benign miss / was absent */
#define PC_OP_PARKED      2            /* *park set: reply comes later */
#define PC_OP_ERR_GET     (-1)
#define PC_OP_ERR_FULL    (-2)
#define PC_OP_ERR_2BIG    (-3)
#define PC_OP_ERR_NOTINT  (-4)
#define PC_OP_ERR_FWD     (-5)
/* The parked-request table was full: BACKPRESSURE, not a failure of the
 * network or the routing, and the only one of the three worth retrying.
 * Kept distinct from PC_OP_ERR_FWD so the dialects can say "try again"
 * instead of "forward failed", which sends an operator to look at a
 * network that is fine.  S38. */
#define PC_OP_ERR_BUSY    (-6)
/* the node is FAILED: writes refused, reads still served */
#define PC_OP_ERR_WRFAIL  (-9)
/* S279: the collection's mode has no node that can decide it (SET NX/XX
 * in a proxy or pull-only cluster) - refused, never decided wrongly */
#define PC_OP_ERR_MODE    (-10)
/* S280: a string command against a JSON document (redis_types = strict) */
#define PC_OP_ERR_TYPE    (-11)
/* S314: a record type the fleet cannot carry yet - a live member does not
 * advertise it (DESIGN 12ic): refused, never written half-typed */
#define PC_OP_ERR_GATE    (-12)

/* the RESP door's text writer and the binary verbs' byte sink (json.h);
 * S313's hash reply tree is a ptree item and renders through src/ptree.h
 * (pc_tree_resp for RESP, pc_tree_json for the HTTP door) */
struct pc_jw;

/* S280: under redis_types = strict, the two refusals Redis 8 makes, in
 * its exact words (probed on 8.10.1): a string command against a JSON
 * document, and a JSON command against a string.  The first carries
 * Redis's WRONGTYPE code; RedisJSON's has no code word at all. */
extern const char pc_wrongtype_msg[];       /* "WRONGTYPE Operation ..." */

/* S331: Redis-style glob (* ? [abc] [a-c] [^a], backslash escapes) - KEYS'
 * matcher, for the read-through allow-list */
int pc_glob_match(const char *p, int pn, const char *s, int sn);
/* S331: store a record as a client write does (table, WAL, eager push) */
int pc_verb_store_local(pcache_htable_t *ht, const str *k, const char *v,
		size_t n, unsigned int exp, unsigned char fl);
extern const char pc_json_wrongtype_msg[];  /* "Existing key has wrong ..." */

/* S279: SET's condition - store only when the key is absent (NX) or
 * present (XX).  Expired counts as absent, as in Redis. */
#define PC_SETCOND_NX  1
#define PC_SETCOND_XX  2

/* the probe-resume replays (proto.c): the write runs the normal op
 * core with the probe suppressed - placement decides, and may re-park
 * on a forward (park id in *park). */
int pc_op_set_resume(const char *col, size_t collen, const char *key,
		size_t klen, const char *val, int vlen, long long ttl,
		unsigned int *park);
int pc_op_add_resume(const char *col, size_t collen, const char *key,
		size_t klen, long long by, long long ttl, long long *nv,
		unsigned int *park);

/* S213: a forwarded re-arm applied at the holder (cluster.c): adopts a
 * passive copy, WALs it whole, pushes it as authored.  @exp is absolute
 * ticks (0 = never).  1 = re-armed, 0 = absent. */
int pc_op_expire_apply(const char *col, size_t collen, const char *key,
		size_t klen, unsigned int exp);

/* S279: a forwarded SET NX/XX decided at the deciding node (cluster.c).
 * @ttl is relative seconds (0 = none).  1 = stored, 0 = the condition
 * did not hold, < 0 = PC_OP_ERR_* (not stored). */
int pc_op_setcond_apply(const char *col, size_t collen, const char *key,
		size_t klen, const char *val, size_t vlen, long long ttl,
		int cond);

/* S297: Redis 8.4's compare-and-set / compare-and-delete - SET ... IFEQ v
 * / IFNE v and DELEX key IFEQ v / IFNE v - decided where SET NX is (the
 * key's deciding node), on the forward plane as ops PC_CMP_FWD_OP(kind).
 * A forwarded one carries [cmplen u32 LE][cmp][new value] as its value. */
#define PC_CMP_SET_IFEQ 1
#define PC_CMP_SET_IFNE 2
#define PC_CMP_DEL_IFEQ 3
#define PC_CMP_DEL_IFNE 4
#define PC_CMP_FWD_OP(kind) (5 + (kind))      /* ops 6..9 */
/* 1 applied, 0 the condition did not hold, PC_OP_ERR_TYPE the key holds
 * a hash, a document or a rate-limit record, else PC_OP_ERR_* */
int pc_op_cmp_apply(const char *col, size_t collen, const char *key,
		size_t klen, const char *cmp, size_t cmplen, const char *val,
		size_t vlen, long long ttl, int kind);

/* JSON path op codes - shared by the verb layer and the cluster
 * forward plane (M_FWD_JSON carries one) */
#define PC_JOP_SET    0
#define PC_JOP_DEL    1
#define PC_JOP_INCR   2
#define PC_JOP_APPEND 3
#define PC_JOP_GET    4
/* S314: RL.HIT rides the JSON forward plane - the same key stripe, the
 * same M_FWD_JSON frame and ack: by = window ms, ttl = limit (have_ttl:
 * one was given), the ack's newval = the count, cnt = allowed */
#define PC_JOP_RLHIT  5
/* S313: a hash command run on the key's deciding node - val carries the
 * command's arguments ([argc u16][(len u32)(bytes)] x argc, argv[0] the
 * name, argv[1] the key), the ack's fragment the reply tree */
#define PC_JOP_HCMD   6
/* S319: a hash READ forwarded in shard mode - val is EMPTY (only the
 * key travels), the ack's fragment is the raw hash RECORD (st 0), or
 * nothing (st 1 = absent, st 3 = the key holds something else); the
 * ASKING node renders the reply over it.  A record is at most PC_HR_MAX
 * bytes by its own invariant, so it always fits the ack - a rendered
 * HGETALL tree is 4N-3 bytes bigger and did not (cluster.c refused it
 * silently above PC_MAX_FWD_VAL).  The command's encoded arguments stay
 * on the asker, in its park entry. */
#define PC_JOP_HCMD_READ 7
/* S170a: an approved Symfony lock script, run on the key's deciding node.
 * The value is [script u8][now_ms i64 LE, -1 = the decider's clock][ttl_ms
 * i64 LE][token]; the ack's newval is the script's answer (1 true, 0 false)
 * and st 3 a key of another type (WRONGTYPE). */
#define PC_JOP_SLOCK  8
#define PC_SLOCK_PROBE   0             /* TIME; SET ... PX 1; return 1 */
#define PC_SLOCK_SAVE    1             /* acquire a write lock */
#define PC_SLOCK_READ    2             /* acquire a read lock */
#define PC_SLOCK_REFRESH 3             /* putOffExpiration */
#define PC_SLOCK_RELEASE 4             /* delete */
#define PC_SLOCK_EXISTS  5
/* one bound for every door and the forwarder: a hash command's argv
 * (name and key included) */
#define PC_HCMD_MAXARGS 512
/* S314: hits kept per rate-limit key, 8 bytes each - the whole record
 * stays under the 58,000-byte forward ceiling (PC_MAX_FWD_VAL) */
#define PC_RL_MAX     6000

/* the striped JSON read-modify-write core (verbs.c) - callable from
 * any thread; the key stripe serializes.  0 ok, 1 benign-absent,
 * -1 error (*errmsg static).  GET: *frag_out malloc'd. */
int pc_json_rmw(pcache_htable_t *ht, const char *key, int klen, int op,
		const char *path, int plen, const char *val, int vlen,
		long long by, int have_ttl, long long ttl, int nx, int xx,
		int mkpath, char **frag_out, int *fraglen_out,
		long long *newval, int *count_out, const char **errmsg);

/* @park_req: set nonzero when the answer is deferred to a cluster pull
 * or a holder forward (the caller parks the request; what is in @out is
 * the fallback it answers with when it cannot park) */
/* @privileged: S129's per-CONNECTION privilege bit, in/out.  Passed
 * explicitly rather than held in thread-local state on purpose: the
 * failure mode of an implicit current-connection pointer is a bit
 * surviving into the NEXT connection on the same worker, which is
 * silently the thing this task exists to prevent.  The compiler finds
 * every caller; a stale pointer finds nobody.  The CMD path passes the
 * connection's bit; NULL from a door that has no privilege concept
 * (RESP) - that one cannot enable and cannot call a privileged verb. */
/* S317: the native door's ONE method entry (PC_VERB_CMD).  @method/@mlen
 * name the method; @v is the request's parsed tree and @params its root
 * map node (-1 when the request carried no parameters).  The reply is
 * written into @out as ONE tree item.  Returns 0 (a reply written, or
 * parked with *park_req set and the park fallback written), or -1 with
 * *errmsg: proto answers an error frame and sends nothing written to
 * @out.  An unknown method returns -1 with *errmsg == pc_unknown_method
 * (pointer-compared; proto counts it, S165).  *outcome is PC_OUT_HIT /
 * PC_OUT_MISS for the query log when a lookup answered, else 0.
 * @privileged is the connection's bit (never NULL on this door; RESP
 * passes NULL to the RESP entry). */
#define PC_OUT_HIT  1
#define PC_OUT_MISS 2
int pc_verb_cmd(const char *method, size_t mlen, const struct pc_tv *v,
		int params, struct pc_tw *out, const char **errmsg,
		unsigned int *park_req, int *privileged, int *outcome,
		void *conn);
extern const char pc_unknown_method[];

/* S37: the fleet as a tree - the `members` reply, and what /members
 * renders as JSON */
void pc_members_tree(struct pc_tw *out);

/* S67: the `stats` body as a tree, so the HTTP door can serve the same
 * numbers the verb does.  @only_col NULL = every collection. */
void pc_stats_tree(struct pc_tw *out, const char *only_col);
/* S123: the running totals start again; returns the unix time of the reset */
long long pc_stats_reset(void);
/* S358: the same, recorded as of @at (a peer's or the schedule's moment) */
long long pc_stats_reset_at(long long at);
/* S358: this node and every live peer, all as of this node's moment */
long long pc_stats_reset_fleet(void);
/* S358: reset every @days days (1-7; 0 = never) at @at_s seconds past
 * local midnight; the maintenance thread calls the tick once a second */
void pc_stats_reset_schedule(int days, int at_s);
void pc_stats_reset_tick(void);

/* S69: whether a client may create and drop collections on this node */
void pc_verb_set_allow_create(int on);
void pc_verb_set_resp_redirect(int moved);           /* RV-10 */

/* the binary codec for the fixed-layout data verbs: @pl/@plen is one
 * request frame's payload (verb byte first).  Writes the response
 * payload into @out (raw bytes) and may set *flags (PC_BIN_F_ERR).
 * Parking mirrors pc_verb_cmd: sets *park_req, pre-writes the
 * park-failure shape, and reports the col/key spans (POINTERS INTO @pl)
 * for the park table.  Returns 0, or -1 with *errmsg set (the proto
 * layer sends an error frame). */
/* S165: the binary dialect's "unknown verb" message, exported so the
 * codec can tell THAT refusal from every other by pointer, not by text */
extern const char pc_bin_unknown_verb[];

int pc_verb_bin(const char *pl, size_t plen, struct pc_jw *out, int *flags,
		unsigned int *park_req, const char **colp, size_t *collenp,
		const char **keyp, size_t *klenp, const char **errmsg, void *conn);

/* the RESP compatibility codec (task S29): @argv/@argl/@nargs is one
 * framed command (multibulk or inline; spans into the conn buffer).
 * Writes the COMPLETE RESP reply - errors included - into @out.
 * @scratch (>= JW scratch size) assembles unknown-count arrays.
 * @cur_col/@cur_collen: the connection's selected collection (SELECT
 * mutates it; cap 40 bytes).  Parking mirrors the other codecs: sets
 * *park_req with the fallback shape pre-written, reports col/key spans.
 * *quit set = QUIT was answered: flush, then close.  *authed is the
 * connection's AUTH state (S33): 1 when no password is configured or
 * AUTH succeeded; while 0 only AUTH/HELLO/QUIT are answered. */
/* S40: a verb that wants a COOPERATIVE walk fills this instead of
 * walking - the proto layer then runs the walk one bounded chunk per
 * event-loop turn, so the worker keeps serving its other connections
 * (measured: a full KEYS held a worker 12-33ms; a chunk is ~100us).
 * Only the RESP KEYS verb uses it today. */
struct pc_enum_start {
	int start;                     /* 1 = begin a chunked walk */
	void *ht;                      /* the resolved collection */
	char pat[256];
	int patlen;                    /* <0 = no pattern */
	int limit;
};

int pc_verb_resp(char *const *argv, const size_t *argl, int nargs,
		struct pc_jw *out, char *scratch, size_t scratch_cap,
		unsigned int *park_req, const char **colp, size_t *collenp,
		const char **keyp, size_t *klenp, char *cur_col,
		size_t *cur_collen, int *quit, int *authed,
		struct pc_enum_start *es, void *obs, void *conn);

/* one bounded chunk of a KEYS walk: 1024 buckets, then on in steps
 * while the turn is under its time budget (S292).  Returns 1
 * while the walk has more, 0 when the table is exhausted; *emitted
 * accumulates, w->overflow and the limit stop it early. */
/* S292: [daemon] keys_turn_us - a walk's time budget per worker turn;
 * 0 walks the 1,024-bucket floor only (0.4.0's behaviour) */
void pc_keys_set_turn_us(int us);

int pc_keys_chunk(void *ht, unsigned int *cursor, const char *pat,
		int patlen, unsigned int now, struct pc_jw *w, int *emitted,
		int limit, int *limit_hit);

/* S319: the asking node's side of a forwarded hash read.  @args is the
 * command as op_hcmd encoded it ([argc u16][(len u32)(bytes)]...), @rec
 * the record the owner answered with (st 0), NULL on st 1 (absent) or
 * st 3 (wrong type).  Renders the reply into @t exactly as the owner
 * would have.  -1 = the arguments do not decode (cannot happen for a
 * frame this node built). */
struct pc_tw;
int pc_hcmd_render_read(const unsigned char *args, size_t argsl,
		const unsigned char *rec, size_t rlen, int st, struct pc_tw *t);
/* op_hcmd leaves a forwarded read's encoded arguments here for the
 * door's park entry to take (same thread, same call); NULL when there
 * are none.  Taking clears it; the taker owns and frees the buffer. */
unsigned char *pc_hcmd_take_park_args(size_t *n);
#endif /* PC_VERBS_H */
