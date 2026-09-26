/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * proto.c — connection + dual-dialect protocol machinery (tasks S7, S25').
 * See proto.h for the wire contract.
 *
 * A connection on an encryption-required listener runs the Noise
 * responder handshake first (task S25'), then every socket byte is a
 * transport record; the decrypted plaintext feeds the exact same dialect
 * machinery a loopback plaintext connection uses.  So the sniff / codec /
 * out-of-order reply core are written once and both planes share them.
 *
 * Buffers per connection: in (plaintext for the dialect), out (plaintext
 * replies the verbs append), wire (the bytes actually written to the
 * socket), and - only when encrypted - raw (undecrypted socket input).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <poll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sodium.h>

#include "compat/dprint.h"
#include "json.h"
#include "metrics.h"
#include "statuspage.h"
#include "pc_noise.h"
#include "verbs.h"
#include "obs.h"
#include "clunk.h"                     /* S165 */
#include "store.h"
#include "events.h"
#include <arpa/inet.h>
#include "pc_slot.h"     /* S127: pc_key_slot, for the spread holder test */
#include "core/pcache_htable.h"
#include "compat/timer.h"
#include "daemon.h"
#include "proto.h"
#include "pubsub.h"
#include "psudp.h"                  /* PS5 */
#include <sodium.h>
#include <time.h>
enum pc_dialect { PC_D_SNIFF = 0, PC_D_TEXT, PC_D_BIN, PC_D_RESP };

#define PC_HS_MAXFRAME  2048
#define PC_RESP_MAXARGS 512            /* args per RESP command (DEL k...) */

struct pc_conn {
	int fd;
	int ep;
	enum pc_dialect dialect;
	int close_after;                   /* RESP QUIT: flush, then close */
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
	char peer[48];                     /* S153: ip:port, filled on first use */
	int resp_only;                     /* S33: RESP-pinned listener */
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
	                                    * refusal, then close */
	int open_counted;                  /* S123: in an open-connections
	                                    * gauge, to be taken out at
	                                    * destroy */

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

	struct pc_conn *next, *prev;
	struct pc_conn **list;
};

/* S153: name this request's origin for the event log.  The door is the
 * dialect it arrived in, which only the dispatch site knows; the peer is
 * read once per connection. */
static void ev_origin(struct pc_conn *c, const char *door)
{
	if (!c->peer[0]) {
		struct sockaddr_in sa;
		socklen_t sl = sizeof sa;
		char ip[INET_ADDRSTRLEN];

		if (getpeername(c->fd, (struct sockaddr *)&sa, &sl) == 0 &&
		        sa.sin_family == AF_INET &&
		        inet_ntop(AF_INET, &sa.sin_addr, ip, sizeof ip))
			snprintf(c->peer, sizeof c->peer, "%s:%u", ip,
				(unsigned int)ntohs(sa.sin_port));
		else
			snprintf(c->peer, sizeof c->peer, "-");
	}
	pc_ev_set_origin(door, c->peer);
}

/* ---- little-endian helpers (memcpy: no unaligned assumptions) ---------- */

static uint16_t get16(const void *p)
{
	unsigned char b[2];

	memcpy(b, p, 2);
	return (uint16_t)(b[0] | (b[1] << 8));
}

static void put16(void *p, uint16_t v)
{
	unsigned char b[2] = { (unsigned char)v, (unsigned char)(v >> 8) };

	memcpy(p, b, 2);
}

static uint32_t get32(const void *p)
{
	unsigned char b[4];

	memcpy(b, p, 4);
	return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
	       ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static uint64_t get64(const void *p)
{
	return (uint64_t)get32(p) | ((uint64_t)get32((const char *)p + 4) << 32);
}

static void put32(void *p, uint32_t v)
{
	unsigned char b[4] = { (unsigned char)v, (unsigned char)(v >> 8),
		(unsigned char)(v >> 16), (unsigned char)(v >> 24) };

	memcpy(p, b, 4);
}

static void put64(void *p, uint64_t v)
{
	put32(p, (uint32_t)v);
	put32((char *)p + 4, (uint32_t)(v >> 32));
}

/* ---- growable buffers -------------------------------------------------- */

static int buf_reserve(char **b, size_t *cap, size_t have, size_t need,
		size_t limit)
{
	size_t want = have + need, c = *cap;
	char *nb;

	if (want > limit)
		return -1;
	if (want <= c)
		return 0;
	while (c < want)
		c = c ? c * 2 : 4096;
	nb = realloc(*b, c);
	if (!nb)
		return -2;                     /* out of memory, not over the limit */
	*b = nb;
	*cap = c;
	return 0;
}

static int in_append(struct pc_conn *c, const char *p, size_t n)
{
	if (buf_reserve(&c->in, &c->in_cap, c->in_len, n,
	        PC_MAX_REQ + PC_BIN_HDR) < 0)
		return -1;
	memcpy(c->in + c->in_len, p, n);
	c->in_len += n;
	return 0;
}

static int out_append(struct pc_conn *c, const char *p, size_t n)
{
	int rc = buf_reserve(&c->out, &c->out_cap, c->out_len, n, PC_MAX_OUTQ);

	if (rc < 0)
		return rc;                     /* -1 at the cap, -2 out of memory */
	memcpy(c->out + c->out_len, p, n);
	c->out_len += n;
	return 0;
}

static int wire_append(struct pc_conn *c, const char *p, size_t n)
{
	if (buf_reserve(&c->wire, &c->wire_cap, c->wire_len, n,
	        PC_MAX_OUTQ + (PC_MAX_OUTQ >> 4)) < 0)
		return -1;
	memcpy(c->wire + c->wire_len, p, n);
	c->wire_len += n;
	return 0;
}

/* ---- parked pulls (thread-local: worker-owned) -------------------------- */

/* Sized from the cluster's parked-request capacity, which is now a
 * runtime knob ([cluster] max_pending).  The invariant is what matters:
 * a worker must never out-park the pending table, or parking becomes
 * the first thing to refuse and the cluster's own backpressure - the
 * retryable TRYAGAIN - never gets a chance to speak.  Raising
 * max_pending while this stayed at 1024 did exactly that: the refusals
 * moved here and the client saw "-ERR busy" with the cluster table
 * reporting pend_exhausted 0.
 *
 * Worst case on purpose: every parked request could belong to ONE
 * worker, so each worker's table is the full capacity.  Allocated once
 * per worker thread, never resized. */
#define PARK_FALLBACK 1024            /* cluster off: parking is unused */

struct parked {
	struct pc_conn *conn;          /* NULL = free */
	unsigned int req;
	int bin;                       /* the request's dialect, not the
	                                * conn's: replies match requests */
	int retried;                   /* shard grace: one broadcast retry */
	char id[72];                   /* text: raw JSON id; bin: 8 LE bytes */
	size_t idlen;                  /* 0 = notification-style, no id */
	char col[40];
	size_t collen;
	char *key;
	size_t klen;
};

static __thread struct parked *parks;
static __thread int parks_cap;

/* S40: the conns on THIS worker with a cooperative walk in flight */
static __thread struct pc_conn *enum_head;

static void enum_unlink(struct pc_conn *c)
{
	struct pc_conn **pp = &enum_head;

	while (*pp && *pp != c)
		pp = &(*pp)->enum_next;
	if (*pp)
		*pp = c->enum_next;
	c->enum_next = NULL;
}

static void enum_abort(struct pc_conn *c)
{
	if (!c->enum_active)
		return;
	pc_jw_free(&c->enum_w);
	c->enum_active = 0;
	enum_unlink(c);
}

static int parks_ready(void)
{
	if (parks)
		return 1;
	parks_cap = pc_cluster_pend_cap();
	if (parks_cap < 1)
		parks_cap = PARK_FALLBACK;
	parks = calloc((size_t)parks_cap, sizeof *parks);
	return parks != NULL;
}

static int park_add(struct pc_conn *c, unsigned int req, const char *id,
		size_t idlen, const char *col, size_t collen, const char *key,
		size_t klen, int bin)
{
	int i;

	if (!parks_ready())
		return -1;
	for (i = 0; i < parks_cap; i++)
		if (!parks[i].conn) {
			parks[i].conn = c;
			parks[i].req = req;
			parks[i].bin = bin;
			parks[i].retried = 0;
			parks[i].idlen = idlen < sizeof parks[i].id ? idlen : 0;
			if (parks[i].idlen)
				memcpy(parks[i].id, id, parks[i].idlen);
			parks[i].collen = collen < sizeof parks[i].col ? collen : 0;
			memcpy(parks[i].col, col, parks[i].collen);
			parks[i].key = malloc(klen);
			if (!parks[i].key) {
				parks[i].conn = NULL;
				return -1;
			}
			memcpy(parks[i].key, key, klen);
			parks[i].klen = klen;
			return 0;
		}
	return -1;
}

static void park_drop_conn(struct pc_conn *c)
{
	int i;

	if (!parks)
		return;
	for (i = 0; i < parks_cap; i++)
		if (parks[i].conn == c) {
			free(parks[i].key);
			parks[i].key = NULL;
			parks[i].conn = NULL;
		}
}

/* ---- lifecycle --------------------------------------------------------- */

static int conn_log_ok(void);          /* S70: defined with pc_conn_accept */

static void udp_free(struct pc_conn *c);   /* PS5, with the push stream */
static void push_unstage(struct pc_conn *c);   /* with the staged pushes */
static void pause_unlink(struct pc_conn *c);   /* with the paused publishers */
static int conn_pause(struct pc_conn *c);
/* this worker's HTTP connections, counted where they are made and
 * destroyed - the worker's wait reads it (pc_conn_http_open), not the
 * sweep's last answer, which was taken before the turn's accepts */
static __thread int http_open_here;
static void conn_destroy(struct pc_conn *c)
{
	/* PS1: its subscriptions go.  Only a connection the engine has seen
	 * takes the engine's lock: every close in the daemon used to. */
	if (c->ps_used)
		pc_pubsub_conn_closed(c);
	push_unstage(c);
	pause_unlink(c);
	if (c->udp)
		udp_free(c);
	if (c->http_only)
		http_open_here--;
	if (c->client_counted) {              /* S161 */
		PC_RESP_DEC(pc_clients_open);
		c->client_counted = 0;
	}
	/* S123: out of the open-connections gauge it was counted into - by
	 * the memo, which is where the count went in */
	if (c->open_counted) {
		if (c->resp_only)
			PC_RESP_DEC(pc_resp_open);
		else if (c->dialect == PC_D_BIN)
			PC_RESP_DEC(pc_nat_bin_open);
		else if (c->dialect == PC_D_TEXT)
			PC_RESP_DEC(pc_nat_text_open);
		else if (c->dialect == PC_D_RESP)
			PC_RESP_DEC(pc_nat_resp_open);
		c->open_counted = 0;
	}
	/* S70: a connection that settled a dialect said hello, so it says
	 * goodbye; one that never sent a byte said nothing and stays silent
	 * (a port probe makes no lines) - unless it died for a REASON, which
	 * is the case the filing is about: a handshake that never happened */
	if (!c->http_only && (c->dialect != PC_D_SNIFF || c->why) &&
	    conn_log_ok())
		LM_NOTICE("client %s disconnected (%s) after %us, %llu "
			"requests\n", pc_obs_conn_addr(c->obs),
			c->why ? c->why : "peer closed",
			(unsigned)(get_ticks() - c->born),
			pc_obs_conn_cmds(c->obs));
	pc_obs_conn_del(c->obs);
	enum_abort(c);
	park_drop_conn(c);
	if (c->prev)
		c->prev->next = c->next;
	else if (c->list)
		*c->list = c->next;
	if (c->next)
		c->next->prev = c->prev;
	close(c->fd);
	free(c->in);
	free(c->out);
	free(c->wire);
	free(c->raw);
	free(c);
}

void pc_conn_destroy_all(struct pc_conn **list)
{
	/* conn_destroy advances the head through c->list - aliasing the
	 * analyzer cannot track; each iteration reads a FRESH head */
	/* NOLINTNEXTLINE(clang-analyzer-unix.Malloc) */
	while (*list)
		conn_destroy(*list); /* NOLINT(clang-analyzer-unix.Malloc) */
}

/* RESP listener guards (task S33) live in daemon.c, which owns CFG.
 * These three counters are written by EVERY worker, so a plain ++ drops
 * increments under load - measured: 199 counted of 200 connections.
 * They are 32-bit and RELAXED-atomic: 32-bit atomics need no libatomic
 * on any target arch, which is what the no-cross-thread-64-bit-atomics
 * rule (par 4) is protecting.  They wrap at 4G events, which for a
 * connection counter is a stat artefact, not a correctness one. */
/* ---- the metrics door (S46) --------------------------------------------
 *
 * A deliberately minimal HTTP/1.0 responder, NOT a web server: read a
 * request head bounded by PC_HTTP_MAX, take the method and path from
 * the request line, ignore every header, answer, close.  No keep-alive,
 * no request body, no state carried between requests - which is what
 * keeps this small enough to reason about on a plaintext port.
 *
 * The alternative was libmicrohttpd (what OpenSIPS' httpd module uses).
 * Rejected on purpose: the daemon's dependency set is libc + pthreads +
 * libsodium and that is a published promise (README), the library wants
 * its own threads or its own event loop where every worker here already
 * owns an epoll, and a full server - POST, chunked bodies, digest auth,
 * TLS - is far more surface than two read-only GET routes need.  If the
 * status page ever grows forms or auth, take the dependency then rather
 * than growing this into a server.
 */
#define PC_HTTP_MAX 8192               /* a request head we will read */

/* memmem() is a GNU extension: it needs _GNU_SOURCE, and it compiled on
 * the Debian 13 build host while failing on Ubuntu 20.04 - a portability
 * split for a search over at most 8 KB.  A plain scan costs nothing at
 * this size and keeps the file free of feature-test macros. */
static char *find_bytes(char *hay, size_t hn, const char *needle, size_t nn)
{
	size_t i;

	if (nn > hn)
		return NULL;
	for (i = 0; i + nn <= hn; i++)
		if (hay[i] == needle[0] && !memcmp(hay + i, needle, nn))
			return hay + i;
	return NULL;
}
/* the exposition is bounded by the collection count, and 64 collections
 * of three series each plus the fixed block fits comfortably; the
 * renderer truncates rather than overruns if that ever stops being
 * true, and a truncated scrape is a visible bug, not a crash */
#define PC_METRICS_MAX ((size_t)256 * 1024)

/* S123: does the request head announce a body?  A Content-Length other
 * than 0, or a Transfer-Encoding, does; the one POST route takes none. */
static int http_has_body(const char *head, size_t hlen)
{
	const char *p = head, *end = head + hlen;

	while (p < end) {
		const char *eol = memchr(p, '\n', (size_t)(end - p));
		size_t ll = eol ? (size_t)(eol - p) : (size_t)(end - p);

		if (ll >= 15 && !strncasecmp(p, "Content-Length:", 15)) {
			const char *v = p + 15;

			while (v < p + ll && (*v == ' ' || *v == '\t'))
				v++;
			if (v < p + ll && *v != '0')
				return 1;
		} else if (ll >= 18 && !strncasecmp(p, "Transfer-Encoding:", 18)) {
			return 1;
		}
		if (!eol)
			break;
		p = eol + 1;
	}
	return 0;
}

/* Authorization: Bearer <token>, compared in constant time.  Header
 * only - never a query parameter, which would land the token in every
 * proxy log and browser history along the way. */
static int http_token_ok(const char *head, size_t hlen, const char *want)
{
	const char *p = head, *end = head + hlen, *v;
	size_t wn = strlen(want), n;
	unsigned char diff = 0;
	size_t i;

	while (p < end) {
		const char *eol = memchr(p, '\n', (size_t)(end - p));
		size_t ll = eol ? (size_t)(eol - p) : (size_t)(end - p);

		if (ll >= 21 && !strncasecmp(p, "Authorization:", 14)) {
			v = p + 14;
			while (v < p + ll && (*v == ' ' || *v == '\t'))
				v++;
			if ((size_t)(p + ll - v) > 7 &&
			    !strncasecmp(v, "Bearer ", 7)) {
				v += 7;
				n = (size_t)(p + ll - v);
				while (n && (v[n-1] == '\r' || v[n-1] == ' '))
					n--;
				if (n != wn)
					return 0;
				for (i = 0; i < n; i++)
					diff |= (unsigned char)(v[i] ^ want[i]);
				return diff == 0;
			}
		}
		if (!eol)
			break;
		p = eol + 1;
	}
	return 0;
}

/* S205: the page's Content-Security-Policy, built once from the page
 * itself.  Every string the page renders comes from the cache - keys,
 * values, client names, command names - and the operator was hacked
 * through exactly that once: a key carrying a script import, a
 * dashboard that rendered it, a browser that fetched the rest from a
 * remote site.  esc() at every sink is the first layer; this is the
 * second, for the sink someone forgets.  script-src is the SHA-256 of
 * the page's one inline <script>, so an injected script - inline or
 * remote - does not run at all; connect-src and img-src keep the page
 * from talking to anywhere but this node; default-src 'none' closes
 * the rest.  Computed at first use from pc_status_page, so it can never
 * drift from the script it names. */
static char page_csp[512];
static pthread_once_t page_csp_once = PTHREAD_ONCE_INIT;

static void page_csp_build(void)
{
	const char *pg = pc_status_page;
	size_t pn = pc_status_page_len;
	char *a = find_bytes((char *)pg, pn, "<script>", 8);
	char *b = a ? find_bytes(a, pn - (size_t)(a - pg), "</script>", 9) : NULL;
	unsigned char h[32];
	char b64[64];

	if (!a || !b) {
		/* no script: nothing to allow */
		snprintf(page_csp, sizeof page_csp,
			"Content-Security-Policy: default-src 'none'; style-src "
			"'unsafe-inline'; img-src data:; connect-src 'self'; "
			"base-uri 'none'; form-action 'none'; frame-ancestors 'none'\r\n");
		return;
	}
	a += 8;
	crypto_hash_sha256(h, (const unsigned char *)a, (size_t)(b - a));
	sodium_bin2base64(b64, sizeof b64, h, sizeof h,
		sodium_base64_VARIANT_ORIGINAL);
	snprintf(page_csp, sizeof page_csp,
		"Content-Security-Policy: default-src 'none'; script-src "
		"'sha256-%s'; style-src 'unsafe-inline'; img-src data:; "
		"connect-src 'self'; base-uri 'none'; form-action 'none'; "
		"frame-ancestors 'none'\r\n", b64);
}

static const char *pc_page_csp(void)
{
	pthread_once(&page_csp_once, page_csp_build);
	return page_csp;
}

/* @extra: further header lines, each CRLF-terminated, or NULL.  Every
 * response carries the three hardening headers: no MIME sniffing, no
 * referrer, no framing. */
static int http_send_ex(struct pc_conn *c, const char *status,
		const char *ctype, const char *extra, const char *body, size_t blen)
{
	char head[1024];
	int n = snprintf(head, sizeof head,
		"HTTP/1.0 %s\r\n"
		"Content-Type: %s\r\n"
		"Content-Length: %zu\r\n"
		"X-Content-Type-Options: nosniff\r\n"
		"Referrer-Policy: no-referrer\r\n"
		"X-Frame-Options: DENY\r\n"
		"%s"
		"Connection: close\r\n"
		"\r\n", status, ctype, blen, extra ? extra : "");

	if (n < 0 || (size_t)n >= sizeof head)
		return -1;
	if (out_append(c, head, (size_t)n) != 0)
		return -1;
	if (blen && out_append(c, body, blen) != 0)
		return -1;
	/* the header says Connection: close, so CLOSE - without this the
	 * socket stays open, every client that reads to EOF (curl and
	 * Prometheus both do) hangs until its own timeout, and the
	 * response looks lost rather than late.  close_after fires once
	 * the staged bytes have actually reached the wire. */
	c->close_after = 1;
	return 0;
}

static int http_send(struct pc_conn *c, const char *status,
		const char *ctype, const char *body, size_t blen)
{
	return http_send_ex(c, status, ctype, NULL, body, blen);
}

/* one request from the buffer at @off.  Returns bytes consumed, 0 when
 * the head is incomplete, or -1 to drop the connection. */
static int http_one(struct pc_conn *c, size_t off)
{
	char *base = c->in + off;
	size_t avail = c->in_len - off;
	char *end, *sp1, *sp2, *body;
	size_t hlen, blen;
	char path[256];

	/* the head ends at a blank line; refuse anything absurd rather
	 * than buffer it - this port answers scrapes, not uploads */
	end = find_bytes(base, avail, "\r\n\r\n", 4);
	hlen = end ? (size_t)(end - base) + 4 : 0;
	if (!end) {
		char *lf = find_bytes(base, avail, "\n\n", 2);  /* bare LF too */

		if (lf) {
			end = lf;
			hlen = (size_t)(lf - base) + 2;
		}
	}
	if (!end) {
		if (avail >= PC_HTTP_MAX)
			return -1;             /* no head in 8 KB: not a client */
		return 0;                      /* incomplete; wait for more */
	}

	/* "GET /path HTTP/1.x" - and, S123, "POST /reset-stats", the one
	 * mutating route, body-less by contract; anything else is refused
	 * unread */
	int post = 0;

	sp1 = memchr(base, ' ', hlen);
	if (sp1 && (size_t)(sp1 - base) == 4 && !memcmp(base, "POST", 4)) {
		post = 1;
	} else if (!sp1 || (size_t)(sp1 - base) != 3 ||
	        memcmp(base, "GET", 3)) {
		http_send(c, "405 Method Not Allowed", "text/plain",
			"only GET, and POST /reset-stats, are served here\n", 48);
		return (int)hlen;
	}
	sp2 = memchr(sp1 + 1, ' ', hlen - (size_t)(sp1 + 1 - base));
	if (!sp2 || (size_t)(sp2 - sp1 - 1) >= sizeof path) {
		http_send(c, "414 URI Too Long", "text/plain", "\n", 1);
		return (int)hlen;
	}
	memcpy(path, sp1 + 1, (size_t)(sp2 - sp1 - 1));
	path[sp2 - sp1 - 1] = 0;
	{
		char *q = strchr(path, '?');   /* ?collect[]= and friends */

		if (q)
			*q = 0;
	}

	/* S37: the shared token, when one is configured, guards EVERY
	 * route - /metrics discloses the fleet's shape as surely as
	 * /members does, so exempting it would be a hole with a tidy
	 * name.  Constant-time compare: it crosses the wire in the
	 * clear, which is no reason to leak its prefix by timing too. */
	{
		const char *want = pc_http_token();

		if (want && !http_token_ok(base, hlen, want)) {
			http_send(c, "401 Unauthorized", "text/plain",
				"a valid Authorization: Bearer token is "
				"required\n", 47);
			return (int)hlen;
		}
	}

	if (post) {
		char rb[64];
		int rn;

		if (strcmp(path, "/reset-stats")) {
			http_send(c, "404 Not Found", "text/plain",
				"POST serves /reset-stats only\n", 30);
			return (int)hlen;
		}
		if (http_has_body(base, hlen)) {
			http_send(c, "400 Bad Request", "text/plain",
				"/reset-stats takes no body\n", 27);
			return (int)hlen;
		}
		rn = snprintf(rb, sizeof rb, "{\"reset\":true,\"at\":%lld}\n",
			pc_stats_reset());
		http_send(c, "200 OK", "application/json", rb, (size_t)rn);
		return (int)hlen;
	}
	if (!strcmp(path, "/")) {
		/* every node serves its own view - see statuspage.h for why
		 * this is emphatically not a master-only page */
		http_send_ex(c, "200 OK", "text/html; charset=utf-8",
			pc_page_csp(), pc_status_page, pc_status_page_len);
	} else if (!strcmp(path, "/members")) {
		struct pc_jw w;

		pc_jw_init(&w, NULL, 0);
		if (pc_jw_init_heap(&w, (size_t)16 * 1024) != 0) {
			http_send(c, "503 Service Unavailable", "text/plain",
				"out of memory\n", 14);
			return (int)hlen;
		}
		pc_members_json(&w);
		http_send(c, "200 OK", "application/json", w.buf, w.len);
		pc_jw_free(&w);
	} else if (!strcmp(path, "/clients")) {
		/* S160: this node's connections as JSON - the walk CLIENT LIST
		 * makes, with the door, dialect, wire, pending bytes and
		 * subscriptions the RESP line has no field for.  Read-only, and
		 * behind the same allow-list and token as its four siblings. */
		struct pc_jw w;
		pc_jw_init(&w, NULL, 0);
		if (pc_jw_init_heap(&w, (size_t)128 * 1024) != 0) {
			http_send(c, "503 Service Unavailable", "text/plain",
				"out of memory\n", 14);
			return (int)hlen;
		}
		pc_obs_clients_json(&w, get_ticks(), 200);
		http_send(c, "200 OK", "application/json", w.buf, w.len);
		pc_jw_free(&w);
	} else if (!strcmp(path, "/stats")) {
		/* S67: the same body the `stats` verb returns.  The
		 * dashboard needs collections, both doors' connection
		 * counts and the cluster counters, none of which /members
		 * carries - and a page that cannot see them is how the
		 * HEADROOM column stayed empty and the native clients
		 * stayed invisible. */
		struct pc_jw w;

		pc_jw_init(&w, NULL, 0);
		if (pc_jw_init_heap(&w, (size_t)64 * 1024) != 0) {
			http_send(c, "503 Service Unavailable", "text/plain",
				"out of memory\n", 14);
			return (int)hlen;
		}
		pc_stats_json(&w, NULL);
		http_send(c, "200 OK", "application/json", w.buf, w.len);
		pc_jw_free(&w);
	} else if (!strcmp(path, "/metrics")) {
		body = malloc(PC_METRICS_MAX);
		if (!body) {
			http_send(c, "503 Service Unavailable", "text/plain",
				"out of memory\n", 14);
			return (int)hlen;
		}
		blen = pc_metrics_render(body, PC_METRICS_MAX);
		/* the exposition's own content type, so Prometheus and a
		 * human with curl both get what they expect */
		http_send(c, "200 OK",
			"text/plain; version=0.0.4; charset=utf-8", body, blen);
		free(body);
	} else if (!strcmp(path, "/health")) {
		/* liveness only: this answers whenever the daemon can run a
		 * worker.  Readiness - whether the node has joined and is
		 * serving - is pc_node_state()'s business and rides
		 * /metrics, because conflating them makes an orchestrator
		 * restart a node that is merely still syncing. */
		http_send(c, "200 OK", "text/plain", "ok\n", 3);
	} else {
		http_send(c, "404 Not Found", "text/plain",
			"try /stats, /members, /metrics or /health\n", 42);
	}
	return (int)hlen;
}

/* S46: a request head that never arrives must not hold a slot for
 * ever - the slowloris shape, and cheap to do here because an HTTP
 * connection is one-shot by construction: it is born, it asks once,
 * it is closed.  Native and RESP connections are deliberately NOT
 * swept: those are long-lived by design (a SIP worker holds one for
 * its process lifetime), so an idle timeout there would be a bug, not
 * a defence.
 *
 * Returns the number of HTTP connections still open, so the worker
 * knows whether it must keep waking to sweep at all. */
int pc_conn_http_open(void)
{
	return http_open_here;
}

int pc_conn_sweep_http(struct pc_conn **list, int timeout_s)
{
	struct pc_conn *c, *next;
	unsigned int now = get_ticks();
	int open = 0;

	for (c = *list; c; c = next) {
		next = c->next;
		if (!c->http_only)
			continue;
		if (timeout_s > 0 && now - c->born >= (unsigned int)timeout_s) {
			LM_DBG("metrics connection idle %us without a complete "
				"request - closing\n", now - c->born);
			conn_destroy(c);
			continue;
		}
		open++;
	}
	return open;
}

unsigned int pc_resp_conns, pc_resp_rejected, pc_resp_authfail;
unsigned int pc_resp_slots_hits, pc_resp_slots_builds;          /* S75 */
unsigned int pc_resp_reqs, pc_nat_conns, pc_nat_bin_conns, pc_nat_text_conns,
	pc_nat_resp_conns, pc_nat_bin_reqs, pc_nat_text_reqs, pc_nat_resp_reqs;
unsigned int pc_resp_open, pc_nat_bin_open, pc_nat_text_open, pc_nat_resp_open;   /* S123 */
int pc_max_clients;                             /* S161: 0 = no limit */
int pc_keepalive_s;                             /* PS2: 0 = off */
unsigned int pc_clients_open;
unsigned long long pc_clients_refused;
static int clients_full_logged;                 /* one line per transition */
struct pc_door_base pc_door_base;

/* S123: the running totals start again from here - see daemon.h */
void pc_doors_reset(void)
{
#define SNAP(n) __atomic_store_n(&pc_door_base.n, PC_RESP_READ(pc_##n), __ATOMIC_RELAXED)
	SNAP(resp_conns); SNAP(resp_rejected); SNAP(resp_authfail);
	SNAP(resp_reqs); SNAP(resp_slots_hits); SNAP(resp_slots_builds);
	SNAP(nat_conns); SNAP(nat_bin_conns); SNAP(nat_text_conns);
	SNAP(nat_resp_conns); SNAP(nat_bin_reqs); SNAP(nat_text_reqs);
	SNAP(nat_resp_reqs);
#undef SNAP
}

/* S70: say who connected and who left.  Per connection, not per request,
 * so the volume is bounded by clients - except a reconnect loop, which is
 * exactly the fault this exists to make visible and would make it
 * unbounded.  So: up to CONN_LOG_BURST lines per CONN_LOG_WIN_S seconds,
 * then one line with the count.  The counters are shared across workers
 * and deliberately unlocked (a torn count mis-limits a log line, nothing
 * else - the CP-06 stance).  HTTP connections are never logged here: the
 * page polls every second and a poll is not a client. */
#define CONN_LOG_BURST 20
#define CONN_LOG_WIN_S 10
static unsigned int conn_log_win, conn_log_n, conn_log_dropped;

static int conn_log_ok(void)
{
	unsigned int now = get_ticks();

	if (now - conn_log_win >= CONN_LOG_WIN_S) {
		if (conn_log_dropped)
			LM_NOTICE("clients: %u more connection events in the "
				"last %u s not logged\n", conn_log_dropped,
				CONN_LOG_WIN_S);
		conn_log_win = now;
		conn_log_n = 0;
		conn_log_dropped = 0;
	}
	if (conn_log_n < CONN_LOG_BURST) {
		conn_log_n++;
		return 1;
	}
	conn_log_dropped++;
	return 0;
}

static void conn_log_open(struct pc_conn *c, const char *dialect)
{
	pc_obs_conn_door(c->obs, NULL, c->resp_only ? "resp" : dialect,
		c->encrypted);                                   /* S160 */
	if (c->http_only || !conn_log_ok())
		return;
	LM_NOTICE("client %s connected (%s, %s)\n", pc_obs_conn_addr(c->obs),
		dialect, c->encrypted ? "encrypted" : "plaintext");
}

/* S65: read a reply's outcome off its own bytes, so the query line can
 * say hit / miss / err without threading a status through every verb.
 * JSON replies carry "found":true/false on the verbs that can miss;
 * RESP says $-1 / *-1 for a miss and leads an error with '-'. */
static int reply_has(const char *b, size_t n, const char *needle)
{
	size_t k = strlen(needle), i;

	if (n < k)
		return 0;
	for (i = 0; i + k <= n; i++)
		if (b[i] == needle[0] && memcmp(b + i, needle, k) == 0)
			return 1;
	return 0;
}

void pc_conn_accept(int ep, int lfd, int plaintext_ok, int kind,
		const struct pc_psk_ctx *psk, struct pc_conn **list)
{
	struct epoll_event ev;
	struct pc_conn *c;
	struct sockaddr_in peer;
	socklen_t plen;
	int fd, one = 1, refused;

	for (;;) {
		plen = sizeof peer;
		/* the peer address used to be collected only for RESP
		 * listeners (their allow-list needs it); CLIENT LIST (S53)
		 * wants it for every connection */
		fd = accept(lfd, (struct sockaddr *)&peer, &plen);
		if (fd < 0)
			return;
		if (kind == PC_LK_RESP && !pc_resp_peer_allowed(&peer, plen)) {
			/* refused BEFORE a byte is read: a RESP listener has
			 * no handshake, so the allow-list is the door */
			PC_RESP_BUMP(pc_resp_rejected);
			close(fd);
			continue;
		}
		if (kind == PC_LK_HTTP &&
		    !pc_http_peer_allowed(&peer, plen)) {
			/* S66: say so on the wire.  A silent close renders in a
			 * browser as ERR_EMPTY_RESPONSE - indistinguishable from
			 * a dead daemon, a wrong port or a firewall drop - and it
			 * cost a bring-up an hour of "the dashboard did not
			 * start" while the log said exactly what was happening.
			 * The trade is a thin secret: the open port already says a
			 * listener is here.  The body names the mechanism and
			 * nothing else - no version, no addresses.  Drain what the
			 * client sent before closing, or the kernel answers the
			 * unread request with a RST and the 403 never arrives. */
			static const char body[] =
				"perfcached: this address is not in http_allow\n";
			char deny[256], sink[512];
			struct pollfd pf = { fd, POLLIN, 0 };
			int dn = snprintf(deny, sizeof deny,
				"HTTP/1.0 403 Forbidden\r\nContent-Type: text/plain\r\n"
				"Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
				sizeof body - 1, body);

			(void)send(fd, deny, (size_t)dn, MSG_NOSIGNAL | MSG_DONTWAIT);
			shutdown(fd, SHUT_WR);
			while (poll(&pf, 1, 200) > 0 &&
			        recv(fd, sink, sizeof sink, MSG_DONTWAIT) > 0)
				;
			close(fd);
			continue;
		}
		fcntl(fd, F_SETFL, O_NONBLOCK);
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
		/* PS2: the daemon never idle-sweeps a data connection (S46 says
		 * why), so a subscriber whose host died silently would sit here
		 * looking live until a message went out and TCP gave up on it
		 * a quarter of an hour later.  The kernel finds it instead:
		 * probes after keepalive_s idle, a third of that apart, three
		 * misses and the socket errors.  redis ships tcp-keepalive 300
		 * beside timeout 0, the same shape. */
		if (pc_keepalive_s > 0 && kind != PC_LK_HTTP) {
			int ka = pc_keepalive_s, iv = ka / 3 > 0 ? ka / 3 : 1, cnt = 3;

			setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
			setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &ka, sizeof ka);
			setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &iv, sizeof iv);
			setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
		}
		/* A pipelined batch of replies is only small for WRITES.
		 * 64 SETs answer in ~320 bytes of "+OK"; 64 GETs of a
		 * 200-byte value answer in ~13 KB - and the kernel's default
		 * send buffer is 16 KB (net.ipv4.tcp_wmem), so reads run into
		 * it and writes never do.  Measured across a bridge: raising
		 * it lifted GET 515,629 -> 719,655/s with SET unchanged, and
		 * on a 2-vCPU host it moved GET from x0.89 of redis to x0.97
		 * while SET went x1.08 -> x1.13.
		 *
		 * Asking for 1 MB, not demanding it: the kernel silently
		 * clamps to net.core.wmem_max, so this is "use what you are
		 * allowed" rather than a requirement, and a host that has
		 * tuned wmem_max upward gets the benefit without configuring
		 * anything here.  The failure mode of asking is a smaller
		 * buffer, which is what we had anyway.
		 */
		{
			int sndbuf = 1 << 20;

			setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf,
				sizeof sndbuf);
		}

		/* S161: THE LIMIT.  Counted across the data doors; HTTP stays
		 * outside it so /stats and the page answer at the limit (the
		 * descriptor reserve covers them).  A RESP client gets Redis's
		 * exact text on the spot, which every Redis client already
		 * maps; a native client is answered in its own dialect at its
		 * first request, since the dialect is not known until it
		 * speaks.  One log line into the limit and one out of it. */
		refused = 0;
		if (kind != PC_LK_HTTP && pc_max_clients > 0) {
			if ((int)PC_RESP_READ(pc_clients_open) >= pc_max_clients) {
				__atomic_fetch_add(&pc_clients_refused, 1,
					__ATOMIC_RELAXED);
				if (!__atomic_exchange_n(&clients_full_logged, 1,
				        __ATOMIC_RELAXED))
					LM_WARN("clients: max_clients %d reached - "
						"refusing new connections until one "
						"closes\n", pc_max_clients);
				if (kind == PC_LK_RESP) {
					static const char full[] =
						"-ERR max number of clients reached\r\n";
					if (write(fd, full, sizeof full - 1) < 0)
						{ /* the close is the message */ }
					close(fd);
					continue;
				}
				refused = 1;
			} else if (__atomic_load_n(&clients_full_logged,
			        __ATOMIC_RELAXED) &&
			        __atomic_exchange_n(&clients_full_logged, 0,
			                __ATOMIC_RELAXED)) {
				LM_NOTICE("clients: below max_clients %d again\n",
					pc_max_clients);
			}
		}
		c = calloc(1, sizeof *c);
		if (!c) {
			close(fd);
			continue;
		}
		c->refused = refused;
		if (kind != PC_LK_HTTP && !refused) {
			PC_RESP_BUMP(pc_clients_open);
			c->client_counted = 1;
		}
		c->obs = pc_obs_conn_add((struct sockaddr *)&peer, plen,
			fd, kind == PC_LK_RESP);
		pc_obs_conn_door(c->obs, kind == PC_LK_RESP ? "resp"
			: kind == PC_LK_HTTP ? "http" : "native",
			kind == PC_LK_RESP ? "resp" : "?", !plaintext_ok);   /* S160 */
		c->fd = fd;
		c->ep = ep;
		c->list = list;
		c->resp_col[0] = '0';          /* RESP db 0 = collection "0" */
		c->resp_collen = 1;
		c->resp_only = kind == PC_LK_RESP;
		c->http_only = kind == PC_LK_HTTP;
		if (c->http_only)
			http_open_here++;
		c->born = get_ticks();
		if (kind == PC_LK_RESP) {
			c->dialect = PC_D_RESP;   /* pinned: no sniff, ever */
			/* unauthenticated until AUTH when a password is set */
			c->resp_authed = !pc_resp_password_set();
			PC_RESP_BUMP(pc_resp_conns);
			PC_RESP_BUMP(pc_resp_open);            /* S123 */
			c->open_counted = 1;
		} else {
			c->resp_authed = 1;
			if (kind == PC_LK_NATIVE)
				PC_RESP_BUMP(pc_nat_conns);   /* S76 */
		}
		if (!plaintext_ok) {
			c->encrypted = 1;
			c->psk = psk;              /* handshake starts on first bytes */
		}
		if (kind == PC_LK_RESP)
			conn_log_open(c, "resp door");   /* S70: pinned, known now */
		c->next = *list;
		if (c->next)
			c->next->prev = c;
		*list = c;

		ev.events = EPOLLIN;
		ev.data.ptr = c;
		epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
	}
}

