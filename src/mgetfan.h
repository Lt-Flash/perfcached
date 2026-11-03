/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * mgetfan.h - G8 (DESIGN 12nh): one client mget waiting on the remote
 * lookups of the keys this node does not hold, all at once, under one
 * deadline.
 *
 * The verb (native mget, RESP MGET) reads every key from the local table;
 * each miss is looked up the way a single get looks it up (op_get_miss:
 * the shard owner, spread's best holder, the proxy locator, a broadcast,
 * eager only while unsettled).  When a lookup was issued, the mget parks
 * here: the door attaches the connection, each pull's answer settles its
 * key, and the last one hands the mget back to the door to answer.
 *
 * A key ends in one of four states: a hit (the value), absent (the fleet
 * said so), or unanswered (its lookup did not answer by the deadline, or
 * could not be asked again).  The native reply says found / value / ttl
 * per key and "unanswered": true where it applies; RESP answers all or
 * nothing - an array when every key was answered, else -TRYAGAIN.
 *
 * Worker-thread state, like the park table: an mget lives and completes
 * on the worker that took it.
 */
#ifndef PC_MGETFAN_H
#define PC_MGETFAN_H

#include <stddef.h>
#include <stdint.h>

struct pc_pull_done;
struct pc_tw;
struct pc_jw;
struct pc_mget;

/* the most remote lookups one mget may wait on; more is refused - split
 * the mget */
#define PC_MGET_MAX_REMOTE 1000

#define PC_MK_ABSENT      0
#define PC_MK_HIT         1
#define PC_MK_WAIT        2
#define PC_MK_UNANSWERED  3

#define PC_MGET_DOOR_CMD  1            /* a CMD frame: a tree, by frame id */
#define PC_MGET_DOOR_RESP 2            /* RESP: an array, in pipeline order */

/* an mget of @n keys of collection @col; NULL on no memory.  Its deadline
 * is a pull's, begun now. */
struct pc_mget *pc_mget_new(const char *col, size_t collen, unsigned int n);

/* key @i settled locally: a hit (@val malloc'd - the mget takes it, frees
 * it), or absent */
void pc_mget_hit(struct pc_mget *m, unsigned int i, char *val, int vlen,
		unsigned int exp);
void pc_mget_absent(struct pc_mget *m, unsigned int i);

/* key @i waits on pull @req; the key's bytes are copied.  0, or a
 * refusal with its text in *@why: PC_MGET_E_CAP - it would wait on more
 * than PC_MGET_MAX_REMOTE (split it; a retry cannot help), PC_MGET_E_BUSY -
 * the pending table is down to its reserve, a quarter kept for every other
 * request (retry, or split it), PC_MGET_E_NOMEM.  The caller cancels @req
 * and refuses the mget. */
#define PC_MGET_E_CAP   (-1)
#define PC_MGET_E_BUSY  (-2)
#define PC_MGET_E_NOMEM (-3)
int pc_mget_wait(struct pc_mget *m, unsigned int i, const char *key,
		size_t klen, uint32_t req, const char **why);

/* the lookups it waits on; its deadline (the monotonic ms clock) */
unsigned int pc_mget_waiting(const struct pc_mget *m);
long long pc_mget_deadline(const struct pc_mget *m);

/* the reply, in the door's shape: {values: [...]} into @w, or a RESP
 * array (-TRYAGAIN when a key is unanswered) */
void pc_mget_tree(const struct pc_mget *m, struct pc_tw *w);
void pc_mget_resp(const struct pc_mget *m, struct pc_jw *w);

/* done with it: lookups still waiting are cancelled, everything freed */
void pc_mget_free(struct pc_mget *m);

/* ---- the park --------------------------------------------------------- */

/* the verb hands a waiting mget to its door through this thread: _park
 * by the verb, _take by the door right after (NULL: nothing parked) */
void pc_mget_park(struct pc_mget *m);
struct pc_mget *pc_mget_take(void);
/* whether one is parked and not taken yet - a verb that would run a
 * command again (S435) must not, its reply is the mget's */
int pc_mget_parked(void);

/* the door: whose it is - the connection, the door, a CMD frame's 8 id
 * bytes as they sat on the wire (NULL for RESP).  0, or -1 on no memory
 * (the caller answers an error and frees it) */
int pc_mget_attach(struct pc_mget *m, void *conn, int door, const char *id);
void *pc_mget_conn(const struct pc_mget *m);
int pc_mget_door(const struct pc_mget *m);
const char *pc_mget_id(const struct pc_mget *m);

/* a pull's answer: 0 when it was not an mget's, 1 when it was (taken).
 * *@done is set to the mget when that was its last key - the door answers
 * it and frees it */
int pc_mget_pull_done(const struct pc_pull_done *d, struct pc_mget **done);

/* a connection closed: its mgets are dropped, their lookups cancelled */
void pc_mget_drop_conn(void *conn);

/* ---- counters (any thread) ------------------------------------------- */

/* mgets that waited on remote lookups; of them, the ones that ended with
 * a key unanswered; mgets refused by the cap or the pending-table guard.
 * @since_reset: counted from the last stats reset (/stats), or from the
 * start (/metrics - a counter never goes back) */
void pc_mget_stats(int since_reset, unsigned long long *fanout,
		unsigned long long *unanswered, unsigned long long *refused);
void pc_mget_stats_reset(void);
void pc_mget_count_refused(void);

#endif /* PC_MGETFAN_H */
