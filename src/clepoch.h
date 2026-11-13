/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clepoch.h - the wire epoch: a node speaks the fleet's COMMITTED one, which the master
 * raises once every live member speaks it, and reads any it has a codec for.  Peer thread. */
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
 * -1 unknown (an older build says nothing) */
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