/* ---- output: stage plaintext, then flush (encrypting if needed) -------- */

static int encrypt_pending(struct pc_conn *c)
{
	uint8_t rec[2 + PC_NOISE_MAXPT + PC_NOISE_TAGLEN];
	size_t off = 0;

	while (off < c->out_len) {
		size_t chunk = c->out_len - off;
		int cl;

		if (chunk > PC_NOISE_MAXPT)
			chunk = PC_NOISE_MAXPT;
		cl = pc_transport_encrypt(&c->send_cs,
			(const uint8_t *)c->out + off, chunk, rec + 2);
		if (cl < 0)
			return -1;
		put16(rec, (uint16_t)cl);
		if (wire_append(c, (char *)rec, 2 + (size_t)cl) < 0)
			return -1;
		off += chunk;
	}
	c->out_len = 0;
	return 0;
}

static int conn_flush(struct pc_conn *c)
{
	struct epoll_event ev;
	ssize_t n;

	/* move staged plaintext into the socket buffer */
	if (c->out_len) {
		if (c->encrypted) {
			if (!c->established) {
				c->out_len = 0;        /* no app data before handshake */
			} else if (encrypt_pending(c) < 0) {
				return -1;
			}
		} else {
			if (wire_append(c, c->out, c->out_len) < 0)
				return -1;
			c->out_len = 0;
		}
	}

	while (c->wire_off < c->wire_len) {
		n = write(c->fd, c->wire + c->wire_off, c->wire_len - c->wire_off);
		if (n > 0) {
			c->wire_off += (size_t)n;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			if (!c->want_write) {
				ev.events = (c->ps_paused ? 0 : EPOLLIN) | EPOLLOUT;
				ev.data.ptr = c;
				epoll_ctl(c->ep, EPOLL_CTL_MOD, c->fd, &ev);
				c->want_write = 1;
			}
			/* S160: what the socket would not take - the figure that
			 * predicts a slow-consumer kill at PC_MAX_OUTQ */
			c->obs_pending = c->out_len + (c->wire_len - c->wire_off);
			pc_obs_conn_pending(c->obs, c->obs_pending);
			return 0;
		}
		return -1;
	}
	c->wire_off = c->wire_len = 0;
	if (c->obs_pending) {
		c->obs_pending = 0;
		pc_obs_conn_pending(c->obs, 0);
	}
	if (c->want_write) {
		ev.events = c->ps_paused ? 0 : EPOLLIN;
		ev.data.ptr = c;
		epoll_ctl(c->ep, EPOLL_CTL_MOD, c->fd, &ev);
		c->want_write = 0;
	}
	return 0;
}

