/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* rpcq.h - a thread's small RPC queue: put under the owner's mutex by any thread, taken by the
 * owner; a header of its own so test/rpcqtest.c checks it without linking the daemon. */
#ifndef PC_RPCQ_H
#define PC_RPCQ_H

#define PC_RPCQ 8                      /* a power of two */

typedef void (*pc_rpcq_fn)(int sender, void *param);

struct pc_rpcq {
	pc_rpcq_fn fn[PC_RPCQ];
	void *arg[PC_RPCQ];
	unsigned int head, tail;        /* free-running: PC_RPCQ divides 2^32 */
};

/* 1 queued, 0 full (the RPC is dropped) */
static inline int pc_rpcq_put(struct pc_rpcq *q, pc_rpcq_fn fn, void *arg)
{
	if (q->tail - q->head >= PC_RPCQ)
		return 0;
	q->fn[q->tail % PC_RPCQ] = fn;
	q->arg[q->tail % PC_RPCQ] = arg;
	q->tail++;
	return 1;
}

/* 1 taken into *fn / *arg, 0 empty */
static inline int pc_rpcq_take(struct pc_rpcq *q, pc_rpcq_fn *fn, void **arg)
{
	if (q->head == q->tail)
		return 0;
	*fn = q->fn[q->head % PC_RPCQ];
	*arg = q->arg[q->head % PC_RPCQ];
	q->head++;
	return 1;
}

#endif /* PC_RPCQ_H */
