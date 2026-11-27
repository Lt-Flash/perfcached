/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* config.h - perfcached configuration: an INI file, strictly validated (an unknown section or key
 * is a startup error); every knob is policy - discoverable facts are probed, never configured. */
#ifndef PC_CONFIG_H
#define PC_CONFIG_H

#include "pc_attr.h"

#include "rdb_rules.h"
#include "mail.h"

#define PC_MAX_COLLECTIONS   64
/* sizing for a collection the cluster declares but this node has no
 * [collection X] block for: 64k buckets, the middle of the road */
#define PC_DEFAULT_BUCKETS_LOG2 16
#define PC_MAX_RESP_ALLOW 16
/* [cluster] identity_source */
#define PC_IDSRC_AUTO 0      /* dmi -> mac -> file */
#define PC_IDSRC_DMI  1
#define PC_IDSRC_MAC  2
#define PC_IDSRC_ENV  3      /* $PERFCACHED_PLATFORM_KEY (k8s metadata.uid) */
#define PC_IDSRC_FILE 4      /* the identity file only */
#define PC_MAX_LISTEN        16
/* [daemon] keys_turn_us default */
#define PC_KEYS_TURN_US_DEFAULT 250
#define PC_MAX_CLIENT_SECRETS 8

enum pc_pull_mode { PC_MODE_STORE = 0, PC_MODE_PROXY, PC_MODE_SHARD,
	PC_MODE_SPREAD };

/* spread holds each record on K holders chosen by shard's rendezvous placement; 1 is refused
 * rather than aliased to shard, and the ceiling is the map's own member ceiling */
#define PC_REPLICAS_MIN 2
#define PC_REPLICAS_MAX 257
enum pc_listen_type { PC_LISTEN_TCP = 0, PC_LISTEN_UNIX };
enum pc_plaintext { PC_PLAINTEXT_NEVER = 0, PC_PLAINTEXT_LOOPBACK };
enum pc_wal_probe_mode { PC_WPROBE_AUTO = 0, PC_WPROBE_ALWAYS, PC_WPROBE_NO };

/* a collection's autoscaling mode ([daemon] / [collection x]
 * autoscale, and the autoscale verb) - see store.h */
#define PC_AUTOSCALE_AUTO 0
#define PC_AUTOSCALE_WARN 1
#define PC_AUTOSCALE_OFF  2
/* as AUTO, but never shrunk below the size it was given -
 * [collection x] buckets_log2, a create's size or an explicit resize */
#define PC_AUTOSCALE_FLOOR 3
/* JSON documents vs the string commands: STRICT is Redis 8 (a string command on a document is
 * WRONGTYPE, MGET nil, JSON.* on a string refused); LOOSE serves a document as its text */
#define PC_TYPES_LOOSE   0
#define PC_TYPES_STRICT  1
int pc_types_parse(const char *s);          /* the mode, or -1 */
const char *pc_types_name(int m);
int pc_autoscale_parse(const char *s);      /* the mode, or -1 */
const char *pc_autoscale_name(int m);

