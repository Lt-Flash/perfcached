/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clmesh.h - S36: `[cluster] discovery = unicast` - who the control
 * frames go to when there is no multicast group.
 *
 * AWS, GCP and Azure VPCs (and most Kubernetes pod networks) carry no
 * multicast.  Seven sends in cluster.c went to the group - every
 * node's ALIVE, the master's MASTER_ALIVE, a join request with no
 * target, the published map, GOODBYE and the beat watchdog's re-sends;
 * every data plane was already unicast to the live peers.  In unicast
 * mode those seven go, one sealed copy each, to the TARGET SET:
 *   - the seeds: `seeds = host[:port], ...`, DNS names re-resolved by a
 *     thread of their own every CLMESH_RESOLVE_MS and whenever asked
 *     (an instance replaced in a cloud comes back at a new address; a
 *     resolver that blocks for seconds must not stall the beat), and
 *   - every address this node has KNOWN - heard from (any admitted
 *     datagram), told about (a peer hint), or named by an adopted map -
 *     kept CLMESH_KNOWN_MS after it was last refreshed, so a member
 *     that went quiet is still beaten at while it may come back.
 * minus this node itself.  O(N^2) beats across the fleet - the stated
 * limit of the mesh (DESIGN 12hx); S304 is the scale step.
 *
 * Thread-safe: the cluster thread, the beat watchdog and the resolver
 * all touch it; one mutex, held for copies only.
 */
#ifndef PC_CLMESH_H
#define PC_CLMESH_H

#include <netinet/in.h>

#define CLMESH_MAX_SEEDS   32
#define CLMESH_MAX_KNOWN   1024
#define CLMESH_RESOLVE_MS  5000
#define CLMESH_KNOWN_MS    (10 * 60 * 1000)

/* parse `seeds` (comma/space separated host[:port]; a missing port is
 * @default_port, the cluster port) and start the resolver thread.
 * -1 on a malformed list (logged). */
int clmesh_start(const char *seeds, int default_port);
/* ask the resolver to run now (a join retry) - never blocks */
void clmesh_resolve_soon(void);
/* note @a (network order addr and port) as known now; 1 if it was not
 * known before (the caller may want to tell the master), else 0 */
int clmesh_heard(const struct sockaddr_in *a, long long now_ms);
/* the target set into @out (at most @cap), without @self; returns n */
int clmesh_targets(struct sockaddr_in *out, int cap,
		const struct sockaddr_in *self, long long now_ms);
/* 1 (and stamped) if @a is known and was last hinted more than
 * @every_ms ago - the rate limit on telling a master about it */
int clmesh_hint_due(const struct sockaddr_in *a, long long now_ms,
		long long every_ms);
/* the known addresses (not expired) into @out; returns n */
int clmesh_known(struct sockaddr_in *out, int cap, long long now_ms);
/* figures for /stats */
void clmesh_figures(int *seeds_named, int *seeds_resolved, int *known,
		unsigned long long *resolve_fail);

#endif
