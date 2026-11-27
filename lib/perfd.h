/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov
 * Part of libperfd - see lib/LICENSE.  This file must stay
 * free of src/core includes: the library is built from the MIT
 * set alone (`make print-libperfd`, test/mitboundtest.sh). */
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
#define PERFD_VERSION_MINOR 7
#define PERFD_VERSION_PATCH 6
#define PERFD_VERSION_TWEAK 3          /* the fourth number; 0: none */
#define PERFD_VERSION "0.7.6.3"
/* the oldest daemon this binary wire works with: an older one answers the first request
 * with "unknown verb", which perfd_error() names */
#define PERFD_NEEDS_PERFCACHED "0.4.5"

/* the first library that routes `mode = spread`; an older one turns routing off and stays
 * correct (the daemon forwards), paying a pull on most reads */
#define PERFD_SPREAD_ROUTING_SINCE "0.2.8"

const char *perfd_version(void);

typedef struct perfd perfd_t;

/* how a handle picks the node it works through: one active connection plus pre-warmed standbys;
 * all but FAILOVER also move 1 in N live handles to a member that comes up, at the next call */
enum perfd_policy {
	PERFD_POLICY_FAILOVER = 0,     /* stay where you connected; the default, so an
	                                * upgrade cannot redistribute a live workload */
	PERFD_POLICY_ROUND_ROBIN,      /* random start per client, then step: clients
	                                * spread with zero coordination */
	PERFD_POLICY_LEAST_CONN,       /* the member reporting the most free arena */
	PERFD_POLICY_WEIGHTED          /* random, weighted by free bytes */
};

#define PERFD_SPARES_NONE (-2)         /* opts.spares: no standbys, no
                                        * fleet learned - the bare handle */

/* the options carry their own size so a shared libperfd can be replaced under an older program:
 * set it with PERFD_OPTS_INIT; 0 = this header's layout (a static caller only) */
typedef struct perfd_opts {
	size_t size;                   /* sizeof(perfd_opts) the caller saw */
	const char *const *secrets;    /* NULL-terminated, tried in order (rotation: add new,
	                                * drain old); NULL/empty = plaintext */
	int connect_timeout_ms;        /* 0 = 5000; the dial: TCP and handshake, within io_timeout_ms */
	int io_timeout_ms;             /* 0 = 5000; per send/recv */
	/* cluster awareness: with standbys the library learns the fleet on connect, so a node
	 * failure costs a swap instead of a TCP+Noise handshake */
	int policy;                    /* enum perfd_policy */
	int spares;                    /* standbys: 0 or -1 = one per other member, N = at
	                                * most N; capped at 8 (and 64 members learned) */
	int refresh_ms;                /* member-list refresh; 0 = 30000 */
	int route_keys;                /* 1 = send each request to its key's node; off by
	                                * default so an upgrade cannot move a workload */
	int eager_push;                /* 1 = write inside every perfd_submit(); 0 batches
	                                * until the next write-readiness or perfd_push() */
	/* TCP keepalive idle s (0 = 30, -1 = off), and a ping on a standby idle this many ms (0 = 30000,
	 * -1 = off); a standby failing it is retired and its member marked down */
	int keepalive_s;
	int idle_ping_ms;
	/* tests only: 1 = keep the member list learned at connect and ignore the daemon's
	 * joined/expelled pushes */
	int ignore_member_push;
	/* proxy mode: remember which node answered each key and send its next call there; a stale entry
	 * costs one hop, never a wrong answer.  Needs spares; off by default. */
	int route_hints;
	/* the name this handle's connections carry on the daemon (`clients`, log lines); NULL = unnamed;
	 * printable ASCII without spaces or quotes, at most 47 characters */
	const char *name;
	/* 0 = after connecting, redial off a daemon worker holding more
	 * than one connection beyond its least-loaded one; -1 = off */
	int worker_balance;
	/* 0 = ask the node for plaintext after the Noise handshake (an
	 * older node, or one with encrypt = yes, stays encrypted); 1 = never - enable needs it */
	int encrypt;
	/* 1 = every call goes to the node dialled - the fleet is still learned, but no
	 * policy, routing, refusal, dead link or expulsion moves the handle; its answer is the caller's */
	int pin;
	/* the most perfd_apool_get waits on a new handle's dial, ms: 0 = 500, -1 = all of
	 * connect_timeout; a dial still going then fails the get and is finished unblocked */
	int dial_wait_ms;
	/* the most one blocking call waits for its whole reply, ms: 0 = 4 x io_timeout_ms, -1 = no
	 * bound.  io_timeout_ms bounds each read, so a reply that drips in, or a stream of
	 * notifications on a subscribed handle, could otherwise hold a call for ever */
	int call_timeout_ms;
} perfd_opts;
/* designated, so -Wextra's missing-field-initializers stays quiet; C++ and
 * FFI callers use perfd_opts_init_sz() with their own struct's size */
