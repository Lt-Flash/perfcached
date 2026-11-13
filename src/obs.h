/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* obs.h - per-command statistics, the CLIENT LIST registry and the slow log.  Per-worker rows
 * are owner-written and merged racily at read: never add a lock to a request path. */
#ifndef PC_OBS_H
#define PC_OBS_H

#include <stddef.h>
#include <sys/socket.h>
#include "ptree.h"                     /* the tree writers */
#include "clunk.h"                     /* CLUNK_DIALECTS sizes the totals */

struct pc_jw;                          /* the RESP door's INFO writers */

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

/* command rows merged by name across the workers: RESP commands by bare name, native verbs as
 * bin:<verb> and CMD methods as cmd:<method>, since the two cost differently */
#define PC_OBS_NAME 48           /* room for dashboard@<host>, perfcli@<host> */
/* the last PC_OBS_WIN minutes beside the totals: a window is now minus the oldest per-minute
 * snapshot, taken by the 1 Hz tick; /metrics keeps the cumulative form Prometheus needs */
#define PC_OBS_WIN  5
#define PC_OBS_SUB  8                   /* buckets per octave */
#define PC_OBS_HIST 129                 /* octaves 1..16 x SUB, + overflow */
struct pc_obs_cmdsum {
	char name[PC_OBS_NAME];
	unsigned int nlen;
	unsigned long long calls, usec, max;
	unsigned long long hist[PC_OBS_HIST];
	/* the same three over the last PC_OBS_WIN minutes */
	unsigned long long w_calls, w_usec, w_max;
	unsigned long long w_hist[PC_OBS_HIST];
	unsigned int w_secs;               /* how long that window is */
};

/* inclusive upper bound of bucket @k in us - the one place that knows the layout */
unsigned long long pc_obs_hist_hi(int k);
int  pc_obs_cmd_merge(struct pc_obs_cmdsum *out, int cap);
void pc_obs_latencystats(struct pc_jw *w);           /* INFO latencystats */
/* the native `stats` reply's "commands" and "slowlog" arrays, rendered as JSON for /stats */
void pc_obs_commands_tree(struct pc_tw *w);
void pc_obs_slowlog_tree(struct pc_tw *w, int max);
int  pc_obs_slow_would(unsigned long long usec);     /* slow log wants it? */
void pc_obs_set_slowlog(long long usec);             /* a config reload */
/* the row for a name on THIS worker, for the caller to cache thread-locally: it skips the
 * per-request lowercase-and-hash of pc_obs_cmd().  NULL when the row is full. */
void *pc_obs_cmd_slot(const char *name, size_t nlen);
void  pc_obs_cmd_at(void *slot, unsigned long long usec);
unsigned long long pc_obs_total_calls(void);
/* the maint thread's 1 Hz tick feeds instantaneous_ops_per_sec */
void pc_obs_tick_1hz(void);
unsigned long long pc_obs_inst_ops(void);

/* ---- the client registry ------------------------------------------- */
void *pc_obs_conn_add(const struct sockaddr *sa, socklen_t slen, int fd,
		int resp_only);
void pc_obs_conn_del(void *row);
const char *pc_obs_conn_addr(void *row);
unsigned long long pc_obs_conn_cmds(void *row);
void pc_obs_conn_touch(void *row, const char *cmd, size_t clen,
		unsigned int now_ticks);
void pc_obs_conn_name(void *row, const char *name, size_t nlen);
/* the row's name as set (CLIENT SETNAME / the native `name` verb), "" when none */
const char *pc_obs_conn_name_get(void *row, size_t *nlen);
/* CLIENT SETINFO: @ver 0 sets lib-name, 1 lib-ver; kept to 47 bytes */
void pc_obs_conn_lib(void *row, int ver, const char *v, size_t n);
int pc_obs_conn_count(void);
/* each worker's live native connections, workers 1..N into @out; returns N */
int pc_obs_worker_conns(int *out, int max);
/* /clients extras, owner-written: door and dialect (NULL = keep), encrypted (0/1, -1 = keep),
 * the bytes staged for the socket (the slow-consumer measure), the subscription count */
void pc_obs_conn_door(void *row, const char *door, const char *dialect,
		int encrypted);
void pc_obs_conn_pending(void *row, size_t bytes);
void pc_obs_conn_subs(void *row, int n);
/* {total, shown, clients: [rows]} for the data doors only (no HTTP), active first, at most @cap
 * rows; a row's dialect is "binary", "resp" or "?" (nothing sent yet) */
void pc_obs_clients_tree(struct pc_tw *w, unsigned int now_ticks, int cap);
void pc_obs_client_list(struct pc_jw *w, unsigned int now_ticks);

/* ---- the slow log --------------------------------------------------- */
/* record a finished RESP command; logs only past the configured
 * threshold.  argv is truncated for storage, never for judgement. */
void pc_obs_slow(char *const *argv, const size_t *argl, int nargs,
		unsigned long long usec, void *connrow);
void pc_obs_slowlog_get(struct pc_jw *w, int want);

/* ---- commands this node does not implement --------------------------- */
struct clunk_table;
/* a sighting names the command (and subcommand), never an argument; @dialect is CLUNK_RESP or
 * CLUNK_BIN.  The first sighting of each name logs one NOTICE per process. */
void pc_obs_unknown(int dialect, const char *cmd, size_t clen,
		const char *sub, size_t slen, void *connrow);
/* an unknown command before AUTH: counted, never named */
void pc_obs_unknown_preauth(int dialect);
/* this node's table, the workers folded */
void pc_obs_unknown_local(struct clunk_table *out);
/* moves on every sighting and on a reset - the peer plane's cue that
 * the table it last sent is out of date */
unsigned long long pc_obs_unknown_gen(void);
/* per-dialect totals since the last reset, for /metrics - indexed by
 * the clunk.h dialect (CLUNK_RESP, CLUNK_BIN) */
void pc_obs_unknown_totals(unsigned long long tot[CLUNK_DIALECTS],
		unsigned long long *preauth);
void pc_obs_unknown_reset(void);
/* the totals past every reset - for /metrics */
void pc_obs_unknown_totals_raw(unsigned long long tot[CLUNK_DIALECTS],
		unsigned long long *preauth);
long long pc_obs_slowlog_len(void);
void pc_obs_slowlog_reset(void);

/* the query log: a line per request (or one in N per worker) through LM_INFO, off by default
 * and free when off; capped at QLOG_MAX_PER_SEC lines a second across the daemon */
void pc_obs_qlog_config(int every, int keys);
int  pc_obs_qlog_on(void);
void pc_obs_qlog(const char *dialect, void *row, const char *verb,
		size_t vlen, const char *col, size_t clen, const char *key,
		size_t klen, const char *outcome, unsigned long long usec);

#endif /* PC_OBS_H */
