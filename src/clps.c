/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clps.c - the pub/sub relay plane: fan-out, receive windows, the relay port and
 * the interest exchange; log wording is the caller's.  See clps.h. */
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#ifdef __linux__
#include <linux/filter.h>
#endif
#include <sodium.h>

#include "iostat.h"
#include "clps.h"
#include "clrpt.h"
#include "clrp.h"
#include "cluster.h"
#include "clreplay.h"
#include "clstate.h"
#include "clpeers.h"

void (*clps_idle_fn)(void);
#include "clsend.h"
#include "clwire.h"
#include "clmsg.h"
#include "psindex.h"
#include "psinterest.h"
#include "psrelay.h"
#include "pubsub.h"
#include "quiesce.h"

static ssize_t relay_sendto(int fd, const void *buf, size_t n,
		const pc_addr *to)
{
	/* never blocks the publishing worker: a full socket falls back to the cluster path */
	ssize_t r = sendto(fd, buf, n, MSG_DONTWAIT, &to->sa, pc_addr_len(to));

	pc_iostat_udp_tx(PC_UDP_RELAY, 1, r >= 0);
	return r;
}

/* ---- the relay ---- one sealed datagram per live peer, fire-and-forget from the
 * publishing worker; no repair: a gap in a sender's sequence counts as relay_lost */
/* the sequence is [epoch 16][lane 8][counter 40] (psrelay.h): a counter per worker
 * lane, as a shared one let N+1 reach the wire before N and read as a loss */
static uint16_t pubsub_epoch;          /* drawn with the incarnation */
static unsigned int pubsub_lanes;      /* lanes handed out; wraps at 256 */
static __thread int pubsub_lane = -1;
/* the counters are the lane's, not the thread's: past 256 publishing threads two share one */
static struct { uint64_t c; char pad[56]; } pubsub_lane_ctr[256];
/* the dedicated relay plane: see clps_rx_open */
#define PS_RX_MAX 16
static int ps_rx_port;                 /* bound and advertised; 0 = off */
static int ps_rx_n;
static int ps_rx_bylane;               /* the lane spread is attached */
static int ps_rx_fd[PS_RX_MAX];
static int ps_rx_lfd[PS_RX_MAX];       /* the TCP listener beside each */
static unsigned long long ps_sent_direct, ps_sent_cluster, ps_rx_dgrams,
	ps_rx_other;
static __thread int ps_send_fd = -1;  /* -2: no socket here, the cluster path */

/* bound to this node's own host, as clrp's: a receiver takes a frame only from
 * the host its header names, and unbound the route picks it */
static int ps_socket(int family)
{
	pc_addr me = cl_self_addr();
	int fd = socket(family, SOCK_DGRAM, 0);

	if (fd < 0)
		return -2;
	pc_addr_set_port(&me, 0);
	if (pc_addr_family(&me) != family || bind(fd, &me.sa, pc_addr_len(&me)) != 0) {
		close(fd);
		return -2;
	}
	return fd;
}
static pthread_mutex_t ps_rx_lk = PTHREAD_MUTEX_INITIALIZER;
static void ps_put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }
static void ps_put32(unsigned char *p, unsigned long v) { ps_put16(p, (unsigned)(v >> 16)); ps_put16(p + 2, (unsigned)v); }
static void ps_put64(unsigned char *p, unsigned long long v) { ps_put32(p, (unsigned long)(v >> 32)); ps_put32(p + 4, (unsigned long)v); }
static unsigned ps_get16(const unsigned char *p) { return ((unsigned)p[0] << 8) | p[1]; }
static unsigned long ps_get32(const unsigned char *p) { return ((unsigned long)ps_get16(p) << 16) | ps_get16(p + 2); }
static unsigned long long ps_get64(const unsigned char *p) { return ((unsigned long long)ps_get32(p) << 32) | ps_get32(p + 4); }

/* ---- relaying only what a peer wants (psinterest.h) ---- the window: a publish
 * just after a peer's first subscription, before its update lands here, misses it */
#define PS_PEER_EPOCH   0x8000
#define PS_PEER_WORDS   ((PC_CL_MAXPEER + 63) / 64)

struct ps_fpat {                       /* a pattern some peers hold */
	struct psi_node node;              /* first */
	uint64_t peers[PS_PEER_WORDS];     /* atomic: the peer slots holding it */
	struct ps_fpat *retired_next;
	unsigned long long stamp;
	char name[];
};
static struct psi_table ps_fpat;       /* written by the cluster thread */
static int ps_fpat_ready;
static struct ps_fpat *ps_fpat_retired;
static unsigned long long ps_int_seq;  /* atomic seqlock: odd while it writes */
static unsigned ps_nfilter;            /* atomic: peers in PV_FILTER */
static unsigned long long ps_skipped, ps_int_upd_sent, ps_int_upd_recv,
	ps_int_full_sent, ps_int_full_recv, ps_int_req_sent;
static uint64_t ps_peer_ctr[256][PC_CL_MAXPEER];   /* per lane, as pubsub_lane_ctr */
static __thread unsigned char *ps_peer_seal;

