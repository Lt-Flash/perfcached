/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * clmigtest.c - the migration in-flight window (M10 slice 1).
 *
 * The window is what makes a lost migration datagram accountable in
 * RECORDS rather than datagrams, and what decides when one is re-sent.
 * Both are testable with no socket at all, because clmig_retransmit()
 * hands the caller the offsets and this file supplies its own sender.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/clcodec.h"
#include "../src/clmig.h"

static int pass, fail;
static void chk(int cond, const char *what)
{
	if (cond) { pass++; return; }
	fail++;
	printf("FAIL: %s\n", what);
}

/* the fake sender: records every offset it is asked to re-send */
static size_t sent[64];
static int nsent;
static void cap(void *ctx, size_t off)
{
	(void)ctx;
	if (nsent < (int)(sizeof sent / sizeof sent[0]))
		sent[nsent++] = off;
}

int main(void)
{
	struct clmig m;
	int i, n;
	uint16_t r;

	printf("=== clmigtest ===\n");
	memset(&m, 0, sizeof m);
	clmig_reset(&m);

	/* A. the window holds exactly CLMIG_WINDOW*2 and then refuses */
	for (i = 0; i < CLMIG_WINDOW * 2; i++)
		chk(clmig_track(&m, (uint32_t)(i + 1), (uint16_t)(i + 1),
			(size_t)(i * 100), 1000) == 1,
			"A1 the window accepts up to CLMIG_WINDOW*2");
	chk(clmig_track(&m, 999, 1, 0, 1000) == 0,
		"A2 one past the window is REFUSED, so the caller drains");
	chk(clmig_inflight(&m) == CLMIG_WINDOW * 2, "A3 inflight counts them all");

	/* B. lost records are summed in RECORDS, not datagrams */
	{
		unsigned long want = 0;

		for (i = 0; i < CLMIG_WINDOW * 2; i++)
			want += (unsigned long)(i + 1);
		chk(clmig_lost_records(&m) == want,
			"B1 outstanding work is counted in records, not datagrams");
	}

	/* C. an ack retires one, returns its records, and is not repeatable */
	r = clmig_ack(&m, 3);
	chk(r == 3, "C1 the ack returns the records that datagram carried");
	chk(clmig_inflight(&m) == CLMIG_WINDOW * 2 - 1, "C2 and retires it");
	chk(clmig_ack(&m, 3) == 0, "C3 a DUPLICATE ack retires nothing");
	chk(clmig_ack(&m, 12345) == 0, "C4 an ack for a datagram we never sent");

	/* C5. req 0 marks a free slot, so an ack of 0 must not retire one */
	{
		int before = clmig_inflight(&m);

		chk(clmig_ack(&m, 0) == 0, "C5 an ack for req 0 retires nothing");
		chk(clmig_inflight(&m) == before,
			"C5 and does not consume a free slot");
	}

	/* D. retransmit respects the timer */
	nsent = 0;
	n = clmig_retransmit(&m, 1000 + CLMIG_RETX_MS - 1, cap, NULL);
	chk(n == 0 && nsent == 0,
		"D1 nothing is re-sent before CLMIG_RETX_MS has passed");
	n = clmig_retransmit(&m, 1000 + CLMIG_RETX_MS, cap, NULL);
	chk(n == CLMIG_WINDOW * 2 - 1,
		"D2 at the deadline every outstanding datagram is re-sent");
	chk(nsent == n, "D3 and the caller was handed one offset each");

	/* D4. the offsets handed back are the ones that were tracked */
	{
		int ok = 1;

		for (i = 0; i < nsent; i++)
			if (sent[i] % 100 != 0 || sent[i] > (size_t)(CLMIG_WINDOW * 2 * 100))
				ok = 0;
		chk(ok, "D4 the offsets are the ones tracked, not indices");
	}

	/* E. a datagram is given up on after CLMIG_RETX attempts */
	{
		long long t = 1000 + CLMIG_RETX_MS;

		for (i = 1; i < CLMIG_RETX; i++) {
			t += CLMIG_RETX_MS;
			clmig_retransmit(&m, t, cap, NULL);
		}
		nsent = 0;
		t += CLMIG_RETX_MS;
		n = clmig_retransmit(&m, t, cap, NULL);
		chk(n == 0 && nsent == 0,
			"E1 after CLMIG_RETX re-sends a datagram is given up on");
		chk(clmig_lost_records(&m) > 0,
			"E2 and its records are still counted as lost");
	}

	/* F. reset clears the window */
	clmig_reset(&m);
	chk(clmig_inflight(&m) == 0 && clmig_lost_records(&m) == 0,
		"F1 reset empties the window");

	/* G. a freed slot is reusable */
	chk(clmig_track(&m, 77, 5, 700, 2000) == 1, "G1 track after reset");
	chk(clmig_ack(&m, 77) == 5, "G2 ack returns its records");
	chk(clmig_track(&m, 78, 6, 800, 2000) == 1, "G3 the slot is reusable");

	/* H. a REUSED slot gets a fresh retry budget.  Without this, a slot
	 * whose previous occupant spent its re-sends would be given up on
	 * immediately - the datagram would go out once and never again.
	 * (mutation: drop `retx = 0` from clmig_track) */
	clmig_reset(&m);
	clmig_track(&m, 10, 1, 0, 1000);
	{
		long long t = 1000;
		int rounds = 0;

		while (rounds < CLMIG_RETX) {
			t += CLMIG_RETX_MS;
			rounds += clmig_retransmit(&m, t, cap, NULL);
		}
		chk(clmig_retransmit(&m, t + CLMIG_RETX_MS, cap, NULL) == 0,
			"H1 that slot has spent its budget");
		clmig_ack(&m, 10);
		chk(clmig_track(&m, 11, 1, 0, t) == 1, "H2 the slot is reused");
		nsent = 0;
		chk(clmig_retransmit(&m, t + CLMIG_RETX_MS, cap, NULL) == 1,
			"H3 and the NEW occupant gets a fresh retry budget");
	}

	/* I. one sweep, one re-send.  Without refreshing sent_ms every sweep
	 * would re-send the same datagram again, flooding the peer at tick
	 * rate instead of once per CLMIG_RETX_MS.
	 * (mutation: drop `sent_ms = now` from clmig_retransmit) */
	clmig_reset(&m);
	clmig_track(&m, 20, 1, 0, 1000);
	{
		long long t = 1000 + CLMIG_RETX_MS;

		nsent = 0;
		chk(clmig_retransmit(&m, t, cap, NULL) == 1, "I1 due: re-sent once");
		chk(clmig_retransmit(&m, t, cap, NULL) == 0,
			"I2 a second sweep at the SAME instant re-sends nothing");
		chk(clmig_retransmit(&m, t + CLMIG_RETX_MS - 1, cap, NULL) == 0,
			"I3 nor before another CLMIG_RETX_MS has passed");
		chk(nsent == 1, "I4 exactly one datagram left in all that");
	}

	/* ---- the gather buffer (slice 2) ---------------------------------
	 * One group becomes one datagram.  The buffer owns the framing and
	 * the sizes; the caller owns what a header and a record contain. */
	{
		struct clmig_buf g;
		unsigned char *hdr = NULL;
		const unsigned char *gp;
		unsigned int gn;
		size_t off;
		int rc, k, groups;

		chk(clmig_buf_init(&g, 4096) == 0, "J1 the buffer allocates");

		/* J2. the first record OPENS a group; the second reuses it */
		rc = clmig_open_for(&g, 100, 9, 1000, &hdr);
		chk(rc == 1, "J2 the first record opens a group");
		chk(hdr != NULL, "J2 and hands the caller its header to stamp");
		memset(hdr, 0, 9);
		clmig_record_done(&g, 100, 7);
		rc = clmig_open_for(&g, 100, 9, 1000, &hdr);
		chk(rc == 0, "J3 the next record REUSES the open group");
		clmig_record_done(&g, 100, 7);

		/* J4. the group's length and record count track what went in */
		chk(pc_g16(g.b + 0) == 9 + 200,
			"J4 the group length counts its header and both records");
		chk(pc_g16(g.b + 2 + 7) == 2, "J4 and its record count is 2");

		/* J5. a record that would overfill the group starts a new one */
		rc = clmig_open_for(&g, 900, 1000, 1000, &hdr);
		chk(rc == 1, "J5 a record past the gather cap opens a NEW group");
		chk(pc_g16(g.b + 0) == 9 + 200,
			"J5 and the closed group keeps its length");

		/* J6. the batch cap refuses rather than overrunning */
		clmig_buf_reset(&g);
		chk(clmig_open_for(&g, 5000, 9, 100000, &hdr) == -1,
			"J6 a record larger than the batch is refused");
		chk(g.len == 0, "J6 and nothing was written");

		/* J6b. the batch cap must also stop an APPEND into a group
		 * that is already open - not just the opening of a new one.
		 * (mutation: drop the second cap check in clmig_open_for) */
		clmig_buf_reset(&g);
		rc = clmig_open_for(&g, 100, 9, 100000, &hdr);
		chk(rc == 1, "J6b a group opens");
		memset(hdr, 0, 9);
		clmig_record_done(&g, 100, 7);
		/* the gather cap is huge, so the group stays open; the BATCH
		 * cap (4096) is what must refuse this one */
		chk(clmig_open_for(&g, 4000, 9, 100000, &hdr) == -1,
			"J6b an append past the BATCH cap is refused too");
		chk(g.len == 2 + 9 + 100,
			"J6b and the refused append wrote nothing");

		/* J7. round trip: pack N records, walk them back */
		clmig_buf_reset(&g);
		for (k = 0; k < 8; k++) {
			rc = clmig_open_for(&g, 100, 9, 250, &hdr);
			chk(rc >= 0, "J7 packing succeeds");
			if (rc == 1)
				memset(hdr, 0, 9);
			clmig_record_done(&g, 100, 7);
		}
		off = 0; groups = 0; k = 0;
		/* BOUNDED.  A broken walk (one that does not advance past the
		 * len2 prefix, say) drifts onto garbage lengths and spins for
		 * ever; a test that hangs reports nothing at all, so it must
		 * fail instead.  Found by mutation - the unbounded version
		 * took the whole run down with it. */
		while (clmig_group_walk(&g, &off, &gp, &gn)) {
			groups++;
			k += pc_g16(gp + 7);
			chk(gn >= 9, "J7 every group carries at least its header");
			if (groups > 64) {
				chk(0, "J7 the walk does not terminate");
				break;
			}
		}
		chk(k == 8, "J7 the walk finds every record that was packed");
		chk(groups > 1, "J7 and a 250-byte gather cap made several groups");
		chk(off >= g.len, "J7 the walk consumed the whole buffer");

		/* J8. reset empties it, and the buffer is reusable */
		clmig_buf_reset(&g);
		off = 0;
		chk(!clmig_group_walk(&g, &off, &gp, &gn), "J8 reset empties it");
		chk(clmig_open_for(&g, 10, 9, 1000, &hdr) == 1, "J8 and it reopens");
		clmig_buf_free(&g);
		chk(g.b == NULL, "J8 free releases it");
	}

	/* ---- the record codec (slice 3) ----------------------------------
	 * The sender wrote this layout by hand and the receiver decoded it
	 * by hand, in two places that had to change together.  Now one
	 * codec does both, so a round trip can pin the format. */
	{
		struct clmig_buf g;
		struct clmig_rec w, r;
		unsigned char *hdr = NULL;
		const unsigned char *gp;
		unsigned int gn;
		size_t off, need;
		int k;

		chk(clmig_buf_init(&g, 8192) == 0, "K0 buffer");

		/* K1. one record, written and read back */
		w.ttl_left = 4242;  w.ver = 0x0102030405060708ULL;
		w.col = "orders";   w.collen = 6;
		w.key = "customer:42";  w.klen = 11;
		w.val = "some-value";   w.vlen = 10;
		need = clmig_rec_size(w.collen, w.klen, w.vlen);
		chk(need == CLMIG_RHDR + 6 + 11 + 10, "K1 rec_size counts every part");
		chk(clmig_open_for(&g, need, 9, 60000, &hdr) == 1, "K1 group opens");
		memset(hdr, 0, 9);
		clmig_rec_write(clmig_record_at(&g), &w);
		clmig_record_done(&g, need, 7);

		off = 0;
		chk(clmig_group_walk(&g, &off, &gp, &gn), "K1 the group is there");
		{
			size_t ro = 9;    /* past the group header */

			chk(clmig_rec_parse(gp, gn, &ro, &r) == 1, "K1 it parses");
			chk(r.ttl_left == 4242, "K1 ttl survives the round trip");
			chk(r.ver == 0x0102030405060708ULL, "K1 and the version");
			chk(r.collen == 6 && !memcmp(r.col, "orders", 6),
				"K1 the collection");
			chk(r.klen == 11 && !memcmp(r.key, "customer:42", 11),
				"K1 the key");
			chk(r.vlen == 10 && !memcmp(r.val, "some-value", 10),
				"K1 the value");
			chk(ro == 9 + need, "K1 and the offset advanced exactly");
			chk(clmig_rec_parse(gp, gn, &ro, &r) == 0,
				"K1 the group holds no second record");
		}

		/* K2. the group header decodes */
		{
			unsigned char *h = g.b + 2;

			pc_p32(h + 1, 0x11223344);
			chk(clmig_group_req(h) == 0x11223344, "K2 group_req");
			chk(clmig_group_count(h) == 1, "K2 group_count sees the record");
		}

		/* K3. several records, in order */
		clmig_buf_reset(&g);
		for (k = 0; k < 5; k++) {
			char key[8];

			snprintf(key, sizeof key, "k%d", k);
			w.ttl_left = (unsigned)(k + 1);
			w.ver = (unsigned long long)(k + 100);
			w.col = "c";  w.collen = 1;
			w.key = key;  w.klen = (unsigned)strlen(key);
			w.val = "vv"; w.vlen = 2;
			need = clmig_rec_size(w.collen, w.klen, w.vlen);
			if (clmig_open_for(&g, need, 9, 60000, &hdr) == 1)
				memset(hdr, 0, 9);
			clmig_rec_write(clmig_record_at(&g), &w);
			clmig_record_done(&g, need, 7);
		}
		off = 0;
		chk(clmig_group_walk(&g, &off, &gp, &gn), "K3 one group holds them");
		chk(clmig_group_count(gp) == 5, "K3 counted five");
		{
			size_t ro = 9;
			int seen = 0;

			while (clmig_rec_parse(gp, gn, &ro, &r)) {
				chk(r.ttl_left == (unsigned)(seen + 1),
					"K3 records come back IN ORDER");
				chk(r.ver == (unsigned long long)(seen + 100),
					"K3 with their versions");
				seen++;
				if (seen > 16) break;
			}
			chk(seen == 5, "K3 all five parsed back");
		}

		/* K4. a TRUNCATED datagram stops the walk instead of reading
		 * past it - the receiver acks what it stored */
		{
			size_t ro = 9;

			chk(clmig_rec_parse(gp, gn - 1, &ro, &r) == 1,
				"K4 a record wholly present still parses");
			ro = 9;
			chk(clmig_rec_parse(gp, 9 + CLMIG_RHDR - 1, &ro, &r) == 0,
				"K4 a header cut short parses nothing");
			ro = 9;
			chk(clmig_rec_parse(gp, 9 + CLMIG_RHDR + 1, &ro, &r) == 0,
				"K4 a body cut short parses nothing");
			chk(ro == 9, "K4 and a refused parse does not advance");
		}

		/* K5. Parse from a TIGHTLY sized heap buffer.  The checks above
		 * all read inside one big allocation, so a parser that reads
		 * past `n` still lands on valid memory and no assertion can
		 * see it.  Copying the datagram into a malloc(gn) makes an
		 * overread a heap overflow, which ASan catches - that is what
		 * covers the header-length check, not a comparison. */
		{
			unsigned char *tight = malloc(gn);
			size_t ro = 9;

			chk(tight != NULL, "K5 tight buffer");
			if (tight) {
				memcpy(tight, gp, gn);
				chk(clmig_rec_parse(tight, gn, &ro, &r) == 1,
					"K5 parses out of an exactly-sized buffer");
				free(tight);
			}
			/* A buffer ALLOCATED at exactly the short length.  The
			 * previous version passed a small `n` against a big
			 * allocation, so a parser that ignored the header-length
			 * check still read inside valid memory and even ASan saw
			 * nothing.  Only an exactly-sized allocation makes the
			 * overread a heap overflow. */
			{
				size_t small = CLMIG_RHDR - 1;
				unsigned char *tiny = malloc(small);
				size_t ro2 = 0;

				chk(tiny != NULL, "K5 tiny buffer");
				if (tiny) {
					memset(tiny, 0, small);
					chk(clmig_rec_parse(tiny, small, &ro2, &r) == 0,
						"K5 a buffer too short for a record header");
					chk(ro2 == 0, "K5 and it did not advance");
					free(tiny);
				}
			}
		}
		clmig_buf_free(&g);
	}

	/* ---- the GOLDEN record layout ------------------------------------
	 *
	 * Captured from clmig_rec_write() before the cursor rewrite.  The
	 * round-trip cases above pair write against parse, so they would
	 * still pass if every offset moved together - and a moved record
	 * layout is a fleet-wide incompatibility, not a test to update. */
	{
	static const unsigned char GOLD[] = {
		0xcd, 0xab, 0x34, 0x12, 0x08, 0x07, 0x00, 0x08, 0x00, 0x00, 0x00, 0x10,
		0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe, 0x73, 0x65, 0x73, 0x73, 0x69,
		0x6f, 0x6e, 0x73, 0x61, 0x62, 0x63, 0x2d, 0x31, 0x32, 0x33, 0x7b, 0x22,
		0x76, 0x22, 0x3a, 0x34, 0x32, 0x7d };
	/* 42 bytes */
		unsigned char g[128];
		struct clmig_rec r;

		memset(g, 0, sizeof g);
		memset(&r, 0, sizeof r);
		r.ttl_left = 0x1234abcd; r.collen = 8; r.klen = 7; r.vlen = 8;
		r.ver = 0xfedcba9876543210ULL;
		r.col = "sessions"; r.key = "abc-123"; r.val = "{\"v\":42}";
		clmig_rec_write(g, &r);
		chk(clmig_rec_size(8, 7, 8) == sizeof GOLD,
			"GOLDEN record length unchanged");
		chk(memcmp(g, GOLD, sizeof GOLD) == 0,
			"GOLDEN record bytes unchanged on the wire");
	}
	/* ---- the frames the records travel in ------------------------
	 *
	 * ref_* below are the byte writes copied verbatim from mig_cb's
	 * group header, handle_migrate's ack and the single-record shape
	 * as cluster.c had them.  The group and the single LOOK alike and
	 * are not - the single has no count and orders its fields
	 * differently - which is why both are pinned here. */
	{
		unsigned char x[256], y[256];
		struct clmig_single sg;
		uint32_t rq;
		unsigned int st;
		size_t la, lb, i;

		/* group header */
		memset(x, 0xAA, sizeof x); memset(y, 0xAA, sizeof y);
		x[0] = 15; pc_p32(x + 1, 0xdeadbeef);
		pc_p16(x + 5, 4097); pc_p16(x + 7, 0);
		chk(clmig_group_hdr(y, sizeof y, 15, 0xdeadbeef, 4097) ==
			CLMIG_GHDR, "G1 the group header is 9 bytes");
		chk(memcmp(x, y, CLMIG_GHDR) == 0, "G2 group header bytes match");
		chk(clmig_group_req(y) == 0xdeadbeef &&
			clmig_group_count(y) == 0,
			"G3 the accessors read it, count starts at 0");

		/* the ack: byte 5 is a STATUS byte, and there are two
		 * shapes - 6 bytes for a single record, 8 for a group */
		memset(x, 0xAA, sizeof x); memset(y, 0xAA, sizeof y);
		x[0] = 16; pc_p32(x + 1, 0x1234); x[5] = 1; pc_p16(x + 6, 77);
		st = 77;
		chk(clmig_ack_write(y, sizeof y, 16, 0x1234, 1, &st) ==
			CLMIG_ACK_LEN, "G4 the group ack is 8 bytes");
		chk(memcmp(x, y, CLMIG_ACK_LEN) == 0, "G5 group ack bytes match");
		memset(x, 0xAA, sizeof x); memset(y, 0xAA, sizeof y);
		x[0] = 16; pc_p32(x + 1, 0x1234); x[5] = 1;
		chk(clmig_ack_write(y, sizeof y, 16, 0x1234, 1, NULL) ==
			CLMIG_ACK_MIN, "G6 the single ack is 6 bytes");
		chk(memcmp(x, y, CLMIG_ACK_MIN) == 0, "G7 single ack bytes match");
		{
			int okf = -1;

			chk(clmig_ack_parse(y, CLMIG_ACK_MIN, &rq, &okf, &st) &&
				rq == 0x1234 && okf == 1 && st == 0,
				"G8 the short ack parses, stored reads 0");
			x[5] = 0; pc_p16(x + 6, 99);
			chk(clmig_ack_parse(x, CLMIG_ACK_LEN, &rq, &okf, &st) &&
				okf == 0 && st == 99,
				"G9 the long ack carries ok and the count");
			for (i = 0; i < CLMIG_ACK_MIN; i++) {
				unsigned char *t = malloc(i ? i : 1);

				memcpy(t, x, i);
				chk(!clmig_ack_parse(t, i, &rq, &okf, &st),
					"G10 an ack under 6 bytes is refused");
				free(t);
			}
		}

		/* the single record frame */
		memset(x, 0xAA, sizeof x); memset(y, 0xAA, sizeof y);
		x[0] = 14; pc_p32(x + 1, 0x99); pc_p16(x + 5, 730);
		pc_p32(x + 7, 4242); x[11] = 8; pc_p16(x + 12, 7);
		memcpy(x + 14, "sessions", 8);
		memcpy(x + 22, "abc-123", 7);
		memcpy(x + 29, "VALUE", 5);
		la = 14 + 8 + 7 + 5;
		memset(&sg, 0, sizeof sg);
		sg.req = 0x99; sg.node = 730; sg.ttl_left = 4242;
		sg.col = "sessions"; sg.collen = 8;
		sg.key = "abc-123"; sg.klen = 7;
		sg.val = "VALUE"; sg.vlen = 5;
		lb = clmig_single_write(y, sizeof y, 14, &sg);
		chk(la == lb && memcmp(x, y, la) == 0,
			"G9 the single-record frame bytes match");
		chk(lb == clmig_single_size(8, 7, 5), "G10 its size helper agrees");
		for (i = 0; i <= la; i++) {
			unsigned char *t = malloc(i ? i : 1);
			int ok;

			memcpy(t, x, i);
			memset(&sg, 0x7F, sizeof sg);
			ok = clmig_single_parse(t, i, &sg);
			if (i < 14 + 8 + 7)
				chk(!ok, "G11 refused until col and key are whole");
			else
				chk(ok && sg.req == 0x99 && sg.node == 730 &&
					sg.ttl_left == 4242 && sg.collen == 8 &&
					sg.klen == 7 &&
					sg.vlen == (unsigned)(i - 29),
					"G12 parses, vlen is whatever remains");
			free(t);
		}
	}

	printf("clmigtest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
