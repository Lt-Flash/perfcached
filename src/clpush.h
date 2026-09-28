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
#include <stdint.h>

#define PC_TIDX_MAX 528                 /* daemon.c: 512 workers + 6 */

struct wgroup {
	unsigned char *q;                  /* hdr + group + one record */
	/* what q ACTUALLY holds.  It used to be implied by the first
	 * record's size and never written down, so the pre-append guard
	 * checked qlen + n against the wire's `cap` - a different number
	 * entirely - and a later, larger record wrote off the end. */
	size_t qcap;
	size_t qlen;
	unsigned int qcount;
	long long qopen_ms;
	struct sockaddr_in to;             /* the peer this group is for -
	                                    * a reused slot is a new peer */
	/* S253: the records in q this node took as a spread NON-holder,
	 * as [collen1][klen2][ver8][col][key] - the copies to drop once
	 * the holder acks the whole group.  Handed to the ack table when
	 * the group is sent, so NULL again after every send. */
	unsigned char *nh;
	size_t nhlen, nhcap;
	unsigned int nhcount;
};

/* S253: a sent group waiting for its ack.  Sequential request ids with
 * the top bit set - migration ids are 31-bit (cluster.c), so an ack can
 * never be mistaken for one - and the slot is the id's low bits.  A
 * slot still busy when its index comes round again refuses the park:
 * that group goes unacked and its copies stay (reclaim's surplus). */
#define CLPUSH_ACKS      4096
#define CLPUSH_ACK_TAG   0x80000000u
struct clpush_ack {
	int state;                         /* 0 free 1 filling 2 sent 3 taken */
	uint32_t req;
	unsigned int count;                /* records in the group */
	long long sent_ms;
	struct sockaddr_in to;
	unsigned char *nh;                 /* owned while parked */
	size_t nhlen;
	unsigned int nhcount;
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
	struct clpush_ack *acks;        /* S253, CLPUSH_ACKS, lazily */
	uint32_t ack_seq;
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

/* S253: as clpush_append, and the record is one this node took as a
 * NON-holder: its (col, key, ver) rides beside it in the SAME group, so
 * the group's ack is what confirms it.  A key that cannot be noted is
 * simply not noted - its copy stays. */
void clpush_append_nh(struct clpush *p, int tidx, int i,
		const struct sockaddr_in *to, const unsigned char *rec,
		size_t n, long long now, clpush_send_fn send, void *ctx,
		const char *col, size_t collen, const char *key, size_t klen,
		unsigned long long ver);

/* S253: park a group that carries non-holder keys, from its send
 * callback, before clpush_sent().  Returns the request id to stamp on
 * the group, or 0 - no keys, or no free slot - for a fire-and-forget
 * send.  Takes g->nh.  Worker thread. */
uint32_t clpush_ack_park(struct clpush *p, struct wgroup *g, long long now);

/* S253: the parked group @req, if it went to @from; the caller owns the
 * slot's keys until clpush_ack_done().  0 = not ours, or already gone. */
struct clpush_ack *clpush_ack_take(struct clpush *p, uint32_t req,
		const struct sockaddr_in *from);
void clpush_ack_done(struct clpush *p, struct clpush_ack *a);

/* S253: groups parked longer than @ms never had an ack: their keys are
 * dropped from the table (the COPIES stay).  Returns how many records
 * that was.  One thread only - the one that takes acks. */
unsigned int clpush_ack_expire(struct clpush *p, long long now, int ms);

/* Every group of thread `tidx` older than flush_ms goes now.  Runs ON
 * that thread - the flush RPC, or the thread itself. */
void clpush_flush_mine(struct clpush *p, int tidx, long long now,
		clpush_send_fn send, void *ctx);

#endif /* PC_CLPUSH_H */
