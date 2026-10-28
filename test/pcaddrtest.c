/* pcaddrtest.c - IP6-1 (DESIGN 12np): the address type (pcaddr.c).
 *
 *   A  IPv4 is what it was: pc_addr_v4 is the network-order s_addr the
 *      placement hash and the wire use, equality is address + port (as
 *      peer_addr_eq / addr_eq were), ntop prints the dotted quad
 *   B  IPv6: parse, equality (scope id included), loopback, ntop with
 *      brackets where a port follows, the %scope round trip
 *   C  the host:port parser: the config's forms (last colon, as it always
 *      split), [v6]:port, and what it refuses
 *   D  pc_addr_from_sa: either family in, a short length or another
 *      family refused */
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include "../src/pcaddr.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

int main(void)
{
	pc_addr a, b;
	struct sockaddr_in s4;
	char buf[PC_ADDR_STRLEN], host[64];
	int port = 0;

	/* A */
	memset(&s4, 0, sizeof s4);
	s4.sin_family = AF_INET;
	s4.sin_port = htons(6480);
	inet_pton(AF_INET, "192.0.2.45", &s4.sin_addr);
	CHK(pc_addr_from_sa(&a, (struct sockaddr *)&s4, sizeof s4) == 0 &&
		pc_addr_family(&a) == AF_INET && pc_addr_len(&a) == sizeof s4,
		"A: a sockaddr_in in, AF_INET, its own length");
	CHK(pc_addr_v4(&a) == s4.sin_addr.s_addr && pc_addr_port(&a) == 6480,
		"A: pc_addr_v4 is the network-order s_addr (0x%08x), the port in host order",
		(unsigned)pc_addr_v4(&a));
	pc_addr_from_v4(&b, s4.sin_addr.s_addr, 6480);
	CHK(pc_addr_eq(&a, &b) && !memcmp(&b.in4, &s4, sizeof s4),
		"A: pc_addr_from_v4 builds the same bytes a sockaddr_in held");
	pc_addr_set_port(&b, 6481);
	CHK(!pc_addr_eq(&a, &b) && pc_addr_eq_host(&a, &b),
		"A: another port: not equal, same host");
	CHK(!strcmp(pc_addr_ntop(&a, buf, sizeof buf), "192.0.2.45") &&
		!strcmp(pc_addr_ntop_port(&a, buf, sizeof buf), "192.0.2.45:6480"),
		"A: ntop '192.0.2.45', with the port '%s'", buf);
	pc_addr_from_v4(&b, htonl(0x7f000105), 1);
	CHK(pc_addr_is_loopback(&b) && !pc_addr_is_loopback(&a),
		"A: 127.0.1.5 is loopback, 192.0.2.45 is not");
	memset(&b, 0, sizeof b);
	CHK(pc_addr_is_empty(&b) && pc_addr_family(&b) == 0 &&
		pc_addr_len(&b) == 0 && pc_addr_v4(&b) == 0 &&
		!strcmp(pc_addr_ntop(&b, buf, sizeof buf), "-"),
		"A: a zeroed address is empty, no family, '-'");

	{                                      /* A: the election's order */
		pc_addr x, y;

		pc_addr_from_v4(&x, htonl(0x0a000002), 9);   /* 10.0.0.2:9 */
		pc_addr_from_v4(&y, htonl(0x09ffffff), 99);  /* 9.255.255.255:99 */
		CHK(pc_addr_cmp(&x, &y) > 0 && pc_addr_cmp(&y, &x) < 0,
			"A: cmp orders by the address as a NUMBER (10.0.0.2 > 9.255.255.255), not by the raw little-endian s_addr");
		pc_addr_from_v4(&y, htonl(0x0a000002), 10);
		CHK(pc_addr_cmp(&x, &y) < 0 && pc_addr_cmp(&x, &x) == 0,
			"A: then by port in host order; equal is 0");
	}
	{                                      /* A: the log-line strings */
		pc_addr x, y;
		char line[160];

		pc_addr_from_v4(&x, htonl(0x0a000001), 1);
		pc_addr_from_v4(&y, htonl(0x0a000002), 2);
		snprintf(line, sizeof line, "%s %s %s %s", pc_addr_str(&x),
			pc_addr_str(&y), pc_addr_str_port(&x), pc_addr_str_port(&y));
		CHK(!strcmp(line, "10.0.0.1 10.0.0.2 10.0.0.1:1 10.0.0.2:2"),
			"A: four pc_addr_str in one call stay distinct: '%s'", line);
	}

	/* B */
	CHK(pc_addr_parse("::1", 6480, &a) == 0 && pc_addr_family(&a) == AF_INET6 &&
		pc_addr_is_loopback(&a) && pc_addr_v4(&a) == 0,
		"B: '::1' parses, AF_INET6, loopback, no v4 value");
	CHK(!strcmp(pc_addr_ntop_port(&a, buf, sizeof buf), "[::1]:6480"),
		"B: ntop with the port brackets it: '%s'", buf);
	CHK(pc_addr_parse("fe80::1%7", 1, &a) == 0 && a.in6.sin6_scope_id == 7 &&
		pc_addr_parse("fe80::1", 1, &b) == 0 && !pc_addr_eq(&a, &b),
		"B: a numeric scope id is carried and compared ('fe80::1%%7' != 'fe80::1')");
	CHK(pc_addr_parse("fe80::1%lo", 1, &a) == 0 && a.in6.sin6_scope_id > 0 &&
		strstr(pc_addr_ntop(&a, buf, sizeof buf), "%lo"),
		"B: an interface scope round-trips: '%s'", buf);
	CHK(pc_addr_parse("fe80::1%no-such-if0", 1, &a) == -1 &&
		pc_addr_family(&a) == 0,
		"B: an unknown interface is refused and leaves the address empty");
	CHK(pc_addr_parse("::ffff:127.0.0.9", 1, &a) == 0 && pc_addr_is_loopback(&a),
		"B: ::ffff:127.0.0.9 is loopback");
	CHK(pc_addr_parse("10.0.0.300", 1, &a) == -1 && pc_addr_parse("host", 1, &a) == -1,
		"B: '10.0.0.300' and a name are not numeric addresses");

	/* C */
	CHK(pc_hostport_split("0.0.0.0:6479", host, sizeof host, &port) == 0 &&
		!strcmp(host, "0.0.0.0") && port == 6479,
		"C: 0.0.0.0:6479");
	CHK(pc_hostport_split("localhost:1", host, sizeof host, &port) == 0 &&
		!strcmp(host, "localhost") && port == 1,
		"C: a name: localhost:1");
	CHK(pc_hostport_split("[fe80::1%eth0]:6480", host, sizeof host, &port) == 0 &&
		!strcmp(host, "fe80::1%eth0") && port == 6480,
		"C: [fe80::1%%eth0]:6480 - the brackets removed");
	CHK(pc_hostport_split("::1:6379", host, sizeof host, &port) == 0 &&
		!strcmp(host, "::1") && port == 6379,
		"C: unbracketed, the LAST colon splits, as the config always did: '::1:6379' is ::1");
	CHK(pc_hostport_split("1.2.3.4", host, sizeof host, &port) == -1 &&
		pc_hostport_split(":80", host, sizeof host, &port) == -1 &&
		pc_hostport_split("h:", host, sizeof host, &port) == -1 &&
		pc_hostport_split("h:0", host, sizeof host, &port) == -1 &&
		pc_hostport_split("h:65536", host, sizeof host, &port) == -1 &&
		pc_hostport_split("h:80x", host, sizeof host, &port) == -1 &&
		pc_hostport_split("[::1]80", host, sizeof host, &port) == -1 &&
		pc_hostport_split("[]:80", host, sizeof host, &port) == -1 &&
		pc_hostport_split("", host, sizeof host, &port) == -1,
		"C: refused: no port, no host, empty port, 0, 65536, '80x', '[::1]80', '[]:80', ''");
	CHK(pc_hostport_split("a-very-long-host-name:1", host, 8, &port) == -1,
		"C: a host that does not fit is refused, not cut");

	/* D */
	{
		struct sockaddr_in6 s6;

		memset(&s6, 0, sizeof s6);
		s6.sin6_family = AF_INET6;
		s6.sin6_port = htons(9);
		CHK(pc_addr_from_sa(&a, (struct sockaddr *)&s6, sizeof s6) == 0 &&
			pc_addr_family(&a) == AF_INET6 && pc_addr_port(&a) == 9,
			"D: a sockaddr_in6 in");
		CHK(pc_addr_from_sa(&a, (struct sockaddr *)&s6, sizeof s4) == -1 &&
			pc_addr_family(&a) == 0,
			"D: an IPv6 sockaddr with a v4-sized length is refused");
		s6.sin6_family = AF_UNIX;
		CHK(pc_addr_from_sa(&a, (struct sockaddr *)&s6, sizeof s6) == -1,
			"D: another family is refused");
	}
	printf("pcaddrtest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
