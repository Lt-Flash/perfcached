/*
 * clpulltest.c - the PULL plane's wire frames.
 *
 * DIFFERENTIAL, like clmembtest: ref_req() and ref_rsp() below are the
 * byte writes copied verbatim out of build_pull_req() and the response
 * builder inside handle_pull_req(), as they stood in cluster.c before
 * the extraction.  A round trip would only prove clpull agrees with
 * itself, which it would do with every offset shifted by one; these
 * bytes are what a peer on an older build actually sends.
 *
 * The pull plane is where a store-mode miss goes, so getting it wrong
 * does not crash anything - it returns the wrong value, or an older one,
 * which is the failure that shows up as a customer complaint rather
 * than an alert.  The VERSION field is the one that guards that, and it
 * is at offset 16 of a 24-byte header.
 *
 * Build: cc -o clpulltest test/clpulltest.c src/clpull.o
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../src/clpull.h"
#include "../src/clcodec.h"
#define p16 pc_p16
#define p32 pc_p32
#define p64 pc_p64
#define g16 pc_g16
#define g32 pc_g32
#define g64 pc_g64

static int pass, fail;
static void chk(int cond, const char *what)
{
	if (cond) { pass++; return; }
	fail++;
	printf("  FAIL %s\n", what);
}

/* ---- verbatim from cluster.c before the extraction ---- */
static size_t ref_req(unsigned char *msg, unsigned char type, uint32_t req,
	int node, const char *col, size_t collen, const char *key, size_t klen)
{
	msg[0] = type;
	p32(msg + 1, req);
	p16(msg + 5, (uint16_t)node);
	msg[7] = (unsigned char)collen;
	p16(msg + 8, (uint16_t)klen);
	memcpy(msg + 10, col, collen);
	memcpy(msg + 10 + collen, key, klen);
	return 10 + collen + klen;
}

static size_t ref_rsp(unsigned char *rsp, unsigned char type, uint32_t req,
	int node, int found, uint32_t ttl_left, uint32_t vlen, uint64_t ver,
	const char *val)
{
	size_t rn;

	rsp[0] = type;
	p32(rsp + 1, req);
	p16(rsp + 5, (uint16_t)node);
	rsp[7] = (unsigned char)found;
	p32(rsp + 8, ttl_left);
	p32(rsp + 12, found ? vlen : 0);
	p64(rsp + 16, found ? ver : 0);
	rn = 24;
	if (found && vlen > 0) {
		memcpy(rsp + 24, val, vlen);
		rn += vlen;
	}
	return rn;
}

