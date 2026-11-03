/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * statscluster.h - the stats tree's "cluster" block (statscluster.c), for
 * statstree.c.
 */
#ifndef PC_STATSCLUSTER_H
#define PC_STATSCLUSTER_H

#include "ptree.h"

/* the value of the "cluster" key: a map, or null when clustering is off */
void stats_tree_cluster(struct pc_tw *out);

#endif
