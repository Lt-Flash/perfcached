/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * proto.h — connection + protocol machinery (task S7; S317 made the
 * native door binary-only).
 *
 * ONE protocol on the native door: binary frames.  Each message still
 * self-describes on its first byte (S7 pinned it per connection, the
 * binary data verbs made it per message, 08-25): 0x9E opens a frame,
 * '*' or a letter a RESP2 command (redis-cli on the native port, task
 * S29), and anything else is answered one plain-text line naming the
 * two and closed.  Until 0.4.5 a '{' or whitespace opened the JSON-RPC
 * text dialect; it was removed whole (DESIGN 12if) - JSON survives as
 * DATA only (the document type, the HTTP bodies), never as a protocol.
 * The FIRST message sets c->dialect as a memo: it picks whether
 * server-initiated notifications can be framed (RESP clients get none).
 * Replies and notifications share one outgoing queue per connection,
 * flushed via EPOLLOUT - the out-of-order reply core: nothing about the
 * wire requires answers in request order.
 *
 * Binary frame (all integers little-endian, per the portability rules):
 *   magic 0x9E (1) | version 0x01 (1) | type (1) | flags (1)
 *   | payload length (4) | id (8) | payload...
 *   type: 1 request, 2 response, 3 notification
 *   flags bit0: error response (payload = ASCII message)
 *   flags bit1: S317, the RV-10 map stamp follows the reply tree
 *   payload byte 0 on requests: verb; the rest is the verb's fixed
 *   fields, then col/key/value bytes RAW (no escaping, no b64 leg):
 *     ping    echo bytes (mirrored)
 *     get/exists/ttl/del  [cn u8][klen u16][col][key]
 *     expire  [cn u8][klen u16][ttl i64][col][key]
 *     add/sub [cn u8][klen u16][by i64][ttl i64][col][key]
 *     set     [cn u8][klen u16][ttl i64][col][key][value...]
 *     cmd     [mlen u8][method][params tree?]   (PC_VERB_CMD below)
 *   response payloads:
 *     ping    echo bytes            get   [found u8][ttl_left u32][value]
 *     set     [stored u8]           del   [deleted u8]
 *     exists  [exists u8]           ttl   [seconds i64: -2 absent, -1 none]
 *     expire  [updated u8]          add/sub  [value i64]
 *     cmd     one tree item (src/ptree.h)
 *   (ttl_left/get 0 = no expiry; errors ride flags bit0 instead.)
 *   Every method without a fixed layout - mget, keys, scan, the JSON
 *   document methods, stats, pub/sub, the admin surface - rides cmd.
 *
 * Until the Noise channel lands (S25'), connections are only served on
 * plaintext-ELIGIBLE listeners (loopback/unix under plaintext=loopback);
 * everything else is refused loudly - the encryption promise is never
 * silently broken.
 */
#ifndef PC_PROTO_H
#define PC_PROTO_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "pc_noise.h"

/* PSKs derived once at startup (Argon2id is never paid per connection).
 * The responder tries each client PSK for a client-principal handshake
 * (msg1 carries no DH, so a failed attempt is cheap) and the single
 * cluster PSK for a cluster-principal one. */
struct pc_psk_ctx {
	uint8_t client[PC_MAX_CLIENT_SECRETS][PC_NOISE_KEYLEN];
	int     n_client;
	uint8_t cluster[PC_NOISE_KEYLEN];
	int     have_cluster;       /* 0: no cluster secret, principal refused */
};

#define PC_BIN_MAGIC   0x9E
#define PC_BIN_VER     0x01
#define PC_BIN_REQ     1
#define PC_BIN_RSP     2
#define PC_BIN_NOTIFY  3
#define PC_BIN_F_ERR   0x01
#define PC_BIN_F_MOVED 0x02            /* S317: a map stamp follows the tree */
/* S295: on a REQUEST, "tell me where the key is"; on the reply to a data
 * verb that another node answered (a proxy pull or a forward), the holder
 * hint [node u32 LE][ttl u32 LE, seconds; 0 none, 0xFFFFFFFF unknown]
 * trails the payload, after any moved stamp.  Only a request that set it
 * gets one: an older library would read the 8 bytes as part of a value. */
#define PC_BIN_F_HINT  0x04
#define PC_HINT_TTL_UNKNOWN 0xFFFFFFFFu
#define PC_BIN_HDR     16
#define PC_VERB_PING   1
#define PC_VERB_GET    2
#define PC_VERB_SET    3
#define PC_VERB_DEL    4
#define PC_VERB_EXISTS 5
#define PC_VERB_TTL    6
#define PC_VERB_EXPIRE 7
#define PC_VERB_ADD    8
#define PC_VERB_SUB    9
/* S317: verbs 10-14 (the binary pub/sub verbs of PS4) are gone - publish
 * and the four subscription methods ride PC_VERB_CMD like every other
 * method; deliveries are NOTIFY frames carrying trees. */
/* S279b: SET only if the key is absent / present - SET's payload
 * ([..][ttl i64][col][key][value]), reply [stored u8], 0 = the condition
 * declined.  Verbs, not a flag on SET: an older daemon answers "unknown
 * verb" instead of storing unconditionally. */
#define PC_VERB_SETNX        15
#define PC_VERB_SETXX        16
/* S314: RL.HIT - ADD's payload ([..][window_ms i64 in by][limit i64 in
 * ttl, 0 = none][col][key]), reply [count i64][allowed u8].  Never
 * replayed by a client: a replay counts the hit twice. */
#define PC_VERB_RLHIT        17
/* S313: any H command - the value part is [argc u16][(len u32)(bytes)] x
 * argc, argv[0] the command name, the rest its arguments AFTER the key
 * (the key is the frame's); the reply payload is the command's reply tree
 * (verbs.c: 'i' int, 'b' bulk, 'n' nil, 'a' array, 'o' OK, 'e' error).
 * Never replayed by a client: HINCRBY twice is not HINCRBY once. */
#define PC_VERB_HCMD         18
/* S317: any native method.  The native door speaks this binary protocol
 * only (the JSON-RPC text dialect was removed in 0.4.5, DESIGN 12if).
 * Payload: [mlen u8][method bytes] then, optionally, the parameters as
 * ONE tree item - a map (src/ptree.h: i d b n t f a m o e).  The reply
 * payload is one tree item; a failed method is an error frame
 * (PC_BIN_F_ERR, the message as the payload).  When the daemon had to
 * forward or pull for the request (RV-10), the reply carries
 * PC_BIN_F_MOVED and the stamp of the map it routed by follows the tree:
 * [term u32][seq u32].  Notifications - pub/sub deliveries, the
 * membership push - are NOTIFY frames whose payload is a tree map
 * {"method": name, "params": map}, the shape the text lines had. */
#define PC_VERB_CMD          19

#define PC_MAX_REQ     (1u << 20)      /* one frame payload / RESP command */
#define PC_MAX_OUTQ    (8u << 20)      /* pending replies; beyond = slow consumer */

struct pc_conn;

/* drain a listener: accept everything pending into new connections on
 * @ep, linked into @list.  plaintext_ok = this listener may speak in
 * the clear (loopback under plaintext=loopback); otherwise the
 * connection runs the Noise handshake first, authenticated by @psk
 * (NULL only ever paired with plaintext_ok). */
/* @kind pins what a connection may speak, decided by its LISTENER and
 * never negotiated afterwards:
 *   PC_LK_NATIVE  binary frames, or RESP2 - sniffed on the first byte
 *                 (S7; S317 removed the JSON-RPC text dialect)
 *   PC_LK_RESP    RESP2 and nothing else (S33) - no native verbs, no
 *                 admin surface, peers checked against resp_allow here
 *   PC_LK_HTTP    plaintext GET-only HTTP (S46): /metrics and /health,
 *                 peers checked against metrics_allow here
 * A narrow listener is the whole security model for the two plaintext
 * kinds, so the pin happens at accept and cannot be widened later. */
#define PC_LK_NATIVE 0
#define PC_LK_RESP   1
#define PC_LK_HTTP   2

/* S161: the client-connection limit across the data doors (native, unix,
 * RESP; HTTP is outside it and inside the descriptor reserve).  The
 * effective limit is set once at boot from the config and the descriptor
 * limit; the counters are read by /stats and /metrics. */
extern int pc_keepalive_s;                     /* PS2: [listen] keepalive_s */
extern int pc_max_clients;
extern const char *pc_max_clients_basis;   /* S366 */
extern unsigned int pc_clients_open;
extern unsigned long long pc_clients_refused;
void pc_conn_accept(int ep, int lfd, int plaintext_ok, int kind,
		const struct pc_psk_ctx *psk, struct pc_conn **list);

/* S46: close HTTP connections whose request head never arrived, and
 * report how many remain open.  A worker calls this from its own loop
 * because it owns its connection list; @timeout_s of 0 disables. */
int pc_conn_sweep_http(struct pc_conn **list, int timeout_s);
/* this worker's open HTTP connections, now: what the worker's wait must
 * read, since a connection accepted after the sweep is not in its count */
int pc_conn_http_open(void);
/* S366: release the buffers of connections idle for a while (once a second,
 * cheap otherwise - call it every turn: it also stamps the turn's tick);
 * this worker's connections holding buffers (the wait stays bounded while
 * there are any); the node's */
void pc_conn_sweep_idle(struct pc_conn **list);
int pc_conn_bufs_held(void);
int pc_conn_bufs_held_all(void);
/* pub/sub pushes are staged per worker turn: a worker calls batching once
 * as it starts and flush_pushes when each turn's events are done */
void pc_conn_push_batching(void);
void pc_conn_flush_pushes(void);
/* publishers paused on a full queue: a worker resumes the ones whose
 * queues drained, before each wait, and waits no longer than a short
 * timer while any are paused */
void pc_conn_resume_paused(void);
int pc_conn_paused_open(void);
/* PS5: pub/sub pushes over UDP (lib/perfd_push.h has the protocol).
 * door_add registers a native TCP door's UDP socket, from the main thread
 * before the workers start.  The three verbs run on the connection's
 * worker: udp_on answers 0 with the stream id and the key in hex (65
 * bytes), 1 when port 0 turned it off, -1 with *err; confirm answers 0
 * with the sequence messages will follow, ack 0; both -1 with *err.  sweep_udp probes, expires and prunes this
 * worker's streams and returns how many are open, so the worker keeps
 * waking while any are. */
void pc_udp_door_add(uint32_t addr_be, uint16_t port, int fd);
int pc_conn_udp_on(void *conn, int port, uint64_t *stream, char *key_hex,
		const char **err);
int pc_conn_udp_confirm(void *conn, const char *cookie_hex, size_t n,
		uint64_t *seq, const char **err);
int pc_conn_udp_ack(void *conn, uint64_t seq, const char **err);
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
/* ST5: a worker turn runs at most this many Noise handshakes (~0.13 ms of
 * crypto each, so ~1 ms a turn).  A connection still to handshake past it
 * is PARKED - its bytes wait in the socket, its epoll registration asks for
 * nothing, and the next turns run the parked ones oldest first, before any
 * new one.  The established clients' requests run in every turn: they never
 * queue behind a backlog of handshakes (left in the level-triggered ready
 * list, one was returned on every turn, and a request waited behind all of
 * them: 1 worker, 6,000 connects/s - p99 63 ms that way, 89-100 ms before
 * any cap). */
#define PC_HS_PER_TURN 8
/* ST5: and accepts at most this many connections a turn per listener - an
 * accept loop drained the whole backlog of a connect storm in one turn
 * (allocation, socket options, epoll registration, a log line each) while
 * the established clients waited.  At ~1-2 ms a turn that is still over
 * 30,000 accepts a second a worker. */
#define PC_ACCEPT_PER_TURN 64
/* ST5: this event would run a handshake: an encrypted connection not yet
 * established, readable, neither hung up nor in error */
int pc_conn_hs_pending(const struct pc_conn *c, uint32_t events);
void pc_conn_hs_park(struct pc_conn *c);      /* put its handshake off */
int pc_conn_hs_run(int budget);               /* run up to @budget parked */
int pc_conn_hs_parked(void);                  /* this worker's parked count */
void pc_conn_hs_deferred(unsigned int n);            /* a turn's deferrals */
unsigned long long pc_conn_hs_deferred_total(void);  /* /stats */

/* S40: the cooperative KEYS walk.  The worker loop polls pending() to
 * pick its epoll timeout (0 while walks are active) and calls step()
 * once per turn; each call advances every active walk by one bounded
 * chunk and completes the ones that finished. */
int pc_enum_pending(void);
unsigned long long pc_enum_ended_on_swap(void);   /* S150 B */
/* S292: cooperative KEYS walks finished, and the turns they took */
void pc_enum_figures(unsigned long long *walks, unsigned long long *turns);
void pc_enum_step(void);

/* enqueue a server-initiated notification: @payload is ONE encoded tree
 * item (src/ptree.h - a map, {"method": name, "params": map} for pub/sub,
 * the flat membership map for S107), sent as a NOTIFY frame.  Only a
 * binary connection has a frame to carry it: RESP clients and one still
 * sniffing are skipped.  First real consumers: membership/steering
 * (M4/M5). */
int pc_conn_notify(struct pc_conn *c, const char *payload, size_t n);
/* S107: the same tree to every binary client on a worker's list (RESP
 * and HTTP have no notification frame); on that worker */
void pc_conn_notify_all(struct pc_conn *list, const char *payload, size_t n);

void pc_conn_destroy_all(struct pc_conn **list);
/* S356: free the connections destroyed since the last call - only between
 * epoll batches (conn_destroy() defers the free, see proto.c) */
void pc_conn_reap(void);

/* ---- the pull-park surface (M4) ----------------------------------------
 * bin_frame (a data verb's frame or a CMD method) and the RESP framer
 * park a deferred get on ITS worker thread; the completion (posted by
 * the peer thread, drained by this worker) builds the reply in the
 * request's framing - a fixed layout, a tree, a RESP reply - and stores
 * the pulled value PASSIVE.  All thread-local: a dying conn invalidates
 * its parked entries on the same thread. */
#include "cluster.h"
void pc_proto_pull_complete(const struct pc_pull_done *d);

/* S331: read-through (src/upstream.c).  The gate holds a connection on a
 * request id; the completion, drained on this worker, releases it and
 * runs the buffered command again, the verdict (PC_UPV_*) readable by the
 * gate on that run. */
void pc_conn_upstream_hold(void *conn, unsigned int req);
/* S331: the native doors - the frame being dispatched parks under @req
 * (proto copies it once the verb returns), and the verdict the re-run of
 * a parked frame carries (PC_UPV_NONE on a first run) */
void pc_conn_upstream_park_frame(unsigned int req);
int pc_conn_upstream_frame_verdict(void);
int pc_conn_upstream_verdict(void *conn);
void pc_proto_upstream_complete(unsigned int req, int status);
/* S295: holder hints sent on parked replies, and hint-asking requests a
 * proxy collection answered from this node's own table */
void pc_proto_hint_stats(unsigned long long *sent, unsigned long long *local);

#endif /* PC_PROTO_H */
