/* respscantest.c - the RESP door's keyspace walk (respscan.c; the 0.7.2
 * split, wave 1) over a fake table: bucket b holds the key "k<bbbb>" when
 * b is a multiple of `every` and at least `from`; bucket 10's key has
 * expired, bucket 12's expires later.  The cursor is the next bucket.
 *
 *   A  SCAN 0: the whole small table in one reply, cursor 0, the expired
 *      key left out and the later one kept
 *   B  SCAN ... COUNT 4 yields about four keys a reply - the next chunk
 *      sized from the density just seen - and the cursor resumes exactly;
 *      a rare MATCH bounds on keys VISITED, not matched (an empty reply
 *      with the cursor moved); an empty region stops at 16 x COUNT buckets
 *   C  MATCH filters; TYPE is accepted and ignored; a bad cursor, a COUNT
 *      out of 1..16384, a pattern of 256 bytes or an unknown word refused;
 *      a reply that cannot fit the scratch refused
 *   D  KEYS' turn (pc_keys_chunk): with no time budget each turn walks the
 *      1,024-bucket floor and yields - four turns for 4,096 buckets, every
 *      key once; with time to spare one turn walks it all, the floor then
 *      steps of 256; a limit stops the walk and says so; a pattern filters
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"
#include "store.h"
#include "core/pcache_htable.h"
#include "verbs.h"
#include "respout.h"
#include "verbsint.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

#define NOW 1000u
unsigned int compat_ticks(void) { return NOW; }

/* ---- the fake table ----------------------------------------------------- */
static unsigned int nb = 64, every = 2, from;
static int scans;

int pc_store_scan(pcache_htable_t *ht, unsigned int *cursor,
		unsigned int max_buckets, unsigned int flags,
		pcache_iter_cb cb, void *ctx)
{
	unsigned int b = *cursor, end = b + max_buckets;

	(void)ht; (void)flags;
	scans++;
	if (end > nb)
		end = nb;
	for (; b < end; b++) {
		char kb[16];
		str k, v = { NULL, 0 };

		if (b < from || b % every)
			continue;
		snprintf(kb, sizeof kb, "k%04u", b);
		k.s = kb;
		k.len = (int)strlen(kb);
		if (cb(&k, &v, b == 10 ? NOW - 1 : b == 12 ? NOW + 4000 : 0, ctx) < 0) {
			*cursor = b;           /* stopped on b: resume there */
			return 0;
		}
	}
	*cursor = end >= nb ? 0 : end;
	return 0;
}

/* ---- the harness ------------------------------------------------------- */
static char outbuf[8192], scratch[8192];
static size_t scratch_cap = sizeof scratch;
static struct pc_jw out;

static void run(const char *const *args)
{
	char *argv[8];
	size_t argl[8];
	struct resp_ctx c;
	int n;

	for (n = 0; n < 8 && args[n]; n++) {
		argv[n] = (char *)args[n];
		argl[n] = strlen(args[n]);
	}
	memset(&c, 0, sizeof c);
	c.argv = argv;
	c.argl = argl;
	c.nargs = n;
	c.out = &out;
	c.cmd = args[0];
	c.cl = strlen(args[0]);
	c.scratch = scratch;
	c.scratch_cap = scratch_cap;
	scans = 0;
	pc_jw_init(&out, outbuf, sizeof outbuf);
	rh_scan(&c);
}
#define SCAN(...) run((const char *const[]){ "SCAN", __VA_ARGS__, NULL })

static int is(const char *want)
{
	return out.len == strlen(want) && !memcmp(out.buf, want, out.len);
}

static int starts(const char *want)
{
	return out.len >= strlen(want) && !memcmp(out.buf, want, strlen(want));
}

static int has(const struct pc_jw *w, const char *needle)
{
	size_t n = strlen(needle), i;

	for (i = 0; i + n <= w->len; i++)
		if (!memcmp(w->buf + i, needle, n))
			return 1;
	return 0;
}

