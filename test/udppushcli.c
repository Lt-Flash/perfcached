/* SPDX-License-Identifier: GPL-2.0-or-later
 * udppushcli.c - PS5 through libperfd: a handle takes its pub/sub
 * deliveries over UDP and hands them to the same notify hook, with the
 * same JSON, as the connection would; a blocking handle and an async one;
 * gaps counted when the socket overflows; a stranger's datagram refused;
 * a stream kept alive by the timer acks while idle, and one that stops
 * acking pruned - subscriptions gone, the hook told, no re-subscription.
 * Usage: udppushcli <port> <port of a door with pubsub_udp = no>
 * (plaintext loopback doors of a standalone daemon) */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "perfd.h"

struct hook {
	int messages, pmessages, pruned, big, b64;
	int order_ok, next;                /* "m<next>" expected on channel u */
	char first[256];
};

static void on_notify(const char *json, size_t len, void *ctx)
{
	struct hook *h = ctx;
	char buf[4096];
	size_t n = len < sizeof buf - 1 ? len : sizeof buf - 1;
	char want[32];

	memcpy(buf, json, n);
	buf[n] = 0;
	if (strstr(buf, "\"method\":\"pubsub_udp_pruned\"")) {
		h->pruned++;
		return;
	}
	if (strstr(buf, "\"method\":\"pmessage\"")) {
		h->pmessages++;
		return;
	}
	if (!strstr(buf, "\"method\":\"message\""))
		return;
	h->messages++;
	if (!h->first[0]) {
		size_t k = n < sizeof h->first - 1 ? n : sizeof h->first - 1;

		memcpy(h->first, buf, k);
		h->first[k] = 0;
	}
	if (len > 2000)
		h->big++;
	if (strstr(buf, "\"enc\":\"b64\""))
		h->b64++;
	snprintf(want, sizeof want, "\"payload\":\"m%d\"", h->next);
	if (strstr(buf, "\"channel\":\"u\"") && strstr(buf, want))
		h->next++;
}

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* read a blocking handle's UDP socket for @ms or until @done */
static void pump(perfd_t *s, int ms, int (*done)(void *), void *arg)
{
	long long end = now_ms() + ms;

	while (now_ms() < end && !(done && done(arg))) {
		struct pollfd pf;

		pf.fd = perfd_pubsub_udp_fd(s);
		pf.events = POLLIN;
		if (pf.fd < 0)
			return;
		poll(&pf, 1, 100);
		perfd_pubsub_udp_ready(s);
	}
}

static struct hook hs, ha, ht;
static int got_110(void *a) { (void)a; return hs.messages + hs.pmessages >= 110; }

/* drive an async handle: its connection and its UDP socket */
static void drive(perfd_t *a, int ms, int (*done)(void *), void *arg)
{
	long long end = now_ms() + ms;

	while (now_ms() < end && !(done && done(arg))) {
		struct pollfd pf[2];
		int n = 1;

		pf[0].fd = perfd_fd(a);
		/* perfd_events speaks PERFD_EV_*, not poll's bits */
		pf[0].events = (short)(((perfd_events(a) & PERFD_EV_READ) ? POLLIN : 0) |
			((perfd_events(a) & PERFD_EV_WRITE) ? POLLOUT : 0));
		pf[1].fd = perfd_pubsub_udp_fd(a);
		pf[1].events = POLLIN;
		if (pf[1].fd >= 0)
			n = 2;
		poll(pf, (nfds_t)n, 100);
		if (pf[0].revents & POLLOUT)
			perfd_write_ready(a);
		if (pf[0].revents & POLLIN)
			perfd_read_ready(a);
		perfd_push(a);
		perfd_pubsub_udp_ready(a);
	}
}

static int replied;
static void on_reply(char *result, size_t len, const char *errmsg, void *ctx)
{
	(void)len; (void)errmsg; (void)ctx;
	free(result);
	replied++;
}
static int is_replied(void *a) { (void)a; return replied > 0; }
static int is_ready(void *a) { return perfd_state(a) == PERFD_ST_READY; }
static int is_active(void *a)
{
	struct perfd_udp_stats st;

	perfd_pubsub_udp_stats(a, &st);
	return st.active;
}
static int got_20(void *a) { (void)a; return ha.messages >= 20; }

