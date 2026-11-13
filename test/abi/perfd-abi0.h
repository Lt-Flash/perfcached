/* SPDX-License-Identifier: MIT
 * FROZEN: libperfd.h as of libperfd.so.0 (libperfd 0.5.0), the ABI baseline test/abitest.sh
 * compiles an old caller against; a new SONAME freezes a new copy beside it.
 * Copyright (c) 2026 Yury Kirsanov
 * Part of libperfd - see lib/LICENSE.  This file must stay
 * free of src/core includes; tools/sync-libperfd.sh exports
 * exactly the MIT set to consumers. */
/* perfd.h - libperfd, the perfcached C client: blocking calls, one thread per connection, and
 * an opt-in event-loop surface at the bottom.  Link: libperfd.a -lsodium -lpthread */
#ifndef PERFD_H
#define PERFD_H

/* a fallible mutating call whose ignored return is a bug; self-contained because this
 * header ships alone */
#if !defined(PERFD_MUST_CHECK)
#if defined(__GNUC__) || defined(__clang__)
#define PERFD_MUST_CHECK __attribute__((warn_unused_result))
#else
#define PERFD_MUST_CHECK
#endif
#endif

#include <stddef.h>

/* the library's semver, independent of the daemon's: PERFD_VERSION is what you compiled
 * against, perfd_version() what you linked - compare them to catch a stale libperfd.a */
#define PERFD_VERSION_MAJOR 0
#define PERFD_VERSION_MINOR 5
#define PERFD_VERSION_PATCH 0
#define PERFD_VERSION "0.5.0"
/* the oldest daemon this binary wire works with: an older one answers the first request
 * with "unknown verb", which perfd_error() names */
#define PERFD_NEEDS_PERFCACHED "0.4.5"

/* the first library that routes `mode = spread`; an older one turns routing off and stays
 * correct (the daemon forwards), paying a pull on most reads */
#define PERFD_SPREAD_ROUTING_SINCE "0.2.8"

const char *perfd_version(void);

typedef struct perfd perfd_t;

/* how a handle picks the node it works through: one active connection plus pre-warmed
 * standbys, so a failure is a swap, not a reconnect */
enum perfd_policy {
	PERFD_POLICY_FAILOVER = 0,     /* stay where you connected; the default, so an
	                                * upgrade cannot redistribute a live workload */
	PERFD_POLICY_ROUND_ROBIN,      /* random start per client, then step: clients
	                                * spread with zero coordination */
	PERFD_POLICY_LEAST_CONN,       /* the member reporting the most free
	                                * arena, i.e. the least loaded one */
	PERFD_POLICY_WEIGHTED          /* random, weighted by free bytes */
};

#define PERFD_SPARES_NONE (-2)         /* opts.spares: no standbys, no
                                        * fleet learned - the bare handle */

/* the options carry their own size so a shared libperfd can be replaced under an older program:
 * set it with PERFD_OPTS_INIT; 0 = this header's layout (a static caller only) */
typedef struct perfd_opts {
	size_t size;                   /* sizeof(perfd_opts) the caller saw */
	const char *const *secrets;    /* NULL-terminated list; NULL/empty =
	                                * plaintext (loopback listeners) */
	int connect_timeout_ms;        /* 0 = 5000 */
	int io_timeout_ms;             /* 0 = 5000; per send/recv */
	/* cluster awareness: with standbys the library learns the fleet on connect, so a node
	 * failure costs a swap instead of a TCP+Noise handshake */
	int policy;                    /* enum perfd_policy */
	int spares;                    /* standbys: 0 or -1 = one per other member, N = at
	                                * most N; capped, so many clients cannot storm sockets */
	int refresh_ms;                /* member-list refresh; 0 = 30000 */
	int route_keys;                /* 1 = send each request to its key's node (needs
	                                * spares); off so an upgrade cannot move a workload */
	int eager_push;                /* 1 = write inside every perfd_submit(); 0 batches
	                                * until the next write-readiness or perfd_push() */
	/* TCP keepalive idle s (0 = 30, -1 = off), and a ping on a standby idle this many ms
	 * (0 = 30000, -1 = off); a standby failing it is retired and its member marked down */
	int keepalive_s;
	int idle_ping_ms;
	/* tests only: 1 = keep the member list learned at connect and ignore the daemon's
	 * joined/expelled pushes */
	int ignore_member_push;
	/* proxy mode: remember which node answered each key and send its next call there; a stale
	 * entry costs one hop, never a wrong answer.  Needs spares; off by default. */
	int route_hints;
} perfd_opts;
/* designated, so -Wextra's missing-field-initializers stays quiet; C++ and
 * FFI callers use perfd_opts_init() */
