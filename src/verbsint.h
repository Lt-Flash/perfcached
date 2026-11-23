/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* verbsint.h - verbs layer internals shared with the RESP family modules: the
 * handler context and the helpers verbs.c owns; daemon-internal */
#ifndef PC_VERBSINT_H
#define PC_VERBSINT_H

#include <stddef.h>

#include "core/pcache_htable.h"     /* pcache_htable_t */
#include "core/pcache_arena.h"      /* PCACHE_CELL_MAX */

struct pc_jw;
struct pc_enum_start;
struct pc_member;
struct pc_tw;

#define VAL_MAX     PCACHE_CELL_MAX          /* largest storable value */
#define KEY_MAX     4096
/* the longest relative TTL (s): with it, the 32-bit tick expiry stays unwrapped for 68 years
 * of uptime; a longer one is refused as Redis refuses an invalid expire */
#define PC_TTL_MAX  0x7FFFFFFFLL
#define PC_TTL_MSG  "invalid expire time - a ttl is at most 2147483647 seconds"
#define JP_NAME_PARAM 512                    /* longest path string */
/* every door's read and write of a key notes it for the hot-key sampler
 * (store.c); the cores carry it, so one hook each covers them all */
#define HOT(ht, s, l) pc_store_hot_note((ht), (s), (size_t)(l))

/* everything a handler may need from the connection, filled once at the top
 * of pc_verb_resp */
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
	void *conn;                    /* the connection, opaque */
};

/* ---- owned by verbs.c --------------------------------------------------- */

/* this thread's member-list buffer, PC_CL_MAXMEMBERS entries, made on first
 * use and never freed (a worker thread's) */
struct pc_member *members_scratch(void);
/* a subcommand this door does not implement, counted by name (a worker
 * thread's) */
void unk_sub(const struct resp_ctx *c);
/* is this a cluster - INFO server, INFO cluster and CLUSTER INFO all answer
 * from this one (any thread) */
int resp_cluster_on(void);

/* the data cores both doors answer through, so the dialects cannot drift (a worker
 * thread's); *park set when the answer waits for a peer or the upstream */
int op_get_buf(pcache_htable_t *ht, str *k, char *buf, unsigned int cap,
		unsigned int *vlen, unsigned int *exp, unsigned int *park);
/* a get's lookup of a key the local table lacks: PC_OP_PARKED or ABSENT */
int op_get_miss(pcache_htable_t *ht, str *k, unsigned int *park);
/* mget across owners (mgetfan.h): mget_fans - may a local miss be looked up
 * elsewhere; op_mget_fan: 0 with *@mp, -1 refused with RESP code word and text */
struct pc_mget;
int mget_fans(pcache_htable_t *ht);
/* @count (EXISTS): each key's presence, any type, answered as the number */
int op_mget_fan(pcache_htable_t *ht, const char *const *keys,
		const size_t *klens, unsigned int n, int count, struct pc_mget **mp,
		const char **code, const char **why);
int op_set(pcache_htable_t *ht, str *k, str *v, long long ttl,
		unsigned int *park, int probed);
int op_setcond(pcache_htable_t *ht, str *k, str *v, long long ttl, int cond,
		unsigned int *park);
int op_cmp(pcache_htable_t *ht, str *k, const char *cmp, size_t cmplen,
		str *v, long long ttl, int kind, unsigned int *park);
int op_del(pcache_htable_t *ht, str *k, unsigned int *park);
int op_addsub(pcache_htable_t *ht, str *k, long long by, long long ttl,
		long long *nv, unsigned int *park, int probed);
int op_expire(pcache_htable_t *ht, str *k, long long ttl, unsigned int *park);
int op_rlhit(pcache_htable_t *ht, str *k, long long window_ms, long long limit,
		long long *count, int *allowed, unsigned int *park, int *lacking);
/* an approved Symfony lock script run as C (PC_SLOCK_*): PC_OP_OK with *result,
 * PARKED forwarded to the deciding node, PC_OP_ERR_GATE with *lacking */
int op_slock(pcache_htable_t *ht, str *k, int script, long long now_arg,
		long long ttl_ms, const char *tok, size_t toklen, int *result,
		unsigned int *park, int *lacking);
int probe_or_present(pcache_htable_t *ht, const str *k, unsigned int *vln,
		unsigned int *exp, int *is_ctr, unsigned char *tflags);
/* this worker's value buffer (VAL_MAX), made on first use */
char *get_buf(void);

