/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * proto.h — connection + dual-dialect protocol machinery (task S7).
 *
 * Dialect is PER MESSAGE (revised with the binary data verbs, 08-25;
 * S7 pinned it per connection): each message self-describes on its
 * first byte - '{'/whitespace opens a newline-delimited JSON-RPC line,
 * 0x9E opens a binary frame, anything else is a protocol error.  One
 * connection carries both, so libperfd routes the hot data verbs
 * binary while the long-tail verbs (mget/keys/scan/json/stats) stay
 * text - no second connection, one id space, one reply matcher.  The
 * FIRST message still sets c->dialect as a memo: it picks the framing
 * for server-initiated notifications only.  Replies and notifications
 * share one outgoing queue per connection, flushed via EPOLLOUT - the
 * out-of-order reply core: nothing about the wire requires answers in
 * request order.
 *
 * Binary frame (all integers little-endian, per the portability rules):
 *   magic 0x9E (1) | version 0x01 (1) | type (1) | flags (1)
 *   | payload length (4) | id (8) | payload...
 *   type: 1 request, 2 response, 3 notification
 *   flags bit0: error response (payload = ASCII message)
 *   payload byte 0 on requests: verb; the rest is the verb's fixed
 *   fields, then col/key/value bytes RAW (no escaping, no b64 leg):
 *     ping    echo bytes (mirrored)
 *     get/exists/ttl/del  [cn u8][klen u16][col][key]
 *     expire  [cn u8][klen u16][ttl i64][col][key]
 *     add/sub [cn u8][klen u16][by i64][ttl i64][col][key]
 *     set     [cn u8][klen u16][ttl i64][col][key][value...]
 *   response payloads:
 *     ping    echo bytes            get   [found u8][ttl_left u32][value]
 *     set     [stored u8]           del   [deleted u8]
 *     exists  [exists u8]           ttl   [seconds i64: -2 absent, -1 none]
 *     expire  [updated u8]          add/sub  [value i64]
 *   (ttl_left/get 0 = no expiry; errors ride flags bit0 instead.)
 *   Batch/scan/JSON verbs have no binary encoding - send them as text
 *   on the same connection.
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
};

#define PC_BIN_MAGIC   0x9E
#define PC_BIN_VER     0x01
#define PC_BIN_REQ     1
#define PC_BIN_RSP     2
#define PC_BIN_NOTIFY  3
#define PC_BIN_F_ERR   0x01
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

#define PC_MAX_REQ     (1u << 20)      /* one request line / frame payload */
#define PC_MAX_OUTQ    (8u << 20)      /* pending replies; beyond = slow consumer */

struct pc_conn;

/* drain a listener: accept everything pending into new connections on
 * @ep, linked into @list.  plaintext_ok = this listener may speak the
 * plaintext dialects in the clear (loopback under plaintext=loopback);
 * otherwise the connection runs the Noise handshake first, authenticated
 * by @psk (NULL only ever paired with plaintext_ok). */
/* @resp_only: this listener speaks RESP2 and nothing else (task S33) -
 * the connection is pinned to the dialect at accept, peers outside the
 * configured allow-list are refused here, and the native verb surface
 * (admin included) is unreachable from it. */
void pc_conn_accept(int ep, int lfd, int plaintext_ok, int resp_only,
		const struct pc_psk_ctx *psk, struct pc_conn **list);

/* handle epoll events for a connection; returns 0, or -1 if the
 * connection was destroyed (and unlinked from its list) */
int pc_conn_event(struct pc_conn *c, uint32_t events);

/* S40: the cooperative KEYS walk.  The worker loop polls pending() to
 * pick its epoll timeout (0 while walks are active) and calls step()
 * once per turn; each call advances every active walk by one bounded
 * chunk and completes the ones that finished. */
int pc_enum_pending(void);
void pc_enum_step(void);

/* enqueue a server-initiated notification (id-less JSON line on text
 * connections, a type-3 frame on binary ones).  First real consumers:
 * membership/steering (M4/M5). */
int pc_conn_notify(struct pc_conn *c, const char *payload, size_t n);

void pc_conn_destroy_all(struct pc_conn **list);

/* ---- the pull-park surface (M4) ----------------------------------------
 * text_line parks a deferred get on ITS worker thread; the completion
 * (posted by the peer thread, drained by this worker) builds the reply
 * and stores the pulled value PASSIVE.  All thread-local: a dying conn
 * invalidates its parked entries on the same thread. */
#include "cluster.h"
void pc_proto_pull_complete(const struct pc_pull_done *d);

#endif /* PC_PROTO_H */