static void fpat_or(struct psi_node *x, void *arg)
{
	struct ps_fpat *f = (struct ps_fpat *)x;
	uint64_t *want = arg;
	int w;

	for (w = 0; w < PS_PEER_WORDS; w++)
		want[w] |= __atomic_load_n(&f->peers[w], __ATOMIC_ACQUIRE);
}

/* the live peers a publish of @chan may skip; 0 = none */
static int ps_skip_mask(const char *chan, size_t clen, uint64_t *skip)
{
	uint32_t h1, h2;
	int tries;

	if (!__atomic_load_n(&ps_nfilter, __ATOMIC_ACQUIRE) || !pc_qs_inside())
		return 0;                      /* nobody filters, or we may not read */
	psb_hash(chan, clen, &h1, &h2);
	for (tries = 0; tries < 16; tries++) {
		unsigned long long s = __atomic_load_n(&ps_int_seq, __ATOMIC_ACQUIRE);
		uint64_t want[PS_PEER_WORDS];
		int i, n, any = 0;

		if (s & 1) {
			sched_yield();
			continue;
		}
		memset(want, 0, sizeof want);
		if (ps_fpat_ready && __atomic_load_n(&ps_fpat.n, __ATOMIC_ACQUIRE))
			psi_match(&ps_fpat, chan, clen, fpat_or, want);
		memset(skip, 0, PS_PEER_WORDS * sizeof *skip);
		n = __atomic_load_n(&cl_peers()->n_peers, __ATOMIC_ACQUIRE);
		for (i = 0; i < n; i++) {
			struct peer *p = &cl_peers()->peers[i];
			const uint8_t *b;

			if (__atomic_load_n(&p->pv_mode, __ATOMIC_ACQUIRE) != PV_FILTER ||
			    (want[i / 64] >> (i % 64) & 1))
				continue;
			b = __atomic_load_n(&p->pv_bloom, __ATOMIC_ACQUIRE);
			if (!b || psb_test(b, h1, h2))
				continue;
			skip[i / 64] |= 1ULL << (i % 64);
			any = 1;
		}
		if (__atomic_load_n(&ps_int_seq, __ATOMIC_ACQUIRE) == s)
			return any;
	}
	return 0;                              /* no stable read: send to all */
}

int pc_cluster_pubsub_relay(const char *chan, size_t clen, const char *data,
		size_t dlen)
{
	unsigned char frame[MAX_DGRAM];
	unsigned char sealed[HDR_LEN + MAX_DGRAM + 16];
	size_t slen = 0;
	size_t n = 1 + 2 + 8 + 2 + clen + 4 + dlen;
	long long now;
	unsigned long long seq;
	uint64_t skip[PS_PEER_WORDS];
	int i, sent = 0, filtering;

	if (!cl_enabled())
		return 0;
	if (clen > 0xffff || n > sizeof frame)
		return -1;                     /* over the datagram: local only */
	if (pubsub_lane < 0) {                 /* this thread's lane, once */
		pubsub_lane = (int)(__atomic_fetch_add(&pubsub_lanes, 1,
			__ATOMIC_RELAXED) & 0xff);
		clwire_take_lane();
	}
	seq = psr_seq(pubsub_epoch, (uint8_t)pubsub_lane,
		__atomic_add_fetch(&pubsub_lane_ctr[pubsub_lane].c, 1, __ATOMIC_RELAXED));
	frame[0] = M_PUBLISH;
	ps_put16(frame + 1, (unsigned)cl_node_id());
	ps_put64(frame + 3, seq);
	ps_put16(frame + 11, (unsigned)clen);
	memcpy(frame + 13, chan, clen);
	ps_put32(frame + 13 + clen, (unsigned long)dlen);
	memcpy(frame + 17 + clen, data, dlen);
	now = cl_now_ms();
	filtering = ps_skip_mask(chan, clen, skip);
	for (i = 0; i < cl_peers()->n_peers; i++) {
		struct peer *p = &cl_peers()->peers[i];
		const unsigned char *out;
		size_t olen;

		if (!__atomic_load_n(&p->node, __ATOMIC_ACQUIRE) ||
		    !clpeers_live(p, now))
			continue;
		if (filtering && (skip[i / 64] >> (i % 64) & 1)) {
			__atomic_add_fetch(&ps_skipped, 1, __ATOMIC_RELAXED);
			continue;                  /* it cannot want this */
		}
		if (__atomic_load_n(&p->pv_mode, __ATOMIC_ACQUIRE) == PV_ALL) {
			/* the same bytes go to every such peer: sealed once */
			if (!slen) {
				ps_put64(frame + 3, seq);
				if (!(slen = clsend_wrap(frame, n, sealed)))
					return sent;
			}
			out = sealed;
			olen = slen;
		} else {
			/* its own counter, so what it is not sent is
			 * not a loss to it */
			if (!ps_peer_seal &&
			    !(ps_peer_seal = malloc(HDR_LEN + MAX_DGRAM + 16)))
				continue;
			ps_put64(frame + 3, psr_seq(pubsub_epoch ^ PS_PEER_EPOCH,
				(uint8_t)pubsub_lane, __atomic_add_fetch(
				&ps_peer_ctr[pubsub_lane][i], 1, __ATOMIC_RELAXED)));
			if (!(olen = clsend_wrap(frame, n, ps_peer_seal)))
				continue;
			out = ps_peer_seal;
		}
		/* a frame over the datagram limit goes by the cluster
		 * socket's path, which hands it to the frame lane */
		if (psr_path_direct(&p->ps, now) && !clsend_lanes(olen)) {
			/* this worker's own socket: each sending thread is its own flow,
			 * so the peer's receive threads keep each lane in order */
			pc_addr to = p->addr;

			if (ps_send_fd == -1)
				ps_send_fd = ps_socket(pc_addr_family(&to));
			pc_addr_set_port(&to, (uint16_t)__atomic_load_n(&p->ps.port,
				__ATOMIC_ACQUIRE));
			if (ps_send_fd >= 0 && relay_sendto(ps_send_fd, out, olen,
			        &to) >= 0) {
				sent++;
				__atomic_add_fetch(&ps_sent_direct, 1, __ATOMIC_RELAXED);
				continue;
			}
		}
		if (clsend_raw(&p->addr, out, olen) >= 0) {
			sent++;
			__atomic_add_fetch(&ps_sent_cluster, 1, __ATOMIC_RELAXED);
		}
	}
	return sent;
}