#define PERFD_OPTS_INIT { .size = sizeof(perfd_opts) }
void perfd_opts_init(perfd_opts *o);           /* zeroes it, sets size */

/* connect (TCP / unix socket).  NULL on failure - call perfd_error(NULL)
 * for the reason.  @opts may be NULL for all-defaults plaintext. */
perfd_t *perfd_connect(const char *host, int port, const perfd_opts *opts);
perfd_t *perfd_connect_unix(const char *path, const perfd_opts *opts);
void perfd_free(perfd_t *p);

/* derive the Noise keys for @secrets into the process-wide cache (Argon2id, ~150 ms and 64 MiB
 * each) - a pre-fork server calls it in the parent.  At most 8 kept, >= 192 bytes skipped. */
#define PERFD_HAS_PSK_PREPARE 1
int perfd_psk_prepare(const char *const *secrets);

/* the last error on @p (NULL: the last failed connect), valid until the next call on it; a
 * transport failure poisons the handle, every later call fails until perfd_free() */
const char *perfd_error(const perfd_t *p);

/* what KIND that error is - branch on this, never on the text (which
 * may change between releases).  A binding maps these to its exceptions. */
enum perfd_error_kind {
	PERFD_E_NONE = 0,
	PERFD_E_TRANSPORT,     /* connect, send, receive, a lost connection */
	PERFD_E_AUTH,          /* the Noise handshake: wrong or missing secret */
	PERFD_E_REFUSED,       /* the daemon declined: client limit, not ready,
	                        * a read-only collection, a conditional write */
	PERFD_E_SERVER,        /* the daemon answered with any other error */
	PERFD_E_NOTFOUND,      /* reserved: a miss is a return value today */
	PERFD_E_ARG,           /* a bad argument: size, name, range */
	PERFD_E_NOMEM,         /* out of memory, or the crypto library failed */
	PERFD_E_PROTOCOL,      /* a reply this library could not read */
	PERFD_E_STATE          /* the call does not fit the handle's state */
};
#define PERFD_HAS_ERROR_KIND 1
int perfd_error_kind(const perfd_t *p);

/* where the library's log lines go: @level 1 critical .. 6 debug, @msg ends in a newline; NULL =
 * errors to stderr (the default).  Set it once, before the first connect. */
typedef void (*perfd_log_cb)(int level, const char *msg, void *ctx);
void perfd_set_log(perfd_log_cb cb, void *ctx);

/* release any buffer libperfd handed out - not libc free(): a program and a
 * shared library need not share one allocator.  NULL is a no-op. */
void perfd_free_value(void *buf);

/* ---- cluster awareness: introspection, so a caller can SEE what the library does ---- */

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
/* member @i as THIS handle's dials found it: active, standby, down (@why gets the error),
 * not-ready, idle or unknown - never inferred from another node's view */
const char *perfd_member_state(const perfd_t *p, int i, char *why,
		size_t wcap);
/* the daemon version reported in the member list; "" until one was
 * read (no standbys wanted, or a daemon older than the field) */
const char *perfd_server_version(const perfd_t *p);
/* idle pings sent to standbys, and how many found a dead one; and
 * this link's last accepted reply (ms clock) */
unsigned long long perfd_pings(const perfd_t *p);
unsigned long long perfd_pings_failed(const perfd_t *p);
long long perfd_last_reply_ms(const perfd_t *p);

/* re-dial a FAILED handle through the members it learned, most recently alive first, the seed
 * last; 0 = usable again or was not failed, -1 = none answered (perfd_error says why) */
int perfd_redial(perfd_t *p);

/* re-dial down members and drain membership pushes without a request: an idle handle acts on
 * nothing, so call this from a timer (a few times a second).  0, or -1 on a failed handle. */
int perfd_maintain(perfd_t *p);
unsigned long long perfd_recovered(const perfd_t *p);   /* members brought back */

/* ---- per-key routing: a request goes to its key's node when the cluster's algorithm is known;
 * never load-bearing (the daemon forwards a wrong guess); needs standbys ---- */

