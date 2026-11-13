/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* busypoll.h - opt-in per-worker busy polling (busy_poll_us): EPIOCSPARAMS on an
 * epoll worker, IORING_REGISTER_NAPI on an io_uring one (kernel 6.9+). */
#ifndef PC_BUSYPOLL_H
#define PC_BUSYPOLL_H

#include <stddef.h>

struct pc_ur;

/* @prefer takes effect only with the device's napi_defer_hard_irqs and
 * gro_flush_timeout set */
int pc_bp_epoll(int ep, unsigned int us, int prefer, char *err, size_t errsz);
int pc_bp_ring(struct pc_ur *r, unsigned int us, int prefer, char *err,
		size_t errsz);
/* for -C: would this host take it, on @io_uring's backend (0 = epoll)?
 * 0 yes, -1 no with @why */
int pc_bp_check(int io_uring, unsigned int us, int prefer, char *why,
		size_t whysz);

#endif /* PC_BUSYPOLL_H */
