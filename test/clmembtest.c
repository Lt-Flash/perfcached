/*
 * clmembtest.c - the membership keepalive frames (M12 slice 2).
 *
 * This is a DIFFERENTIAL test, not a round-trip one, and the difference
 * matters.  A round trip proves clmemb agrees with itself, which it
 * would do just as happily with every offset shifted by one.  What has
 * to hold is that it agrees with the layout that is already on the wire
 * - because MASTER_ALIVE is the frame master-down detection runs on, and
 * a fleet mid-upgrade has both codecs on it at once.
 *
 * So ref_malive() and ref_alive() below are the byte writes copied
 * VERBATIM out of send_master_alive() and send_alive() as they stood
 * before the extraction - raw host-endian memcpy for the 64-bit fields
 * included - and the parse side is checked field by field against reads
 * taken straight from the frame at every length from 0 to full.  When
 * the handlers stopped carrying those offsets, this file became the only
 * remaining copy of the pre-extraction layout, which is what makes it a
 * golden reference rather than a restatement.
 *
 * The digest values include 0x8000000000000000 and ~0 on purpose: those
 * are where a host-endian memcpy and an explicit little-endian pack
 * would diverge if this daemon ever ran big-endian.
 *
 * Build: cc -o clmembtest test/clmembtest.c src/clmemb.o
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <arpa/inet.h>
#include "../src/clmemb.h"
#include "../src/clcodec.h"
#define p16 pc_p16
#define p32 pc_p32
#define g16 pc_g16
#define g32 pc_g32

static int bad, checks;
#define CHK(c, w) do { checks++; if (!(c)) { printf("  FAIL %s\n", w); bad++; } } while (0)

/* ---------- reference encoders, verbatim from cluster.c ---------- */
static void ref_malive(unsigned char *msg, unsigned char type, int node,
	int members, uint64_t d, uint32_t freeb, uint32_t total, uint32_t live,
	int mode, int eager, uint64_t cd, uint32_t term)
{
	msg[0] = type;
	p16(msg + 1, (uint16_t)node);
	p16(msg + 3, (uint16_t)members);
	memcpy(msg + 5, &d, 8);
	p32(msg + 13, freeb);
	p32(msg + 17, total);
	p32(msg + 21, live);
	msg[25] = (unsigned char)mode;
	msg[26] = (unsigned char)eager;
	memcpy(msg + 27, &cd, 8);
	p32(msg + 35, term);
}

static void ref_alive(unsigned char *msg, unsigned char type, int node,
	uint32_t freeb, uint32_t total, uint32_t live, int mode, int eager,
	uint64_t d, int cliport, const unsigned char *ident, uint32_t incarn,
	uint32_t entries, unsigned long long lc, int nstate, int tier,
	int respport, int startkind, int httpport, uint32_t uptime)
{
	msg[0] = type;
	p16(msg + 1, (uint16_t)node);
	p32(msg + 3, freeb);
	p32(msg + 7, total);
	p32(msg + 11, live);
	msg[15] = (unsigned char)mode;
	msg[16] = (unsigned char)eager;
	memcpy(msg + 17, &d, 8);
	p16(msg + 25, (uint16_t)cliport);
	memcpy(msg + 27, ident, 16);
	p32(msg + 43, incarn);
	p32(msg + 47, entries);
	memcpy(msg + 51, &lc, 8);
	msg[59] = (unsigned char)nstate;
	msg[60] = (unsigned char)tier;
	p16(msg + 61, (uint16_t)respport);
	msg[63] = (unsigned char)startkind;
	p16(msg + 64, (uint16_t)httpport);
	p32(msg + 66, uptime);
}


/* ---- reference encoders for the handshake, verbatim from cluster.c's
 * send_join_req / send_assign / send_join_rej / send_join_wait /
 * send_goodbye as they stood before the extraction ---- */
