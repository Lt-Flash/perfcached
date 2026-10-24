/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * protoint.h - the client doors' internal types (proto.c): one client
 * connection, whichever door it came in by.  For the files proto.c is
 * split into (the 0.7.2 split, wave 1 - doc/MODULARITY.md); proto.h stays
 * the API the rest of the daemon calls.
 */
#ifndef PC_PROTOINT_H
#define PC_PROTOINT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "json.h"                       /* struct pc_jw */
#include "pc_noise.h"                   /* the handshake and cipher states */
#include "pcaddr.h"                     /* pc_addr */
#include "proto.h"                      /* PC_MAX_REQ */

struct pc_udp;
struct pc_enum_start;

/* ---- little-endian helpers (memcpy: no unaligned assumptions) ---------- */

static inline uint16_t get16(const void *p)
{
	unsigned char b[2];

	memcpy(b, p, 2);
	return (uint16_t)(b[0] | (b[1] << 8));
}

static inline void put16(void *p, uint16_t v)
{
	unsigned char b[2] = { (unsigned char)v, (unsigned char)(v >> 8) };

	memcpy(p, b, 2);
}

static inline uint32_t get32(const void *p)
{
	unsigned char b[4];

	memcpy(b, p, 4);
	return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
	       ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static inline uint64_t get64(const void *p)
{
	return (uint64_t)get32(p) | ((uint64_t)get32((const char *)p + 4) << 32);
}

static inline void put32(void *p, uint32_t v)
{
	unsigned char b[4] = { (unsigned char)v, (unsigned char)(v >> 8),
		(unsigned char)(v >> 16), (unsigned char)(v >> 24) };

	memcpy(p, b, 4);
}

static inline void put64(void *p, uint64_t v)
{
	put32(p, (uint32_t)v);
	put32((char *)p + 4, (uint32_t)(v >> 32));
}

#define PC_HS_MAXFRAME  2048
#define PC_RESP_MAXARGS 512            /* args per RESP command (DEL k...) */
/* the reply scratch's size: a request's largest payload, and room */
#define JW_REPLY_CAP (PC_MAX_REQ + 4096)
/* how a parked request is to be answered - the request's framing, not
 * the connection's: replies match requests */
#define PARK_BIN  1                   /* a data verb's fixed reply layout */
#define PARK_RESP 2                   /* a RESP command */
#define PARK_CMD  3                   /* S317: a CMD method - a tree reply */

/* what a native connection's first byte settled it into; the memo that
 * picks whether a notification can be framed (S317: PC_D_TEXT is gone) */
enum pc_dialect { PC_D_SNIFF = 0, PC_D_BIN, PC_D_RESP };

struct pc_conn {
	int fd;
	int ep;
	enum pc_dialect dialect;
	int close_after;                   /* RESP QUIT: flush, then close */
	int dead;                          /* S356: destroyed, awaiting the reap */
	int rt_fetcher;                    /* S384: another perfcached's read-
	                                    * through fetch connection - this
	                                    * node never reads through for it */
	/* S40: an in-flight cooperative KEYS walk.  While active, further
	 * input on this conn stays buffered (RESP replies are ordered), and
	 * pc_enum_step() advances the walk one bounded chunk per event-loop
	 * turn.  The reply grows in enum_w, a HEAP writer - the shared
	 * per-worker scratch cannot hold state across turns. */
	void *obs;                         /* S53: the CLIENT LIST row */
	int enum_active;
	unsigned int enum_cursor;
	struct pc_jw enum_w;
	int enum_emitted, enum_limit, enum_patlen;
	char enum_pat[256];
	void *enum_ht;
	int enum_col;                      /* S150 B: enum_ht's registry slot */
	unsigned long long enum_pub;       /* and its publish generation at start */
	struct pc_conn *enum_next;
	/* S331: a command parked on a read-through fetch (its request id).
	 * While nonzero, further input stays buffered (as for a walk) and the
	 * command itself stays in the buffer, to be parsed and run again when
	 * the completion arrives.  up_verdict is what the completion said
	 * (PC_UPV_*): the upstream gate reads it on that run, and it is
	 * cleared once a command has run without parking. */
	unsigned int up_hold;
	int up_verdict;
	int up_lost;                       /* S435: the command's re-gates after a
	                                    * read lost its key (at most 2) */
	unsigned long long up_mark;        /* S435: the removals done when the
	                                    * gate parked it (upstream.c's) */
	/* S332, G8: a RESP command waits on the cluster - a pull, a forward
	 * or a probe (a park entry), or an MGET's lookups (mgetfan.c).  The
	 * command itself was consumed - its reply is written when the answer
	 * comes - but what the client pipelined behind it stays buffered, as
	 * for a walk, until that reply is out: RESP replies are ordered */
	int park_hold;
	char peer[48];                     /* S153: ip:port, filled on first use */
	int resp_only;                     /* S33: RESP-pinned listener */
	int http_loop;                     /* 0.6.2: an HTTP peer on loopback - no
	                                    * rate limit, never a cached /stats */
	int http_only;                     /* S46: metrics listener - GET
	                                    * only, one request, then close */
	unsigned int born;                 /* get_ticks() at accept; only
	                                    * read for http_only conns */
	const char *why;                   /* S70: why it closed, for the log;
	                                    * NULL = the peer closed it */
	int resp_authed;                   /* AUTH satisfied (or not needed) */
	/* S129: this CONNECTION has raised privilege with `enable`.  Set by
	 * the verb, cleared by `disable` and by closing the connection -
	 * it lives and dies here, is never in the table, never on the wire
	 * between nodes, and is never consulted when a PEER applies DDL
	 * (a replicated create is the fleet's decision, not a client's). */
	int privileged;
	int client_counted;                /* S161: in pc_clients_open */
	int refused;                       /* S161: over max_clients - answer
	                                    * the first request with the
	                                    * refusal, then close; 2 = refused
	                                    * at the memory floor (S366) */
	int open_counted;                  /* S123: in an open-connections
	                                    * gauge, to be taken out at
	                                    * destroy */
	/* S366: an idle connection gives its buffers back.  bufs_held: in,
	 * out, wire or raw is allocated (counted in bufs_held_here); last_io:
	 * the worker's tick at its last event - the sweep frees the buffers
	 * of a connection quiet for PC_IDLE_RELEASE_S with nothing in them */
	int bufs_held;
	unsigned int last_io;

	/* RESP database-index state: the selected collection NAME (SELECT
	 * n moves it; "0" until then - see pc_verb_resp) */
	char resp_col[40];
	size_t resp_collen;

	char *in;
	size_t in_len, in_cap;
	char *out;                          /* plaintext replies (staging) */
	size_t out_len, out_cap;
	char *wire;                         /* socket output buffer */
	size_t wire_len, wire_off, wire_cap;
	int want_write;
	size_t obs_pending;                 /* S160: what the row was last told */
	int ps_used;                        /* the pub/sub engine knows this conn */
	int ps_killed;                      /* pc_conn_kill already closing it */
	/* S372: its replies outran its socket - its requests wait, its socket
	 * is not read, until they drain (pc_conn_resume_out) */
	int out_paused;
	unsigned int out_pauses_n;          /* times it was throttled: its close says */
	unsigned int out_progress;          /* the tick its output last moved */
	struct pc_conn *outp_next;
	int out_failed;                     /* a reply did not fit: 1 = past
	                                     * PC_MAX_OUTQ (a slow consumer),
	                                     * 2 = out of memory - the reason
	                                     * its close gives, if none other */
	int push_staged;                    /* on this worker's push_dirty list */
	struct pc_conn *push_next;
	int ps_paused;                      /* a queue it publishes into is full */
	uint64_t *pause_mask;               /* ...these workers' queues */
	struct pc_conn *pause_next;
	struct pc_udp *udp;                 /* PS5: its push stream, or NULL */
	/* encryption (NULL psk => plaintext connection) */
	const struct pc_psk_ctx *psk;
	int encrypted;
	int established;
	char *raw;                          /* undecrypted socket input */
	size_t raw_len, raw_cap;
	struct pc_handshake hs;
	struct pc_cipherstate send_cs, recv_cs;
	int hs_parked;                      /* ST5: waiting for a turn's handshake */
	struct pc_conn *hs_next, *hs_prev;
	/* S361: on an io_uring worker (ep < 0), the requests in flight - see
	 * "the io_uring backend" below */
	unsigned char ur_recv, ur_cancel, ur_pollout, ur_in_limbo, ur_starved;
	unsigned char ur_send;              /* step 3: a send of wire is in flight */
	char *ur_wire_hold;                 /* a dead conn's wire, until that send ends */
	int ur_inflight;
	struct pc_conn *ur_starved_next;

	struct pc_conn *next, *prev;
	struct pc_conn **list;
};

/* ---- owned by proto.c ------------------------------------------------- */

/* stage @n bytes of reply on @c (encrypted at the flush when the
 * connection is); 0, or -1 when it would not fit (c->out_failed says why) */
int out_append(struct pc_conn *c, const char *p, size_t n);
/* the monotonic clock in ms (the deny queue's, the HTTP rate limit's) */
long long deny_now(void);
/* S153: name this request's origin (@door, the connection's peer) for the
 * event log */
void ev_origin(struct pc_conn *c, const char *door);
/* this worker's reply scratch, JW_REPLY_CAP bytes: slot 0 the reply a
 * request builds, slot 1 the RESP codec's assembly space; NULL without
 * memory */
char *scratch_buf(int i);
/* park request @req on @c for a cluster pull or a forward to answer (@id:
 * a frame's 8 id bytes, NULL for RESP; @bin a PARK_*); 0 parked, -1 not */
int park_add(struct pc_conn *c, unsigned int req, const char *id,
		const char *col, size_t collen, const char *key, size_t klen,
		int bin, int hint);
/* @n bytes onto the connection's plaintext input (decrypted, or read in
 * the clear) or its socket output (sealed records); 0, or -1 past the
 * bound or without memory - and, for the output, while an io_uring send
 * of it is in flight (S361) */
int in_append(struct pc_conn *c, const char *p, size_t n);
int wire_append(struct pc_conn *c, const char *p, size_t n);
/* the connection's life: everything buffered in its input run in order
 * (0, or -1 a protocol error); its staged replies to the socket (0, or -1
 * a write error); and its end */
int process_plaintext(struct pc_conn *c);
int conn_flush(struct pc_conn *c);
void conn_destroy(struct pc_conn *c);
/* S159: the clock carried from one request's end to the next's start
 * within one drain (0 = read a fresh one) */
extern __thread unsigned long long obs_carry;

/* ---- noisechan.c: the Noise channel, the responder's side -------------- */

/* one handshake frame from c->raw: 1 established (message 2 staged on the
 * wire), 0 more to read, -1 fatal - a plaintext binary header (S339), an
 * unknown principal, no secret matched, an unknown client version */
int channel_handshake(struct pc_conn *c);
/* every complete transport record in c->raw decrypted onto c->in; 0, or
 * -1 a record out of bounds or a tag that does not verify */
int channel_records(struct pc_conn *c);

/* ---- enumwalk.c: the cooperative KEYS walk (S40) ------------------------ */

/* a walk the verb asked for (@es) begins on @c, joining this worker's
 * list; -1 when its reply buffer cannot be had */
int enum_begin(struct pc_conn *c, const struct pc_enum_start *es);
/* the walk on @c (if any) ends unanswered: its writer freed, unlinked */
void enum_abort(struct pc_conn *c);

/* ---- respdoor.c: the RESP door's framing (S29) ------------------------- */

/* one RESP2 command from c->in at @off - multibulk, or an inline line -
 * run through pc_verb_resp: the bytes consumed, 0 while it is incomplete
 * (or parked, left in the buffer), -1 to drop the connection */
int resp_one(struct pc_conn *c, size_t off);
/* a RESP length field: an optional sign, then digits - 17 characters at
 * most; 0, or -1 */
int resp_ll_span(const char *p, size_t n, long long *out);

/* ---- httpdoor.c: the metrics door (S46) --------------------------------- */

/* one request from c->in at @off: the bytes consumed (answered), 0 while
 * the head is incomplete, -1 to drop the connection */
int http_one(struct pc_conn *c, size_t off);
/* 0.6.2: a new HTTP connection from @sa within http_rate a second - a
 * token bucket two seconds deep per IPv4 address or IPv6 /64, loopback
 * exempt unless it is limited too; 0 = over it (the caller answers 429) */
int http_rate_ok(const pc_addr *sa);

#endif