/* per sender id held by a live member, 256 lane windows (10 KB); never freed, as
 * the receive threads reach them unlocked, so the id space bounds it at 10 MB */
struct ps_rxl {
	struct psr_lane w;
	int lk;                            /* a lane comes by the relay port and the cluster path */
};
static struct ps_rxl *ps_rx[PC_CL_NODE_MAX + 1];

/* a node's lanes are made under ps_rx_lk and published once */
static struct ps_rxl *ps_rx_lanes(unsigned node)
{
	struct ps_rxl *l;

	if (!node || node > PC_CL_NODE_MAX)
		return NULL;                   /* delivered, unaccounted */
	l = __atomic_load_n(&ps_rx[node], __ATOMIC_ACQUIRE);
	if (l)
		return l;
	if (!clpeers_by_id_live(cl_peers(), (int)node, cl_now_ms()))
		return NULL;                   /* no live member holds it */
	pthread_mutex_lock(&ps_rx_lk);
	l = ps_rx[node];
	if (!l) {
		l = calloc(256, sizeof *l);
		if (l)
			__atomic_store_n(&ps_rx[node], l, __ATOMIC_RELEASE);
	}
	pthread_mutex_unlock(&ps_rx_lk);
	return l;                              /* NULL: delivered, unaccounted */
}

void clps_handle_publish(const unsigned char *pt, size_t n)
{
	unsigned node, clen;
	unsigned long dlen;
	unsigned long long seq;
	uint64_t lost = 0;
	struct ps_rxl *lanes;
	int r = PSR_DELIVER;

	if (n < 17)
		return;
	node = ps_get16(pt + 1);
	seq = ps_get64(pt + 3);
	clen = ps_get16(pt + 11);
	if (n < 17 + clen)
		return;
	dlen = ps_get32(pt + 13 + clen);
	if (n != 17 + clen + dlen)
		return;
	lanes = ps_rx_lanes(node);
	if (lanes) {
		struct ps_rxl *x = &lanes[psr_lane(seq)];

		while (__atomic_exchange_n(&x->lk, 1, __ATOMIC_ACQUIRE))
			while (__atomic_load_n(&x->lk, __ATOMIC_RELAXED))
				;
		r = psr_accept(&x->w, seq, &lost);
		__atomic_store_n(&x->lk, 0, __ATOMIC_RELEASE);
	}
	if (r == PSR_DUPLICATE) {
		pc_pubsub_relay_account(0, 0, 0, 1);
		return;
	}
	pc_pubsub_relay_account(0, lost, 0, 0);
	pc_pubsub_publish((const char *)pt + 13, clen,
		(const char *)pt + 17 + clen, dlen, 1);
}

int clps_handle_relay_publish(const unsigned char *pt, size_t n, const pc_addr *from)
{
	if (n < 3 || !clpeers_by_id_from(cl_peers(), ps_get16(pt + 1), from))
		return 0;
	clps_handle_publish(pt, n);
	return 1;
}

/* ---- the dedicated relay plane ---- a port of its own, so a publish flood cannot
 * delay heartbeats: SO_REUSEPORT threads, used once a probe there is answered */

/* nothing else may hold the port: a stray daemon of the same user would
 * silently join a SO_REUSEPORT group and take a share of the flows */
static int ps_port_taken(const pc_addr *ip, int port)
{
	pc_addr sa = *ip;
	int fd = socket(pc_addr_family(ip), SOCK_DGRAM, 0), taken;

	if (fd < 0)
		return 1;
	pc_addr_set_port(&sa, (uint16_t)port);
	taken = bind(fd, &sa.sa, pc_addr_len(&sa)) != 0;
	close(fd);
	return taken;
}

