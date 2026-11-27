/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clps.h - the pub/sub relay plane: a publish goes to each peer that can want it (its Bloom filter
 * and patterns), a sealed datagram per sending lane, onto the peer's relay port while it answers */
#ifndef PC_CLPS_H
#define PC_CLPS_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include "pcaddr.h"

struct peer;

/* a peer's relay mode (struct peer's pv_mode), which the publishing
 * workers read - see clps.c */
#define PV_ALL          0              /* it wants everything */
#define PV_FILTER       1              /* our copy of its interest is current */
#define PV_BROADCAST    2              /* it filters, our copy is not current */

/* once, at cluster init: the fleet pattern index */
void clps_init(void);
/* the receive threads call it before each wait; NULL none */
extern void (*clps_idle_fn)(void);
/* once per incarnation: the relay sequence's epoch */
void clps_new_epoch(void);
/* the bound relay port, 0 = none - advertised on the heartbeat */
int clps_rx_port(void);

/* bind the relay port, @threads sockets sharing it: the sockets bound, 0 for none, and in *why
 * what the caller should say: */
#define CLPS_RX_OFF     0              /* not asked for, or no cluster */
#define CLPS_RX_TAKEN   1              /* something already holds the port */
#define CLPS_RX_BIND    2              /* bind(2) failed; *err is its errno */
#define CLPS_RX_SOCKET  3              /* socket(2) failed (never said) */
#define CLPS_RX_OPEN    4              /* bound */
int clps_rx_open(int port, int threads, int *why, int *err);
/* give each sender lane (clwire.h) the group's socket (lane - 1) % n, attached on any member @fd;
 * laneless frames keep the kernel's hash.  1 when attached; clps_rx_lanes: whether the group has it */
int clps_lane_spread(int fd, int n);
int clps_rx_lanes(void);

/* one peer's step of the cluster thread's once-a-second probe; returns the change to say: */
#define CLPS_EV_NONE    0
#define CLPS_EV_DIRECT  1              /* relays now use its relay port */
#define CLPS_EV_STOPPED 2              /* it stopped answering there */
#define CLPS_EV_SILENT  3              /* it advertises a port and has not
                                        * answered there in 10 s */
int clps_probe_peer(struct peer *p, long long now);

/* a heartbeat's relay port (0 = none) and interest version (0 = relay it everything): 1 when it
 * withdrew a port it was being sent on.  Peer thread. */
int clps_heard(struct peer *p, uint16_t port, uint64_t interest,
		long long now);

/* the peer wants everything again, or its slot is being freed */
void clps_forget(struct peer *p);

/* the datagrams, from the cluster receive loop (and M_PUBLISH also from
 * the relay port's threads) */
void clps_handle_publish(const unsigned char *pt, size_t n);
/* the same from the relay port, whose senders no cluster-port gate has seen: only from the
 * member holding the id the frame names, at the address it came from; 0 refused */
int clps_handle_relay_publish(const unsigned char *pt, size_t n, const pc_addr *from);
void clps_handle_probe_ack(const unsigned char *pt, size_t n,
		const pc_addr *from);
void clps_handle_int_add(const unsigned char *pt, size_t n,
		const pc_addr *from);
void clps_handle_int_req(const unsigned char *pt, size_t n,
		const pc_addr *from);
void clps_handle_int_full(const unsigned char *pt, size_t n,
		const pc_addr *from);

/* once a second beside the probes: re-evaluate each live peer's
 * mode, ask for missing state, retire dropped patterns */
void clps_interest_tick(long long now);

#endif /* PC_CLPS_H */
