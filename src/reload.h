/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * reload.h - a configuration reload (DESIGN 12nw): SIGHUP, `systemctl
 * reload` (ExecReload runs `perfcached -f <conf> -H $MAINPID`) or the
 * privileged `reload` verb.  The file is read again and compared key by
 * key with the one the daemon runs; when every key that changed is one a
 * running daemon can take - the doors' allow-lists, the secrets, the log
 * level, the slow-log threshold - they are applied, and when ANY other key
 * changed (the cluster mode, a listener, a collection, the memory...) the
 * reload is refused whole, naming those keys, and nothing changes.
 */
#ifndef PC_RELOAD_H
#define PC_RELOAD_H

#include <stddef.h>
#include "config.h"

/* what a reload changed, for pc_live_reload (daemon.c) */
#define PC_RL_RESP_ALLOW    0x001u
#define PC_RL_HTTP_ALLOW    0x002u
#define PC_RL_HTTP_TOKEN    0x004u
#define PC_RL_RESP_PASSWORD 0x008u
#define PC_RL_ENABLE        0x010u
#define PC_RL_CLIENT        0x020u
#define PC_RL_LOGLEVEL      0x040u
#define PC_RL_SLOWLOG       0x080u

/* the daemon, before the threads: the file it runs and what was read from
 * it (taken over; freed at exit), and where the outcome file goes
 * ($RUNTIME_DIRECTORY, else state_dir, else none) */
void pc_reload_init(const char *path, struct pc_keymap *boot,
		const char *state_dir);
/* async-signal-safe: SIGHUP's handler */
void pc_reload_ask(void);
/* the maintenance thread, once a second: run an asked reload */
void pc_reload_tick(void);
/* one reload, now (the verb, and the tick).  0 applied or nothing to do,
 * 1 refused or failed; @msg says which and why. */
int pc_reload_run(char *msg, size_t ml);
/* stats: reloads applied, refused, the last one's time and words */
void pc_reload_stats(unsigned long long *ok, unsigned long long *refused,
		long long *last_unix, const char **last_msg);

/* -H <pid>: the ExecReload side.  Checks the file, signals the daemon and
 * waits for its outcome; prints it; 0 applied or nothing to do, 1 not. */
int pc_reload_client(const char *path, long pid);

/* daemon.c */
int pc_live_reload(const struct pc_config *nc, unsigned int what,
		char *msg, size_t ml);
void pc_live_retire_tick(void);

#endif /* PC_RELOAD_H */
