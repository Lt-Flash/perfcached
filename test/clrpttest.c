/* clrpttest.c - S469: replication groups over TCP (clrpt.c), sender and
 * receiver in one thread over loopback; cases A-E say what each checks */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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

int main(void)
{
	static unsigned char b[60000];
	struct clrpt_figs f0, f1;
	struct clrpt_rx *rx;
	pc_addr at;
	socklen_t al = sizeof at;
	int lfd, i, sent, refused;

	memset(&at, 0, sizeof at);
	at.in4.sin_family = AF_INET;
	at.in4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	lfd = clrpt_listen(&at);
	if (lfd < 0 || getsockname(lfd, &at.sa, &al) != 0) {
		printf("clrpttest: no listener\n");
		return 2;
	}
	rx = clrpt_rx_new(lfd, NULL);

	/* A */
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

	/* B */
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

	/* C */
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

	/* D */
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

	/* E: OM2-11 - an address the gate refuses is closed at accept */
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
	printf("clrpttest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
