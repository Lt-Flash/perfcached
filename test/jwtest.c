/* jwtest.c - the reply writer, including the heap mode CLUSTER SLOTS needs: its growth path never
 * runs in production (the buffer is sized right), so it is driven directly here. */
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "json.h"

static int pass, fail;
static void ok(const char *m) { pass++; printf("  ok   %s\n", m); }
static void bad(const char *m) { fail++; printf("  FAIL %s\n", m); }

/* a fixed writer must REFUSE to overrun and say so */
static void t_fixed(void)
{
	char buf[16];
	struct pc_jw w;

	pc_jw_init(&w, buf, sizeof buf);
	pc_jw_raw(&w, "0123456789", 10);
	if (w.len == 10 && !w.overflow)
		ok("fixed writer accepts what fits");
	else
		bad("fixed writer mishandled a fitting write");
	pc_jw_raw(&w, "0123456789", 10);
	if (w.overflow && w.len == 10)
		ok("fixed writer flags overflow and drops the write");
	else
		bad("fixed writer did NOT flag overflow (len now doubtful)");
	if (!w.owned)
		ok("fixed writer owns nothing to free");
	else
		bad("fixed writer claims ownership of a stack buffer");
	pc_jw_free(&w);                        /* must be a no-op, not a free */
	ok("pc_jw_free on a fixed writer is safe");
}

/* the heap writer must GROW rather than flag, and keep every byte */
static void t_grow(void)
{
	struct pc_jw w;
	size_t i, want = 0;
	char *ref;
	int intact = 1;

	memset(&w, 0, sizeof w);
	if (pc_jw_init_heap(&w, 64) != 0) {
		bad("pc_jw_init_heap(64) failed");
		return;
	}
	ok("heap writer initialised");
	ref = malloc(200 * 37 + 1);
	if (!ref) { bad("out of memory in the test"); pc_jw_free(&w); return; }
	/* 200 writes of 37 bytes = 7400, from a 64-byte start: many grows */
	for (i = 0; i < 200; i++) {
		static const char chunk[] = "abcdefghijklmnopqrstuvwxyz0123456789\n";

		pc_jw_raw(&w, chunk, 37);
		memcpy(ref + want, chunk, 37);
		want += 37;
	}
	if (w.overflow)
		bad("heap writer flagged overflow instead of growing");
	else if (w.len != want)
		bad("heap writer lost bytes while growing");
	else {
		ok("heap writer grew from 64 bytes to hold 7400");
		for (i = 0; i < want; i++)
			if (w.buf[i] != ref[i]) { intact = 0; break; }
		if (intact)
			ok("every byte survived the reallocs intact");
		else
			bad("bytes were corrupted across a realloc");
	}
	if (w.cap >= want)
		ok("capacity covers the content");
	else
		bad("capacity is below the content length");
	free(ref);
	pc_jw_free(&w);
	if (!w.buf && !w.owned)
		ok("pc_jw_free releases and clears the heap writer");
	else
		bad("pc_jw_free left the writer pointing at freed memory");
}

/* the bound still has to hold, or a bug becomes an OOM */
static void t_bound(void)
{
	struct pc_jw w;

	/* the len guard reads whatever is in the struct, so an uninitialised stack writer refuses
	 * as if OOM: every call site must zero or pc_jw_init first */
	memset(&w, 0, sizeof w);
	if (pc_jw_init_heap(&w, (size_t)JW_HEAP_MAX + 1) != 0)
		ok("pc_jw_init_heap refuses a capacity past JW_HEAP_MAX");
	else {
		bad("pc_jw_init_heap accepted a capacity past JW_HEAP_MAX");
		pc_jw_free(&w);
	}
	memset(&w, 0, sizeof w);
	pc_jw_init(&w, NULL, 0);
	w.len = 1;                             /* pretend something was written */
	if (pc_jw_init_heap(&w, 4096) != 0)
		ok("heap mode is refused once bytes have been written");
	else
		bad("heap mode stranded already-written bytes");
}

/* pc_jw_i64 is formatted by hand (no snprintf on the reply path): it must
 * print exactly what %lld prints, at the edges and across a sweep */
