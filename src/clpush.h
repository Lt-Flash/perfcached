/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clpush.h - the write path's push groups, per THREAD per peer so no lock sits on it: a write appends
 * one record to every live peer's open group; nothing here sends (the caller's callback seals) */
#ifndef PC_CLPUSH_H
#define PC_CLPUSH_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include "pcaddr.h"

#define PC_TIDX_MAX 528                 /* daemon.c: 512 workers + 6 */

struct wgroup {
	unsigned char *q;                  /* hdr + group + one record */
	/* what q ACTUALLY holds - guard appends against this, not the
	 * wire's `cap`, or a larger record writes off the end */
	size_t qcap;
	size_t qlen;
	unsigned int qcount;
	long long qopen_ms;
	pc_addr to;             /* the peer this group is for -
	                                    * a reused slot is a new peer */
	/* the records this node took as a spread NON-holder ([collen1][klen2][ver8][col][key]), dropped once
	 * the holder acks the group; handed to the ack table at send, so NULL after every send */
	unsigned char *nh;
	size_t nhlen, nhcap;
	unsigned int nhcount;
};

/* a sent group awaiting its ack: sequential ids with the top bit set (migration ids are 31-bit),
 * slot = the low bits; a slot still busy refuses the park, and those copies stay (reclaim's surplus) */
#define CLPUSH_ACKS      4096
#define CLPUSH_ACK_TAG   0x80000000u
struct clpush_ack {
	int state;                         /* 0 free 1 filling 2 sent 3 taken */
	uint32_t req;
	unsigned int count;                /* records in the group */
	long long sent_ms;
	pc_addr to;
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
	struct clpush_ack *acks;        /* CLPUSH_ACKS, lazily */
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

/* append to thread `tidx`'s group for peer `i`, sending a group full or stale BEFORE the append and
 * one past flush_bytes after it; a slot that changed hands drops its old gathering.  Worker thread. */
void clpush_append(struct clpush *p, int tidx, int i,
		const pc_addr *to, const unsigned char *rec,
		size_t n, long long now, clpush_send_fn send, void *ctx);

/* as clpush_append, for a record taken as a NON-holder: its (col, key, ver) rides in the SAME group,
 * so the group's ack confirms it; a key that cannot be noted keeps its copy */
void clpush_append_nh(struct clpush *p, int tidx, int i,
		const pc_addr *to, const unsigned char *rec,
		size_t n, long long now, clpush_send_fn send, void *ctx,
		const char *col, size_t collen, const char *key, size_t klen,
		unsigned long long ver);

/* park a group carrying non-holder keys, from its send callback before clpush_sent(): the request id
 * to stamp, or 0 (no keys, no slot) for a fire-and-forget send.  Takes g->nh.  Worker thread. */
uint32_t clpush_ack_park(struct clpush *p, struct wgroup *g, long long now);

/* the parked group @req, if it went to @from; the caller owns the
 * slot's keys until clpush_ack_done().  0 = not ours, or already gone. */
struct clpush_ack *clpush_ack_take(struct clpush *p, uint32_t req,
		const pc_addr *from);
void clpush_ack_done(struct clpush *p, struct clpush_ack *a);

/* groups parked longer than @ms never had an ack: their keys leave the table (the COPIES stay);
 * returns how many records.  One thread only - the one that takes acks. */
unsigned int clpush_ack_expire(struct clpush *p, long long now, int ms);

/* Every group of thread `tidx` older than flush_ms goes now.  Runs ON
 * that thread - the flush RPC, or the thread itself. */
void clpush_flush_mine(struct clpush *p, int tidx, long long now,
		clpush_send_fn send, void *ctx);

#endif /* PC_CLPUSH_H */
