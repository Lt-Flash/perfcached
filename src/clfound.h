/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clfound.h - who founds a blank fleet: the joiners a joining node hears, the unicast founding
 * rule and the settle window at wire epoch 4.  Every entry point runs on the peer thread. */
#ifndef PC_CLFOUND_H
#define PC_CLFOUND_H

#include "pcaddr.h"

struct clmemb_wait;

/* handle_join_req, while joining: a joiner heard (@ident NULL: it sends none) */
void clfound_note(const pc_addr *a, const unsigned char *ident, long long now);
/* membership_tick: 1 = this blank unicast node must not found yet */
int clfound_gate_blocks(long long now);
/* membership_tick, at epoch 4: when this joiner may found */
long long clfound_settle_at(long long now);
/* a join starts: the settle window opens at @now */
void clfound_window_open(long long now);
/* handle_join_wait, QUEUED or ELECTING: the window restarts; held until the time returned */
long long clfound_wait(const struct clmemb_wait *w, const pc_addr *from, long long now);

#endif /* PC_CLFOUND_H */
