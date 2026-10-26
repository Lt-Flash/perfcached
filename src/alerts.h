/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * alerts.h - S352b: a node says itself when something is wrong.
 *
 * The maintenance thread evaluates a fixed set of conditions once a
 * second (pc_alerts_tick).  Each has a KEY (kind + subject), a level, a
 * time it must hold before it is raised and a time it must stay away
 * before it is cleared.  A raise and its clear are logged, kept in a ring
 * of the last PC_ALERT_RING (/stats `alerts`, the page), and mailed when
 * this node has a mailer (mail.h) and the level reaches [mail] level:
 * the same key at most once per min_interval_s, and everything raised or
 * cleared within digest_s in one mail.  A clear is mailed only when its
 * raise was.  Mail is one way out, not the only record.
 *
 * S352c: a node without [mail] relays what it would mail to the elected
 * mailer, and fleet events (a member gone, missing, the fleet below a
 * majority) are mailed by that one node alone - see alerts.c.
 */
#ifndef PC_ALERTS_H
#define PC_ALERTS_H

#include "config.h"

#define PC_ALERT_RING    100
#define PC_ALERT_KEYS    64            /* distinct keys tracked at once */
#define PC_ALERT_TEXT    200

#define PC_AL_INFO       0             /* S377: routine (a clean start) */
#define PC_AL_WARNING    1             /* = PC_MAIL_LVL_WARNING */
#define PC_AL_CRITICAL   2             /* = PC_MAIL_LVL_CRITICAL */

struct pc_tw;

/* before the maintenance thread starts: [mail]'s level, min_interval_s
 * and digest_s (defaults without [mail]).  PERFCACHED_ALERT_FAST=<n> in
 * the environment divides every hold and clear time by n - for tests. */
void pc_alerts_init(const struct pc_config *cfg);

/* the maintenance thread, once a second */
void pc_alerts_tick(void);
/* S381: the CPUs this process may use - its affinity, capped by a cgroup
 * cpu.max quota (fractional when a quota is) */
double pc_process_cpus(void);

/* /stats `alerts`: active, recent (newest first), the mailer */
void pc_alerts_stats_tree(struct pc_tw *out);

/* S352c (DESIGN 12le): the fleet side.  A relay from another node, for the
 * tick to mail - the cluster thread calls it (pc_cluster_set_alert_relay_cb)
 * and it only queues */
struct pc_alert_relay;
void pc_alerts_relayed(const struct pc_alert_relay *r);
/* relays mailed here for other nodes, sent from here, and lost (a full
 * inbox, a send that failed) - cumulative since start or the last reset */
void pc_alerts_relay_stats(unsigned long long *in, unsigned long long *out,
		unsigned long long *dropped);
void pc_alerts_stats_reset(void);        /* S358 */
/* /metrics: the relay counters since start (never reset) and the alerts
 * raised now, by level */
void pc_alerts_metrics(unsigned long long *in, unsigned long long *out,
		unsigned long long *dropped, int *warning, int *critical);

#endif /* PC_ALERTS_H */