/* ---- text dialect ------------------------------------------------------ */

#define JW_REPLY_CAP (PC_MAX_REQ + 4096)

/* Per-thread reply scratch.  These were malloc'd/freed on EVERY
 * request - invisible under glibc's caching allocator, catastrophic
 * under a non-caching allocator, where each 1MB allocation is an
 * mmap+munmap+fault cycle: a container bench measured a ~50x
 * throughput loss before the scratch became thread-owned, and the
 * fix is worth keeping regardless.  Workers live as long as
 * the daemon; the buffers are never freed. */
static __thread char *jw_scratch[2];

static char *scratch_buf(int i)
{
	if (!jw_scratch[i])
		jw_scratch[i] = malloc(JW_REPLY_CAP);
	return jw_scratch[i];
}

static int text_error(struct pc_conn *c, const char *idraw, size_t idlen,
		int code, const char *msg)
{
	char buf[512];
	struct pc_jw w;

	pc_jw_init(&w, buf, sizeof buf);
	pc_jw_lit(&w, "{\"jsonrpc\":\"2.0\",\"id\":");
	if (idraw)
		pc_jw_raw(&w, idraw, idlen);
	else
		pc_jw_lit(&w, "null");
	pc_jw_lit(&w, ",\"error\":{\"code\":");
	pc_jw_i64(&w, code);
	pc_jw_lit(&w, ",\"message\":");
	pc_jw_str(&w, msg, strlen(msg));
	pc_jw_lit(&w, "}}\n");
	if (w.overflow)
		return -1;
	return out_append(c, w.buf, w.len);
}

/* S159: the clock carried from the end of one request to the start of
 * the next inside one drain of a connection's buffer - a pipelined
 * batch pays one clock read per request instead of two (the second
 * read measured 2% of a 1 us path).  Zeroed per drain, so the first
 * request of a batch reads a fresh clock; the RESP wrapper refreshes
 * it too, so a mixed-dialect connection never charges a RESP command
 * to the JSON request behind it. */
static __thread unsigned long long obs_carry;

