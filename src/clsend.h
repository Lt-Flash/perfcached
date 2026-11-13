/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clsend.h - the cluster plane's send path: seal (clwire's) and address one peer, a node id or every
 * live peer.  Any thread, no lock; @xmit lets the unit test capture what would go out. */
#ifndef PC_CLSEND_H
#define PC_CLSEND_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include "pcaddr.h"

struct clpeers;

/* returns what sendto(2) would: bytes sent, or -1 */
typedef long (*clsend_xmit_fn)(int fd, const void *buf, size_t n,
		const pc_addr *to);

/* once, after the cluster socket is open; @xmit NULL = sendto(2) */
void clsend_init(int fd, const uint8_t *psk, clsend_xmit_fn xmit);
/* a second key clsend_open tries; nothing is sealed with it */
void clsend_set_accept(const uint8_t *psk);

/* one sealed datagram to @to: 0 sent, -1 not (too large, or the transmit failed); _udp sends by UDP
 * whatever the lane says - the probe of a forced peer's UDP, and its answer */
int clsend_seal_udp(const pc_addr *to, const unsigned char *pt,
		size_t n);
int clsend_seal(const pc_addr *to, const unsigned char *pt,
		size_t n);

/* to the LIVE peer holding node id @node: 0 sent, -1 no such live peer
 * or the send failed */
int clsend_to_node(struct clpeers *pt, int node, const unsigned char *pt_,
		size_t n, long long now);

/* one sealed datagram to every live peer, in peer-table order;
 * returns how many were sent */
int clsend_live(struct clpeers *pt, const unsigned char *msg, size_t n,
		long long now);

/* ---- the pieces, for a sender that holds the sealed bytes: the pub/sub relay seals once for many
 * peers and answers a probe from its own socket; the key and the cluster socket stay here ---- */

/* seal without sending: clwire_seal() with the cluster secret, the
 * sealed length or 0; @out holds HDR_LEN + n + CLWIRE_TAG */
size_t clsend_wrap(const unsigned char *pt, size_t n, unsigned char *out);

/* bytes already sealed, out of the cluster socket through the transmit:
 * what sendto(2) returned */
long clsend_raw(const pc_addr *to, const void *buf, size_t n);
/* a sealed frame over @min bytes goes to @lane instead of the
 * socket (unicast mode's TCP frame lane); NULL turns it off */
typedef int (*clsend_lane_fn)(const pc_addr *to,
		const unsigned char *sealed, size_t n);
/* 1 if every frame to @to must take the lane (UDP blocked) */
typedef int (*clsend_forced_fn)(const pc_addr *to);
void clsend_set_lane(clsend_lane_fn lane, size_t min, clsend_forced_fn forced);
/* 1 if a sealed frame of @n bytes would go by the lane */
int clsend_lanes(size_t n);

/* open a datagram sealed with the cluster secret: clwire_open()'s 0/-1 */
int clsend_open(const unsigned char *buf, size_t n, unsigned char *pt,
		unsigned long long *ptlen);

#endif /* PC_CLSEND_H */