int clps_rx_open(int port, int threads, int *why, int *err)
{
	pc_addr sa;
	int i, one = 1;

	*why = CLPS_RX_OFF;
	*err = 0;
	if (!cl_enabled() || threads <= 0 || port <= 0 || port > 65535)
		return 0;
	if (threads > PS_RX_MAX)
		threads = PS_RX_MAX;
	sa = cl_self_addr();
	if (ps_port_taken(&sa, port)) {
		*why = CLPS_RX_TAKEN;
		return 0;
	}
	pc_addr_set_port(&sa, (uint16_t)port);
	for (i = 0; i < threads; i++) {
		int fd = socket(pc_addr_family(&sa), SOCK_DGRAM, 0), rb = PC_CLUSTER_RCVBUF;

		if (fd < 0) {
			*why = CLPS_RX_SOCKET;
			goto fail;
		}
		setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#ifdef SO_RCVBUFFORCE
		if (setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &rb, sizeof rb) != 0)
#endif
			setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rb, sizeof rb);
		if (bind(fd, &sa.sa, pc_addr_len(&sa)) != 0) {
			*why = CLPS_RX_BIND;
			*err = errno;
			close(fd);
			goto fail;
		}
		ps_rx_fd[i] = fd;
		ps_rx_lfd[i] = clrpt_listen(&sa);  /* -1: senders fall back to UDP */
		ps_rx_n = i + 1;
	}
	ps_rx_bylane = clps_lane_spread(ps_rx_fd[0], ps_rx_n);
	ps_rx_port = port;
	*why = CLPS_RX_OPEN;
	return ps_rx_n;
fail:
	for (i = 0; i < ps_rx_n; i++) {
		close(ps_rx_fd[i]);
		if (ps_rx_lfd[i] >= 0)
			close(ps_rx_lfd[i]);
	}
	ps_rx_n = 0;
	ps_rx_port = 0;
	return 0;
}

/* the kernel's flow hash can load receive threads unevenly; a spread by wire lane
 * is even by count */
int clps_lane_spread(int fd, int n)
{
#if defined(__linux__) && defined(SO_ATTACH_REUSEPORT_CBPF)
	struct sock_filter f[] = {
		BPF_STMT(BPF_LD | BPF_B | BPF_ABS, CLWIRE_FLAGS_OFF),
		BPF_STMT(BPF_ALU | BPF_RSH | BPF_K, CLWIRE_LANE_SHIFT),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 3, 0),
		BPF_STMT(BPF_ALU | BPF_SUB | BPF_K, 1),
		BPF_STMT(BPF_ALU | BPF_MOD | BPF_K, (unsigned)n),
		BPF_STMT(BPF_RET | BPF_A, 0),
		BPF_STMT(BPF_RET | BPF_K, 0xffffffffu),   /* past the group: the hash */
	};
	struct sock_fprog prog = { (unsigned short)(sizeof f / sizeof f[0]), f };

	return n > 1 && setsockopt(fd, SOL_SOCKET, SO_ATTACH_REUSEPORT_CBPF,
		&prog, sizeof prog) == 0;
#else
	(void)fd;
	(void)n;
	return 0;
#endif
}

int clps_rx_lanes(void)
{
	return ps_rx_bylane;
}

static void ps_answer_probe(int fd, const unsigned char *pt, size_t n,
		const pc_addr *from, unsigned char ack)
{
	unsigned char msg[11], buf[HDR_LEN + 64];
	size_t len;

	if (n < 11)
		return;
	/* answered only for the live peer holding the id it names, at the address it
	 * came from (clpeers_by_id_live is safe from any thread) */
	{
		struct peer *pp = clpeers_by_id_live(cl_peers(),
			(int)ps_get16(pt + 1), cl_now_ms());

		if (!pp || !pc_addr_eq(&pp->addr, from))
			return;
	}
	msg[0] = ack;
	ps_put16(msg + 1, (unsigned)cl_node_id());
	memcpy(msg + 3, pt + 3, 8);            /* the nonce, echoed */
	len = clsend_wrap(msg, sizeof msg, buf);
	if (len)
		sendto(fd, buf, len, 0, &from->sa, pc_addr_len(from));
}

/* one relay frame, from a datagram or a TCP connection */
struct ps_rx_ctx {
	int fd;
	unsigned char *pt;
};

static void ps_rx_one(void *vctx, const unsigned char *buf, size_t r,
		const pc_addr *from)
{
	struct ps_rx_ctx *x = vctx;
	unsigned char *pt = x->pt;
	unsigned long long ptlen = 0;

	if (clsend_open(buf, r, pt, &ptlen) != 0 || !ptlen) {
		__atomic_add_fetch(&ps_rx_other, 1, __ATOMIC_RELAXED);
		return;
	}
	/* relays are data - only this node's own cluster's,
	 * once it has one (a relay never bootstraps anything) */
	{
		unsigned char mine[CLWIRE_CID_LEN];

		if (clwire_cid(mine) != CLWIRE_CID_NONE &&
		        memcmp(CLWIRE_HDR_CID(buf), mine, CLWIRE_CID_LEN) != 0) {
			__atomic_add_fetch(&ps_rx_other, 1, __ATOMIC_RELAXED);
			return;
		}
	}
	{                                       /* each frame once */
		struct clwire_id wid;
		int rr;

		clwire_id_read(buf, &wid);
		rr = clreplay_admit(&wid, from->in4.sin_addr.s_addr, cl_now_ms());
		if (!clreplay_taken(rr)) {
			__atomic_add_fetch(&ps_rx_other, 1, __ATOMIC_RELAXED);
			return;
		}
	}
	__atomic_add_fetch(&ps_rx_dgrams, 1, __ATOMIC_RELAXED);
	if (pt[0] == M_PUBLISH) {
		if (!clps_handle_relay_publish(pt, ptlen, from))
			__atomic_add_fetch(&ps_rx_other, 1, __ATOMIC_RELAXED);
	} else if (pt[0] == M_PSPROBE)
		ps_answer_probe(x->fd, pt, ptlen, from, M_PSPROBE_ACK);
	else if (pt[0] == M_REPL_MANY)
		clrp_rx(pt, ptlen, from);
	else if (pt[0] == M_RPPROBE && clrp_taking())
		ps_answer_probe(x->fd, pt, ptlen, from, M_RPPROBE_ACK);
	else
		__atomic_add_fetch(&ps_rx_other, 1, __ATOMIC_RELAXED);
}