/* the RESP door's collection for the connection's DB index: NULL with the
 * error written when there is none */
pcache_htable_t *resp_col(struct pc_jw *out, const char *cur_col,
		size_t cur_collen);
int resp_col_allowed(const char *name, size_t n);
size_t resp_map_index(const char *sel, size_t sl, char *out, size_t cap);
/* the index a RESP client selects to reach @name (INFO keyspace); a collection
 * with no index entry is reported under its own name */
size_t resp_index_for(const char *name, size_t nl, char *out, size_t cap);

/* the collection a table belongs to, by name (the WAL logs by name) */
const char *col_name_of(pcache_htable_t *ht);
/* a JSON path error's text (PC_JP_E_*): the same pointer for the same code,
 * so a caller may compare against it */
const char *jp_strerror(int rc);

/* the ENABLE guard's tallies - wrong secrets, lockouts applied, enables
 * refused while locked out (stats' resp block) */
unsigned long long pc_verb_enable_fails(void);
unsigned long long pc_verb_enable_lockouts(void);
unsigned long long pc_verb_enable_refused(void);

/* the refusal texts the gates write */
const char *rl_gate_text(int lacking);
const char *wr_refused_msg(void);
extern const char cmp_mode_msg[], delex_type_msg[], rl_mode_msg[],
	setcond_mode_msg[];
extern const char slock_gate_fmt[], slock_mode_msg[];

/* ---- statstree.c -------------------------------------------------------- */

/* the DB indexes the RESP door answers, one row each - stats' resp.map and
 * the maps verb */
void respmap_rows(struct pc_tw *out);

/* ---- the families' handlers, in verbs.c's command table ----------------- */

/* respcl.c: CLUSTER INFO / MYID / NODES / SLOTS / SHARDS / KEYSLOT - a worker
 * thread's */
int rh_cluster(struct resp_ctx *c);
/* resppubsub.c: SUBSCRIBE / PSUBSCRIBE / UNSUBSCRIBE / PUNSUBSCRIBE / PUBLISH /
 * PUBSUB - a worker thread's */
int rh_subscribe(struct resp_ctx *c);
int rh_psubscribe(struct resp_ctx *c);
int rh_unsubscribe(struct resp_ctx *c);
int rh_punsubscribe(struct resp_ctx *c);
int rh_publish(struct resp_ctx *c);
int rh_pubsub(struct resp_ctx *c);
/* respkv.c: the key-value and connection commands - a worker thread's */
int rh_reset(struct resp_ctx *c);
int rh_echo(struct resp_ctx *c);
int rh_quit(struct resp_ctx *c);
int rh_command(struct resp_ctx *c);
int rh_config(struct resp_ctx *c);
int rh_dbsize(struct resp_ctx *c);
int rh_flush(struct resp_ctx *c);
int rh_client(struct resp_ctx *c);
int rh_slowlog(struct resp_ctx *c);
int rh_select(struct resp_ctx *c);
int rh_memory(struct resp_ctx *c);
int rh_keys(struct resp_ctx *c);
int rh_exists(struct resp_ctx *c);
int rh_type(struct resp_ctx *c);
int rh_ttl(struct resp_ctx *c);
int rh_mget(struct resp_ctx *c);
int rh_get(struct resp_ctx *c);
int rh_delex(struct resp_ctx *c);
int rh_del(struct resp_ctx *c);
int rh_expire(struct resp_ctx *c);
int rh_set(struct resp_ctx *c);
int rh_rlhit(struct resp_ctx *c);
int rh_incr(struct resp_ctx *c);
/* respjson.c: JSON.SET / GET / DEL / NUMINCRBY / ARRAPPEND / DEBUG */
int rh_json(struct resp_ctx *c);
int rh_json_debug(struct resp_ctx *c);
/* respscript.c: EVAL / EVALSHA / SCRIPT - the approved Symfony scripts only */
int rh_eval(struct resp_ctx *c);
int rh_script(struct resp_ctx *c);
/* respsrv.c: the connection and server commands - PING, AUTH, HELLO, INFO,
 * TIME */
int rh_ping(struct resp_ctx *c);
int rh_auth(struct resp_ctx *c);
int rh_hello(struct resp_ctx *c);
int rh_info(struct resp_ctx *c);
int rh_time(struct resp_ctx *c);
/* respscan.c: SCAN (KEYS walks through pc_keys_chunk, verbs.h) */
int rh_scan(struct resp_ctx *c);

#endif
