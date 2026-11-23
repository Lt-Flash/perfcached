/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* verbs.h - the verb set behind the native door; pc_verb_cmd, _bin and _resp answer
 * through the same dialect-neutral op cores, so cluster semantics cannot drift. */
#ifndef PC_VERBS_H
#define PC_VERBS_H

#include "json.h"
#include "ptree.h"
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
/* the parked-request table was full: BACKPRESSURE, the only one worth retrying - apart
 * from ERR_FWD so a dialect says "try again", not "forward failed" */
#define PC_OP_ERR_BUSY    (-6)
/* the node is FAILED: writes refused, reads still served */
#define PC_OP_ERR_WRFAIL  (-9)
/* the collection's mode has no node that can decide it (SET NX/XX
 * in a proxy or pull-only cluster) - refused, never decided wrongly */
#define PC_OP_ERR_MODE    (-10)
/* a string command against a JSON document (redis_types = strict) */
#define PC_OP_ERR_TYPE    (-11)
/* a record type the fleet cannot carry yet - a live member does not
 * advertise it: refused, never written half-typed */
#define PC_OP_ERR_GATE    (-12)
/* an add whose sum would pass int64: refused, the counter kept (Redis's overflow error) */
#define PC_OP_ERR_OVF     (-13)
#define PC_OVF_MSG "increment or decrement would overflow"

/* the RESP door's text writer and the binary verbs' byte sink (json.h); a hash
 * reply tree is a ptree item and renders through src/ptree.h */
struct pc_jw;

/* under redis_types = strict, Redis 8's two refusals in its exact words: a string command
 * on a JSON document (WRONGTYPE), and the reverse (RedisJSON's, no code word) */
extern const char pc_wrongtype_msg[];       /* "WRONGTYPE Operation ..." */

/* Redis-style glob (* ? [abc] [a-c] [^a], backslash escapes) - KEYS'
 * matcher, for the read-through allow-list */
int pc_glob_match(const char *p, int pn, const char *s, int sn);
/* remove a key as a client delete does (table, WAL, tombstone) */
void pc_verb_remove_local(pcache_htable_t *ht, const str *k);
/* dropped HERE only (Redis changed it): table + WAL, no tombstone */
void pc_verb_forget_local(pcache_htable_t *ht, const str *k);
/* as forget_local, while the record is still at @ver or older */
void pc_verb_forget_upto(pcache_htable_t *ht, const str *k,
		unsigned long long ver);
/* store a record as a client write does (table, WAL, eager push) */
int pc_verb_store_local(pcache_htable_t *ht, const str *k, const char *v,
		size_t n, unsigned int exp, unsigned char fl);
extern const char pc_json_wrongtype_msg[];  /* "Existing key has wrong ..." */

/* SET's condition - store only when the key is absent (NX) or
 * present (XX).  Expired counts as absent, as in Redis. */
#define PC_SETCOND_NX  1
#define PC_SETCOND_XX  2

/* the probe-resume replays (proto.c): the normal op core with the probe suppressed;
 * placement decides, and may re-park on a forward (park id in *park) */
int pc_op_set_resume(const char *col, size_t collen, const char *key,
		size_t klen, const char *val, int vlen, long long ttl,
		unsigned int *park);
int pc_op_add_resume(const char *col, size_t collen, const char *key,
		size_t klen, long long by, long long ttl, long long *nv,
		unsigned int *park);

/* a forwarded re-arm applied at the holder: adopts a passive copy, WALs it whole, pushes it
 * as authored.  @exp is absolute ticks (0 = never).  1 = re-armed, 0 = absent. */
int pc_op_expire_apply(const char *col, size_t collen, const char *key,
		size_t klen, unsigned int exp);

/* a forwarded SET NX/XX decided at the deciding node; @ttl is relative seconds (0 =
 * none).  1 = stored, 0 = the condition did not hold, < 0 = PC_OP_ERR_* (not stored). */
int pc_op_setcond_apply(const char *col, size_t collen, const char *key,
		size_t klen, const char *val, size_t vlen, long long ttl,
		int cond);

