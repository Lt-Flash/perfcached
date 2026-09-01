/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov
 * Part of libperfd - see lib/LICENSE.  This file must stay
 * free of src/core includes; tools/sync-libperfd.sh exports
 * exactly the MIT set to consumers. */
/*
 * perfd.h — libperfd, the perfcached C client (the hiredis analogue).
 *
 * One connection object, blocking calls, one thread per connection
 * (exactly hiredis's contract).  Typed helpers cover the verb set; the
 * JSON escape hatch covers everything else; a pipeline API queues
 * requests and delivers replies IN REQUEST ORDER even though the
 * daemon answers out of order (matched by id internally).
 *
 * Also hiredis's OTHER contract: an event-loop surface (task S32) for
 * consumers that own a loop and cannot block in it.  It is additive and
 * opt-in - see "event-loop surface" at the bottom of this header - and
 * the blocking calls are unchanged by its existence.
 *
 * Transport: the text dialect (newline JSON-RPC) - like RESP, a text
 * wire behind a typed API - over TCP or a unix socket, plaintext or
 * the Noise channel (NNpsk0, client principal).  perfd_opts.secrets is
 * a LIST tried in order, so secret rotation is add-new/drain-old with
 * no client downtime.  Values are binary-safe both ways (the library
 * handles the base64 leg of the wire when bytes demand it).
 *
 * perfd_opts.binary = 1 switches the DATA verbs (get set del exists
 * ttl expire add sub ping) to the binary frame dialect on the same
 * connection: raw value bytes, no JSON/b64 leg either way.  Every
 * other call (mget, keys, JSON path verbs, perfd_command, the
 * pipeline API) keeps the text dialect - the daemon dispatches per
 * message, so the two mix freely on one connection and one id space.
 * The typed API is IDENTICAL either way; only the wire changes.
 *
 * Errors: calls return -1 (or NULL) and perfd_error() holds a static
 * message valid until the next call on the same connection.  A
 * transport failure poisons the connection: every later call fails
 * fast until perfd_free().
 *
 * Dependencies: libsodium (the daemon's own rule).  Link:
 *     cc app.c libperfd.a -lsodium -lpthread
 */
#ifndef PERFD_H
#define PERFD_H

#include <stddef.h>

/* the library's version - semver, independent of the daemon's (this
 * archive ships into other codebases and moves on its own cadence).
 * PERFD_VERSION is what you compiled against; perfd_version() is what
 * you linked - compare them to catch a stale libperfd.a. */
#define PERFD_VERSION_MAJOR 0
#define PERFD_VERSION_MINOR 2
#define PERFD_VERSION_PATCH 0
#define PERFD_VERSION "0.2.0"

const char *perfd_version(void);

typedef struct perfd perfd_t;

/* How a client picks WHICH node it works through (task S34).  The
 * choice is per CONNECTION: every policy ends with one active
 * connection plus pre-warmed standbys, so a failure is a swap, not a
 * reconnect. */
enum perfd_policy {
	PERFD_POLICY_FAILOVER = 0,     /* stay where you connected; spares
	                                * exist only to take over (default:
	                                * upgrading a client cannot silently
	                                * redistribute a live workload) */
	PERFD_POLICY_ROUND_ROBIN,      /* independent random start per
	                                * client, then step - 1000 clients
	                                * spread with zero coordination */
	PERFD_POLICY_LEAST_CONN,       /* the member reporting the most free
	                                * arena, i.e. the least loaded one */
	PERFD_POLICY_WEIGHTED          /* random, weighted by free bytes */
};

