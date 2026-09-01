/*
 * jwtest.c — the reply writer, including the heap mode CLUSTER SLOTS
 * needs.
 *
 * The growth path is the reason this file exists.  In production the
 * SLOTS branch sizes its buffer from the range count and gets it right,
 * so the realloc never runs - which would leave it untested until the
 * day an estimate is wrong.  Here it is driven directly.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "json.h"

static int pass, fail;
static void ok(const char *m) { pass++; printf("  ok   %s\n", m); }
static void bad(const char *m) { fail++; printf("  FAIL %s\n", m); }

/* a fixed writer must REFUSE to overrun and say so */
static void t_fixed(void)
{
	char buf[16];
	struct pc_jw w;

	pc_jw_init(&w, buf, sizeof buf);
	pc_jw_raw(&w, "0123456789", 10);
	if (w.len == 10 && !w.overflow)
		ok("fixed writer accepts what fits");
	else
		bad("fixed writer mishandled a fitting write");
	pc_jw_raw(&w, "0123456789", 10);
	if (w.overflow && w.len == 10)
		ok("fixed writer flags overflow and drops the write");
	else
		bad("fixed writer did NOT flag overflow (len now doubtful)");
	if (!w.owned)
		ok("fixed writer owns nothing to free");
	else
		bad("fixed writer claims ownership of a stack buffer");
	pc_jw_free(&w);                        /* must be a no-op, not a free */
	ok("pc_jw_free on a fixed writer is safe");
}

/* the heap writer must GROW rather than flag, and keep every byte */
static void t_grow(void)
{
	struct pc_jw w;
	size_t i, want = 0;
	char *ref;
	int intact = 1;

	memset(&w, 0, sizeof w);
	if (pc_jw_init_heap(&w, 64) != 0) {
		bad("pc_jw_init_heap(64) failed");
		return;
	}
	ok("heap writer initialised");
	ref = malloc(200 * 37 + 1);
	if (!ref) { bad("out of memory in the test"); pc_jw_free(&w); return; }
	/* 200 writes of 37 bytes = 7400, from a 64-byte start: many grows */
	for (i = 0; i < 200; i++) {
		static const char chunk[] = "abcdefghijklmnopqrstuvwxyz0123456789\n";

		pc_jw_raw(&w, chunk, 37);
		memcpy(ref + want, chunk, 37);
		want += 37;
	}
	if (w.overflow)
		bad("heap writer flagged overflow instead of growing");
	else if (w.len != want)
		bad("heap writer lost bytes while growing");
	else {
		ok("heap writer grew from 64 bytes to hold 7400");
		for (i = 0; i < want; i++)
			if (w.buf[i] != ref[i]) { intact = 0; break; }
		if (intact)
			ok("every byte survived the reallocs intact");
		else
			bad("bytes were corrupted across a realloc");
	}
	if (w.cap >= want)
		ok("capacity covers the content");
	else
		bad("capacity is below the content length");
	free(ref);
	pc_jw_free(&w);
	if (!w.buf && !w.owned)
		ok("pc_jw_free releases and clears the heap writer");
	else
		bad("pc_jw_free left the writer pointing at freed memory");
}

/* the bound still has to hold, or a bug becomes an OOM */
static void t_bound(void)
{
	struct pc_jw w;

	/* the len guard reads whatever is in the struct, so a caller with
	 * an UNINITIALISED stack writer gets a refusal that presents as
	 * OOM - exactly how CLUSTER NODES shipped broken.  Every call
	 * site must zero or pc_jw_init first; asserted here so the
	 * contract has a name. */
	memset(&w, 0, sizeof w);
	if (pc_jw_init_heap(&w, (size_t)JW_HEAP_MAX + 1) != 0)
		ok("pc_jw_init_heap refuses a capacity past JW_HEAP_MAX");
	else {
		bad("pc_jw_init_heap accepted a capacity past JW_HEAP_MAX");
		pc_jw_free(&w);
	}
	memset(&w, 0, sizeof w);
	pc_jw_init(&w, NULL, 0);
	w.len = 1;                             /* pretend something was written */
	if (pc_jw_init_heap(&w, 4096) != 0)
		ok("heap mode is refused once bytes have been written");
	else
		bad("heap mode stranded already-written bytes");
}

int main(void)
{
	t_fixed();
	t_grow();
	t_bound();
	printf("jwtest: %d passed, %d failed\n", pass, fail);
	return fail != 0;
}
