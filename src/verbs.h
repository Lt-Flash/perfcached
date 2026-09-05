/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * verbs.h — the v1 verb set, text (JSON-RPC) dialect (task S8).
 *
 * pc_verb_text() takes a parsed JSON-RPC request and writes the VALUE of
 * the "result" member into @out (the caller wraps the JSON-RPC
 * envelope).  On a verb-level failure it returns a JSON-RPC error code
 * and points *errmsg at a static message instead.
 *
 * Verbs: get set del exists expire ttl add sub mget mset stats ping.
 * pc_verb_bin() is the binary codec for the DATA verbs (layouts in
 * proto.h); both codecs answer through the same dialect-neutral op
 * cores, so cluster semantics cannot drift between them.
 */
#ifndef PC_VERBS_H
#define PC_VERBS_H

#include "json.h"
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

/* the probe-resume replays (proto.c): the write runs the normal op
 * core with the probe suppressed - placement decides, and may re-park
 * on a forward (park id in *park). */
int pc_op_set_resume(const char *col, size_t collen, const char *key,
		size_t klen, const char *val, int vlen, long long ttl,
		unsigned int *park);
int pc_op_add_resume(const char *col, size_t collen, const char *key,
		size_t klen, long long by, long long ttl, long long *nv,
		unsigned int *park);

/* JSON path op codes - shared by the verb layer and the cluster
 * forward plane (M_FWD_JSON carries one) */
#define PC_JOP_SET    0
#define PC_JOP_DEL    1
#define PC_JOP_INCR   2
#define PC_JOP_APPEND 3
#define PC_JOP_GET    4

/* the striped JSON read-modify-write core (verbs.c) - callable from
 * any thread; the key stripe serializes.  0 ok, 1 benign-absent,
 * -1 error (*errmsg static).  GET: *frag_out malloc'd. */
int pc_json_rmw(pcache_htable_t *ht, const char *key, int klen, int op,
		const char *path, int plen, const char *val, int vlen,
		long long by, int have_ttl, long long ttl, int nx, int xx,
		int mkpath, char **frag_out, int *fraglen_out,
		long long *newval, int *count_out, const char **errmsg);

/* @line/@toks/@ntok: the parsed request.  @method_tok: the method string
 * token.  @params_tok: the params object token index, or -1 if absent.
 * @out: result-value writer.  Returns 0 (result written) or a negative
 * JSON-RPC error code with *errmsg set. */
/* @park_req: set nonzero when the answer is deferred to a cluster pull
 * (the caller parks the request; nothing is written to @out) */
int pc_verb_text(const char *line, const struct pc_jtok *toks, int ntok,
		int method_tok, int params_tok, struct pc_jw *out,
		const char **errmsg, unsigned int *park_req);

/* the binary codec: @pl/@plen is one request frame's payload (verb
 * byte first).  Writes the response payload into @out (raw bytes) and
 * may set *flags (PC_BIN_F_ERR).  Parking mirrors pc_verb_text: sets
 * *park_req, pre-writes the park-failure shape, and reports the
 * col/key spans (POINTERS INTO @pl) for the park table.  Returns 0, or
 * -1 with *errmsg set (the proto layer sends an error frame). */
/* S37: the fleet as JSON - shared by the members verb and /members */
void pc_members_json(struct pc_jw *out);

/* S67: the `stats` body, so the HTTP door can serve the same numbers
 * the verb does.  @only_col NULL = every collection. */
void pc_stats_json(struct pc_jw *out, const char *only_col);

int pc_verb_bin(const char *pl, size_t plen, struct pc_jw *out, int *flags,
		unsigned int *park_req, const char **colp, size_t *collenp,
		const char **keyp, size_t *klenp, const char **errmsg);

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
		struct pc_enum_start *es, void *obs);

/* one bounded chunk of a KEYS walk: up to 1024 buckets.  Returns 1
 * while the walk has more, 0 when the table is exhausted; *emitted
 * accumulates, w->overflow and the limit stop it early. */
int pc_keys_chunk(void *ht, unsigned int *cursor, const char *pat,
		int patlen, unsigned int now, struct pc_jw *w, int *emitted,
		int limit, int *limit_hit);

#endif /* PC_VERBS_H */