#define PERFD_OPTS_INIT { .size = sizeof(perfd_opts) }
/* zeroes the caller's struct and sets its size; the plain symbol, which a program built against an
 * older header calls, touches only the first 64 bytes - what every such program holds */
void perfd_opts_init(perfd_opts *o);
void perfd_opts_init_sz(perfd_opts *o, size_t size);
#define perfd_opts_init(o) perfd_opts_init_sz((o), sizeof *(o))

/* NULL on failure (perfd_error(NULL) says why); @opts NULL = defaults, plaintext.  A handle whose
 * fleet-learning request failed is still returned: check perfd_error_kind(p). */
perfd_t *perfd_connect(const char *host, int port, const perfd_opts *opts);
perfd_t *perfd_connect_unix(const char *path, const perfd_opts *opts);
void perfd_free(perfd_t *p);

/* derive the Noise keys for @secrets into the process-wide cache (Argon2id, ~150 ms and 64 MiB
 * each) - a pre-fork server calls it in the parent.  At most 8 kept, >= 192 bytes skipped; 0 or -1. */
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
	PERFD_E_REFUSED,       /* the daemon declined and applied nothing: limits,
	                        * not READY, busy, a forward lost, an mget timeout */
	PERFD_E_SERVER,        /* the daemon answered with any other error */
	PERFD_E_NOTFOUND,      /* reserved: a miss is a return value today */
	PERFD_E_ARG,           /* a bad argument: size, name, range */
	PERFD_E_NOMEM,         /* out of memory, or the crypto library failed */
	PERFD_E_PROTOCOL,      /* a reply this library could not read */
	PERFD_E_STATE,         /* the call does not fit the handle's state */
	PERFD_E_BACKOFF        /* perfd_url_connect holds off after a dial no seed answered */
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

/* ---- admin verbs: a tree comes back as malloc'd JSON text (perfd_free_value) or NULL, an int
 * verb as 0 or -1; the daemon's words in perfd_error() ---- */
char *perfd_stats(perfd_t *p, const char *col);     /* col NULL = every one */
char *perfd_members(perfd_t *p);
char *perfd_clients(perfd_t *p, int cap);           /* at most cap rows; 0 = the daemon's 200 */
/* raise privilege with the [secrets] enable value (needs opts.encrypt = 1); kept and re-sent once
 * after a re-dial or failover; an enable or disable through perfd_command() is kept the same way */
int perfd_enable(perfd_t *p, const char *secret);
/* the sweeps (perfcli updrain / upfill): run, CHECK (compare with Redis, write nothing) or stop;
 * privileged; the reply names the collection and what happened */
#define PERFD_SWEEP_RUN   0
#define PERFD_SWEEP_CHECK 1
#define PERFD_SWEEP_STOP  2
char *perfd_upfill(perfd_t *p, const char *col, int what);
char *perfd_updrain(perfd_t *p, const char *col, int what);   /* CHECK runs */
/* ask the master to commit the fleet's wire epoch (privileged);
 * the daemon refuses by name while a live member cannot speak it */
char *perfd_cluster_epoch_commit(perfd_t *p, int epoch);

/* ---- operations verbs: JSON text or NULL as above.  The DDL, fleetstop and mailtest need
 * privilege; create, drop, rename, map and unmap also [daemon] allow_create = yes ---- */
char *perfd_collections(perfd_t *p);                /* every collection: name, mode, entries, buckets */
char *perfd_create(perfd_t *p, const char *col, int buckets_log2);  /* 0 = the daemon's default (12) */
char *perfd_drop(perfd_t *p, const char *col, int force);   /* a non-empty collection needs force */
char *perfd_resize(perfd_t *p, const char *col, int buckets_log2);  /* a background migration; stats shows it */
char *perfd_rename(perfd_t *p, const char *col, const char *to);
/* the RESP door's DB-index map on the running fleet: every member takes it, it survives a restart
 * and outranks resp_collections; unmap returns the index to the config's word */
