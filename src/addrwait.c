/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * addrwait.c - S294: wait for a configured local address.  See addrwait.h.
 */
#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "compat/dprint.h"
#include "addrwait.h"

/* bind a throwaway UDP socket to @addr, port 0: the cheapest question
 * the kernel answers with exactly "is this address configured here" */
static int addr_present(const struct sockaddr *sa, socklen_t sl)
{
	int fd = socket(sa->sa_family, SOCK_DGRAM, 0), rc, err;

	if (fd < 0)
		return 1;                      /* cannot ask: let the caller try */
	rc = bind(fd, sa, sl);
	err = errno;
	close(fd);
	if (rc == 0)
		return 1;
	return err == EADDRNOTAVAIL ? 0 : 1;   /* anything else is not ours */
}

static long long mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int pc_addr_wait(const char *addr, int wait_s, const char *what)
{
	struct sockaddr_in s4;
	struct sockaddr_in6 s6;
	const struct sockaddr *sa;
	socklen_t sl;
	long long t0, waited;

	if (!addr || !*addr || !strcmp(addr, "*"))
		return 0;
	memset(&s4, 0, sizeof s4);
	memset(&s6, 0, sizeof s6);
	if (inet_pton(AF_INET, addr, &s4.sin_addr) == 1) {
		if (s4.sin_addr.s_addr == htonl(INADDR_ANY))
			return 0;
		s4.sin_family = AF_INET;
		sa = (const struct sockaddr *)&s4;
		sl = sizeof s4;
	} else if (inet_pton(AF_INET6, addr, &s6.sin6_addr) == 1) {
		if (IN6_IS_ADDR_UNSPECIFIED(&s6.sin6_addr))
			return 0;
		s6.sin6_family = AF_INET6;
		sa = (const struct sockaddr *)&s6;
		sl = sizeof s6;
	} else {
		return 0;                      /* a name: getaddrinfo's business */
	}
	if (addr_present(sa, sl))
		return 0;
	if (wait_s <= 0) {
		LM_ERR("%s: %s is not configured on this host "
			"(address_wait_s = 0, not waiting)\n", what, addr);
		return -1;
	}
	LM_NOTICE("%s: waiting up to %d s for %s to appear on this host - "
		"started before the network was up\n", what, wait_s, addr);
	t0 = mono_ms();
	for (;;) {
		usleep(250 * 1000);
		waited = mono_ms() - t0;
		if (addr_present(sa, sl)) {
			LM_NOTICE("%s: %s appeared after %lld ms\n", what, addr,
				waited);
			return 0;
		}
		if (waited >= (long long)wait_s * 1000)
			break;
	}
	LM_ERR("%s: %s did not appear on this host within %d s - is it this "
		"host's address? ([daemon] address_wait_s)\n", what, addr, wait_s);
	return -1;
}
