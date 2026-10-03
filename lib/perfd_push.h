/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov
 * Part of libperfd - see lib/LICENSE.  This file must stay
 * free of src/core includes; tools/sync-libperfd.sh exports
 * exactly the MIT set to consumers. */
/*
 * perfd_push.h - pub/sub pushes over UDP (PS5): the datagram both ends
 * agree on.  The daemon seals, libperfd opens.
 *
 * A native connection asks for its deliveries over UDP to a port it names
 * (`pubsub_udp {"port":N}`).  The address is never the client's to name:
 * it is the connection's own TCP peer.  The daemon answers with a stream
 * id and a random key over the connection, then sends PROBES from the
 * door's own address and port; nothing else goes over UDP until the
 * client echoes a probe's cookie over the connection
 * (`pubsub_udp_confirm`), which proves the datagrams reach it.  From
 * then on every delivery that fits PFP_MAX goes as a sealed datagram and
 * larger ones stay on the connection.  The client acknowledges the
 * highest sequence it has seen (`pubsub_udp_ack`) every PFP_ACK_EVERY
 * messages or PFP_ACK_MS, whichever comes first, and on the timer even
 * when nothing arrives.  After PFP_PRUNE_MS without an acknowledgement,
 * or of sending past one that stopped moving, the daemon drops every
 * subscription on the connection and says so over it
 * (`pubsub_udp_pruned`).  There is no retransmit: the client counts gaps
 * and rejects duplicates.
 *
 * THE DATAGRAM
 *     header (in clear, authenticated):
 *         [magic 1][version 1][type 1][stream 8][seq 8]
 *     sealed with ChaCha20-Poly1305 (IETF) under the stream's key, nonce =
 *     [0 0 0 0][seq 8] - the key is the stream's alone and the sequence
 *     never repeats, so neither does the nonce:
 *         PFP_PROBE    [cookie 16]
 *         PFP_MESSAGE  [kind 1][plen 2][pattern][clen 2][channel][dlen 4][payload]
 *                      kind 0 = message, 1 = pmessage
 *     then the 16-byte tag.  Integers are big-endian.  Probes and
 *     messages share the one sequence.
 */
#ifndef PERFD_PUSH_H
#define PERFD_PUSH_H

#include <stdint.h>
#include <string.h>

#define PFP_MAGIC        0x70
#define PFP_VERSION      1
#define PFP_PROBE        1
#define PFP_MESSAGE      2
#define PFP_HDR          19
#define PFP_TAG          16
#define PFP_MAX          1400          /* the whole datagram; larger: TCP */
#define PFP_KEY          32
#define PFP_COOKIE       16
#define PFP_ACK_EVERY    256
#define PFP_ACK_MS       5000
#define PFP_PRUNE_MS     15000

static inline void pfp_put16(unsigned char *p, unsigned v)
{
	p[0] = (unsigned char)(v >> 8);
	p[1] = (unsigned char)v;
}

static inline void pfp_put32(unsigned char *p, uint32_t v)
{
	pfp_put16(p, (unsigned)(v >> 16));
	pfp_put16(p + 2, (unsigned)v);
}

static inline void pfp_put64(unsigned char *p, uint64_t v)
{
	pfp_put32(p, (uint32_t)(v >> 32));
	pfp_put32(p + 4, (uint32_t)v);
}

static inline unsigned pfp_get16(const unsigned char *p)
{
	return ((unsigned)p[0] << 8) | p[1];
}

static inline uint32_t pfp_get32(const unsigned char *p)
{
	return ((uint32_t)pfp_get16(p) << 16) | pfp_get16(p + 2);
}

static inline uint64_t pfp_get64(const unsigned char *p)
{
	return ((uint64_t)pfp_get32(p) << 32) | pfp_get32(p + 4);
}

static inline void pfp_header(unsigned char *h, unsigned type, uint64_t stream,
		uint64_t seq)
{
	h[0] = PFP_MAGIC;
	h[1] = PFP_VERSION;
	h[2] = (unsigned char)type;
	pfp_put64(h + 3, stream);
	pfp_put64(h + 11, seq);
}

static inline void pfp_nonce(unsigned char n[12], uint64_t seq)
{
	memset(n, 0, 4);
	pfp_put64(n + 4, seq);
}

#endif /* PERFD_PUSH_H */