static void ref_joinreq(unsigned char *msg, unsigned char type, uint64_t tok,
	int mode, int eager, uint64_t d, const unsigned char *ident,
	int proposed, uint32_t incarn, int wal)
{
	msg[0] = type;
	memcpy(msg + 1, &tok, 8);
	msg[9] = (unsigned char)mode;
	msg[10] = (unsigned char)eager;
	memcpy(msg + 11, &d, 8);
	memcpy(msg + 19, ident, 16);
	p16(msg + 35, (uint16_t)proposed);
	p32(msg + 37, incarn);
	msg[41] = (unsigned char)wal;
}

static void ref_assign_hdr(unsigned char *msg, unsigned char type, uint64_t tok,
	int your_id, int master_id, int cnt)
{
	msg[0] = type;
	memcpy(msg + 1, &tok, 8);
	p16(msg + 9, (uint16_t)your_id);
	p16(msg + 11, (uint16_t)master_id);
	p16(msg + 13, (uint16_t)cnt);
}

static void ref_assign_rec(unsigned char *msg, int node, uint32_t araw,
	uint16_t praw, uint32_t f, uint32_t t, uint32_t l)
{
	p16(msg, (uint16_t)node);
	memcpy(msg + 2, &araw, 4);
	memcpy(msg + 6, &praw, 2);
	p32(msg + 8, f);
	p32(msg + 12, t);
	p32(msg + 16, l);
}