static int text_line(struct pc_conn *c, char *line, size_t n)
{
	struct pc_jtok *toks;
	const char *idraw = NULL, *errmsg = "error";
	size_t idlen = 0;
	char *resbuf = NULL, *reply = NULL;
	int ntok, maxtok, tid, tm, tp, rc, verr, ret = -1;
	struct pc_jw rw, w;

	/* tokens sized by the line: every token needs >= 2 source bytes
	 * (batch verbs - mget/mset - carry hundreds of them).  The array
	 * is per-thread and grow-only: a malloc/free per line kept musl's
	 * allocator on the mmap lock (same profile as the jw scratch). */
	static __thread struct pc_jtok *toks_scratch;
	static __thread int toks_cap;

	maxtok = (int)(n / 2) + 8;
	if (maxtok > 262144)
		maxtok = 262144;
	if (maxtok > toks_cap) {
		struct pc_jtok *nt = realloc(toks_scratch,
			(size_t)maxtok * sizeof *nt);

		if (!nt)
			return text_error(c, NULL, 0, -32603, "out of memory");
		toks_scratch = nt;
		toks_cap = maxtok;
	}
	toks = toks_scratch;
	/* unreachable while the grow above pairs cap with scratch - but
	 * the pairing is an invariant, and a guard outlives an argument
	 * (the analyzer models statics as arbitrary entry state) */
	if (!toks)
		return text_error(c, NULL, 0, -32603, "out of memory");

	ntok = pc_json_parse(line, n, toks, maxtok);
	if (ntok < 1 || toks[0].type != PC_J_OBJ)
		return text_error(c, NULL, 0, -32700, "parse error");

	tid = pc_json_get(line, toks, ntok, 0, "id");
	if (tid >= 0) {
		idraw = line + (toks[tid].type == PC_J_STR ?
			toks[tid].start - 1 : toks[tid].start);
		idlen = (size_t)(toks[tid].end - toks[tid].start) +
			(toks[tid].type == PC_J_STR ? 2 : 0);
	}
	tm = pc_json_get(line, toks, ntok, 0, "method");
	if (tm < 0 || toks[tm].type != PC_J_STR)
		return text_error(c, idraw, idlen, -32600, "invalid request");
	tp = pc_json_get(line, toks, ntok, 0, "params");
	if (tp >= 0 && toks[tp].type != PC_J_OBJ)
		tp = -1;

	resbuf = scratch_buf(0);
	reply = scratch_buf(1);
	if (!resbuf || !reply)
		goto out;

	pc_jw_init(&rw, resbuf, JW_REPLY_CAP);
	{
		unsigned int park_req = 0;

		/* S159: timed and counted like a RESP command - as json.<method>,
		 * into the same rows and the same slow log; a clock read is the
		 * whole cost on the fast path, the token lookups below run only
		 * for a slow-log or query-log entry */
		unsigned long long qt0 = obs_carry ? obs_carry : pc_obs_usec_now(), qdt;
		int qlog = pc_obs_qlog_on();

		/* S111: the connection's request count and last command, on
		 * every dialect - it was touched on the RESP dispatch only, so
		 * a JSON or binary link closed with "0 requests" whatever it did */
		pc_obs_conn_touch(c->obs, line + toks[tm].start,
			(size_t)(toks[tm].end - toks[tm].start), get_ticks());
		ev_origin(c, "json");                       /* S153 */
		verr = pc_verb_text(line, toks, ntok, tm, tp, &rw, &errmsg,
			&park_req, &c->privileged, c);
		if (verr == -32601)            /* S165: a method we do not implement */
			pc_obs_unknown(CLUNK_JSON, line + toks[tm].start,
				(size_t)(toks[tm].end - toks[tm].start), NULL, 0,
				c->obs);
		obs_carry = pc_obs_usec_now();
		qdt = obs_carry - qt0;
		{
			/* S189: "json:" with a COLON, not a dot.  A Redis
			 * command name may contain a dot - JSON.GET, TS.ADD,
			 * FT.SEARCH are real commands - so a RESP client
			 * sending JSON.GET landed on the same row as the
			 * native JSON door's `get` and nothing distinguished
			 * them.  A colon cannot appear in a RESP command
			 * name, so the dialect prefix is unambiguous. */
			char cn[32] = "json:";    /* length-counted, never a C string */
			size_t ml = (size_t)(toks[tm].end - toks[tm].start);

			if (ml > sizeof cn - 6)
				ml = sizeof cn - 6;
			/* the row slot of the last method seen on this worker -
			 * a door serves one or two methods at a time, so the
			 * compare hits and the hash is paid on a change only */
			static __thread char lm[32];
			static __thread size_t lml;
			static __thread void *lslot;

			memcpy(cn + 5, line + toks[tm].start, ml);
			if (lml != 5 + ml || memcmp(lm, cn, 5 + ml)) {
				lml = 5 + ml;
				memcpy(lm, cn, lml);
				lslot = pc_obs_cmd_slot(cn, lml);
			}
			pc_obs_cmd_at(lslot, qdt);
			if (pc_obs_slow_would(qdt)) {
				int sc = tp >= 0 ? pc_json_get(line, toks, ntok, tp, "col") : -1;
				int sk = tp >= 0 ? pc_json_get(line, toks, ntok, tp, "key") : -1;
				char *av[3];
				size_t al[3];
				int na = 1;

				av[0] = cn;
				al[0] = 5 + ml;
				if (sc >= 0) {
					av[na] = line + toks[sc].start;
					al[na++] = (size_t)(toks[sc].end - toks[sc].start);
				}
				if (sk >= 0) {
					av[na] = line + toks[sk].start;
					al[na++] = (size_t)(toks[sk].end - toks[sk].start);
				}
				pc_obs_slow(av, al, na, qdt, c->obs);
			}
		}
		if (qlog) {
			int qc = tp >= 0 ? pc_json_get(line, toks, ntok, tp, "col") : -1;
			int qk = tp >= 0 ? pc_json_get(line, toks, ntok, tp, "key") : -1;

			pc_obs_qlog("json", c->obs, line + toks[tm].start,
				(size_t)(toks[tm].end - toks[tm].start),
				qc >= 0 ? line + toks[qc].start : NULL,
				qc >= 0 ? (size_t)(toks[qc].end - toks[qc].start) : 0,
				qk >= 0 ? line + toks[qk].start : NULL,
				qk >= 0 ? (size_t)(toks[qk].end - toks[qk].start) : 0,
				verr < 0 ? "err"
				: reply_has(rw.buf, rw.len, "\"found\":false") ? "miss"
				: reply_has(rw.buf, rw.len, "\"found\":true") ? "hit"
				: "ok",
				qdt);
		}
		if (verr == 0 && park_req) {
			/* deferred to a cluster pull: park and reply later */
			int tcol = pc_json_get(line, toks, ntok, tp, "col");
			int tkey = pc_json_get(line, toks, ntok, tp, "key");
			char kbuf[4096];
			int kl = pc_json_unescape(line, &toks[tkey], kbuf,
				sizeof kbuf);

			if (tcol >= 0 && kl >= 0 &&
			        park_add(c, park_req, idraw, idlen,
			            line + toks[tcol].start,
			            (size_t)(toks[tcol].end - toks[tcol].start),
			            kbuf, (size_t)kl, 0) == 0) {
				ret = 0;
				goto out;
			}
			/* could not park: the verb pre-wrote its OWN
			 * failure shape into rw - a set answers
			 * stored:false, never a pull-miss (the shape bug
			 * this comment used to be) */
			if (!rw.len)
				pc_jw_lit(&rw, "{\"error\":\"busy\"}");
		}
	}
	if (verr != 0 || rw.overflow) {
		ret = text_error(c, idraw, idlen,
			rw.overflow ? -32603 : verr,
			rw.overflow ? "reply too large" : errmsg);
		pc_jw_free(&rw);               /* no-op unless a verb grew one */
		goto out;
	}

	/* S112: a verb may have swapped a heap buffer into rw (dump: a
	 * chunk is bounded by buckets, not bytes) - then the envelope
	 * cannot be built in the fixed scratch either */
	if (rw.owned) {
		memset(&w, 0, sizeof w);
		if (pc_jw_init_heap(&w, rw.len + 128) != 0) {
			pc_jw_free(&rw);
			ret = text_error(c, idraw, idlen, -32603, "out of memory");
			goto out;
		}
	} else
		pc_jw_init(&w, reply, JW_REPLY_CAP);
	pc_jw_lit(&w, "{\"jsonrpc\":\"2.0\",\"id\":");
	if (idraw)
		pc_jw_raw(&w, idraw, idlen);
	else
		pc_jw_lit(&w, "null");
	pc_jw_lit(&w, ",\"result\":");
	pc_jw_raw(&w, rw.buf, rw.len);
	pc_jw_lit(&w, "}\n");
	pc_jw_free(&rw);
	if (w.overflow) {
		pc_jw_free(&w);
		ret = text_error(c, idraw, idlen, -32603, "reply too large");
		goto out;
	}
	rc = out_append(c, w.buf, w.len);
	pc_jw_free(&w);                    /* no-op unless it is the heap one */
	ret = rc;
out:
	return ret;
}

/* one line starting at in+off; returns bytes consumed (0 = need more
 * bytes), -1 fatal.  The caller compacts the buffer ONCE per drain -
 * a per-message memmove was 13% of a pipelined run in the musl
 * profile (quadratic in pipeline depth). */
static int text_one(struct pc_conn *c, size_t off)
{
	char *base = c->in + off;
	size_t avail = c->in_len - off;
	char *nl = memchr(base, '\n', avail);
	size_t linelen;

	if (!nl)
		return 0;
	linelen = (size_t)(nl - base);
	if (linelen && text_line(c, base, linelen) < 0)
		return -1;
	return (int)(linelen + 1);
}

/* ---- binary dialect ---------------------------------------------------- */

static int bin_reply(struct pc_conn *c, uint64_t id, int flags,
		const char *payload, size_t n)
{
	char hdr[PC_BIN_HDR];

	hdr[0] = (char)PC_BIN_MAGIC;
	hdr[1] = PC_BIN_VER;
	hdr[2] = PC_BIN_RSP;
	hdr[3] = (char)flags;
	put32(hdr + 4, (uint32_t)n);
	put64(hdr + 8, id);
	if (out_append(c, hdr, sizeof hdr) < 0)
		return -1;
	return out_append(c, payload, n);
}

/* one frame starting at in+off; returns bytes consumed (0 = need
 * more), -1 fatal.  Compaction is the caller's, once per drain. */
static int bin_frame(struct pc_conn *c, size_t off)
{
	char *base = c->in + off;
	size_t avail = c->in_len - off;
	uint32_t plen;
	uint64_t id;
	char *pl, *resbuf;
	struct pc_jw rw;
	const char *errmsg = "error", *col = NULL, *key = NULL;
	size_t collen = 0, klen = 0;
	unsigned int park_req = 0;
	int flags = 0, rc;

	if (avail < PC_BIN_HDR)
		return 0;
	if ((unsigned char)base[0] != PC_BIN_MAGIC || base[1] != PC_BIN_VER)
		return -1;
	plen = get32(base + 4);
	if (plen > PC_MAX_REQ)
		return -1;
	if (avail < PC_BIN_HDR + plen)
		return 0;
	if (base[2] != PC_BIN_REQ)
		return -1;
	id = get64(base + 8);
	pl = base + PC_BIN_HDR;

	resbuf = scratch_buf(0);
	if (!resbuf)
		return -1;
	pc_jw_init(&rw, resbuf, JW_REPLY_CAP);
	{
		/* S159: timed and counted like a RESP command, as bin.<verb> */
		unsigned long long qt0 = obs_carry ? obs_carry : pc_obs_usec_now(), qdt;
		static const char *const names[] = { "?", "ping", "get",
			"set", "del", "exists", "ttl", "expire", "add", "sub",
			"publish", "subscribe", "unsubscribe", "psubscribe",
			"punsubscribe" };
		/* S189: a colon for the same reason as json: above - the
		 * binary door's names could not collide today, but one
		 * convention beats two and a reader should not have to know
		 * which door invented which separator. */
		static const char *const bnames[] = { "bin:?", "bin:ping",
			"bin:get", "bin:set", "bin:del", "bin:exists", "bin:ttl",
			"bin:expire", "bin:add", "bin:sub", "bin:publish",
			"bin:subscribe", "bin:unsubscribe", "bin:psubscribe",
			"bin:punsubscribe" };
		unsigned int vb = plen ? (unsigned char)pl[0] : 0;
		const char *vn = vb < sizeof names / sizeof names[0]
			? names[vb] : "?";
		const char *bn = vb < sizeof bnames / sizeof bnames[0]
			? bnames[vb] : "bin:?";

		pc_obs_conn_touch(c->obs, vn, strlen(vn), get_ticks());   /* S111 */
	ev_origin(c, "binary");                      /* S153 */
	rc = pc_verb_bin(pl, plen, &rw, &flags, &park_req,
		&col, &collen, &key, &klen, &errmsg, c);
		if (rc < 0 && errmsg == pc_bin_unknown_verb) {   /* S165 */
			char vnum[8];
			int vl = snprintf(vnum, sizeof vnum, "%u", vb);

			pc_obs_unknown(CLUNK_BIN, "verb", 4, vnum, (size_t)vl,
				c->obs);
		}
		obs_carry = pc_obs_usec_now();
		qdt = obs_carry - qt0;
		{
			/* the row slot per verb, resolved once per worker */
			static __thread void *slot[16];
			unsigned int si = vb < 15 ? vb : 15;

			if (!slot[si])
				slot[si] = pc_obs_cmd_slot(bn, strlen(bn));
			pc_obs_cmd_at(slot[si], qdt);
		}
		if (pc_obs_slow_would(qdt)) {
			char *av[3];
			size_t al[3];
			int na = 1;

			av[0] = (char *)bn;
			al[0] = strlen(bn);
			if (col) {
				av[na] = (char *)col;
				al[na++] = collen;
			}
			if (key) {
				av[na] = (char *)key;
				al[na++] = klen;
			}
			pc_obs_slow(av, al, na, qdt, c->obs);
		}
		if (pc_obs_qlog_on()) {
			pc_obs_qlog("bin", c->obs, vn, strlen(vn), col, collen, key,
				klen, rc < 0 || (flags & PC_BIN_F_ERR) ? "err"
				: strcmp(vn, "get") == 0 ? (rw.len ? "hit" : "miss")
				: "ok", qdt);
		}
	}
	if (rc == 0 && park_req) {
		/* deferred to a cluster pull/forward: the 8 id bytes park as
		 * they sit on the wire (LE) */
		if (col && key && park_add(c, park_req, base + 8, 8,
		        col, collen, key, klen, 1) == 0)
			goto consume;          /* reply comes later */
		/* could not park: the codec pre-wrote its own failure shape */
	}
	if (rc < 0)
		rc = bin_reply(c, id, PC_BIN_F_ERR, errmsg, strlen(errmsg));
	else if (rw.overflow)
		rc = bin_reply(c, id, PC_BIN_F_ERR, "reply too large", 15);
	else
		rc = bin_reply(c, id, flags, rw.buf, rw.len);
	if (rc < 0)
		return -1;
consume:
	return (int)(PC_BIN_HDR + plen);
}

/* ---- RESP dialect (task S29) --------------------------------------------
 * Frames one RESP2 command - multibulk ('*') from real clients, inline
 * (bare words + newline) from netcat - into argv spans over the in
 * buffer, then answers through pc_verb_resp.  Same consumed/0/-1
 * contract as the other two framers; compaction is the caller's. */

static int resp_ll_span(const char *p, size_t n, long long *out)
{
	long long v = 0;
	size_t i = 0;
	int neg = 0;

	if (n && (p[0] == '-' || p[0] == '+')) {
		neg = p[0] == '-';
		i = 1;
	}
	if (i == n || n > 17)
		return -1;
	for (; i < n; i++) {
		if (p[i] < '0' || p[i] > '9')
			return -1;
		v = v * 10 + (p[i] - '0');
	}
	*out = neg ? -v : v;
	return 0;
}

