/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov
 * Part of libperfd - see lib/LICENSE.  This file must stay
 * free of src/core includes; tools/sync-libperfd.sh exports
 * exactly the MIT set to consumers. */
/*
 * pcaddr.h - IP6-1 (DESIGN 12np): one address type for every socket the
 * daemon, the tools and libperfd open, so that IPv6 is a family, not a
 * rewrite.  pc_addr is a sockaddr of either family with its length implied
 * by the family; the helpers compare, print and parse it the same way
 * everywhere.
 *
 * Nothing here changes behaviour for IPv4: pc_addr_v4() is the network-order
 * s_addr the placement hash (pc_hrw_mix) and the wire (clmemb's four raw
 * bytes) have always used, and equality is address and port, as
 * peer_addr_eq() and addr_eq() were.  The scope id of an IPv6 link-local
 * address is carried and compared (IP6-4 replies to such a sender need it).
 *
 * The one host:port parser: "a.b.c.d:port", "[v6]:port", "name:port" (the
 * host as written - resolution stays with the caller, getaddrinfo or
 * pc_addr_parse).  Without brackets the port is after the LAST colon, as
 * the config has always split it ("::1:6379" is ::1 port 6379); brackets
 * are the unambiguous form and what pc_addr_ntop_port writes.
 */
#ifndef PC_ADDR_H
#define PC_ADDR_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>

typedef union pc_addr {
	struct sockaddr sa;
	struct sockaddr_in in4;
	struct sockaddr_in6 in6;
} pc_addr;

/* "[" + the longest IPv6 text + "%" + an interface name + "]:65535" */
#define PC_ADDR_STRLEN (INET6_ADDRSTRLEN + 1 + 16 + 8)

/* AF_INET, AF_INET6, or 0 for an empty (zeroed) address */
int pc_addr_family(const pc_addr *a);
/* the length a socket call takes for @a's family (0 when empty) */
socklen_t pc_addr_len(const pc_addr *a);
/* the port in host order (0 when empty) */
uint16_t pc_addr_port(const pc_addr *a);
void pc_addr_set_port(pc_addr *a, uint16_t port);

/* same family, address, port (and IPv6 scope id) */
int pc_addr_eq(const pc_addr *a, const pc_addr *b);
/* same family and address (and IPv6 scope id), any port */
int pc_addr_eq_host(const pc_addr *a, const pc_addr *b);
/* a total order: by family, then the address as a number (IPv4 in host
 * order, IPv6 bytewise), then the scope id, then the port - for IPv4 exactly
 * the (ip, port) order the cluster's election has always used */
int pc_addr_cmp(const pc_addr *a, const pc_addr *b);
/* 1 when nothing is set (the family is 0, or the address and port are) */
int pc_addr_is_empty(const pc_addr *a);
/* 127.0.0.0/8 or ::1 (and ::ffff:127.x) */
int pc_addr_is_loopback(const pc_addr *a);

/* IPv4: the network-order s_addr; 0 for any other family */
uint32_t pc_addr_v4(const pc_addr *a);
/* an IPv4 address from a network-order s_addr and a host-order port */
void pc_addr_from_v4(pc_addr *a, uint32_t s_addr_net, uint16_t port);
/* copy in from / out to a raw sockaddr of either family (accept, recvfrom);
 * pc_addr_from_sa answers -1 for another family or a short length */
int pc_addr_from_sa(pc_addr *a, const struct sockaddr *sa, socklen_t len);

/* the address alone: "10.0.0.1", "::1", "fe80::1%eth0"; "-" when empty */
const char *pc_addr_ntop(const pc_addr *a, char *buf, size_t cap);
/* with the port: "10.0.0.1:6480", "[::1]:6480" */
const char *pc_addr_ntop_port(const pc_addr *a, char *buf, size_t cap);

/* for log lines: pc_addr_ntop / pc_addr_ntop_port into a thread-local
 * buffer, one of PC_ADDR_STR_RING used in turn - so up to that many may
 * appear in one call (inet_ntoa's single static buffer allowed one) */
#define PC_ADDR_STR_RING 4
const char *pc_addr_str(const pc_addr *a);
const char *pc_addr_str_port(const pc_addr *a);

/* a NUMERIC address (IPv4 dotted quad, or IPv6 with an optional %scope)
 * and a host-order port into @out; -1 when @host is not one */
int pc_addr_parse(const char *host, uint16_t port, pc_addr *out);

/* split "host:port" / "[v6]:port" into @host (brackets removed) and @port
 * (1..65535); -1 when malformed, the host does not fit, or the port is not
 * a whole decimal number in range */
int pc_hostport_split(const char *s, char *host, size_t hcap, int *port);

/* 12np, the IPv6 guard: can this host open an IPv6 socket?  A FUNCTIONAL
 * check, never a kernel version or a sysctl alone: socket(AF_INET6,
 * SOCK_DGRAM) and a bind to [::1]:0, once per process.  It fails under
 * ipv6.disable=1 (no socket) and under disable_ipv6 = 1 (no ::1 to bind);
 * a container without IPv6 is either.  1 usable; 0 not, *@why (may be
 * NULL) saying which */
int pc_ipv6_usable(const char **why);

/* 12np: the family every resolver call asks for - AF_INET when IPv6 is not
 * usable (only the A query goes out: no AAAA from an IPv6-less host),
 * AF_UNSPEC when it is.  Not AI_ADDRCONFIG: glibc's ignores loopback, so a
 * host or namespace with only lo would resolve nothing at all */
int pc_resolve_family(void);

/* 12np IP6-2: is @a in @net/@bits of @family (@net network order, IPv4 in
 * its first 4 bytes)?  An IPv4 client of a dual-stack listener arrives as
 * ::ffff:a.b.c.d and is matched against an IPv4 prefix as the IPv4
 * address it is.  1 yes, 0 no (another family, or @bits out of range) */
int pc_addr_in_prefix(const pc_addr *a, int family, const unsigned char *net,
		int bits);

#endif