/* compare-and-set / -delete (SET ... IFEQ|IFNE v, DELEX key IFEQ|IFNE v), decided where SET NX
 * is; forwarded as op PC_CMP_FWD_OP(kind), value [cmplen u32 LE][cmp][new value] */
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
/* RL.HIT rides the JSON forward plane: by = window ms, ttl = limit (have_ttl:
 * one was given), the ack's newval = the count, cnt = allowed */
#define PC_JOP_RLHIT  5
/* a hash command run on the key's deciding node: val = [argc u16][(len u32)(bytes)] x argc
 * (argv[0] the name, argv[1] the key), the ack's fragment the reply tree */
#define PC_JOP_HCMD   6
/* a hash READ in shard mode: val EMPTY, the ack the raw RECORD (st 0) or nothing (1 absent,
 * 3 other type), rendered by the asker - a record (<= PC_HR_MAX) always fits, a tree may not */
#define PC_JOP_HCMD_READ 7
/* an approved Symfony lock script on the key's deciding node: value [script u8][now_ms i64 LE,
 * -1 = the decider's clock][ttl_ms i64 LE][token]; the ack's newval 1/0, st 3 WRONGTYPE */
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
/* hits kept per rate-limit key, 8 bytes each - the whole record
 * stays under the 58,000-byte forward ceiling (PC_MAX_FWD_VAL) */
#define PC_RL_MAX     6000

/* the striped JSON read-modify-write core: any thread, the key stripe serializes; 0 ok,
 * 1 benign-absent, -1 error (*errmsg static); GET's *frag_out is borrowed until the next call */
int pc_json_rmw(pcache_htable_t *ht, const char *key, int klen, int op,
		const char *path, int plen, const char *val, int vlen,
		long long by, int have_ttl, long long ttl, int nx, int xx,
		int mkpath, char **frag_out, int *fraglen_out,
		long long *newval, int *count_out, const char **errmsg);

/* @privileged: the CONNECTION's privilege bit, in/out, passed explicitly so it can never
 * survive into the next connection on the worker; @park_req set when the answer is parked */
/* the native door's one method entry: the reply is ONE tree item in @out; -1 sets *errmsg
 * (pc_unknown_method, pointer-compared, for an unknown one); *outcome PC_OUT_HIT/_MISS */
#define PC_OUT_HIT  1
#define PC_OUT_MISS 2
int pc_verb_cmd(const char *method, size_t mlen, const struct pc_tv *v,
		int params, struct pc_tw *out, const char **errmsg,
		unsigned int *park_req, int *privileged, int *outcome,
		void *conn);
extern const char pc_unknown_method[];

/* the fleet as a tree - the `members` reply, and what /members renders as JSON */
void pc_members_tree(struct pc_tw *out);

/* the `stats` body as a tree, so the HTTP door serves the same numbers the verb
 * does; @only_col NULL = every collection */
void pc_stats_tree(struct pc_tw *out, const char *only_col);
/* the running totals start again; returns the unix time of the reset */
long long pc_stats_reset(void);
/* the same, recorded as of @at (a peer's or the schedule's moment) */
long long pc_stats_reset_at(long long at);
/* this node and every live peer, all as of this node's moment */
long long pc_stats_reset_fleet(void);
/* reset every @days days (1-7; 0 = never) at @at_s seconds past
 * local midnight; the maintenance thread calls the tick once a second */
void pc_stats_reset_schedule(int days, int at_s);
void pc_stats_reset_tick(void);

/* whether a client may create and drop collections on this node */
void pc_verb_set_allow_create(int on);
void pc_verb_set_resp_redirect(int moved);

/* the binary dialect's "unknown verb" message, exported so the
 * codec can tell THAT refusal from every other by pointer, not by text */
extern const char pc_bin_unknown_verb[];

/* the fixed-layout data verbs: @pl is one frame's payload, verb byte first; parks as
 * pc_verb_cmd does, the col/key spans POINT INTO @pl.  0, or -1 with *errmsg. */