static int resp_execute_inner(struct pc_conn *c, char **argv,
		size_t *argl, int nargs)
{
	char *resbuf = scratch_buf(0), *scratch = scratch_buf(1);
	struct pc_jw rw;
	const char *col = NULL, *key = NULL;
	size_t collen = 0, klen = 0;
	unsigned int park_req = 0;
	struct pc_enum_start es;
	int quit = 0;

	if (!resbuf || !scratch)
		return -1;
	pc_jw_init(&rw, resbuf, JW_REPLY_CAP);
	memset(&es, 0, sizeof es);
	ev_origin(c, "resp");                        /* S153 */
	if (pc_verb_resp(argv, argl, nargs, &rw, scratch, JW_REPLY_CAP,
	        &park_req, &col, &collen, &key, &klen,
	        c->resp_col, &c->resp_collen, &quit, &c->resp_authed,
	        &es, c->obs, c) < 0)
		return -1;
	if (es.start) {
		memset(&c->enum_w, 0, sizeof c->enum_w);
		if (pc_jw_init_heap(&c->enum_w, 64L * 1024) != 0) {
			static const char msg[] =
				"-ERR out of memory starting the walk\r\n";

			return out_append(c, msg, sizeof msg - 1);
		}
		c->enum_active = 1;
		c->enum_cursor = 0;
		c->enum_emitted = 0;
		c->enum_limit = es.limit;
		c->enum_patlen = es.patlen;
		if (es.patlen > 0)
			memcpy(c->enum_pat, es.pat, (size_t)es.patlen);
		c->enum_ht = es.ht;
		/* the slot and its generation as ONE pair (see store.c): a
		 * table swapped out since the verb resolved it gives -1, and
		 * the first step ends the walk as a swap - counted */
		c->enum_pub = 0;
		c->enum_col = pc_store_slot_pub(es.ht, &c->enum_pub);
		c->enum_next = enum_head;
		enum_head = c;
		return 0;                      /* reply comes chunk by chunk */
	}
	if (park_req && col && key &&
	        park_add(c, park_req, NULL, 0, col, collen, key, klen, 2) == 0)
		return 0;                      /* reply comes on completion */
	if (rw.overflow) {
		static const char msg[] = "-ERR reply too large\r\n";

		pc_jw_free(&rw);               /* no-op unless it grew one */
		if (out_append(c, msg, sizeof msg - 1) < 0)
			return -1;
		return 0;
	}
	if (quit)
		c->close_after = 1;
	/* a verb may have swapped in a heap buffer of its own (CLUSTER
	 * SLOTS); out_append copies, so it can be released right after */
	{
		int rc = out_append(c, rw.buf, rw.len);

		pc_jw_free(&rw);
		return rc;
	}
}


/* S53: every RESP command is timed and counted around the real
 * dispatch.  A parked or walk-starting command is timed to the point
 * it left this worker - honest for what the worker PAID, understated
 * for what the client waited; the slow log records the former. */
static int resp_execute(struct pc_conn *c, char **argv, size_t *argl,
		int nargs)
{
	unsigned long long t0 = pc_obs_usec_now(), dt;
	size_t o0 = c->out_len;            /* S65: where this reply starts */
	int rc = resp_execute_inner(c, argv, argl, nargs);

	if (nargs > 0) {
		obs_carry = pc_obs_usec_now();     /* S159 */
		dt = obs_carry - t0;
		pc_obs_cmd(argv[0], argl[0], dt);
		pc_obs_conn_touch(c->obs, argv[0], argl[0], get_ticks());
		pc_obs_slow(argv, argl, nargs, dt, c->obs);
		if (pc_obs_qlog_on())
			pc_obs_qlog("resp", c->obs, argv[0], argl[0], NULL, 0,
				nargs > 1 ? argv[1] : NULL, nargs > 1 ? argl[1] : 0,
				rc < 0 || (c->out_len > o0 && c->out[o0] == '-') ? "err"
				: c->out_len - o0 >= 3 && (memcmp(c->out + o0, "$-1", 3) == 0
				        || memcmp(c->out + o0, "*-1", 3) == 0) ? "miss"
				: c->out_len > o0 && c->out[o0] == '$' ? "hit" : "ok", dt);
	}
	return rc;
}

static int resp_one(struct pc_conn *c, size_t off)
{
	char *base = c->in + off;
	size_t avail = c->in_len - off, pos;
	char *argv[PC_RESP_MAXARGS];
	size_t argl[PC_RESP_MAXARGS];
	long long nargs, blen;
	int i;

	if (base[0] != '*') {
		/* inline command: one line, split on blanks */
		char *nl = memchr(base, '\n', avail);
		size_t n, s = 0;
		int na = 0;

		if (!nl) {
			if (avail > 65536)
				return -1;     /* runaway inline line */
			return 0;
		}
		n = (size_t)(nl - base);
		if (n && base[n - 1] == '\r')
			n--;
		while (s < n && na < PC_RESP_MAXARGS) {
			size_t e;

			while (s < n && (base[s] == ' ' || base[s] == '\t'))
				s++;
			if (s == n)
				break;
			e = s;
			while (e < n && base[e] != ' ' && base[e] != '\t')
				e++;
			argv[na] = base + s;
			argl[na] = e - s;
			na++;
			s = e;
		}
		if (na && resp_execute(c, argv, argl, na) < 0)
			return -1;
		return (int)((size_t)(nl - base) + 1);
	}

	/* multibulk: *N\r\n then N x ($len\r\n bytes \r\n) */
	{
		char *nl = memchr(base, '\n', avail < 16 ? avail : 16);

		if (!nl)
			return avail >= 16 ? -1 : 0;
		if (resp_ll_span(base + 1, (size_t)(nl - base) -
		        (nl[-1] == '\r' ? 2 : 1), &nargs) < 0 ||
		        nargs < 0 || nargs > PC_RESP_MAXARGS)
			return -1;
		pos = (size_t)(nl - base) + 1;
	}
	for (i = 0; i < (int)nargs; i++) {
		char *nl;

		if (pos >= avail)
			return 0;
		if (base[pos] != '$')
			return -1;
		nl = memchr(base + pos, '\n',
			(avail - pos) < 16 ? (avail - pos) : 16);
		if (!nl)
			return (avail - pos) >= 16 ? -1 : 0;
		if (resp_ll_span(base + pos + 1, (size_t)(nl - (base + pos)) -
		        (nl[-1] == '\r' ? 2 : 1), &blen) < 0 ||
		        blen < 0 || blen > (long long)PC_MAX_REQ)
			return -1;
		pos = (size_t)(nl - base) + 1;
		if (avail - pos < (size_t)blen + 2)
			return 0;              /* body + CRLF not here yet */
		argv[i] = base + pos;
		argl[i] = (size_t)blen;
		pos += (size_t)blen;
		if (base[pos] == '\r')
			pos++;
		if (base[pos] != '\n')
			return -1;
		pos++;
	}
	if (nargs && resp_execute(c, argv, argl, (int)nargs) < 0)
		return -1;
	return (int)pos;
}


static int process_plaintext(struct pc_conn *c);

/* ---- S40: the cooperative walk, one chunk per event-loop turn ---------- */

int pc_enum_pending(void)
{
	return enum_head != NULL;
}

/* S150 B: walks that ended because the registry moved their table */
static unsigned long long enum_ended_on_swap;

unsigned long long pc_enum_ended_on_swap(void)
{
	return __atomic_load_n(&enum_ended_on_swap, __ATOMIC_RELAXED);
}

void pc_enum_step(void)
{
	struct pc_conn *c = enum_head, *next;
	int more, limit_hit;

	/* one chunk per active conn per turn: fairness between walks, and
	 * a bounded stall (~1024 buckets) for everything else this worker
	 * serves */
	for (; c; c = next) {
		next = c->enum_next;
		limit_hit = 0;
		/* S150 B: the pointer was taken on an earlier turn.  If the
		 * registry has published the slot since - a resize swap, a
		 * drop - the table under this walk is retired or retiring:
		 * what was emitted stands, and the walk ends here rather than
		 * read on from a table that may be reused. */
		if (c->enum_col < 0 ||
		        pc_store_pub_gen(c->enum_col) != c->enum_pub) {
			__atomic_fetch_add(&enum_ended_on_swap, 1,
				__ATOMIC_RELAXED);
			more = 0;
		} else
			more = pc_keys_chunk(c->enum_ht, &c->enum_cursor,
				c->enum_pat, c->enum_patlen, get_ticks(),
				&c->enum_w, &c->enum_emitted, c->enum_limit,
				&limit_hit);
		if (more && !limit_hit && !c->enum_w.overflow)
			continue;              /* walk on next turn */

		/* done (or stopped): assemble the one ordered reply */
		if (c->enum_w.overflow) {
			static const char msg[] = "-ERR reply too large\r\n";

			if (out_append(c, msg, sizeof msg - 1) < 0)
				goto drop;
		} else {
			char hdr[24];
			int hn = snprintf(hdr, sizeof hdr, "*%d\r\n",
				c->enum_emitted);

			if (out_append(c, hdr, (size_t)hn) < 0 ||
			        out_append(c, c->enum_w.buf,
			                c->enum_w.len) < 0)
				goto drop;
		}
		enum_abort(c);                 /* frees the writer, unlinks */
		/* input that arrived behind the KEYS is still buffered:
		 * serve it now, in order, then flush the lot */
		if (process_plaintext(c) < 0) {
			c->why = "protocol error";
			goto drop;
		}
		if (conn_flush(c) < 0) {
			c->why = "write error";
			goto drop;
		}
		if (c->close_after && !c->out_len && !c->wire_len)
			goto drop;
		continue;
drop:
		conn_destroy(c);
	}
}

/* dialect is per MESSAGE (see proto.h): each message self-describes on
 * its first byte, so one connection can mix JSON lines, binary frames
 * and RESP commands - libperfd sends the hot data verbs binary and the
 * long-tail verbs as text; redis-cli/rtpengine speak RESP (task S29).
 * c->dialect keeps the FIRST message's dialect as the memo that picks
 * the notification framing (RESP clients get none - Redis clients
 * cannot parse push frames). */
static int process_plaintext(struct pc_conn *c)
{
	size_t off = 0;
	int rc = 0;

	obs_carry = 0;                     /* S159: a fresh clock per drain */
	if (pc_ps_pressed)
		pc_pubsub_pressure_take(NULL); /* not ours: nothing waits on it */

	while (off < c->in_len) {
		/* a cooperative walk is in flight: its reply must precede
		 * any later request's (RESP replies are ordered), so leave
		 * the rest of the input buffered until the walk completes */
		if (c->enum_active)
			break;
		unsigned char f = (unsigned char)c->in[off];

		if (c->refused && !c->http_only && !c->resp_only) {
			/* S161: the refusal, in the dialect the client chose */
			if (f == '{') {
				text_error(c, NULL, 0, -32005,
					"max number of clients reached");
			} else {
				uint64_t id = c->in_len - off >= PC_BIN_HDR
					? get64((const unsigned char *)c->in + off + 8) : 0;
				bin_reply(c, id, PC_BIN_F_ERR,
					"max number of clients reached", 29);
			}
			c->close_after = 1;
			c->why = "max_clients";
			break;
		}
		if (c->http_only) {
			rc = http_one(c, off);
		} else if (c->resp_only) {
			/* pinned at accept: this listener speaks RESP and
			 * NOTHING else, so the native dialects - and with
			 * them the admin verbs - are unreachable here even
			 * if a client tries to open with '{' or 0x9E */
			if (f != '*' && !(f >= 'A' && f <= 'Z') &&
			        !(f >= 'a' && f <= 'z') &&
			        f != '\r' && f != '\n')
				return -1;
			rc = resp_one(c, off);
			if (rc > 0)
				PC_RESP_BUMP(pc_resp_reqs);
		} else if (f == PC_BIN_MAGIC) {
			if (c->dialect == PC_D_SNIFF) {
				c->dialect = PC_D_BIN;
				PC_RESP_BUMP(pc_nat_bin_conns);
				PC_RESP_BUMP(pc_nat_bin_open);     /* S123 */
				c->open_counted = 1;
				conn_log_open(c, "binary");
			}
			rc = bin_frame(c, off);
			if (rc > 0)
				PC_RESP_BUMP(pc_nat_bin_reqs);
		} else if (f == '{' || f == ' ' || f == '\t' || f == '\r' ||
		        f == '\n') {
			if (c->dialect == PC_D_SNIFF) {
				c->dialect = PC_D_TEXT;
				PC_RESP_BUMP(pc_nat_text_conns);
				PC_RESP_BUMP(pc_nat_text_open);    /* S123 */
				c->open_counted = 1;
				conn_log_open(c, "json");
			}
			rc = text_one(c, off);
			if (rc > 0)
				PC_RESP_BUMP(pc_nat_text_reqs);
		} else if (f == '*' || (f >= 'A' && f <= 'Z') ||
		        (f >= 'a' && f <= 'z')) {
			if (c->dialect == PC_D_SNIFF) {
				c->dialect = PC_D_RESP;
				PC_RESP_BUMP(pc_nat_resp_conns);
				PC_RESP_BUMP(pc_nat_resp_open);    /* S123 */
				c->open_counted = 1;
				conn_log_open(c, "resp");
			}
			rc = resp_one(c, off);
			if (rc > 0)
				PC_RESP_BUMP(pc_nat_resp_reqs);
		} else {
			return -1;
		}
		if (rc <= 0)
			break;
		off += (size_t)rc;
		if (c->close_after)
			break;                 /* QUIT answered: drop the rest */
		if (pc_ps_pressed && conn_pause(c))
			break;                 /* a queue it feeds is full: the rest waits */
	}
	if (rc < 0)
		return -1;
	if (off) {
		c->in_len -= off;
		memmove(c->in, c->in + off, c->in_len);
	}
	return 0;
}

/* ---- Noise channel ----------------------------------------------------- */

/* consume one handshake frame from c->raw; 1 established, 0 need more,
 * -1 fatal (auth failure / malformed) */