/* 1 = this handle is routing by key */
int perfd_routing(const perfd_t *p);

/* the cluster wire epoch the daemon last advertised (0 = unknown); when it differs from
 * PERFD_WIRE_EPOCH the handle stops routing and lets the daemon forward */
int perfd_wire_epoch(const perfd_t *p);
/* requests whose owner was known but not connected (they went to the
 * active node and paid a forward) - raise opts.spares if this grows */
unsigned long long perfd_route_missed(const perfd_t *p);
/* times a reply said "you are behind" (the daemon forwarded a request routed by an older map)
 * and the handle re-learned its members */
unsigned long long perfd_moved_hints(const perfd_t *p);
/* with opts.route_hints: calls a hinted holder answered itself, and calls whose answer named
 * ANOTHER holder (the key had moved - one extra hop, the entry updated) */
unsigned long long perfd_hint_hits(const perfd_t *p);
unsigned long long perfd_hint_stale(const perfd_t *p);

/* which member should hold @key (an index for perfd_member_info), -1 = routing does not apply;
 * async callers route with this over a handle per node.  Needs opts.route_keys. */
int perfd_owner_of(perfd_t *p, const char *col, const char *key);

/* ---- typed verbs: -1 = error (perfd_error set), else the documented value; out-buffers
 * are the caller's - release them with perfd_free_value() ---- */

/* 0 ok */
PERFD_MUST_CHECK int perfd_set(perfd_t *p, const char *col, const char *key,
		const void *val, size_t vlen, long long ttl);

/* store only when the key is ABSENT (NX; expired counts) or PRESENT (XX): 1 stored, 0 declined;
 * one node decides each key (the lock idiom), and it is never replayed after a failover */
#define PERFD_NX 1
#define PERFD_XX 2
PERFD_MUST_CHECK int perfd_set_cond(perfd_t *p, const char *col,
		const char *key, const void *val, size_t vlen, long long ttl,
		int cond);

/* Redis 8.4's SET / DELEX IFEQ (@ne 0) and IFNE (@ne 1; set_if: or absent) against @cmp: 1 applied,
 * 0 did not hold, -1 error (a non-string key too); never replayed.  Needs perfcached 0.5.0. */
PERFD_MUST_CHECK int perfd_set_if(perfd_t *p, const char *col,
		const char *key, const void *val, size_t vlen, long long ttl,
		const void *cmp, size_t cmplen, int ne);
PERFD_MUST_CHECK int perfd_del_if(perfd_t *p, const char *col,
		const char *key, const void *cmp, size_t cmplen, int ne);

/* sliding-window rate limit: record a hit, drop those older than @window_ms; 1 = within @limit
 * (0 = none), 0 = over (still recorded), *@count the hits.  Never replayed after a failover. */
PERFD_MUST_CHECK int perfd_rl_hit(perfd_t *p, const char *col,
		const char *key, long long window_ms, long long limit,
		long long *count);

/* hashes, as Redis's: hset 1 new / 0 updated, hsetnx 1 set / 0 existed, hget hdel hexists 1 / 0;
 * hlen, hincrby 0 + out; hgetall the count, @cb sees pairs in order (non-zero stops); no replay */
typedef int (*perfd_hfield_cb)(void *ctx, const char *field, size_t flen,
		const void *val, size_t vlen);
PERFD_MUST_CHECK int perfd_hset(perfd_t *p, const char *col,
		const char *key, const char *field, const void *val, size_t vlen);
PERFD_MUST_CHECK int perfd_hsetnx(perfd_t *p, const char *col,
		const char *key, const char *field, const void *val, size_t vlen);
PERFD_MUST_CHECK int perfd_hget(perfd_t *p, const char *col,
		const char *key, const char *field, void **val, size_t *vlen);
PERFD_MUST_CHECK int perfd_hdel(perfd_t *p, const char *col,
		const char *key, const char *field);
PERFD_MUST_CHECK int perfd_hexists(perfd_t *p, const char *col,
		const char *key, const char *field);
PERFD_MUST_CHECK int perfd_hlen(perfd_t *p, const char *col,
		const char *key, long long *n);
PERFD_MUST_CHECK int perfd_hincrby(perfd_t *p, const char *col,
		const char *key, const char *field, long long by,
		long long *result);
