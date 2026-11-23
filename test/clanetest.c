/* clanetest.c - what a frame-lane connection may hold before it shows it is a lane: the real lane
 * thread on loopback, clients from 127.0.0.1-3 for the per-address limit; a real hello deadline. */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
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

/* a listener on 127.0.0.1 with @backlog; its port into *@pt */
static int listener(int backlog, int *pt)
{
	struct sockaddr_in sa;
	socklen_t sl = sizeof sa;
	int s = socket(AF_INET, SOCK_STREAM, 0);

	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(0x7f000001u);
	if (s < 0 || bind(s, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(s, backlog) != 0 ||
	    getsockname(s, (struct sockaddr *)&sa, &sl) != 0)
		return -1;
	*pt = ntohs(sa.sin_port);
	return s;
}

/* the lane's state for 127.0.0.1:@pt after waiting up to @ms for UP */
static int state_after(int pt, int ms)
{
	pc_addr to;
	long long age;
	int st = CLANE_TCP_DOWN;

	if (pc_addr_parse("127.0.0.1", (uint16_t)pt, &to) != 0)
		return -1;
	for (; ms > 0; ms -= 50) {
		if ((st = clane_tcp_state(&to, &age)) == CLANE_TCP_UP)
			break;
		usleep(50 * 1000);
	}
	return st;
}

/* the fd in this process whose local port is @lp and peer port @rp; -1 if none */
static int fd_by_ports(int lp, int rp)
{
	struct sockaddr_in l, r;
	socklen_t ll, rl;
	int fd;

	for (fd = 3; fd < 4096; fd++) {
		ll = sizeof l;
		rl = sizeof r;
		if (getsockname(fd, (struct sockaddr *)&l, &ll) == 0 && l.sin_family == AF_INET &&
		    getpeername(fd, (struct sockaddr *)&r, &rl) == 0 &&
		    ntohs(l.sin_port) == lp && ntohs(r.sin_port) == rp)
			return fd;
	}
	return -1;
}

/* 1 when @fd would notice a vanished peer within ten seconds */
static int dead_peer_bounded(int fd)
{
	int ka = 0, idle = 0, intvl = 0, cnt = 0;
	unsigned int ut = 0;
	socklen_t l = sizeof ka;

	if (fd < 0 || getsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &ka, &l) != 0)
		return 0;
	l = sizeof idle;
	getsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, &l);
	l = sizeof intvl;
	getsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, &l);
	l = sizeof cnt;
	getsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, &l);
	l = sizeof ut;
	getsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &ut, &l);
	printf("  ..   fd %d keepalive %d idle %d s intvl %d s cnt %d user timeout %u ms\n",
		fd, ka, idle, intvl, cnt, ut);
	return ka && idle + intvl * cnt <= 10 && ut > 0 && ut <= 10000;
}

/* a lane that never stops sending minimal frames, as fast as the socket takes them */
static int flood_fd = -1, flood_stop;