static void t_i64(void)
{
	static const long long edge[] = { 0, 1, -1, 9, 10, -10, 99, 100,
		1234567890LL, -1234567890LL, LLONG_MAX, LLONG_MIN, LLONG_MIN + 1 };
	char want[32], buf[64];
	struct pc_jw w;
	long long v;
	size_t i;
	int bad_n = 0;

	for (i = 0; i < sizeof edge / sizeof edge[0] + 200000; i++) {
		v = i < sizeof edge / sizeof edge[0] ? edge[i]
			: (long long)((i * 2654435761ULL) ^ (i << 40)) * (i & 1 ? -1 : 1);
		pc_jw_init(&w, buf, sizeof buf);
		pc_jw_i64(&w, v);
		snprintf(want, sizeof want, "%lld", v);
		if (w.len != strlen(want) || memcmp(buf, want, w.len)) {
			if (bad_n++ < 3)
				printf("    %lld printed as %.*s\n", v, (int)w.len, buf);
		}
	}
	if (!bad_n)
		ok("pc_jw_i64 prints what %lld prints: 0, +-1, LLONG_MAX, LLONG_MIN and 200,000 more");
	else
		bad("pc_jw_i64 disagrees with %lld");
}

/* the inline append and pc_jw_i64's direct write: at exact capacity, one byte past (the bytes
 * after it are sentinels), after content, and a number that must grow a heap writer */
static void t_edges(void)
{
	char buf[32];
	struct pc_jw w;
	size_t i;
	int clean = 1;

	memset(buf, '#', sizeof buf);
	pc_jw_init(&w, buf, 16);
	pc_jw_raw(&w, "abcdefghijklmn", 14);
	pc_jw_lit(&w, "\r\n");                  /* 14 + 2 = exactly 16 */
	if (w.len == 16 && !w.overflow && !memcmp(buf, "abcdefghijklmn\r\n", 16))
		ok("an append that exactly fills a fixed writer lands");
	else
		bad("an exact-fit append was refused or misplaced");
	pc_jw_lit(&w, "x");
	for (i = 16; i < sizeof buf; i++)
		if (buf[i] != '#')
			clean = 0;
	if (w.overflow && w.len == 16 && clean)
		ok("one byte past a fixed writer: flagged, nothing written beyond it");
	else
		bad("an append past the capacity was taken or wrote beyond it");

	memset(buf, '#', sizeof buf);
	pc_jw_init(&w, buf, 16);
	pc_jw_lit(&w, "$");
	pc_jw_i64(&w, -12345678901234LL);      /* 1 + 15 = exactly 16 */
	if (w.len == 16 && !w.overflow && !memcmp(buf, "$-12345678901234", 16))
		ok("pc_jw_i64 after other content, exactly filling the writer");
	else
		bad("pc_jw_i64 misplaced a number after other content");
	pc_jw_i64(&w, 7);
	clean = 1;
	for (i = 16; i < sizeof buf; i++)
		if (buf[i] != '#')
			clean = 0;
	if (w.overflow && w.len == 16 && clean)
		ok("pc_jw_i64 past a fixed writer: flagged, nothing written beyond it");
	else
		bad("pc_jw_i64 wrote past a fixed writer's capacity");

	/* a heap writer starts at 4096 at least: fill it to 4090, so the
	 * 20-character number cannot fit and must grow it */
	memset(&w, 0, sizeof w);
	if (pc_jw_init_heap(&w, 64) != 0) {
		bad("pc_jw_init_heap(64) failed");
		return;
	}
	for (i = 0; i < 409; i++)
		pc_jw_raw(&w, "aaaaaaaaaa", 10);
	{
		size_t cap0 = w.cap;

		pc_jw_i64(&w, LLONG_MIN);            /* 4090 + 20 > cap0 */
		if (!w.overflow && w.len == 4110 && w.cap > cap0 && w.buf[4089] == 'a' &&
		        !memcmp(w.buf + 4090, "-9223372036854775808", 20))
			ok("pc_jw_i64 grows a heap writer and keeps every byte");
		else
			bad("pc_jw_i64 lost a number that needed a heap writer to grow");
	}
	pc_jw_free(&w);
}

/* a \u escape outside the BMP comes as a surrogate pair: one character, four UTF-8 bytes; a
 * lone half is not a character and is refused */
static int unesc(const char *doc, char *out, size_t cap)
{
	struct pc_jtok t[4];

	if (pc_json_parse(doc, strlen(doc), t, 4) != 2 || t[1].type != PC_J_STR)
		return -2;
	return pc_json_unescape(doc, &t[1], out, cap);
}

