/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clddl.h - the collection DDL across the fleet: what cluster.c's loops call.
 * The announces and pc_cluster_bad_col_names are cluster.h's. */
#ifndef PC_CLDDL_H
#define PC_CLDDL_H

#include <stddef.h>

struct peer;

/* heartbeat thread: the due repeats of a RESIZE announce */
void rz_again_tick(long long now);
/* peer thread: the created set, the RESP map and the autoscale modes, to a
 * peer seen for the first time */
void col_sync_peer(struct peer *p);
/* peer thread: an M_COL_SET from a peer */
void handle_col_set(const unsigned char *pt, size_t n);

#endif /* PC_CLDDL_H */
