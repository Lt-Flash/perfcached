/* ptreetest.c - S317: the native door's value codec (src/ptree.c).
 *
 * The writer: every tag, nested containers whose counts are patched when
 * they close, a map counting keys not values, bytes with a NUL, a
 * rollback that restores the count and clears an overflow, a fixed
 * buffer that overflows cleanly, a heap buffer that grows, nesting past
 * PT_DEPTH refused, an unclosed container not "done".  The reader: every
 * tag back, get/at/key_at, the typed accessors ('i' and 'd' integers, a
 * non-integer 'd' refused), and a refusal of every truncation, a bad
 * tag, a map key that is not bytes, trailing bytes, a count that lies,
 * nesting past the limit.  The renderers: a known tree to its JSON and
 * its RESP, a map as an object and as a flat array, bools as true/:1,
 * an error as {"error":...} and -line, bytes that are not UTF-8 not
 * breaking the JSON.  Fail-first: the module does not exist before S317.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ptree.h"

static int pass, fail;
#define OK(c, m) do { if (c) pass++; else { fail++; printf("FAIL: %s\n", m); } } while (0)

static unsigned char B[65536];

/* the sample every part uses: {"a":1,"b":[true,false,null,"x\0y",1.10],
 *   "c":{"k":"v"},"ok":OK,"err":ERR boom,"big":-2}  */
static size_t sample(void)
{
	struct pc_tw w;

	pc_tw_init(&w, B, sizeof B);
	pc_tw_map(&w);
	pc_tw_key(&w, "a"); pc_tw_i64(&w, 1);
	pc_tw_key(&w, "b"); pc_tw_arr(&w);
		pc_tw_bool(&w, 1); pc_tw_bool(&w, 0); pc_tw_nil(&w);
		pc_tw_bulk(&w, "x\0y", 3); pc_tw_num(&w, "1.10", 4);
	pc_tw_end(&w);
	pc_tw_key(&w, "c"); pc_tw_map(&w); pc_tw_key(&w, "k"); pc_tw_str(&w, "v"); pc_tw_end(&w);
	pc_tw_key(&w, "ok"); pc_tw_ok(&w);
	pc_tw_key(&w, "err"); pc_tw_err(&w, "ERR boom");
	pc_tw_key(&w, "big"); pc_tw_i64(&w, -2);
	pc_tw_end(&w);
	OK(pc_tw_done(&w) == 0, "the sample closes whole");
	return w.n;
}

static unsigned int r32(const unsigned char *p)
{
	return (unsigned int)p[0] | (unsigned int)p[1] << 8 |
		(unsigned int)p[2] << 16 | (unsigned int)p[3] << 24;
}