static void t_unescape(void)
{
	char out[32];
	int n;

	n = unesc("[\"\\ud83d\\ude00\"]", out, sizeof out);
	if (n == 4 && !memcmp(out, "\xF0\x9F\x98\x80", 4))
		ok("a surrogate pair decodes to the one four-byte character");
	else
		bad("a surrogate pair did not decode to F0 9F 98 80");
	n = unesc("[\"a\\uD834\\uDD1Eb\"]", out, sizeof out);
	if (n == 6 && !memcmp(out, "a\xF0\x9D\x84\x9E" "b", 6))
		ok("a pair between other bytes, upper-case hex");
	else
		bad("a pair between other bytes decoded wrong");
	if (unesc("[\"\\ud83d\"]", out, sizeof out) == -1)
		ok("a lone high surrogate is refused");
	else
		bad("a lone high surrogate was decoded");
	if (unesc("[\"\\ude00\"]", out, sizeof out) == -1)
		ok("a lone low surrogate is refused");
	else
		bad("a lone low surrogate was decoded");
	if (unesc("[\"\\ud83d\\u0041\"]", out, sizeof out) == -1)
		ok("a high surrogate followed by a non-surrogate escape is refused");
	else
		bad("a high surrogate followed by \\u0041 was decoded");
	if (unesc("[\"\\ud83d\\ude00\"]", out, 3) == -1)
		ok("a pair that does not fit the output is refused, not cut");
	else
		bad("a pair was written past the output's room");
	n = unesc("[\"\\u20ac\\u0000\"]", out, sizeof out);
	if (n == 4 && !memcmp(out, "\xE2\x82\xAC\0", 4))
		ok("BMP escapes and a NUL are unchanged");
	else
		bad("a BMP escape or NUL decoded wrong");
}

/* pc_jw_str_utf8 and pc_utf8_clean take only well-formed UTF-8 (RFC 3629): no overlong form, no
 * surrogate, nothing past U+10FFFF; every byte of anything else becomes U+FFFD */
static void t_utf8(void)
{
	static const struct { const char *in; size_t n; } badseq[] = {
		{ "\xED\xA0\x80", 3 },         /* U+D800, a surrogate */
		{ "\xED\xBF\xBF", 3 },         /* U+DFFF */
		{ "\xE0\x80\x80", 3 },         /* overlong NUL */
		{ "\xE0\x9F\xBF", 3 },         /* overlong U+07FF */
		{ "\xF0\x80\x80\x80", 4 },     /* overlong */
		{ "\xF0\x8F\xBF\xBF", 4 },     /* overlong U+FFFF */
		{ "\xF4\x90\x80\x80", 4 },     /* U+110000 */
		{ "\xF5\x80\x80\x80", 4 },     /* lead past F4 */
		{ "\xF7\xBF\xBF\xBF", 4 },
	};
	static const char *good[] = { "\xF0\x9F\x98\x80", "\xE2\x82\xAC", "\xED\x9F\xBF",
		"\xEE\x80\x80", "\xF4\x8F\xBF\xBF", "\xF0\x90\x80\x80", "\xC2\x80" };
	char buf[128], want[128];
	struct pc_jw w;
	size_t i, k;
	int okb = 1, okg = 1, okc = 1;

	for (i = 0; i < sizeof badseq / sizeof badseq[0]; i++) {
		pc_jw_init(&w, buf, sizeof buf);
		pc_jw_str_utf8(&w, badseq[i].in, badseq[i].n);
		strcpy(want, "\"");
		for (k = 0; k < badseq[i].n; k++)
			strcat(want, "\\ufffd");
		strcat(want, "\"");
		if (w.len != strlen(want) || memcmp(buf, want, w.len)) {
			okb = 0;
			printf("    sequence %zu rendered as %.*s\n", i, (int)w.len, buf);
		}
		if (pc_utf8_clean(badseq[i].in, badseq[i].n))
			okc = 0;
	}
	for (i = 0; i < sizeof good / sizeof good[0]; i++) {
		size_t n = strlen(good[i]);

		pc_jw_init(&w, buf, sizeof buf);
		pc_jw_str_utf8(&w, good[i], n);
		if (w.len != n + 2 || memcmp(buf + 1, good[i], n))
			okg = 0;
		if (!pc_utf8_clean(good[i], n))
			okc = 0;
	}
	if (okb)
		ok("surrogates, overlong forms and code points past U+10FFFF become U+FFFD");
	else
		bad("pc_jw_str_utf8 passed an ill-formed sequence through as UTF-8");
	if (okg)
		ok("well-formed sequences at the range edges pass through raw");
	else
		bad("pc_jw_str_utf8 replaced a well-formed sequence");
	if (okc)
		ok("pc_utf8_clean draws the same line");
	else
		bad("pc_utf8_clean called an ill-formed sequence valid, or a valid one not");
}

int main(void)
{
	t_fixed();
	t_grow();
	t_bound();
	t_i64();
	t_edges();
	t_unescape();
	t_utf8();
	printf("jwtest: %d passed, %d failed\n", pass, fail);
	return fail != 0;
}
