/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clsend.h - the cluster plane's send path (M14, wave 4).
 *
 * doc/MODULARITY.md §182 names two primitives the extracted modules
 * should consume instead of re-declaring: "send to node id" and "complete
 * request".  This is the first.  Until now it was cluster.c's static
 * seal_send() - 31 callers, every one of them in cluster.c, because the
 * socket and the key were fields of `C` - and that single static is what
 * kept M9's six worker entry points from moving.
 *
 * The socket and the key are handed over ONCE, at init, and never change
 * after (the socket is opened once, the key is derived once).  Sealing is
 * clwire's; this adds the address - one peer, a node id, or every live
 * peer - and the transmit.
 *
 * The transmit is a function pointer that defaults to sendto(2), so the
 * unit test captures what would have gone out and opens it with
 * clwire_open: rule 7 without a socket.  Any thread: nothing here takes a
 * lock, and the peer table is read the way clpeers.h says any thread may.
 */
#ifndef PC_CLSEND_H
#define PC_CLSEND_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

struct clpeers;

/* returns what sendto(2) would: bytes sent, or -1 */
typedef long (*clsend_xmit_fn)(int fd, const void *buf, size_t n,
		const struct sockaddr_in *to);

/* once, after the cluster socket is open; @xmit NULL = sendto(2) */
void clsend_init(int fd, const uint8_t *psk, clsend_xmit_fn xmit);

/* one sealed datagram to @to: 0 sent, -1 not (too large, or the transmit
 * failed) - seal_send()'s contract, unchanged */
int clsend_seal(const struct sockaddr_in *to, const unsigned char *pt,
		size_t n);

/* to the LIVE peer holding node id @node: 0 sent, -1 no such live peer
 * or the send failed */
int clsend_to_node(struct clpeers *pt, int node, const unsigned char *pt_,
		size_t n, long long now);

/* one sealed datagram to every live peer, in peer-table order (what
 * broadcast_live() did); returns how many were sent */
int clsend_live(struct clpeers *pt, const unsigned char *msg, size_t n,
		long long now);

/* ---- the pieces, for a sender that must hold the sealed bytes (M16) ----
 * The pub/sub relay seals one frame once and sends the same bytes to
 * every peer that wants everything, and answers a probe out of its own
 * receive socket - neither is one seal per send.  The key and the
 * cluster socket stay here all the same. */

/* seal without sending: clwire_seal() with the cluster secret, the
 * sealed length or 0; @out holds HDR_LEN + n + CLWIRE_TAG */
size_t clsend_wrap(const unsigned char *pt, size_t n, unsigned char *out);

/* bytes already sealed, out of the cluster socket through the transmit:
 * what sendto(2) returned */
long clsend_raw(const struct sockaddr_in *to, const void *buf, size_t n);

/* open a datagram sealed with the cluster secret: clwire_open()'s 0/-1 */
int clsend_open(const unsigned char *buf, size_t n, unsigned char *pt,
		unsigned long long *ptlen);

#endif /* PC_CLSEND_H */