int main(void)
{
	const char *col = "sessions", *key = "abc-123";
	const char *val = "{\"v\":42,\"who\":\"a\"}";
	unsigned char a[512], b[512];
	struct clpull_req q;
	struct clpull_rsp r;
	size_t n, la, lb;
	unsigned int i;

	printf("=== clpulltest ===\n");

	/* ---- A. PULL_REQ bytes, against the old builder --------------- */
	memset(a, 0xAA, sizeof a); memset(b, 0xAA, sizeof b);
	la = ref_req(a, 2, 0xdeadbeef, 4097, col, 8, key, 7);
	memset(&q, 0, sizeof q);
	q.req = 0xdeadbeef; q.node = 4097;
	q.col = col; q.collen = 8; q.key = key; q.klen = 7;
	lb = clpull_req_write(b, sizeof b, 2, &q);
	chk(la == lb, "A1 PULL_REQ length matches the old builder");
	chk(la == lb && memcmp(a, b, la) == 0, "A2 PULL_REQ bytes unchanged");
	chk(lb == clpull_req_size(8, 7), "A3 and the size helper agrees");

	/* ---- B. PULL_REQ parse, at every length ----------------------- */
	for (n = 0; n <= la; n++) {
		unsigned char *tight = malloc(n ? n : 1);
		int ok;

		memcpy(tight, a, n);
		memset(&q, 0x7F, sizeof q);
		ok = clpull_req_parse(tight, n, &q);
		if (n < la)
			chk(!ok, "B1 a short PULL_REQ is refused, never half-read");
		else
			chk(ok && q.req == 0xdeadbeef && q.node == 4097 &&
				q.collen == 8 && q.klen == 7 &&
				memcmp(q.col, col, 8) == 0 &&
				memcmp(q.key, key, 7) == 0,
				"B2 the full PULL_REQ parses");
		free(tight);
	}

	/* ---- C. PULL_RSP bytes: found, absent, and the too-big case --- */
	{
		struct { const char *nm; int found; uint32_t ttl, vlen;
			uint64_t ver; } cs[] = {
			{ "found with a value", 1, 900, 18, 0x0123456789abcdefULL },
			{ "found, no value",    1, 0,   0,  7 },
			{ "absent",             0, 0,   0,  0 },
			/* the too-big path: the caller has already forced found,
			 * vlen and ver to 0 but LEFT ttl_left alone */
			{ "too big: ttl survives found=0", 0, 1234, 0, 0 },
		};
		size_t k;

		for (k = 0; k < sizeof cs / sizeof cs[0]; k++) {
			memset(a, 0xAA, sizeof a); memset(b, 0xAA, sizeof b);
			la = ref_rsp(a, 3, 0x01020304, 730, cs[k].found, cs[k].ttl,
				cs[k].vlen, cs[k].ver, val);
			memset(&r, 0, sizeof r);
			r.req = 0x01020304; r.node = 730; r.found = cs[k].found;
			r.ttl_left = cs[k].ttl; r.vlen = cs[k].vlen;
			r.ver = cs[k].ver; r.val = val;
			lb = clpull_rsp_write(b, sizeof b, 3, &r);
			chk(la == lb && memcmp(a, b, la) == 0, cs[k].nm);
		}
		/* the asymmetry is real and must stay: ttl at offset 8 is NOT
		 * cleared when found goes to 0 */
		memset(b, 0, sizeof b);
		r.found = 0; r.ttl_left = 1234; r.vlen = 0; r.ver = 0;
		clpull_rsp_write(b, sizeof b, 3, &r);
		chk(b[7] == 0 && g32(b + 8) == 1234,
			"C5 found=0 keeps the ttl the sender computed");
	}

	/* ---- D. PULL_RSP parse, at every length ----------------------- */
	memset(a, 0xAA, sizeof a);
	la = ref_rsp(a, 3, 0x01020304, 730, 1, 900, 18,
		0x0123456789abcdefULL, val);
	for (n = 0; n <= la; n++) {
		unsigned char *tight = malloc(n ? n : 1);
		int ok;

		memcpy(tight, a, n);
		memset(&r, 0x7F, sizeof r);
		ok = clpull_rsp_parse(tight, n, &r);
		if (n < CLPULL_RHDR)
			chk(!ok, "D1 a PULL_RSP under its header is refused");
		else
			chk(ok && r.req == 0x01020304 && r.node == 730 &&
				r.found == 1 && r.ttl_left == 900 &&
				r.vlen == 18 &&
				r.ver == 0x0123456789abcdefULL,
				"D2 the header parses at or above 24 bytes");
		free(tight);
	}
	chk(clpull_rsp_parse(a, la, &r) && memcmp(r.val, val, 18) == 0,
		"D3 the value is reachable at the end of the header");

	/* ---- E. every field is distinct on the wire -------------------
	 * a field written into the wrong slot would still round-trip, so
	 * poke each byte of the header and require the parse to notice */
	{
		unsigned char base[64];
		struct clpull_rsp r0, r1;

		memset(base, 0, sizeof base);
		ref_rsp(base, 3, 0x11223344, 555, 1, 777, 4, 0x99, "abcd");
		clpull_rsp_parse(base, 28, &r0);
		for (i = 1; i < CLPULL_RHDR; i++) {
			unsigned char t[64];

			memcpy(t, base, sizeof t);
			t[i] ^= 0xff;
			clpull_rsp_parse(t, 28, &r1);
			if (r1.req == r0.req && r1.node == r0.node &&
			    r1.found == r0.found && r1.ttl_left == r0.ttl_left &&
			    r1.vlen == r0.vlen && r1.ver == r0.ver)
				break;
		}
		chk(i == CLPULL_RHDR,
			"E1 every header byte reaches some field");
	}

	printf("clpulltest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