int main(int argc, char **argv)
{
	int port = argc > 1 ? atoi(argv[1]) : 17491;
	int noport = argc > 2 ? atoi(argv[2]) : 17495;
	int pass = 0, fail = 0, i, n;
	perfd_opts o;
	perfd_t *s, *pub, *t, *a, *no;
	struct perfd_udp_stats st, st0;
	char payload[4096];
	long long t0;

	setvbuf(stdout, NULL, _IONBF, 0);      /* lines survive a timeout */
	memset(&o, 0, sizeof o);
	s = perfd_connect("127.0.0.1", port, &o);
	pub = perfd_connect("127.0.0.1", port, &o);
	t = perfd_connect("127.0.0.1", port, &o);
	no = perfd_connect("127.0.0.1", noport, &o);
	if (!s || !pub || !t || !no) {
		printf("  FAIL connect: %s\n", perfd_error(NULL));
		printf("udppushcli: 0 passed, 1 failed\n");
		return 1;
	}
#define OK(c, ...) do { char m_[512]; snprintf(m_, sizeof m_, __VA_ARGS__); \
	if (c) { pass++; printf("  ok   %s\n", m_); } else { fail++; printf("  FAIL %s\n", m_); } } while (0)
	perfd_set_notify(s, on_notify, &hs);
	perfd_set_notify(t, on_notify, &ht);
	OK(perfd_subscribe(s, "u") == 1 && perfd_psubscribe(s, "p.*") == 2, "subscribed on the handle");

	/* ---- a blocking handle ---- */
	n = perfd_pubsub_udp(s, 0, 3000);
	perfd_pubsub_udp_stats(s, &st);
	OK(n == 0 && st.active && st.port > 0 && perfd_pubsub_udp_fd(s) >= 0,
		"perfd_pubsub_udp on a blocking handle: probe seen, confirmed, active on port %d (%s)",
		st.port, n ? perfd_error(s) : "ok");
	for (i = 0; i < 100; i++) {
		n = snprintf(payload, sizeof payload, "m%d", i);
		if (perfd_publish(pub, "u", payload, (size_t)n) != 1)
			break;
	}
	for (i = 0; i < 10; i++)
		perfd_publish(pub, "p.x", "q", 1);
	pump(s, 3000, got_110, NULL);
	perfd_pubsub_udp_stats(s, &st);
	OK(hs.messages == 100 && hs.pmessages == 10 && st.received == 110,
		"110 deliveries reach the notify hook through UDP (%d + %d, %llu over UDP)",
		hs.messages, hs.pmessages, st.received);
	OK(hs.next == 100, "the 100 on channel u in order (%d)", hs.next);
	OK(!strcmp(hs.first, "{\"jsonrpc\":\"2.0\",\"method\":\"message\",\"params\":{\"channel\":\"u\",\"payload\":\"m0\"}}"),
		"the hook's JSON is what the connection would carry: %s", hs.first);
	OK(st.gaps == 0 && st.duplicates == 0 && st.rejected == 0, "no gaps, duplicates or rejections");
	perfd_publish(pub, "u", "\x00\xff", 2);
	pump(s, 300, NULL, NULL);
	OK(hs.b64 == 1, "a binary payload arrives base64 with \"enc\", as on the connection");
	memset(payload, 'x', 2500);
	perfd_publish(pub, "u", payload, 2500);
	pump(s, 300, NULL, NULL);
	perfd_maintain(s);                     /* the connection's pushes */
	perfd_pubsub_udp_stats(s, &st0);
	OK(hs.big == 1 && st0.received == st.received + 1,
		"a 2,500-byte payload comes over the connection to the same hook (big %d)", hs.big);

	/* a datagram from anyone but the door is refused */
	{
		int x = socket(AF_INET, SOCK_DGRAM, 0);
		struct sockaddr_in to;

		memset(&to, 0, sizeof to);
		to.sin_family = AF_INET;
		to.sin_port = htons((unsigned short)st.port);
		to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		sendto(x, "\x70\x01\x02nothing-sealed-here-at-all", 29, 0,
			(struct sockaddr *)&to, sizeof to);
		close(x);
		pump(s, 200, NULL, NULL);
		perfd_pubsub_udp_stats(s, &st);
		OK(st.rejected == 1 && st.received == st0.received,
			"a stranger's datagram is rejected, not delivered (%llu)", st.rejected);
	}

	/* gaps: a socket too small for a burst */
	{
		int small = 2048, big = 1 << 20;
		unsigned long long r0 = st.received, g0 = st.gaps;

		setsockopt(perfd_pubsub_udp_fd(s), SOL_SOCKET, SO_RCVBUF, &small, sizeof small);
		for (i = 0; i < 3000; i++)
			perfd_publish(pub, "u", "burst", 5);
		setsockopt(perfd_pubsub_udp_fd(s), SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
		pump(s, 300, NULL, NULL);
		for (i = 0; i < 70; i++) {
			perfd_publish(pub, "u", "tail", 4);
			pump(s, 5, NULL, NULL);
		}
		pump(s, 300, NULL, NULL);
		perfd_pubsub_udp_stats(s, &st);
		OK(st.gaps > 0 && (st.received - r0) + (st.gaps - g0) == 3070,
			"a burst over a small socket: gaps counted, and delivered + gaps = 3,070 sent (%llu + %llu)",
			st.received - r0, st.gaps - g0);
		OK(st.acks >= 1, "acknowledged along the way (%llu acks)", st.acks);
	}

	/* ---- an async handle ---- */
	a = perfd_connect_async("127.0.0.1", port, &o);
	drive(a, 3000, is_ready, a);
	perfd_set_notify(a, on_notify, &ha);
	n = perfd_submit(a, "subscribe", "{\"channel\":\"a\"}", on_reply, NULL);
	OK(n == 0, "async handle subscribed (state %d, %s)", perfd_state(a), n ? perfd_error(a) : "ok");
	drive(a, 2000, is_replied, NULL);
	t0 = now_ms();
	n = perfd_pubsub_udp(a, 0, 3000);
	OK(n == 0, "perfd_pubsub_udp on an async handle submits the request (%s)", n ? perfd_error(a) : "ok");
	/* the daemon sends its first probe while it answers: read the UDP
	 * socket BEFORE the reply is taken off the connection, which is the
	 * order an event loop can see them in */
	perfd_push(a);
	{
		struct pollfd pf;

		pf.fd = perfd_pubsub_udp_fd(a);
		pf.events = POLLIN;
		if (pf.fd >= 0 && poll(&pf, 1, 2000) > 0)
			perfd_pubsub_udp_ready(a);
	}
	drive(a, 3000, is_active, a);
	perfd_pubsub_udp_stats(a, &st);
	OK(st.active, "the async handle's loop takes the probe and confirms (active %d)", st.active);
	OK(st.rejected == 0 && now_ms() - t0 < 700,
		"a probe read before the reply is kept, not rejected: active in %lld ms (rejected %llu)",
		now_ms() - t0, st.rejected);
	for (i = 0; i < 20; i++)
		perfd_publish(pub, "a", "async", 5);
	drive(a, 3000, got_20, NULL);
	perfd_pubsub_udp_stats(a, &st);
	OK(ha.messages == 20 && st.received == 20, "20 deliveries through its UDP socket to its hook (%d, %llu)",
		ha.messages, st.received);

	/* ---- idle but acknowledged, and silent ---- */
	OK(perfd_subscribe(t, "t") == 1 && perfd_pubsub_udp(t, 0, 3000) == 0, "a third handle takes UDP pushes too");
	t0 = now_ms();
	while (now_ms() - t0 < 17500) {
		pump(s, 100, NULL, NULL);          /* s acks on its timer */
		drive(a, 100, NULL, NULL);         /* so does the async one */
		/* t never calls perfd_pubsub_udp_ready: no acks */
	}
	perfd_maintain(t);
	perfd_pubsub_udp_stats(s, &st);
	n = perfd_publish(pub, "u", "still", 5);
	OK(st.active && !st.pruned && n == 1,
		"17 s idle, acknowledged on the timer: still active, still subscribed (active %d, pruned %d, receivers %d, acks %llu)",
		st.active, st.pruned, n, st.acks);
	perfd_pubsub_udp_stats(a, &st);
	OK(st.active && !st.pruned, "the async handle too");
	perfd_pubsub_udp_stats(t, &st);
	OK(ht.pruned == 1 && st.pruned && !st.active && perfd_pubsub_udp_fd(t) == -1 &&
		perfd_pubsub_udp_ready(t) == -1,
		"a handle that never acknowledged is pruned: the hook is told, the socket closed (pruned %d)", ht.pruned);
	OK(perfd_publish(pub, "t", "gone", 4) == 0, "its subscriptions are gone and it did not re-subscribe");
	OK(perfd_ping(t) == 0, "its connection is still usable");

	/* ---- off, and a door that refuses ---- */
	OK(perfd_pubsub_udp_off(s) == 0 && perfd_pubsub_udp_fd(s) == -1, "perfd_pubsub_udp_off closes the stream");
	n = perfd_pubsub_udp(no, 0, 1000);
	OK(n == -1 && strstr(perfd_error(no), "no UDP pushes"), "a door with pubsub_udp = no refuses (%s)", perfd_error(no));

	perfd_free(a);
	perfd_free(s);
	perfd_free(t);
	perfd_free(no);
	perfd_free(pub);
	printf("udppushcli: %d passed, %d failed\n", pass, fail);
	return fail != 0;
}