void pc_cluster_pubsub_rx_thread(volatile int *stop, int idx)
{
	int fd = idx >= 0 && idx < ps_rx_n ? ps_rx_fd[idx] : -1;
	unsigned char *buf = malloc(HDR_LEN + MAX_DGRAM + 64);
	unsigned char *pt = malloc(MAX_DGRAM + 16);
	struct pollfd pf[1 + 1 + CLRPT_RX_CONNS];
	struct clrpt_rx *tr = NULL;
	struct ps_rx_ctx x;

	if (fd < 0 || !buf || !pt) {
		free(buf);
		free(pt);
		return;
	}
	if (ps_rx_lfd[idx] >= 0)
		tr = clrpt_rx_new(ps_rx_lfd[idx], clrp_host_ok);
	x.fd = fd;
	x.pt = pt;
	pf[0].fd = fd;
	pf[0].events = POLLIN;
	pc_qs_enter();                         /* the shared pub/sub index is read here */
	while (!*stop) {
		int seen = 0, prc, nt = tr ? clrpt_rx_pollset(tr, pf + 1,
			1 + CLRPT_RX_CONNS) : 0;

		if (clps_idle_fn)
			clps_idle_fn();
		pc_qs_exit();
		prc = poll(pf, (nfds_t)(1 + nt), 100);
		pc_qs_enter();
		if (prc <= 0)
			continue;
		if (nt)
			clrpt_rx_serve(tr, pf + 1, nt, cl_now_ms(), ps_rx_one, &x);
		if (!(pf[0].revents & POLLIN))
			continue;
		while (seen++ < 512) {
			pc_addr from;
			socklen_t fl = sizeof from;
			ssize_t r = recvfrom(fd, buf, HDR_LEN + MAX_DGRAM + 64,
				MSG_DONTWAIT, &from.sa, &fl);

			pc_iostat_udp_rx(PC_UDP_RELAY, 1, r > 0);
			if (r <= 0)
				break;
			ps_rx_one(&x, buf, (size_t)r, &from);
		}
	}
	pc_qs_exit();
	clrpt_rx_free(tr);
	free(buf);
	free(pt);
}

/* one live peer's step of the once-a-second probe: probe its relay port until it
 * answers, re-confirm while it does, and return the change for the caller to say */
int clps_probe_peer(struct peer *p, long long now)
{
	uint64_t nonce;
	int ev;

	if (!__atomic_load_n(&p->node, __ATOMIC_ACQUIRE) ||
	    !clpeers_live(p, now))
		return CLPS_EV_NONE;
	if (!cl_node_id())
		return CLPS_EV_NONE;           /* probes are answered by id: need one first */
	randombytes_buf(&nonce, sizeof nonce);
	if (psr_path_probe_due(&p->ps, now, nonce | 1)) {
		unsigned char msg[11];
		pc_addr to = p->addr;

		if (!p->ps_since_ms)
			p->ps_since_ms = now;
		msg[0] = M_PSPROBE;
		ps_put16(msg + 1, (unsigned)cl_node_id());
		memcpy(msg + 3, &p->ps.nonce, 8);
		pc_addr_set_port(&to, (uint16_t)p->ps.port);
		clsend_seal(&to, msg, sizeof msg);
	}
	ev = psr_path_transition(&p->ps, now);
	if (ev > 0) {
		p->ps_since_ms = 0;
		p->ps_warned = 0;
		return CLPS_EV_DIRECT;
	} else if (ev < 0) {
		/* this is the warning for this outage: the "never answered"
		 * one below is for a port that was never proven */
		p->ps_warned = 1;
		return CLPS_EV_STOPPED;
	} else if (!p->ps.direct && p->ps.port && p->ps_since_ms &&
	        now - p->ps_since_ms >= 10000 && !p->ps_warned) {
		p->ps_warned = 1;
		return CLPS_EV_SILENT;
	}
	return CLPS_EV_NONE;
}

void clps_handle_probe_ack(const unsigned char *pt, size_t n,
		const pc_addr *from)
{
	uint64_t nonce;
	int node, i;

	if (n < 11)
		return;
	node = (int)ps_get16(pt + 1);
	memcpy(&nonce, pt + 3, 8);
	for (i = 0; i < cl_peers()->n_peers; i++) {
		struct peer *p = &cl_peers()->peers[i];

		if (__atomic_load_n(&p->node, __ATOMIC_ACQUIRE) == node &&
		    pc_addr_eq_host(&p->addr, from)) {
			psr_path_answer(&p->ps, nonce, cl_now_ms());
			return;
		}
	}
}

