/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* listenfd.c - the listener sockets: the probe for a listener already there (SO_REUSEPORT would
 * hide it), a worker's TCP socket, the unix socket with its live-or-stale check. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include "compat/dprint.h"
#include "pcaddr.h"
#include "config.h"
#include "listenfd.h"

/* ---- listeners --------------------------------------------------------- */

/* is something ALREADY listening on address:port?  Under SO_REUSEPORT a second daemon silently
 * shares the accepts, so bind once without it; only EADDRINUSE counts, the rest: generic probe */
int tcp_port_taken(const struct pc_listener *l)
{
	struct addrinfo hints, *res = NULL, *ai;
	char port[16];
	int taken = 0;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = pc_resolve_family();     /* no AAAA without IPv6 */
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	snprintf(port, sizeof port, "%d", l->port);
	if (getaddrinfo(strcmp(l->addr, "*") ? l->addr : NULL, port,
	        &hints, &res) != 0 || !res)
		return 0;                      /* the real bind will report it */
	for (ai = res; ai; ai = ai->ai_next) {
		int one = 1, fd = socket(ai->ai_family, ai->ai_socktype, 0);

		if (fd < 0)
			continue;
		/* REUSEADDR yes (TIME_WAIT is not a conflict), REUSEPORT no -
		 * omitting it is the entire point of this probe */
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
		if (bind(fd, ai->ai_addr, ai->ai_addrlen) != 0 &&
		        errno == EADDRINUSE)
			taken = 1;
		close(fd);
		if (taken)
			break;
	}
	freeaddrinfo(res);
	return taken;
}

int tcp_listener_fd(const struct pc_listener *l)
{
	struct addrinfo hints, *res = NULL, *ai;
	char port[16];
	int fd = -1, one = 1;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = pc_resolve_family();     /* no AAAA without IPv6 */
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	snprintf(port, sizeof port, "%d", l->port);

	if (getaddrinfo(strcmp(l->addr, "*") ? l->addr : NULL, port,
	        &hints, &res) != 0 || !res) {
		LM_ERR("cannot resolve listener %s:%d\n", l->addr, l->port);
		return -1;
	}
	for (ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, 0);
		if (fd < 0)
			continue;
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
		setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
		if (ai->ai_family == AF_INET6) {
			/* [::]:port takes IPv4 clients too (as ::ffff:a.b.c.d), whatever the
			 * host's bindv6only says, where the host allows it */
			int off = 0;

			setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
		}
		if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 &&
		        listen(fd, 511) == 0) {
			fcntl(fd, F_SETFL, O_NONBLOCK);
			break;
		}
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd < 0)
		LM_ERR("cannot bind listener %s:%d (%s)\n", l->addr, l->port,
			strerror(errno));
	return fd;
}

int unix_listener_fd(const char *path)
{
	struct sockaddr_un sa;
	int fd, probe;

	if (strlen(path) >= sizeof sa.sun_path) {
		LM_ERR("unix socket path too long\n");
		return -1;
	}
	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	strcpy(sa.sun_path, path);

	/* a stale socket file is normal after a crash; a LIVE one means
	 * another instance - probe before unlinking */
	probe = socket(AF_UNIX, SOCK_STREAM, 0);
	if (probe >= 0) {
		if (connect(probe, (struct sockaddr *)&sa, sizeof sa) == 0) {
			close(probe);
			LM_ERR("%s is accepting connections - another perfcached "
				"is running\n", path);
			return -1;
		}
		close(probe);
	}
	unlink(path);

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 ||
	        listen(fd, 511) < 0) {
		LM_ERR("cannot bind unix listener %s (%s)\n", path,
			strerror(errno));
		if (fd >= 0)
			close(fd);
		return -1;
	}
	fcntl(fd, F_SETFL, O_NONBLOCK);
	return fd;
}