typedef struct perfd_opts {
	const char *const *secrets;    /* NULL-terminated list; NULL/empty =
	                                * plaintext (loopback listeners) */
	int connect_timeout_ms;        /* 0 = 5000 */
	int io_timeout_ms;             /* 0 = 5000; per send/recv */
	int binary;                    /* 1 = data verbs ride the binary
	                                * frames (see the header comment) */
	/* ---- cluster awareness (S34) ----
	 * With spares > 0 the library learns the fleet on connect and keeps
	 * standby connections open, so a node failure costs a swap instead
	 * of a TCP+Noise handshake at the worst possible moment. */
	int policy;                    /* enum perfd_policy */
	int spares;                    /* standby connections to keep; 0 =
	                                * off (today's behaviour), -1 = one
	                                * per other member.  Capped so a big
	                                * fleet x many clients cannot become
	                                * a socket storm. */
	int refresh_ms;                /* member-list refresh; 0 = 30000 */
	int route_keys;                /* 1 = send each request to the node
	                                * that should hold its key (S35).
	                                * OFF by default, for the same
	                                * reason the default policy is
	                                * failover: upgrading a library must
	                                * not silently change where a live
	                                * workload lands.  Needs spares. */
	int eager_push;                /* async: write inside every
	                                * perfd_submit() instead of batching.
	                                *
	                                * DEFAULT IS BATCHING (this field 0).
	                                * A submit queues; the bytes leave on
	                                * the next write-readiness, or at once
	                                * if you call perfd_push().  Writing
	                                * per submit costs one write() syscall
	                                * per request - a depth of 64 pays 64
	                                * where one would do, and a pipelined
	                                * client is then mostly kernel time.
	                                * Measured 1.82x throughput and 3.9x
	                                * less client CPU per op from batching
	                                * alone.
	                                *
	                                * Batching is safe as the default
	                                * because perfd_events() reports
	                                * PERFD_EV_WRITE whenever anything is
	                                * queued, and perfd_write_ready()
	                                * pushes it - so a loop that honours
	                                * perfd_events (which the async
	                                * contract already requires) flushes
	                                * on its next turn.  The cost is at
	                                * most one loop iteration of latency,
	                                * not a stall.
	                                *
	                                * Set this to 1 only if you submit
	                                * without ever returning to your event
	                                * loop and cannot call perfd_push. */
} perfd_opts;

/* connect (TCP / unix socket).  NULL on failure - call perfd_error(NULL)
 * for the reason.  @opts may be NULL for all-defaults plaintext. */
perfd_t *perfd_connect(const char *host, int port, const perfd_opts *opts);
perfd_t *perfd_connect_unix(const char *path, const perfd_opts *opts);
void perfd_free(perfd_t *p);

/* the last error on @p (or on the most recent failed connect if NULL) */
const char *perfd_error(const perfd_t *p);

/* ---- cluster awareness (S34) -------------------------------------------
 * Introspection, so a caller can SEE what the library is doing rather
 * than infer it. */

/* members the client learned (0 when the daemon is standalone) */
int perfd_member_count(const perfd_t *p);
/* member @i: address/port/node id, and whether it is the active one.
 * Any out pointer may be NULL.  0 ok, -1 = no such member. */
int perfd_member_info(const perfd_t *p, int i, char *addr, size_t acap,
		int *port, int *node, int *active);
/* standby connections currently held open */
int perfd_spare_count(const perfd_t *p);
/* how many times this handle has swapped onto a standby */
unsigned long long perfd_failovers(const perfd_t *p);
/* the node id currently serving this handle (0 = unknown/standalone) */
int perfd_active_node(const perfd_t *p);

/* ---- per-key routing (S35) ---------------------------------------------
 * When the cluster reports an owner-selection algorithm this client
 * implements, and a mode where an owner means something (shard, and
 * store for write-stickiness), each request goes to the node that
 * should hold its key - removing the daemon's forward hop.
 *
 * It is never load-bearing: the daemon re-checks ownership and
 * forwards a wrong guess, so a stale view costs a hop, not
 * correctness.  Requires standbys (opts.spares) - routing can only use
 * connections that are already open. */

/* 1 = this handle is routing by key */
int perfd_routing(const perfd_t *p);
/* requests whose owner was known but not connected (they went to the
 * active node and paid a forward) - raise opts.spares if this grows */
unsigned long long perfd_route_missed(const perfd_t *p);

/* Which member should hold @key: an index for perfd_member_info(), or
 * -1 when routing does not apply.
 *
 * ASYNC CALLERS NEED THIS.  The library's own routing needs standby
 * CONNECTIONS, and it cannot open them behind an async caller's back -
 * that caller drives one fd per handle and would never poll them.  So
 * on the async API the library learns the fleet and computes owners,
 * and the APPLICATION opens a handle per node and picks with this.  It
 * is the same computation the blocking path uses internally, so the two
 * agree on where a key belongs.
 *
 * Requires the handle to have learned the member list: pass
 * opts.route_keys (and opts.spares for the blocking path). */
int perfd_owner_of(perfd_t *p, const char *col, const char *key);

/* ---- typed verbs -------------------------------------------------------
 * Return contract: -1 = error (perfd_error set); otherwise the
 * documented value.  Out-buffers are malloc'd, caller frees. */

/* 0 ok */
int perfd_set(perfd_t *p, const char *col, const char *key,
		const void *val, size_t vlen, long long ttl);

