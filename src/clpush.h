/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clpush.h - the per-thread write-path push groups (M5).
 *
 * S73 decided the coalescing buffer: a write appends one record to the
 * open group of every live peer, so its cost grows by an append rather
 * than by a seal and a syscall per peer.  S110 made the group per
 * THREAD per peer, which is what took the lock off the write path - a
 * shared per-peer mutex had 57% of every worker sitting in futex, with
 * throughput flat from 50 to 400 clients.
 *
 * Nothing here sends.  A group that is ready goes out through a SEND
 * CALLBACK the caller supplies, because sealing a datagram is clwire's
 * (M6) and the peer table is clpeers'.  That is also what lets the test
 * run with no socket: it passes a stub that records instead.
 *
 * The geometry is the caller's too - header size, gather cap, flush
 * bytes and flush interval are the migrate wire's numbers (clmig, M10),
 * so they arrive at init rather than being duplicated here.
 */
#ifndef PC_CLPUSH_H
#define PC_CLPUSH_H

#include <netinet/in.h>
#include <stddef.h>

#define PC_TIDX_MAX 528                 /* daemon.c: 512 workers + 6 */

struct wgroup {
	unsigned char *q;                  /* hdr + group + one record */
	size_t qlen;
	unsigned int qcount;
	long long qopen_ms;
	struct sockaddr_in to;             /* the peer this group is for -
	                                    * a reused slot is a new peer */
};

/* how a ready group leaves: the caller seals and sends it, then calls
 * clpush_sent() so the group is reset and the open count drops */
typedef void (*clpush_send_fn)(struct wgroup *g, void *ctx);

struct clpush {
	struct wgroup *g[PC_TIDX_MAX];  /* [thread slot] -> per-peer array */
	int npeers;                     /* width of each thread's array */
	size_t hdr, cap, flush_bytes;   /* the migrate wire's geometry */
	int flush_ms;
	int open;                       /* groups open across all peers */
};

void clpush_init(struct clpush *p, int npeers, size_t hdr, size_t cap,
		size_t flush_bytes, int flush_ms);
void clpush_fini(struct clpush *p);

/* groups open right now - the cluster loop's timer skips the flush RPC
 * entirely when this is zero.  Any thread. */
int clpush_open(const struct clpush *p);

/* Reset a group the caller has just sent, and drop the open count.  The
 * send callback calls this, or the caller does straight after. */
void clpush_sent(struct clpush *p, struct wgroup *g);

/* Append one record to thread `tidx`'s group for peer `i`, sending
 * through `send` at either of the two points the write path has: a
 * group that is full or stale BEFORE the append, and a group that has
 * crossed flush_bytes after it.  A slot that changed hands drops what
 * was gathered for its old occupant.  Worker thread. */
void clpush_append(struct clpush *p, int tidx, int i,
		const struct sockaddr_in *to, const unsigned char *rec,
		size_t n, long long now, clpush_send_fn send, void *ctx);

/* Every group of thread `tidx` older than flush_ms goes now.  Runs ON
 * that thread - the flush RPC, or the thread itself. */
void clpush_flush_mine(struct clpush *p, int tidx, long long now,
		clpush_send_fn send, void *ctx);

#endif /* PC_CLPUSH_H */
