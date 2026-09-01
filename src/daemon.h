/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * daemon.h — the running daemon (task S6): threading frame, listeners,
 * registry, shutdown.  pc_daemon_run() owns the process from a validated
 * config until shutdown; returns the process exit code.
 */
#ifndef PC_DAEMON_H
#define PC_DAEMON_H

#include "config.h"
#include "walprobe.h"

int pc_daemon_run(struct pc_config *cfg);

/* re-run the WAL storage probe (the `probe` admin verb).  Blocks the
 * caller for the measurement (~1-2s, longer with @secs); -1 = no WAL
 * configured or the probe failed. */
int pc_wal_reprobe(int secs, struct pc_wal_policy *pol);


/* ---- RESP listener guards (task S33) ------------------------------------
 * A RESP listener has no handshake, so access control is config-only. */
struct sockaddr_in;
int pc_resp_password_set(void);
int pc_resp_password_ok(const char *p, size_t n);
int pc_resp_peer_allowed(const struct sockaddr_in *sa, unsigned int len);
const char *pc_resp_collections(void);

/* RESP counters (proto.c).  Written by every worker, so they are
 * 32-bit relaxed atomics - see the note at their definition. */
extern unsigned int pc_resp_conns, pc_resp_rejected, pc_resp_authfail;
#define PC_RESP_BUMP(c) __atomic_add_fetch(&(c), 1, __ATOMIC_RELAXED)
#define PC_RESP_READ(c) __atomic_load_n(&(c), __ATOMIC_RELAXED)


/* This worker's index, or -1 off a worker thread.
 *
 * The daemon already keeps a per-worker context (struct pc_thread, in a
 * thread-local set by slot_attach), so this is an accessor over state we
 * own rather than a second identity.  It replaces reaching for the
 * OpenSIPS shim's `process_no` from our own files: that global still
 * exists and is still set, because src/core/ is VENDORED from
 * cachedb_perf and compiles against those names (tools/sync-core.sh),
 * but nothing outside src/core/ needs to know that. */
int pc_worker_id(void);

#endif /* PC_DAEMON_H */
