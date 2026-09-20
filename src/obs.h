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

/* S159: the command rows merged by name across the workers, with the
 * log2 latency histogram behind each: hist[k] holds the calls that
 * took at most 2^k us (k < 16: 1 us .. 32.768 ms), hist[16] the rest.
 * RESP commands sit under their bare Redis name, the native doors as
 * json.<verb> and bin.<verb> - the two dialects cost differently and a
 * merged row would hide it. */
#define PC_OBS_NAME 24
#define PC_OBS_HIST 17
struct pc_obs_cmdsum {
	char name[PC_OBS_NAME];
	unsigned int nlen;
	unsigned long long calls, usec;
	unsigned long long hist[PC_OBS_HIST];
};
int  pc_obs_cmd_merge(struct pc_obs_cmdsum *out, int cap);
void pc_obs_latencystats(struct pc_jw *w);           /* INFO latencystats */
void pc_obs_commands_json(struct pc_jw *w);          /* /stats "commands" */
void pc_obs_slowlog_json(struct pc_jw *w, int want); /* /stats "slowlog" */
int  pc_obs_slow_would(unsigned long long usec);     /* slow log wants it? */
/* the row for a name on THIS worker, resolved once and kept in a
 * thread-local by the caller: a native door pays the lowercase-and-hash
 * of pc_obs_cmd() per request otherwise, which measured 3% of a 1 us
 * path.  NULL when the row is full (the name is dropped, as today). */
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
const char *pc_obs_conn_addr(void *row);            /* S70 */
unsigned long long pc_obs_conn_cmds(void *row);      /* S70 */
void pc_obs_conn_touch(void *row, const char *cmd, size_t clen,
		unsigned int now_ticks);
void pc_obs_conn_name(void *row, const char *name, size_t nlen);
/* CLIENT SETINFO: @ver 0 sets lib-name, 1 lib-ver; kept to 47 bytes */
void pc_obs_conn_lib(void *row, int ver, const char *v, size_t n);
int pc_obs_conn_count(void);
/* S160: what /clients shows beyond CLIENT LIST - the door and dialect
 * (NULL = keep), the wire (encrypted 0/1, -1 = keep), the bytes staged
 * for the socket (the slow-consumer measure), the subscription count.
 * All owner-written, like the rest of the row. */
void pc_obs_conn_door(void *row, const char *door, const char *dialect,
		int encrypted);
void pc_obs_conn_pending(void *row, size_t bytes);
void pc_obs_conn_subs(void *row, int n);
/* {"total":N,"shown":K,"clients":[...]} - the data doors only (HTTP
 * excluded, as clients.open excludes it), active first, at most @cap rows */
void pc_obs_clients_json(struct pc_jw *w, unsigned int now_ticks, int cap);
void pc_obs_client_list(struct pc_jw *w, unsigned int now_ticks);

/* ---- the slow log --------------------------------------------------- */
/* record a finished RESP command; logs only past the configured
 * threshold.  argv is truncated for storage, never for judgement. */
void pc_obs_slow(char *const *argv, const size_t *argl, int nargs,
		unsigned long long usec, void *connrow);
void pc_obs_slowlog_get(struct pc_jw *w, int want);

/* ---- S165: commands this node does not implement --------------------
 * A sighting names the command (and the subcommand, when the refusal is
 * a subcommand's) and never an argument; @dialect is CLUNK_RESP / _JSON
 * / _BIN (clunk.h).  Per worker like the slow log: owner-written, folded
 * racily at read.  The first sighting of each distinct name logs ONE
 * NOTICE per process, the client's address and name with it. */
struct clunk_table;
void pc_obs_unknown(int dialect, const char *cmd, size_t clen,
		const char *sub, size_t slen, void *connrow);
/* an unknown command before AUTH: counted, never named */
void pc_obs_unknown_preauth(int dialect);
/* this node's table, the workers folded */
void pc_obs_unknown_local(struct clunk_table *out);
/* moves on every sighting and on a reset - the peer plane's cue that
 * the table it last sent is out of date */
unsigned long long pc_obs_unknown_gen(void);
/* per-dialect totals since the last reset, for /metrics */
void pc_obs_unknown_totals(unsigned long long tot[3],
		unsigned long long *preauth);
void pc_obs_unknown_reset(void);
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
		size_t klen, const char *outcome, unsigned long long usec);

#endif /* PC_OBS_H */