static void *flood(void *arg)
{
	static unsigned char b[5 * 13000];
	size_t i;

	(void)arg;
	for (i = 0; i < sizeof b; i += 5) {
		b[i] = b[i + 1] = b[i + 2] = 0;
		b[i + 3] = 1;
		b[i + 4] = 'a';
	}
	while (!__atomic_load_n(&flood_stop, __ATOMIC_ACQUIRE))
		if (send(flood_fd, b, sizeof b, MSG_NOSIGNAL) < 0)
			break;
	return NULL;
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

	/* A: four connections from one address held; a fifth closed at once, counted as refused */
	for (n = 0, i = 0; i < 4; i++)
		n += (a[i] = dial(1)) >= 0;
	usleep(300 * 1000);
	for (still = 0, i = 0; i < 4; i++)
		still += a[i] >= 0 && !closed_within(a[i], 0);
	CHK(n == 4 && still == 4, "A: four connections from 127.0.0.1 - held");
	fd5 = dial(1);
	CHK(fd5 >= 0 && closed_within(fd5, 2000) && figs().refused == 1,
		"A: a fifth from the same address - closed, refused (%llu)", figs().refused);

	/* B: another address's hello then frame reaches the queue with the hello's cluster address */
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

	/* C: the four with no hello are closed after the 3 s deadline, counted; the lane stays open */
	for (n = 0, i = 0; i < 4; i++)
		n += closed_within(a[i], 6000);
	CHK(n == 4 && figs().inbound_timeouts == 4,
		"C: the four with no hello - closed at the deadline, timed out (%llu)", figs().inbound_timeouts);
	CHK(!closed_within(lane, 0), "C: the lane with a good hello - still open past it");

	/* D: a hello with the wrong magic is closed, counted as refused */
	bad = dial(3);
	CHK(bad >= 0 && hello(bad, 3, 7777, "XCL1") == 0 && closed_within(bad, 2000) && figs().refused == 2,
		"D: a hello with the wrong magic - closed, refused (%llu)", figs().refused);

	/* E: a connect whose SYN is never answered is not UP; one that is answered is */
	{
		int full, fill, live, fpt, lpt;
		pc_addr to;

		/* backlog 0 holds one connection; with it queued the kernel drops further SYNs */
		full = listener(0, &fpt);
		fill = socket(AF_INET, SOCK_STREAM, 0);
		memset(&sa, 0, sizeof sa);
		sa.sin_family = AF_INET;
		sa.sin_addr.s_addr = htonl(0x7f000001u);
		sa.sin_port = htons((uint16_t)fpt);
		n = full >= 0 && fill >= 0 && connect(fill, (struct sockaddr *)&sa, sizeof sa) == 0 &&
			pc_addr_parse("127.0.0.1", (uint16_t)fpt, &to) == 0;
		if (n)
			clane_probe(&to);
		still = n ? state_after(fpt, 2500) : -1;
		CHK(still == CLANE_TCP_PENDING,
			"E: a probe to a host that never answers the SYN - pending after 2.5 s, not up (%d)", still);
		live = listener(16, &lpt);
		if (live >= 0 && pc_addr_parse("127.0.0.1", (uint16_t)lpt, &to) == 0)
			clane_probe(&to);
		still = live >= 0 ? state_after(lpt, 3000) : -1;
		CHK(still == CLANE_TCP_UP, "E: a probe to a listening host - up (%d)", still);
	}

	/* F: both ends of a lane notice a vanished host in seconds, not at the retransmission limit */
	{
		struct sockaddr_in pa;
		socklen_t pl = sizeof pa;
		int live, lpt, acc = -1;
		pc_addr to;

		memset(&pa, 0, sizeof pa);
		live = listener(16, &lpt);
		if (live >= 0 && pc_addr_parse("127.0.0.1", (uint16_t)lpt, &to) == 0) {
			clane_probe(&to);
			acc = state_after(lpt, 3000) == CLANE_TCP_UP ?
				accept(live, (struct sockaddr *)&pa, &pl) : -1;
		}
		CHK(acc >= 0 && dead_peer_bounded(fd_by_ports(ntohs(pa.sin_port), lpt)),
			"F: the outbound lane socket - keepalive and a user timeout of seconds");
		pl = sizeof pa;
		CHK(lane >= 0 && getsockname(lane, (struct sockaddr *)&pa, &pl) == 0 &&
			dead_peer_bounded(fd_by_ports(port, ntohs(pa.sin_port))),
			"F: the inbound lane socket - keepalive and a user timeout of seconds");
	}

	/* G: frames for peers that never answer age out, the reconnects stop, and their slots go
	 * to new addresses once the table is full */
	{
		struct sockaddr_in cs;
		socklen_t cl = sizeof cs;
		int closed = socket(AF_INET, SOCK_STREAM, 0), queued = 0, again = -1, k;
		unsigned long long stale0 = figs().dropped_stale, c1, c2;
		unsigned char fr[32];
		pc_addr to;

		/* bound, never listening: every 127.x address on that port refuses at once */
		memset(&cs, 0, sizeof cs);
		cs.sin_family = AF_INET;
		memset(fr, 0x5a, sizeof fr);
		n = closed >= 0 && bind(closed, (struct sockaddr *)&cs, sizeof cs) == 0 &&
			getsockname(closed, (struct sockaddr *)&cs, &cl) == 0;
		for (k = 0; n && k < 520; k++) {
			pc_addr_from_v4(&to, htonl(0x7f010000u | (unsigned)(k + 1)), ntohs(cs.sin_port));
			queued += clane_send(&to, fr, sizeof fr) == 0;
		}
		usleep((CLANE_MAX_AGE_MS + 2500) * 1000);
		c1 = figs().connects;
		sleep(6);
		c2 = figs().connects;
		CHK(n && queued > 0 && figs().dropped_stale == stale0 + (unsigned long long)queued,
			"G: %d frames to peers that never answered - each aged out, counted (%llu)",
			queued, figs().dropped_stale - stale0);
		CHK(n && c2 == c1, "G: and with nothing left for them, no more connects (%llu in 6 s)",
			c2 - c1);
		if (n) {
			pc_addr_from_v4(&to, htonl(0x7f020001u), ntohs(cs.sin_port));
			again = clane_send(&to, fr, sizeof fr);
		}
		CHK(again == 0, "G: a new address once the table is full of idle peers - queued (%d)",
			again);
		if (closed >= 0)
			close(closed);
	}

	/* H: one inbound lane flooding minimal frames holds neither the lane thread nor the whole
	 * receive queue: another lane's frame still arrives */
	{
		pthread_t th;
		int b, found = 0, k, started = 0;
		unsigned char fb[4 + 5] = { 0, 0, 0, 5, 'b', 'b', 'b', 'b', 'b' };
		unsigned char got[64];
		pc_addr gf;

		flood_fd = dial(6);
		n = started = flood_fd >= 0 && hello(flood_fd, 6, 7006, "PCL1") == 0 &&
			pthread_create(&th, NULL, flood, NULL) == 0;
		usleep(300 * 1000);
		b = dial(7);
		n = n && b >= 0 && hello(b, 7, 7007, "PCL1") == 0 &&
			send(b, fb, sizeof fb, 0) == (ssize_t)sizeof fb;
		for (k = 0; n && k < 300 && !found; k++) {
			struct pollfd p = { clane_rx_fd(), POLLIN, 0 };
			size_t g;

			if (poll(&p, 1, 10) != 1)
				continue;
			while (!found && (g = clane_rx_take(got, sizeof got, &gf)) > 0)
				found = g == 5 && !memcmp(got, "bbbbb", 5) && pc_addr_port(&gf) == 7007;
		}
		CHK(n && found, "H: a lane flooding frames from 127.0.0.6 - a frame from 127.0.0.7 "
			"still queued within 3 s (%llu dropped from the full queue)", figs().dropped_rx);
		__atomic_store_n(&flood_stop, 1, __ATOMIC_RELEASE);
		if (flood_fd >= 0)
			shutdown(flood_fd, SHUT_RDWR);
		if (started)
			pthread_join(th, NULL);
		if (flood_fd >= 0)
			close(flood_fd);
		if (b >= 0)
			close(b);
	}

	for (i = 0; i < 4; i++)
		if (a[i] >= 0)
			close(a[i]);
	close(fd5);
	close(lane);
	close(bad);
	printf("clanetest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