char *perfd_map(perfd_t *p, int index, const char *col);
char *perfd_unmap(perfd_t *p, int index);
char *perfd_maps(perfd_t *p);
char *perfd_reset_stats(perfd_t *p);                /* this node's running totals */
char *perfd_reset_stats_fleet(perfd_t *p);          /* this node and every live peer, as of one moment */
char *perfd_save(perfd_t *p);                       /* an RDB snapshot in the background */
char *perfd_sync(perfd_t *p, int timeout_ms);       /* the WAL barrier; 0 = the daemon's default */
char *perfd_load(perfd_t *p);                       /* the snapshot, additively; privileged */
char *perfd_mailtest(perfd_t *p);                   /* one mail through [mail] */
char *perfd_upgrade_status(perfd_t *p);             /* the builds staged here, the install mode */
char *perfd_upgrade_revert(perfd_t *p, int force);  /* back to the prefix the helper kept; {request, to, helper} */
char *perfd_upgrade_switch(perfd_t *p, const char *sha256, int force); /* the helper installs the staged build; {request, version, helper} */
/* stop every member, this one last; answers once its peers have gone, up to timeout_ms (0 = 30 s)
 * - set the handle's io timeout above it */
char *perfd_fleetstop(perfd_t *p, int timeout_ms);

/* ship a release artifact (perfcli upgrade-stage); @sig = 64-byte ed25519 signature or NULL (refused
 * unless [upgrade] allow_unsigned); privileged; the end reply as JSON text, or NULL */
typedef void (*perfd_progress_cb)(void *ctx, size_t done, size_t total);
char *perfd_upgrade_stage(perfd_t *p, const void *data, size_t size,
		const char *version, const unsigned char *sig,
		perfd_progress_cb progress, void *ctx);
/* the same from a file of 1 byte to 1 GB; <path>.sig beside it (64 raw bytes or 128 hex) is sent
 * when present, and *@signed_out (may be NULL) says whether */
char *perfd_upgrade_stage_file(perfd_t *p, const char *path,
		const char *version, perfd_progress_cb progress, void *ctx,
		int *signed_out);

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

/* re-dial a FAILED handle through the members it learned, most recently alive first, the seed last
 * (a pinned handle: its own node only); 0 = usable again or was not failed, -1 = none answered */
int perfd_redial(perfd_t *p);

/* re-dial down members and drain membership pushes without a request: an idle handle acts on
 * nothing, so call this from a timer (a few times a second).  0, or -1 on a failed handle. */
int perfd_maintain(perfd_t *p);
unsigned long long perfd_recovered(const perfd_t *p);   /* members brought back */

/* ---- per-key routing: a request goes to its key's node when the cluster's algorithm is known;
 * never load-bearing (the daemon forwards a wrong guess); needs standbys ---- */

/* 1 = this handle is routing by key */
int perfd_routing(const perfd_t *p);

/* the cluster wire epoch the daemon last advertised (0 = unknown); when it is not 4 the handle
 * stops routing and lets the daemon forward */
int perfd_wire_epoch(const perfd_t *p);
/* requests whose owner was known but not connected (they paid a forward) - raise opts.spares
 * if this grows, up to the cap of 8 */
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

/* hashes, as Redis's - 1/0: hset new/updated, hsetnx set/existed, hget hdel hexists found/absent; hlen,
 * hincrby 0 + out; hgetall the count, @cb sees pairs in order, non-zero stops.  Never replayed. */
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

/* reads into the caller's @buf: 1 found (*vlen bytes, not NUL-terminated), 0 absent, -1 error,
 * -2 too small (*vlen = size needed, @buf untouched; NULL/0 asks the size).  Blocking handles. */
#define PERFD_HAS_GET_BUF 1
PERFD_MUST_CHECK int perfd_get_buf(perfd_t *p, const char *col,
		const char *key, void *buf, size_t cap, size_t *vlen,
		long long *ttl_out);
