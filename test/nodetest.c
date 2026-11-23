/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* nodetest.c - the node's state (node.c) set from two threads: the WAL's FAILED landing between
 * the cluster thread's check and its store must stand, and a DRAINING node keeps its reason. */
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include "cluster.h"
#include "wal.h"

extern void (*pc_node_test_mid)(void);

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static pthread_t wal_thr;
static int wal_done, mid_once;

static void *wal_main(void *a)
{
	(void)a;
	pc_node_wal_shed(PC_WAL_SHED_IO);
	__atomic_store_n(&wal_done, 1, __ATOMIC_RELEASE);
	return NULL;
}

/* the first transition's window: the WAL thread sheds, waited for up to 300 ms (a serialised
 * setter makes it wait for this one instead) */
static void mid(void)
{
	int i;

	if (__atomic_exchange_n(&mid_once, 1, __ATOMIC_ACQ_REL))
		return;
	if (pthread_create(&wal_thr, NULL, wal_main, NULL) != 0)
		return;
	for (i = 0; i < 300 && !__atomic_load_n(&wal_done, __ATOMIC_ACQUIRE); i++)
		usleep(1000);
}

int main(void)
{
	/* A: RECOVERING -> READY on this thread, the WAL's FAILED inside its window */
	pc_node_state_set(PC_NST_RECOVERING);
	pc_node_test_mid = mid;
	pc_node_state_set(PC_NST_READY);
	if (__atomic_load_n(&mid_once, __ATOMIC_ACQUIRE))
		pthread_join(wal_thr, NULL);
	pc_node_test_mid = NULL;
	CHK(pc_node_state() == PC_NST_FAILED && pc_node_reason() == PC_NRE_WAL_IO,
		"A: the WAL's FAILED inside a READY transition stands - state %s, reason %d",
		pc_node_state_name(pc_node_state()), pc_node_reason());

	/* B: a DRAINING node the WAL then sheds keeps DRAINING and its reason */
	pc_node_state_set(PC_NST_DRAINING);
	pc_node_wal_shed(PC_WAL_SHED_LOSS);
	CHK(pc_node_state() == PC_NST_DRAINING && pc_node_reason() == PC_NRE_DRAINING,
		"B: a shed refused on a DRAINING node leaves its reason - state %s, reason %d",
		pc_node_state_name(pc_node_state()), pc_node_reason());

	printf("nodetest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
