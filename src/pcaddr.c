/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov
 * Part of libperfd - see lib/LICENSE.  This file must stay
 * free of src/core includes; tools/sync-libperfd.sh exports
 * exactly the MIT set to consumers. */
/*
 * pcaddr.c - IP6-1 (DESIGN 12np): the address type.  See pcaddr.h.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <unistd.h>

#include "pcaddr.h"

int pc_addr_family(const pc_addr *a)
{
	if (a->sa.sa_family == AF_INET || a->sa.sa_family == AF_INET6)
		return a->sa.sa_family;
	return 0;
}

socklen_t pc_addr_len(const pc_addr *a)
{
	switch (pc_addr_family(a)) {
	case AF_INET:
		return (socklen_t)sizeof a->in4;
	case AF_INET6:
		return (socklen_t)sizeof a->in6;
	}
	return 0;
}

uint16_t pc_addr_port(const pc_addr *a)
{
	switch (pc_addr_family(a)) {
	case AF_INET:
		return ntohs(a->in4.sin_port);
	case AF_INET6:
		return ntohs(a->in6.sin6_port);
	}
	return 0;
}

void pc_addr_set_port(pc_addr *a, uint16_t port)
{
	switch (pc_addr_family(a)) {
	case AF_INET:
		a->in4.sin_port = htons(port);
		break;
	case AF_INET6:
		a->in6.sin6_port = htons(port);
		break;
	}
}

int pc_addr_eq_host(const pc_addr *a, const pc_addr *b)
{
	int f = pc_addr_family(a);

	if (f != pc_addr_family(b))
		return 0;
	switch (f) {
	case AF_INET:
		return a->in4.sin_addr.s_addr == b->in4.sin_addr.s_addr;
	case AF_INET6:
		return !memcmp(&a->in6.sin6_addr, &b->in6.sin6_addr,
			sizeof a->in6.sin6_addr) &&
			a->in6.sin6_scope_id == b->in6.sin6_scope_id;
	}
	return 1;                              /* both empty */
}

int pc_addr_eq(const pc_addr *a, const pc_addr *b)
{
	return pc_addr_eq_host(a, b) && pc_addr_port(a) == pc_addr_port(b);
}

int pc_addr_cmp(const pc_addr *a, const pc_addr *b)
{
	int fa = pc_addr_family(a), fb = pc_addr_family(b), c;

	if (fa != fb)
		return fa < fb ? -1 : 1;
	if (fa == AF_INET) {
		uint32_t ia = ntohl(a->in4.sin_addr.s_addr);
		uint32_t ib = ntohl(b->in4.sin_addr.s_addr);

		if (ia != ib)
			return ia < ib ? -1 : 1;
	} else if (fa == AF_INET6) {
		c = memcmp(&a->in6.sin6_addr, &b->in6.sin6_addr,
			sizeof a->in6.sin6_addr);
		if (c)
			return c < 0 ? -1 : 1;
		if (a->in6.sin6_scope_id != b->in6.sin6_scope_id)
			return a->in6.sin6_scope_id < b->in6.sin6_scope_id ? -1 : 1;
	}
	if (pc_addr_port(a) != pc_addr_port(b))
		return pc_addr_port(a) < pc_addr_port(b) ? -1 : 1;
	return 0;
}

int pc_addr_is_empty(const pc_addr *a)
{
	static const struct in6_addr any6 = IN6ADDR_ANY_INIT;

	switch (pc_addr_family(a)) {
	case AF_INET:
		return !a->in4.sin_addr.s_addr && !a->in4.sin_port;
	case AF_INET6:
		return !memcmp(&a->in6.sin6_addr, &any6, sizeof any6) &&
			!a->in6.sin6_port;
	}
	return 1;
}

int pc_addr_is_loopback(const pc_addr *a)
{
	switch (pc_addr_family(a)) {
	case AF_INET:
		return (ntohl(a->in4.sin_addr.s_addr) >> 24) == 127;
	case AF_INET6:
		if (IN6_IS_ADDR_LOOPBACK(&a->in6.sin6_addr))
			return 1;
		return IN6_IS_ADDR_V4MAPPED(&a->in6.sin6_addr) &&
			a->in6.sin6_addr.s6_addr[12] == 127;
	}
	return 0;
}

