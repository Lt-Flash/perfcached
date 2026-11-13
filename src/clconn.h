/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clconn.h - does a peer's TCP listener answer?  One non-blocking connect per (node,
 * path), polled on the 500 ms link check, closed once it completes.  Peer thread only. */
#ifndef PC_CLCONN_H
#define PC_CLCONN_H

#include "pcaddr.h"

#define CLCONN_MAX        256          /* probes in flight at once */
#define CLCONN_TIMEOUT_MS 2000         /* one not connected by then failed */

enum { CLCONN_LANE, CLCONN_RPT, CLCONN_BULK, CLCONN_PATHS };

/* 1: @to accepted a connection since the probe began; 0: in flight (or no slot free); -1: it
 * failed or timed out - the next call begins another.  @src binds the source (port 0) */
int clconn_probe(int node, int path, const pc_addr *src, const pc_addr *to, long long now);
/* close every probe not asked about since @since (a check that ended, a peer gone) */
void clconn_reap(long long since);
/* tests: probes open now */
int clconn_open(void);

#endif /* PC_CLCONN_H */
