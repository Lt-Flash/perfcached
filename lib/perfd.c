/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov
 * Part of libperfd - see lib/LICENSE.  This file must stay
 * free of src/core includes; tools/sync-libperfd.sh exports
 * exactly the MIT set to consumers. */
/* perfd.c - libperfd (contract in perfd.h): replies go out in request order from a FIFO of ids,
 * out-of-order arrivals parked; JSON text exists only at the API edge, never on the wire. */
#include <errno.h>
#include <stdarg.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <pthread.h>
#include <sodium.h>

/* a handshake message's ceiling - the daemon refuses a larger frame (proto.c PC_HS_MAXFRAME); not
 * PC_NOISE_MAXMSG: two 64 KB stack buffers overflowed a musl thread's 128 KB stack */
#define PERFD_HS_MAX   2048
#define PERFD_XBUF     (2 + PC_NOISE_MAXPT + PC_NOISE_TAGLEN)

#include "../src/json.h"
#include "../src/ptree.h"             /* the tree codec, both ways */
#include "../src/pc_noise.h"
#include "perfd.h"
#include "perfd_push.h"                /* the UDP push datagram */
#include "../src/pc_slot.h"           /* the ONE key->slot function */
#include "../src/pc_mix.h"            /* ...and the ONE rendezvous mix */
#include "../src/pcaddr.h"            /* the ONE address type */

#define DEF_TIMEOUT_MS 5000
#define DEF_KEEPALIVE_S   30       /* TCP keepalive idle */
#define DEF_IDLE_PING_MS  30000    /* ping a standby idle this long */
#define LOOK_QUIET_MS     100      /* look at a link this quiet before sending */
#define KEY_MAX  4096
/* no value ceiling here: the daemon is the authority on it, and a stale copy is how a client
 * starts refusing what the server accepts */

/* the binary frame constants, pinned to src/proto.h: the daemon is the authority, and the lib
 * includes no daemon-internal headers beyond json/ptree/noise */
#define BIN_MAGIC   0x9E
#define BIN_VER     0x01
#define BIN_REQ     1
#define BIN_RSP     2
#define BIN_NOTIFY  3
#define BIN_F_ERR   0x01
#define BIN_F_MOVED 0x02               /* [term u32][seq u32] trail the tree */
#define BIN_F_HINT  0x04               /* request: send a holder hint;
                                        * reply: [node u32][ttl u32] last */
#define HINT_TTL_UNKNOWN 0xFFFFFFFFu
#define HINT_SLOTS  4096               /* the holder table: */
#define HINT_WAYS   4                  /* 1024 sets of 4 */
#define BIN_HDR     16
#define BV_PING     1
#define BV_GET      2
#define BV_SET      3
#define BV_DEL      4
#define BV_EXISTS   5
#define BV_TTL      6
#define BV_EXPIRE   7
#define BV_ADD      8
#define BV_SUB      9
#define BV_SETNX    15
#define BV_SETXX    16
#define BV_RLHIT    17                 /* ADD's layout, never replayed */
#define BV_HCMD     18                 /* any H command, never replayed */
#define BV_CMD      19                 /* [mlen u8][method][tree?] */
#define BIN_MAX_RSP (16u << 20)        /* reply payload sanity ceiling */

/* the keys whose JSON value is a DOCUMENT at the API edge: it travels as one bulk of its JSON
 * text, whatever its type (jset's and jarrappend's val) */
static const char *const DOC_KEYS[] = { "val", NULL };

struct early {
	unsigned long long id;
	char *result;                  /* NULL = the error below applies */
	size_t rlen;                   /* payload length (a tree is not NUL-safe) */
	char err[256];
	struct early *next;
};

/* bounded by construction: a 50-node fleet with 1000 clients must not
 * become 50k sockets, so a client keeps a SUBSET */
