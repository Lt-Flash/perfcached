/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clsend.c - the cluster plane's send path (M14).  See clsend.h.
 */
#include <sys/socket.h>

#include "clsend.h"
#include "clpeers.h"
#include "clwire.h"

static int send_fd = -1;
static const uint8_t *send_psk;
static clsend_xmit_fn send_xmit;
static clsend_lane_fn lane_fn;                 /* S36 step b */
static clsend_forced_fn lane_forced;           /* S36 e2 */
static size_t lane_min;

void clsend_set_lane(clsend_lane_fn lane, size_t min, clsend_forced_fn forced)
{
	lane_min = min;
	lane_forced = forced;
	lane_fn = lane;
}

static int by_lane(const struct sockaddr_in *to, size_t len)
{
	return lane_fn && (len > lane_min || (lane_forced && lane_forced(to)));
}

int clsend_lanes(size_t n)
{
	return lane_fn && n > lane_min;
}

static long xmit_sendto(int fd, const void *buf, size_t n,
		const struct sockaddr_in *to)
{
	return (long)sendto(fd, buf, n, 0, (const struct sockaddr *)to,
		sizeof *to);
}

void clsend_init(int fd, const uint8_t *psk, clsend_xmit_fn xmit)
{
	send_fd = fd;
	send_psk = psk;
	send_xmit = xmit ? xmit : xmit_sendto;
}

int clsend_seal(const struct sockaddr_in *to, const unsigned char *pt,
		size_t n)
{
	unsigned char buf[HDR_LEN + MAX_DGRAM + 16];
	size_t len = clwire_seal(send_psk, pt, n, buf);

	if (!len)
		return -1;
	if (by_lane(to, len))
		return lane_fn(to, buf, len);
	return send_xmit(send_fd, buf, len, to) < 0 ? -1 : 0;
}

int clsend_seal_udp(const struct sockaddr_in *to, const unsigned char *pt,
		size_t n)
{
	unsigned char buf[HDR_LEN + MAX_DGRAM + 16];
	size_t len = clwire_seal(send_psk, pt, n, buf);

	if (!len)
		return -1;
	return send_xmit(send_fd, buf, len, to) < 0 ? -1 : 0;
}

int clsend_to_node(struct clpeers *pt, int node, const unsigned char *pt_,
		size_t n, long long now)
{
	struct peer *p = clpeers_by_id_live(pt, node, now);

	return p ? clsend_seal(&p->addr, pt_, n) : -1;
}

int clsend_live(struct clpeers *pt, const unsigned char *msg, size_t n,
		long long now)
{
	int i, sent = 0;

	for (i = 0; i < pt->n_peers; i++)
		if (clpeers_live(&pt->peers[i], now) &&
		    clsend_seal(&pt->peers[i].addr, msg, n) == 0)
			sent++;
	return sent;
}

size_t clsend_wrap(const unsigned char *pt, size_t n, unsigned char *out)
{
	return clwire_seal(send_psk, pt, n, out);
}

long clsend_raw(const struct sockaddr_in *to, const void *buf, size_t n)
{
	if (by_lane(to, n))
		return lane_fn(to, buf, n) == 0 ? (long)n : -1;
	return send_xmit(send_fd, buf, n, to);
}

int clsend_open(const unsigned char *buf, size_t n, unsigned char *pt,
		unsigned long long *ptlen)
{
	return clwire_open(send_psk, buf, n, pt, ptlen);
}
