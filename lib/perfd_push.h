/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov
 * Part of libperfd - see lib/LICENSE.  This file must stay
 * free of src/core includes: the library is built from the MIT
 * set alone (`make print-libperfd`, test/mitboundtest.sh). */
/* perfd_push.h - pub/sub pushes over UDP (pubsub_udp), sealed by the daemon, opened by
 * libperfd; sent only to the TCP peer once it echoes a probe's cookie (pubsub_udp_confirm) */
#ifndef PERFD_PUSH_H
#define PERFD_PUSH_H

#include <stdint.h>
#include <string.h>

/* header [magic 1][version 1][type 1][stream 8][seq 8], clear but authenticated;
 * integers big-endian; probes and messages share one sequence, never retransmitted */
#define PFP_MAGIC        0x70
#define PFP_VERSION      1
#define PFP_PROBE        1             /* [cookie 16] */
/* [kind 1][plen 2][pattern][clen 2][channel][dlen 4][payload], kind 1 = pmessage */
#define PFP_MESSAGE      2
#define PFP_HDR          19
#define PFP_TAG          16            /* after the sealed body */
#define PFP_MAX          1400          /* the whole datagram; larger: TCP */
#define PFP_KEY          32
#define PFP_COOKIE       16
#define PFP_ACK_EVERY    256           /* pubsub_udp_ack: the highest seq this */
#define PFP_ACK_MS       5000          /* often, or on this timer regardless */
#define PFP_PRUNE_MS     15000         /* unacked this long: subscriptions dropped (pubsub_udp_pruned) */

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

/* ChaCha20-Poly1305 (IETF) nonce [0 0 0 0][seq 8]: the key is the stream's alone
 * and seq never repeats, so neither does the nonce */
static inline void pfp_nonce(unsigned char n[12], uint64_t seq)
{
	memset(n, 0, 4);
	pfp_put64(n + 4, seq);
}

#endif /* PERFD_PUSH_H */