uint32_t pc_addr_v4(const pc_addr *a)
{
	return pc_addr_family(a) == AF_INET ? a->in4.sin_addr.s_addr : 0;
}

void pc_addr_from_v4(pc_addr *a, uint32_t s_addr_net, uint16_t port)
{
	memset(a, 0, sizeof *a);
	a->in4.sin_family = AF_INET;
	a->in4.sin_addr.s_addr = s_addr_net;
	a->in4.sin_port = htons(port);
}

int pc_addr_from_sa(pc_addr *a, const struct sockaddr *sa, socklen_t len)
{
	memset(a, 0, sizeof *a);
	if (sa->sa_family == AF_INET && len >= (socklen_t)sizeof a->in4) {
		memcpy(&a->in4, sa, sizeof a->in4);
		return 0;
	}
	if (sa->sa_family == AF_INET6 && len >= (socklen_t)sizeof a->in6) {
		memcpy(&a->in6, sa, sizeof a->in6);
		return 0;
	}
	return -1;
}

const char *pc_addr_ntop(const pc_addr *a, char *buf, size_t cap)
{
	char t[INET6_ADDRSTRLEN];

	if (!cap)
		return buf;
	switch (pc_addr_family(a)) {
	case AF_INET:
		if (!inet_ntop(AF_INET, &a->in4.sin_addr, t, sizeof t))
			break;
		snprintf(buf, cap, "%s", t);
		return buf;
	case AF_INET6:
		if (!inet_ntop(AF_INET6, &a->in6.sin6_addr, t, sizeof t))
			break;
		if (a->in6.sin6_scope_id) {
			char ifn[IF_NAMESIZE];

			if (if_indextoname(a->in6.sin6_scope_id, ifn))
				snprintf(buf, cap, "%s%%%s", t, ifn);
			else
				snprintf(buf, cap, "%s%%%u", t,
					(unsigned)a->in6.sin6_scope_id);
		} else
			snprintf(buf, cap, "%s", t);
		return buf;
	}
	snprintf(buf, cap, "-");
	return buf;
}

const char *pc_addr_ntop_port(const pc_addr *a, char *buf, size_t cap)
{
	char h[PC_ADDR_STRLEN];

	pc_addr_ntop(a, h, sizeof h);
	if (pc_addr_family(a) == AF_INET6)
		snprintf(buf, cap, "[%s]:%u", h, (unsigned)pc_addr_port(a));
	else if (pc_addr_family(a) == AF_INET)
		snprintf(buf, cap, "%s:%u", h, (unsigned)pc_addr_port(a));
	else
		snprintf(buf, cap, "-");
	return buf;
}

static __thread char str_ring[PC_ADDR_STR_RING][PC_ADDR_STRLEN];
static __thread unsigned str_next;

const char *pc_addr_str(const pc_addr *a)
{
	char *b = str_ring[str_next++ % PC_ADDR_STR_RING];

	return pc_addr_ntop(a, b, PC_ADDR_STRLEN);
}

const char *pc_addr_str_port(const pc_addr *a)
{
	char *b = str_ring[str_next++ % PC_ADDR_STR_RING];

	return pc_addr_ntop_port(a, b, PC_ADDR_STRLEN);
}

int pc_addr_parse(const char *host, uint16_t port, pc_addr *out)
{
	char h[INET6_ADDRSTRLEN + 1 + IF_NAMESIZE];
	const char *pct;

	memset(out, 0, sizeof *out);
	if (inet_pton(AF_INET, host, &out->in4.sin_addr) == 1) {
		out->in4.sin_family = AF_INET;
		out->in4.sin_port = htons(port);
		return 0;
	}
	if (strlen(host) >= sizeof h)
		return -1;
	strcpy(h, host);
	pct = strchr(h, '%');
	if (pct)
		h[pct - h] = 0;
	if (inet_pton(AF_INET6, h, &out->in6.sin6_addr) != 1) {
		memset(out, 0, sizeof *out);
		return -1;
	}
	out->in6.sin6_family = AF_INET6;
	out->in6.sin6_port = htons(port);
	if (pct) {
		const char *sc = host + (pct - h) + 1;
		char *end;
		unsigned long n;

		if (!*sc)
			goto bad;
		errno = 0;
		n = strtoul(sc, &end, 10);
		if (!*end && !errno && n > 0 && n <= 0xffffffffUL)
			out->in6.sin6_scope_id = (uint32_t)n;
		else if (!(out->in6.sin6_scope_id = if_nametoindex(sc)))
			goto bad;
	}
	return 0;
bad:
	memset(out, 0, sizeof *out);
	return -1;
}