PERFD_MUST_CHECK int perfd_jget_buf(perfd_t *p, const char *col,
		const char *key, const char *path, void *buf, size_t cap,
		size_t *vlen);
PERFD_MUST_CHECK int perfd_hget_buf(perfd_t *p, const char *col,
		const char *key, const char *field, void *buf, size_t cap,
		size_t *vlen);

/* 1 deleted, 0 was absent */
PERFD_MUST_CHECK int perfd_del(perfd_t *p, const char *col, const char *key);

/* 1 exists, 0 not */
int perfd_exists(perfd_t *p, const char *col, const char *key);

/* seconds left; -1 = no expiry; -2 = no such key.  An error is also -1, and
 * then perfd_error_kind(p) is not PERFD_E_NONE (set by this call alone) */
long long perfd_ttl(perfd_t *p, const char *col, const char *key);

/* 0 ok (key existed), 1 no such key */
PERFD_MUST_CHECK int perfd_expire(perfd_t *p, const char *col, const char *key,
		long long ttl);

/* counters: 0 ok, *newval = the resulting value */
PERFD_MUST_CHECK int perfd_add(perfd_t *p, const char *col, const char *key, long long by,
		long long ttl, long long *newval);
PERFD_MUST_CHECK int perfd_sub(perfd_t *p, const char *col, const char *key, long long by,
		long long *newval);

/* 0 ok; values[i] NULL = miss (else malloc'd), arrays nkeys wide.  A key whose node did not
 * answer in time fails the whole call (PERFD_E_REFUSED - retry), never a miss. */
int perfd_mget(perfd_t *p, const char *col, const char *const *keys,
		int nkeys, void **values, size_t *vlens);

/* as perfd_mget with each key's outcome in @status: FOUND, ABSENT, or UNANSWERED (its node did
 * not answer in time - ask again); -1 only when the call failed as a whole */
#define PERFD_HAS_MGET_EX 1
#define PERFD_MGET_ABSENT     0
#define PERFD_MGET_FOUND      1
#define PERFD_MGET_UNANSWERED 2
int perfd_mget_ex(perfd_t *p, const char *col, const char *const *keys,
		int nkeys, void **values, size_t *vlens, int *status);

/* keys matching @match (NULL = all), up to @limit (0 = server default): the count, and *keys_out
 * a malloc'd array of malloc'd strings for perfd_free_keys; -1 on error */
int perfd_keys(perfd_t *p, const char *col, const char *match, int limit,
		char ***keys_out);
/* as perfd_keys, with *flags: PARTIAL = the listing is the answering node's share only (a sharded
 * collection on a fleet), TRUNCATED = it stopped at @limit */
#define PERFD_HAS_KEYS_EX 1
#define PERFD_KEYS_PARTIAL   0x1
#define PERFD_KEYS_TRUNCATED 0x2
int perfd_keys_ex(perfd_t *p, const char *col, const char *match, int limit,
		char ***keys_out, int *flags);
void perfd_free_keys(char **keys, int n);

/* JSON path verbs: fragments travel as JSON text (out malloc'd, NUL-terminated); a NULL path is "$".
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
 * returns the receivers on the node reached, (un)subscribe the handle's count (NULL name = all) ---- */
int perfd_publish(perfd_t *p, const char *channel, const void *payload,
		size_t plen);
int perfd_subscribe(perfd_t *p, const char *channel);
int perfd_psubscribe(perfd_t *p, const char *pattern);
int perfd_unsubscribe(perfd_t *p, const char *channel);
int perfd_punsubscribe(perfd_t *p, const char *pattern);

/* ---- pub/sub over UDP (lib/perfd_push.h): deliveries as sealed datagrams to the same hook; call
 * _ready on readability AND once a second - 15 s without an ack drops every subscription.  The
 * stream's key arrives in perfd_pubsub_udp's reply, so the seal is only as private as the link:
 * on a plaintext one (opts.encrypt = 0, the default) whoever reads the connection can read and
 * forge the datagrams - open the handle with encrypt = 1 where that matters (the first such call
 * in a process logs a warning) ---- */
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

/* byte-exact tree forms (src/ptree.h, in libperfd.a only): @params one encoded map or NULL; a
 * pipeline may mix text and tree requests, each reply in the form asked */
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

