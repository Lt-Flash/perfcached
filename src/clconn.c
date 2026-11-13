/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clconn.c - the link check's TCP probe (see clconn.h): a completed connect is the proof, closed
 * before a byte is written, so every far-side accept just reads end of stream.  Peer thread only. */
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "clconn.h"

static struct cprobe {
	int node, path, fd;
	pc_addr to;
	long long start_ms, asked_ms;
} cp[CLCONN_MAX];
static int cp_n;

static struct cprobe *cp_find(int node, int path)
{
	int i;

	for (i = 0; i < cp_n; i++)
		if (cp[i].node == node && cp[i].path == path)
			return &cp[i];
	return NULL;
}

static void cp_drop(struct cprobe *c)
{
	close(c->fd);
	*c = cp[--cp_n];
}

static struct cprobe *cp_begin(int node, int path, const pc_addr *src, const pc_addr *to,
		long long now, int *failed)
{
	pc_addr me = *src;
	struct cprobe *c;
	int fd;

	*failed = 0;
	if (cp_n >= CLCONN_MAX)
		return NULL;
	fd = socket(pc_addr_family(to), SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		*failed = 1;
		return NULL;
	}
	pc_addr_set_port(&me, 0);              /* from this node's host: the listeners gate on it */
	if (bind(fd, &me.sa, pc_addr_len(&me)) != 0 ||
	        (connect(fd, &to->sa, pc_addr_len(to)) != 0 && errno != EINPROGRESS)) {
		close(fd);
		*failed = 1;
		return NULL;
	}
	c = &cp[cp_n++];
	c->node = node;
	c->path = path;
	c->fd = fd;
	c->to = *to;
	c->start_ms = now;
	return c;
}

int clconn_probe(int node, int path, const pc_addr *src, const pc_addr *to, long long now)
{
	struct cprobe *c = cp_find(node, path);
	struct pollfd pf;
	socklen_t el = sizeof(int);
	int err = 0, failed;

	if (c && !pc_addr_eq(&c->to, to)) {   /* the peer's address or port moved */
		cp_drop(c);
		c = NULL;
	}
	if (!c && !(c = cp_begin(node, path, src, to, now, &failed)))
		return failed ? -1 : 0;
	c->asked_ms = now;
	pf.fd = c->fd;
	pf.events = POLLOUT;
	pf.revents = 0;
	if (poll(&pf, 1, 0) == 1) {
		if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0)
			err = errno ? errno : EIO;
		cp_drop(c);
		return err ? -1 : 1;
	}
	if (now - c->start_ms >= CLCONN_TIMEOUT_MS) {
		cp_drop(c);
		return -1;
	}
	return 0;
}

void clconn_reap(long long since)
{
	int i;

	for (i = cp_n - 1; i >= 0; i--)
		if (cp[i].asked_ms < since)
			cp_drop(&cp[i]);
}

int clconn_open(void)
{
	return cp_n;
}