void pc_cluster_pubsub_relay_figures(int *port, int *threads, int *peers_direct,
		unsigned long long *sent_direct, unsigned long long *sent_cluster,
		unsigned long long *rx_dgrams)
{
	long long now = cl_now_ms();
	int i, d = 0;

	for (i = 0; cl_enabled() && i < cl_peers()->n_peers; i++)
		if (__atomic_load_n(&cl_peers()->peers[i].node, __ATOMIC_ACQUIRE) &&
		    psr_path_direct(&cl_peers()->peers[i].ps, now))
			d++;
	*port = ps_rx_port;
	*threads = ps_rx_n;
	*peers_direct = d;
	*sent_direct = __atomic_load_n(&ps_sent_direct, __ATOMIC_RELAXED);
	*sent_cluster = __atomic_load_n(&ps_sent_cluster, __ATOMIC_RELAXED);
	*rx_dgrams = __atomic_load_n(&ps_rx_dgrams, __ATOMIC_RELAXED);
}

/* ---- the cluster thread's side of interest ------------------------------ */

static void pv_begin(void) { __atomic_add_fetch(&ps_int_seq, 1, __ATOMIC_ACQ_REL); }
static void pv_end(void) { __atomic_add_fetch(&ps_int_seq, 1, __ATOMIC_ACQ_REL); }

static void pv_mode_set(struct peer *p, unsigned char m)   /* inside pv_begin */
{
	if (p->pv_mode == m)
		return;
	if (p->pv_mode == PV_FILTER)
		__atomic_sub_fetch(&ps_nfilter, 1, __ATOMIC_ACQ_REL);
	if (m == PV_FILTER)
		__atomic_add_fetch(&ps_nfilter, 1, __ATOMIC_ACQ_REL);
	__atomic_store_n(&p->pv_mode, m, __ATOMIC_RELEASE);
}

static int fpat_add(int slot, const char *name, size_t nlen)   /* inside pv_begin */
{
	struct psi_node *x;
	struct ps_fpat *f;

	if (!ps_fpat_ready)
		return -1;
	x = psi_find(&ps_fpat, name, nlen);
	if (x) {
		f = (struct ps_fpat *)x;
	} else {
		f = calloc(1, sizeof *f + nlen + 1);
		if (!f)
			return -1;
		memcpy(f->name, name, nlen);
		f->node.name = f->name;
		f->node.nlen = (uint32_t)nlen;
		psi_insert(&ps_fpat, &f->node);
	}
	__atomic_or_fetch(&f->peers[slot / 64], 1ULL << (slot % 64),
		__ATOMIC_ACQ_REL);
	return 0;
}

static int fpat_drop_slot(struct psi_node *x, void *arg)   /* inside pv_begin */
{
	struct ps_fpat *f = (struct ps_fpat *)x;
	int slot = *(int *)arg, w, empty = 1;

	__atomic_and_fetch(&f->peers[slot / 64], ~(1ULL << (slot % 64)),
		__ATOMIC_ACQ_REL);
	for (w = 0; w < PS_PEER_WORDS; w++)
		if (__atomic_load_n(&f->peers[w], __ATOMIC_ACQUIRE))
			empty = 0;
	if (empty) {
		/* its next link stays, so a walk standing here carries on */
		psi_remove(&ps_fpat, x);
		f->stamp = pc_qs_stamp();
		f->retired_next = ps_fpat_retired;
		ps_fpat_retired = f;
	}
	return 0;
}

static void fpat_reclaim(void)
{
	struct ps_fpat **pp = &ps_fpat_retired;

	while (*pp) {
		struct ps_fpat *f = *pp;

		if (pc_qs_clear(f->stamp)) {
			*pp = f->retired_next;
			free(f);
		} else {
			pp = &f->retired_next;
		}
	}
}

/* the peer wants everything again, or its slot is being freed */
void clps_forget(struct peer *p)
{
	int slot = (int)(p - cl_peers()->peers);

	pv_begin();
	pv_mode_set(p, PV_ALL);
	if (ps_fpat_ready)
		psi_each(&ps_fpat, fpat_drop_slot, &slot);
	pv_end();
	memset(&p->pv, 0, sizeof p->pv);
	p->pv_hb = 0;
	p->pv_everything = 0;
	p->pv_uncovered_ms = 0;
	p->pv_req_ms = 0;
}

static void pv_evaluate(struct peer *p, long long now)
{
	unsigned char m;

	if (!p->pv_hb)
		m = PV_ALL;
	else if (!p->pv_bloom || p->pv_everything || !psv_covered(&p->pv, p->pv_hb))
		m = PV_BROADCAST;
	else
		m = PV_FILTER;
	if (m != PV_BROADCAST)
		p->pv_uncovered_ms = 0;
	else if (!p->pv_uncovered_ms)
		p->pv_uncovered_ms = now ? now : 1;
	if (m != p->pv_mode) {
		pv_begin();
		pv_mode_set(p, m);
		pv_end();
	}
}

/* ask for its full state when there is none, it is from another epoch, the
 * peer rebuilt, or an update has been missing for a second; once a second */
