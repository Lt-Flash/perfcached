/*
 * clhisttest.c — the identity history (step 3b).
 *
 * It answers "have we seen this node before", which is what separates
 * STARTING from RECOVERING - and which the node itself cannot answer,
 * since one whose state directory was wiped sincerely believes it is
 * new.  The tests cover the two ways that answer goes wrong: losing it
 * across a promotion, and letting it grow without bound.
 *
 * Build: cc -o clhisttest test/clhisttest.c src/clhist.o
 */
#include <stdio.h>
#include <string.h>

#include "../src/clhist.h"

static int pass, fail;
#define CHK(cond, ...) do { \
	if (cond) { pass++; } \
	else { fail++; printf("  FAIL "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static void mkid(unsigned char id[16], int n)
{
	memset(id, 0, 16);
	memcpy(id, &n, sizeof n);
	id[15] = 0x5A;
}

int main(void)
{
	static struct pc_clhist h, back;
	static unsigned char buf[2 + PC_CLHIST_MAX * PC_CLHIST_ENTSZ + 64];
	unsigned char a[16], b[16], id[16];
	const char *why;
	long n;
	int i;

	mkid(a, 1);
	mkid(b, 2);

	/* ---- 1. first sight vs a return ---------------------------- */
	pc_clhist_init(&h);
	CHK(!pc_clhist_seen(&h, a), "an empty history claims to know a node");
	CHK(pc_clhist_note(&h, a, 11) == 0,
		"a first sighting reported as already known - the node would "
		"be treated as RECOVERING when it is genuinely new");
	CHK(pc_clhist_note(&h, a, 11) == 1,
		"a second sighting reported as new - a returning node would "
		"be treated as STARTING");
	CHK(pc_clhist_seen(&h, a), "a noted identity is not seen");
	CHK(!pc_clhist_seen(&h, b), "an unknown identity is seen");
	CHK(h.n == 1, "a repeat sighting created a second entry (n=%u)", h.n);

	/* ---- 2. the id it last held -------------------------------- */
	CHK(pc_clhist_last_id(&h, a) == 11, "last id lost");
	pc_clhist_note(&h, a, 12);
	CHK(pc_clhist_last_id(&h, a) == 12, "last id did not update");
	CHK(pc_clhist_last_id(&h, b) == 0, "an unknown identity has an id");

	/* ---- 3. bounded, and it evicts the LEAST RECENTLY seen ------ */
	pc_clhist_init(&h);
	for (i = 0; i < PC_CLHIST_MAX; i++) {
		mkid(id, 1000 + i);
		pc_clhist_note(&h, id, (uint16_t)i);
	}
	CHK(h.n == PC_CLHIST_MAX, "history is %u, expected full", h.n);

	/* touch the oldest so it is no longer the eviction candidate */
	mkid(id, 1000);
	pc_clhist_note(&h, id, 0);

	mkid(id, 9999);
	pc_clhist_note(&h, id, 77);         /* forces an eviction */
	CHK(h.n == PC_CLHIST_MAX,
		"the history grew past its bound (%u) - a fleet of restarting "
		"containers would grow it without limit", h.n);
	CHK(pc_clhist_seen(&h, id), "the newest identity was not kept");
	mkid(id, 1000);
	CHK(pc_clhist_seen(&h, id),
		"a RECENTLY TOUCHED identity was evicted - eviction is not "
		"least-recently-seen");
	mkid(id, 1001);
	CHK(!pc_clhist_seen(&h, id),
		"the least recently seen identity survived eviction");

	/* ---- 4. it survives the trip to the backup ----------------- */
	pc_clhist_init(&h);
	pc_clhist_note(&h, a, 11);
	pc_clhist_note(&h, b, 22);
	n = pc_clhist_encode(&h, buf, sizeof buf);
	CHK(n == 2 + 2 * PC_CLHIST_ENTSZ, "encoded %ld bytes", n);
	CHK(pc_clhist_decode(buf, (size_t)n, &back, &why) == 0,
		"decode failed: %s", why);
	CHK(back.n == 2, "count lost");
	CHK(pc_clhist_seen(&back, a) && pc_clhist_seen(&back, b),
		"an identity did not survive the sync - a promoted backup "
		"would call a returning node new");
	CHK(pc_clhist_last_id(&back, b) == 22, "last id did not survive");

	/* a full history must round-trip too - that is the size that
	 * actually travels */
	pc_clhist_init(&h);
	for (i = 0; i < PC_CLHIST_MAX; i++) {
		mkid(id, 5000 + i);
		pc_clhist_note(&h, id, (uint16_t)(i & 0xFFFF));
	}
	n = pc_clhist_encode(&h, buf, sizeof buf);
	CHK(n > 0 && pc_clhist_decode(buf, (size_t)n, &back, &why) == 0 &&
		back.n == PC_CLHIST_MAX, "a full history did not round trip: %s",
		why);

	/* ---- 5. refusals ------------------------------------------- */
	pc_clhist_init(&h);
	pc_clhist_note(&h, a, 11);
	pc_clhist_note(&h, b, 22);
	n = pc_clhist_encode(&h, buf, sizeof buf);

	CHK(pc_clhist_decode(buf, (size_t)n - 1, &back, &why) < 0 &&
		!strcmp(why, "trunc"), "truncation accepted (why=%s)", why);
	CHK(pc_clhist_decode(buf, (size_t)n + 1, &back, &why) < 0 &&
		!strcmp(why, "trunc"), "trailing bytes accepted (why=%s)", why);
	CHK(pc_clhist_decode(buf, 1, &back, &why) < 0, "a runt was accepted");

	buf[0] = 0xFF; buf[1] = 0xFF;
	CHK(pc_clhist_decode(buf, (size_t)n, &back, &why) < 0 &&
		!strcmp(why, "count"), "an impossible count accepted (why=%s)",
		why);
	buf[0] = 2; buf[1] = 0;

	/* a duplicate identity would make the STARTING/RECOVERING answer
	 * depend on iteration order */
	memcpy(buf + 2 + PC_CLHIST_ENTSZ, buf + 2, 16);
	CHK(pc_clhist_decode(buf, (size_t)n, &back, &why) < 0 &&
		!strcmp(why, "dup"), "a duplicate identity accepted (why=%s)",
		why);

	CHK(pc_clhist_encode(&h, buf, 4) == -1,
		"encode into a too-small buffer succeeded");

	printf("clhisttest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
