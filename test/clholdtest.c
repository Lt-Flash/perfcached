/* clholdtest.c - S394's hold-check frames (clhold.c; the 0.7.2 split, wave
 * 1): the codec cluster.c writes and reads them with.
 *
 *   A  a query byte for byte - [M_HOLD_Q][node2][collen][col][n2] then each
 *      [req4][klen2][key] - and read back: the node, the collection, every
 *      key with its request id, then none
 *   B  the query's bounds: a collection name over 255 or a buffer too small
 *      for the header refused; CLHOLD_MAXN keys at most; a key that does
 *      not fit refused; is_col
 *   C  a damaged query: a header shorter than its own collection refused; a
 *      frame cut inside an entry yields the whole entries only; a count
 *      larger than the entries present stops at the frame's end
 *   D  answers byte for byte - [M_HOLD_A][node2][n2] then [req4][held1]
 *      [ver8] - held normalised to 1, no version when not held; read back,
 *      and an entry past the frame refused
 */
#include <stdio.h>
#include <string.h>
#include "../src/clhold.h"
#include "../src/clmsg.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

int main(void)
{
	static unsigned char buf[CLHOLD_MAX];
	struct clhold_qw w;
	struct clhold_qr r;
	const char *key;
	size_t n, kl;
	uint32_t req;
	unsigned int i, node, cnt;

	/* A */
	CHK(clhold_q_begin(&w, buf, sizeof buf, 3, "0", 1) == 0, "A: a query begun");
	CHK(clhold_q_add(&w, 0x01020304u, "k1", 2) == 0 &&
		clhold_q_add(&w, 7, "abc", 3) == 0, "A: two keys added");
	n = clhold_q_end(&w);
	{
		static const unsigned char want[] = {
			M_HOLD_Q, 0x00, 0x03, 0x01, '0', 0x00, 0x02,
			0x01, 0x02, 0x03, 0x04, 0x00, 0x02, 'k', '1',
			0x00, 0x00, 0x00, 0x07, 0x00, 0x03, 'a', 'b', 'c' };

		CHK(n == sizeof want && !memcmp(buf, want, n),
			"A: the frame byte for byte (%zu bytes)", n);
	}
	CHK(clhold_q_open(&r, buf, n) == 0 && r.node == 3 && r.collen == 1 &&
		r.col[0] == '0' && r.cnt == 2, "A: read back - node 3, collection 0, 2 keys");
	CHK(clhold_q_next(&r, &req, &key, &kl) == 1 && req == 0x01020304u &&
		kl == 2 && !memcmp(key, "k1", 2), "A: the first key and its request id");
	CHK(clhold_q_next(&r, &req, &key, &kl) == 1 && req == 7 &&
		kl == 3 && !memcmp(key, "abc", 3), "A: the second");
	CHK(clhold_q_next(&r, &req, &key, &kl) == 0, "A: then none");

	/* B */
	{
		char big[300];

		memset(big, 'c', sizeof big);
		CHK(clhold_q_begin(&w, buf, sizeof buf, 1, big, 256) == -1,
			"B: a collection name over 255 refused");
		CHK(clhold_q_begin(&w, buf, 6, 1, "col", 3) == -1,
			"B: a buffer too small for the header refused");
	}
	clhold_q_begin(&w, buf, sizeof buf, 1, "c", 1);
	for (i = 0; i < CLHOLD_MAXN; i++)
		if (clhold_q_add(&w, i, "k", 1) != 0)
			break;
	CHK(i == CLHOLD_MAXN && !clhold_q_fits(&w, 1) &&
		clhold_q_add(&w, 9999, "k", 1) == -1, "B: %d keys at most", CLHOLD_MAXN);
	{
		static unsigned char small[32];

		clhold_q_begin(&w, small, sizeof small, 1, "c", 1);
		CHK(clhold_q_add(&w, 1, "0123456789", 10) == 0 &&
			clhold_q_add(&w, 2, "0123456789", 10) == -1,
			"B: a key that does not fit the buffer refused");
		CHK(clhold_q_is_col(&w, "c", 1) && !clhold_q_is_col(&w, "d", 1) &&
			!clhold_q_is_col(&w, "cc", 2), "B: is_col");
	}

	/* C */
	clhold_q_begin(&w, buf, sizeof buf, 2, "col", 3);
	clhold_q_add(&w, 1, "aa", 2);
	clhold_q_add(&w, 2, "bbbb", 4);
	n = clhold_q_end(&w);
	CHK(clhold_q_open(&r, buf, 5) == -1 && clhold_q_open(&r, buf, 8) == -1,
		"C: a header shorter than its own collection refused");
	CHK(clhold_q_open(&r, buf, n - 2) == 0 &&
		clhold_q_next(&r, &req, &key, &kl) == 1 && req == 1 &&
		clhold_q_next(&r, &req, &key, &kl) == 0,
		"C: a frame cut inside an entry - the whole entries only");
	buf[4 + 3] = 0x00;
	buf[4 + 3 + 1] = 0x09;                    /* claims 9 keys */
	cnt = 0;
	if (clhold_q_open(&r, buf, n) == 0)
		while (clhold_q_next(&r, &req, &key, &kl))
			cnt++;
	CHK(cnt == 2, "C: a count larger than the entries present stops at the frame's end");

	/* D */
	n = clhold_a_begin(buf, 9);
	n = clhold_a_put(buf, n, 5, 7, 0x0102030405060708ULL);   /* held: any non-zero */
	n = clhold_a_put(buf, n, 6, 0, 999);
	clhold_a_end(buf, 2);
	{
		static const unsigned char want[] = {
			M_HOLD_A, 0x00, 0x09, 0x00, 0x02,
			0x00, 0x00, 0x00, 0x05, 0x01,
			0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
			0x00, 0x00, 0x00, 0x06, 0x00,
			0, 0, 0, 0, 0, 0, 0, 0 };

		CHK(n == sizeof want && !memcmp(buf, want, n),
			"D: the answer byte for byte - held as 1, no version when not held");
	}
	{
		unsigned long long ver;
		int held;

		CHK(clhold_a_open(buf, n, &node, &cnt) == 0 && node == 9 && cnt == 2,
			"D: read back - node 9, 2 entries");
		CHK(clhold_a_get(buf, n, 0, &req, &held, &ver) == 1 && req == 5 &&
			held == 1 && ver == 0x0102030405060708ULL, "D: the first");
		CHK(clhold_a_get(buf, n, 1, &req, &held, &ver) == 1 && req == 6 &&
			held == 0 && ver == 0, "D: the second");
		CHK(clhold_a_get(buf, n, 2, &req, &held, &ver) == 0 &&
			clhold_a_get(buf, n - 1, 1, &req, &held, &ver) == 0,
			"D: an entry past the frame refused");
		CHK(clhold_a_open(buf, 4, &node, &cnt) == -1, "D: a header too short refused");
	}

	printf("clholdtest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
