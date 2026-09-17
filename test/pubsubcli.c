/* SPDX-License-Identifier: GPL-2.0-or-later
 * pubsubcli.c - PS4 through libperfd: one handle subscribes, another
 * publishes, the first handle's notify hook receives the message on its
 * next read.  Usage: pubsubcli <port>   (plaintext JSON on loopback) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "perfd.h"

static int got, pgot;
static char last[512];

/* the hook gets (json, len): the bytes are NOT NUL-terminated at len -
 * the next line may follow them in the library's buffer - so every
 * look at them is bounded by the copy */
static void on_notify(const char *json, size_t len, void *ctx)
{
	size_t n = len < sizeof last - 1 ? len : sizeof last - 1;

	(void)ctx;
	memcpy(last, json, n);
	last[n] = 0;
	if (strstr(last, "\"method\":\"message\"") && strstr(last, "\"channel\":\"cli\""))
		got++;
	if (strstr(last, "\"method\":\"pmessage\"") && strstr(last, "\"pattern\":\"c*\""))
		pgot++;
}

int main(int argc, char **argv)
{
	int port = argc > 1 ? atoi(argv[1]) : 17471, pass = 0, fail = 0, n, i;
	perfd_opts o;
	perfd_t *s, *p;
	void *val = NULL;
	size_t vlen = 0;
	long long ttl = 0;

	memset(&o, 0, sizeof o);
	s = perfd_connect("127.0.0.1", port, &o);
	p = perfd_connect("127.0.0.1", port, &o);
	if (!s || !p) {
		printf("  FAIL connect: %s\n", perfd_error(NULL));
		printf("pubsubcli: 0 passed, 1 failed\n");
		return 1;
	}
	perfd_set_notify(s, on_notify, NULL);
#define OK(c, m) do { if (c) { pass++; printf("  ok   %s\n", m); } else { fail++; printf("  FAIL %s\n", m); } } while (0)
	n = perfd_subscribe(s, "cli");
	OK(n == 1, "perfd_subscribe answers the subscription count (1)");
	n = perfd_psubscribe(s, "c*");
	OK(n == 2, "perfd_psubscribe answers 2");
	n = perfd_publish(p, "cli", "hello", 5);
	OK(n == 2, "perfd_publish from another handle answers the receivers (2: channel + pattern)");
	/* the subscriber's handle reads its socket on its next call: a get on a
	 * multiplexed connection keeps waiting for its own id while the
	 * notifications are dispatched to the hook.  A publish returns once
	 * the message is QUEUED to the subscriber's worker, so the delivery
	 * can land after this handle's next reply - poll, bounded. */
	n = perfd_get(s, "0", "nokey", &val, &vlen, &ttl);
	free(val);
	OK(n == 0, "a data call on the subscribed handle still works (get miss)");
	for (i = 0; i < 300 && !(got == 1 && pgot == 1); i++) {
		usleep(10000);
		n = perfd_get(s, "0", "nokey", &val, &vlen, &ttl);
		free(val);
	}
	if (!(got == 1 && pgot == 1))
		printf("  (hook: got=%d pgot=%d after %d polls, last=%s)\n", got, pgot, i, last);
	OK(got == 1 && pgot == 1, "the notify hook received the message and the pmessage");
	OK(strstr(last, "\"payload\":\"hello\"") != NULL, "the payload arrives as a plain string");
	{
		unsigned char bin[3] = { 0, 0xff, 'x' };

		got = 0;
		n = perfd_publish(p, "cli", bin, sizeof bin);
		for (i = 0; i < 50 && got < 1; i++) {
			usleep(10000);
			n = perfd_get(s, "0", "nokey", &val, &vlen, &ttl);
			free(val);
		}
		OK(strstr(last, "\"enc\":\"b64\"") != NULL, "a binary payload arrives base64 with the enc sibling");
	}
	n = perfd_unsubscribe(s, NULL);
	OK(n == 1, "perfd_unsubscribe(NULL) drops every channel, one pattern left");
	n = perfd_punsubscribe(s, "c*");
	OK(n == 0, "perfd_punsubscribe drops the pattern");
	n = perfd_publish(p, "cli", "x", 1);
	OK(n == 0, "nobody left: publish answers 0");
	n = perfd_publish(p, "__pc.internal", "x", 1);
	OK(n < 0, "publish into the reserved prefix fails");
	perfd_free(s);
	perfd_free(p);
	printf("pubsubcli: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
