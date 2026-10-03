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
 * Transport (0.3.0, S317): binary frames only, over TCP or a unix
 * socket, plaintext or the Noise channel (NNpsk0, client principal).
 * The data verbs (get set del exists ttl expire add sub ping, the
 * conditional sets, rl_hit, the hash commands) ride their fixed-layout
 * frames; every other call - mget, keys, the JSON path verbs, pub/sub,
 * perfd_command, the pipeline, perfd_submit - rides PC_VERB_CMD with
 * its parameters as a tree and answers a tree (src/ptree.h, exported
 * with the library).  Values and keys are bytes both ways: nothing is
 * escaped, encoded or base64'd on the wire.  perfd_opts.secrets is a
 * LIST tried in order, so secret rotation is add-new/drain-old with no
 * client downtime.
 *
 * JSON appears only at the API EDGE, as a convenience for tools: the
 * params text perfd_command / perfd_append / perfd_submit take is
 * converted to the request tree in the library, the reply tree is
 * rendered back to JSON text for the caller, and the notify hook sees
 * a notification rendered the same way.  A byte that is not UTF-8
 * renders as U+FFFD there (lossy); a byte-exact caller takes the tree
 * forms (perfd_command_tree & co) or the typed verbs.
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

/* S59(b): a fallible mutating call whose ignored return is a bug.
 * Self-contained (this header ships alone); -Werror consumers gate. */
#if !defined(PERFD_MUST_CHECK)
#if defined(__GNUC__) || defined(__clang__)
#define PERFD_MUST_CHECK __attribute__((warn_unused_result))
#else
#define PERFD_MUST_CHECK
#endif
#endif

#include <stddef.h>

/* the library's version - semver, independent of the daemon's (this
 * archive ships into other codebases and moves on its own cadence).
 * PERFD_VERSION is what you compiled against; perfd_version() is what
 * you linked - compare them to catch a stale libperfd.a. */
#define PERFD_VERSION_MAJOR 0
#define PERFD_VERSION_MINOR 3
#define PERFD_VERSION_PATCH 0
#define PERFD_VERSION "0.3.0"
/* S317: 0.3 is the first library of the binary-only wire and NEEDS
 * perfcached 0.4.5 or later - an older daemon answers its first request
 * (members, on connect) with "unknown verb", which perfd_error() names.
 * 0.2.x libraries cannot talk to a 0.4.5 daemon at all: their JSON line
 * after the first frame closes the connection.  The minor step says so. */
#define PERFD_NEEDS_PERFCACHED "0.4.5"

/* S127: 0.2.8 is the first library that can route `mode = spread`.
 * An older one reads a mode it does not recognise, turns routing
 * OFF and dials any node - it stays CORRECT because the daemon
 * forwards, and simply pays a pull on most reads (a random node
 * holds a given key K of P times).  That is a performance floor,
 * not a compatibility break, so nothing refuses an older client;
 * perfd_route_missed() is how an operator sees it. */
#define PERFD_SPREAD_ROUTING_SINCE "0.2.8"

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

#define PERFD_SPARES_NONE (-2)         /* opts.spares: no standbys, no
                                        * fleet learned - the bare handle */

