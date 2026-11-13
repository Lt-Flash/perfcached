/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* cltomb.h - tombstones+acks: the watermark plane's ticks and frames and the
 * tombstone catch-up.  Every entry point runs on the peer thread. */
#ifndef PC_CLTOMB_H
#define PC_CLTOMB_H

#include <stddef.h>
#include <stdint.h>

#include "pcaddr.h"

struct peer;

/* pc_cluster_thread, every beat */
void acks_forget_tick(unsigned int now_s);
void acks_gap_tick(long long now);
void acks_rebuild_tick(long long now);
/* pc_rebalance_tick */
void tomb_catchup_tick(void);

/* dispatch_dgram's M_GAP_REQ and M_GAP_FLOOR */
void handle_gap_req(const unsigned char *pt, size_t n, const pc_addr *from);
void handle_gap_floor(const unsigned char *pt, size_t n, const pc_addr *from);

/* send_alive, handle_alive, boot_request, handle_rebuild */
uint64_t freeze_mark(uint64_t oid, uint32_t incarn, uint64_t mark);
void acks_back_check(struct peer *p, uint64_t member,
		long long was, long long now);
void acks_boot_floors(const int *ids, int n);
void rebuild_send(const pc_addr *to, int kind, long long age_ms);

#endif /* PC_CLTOMB_H */