/* 1 found (val/vlen/ttl_out filled; *ttl_out -1 = no expiry), 0 miss */
int perfd_get(perfd_t *p, const char *col, const char *key,
		void **val, size_t *vlen, long long *ttl_out);

/* 1 deleted, 0 was absent */
int perfd_del(perfd_t *p, const char *col, const char *key);

/* 1 exists, 0 not */
int perfd_exists(perfd_t *p, const char *col, const char *key);

/* seconds left; -1 = no expiry; -2 = no such key */
long long perfd_ttl(perfd_t *p, const char *col, const char *key);

/* 0 ok (key existed), 1 no such key */
int perfd_expire(perfd_t *p, const char *col, const char *key,
		long long ttl);

/* counters: 0 ok, *newval = the resulting value */
int perfd_add(perfd_t *p, const char *col, const char *key, long long by,
		long long ttl, long long *newval);
int perfd_sub(perfd_t *p, const char *col, const char *key, long long by,
		long long *newval);

/* 0 ok; values[i] NULL = miss (else malloc'd, vlens[i] set).
 * Arrays are caller-provided, nkeys wide. */
int perfd_mget(perfd_t *p, const char *col, const char *const *keys,
		int nkeys, void **values, size_t *vlens);

/* keys matching @match (NULL = all), up to @limit (0 = server default).
 * Returns the count and fills *keys_out with a malloc'd array of
 * malloc'd strings - free with perfd_free_keys.  -1 on error. */
int perfd_keys(perfd_t *p, const char *col, const char *match, int limit,
		char ***keys_out);
void perfd_free_keys(char **keys, int n);

/* JSON path verbs: raw JSON fragments in and out (malloc'd out).
 * perfd_jget: 1 found, 0 miss.  perfd_jset: 0 ok.  perfd_jincr: 0 ok,
 * *newval = result.  perfd_jdel: 1 deleted, 0 absent. */
int perfd_jget(perfd_t *p, const char *col, const char *key,
		const char *path, char **frag_out);
int perfd_jset(perfd_t *p, const char *col, const char *key,
		const char *path, const char *json_val, long long ttl);
int perfd_jdel(perfd_t *p, const char *col, const char *key,
		const char *path);
int perfd_jincr(perfd_t *p, const char *col, const char *key,
		const char *path, long long by, long long *newval);

/* 0 ok (round trip proven) */
int perfd_ping(perfd_t *p);

/* the escape hatch: any method + params (params_json may be NULL).
 * Returns the malloc'd result JSON, or NULL with the error member's
 * message in perfd_error().  Admin verbs ride here too - e.g.
 * perfd_command(p, "probe", NULL) re-measures the WAL storage
 * (iops/latency at 4K QD1 + seq bandwidth; blocks for the probe). */
char *perfd_command(perfd_t *p, const char *method,
		const char *params_json);

/* ---- pipelining --------------------------------------------------------
 * perfd_append queues without touching the socket; perfd_flush writes
 * the batch; perfd_next_reply returns each result IN REQUEST ORDER
 * (malloc'd result JSON, or NULL with perfd_error set for that
 * request's error member).  Mixing typed calls between append and the
 * final next_reply is refused. */
int perfd_append(perfd_t *p, const char *method, const char *params_json);
int perfd_flush(perfd_t *p);
char *perfd_next_reply(perfd_t *p);
int perfd_pending(const perfd_t *p);   /* replies not yet collected */

/* id-less server notifications (membership, steering) are skipped by
 * default; set a hook to see them (payload = the raw JSON line, valid
 * only during the call) */
typedef void (*perfd_notify_cb)(const char *json, size_t len, void *ctx);
void perfd_set_notify(perfd_t *p, perfd_notify_cb cb, void *ctx);

/* ---- event-loop surface (S32) ------------------------------------------
 * An ADDITIVE, opt-in surface beside the blocking calls above, which keep
 * working byte for byte.  It exists because a consumer that already owns
 * an event loop - rtpengine drives its storage from libevent - cannot
 * afford a blocking call: one stall there is every call on the box.
 *
 * The library imposes no threading model and never calls back from a
 * thread of its own.  Everything happens inside the four entry points
 * below, on the caller's thread, when the caller says so.
 *
 * Shape:  perfd_connect_async() -> watch perfd_fd() for perfd_events()
 *         -> perfd_read_ready()/perfd_write_ready() on readiness
 *         -> perfd_state() reaches PERFD_ST_READY -> perfd_submit().
 * Replies arrive at their per-request callback IN ARRIVAL ORDER, which
 * is the point: a slow KEYS cannot hold up the gets behind it.
 */
