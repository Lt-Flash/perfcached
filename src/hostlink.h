/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* hostlink.h - S465: the node's network link, read beside hostmon's figures
 * (bytes a second against its speed, drops), for alerts.c's host.link */
#ifndef PC_HOSTLINK_H
#define PC_HOSTLINK_H

#include "hostmon.h"                   /* HM_N */

#define HL_PEAK_S   10                 /* the peak's window */
#define HL_RESOLVE_MS 10000            /* a bond's active slave, re-read */

struct hostlink {
	int on;                            /* a device is read */
	char want[64];                     /* link_monitor: "" = auto */
	char dev[64];                      /* where the node's address lives */
	char phys[64];                     /* the device whose counters are read */
	char path[192];                    /* dev -> ... -> phys, as said at start */
	char why[192];                     /* not watched: why */
	int speed_cfg, speed_mbps;         /* link_speed_mbps; the speed used, -1 = none */
	unsigned long long rx[HM_N], tx[HM_N], drops[HM_N], udperr[HM_N];
	long long ms[HM_N];
	int i, have;
	long long rx_bps, tx_bps;          /* bytes a second over the minute */
	int rx_pct, tx_pct;                /* ...of the speed; -1 = no speed */
	int rx_peak_pct, tx_peak_pct;      /* the busiest second of the last 10 */
	long long drops_ps, udperr_ps;     /* a second, over the minute */
};
extern struct hostlink HL;

struct hl_obs {
	int ok, moved, speed;
	unsigned long long rx, tx, drops, udperr;
	char phys[64], path[192];
	long long now;
};

/* @mode: "auto", "off" or a device; @addr: auto's address (NULL = the
 * default route's device); @speed_mbps > 0 stands for a device with none */
void hl_init(const char *mode, int speed_mbps, const char *addr);
/* one second's counters at @now (monotonic ms), no lock held; re-resolves
 * the physical device every HL_RESOLVE_MS */
void hl_read(struct hl_obs *o, long long now);
void hl_apply(const struct hl_obs *o, int fast);
/* the busier direction's share of the speed over the minute, -1 = none */
int hl_pct(void);

#endif
