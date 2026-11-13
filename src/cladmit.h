/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* cladmit.h - admission: the master's queue and gate, the link check holding a joiner CONNECTING,
 * ELECTING from a masterless fleet, member-list repair.  Every entry runs on the peer thread. */
#ifndef PC_CLADMIT_H
#define PC_CLADMIT_H

#include <stddef.h>
#include <stdint.h>

#include "pcaddr.h"

struct peer;

/* handle_join_req, as the master: 1 = queued (told QUEUED) or dropped, 0 = handle it now */
int cladmit_hold(const unsigned char *pt, size_t n, const pc_addr *from, uint64_t tok);
/* handle_join_req, as a member: ELECTING to the joiner while its master is silent */
void cladmit_electing(const pc_addr *from, uint64_t tok, long long master_seen_ms);
/* become_master: the gate counts from @now; the queue and the checks start over */
void cladmit_master_begin(long long now);
/* membership_tick, as the master; handle_alive, as the master (an ALIVE may open the gate) */
void cladmit_tick(long long now);
void cladmit_alive(long long now);
/* pc_cluster_thread, every loop: the link check's probes and reports, every 500 ms */
void cladmit_link_tick(long long now);
struct clmemb_assign;
/* handle_assign: admitted CONNECTING by the ASSIGN answering @tok; a later part of it */
void cladmit_connect_begin(long long now, uint64_t tok);
void cladmit_assign_part(const unsigned char *pt, size_t n, const struct clmemb_assign *as,
		const pc_addr *from);
/* handle_lane_probe_r: @p (NULL: none) answered a probe */
void cladmit_probe_answered(struct peer *p, long long now);
/* membership_tick: when this node went ONLINE, 0 = never */
long long cladmit_online_ms(void);
/* handle_master_alive, as a member: the beacon's digest against this node's view */
void cladmit_members_check(uint64_t theirs, const pc_addr *master);

/* send_assign, and the repair's M_MEMBERS: the member list, in parts past a datagram */
void send_member_list(unsigned char type, const pc_addr *to, uint64_t tok, int your_id);

/* dispatch_dgram: M_MEMBERS_REQ, M_MEMBERS, M_LINK_REPORT, M_ONLINE */
void cladmit_members_req(const unsigned char *pt, size_t n, const pc_addr *from);
void cladmit_members(const unsigned char *pt, size_t n, const pc_addr *from);
void cladmit_link_report(const unsigned char *pt, size_t n, const pc_addr *from);
void cladmit_online(const unsigned char *pt, size_t n, const pc_addr *from);

#endif /* PC_CLADMIT_H */
