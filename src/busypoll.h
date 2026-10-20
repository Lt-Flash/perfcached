/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * busypoll.h - S363 (0.6.0): opt-in busy polling, per worker.
 *
 * [daemon] busy_poll_us = N (0, the default, is off) asks the kernel to
 * spin on the network device's queue for up to N microseconds before a
 * worker's wait sleeps.  What it buys perfcached is not measured yet (the
 * suites run on loopback, where it never engages); what it costs is CPU
 * that spins (felt on shared VMs).  busy_poll_prefer = yes also asks it to prefer
 * the busy poll over the device's interrupts, which takes effect only with
 * the device's napi_defer_hard_irqs and gro_flush_timeout set.
 *
 *   an epoll worker:    EPIOCSPARAMS on its epoll (kernel 6.9+)
 *   an io_uring worker: IORING_REGISTER_NAPI on its ring (kernel 6.9+)
 *
 * Both are per worker, not system-wide (net.core.busy_poll is).  A kernel
 * that refuses leaves the worker running without it, and says why.
 */
#ifndef PC_BUSYPOLL_H
#define PC_BUSYPOLL_H

#include <stddef.h>

struct pc_ur;

int pc_bp_epoll(int ep, unsigned int us, int prefer, char *err, size_t errsz);
int pc_bp_ring(struct pc_ur *r, unsigned int us, int prefer, char *err,
		size_t errsz);
/* for -C: would this host take it, on @io_uring's backend (0 = epoll)?
 * 0 yes, -1 no with @why */
int pc_bp_check(int io_uring, unsigned int us, int prefer, char *why,
		size_t whysz);

#endif /* PC_BUSYPOLL_H */
