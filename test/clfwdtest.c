/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * clfwdtest.c - the proxy write plane's wire frames (M11 slice 1).
 *
 * Both frames were built at one end of cluster.c and decoded at the
 * other, so nothing checked that the two agreed.  These round-trip
 * them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/clfwd.h"

static int pass, fail;
static void chk(int cond, const char *what)
{
	if (cond) { pass++; return; }
	fail++;
	printf("FAIL: %s\n", what);
}

/* ---- GOLDEN WIRE VECTORS ------------------------------------------
 *
 * Captured from clfwd.c as it stood BEFORE the cursor rewrite, by
 * running the builders and dumping their output.  The rest of this file
 * round-trips build against parse, which proves the two agree with each
 * other - and would go on passing if every offset in the file shifted
 * by one, because both halves would shift together.  These bytes are
 * what a peer on an older build actually puts on the wire, so they are
 * the only thing here that can catch that.
 *
 * If one of these fails, the frame layout MOVED.  That is a fleet-wide
 * incompatibility, not a test to update.
 */
/* GENERATED from the pre-cursor clfwd.c - do not hand-edit */
static const struct golden { const char *nm; size_t n;
	unsigned char b[128]; } GOLDEN[] = {
	{ "FWD_OP", 46, {
		0x11, 0xef, 0xbe, 0xad, 0xde, 0x01, 0x10, 0x03, 0x84, 0x03, 0x00, 0x00,
		0x35, 0xfb, 0x04, 0x8e, 0xe0, 0xfe, 0xff, 0xff, 0x08, 0x07, 0x00, 0x73,
		0x65, 0x73, 0x73, 0x69, 0x6f, 0x6e, 0x73, 0x61, 0x62, 0x63, 0x2d, 0x31,
		0x32, 0x33, 0x7b, 0x22, 0x76, 0x22, 0x3a, 0x34, 0x32, 0x7d } },
	{ "FWD_ACK", 14, {
		0x12, 0x67, 0x45, 0x23, 0x01, 0x01, 0x16, 0xe9, 0x4f, 0xb3, 0xfd, 0xff,
		0xff, 0xff } },
	{ "DEMOTE", 21, {
		0x14, 0x01, 0x10, 0x08, 0x07, 0x00, 0x73, 0x65, 0x73, 0x73, 0x69, 0x6f,
		0x6e, 0x73, 0x61, 0x62, 0x63, 0x2d, 0x31, 0x32, 0x33 } },
	{ "TOMBSTONE", 19, {
		0x16, 0x08, 0x07, 0x00, 0x73, 0x65, 0x73, 0x73, 0x69, 0x6f, 0x6e, 0x73,
		0x61, 0x62, 0x63, 0x2d, 0x31, 0x32, 0x33 } },
	{ "FWD_JSON", 58, {
		0x10, 0xef, 0xcd, 0xab, 0x89, 0xda, 0x02, 0x02, 0x05, 0xb0, 0x04, 0x00,
		0x00, 0xb2, 0x41, 0xde, 0xfc, 0x00, 0x00, 0x00, 0x00, 0x08, 0x07, 0x00,
		0x05, 0x00, 0x08, 0x00, 0x00, 0x00, 0x73, 0x65, 0x73, 0x73, 0x69, 0x6f,
		0x6e, 0x73, 0x61, 0x62, 0x63, 0x2d, 0x31, 0x32, 0x33, 0x24, 0x2e, 0x61,
		0x2e, 0x62, 0x7b, 0x22, 0x76, 0x22, 0x3a, 0x34, 0x32, 0x7d } },
	{ "FWD_JACK", 23, {
		0x18, 0x0f, 0x0f, 0x0f, 0x0f, 0x01, 0x02, 0xfd, 0x26, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
};

static void check_golden(const unsigned char *got, size_t n, const char *nm)
{
	size_t i;

	for (i = 0; i < sizeof GOLDEN / sizeof GOLDEN[0]; i++) {
		char w[64];

		if (strcmp(GOLDEN[i].nm, nm))
			continue;
		snprintf(w, sizeof w, "GOLDEN %s length", nm);
		chk(n == GOLDEN[i].n, w);
		snprintf(w, sizeof w, "GOLDEN %s bytes unchanged on the wire", nm);
		chk(n == GOLDEN[i].n && memcmp(got, GOLDEN[i].b, n) == 0, w);
		return;
	}
	chk(0, "GOLDEN vector missing");
}

int main(void)
{
	unsigned char buf[512];
	struct clfwd_op w, r;
	struct clfwd_ack aw, ar;
	size_t len;

	printf("=== clfwdtest ===\n");

	/* A. a SET forward, every field back intact */
	w.req = 0xDEADBEEF;  w.node = 4242;  w.op = 0;
	w.ttl_rel = 3600;    w.delta = 0;
	w.col = "orders";    w.collen = 6;
	w.key = "cust:99";   w.klen = 7;
	w.val = "the-value"; w.vlen = 9;
	chk(clfwd_op_size(6, 7, 9) == CLFWD_OP_HDR + 22, "A1 op_size counts every part");
	len = clfwd_op_build(buf, 0x42, &w);
	chk(len == CLFWD_OP_HDR + 22, "A2 build returns the frame length");
	chk(buf[0] == 0x42, "A2 the type byte is the caller's, not baked in");
	chk(clfwd_op_parse(buf, len, &r) == 1, "A3 it parses");
	chk(r.req == 0xDEADBEEF, "A3 req");
	chk(r.node == 4242, "A3 the forwarding node");
	chk(r.op == 0, "A3 op");
	chk(r.ttl_rel == 3600, "A3 ttl");
	chk(r.collen == 6 && !memcmp(r.col, "orders", 6), "A3 collection");
	chk(r.klen == 7 && !memcmp(r.key, "cust:99", 7), "A3 key");
	chk(r.vlen == 9 && !memcmp(r.val, "the-value", 9), "A3 value");

	/* B. the delta crosses the wire as two 32-bit halves, so a NEGATIVE
	 * one and one past 32 bits are the cases that catch a bad split */
	w.delta = -1;
	w.vlen = 0;
	len = clfwd_op_build(buf, 0x42, &w);
	chk(clfwd_op_parse(buf, len, &r) == 1, "B1 parses");
	chk(r.delta == -1, "B1 a negative delta survives the 32-bit split");
	w.delta = 0x7FFFFFFFFFLL;                  /* needs both halves */
	len = clfwd_op_build(buf, 0x42, &w);
	clfwd_op_parse(buf, len, &r);
	chk(r.delta == 0x7FFFFFFFFFLL, "B2 a delta past 32 bits survives");
	w.delta = -0x7FFFFFFFFFLL;
	len = clfwd_op_build(buf, 0x42, &w);
	clfwd_op_parse(buf, len, &r);
	chk(r.delta == -0x7FFFFFFFFFLL, "B3 and a large NEGATIVE one");

	/* C. no value at all - a delete or an add says so by leaving none */
	w.delta = 0; w.op = 1; w.val = NULL; w.vlen = 0;
	len = clfwd_op_build(buf, 0x42, &w);
	chk(len == CLFWD_OP_HDR + 13, "C1 a valueless frame is header+col+key");
	chk(clfwd_op_parse(buf, len, &r) == 1, "C1 parses");
	chk(r.vlen == 0, "C1 and reports no value");
	chk(r.op == 1, "C1 with the op intact");

	/* D. truncation is REFUSED, never half-applied */
	w.val = "v"; w.vlen = 1;
	len = clfwd_op_build(buf, 0x42, &w);
	chk(clfwd_op_parse(buf, CLFWD_OP_HDR - 1, &r) == 0,
		"D1 a frame shorter than the header is refused");
	chk(clfwd_op_parse(buf, CLFWD_OP_HDR + 3, &r) == 0,
		"D2 one whose col+key do not fit is refused");
	chk(clfwd_op_parse(buf, len, &r) == 1, "D3 the whole frame is accepted");

	/* D4. an exactly-sized allocation, so an overread is a heap overflow
	 * ASan can see - a short length against a big buffer proves nothing */
	{
		unsigned char *tiny = malloc(CLFWD_OP_HDR - 1);

		chk(tiny != NULL, "D4 tiny buffer");
		if (tiny) {
			memset(tiny, 0, CLFWD_OP_HDR - 1);
			chk(clfwd_op_parse(tiny, CLFWD_OP_HDR - 1, &r) == 0,
				"D4 refused, and without reading past the buffer");
			free(tiny);
		}
	}

	/* E. the ack */
	aw.req = 0x01020304; aw.ok = 1; aw.newval = 987654321012345LL;
	chk(clfwd_ack_build(buf, 0x43, &aw) == CLFWD_ACK_LEN, "E1 ack length");
	chk(buf[0] == 0x43, "E1 its type is the caller's too");
	chk(clfwd_ack_parse(buf, CLFWD_ACK_LEN, &ar) == 1, "E2 it parses");
	chk(ar.req == 0x01020304, "E2 req");
	chk(ar.ok == 1, "E2 ok");
	chk(ar.newval == 987654321012345LL, "E2 newval past 32 bits");
	aw.newval = -5; aw.ok = 0;
	clfwd_ack_build(buf, 0x43, &aw);
	clfwd_ack_parse(buf, CLFWD_ACK_LEN, &ar);
	chk(ar.newval == -5, "E3 a NEGATIVE newval survives the split");
	chk(ar.ok == 0, "E3 and a failed outcome");
	chk(clfwd_ack_parse(buf, CLFWD_ACK_LEN - 1, &ar) == 0,
		"E4 a short ack is refused");

	/* ---- F. DEMOTE and TOMBSTONE (slice 3) ---------------------------
	 * Near-identical frames: the demote carries the winning node id and
	 * the tombstone does not.  Both were built at one end of cluster.c
	 * and decoded at the other. */
	{
		struct clfwd_key kw, kr;
		unsigned int winner = 0;

		kw.col = "orders"; kw.collen = 6;
		kw.key = "cust:7";  kw.klen = 6;

		/* F1. demote round trip */
		len = clfwd_demote_build(buf, 0x51, 1234, &kw);
		chk(len == CLFWD_DEMOTE_HDR + 12, "F1 demote length");
		chk(buf[0] == 0x51, "F1 its type is the caller's");
		chk(clfwd_demote_parse(buf, len, &winner, &kr) == 1, "F1 parses");
		chk(winner == 1234, "F1 the winning node id survives");
		chk(kr.collen == 6 && !memcmp(kr.col, "orders", 6), "F1 collection");
		chk(kr.klen == 6 && !memcmp(kr.key, "cust:7", 6), "F1 key");

		/* F2. tombstone round trip - two bytes shorter, no winner */
		len = clfwd_tomb_build(buf, 0x52, &kw);
		chk(len == CLFWD_TOMB_HDR + 12, "F2 tombstone is 2 bytes shorter");
		chk(clfwd_tomb_parse(buf, len, &kr) == 1, "F2 parses");
		chk(kr.collen == 6 && !memcmp(kr.col, "orders", 6), "F2 collection");
		chk(kr.klen == 6 && !memcmp(kr.key, "cust:7", 6), "F2 key");

		/* F3. the two are NOT interchangeable - parsing one as the
		 * other must not quietly succeed with shifted fields */
		len = clfwd_tomb_build(buf, 0x52, &kw);
		if (clfwd_demote_parse(buf, len, &winner, &kr))
			chk(kr.klen != 6 || memcmp(kr.key, "cust:7", 6) != 0,
				"F3 a tombstone read as a demote does not look valid");
		else
			chk(1, "F3 a tombstone read as a demote is refused");

		/* F4. truncation refused, both frames */
		len = clfwd_demote_build(buf, 0x51, 7, &kw);
		chk(clfwd_demote_parse(buf, CLFWD_DEMOTE_HDR - 1, &winner, &kr) == 0,
			"F4 a demote shorter than its header is refused");
		chk(clfwd_demote_parse(buf, CLFWD_DEMOTE_HDR + 3, &winner, &kr) == 0,
			"F4 one whose col+key do not fit is refused");
		len = clfwd_tomb_build(buf, 0x52, &kw);
		chk(clfwd_tomb_parse(buf, CLFWD_TOMB_HDR - 1, &kr) == 0,
			"F4 a tombstone shorter than its header is refused");
		chk(clfwd_tomb_parse(buf, CLFWD_TOMB_HDR + 3, &kr) == 0,
			"F4 and one whose col+key do not fit");

		/* F5. exactly-sized allocations, so an overread is a heap
		 * overflow ASan can see */
		{
			unsigned char *t1 = malloc(CLFWD_DEMOTE_HDR - 1);
			unsigned char *t2 = malloc(CLFWD_TOMB_HDR - 1);

			if (t1) {
				memset(t1, 0, CLFWD_DEMOTE_HDR - 1);
				chk(clfwd_demote_parse(t1, CLFWD_DEMOTE_HDR - 1,
					&winner, &kr) == 0, "F5 demote, tight buffer");
				free(t1);
			}
			if (t2) {
				memset(t2, 0, CLFWD_TOMB_HDR - 1);
				chk(clfwd_tomb_parse(t2, CLFWD_TOMB_HDR - 1, &kr) == 0,
					"F5 tombstone, tight buffer");
				free(t2);
			}
		}

		/* F6. an empty collection name, which the wire allows */
		kw.col = ""; kw.collen = 0;
		len = clfwd_tomb_build(buf, 0x52, &kw);
		chk(clfwd_tomb_parse(buf, len, &kr) == 1, "F6 an empty collection");
		chk(kr.collen == 0 && kr.klen == 6, "F6 and the key still lands");
	}

	/* ---- G. the JSON forward pair (slice 4) --------------------------
	 * The widest frame here: operation, path, flags, ttl and a 64-bit
	 * `by`, then four variable parts.  `by` and `newval` used to be a
	 * raw memcpy of the host's bytes; they pack explicitly now. */
	{
		struct clfwd_json jw, jr;
		struct clfwd_jack aw2, ar2;

		jw.req = 0xAABBCCDD; jw.node = 77; jw.jop = 3;
		jw.flags = CLFWD_J_NX | CLFWD_J_MKPATH;
		jw.ttl = 900;  jw.by = -1234567890123LL;
		jw.col  = "orders"; jw.collen = 6;
		jw.key  = "o:1";    jw.klen = 3;
		jw.path = "$.a.b";  jw.plen = 5;
		jw.val  = "{\"x\":1}"; jw.vlen = 7;

		len = clfwd_json_build(buf, 0x60, &jw);
		chk(len == CLFWD_JSON_HDR + 21, "G1 json frame length");
		chk(buf[0] == 0x60, "G1 the caller's type");
		chk(clfwd_json_parse(buf, len, &jr) == 1, "G2 it parses");
		chk(jr.req == 0xAABBCCDD, "G2 req");
		chk(jr.node == 77, "G2 node");
		chk(jr.jop == 3, "G2 jop");
		chk(jr.flags == (CLFWD_J_NX | CLFWD_J_MKPATH), "G2 flags");
		chk((jr.flags & CLFWD_J_XX) == 0, "G2 and only those flags");
		chk(jr.ttl == 900, "G2 ttl");
		chk(jr.by == -1234567890123LL,
			"G2 a large NEGATIVE by survives the 64-bit split");
		chk(jr.collen == 6 && !memcmp(jr.col, "orders", 6), "G2 collection");
		chk(jr.klen == 3 && !memcmp(jr.key, "o:1", 3), "G2 key");
		chk(jr.plen == 5 && !memcmp(jr.path, "$.a.b", 5), "G2 path");
		chk(jr.vlen == 7 && !memcmp(jr.val, "{\"x\":1}", 7), "G2 value");

		/* G3. the four variable parts must not overlap or shift */
		chk(jr.key == jr.col + 6, "G3 key follows col");
		chk(jr.path == jr.key + 3, "G3 path follows key");
		chk(jr.val == jr.path + 5, "G3 val follows path");

		/* G4. truncation refused */
		chk(clfwd_json_parse(buf, CLFWD_JSON_HDR - 1, &jr) == 0,
			"G4 shorter than the header");
		chk(clfwd_json_parse(buf, CLFWD_JSON_HDR + 10, &jr) == 0,
			"G4 declared parts do not fit");

		/* G4b. a length that fits WITHOUT the path but not WITH it.
		 * The earlier truncation cases were refused by the other
		 * terms whatever plen did, so dropping plen from the fit
		 * check went unnoticed.  col 6 + key 3 + val 7 = 16 fits in
		 * 18; adding path 5 needs 21 and does not.
		 * (mutation: omit j->plen from the fit check) */
		len = clfwd_json_build(buf, 0x60, &jw);
		chk(clfwd_json_parse(buf, CLFWD_JSON_HDR + 18, &jr) == 0,
			"G4b a frame whose PATH does not fit is refused");
		chk(clfwd_json_parse(buf, CLFWD_JSON_HDR + 21, &jr) == 1,
			"G4b and one where it just does is accepted");

		/* G5. the ack, and its fragment-only-on-success rule */
		aw2.req = 0x11223344; aw2.st = 0; aw2.jop = 3;
		aw2.newval = -9876543210LL; aw2.cnt = 42;
		aw2.frag = "frag!"; aw2.fraglen = 5;
		len = clfwd_jack_build(buf, 0x61, &aw2);
		chk(len == CLFWD_JACK_HDR + 5, "G5 ack carries the fragment");
		chk(clfwd_jack_parse(buf, len, &ar2) == 1, "G5 parses");
		chk(ar2.req == 0x11223344, "G5 req");
		chk(ar2.st == 0 && ar2.jop == 3, "G5 status and jop");
		chk(ar2.newval == -9876543210LL, "G5 a negative newval survives");
		chk(ar2.cnt == 42, "G5 count");
		chk(ar2.fraglen == 5 && !memcmp(ar2.frag, "frag!", 5), "G5 fragment");

		/* G6. a FAILED result carries no fragment, even if one is given */
		aw2.st = 7;
		len = clfwd_jack_build(buf, 0x61, &aw2);
		chk(len == CLFWD_JACK_HDR,
			"G6 a failed ack drops the fragment");
		chk(clfwd_jack_parse(buf, len, &ar2) == 1, "G6 parses");
		chk(ar2.fraglen == 0 && ar2.frag == NULL,
			"G6 and reports none");
		chk(ar2.st == 7, "G6 with the failure status intact");

		/* G6b. an ack whose declared fragment runs past the datagram.
		 * G7 only covers one shorter than the HEADER, which the
		 * header check catches whatever the fit check does.
		 * (mutation: remove the ack fit check) */
		aw2.st = 0; aw2.fraglen = 5; aw2.frag = "frag!";
		len = clfwd_jack_build(buf, 0x61, &aw2);
		chk(clfwd_jack_parse(buf, CLFWD_JACK_HDR + 2, &ar2) == 0,
			"G6b an ack declaring more fragment than it carries");
		chk(clfwd_jack_parse(buf, CLFWD_JACK_HDR + 5, &ar2) == 1,
			"G6b and the whole one is accepted");

		/* G7. truncation, tight allocations */
		{
			unsigned char *t = malloc(CLFWD_JACK_HDR - 1);

			if (t) {
				memset(t, 0, CLFWD_JACK_HDR - 1);
				chk(clfwd_jack_parse(t, CLFWD_JACK_HDR - 1, &ar2) == 0,
					"G7 a short ack, tight buffer");
				free(t);
			}
		}
	}

	/* ---- the golden wire vectors -------------------------------- */
	{
		unsigned char g[512];
		const char *col = "sessions", *key = "abc-123";
		const char *val = "{\"v\":42}", *path = "$.a.b";

		{
			struct clfwd_op f;
			memset(&f, 0, sizeof f);
			f.req = 0xdeadbeef; f.node = 4097; f.op = 3;
			f.ttl_rel = 900; f.delta = -1234567890123LL;
			f.col = col; f.collen = 8; f.key = key; f.klen = 7;
			f.val = val; f.vlen = 8;
			check_golden(g, clfwd_op_build(g, 17, &f), "FWD_OP");
		}
		{
			struct clfwd_ack a;
			memset(&a, 0, sizeof a);
			a.req = 0x01234567; a.ok = 1; a.newval = -9876543210LL;
			check_golden(g, clfwd_ack_build(g, 18, &a), "FWD_ACK");
		}
		{
			struct clfwd_key k;
			k.col = col; k.collen = 8; k.key = key; k.klen = 7;
			check_golden(g, clfwd_demote_build(g, 20, 4097, &k), "DEMOTE");
			check_golden(g, clfwd_tomb_build(g, 22, &k), "TOMBSTONE");
		}
		{
			struct clfwd_json j;
			memset(&j, 0, sizeof j);
			j.req = 0x89abcdef; j.node = 730; j.jop = 2; j.flags = 5;
			j.ttl = 1200; j.by = 4242424242LL;
			j.col = col; j.collen = 8; j.key = key; j.klen = 7;
			j.path = path; j.plen = 5; j.val = val; j.vlen = 8;
			check_golden(g, clfwd_json_build(g, 16, &j), "FWD_JSON");
		}
		{
			struct clfwd_jack a;
			memset(&a, 0, sizeof a);
			a.req = 0x0f0f0f0f; a.st = 1; a.jop = 2;
			a.newval = -55555LL; a.cnt = 9; a.frag = val; a.fraglen = 8;
			check_golden(g, clfwd_jack_build(g, 24, &a), "FWD_JACK");
		}
	}

	printf("clfwdtest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
