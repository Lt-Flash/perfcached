/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* proto.h - connections and the native door's protocol: binary frames, or RESP2 sniffed on the
 * first byte; replies and notifications share one out-of-order queue per connection. */
#ifndef PC_PROTO_H
#define PC_PROTO_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "pc_noise.h"

/* PSKs derived once at startup: Argon2id is never paid per connection, and trying each client
 * PSK is cheap because msg1 carries no DH */
struct pc_psk_ctx {
	uint8_t client[PC_MAX_CLIENT_SECRETS][PC_NOISE_KEYLEN];
	int     n_client;
	uint8_t cluster[PC_NOISE_KEYLEN];
	int     have_cluster;       /* 0: no cluster secret, principal refused */
	uint8_t cluster_accept[PC_NOISE_KEYLEN];   /* tried second */
	int     have_cluster_accept;
};

/* frame, integers LE: magic | version | type | flags | payload length u32 | id u64 | payload;
 * a request payload is [verb u8], the verb's fixed fields, then col/key/value bytes raw */
#define PC_BIN_MAGIC   0x9E
#define PC_BIN_VER     0x01
#define PC_BIN_REQ     1
#define PC_BIN_RSP     2
#define PC_BIN_NOTIFY  3
#define PC_BIN_F_ERR   0x01            /* error response: the payload is an ASCII message */
#define PC_BIN_F_MOVED 0x02            /* [term u32][seq u32] of the routing map follows the tree */
/* on a request: "where is the key"; only then does a reply another node answered trail
 * [node u32 LE][ttl u32 LE, s; 0 none, 0xFFFFFFFF unknown], after any moved stamp */
#define PC_BIN_F_HINT  0x04
#define PC_HINT_TTL_UNKNOWN 0xFFFFFFFFu
#define PC_BIN_HDR     16
/* data verbs: [cn u8][klen u16], expire/set [ttl i64], add/sub [by i64][ttl i64], [col][key],
 * set [value...]; replies a u8 result, get [found u8][ttl_left u32, 0 = no expiry][value] */
#define PC_VERB_PING   1               /* echo bytes, mirrored */
#define PC_VERB_GET    2
#define PC_VERB_SET    3
#define PC_VERB_DEL    4
#define PC_VERB_EXISTS 5
#define PC_VERB_TTL    6               /* reply [seconds i64: -2 absent, -1 none] */
#define PC_VERB_EXPIRE 7
#define PC_VERB_ADD    8               /* reply [value i64] */
#define PC_VERB_SUB    9
/* verbs 10-14 are retired: pub/sub rides PC_VERB_CMD, deliveries are NOTIFY frames of trees */
/* SET only if the key is absent / present; SET's payload, reply [stored u8], 0 = declined.
 * Verbs, not a SET flag: an older daemon answers "unknown verb" instead of storing anyway. */
#define PC_VERB_SETNX        15
#define PC_VERB_SETXX        16
/* RL.HIT: ADD's payload (window_ms in by, limit in ttl, 0 none) -> [count i64][allowed u8];
 * never replayed by a client: a replay counts the hit twice */
#define PC_VERB_RLHIT        17
/* any H command: value [argc u16] then argc x [len u32][bytes], argv[0] the name, then the args
 * after the key -> the reply tree.  Never replayed by a client: HINCRBY twice is not once. */
#define PC_VERB_HCMD         18
/* any native method: [mlen u8][method][params: ONE tree map, optional] -> one tree item or an
 * error frame; NOTIFY payloads are tree maps {"method": name, "params": map} */
#define PC_VERB_CMD          19

#define PC_MAX_REQ     (1u << 20)      /* one frame payload / RESP command */
#define PC_MAX_OUTQ    (8u << 20)      /* pending replies; beyond = slow consumer */
#define PC_UR_HOLD_MAX (16u << 20)     /* io_uring: input taken off the socket before a pause's cancel */
/* pending replies at PAUSE_AT stop a connection being read or served until they drain below
 * RESUME_AT; PC_MAX_OUTQ stays the hard cap for unpaced output (pushes, one huge reply) */
#define PC_OUT_PAUSE_AT  (4u << 20)
#define PC_OUT_RESUME_AT (1u << 20)

struct pc_conn;

/* a connection's kind is pinned by its listener at accept and never widened: a narrow listener
 * is the whole security model of the two plaintext kinds */
#define PC_LK_NATIVE 0                 /* binary frames, or RESP2 sniffed on the first byte */
#define PC_LK_RESP   1                 /* RESP2 only, peers vs resp_allow */
#define PC_LK_HTTP   2                 /* plaintext GET /metrics, /health; peers vs metrics_allow */
#define PC_LK_TLS    0x10   /* or'ed into a RESP or HTTP listener's kind */
/* pc_conn_accept's plaintext_ok: 0 Noise only (@psk required); PC_DOOR_PLAIN in the clear
 * unauthenticated; PC_DOOR_AUTH Noise or the plaintext hello */
