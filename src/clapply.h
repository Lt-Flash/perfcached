/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clapply.h - S217: replica apply sharded by key hash over N threads.
 *
 * One thread (pc-cluster) receives, authenticates and decrypts every
 * datagram, and until now also APPLIED every replicated record in it.
 * With `[cluster] apply_threads = N` (N >= 2) the receiver keeps the
 * socket and the decrypt and hands each record to apply thread
 * hash(col, key) % N over that thread's own ring - one producer, one
 * consumer, no lock.
 *
 * WHY A HASH AND NOT A QUEUE.  A work queue with competing consumers
 * gives no order between two items taken by different workers, and
 * replica apply is the one job here where order is the correctness
 * condition: a set and the delete that follows it, or two versions of
 * one key, applied by two workers in either order, is S209 built back
 * in.  Versions make the END state converge; a reader between the two
 * applies would see the older value after the newer, and the tombstone
 * and negative-cache paths assume per-key order.  So every record of a
 * key goes to the same thread, in the order it arrived - what RabbitMQ
 * calls a consistent-hash exchange.  The trade-off is accepted: a hot
 * key pins one thread.
 *
 * A full ring BLOCKS the receiver (the socket buffer takes the
 * backpressure, where S38 already counts drops); nothing here drops or
 * reorders a record.
 *
 * Pure over its struct: no socket, no log, no clock.  The apply itself
 * and the ack are callbacks, so the module is exercised without a
 * daemon (clapplytest).
 */
#ifndef PC_CLAPPLY_H
#define PC_CLAPPLY_H

#include <netinet/in.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#define CLAPPLY_MAX      16
#define CLAPPLY_RING_SZ  (1u << 20)    /* per thread; a record is <= a datagram */

/* one record, as the receiver parsed it */
struct clapply_rec {
	const unsigned char *col, *key, *val;
	unsigned int collen, klen, vlen, ttl_left;
	unsigned long long ver;
	int passive;
};

/* a group that wants an ack once EVERY record of it has been applied,
 * whichever threads applied them */
struct clapply_grp {
	struct sockaddr_in from;
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
	/* the consumer's progress, for the stall detector (S216): the
	 * moment it took the item it is on, 0 = idle */
	long long busy_since_ms;
	/* and the other way a consumer stalls: it never TAKES.  The stall
	 * detector stamps this when it sees a backlog on an idle ring and
	 * the consumer clears it on every take, so "a backlog nobody has
	 * touched for apply_stall_ms" is readable from outside. */
	long long wait_since_ms;
	unsigned long long items, blocks;   /* handled; producer waits on full */
	pthread_mutex_t mx;            /* the consumer's wait, not the data */
	pthread_cond_t cv;
	int sleeping;
};

struct clapply {
	int n;                         /* threads; 0 = off, apply inline */
	struct clapply_ring ring[CLAPPLY_MAX];
	/* callbacks */
	int (*apply)(const struct clapply_rec *r, void *ctx);   /* 1 = stored */
	void (*ack)(const struct clapply_grp *g, void *ctx);
	void *ctx;
	volatile int *stop;            /* set by the owner; a push waits on a
	                                * full ring until this says otherwise */
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

/* Receiver side.  A group is opened when the sender wants an ack
 * (req != 0): the receiver holds one count on it until clapply_close()
 * so an early thread cannot finish it first.  clapply_push() copies the
 * record into its thread's ring, waiting (never dropping) while the ring
 * is full; @now_ms is stamped for the stall detector.  Returns the
 * thread it went to. */
struct clapply_grp *clapply_open(struct clapply *a,
		const struct sockaddr_in *from, uint32_t req);
int clapply_push(struct clapply *a, const struct clapply_rec *r,
		struct clapply_grp *g);
void clapply_close(struct clapply *a, struct clapply_grp *g);

/* Consumer side, thread @idx.  clapply_take() blocks until an item is
 * there or @stop is set (0 = stopped, 1 = an item); clapply_done()
 * releases it and completes the group. */
int clapply_take(struct clapply *a, int idx, volatile int *stop,
		struct clapply_item *out, long long now_ms);
void clapply_done(struct clapply *a, int idx, struct clapply_item *it);

/* bytes queued on thread @idx (a gauge) */
unsigned int clapply_backlog(const struct clapply *a, int idx);

#endif /* PC_CLAPPLY_H */