#define PERFD_MAX_MEMBERS 64
#define PERFD_MAX_SPARES  8
#define PERFD_MAX_SECRETS 8
/* the owner-selection contract this client implements; a mismatch with the daemon's turns
 * routing OFF rather than routing wrongly */
#define PERFD_ROUTE_ALGO "hrw-slot16k-v1"   /* must equal PC_ROUTE_ALGO */
/* must equal PC_CL_EPOCH: a daemon announcing another epoch may mean something else by the same
 * bytes, so routing turns OFF and the daemon forwards; only 4 is routed by */
#define PERFD_WIRE_EPOCH 4

/* one entry of the holder table - which node answered for a key
 * (by a 64-bit hash of collection and key), until when (ms, 0 = no TTL) */
struct hint_e {
	unsigned long long h;
	long long deadline_ms;
	int node;
};

struct perfd {
	int fd;
	int encrypted;
	int want_enc;                  /* opts.encrypt - never ask for plaintext */
	int poisoned;
	int eager_push;                /* opts.eager_push: write per submit */
	char *enable_sec;              /* the privilege secret this
	                                * handle raised with, NULL = none */
	int enable_due;                /* ...to raise again before the next
	                                * call: the connection changed */
	int in_replay;                 /* that raise is in progress */
	char *conn_name;               /* opts.name, NULL = unnamed */
	int name_due;                  /* ...to send before the next call */
	int io_ms;
	struct pc_cipherstate cs_send, cs_recv;
	char err[256];
	int err_kind;                  /* PERFD_E_* of err */

	unsigned long long next_id;

	char *rbuf;                    /* the decrypted/received stream */
	size_t rlen, rcap;
	/* one transport record's scratch (fill's ciphertext or send_bytes' record, never both): 64 KB
	 * on the heap at the first encrypted transfer - on the stack it overflowed musl's */
	uint8_t *xbuf;
	size_t roff;                    /* consumed prefix; see take_msg */
	/* a blocking call's reply is BORROWED from rsc until the next collect, so a warm call allocates
	 * nothing; own_reply wants a malloc'd copy (learn_members runs mid-call); wsc: request scratch */
	char *rsc;
	size_t rscap;
	int own_reply;
	unsigned char *wsc;
	size_t wscap;
	struct pc_tn *tvn;             /* the reply reader's node array, lent */
	int tvcap;                     /* to each parse and taken back */

	char *q;                       /* queued (pipelined) request bytes */
	size_t qlen, qcap;
	unsigned long long *ids;       /* FIFO of ids awaiting replies */
	int nids, idhead, idcap;

	struct early *early;
	unsigned long long stray_replies;      /* replies no request waited for, dropped */

	perfd_notify_cb ncb;
	void *nctx;
	int in_call;                   /* a blocking call is running */
	int hook_refuses;              /* the hook runs inside one: calls back are refused */
	char *nj;                      /* the hook's JSON, rendered from the
	                                * NOTIFY tree; reused */
	size_t njcap;

