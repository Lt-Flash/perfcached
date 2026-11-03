/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clrp.c - S461: eager replication pushes on the relay port.  See clrp.h.
 */
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sodium.h>

#include "clrp.h"
#include "clrpt.h"                     /* S469 */
#include "clps.h"
#include "clstate.h"
#include "clpeers.h"
#include "clsend.h"
#include "clwire.h"
#include "clmsg.h"
#include "clmig.h"
#include "iostat.h"
#include "psrelay.h"

extern void cl_repl_apply_group(const unsigned char *pt, size_t n,
		const pc_addr *from);

/* each sending worker its own socket and lane: one receive thread, in order */
static __thread int rp_send_fd = -1;
static unsigned long long rp_sent_direct, rp_sent_cluster, rp_rx_groups,
	rp_rx_refused, rp_sent_tcp;
static size_t rp_tcp_min = 1232;       /* S469: max_datagram - above it, TCP */

void clrp_set_tcp_min(size_t n)
{
	rp_tcp_min = n ? n : (size_t)-1;   /* max_datagram = 0: every group a datagram */
}
static int rp_take = 1;

void clrp_take(int on)
{
	rp_take = on;
}

int clrp_taking(void)
{
	return rp_take;
}

/* bound to this node's own host: the receiver gates on the sender's id AND
 * host, and an unbound socket leaves a multi-homed host's source to the route */
static int rp_socket(int family)
{
	pc_addr me = cl_self_addr();
	int fd = socket(family, SOCK_DGRAM, 0);

	if (fd < 0)
		return -2;
	pc_addr_set_port(&me, 0);
	if (pc_addr_family(&me) != family ||
	    bind(fd, &me.sa, pc_addr_len(&me)) != 0) {
		close(fd);
		return -2;                     /* this thread keeps the cluster path */
	}
	fcntl(fd, F_SETFL, O_NONBLOCK);    /* OM2-13: a full buffer falls back, never blocks */
	return fd;
}

int clrp_send(const pc_addr *to, const unsigned char *pt, size_t n)
{
	unsigned char buf[HDR_LEN + MAX_DGRAM + 16];
	struct peer *p = clpeers_by_addr(cl_peers(), to);
	size_t len;

	clwire_take_lane();                   /* the receiver keeps it on one thread */
	len = clsend_wrap(pt, n, buf);

	if (!len)
		return -1;
	if (p && psr_path_direct(&p->rp, cl_now_ms()) && len > rp_tcp_min) {
		pc_addr d = *to;

		pc_addr_set_port(&d, (uint16_t)__atomic_load_n(&p->rp.port,
			__ATOMIC_ACQUIRE));
		if (clrpt_send(&d, buf, len) == 0) {
			__atomic_add_fetch(&rp_sent_tcp, 1, __ATOMIC_RELAXED);
			return 0;
		}
	}
	if (p && psr_path_direct(&p->rp, cl_now_ms()) && !clsend_lanes(len)) {
		pc_addr d = *to;

		if (rp_send_fd == -1)
			rp_send_fd = rp_socket(pc_addr_family(&d));
		pc_addr_set_port(&d, (uint16_t)__atomic_load_n(&p->rp.port,
			__ATOMIC_ACQUIRE));
		if (rp_send_fd >= 0) {
			ssize_t r = sendto(rp_send_fd, buf, len, MSG_DONTWAIT, &d.sa,
				pc_addr_len(&d));

			pc_iostat_udp_tx(PC_UDP_RELAY, 1, r >= 0);
			if (r >= 0) {
				__atomic_add_fetch(&rp_sent_direct, 1, __ATOMIC_RELAXED);
				return 0;
			}
		}
	}
	__atomic_add_fetch(&rp_sent_cluster, 1, __ATOMIC_RELAXED);
	return clsend_raw(to, buf, len) >= 0 ? 0 : -1;
}

