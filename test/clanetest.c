/* clanetest.c - the frame lane's inbound side (clane.c), CH-18: what a
 * connection may hold before it has shown it is a lane.  The real lane
 * thread on 127.0.0.1 at a port the kernel picked; clients from
 * 127.0.0.1, .2 and .3, so the per-address limit can be met on one and
 * not the others.  About ten seconds: the hello's deadline is real time.
 *
 *   A  four connections from one address are held; a fifth is closed at
 *      once, counted as refused
 *   B  a lane from another address - a good hello, then a frame: the
 *      frame reaches the cluster thread's queue with the sender's cluster
 *      address from the hello
 *   C  the four that never sent a hello are closed once the deadline
 *      (3 s) passes, counted as timeouts; the lane that did is still open
 *   D  a hello with the wrong magic is closed, counted as refused
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "compat/dprint.h"
#include "pcaddr.h"
#include "clane.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* ---- the stub: the log -------------------------------------------------- */
int compat_log_level = L_WARN;

void compat_log(int level, const char *tag, const char *fmt, ...)
{
	va_list ap;

	(void)level; (void)tag;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

/* ---- the harness -------------------------------------------------------- */
static int port;

/* a connection to the lane from 127.0.0.@host */
static int dial(int host)
{
	struct sockaddr_in me, to;
	int fd = socket(AF_INET, SOCK_STREAM, 0);

	if (fd < 0)
		return -1;
	memset(&me, 0, sizeof me);
	me.sin_family = AF_INET;
	me.sin_addr.s_addr = htonl(0x7f000000u | (unsigned)host);
	memset(&to, 0, sizeof to);
	to.sin_family = AF_INET;
	to.sin_addr.s_addr = htonl(0x7f000001u);
	to.sin_port = htons((uint16_t)port);
	if (bind(fd, (struct sockaddr *)&me, sizeof me) != 0 ||
	    connect(fd, (struct sockaddr *)&to, sizeof to) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* 1 when the lane closed @fd within @ms, 0 when it is still open */
static int closed_within(int fd, int ms)
{
	struct pollfd p = { fd, POLLIN, 0 };
	char c;

	if (poll(&p, 1, ms) <= 0)
		return 0;
	return recv(fd, &c, 1, MSG_DONTWAIT) <= 0;
}

static struct clane_figs figs(void)
{
	struct clane_figs f;

	clane_figures(&f);
	return f;
}

/* the hello a peer at 127.0.0.@host, cluster port @cport, sends */
static int hello(int fd, int host, uint16_t cport, const char *magic)
{
	unsigned char h[10];
	uint32_t a = htonl(0x7f000000u | (unsigned)host);
	uint16_t p = htons(cport);

	memcpy(h, magic, 4);
	memcpy(h + 4, &a, 4);
	memcpy(h + 8, &p, 2);
	return send(fd, h, sizeof h, 0) == (ssize_t)sizeof h ? 0 : -1;
}

int main(void)
{
	struct sockaddr_in sa;
	socklen_t sl = sizeof sa;
	pc_addr self, from;
	int a[4], fd5, lane, bad, i, n, still;
	unsigned char buf[64];

	/* a free port, then the lane on it (lane_off 0: the lane port is the
	 * cluster port here) */
	{
		int s = socket(AF_INET, SOCK_STREAM, 0);

		memset(&sa, 0, sizeof sa);
		sa.sin_family = AF_INET;
		sa.sin_addr.s_addr = htonl(0x7f000001u);
		if (s < 0 || bind(s, (struct sockaddr *)&sa, sizeof sa) != 0 ||
		    getsockname(s, (struct sockaddr *)&sa, &sl) != 0)
			return 2;
		port = ntohs(sa.sin_port);
		close(s);
	}
	if (pc_addr_parse("127.0.0.1", (uint16_t)port, &self) != 0 || clane_start(&self, 0) != 0)
		return 2;

	/* A */
	for (n = 0, i = 0; i < 4; i++)
		n += (a[i] = dial(1)) >= 0;
	usleep(300 * 1000);
	for (still = 0, i = 0; i < 4; i++)
		still += a[i] >= 0 && !closed_within(a[i], 0);
	CHK(n == 4 && still == 4, "A: four connections from 127.0.0.1 - held");
	fd5 = dial(1);
	CHK(fd5 >= 0 && closed_within(fd5, 2000) && figs().refused == 1,
		"A: a fifth from the same address - closed, refused (%llu)", figs().refused);

	/* B */
	lane = dial(2);
	n = lane >= 0 && hello(lane, 2, 7777, "PCL1") == 0;
	{
		unsigned char fr[4 + 5] = { 0, 0, 0, 5, 'f', 'r', 'a', 'm', 'e' };

		n = n && send(lane, fr, sizeof fr, 0) == (ssize_t)sizeof fr;
	}
	{
		struct pollfd p = { clane_rx_fd(), POLLIN, 0 };
		size_t got = 0;

		if (n && poll(&p, 1, 2000) == 1)
			got = clane_rx_take(buf, sizeof buf, &from);
		CHK(got == 5 && !memcmp(buf, "frame", 5) && pc_addr_port(&from) == 7777,
			"B: a lane from 127.0.0.2 - its frame queued, from cluster port %d", pc_addr_port(&from));
	}

	/* C */
	for (n = 0, i = 0; i < 4; i++)
		n += closed_within(a[i], 6000);
	CHK(n == 4 && figs().inbound_timeouts == 4,
		"C: the four with no hello - closed at the deadline, timed out (%llu)", figs().inbound_timeouts);
	CHK(!closed_within(lane, 0), "C: the lane with a good hello - still open past it");

	/* D */
	bad = dial(3);
	CHK(bad >= 0 && hello(bad, 3, 7777, "XCL1") == 0 && closed_within(bad, 2000) && figs().refused == 2,
		"D: a hello with the wrong magic - closed, refused (%llu)", figs().refused);

	for (i = 0; i < 4; i++)
		if (a[i] >= 0)
			close(a[i]);
	close(fd5);
	close(lane);
	close(bad);
	printf("clanetest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
