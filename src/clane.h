/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clane.h - S36 step b: the FRAME LANE - sealed cluster frames too big
 * for one datagram, carried over TCP in unicast mode.
 *
 * A cloud path MTU is ~1,460 bytes (1,450 on a VXLAN pod network); a
 * 58 KB datagram becomes 40-odd IP fragments, one lost fragment loses
 * all of it, and some environments drop fragments outright.  So in
 * `discovery = unicast` every sealed frame over `[cluster]
 * max_datagram` (default 1,400) goes to the peer over a TCP connection
 * of its own instead: one connection per peer, made on demand to the
 * peer's lane port (`[cluster] lane_port`, default the cluster port + 2,
 * the same on every node), a bounded queue per peer, frames older than
 * CLANE_MAX_AGE_MS dropped rather than delivered stale.  Small frames -
 * the beats - stay datagrams, so failure detection is still silence on
 * UDP and is not hidden behind TCP's retransmissions.
 *
 * The lane adds FRAMING, not crypto: every frame is already sealed (the
 * cluster key, S302's id in the authenticated header) and is opened by
 * the same code as a datagram.  A connection opens with a hello naming
 * the sender's cluster address; it is refused unless the address is the
 * connection's own source IP.  Received frames are queued for the
 * cluster thread (an eventfd it polls), so every handler keeps its
 * thread.
 *
 *   wire:  hello  [ "PCL1" ][ip4][cluster port 2]   (network order)
 *          frame  [len 4, big-endian][sealed frame of len bytes]
 */
#ifndef PC_CLANE_H
#define PC_CLANE_H

#include <stddef.h>
#include <netinet/in.h>

#define CLANE_MAX_QUEUE     (4u << 20)   /* bytes queued per peer */
#define CLANE_MAX_RXQ       (16u << 20)  /* bytes waiting for the cluster thread */
#define CLANE_MAX_AGE_MS    5000
#define CLANE_MAX_INBOUND   512
#define CLANE_DEFAULT_MTU   1400

struct clane_figs {
	unsigned long long sent_frames, sent_bytes;     /* written to a lane */
	unsigned long long recv_frames, recv_bytes;     /* handed to dispatch */
	unsigned long long dropped_full, dropped_stale, dropped_rx;
	unsigned long long connects, connect_fail, refused;
	int forced;                                     /* S36 e2: peers on TCP only */
	int peers_up, inbound;
};

/* bind the lane listener on @self (advertise ip, lane port) and start
 * the lane thread; @lane_off = lane port - cluster port, fleet-wide.
 * -1 on failure (logged). */
int clane_start(const struct sockaddr_in *self_cluster, int lane_off);
/* queue a sealed frame for @to (a peer's CLUSTER address); thread-safe,
 * never blocks.  0 queued, -1 dropped (counted). */
int clane_send(const struct sockaddr_in *to, const unsigned char *sealed,
		size_t n);
/* the eventfd the cluster thread polls; -1 if the lane is off */
int clane_rx_fd(void);
/* take one received frame: its length (0 = none left), the sender's
 * cluster address into @from.  Clears the eventfd when it returns 0. */
size_t clane_rx_take(unsigned char *buf, size_t cap,
		struct sockaddr_in *from);
void clane_figures(struct clane_figs *f);

/* S36 step e2: is TCP to @to working?  CLANE_TCP_UP (a lane connection
 * is established), CLANE_TCP_PENDING (a connect is in flight, started
 * @age_ms ago), CLANE_TCP_DOWN.  clane_probe() starts a connection with
 * nothing to send - its success alone is the answer. */
#define CLANE_TCP_DOWN    0
#define CLANE_TCP_PENDING 1
#define CLANE_TCP_UP      2
int clane_tcp_state(const struct sockaddr_in *to, long long *age_ms);
void clane_probe(const struct sockaddr_in *to);
/* every frame to @to by the lane, whatever its size (UDP to it is
 * blocked); 0 turns it off.  clane_forced() is cheap when none is. */
void clane_force(const struct sockaddr_in *to, int on);
int clane_forced(const struct sockaddr_in *to);

#endif