int pc_hostport_split(const char *s, char *host, size_t hcap, int *port)
{
	const char *colon, *p;
	size_t n;
	long v = 0;

	if (!s || !*s)
		return -1;
	if (*s == '[') {
		const char *rb = strchr(s, ']');

		if (!rb || rb[1] != ':')
			return -1;
		n = (size_t)(rb - s - 1);
		if (n == 0 || n >= hcap)
			return -1;
		memcpy(host, s + 1, n);
		host[n] = 0;
		colon = rb + 1;
	} else {
		colon = strrchr(s, ':');
		if (!colon || colon == s)
			return -1;
		n = (size_t)(colon - s);       /* the LAST colon, as the config
		                                * always split: "::1:6379" is ::1 */
		if (n >= hcap)
			return -1;
		memcpy(host, s, n);
		host[n] = 0;
	}
	p = colon + 1;
	if (!*p)
		return -1;
	for (; *p; p++) {
		if (*p < '0' || *p > '9')
			return -1;
		v = v * 10 + (*p - '0');
		if (v > 65535)
			return -1;
	}
	if (v < 1)
		return -1;
	*port = (int)v;
	return 0;
}

/* 12np: 0 not looked yet, 1 usable, 2 not - the first answer is kept (two
 * threads asking at once both look, and agree) */
static int v6_state;
static const char *v6_why = "";

int pc_ipv6_usable(const char **why)
{
	int st = __atomic_load_n(&v6_state, __ATOMIC_ACQUIRE);

	if (!st) {
		struct sockaddr_in6 a;
		const char *w = "";
		int fd = socket(AF_INET6, SOCK_DGRAM, 0);

		if (fd < 0)
			w = "no IPv6 socket - the kernel has none (ipv6.disable=1, "
				"or a container without it)";
		else {
			memset(&a, 0, sizeof a);
			a.sin6_family = AF_INET6;
			a.sin6_addr = in6addr_loopback;
			if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0)
				w = "no ::1 to bind - IPv6 is disabled (disable_ipv6 = 1)";
			close(fd);
		}
		v6_why = w;
		st = *w ? 2 : 1;
		__atomic_store_n(&v6_state, st, __ATOMIC_RELEASE);
	}
	if (why)
		*why = st == 1 ? "" : v6_why;
	return st == 1;
}

int pc_resolve_family(void)
{
	return pc_ipv6_usable(NULL) ? AF_UNSPEC : AF_INET;
}

int pc_addr_in_prefix(const pc_addr *a, int family, const unsigned char *net,
		int bits)
{
	const unsigned char *b;
	int n, i;

	switch (pc_addr_family(a)) {
	case AF_INET:
		if (family != AF_INET)
			return 0;
		b = (const unsigned char *)&a->in4.sin_addr;
		n = 32;
		break;
	case AF_INET6:
		if (family == AF_INET &&
		    IN6_IS_ADDR_V4MAPPED(&a->in6.sin6_addr)) {
			b = a->in6.sin6_addr.s6_addr + 12;
			n = 32;
		} else if (family == AF_INET6) {
			b = a->in6.sin6_addr.s6_addr;
			n = 128;
		} else
			return 0;
		break;
	default:
		return 0;
	}
	if (bits < 0 || bits > n)
		return 0;
	for (i = 0; i < bits / 8; i++)
		if (b[i] != net[i])
			return 0;
	if (bits % 8) {
		unsigned char m = (unsigned char)(0xFFu << (8 - bits % 8));

		if ((b[i] & m) != (net[i] & m))
			return 0;
	}
	return 1;
}
