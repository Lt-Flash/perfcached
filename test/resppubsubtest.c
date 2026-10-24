/* resppubsubtest.c - the RESP door's pub/sub (resppubsub.c; the 0.7.2
 * split, wave 1), the engine stubbed: every reply byte for byte.
 *
 *   A  SUBSCRIBE: one confirmation per channel with the running count; no
 *      channel, or a name the engine refuses, is an error
 *   B  PSUBSCRIBE: channels and patterns counted together, as Redis does
 *   C  UNSUBSCRIBE by name; with no name every channel goes, one
 *      confirmation each with what is left; PUNSUBSCRIBE likewise for
 *      patterns, and a bare nil confirmation when there is none
 *   D  PUBLISH: the receivers counted, the reserved __pc. prefix refused,
 *      the arity checked
 *   E  PUBSUB CHANNELS / NUMSUB / NUMPAT, and an unknown subcommand counted
 *      (S165) and refused
 */
#include <stdio.h>
#include <string.h>
#include "json.h"
#include "pubsub.h"
#include "respout.h"
#include "verbsint.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* ---- the stubs: the engine, a worker, the S165 counter ------------------ */
static struct { char name[64]; size_t len; int pattern; } subs[16];
static int nsubs, unk_calls;
static char pub_chan[64], pub_data[64];

int pc_worker_id(void) { return 1; }
void unk_sub(const struct resp_ctx *c) { (void)c; unk_calls++; }

int pc_pubsub_subscribe(void *conn, int worker, const char *name,
		size_t nlen, int pattern)
{
	(void)conn; (void)worker;
	if (nlen >= sizeof subs[0].name || nsubs == 16)
		return -1;
	memcpy(subs[nsubs].name, name, nlen);
	subs[nsubs].len = nlen;
	subs[nsubs].pattern = pattern;
	return ++nsubs;
}

int pc_pubsub_unsubscribe(void *conn, const char *name, size_t nlen, int pattern)
{
	int i;

	(void)conn;
	for (i = 0; i < nsubs; i++)
		if (subs[i].pattern == pattern && subs[i].len == nlen &&
		    !memcmp(subs[i].name, name, nlen)) {
			subs[i] = subs[--nsubs];
			break;
		}
	return nsubs;
}

int pc_pubsub_pop(void *conn, int pattern, char *name, size_t cap,
		size_t *nlen, int *left)
{
	int i;

	(void)conn; (void)cap;
	for (i = 0; i < nsubs; i++)
		if (subs[i].pattern == pattern) {
			memcpy(name, subs[i].name, subs[i].len);
			*nlen = subs[i].len;
			subs[i] = subs[--nsubs];
			*left = nsubs;
			return 1;
		}
	return 0;
}

int pc_pubsub_count(void *conn) { (void)conn; return nsubs; }

int pc_pubsub_publish(const char *chan, size_t clen, const char *data,
		size_t dlen, int origin)
{
	(void)origin;
	memcpy(pub_chan, chan, clen); pub_chan[clen] = 0;
	memcpy(pub_data, data, dlen); pub_data[dlen] = 0;
	return 2;
}

int pc_pubsub_channels(const char *glob, size_t glen, pc_pubsub_name_f cb,
		void *arg)
{
	(void)glob; (void)glen;
	cb("news", 4, 3, arg);
	cb("sports", 6, 1, arg);
	return 0;
}

int pc_pubsub_numsub(const char *name, size_t nlen)
{
	return nlen == 4 && !memcmp(name, "news", 4) ? 3 : 0;
}

int pc_pubsub_numpat(void) { return 1; }

/* ---- the harness ------------------------------------------------------- */
static char outbuf[4096];
static struct pc_jw out;

static void run(int (*h)(struct resp_ctx *), const char *a0, const char *a1,
		const char *a2, const char *a3)
{
	char *argv[4] = { (char *)a0, (char *)a1, (char *)a2, (char *)a3 };
	size_t argl[4];
	struct resp_ctx c;
	int n, i;

	for (n = 0; n < 4 && argv[n]; n++)
		;
	for (i = 0; i < n; i++)
		argl[i] = strlen(argv[i]);
	memset(&c, 0, sizeof c);
	c.argv = argv;
	c.argl = argl;
	c.nargs = n;
	c.out = &out;
	c.conn = &out;                     /* any non-NULL connection */
	pc_jw_init(&out, outbuf, sizeof outbuf);
	h(&c);
}

