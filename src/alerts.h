/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* alerts.h - a node says itself when something is wrong: conditions held and cleared over set
 * times, kept in a ring (/stats `alerts`), and mailed, or relayed to the fleet's elected mailer */
#ifndef PC_ALERTS_H
#define PC_ALERTS_H

#include "config.h"

#define PC_ALERT_RING    100
#define PC_ALERT_KEYS    64            /* distinct keys tracked at once */
#define PC_ALERT_TEXT    200

#define PC_AL_INFO       0             /* routine (a clean start) */
#define PC_AL_WARNING    1             /* = PC_MAIL_LVL_WARNING */
#define PC_AL_CRITICAL   2             /* = PC_MAIL_LVL_CRITICAL */

struct pc_tw;

/* before the maintenance thread starts: [mail]'s level, min_interval_s and digest_s;
 * PERFCACHED_ALERT_FAST=<n> divides every hold and clear time by n - for tests */
void pc_alerts_init(const struct pc_config *cfg);

/* the maintenance thread, once a second */
void pc_alerts_tick(void);
/* the CPUs this process may use - its affinity, capped by a cgroup
 * cpu.max quota (fractional when a quota is) */
double pc_process_cpus(void);

/* /stats `alerts`: active, recent (newest first), the mailer */
void pc_alerts_stats_tree(struct pc_tw *out);

/* the fleet side: a relay from another node for the tick to mail - the cluster thread calls it
 * (pc_cluster_set_alert_relay_cb), and it only queues */
struct pc_alert_relay;
void pc_alerts_relayed(const struct pc_alert_relay *r);
/* relays mailed here for other nodes, sent from here, and lost (a full
 * inbox, a send that failed) - cumulative since start or the last reset */
void pc_alerts_relay_stats(unsigned long long *in, unsigned long long *out,
		unsigned long long *dropped);
void pc_alerts_stats_reset(void);
/* /metrics: the relay counters since start (never reset) and the alerts
 * raised now, by level */
void pc_alerts_metrics(unsigned long long *in, unsigned long long *out,
		unsigned long long *dropped, int *warning, int *critical);
/* the link's figures, copied under the tick's lock; 0 = not watched */
struct hostlink;
int pc_alerts_link(struct hostlink *out);
/* for this node's ALIVE, lock-free - the busier direction's % (-1 =
 * not watched) and host.link's level (0 none, 1 warning, 2 critical) */
void pc_alerts_link_state(int *pct, int *level);

#endif /* PC_ALERTS_H */
