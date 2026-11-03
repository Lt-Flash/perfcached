/*
 * psinteresttest.c - the interest filter, its version and a sender's view
 * of a peer (PS12), driven through src/psinterest.h exactly as the nodes
 * run it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psinterest.h"

static int pass, fail;

#define CHK(c, msg) do { \
	if (c) pass++; \
	else { fail++; printf("  FAIL %s\n", msg); } \
} while (0)

static void name(char *buf, size_t cap, const char *kind, unsigned i)
{
	snprintf(buf, cap, "%s:%u", kind, i);
}

int main(void)
{
	static uint8_t bits[PSB_BYTES];
	char nm[64];
	unsigned i, fn = 0, fp = 0;
	uint32_t h1, h2;
	struct psv_view v;
	uint64_t e = psv_make(0x1234, 0, 0);

	/* ---- the filter ---- */
	for (i = 0; i < 10000; i++) {
		name(nm, sizeof nm, "presence:ext", i);
		psb_hash(nm, strlen(nm), &h1, &h2);
		psb_add(bits, h1, h2);
	}
	for (i = 0; i < 10000; i++) {
		name(nm, sizeof nm, "presence:ext", i);
		psb_hash(nm, strlen(nm), &h1, &h2);
		fn += !psb_test(bits, h1, h2);
	}
	CHK(fn == 0, "filter: none of 10,000 names added is missing");
	for (i = 0; i < 200000; i++) {
		name(nm, sizeof nm, "other:chan", i);
		psb_hash(nm, strlen(nm), &h1, &h2);
		fp += psb_test(bits, h1, h2);
	}
	printf("  false positives at 10,000 names: %u of 200,000 (%.3f%%)\n",
		fp, 100.0 * fp / 200000);
	CHK(fp <= 400, "filter: at 10,000 names under 0.2% of other names pass (expected ~0.04%)");
	psb_hash("a", 1, &h1, &h2);
	CHK(h2 & 1, "filter: the probe step is odd");
	{
		uint32_t a1, a2, b1, b2;

		psb_hash("ab", 2, &a1, &a2);
		psb_hash("ba", 2, &b1, &b2);
		CHK(a1 != b1 && a2 != b2, "filter: two-byte names that differ by order hash apart");
	}

	/* ---- the version ---- */
	{
		uint64_t x = psv_make(0xBEEF, 7, 123456);

		CHK(psv_epoch(x) == 0xBEEF && psv_rebuild(x) == 7 && psv_adds(x) == 123456,
			"version: epoch, rebuild and adds round-trip");
	}

	/* ---- the view ---- */
	memset(&v, 0, sizeof v);
	CHK(!psv_covered(&v, e), "view: nothing applied covers nothing");
	CHK(psv_add(&v, e | 1) == PSV_FOREIGN, "view: an update before any full state is not applied");
	CHK(!psv_covered(&v, 0), "view: a zero heartbeat (relay me everything) is never covered");
	psv_full(&v, e | 5);
	CHK(psv_covered(&v, e | 5) && psv_covered(&v, e | 3),
		"view: a full state covers its own version and older heartbeats");
	CHK(!psv_covered(&v, e | 6), "view: a heartbeat one update ahead is not covered");
	CHK(psv_add(&v, e | 6) == PSV_APPLY && psv_covered(&v, e | 6),
		"view: the next update arrives and covers it");
	/* out of order inside the window */
	CHK(psv_add(&v, e | 8) == PSV_APPLY && !psv_covered(&v, e | 8),
		"view: update 8 before 7 is applied but leaves a hole");
	CHK(psv_add(&v, e | 7) == PSV_APPLY && psv_covered(&v, e | 8),
		"view: 7 arrives late and closes it");
	CHK(psv_add(&v, e | 4) == PSV_APPLY && psv_covered(&v, e | 8),
		"view: an old update is harmless");
	/* a lost update: the heartbeat moves past it and coverage stays short */
	psv_add(&v, e | 10);
	CHK(!psv_covered(&v, e | 10), "view: update 9 lost - a heartbeat at 10 is not covered");
	CHK(!psv_full_applies(&v, e | 7), "view: a full state older than what it holds is refused");
	CHK(psv_full_applies(&v, e | 10), "view: a full state at least as new is taken");
	psv_full(&v, e | 10);
	CHK(psv_covered(&v, e | 10), "view: the full state repairs the hole");
	/* beyond the window: content applies, coverage waits for a full state */
	CHK(psv_add(&v, e | 200) == PSV_APPLY && !psv_covered(&v, e | 200),
		"view: an update 190 ahead applies but cannot be tracked");
	psv_full(&v, e | 200);
	/* another epoch: the peer restarted */
	{
		uint64_t e2 = psv_make(0x4321, 0, 0);

		CHK(!psv_covered(&v, e2 | 200) && !psv_covered(&v, e2),
			"view: a heartbeat from a new epoch is never covered, whatever its count");
		CHK(psv_add(&v, e2 | 1) == PSV_FOREIGN, "view: an update from a new epoch is not applied");
		CHK(psv_full_applies(&v, e2 | 0), "view: a new epoch's full state is taken even at count 0");
	}
	/* a rebuild: still covered, but worth asking for */
	{
		uint64_t r = psv_make(0x1234, 1, 200);

		CHK(psv_covered(&v, r) && psv_behind_rebuild(&v, r),
			"view: a rebuild keeps coverage (a superset is safe) and asks for the new state");
		CHK(!psv_behind_rebuild(&v, e | 200), "view: no rebuild, nothing to ask");
	}

	printf("psinteresttest: %d passed, %d failed\n", pass, fail);
	return fail != 0;
}
