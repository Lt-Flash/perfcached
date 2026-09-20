/*
 * clcoltest.c - the collection-announce frame (M_COL_SET).
 *
 * DIFFERENTIAL against the three builders it replaces.  ref_one() and
 * ref_rename() are the byte writes copied verbatim from
 * pc_cluster_col_announce, col_sync_peer and pc_cluster_col_announce2 -
 * three near-identical builders differing only in the op byte, which is
 * the shape that drifts.
 *
 * `gen` is a Lamport value and orders competing announcements.  Read it
 * wrong and a create and a drop reorder: a collection comes back after
 * being deleted, or does not come back at all.  Nothing about that
 * looks like a parse error at the time.
 *
 * Build: cc -o clcoltest test/clcoltest.c src/clcol.o
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../src/clcol.h"
#include "../src/clcodec.h"

static int pass, fail;
static void chk(int cond, const char *what)
{
	if (cond) { pass++; return; }
	fail++;
	printf("  FAIL %s\n", what);
}

/* ---- verbatim from cluster.c before the extraction ---- */
static size_t ref_one(unsigned char *msg, unsigned char type, int op, int bl,
	unsigned long long gen, const char *name, size_t nlen)
{
	msg[0] = type;
	msg[1] = (unsigned char)op;
	msg[2] = (unsigned char)bl;
	memcpy(msg + 3, &gen, 8);
	msg[11] = (unsigned char)nlen;
	memcpy(msg + 12, name, nlen);
	return 12 + nlen;
}

static size_t ref_rename(unsigned char *msg, unsigned char type,
	unsigned long long gen, const char *from, size_t flen,
	const char *to, size_t tlen)
{
	msg[0] = type;
	msg[1] = 2;
	msg[2] = 0;
	memcpy(msg + 3, &gen, 8);
	msg[11] = (unsigned char)flen;
	memcpy(msg + 12, from, flen);
	msg[12 + flen] = (unsigned char)tlen;
	memcpy(msg + 13 + flen, to, tlen);
	return 13 + flen + tlen;
}

int main(void)
{
	const char *nm = "sessions", *to = "sessions-v2";
	const unsigned long long GEN = 0x0123456789abcdefULL;
	unsigned char a[256], b[256];
	struct clcol_ann c;
	size_t la, lb, n;
	int op;

	printf("=== clcoltest ===\n");

	/* ---- A. bytes, for every op the three builders emit ---------- */
	for (op = 0; op <= 3; op++) {
		char w[48];

		if (op == CLCOL_OP_RENAME)
			continue;              /* its own shape, below */
		memset(a, 0xAA, sizeof a); memset(b, 0xAA, sizeof b);
		la = ref_one(a, 9, op, 14, GEN, nm, 8);
		memset(&c, 0, sizeof c);
		c.op = op; c.buckets_log2 = 14; c.gen = GEN;
		c.name = nm; c.nlen = 8;
		lb = clcol_write(b, sizeof b, 9, &c);
		snprintf(w, sizeof w, "A op=%d bytes unchanged", op);
		chk(la == lb && memcmp(a, b, la) == 0, w);
	}
	memset(a, 0xAA, sizeof a); memset(b, 0xAA, sizeof b);
	la = ref_rename(a, 9, GEN, nm, 8, to, 11);
	memset(&c, 0, sizeof c);
	c.op = CLCOL_OP_RENAME; c.gen = GEN;
	c.name = nm; c.nlen = 8; c.to = to; c.tlen = 11;
	lb = clcol_write(b, sizeof b, 9, &c);
	chk(la == lb && memcmp(a, b, la) == 0, "A4 rename bytes unchanged");
	chk(lb == clcol_size(8, 11), "A5 the size helper agrees");
	chk(clcol_size(8, 0) == 20, "A6 and for the single-name form");

	/* ---- B. gen survives, bit for bit ---------------------------- */
	{
		static const unsigned long long G[] = { 0, 1, ~0ULL,
			0x8000000000000000ULL, 0x00000000ffffffffULL };
		size_t i;

		for (i = 0; i < sizeof G / sizeof G[0]; i++) {
			memset(a, 0, sizeof a);
			ref_one(a, 9, 0, 14, G[i], nm, 8);
			memset(&c, 0, sizeof c);
			c.op = 0; c.buckets_log2 = 14; c.gen = G[i];
			c.name = nm; c.nlen = 8;
			clcol_write(b, sizeof b, 9, &c);
			chk(memcmp(a, b, 20) == 0, "B1 the Lamport gen is identical");
			chk(clcol_parse(b, 20, &c) && c.gen == G[i],
				"B2 and parses back exactly");
		}
	}

	/* ---- C. parse at every length, buffers cut to size ----------- */
	la = ref_rename(a, 9, GEN, nm, 8, to, 11);
	for (n = 0; n <= la; n++) {
		unsigned char *t = malloc(n ? n : 1);
		int ok;

		memcpy(t, a, n);
		memset(&c, 0x7F, sizeof c);
		ok = clcol_parse(t, n, &c);
		if (n < la)
			chk(!ok, "C1 a rename is refused until BOTH names are there");
		else
			chk(ok && c.op == CLCOL_OP_RENAME && c.nlen == 8 &&
				c.tlen == 11 && memcmp(c.name, nm, 8) == 0 &&
				memcmp(c.to, to, 11) == 0,
				"C2 the whole rename parses");
		free(t);
	}
	/* the single-name ops must NOT demand a second name */
	la = ref_one(a, 9, CLCOL_OP_DROP, 0, GEN, nm, 8);
	chk(clcol_parse(a, la, &c) && c.op == CLCOL_OP_DROP && !c.to,
		"C3 a drop parses without a second name");
	for (n = 0; n < la; n++) {
		unsigned char *t = malloc(n ? n : 1);

		memcpy(t, a, n);
		chk(!clcol_parse(t, n, &c), "C4 a short drop is refused");
		free(t);
	}

	/* ---- D. a frame that names nothing --------------------------- */
	memset(a, 0, sizeof a);
	ref_one(a, 9, 0, 14, GEN, nm, 8);
	a[11] = 0;
	chk(!clcol_parse(a, 20, &c), "D1 nlen 0 is refused");
	la = ref_rename(a, 9, GEN, nm, 8, to, 11);
	a[12 + 8] = 0;
	chk(!clcol_parse(a, la, &c), "D2 a rename to a zero-length name is refused");

	printf("clcoltest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
