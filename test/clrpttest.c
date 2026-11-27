/* clrpttest.c - replication groups over TCP (clrpt.c), with sender and receiver in one thread
 * over loopback */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "clrpt.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static int got, bad;
static size_t want_len[400];
static unsigned char want_seed[400];

static void on_frame(void *ctx, const unsigned char *f, size_t n, const pc_addr *from)
{
	size_t i;

	(void)ctx;
	(void)from;
	if (got >= 400 || n != want_len[got])
		bad++;
	else
		for (i = 0; i < n; i++)
			if (f[i] != (unsigned char)(want_seed[got] + i * 7)) {
				bad++;
				break;
			}
	got++;
}

static void pump(struct clrpt_rx *rx, int want, int rounds)
{
	struct pollfd pf[1 + CLRPT_RX_CONNS];
	int k, n;

	for (k = 0; k < rounds && got < want; k++) {
		clrpt_flush();
		n = clrpt_rx_pollset(rx, pf, 1 + CLRPT_RX_CONNS);
		if (poll(pf, (nfds_t)n, 20) > 0)
			clrpt_rx_serve(rx, pf, n, 0, on_frame, NULL);
	}
}

static int host_never(const pc_addr *from)
{
	(void)from;
	return 0;
}

static int frame(unsigned char *b, int i, size_t n)
{
	size_t j;

	want_len[i] = n;
	want_seed[i] = (unsigned char)(i * 13);
	for (j = 0; j < n; j++)
		b[j] = (unsigned char)(want_seed[i] + j * 7);
	return 0;
}

/* a plain listener on loopback, standing for the peer's relay port; -1 none */
static int plain_listener(pc_addr *at, int backlog)
{
	socklen_t al = sizeof *at;
	int l = socket(AF_INET, SOCK_STREAM, 0);

	memset(at, 0, sizeof *at);
	at->in4.sin_family = AF_INET;
	at->in4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (l < 0 || bind(l, &at->sa, sizeof at->in4) != 0 || listen(l, backlog) != 0 ||
	    getsockname(l, &at->sa, &al) != 0) {
		if (l >= 0)
			close(l);
		return -1;
	}
	return l;
}

/* the next connection on @l within a second; -1 none */
static int accept_wait(int l)
{
	struct pollfd pf = { .fd = l, .events = POLLIN };

	if (poll(&pf, 1, 1000) != 1)
		return -1;
	return accept(l, NULL, NULL);
}

/* whole [len][frame]s read from @c until @want or half a second of nothing */
static int read_frames(int c, int want)
{
	static unsigned char rb[65536];
	size_t have = 0;
	int n = 0;

	while (n < want) {
		struct pollfd pf = { .fd = c, .events = POLLIN };
		ssize_t r;

		if (poll(&pf, 1, 500) != 1)
			break;
		r = recv(c, rb + have, sizeof rb - have, 0);
		if (r <= 0)
			break;
		have += (size_t)r;
		while (have >= 4) {
			size_t fl = (size_t)rb[0] << 24 | (size_t)rb[1] << 16 | (size_t)rb[2] << 8 | rb[3];

			if (have < 4 + fl)
				break;
			memmove(rb, rb + 4 + fl, have - 4 - fl);
			have -= 4 + fl;
			n++;
		}
	}
	return n;
}