struct pc_collection {
	char *name;
	int   buckets_log2;         /* the exponent convention: 16 = 65536 */
	unsigned int log_events;    /* PC_EV_* mask, if has_log_events */
	int   has_log_events;
	unsigned int notify_events; /* PC_EV_* mask published as keyspace
	                             * notifications, if has_notify_events */
	int   has_notify_events;
	enum pc_pull_mode mode;
	int has_mode;                  /* an explicit per-collection mode = (only
	                                * a hint under a cluster-authoritative config) */
	int eager;                     /* store mode: background push of
	                                * every record to all peers */
	int   pull;                 /* opt-in to cluster pull-on-miss */
	int   autoscale;            /* PC_AUTOSCALE_*, if has_autoscale */
	int   has_autoscale;
	int   redis_types;          /* PC_TYPES_*, if has_redis_types */
	int   has_redis_types;
	int   nondurable;           /* durable = no: a cache, off the WAL and
	                             * the snapshot (0, the default, is durable) */
	/* read-through from a RESP upstream (src/upstream.c); NULL upstream = off */
	char *upstream;             /* redis://[user@]host:port[/db], up to
	                             * PC_UP_MAX_SRV of them, comma-separated */
	char *up_prefixes;          /* the allow-list, glob patterns,
	                             * comma-separated - required */
	int   up_negative_ms;       /* absent upstream: remembered this long */
	int   up_rewarm_max;        /* names refetched after a tracking gap
	                             * (0 = off) */
	int   up_echo_ms;           /* a write's own invalidation inside
	                             * this window refetches, 0 = drop */
	int   up_discover;          /* PC_UP_DISC_* - the servers a master names */
	int   up_timeout_ms;        /* one fetch, connect included */
	int   up_conns;             /* connections (threads) per node */
	int   up_tracking;          /* CLIENT TRACKING invalidations */
	int   up_on_error;          /* PC_UP_ON_ERROR_* for a READ */
	int   up_refuse_writes;     /* upstream_writes = refuse */
	int   up_write_through;     /* upstream_writes = through: a client write
	                             * goes to the upstream's master first, then here */
	int   up_unknown;           /* PC_UP_UNKNOWN_* (through only) */
	int   has_up_unknown;       /* said in the config */
	int   up_pass_max_kb;       /* the largest reply relayed */
	int   up_write_queue;       /* writes queued for the master at most */
	int   has_up_write_queue;
	int   up_drain_rate;        /* the drain sweep's keys/s, 0 = no cap */
	int   up_drain_mb_rate;     /* both sweeps' MB/s, 0 = no cap */
	int   up_role;              /* PC_UP_ROLE_* - what an upstream
	                             * must be (ROLE) before a fetch goes to it */
	int   up_master_fallback;   /* a master only while no replica is */
	int   up_max_lag_ms;        /* a replica further behind is
	                             * skipped; 0 = not checked */
	int   up_mode;              /* PC_UP_MODE_* */
	int   has_up_mode, has_up_preempt;   /* said in the config */
	char *up_primary;           /* failover - "host:port" active
	                             * first; NULL = the first listed */
	int   up_preempt;           /* a returning primary takes over */
	int   up_preempt_hold_s;    /* ...once healthy this long */
	int   up_allow_pc;          /* upstream_allow_perfcached - a
	                             * perfcached as the upstream (one hop) */
	char *up_password;          /* upstream_password: this collection's
	                             * upstreams' AUTH; NULL = none; never logged */
	char *up_wuser;             /* upstream_write_user - the writer's
	                             * ACL user; NULL = the upstream URL's */
	char *up_wpassword;         /* upstream_write_password - the writer's AUTH;
	                             * NULL = upstream_password; never logged */
	char *up_smaster;           /* upstream_sentinel_master - the one of several */
	char *up_spassword;         /* upstream_sentinel_password - its sentinels' AUTH;
	                             * NULL = none.  Never logged or dumped */
	int up_tls;                 /* the upstreams are rediss:// */
	char *up_tls_ca, *up_tls_name, *up_tls_cert, *up_tls_key;   /* upstream_tls_* */
	int up_tls_verify, up_tls_verify_set;
};

#define PC_UP_ON_ERROR_ERROR 0      /* the command answers an error */
#define PC_UP_ON_ERROR_MISS  1      /* a read answers as a miss */
#define PC_UP_MAX_SRV        32     /* upstreams of one collection */
#define PC_UP_ROLE_ANY       0      /* whatever answers */
#define PC_UP_ROLE_REPLICA   1      /* a replica, checked by ROLE */
#define PC_UP_DISC_AUTO      0      /* used when the configured address is one of them */
#define PC_UP_DISC_YES       1      /* used, dialled directly */
#define PC_UP_DISC_NO        2      /* neither used nor shown */
#define PC_UP_MODE_BALANCE   0      /* the fewest in flight */
#define PC_UP_MODE_FAILOVER  1      /* one active, the rest standby */
#define PC_UP_UNKNOWN_REFUSE 0      /* a command perfcached does not
                                     * know answers "unknown command" */
#define PC_UP_UNKNOWN_PASS   1      /* ...goes to the upstream, as
                                     * Redis's COMMAND INFO classes it */

struct pc_listener {
	enum pc_listen_type type;
	char *addr;                 /* tcp: host part verbatim; unix: path */
	int   port;                 /* tcp only */
	int   loopback;             /* computed: eligible for plaintext */
	int   encrypt;              /* -1 = [listen] encrypt, 0 = the
	                             * plaintext hello too, 1 = Noise only */
	int   http;                 /* the HTTP door: plaintext GET-only, no
	                             * dialect; guarded by http_allow */
	int   resp;                 /* RESP-only: plain RESP2 and nothing else, so
	                             * the native methods (admin too) are unreachable */
	int   tls;                  /* a RESP or HTTP door spoken over TLS */
};

/* one CIDR of an allow-list (resp_allow, http_allow), either family: an IPv4 client of a
 * dual-stack listener (::ffff:a.b.c.d) matches as its IPv4 address (pc_addr_in_prefix) */
