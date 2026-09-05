/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * config.h — perfcached configuration (task S5).
 *
 * INI-style file, sections [daemon] [memory] [secrets] [listen] and one
 * [collection <name>] per collection.  Keys are validated strictly:
 * unknown sections or keys are startup errors (typo protection), and
 * every knob is POLICY - discoverable facts are probed, never configured
 * (DESIGN.md; feedback-configure-policy-not-facts).
 *
 * Hard rules enforced by pc_config_validate():
 * - the cluster secret MUST differ from every client secret (the
 *   two-principal separation is structural, not advisory);
 * - plaintext is only ever possible on loopback/UNIX listeners, and only
 *   when [listen] plaintext = loopback; a non-loopback TCP listener is
 *   Noise-encrypted no matter what the config says;
 * - collection mode "proxy" parses but is refused until M5 ships it.
 */
#ifndef PC_CONFIG_H
#define PC_CONFIG_H

#include "pc_attr.h"

#include "rdb_rules.h"

#define PC_MAX_COLLECTIONS   64
/* sizing for a collection the cluster declares but this node has no
 * [collection X] block for: 64k buckets, the middle of the road */
#define PC_DEFAULT_BUCKETS_LOG2 16
#define PC_MAX_RESP_ALLOW 16
#define PC_MAX_LISTEN        16
#define PC_MAX_CLIENT_SECRETS 8

enum pc_pull_mode { PC_MODE_STORE = 0, PC_MODE_PROXY, PC_MODE_SHARD };
enum pc_listen_type { PC_LISTEN_TCP = 0, PC_LISTEN_UNIX };
enum pc_plaintext { PC_PLAINTEXT_NEVER = 0, PC_PLAINTEXT_LOOPBACK };
enum pc_wal_probe_mode { PC_WPROBE_AUTO = 0, PC_WPROBE_ALWAYS, PC_WPROBE_NO };

struct pc_collection {
	char *name;
	int   buckets_log2;         /* the exponent convention: 16 = 65536 */
	enum pc_pull_mode mode;
	int has_mode;                  /* an explicit per-collection mode =
	                                * (S30: only a HINT under a
	                                * cluster-authoritative config, and
	                                * it must match) */
	int eager;                     /* store mode: background push of
	                                * every record to all peers */
	int   pull;                 /* opt-in to cluster pull-on-miss */
};

struct pc_listener {
	enum pc_listen_type type;
	char *addr;                 /* tcp: host part verbatim; unix: path */
	int   port;                 /* tcp only */
	int   loopback;             /* computed: eligible for plaintext */
	int   http;                 /* S46/S37: the HTTP door -
	                             * plaintext GET-only, no dialect at
	                             * all; guarded by http_allow the same
	                             * way resp is by resp_allow */
	int   resp;                 /* RESP-ONLY listener (task S33): plain
	                             * RESP2 whatever `plaintext` says, and
	                             * NOTHING else - no JSON-RPC, no binary
	                             * frames, so the native verb surface
	                             * (admin included) is unreachable from
	                             * it.  Guarded by resp_allow, and by
	                             * the AUTH password when set. */
};

/* one CIDR of the RESP allow-list */
struct pc_cidr {
	unsigned int net;           /* network order, masked */
	unsigned int mask;          /* network order */
};