int clrp_host_ok(const pc_addr *from)
{
	struct clpeers *pt = cl_peers();
	int i;

	for (i = 0; i < pt->n_peers; i++)
		if (__atomic_load_n(&pt->peers[i].node, __ATOMIC_ACQUIRE) &&
		    pc_addr_eq_host(&pt->peers[i].addr, from))
			return 1;
	return 0;
}

void clrp_rx(const unsigned char *pt, size_t n, const pc_addr *from)
{
	/* only fire-and-forget groups ride here: an acked one's ack and window
	 * are the cluster thread's (S253, clpush) */
	if (n < CLMIG_GHDR || clmig_group_req(pt) ||
	    !clpeers_by_id_from(cl_peers(), clmig_group_node(pt), from)) {
		__atomic_add_fetch(&rp_rx_refused, 1, __ATOMIC_RELAXED);
		return;
	}
	__atomic_add_fetch(&rp_rx_groups, 1, __ATOMIC_RELAXED);
	cl_repl_apply_group(pt, n, from);
}

int clrp_probe_peer(struct peer *p, long long now)
{
	uint64_t nonce;
	int ev;

	if (!__atomic_load_n(&p->node, __ATOMIC_ACQUIRE) ||
	    !clpeers_live(p, now) || !cl_node_id())
		return CLPS_EV_NONE;
	randombytes_buf(&nonce, sizeof nonce);
	if (psr_path_probe_due(&p->rp, now, nonce | 1)) {
		unsigned char msg[11];
		pc_addr to = p->addr;

		if (!p->rp_since_ms)
			p->rp_since_ms = now;
		msg[0] = M_RPPROBE;
		msg[1] = (unsigned char)(cl_node_id() >> 8);
		msg[2] = (unsigned char)cl_node_id();
		memcpy(msg + 3, &p->rp.nonce, 8);
		pc_addr_set_port(&to, (uint16_t)p->rp.port);
		clsend_seal(&to, msg, sizeof msg);
	}
	ev = psr_path_transition(&p->rp, now);
	if (ev > 0) {
		p->rp_since_ms = 0;
		p->rp_warned = 0;
		return CLPS_EV_DIRECT;
	} else if (ev < 0) {
		p->rp_warned = 1;
		return CLPS_EV_STOPPED;
	} else if (!p->rp.direct && p->rp.port && p->rp_since_ms &&
	        now - p->rp_since_ms >= 10000 && !p->rp_warned) {
		p->rp_warned = 1;
		return CLPS_EV_SILENT;
	}
	return CLPS_EV_NONE;
}

void clrp_handle_probe_ack(const unsigned char *pt, size_t n,
		const pc_addr *from)
{
	struct peer *p;
	uint64_t nonce;

	if (n < 11)
		return;
	p = clpeers_by_id_from(cl_peers(), ((unsigned)pt[1] << 8) | pt[2], from);
	memcpy(&nonce, pt + 3, 8);
	if (p)
		psr_path_answer(&p->rp, nonce, cl_now_ms());
}

void clrp_figures(struct clrp_figures *f)
{
	long long now = cl_now_ms();
	int i;

	memset(f, 0, sizeof *f);
	for (i = 0; cl_enabled() && i < cl_peers()->n_peers; i++)
		if (__atomic_load_n(&cl_peers()->peers[i].node, __ATOMIC_ACQUIRE) &&
		    psr_path_direct(&cl_peers()->peers[i].rp, now))
			f->peers_direct++;
	f->sent_direct = __atomic_load_n(&rp_sent_direct, __ATOMIC_RELAXED);
	f->sent_cluster = __atomic_load_n(&rp_sent_cluster, __ATOMIC_RELAXED);
	f->rx_groups = __atomic_load_n(&rp_rx_groups, __ATOMIC_RELAXED);
	f->rx_refused = __atomic_load_n(&rp_rx_refused, __ATOMIC_RELAXED);
	f->sent_tcp = __atomic_load_n(&rp_sent_tcp, __ATOMIC_RELAXED);
}
