/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* daemon.h - the running daemon: threads, listeners, registry, shutdown.  pc_daemon_run() owns
 * the process from a validated config until shutdown and returns the exit code. */
#ifndef PC_DAEMON_H
#define PC_DAEMON_H

#include "config.h"
#include "walprobe.h"
#include "pcaddr.h"

int pc_daemon_run(struct pc_config *cfg);
/* stop as SIGTERM does, with no shutdown snapshot when rdb is held, and exit 75 so the
 * supervisor starts the node again from its files (a copy of the fleet's state that failed) */
void pc_daemon_restart_from_files(void);

/* re-run the WAL storage probe (the `probe` verb); blocks the caller ~1-2 s (longer with
 * @secs); -1 = no WAL configured or the probe failed */
int pc_wal_reprobe(int secs, struct pc_wal_policy *pol);


/* ---- RESP listener guards: no handshake, so access control is config-only ---- */
int pc_resp_password_set(void);
/* a secret that crossed the network in the clear is refused (auth_needs_tls); a
 * connection is secure when it is TLS or its peer is on loopback */
int pc_auth_needs_tls(void);
int pc_conn_secure(const void *conn);
int pc_resp_password_ok(const char *p, size_t n);
/* the privilege secret: _ok is constant time and fail-closed (no secret = never true) */
int pc_enable_secret_ok(const char *p, size_t n);
int pc_enable_configured(void);
int pc_http_peer_allowed(const pc_addr *sa,
		unsigned int len);
/* the HTTP door's shared token, or NULL when none is set */
const char *pc_http_token(void);
int pc_http_rate(void);                /* [listen] http_rate */
int pc_http_cache_ms(void);            /* [listen] http_cache_ms */
/* the configured listeners, for stats and the page - what the
 * daemon runs with, never re-parsed */
int pc_listener_count(void);
const struct pc_listener *pc_listener_at(int i);
int pc_listener_plaintext(const struct pc_listener *l);
int pc_listener_allow(const struct pc_listener *l);
const char *pc_resp_door(void);         /* "addr:port" or NULL */
int pc_resp_peer_allowed(const pc_addr *sa, unsigned int len);
const char *pc_resp_collections(void);

/* RESP counters (proto.c).  Written by every worker, so they are
 * 32-bit relaxed atomics - see the note at their definition. */
extern unsigned int pc_resp_conns, pc_resp_rejected, pc_resp_authfail;
extern unsigned int pc_resp_slots_hits, pc_resp_slots_builds;
/* the native door: a connection settles into ONE protocol on its first byte (binary 0x9E, else
 * RESP); requests are counted per protocol because the two cost differently */
extern unsigned int pc_resp_reqs, pc_nat_conns, pc_nat_bin_conns,
	pc_nat_resp_conns, pc_nat_bin_reqs, pc_nat_resp_reqs,
	pc_nat_bin_records,            /* sealed records opened */
	pc_nat_plain_conns, pc_nat_plain_refused;   /* the plaintext hello */
#define PC_RESP_BUMP(c) __atomic_add_fetch(&(c), 1, __ATOMIC_RELAXED)
#define PC_RESP_READ(c) __atomic_load_n(&(c), __ATOMIC_RELAXED)
#define PC_RESP_DEC(c)  __atomic_sub_fetch(&(c), 1, __ATOMIC_RELAXED)

/* connections open now, per door and protocol: up when the first message settles the protocol,
 * down when the connection is destroyed.  Gauges: a counter reset never touches them. */
extern unsigned int pc_resp_open, pc_nat_bin_open, pc_nat_resp_open;

/* the door totals are never rewound (a store of zero would race the relaxed adds) and wrap at
 * 2^32: each reading folds the 32-bit distance into a 64-bit total, and a reset records that */
enum pc_door_ctr {
	PC_DW_resp_conns, PC_DW_resp_rejected, PC_DW_resp_authfail,
	PC_DW_resp_reqs, PC_DW_resp_slots_hits, PC_DW_resp_slots_builds,
	PC_DW_nat_conns, PC_DW_nat_bin_conns, PC_DW_nat_resp_conns,
	PC_DW_nat_bin_reqs, PC_DW_nat_resp_reqs, PC_DW_nat_bin_records,
	PC_DW_nat_plain_conns, PC_DW_nat_plain_refused, PC_DW_N
};
void pc_doors_reset(void);
void pc_doors_widen(void);                     /* the maintenance tick */
unsigned long long pc_door_since(enum pc_door_ctr c);
/* tests only - start every door counter at @v (just below the wrap) */
void pc_doors_test_start(unsigned int v);
#define PC_DOOR_SINCE(name) ((long long)pc_door_since(PC_DW_##name))


/* this worker's index, or -1 off a worker thread: an accessor over struct pc_thread, so our
 * files need not reach for the vendored core's process_no */
int pc_worker_id(void);
/* hand @payload (one encoded tree item, the membership map as a NOTIFY frame) to every native
 * client of every worker, from any thread; best effort - a worker with a full queue is skipped */
void pc_clients_notify(const char *payload, size_t n);

#endif /* PC_DAEMON_H */