typedef struct perfd_opts {
	const char *const *secrets;    /* NULL-terminated list; NULL/empty =
	                                * plaintext (loopback listeners) */
	int connect_timeout_ms;        /* 0 = 5000 */
	int io_timeout_ms;             /* 0 = 5000; per send/recv */
	/* (0.3.0: the `binary` option is gone - the wire is binary only) */
	/* ---- cluster awareness (S34) ----
	 * With spares > 0 the library learns the fleet on connect and keeps
	 * standby connections open, so a node failure costs a swap instead
	 * of a TCP+Noise handshake at the worst possible moment. */
	int policy;                    /* enum perfd_policy */
	int spares;                    /* standby connections to keep.
	                                * S104 (0.2.5): 0 = the default = one
	                                * per other member - a handle holds a
	                                * connection to every member it has
	                                * learned, so a node failure is a
	                                * swap and a request fails only when
	                                * every held link has; -1 = the
	                                * same, spelled out; N > 0 = at most
	                                * N; PERFD_SPARES_NONE = a bare
	                                * handle, no fleet learned.  Capped
	                                * so a big fleet x many clients
	                                * cannot become a socket storm. */
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
	/* S104 liveness.  TCP keepalive on every link (idle seconds; 0 = 30,
	 * -1 = off), and an application ping on a standby that has been idle
	 * this long (ms; 0 = 30000, -1 = off), sent from the maintenance
	 * that runs after a successful call.  A standby that fails its ping
	 * is retired and its member marked down, so a failover never adopts
	 * a link that died while idle. */
	int keepalive_s;
	int idle_ping_ms;
	/* RV-10, for tests: 1 = leave the member list as learned at connect
	 * and ignore the daemon's "joined"/"expelled" pushes.  It is how a
	 * test builds the stale map a missed push leaves behind; an
	 * application has no reason to set it. */
	int ignore_member_push;
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
/* S70: member @i as THIS handle's own dials found it - "active" (serves
 * this handle), "standby" (a spare is open to it from here), "down"
 * (this handle failed to dial it; @why gets the error), "not-ready"
 * (it says it is not serving data yet), "idle" (no connection held from
 * here, nothing known against it), "unknown" (no such member).  Never
 * inferred from another node's view. */
const char *perfd_member_state(const perfd_t *p, int i, char *why,
		size_t wcap);
/* S70: the daemon version reported in the member list; "" until one was
 * read (no standbys wanted, or a daemon older than the field) */
const char *perfd_server_version(const perfd_t *p);
/* S104 liveness counters: idle pings sent to standbys, and how many
 * found a dead one; and this link's last accepted reply (ms clock) */
unsigned long long perfd_pings(const perfd_t *p);
unsigned long long perfd_pings_failed(const perfd_t *p);
long long perfd_last_reply_ms(const perfd_t *p);

/* S104: a FAILED handle (every held link died in one attempt) re-dials
 * through the members it LEARNED - the ones most recently alive first,
 * the seed it was given last - rather than being thrown away and
 * re-dialled at the seed alone, which may be the one node that is
 * still down.  0 = the handle is usable again (members re-learned and
 * standbys re-warmed on its next successful call); -1 = no member
 * answered, perfd_error(p) says the last reason.  A handle that is not
 * failed returns 0 at once. */
int perfd_redial(perfd_t *p);

/* S107: members come back on their own, and the fleet tells the handle
 * when its shape changes.  A member marked down (a failed dial, a dead
 * standby, a probe that failed) is re-dialled in the background from
 * the maintenance that follows every successful call - one dial per
 * second at most, on a backoff of 1 s doubling to 30 s - and re-enters
 * as a standby and a routing target when it answers.  The daemon pushes
 * a membership notification (a NOTIFY frame carrying the map {"notify":
 * "membership", "event": "joined"|"expelled", "addr", "port", "node",
 * "identity", "members"}) to every client when a node joins or is
 * expelled; the library dials a newcomer and drops an expelled member -
 * connection, standby, routing weight - before the application's
 * perfd_set_notify() hook sees it, rendered as that JSON object.  A
 * handle with no traffic acts on nothing: call
 * perfd_maintain() from a timer (a few times a second is plenty) to
 * drain pushes and run the recovery without a request.  0, or -1 on a
 * failed handle. */
int perfd_maintain(perfd_t *p);
unsigned long long perfd_recovered(const perfd_t *p);   /* members brought back */

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

/* S221: the cluster wire epoch the daemon last advertised, 0 if it is
 * older than the field or nothing has been fetched yet.  When this
 * differs from PERFD_WIRE_EPOCH the handle stops ROUTING and lets the
 * daemon forward - correct, one hop slower - so a consumer that cares
 * about the hop reads this to say WHY in its own log. */
int perfd_wire_epoch(const perfd_t *p);
/* requests whose owner was known but not connected (they went to the
 * active node and paid a forward) - raise opts.spares if this grows */
unsigned long long perfd_route_missed(const perfd_t *p);
/* RV-10: times a reply said "you are behind" - the daemon had to forward
 * a routed request and the map stamp it routed by is not the one this
 * handle learned - and the handle re-learned its members because of it.
 * (0.2.12; needs a daemon that sends the hint.  The periodic refresh,
 * opts.refresh_ms, and the membership pushes still do their part: this
 * closes the gap between a change and either of them to ONE request.) */
unsigned long long perfd_moved_hints(const perfd_t *p);

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
PERFD_MUST_CHECK int perfd_set(perfd_t *p, const char *col, const char *key,
		const void *val, size_t vlen, long long ttl);

/* S279b (0.2.13): store only when the key is ABSENT (PERFD_NX - an
 * expired key is absent) or PRESENT (PERFD_XX).  1 stored, 0 declined,
 * -1 error.  In a cluster one node decides each key, so two callers on
 * two nodes cannot both be granted PERFD_NX - the lock idiom.  NEVER
 * replayed after a failover: a replay would read its own write and
 * answer 0 for a lock the caller holds, so a lost connection is -1 and
 * the caller decides.  A daemon without S279b answers -1 ("unknown
 * verb" / "method not found"), never an unconditional store. */
#define PERFD_NX 1
#define PERFD_XX 2
PERFD_MUST_CHECK int perfd_set_cond(perfd_t *p, const char *col,
		const char *key, const void *val, size_t vlen, long long ttl,
		int cond);

/* S314 (0.2.14): an atomic sliding-window rate-limit hit - record this
 * hit on @key, drop hits older than @window_ms (1..86400000), and answer
 * whether the hits now in the window are within @limit (1..5999; 0 = no
 * limit, always allowed).  1 allowed, 0 over the limit (the hit IS
 * recorded, as Redis's ZADD-first limiter does), -1 error.  *@count
 * (NULL ok) gets the hits in the window.  One node decides each key, so
 * hits through every node count together.  NEVER replayed after a
 * failover - a replay counts the hit twice.  A daemon before 0.4.4
 * answers -1 ("unknown verb" / "method not found"); a fleet with such a
 * member answers -1 with the node named. */
PERFD_MUST_CHECK int perfd_rl_hit(perfd_t *p, const char *col,
		const char *key, long long window_ms, long long limit,
		long long *count);

/* S313 (0.2.14): hashes - a key holding field/value pairs, as Redis's.
 * One node decides each key, so writes to different fields of one hash
 * through different nodes all land.  Field names are C strings; values
 * are bytes, exact.  -1 = error, perfd_error() says
 * why - including the daemon's own refusal ("WRONGTYPE ...", "hash too
 * large ...", "hash value is not an integer", or, on a fleet with a
 * pre-0.4.4 member, the gate's line).  NEVER replayed after a failover.
 *   perfd_hset     1 the field is new, 0 it was updated
 *   perfd_hsetnx   1 set, 0 the field existed (unchanged)
 *   perfd_hget     1 found (*val malloc'd, the caller frees), 0 absent
 *   perfd_hdel     1 removed, 0 absent (the last field deletes the key)
 *   perfd_hexists  1 / 0
 *   perfd_hlen     0, *n = the fields (0 for an absent key)
 *   perfd_hincrby  0, *result = the new value
 *   perfd_hgetall  the field count; @cb (may be NULL) sees each pair in
 *                  insertion order and stops the walk by returning
 *                  non-zero; the pointers live for the call only */
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

/* keys matching @match (NULL = all), up to @limit (0 = server default).
 * Returns the count and fills *keys_out with a malloc'd array of
 * malloc'd strings - free with perfd_free_keys.  -1 on error. */
int perfd_keys(perfd_t *p, const char *col, const char *match, int limit,
		char ***keys_out);
void perfd_free_keys(char **keys, int n);

/* JSON path verbs: raw JSON fragments in and out (malloc'd out) - the
 * document is DATA, and travels as one bulk of its JSON text: jset's
 * json_val goes as written, jget's fragment comes back as the daemon
 * wrote it, NUL-terminated.  perfd_jget: 1 found, 0 miss.  perfd_jset:
 * 0 ok.  perfd_jincr: 0 ok, *newval = result.  perfd_jdel: 1 deleted,
 * 0 absent.  A NULL path is "$". */
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

/* ---- pub/sub (PS4) ------------------------------------------------------
 * Channels are one global string space on every node.  A subscription
 * lives on this handle's connection; deliveries arrive on the notify hook
 * (perfd_set_notify) as NOTIFY frames rendered to JSON: {"method":
 * "message","params":{"channel":"...","payload":"..."}} and {"method":
 * "pmessage","params":{"pattern":"...","channel":"...","payload":"..."}}
 * (that member order).  The payload is bytes on the wire; the hook's
 * text shows a payload that is not UTF-8 with U+FFFD in place of the
 * offending bytes - an application that publishes binary payloads and
 * needs them exact takes them over UDP (below) or keeps them UTF-8.
 * The connection stays multiplexed, a blocking call keeps waiting for
 * its own id.  They are dispatched whenever the
 * handle reads its socket: inside any call on the handle, or on an async
 * handle's perfd_read_ready().  perfd_publish returns the receivers on
 * the node it reached (the fleet relays to the rest); the subscribe
 * calls return the handle's subscription count after the call; a NULL
 * name on an unsubscribe drops every subscription of that kind.  -1 with
 * perfd_error() set on failure. */
int perfd_publish(perfd_t *p, const char *channel, const void *payload,
		size_t plen);
int perfd_subscribe(perfd_t *p, const char *channel);
int perfd_psubscribe(perfd_t *p, const char *pattern);
int perfd_unsubscribe(perfd_t *p, const char *channel);
int perfd_punsubscribe(perfd_t *p, const char *pattern);

/* ---- pub/sub over UDP (PS5) --------------------------------------------
 * A handle may take its pub/sub deliveries as sealed datagrams instead of
 * on its connection - every subscription the handle holds, one stream.
 * They reach the same notify hook with the same text as a delivery on
 * the connection (the library builds the same tree and renders it the
 * same way).  The daemon sends only to this connection's own address,
 * from the door's own address and port, and only after the handle proved
 * the datagrams reach it; the protocol is lib/perfd_push.h.
 *
 * perfd_pubsub_udp opens a UDP socket on @port (0 = any free port) and
 * asks for the stream.  On a blocking handle it waits up to @timeout_ms
 * (0 = 3000) for the daemon's probe, confirms it and returns 0, or -1
 * with perfd_error() set - no probe usually means a firewall or NAT
 * between the two, and deliveries simply stay on the connection.  On an
 * async handle it returns 0 once the request is submitted; the probe and
 * the confirmation are handled by perfd_pubsub_udp_ready, and
 * perfd_pubsub_udp_stats says when the stream is active.
 *
 * perfd_pubsub_udp_fd is the socket to watch for readability (-1 when
 * none).  perfd_pubsub_udp_ready reads what arrived, checks and opens it,
 * hands each message to the notify hook, and acknowledges to the daemon
 * every 256 messages or 5 s; call it on readability AND at least once a
 * second - the acknowledgement is due even when nothing arrives, and a
 * daemon that hears none for 15 s drops every subscription on the
 * connection.  That drop arrives as the notification {"method":
 * "pubsub_udp_pruned","params":{"stream":"<16 hex>","reason":"..."}}:
 * the handle closes its socket and does NOT re-subscribe - the
 * application decides.  A failover to
 * another node ends the stream too.  Returns messages delivered, or -1.
 *
 * Duplicates are dropped and gaps counted, never repaired. */
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

/* the escape hatch: any method + params (params_json may be NULL).
 * The JSON EDGE: the params text is converted to the request tree here
 * (objects -> maps, arrays, strings -> bytes, numbers, bools, null; a
 * value under "val" - a jset/jarrappend document - goes as one bulk of
 * its JSON text), and the reply tree comes back rendered as malloc'd
 * NUL-terminated JSON text, or NULL with the daemon's error message in
 * perfd_error().  It is a convenience, not the wire: bytes that are not
 * UTF-8 in a reply render as U+FFFD.  Admin verbs ride here too - e.g.
 * perfd_command(p, "probe", NULL) re-measures the WAL storage
 * (iops/latency at 4K QD1 + seq bandwidth; blocks for the probe). */
char *perfd_command(perfd_t *p, const char *method,
		const char *params_json);

/* S317: the tree-level forms, byte-exact where JSON text cannot be (a
 * key or a value that is not UTF-8 - perfdump and perfload live here).
 * @params is ONE encoded tree item (a map) or NULL; the reply is the
 * malloc'd reply tree, *outlen bytes, or NULL with perfd_error() (the
 * daemon's error frame).  src/ptree.h, exported with the library,
 * writes and reads trees.  perfd_append_tree / perfd_next_reply_tree are
 * the pipelined pair; a pipeline may mix the JSON-text and tree forms
 * request by request - each reply comes back in the form it was asked. */
unsigned char *perfd_command_tree(perfd_t *p, const char *method,
		const unsigned char *params, size_t plen, size_t *outlen);
int perfd_append_tree(perfd_t *p, const char *method,
		const unsigned char *params, size_t plen);
unsigned char *perfd_next_reply_tree(perfd_t *p, size_t *outlen);

/* ---- pipelining --------------------------------------------------------
 * perfd_append queues without touching the socket; perfd_flush writes
 * the batch; perfd_next_reply returns each result IN REQUEST ORDER
 * (malloc'd result JSON, or NULL with perfd_error set for that
 * request's error frame).  The same edge conversion as perfd_command:
 * params JSON in, reply JSON out.  Mixing typed calls between append
 * and the final next_reply is refused. */
PERFD_MUST_CHECK int perfd_append(perfd_t *p, const char *method, const char *params_json);
PERFD_MUST_CHECK int perfd_flush(perfd_t *p);
char *perfd_next_reply(perfd_t *p);
int perfd_pending(const perfd_t *p);   /* replies not yet collected */

/* server notifications (membership, pub/sub deliveries, the UDP prune)
 * are skipped by default; set a hook to see them.  @json is the NOTIFY
 * tree rendered as a JSON object, @len bytes, NOT NUL-terminated at len
 * and valid only during the call. */
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
 * @len is the result length: a perfd_submit() reply is the reply tree
 * rendered as NUL-terminated JSON text (the JSON edge), a
 * perfd_submit_kv() reply is the data verb's raw payload. */
typedef void (*perfd_reply_cb)(char *result, size_t len, const char *errmsg,
		void *ctx);

/* Queue a request and write as much as the socket will take; never
 * blocks.  0 ok, -1 = error (perfd_error set).  Legal only in
 * PERFD_ST_READY.  The params JSON is converted to the request tree
 * here, as perfd_command does. */
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
 * Async submit for a data verb on its fixed-layout frame.
 *
 * perfd_submit() takes its params as JSON text and converts them to a
 * tree per call, then renders the reply tree back to JSON for the
 * callback - that edge conversion is the whole cost of the JSON
 * surface now, and on a pipelined GET workload it was once ~20% of the
 * client's CPU.  This call avoids it entirely.
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
