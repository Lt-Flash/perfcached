/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* rpcqtest.c - the per-thread RPC queue: FIFO, full at PC_RPCQ, and still inside its arrays when
 * its counters pass 2^31 and 2^32 (the 3 ms replication flush gets there in about 75 days). */
#include <stdio.h>
#include <string.h>
#include "rpcq.h"

static int pass, fail;

static void chk(int cond, const char *what)
{
	if (cond)
		pass++;
	else
		fail++;
	printf("  %s %s\n", cond ? "ok  " : "FAIL", what);
}

static void f(int s, void *p) { (void)s; (void)p; }

/* the queue between guards, so a write outside it lands in them, not on the stack */
static struct { char lo[256]; struct pc_rpcq q; char hi[256]; } g;
static long vals[16];

static int guards_clean(void)
{
	size_t i;

	for (i = 0; i < sizeof g.lo; i++)
		if (g.lo[i] || g.hi[i])
			return 0;
	return 1;
}

static void run(unsigned int seed, const char *at)
{
	char what[160];
	pc_rpcq_fn fn;
	void *arg;
	int i, inside = 0, k, order = 1;

	memset(&g, 0, sizeof g);
	g.q.head = g.q.tail = seed;
	for (i = 0; i < 6; i++)
		pc_rpcq_put(&g.q, f, &vals[i]);
	for (k = 0; k < PC_RPCQ; k++)
		for (i = 0; i < 6; i++)
			if (g.q.arg[k] == &vals[i])
				inside++;
	snprintf(what, sizeof what, "six RPCs put from %s all land inside the queue", at);
	chk(inside == 6 && guards_clean(), what);
	for (i = 0; i < 6; i++)
		if (!pc_rpcq_take(&g.q, &fn, &arg) || arg != &vals[i] || fn != f)
			order = 0;
	snprintf(what, sizeof what, "and come out in order, then empty (%s)", at);
	chk(order && !pc_rpcq_take(&g.q, &fn, &arg), what);
}

int main(void)
{
	pc_rpcq_fn fn;
	void *arg;
	int i, n = 0;

	memset(&g, 0, sizeof g);
	for (i = 0; i < PC_RPCQ + 3; i++)
		n += pc_rpcq_put(&g.q, f, &vals[i]);
	chk(n == PC_RPCQ, "the queue takes PC_RPCQ and refuses the rest");
	chk(pc_rpcq_take(&g.q, &fn, &arg) && arg == &vals[0], "the first in is the first out");

	run(0, "0");
	run(0x7ffffffdu, "3 short of 2^31");
	run(0xfffffffdu, "3 short of 2^32");

	printf("rpcqtest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
