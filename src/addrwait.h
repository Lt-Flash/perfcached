/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * addrwait.h - S294: wait for a configured local address to exist before
 * binding it.
 *
 * A unit ordered After=network-online.target promises nothing where no
 * wait-online service exists - on the Debian 13 LXCs perfcached runs on,
 * systemd-networkd-wait-online is disabled and no other is installed, so
 * the target is reached at once and the daemon can start before its
 * address is configured.  The first bind then fails EADDRNOTAVAIL, the
 * start is an ERROR, and systemd's restart picks it up a second later -
 * a working node that reads as a failure (245, 2026-10-01).
 *
 * So a configured, non-wildcard address that is not on the host YET is
 * waited for, up to [daemon] address_wait_s (default 30, 0 = never),
 * with one NOTICE when the wait starts and one when it ends.  An address
 * that never appears is still an ERROR at the end of the window - a
 * typo'd address must not hide (which is why IP_FREEBIND is not used).
 */
#ifndef PC_ADDRWAIT_H
#define PC_ADDRWAIT_H

/* 0: @addr is on this host (or is a wildcard / a name / not an IP this
 * helper can judge - the caller's own bind reports those); -1: it did
 * not appear within @wait_s seconds (logged as an ERROR naming @what) */
int pc_addr_wait(const char *addr, int wait_s, const char *what);

#endif