	/* cluster awareness: a SPARE is a fully-formed handle, so a failover adopts its connection - no
	 * reconnect, no handshake.  Children carry none of this. */
	int is_child;
	int worker_bal;                /* redial off a crowded worker */
	int pin;                       /* never moves off the dialled node */
	int policy;
	int want_spares;
	int refresh_ms;
	long long next_refresh_ms;
	struct pd_member {
		char addr[46];
		int port, node, master, free_mb, total_mb;
		int down;              /* failed: do not re-pick immediately */
		/* a node still replaying its WAL may hold keys deleted while it was down, so it refuses data and
		 * is skipped; 1 for a daemon too old to send the field, as the peer plane reads READY */
		int ready;
		int draining;          /* ready, but being emptied: no new traffic */
		int returning;         /* was down, then answered as not serving: a recovery when it serves */
		char why[64];          /* this handle's last failed dial to it, "" = none */
		long long retry_ms;    /* next background re-dial */
		int backoff_ms;        /* 1 s doubling to 30 s */
	} mem[PERFD_MAX_MEMBERS];
	long long next_recover_ms;     /* the recovery scan runs <= 1/s */
	int active_stale;              /* the active member was expelled -
	                                * fail over before the next request */
	unsigned long long recovered;  /* members brought back */
	/* a spreading handle's move to a member that came up, drawn once */
	int spread_due, spread_node, spread_drawn, spread_left;
	long long spread_next_ms, spread_until_ms, spread_drawn_ms;
	int ps_subs;                   /* the daemon's count: subscribed here, so it stays */
	int nmem;
	int active_node;
	perfd_t *spare[PERFD_MAX_SPARES];
	int nspare;
	/* standbys being dialled; no caller waits for them */
	struct pd_dial {
		perfd_t *c;
		int node, arm, tries, secn;
		long long until_ms;
	} dial[PERFD_MAX_SPARES];
	int ndial;
	char srv_version[32];          /* from the members reply, "" = not learned */
	unsigned int rr;               /* round-robin cursor (random start) */
	unsigned long long failovers;
	/* liveness */
	int keepalive_s, idle_ping_ms;     /* 0 = off, after normalisation */
	long long last_reply_ms;           /* this link's last accepted reply */
	long long next_ping_ms;            /* the idle-ping scan runs <= 1/s */
	unsigned long long pings, pings_failed;
	int io_silent;                     /* a read or write ran out its timeout */
	/* per-key routing: route_ok only when the cluster declared an algorithm we implement AND
	 * a mode worth routing */
	int route_ok, route_want, route_cport;
	int route_epoch;              /* what the daemon last announced */
	/* the stamp of the map the member list was learned under,
	 * and the stamp the last forwarded reply on THIS link carried */
	unsigned int map_term, map_seq, hint_term, hint_seq;
	int map_known, hint_seen, ignore_push;
	long long hint_next_ms;
	unsigned long long moved_hints;
	int learning, learned_async;   /* async fleet discovery */
	long long async_learn_ms;      /* the next one, at refresh_ms */
	unsigned long long route_missed;   /* owner known, not connected */
	/* proxy holder hints: hh_* is what the LAST reply on this link carried, hints the per-key table
	 * (the parent's, route_hints only), want_hh asks the daemon for hints */
	int route_hints, want_hh, hh_got, hh_node;
	unsigned int hh_ttl;
	struct hint_e *hints;
	unsigned long long hint_hits, hint_stale;
	int hinted;                        /* the node route_pick chose by a
	                                    * hint for the current call, or 0 */
	int route_k;                       /* the fleet's copy factor (routing.replicas); 1 for
	                                    * shard and for a daemon that does not publish it */
	/* the options spares are opened with - the secrets are copied, so
	 * the caller's array need not outlive the handle */
	char *sec[PERFD_MAX_SECRETS + 1];
	int nsec;
	int o_connect_ms, o_io_ms;

	/* event-loop surface: on every handle, inert unless perfd_connect_async() made it */
	int async;
	int st;                        /* enum perfd_state */
	int hs_stage;                  /* see PD_HS_* */
	int hs_sec;                    /* which secret is being tried */
	struct pc_handshake hs;
	char *w;                       /* bytes owed to the socket */
	size_t wlen, wcap, woff;
	char *wst;                     /* plaintext staged for one seal a push */
	size_t wstlen, wstcap;
	char *cin;                     /* ciphertext not yet a whole record */
	size_t cinlen, cincap;
	struct pd_call {               /* id -> who asked */
		unsigned long long id;
		perfd_reply_cb cb;
		void *ctx;
		int render;            /* 1 = the reply tree goes to the callback as
		                        * JSON text (perfd_submit); 0 = the raw payload */
	} *calls;
	int ncalls, callcap;
	char ahost[256];
	int aport;
	struct pd_udp *udp;            /* pushes over UDP, or NULL */
	/* a call never replayed on a failover whatever its method (a conditional jset) */
	int once;
	/* the member list's fingerprint, and the count of its changes (perfd_fleet_changes) */
	unsigned long long fleet_fp, fleet_changes;
};