enum perfd_state {
	PERFD_ST_CONNECTING = 0,       /* TCP and/or Noise handshake running */
	PERFD_ST_READY,                /* requests may be submitted */
	PERFD_ST_FAILED                /* poisoned; perfd_error() says why */
};

#define PERFD_EV_READ   0x1
#define PERFD_EV_WRITE  0x2

/* the socket for the loop to watch.  -1 if the handle has none. */
int perfd_fd(const perfd_t *p);

/* which readiness the handle wants watched RIGHT NOW - re-read after
 * every entry point, because a partial write turns PERFD_EV_WRITE on and
 * a drained buffer turns it off. */
int perfd_events(const perfd_t *p);

/* connection progress; see enum perfd_state */
int perfd_state(const perfd_t *p);

/* Non-blocking connect: returns a handle immediately, in
 * PERFD_ST_CONNECTING.  The TCP connect and the Noise handshake are then
 * driven by the readiness callbacks - no blocking call anywhere on the
 * path, which is what lets a consumer RECONNECT from inside its loop.
 * NULL only if the socket could not be created at all. */
perfd_t *perfd_connect_async(const char *host, int port,
		const perfd_opts *opts);

/* Per-request completion.  @result is malloc'd and the callback OWNS it
 * (free it); @result NULL means the request failed and @errmsg says why.
 * @len is the result length (results are NUL-terminated JSON in the text
 * dialect, raw bytes in the binary one). */
typedef void (*perfd_reply_cb)(char *result, size_t len, const char *errmsg,
		void *ctx);

/* Queue a request and write as much as the socket will take; never
 * blocks.  0 ok, -1 = error (perfd_error set).  Legal only in
 * PERFD_ST_READY. */
int perfd_submit(perfd_t *p, const char *method, const char *params_json,
		perfd_reply_cb cb, void *ctx);

/* Call when the loop reports the fd readable / writable.  0 ok, -1 =
 * the connection failed (state becomes PERFD_ST_FAILED; every in-flight
 * callback is invoked with an error first, so a consumer never loses
 * track of a request it issued). */
int perfd_read_ready(perfd_t *p);
int perfd_write_ready(perfd_t *p);

/* requests submitted whose callback has not fired yet */
int perfd_inflight(const perfd_t *p);

/* Write whatever perfd_submit() queued.  Only needed with
 * Submits are BATCHED by default, so this is how you make them leave
 * NOW rather than on the next write-readiness.  0 ok (or nothing pending),
 * -1 = the connection failed.  A partial write leaves the rest queued
 * and turns PERFD_EV_WRITE on, exactly as submit does.
 *
 * NOT perfd_flush(): that name is the SYNC pipeline's
 * (perfd_append/perfd_flush/perfd_next_reply) and uses a different
 * queue. */
int perfd_push(perfd_t *p);

/* Data verbs for perfd_submit_kv(), matching the daemon's wire (see
 * src/proto.h - the daemon is the authority). */
#define PERFD_V_PING    1
#define PERFD_V_GET     2
#define PERFD_V_SET     3
#define PERFD_V_DEL     4
#define PERFD_V_EXISTS  5
#define PERFD_V_TTL     6
#define PERFD_V_EXPIRE  7
#define PERFD_V_ADD     8
#define PERFD_V_SUB     9

/*
 * Async submit for a data verb over the BINARY dialect.
 *
 * perfd_submit() takes its params as a JSON string and always frames
 * the request as JSON-RPC text, whatever opts.binary says - so before
 * this existed, an async caller could not reach the binary dialect at
 * all, and paid a JSON parse per reply for the privilege.  Measured on
 * a pipelined GET workload, JSON handling was ~20% of the client's CPU
 * while it believed it was running binary.
 *
 * Takes the fields directly, so nothing is formatted or parsed as JSON
 * on either leg.  @val is the value for SET (and the echo payload for
 * PING); @by is the delta for ADD/SUB; @ttl is seconds for SET/EXPIRE.
 * Pass 0/NULL for what a verb does not use.
 *
 * Same rules as perfd_submit otherwise: legal only in PERFD_ST_READY,
 * the callback owns @result and frees it, and submits are batched -
 * they leave on the next write-readiness, or immediately if you call
 * perfd_push().
 */
int perfd_submit_kv(perfd_t *p, int verb, const char *col, const char *key,
		const void *val, size_t vlen, long long by, long long ttl,
		perfd_reply_cb cb, void *ctx);

#endif /* PERFD_H */
