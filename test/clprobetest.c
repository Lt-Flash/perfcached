/* clprobetest.c - the lane probes' table (clprobe.h): an address being probed keeps its entry while
 * other addresses come and go, once the table is full too. */
#include <stdio.h>
#include <arpa/inet.h>

#include "clprobe.h"

static int pass, fail;
static void chk(int cond, const char *what)
{
	if (cond) { pass++; return; }
	fail++;
	printf("  FAIL %s\n", what);
}

static pc_addr addr(int n)
{
	pc_addr a;

	memset(&a, 0, sizeof a);
	pc_addr_from_v4(&a, htonl(0x0a000000u | (unsigned)n), 17000);
	return a;
}

int main(void)
{
	static struct clprobe_tab t;
	struct lprobe *p, *q;
	pc_addr a, b;
	long long now = 1000;
	int i, kept = 1;

	/* 64 addresses probed once, long ago */
	for (i = 1; i <= CLPROBE_MAX; i++) {
		pc_addr x = addr(i);

		p = clprobe_slot(&t, &x, 1, now++);
		chk(p != NULL, "fill: every address gets an entry");
		p->ms = now;
	}
	a = addr(1000);
	b = addr(1001);
	/* two new silent addresses probed on every tick, in turn: neither may reset the other */
	now += 100000;
	p = clprobe_slot(&t, &a, 1, now);
	p->ms = now;
	q = clprobe_slot(&t, &b, 1, now);
	q->ms = now + 1;
	for (i = 0; i < 10; i++) {
		now += 1000;
		p = clprobe_slot(&t, &a, 1, now);
		q = clprobe_slot(&t, &b, 1, now);
		if (p == q || p->ms == 0 || q->ms == 0)
			kept = 0;
	}
	chk(kept, "full table: two new addresses probed in turn keep an entry each (one slot shared resets both every tick)");
	p = clprobe_slot(&t, &a, 0, now);
	chk(p && p->ms == 100000 + 1000 + CLPROBE_MAX, "full table: the first new address still holds the stamp its first probe set");
	q = clprobe_slot(&t, &b, 0, now);
	chk(q && q->ms == p->ms + 1, "full table: and so does the second");
	chk(clprobe_slot(&t, &a, 0, now) == p, "a lookup without make finds the entry");
	{
		pc_addr x = addr(5000);

		chk(clprobe_slot(&t, &x, 0, now) == NULL, "a lookup without make finds nothing for an unknown address");
	}
	printf("clprobetest: %d passed, %d failed\n", pass, fail);
	return fail != 0;
}