/* notifications go to the hook, if set: @json not NUL-terminated, valid only during the call; a hook
 * run inside a blocking call may not call its handle (PERFD_E_STATE), one between calls may */
typedef void (*perfd_notify_cb)(const char *json, size_t len, void *ctx);
void perfd_set_notify(perfd_t *p, perfd_notify_cb cb, void *ctx);

/* ---- event-loop surface: opt-in, never blocks, never calls back from a thread of its own.  Watch
 * perfd_fd() for perfd_events(), call *_ready(), submit once READY; replies in ARRIVAL order ---- */
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
 * the readiness calls; NULL only if no socket could be created.  It tries opts.secrets[0] only:
 * a node that refuses it fails the handle as PERFD_E_AUTH, and the caller dials again from the next */
perfd_t *perfd_connect_async(const char *host, int port,
		const perfd_opts *opts);

/* the index in opts.secrets of the secret the handshake used - or, after an async PERFD_E_AUTH,
 * tried: dial again with opts.secrets + index + 1.  -1 = plaintext or NULL. */
#define PERFD_HAS_SECRET_INDEX 1
int perfd_secret_index(const perfd_t *p);

/* per-request completion: the callback OWNS @result (perfd_free_value); NULL = failed, @errmsg
 * says why.  A submit reply is JSON text, a submit_kv reply the verb's raw payload. */
typedef void (*perfd_reply_cb)(char *result, size_t len, const char *errmsg,
		void *ctx);

/* queue a request and write what the socket takes; never blocks.  0 ok, -1 error; legal only in
 * PERFD_ST_READY */
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

/* ---- URLs, conditional jset, describing a handle (libperfd 0.7.6.1) ---- */

/* perfd[:group]://[user[:secret]@]host:port[,host:port...]/collection[?opt,opt...]: names, IPv4 or
 * [IPv6], ports 1-65535, up to 8 seeds; %XX escapes in user and secret; options split by , or & */
#define PERFD_HAS_URL 1
#define PERFD_URL_MAX_SEEDS 8
typedef struct perfd_url perfd_url;
/* NULL with the reason in @err (may be NULL); every option name and value is checked:
 * route=0|1, encrypt=0|1, spares=-1..8 (0 none, -1 one per member), policy=failover|rr|least|weighted */
perfd_url *perfd_url_parse(const char *url, char *err, size_t errcap);
void perfd_url_free(perfd_url *u);
const char *perfd_url_collection(const perfd_url *u);
/* the password, or the user when there is none; NULL = none */
const char *perfd_url_secret(const perfd_url *u);
/* the group after "perfd:", NULL = none */
const char *perfd_url_group(const perfd_url *u);
/* seed @i (0..count-1): host (an IPv6 one without brackets) and port; 0, or -1 = no such seed */
int perfd_url_seed_count(const perfd_url *u);
int perfd_url_seed(const perfd_url *u, int i, const char **host, int *port);
/* writes route_keys, encrypt, spares and policy into @o - what the URL says, else route 1, encrypt 1,
 * one standby per member, failover - and secrets when it has one (valid while @u lives) */
void perfd_url_apply(const perfd_url *u, perfd_opts *o);
/* a blocking handle from the seeds (start rotated by pid; @o as perfd_connect, the URL applied); after
 * a dial no seed answered it fails at once (PERFD_E_BACKOFF) 1 s doubling to 30 s - one thread per @u */
perfd_t *perfd_url_connect(perfd_url *u, const perfd_opts *o);

/* as perfd_jset, conditional: NX only where the path holds nothing, XX only where it holds a value;
 * 1 set, 0 declined, -1 error.  A conditional one is never replayed after a failover. */
#define PERFD_JSET_NX 1
#define PERFD_JSET_XX 2
PERFD_MUST_CHECK int perfd_jset_ex(perfd_t *p, const char *col,
		const char *key, const char *path, const char *json,
		long long ttl, int flags);

/* one line into @buf: the server's version, members learned, routing on or off, standbys held;
 * the length it needed (as snprintf), -1 on a NULL handle */
int perfd_describe(const perfd_t *p, char *buf, size_t cap);
/* moves whenever this handle's member list, or a member's state as it sees it (ready, draining,
 * down), changes - compare two readings */
unsigned long long perfd_fleet_changes(const perfd_t *p);