int main(void)
{
	static unsigned char b[60000];
	struct clrpt_figs f0, f1;
	struct clrpt_rx *rx;
	pc_addr at;
	socklen_t al = sizeof at;
	int lfd, i, sent, refused;

	/* every line printed as it is said, and a hang ended in two minutes: the hung case shows */
	setvbuf(stdout, NULL, _IONBF, 0);
	alarm(120);
	memset(&at, 0, sizeof at);
	at.in4.sin_family = AF_INET;
	at.in4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	lfd = clrpt_listen(&at);
	if (lfd < 0 || getsockname(lfd, &at.sa, &al) != 0) {
		printf("clrpttest: no listener\n");
		return 2;
	}
	rx = clrpt_rx_new(lfd, NULL);

	/* A: frames of 1 B to 60,000 B arrive whole and in order */
	printf("  ..   case A\n");
	for (i = 0, sent = 0; i < 100; i++) {
		size_t n = i == 0 ? 1 : (size_t)(i * 599 % 60000) + 1;

		frame(b, i, n);
		sent += clrpt_send(&at, b, n) == 0;
		pump(rx, 0, 1);
	}
	pump(rx, 100, 500);
	CHK(sent == 100 && got == 100 && bad == 0,
		"A: 100 frames of 1 B to 60,000 B - queued %d, received %d whole and in order (%d bad)",
		sent, got, bad);

	/* B: a stopped reader - a send past the bound is refused (a fallback), then all drain */
	printf("  ..   case B\n");
	clrpt_figures(&f0);
	clrpt_set_qmax(200000);
	got = 0;
	for (i = 0, refused = 0, sent = 0; i < 400 && !refused; i++) {
		frame(b, sent, 30000);
		if (clrpt_send(&at, b, 30000) == 0)
			sent++;
		else
			refused = 1;
	}
	clrpt_figures(&f1);
	CHK(refused && f1.fallbacks == f0.fallbacks + 1,
		"B: the reader stopped - %d queued, then a send past the bound refused (a fallback)", sent);
	pump(rx, sent, 2000);
	CHK(got == sent && bad == 0, "B: the reader drained all %d (%d received, %d bad)", sent, got, bad);
	clrpt_set_qmax(CLRPT_QMAX);

	/* C: a length past the frame limit drops the connection */
	printf("  ..   case C\n");
	{
		int c = socket(AF_INET, SOCK_STREAM, 0);
		unsigned char h[4] = { 0x7f, 0xff, 0xff, 0xff };

		clrpt_figures(&f0);
		if (c >= 0 && connect(c, &at.sa, sizeof at.in4) == 0 &&
		    write(c, h, 4) == 4) {
			got = 0;
			pump(rx, 1, 50);
		}
		clrpt_figures(&f1);
		CHK(f1.rx_refused == f0.rx_refused + 1 && got == 0,
			"C: a length past the frame limit - the connection dropped, refused counted");
		if (c >= 0)
			close(c);
	}

	/* D: nothing listening - the connect fails and the sends fall back */
	printf("  ..   case D\n");
	{
		pc_addr none = at;
		int r1, r2;

		pc_addr_set_port(&none, 1);       /* nobody listens on port 1 */
		clrpt_figures(&f0);
		r1 = clrpt_send(&none, b, 100);
		pump(rx, 0, 3);
		r2 = clrpt_send(&none, b, 100);
		clrpt_figures(&f1);
		CHK(f1.connects > f0.connects && f1.connect_fail > f0.connect_fail &&
		    f1.fallbacks > f0.fallbacks && (r1 != 0 || r2 != 0),
			"D: nothing listening - the connect failed and the sends fell back (%d, %d)",
			r1, r2);
	}

	printf("  ..   case F\n");
	/* F: a header naming a 60,000-byte frame commits no 60 KB: the buffer grows with what
	 * arrives, and the frame still lands whole */
	{
		static unsigned char h[4 + 60000];
		int c = socket(AF_INET, SOCK_STREAM, 0);
		unsigned long long held = ~0ULL;

		frame(h + 4, 0, 60000);
		h[0] = 0; h[1] = 0; h[2] = 0xea; h[3] = 0x60;
		got = bad = 0;
		clrpt_figures(&f0);            /* the other connections' buffers stand */
		if (c >= 0 && connect(c, &at.sa, sizeof at.in4) == 0 &&
		    write(c, h, 104) == 104) {
			size_t o = 104;
			int k;

			pump(rx, 1, 25);
			clrpt_figures(&f1);
			held = f1.rx_held - f0.rx_held;
			/* the rest without blocking: this thread is also the reader, and a
			 * blocking write past the socket buffers waits for it for ever */
			fcntl(c, F_SETFL, O_NONBLOCK);
			for (k = 0; k < 2000 && got == 0; k++) {
				if (o < sizeof h) {
					ssize_t w = write(c, h + o, sizeof h - o);

					if (w > 0)
						o += (size_t)w;
				}
				pump(rx, 1, 1);
			}
		}
		CHK(held < 16384 && got == 1 && bad == 0,
			"F: 100 bytes of a 60,000-byte frame hold %llu bytes; the rest sent, it lands whole (%d, %d bad)",
			held, got, bad);
		got = bad = 0;            /* E's pump waits on got */
		if (c >= 0)
			close(c);
	}

	printf("  ..   case E\n");
	/* E: an address the gate refuses is closed at accept */
	clrpt_rx_free(rx);
	at.in4.sin_port = 0;
	lfd = clrpt_listen(&at);
	al = sizeof at;
	if (lfd < 0 || getsockname(lfd, &at.sa, &al) != 0) {
		printf("clrpttest: no listener for E\n");
		return 2;
	}
	rx = clrpt_rx_new(lfd, host_never);
	{
		int c = socket(AF_INET, SOCK_STREAM, 0);
		unsigned char x = 0;
		ssize_t got = -1;

		clrpt_figures(&f0);
		if (c >= 0 && connect(c, &at.sa, sizeof at.in4) == 0) {
			pump(rx, 1, 20);
			got = recv(c, &x, 1, 0);
		}
		clrpt_figures(&f1);
		CHK(got == 0 && f1.rx_refused == f0.rx_refused + 1,
			"E: a host the gate refuses is closed at accept, counted (recv %zd)", got);
		if (c >= 0)
			close(c);
	}
	clrpt_rx_free(rx);

	printf("  ..   case G\n");
	/* G: the receiver closed a connection that went quiet (its idle close): the next frames
	 * go on a new connection, or are refused - none is taken into the dead socket */
	{
		pc_addr g;
		int l = plain_listener(&g, 8), c, r1 = -1, r2 = -1, r3 = -1, n1 = 0, n2 = 0;

		if (l >= 0) {
			frame(b, 0, 100);
			r1 = clrpt_send(&g, b, 100);
			if ((c = accept_wait(l)) >= 0) {
				n1 = read_frames(c, 1);
				close(c);              /* the receiver's idle close */
			}
			usleep(1500000);
			r2 = clrpt_send(&g, b, 100);
			usleep(50000);
			r3 = clrpt_send(&g, b, 100);
			if ((c = accept_wait(l)) >= 0) {
				n2 = read_frames(c, 2);
				close(c);
			}
			close(l);
		}
		CHK(r1 == 0 && n1 == 1 && n2 == (r2 == 0) + (r3 == 0) && n2 > 0,
			"G: after the receiver's close, every frame said queued arrives on a new connection "
			"(first %d/%d, then queued %d %d, arrived %d)", r1, n1, r2, r3, n2);
	}

	printf("  ..   case H\n");
	/* H: a connect the peer has not answered (its accept queue full: the SYN is dropped) queues
	 * nothing - "no connection yet" is -1, and the caller falls back */
	{
		pc_addr h;
		int l = plain_listener(&h, 0), fill[4], k, r = 0;

		for (k = 0; k < 4; k++) {
			fill[k] = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
			if (l >= 0 && fill[k] >= 0)
				(void)connect(fill[k], &h.sa, sizeof h.in4);
		}
		usleep(100000);
		frame(b, 0, 100);
		if (l >= 0)
			r = clrpt_send(&h, b, 100);
		CHK(l >= 0 && r == -1,
			"H: a connect still in flight: not queued, the caller falls back (%d)", r);
		for (k = 0; k < 4; k++)
			if (fill[k] >= 0)
				close(fill[k]);
		if (l >= 0)
			close(l);
	}

	printf("  ..   case I\n");
	/* I: a sending thread's table full of peers that refuse (gone): a new peer still gets an
	 * entry, and its frame arrives */
	{
		pc_addr d, n;
		int l, c, k, r = -1, got_n = 0;

		frame(b, 0, 100);
		for (k = 1; k <= CLRPT_PEERS; k++) {
			memset(&d, 0, sizeof d);
			d.in4.sin_family = AF_INET;
			d.in4.sin_addr.s_addr = htonl(0x7f030000u | (unsigned)k);
			pc_addr_set_port(&d, 1);   /* nobody listens on port 1 */
			(void)clrpt_send(&d, b, 100);
			(void)clrpt_send(&d, b, 100);
		}
		l = plain_listener(&n, 8);
		if (l >= 0) {
			r = clrpt_send(&n, b, 100);
			if (r == 0 && (c = accept_wait(l)) >= 0) {
				got_n = read_frames(c, 1);
				close(c);
			}
			close(l);
		}
		CHK(l >= 0 && r == 0 && got_n == 1,
			"I: %d refusing peers fill the table - a new peer is still sent to (%d, arrived %d)",
			CLRPT_PEERS, r, got_n);
	}

	printf("clrpttest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