PERFD_MUST_CHECK int perfd_hgetall(perfd_t *p, const char *col,
		const char *key, perfd_hfield_cb cb, void *ctx);

/* 1 found (val/vlen/ttl_out filled; *ttl_out -1 = no expiry), 0 miss */
int perfd_get(perfd_t *p, const char *col, const char *key,
		void **val, size_t *vlen, long long *ttl_out);

/* 1 deleted, 0 was absent */
PERFD_MUST_CHECK int perfd_del(perfd_t *p, const char *col, const char *key);

/* 1 exists, 0 not */
int perfd_exists(perfd_t *p, const char *col, const char *key);

/* seconds left; -1 = no expiry; -2 = no such key */
long long perfd_ttl(perfd_t *p, const char *col, const char *key);

/* 0 ok (key existed), 1 no such key */
PERFD_MUST_CHECK int perfd_expire(perfd_t *p, const char *col, const char *key,
		long long ttl);

/* counters: 0 ok, *newval = the resulting value */
PERFD_MUST_CHECK int perfd_add(perfd_t *p, const char *col, const char *key, long long by,
		long long ttl, long long *newval);
PERFD_MUST_CHECK int perfd_sub(perfd_t *p, const char *col, const char *key, long long by,
		long long *newval);

/* 0 ok; values[i] NULL = miss (else malloc'd, vlens[i] set).
 * Arrays are caller-provided, nkeys wide. */
int perfd_mget(perfd_t *p, const char *col, const char *const *keys,
		int nkeys, void **values, size_t *vlens);

/* keys matching @match (NULL = all), up to @limit (0 = server default): the count, and *keys_out
 * a malloc'd array of malloc'd strings for perfd_free_keys; -1 on error */
int perfd_keys(perfd_t *p, const char *col, const char *match, int limit,
		char ***keys_out);
void perfd_free_keys(char **keys, int n);

/* JSON path verbs: fragments travel as JSON text (out malloc'd, NUL-terminated); NULL path = "$".
 * jget 1 found / 0 miss, jset 0, jincr 0 + *newval, jdel 1 deleted / 0 absent. */
int perfd_jget(perfd_t *p, const char *col, const char *key,
		const char *path, char **frag_out);
PERFD_MUST_CHECK int perfd_jset(perfd_t *p, const char *col, const char *key,
		const char *path, const char *json_val, long long ttl);
PERFD_MUST_CHECK int perfd_jdel(perfd_t *p, const char *col, const char *key,
		const char *path);
PERFD_MUST_CHECK int perfd_jincr(perfd_t *p, const char *col, const char *key,
		const char *path, long long by, long long *newval);

/* 0 ok (round trip proven) */
int perfd_ping(perfd_t *p);

/* ---- pub/sub: deliveries reach the notify hook as {"method":"message"|"pmessage",...}; publish
 * returns the receivers on the node reached, (un)subscribe the handle's count (NULL = all) ---- */
int perfd_publish(perfd_t *p, const char *channel, const void *payload,
		size_t plen);
int perfd_subscribe(perfd_t *p, const char *channel);
int perfd_psubscribe(perfd_t *p, const char *pattern);
int perfd_unsubscribe(perfd_t *p, const char *channel);
int perfd_punsubscribe(perfd_t *p, const char *pattern);

/* ---- pub/sub over UDP (lib/perfd_push.h): deliveries as sealed datagrams to the same hook; call
 * _ready on readability AND once a second - 15 s without an ack drops every subscription ---- */
PERFD_MUST_CHECK int perfd_pubsub_udp(perfd_t *p, int port, int timeout_ms);
int perfd_pubsub_udp_fd(const perfd_t *p);
int perfd_pubsub_udp_ready(perfd_t *p);
int perfd_pubsub_udp_off(perfd_t *p);          /* 0; stops the stream */
struct perfd_udp_stats {
	int active;                    /* confirmed and delivering */
	int pruned;                    /* the daemon dropped it (see above) */
	int port;                      /* the local UDP port */
	unsigned long long received;   /* messages delivered to the hook */
	unsigned long long gaps;       /* sequence numbers that never came */
	unsigned long long duplicates; /* dropped */
	unsigned long long rejected;   /* wrong source, stream, or seal */
	unsigned long long acks;       /* acknowledgements sent */
};
int perfd_pubsub_udp_stats(const perfd_t *p, struct perfd_udp_stats *s);