static void pv_maybe_request(struct peer *p, long long now)
{
	unsigned char msg[3];
	int need;

	if (!p->pv_hb)
		return;
	need = !p->pv.full || psv_epoch(p->pv_hb) != psv_epoch(p->pv.base) ||
		psv_behind_rebuild(&p->pv, p->pv_hb) ||
		(!psv_covered(&p->pv, p->pv_hb) && p->pv_uncovered_ms &&
		 now - p->pv_uncovered_ms >= 1000);
	if (!need || (p->pv_req_ms && now - p->pv_req_ms < 1000))
		return;
	p->pv_req_ms = now;
	msg[0] = M_PSINT_REQ;
	ps_put16(msg + 1, (unsigned)cl_node_id());
	if (clsend_seal(&p->addr, msg, sizeof msg) == 0)
		__atomic_add_fetch(&ps_int_req_sent, 1, __ATOMIC_RELAXED);
}

/* what it may have told us beyond its last heartbeat: a newer epoch, or
 * more updates */
static void pv_learn(struct peer *p, uint64_t ver)
{
	if (!p->pv_hb || psv_epoch(ver) != psv_epoch(p->pv_hb) ||
	    psv_adds(ver) > psv_adds(p->pv_hb))
		p->pv_hb = ver;
}

static void pv_heartbeat(struct peer *p, uint64_t iv, long long now)
{
	if (!iv) {
		if (p->pv_hb || p->pv_mode != PV_ALL)
			clps_forget(p);
		return;
	}
	p->pv_hb = iv;
	pv_evaluate(p, now);
	pv_maybe_request(p, now);
}

/* a heartbeat's relay fields, from the peer thread: 1 when it withdrew a port
 * it was being sent on */
int clps_heard(struct peer *p, uint16_t np, uint64_t interest, long long now)
{
	int withdrawn = 0;

	/* a new or withdrawn relay port starts unproven */
	if (np != __atomic_load_n(&p->ps.port, __ATOMIC_ACQUIRE)) {
		/* a withdrawn port is a peer's configuration, not an
		 * outage: the caller says so, and not as a fallback */
		withdrawn = !np && p->ps.direct;
		p->ps.direct = 0;
		p->ps_since_ms = 0;
		p->ps_warned = 0;
		p->rp.direct = 0;
		p->rp_since_ms = 0;
		p->rp_warned = 0;
	}
	psr_path_port(&p->ps, np);
	psr_path_port(&p->rp, np);                  /* the same port */
	/* what it wants relayed */
	pv_heartbeat(p, interest, now);
	return withdrawn;
}

void pc_cluster_pubsub_interest_add(uint64_t version, int pattern,
		const char *name, size_t nlen, uint32_t h1, uint32_t h2)
{
	unsigned char msg[14 + 4096];
	unsigned char sealed[HDR_LEN + sizeof msg + 32];
	size_t n, slen;
	long long now;
	int i;

	if (!cl_enabled())
		return;
	msg[0] = M_PSINT_ADD;
	ps_put16(msg + 1, (unsigned)cl_node_id());
	ps_put64(msg + 3, version);
	if (!pattern) {
		msg[11] = 0;
		ps_put32(msg + 12, h1);
		ps_put32(msg + 16, h2);
		n = 20;
	} else {
		/* too long to send: a peer sees the count move without it and
		 * asks for the full state */
		if (nlen > sizeof msg - 14)
			return;
		msg[11] = 1;
		ps_put16(msg + 12, (unsigned)nlen);
		memcpy(msg + 14, name, nlen);
		n = 14 + nlen;
	}
	if (!(slen = clsend_wrap(msg, n, sealed)))
		return;
	now = cl_now_ms();
	for (i = 0; i < cl_peers()->n_peers; i++) {
		struct peer *p = &cl_peers()->peers[i];

		if (__atomic_load_n(&p->node, __ATOMIC_ACQUIRE) &&
		    clpeers_live(p, now) &&
		    clsend_raw(&p->addr, sealed, slen) >= 0)
			__atomic_add_fetch(&ps_int_upd_sent, 1, __ATOMIC_RELAXED);
	}
}

void clps_handle_int_add(const unsigned char *pt, size_t n,
		const pc_addr *from)
{
	struct peer *p;
	uint64_t ver;
	unsigned kind;
	int slot;

	if (n < 12 || !(p = clpeers_by_id_from(cl_peers(), ps_get16(pt + 1), from)))
		return;
	ver = ps_get64(pt + 3);
	kind = pt[11];
	if (!ver || kind > 1 || (kind == 0 && n != 20) ||
	    (kind == 1 && (n < 14 || n != 14 + (size_t)ps_get16(pt + 12))))
		return;
	__atomic_add_fetch(&ps_int_upd_recv, 1, __ATOMIC_RELAXED);
	slot = (int)(p - cl_peers()->peers);
	if (psv_add(&p->pv, ver) == PSV_APPLY) {
		pv_begin();
		if (kind == 0 && p->pv_bloom)
			psb_add(p->pv_bloom, (uint32_t)ps_get32(pt + 12),
				(uint32_t)ps_get32(pt + 16));
		else if (kind == 1 &&
		         fpat_add(slot, (const char *)pt + 14, ps_get16(pt + 12)) != 0)
			p->pv_everything = 1;      /* a pattern we cannot hold */
		pv_end();
	}
	pv_learn(p, ver);
	pv_evaluate(p, cl_now_ms());
	pv_maybe_request(p, cl_now_ms());
}