static int channel_handshake(struct pc_conn *c)
{
	uint8_t pay[PC_HS_MAXFRAME], m2[PC_HS_MAXFRAME], key_id, status;
	const struct pc_psk_ctx *psk = c->psk;
	size_t flen, noise_len, pl, m2len;
	const uint8_t *noise;
	char frame[2 + PC_HS_MAXFRAME];
	int i, matched = 0;

	if (c->raw_len < 2)
		return 0;
	flen = get16(c->raw);
	if (flen < 1 + PC_NOISE_DHLEN + PC_NOISE_TAGLEN || flen > PC_HS_MAXFRAME)
		return -1;
	if (c->raw_len < 2 + flen)
		return 0;

	key_id = (uint8_t)c->raw[2];
	noise = (const uint8_t *)c->raw + 3;
	noise_len = flen - 1;

	if (key_id == PC_PRIN_CLIENT) {
		for (i = 0; i < psk->n_client; i++) {
			pc_hs_init_responder(&c->hs, &key_id, 1);
			if (pc_hs_read_msg1(&c->hs, psk->client[i], noise, noise_len,
			        pay, &pl) == 0) {
				matched = 1;
				break;
			}
		}
	} else if (key_id == PC_PRIN_CLUSTER) {
		/* standalone with no cluster secret: the slot was never
		 * derived and holds zeros - a key anyone can present */
		if (!psk->have_cluster) {
			LM_WARN("handshake: cluster principal refused - no "
				"cluster secret configured\n");
			return -1;
		}
		pc_hs_init_responder(&c->hs, &key_id, 1);
		if (pc_hs_read_msg1(&c->hs, psk->cluster, noise, noise_len,
		        pay, &pl) == 0)
			matched = 1;
	} else {
		LM_WARN("handshake: unknown principal %u\n", key_id);
		return -1;
	}
	if (!matched) {
		LM_WARN("handshake: authentication failed (principal %u)\n", key_id);
		return -1;
	}
	if (pl < 1 || pay[0] != PC_CLIENT_VER) {
		LM_WARN("handshake: unsupported client version\n");
		return -1;
	}

	status = 0x00;
	if (pc_hs_write_msg2(&c->hs, &status, 1, m2, &m2len,
	        &c->send_cs, &c->recv_cs) != 0)
		return -1;
	put16(frame, (uint16_t)m2len);
	memcpy(frame + 2, m2, m2len);
	if (wire_append(c, frame, 2 + m2len) < 0)
		return -1;

	c->raw_len -= 2 + flen;
	memmove(c->raw, c->raw + 2 + flen, c->raw_len);
	c->established = 1;
	return 1;
}

/* decrypt all complete transport records in c->raw into c->in */
static int channel_records(struct pc_conn *c)
{
	uint8_t pt[PC_NOISE_MAXPT];
	size_t rlen;
	int pl;

	for (;;) {
		if (c->raw_len < 2)
			return 0;
		rlen = get16(c->raw);
		if (rlen < PC_NOISE_TAGLEN || rlen > PC_NOISE_MAXMSG)
			return -1;
		if (c->raw_len < 2 + rlen)
			return 0;
		pl = pc_transport_decrypt(&c->recv_cs, (const uint8_t *)c->raw + 2,
			rlen, pt);
		if (pl < 0)
			return -1;             /* bad tag: tampered / desynced */
		if (in_append(c, (char *)pt, (size_t)pl) < 0)
			return -1;
		c->raw_len -= 2 + rlen;
		memmove(c->raw, c->raw + 2 + rlen, c->raw_len);
	}
}

static int raw_append(struct pc_conn *c, const char *p, size_t n)
{
	if (buf_reserve(&c->raw, &c->raw_cap, c->raw_len, n,
	        PC_MAX_REQ + 4 * PC_NOISE_MAXMSG) < 0)
		return -1;
	memcpy(c->raw + c->raw_len, p, n);
	c->raw_len += n;
	return 0;
}

/* ---- pull completion (runs on the owning worker) ------------------------ */

void pc_proto_pull_complete(const struct pc_pull_done *d)
{
	struct parked *pk = NULL;
	struct pc_conn *c;
	char *reply;
	struct pc_jw w;
	long long resume_nv;
	int i, resume_rc;

	if (!parks)
		return;
	for (i = 0; i < parks_cap; i++)
		if (parks[i].conn && parks[i].req == d->req) {
			pk = &parks[i];
			break;
		}
	if (!pk)
		return;                        /* conn died while pulling */
	c = pk->conn;

	if (d->kind == PC_DONE_PULL && d->found) {
		pcache_htable_t *ht = pc_store_find(pk->col, pk->collen);

		if (ht && (pc_store_shard_enabled(ht) ||
		        (pc_cluster_replicas() &&
		         !pc_spread_in_set(pc_key_slot(pk->key,
		                (size_t)pk->klen), 0,
		                pc_cluster_replicas())))) {
			/* serve only.  Shard: the one copy stays with the
			 * owner.
			 * S127: spread, and placement does not make this node
			 * a holder - SERVE ONLY.  Keeping it would let the
			 * orphan the write path was taught to avoid back in
			 * through the read door: measured, reading the whole
			 * keyspace through one non-holder took it from its
			 * placement share of 67 back to all 120, and the
			 * fleet from K*N to 293.  A holder that pulled DOES
			 * keep it - that is repair, not an extra copy. */
		} else if (ht && !pc_store_proxy_enabled(ht)) {
			/* store mode: keep the pulled value PASSIVE, TTL
			 * re-anchored - the replicate-on-demand behavior */
			str k, v;
			unsigned int exp = d->ttl_left ?
				get_ticks() + d->ttl_left : 0;

			k.s = pk->key;
			k.len = (int)pk->klen;
			v.s = d->val;
			v.len = d->vlen;
			/* store it under the HOLDER's version, not a fresh
			 * local tick: the clocks converge to the fleet
			 * maximum, so a local number here can land ABOVE the
			 * holder's and make its very next genuine update look
			 * older than this copy of it */
			pcache_ht_store_ver(ht, &k, &v, exp, PCACHE_F_PASSIVE,
				d->ver);
		} else if (ht) {
			/* proxy mode: serve WITHOUT storing, remember the holder */
			pc_loc_set(pk->col, pk->collen, pk->key, pk->klen,
				d->from_node);
		}
	} else if (d->kind == PC_DONE_PULL) {
		pcache_htable_t *ht = pc_store_find(pk->col, pk->collen);

		if (ht && pc_store_shard_enabled(ht)) {
			/* an owner miss is authoritative - no negative cache
			 * needed.  EXCEPT during the reshard grace: the data
			 * may still sit on the old owner, so retry ONCE as a
			 * broadcast before answering the miss. */
			if (!pk->retried && pc_shard_grace()) {
				unsigned int req2 = pc_pull_begin(pk->col,
					pk->collen, pk->key, pk->klen);

				if (req2) {
					pk->retried = 1;
					pk->req = req2;
					return;
				}
			}
		} else {
			pc_neg_set(pk->col, pk->collen, pk->key, pk->klen,
				pc_cluster_neg_ms());
			pc_loc_clear(pk->col, pk->collen, pk->key, pk->klen);
		}
	} else if ((d->kind == PC_DONE_FWD_SET || d->kind == PC_DONE_FWD_TOUCH)
	        && d->ok) {
		/* the write landed at the holder: remember where */
		pcache_htable_t *ht = pc_store_find(pk->col, pk->collen);

		(void)ht;
		pc_neg_clear(pk->col, pk->collen, pk->key, pk->klen);
	}

	/* probe-before-place resolved to a fleet-wide miss: replay the
	 * write through the normal core (probe suppressed - placement
	 * decides).  A forward re-parks THIS entry: the ack completes it
	 * later as FWD_SET/FWD_ADD, nothing is written now. */
	resume_rc = -1000;
	resume_nv = 0;
	if (d->kind == PC_DONE_SET_RESUME && d->ok) {
		unsigned int req2 = 0;

		if (d->jop == 3 || d->jop == 4) {
			/* S213, RV-11: a re-arm or a delete never places.  The
			 * fleet said no - remember it, so the next miss of this
			 * key costs nothing */
			resume_rc = PC_OP_ABSENT;
			pc_neg_set(pk->col, pk->collen, pk->key, pk->klen,
				pc_cluster_neg_ms());
		} else if (d->jop == 2)
			resume_rc = pc_op_add_resume(pk->col, pk->collen,
				pk->key, pk->klen, d->newval,
				d->ttl_left == 0xFFFFFFFFu ? -1 :
				(long long)d->ttl_left, &resume_nv, &req2);
		else
			resume_rc = pc_op_set_resume(pk->col, pk->collen,
				pk->key, pk->klen, d->val, d->vlen,
				(long long)d->ttl_left, &req2);
		if (resume_rc == PC_OP_PARKED) {
			pk->req = req2;
			return;
		}
	}

	if (pk->bin == 2) {
		/* the request came as a RESP command: complete in kind.
		 * The done kind names the verb, so no extra state parks. */
		char nb[32];
		struct pc_jw w;

		pc_jw_init(&w, nb, sizeof nb);
		switch (d->kind) {
		case PC_DONE_PULL:
			if (d->found) {
				pc_jw_lit(&w, "$");
				pc_jw_i64(&w, d->vlen);
				pc_jw_lit(&w, "\r\n");
				out_append(c, w.buf, w.len);
				out_append(c, d->val, (size_t)d->vlen);
				out_append(c, "\r\n", 2);
			} else {
				out_append(c, "$-1\r\n", 5);
			}
			break;
		case PC_DONE_FWD_SET:
			/* "forward failed" used to cover BOTH of these and
			 * the begin-time failure in verbs.c, so an operator
			 * could not tell a network problem from an
			 * overloaded holder.  Measured under load: 3642
			 * timeouts with every begin-time counter at zero -
			 * nothing was wrong with the network at all.
			 *
			 * Deliberately NOT retryable: the holder may have
			 * stored it, so a retry is safe for SET and not for
			 * INCR.  An honest refusal beats a potential fork. */
			if (d->ok)
				out_append(c, "+OK\r\n", 5);
			else if (d->timedout)
				out_append(c, "-ERR holder timed out\r\n", 23);
			else
				out_append(c, "-ERR holder rejected\r\n", 22);
			break;
		case PC_DONE_FWD_DEL:
		case PC_DONE_FWD_TOUCH:        /* S213 */
			out_append(c, d->ok ? ":1\r\n" : ":0\r\n", 4);
			break;
		case PC_DONE_FWD_ADD:
			if (d->ok) {
				pc_jw_lit(&w, ":");
				pc_jw_i64(&w, d->newval);
				pc_jw_lit(&w, "\r\n");
				out_append(c, w.buf, w.len);
			} else if (d->timedout) {
				out_append(c, "-ERR holder timed out\r\n", 23);
			} else {
				out_append(c, "-ERR holder rejected\r\n", 22);
			}
			break;
		case PC_DONE_SET_RESUME:
			if (d->jop == 3 || d->jop == 4) {
				/* a re-arm or a delete: nobody has it, or nobody
				 * said so in time - nothing was done either way */
				out_append(c, ":0\r\n", 4);
			} else if (!d->ok) {   /* probe timeout: refuse */
				out_append(c, "-ERR cluster busy\r\n", 19);
			} else if (d->jop == 2) {
				if (resume_rc == PC_OP_OK) {
					pc_jw_lit(&w, ":");
					pc_jw_i64(&w, resume_nv);
					pc_jw_lit(&w, "\r\n");
					out_append(c, w.buf, w.len);
				} else {
					out_append(c, "-ERR value is not an "
						"integer or out of range\r\n",
						46);
				}
			} else if (resume_rc == PC_OP_OK) {
				out_append(c, "+OK\r\n", 5);
			} else if (resume_rc == PC_OP_ERR_FULL) {
				out_append(c, "-ERR cache full\r\n", 17);
			} else {
				out_append(c, "-ERR value too large\r\n", 22);
			}
			break;
		default:                       /* JSON ops never park RESP */
			out_append(c, "-ERR protocol\r\n", 15);
		}
		conn_flush(c);
		free(pk->key);
		pk->key = NULL;
		pk->conn = NULL;
		return;
	}

	if (pk->bin) {
		/* the request came in a binary frame: complete in kind */
		uint64_t id = get64(pk->id);
		char pay[16];
		const char *emsg = NULL;
		size_t pn = 0;

		switch (d->kind) {
		case PC_DONE_FWD_SET:          /* [stored u8] */
		case PC_DONE_FWD_DEL:          /* [deleted u8] */
		case PC_DONE_FWD_TOUCH:        /* [updated u8] (S213) */
			pay[0] = d->ok ? 1 : 0;
			pn = 1;
			break;
		case PC_DONE_FWD_ADD:          /* [value i64] */
			if (d->ok) {
				put64(pay, (uint64_t)d->newval);
				pn = 8;
			} else {
				emsg = d->timedout ? "holder timed out"
					: "holder rejected";
			}
			break;
		case PC_DONE_PULL:             /* the get reply shape */
			break;
		case PC_DONE_SET_RESUME:
			if (!d->ok) {          /* probe timeout: refuse */
				if (d->jop == 2)
					emsg = "cluster busy";
				else {
					pay[0] = 0;
					pn = 1;
				}
			} else if (d->jop == 3 || d->jop == 4) {
				pay[0] = 0;            /* nobody has it */
				pn = 1;
			} else if (d->jop == 2) {
				if (resume_rc == PC_OP_OK) {
					put64(pay, (uint64_t)resume_nv);
					pn = 8;
				} else {
					emsg = "value is not an integer";
				}
			} else if (resume_rc == PC_OP_OK) {
				pay[0] = 1;
				pn = 1;
			} else if (resume_rc == PC_OP_ERR_FULL) {
				emsg = "cache full";
			} else {
				emsg = "value too large";
			}
			break;
		default:
			emsg = "protocol";     /* JSON ops never park binary */
		}
		if (emsg) {
			bin_reply(c, id, PC_BIN_F_ERR, emsg, strlen(emsg));
		} else if (d->kind == PC_DONE_PULL) {
			if (d->found) {
				char *pb = malloc(5 + (size_t)d->vlen);

				if (pb) {
					pb[0] = 1;
					put32(pb + 1, d->ttl_left);
					memcpy(pb + 5, d->val, (size_t)d->vlen);
					bin_reply(c, id, 0, pb,
						5 + (size_t)d->vlen);
					free(pb);
				}
			} else {
				pay[0] = 0;
				bin_reply(c, id, 0, pay, 1);
			}
		} else {
			bin_reply(c, id, 0, pay, pn);
		}
		conn_flush(c);
		free(pk->key);
		pk->key = NULL;
		pk->conn = NULL;
		return;
	}

	reply = malloc(JW_REPLY_CAP);
	if (reply) {
		pc_jw_init(&w, reply, JW_REPLY_CAP);
		pc_jw_lit(&w, "{\"jsonrpc\":\"2.0\",\"id\":");
		if (pk->idlen)
			pc_jw_raw(&w, pk->id, pk->idlen);
		else
			pc_jw_lit(&w, "null");
		pc_jw_lit(&w, ",\"result\":");
		switch (d->kind) {
		case PC_DONE_FWD_SET:
			pc_jw_lit(&w, d->ok ? "{\"stored\":true}"
				: "{\"stored\":false}");
			break;
		case PC_DONE_FWD_DEL:
			pc_jw_lit(&w, d->ok ? "{\"deleted\":true}"
				: "{\"deleted\":false}");
			break;
		case PC_DONE_FWD_TOUCH:        /* S213 */
			pc_jw_lit(&w, d->ok ? "{\"updated\":true}"
				: "{\"updated\":false}");
			break;
		case PC_DONE_FWD_ADD:
			if (d->ok) {
				pc_jw_lit(&w, "{\"value\":");
				pc_jw_i64(&w, d->newval);
				pc_jw_lit(&w, "}");
			} else {
				pc_jw_lit(&w, d->timedout
					? "{\"error\":\"holder timed out\"}"
					: "{\"error\":\"holder rejected\"}");
			}
			break;
		case PC_DONE_SET_RESUME:
			if (!d->ok) {          /* probe timeout: refuse */
				pc_jw_lit(&w, d->jop == 2
					? "{\"error\":\"cluster busy\"}"
					: d->jop == 3 ? "{\"updated\":false}"
					: d->jop == 4 ? "{\"deleted\":false}"
					: "{\"stored\":false}");
			} else if (d->jop == 3) {
				pc_jw_lit(&w, "{\"updated\":false}");
			} else if (d->jop == 4) {
				pc_jw_lit(&w, "{\"deleted\":false}");
			} else if (d->jop == 2) {
				if (resume_rc == PC_OP_OK) {
					pc_jw_lit(&w, "{\"value\":");
					pc_jw_i64(&w, resume_nv);
					pc_jw_lit(&w, "}");
				} else {
					pc_jw_lit(&w, "{\"error\":"
						"\"value is not an integer\"}");
				}
			} else if (resume_rc == PC_OP_OK) {
				pc_jw_lit(&w, "{\"stored\":true}");
			} else if (resume_rc == PC_OP_ERR_FULL) {
				pc_jw_lit(&w, "{\"error\":\"cache full\"}");
			} else {
				pc_jw_lit(&w, "{\"error\":\"value too large\"}");
			}
			break;
		case PC_DONE_JSON:
			/* d->ok carries the holder status: 0 ok, 1 benign
			 * absent, 2 error/timeout */
			switch (d->jop) {
			case PC_JOP_SET:
				pc_jw_lit(&w, d->ok == 0 ? "{\"set\":true}"
					: "{\"set\":false}");
				break;
			case PC_JOP_DEL:
				pc_jw_lit(&w, d->ok == 0 ?
					"{\"deleted\":true}"
					: "{\"deleted\":false}");
				break;
			case PC_JOP_INCR:
				if (d->ok == 0) {
					pc_jw_lit(&w, "{\"value\":");
					pc_jw_i64(&w, d->newval);
					pc_jw_lit(&w, "}");
				} else {
					pc_jw_lit(&w, "{\"error\":"
						"\"holder rejected\"}");
				}
				break;
			case PC_JOP_APPEND:
				if (d->ok == 0) {
					pc_jw_lit(&w, "{\"count\":");
					pc_jw_i64(&w, d->count);
					pc_jw_lit(&w, "}");
				} else {
					pc_jw_lit(&w, "{\"error\":"
						"\"holder rejected\"}");
				}
				break;
			default:               /* GET */
				if (d->ok == 0 && d->val) {
					pc_jw_lit(&w,
						"{\"found\":true,\"value\":");
					pc_jw_raw(&w, d->val,
						(size_t)d->vlen);
					pc_jw_lit(&w, "}");
				} else {
					pc_jw_lit(&w, "{\"found\":false}");
				}
			}
			break;
		default:
			if (d->found) {
				pc_jw_lit(&w,
					"{\"found\":true,\"source\":\"cluster\",");
				pc_jw_value(&w, "value", "enc", d->val,
					(size_t)d->vlen);
				pc_jw_lit(&w, ",\"ttl\":");
				pc_jw_i64(&w, d->ttl_left ? (long long)d->ttl_left
					: -1);
				pc_jw_lit(&w, "}");
			} else {
				pc_jw_lit(&w, "{\"found\":false}");
			}
		}
		/* RV-10: this reply was PARKED - the node could not answer
		 * from its own table.  Under shard and spread that means the
		 * client asked the wrong node, and nothing told it: it paid
		 * the hop again on the next such key, until a membership
		 * push or its own periodic refresh (libperfd: refresh_ms,
		 * 30 s) caught up.  The reply says so now, with the stamp of
		 * the map this node routed by; a client holding another
		 * stamp re-learns.  The map itself never rides here - the
		 * slot space is ~11k ranges (S44) and this is the data path.
		 * Store, eager and proxy say nothing: a pull or a forward
		 * there is the mode working, not a client that is behind. */
		{
			pcache_htable_t *mht = pc_store_find(pk->col, pk->collen);
			unsigned int mt = 0, ms = 0;

			if (mht && (pc_store_shard_enabled(mht) ||
			            pc_cluster_replicas()) &&
			    pc_cluster_map_stamp(&mt, &ms)) {
				pc_jw_lit(&w, ",\"moved\":{\"term\":");
				pc_jw_i64(&w, mt);
				pc_jw_lit(&w, ",\"seq\":");
				pc_jw_i64(&w, ms);
				pc_jw_lit(&w, "}");
			}
		}
		pc_jw_lit(&w, "}\n");
		if (!w.overflow)
			out_append(c, w.buf, w.len);
		free(reply);
		conn_flush(c);
	}

	free(pk->key);
	pk->key = NULL;
	pk->conn = NULL;
}

