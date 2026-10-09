/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov
 * Part of libperfd - see lib/LICENSE.  This file must stay
 * free of src/core includes; tools/sync-libperfd.sh exports
 * exactly the MIT set to consumers. */
/*
 * perfd.c — libperfd.  See perfd.h for the contract.
 *
 * Internals: every request (typed or appended) pushes its id onto a
 * FIFO; the reply reader delivers strictly in that order, parking
 * out-of-order arrivals on a small list - the daemon is entitled to
 * answer out of order and the library owes the caller order.  One
 * frame reader (take_msg) serves both plaintext and Noise transports;
 * the Noise leg reuses the daemon's own pc_noise.[ch] verbatim.
 *
 * S317: the wire is binary only.  The data verbs keep their fixed
 * layouts (enqueue_bin); every other method rides PC_VERB_CMD with its
 * parameters as a tree and answers a tree (src/ptree.h, the one codec).
 * JSON text exists only at the API edge - perfd_command / perfd_append
 * / perfd_submit take params as JSON and hand results back as JSON, the
 * notify hook sees JSON - converted here, never sent.
 */
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

/* S369: a handshake message's ceiling - the daemon refuses a frame larger
 * than this (proto.c PC_HS_MAXFRAME); msg1 and msg2 are ~50 bytes.  Sized
 * to it, not to PC_NOISE_MAXMSG: two 64 KB buffers on the stack were
 * noise_handshake's 131,440-byte frame, past a musl thread's 128 KB. */
#define PERFD_HS_MAX   2048
#define PERFD_XBUF     (2 + PC_NOISE_MAXPT + PC_NOISE_TAGLEN)

#include "../src/json.h"
#include "../src/ptree.h"             /* S317: the tree codec, both ways */
#include "../src/pc_noise.h"
#include "perfd.h"
#include "perfd_push.h"                /* PS5: the UDP push datagram */
#include "../src/pc_slot.h"           /* the ONE key->slot function */
#include "../src/pc_mix.h"            /* ...and the ONE rendezvous mix */

#define DEF_TIMEOUT_MS 5000
#define DEF_KEEPALIVE_S   30       /* S104: TCP keepalive idle */
#define DEF_IDLE_PING_MS  30000    /* S104: ping a standby idle this long */
#define KEY_MAX  4096
/* S174: there was a VAL_MAX 65536 here.  Nothing read it - the daemon
 * is the authority on the value ceiling and answers with an error - and
 * a stale copy of a ceiling is how a client starts refusing what the
 * server accepts. */

/* the binary frame constants - pinned to src/proto.h (the daemon is
 * the authority; the lib deliberately does not include daemon-internal
 * headers beyond json/ptree/noise) */
#define BIN_MAGIC   0x9E
#define BIN_VER     0x01
#define BIN_REQ     1
#define BIN_RSP     2
#define BIN_NOTIFY  3
#define BIN_F_ERR   0x01
#define BIN_F_MOVED 0x02               /* S317: [term u32][seq u32] trail the tree */
#define BIN_F_HINT  0x04               /* S295: request: send a holder hint;
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
#define BV_SETNX    15                 /* S279b */
#define BV_SETXX    16
#define BV_RLHIT    17                 /* S314: ADD's layout, never replayed */
#define BV_HCMD     18                 /* S313: any H command, never replayed */
#define BV_CMD      19                 /* S317: [mlen u8][method][tree?] */
#define BIN_MAX_RSP (16u << 20)        /* reply payload sanity ceiling */

/* S317: the keys whose JSON value is a DOCUMENT at the API edge - it
 * travels as one bulk of its JSON text, whatever its type (jset's and
 * jarrappend's val); pc_tree_from_json does the cut */
static const char *const DOC_KEYS[] = { "val", NULL };

struct early {
	unsigned long long id;
	char *result;                  /* NULL = the error below applies */
	size_t rlen;                   /* payload length (a tree is not NUL-safe) */
	char err[256];
	struct early *next;
};

/* Bounded by construction: a 50-node fleet with 1000 clients must not
 * become 50k sockets, so a client keeps a SUBSET (S34). */
#define PERFD_MAX_MEMBERS 64
#define PERFD_MAX_SPARES  8
#define PERFD_MAX_SECRETS 8
/* the owner-selection contract this client implements; the daemon
 * reports its own in the members reply and a mismatch turns routing
 * OFF rather than routing wrongly (S35) */
