/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov
 * Part of libperfd - see lib/LICENSE.  This file must stay
 * free of src/core includes; tools/sync-libperfd.sh exports
 * exactly the MIT set to consumers. */
/* pcaddr.h - one address type (either family) for every socket, so IPv6 is a family and
 * not a rewrite; IPv4's network-order s_addr and (address, port) equality are unchanged. */
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

/* the longest IPv6 text + "%" + an interface name; "[" + that + "]:65535" */
#define PC_ADDR_HOSTLEN (INET6_ADDRSTRLEN + 1 + 16)
#define PC_ADDR_STRLEN (PC_ADDR_HOSTLEN + 8)

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
/* a total order: family, the address as a number (IPv4 host order, IPv6 bytewise), scope id,
 * port - for IPv4 the (ip, port) order the cluster's election has always used */
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

/* for log lines: into a thread-local ring of PC_ADDR_STR_RING buffers, so up to that
 * many may appear in one call */
#define PC_ADDR_STR_RING 4
const char *pc_addr_str(const pc_addr *a);
const char *pc_addr_str_port(const pc_addr *a);

/* a NUMERIC address (IPv4 dotted quad, or IPv6 with an optional %scope)
 * and a host-order port into @out; -1 when @host is not one */
int pc_addr_parse(const char *host, uint16_t port, pc_addr *out);

/* split "host:port" / "[v6]:port"; unbracketed, the port follows the LAST colon ("::1:6379" is
 * ::1 port 6379).  -1 when malformed, the host does not fit, or the port is not 1..65535 */
int pc_hostport_split(const char *s, char *host, size_t hcap, int *port);

/* the IPv6 guard, FUNCTIONAL: socket(AF_INET6) and a bind to [::1]:0, once per process (fails
 * under ipv6.disable=1 and disable_ipv6 = 1).  1 usable; 0 not, *@why (may be NULL) says which */
int pc_ipv6_usable(const char **why);

/* the family every resolver call asks for: AF_INET when IPv6 is unusable (no AAAA query), else
 * AF_UNSPEC; not AI_ADDRCONFIG, whose glibc version ignores loopback */
int pc_resolve_family(void);

/* is @a in @net/@bits of @family (@net network order, IPv4 in its first 4 bytes)?  A ::ffff:
 * client matches an IPv4 prefix as the IPv4 address it is.  1 yes, 0 no. */
int pc_addr_in_prefix(const pc_addr *a, int family, const unsigned char *net,
		int bits);

#endif
