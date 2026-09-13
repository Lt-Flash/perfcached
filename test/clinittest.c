/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * clinittest.c - the node identity file format (M13 slice 1).
 *
 * The doc's test note for M13 asks for "digest golden values; each
 * refusal path".  Both are here: fnv1a64 against fixed vectors, and one
 * case per named refusal - which is only possible because each refusal
 * has its own message rather than a shared -1.
 */
#include <stdio.h>
#include <string.h>

#include "../src/clinit.h"

static int pass, fail;
static void chk(int cond, const char *what)
{
	if (cond) { pass++; return; }
	fail++;
	printf("FAIL: %s\n", what);
}
/* the refusal must be the RIGHT one, not merely a refusal */
static void chk_why(int rc, const char *why, const char *want, const char *what)
{
	if (rc == -1 && strstr(why, want)) { pass++; return; }
	fail++;
	printf("FAIL: %s (rc=%d why=\"%s\")\n", what, rc, why);
}

int main(void)
{
	unsigned char id[16], out[16];
	char buf[256], hex[33], why[160];
	size_t len;
	int i, rc;

	printf("=== clinittest ===\n");
	for (i = 0; i < 16; i++)
		id[i] = (unsigned char)(i * 17);

	/* A. fnv1a64 GOLDEN VALUES - the published FNV-1a 64 vectors */
	/* the PUBLISHED FNV-1a 64 vectors.  They failed before 2026-09-13
	 * because the offset basis was one digit short; that is fixed, so
	 * these now pin it to the real algorithm. */
	chk(clinit_fnv1a64((const unsigned char *)"", 0) == 14695981039346656037ULL,
		"A1 fnv1a64(\"\") is the offset basis");
	chk(clinit_fnv1a64((const unsigned char *)"a", 1) == 0xaf63dc4c8601ec8cULL,
		"A2 fnv1a64(\"a\")");
	chk(clinit_fnv1a64((const unsigned char *)"foobar", 6) == 0x85944171f73967e8ULL,
		"A3 fnv1a64(\"foobar\")");
	chk(clinit_fnv1a64(id, 16) == clinit_fnv1a64(id, 16), "A4 it is stable");
	{
		unsigned char other[16];

		memcpy(other, id, 16);
		other[7] ^= 1;
		chk(clinit_fnv1a64(other, 16) != clinit_fnv1a64(id, 16),
			"A5 one flipped bit changes it");
	}

	/* B. hex both ways */
	clinit_ident_hex(id, hex);
	chk(strlen(hex) == 32, "B1 32 hex digits");
	chk(!strcmp(hex, "00112233445566778899aabbccddeeff"), "B2 the right ones");

	/* B2b. NIBBLE ORDER.  Every byte above is 0x00, 0x11, 0x22 ... -
	 * palindromic, so swapping the nibbles produces identical output
	 * and the check above cannot see it.  Use bytes whose halves
	 * differ.  (mutation: swap the two hx[] lookups) */
	{
		unsigned char asym[16];
		char ah[33];
		int j;

		for (j = 0; j < 16; j++)
			asym[j] = (unsigned char)(0xA0 | j);   /* a0 a1 .. af */
		clinit_ident_hex(asym, ah);
		chk(!strcmp(ah, "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf"),
			"B2b high nibble first, not low");
		chk(clinit_hex2bin(ah, out, 16) == 0 && !memcmp(out, asym, 16),
			"B2b and hex2bin agrees with it");
	}
	chk(clinit_hex2bin(hex, out, 16) == 0 && !memcmp(out, id, 16),
		"B3 and back to the same bytes");
	chk(clinit_hex2bin("00112233445566778899AABBCCDDEEFF", out, 16) == 0 &&
		!memcmp(out, id, 16), "B4 uppercase reads too");
	chk(clinit_hex2bin("0g", out, 1) == -1, "B5 a non-hex digit is refused");
	chk(clinit_hex2bin("g0", out, 1) == -1, "B5 in either position");

	/* C. render and parse round trip */
	len = clinit_ident_render(buf, sizeof buf, id);
	chk(len > 0, "C1 renders");
	chk(!memcmp(buf, CLINIT_IDENT_HEADER, strlen(CLINIT_IDENT_HEADER)),
		"C1 starting with the header");
	chk(buf[len - 1] == '\n', "C1 and ending with a newline");
	memset(out, 0, 16);
	rc = clinit_ident_parse((const unsigned char *)buf, len, out, why, sizeof why);
	chk(rc == 1, "C2 parses as current-format");
	chk(!memcmp(out, id, 16), "C2 with the identity intact");
	chk(clinit_ident_render(buf, 8, id) == 0, "C3 a buffer too small returns 0");

	/* D. the LEGACY bare 16 bytes still read - an upgrade must not lose
	 * a node's id */
	memset(out, 0, 16);
	rc = clinit_ident_parse(id, 16, out, why, sizeof why);
	chk(rc == 0, "D1 sixteen raw bytes are the legacy format");
	chk(!memcmp(out, id, 16), "D1 and carry the identity");

	/* E. EVERY refusal path, each by its own message */
	rc = clinit_ident_parse((const unsigned char *)"", 0, out, why, sizeof why);
	chk_why(rc, why, "empty", "E1 an empty file");

	rc = clinit_ident_parse((const unsigned char *)"wrong header\n", 13, out,
		why, sizeof why);
	chk_why(rc, why, "first line", "E2 a wrong first line");

	/* E2b. A file of the RIGHT LENGTH whose header text is wrong.  The
	 * case above is short enough that the length check refuses it
	 * first, so removing the header comparison went unnoticed.
	 * (mutation: drop the memcmp against CLINIT_IDENT_HEADER) */
	{
		char b3[256];
		size_t l3 = clinit_ident_render(b3, sizeof b3, id);

		b3[4] = 'X';                       /* inside the header text */
		rc = clinit_ident_parse((const unsigned char *)b3, l3, out, why,
			sizeof why);
		chk_why(rc, why, "first line",
			"E2b a full-length file with a corrupted header");
	}

	len = clinit_ident_render(buf, sizeof buf, id);
	{
		char b2[256];

		/* second line broken */
		memcpy(b2, buf, len);
		b2[strlen(CLINIT_IDENT_HEADER) + 3] = 'X';
		rc = clinit_ident_parse((const unsigned char *)b2, len, out, why,
			sizeof why);
		chk_why(rc, why, "second line", "E3 a broken identity line");

		/* a non-hex digit inside the identity */
		memcpy(b2, buf, len);
		b2[strlen(CLINIT_IDENT_HEADER) + 1 + 9] = 'z';
		rc = clinit_ident_parse((const unsigned char *)b2, len, out, why,
			sizeof why);
		chk_why(rc, why, "second line", "E4 non-hex in the identity");

		/* third line broken */
		memcpy(b2, buf, len);
		b2[strlen(CLINIT_IDENT_HEADER) + 1 + 9 + 32 + 1 + 1] = 'X';
		rc = clinit_ident_parse((const unsigned char *)b2, len, out, why,
			sizeof why);
		chk_why(rc, why, "third line", "E5 a broken check line");

		/* trailing junk */
		memcpy(b2, buf, len);
		memcpy(b2 + len, "junk\n", 5);
		rc = clinit_ident_parse((const unsigned char *)b2, len + 5, out,
			why, sizeof why);
		chk_why(rc, why, "more after the check line", "E6 trailing junk");

		/* THE ONE THAT MATTERS: a plausible file whose check does not
		 * match - an edited identity, or a torn write */
		memcpy(b2, buf, len);
		b2[strlen(CLINIT_IDENT_HEADER) + 1 + 9] =
			(b2[strlen(CLINIT_IDENT_HEADER) + 1 + 9] == 'a') ? 'b' : 'a';
		rc = clinit_ident_parse((const unsigned char *)b2, len, out, why,
			sizeof why);
		chk_why(rc, why, "check value does not match",
			"E7 an identity edited under a stale check value");
	}

	/* F. a truncated file is refused, not read past */
	for (i = 1; i < (int)len; i++) {
		rc = clinit_ident_parse((const unsigned char *)buf, (size_t)i,
			out, why, sizeof why);
		if (i == 16)
			continue;           /* that length IS the legacy format */
		if (rc != -1) {
			chk(0, "F1 every truncation of the file is refused");
			break;
		}
	}
	if (i >= (int)len)
		chk(1, "F1 every truncation of the file is refused");

	/* ---- G. the config digest (slice 2) ------------------------------
	 * The number two nodes compare to decide whether they may federate.
	 * A digest that drifts refuses every join, so these golden values
	 * were computed from an INDEPENDENT reimplementation of the fold,
	 * not read out of this code - otherwise they would pin whatever it
	 * happens to do rather than what it must do. */
	{
		struct clinit_digest_in in;
		const char *ab[2] = { "a", "b" };
		const char *ba[2] = { "b", "a" };
		const char *one[1] = { "orders" };
		uint64_t d0, d1, d2;

		in.mode = 0; in.eager = 0; in.wal = 0;
		in.route_algo = "hrw-slot16k-v1";
		in.names = NULL; in.n_names = 0;
		chk(clinit_config_digest(&in) == 0x93368ccc45435527ULL,
			"G1 the empty digest matches its golden value");

		in.names = ab; in.n_names = 2;
		d1 = clinit_config_digest(&in);
		chk(d1 == 0x5f025e56d9975374ULL, "G2 with two collections");

		/* G3. ORDER INDEPENDENCE - the property the sort exists for.
		 * Two nodes listing their collections differently must still
		 * federate. */
		in.names = ba;
		d2 = clinit_config_digest(&in);
		chk(d2 == d1, "G3 the same set in the other order digests the same");

		in.mode = 2; in.eager = 1; in.wal = 1;
		in.names = one; in.n_names = 1;
		chk(clinit_config_digest(&in) == 0x950b4c232b292ef7ULL,
			"G4 mode+eager+wal+one collection matches its golden value");

		/* G5. every input must MOVE the digest, or it is not protecting
		 * what it claims to */
		in.mode = 0; in.eager = 0; in.wal = 0;
		in.names = one; in.n_names = 1;
		d0 = clinit_config_digest(&in);
		in.mode = 1;
		chk(clinit_config_digest(&in) != d0, "G5 mode changes it");
		in.mode = 0; in.eager = 1;
		chk(clinit_config_digest(&in) != d0, "G5 eager changes it");
		in.eager = 0; in.wal = 1;
		chk(clinit_config_digest(&in) != d0, "G5 the WAL flag changes it");
		in.wal = 0; in.route_algo = "hrw-fnv1a-v1";
		chk(clinit_config_digest(&in) != d0,
			"G5 the placement contract changes it");
		in.route_algo = "hrw-slot16k-v1"; in.n_names = 0;
		chk(clinit_config_digest(&in) != d0,
			"G5 dropping a collection changes it");

		/* G6. a fleet with no WAL keeps the digest it had before the
		 * flag existed - what makes a rolling upgrade safe (S73b) */
		in.n_names = 1; in.wal = 0;
		chk(clinit_config_digest(&in) == d0,
			"G6 wal=0 folds nothing, so the pre-flag digest is unchanged");

		/* G7. names that differ only by a NUL boundary must not
		 * collide: {"ab"} and {"a","b"} fold different bytes */
		{
			const char *abc[1] = { "ab" };
			uint64_t da, db;

			in.mode = 0; in.eager = 0; in.wal = 0;
			in.names = abc; in.n_names = 1;
			da = clinit_config_digest(&in);
			in.names = ab; in.n_names = 2;
			db = clinit_config_digest(&in);
			chk(da != db,
				"G7 {\"ab\"} and {\"a\",\"b\"} do not collide");
		}
	}

	/* ---- H. the proposed node id (slice 3) ---------------------------
	 * Derived from the identity so a node suggests the same id every
	 * start.  The master arbitrates, so a collision is resolved rather
	 * than fatal - but the range must hold and it must be stable. */
	{
		unsigned char a[16], b[16];
		int idA, idB, j;

		memset(a, 0, 16);
		chk(clinit_proposed_id(a) >= 1 && clinit_proposed_id(a) <= 1023,
			"H1 an all-zero identity still lands in 1..1023");
		idA = clinit_proposed_id(id);
		chk(idA >= 1 && idA <= 1023, "H2 in range");
		chk(clinit_proposed_id(id) == idA, "H3 and stable across calls");
		memcpy(b, id, 16);
		b[15] ^= 0xFF;
		idB = clinit_proposed_id(b);
		chk(idB != idA, "H4 a different identity proposes a different id");

		/* H5. never 0 - id 0 means "no node" everywhere else */
		for (j = 0; j < 256; j++) {
			memset(a, (unsigned char)j, 16);
			if (clinit_proposed_id(a) == 0) {
				chk(0, "H5 the proposed id is never 0");
				break;
			}
		}
		if (j == 256)
			chk(1, "H5 the proposed id is never 0, over 256 identities");
	}

	printf("clinittest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