struct pc_cidr {
	int family;                 /* AF_INET or AF_INET6 */
	int bits;                   /* the prefix length */
	unsigned char net[16];      /* network order, masked; IPv4: the first 4 */
};

/* [daemon] io - the workers' I/O backend */
enum { PC_IO_EPOLL = 0, PC_IO_URING = 1 };

#define PC_UPG_MAX_KEYS 4               /* [upgrade] trusted_keys */

#define PC_INSTALL_AUTO    0
#define PC_INSTALL_PACKAGE 1
#define PC_INSTALL_PREFIX  2
#define PC_INSTALL_LOCAL   3

struct pc_config {
	/* [daemon] */
	int workers;                /* default: online CPUs, clamped */
	int io_backend;             /* PC_IO_URING (the default) or PC_IO_EPOLL;
	                             * io_uring falls back where the host refuses it */
	int io_explicit;            /* io = was in the file */
	int output_stall_s;         /* an output-paused connection with no
	                             * progress this long is closed */
	int busy_poll_us;           /* per-worker busy poll, 0 = off */
	char link_monitor[64];      /* "" = auto, "off", or a device */
	int link_speed_mbps;        /* for a device that reports none */
	int busy_poll_prefer;       /* prefer it over the device's IRQs */
	int address_wait_s;         /* how long a start waits for a
	                             * configured address to appear; 0 = never */
	int keys_turn_us;           /* a KEYS walk's time budget per
	                             * worker turn; 0 = the 1,024-bucket floor */
	int autoscale;              /* [daemon] autoscale, the default */
	int redis_types;            /* [daemon] redis_types, the default */
	int grow_at_pct;            /* split once a table passes this
	                             * percent of its slot capacity; default 75 */
	int shrink_at_pct;          /* resize down below this percent of slot
	                             * capacity, past the cool-off; 0 = grow_at_pct / 4 */
	int grow_floor_pct;         /* how far a table may be widened when its overflow
	                             * leg blames the load factor; 0 = never, default 50 */
	int leg_stuck_pct;          /* the leg is "a tax" once a drain pass leaves this
	                             * percent of the entries unable to return; default 1 */
	int prefetch_min_keys;      /* RESP lookahead prefetch only on a collection of
	                             * at least this many keys (0 = always): below, it fits cache */
	int shrink_cooloff_s;       /* no shrink within this of a split,
	                             * a resize or the start; default 60 */
	int pubsub_queue_mb;       /* pub/sub: MB queued for one worker at most;
	                            * past it new messages are dropped and counted.  0 = no cap */
	int max_clients;           /* client connections across the data doors; 0 =
	                             * from the descriptor limit minus the reserve, and memory */
	/* new clients are refused while less than this floor of the memory limit is free (memguard.h):
	 * pct > 0 that share, 128 MB..4 GB; mb > 0 exactly that; both 0 = off.  Default 15%. */
	int memory_floor_pct;
	int memory_floor_mb;
	int stats_reset_days;       /* reset /stats every N days (1-7), 0 = never */
	int stats_reset_at_s;       /* ... at this many seconds past local midnight */
	int slowlog_usec;           /* log commands slower than this; 0 logs
	                             * everything, default 10000 (10ms, redis's own) */
	int log_level;              /* L_* from compat/dprint.h */
	/* per-key event logging: the fleet default, a collection may override with its own
	 * log_events.  PC_EV_* bits (events.h). */
	unsigned int log_events;
	unsigned int notify_events; /* the [daemon] default mask, 0 = off */
	int log_events_rate;        /* lines per second per collection per kind */
	int log_events_hash;        /* log a hash of the key, not the key */
	/* [memory] */
	int arena_mb;
	int arena_cap_mb;           /* 0 = fixed at arena_mb */
	int backing_heap;           /* 1 = no arena (testing); default own */
	int reclaim_keep, reclaim_quiet_s, reclaim_cooloff_s, reclaim_giveback;
	int reclaim_floor_mb;          /* never give back below this */
	/* pinning is always attempted and degrades to a warning; 1 = a node that must be resident
	 * is refused a start it cannot honour */
	int pin_require;
	int pin_off;                   /* pin = no - never locked, so idle groups go back */
	/* the plain-4K tier: no MAP_HUGETLB or MADV_HUGEPAGE, THP off (PR_SET_THP_DISABLE), so a 2 MB
	 * group costs 512 cheap faults instead of one that may compact */
	int huge_pages_off;
	int shrink_step_mb;            /* give-back per tick; 0 = sized to the arena */
	/* [secrets] */
	char *client_secret[PC_MAX_CLIENT_SECRETS];
	int   n_client_secrets;
	/* the privilege secret, distinct from `client` (which every application link presents) so DDL
	 * is not open to all.  Fail-closed: none set = `enable` always fails, whatever allow_create is. */
	char *enable_secret[PC_MAX_CLIENT_SECRETS];
	int   n_enable_secrets;
	char *cluster_secret;
	char *cluster_accept_secret;   /* also opened with, never sealed with */
	/* [listen] */
	struct pc_listener listen[PC_MAX_LISTEN];
	int   n_listen;
	/* RESP listeners: who may connect, and which collections they see */
	struct pc_cidr resp_allow[PC_MAX_RESP_ALLOW];
	int   n_resp_allow;
	struct pc_cidr http_allow[PC_MAX_RESP_ALLOW];
	int   n_http_allow;
	char *http_token;           /* [secrets] http; NULL = none */
	int   pubsub_udp;           /* native doors may push over UDP */
	int   keepalive_s;          /* TCP keepalive on accepted data-door
	                             * sockets; 0 = off (redis: tcp-keepalive) */
	int   http_rate;            /* HTTP requests a second per client
	                             * address (bucket of twice that); 0 = off */
	int   enable_lockout_after; /* wrong enable secrets from one address
	                             * before its enable is refused; 0 = off */
	int   enable_lockout_s;     /* the window, and the refusal's length */
	int   install_mode;         /* PC_INSTALL_AUTO (detect), _PACKAGE,
	                             * _PREFIX, _LOCAL - what upgrade_stage may do */
	int   http_cache_ms;        /* /stats and /metrics built at most
	                             * once in this long; 0 = every request */
	int   http_timeout_s;       /* drop an HTTP connection whose request head has
	                             * not arrived in this long; 0 disables, default 5 */
	char *resp_password;        /* optional Redis AUTH; NULL = none */
	/* [mail] - this node's alert mailer.  The password is
	 * [secrets] mail (mail.pass), never a [mail] key. */
	int mail_on;                /* a [mail] section is present */
	struct pc_mail_conf mail;
	/* [alerts] - the high-water alarms, each a warning at *_warn
	 * and critical at *_crit, percent (defaults 65 / 75) */
	struct pc_alerts_conf {
		int cpu_warn, cpu_crit;     /* host CPU busy */
		int io_warn, io_crit;       /* the state device busy (%util) */
		int mem_warn, mem_crit;     /* host memory used */
		int arena_warn, arena_crit; /* arena held of its ceiling */
		int epoll_busy;             /* an epoll worker's busy share */
		int link_warn, link_crit;   /* the link's busier direction */
	} alerts;
	/* [upgrade] - signers a build to install may carry beside the release key built into
	 * upgrade.c (public halves, 32 bytes each) */
	struct pc_upgrade_conf {
		unsigned char keys[PC_UPG_MAX_KEYS][32];
		int n_keys;
		int allow_unsigned;         /* yes: an unsigned artifact stages */
		int stage_mb;               /* the staged artifacts' space */
		char results_dir[256];      /* where the helper answers */
	} upgrade;
	char *resp_collections;     /* optional "a,b" restriction; NULL = all */
	enum pc_plaintext plaintext;
	int listen_encrypt;         /* [listen] encrypt, 0 = no */
	char *tls_cert, *tls_key;   /* PEM files for the TLS doors (a chain allowed) */
	int tls_min13;              /* tls_min = 1.3 */
	int auth_needs_tls;         /* AUTH / the token refused in the clear off loopback */
	int   resp_redirect;          /* 0 forward (default), 1 moved */
	/* [wal] */
	char *wal_dir;                 /* NULL = no WAL */
	/* where identity and the mastership term live.  NULL = the WAL dir
	 * if there is one, else ephemeral. */
	char *state_dir;
	int   allow_create;         /* a client may create and drop
	                             * collections at runtime */
	/* the query log: 0 = off (one predictable branch), 1 = every request, N > 1 = one in N per
	 * worker.  Keys: 0 omitted, 1 a short hash (correlatable, not readable), 2 in full. */
	int query_log;
	int query_log_keys;
	enum pc_wal_probe_mode wal_probe;
	int   wal_probe_secs;          /* sustained phase for probe=always */
	int   wal_fsync;               /* 0 everysec, 1 always, 2 no, 3 sync */
	int   wal_segment_mb;          /* 0 = follow the probe's policy */
	int   wal_segments;            /* the floor (segments_min) */
	int   wal_segments_max;        /* the cap; 0 = the floor */
	int   wal_unknown_skip;        /* [wal] unknown_records = skip */
	int   wal_ring_kb;
	int   wal_ring_kb_set;         /* set in the file */
	int   wal_on_full;             /* 0 wait, 1 heal */
	struct pc_rdb_rule rdb_rules[PC_RDB_MAX_RULES];  /* [wal] save = */
	int   n_rdb_rules;             /* -1 = default set, 0 = manual only */
	int   rdb_mb_s;                /* 0 = auto from the probe */
	/* [cluster] */
	/* membership is automatic: only the multicast group is required;
	 * node ids are assigned by the elected master */
	int   cl_enabled;              /* [cluster] section present */
	char *cl_mcast;                /* multicast group ip */
	int   cl_unicast;              /* discovery = unicast */
	char *cl_seeds;                /* seeds = host[:port], ... */
	int   cl_max_dgram;            /* frames over this go by TCP lane */
	int   cl_lane_port;            /* 0 = port + 2 */
	int   cl_lane_retry_s;         /* probe a forced lane's UDP; 0 = 30 */
	int   cl_expect;               /* blank nodes before founding */
	int   cl_id_source;            /* PC_IDSRC_* */
	int   cl_port;                 /* group port; unicast binds it too */
	char *cl_advertise;            /* own unicast ip; NULL = auto-detect */
	char *cl_site_salt;            /* [cluster] site_salt: mixed into a platform-derived identity */
	int   cl_pull_timeout_ms, cl_negative_ms, cl_tombstone_ms;
	int   cl_sweep_digests;   /* the digest-gated sweep, default on */
	int   cl_reconcile_fast;  /* the reconcile's fast path, default on */
	int   cl_reconcile_rate_max;   /* walk probes a second at most */
	char *cl_cluster_id;      /* pin the cluster id (UUID or name) */
	int   cl_max_members;     /* refuse joins past this many members,
	                           * 0 = the build's limit (256 + this node) */
	int   cl_mcast_ttl;       /* the cluster group's multicast TTL,
	                           * default 1 (one L2 segment) */
	int   cl_tomb_retain_s, cl_tomb_retain_max;
	int   cl_resync_after_s;   /* a node away longer resyncs (0: never by time) */
	int   cl_tomb_acks;          /* [cluster] tombstone_acks */
	int   cl_data_mcast;         /* [cluster] data_multicast */
	int   cl_apply_stall_ms;            /* 0 = off */
	int   cl_apply_threads;             /* 1 = on the receiver */
	/* parked-request slots - backpressure, not a buffer: a forward that cannot park is refused.
	 * Size it from stats.cluster.pend_peak, not by guessing. */
	int   cl_max_pending;
	/* pub/sub relays on their own port (0 = the cluster port + 1), read by this many threads:
	 * 0 = on the cluster socket, -1 (auto, the default) = half the workers, at most CLWIRE_LANES */
	int   cl_ps_rx_threads;
	int   cl_ps_rx_auto;
	int   cl_ps_port;
	int   cl_ps_interested;      /* pubsub_relay = interested */
	/* [cluster] mode + collections: the cluster owns the clustered collections' mode and set */
	int   cl_has_mode;             /* [cluster] mode = ... present */
	enum pc_pull_mode cl_mode;     /* the cluster-wide mode */
	int   cl_eager;                /* mode=store: eager replication */
	int   cl_replicas;             /* mode=spread: K holders per key; 0 when
	                                * not spread, which keeps it out of the digest */
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

/* for a config reload: every key = value read, in file order, the value a BLAKE2b digest keyed
 * per process (no secret held in clear); sect and key lower-cased, sect's spaces collapsed */
#define PC_KM_MAX 2048
struct pc_km_ent {
	char sect[96];
	char key[48];
	unsigned char h[16];
};
struct pc_keymap {
	int n;
	int overflow;              /* more keys than PC_KM_MAX: no diff possible */
	struct pc_km_ent e[PC_KM_MAX];
};
/* pc_config_load, also filling @km (may be NULL) */
int pc_config_load_km(const char *path, struct pc_config *cfg,
		struct pc_keymap *km);

/* zero the raw secret strings (CWE-14): by pc_config_free and, in the daemon, as soon as
 * the cached PSKs are derived */
void pc_config_wipe_secrets(struct pc_config *cfg);

#endif /* PC_CONFIG_H */