#define PC_DOOR_PLAIN 1
#define PC_DOOR_AUTH  2

/* the client limit across the data doors (native, unix, RESP; not HTTP), set once at boot from
 * the config and the descriptor limit */
extern int pc_keepalive_s;                     /* [listen] keepalive_s */
extern int pc_max_clients;
extern const char *pc_max_clients_basis;
extern unsigned int pc_clients_open;
extern unsigned long long pc_clients_refused;
int pc_conn_secure(const void *conn);       /* TLS or a loopback peer */
/* accept a listener's pending connections onto @ep and @list; 1 = the turn's budget stopped it
 * with more perhaps waiting (an io_uring poll does not report the listener again) */
int pc_conn_accept(int ep, int lfd, int plaintext_ok, int kind,
		const struct pc_psk_ctx *psk, struct pc_conn **list);
/* the PSK table new connections take (daemon.c; a reload replaces it) */
const struct pc_psk_ctx *pc_psk_current(void);

/* close HTTP connections whose request head never arrived (@timeout_s 0: off) and return how
 * many remain; called from the worker that owns @list */
int pc_conn_sweep_http(struct pc_conn **list, int timeout_s);
/* this worker's open HTTP connections, now: what the worker's wait must
 * read, since a connection accepted after the sweep is not in its count */
int pc_conn_http_open(void);
/* free idle connections' buffers (once a second; call it every turn, it stamps the tick); then
 * the connections holding buffers, this worker's and the node's */
void pc_conn_sweep_idle(struct pc_conn **list);
int pc_conn_bufs_held(void);
int pc_conn_bufs_held_all(void);
/* pub/sub pushes are staged per worker turn: a worker calls batching once
 * as it starts and flush_pushes when each turn's events are done */
void pc_conn_push_batching(void);
void pc_conn_flush_pushes(void);
/* publishers paused on a full queue: resumed before each wait once drained; the wait is a short
 * timer while any are paused */
void pc_conn_resume_paused(void);
int pc_conn_paused_open(void);
/* refused HTTP peers parked for their drain (non-zero: wake within 50 ms), and the drain -
 * each worker turn */
int pc_conn_deny_open(void);
/* the HTTP door's cache dropped (a stats reset), and its counters:
 * requests answered 429, answers from the cache, builds */
void pc_http_cache_drop(void);
void pc_http_counters(unsigned long long *limited, unsigned long long *hits,
		unsigned long long *builds);
void pc_conn_sweep_deny(void);
/* this worker's output-paused connections, served again (or closed
 * for a stall) once a turn; the counts for /stats and /metrics */
void pc_conn_resume_out(void);
int pc_conn_out_paused_open(void);
/* writers held while the worker's WAL ring is nearly full */
int pc_conn_wal_paused_open(void);
void pc_conn_resume_wal(void);
void pc_conn_wal_pause_stats(unsigned long long *now, unsigned long long *pauses);
struct pc_out_pause_stats { unsigned long long paused_now, pauses, stall_kills; };
void pc_conn_out_pause_stats(struct pc_out_pause_stats *s);
extern int pc_output_stall_s;               /* [daemon] output_stall_s: stalled that long, closed */
/* pub/sub pushes over UDP (lib/perfd_push.h): door_add runs on the main thread before the
 * workers start, the rest on the connection's worker */
void pc_udp_door_add(uint32_t addr_be, uint16_t port, int fd);
/* 0 with the stream id and the key in hex (65 bytes), 1 when port 0 turned it off, -1 with *err */
int pc_conn_udp_on(void *conn, int port, uint64_t *stream, char *key_hex,
		const char **err);
/* confirm: 0 with the sequence messages will follow; ack: 0; both -1 with *err */
int pc_conn_udp_confirm(void *conn, const char *cookie_hex, size_t n,
		uint64_t *seq, const char **err);
int pc_conn_udp_ack(void *conn, uint64_t seq, const char **err);
/* probe, expire and prune this worker's streams; the open count keeps the worker waking */
int pc_conn_sweep_udp(struct pc_conn **list);
int pc_conn_udp_open(void);         /* this worker's streams, now */
struct pc_udp_figures {
	long streams;
	unsigned long long probes, confirmed, expired, pushed, oversized,
		send_errors, acks, pruned_no_ack, pruned_no_progress;
};
void pc_conn_udp_figures(struct pc_udp_figures *f);
/* handle epoll events for a connection; returns 0, or -1 if the
 * connection was destroyed (and unlinked from its list) */
int pc_conn_event(struct pc_conn *c, uint32_t events);
/* io_uring: ep < 0 in pc_conn_accept registers on the worker's ring; completions come back via
 * ur_complete, and ur_retry arms what a full queue left unarmed */
