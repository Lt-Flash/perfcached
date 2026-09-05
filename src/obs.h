/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * obs.h — the observability surfaces the Grafana Redis datasource
 * renders its panels from (S53): per-command statistics, the client
 * registry behind CLIENT LIST, and the slow log.
 *
 * Everything here is monitoring, so the accounting rules are relaxed
 * on purpose and stated: per-worker rows are written by their owner
 * only (the CP-06 pattern) and merged racily at read - a snapshot may
 * be a few operations stale, never torn enough to matter.  Nothing in
 * this file may add a lock to a request path.
 */
#ifndef PC_OBS_H
#define PC_OBS_H

#include <stddef.h>
#include <sys/socket.h>

struct pc_jw;

/* sized at boot: cfg->workers rows plus one shared spare for
 * non-worker threads.  Called once, pre-fork of the worker set. */
int pc_obs_init(int nworkers, long long slowlog_usec);

/* ---- command statistics -------------------------------------------- */
unsigned long long pc_obs_usec_now(void);
/* record one executed command on the calling worker's row.  @name need
 * not be lowercase; it is folded here. */
void pc_obs_cmd(const char *name, size_t nlen, unsigned long long usec);
/* emit "cmdstat_<name>:calls=..,usec=..,usec_per_call=.." lines */
void pc_obs_cmdstats(struct pc_jw *w);
unsigned long long pc_obs_total_calls(void);
/* the maint thread's 1 Hz tick feeds instantaneous_ops_per_sec */
void pc_obs_tick_1hz(void);
unsigned long long pc_obs_inst_ops(void);

/* ---- the client registry ------------------------------------------- */
void *pc_obs_conn_add(const struct sockaddr *sa, socklen_t slen, int fd,
		int resp_only);
void pc_obs_conn_del(void *row);
void pc_obs_conn_touch(void *row, const char *cmd, size_t clen,
		unsigned int now_ticks);
void pc_obs_conn_name(void *row, const char *name, size_t nlen);
int pc_obs_conn_count(void);
void pc_obs_client_list(struct pc_jw *w, unsigned int now_ticks);

/* ---- the slow log --------------------------------------------------- */
/* record a finished RESP command; logs only past the configured
 * threshold.  argv is truncated for storage, never for judgement. */
void pc_obs_slow(char *const *argv, const size_t *argl, int nargs,
		unsigned long long usec, void *connrow);
void pc_obs_slowlog_get(struct pc_jw *w, int want);
long long pc_obs_slowlog_len(void);
void pc_obs_slowlog_reset(void);

/* S65: the query log - a line per request (or one in N per worker)
 * through LM_INFO, off by default and free when off.  @dialect names
 * the door (json / bin / resp); @row is the connection's client-list
 * row, or NULL.  Capped at QLOG_MAX_PER_SEC lines a second across the
 * daemon; what is dropped is counted and said once a second. */
void pc_obs_qlog_config(int every, int keys);
int  pc_obs_qlog_on(void);
void pc_obs_qlog(const char *dialect, void *row, const char *verb,
		size_t vlen, const char *col, size_t clen, const char *key,
		size_t klen, unsigned long long usec);

#endif /* PC_OBS_H */
