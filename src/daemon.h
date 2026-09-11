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
int pc_http_peer_allowed(const struct sockaddr_in *sa,
		unsigned int len);
/* S37: the HTTP door's shared token, or NULL when none is set */
const char *pc_http_token(void);
/* S89: the configured listeners, for stats and the page - what the
 * daemon runs with, never re-parsed */
int pc_listener_count(void);
const struct pc_listener *pc_listener_at(int i);
int pc_listener_plaintext(const struct pc_listener *l);
int pc_listener_allow(const struct pc_listener *l);
const char *pc_resp_door(void);         /* "addr:port" or NULL */
int pc_resp_peer_allowed(const struct sockaddr_in *sa, unsigned int len);
const char *pc_resp_collections(void);

/* RESP counters (proto.c).  Written by every worker, so they are
 * 32-bit relaxed atomics - see the note at their definition. */
extern unsigned int pc_resp_conns, pc_resp_rejected, pc_resp_authfail;
extern unsigned int pc_resp_slots_hits, pc_resp_slots_builds;   /* S75 */
/* S76: the native door, which carries production traffic and had no
 * counters at all.  A connection settles into ONE dialect on its first
 * byte (binary 0x9E, JSON-RPC text, or RESP spoken to the native port);
 * requests are counted per dialect because the dialects cost
 * differently (0.72 us binary against 1.43 us RESP, measured) and a
 * single native total would hide which one a client is paying for. */
extern unsigned int pc_resp_reqs, pc_nat_conns, pc_nat_bin_conns,
	pc_nat_text_conns, pc_nat_resp_conns, pc_nat_bin_reqs,
	pc_nat_text_reqs, pc_nat_resp_reqs;
#define PC_RESP_BUMP(c) __atomic_add_fetch(&(c), 1, __ATOMIC_RELAXED)
#define PC_RESP_READ(c) __atomic_load_n(&(c), __ATOMIC_RELAXED)
#define PC_RESP_DEC(c)  __atomic_sub_fetch(&(c), 1, __ATOMIC_RELAXED)

/* S123: connections open RIGHT NOW, per door and dialect - up when a
 * connection's first message settles its dialect (at accept on the
 * pinned RESP door), down when it is destroyed, by the memo it kept.
 * Gauges: a counter reset never touches them. */
extern unsigned int pc_resp_open, pc_nat_bin_open, pc_nat_text_open,
	pc_nat_resp_open;

/* S123: the door counters above are never rewound - every worker adds
 * to them relaxed, and a store of zero would race those adds.  A reset
 * records where to count from and the stats block reports the
 * difference, the shape the core gives its own table counters
 * (pcache_ht_stats_reset).  /metrics keeps the raw totals: Prometheus
 * counters are meant to be monotonic. */
struct pc_door_base {
	unsigned int resp_conns, resp_rejected, resp_authfail, resp_reqs,
		resp_slots_hits, resp_slots_builds, nat_conns, nat_bin_conns,
		nat_text_conns, nat_resp_conns, nat_bin_reqs, nat_text_reqs,
		nat_resp_reqs;
};
extern struct pc_door_base pc_door_base;
void pc_doors_reset(void);
#define PC_DOOR_SINCE(name) ((long long)PC_RESP_READ(pc_##name) - \
	(long long)PC_RESP_READ(pc_door_base.name))


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
/* S107: hand @payload (an id-less JSON object) to every native client
 * of every worker, from any thread: queued to each worker and sent by
 * it on its own connections.  Best effort - a worker whose control
 * queue is full is skipped; the member list stays the truth. */
void pc_clients_notify(const char *payload, size_t n);

#endif /* PC_DAEMON_H */