#define PERFD_ROUTE_ALGO "hrw-slot16k-v1"   /* must equal PC_ROUTE_ALGO */
/* S221: the cluster wire epoch this library was built against - it must
 * equal PC_CL_EPOCH.  A routing client reads the daemon's map and
 * decides ownership for itself, so it is party to the same contract the
 * epoch names; a daemon announcing another one may mean something else
 * by the same bytes, and the answer is the one the algo already gets -
 * routing OFF, the daemon forwards, correctness costs a hop.  S317
 * (0.4.5) moved it to 2 when the ALIVE frame lost its JSON-client
 * count; a daemon that says 1 or 0 is older than this wire and is not
 * routed by. */
#define PERFD_WIRE_EPOCH 2

/* S295: one entry of the holder table - which node answered for a key
 * (by a 64-bit hash of collection and key), until when (ms, 0 = no TTL) */
struct hint_e {
	unsigned long long h;
	long long deadline_ms;
	int node;
};

struct perfd {
	int fd;
	int encrypted;
	int poisoned;
	int eager_push;                /* opts.eager_push: write per submit */
	int io_ms;
	struct pc_cipherstate cs_send, cs_recv;
	char err[256];

	unsigned long long next_id;

	char *rbuf;                    /* the decrypted/received stream */
	size_t rlen, rcap;
	/* S369: one transport record's scratch - the ciphertext fill reads into
	 * and the record send_bytes builds (never both at once).  64 KB, made
	 * at the first encrypted transfer, on the heap: on the STACK (two
	 * arrays) it overflowed musl's 128 KB thread stack. */
	uint8_t *xbuf;
	size_t roff;                    /* consumed prefix; see take_msg */
	/* S284: the reply a blocking call hands its caller is BORROWED from
	 * rsc - copied there by collect_head, valid until this handle's next
	 * collect - so a call on a warm handle allocates nothing.  own_reply
	 * asks collect_head for a malloc'd copy instead: learn_members sets
	 * it, because it runs inside after_success, between the collect of
	 * the caller's reply and the caller reading it.  wsc is the request
	 * scratch the typed calls encode into. */
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

	perfd_notify_cb ncb;
	void *nctx;
	char *nj;                      /* the hook's JSON, rendered from the
	                                * NOTIFY tree; reused (S317) */
	size_t njcap;

	/* ---- cluster awareness (S34) ----
	 * A SPARE is itself a fully-formed handle (own socket, own cipher
	 * state, own buffers), so a failover is an adopt() of its
	 * connection - no reconnect, no handshake, no second wire
	 * implementation.  Children carry none of this. */
	int is_child;
	int policy;
	int want_spares;
	int refresh_ms;
	long long next_refresh_ms;
	struct pd_member {
		char addr[46];
		int port, node, master, free_mb, total_mb;
		int down;              /* failed: do not re-pick immediately */
		/* C7: the daemon has published per-member state since B1 and
		 * nothing read it.  A node still replaying its WAL can hold a
		 * key that was DELETED while it was down, so it answers
		 * wrongly rather than answering a miss - and it refuses data
		 * verbs for exactly that reason.  Skipping it here is what
		 * turns that refusal into a non-event.
		 * Defaults to 1 for a daemon too old to send the field, the
		 * same way the peer plane reads a missing state as READY. */
		int ready;
		char why[64];          /* S70: this handle's last failed dial to it, "" = none */
		long long retry_ms;    /* S107: next background re-dial */
		int backoff_ms;        /* S107: 1 s doubling to 30 s */
	} mem[PERFD_MAX_MEMBERS];
	long long next_recover_ms;     /* S107: the recovery scan runs <= 1/s */
	int active_stale;              /* S107: the active member was expelled -
	                                * fail over before the next request */
	unsigned long long recovered;  /* S107: members brought back */
	int nmem;
	int active_node;
	perfd_t *spare[PERFD_MAX_SPARES];
	int nspare;
	char srv_version[32];          /* S70: from the members reply, "" = not learned */
	unsigned int rr;               /* round-robin cursor (random start) */
	unsigned long long failovers;
	/* S104 liveness */
	int keepalive_s, idle_ping_ms;     /* 0 = off, after normalisation */
	long long last_reply_ms;           /* this link's last accepted reply */
	long long next_ping_ms;            /* the idle-ping scan runs <= 1/s */
	unsigned long long pings, pings_failed;
	/* S35: per-key routing.  route_ok is set only when the cluster
	 * declared an algorithm we implement AND a mode worth routing. */
	int route_ok, route_want, route_cport;
	int route_epoch;              /* S221: what the daemon last announced */
	/* RV-10: the stamp of the map the member list was learned under,
	 * and the stamp the last forwarded reply on THIS link carried */
	unsigned int map_term, map_seq, hint_term, hint_seq;
	int map_known, hint_seen, ignore_push;
	long long hint_next_ms;
	unsigned long long moved_hints;
	int learning, learned_async;   /* async fleet discovery, once */
	unsigned long long route_missed;   /* owner known, not connected */
	/* S295: proxy holder hints.  hh_* is the hint the LAST reply on this
	 * link carried (take_msg), hints the per-key table (route_hints
	 * only; the parent's), want_hh asks the daemon for hints */
	int route_hints, want_hh, hh_got, hh_node;
	unsigned int hh_ttl;
	struct hint_e *hints;
	unsigned long long hint_hits, hint_stale;
	int hinted;                        /* the node route_pick chose by a
	                                    * hint for the current call, or 0 */
	int route_k;                       /* S127: the fleet's copy factor,
	                                    * learned from routing.replicas;
	                                    * 1 for shard and for any daemon
	                                    * that does not publish it */
	/* the options spares are opened with - the secrets are copied, so
	 * the caller's array need not outlive the handle */
	char *sec[PERFD_MAX_SECRETS + 1];
	int nsec;
	int o_connect_ms, o_io_ms;

