/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clane.h - the frame lane: in unicast mode a sealed frame over max_datagram goes to its peer over
 * TCP, framed not re-sealed; beats stay datagrams so failure detection is still UDP silence. */
#ifndef PC_CLANE_H
#define PC_CLANE_H

#include <stddef.h>
#include <netinet/in.h>
#include "pcaddr.h"

/* wire: hello ["PCL1"][ip4][cluster port 2], refused unless ip4 is the connection's source IP;
 * frame [len 4, big-endian][sealed frame] */
#define CLANE_MAX_QUEUE     (4u << 20)   /* bytes queued per peer */
#define CLANE_MAX_RXQ       (16u << 20)  /* bytes waiting for the cluster thread */
#define CLANE_MAX_AGE_MS    5000
#define CLANE_MAX_INBOUND   512
#define CLANE_DEFAULT_MTU   1232

struct clane_figs {
	unsigned long long sent_frames, sent_bytes;     /* written to a lane */
	unsigned long long recv_frames, recv_bytes;     /* handed to dispatch */
	unsigned long long dropped_full, dropped_stale, dropped_rx;
	unsigned long long connects, connect_fail, refused;
	unsigned long long inbound_timeouts;            /* hello late, or idle */
	int forced;                                     /* peers on TCP only */
	int peers_up, inbound;
};

/* bind the lane listener on @self (advertise ip, lane port) and start the lane thread;
 * @lane_off = lane port - cluster port, fleet-wide.  -1 on failure (logged). */
int clane_start(const pc_addr *self_cluster, int lane_off);
/* that @lane_off while the lane runs, 0 when it is off */
int clane_off(void);
/* queue a sealed frame for @to (a peer's CLUSTER address); thread-safe,
 * never blocks.  0 queued, -1 dropped (counted). */
int clane_send(const pc_addr *to, const unsigned char *sealed,
		size_t n);
/* the eventfd the cluster thread polls; -1 if the lane is off */
int clane_rx_fd(void);
/* take one received frame: its length (0 = none left), the sender's
 * cluster address into @from.  Clears the eventfd when it returns 0. */
size_t clane_rx_take(unsigned char *buf, size_t cap,
		pc_addr *from);
void clane_figures(struct clane_figs *f);

/* is TCP to @to working: UP (connected), PENDING (a connect in flight for @age_ms), DOWN.
 * clane_probe() connects with nothing to send - its success alone is the answer. */
#define CLANE_TCP_DOWN    0
#define CLANE_TCP_PENDING 1
#define CLANE_TCP_UP      2
int clane_tcp_state(const pc_addr *to, long long *age_ms);
void clane_probe(const pc_addr *to);
/* every frame to @to by the lane, whatever its size (UDP to it is
 * blocked); 0 turns it off.  clane_forced() is cheap when none is. */
void clane_force(const pc_addr *to, int on);
int clane_forced(const pc_addr *to);

#endif
