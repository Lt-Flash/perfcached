/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* addrwait.h - wait for a configured local address before binding it: network-online.target
 * promises nothing without a wait-online service (IP_FREEBIND would hide a typo'd address). */
#ifndef PC_ADDRWAIT_H
#define PC_ADDRWAIT_H

/* 0: @addr is on this host, or is not an IP this helper can judge (the caller's bind reports
 * those); -1: it did not appear within @wait_s seconds (an ERROR naming @what) */
int pc_addr_wait(const char *addr, int wait_s, const char *what);

#endif
