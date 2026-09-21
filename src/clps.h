/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clps.h - the pub/sub relay plane (M16, wave 4).
 *
 * PS3 relays a publish to the fleet: one sealed datagram per live peer,
 * from the publishing worker, fire-and-forget.  PS7 gives each sending
 * thread a lane with its own counter, so a receiver counts what it missed
 * rather than mistaking reordering for loss.  PS11 moves relays onto a
 * port of their own, read by several threads, and uses a peer's port only
 * while that peer answers probes there.  PS12 relays only what a peer can
 * want: it tells us its subscriptions (a Bloom filter for names, a list
 * for patterns), and a publish that cannot match is not sent to it.
 *
 * Those four were 753 lines of cluster.c, added in three days because
 * there was nowhere else to put them; this is the somewhere else (rule
 * 11).  cluster.c keeps the dispatch cases, the tick calls and the words:
 * the relay port's open and each peer's probe step DECIDE here and return
 * what happened, and cluster.c logs it (rule 4).
 *
 * Threads: the publishing workers (pc_cluster_pubsub_relay), the relay
 * port's receive threads (pc_cluster_pubsub_rx_thread), and the cluster
 * thread (everything else).  The per-peer state is struct peer's (the
 * pv_* and ps_* fields, clpeers.h); how the workers read what the cluster
 * thread writes is described at the top of clps.c's PS12 section.
 *
 * The exported pc_cluster_pubsub_* entry points keep their declarations
 * where they were (cluster.h, and pubsub.h for the relay) - pubsub.c and
 * daemon.c call them by those names.
 */
#ifndef PC_CLPS_H
#define PC_CLPS_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

struct peer;

/* a peer's relay mode (struct peer's pv_mode), which the publishing
 * workers read - see clps.c */
#define PV_ALL          0              /* it wants everything */
#define PV_FILTER       1              /* our copy of its interest is current */
#define PV_BROADCAST    2              /* it filters, our copy is not current */

/* once, at cluster init: the fleet pattern index */
void clps_init(void);
/* once per incarnation: the relay sequence's epoch (PS7) */
void clps_new_epoch(void);
/* the bound relay port, 0 = none - advertised on the heartbeat */
int clps_rx_port(void);

/* PS11: bind the relay port, @threads sockets sharing it.  Returns the
 * sockets bound, 0 for none, and in *why what the caller should say: */
#define CLPS_RX_OFF     0              /* not asked for, or no cluster */
#define CLPS_RX_TAKEN   1              /* something already holds the port */
#define CLPS_RX_BIND    2              /* bind(2) failed; *err is its errno */
#define CLPS_RX_SOCKET  3              /* socket(2) failed (never said) */
#define CLPS_RX_OPEN    4              /* bound */
int clps_rx_open(int port, int threads, int *why, int *err);

/* PS11: one peer's step of the cluster thread's once-a-second probe.
 * Returns the change to say, if any: */
#define CLPS_EV_NONE    0
#define CLPS_EV_DIRECT  1              /* relays now use its relay port */
#define CLPS_EV_STOPPED 2              /* it stopped answering there */
#define CLPS_EV_SILENT  3              /* it advertises a port and has not
                                        * answered there in 10 s */
int clps_probe_peer(struct peer *p, long long now);

/* a heartbeat's relay fields: its relay port (0 = none) and interest
 * version (0 = relay it everything).  Returns 1 when it withdrew a port
 * it was being sent on.  Peer thread. */
int clps_heard(struct peer *p, uint16_t port, uint64_t interest,
		long long now);

/* the peer wants everything again, or its slot is being freed */
void clps_forget(struct peer *p);

/* the datagrams, from the cluster receive loop (and M_PUBLISH also from
 * the relay port's threads) */
void clps_handle_publish(const unsigned char *pt, size_t n);
void clps_handle_probe_ack(const unsigned char *pt, size_t n,
		const struct sockaddr_in *from);
void clps_handle_int_add(const unsigned char *pt, size_t n,
		const struct sockaddr_in *from);
void clps_handle_int_req(const unsigned char *pt, size_t n,
		const struct sockaddr_in *from);
void clps_handle_int_full(const unsigned char *pt, size_t n,
		const struct sockaddr_in *from);

/* PS12, once a second beside the probes: re-evaluate each live peer's
 * mode, ask for missing state, retire dropped patterns */
void clps_interest_tick(long long now);

#endif /* PC_CLPS_H */