int main(void)
{
	static const uint64_t D[] = { 0, 1, 0x00000000ffffffffULL,
		0x0123456789abcdefULL, 0x8000000000000000ULL, ~0ULL };
	unsigned char ident[16], a[80], b[80];
	struct clmemb_alive A;
	struct clmemb_malive M;
	size_t i, n;
	int k;

	for (k = 0; k < 16; k++) ident[k] = (unsigned char)(k * 37 + 5);
	printf("=== clmembtest ===\n");

	/* ---- encoders must be byte-identical ---- */
	for (i = 0; i < sizeof D / sizeof D[0]; i++) {
		memset(a, 0xAA, sizeof a); memset(b, 0xAA, sizeof b);
		ref_malive(a, 9, 4097, 3, D[i], 1234, 5678, 91011, 2, 1,
			D[(i + 1) % 6], 0xdeadbeef);
		{
			struct clmemb_malive in = { 0 };
			in.node = 4097; in.members = 3; in.member_digest = D[i];
			in.free_mb = 1234; in.total_mb = 5678; in.live_kb = 91011;
			in.mode = 2; in.eager = 1; in.cfg_digest = D[(i + 1) % 6];
			in.term = 0xdeadbeef;
			CHK(clmemb_malive_write(b, sizeof b, 9, &in) == CLMEMB_MALIVE_LEN,
				"malive length");
		}
		CHK(memcmp(a, b, CLMEMB_MALIVE_LEN) == 0, "MASTER_ALIVE bytes");

		memset(a, 0xAA, sizeof a); memset(b, 0xAA, sizeof b);
		ref_alive(a, 8, 65535, 11, 22, 33, 3, 1, D[i], 6379, ident,
			0x11223344, 999, D[(i + 2) % 6], 2, 1, 6380, 3, 8080,
			123456);
		{
			struct clmemb_alive in = { 0 };
			in.node = 65535; in.free_mb = 11; in.total_mb = 22;
			in.live_kb = 33; in.mode = 3; in.eager = 1;
			in.cfg_digest = D[i]; in.client_port = 6379;
			in.ident = ident; in.incarn = 0x11223344; in.entries = 999;
			in.lamport = D[(i + 2) % 6]; in.nstate = 2; in.mem_tier = 1;
			in.resp_port = 6380; in.start_kind = 3; in.http_port = 8080;
			in.uptime_s = 123456;
			CHK(clmemb_alive_write(b, sizeof b, 8, &in) == CLMEMB_ALIVE_LEN,
				"alive length");
		}
		CHK(memcmp(a, b, CLMEMB_ALIVE_LEN) == 0, "ALIVE bytes");
	}

	/* ---- parsers must agree at EVERY length ---- */
	ref_alive(a, 8, 65535, 11, 22, 33, 3, 1, 0x0123456789abcdefULL, 6379,
		ident, 0x11223344, 999, 0xfedcba9876543210ULL, 2, 1, 6380, 3,
		8080, 123456);
	for (n = 0; n <= CLMEMB_ALIVE_LEN; n++) {
		int ok = clmemb_alive_parse(a, n, &A);
		uint64_t rd; unsigned long long rlc;
		char w[64];

		snprintf(w, sizeof w, "ALIVE n=%zu", n);
		if (n < 7) { CHK(!ok, w); continue; }
		CHK(ok, w);
		CHK(A.node == g16(a + 1) && A.free_mb == g32(a + 3), w);
		CHK(!!(A.have & CLMEMB_A_MEM) == (n >= 15), w);
		if (n >= 15) CHK(A.total_mb == g32(a + 7) &&
			A.live_kb == g32(a + 11), w);
		CHK(!!(A.have & CLMEMB_A_CFG) == (n >= 25), w);
		if (n >= 25) { memcpy(&rd, a + 17, 8);
			CHK(A.mode == a[15] && A.eager == a[16] &&
				A.cfg_digest == rd, w); }
		CHK(!!(A.have & CLMEMB_A_CLIPORT) == (n >= 27), w);
		if (n >= 27) CHK(A.client_port == g16(a + 25), w);
		CHK(!!(A.have & CLMEMB_A_IDENT) == (n >= 47), w);
		if (n >= 47) CHK(A.ident == a + 27 &&
			memcmp(A.ident, ident, 16) == 0 &&
			A.incarn == g32(a + 43), w);
		CHK(!!(A.have & CLMEMB_A_ENTRIES) == (n >= 51), w);
		if (n >= 51) CHK(A.entries == g32(a + 47), w);
		CHK(!!(A.have & CLMEMB_A_LAMPORT) == (n >= 59), w);
		if (n >= 59) { memcpy(&rlc, a + 51, 8);
			CHK(A.lamport == rlc, w); }
		CHK(!!(A.have & CLMEMB_A_STATE) == (n >= 60), w);
		if (n >= 60) CHK(A.nstate == a[59], w);
		CHK(!!(A.have & CLMEMB_A_TIER) == (n >= 61), w);
		if (n >= 61) CHK(A.mem_tier == a[60], w);
		CHK(!!(A.have & CLMEMB_A_RESPPORT) == (n >= 63), w);
		if (n >= 63) CHK(A.resp_port == g16(a + 61), w);
		CHK(!!(A.have & CLMEMB_A_STARTKIND) == (n >= 64), w);
		if (n >= 64) CHK(A.start_kind == a[63], w);
		CHK(!!(A.have & CLMEMB_A_HTTP) == (n >= 70), w);
		if (n >= 70) CHK(A.http_port == g16(a + 64) &&
			A.uptime_s == g32(a + 66), w);
	}

	ref_malive(b, 9, 4097, 3, 0x0123456789abcdefULL, 1234, 5678, 91011,
		2, 1, 0xfedcba9876543210ULL, 0xdeadbeef);
	for (n = 0; n <= CLMEMB_MALIVE_LEN; n++) {
		int ok = clmemb_malive_parse(b, n, &M);
		uint64_t rd; char w[64];

		snprintf(w, sizeof w, "MASTER_ALIVE n=%zu", n);
		if (n < 17) { CHK(!ok, w); continue; }
		CHK(ok, w);
		memcpy(&rd, b + 5, 8);
		CHK(M.node == g16(b + 1) && M.members == g16(b + 3) &&
			M.member_digest == rd && M.free_mb == g32(b + 13), w);
		CHK(!!(M.have & CLMEMB_M_MEM) == (n >= 25), w);
		if (n >= 25) CHK(M.total_mb == g32(b + 17) &&
			M.live_kb == g32(b + 21), w);
		CHK(!!(M.have & CLMEMB_M_CFG) == (n >= 35), w);
		if (n >= 35) { memcpy(&rd, b + 27, 8);
			CHK(M.mode == b[25] && M.eager == b[26] &&
				M.cfg_digest == rd, w); }
		CHK(!!(M.have & CLMEMB_M_TERM) == (n >= 39), w);
		if (n >= 39) CHK(M.term == g32(b + 35), w);
	}

	/* ---- JOIN_REQ: bytes, then the out-of-order gates -------------- */
	for (i = 0; i < 6; i++) {
		memset(a, 0xAA, sizeof a); memset(b, 0xAA, sizeof b);
		ref_joinreq(a, 4, D[i], 3, 1, D[(i + 3) % 6], ident, 4097,
			0x11223344, 2);
		{
			struct clmemb_joinreq in = { 0 };
			in.tok = D[i]; in.mode = 3; in.eager = 1;
			in.cfg_digest = D[(i + 3) % 6]; in.ident = ident;
			in.proposed = 4097; in.incarn = 0x11223344; in.wal = 2;
			CHK(clmemb_joinreq_write(b, sizeof b, 4, &in) == CLMEMB_JOINREQ_LEN,
				"joinreq length");
		}
		CHK(memcmp(a, b, CLMEMB_JOINREQ_LEN) == 0, "JOIN_REQ bytes");
	}
	ref_joinreq(a, 4, 0x0123456789abcdefULL, 3, 1, 0xfedcba9876543210ULL,
		ident, 4097, 0x11223344, 2);
	for (n = 0; n <= CLMEMB_JOINREQ_LEN; n++) {
		struct clmemb_joinreq J;
		int ok = clmemb_joinreq_parse(a, n, &J);
		uint64_t rd; char w[48];

		snprintf(w, sizeof w, "JOIN_REQ n=%zu", n);
		if (n < 9) { CHK(!ok, w); continue; }
		CHK(ok, w);
		{ uint64_t rt; memcpy(&rt, a + 1, 8); CHK(J.tok == rt, w); }
		/* the gates are NOT in offset order and must stay that way */
		CHK(!!(J.have & CLMEMB_J_CFG) == (n >= 19), w);
		if (n >= 19) { memcpy(&rd, a + 11, 8);
			CHK(J.mode == a[9] && J.eager == a[10] &&
				J.cfg_digest == rd, w); }
		CHK(!!(J.have & CLMEMB_J_IDENT) == (n >= 35), w);
		if (n >= 35) CHK(J.ident == a + 19 &&
			memcmp(J.ident, ident, 16) == 0, w);
		CHK(!!(J.have & CLMEMB_J_PROP) == (n >= 37), w);
		if (n >= 37) CHK(J.proposed == g16(a + 35), w);
		CHK(!!(J.have & CLMEMB_J_INCARN) == (n >= 41), w);
		if (n >= 41) CHK(J.incarn == g32(a + 37), w);
		CHK(!!(J.have & CLMEMB_J_WAL) == (n >= 42), w);
		if (n >= 42) CHK(J.wal == a[41], w);
	}

	/* ---- ASSIGN: header, records, and the bound on a lying count --- */
	{
		unsigned char x[15 + 3 * 20], y[15 + 3 * 20];
		struct clmemb_assign A2;
		struct clmemb_member M2;
		/* network-order values with a byte order a LE pack would
		 * visibly reverse: 10.99.0.11 and port 7100 */
		uint32_t araw = htonl(0x0A63000B);
		uint16_t praw = htons(7100);

		memset(x, 0xAA, sizeof x); memset(y, 0xAA, sizeof y);
		ref_assign_hdr(x, 5, 0x0123456789abcdefULL, 4097, 730, 3);
		for (k = 0; k < 3; k++)
			ref_assign_rec(x + 15 + k * 20, 700 + k,
				htonl(0x0A63000BU + (unsigned)k),
				htons((unsigned short)(7100 + k)),
				11u + k, 22u + k, 33u + k);
		{
			struct clmemb_assign in;
			in.tok = 0x0123456789abcdefULL; in.your_id = 4097;
			in.master_id = 730; in.count = 0;
			CHK(clmemb_assign_hdr_write(y, sizeof y, 5, &in) ==
				CLMEMB_ASSIGN_HDR, "assign hdr length");
		}
		for (k = 0; k < 3; k++) {
			struct clmemb_member m;
			memset(&m, 0, sizeof m);
			m.node = 700 + k;
			m.addr.s_addr = htonl(0x0A63000BU + (unsigned)k);
			m.port = htons((unsigned short)(7100 + k));
			m.free_mb = 11u + k; m.total_mb = 22u + k;
			m.live_kb = 33u + k;
			CHK(clmemb_member_write(y + 15 + k * 20, sizeof y - 15 - k * 20, &m) ==
				CLMEMB_ASSIGN_REC, "member length");
		}
		clmemb_assign_count_set(y, 3);
		CHK(memcmp(x, y, sizeof x) == 0, "ASSIGN bytes");

		CHK(clmemb_assign_parse(x, sizeof x, &A2) &&
			A2.your_id == 4097 && A2.master_id == 730 &&
			A2.count == 3, "ASSIGN header parses");
		CHK(!clmemb_assign_parse(x, 14, &A2), "ASSIGN under 15 refused");
		for (k = 0; k < 3; k++) {
			CHK(clmemb_member_at(x, sizeof x, k, &M2), "record present");
			CHK(M2.node == 700 + k &&
				M2.addr.s_addr == htonl(0x0A63000BU + (unsigned)k) &&
				M2.port == htons((unsigned short)(7100 + k)) &&
				M2.free_mb == 11u + (unsigned)k,
				"record fields, address NOT byte-swapped");
		}
		CHK(!clmemb_member_at(x, sizeof x, 3, &M2), "one past the end");
		CHK(!clmemb_member_at(x, sizeof x, -1, &M2), "negative index");
		/* a count of 3 in a frame holding two: the bound is the LENGTH */
		CHK(clmemb_member_at(x, 15 + 2 * 20, 1, &M2), "2 fit in 55 bytes");
		CHK(!clmemb_member_at(x, 15 + 2 * 20, 2, &M2),
			"a lying count cannot read past the frame");
		CHK(!clmemb_member_at(x, 15 + 2 * 20 - 1, 1, &M2),
			"a record one byte short is not present");
		CHK(araw != 0x0A63000BU || praw != 7100,
			"the test values are genuinely network order");
	}

	/* ---- JOIN_WAIT / JOIN_REJ / GOODBYE ---------------------------- */
	{
		unsigned char x[10], y[10];
		uint64_t tok = 0x0123456789abcdefULL, rt;
		int reason, hr;

		memset(x, 0xAA, sizeof x); memset(y, 0xAA, sizeof y);
		x[0] = 6; memcpy(x + 1, &tok, 8);
		CHK(clmemb_tok_write(y, sizeof y, 6, tok) == CLMEMB_TOK_LEN, "tok length");
		CHK(memcmp(x, y, 9) == 0, "JOIN_WAIT bytes");

		memset(x, 0xAA, sizeof x); memset(y, 0xAA, sizeof y);
		x[0] = 7; memcpy(x + 1, &tok, 8); x[9] = 3;
		CHK(clmemb_rej_write(y, sizeof y, 7, tok, 3) == CLMEMB_REJ_LEN, "rej length");
		CHK(memcmp(x, y, 10) == 0, "JOIN_REJ bytes");

		CHK(!clmemb_tok_parse(x, 8, &rt), "a token under 9 is refused");
		CHK(clmemb_tok_parse(x, 9, &rt) && rt == tok, "token parses");
		CHK(clmemb_rej_parse(x, 10, &rt, &reason, &hr) && hr &&
			reason == 3, "JOIN_REJ reason present at 10");
		CHK(clmemb_rej_parse(x, 9, &rt, &reason, &hr) && !hr &&
			reason == 0, "a 9-byte JOIN_REJ has no reason");

		memset(x, 0xAA, sizeof x); memset(y, 0xAA, sizeof y);
		x[0] = 8; p16(x + 1, 4097);
		CHK(clmemb_goodbye_write(y, sizeof y, 8, 4097) == CLMEMB_GOODBYE_LEN,
			"goodbye length");
		CHK(memcmp(x, y, 3) == 0, "GOODBYE bytes");
	}

	/* ---- short frames: nothing written, nothing read past the end --
	 *
	 * These exist because the cursor rewrite briefly had both faults.
	 * A parser that reads its minimum field by field leaves the earlier
	 * ones in *out behind a "no" return; and a cursor advanced past its
	 * end computes a NEGATIVE remaining length that becomes SIZE_MAX
	 * when cast, so every later fits() says yes.  Run under ASan the
	 * second one is a heap overread on a zero-length frame.
	 *
	 * The buffers are sized EXACTLY to the short length - a short `n`
	 * against a large buffer is not a short buffer, and ASan sees
	 * nothing. */
	{
		struct clmemb_alive A3;
		struct clmemb_malive M3;
		struct clmemb_joinreq J3;
		struct clmemb_assign S3;
		uint64_t t3;
		int r3, hr3;
		size_t z;

		for (z = 0; z < 7; z++) {
			unsigned char *tight = malloc(z ? z : 1);
			char w[48];

			memset(tight, 0x5A, z ? z : 1);
			snprintf(w, sizeof w, "ALIVE short n=%zu", z);
			memset(&A3, 0x7F, sizeof A3);
			CHK(!clmemb_alive_parse(tight, z, &A3), w);
			CHK(A3.node == 0x7F7F7F7F && A3.free_mb == 0x7F7F7F7FU,
				"a refused ALIVE leaves *out untouched");
			free(tight);
		}
		for (z = 0; z < 17; z++) {
			unsigned char *tight = malloc(z ? z : 1);

			memset(tight, 0x5A, z ? z : 1);
			memset(&M3, 0x7F, sizeof M3);
			CHK(!clmemb_malive_parse(tight, z, &M3),
				"MASTER_ALIVE under 17 refused");
			CHK(M3.node == 0x7F7F7F7F,
				"a refused MASTER_ALIVE leaves *out untouched");
			free(tight);
		}
		for (z = 0; z < 9; z++) {
			unsigned char *tight = malloc(z ? z : 1);

			memset(tight, 0x5A, z ? z : 1);
			memset(&J3, 0x7F, sizeof J3);
			CHK(!clmemb_joinreq_parse(tight, z, &J3),
				"JOIN_REQ under 9 refused");
			CHK(!clmemb_tok_parse(tight, z, &t3),
				"a token frame under 9 refused");
			CHK(!clmemb_rej_parse(tight, z, &t3, &r3, &hr3),
				"JOIN_REJ under 9 refused");
			free(tight);
		}
		for (z = 0; z < 15; z++) {
			unsigned char *tight = malloc(z ? z : 1);

			memset(tight, 0x5A, z ? z : 1);
			memset(&S3, 0x7F, sizeof S3);
			CHK(!clmemb_assign_parse(tight, z, &S3),
				"ASSIGN under 15 refused");
			CHK(S3.your_id == 0x7F7F7F7F,
				"a refused ASSIGN leaves *out untouched");
			free(tight);
		}
	}

	printf("clmembtest: %d checks, %d failed\n", checks, bad);
	return bad ? 1 : 0;
}