static int is(const char *want)
{
	return out.len == strlen(want) && !memcmp(out.buf, want, out.len);
}

int main(void)
{
	char longname[80];

	/* A */
	run(rh_subscribe, "SUBSCRIBE", "a", "b", NULL);
	CHK(is("*3\r\n$9\r\nsubscribe\r\n$1\r\na\r\n:1\r\n"
		"*3\r\n$9\r\nsubscribe\r\n$1\r\nb\r\n:2\r\n"),
		"A: SUBSCRIBE a b - one confirmation each, the count running");
	run(rh_subscribe, "SUBSCRIBE", NULL, NULL, NULL);
	CHK(is("-ERR wrong number of arguments for 'subscribe' command\r\n"),
		"A: SUBSCRIBE with no channel refused");
	memset(longname, 'x', sizeof longname - 1);
	longname[sizeof longname - 1] = 0;
	run(rh_subscribe, "SUBSCRIBE", longname, NULL, NULL);
	CHK(is("-ERR cannot subscribe (out of memory or the name is too long)\r\n"),
		"A: a name the engine refuses is an error");

	/* B */
	run(rh_psubscribe, "PSUBSCRIBE", "n.*", NULL, NULL);
	CHK(is("*3\r\n$10\r\npsubscribe\r\n$3\r\nn.*\r\n:3\r\n"),
		"B: PSUBSCRIBE - channels and patterns counted together");

	/* C */
	run(rh_unsubscribe, "UNSUBSCRIBE", "a", NULL, NULL);
	CHK(is("*3\r\n$11\r\nunsubscribe\r\n$1\r\na\r\n:2\r\n"), "C: UNSUBSCRIBE a");
	run(rh_unsubscribe, "UNSUBSCRIBE", NULL, NULL, NULL);
	CHK(is("*3\r\n$11\r\nunsubscribe\r\n$1\r\nb\r\n:1\r\n"),
		"C: UNSUBSCRIBE alone - every channel, the pattern left counted");
	run(rh_punsubscribe, "PUNSUBSCRIBE", NULL, NULL, NULL);
	CHK(is("*3\r\n$12\r\npunsubscribe\r\n$3\r\nn.*\r\n:0\r\n"),
		"C: PUNSUBSCRIBE alone - the pattern");
	run(rh_punsubscribe, "PUNSUBSCRIBE", NULL, NULL, NULL);
	CHK(is("*3\r\n$12\r\npunsubscribe\r\n$-1\r\n:0\r\n"),
		"C: and with none left, a bare nil confirmation");

	/* D */
	run(rh_publish, "PUBLISH", "ch", "msg", NULL);
	CHK(is(":2\r\n") && !strcmp(pub_chan, "ch") && !strcmp(pub_data, "msg"),
		"D: PUBLISH - the receivers counted, channel and message passed");
	run(rh_publish, "PUBLISH", "__pc.events", "x", NULL);
	CHK(is("-ERR channel prefix __pc. is reserved for the daemon's own events\r\n"),
		"D: the reserved prefix refused");
	run(rh_publish, "PUBLISH", "ch", NULL, NULL);
	CHK(is("-ERR wrong number of arguments for 'publish' command\r\n"),
		"D: PUBLISH without a message refused");

	/* E */
	run(rh_pubsub, "PUBSUB", "CHANNELS", NULL, NULL);
	CHK(is("*2\r\n$4\r\nnews\r\n$6\r\nsports\r\n"), "E: PUBSUB CHANNELS");
	run(rh_pubsub, "PUBSUB", "NUMSUB", "news", "x");
	CHK(is("*4\r\n$4\r\nnews\r\n:3\r\n$1\r\nx\r\n:0\r\n"), "E: PUBSUB NUMSUB news x");
	run(rh_pubsub, "PUBSUB", "numpat", NULL, NULL);
	CHK(is(":1\r\n"), "E: PUBSUB NUMPAT (the word in any case)");
	unk_calls = 0;
	run(rh_pubsub, "PUBSUB", "HELP", NULL, NULL);
	CHK(unk_calls == 1 && is("-ERR unsupported PUBSUB subcommand (CHANNELS, "
		"NUMSUB, NUMPAT)\r\n"), "E: an unknown subcommand counted (S165) and refused");

	printf("resppubsubtest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