struct pc_config {
	/* [daemon] */
	int workers;                /* default: online CPUs, clamped */
	int slowlog_usec;           /* S53: log commands slower than this;
	                             * 0 logs everything, default 10000
	                             * (10ms, redis's own default) */
	int log_level;              /* L_* from compat/dprint.h */
	/* [memory] */
	int arena_mb;
	int arena_cap_mb;           /* 0 = fixed at arena_mb */
	int backing_heap;           /* 1 = no arena (testing); default own */
	int reclaim_keep, reclaim_quiet_s, reclaim_cooloff_s, reclaim_giveback;
	int reclaim_floor_mb;          /* S47: never give back below this */
	/* [secrets] */
	char *client_secret[PC_MAX_CLIENT_SECRETS];
	int   n_client_secrets;
	char *cluster_secret;
	/* [listen] */
	struct pc_listener listen[PC_MAX_LISTEN];
	int   n_listen;
	/* RESP listeners (S33): who may connect, who they must be, and
	 * which collections they can see.  A non-loopback RESP listener
	 * REFUSES TO START without an allow-list - it has no handshake, so
	 * the network IS the authentication until a password is set. */
	struct pc_cidr resp_allow[PC_MAX_RESP_ALLOW];
	int   n_resp_allow;
	struct pc_cidr http_allow[PC_MAX_RESP_ALLOW];
	int   n_http_allow;
	char *http_token;           /* [secrets] http; NULL = none */
	int   http_timeout_s;       /* S46: drop an HTTP connection whose
	                             * request head has not arrived in this
	                             * long; 0 disables.  Default 5 - a
	                             * scrape completes in milliseconds. */
	char *resp_password;        /* optional Redis AUTH; NULL = none */
	char *resp_collections;     /* optional "a,b" restriction; NULL = all */
	enum pc_plaintext plaintext;
	/* [wal] (S12: identity + probe at startup; the WAL itself is S13) */
	char *wal_dir;                 /* NULL = no WAL */
	/* S80: where identity and the mastership term live.  Named by the
	 * admin.  NULL = the WAL dir if there is one, else ephemeral. */
	char *state_dir;
	/* S65: the query log.  0 = off, and free - one predictable branch on
	 * the hot path; 1 = every request; N > 1 = one request in N per
	 * worker, the rate printed on every line.  Keys: 0 omitted, 1 a
	 * short hash (correlatable, not readable), 2 in full. */
	int query_log;
	int query_log_keys;
	enum pc_wal_probe_mode wal_probe;
	int   wal_probe_secs;          /* sustained phase for probe=always */
	int   wal_fsync;               /* 0 everysec, 1 always, 2 no (S13) */
	int   wal_segment_mb;          /* 0 = follow the probe's policy */
	int   wal_segments;
	int   wal_ring_kb;
	int   wal_ring_kb_set;         /* the operator pinned it */
	struct pc_rdb_rule rdb_rules[PC_RDB_MAX_RULES];  /* [wal] save = */
	int   n_rdb_rules;             /* -1 = default set, 0 = manual only */
	int   rdb_mb_s;                /* 0 = auto from the probe */
	/* [cluster] (M4) */
	/* membership is AUTOMATIC (S16 as designed): only the multicast
	 * group is required; node ids are assigned by the elected master */
	int   cl_enabled;              /* [cluster] section present */
	char *cl_mcast;                /* multicast group ip */
	int   cl_port;                 /* group port; unicast binds it too */
	char *cl_advertise;            /* own unicast ip; NULL = auto-detect */
	int   cl_pull_timeout_ms, cl_negative_ms, cl_tombstone_ms;
	/* parked-request slots.  Backpressure, not a buffer: a forward
	 * that cannot park is REFUSED, so this bounds how much state a
	 * pipelining client can make the daemon hold.  Size it from the
	 * measured stats.cluster.pend_peak, not by guessing. */
	int   cl_max_pending;
	/* S30: the cluster owns the interchange-relevant collection
	 * properties.  ONE mode for the whole cluster (per-collection
	 * modes inside a cluster are rejected by design - one membership
	 * whose members mean different things per collection cannot be
	 * reasoned about during an incident), plus the EXHAUSTIVE set of
	 * clustered collections.  Declaring `collections` switches a
	 * daemon into cluster-authoritative mode. */
	int   cl_has_mode;             /* [cluster] mode = ... present */
	enum pc_pull_mode cl_mode;     /* the cluster-wide mode */
	int   cl_eager;                /* mode=store: eager replication */
	char *cl_collections;          /* verbatim "a, b" list; NULL = the
	                                * legacy per-collection form */
	char *cl_colname[PC_MAX_COLLECTIONS];  /* parsed set */
	int   n_cl_col;
	/* collections */
	struct pc_collection col[PC_MAX_COLLECTIONS];
	int   n_col;
};

/* parse + validate; returns 0 and fills *cfg, or -1 with errors logged
 * (file:line for parse errors, named rule for validation errors) */
PC_MUST_CHECK int pc_config_load(const char *path, struct pc_config *cfg);

/* print the normalized effective config to stdout; secrets are masked */
void pc_config_dump(const struct pc_config *cfg);

void pc_config_free(struct pc_config *cfg);

/* zero the raw secret strings (S59/CWE-14) - called by pc_config_free
 * and, in the daemon, as soon as the cached PSKs are derived so the
 * raw copies do not live for the process lifetime */
void pc_config_wipe_secrets(struct pc_config *cfg);

#endif /* PC_CONFIG_H */