/* the escape hatch: any method + params JSON (may be NULL); the reply tree as malloc'd JSON text,
 * or NULL with the daemon's error in perfd_error().  Bytes not UTF-8 render as U+FFFD. */
char *perfd_command(perfd_t *p, const char *method,
		const char *params_json);

/* byte-exact tree forms (src/ptree.h, exported with the library): @params one encoded map or
 * NULL; a pipeline may mix text and tree requests, each reply in the form asked */
unsigned char *perfd_command_tree(perfd_t *p, const char *method,
		const unsigned char *params, size_t plen, size_t *outlen);
int perfd_append_tree(perfd_t *p, const char *method,
		const unsigned char *params, size_t plen);
unsigned char *perfd_next_reply_tree(perfd_t *p, size_t *outlen);

/* ---- pipelining: append queues, flush writes, next_reply returns each result IN REQUEST ORDER
 * (NULL + perfd_error for an error frame); typed calls in between are refused ---- */
PERFD_MUST_CHECK int perfd_append(perfd_t *p, const char *method, const char *params_json);
PERFD_MUST_CHECK int perfd_flush(perfd_t *p);
char *perfd_next_reply(perfd_t *p);
int perfd_pending(const perfd_t *p);   /* replies not yet collected */

/* server notifications (membership, pub/sub, the UDP prune) are skipped unless a hook is set;
 * @json is NOT NUL-terminated and valid only during the call */
typedef void (*perfd_notify_cb)(const char *json, size_t len, void *ctx);
void perfd_set_notify(perfd_t *p, perfd_notify_cb cb, void *ctx);

/* ---- event-loop surface: opt-in, never blocks, never calls back from a thread of its own.  Watch
 * perfd_fd() for perfd_events(), call *_ready(), submit once READY; replies in ARRIVAL order */
enum perfd_state {
	PERFD_ST_CONNECTING = 0,       /* TCP and/or Noise handshake running */
	PERFD_ST_READY,                /* requests may be submitted */
	PERFD_ST_FAILED                /* poisoned; perfd_error() says why */
};

#define PERFD_EV_READ   0x1
#define PERFD_EV_WRITE  0x2

/* the socket for the loop to watch.  -1 if the handle has none. */
int perfd_fd(const perfd_t *p);

/* which readiness the handle wants watched RIGHT NOW - re-read after every entry point
 * (a partial write turns PERFD_EV_WRITE on) */
int perfd_events(const perfd_t *p);

/* connection progress; see enum perfd_state */
int perfd_state(const perfd_t *p);

/* non-blocking connect: a handle in PERFD_ST_CONNECTING, the TCP connect and handshake driven by
 * the readiness calls; NULL only if no socket could be created */
perfd_t *perfd_connect_async(const char *host, int port,
		const perfd_opts *opts);

/* per-request completion: the callback OWNS @result (perfd_free_value); NULL = failed, @errmsg
 * says why.  A submit reply is JSON text, a submit_kv reply the verb's raw payload. */
typedef void (*perfd_reply_cb)(char *result, size_t len, const char *errmsg,
		void *ctx);

/* queue a request and write what the socket takes; never blocks.  0 ok, -1 error; legal only in
 * PERFD_ST_READY.  The params JSON becomes the request tree here. */
int perfd_submit(perfd_t *p, const char *method, const char *params_json,
		perfd_reply_cb cb, void *ctx);

/* call on readable / writable: 0 ok, -1 = the connection failed (PERFD_ST_FAILED; every
 * in-flight callback gets an error first) */
int perfd_read_ready(perfd_t *p);
int perfd_write_ready(perfd_t *p);

/* requests submitted whose callback has not fired yet */
int perfd_inflight(const perfd_t *p);

/* write whatever perfd_submit() queued (eager_push 0 batches); 0 ok, -1 failed.  NOT perfd_flush(),
 * which is the sync pipeline's, on a different queue. */
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

/* async data verb on its fixed frame, no JSON either way: @val for SET (PING's echo), @by for
 * ADD/SUB, @ttl seconds for SET/EXPIRE, 0/NULL otherwise.  Otherwise as perfd_submit. */
int perfd_submit_kv(perfd_t *p, int verb, const char *col, const char *key,
		const void *val, size_t vlen, long long by, long long ttl,
		perfd_reply_cb cb, void *ctx);

#endif /* PERFD_H */