int main(void)
{
	static char bigbuf[1 << 16], longpat[257];
	struct pc_jw w;
	unsigned int cur;
	int emitted, lh, more, turns;

	/* A */
	SCAN("0");
	CHK(starts("*2\r\n$1\r\n0\r\n*31\r\n$5\r\nk0000\r\n"),
		"A: SCAN 0 - the whole table, cursor 0, 31 of 32 keys");
	CHK(!has(&out, "k0010") && has(&out, "k0012") && has(&out, "k0062"),
		"A: the expired key left out, the later one and the last kept");

	/* B */
	SCAN("0", "COUNT", "4");
	CHK(is("*2\r\n$1\r\n8\r\n*4\r\n$5\r\nk0000\r\n$5\r\nk0002\r\n$5\r\nk0004\r\n$5\r\nk0006\r\n") &&
		scans == 2, "B: COUNT 4 - four keys in two density-sized chunks, cursor 8");
	SCAN("8", "COUNT", "4");
	CHK(is("*2\r\n$2\r\n16\r\n*3\r\n$5\r\nk0008\r\n$5\r\nk0012\r\n$5\r\nk0014\r\n"),
		"B: resumed at 8 - four visited (the expired one counts), three answered, cursor 16");
	SCAN("0", "MATCH", "zzz*", "COUNT", "2");
	CHK(is("*2\r\n$1\r\n4\r\n*0\r\n"), "B: a MATCH nothing meets - empty, the cursor moved on");
	from = 48;
	SCAN("0", "COUNT", "2");
	CHK(is("*2\r\n$2\r\n32\r\n*0\r\n") && scans == 16,
		"B: an empty region - 16 x COUNT buckets in 16 calls, then the reply");
	from = 0;

	/* C */
	SCAN("0", "MATCH", "k001*", "COUNT", "1000");
	CHK(starts("*2\r\n$1\r\n0\r\n*4\r\n") && has(&out, "k0012") && has(&out, "k0018") &&
		!has(&out, "k0020"), "C: MATCH k001* - four keys (k0010 has expired)");
	SCAN("0", "TYPE", "string", "COUNT", "1000");
	CHK(starts("*2\r\n$1\r\n0\r\n*31\r\n"), "C: TYPE accepted and ignored");
	run((const char *const[]){ "SCAN", NULL });
	CHK(is("-ERR invalid cursor\r\n"), "C: no cursor");
	SCAN("-1");
	CHK(is("-ERR invalid cursor\r\n"), "C: a negative cursor");
	SCAN("4294967296");
	CHK(is("-ERR invalid cursor\r\n"), "C: a cursor past 32 bits");
	SCAN("0", "COUNT", "0");
	CHK(is("-ERR invalid COUNT\r\n"), "C: COUNT 0");
	SCAN("0", "COUNT", "16385");
	CHK(is("-ERR invalid COUNT\r\n"), "C: COUNT 16385");
	memset(longpat, 'p', 256);
	SCAN("0", "MATCH", longpat);
	CHK(is("-ERR syntax error\r\n"), "C: a pattern of 256 bytes");
	SCAN("0", "BOGUS");
	CHK(is("-ERR syntax error\r\n"), "C: an unknown word");
	scratch_cap = 16;
	SCAN("0");
	CHK(is("-ERR reply too large\r\n"), "C: a reply that cannot fit the scratch");
	scratch_cap = sizeof scratch;

	/* D */
	nb = 4096;
	every = 4;
	pc_keys_set_turn_us(0);
	pc_jw_init(&w, bigbuf, sizeof bigbuf);
	cur = 0;
	emitted = 0;
	turns = 0;
	do {
		scans = 0;
		more = pc_keys_chunk(NULL, &cur, NULL, -1, NOW, &w, &emitted, 1 << 30, &lh);
		turns++;
		if (scans != 1)
			break;
	} while (more && turns < 100);
	CHK(turns == 4 && !more && emitted == 1024 && !lh && !w.overflow,
		"D: no time budget - the 1,024-bucket floor a turn, 4 turns, 1,024 keys (%d turns)", turns);
	CHK(has(&w, "k0000") && has(&w, "k4092") && !has(&w, "k0002"), "D: every key, and only keys");
	pc_keys_set_turn_us(1000000);
	pc_jw_init(&w, bigbuf, sizeof bigbuf);
	cur = 0;
	emitted = 0;
	scans = 0;
	more = pc_keys_chunk(NULL, &cur, NULL, -1, NOW, &w, &emitted, 1 << 30, &lh);
	CHK(!more && emitted == 1024 && scans == 13,
		"D: time to spare - one turn, the floor then 12 steps of 256 (%d scans)", scans);
	pc_jw_init(&w, bigbuf, sizeof bigbuf);
	cur = 0;
	emitted = 0;
	more = pc_keys_chunk(NULL, &cur, NULL, -1, NOW, &w, &emitted, 5, &lh);
	CHK(emitted == 5 && lh && more, "D: a limit of 5 - five keys, the limit reported, more to come");
	pc_jw_init(&w, bigbuf, sizeof bigbuf);
	cur = 0;
	emitted = 0;
	more = pc_keys_chunk(NULL, &cur, "k1*", 3, NOW, &w, &emitted, 1 << 30, &lh);
	CHK(!more && emitted == 250, "D: KEYS k1* - the 250 keys k1000..k1996");

	printf("respscantest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
