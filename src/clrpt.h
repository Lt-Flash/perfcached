/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clrpt.h - S469: replication groups over max_datagram go by TCP to the peer's
 * relay port - a connection per sending worker, read by the relay threads */
#ifndef PC_CLRPT_H
#define PC_CLRPT_H

#include <poll.h>
#include <stddef.h>
#include "pcaddr.h"

#define CLRPT_QMAX       (4u << 20)    /* bytes queued on one connection */
#define CLRPT_QTOTAL     (64u << 20)   /* OM2-12: what every sending thread's queues hold together */
#define CLRPT_PEERS      64            /* connections a sending thread holds */
#define CLRPT_RX_CONNS   256           /* connections a receive thread holds */
#define CLRPT_IDLE_MS    120000        /* an inbound connection silent this long goes */
#define CLRPT_FRAME_MS   10000         /* OM2-11: a frame begun must be whole by then */
#define CLRPT_RETRY_MS   1000          /* after a failed connect or a broken one */

/* ---- sending: any worker, its own connections (thread-local) ---- */
/* queue sealed @f (@n bytes) to @relay (the peer's relay address): 0 queued,
 * -1 not (no connection yet, the queue full, an error) - the caller falls back */
int clrpt_send(const pc_addr *relay, const unsigned char *f, size_t n);
/* write what this thread has queued; from its flush tick */
void clrpt_flush(void);
/* tests: the queue's bound, and the source address connections bind (NULL: none) */
void clrpt_set_qmax(size_t qmax);
void clrpt_set_source(const pc_addr *src);

/* ---- receiving: a relay thread's listener and connections ---- */
/* a listening socket on @at (SO_REUSEPORT: one a receive thread); -1 */
int clrpt_listen(const pc_addr *at);
typedef void (*clrpt_frame_fn)(void *ctx, const unsigned char *f, size_t n,
		const pc_addr *from);
struct clrpt_rx;
/* OM2-11: @host_ok refuses a connection at accept by its address (NULL: any) */
typedef int (*clrpt_host_fn)(const pc_addr *from);
struct clrpt_rx *clrpt_rx_new(int lfd, clrpt_host_fn host_ok);
void clrpt_rx_free(struct clrpt_rx *r);
/* its fds after pf[0..] for poll: returns how many were added (<= max) */
int clrpt_rx_pollset(struct clrpt_rx *r, struct pollfd *pf, int max);
/* serve what poll found ready in those @n entries; each whole frame to @fn */
void clrpt_rx_serve(struct clrpt_rx *r, const struct pollfd *pf, int n,
		long long now_ms, clrpt_frame_fn fn, void *ctx);

struct clrpt_figs {
	unsigned long long sent_frames, sent_bytes, fallbacks, connects,
		connect_fail, lost_frames;
	unsigned long long rx_frames, rx_bytes, rx_accepts, rx_refused;
};
void clrpt_figures(struct clrpt_figs *f);

#endif
