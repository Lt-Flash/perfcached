/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * busypoll.c - S363: opt-in busy polling, per worker (busypoll.h).  The
 * kernel ABI is spelled out here, as in uring.c, so it builds on old
 * kernel headers and musl.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/utsname.h>

#include "busypoll.h"
#include "uring.h"

#ifndef __NR_io_uring_register
#define __NR_io_uring_register 427
#endif

/* linux/eventpoll.h (6.9) */
struct bp_epoll_params {
	uint32_t busy_poll_usecs;
	uint16_t busy_poll_budget;
	uint8_t prefer_busy_poll;
	uint8_t pad;
};
#define BP_EPIOCSPARAMS  0x40088a01u   /* _IOW(0x8A, 0x01, 8 bytes) */
#define BP_BUDGET        8             /* the kernel's BUSY_POLL_BUDGET; past
                                        * 64 needs CAP_NET_ADMIN */

/* linux/io_uring.h (6.9) */
struct bp_uring_napi {
	uint32_t busy_poll_to;
	uint8_t prefer_busy_poll;
	uint8_t pad[3];
	uint64_t resv;
};
#define BP_REGISTER_NAPI 27

/* PERFCACHED_BUSYPOLL_DENY=1 makes the kernel's answer a refusal - the
 * fallback's tests, on hosts whose kernel would take it */
static int denied(char *err, size_t errsz)
{
	const char *d = getenv("PERFCACHED_BUSYPOLL_DENY");

	if (!d || *d != '1')
		return 0;
	snprintf(err, errsz, "refused by PERFCACHED_BUSYPOLL_DENY=1 in the "
		"environment (the fallback's tests)");
	return 1;
}

static const char *release(void)
{
	static char rel[65];
	struct utsname u;

	if (!rel[0])
		snprintf(rel, sizeof rel, "%s", uname(&u) == 0 ? u.release : "?");
	return rel;
}

int pc_bp_epoll(int ep, unsigned int us, int prefer, char *err, size_t errsz)
{
	struct bp_epoll_params p;

	if (denied(err, errsz))
		return -1;
	memset(&p, 0, sizeof p);
	p.busy_poll_usecs = us;
	p.busy_poll_budget = BP_BUDGET;
	p.prefer_busy_poll = prefer ? 1 : 0;
	if (ioctl(ep, BP_EPIOCSPARAMS, &p) == 0)
		return 0;
	if (errno == ENOTTY || errno == EINVAL)
		snprintf(err, errsz, "kernel %s has no per-epoll busy poll "
			"(EPIOCSPARAMS, 6.9+: %s)", release(), strerror(errno));
	else
		snprintf(err, errsz, "EPIOCSPARAMS: %s (kernel %s)",
			strerror(errno), release());
	return -1;
}

int pc_bp_ring(struct pc_ur *r, unsigned int us, int prefer, char *err,
		size_t errsz)
{
	struct bp_uring_napi n;

	if (denied(err, errsz))
		return -1;
	memset(&n, 0, sizeof n);
	n.busy_poll_to = us;
	n.prefer_busy_poll = prefer ? 1 : 0;
	if (syscall(__NR_io_uring_register, r->fd, BP_REGISTER_NAPI, &n, 1) == 0)
		return 0;
	if (errno == EINVAL)
		snprintf(err, errsz, "kernel %s has no io_uring NAPI busy poll "
			"(IORING_REGISTER_NAPI, 6.9+: %s)", release(),
			strerror(errno));
	else
		snprintf(err, errsz, "IORING_REGISTER_NAPI: %s (kernel %s)",
			strerror(errno), release());
	return -1;
}

int pc_bp_check(int io_uring, unsigned int us, int prefer, char *why,
		size_t whysz)
{
	int rc;

	if (io_uring) {
		struct pc_ur r;

		if (pc_ur_init(&r, 8, 0, why, whysz) < 0)
			return -1;
		rc = pc_bp_ring(&r, us, prefer, why, whysz);
		pc_ur_free(&r);
		return rc;
	} else {
		int ep = epoll_create1(EPOLL_CLOEXEC);

		if (ep < 0) {
			snprintf(why, whysz, "epoll_create1: %s", strerror(errno));
			return -1;
		}
		rc = pc_bp_epoll(ep, us, prefer, why, whysz);
		close(ep);
		return rc;
	}
}
