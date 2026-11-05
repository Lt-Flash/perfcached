/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clepoch.h - the cluster wire epoch (S221, 12nu): what a peer's frames
 * mean.  This node speaks the fleet's COMMITTED epoch and reads any from
 * PC_CL_EPOCH_MIN to the newest it carries a codec for; the master
 * commits a new one once every live member speaks it, members adopt it
 * from the master's keepalive, and it is persisted (node-epoch) so a new
 * master continues with it.  pc_cluster_epoch, _min, _max and _commit
 * are in cluster.h.  Peer thread, but for the readers.
 */
#ifndef PC_CLEPOCH_H
#define PC_CLEPOCH_H

#include <stdint.h>

#include "pcaddr.h"

struct peer;

/* may we talk to a peer announcing @theirs (@what: "join", "keepalive",
 * ...)?  0 refuses it, loudly, by name */
int epoch_ok(int theirs, const pc_addr *from, const char *what);
/* a peer's fact from the settings its keepalive carried: 1 found */
int peer_fact(const struct peer *p, uint32_t key, uint32_t *val);
/* can @p speak @epoch?  1 yes, 0 no (its range, *lo..*hi, excludes it),
 * -1 unknown (a build before 0.6.5 says nothing) */
int peer_speaks(const struct peer *p, int epoch, int *lo, int *hi);
/* the committed epoch to the state dir, and back at start */
void epoch_save(void);
void epoch_load(void);
/* a member adopts the master's committed epoch (and persists it) */
void epoch_adopt(int epoch);
/* the newest epoch this build claims - the suites raise it to prove a
 * commit (PERFCACHED_TEST_EPOCH_MAX) */
void epoch_set_max(int epoch);

#endif