/* ---- notifications ----------------------------------------------------- */

/* PS1: a pub/sub frame onto a connection THIS thread owns.  The engine
 * resolved the subscriber on the owner's turn, so the append and the
 * flush run where every other write to this connection runs.  A refused
 * append is the output cap - the slow subscriber - and the engine kills
 * the connection for it. */
static int push_flush(struct pc_conn *c);   /* with pc_conn_notify */
static int push_stage(struct pc_conn *c);   /* staged per worker turn */

/* ---- PS5: pub/sub pushes over UDP ------------------------------------------
 * The protocol is lib/perfd_push.h, the stream's life src/psudp.h.  A
 * stream belongs to its connection and so to one worker: no lock.  Its
 * datagrams go to the connection's own TCP peer address - never one the
 * client names - and leave from the door's UDP socket, bound by daemon.c
 * to the door's address and port and shared by the workers (a sendto on
 * one UDP socket is safe from several threads). */
struct pc_udp {
	struct psu_stream st;
	uint64_t confirmed_seq;            /* the last sequence before messages */
	unsigned char key[PFP_KEY];
	unsigned char cookie[PFP_COOKIE];
	struct sockaddr_in dst;
	int fd;
};

#define PC_UDP_DOORS 16
static struct { uint32_t addr; uint16_t port; int fd; } udp_doors[PC_UDP_DOORS];
static int n_udp_doors;
static long udp_streams;                                /* atomic */
static unsigned long long udp_probes, udp_confirmed, udp_expired, udp_pushed,
	udp_oversized, udp_send_errors, udp_acks, udp_pruned_silent,
	udp_pruned_stalled;                                 /* atomic */
static __thread int udp_open_here;     /* this worker's streams */

#define UDP_COUNT(x) __atomic_add_fetch(&(x), 1, __ATOMIC_RELAXED)

void pc_udp_door_add(uint32_t addr_be, uint16_t port, int fd)
{
	if (n_udp_doors < PC_UDP_DOORS) {
		udp_doors[n_udp_doors].addr = addr_be;
		udp_doors[n_udp_doors].port = port;
		udp_doors[n_udp_doors].fd = fd;
		n_udp_doors++;
	}
}

static long long udp_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* the UDP socket of the door this connection came in on: its local port,
 * on its local address or on a wildcard bind */
static int udp_door_fd(struct pc_conn *c)
{
	struct sockaddr_in me;
	socklen_t ml = sizeof me;
	int i, wild = -1;

	if (getsockname(c->fd, (struct sockaddr *)&me, &ml) != 0 ||
	    me.sin_family != AF_INET)
		return -1;
	for (i = 0; i < n_udp_doors; i++) {
		if (udp_doors[i].port != ntohs(me.sin_port))
			continue;
		if (udp_doors[i].addr == me.sin_addr.s_addr)
			return udp_doors[i].fd;
		if (udp_doors[i].addr == htonl(INADDR_ANY))
			wild = udp_doors[i].fd;
	}
	return wild;
}

static int udp_seal_send(struct pc_udp *u, unsigned type, uint64_t seq,
		const unsigned char *pt, size_t n)
{
	unsigned char dg[PFP_MAX + PFP_HDR + PFP_TAG], nonce[12];
	unsigned long long clen = 0;

	if (PFP_HDR + n + PFP_TAG > sizeof dg)
		return -1;
	pfp_header(dg, type, u->st.id, seq);
	pfp_nonce(nonce, seq);
	crypto_aead_chacha20poly1305_ietf_encrypt(dg + PFP_HDR, &clen, pt, n,
		dg, PFP_HDR, NULL, nonce, u->key);
	if (sendto(u->fd, dg, PFP_HDR + (size_t)clen, MSG_DONTWAIT,
	        (const struct sockaddr *)&u->dst, sizeof u->dst) < 0) {
		UDP_COUNT(udp_send_errors);
		return -1;
	}
	return 0;
}

static void udp_probe(struct pc_udp *u, long long now)
{
	uint64_t seq = psu_probe_sent(&u->st, now);

	UDP_COUNT(udp_probes);
	udp_seal_send(u, PFP_PROBE, seq, u->cookie, sizeof u->cookie);
}

/* 0 sent (or refused by the socket - a gap), 1 too large: use TCP */
static int udp_push(struct pc_udp *u, const char *pat, size_t plen,
		const char *chan, size_t clen, const char *data, size_t dlen)
{
	unsigned char pt[PFP_MAX], *w = pt;
	size_t n;

	if (!pat)
		plen = 0;                      /* a message, not a pmessage */
	n = 1 + 2 + plen + 2 + clen + 4 + dlen;
	if (PFP_HDR + n + PFP_TAG > PFP_MAX || plen > 0xffff || clen > 0xffff) {
		UDP_COUNT(udp_oversized);
		return 1;
	}
	*w++ = pat ? 1 : 0;
	pfp_put16(w, (unsigned)plen);
	w += 2;
	if (plen)
		memcpy(w, pat, plen);
	w += plen;
	pfp_put16(w, (unsigned)clen);
	w += 2;
	memcpy(w, chan, clen);
	w += clen;
	pfp_put32(w, (uint32_t)dlen);
	w += 4;
	if (dlen)
		memcpy(w, data, dlen);
	if (udp_seal_send(u, PFP_MESSAGE, psu_message_sent(&u->st, udp_now_ms()),
	        pt, n) == 0)
		UDP_COUNT(udp_pushed);
	return 0;
}

static void udp_free(struct pc_conn *c)
{
	sodium_memzero(c->udp, sizeof *c->udp);
	free(c->udp);
	c->udp = NULL;
	udp_open_here--;
	__atomic_sub_fetch(&udp_streams, 1, __ATOMIC_RELAXED);
}

int pc_conn_udp_on(void *conn, int port, uint64_t *stream, char *key_hex,
		const char **err)
{
	struct pc_conn *c = conn;
	struct sockaddr_in peer;
	socklen_t pl = sizeof peer;
	struct pc_udp *u;
	int fd;

	if (!c || c->resp_only || c->http_only) {
		*err = "UDP pushes are for native connections";
		return -1;
	}
	if (!port) {
		if (c->udp)
			udp_free(c);
		return 1;
	}
	if (port < 1 || port > 65535) {
		*err = "port must be 1..65535, or 0 to stop";
		return -1;
	}
	/* the destination is this connection's peer and nothing else */
	if (getpeername(c->fd, (struct sockaddr *)&peer, &pl) != 0 ||
	    peer.sin_family != AF_INET) {
		*err = "UDP pushes need an IPv4 connection";
		return -1;
	}
	fd = udp_door_fd(c);
	if (fd < 0) {
		*err = "this door sends no UDP pushes (pubsub_udp = no, or its "
			"UDP port was taken)";
		return -1;
	}
	u = c->udp;
	if (!u) {
		u = calloc(1, sizeof *u);
		if (!u) {
			*err = "out of memory";
			return -1;
		}
		c->udp = u;
		udp_open_here++;
		__atomic_add_fetch(&udp_streams, 1, __ATOMIC_RELAXED);
	}
	/* a new call is a new stream: new key, cookie and sequence */
	randombytes_buf(u->key, sizeof u->key);
	randombytes_buf(u->cookie, sizeof u->cookie);
	do
		randombytes_buf(stream, sizeof *stream);
	while (!*stream);
	u->dst = peer;
	u->dst.sin_port = htons((uint16_t)port);
	u->fd = fd;
	psu_start(&u->st, *stream, udp_now_ms());
	udp_probe(u, u->st.started_ms);
	sodium_bin2hex(key_hex, 2 * PFP_KEY + 1, u->key, sizeof u->key);
	return 0;
}

int pc_conn_udp_confirm(void *conn, const char *cookie_hex, size_t n,
		uint64_t *seq, const char **err)
{
	struct pc_conn *c = conn;
	unsigned char ck[PFP_COOKIE];
	size_t got = 0;

	if (!c || !c->udp) {
		*err = "no UDP stream on this connection (pubsub_udp first)";
		return -1;
	}
	if (n != 2 * sizeof ck || sodium_hex2bin(ck, sizeof ck, cookie_hex, n,
	        NULL, &got, NULL) != 0 || got != sizeof ck ||
	    sodium_memcmp(ck, c->udp->cookie, sizeof ck) != 0) {
		*err = "that is not this stream's cookie";
		return -1;
	}
	if (!c->udp->st.active) {
		psu_confirm(&c->udp->st, udp_now_ms());
		c->udp->confirmed_seq = c->udp->st.seq;
		UDP_COUNT(udp_confirmed);
	}
	/* every message after this carries a higher sequence: the client's
	 * gap count starts here, whatever probes it did or did not see */
	*seq = c->udp->confirmed_seq;
	return 0;
}

int pc_conn_udp_ack(void *conn, uint64_t seq, const char **err)
{
	struct pc_conn *c = conn;

	if (!c || !c->udp || !c->udp->st.active) {
		*err = "no confirmed UDP stream on this connection";
		return -1;
	}
	if (psu_ack(&c->udp->st, seq, udp_now_ms()) != 0) {
		*err = "seq is beyond the last datagram sent";
		return -1;
	}
	UDP_COUNT(udp_acks);
	return 0;
}

/* the client stopped acknowledging: every subscription on the connection
 * goes, and the connection is told why */
static void udp_prune(struct pc_conn *c, int why)
{
	char msg[200];
	int n = snprintf(msg, sizeof msg,
		"{\"jsonrpc\":\"2.0\",\"method\":\"pubsub_udp_pruned\",\"params\":"
		"{\"stream\":\"%016llx\",\"reason\":\"%s\"}}",
		(unsigned long long)c->udp->st.id,
		why == PSU_PRUNE_SILENT ? "no ack" : "no progress");

	UDP_COUNT(*(why == PSU_PRUNE_SILENT ? &udp_pruned_silent
		: &udp_pruned_stalled));
	LM_NOTICE("pub/sub: UDP pushes to %s pruned (%s) - the connection's "
		"subscriptions are dropped\n", inet_ntoa(c->udp->dst.sin_addr),
		why == PSU_PRUNE_SILENT ? "no ack" : "no progress");
	udp_free(c);
	if (c->ps_used)
		pc_pubsub_conn_closed(c);
	pc_conn_notify(c, msg, (size_t)n);
}