int main(void)
{
	size_t n = sample(), k;
	struct pc_tv v = { 0 };     /* a reader starts zeroed: its node array grows inside it */
	char out[4096];
	struct pc_jw jw;
	int m, t;

	/* ---- writer ---- */
	OK(B[0] == 'm' && r32(B + 1) == 6, "the outer map counts its 6 keys, not 12 items");
	{
		/* the array sits after 'b' key: m(5) b(5+1 'a') i(9) b(5+1 'b') a(5) */
		size_t at = 5 + 6 + 9 + 6;

		OK(B[at] == 'a' && r32(B + at + 1) == 5, "the array's count is patched at close");
	}
	{
		struct pc_tw w;
		struct pc_tw_mark mk;
		unsigned char small[64];

		pc_tw_init(&w, small, sizeof small);
		pc_tw_arr(&w);
		pc_tw_i64(&w, 1);
		mk = pc_tw_mark(&w);
		pc_tw_i64(&w, 2);
		pc_tw_map(&w); pc_tw_key(&w, "x"); pc_tw_i64(&w, 3); pc_tw_end(&w);
		OK(w.st[0].cnt == 3, "three items before the rollback");
		pc_tw_rollback(&w, mk);
		OK(w.n == mk.n && w.st[0].cnt == 1 && w.depth == 1, "rollback restores length, count and depth");
		pc_tw_bulk(&w, B, 100);
		OK(w.over, "a fixed buffer overflows cleanly");
		pc_tw_rollback(&w, mk);
		OK(!w.over, "and the rollback clears the overflow");
		pc_tw_end(&w);
		OK(pc_tw_done(&w) == 0 && r32(small + 1) == 1, "closed with one item");

		pc_tw_init(&w, small, sizeof small);
		pc_tw_arr(&w);
		OK(pc_tw_done(&w) != 0, "an unclosed container is not done");
		pc_tw_end(&w); pc_tw_end(&w);
		OK(w.over, "closing more than was opened is an overflow");

		pc_tw_init(&w, small, sizeof small);
		pc_tw_key(&w, "k");
		OK(w.over, "a key outside a map is an overflow");

		pc_tw_init(&w, NULL, 0);
		pc_tw_arr(&w);
		for (k = 0; k < 10000; k++)
			pc_tw_i64(&w, (long long)k);
		pc_tw_end(&w);
		OK(pc_tw_done(&w) == 0 && w.n == 5 + 9 * 10000 && w.owned, "a heap writer grows");
		OK(pc_tv_parse(&v, w.b, w.n) == 0 && v.n[0].len == 10000, "and reads back");
		pc_tv_free(&v);
		pc_tw_free(&w);

		pc_tw_init(&w, NULL, 0);
		for (k = 0; k < PT_DEPTH + 1; k++)
			pc_tw_arr(&w);
		OK(w.over, "nesting past PT_DEPTH is refused");
		pc_tw_free(&w);
	}

	/* ---- reader ---- */
	memset(&v, 0, sizeof v);
	OK(pc_tv_parse(&v, B, n) == 0, "the sample parses");
	OK(v.n[0].type == 'm' && v.n[0].len == 6, "root: a map of 6");
	m = pc_tv_get(&v, 0, "b");
	OK(m >= 0 && v.n[m].type == 'a' && v.n[m].len == 5, "get b: the array");
	t = pc_tv_at(&v, m, 3);
	OK(t >= 0 && v.n[t].type == 'b' && v.n[t].len == 3 && !memcmp(v.n[t].p, "x\0y", 3), "at 3: bytes with a NUL");
	t = pc_tv_at(&v, m, 4);
	OK(t >= 0 && v.n[t].type == 'd' && v.n[t].len == 4 && !memcmp(v.n[t].p, "1.10", 4), "at 4: the number's text as written");
	OK(pc_tv_at(&v, m, 5) < 0, "at 5: past the end");
	t = pc_tv_at(&v, 0, 2);
	OK(t >= 0 && v.n[t].type == 'm', "a map's k-th VALUE");
	t = pc_tv_key_at(&v, 0, 2);
	OK(t >= 0 && pc_tv_streq(&v, t, "c"), "a map's k-th KEY");
	OK(pc_tv_get(&v, 0, "zz") < 0, "an absent key");
	OK(pc_tv_get(&v, m, "a") < 0, "get on an array");
	{
		char sb[8];
		long long iv = 0;

		OK(pc_tv_get_str(&v, pc_tv_get(&v, 0, "c"), "k", sb, sizeof sb) == 1 && sb[0] == 'v', "get_str");
		OK(pc_tv_get_str(&v, pc_tv_get(&v, 0, "c"), "k", sb, 0) < 0, "get_str: no room");
		OK(pc_tv_get_int(&v, 0, "a", &iv) == 0 && iv == 1, "get_int on an 'i'");
		OK(pc_tv_get_int(&v, 0, "big", &iv) == 0 && iv == -2, "get_int negative");
		OK(pc_tv_get_int(&v, 0, "b", &iv) < 0, "get_int on an array");
		OK(pc_tv_get_bool(&v, 0, "ok") == 0, "get_bool on OK is 0");
		t = pc_tv_get(&v, 0, "err");
		OK(t >= 0 && v.n[t].type == 'e' && v.n[t].len == 8, "the error line");
	}
	{
		struct pc_tw w;
		unsigned char d[64];
		long long iv;

		pc_tw_init(&w, d, sizeof d);
		pc_tw_map(&w); pc_tw_key(&w, "n"); pc_tw_num(&w, "42", 2);
		pc_tw_key(&w, "f"); pc_tw_num(&w, "4.5", 3); pc_tw_key(&w, "t"); pc_tw_bool(&w, 1); pc_tw_end(&w);
		pc_tv_free(&v);
		OK(pc_tv_parse(&v, d, w.n) == 0, "a small map parses");
		OK(pc_tv_get_int(&v, 0, "n", &iv) == 0 && iv == 42, "get_int on a 'd' holding an integer");
		OK(pc_tv_get_int(&v, 0, "f", &iv) < 0, "get_int on a 'd' holding 4.5: refused");
		OK(pc_tv_get_bool(&v, 0, "t") == 1, "get_bool true");
	}
	pc_tv_free(&v);

	/* ---- refusals ---- */
	{
		int bad = 0;

		for (k = 0; k < n; k++)
			if (pc_tv_parse(&v, B, k) == 0)
				bad++;
		OK(bad == 0, "every truncation of the sample is refused");
		OK(pc_tv_parse(&v, B, n) == 0, "and the whole one parses");
		memcpy(out, B, n);
		out[n] = 'n';
		OK(pc_tv_parse(&v, (unsigned char *)out, n + 1) != 0 && !strcmp(v.err, "trailing bytes"), "trailing bytes");
		out[0] = 'q';
		OK(pc_tv_parse(&v, (unsigned char *)out, n) != 0 && !strcmp(v.err, "bad tag"), "a bad tag");
		memcpy(out, B, n);
		out[5] = 'i';              /* the first key's tag */
		OK(pc_tv_parse(&v, (unsigned char *)out, n) != 0 && !strcmp(v.err, "map key not bytes"), "a map key that is not bytes");
		memcpy(out, B, n);
		out[1] = 7;                /* the root count lies */
		OK(pc_tv_parse(&v, (unsigned char *)out, n) != 0 && !strcmp(v.err, "truncated"), "a count that lies runs out of bytes");
		for (k = 0; k < PT_DEPTH + 1; k++) {
			out[5 * k] = 'a';
			out[5 * k + 1] = 1; out[5 * k + 2] = out[5 * k + 3] = out[5 * k + 4] = 0;
		}
		out[5 * (PT_DEPTH + 1)] = 'n';
		OK(pc_tv_parse(&v, (unsigned char *)out, 5 * (PT_DEPTH + 1) + 1) != 0 && !strcmp(v.err, "too deep"), "nesting past the limit");
		OK(pc_tv_parse(&v, (unsigned char *)out, 0) != 0, "an empty tree");
		pc_tv_free(&v);
	}

	/* ---- renderers ---- */
	pc_jw_init(&jw, out, sizeof out);
	k = pc_tree_json(&jw, B, n);
	out[jw.len] = 0;
	OK(k == n && !jw.overflow, "JSON render consumes the whole sample");
	OK(!strcmp(out, "{\"a\":1,\"b\":[true,false,null,\"x\\u0000y\",1.10],\"c\":{\"k\":\"v\"},"
		"\"ok\":\"OK\",\"err\":{\"error\":\"ERR boom\"},\"big\":-2}"), "the JSON text");
	pc_jw_init(&jw, out, sizeof out);
	k = pc_tree_resp(&jw, B, n);
	out[jw.len] = 0;
	OK(k == n && !strcmp(out, "*12\r\n$1\r\na\r\n:1\r\n$1\r\nb\r\n*5\r\n:1\r\n:0\r\n$-1\r\n$3\r\nx\0y\r\n"
		"$4\r\n1.10\r\n$1\r\nc\r\n*2\r\n$1\r\nk\r\n$1\r\nv\r\n$2\r\nok\r\n+OK\r\n$3\r\nerr\r\n-ERR boom\r\n"
		"$3\r\nbig\r\n:-2\r\n") == 0 ? 0 : 1, "unused");
	pass--;                        /* the strcmp above stops at the NUL: compare by memcmp */
	{
		static const char want[] = "*12\r\n$1\r\na\r\n:1\r\n$1\r\nb\r\n*5\r\n:1\r\n:0\r\n$-1\r\n$3\r\nx\0y\r\n"
			"$4\r\n1.10\r\n$1\r\nc\r\n*2\r\n$1\r\nk\r\n$1\r\nv\r\n$2\r\nok\r\n+OK\r\n$3\r\nerr\r\n-ERR boom\r\n"
			"$3\r\nbig\r\n:-2\r\n";

		OK(jw.len == sizeof want - 1 && !memcmp(out, want, jw.len), "the RESP text: a map flat, bools :1/:0");
	}
	{
		struct pc_tw w;
		unsigned char d[64];
		struct pc_jtok tk[16];

		pc_tw_init(&w, d, sizeof d);
		pc_tw_bulk(&w, "\xff\xfe ok", 5);
		pc_jw_init(&jw, out, sizeof out);
		OK(pc_tree_json(&jw, d, w.n) == w.n, "bytes that are not UTF-8 render");
		OK(pc_json_parse(out, jw.len, tk, 16) == 1 && tk[0].type == PC_J_STR, "and the JSON still parses");
		OK(!memchr(out, 0xff, jw.len), "with the bad bytes replaced");
		pc_jw_init(&jw, out, sizeof out);
		OK(pc_tree_json(&jw, d, w.n - 1) == 0, "a truncated item renders nothing");
	}

	/* ---- JSON text to a tree ---- */
	{
		static const char *const docs[] = { "val", NULL };
		const char *err = NULL;
		struct pc_tw w;
		static const char js[] = "{\"col\":\"0\",\"key\":\"k\\u00e9\\\"y\",\"ttl\":-7,"
			"\"big\":12345678901234567890,\"f\":1.10,\"nx\":true,\"z\":null,"
			"\"keys\":[\"a\",\"b\"],\"val\":{\"a\":[1,1.10,\"s\"]},\"items\":[{\"key\":\"x\"}]}";

		pc_tw_init(&w, NULL, 0);
		OK(pc_tree_from_json(js, strlen(js), &w, docs, &err) == 0 && pc_tw_done(&w) == 0,
			"a params object converts");
		OK(pc_tv_parse(&v, w.b, w.n) == 0 && v.n[0].type == 'm' && v.n[0].len == 10,
			"ten keys counted");
		t = pc_tv_get(&v, 0, "key");
		OK(t >= 0 && v.n[t].len == 5 && !memcmp(v.n[t].p, "k\xc3\xa9\"y", 5), "a string unescaped");
		{
			long long iv = 0;

			OK(pc_tv_get_int(&v, 0, "ttl", &iv) == 0 && iv == -7, "a plain integer is an i");
		}
		t = pc_tv_get(&v, 0, "big");
		OK(t >= 0 && v.n[t].type == 'd' && v.n[t].len == 20, "an integer past 64 bits keeps its text");
		t = pc_tv_get(&v, 0, "f");
		OK(t >= 0 && v.n[t].type == 'd' && !memcmp(v.n[t].p, "1.10", 4), "a decimal keeps its text");
		OK(pc_tv_get_bool(&v, 0, "nx") == 1, "true");
		t = pc_tv_get(&v, 0, "z");
		OK(t >= 0 && v.n[t].type == 'n', "null");
		t = pc_tv_get(&v, 0, "keys");
		OK(t >= 0 && v.n[t].type == 'a' && v.n[t].len == 2 && pc_tv_streq(&v, pc_tv_at(&v, t, 1), "b"), "an array of strings");
		t = pc_tv_get(&v, 0, "val");
		OK(t >= 0 && v.n[t].type == 'b' && v.n[t].len == 18 && !memcmp(v.n[t].p, "{\"a\":[1,1.10,\"s\"]}", 18),
			"a document key travels as its JSON text");
		t = pc_tv_get(&v, 0, "items");
		OK(t >= 0 && v.n[t].type == 'a' && pc_tv_streq(&v, pc_tv_get(&v, pc_tv_at(&v, t, 0), "key"), "x"),
			"and the key after the document is still read");
		pc_tv_free(&v);
		pc_tw_free(&w);

		pc_tw_init(&w, NULL, 0);
		OK(pc_tree_from_json("\"just a string\"", 15, &w, NULL, &err) == 0 && w.b[0] == 'b',
			"a bare string converts");
		pc_tw_free(&w);
		pc_tw_init(&w, NULL, 0);
		OK(pc_tree_from_json("{\"a\":1} x", 9, &w, NULL, &err) != 0, "trailing text refused");
		pc_tw_free(&w);
		pc_tw_init(&w, NULL, 0);
		OK(pc_tree_from_json("{\"a\":", 5, &w, NULL, &err) != 0, "a cut object refused");
		pc_tw_free(&w);
		pc_tw_init(&w, NULL, 0);
		OK(pc_tree_from_json("{\"a\":\"\\q\"}", 10, &w, NULL, &err) != 0 && !strcmp(err, "bad escape"),
			"a bad escape refused");
		pc_tw_free(&w);
		{
			unsigned char small[16];

			pc_tw_init(&w, small, sizeof small);
			OK(pc_tree_from_json("{\"abc\":\"0123456789\"}", 20, &w, NULL, &err) != 0,
				"a fixed buffer too small is refused, not overrun");
		}
	}
	/* ---- the heap switch ---- */
	{
		unsigned char small[8];
		struct pc_tw w;

		pc_tw_init(&w, small, sizeof small);
		OK(pc_tw_to_heap(&w, 1024) == 0 && w.owned && w.cap >= 1024, "an empty fixed writer moves to the heap");
		pc_tw_arr(&w);
		for (k = 0; k < 1000; k++)
			pc_tw_i64(&w, 1);
		pc_tw_end(&w);
		OK(pc_tw_done(&w) == 0 && w.n == 5 + 9000, "and grows");
		OK(pc_tw_to_heap(&w, 64) != 0, "a written writer refuses the switch");
		pc_tw_free(&w);
	}

	printf("ptreetest: %d passed, %d failed\n", pass, fail);
	return fail != 0;
}
