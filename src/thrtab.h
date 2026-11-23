/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* thrtab.h - how many slots daemon.c's thread table has and how many a run takes; a header of its
 * own so test/thrtabtest.c checks the two against the config's ceilings without the daemon. */
#ifndef PC_THRTAB_H
#define PC_THRTAB_H

/* the most a config asks for: 512 workers, maint, wal, rdb, peer, bulk, beat, 16 apply threads
 * (CLAPPLY_MAX) and 16 relay threads */
#define PC_THREAD_SLOTS (512 + 6 + 16 + 16)

/* one per worker, maint and wal; rdb with a WAL dir; peer, bulk, beat, the apply and the relay
 * threads with a cluster */
static inline int pc_thread_slots(int workers, int wal, int cluster, int napply, int npsrx)
{
	return workers + 2 + (wal ? 1 : 0) + (cluster ? 3 + napply + npsrx : 0);
}

/* the next free slot of @cap, or -1 */
static inline int pc_thread_slot_take(int *n, int cap)
{
	if (*n >= cap)
		return -1;
	return (*n)++;
}

#endif