	/* ---- event-loop surface (S32) ----
	 * Present on every handle but inert unless perfd_connect_async()
	 * created it, so the blocking API is untouched by its existence. */
	int async;
	int st;                        /* enum perfd_state */
	int hs_stage;                  /* see PD_HS_* */
	int hs_sec;                    /* which secret is being tried */
	struct pc_handshake hs;
	char *w;                       /* bytes owed to the socket */
	size_t wlen, wcap, woff;
	char *cin;                     /* ciphertext not yet a whole record */
	size_t cinlen, cincap;
	struct pd_call {               /* id -> who asked */
		unsigned long long id;
		perfd_reply_cb cb;
		void *ctx;
		int render;            /* S317: the reply tree goes to the
		                        * callback as JSON text (perfd_submit);
		                        * 0 = the raw payload (submit_kv, the
		                        * library's own requests) */
	} *calls;
	int ncalls, callcap;
	char ahost[256];
	int aport;
	struct pd_udp *udp;            /* PS5: pushes over UDP, or NULL */
};

/* C7: usable = reachable AND willing to serve.  `down` is what THIS
 * client observed; `ready` is what the fleet says.  Both gate every
 * selection below, so a recovering node is skipped rather than dialled
 * and refused. */
static int pd_usable(const struct pd_member *m)
{
	return !m->down && m->ready;
}

static void notify_internal(perfd_t *p, const unsigned char *tree, size_t n);
static void notify_hook(perfd_t *p, const unsigned char *tree, size_t n);
static int failover(perfd_t *p);
static void udp_drop(perfd_t *p, int pruned);   /* PS5 */
/* async connect/handshake progression */
#define PD_HS_TCP     0            /* connect() outstanding */
#define PD_HS_SEND    1            /* msg1 built, still going out */
#define PD_HS_WAIT    2            /* msg1 sent, msg2 not yet complete */
#define PD_HS_DONE    3

static char g_connect_err[256];    /* perfd_error(NULL) - see header */

const char *perfd_version(void)
{
	return PERFD_VERSION;
}

static int err(perfd_t *p, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	/* textbook va_start/vsnprintf/va_end; the checker misfires on the
	 * ternary in the first argument */
	/* NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized) */
	vsnprintf(p ? p->err : g_connect_err,
		sizeof ((perfd_t *)0)->err, fmt, ap);
	va_end(ap);
	return -1;
}

const char *perfd_error(const perfd_t *p)
{
	return p ? p->err : g_connect_err;
}

/* The body lives in parts (2026-10-05), included in this order - one
 * translation unit, as before; see each part's header for what it holds. */
#include "perfd-io.inc"
#include "perfd-request.inc"
#include "perfd-fleet.inc"
#include "perfd-members.inc"
#include "perfd-call.inc"
#include "perfd-pubsub.inc"
#include "perfd-verbs.inc"
#include "perfd-connect.inc"
#include "perfd-async.inc"