int pc_conn_udp_open(void)
{
	return udp_open_here;
}

int pc_conn_sweep_udp(struct pc_conn **list)
{
	static __thread long long last;
	struct pc_conn *c, *next;
	long long now;

	if (!udp_open_here)
		return 0;
	now = udp_now_ms();
	if (now - last < 250)
		return udp_open_here;
	last = now;
	for (c = *list; c; c = next) {
		int due;

		next = c->next;
		if (!c->udp)
			continue;
		due = psu_due(&c->udp->st, now);
		if (due == PSU_PROBE) {
			udp_probe(c->udp, now);
		} else if (due == PSU_EXPIRE) {
			UDP_COUNT(udp_expired);
			udp_free(c);
		} else if (due == PSU_PRUNE_SILENT || due == PSU_PRUNE_STALLED) {
			udp_prune(c, due);
		}
	}
	return udp_open_here;
}

void pc_conn_udp_figures(struct pc_udp_figures *f)
{
	f->streams = __atomic_load_n(&udp_streams, __ATOMIC_RELAXED);
	f->probes = __atomic_load_n(&udp_probes, __ATOMIC_RELAXED);
	f->confirmed = __atomic_load_n(&udp_confirmed, __ATOMIC_RELAXED);
	f->expired = __atomic_load_n(&udp_expired, __ATOMIC_RELAXED);
	f->pushed = __atomic_load_n(&udp_pushed, __ATOMIC_RELAXED);
	f->oversized = __atomic_load_n(&udp_oversized, __ATOMIC_RELAXED);
	f->send_errors = __atomic_load_n(&udp_send_errors, __ATOMIC_RELAXED);
	f->acks = __atomic_load_n(&udp_acks, __ATOMIC_RELAXED);
	f->pruned_no_ack = __atomic_load_n(&udp_pruned_silent, __ATOMIC_RELAXED);
	f->pruned_no_progress = __atomic_load_n(&udp_pruned_stalled,
		__ATOMIC_RELAXED);
}
int pc_conn_pubsub_push(void *conn, const char *pat, size_t plen,
		const char *chan, size_t clen, const char *data, size_t dlen,
		const char *resp_frame, size_t flen, struct pc_ps_note *note)
{
	struct pc_conn *c = conn;
	int rc;

	if (c->encrypted && !c->established)
		return PC_PS_PUSH_NOMEM;           /* not yet a channel: skip */
	if (c->dialect == PC_D_RESP) {
		rc = out_append(c, resp_frame, flen);
		return rc < 0 ? rc : push_stage(c);
	}
	/* PS5: a confirmed UDP stream takes what fits a datagram.  A send the
	 * socket refused is a gap the client counts, not a reason for TCP */
	if (c->udp && c->udp->st.active &&
	    udp_push(c->udp, pat, plen, chan, clen, data, dlen) <= 0)
		return 0;
	if (c->dialect != PC_D_TEXT && c->dialect != PC_D_BIN)
		return 0;                      /* still sniffing: nothing to
		                                * frame it in yet */
	/* PS4: the native doors get an id-less JSON notification - the S107
	 * shape libperfd's notify hook already receives - framed per
	 * dialect by pc_conn_notify: a line on the text dialect, a NOTIFY
	 * frame on the binary one.  The payload goes out as a string when
	 * it is clean UTF-8 and base64 with the "enc" sibling otherwise
	 * (decision 4, DESIGN 12eb).  The body is the same for every native
	 * subscriber of the message (and pattern), so the first one builds it
	 * into the caller's note: built per subscriber it was 45% of the JSON
	 * door's CPU delivering to 64. */
	if (!note->state) {
		size_t cap = 128 + 2 * plen + 2 * clen + 2 * dlen;
		struct pc_jw w;

		note->state = -1;
		note->buf = malloc(cap);
		if (!note->buf)
			return PC_PS_PUSH_NOMEM;
		pc_jw_init(&w, note->buf, cap);
		if (pat) {
			pc_jw_lit(&w, "{\"jsonrpc\":\"2.0\",\"method\":\"pmessage\","
				"\"params\":{\"pattern\":");
			pc_jw_str(&w, pat, plen);
			pc_jw_lit(&w, ",\"channel\":");
		} else {
			pc_jw_lit(&w, "{\"jsonrpc\":\"2.0\",\"method\":\"message\","
				"\"params\":{\"channel\":");
		}
		pc_jw_str(&w, chan, clen);
		pc_jw_lit(&w, ",");
		pc_jw_value(&w, "payload", "enc", data, dlen);
		pc_jw_lit(&w, "}}");
		if (w.overflow)
			return PC_PS_PUSH_NOMEM;       /* state stays -1: every one skips */
		note->len = w.len;
		note->state = 1;
	}
	if (note->state < 0)
		return PC_PS_PUSH_NOMEM;
	return pc_conn_notify(c, note->buf, note->len);
}

/* S160: the row's subscription count, kept by the engine's bookkeeping */
void pc_conn_subs_note(void *conn, int n)
{
	struct pc_conn *c = conn;

	c->ps_used = 1;
	pc_obs_conn_subs(c->obs, n);
}

int pc_conn_kill(void *conn, const char *why)
{
	struct pc_conn *c = conn;

	if (c->ps_killed)
		return 0;                      /* already closing: not a second kill */
	c->ps_killed = 1;
	c->why = why;
	shutdown(c->fd, SHUT_RDWR);        /* the event loop tears it down */
	return 1;
}

/* a push has been staged: send what the socket takes.  A failure is the
 * slow consumer when the output is at the cap, a broken socket otherwise -
 * the two close the connection for different reasons and are counted
 * differently. */
static int push_flush(struct pc_conn *c)
{
	if (conn_flush(c) == 0)
		return 0;
	return c->out_len + (c->wire_len - c->wire_off) >= PC_MAX_OUTQ
		? PC_PS_PUSH_SLOW : PC_PS_PUSH_BROKEN;
}

/* Staged pushes.  A push appends its frame and puts the connection on
 * this worker's list; the worker writes the list once its turn is over
 * (pc_conn_flush_pushes) - one write for a turn's frames, where a write
 * per frame capped a worker at ~120k deliveries a second.  Output past
 * PUSH_FLUSH_AT is written at once, so a burst meets the slow-consumer
 * cap only on output the socket refused.  Only a worker stages: a push
 * on any other thread writes at once, since nothing there would flush. */
#define PUSH_FLUSH_AT (64u << 10)
static __thread struct pc_conn *push_dirty;
static __thread int push_batching;

static int push_stage(struct pc_conn *c)
{
	if (!push_batching || c->out_len >= PUSH_FLUSH_AT)
		return push_flush(c);
	if (!c->push_staged) {
		c->push_staged = 1;
		c->push_next = push_dirty;
		push_dirty = c;
	}
	return 0;
}

static void push_unstage(struct pc_conn *c)
{
	struct pc_conn **pp;

	if (!c->push_staged)
		return;
	for (pp = &push_dirty; *pp; pp = &(*pp)->push_next)
		if (*pp == c) {
			*pp = c->push_next;
			break;
		}
	c->push_staged = 0;
	c->push_next = NULL;
}

void pc_conn_push_batching(void)
{
	push_batching = 1;
}

/* Paused publishers (see pubsub.h, pressure).  A command that left a
 * worker's queue at half its cap pauses its connection: no more of its
 * input is served or read - the socket fills and the client's own writes
 * wait, which is the backpressure - until every queue it pressed is below
 * a quarter.  The drains wake this worker when they pass that mark; the
 * worker also asks again on a short timer, so a missed wake costs 50 ms,
 * not a stuck client.  Other connections on the worker are untouched. */
static __thread struct pc_conn *paused_head;
static __thread int paused_here;

static void pause_set_events(struct pc_conn *c)
{
	struct epoll_event ev;

	ev.events = (c->ps_paused ? 0 : EPOLLIN) | (c->want_write ? EPOLLOUT : 0);
	ev.data.ptr = c;
	epoll_ctl(c->ep, EPOLL_CTL_MOD, c->fd, &ev);
}

static int conn_pause(struct pc_conn *c)
{
	uint64_t *mask;

	if (c->ps_paused) {                /* already waiting: never listed twice */
		pc_pubsub_pressure_take(NULL);
		return 1;
	}
	mask = malloc(PC_PS_MASK_WORDS * sizeof *mask);
	if (!mask) {
		pc_pubsub_pressure_take(NULL); /* the cap still bounds it */
		return 0;
	}
	pc_pubsub_pressure_take(mask);
	if (pc_pubsub_pressure_wait(mask)) {
		free(mask);                    /* drained while it was answered */
		return 0;
	}
	c->pause_mask = mask;
	c->ps_paused = 1;
	c->pause_next = paused_head;
	paused_head = c;
	paused_here++;
	pause_set_events(c);
	pc_pubsub_pause_note();
	return 1;
}

static void pause_unlink(struct pc_conn *c)
{
	struct pc_conn **pp;

	if (!c->ps_paused)
		return;
	for (pp = &paused_head; *pp; pp = &(*pp)->pause_next)
		if (*pp == c) {
			*pp = c->pause_next;
			break;
		}
	c->pause_next = NULL;
	c->ps_paused = 0;
	free(c->pause_mask);
	c->pause_mask = NULL;
	paused_here--;
}

int pc_conn_paused_open(void)
{
	return paused_here;
}

void pc_conn_resume_paused(void)
{
	struct pc_conn *c, *next, *list = paused_head;

	/* taken whole: a connection resumed below can pause again, onto a
	 * list this walk is no longer reading */
	paused_head = NULL;
	for (c = list; c; c = next) {
		next = c->pause_next;
		c->pause_next = NULL;
		if (!pc_pubsub_pressure_wait(c->pause_mask)) {
			c->pause_next = paused_head;
			paused_head = c;
			continue;
		}
		c->ps_paused = 0;
		free(c->pause_mask);
		c->pause_mask = NULL;
		paused_here--;
		pause_set_events(c);
		/* what it sent while paused is still buffered: serve it now,
		 * in order; what is still in the socket is read on its event */
		if (process_plaintext(c) < 0) {
			c->why = "protocol error";
			conn_destroy(c);
			continue;
		}
		if (conn_flush(c) < 0) {
			c->why = "write error";
			conn_destroy(c);
			continue;
		}
		if (c->close_after && !c->out_len && c->wire_off >= c->wire_len) {
			c->why = "quit";
			conn_destroy(c);
		}
	}
}

void pc_conn_flush_pushes(void)
{
	struct pc_conn *c;
	int rc;

	while ((c = push_dirty) != NULL) {
		push_dirty = c->push_next;
		c->push_next = NULL;
		c->push_staged = 0;
		if (c->ps_killed)
			continue;                  /* closing: the loop tears it down */
		rc = push_flush(c);
		if (rc != 0)
			pc_pubsub_push_failed(c, rc);
	}
}

int pc_conn_notify(struct pc_conn *c, const char *payload, size_t n)
{
	char hdr[PC_BIN_HDR];
	int rc;

	if (c->encrypted && !c->established)
		return PC_PS_PUSH_NOMEM;           /* not yet a channel: skip */
	if (c->dialect == PC_D_TEXT) {
		rc = out_append(c, payload, n);
		if (rc == 0)
			rc = out_append(c, "\n", 1);
	} else if (c->dialect == PC_D_BIN) {
		hdr[0] = (char)PC_BIN_MAGIC;
		hdr[1] = PC_BIN_VER;
		hdr[2] = PC_BIN_NOTIFY;
		hdr[3] = 0;
		put32(hdr + 4, (uint32_t)n);
		put64(hdr + 8, 0);
		rc = out_append(c, hdr, sizeof hdr);
		if (rc == 0)
			rc = out_append(c, payload, n);
	} else {
		return PC_PS_PUSH_NOMEM;       /* nothing to frame it in: skip */
	}
	if (rc < 0)
		return rc;                     /* SLOW at the cap, NOMEM */
	return push_stage(c);
}

void pc_conn_notify_all(struct pc_conn *list, const char *payload, size_t n)
{
	struct pc_conn *c;

	for (c = list; c; c = c->next) {
		if (c->resp_only || c->http_only)
			continue;
		if (c->dialect != PC_D_TEXT && c->dialect != PC_D_BIN)
			continue;                  /* still sniffing: nothing to
			                            * frame it in yet */
		pc_conn_notify(c, payload, n);
	}
}

/* ---- the event pump ---------------------------------------------------- */

int pc_conn_event(struct pc_conn *c, uint32_t events)
{
	ssize_t n;
	char tmp[16384];

	if (events & (EPOLLHUP | EPOLLERR))
		goto drop;

	if (events & EPOLLIN) {
		for (;;) {
			if (c->ps_paused)
				break;                 /* its input waits in the socket */
			n = read(c->fd, tmp, sizeof tmp);
			if (n == 0)
				goto drop;
			if (n < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					break;
				c->why = "read error";
				goto drop;
			}
			if (c->encrypted) {
				int st;

				if (raw_append(c, tmp, (size_t)n) < 0)
					goto drop;
				if (!c->established) {
					st = channel_handshake(c);
					if (st < 0) {
						/* S89: the commonest one-digit mistake is
						 * a RESP client on the native door.  A
						 * Noise handshake never opens with '*'
						 * or a letter; say what was contacted
						 * and where the RESP door is. */
						unsigned char f = c->raw_len
							? (unsigned char)c->raw[0] : 0;

						if (f == '*' || (f >= 'A' && f <= 'Z') ||
						    (f >= 'a' && f <= 'z')) {
							struct sockaddr_in me;
							socklen_t ml = sizeof me;
							int lp = getsockname(c->fd,
								(struct sockaddr *)&me, &ml) == 0
								? ntohs(me.sin_port) : 0;

							if (conn_log_ok())
								LM_NOTICE("client %s sent a RESP "
									"command to the native door on "
									"port %d, which speaks the native "
									"dialect (Noise) - the RESP door "
									"is %s\n", pc_obs_conn_addr(c->obs),
									lp, pc_resp_door()
									? pc_resp_door()
									: "not configured");
							c->why = "RESP on the native door";
						} else
							c->why = "handshake failed - no or wrong client secret";
						goto drop;
					}
					if (st == 0)
						continue;      /* need more handshake bytes */
					if (conn_flush(c) < 0)   /* send msg2 now */
						goto drop;
				}
				if (channel_records(c) < 0)
					goto drop;
			} else {
				if (in_append(c, tmp, (size_t)n) < 0)
					goto drop;
			}
			if (process_plaintext(c) < 0)
				goto drop;
		}
	}
	if (conn_flush(c) < 0)
		goto drop;
	if (c->close_after && !c->out_len && c->wire_off >= c->wire_len) {
		c->why = "quit";               /* QUIT: reply flushed, close */
		goto drop;
	}
	return 0;
drop:
	conn_destroy(c);
	return -1;
}