/* usable = reachable AND willing: `down` is what THIS client saw, `ready` what the fleet says;
 * both gate every selection, so a recovering node is skipped rather than dialled and refused */
static int pd_usable(const struct pd_member *m)
{
	return !m->down && m->ready;
}

/* where a spreading handle may go: known members in any other state stay known */
static int pd_spreadable(const struct pd_member *m)
{
	return pd_usable(m) && !m->draining;
}

static struct pd_member *member_of(perfd_t *p, int node)
{
	int i;

	for (i = 0; i < p->nmem; i++)
		if (p->mem[i].node == node)
			return &p->mem[i];
	return NULL;
}

static void notify_internal(perfd_t *p, const unsigned char *tree, size_t n);
static void notify_hook(perfd_t *p, const unsigned char *tree, size_t n);
static int failover(perfd_t *p);
static void spread_arm(perfd_t *p, const struct pd_member *m);
static int spare_for(perfd_t *p, int node);
static int conn_where(perfd_t *p, int *mine, int *least, int *workers, int *serving);
static int state_usable(const char *s, int sl);
static void spread_leave(perfd_t *p);
static void udp_drop(perfd_t *p, int pruned);
static void fleet_seen(perfd_t *p);
/* async connect/handshake progression */
#define PD_HS_TCP     0            /* connect() outstanding */
#define PD_HS_SEND    1            /* msg1 built, still going out */
#define PD_HS_WAIT    2            /* msg1 sent, msg2 not yet complete */
#define PD_HS_DONE    3

static char g_connect_err[256];    /* perfd_error(NULL) - see header */
static int g_connect_kind;         /* perfd_error_kind(NULL) */

const char *perfd_version(void)
{
	return PERFD_VERSION;
}

/* every error says what KIND it is (PERFD_E_*): callers and bindings branch on that, never on
 * the text */
static int errk(perfd_t *p, int kind, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	/* textbook va_start/vsnprintf/va_end; the checker misfires on the
	 * ternary in the first argument */
	/* NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized) */
	vsnprintf(p ? p->err : g_connect_err,
		sizeof ((perfd_t *)0)->err, fmt, ap);
	va_end(ap);
	if (p)
		p->err_kind = kind;
	else
		g_connect_kind = kind;
	return -1;
}

/* a daemon's error frame: refusals a client may retry later or elsewhere are REFUSED, the rest
 * SERVER; "forward failed" is answered before the forward is sent - nothing applied */
static int server_kind(const char *msg)
{
	static const char *const refusals[] = {
		"max number of clients", "node is not READY", "refuses writes",
		"cluster busy", "parked-request table full", "node is FAILED",
		"node is HEALING", "forward failed", NULL };
	int i;

	for (i = 0; msg && refusals[i]; i++)
		if (strstr(msg, refusals[i]))
			return PERFD_E_REFUSED;
	return PERFD_E_SERVER;
}

const char *perfd_error(const perfd_t *p)
{
	return p ? p->err : g_connect_err;
}

int perfd_error_kind(const perfd_t *p)
{
	return p ? p->err_kind : g_connect_kind;
}

/* the body lives in parts, one translation unit, included in this order */
#include "perfd-io.inc"
#include "perfd-request.inc"
#include "perfd-fleet.inc"
#include "perfd-members.inc"
#include "perfd-call.inc"
#include "perfd-pubsub.inc"
#include "perfd-verbs.inc"
#include "perfd-connect.inc"
#include "perfd-async.inc"
#include "perfd-url.inc"
#include "perfd-fleetinfo.inc"
#include "perfd-exec.inc"

#include "perfd-apool.inc"