int pc_verb_bin(const char *pl, size_t plen, struct pc_jw *out, int *flags,
		unsigned int *park_req, const char **colp, size_t *collenp,
		const char **keyp, size_t *klenp, const char **errmsg, void *conn);

/* a verb wanting a COOPERATIVE walk fills this instead of walking: proto runs it one
 * bounded chunk per event-loop turn, so the worker keeps serving its other connections */
struct pc_enum_start {
	int start;                     /* 1 = begin a chunked walk */
	void *ht;                      /* the resolved collection */
	char pat[256];
	int patlen;                    /* <0 = no pattern */
	int limit;
};

/* 1 when the RESP door implements @cmd (any case) */
int pc_verb_resp_known(const char *cmd, size_t cl);

/* the RESP codec: one framed command in, the COMPLETE reply (errors too) out; @cur_col is the
 * SELECTed collection (cap 40); *quit: flush, then close; *authed 0: AUTH/HELLO/QUIT only */
int pc_verb_resp(char *const *argv, const size_t *argl, int nargs,
		struct pc_jw *out, char *scratch, size_t scratch_cap,
		unsigned int *park_req, const char **colp, size_t *collenp,
		const char **keyp, size_t *klenp, char *cur_col,
		size_t *cur_collen, int *quit, int *authed,
		struct pc_enum_start *es, void *obs, void *conn);

/* [daemon] keys_turn_us - a walk's time budget per worker turn;
 * 0 walks the 1,024-bucket floor only */
void pc_keys_set_turn_us(int us);

/* one bounded chunk of a KEYS walk: 1024 buckets, then on while the turn has budget;
 * 1 while more remains, 0 when exhausted; w->overflow and the limit stop it early */
int pc_keys_chunk(void *ht, unsigned int *cursor, const char *pat,
		int patlen, unsigned int now, struct pc_jw *w, int *emitted,
		int limit, int *limit_hit);

/* the asker's side of a forwarded hash read: @rec the owner's record (st 0), NULL on st 1
 * (absent) or 3 (wrong type); renders as the owner would, -1 if @args do not decode */
struct pc_tw;
int pc_hcmd_render_read(const unsigned char *args, size_t argsl,
		const unsigned char *rec, size_t rlen, int st, struct pc_tw *t);
/* op_hcmd leaves a forwarded read's encoded arguments here for the door's park entry
 * (same thread); taking clears it, and the taker frees the buffer */
unsigned char *pc_hcmd_take_park_args(size_t *n);
/* a jget parked on a pull - its park arguments name this first,
 * then the key and the path */
#define PC_JGET_PARK "\001jget"
/* the pulled record's fragment at the parked path, as the owner's jget gives it: 0 (@frag
 * borrowed until the next JSON op), 1 absent, 3 wrong type, 2 an error; -1 not a jget's */
int pc_jget_render_pull(const unsigned char *args, size_t argsl,
		const char *col, size_t collen, const char *rec, size_t rlen,
		int found, long long type, char **frag, int *fraglen);
/* a JSON GET answered by a pull (a read-through key's owner, or a spread holder lacking
 * the key): 1 = parked on *@park, with @shape (RESP: 1 the root path, 2 a $ path) */
int pc_jget_park(pcache_htable_t *ht, const char *key, size_t klen,
		const char *path, size_t plen, int shape, unsigned int *park);
/* a parked jget's shape, -1 if @args are not one's */
int pc_jget_park_shape(const unsigned char *args, size_t argsl);
/* JSON.GET's RESP reply: @st 0 the fragment, 1 absent, 2 no answer,
 * 3 not a document */
void pc_resp_jget_reply(struct pc_jw *out, int st, const char *frag,
		int fraglen, int rootpath, int v2);
#endif /* PC_VERBS_H */

/* wrong enable secrets per source address: after this many within the window every
 * enable from it is refused for the window; 0 = off */
extern int pc_enable_lockout_after;
extern int pc_enable_lockout_s;
