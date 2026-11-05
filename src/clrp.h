/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clrp.h - S461: eager replication pushes on the relay port (PS11), so the
 * relay's receive threads apply them and the cluster thread does not.
 */
#ifndef PC_CLRP_H
#define PC_CLRP_H

#include <stddef.h>
#include "pcaddr.h"

struct peer;

/* a sealed-free write-path group (req 0) to @to: the relay port while the
 * peer answers RPPROBE there, else the cluster path.  0 = sent */
int clrp_send(const pc_addr *to, const unsigned char *pt, size_t n);
/* a relay receive thread's REPL_MANY: gated by the sender's id and host */
void clrp_rx(const unsigned char *pt, size_t n, const pc_addr *from);
/* OM2-11: 1 when @from is a member's host - the relay's TCP accept gate */
int clrp_host_ok(const pc_addr *from);
/* 0: this node answers no RPPROBE (apply_threads >= 2 apply its pushes) */
void clrp_take(int on);
int clrp_taking(void);
/* the cluster thread's once-a-second probe of one peer: a CLPS_EV_* */
int clrp_probe_peer(struct peer *p, long long now);
void clrp_handle_probe_ack(const unsigned char *pt, size_t n,
		const pc_addr *from);

struct clrp_figures {
	int peers_direct;
	unsigned long long sent_direct, sent_cluster, rx_groups, rx_refused;
	unsigned long long sent_tcp;       /* S469: groups over the threshold, by TCP */
};
/* S469: groups longer than this go by TCP (max_datagram; 0 = never) */
void clrp_set_tcp_min(size_t n);
void clrp_figures(struct clrp_figures *f);

#endif
