/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clmesh.h - unicast discovery: control frames go to the seeds (re-resolved by a thread of their
 * own) plus every recently known address, minus self.  Any thread; one mutex, held for copies. */
#ifndef PC_CLMESH_H
#define PC_CLMESH_H

#include <netinet/in.h>
#include "pcaddr.h"

#define CLMESH_MAX_SEEDS   32
#define CLMESH_MAX_KNOWN   1024
#define CLMESH_RESOLVE_MS  5000
#define CLMESH_KNOWN_MS    (10 * 60 * 1000)
/* an address heard only from a JOIN_REQ, a hint or an unconfirmed beacon is kept this long: a
 * real joiner re-announces every second, a replayed JOIN_REQ must not buy CLMESH_KNOWN_MS */
#define CLMESH_TRIAL_MS    (30 * 1000)

/* parse `seeds` (comma/space separated host[:port], the port defaulting to @default_port) and
 * start the resolver thread; -1 on a malformed list (logged) */
int clmesh_start(const char *seeds, int default_port);
/* ask the resolver to run now (a join retry) - never blocks */
void clmesh_resolve_soon(void);
/* note @a (network order addr and port) as known now; 1 if it was not
 * known before (the caller may want to tell the master), else 0 */
int clmesh_heard(const pc_addr *a, long long now_ms, int trial);
/* the target set into @out (at most @cap), without @self; returns n */
int clmesh_targets(pc_addr *out, int cap,
		const pc_addr *self, long long now_ms);
/* 1 (and stamped) if @a is known and was last hinted more than
 * @every_ms ago - the rate limit on telling a master about it */
int clmesh_hint_due(const pc_addr *a, long long now_ms,
		long long every_ms);
/* the known addresses (not expired) into @out; returns n */
int clmesh_known(pc_addr *out, int cap, long long now_ms);
/* figures for /stats */
void clmesh_figures(int *seeds_named, int *seeds_resolved, int *known,
		unsigned long long *resolve_fail);

#endif
