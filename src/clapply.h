/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clapply.h - replica apply sharded by hash(col, key) over N threads, one SPSC ring
 * each, so a key's records apply in arrival order (a hot key pins one thread). */
#ifndef PC_CLAPPLY_H
#define PC_CLAPPLY_H

#include <netinet/in.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include "pcaddr.h"

#define CLAPPLY_MAX      16
#define CLAPPLY_RING_SZ  (1u << 20)    /* per thread; a record is <= a datagram */

/* one record, as the receiver parsed it */
struct clapply_rec {
	const unsigned char *col, *key, *val;
	unsigned int collen, klen, vlen, ttl_left;
	unsigned long long ver;
	int passive;
	unsigned char flags;           /* the record's type */
};

/* a group that wants an ack once EVERY record of it has been applied,
 * whichever threads applied them */
struct clapply_grp {
	pc_addr from;
	uint32_t req;
	int remaining;                 /* atomic; the receiver holds one
	                                * until it has dispatched the lot */
	unsigned int stored;           /* atomic */
};

/* what the consumer sees; the bytes point into the ring and are valid
 * until clapply_done() */
struct clapply_item {
	struct clapply_rec r;
	struct clapply_grp *grp;
};

struct clapply_ring {
	unsigned char *buf;
	unsigned int cap;
	unsigned int head;             /* consumer reads, producer writes: atomic */
	unsigned int tail;             /* producer writes, consumer reads: atomic */
	/* for the stall detector: when the consumer took its current item, 0 = idle; stamped
	 * by a->clock at the take, not from now_ms, which is read before the take blocks */
	long long busy_since_ms;
	/* the other stall, a consumer that never TAKES: the detector stamps this on a
	 * backlog over an idle ring, and every take clears it */
	long long wait_since_ms;
	unsigned long long items, blocks;   /* handled; producer waits on full */
	pthread_mutex_t mx;            /* the consumer's wait, not the data */
	pthread_cond_t cv;
	int sleeping;
};

struct clapply {
	int n;                         /* threads; 0 = off, apply inline */
	struct clapply_ring ring[CLAPPLY_MAX];
	int (*apply)(const struct clapply_rec *r, void *ctx);   /* 1 = stored,
	                                                         * 2 = already had it newer */
	void (*ack)(const struct clapply_grp *g, void *ctx);
	void *ctx;
	long long (*clock)(void);      /* ms clock for busy_since_ms; NULL =
	                                * the take's now_ms argument (tests) */
	void (*idle)(void);            /* a take that waited for nothing calls it; NULL none */
	volatile int *stop;            /* the owner's, else the first take's; a push
	                                * waits on a full ring until this says otherwise */
	unsigned long long dispatched, grp_acked;
};

/* @n threads; 0 or 1 = off.  Returns the thread count in use, -1 = no memory. */
int clapply_init(struct clapply *a, int n,
		int (*apply)(const struct clapply_rec *, void *),
		void (*ack)(const struct clapply_grp *, void *), void *ctx);
void clapply_free(struct clapply *a);

/* which thread @key belongs to */
int clapply_shard(const struct clapply *a, const unsigned char *col,
		unsigned int collen, const unsigned char *key, unsigned int klen);

/* Receiver side: a group (req != 0) holds a count until clapply_close(), so no thread
 * finishes it early; clapply_push() copies into the ring, waiting - never dropping - if full. */
struct clapply_grp *clapply_open(struct clapply *a,
		const pc_addr *from, uint32_t req);
int clapply_push(struct clapply *a, const struct clapply_rec *r,
		struct clapply_grp *g);
void clapply_close(struct clapply *a, struct clapply_grp *g);

/* Consumer side, thread @idx: clapply_take() blocks for an item or @stop (0 = stopped,
 * 1 = an item); clapply_done() releases it and completes the group. */
int clapply_take(struct clapply *a, int idx, volatile int *stop,
		struct clapply_item *out, long long now_ms);
void clapply_done(struct clapply *a, int idx, struct clapply_item *it);

/* bytes queued on thread @idx (a gauge) */
unsigned int clapply_backlog(const struct clapply *a, int idx);

#endif /* PC_CLAPPLY_H */
