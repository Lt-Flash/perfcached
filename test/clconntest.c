/* clconntest.c - the TCP probe against loopback: a listener takes it from the asked source and
 * reads EOF at once, nothing left open; a closed port gives -1, and the next call probes again. */
#include <arpa/inet.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "clconn.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static pc_addr loop(int last, int port)
{
	pc_addr a;

	memset(&a, 0, sizeof a);
	pc_addr_from_v4(&a, htonl(0x7f000000u | (unsigned)last), (uint16_t)port);
	return a;
}

/* a listening socket on 127.0.0.1 at a port the kernel picks; *port says which */
static int listener(int *port)
{
	pc_addr a = loop(1, 0);
	socklen_t al = sizeof a;
	int fd = socket(AF_INET, SOCK_STREAM, 0);

	if (fd < 0 || bind(fd, &a.sa, pc_addr_len(&a)) != 0 || listen(fd, 8) != 0 ||
	        getsockname(fd, &a.sa, &al) != 0)
		return -1;
	*port = pc_addr_port(&a);
	return fd;
}

/* call until it is not in flight, 1 ms apart, at most 2 s */
static int settle(int node, int path, const pc_addr *src, const pc_addr *to, long long *now)
{
	struct timespec ms = { 0, 1000000 };
	int i, r = 0;

	for (i = 0; i < 2000 && (r = clconn_probe(node, path, src, to, *now)) == 0; i++) {
		nanosleep(&ms, NULL);
		(*now)++;
	}
	return r;
}

int main(void)
{
	pc_addr src = loop(2, 6480), to, from;
	socklen_t fl = sizeof from;
	long long now = 1000;
	struct pollfd pf;
	char b[8];
	int lfd, port = 0, cfd, r;

	printf("--- A: a listener takes the connect\n");
	lfd = listener(&port);
	CHK(lfd >= 0, "A. a listener on 127.0.0.1:%d", port);
	to = loop(1, port);
	r = settle(7, CLCONN_LANE, &src, &to, &now);
	CHK(r == 1 && clconn_open() == 0, "A. the probe: %d, and closed (%d open)", r, clconn_open());
	pf.fd = lfd;
	pf.events = POLLIN;
	cfd = poll(&pf, 1, 1000) == 1 ? accept(lfd, &from.sa, &fl) : -1;
	CHK(cfd >= 0 && from.in4.sin_addr.s_addr == htonl(0x7f000002u),
		"A. accepted from 127.0.0.2, the source asked for");
	pf.fd = cfd;
	CHK(cfd >= 0 && poll(&pf, 1, 1000) == 1 && read(cfd, b, sizeof b) == 0,
		"A. and it reads an end of stream at once: the probe carried nothing");
	if (cfd >= 0)
		close(cfd);

	printf("--- B: a closed port\n");
	close(lfd);
	r = settle(7, CLCONN_RPT, &src, &to, &now);
	CHK(r == -1 && clconn_open() == 0, "B. refused: %d, nothing left open", r);
	r = settle(7, CLCONN_RPT, &src, &to, &now);
	CHK(r == -1 && clconn_open() == 0, "B. asked again, a new probe: refused again (%d)", r);

	printf("clconntest: %s\n", fails ? "FAILED" : "all passed");
	return fails != 0;
}