/* the RESP door's keyed data commands on @col, each on its key's node: GET SET SETEX PSETEX DEL UNLINK
 * EXISTS EXPIRE PEXPIRE TTL PTTL INCR INCRBY DECR DECRBY MGET MSET, the H commands, JSON.GET/SET/DEL */
#define PERFD_HAS_EXEC 1
enum perfd_reply_type {
	PERFD_REPLY_NIL = 0, PERFD_REPLY_INTEGER, PERFD_REPLY_STRING,
	PERFD_REPLY_STATUS, PERFD_REPLY_ERROR, PERFD_REPLY_ARRAY
};
typedef struct perfd_reply {
	int type;                      /* enum perfd_reply_type */
	long long integer;             /* INTEGER */
	char *str;                     /* STRING, STATUS, ERROR: NUL-terminated, @len bytes */
	size_t len;
	size_t elements;               /* ARRAY */
	struct perfd_reply **element;
} perfd_reply;
/* the reply as the RESP door shapes it - a daemon's error, or a command not in the list, is an ERROR
 * reply; NULL when the call failed (perfd_error).  @argvlen NULL: the arguments are C strings */
perfd_reply *perfd_exec(perfd_t *p, const char *col, int argc,
		const char *const *argv, const size_t *argvlen);
void perfd_reply_free(perfd_reply *r);

/* ---- the async pool (0.7.6.1): for loops that watch one fd per request, every in-flight GET has
 * an event-loop handle of its own.  Per process, not thread-safe; after fork() it resets itself ---- */
#define PERFD_HAS_APOOL 1
typedef struct perfd_apool perfd_apool;
/* @hosts/@ports: 1 to 8 seeds, tried from a start rotated by the pid.  With opts.route_keys a GET
 * goes to a handle pinned to its key's owner, else to a seed.  @max_conns 1 to 1024 handles */
perfd_apool *perfd_apool_new_seeds(const char *const *hosts, const int *ports,
		int nseeds, const char *col, const perfd_opts *opts, int max_conns);
/* the same from a parsed perfd:// URL (its options applied over @o, which may be NULL) */
perfd_apool *perfd_apool_new(perfd_url *u, const perfd_opts *o, int max_conns);
void perfd_apool_free(perfd_apool *pool);     /* in-flight callbacks never run */

/* submit GET @key: @cb gets the get reply ([u8 found][u32 ttl][value]) or NULL + errmsg.  The fd to
 * watch for reading, PERFD_APOOL_DONE (@cb already ran), or -1 (@cb not called; perfd_error(NULL)) */
#define PERFD_APOOL_DONE (-2)
/* a new handle's dial blocks the get dial_wait_ms at most (500; connect_timeout if lower): one still
 * connecting fails the get at once (-1) and is finished, without blocking, by the gets after it */
int perfd_apool_get(perfd_apool *pool, const char *key, perfd_reply_cb cb,
		void *ctx);
/* on readiness of @fd: 1 = @cb ran (reply or error), 0 = still pending, -1 = no request on @fd */
int perfd_apool_ready(perfd_apool *pool, int fd);
/* block until @cb ran (1) or @timeout_ms passed (0: cancelled, @cb never runs); -1 as above */
int perfd_apool_wait(perfd_apool *pool, int fd, int timeout_ms);
/* give up on the request on @fd: @cb never runs, the handle is rebuilt at a later get - stop
 * watching @fd before then */
void perfd_apool_cancel(perfd_apool *pool, int fd);

/* the node the request on @fd went to; 0, or -1 = no request on @fd */
int perfd_apool_node(perfd_apool *pool, int fd, char *addr, size_t acap,
		int *port);
struct perfd_apool_stats {
	int slots;                     /* handles held */
	int busy;                      /* requests in flight */
	int routing;                   /* 1 = GETs go to their key's owner */
	unsigned long long gets;       /* perfd_apool_get calls */
	unsigned long long dials;      /* connections opened, router included */
	unsigned long long dial_failures;
	unsigned long long rebuilt;    /* failed or cancelled handles replaced */
	unsigned long long evicted;    /* idle handles of another node re-dialled */
	unsigned long long cancelled;
	unsigned long long resets;     /* fork() resets */
};
int perfd_apool_stats(perfd_apool *pool, struct perfd_apool_stats *st);

#endif /* PERFD_H */