struct pc_ur;
void pc_conn_uring_attach(struct pc_ur *r);
void pc_conn_ur_complete(uint64_t ud, int res, uint32_t flags, int *hs,
		unsigned int *hs_deferred);
void pc_conn_ur_retry(void);
/* handshakes a worker turn runs (~0.13 ms of crypto each); the rest park and run oldest first
 * later, so established clients' requests never queue behind a handshake backlog */
#define PC_HS_PER_TURN 8
/* accepts per listener per turn, so a connect storm cannot stall established clients; at 1-2 ms
 * a turn that is still over 30,000 accepts/s a worker */
#define PC_ACCEPT_PER_TURN 64
/* this event would run a handshake: an encrypted connection not yet
 * established, readable, neither hung up nor in error */
int pc_conn_hs_pending(const struct pc_conn *c, uint32_t events);
void pc_conn_hs_park(struct pc_conn *c);      /* put its handshake off */
int pc_conn_hs_run(int budget);               /* run up to @budget parked */
int pc_conn_hs_parked(void);                  /* this worker's parked count */
void pc_conn_hs_deferred(unsigned int n);            /* a turn's deferrals */
unsigned long long pc_conn_hs_deferred_total(void);  /* /stats */

/* the cooperative KEYS walk: pending() picks the worker's epoll timeout (0 while active), step()
 * runs once a turn and advances every active walk by one bounded chunk */
int pc_enum_pending(void);
unsigned long long pc_enum_ended_on_swap(void);
/* cooperative KEYS walks finished, and the turns they took */
void pc_enum_figures(unsigned long long *walks, unsigned long long *turns);
void pc_enum_step(void);

/* enqueue a NOTIFY frame of ONE encoded tree item (src/ptree.h); RESP clients and connections
 * still sniffing have no frame for it and are skipped */
int pc_conn_notify(struct pc_conn *c, const char *payload, size_t n);
/* the same tree to every binary client on a worker's list (RESP
 * and HTTP have no notification frame); on that worker */
void pc_conn_notify_all(struct pc_conn *list, const char *payload, size_t n);

void pc_conn_destroy_all(struct pc_conn **list);
/* free the connections destroyed since the last call - only between
 * epoll batches (conn_destroy() defers the free, see proto.c) */
void pc_conn_reap(void);

/* pull-park: a deferred get parks on ITS worker; the completion (posted by the peer thread) builds
 * the reply in the request's framing and stores the value PASSIVE - all thread-local */
#include "cluster.h"
void pc_proto_pull_complete(const struct pc_pull_done *d);
/* a pull found the key: keep this node's share (a passive copy in store mode, the holder's name
 * in proxy mode; nothing in shard, nor in spread off the key's holders) */
void pc_pull_found_keep(const char *col, size_t collen, const char *key,
		size_t klen, const struct pc_pull_done *d);

/* read-through (src/upstream.c): hold a connection on @req; the completion, drained on this
 * worker, re-runs the buffered command with the verdict (PC_UPV_*) readable by the gate */
void pc_conn_upstream_hold(void *conn, unsigned int req);
/* native doors: the frame being dispatched parks under @req (copied once the verb returns); the
 * verdict a re-run carries (PC_UPV_NONE on a first run) */
void pc_conn_upstream_park_frame(unsigned int req);
int pc_conn_upstream_frame_verdict(void);
/* the connection named itself perfcached-readthrough */
void pc_conn_set_rt_fetcher(void *conn);
int pc_conn_rt_fetcher(void *conn);
/* the peer as "ip:port" text (the CLIENT LIST row's), or NULL */
const char *pc_conn_peer_text(void *conn);
/* name the connection (the native `name` verb; RESP has CLIENT SETNAME) */
void pc_conn_set_name(void *conn, const char *name, size_t n);
/* "name@ip:port", or "ip:port" unnamed - for the audit lines */
const char *pc_conn_who(void *conn, char *buf, size_t cap);
/* 1 when @conn was authenticated by the plaintext hello */
int pc_conn_plain_auth(const void *conn);
int pc_conn_upstream_verdict(void *conn);
/* re-gate a read that lost its key - 1 yes (verdict cleared), 0 not again */
int pc_conn_upstream_retry(void *conn);
/* upstream.c's removal count when the gate parked the command - its
 * run after the fetch compares against it */
void pc_conn_upstream_set_mark(void *conn, unsigned long long m);
unsigned long long pc_conn_upstream_mark(void *conn);
void pc_proto_upstream_complete(unsigned int req, int status);
/* holder hints sent on parked replies, and hint-asking requests a
 * proxy collection answered from this node's own table */
void pc_proto_hint_stats(unsigned long long *sent, unsigned long long *local);

#endif /* PC_PROTO_H */