void clps_handle_int_req(const unsigned char *pt, size_t n,
		const pc_addr *from)
{
	static unsigned char *msg;             /* the cluster thread's */
	size_t len;

	if (n < 3 || !pc_pubsub_interest_version() ||
	    !clpeers_by_id_from(cl_peers(), ps_get16(pt + 1), from))
		return;
	if (!msg && !(msg = malloc(MAX_DGRAM)))
		return;
	msg[0] = M_PSINT_FULL;
	ps_put16(msg + 1, (unsigned)cl_node_id());
	len = pc_pubsub_interest_full(msg + 3, MAX_DGRAM - 3);
	if (len && clsend_seal(from, msg, 3 + len) == 0)
		__atomic_add_fetch(&ps_int_full_sent, 1, __ATOMIC_RELAXED);
}

void clps_handle_int_full(const unsigned char *pt, size_t n,
		const pc_addr *from)
{
	const unsigned char *b = pt + 3;
	struct peer *p;
	uint64_t ver;
	size_t bn, off;
	unsigned npat, k;
	int slot, failed = 0;

	if (n < 3 + PSF_HDR || !(p = clpeers_by_id_from(cl_peers(), ps_get16(pt + 1), from)))
		return;
	bn = n - 3;
	ver = ps_get64(b);
	npat = ps_get16(b + PSF_HDR - 2);
	for (off = PSF_HDR, k = 0; k < npat; k++) {
		if (off + 2 > bn || off + 2 + ps_get16(b + off) > bn)
			return;
		off += 2 + ps_get16(b + off);
	}
	if (!ver || off != bn || !psv_full_applies(&p->pv, ver))
		return;
	if (!p->pv_bloom) {
		uint8_t *nb = calloc(1, PSB_BYTES);

		if (!nb)
			return;
		__atomic_store_n(&p->pv_bloom, nb, __ATOMIC_RELEASE);
	}
	slot = (int)(p - cl_peers()->peers);
	pv_begin();
	memcpy(p->pv_bloom, b + 9, PSB_BYTES);
	if (ps_fpat_ready)
		psi_each(&ps_fpat, fpat_drop_slot, &slot);
	for (off = PSF_HDR, k = 0; k < npat; k++) {
		unsigned len = ps_get16(b + off);

		if (fpat_add(slot, (const char *)b + off + 2, len) != 0)
			failed = 1;
		off += 2 + len;
	}
	p->pv_everything = (b[8] & PSF_EVERYTHING) || failed;
	psv_full(&p->pv, ver);
	pv_end();
	__atomic_add_fetch(&ps_int_full_recv, 1, __ATOMIC_RELAXED);
	pv_learn(p, ver);
	pv_evaluate(p, cl_now_ms());
}

/* once a second, beside the relay-port probes */
void clps_interest_tick(long long now)
{
	int i;

	for (i = 0; i < cl_peers()->n_peers; i++) {
		struct peer *p = &cl_peers()->peers[i];

		if (!__atomic_load_n(&p->node, __ATOMIC_ACQUIRE) ||
		    !clpeers_live(p, now) || !p->pv_hb)
			continue;
		pv_evaluate(p, now);
		pv_maybe_request(p, now);
	}
	fpat_reclaim();
	pc_pubsub_interest_tick(now);
}

void pc_cluster_pubsub_interest_figures(struct pc_psint_figures *f)
{
	int i;

	memset(f, 0, sizeof *f);
	f->version = pc_pubsub_interest_version();
	for (i = 0; cl_enabled() && i < cl_peers()->n_peers; i++) {
		const struct peer *p = &cl_peers()->peers[i];
		unsigned char m;

		if (!__atomic_load_n(&p->node, __ATOMIC_ACQUIRE))
			continue;
		m = __atomic_load_n(&p->pv_mode, __ATOMIC_ACQUIRE);
		f->peers_filtered += m == PV_FILTER;
		f->peers_broadcast += m == PV_BROADCAST;
	}
	f->skipped = __atomic_load_n(&ps_skipped, __ATOMIC_RELAXED);
	f->updates_sent = __atomic_load_n(&ps_int_upd_sent, __ATOMIC_RELAXED);
	f->updates_received = __atomic_load_n(&ps_int_upd_recv, __ATOMIC_RELAXED);
	f->resyncs_sent = __atomic_load_n(&ps_int_full_sent, __ATOMIC_RELAXED);
	f->resyncs_received = __atomic_load_n(&ps_int_full_recv, __ATOMIC_RELAXED);
	f->requests_sent = __atomic_load_n(&ps_int_req_sent, __ATOMIC_RELAXED);
}

/* ---- the plane's state, for cluster.c ------------------------------------ */

/* patterns the peers hold; fixed buckets, read without a lock */
void clps_init(void)
{
	ps_fpat_ready = psi_init(&ps_fpat, 4096, PSI_PATTERNS) == 0;
}

/* a receiver tells this process's relay sequence from the last one's
 * by it (psrelay.h) */
void clps_new_epoch(void)
{
	randombytes_buf(&pubsub_epoch, sizeof pubsub_epoch);
}

int clps_rx_port(void)
{
	return ps_rx_port;
}
