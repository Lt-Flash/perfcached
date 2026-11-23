/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* thrtabtest.c - the daemon's thread table holds every thread the config can ask for (512 workers,
 * a WAL, a cluster with CLAPPLY_MAX apply and 16 relay threads), and refuses a slot past its end. */
#include <stdio.h>
#include "thrtab.h"
#include "clapply.h"

static int pass, fail;

static void chk(int cond, const char *what)
{
	if (cond)
		pass++;
	else
		fail++;
	printf("  %s %s\n", cond ? "ok  " : "FAIL", what);
}

int main(void)
{
	char what[160];
	int i, n = 0, last = 0, need;

	need = pc_thread_slots(512, 0, 0, 0, 0);
	snprintf(what, sizeof what, "512 workers standalone: %d slots, the table %d", need,
		PC_THREAD_SLOTS);
	chk(need <= PC_THREAD_SLOTS, what);
	need = pc_thread_slots(495, 1, 1, 1, 15);
	snprintf(what, sizeof what, "495 workers, WAL, cluster, defaults: %d slots, the table %d",
		need, PC_THREAD_SLOTS);
	chk(need <= PC_THREAD_SLOTS, what);
	need = pc_thread_slots(512, 1, 1, CLAPPLY_MAX, 16);
	snprintf(what, sizeof what, "512 workers, WAL, cluster, %d apply, 16 relay: %d slots, "
		"the table %d", CLAPPLY_MAX, need, PC_THREAD_SLOTS);
	chk(need <= PC_THREAD_SLOTS, what);

	for (i = 0; i < PC_THREAD_SLOTS; i++)
		if (pc_thread_slot_take(&n, PC_THREAD_SLOTS) != i)
			last = -1;
	chk(last == 0 && n == PC_THREAD_SLOTS, "every slot of the table taken in order");
	chk(pc_thread_slot_take(&n, PC_THREAD_SLOTS) == -1 && n == PC_THREAD_SLOTS,
		"one past the end refused, the count unchanged");

	printf("thrtabtest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
