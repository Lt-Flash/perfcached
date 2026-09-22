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
 * line-parse path serves both plaintext and Noise transports; the
 * Noise leg reuses the daemon's own pc_noise.[ch] verbatim.
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

#include "../src/json.h"
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
 * headers beyond json/noise) */
#define BIN_MAGIC   0x9E
#define BIN_VER     0x01
#define BIN_REQ     1
#define BIN_RSP     2
#define BIN_NOTIFY  3
#define BIN_F_ERR   0x01
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
#define BIN_MAX_RSP (16u << 20)        /* reply payload sanity ceiling */

struct early {
	unsigned long long id;
	char *result;                  /* NULL = the error below applies */
	size_t rlen;                   /* binary payload length */
	int bin;                       /* which dialect answered */
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

struct perfd {
	int fd;
	int encrypted;
	int poisoned;
	int binary;                    /* opts.binary: data verbs go binary */
	int eager_push;                /* opts.eager_push: write per submit */
	int io_ms;
	struct pc_cipherstate cs_send, cs_recv;
	char err[256];

	unsigned long long next_id;

	char *rbuf;                    /* the decrypted/received stream */
	size_t rlen, rcap;
	size_t roff;                    /* consumed prefix; see take_msg */

	char *q;                       /* queued (pipelined) request bytes */
	size_t qlen, qcap;
	unsigned long long *ids;       /* FIFO of ids awaiting replies */
	int nids, idhead, idcap;

	struct early *early;

	perfd_notify_cb ncb;
	void *nctx;

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
	/* RV-10: the stamp of the map the member list was learned under,
	 * and the stamp the last forwarded reply on THIS link carried */
	unsigned int map_term, map_seq, hint_term, hint_seq;
	int map_known, hint_seen, ignore_push;
	long long hint_next_ms;
	unsigned long long moved_hints;
	int learning, learned_async;   /* async fleet discovery, once */
	unsigned long long route_missed;   /* owner known, not connected */
	int route_k;                       /* S127: the fleet's copy factor,
	                                    * learned from routing.replicas;
	                                    * 1 for shard and for any daemon
	                                    * that does not publish it */
	/* the options spares are opened with - the secrets are copied, so
	 * the caller's array need not outlive the handle */
	char *sec[PERFD_MAX_SECRETS + 1];
	int nsec;
	int o_connect_ms, o_io_ms, o_binary;

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
	struct pc_jtok *tok;           /* JSON token scratch, see parse_line */
	int tokcap;                    /* in ELEMENTS, not bytes */
	struct pd_call {               /* id -> who asked */
		unsigned long long id;
		perfd_reply_cb cb;
		void *ctx;
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

static void notify_internal(perfd_t *p, const char *json, size_t n);
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

/* ---- raw IO ------------------------------------------------------------ */

static int io_all(perfd_t *p, int wr, void *buf, size_t n)
{
	size_t off = 0;

	while (off < n) {
		ssize_t r = wr ? write(p->fd, (char *)buf + off, n - off)
		               : read(p->fd, (char *)buf + off, n - off);

		if (r <= 0) {
			if (r < 0 && errno == EINTR)
				continue;
			p->poisoned = 1;
			return err(p, "connection %s (%s)",
				r == 0 ? "closed" : "failed",
				r == 0 ? "EOF" : strerror(errno));
		}
		off += (size_t)r;
	}
	return 0;
}

static int send_bytes(perfd_t *p, const char *b, size_t n)
{
	if (!p->encrypted)
		return io_all(p, 1, (void *)b, n);
	while (n) {
		uint8_t rec[2 + PC_NOISE_MAXPT + PC_NOISE_TAGLEN];
		size_t chunk = n > PC_NOISE_MAXPT ? PC_NOISE_MAXPT : n;
		int cl = pc_transport_encrypt(&p->cs_send, (const uint8_t *)b,
			chunk, rec + 2);

		if (cl < 0) {
			p->poisoned = 1;
			return err(p, "transport encrypt failed");
		}
		rec[0] = (uint8_t)cl;
		rec[1] = (uint8_t)(cl >> 8);
		if (io_all(p, 1, rec, 2 + (size_t)cl) != 0)
			return -1;
		b += chunk;
		n -= chunk;
	}
	return 0;
}

static int rbuf_reserve(perfd_t *p, size_t more)
{
	/* Drop the consumed prefix in ONE move, rather than shifting the
	 * whole remainder down after every message the way take_msg used
	 * to.  That was O(bytes remaining) per reply, so a read carrying
	 * a deep pipeline cost O(n^2): at depth 256 the client spent 61%
	 * of its CPU in memmove and throughput went BACKWARDS - 5.4M ops/s
	 * at depth 64 down to 3.1M at 256. */
	if (p->roff) {
		if (p->rlen > p->roff)
			memmove(p->rbuf, p->rbuf + p->roff, p->rlen - p->roff);
		p->rlen -= p->roff;
		p->roff = 0;
	}
	if (p->rlen + more <= p->rcap)
		return 0;
	while (p->rcap < p->rlen + more)
		p->rcap = p->rcap ? p->rcap * 2 : 65536;
	{
		/* never p->x = realloc(p->x, ...): a failed grow would lose
		 * the old block (leak) AND the buffered bytes */
		void *nb = realloc(p->rbuf, p->rcap);

		if (!nb) {
			p->poisoned = 1;
			return err(p, "out of memory");
		}
		p->rbuf = nb;
	}
	return 0;
}

/* grow the stream by at least one byte (blocking, timeout-bounded) */
static int fill(perfd_t *p)
{
	if (!p->encrypted) {
		ssize_t r;

		if (rbuf_reserve(p, 65536) != 0)
			return -1;
		r = read(p->fd, p->rbuf + p->rlen, p->rcap - p->rlen);
		if (r <= 0) {
			p->poisoned = 1;
			return err(p, "connection %s",
				r == 0 ? "closed" : strerror(errno));
		}
		p->rlen += (size_t)r;
		return 0;
	}
	{
		uint8_t hdr[2], ct[PC_NOISE_MAXMSG];
		size_t cl;
		int pl;

		if (io_all(p, 0, hdr, 2) != 0)
			return -1;
		cl = (size_t)hdr[0] | ((size_t)hdr[1] << 8);
		if (cl < PC_NOISE_TAGLEN || cl > PC_NOISE_MAXMSG) {
			p->poisoned = 1;
			return err(p, "bad transport record");
		}
		if (io_all(p, 0, ct, cl) != 0)
			return -1;
		if (rbuf_reserve(p, cl) != 0)
			return -1;
		pl = pc_transport_decrypt(&p->cs_recv, ct, cl,
			(uint8_t *)p->rbuf + p->rlen);
		if (pl < 0) {
			p->poisoned = 1;
			return err(p, "transport decrypt failed");
		}
		p->rlen += (size_t)pl;
		return 0;
	}
}

/* ---- little-endian field helpers (the binary frames) ------------------- */

static void wle16(unsigned char *b, unsigned int v)
{
	b[0] = (unsigned char)v;
	b[1] = (unsigned char)(v >> 8);
}

static void wle32(unsigned char *b, unsigned int v)
{
	int i;

	for (i = 0; i < 4; i++)
		b[i] = (unsigned char)(v >> (8 * i));
}

static void wle64(unsigned char *b, unsigned long long v)
{
	int i;

	for (i = 0; i < 8; i++)
		b[i] = (unsigned char)(v >> (8 * i));
}

static unsigned int rle32(const char *b)
{
	unsigned int v = 0;
	int i;

	for (i = 0; i < 4; i++)
		v |= (unsigned int)(unsigned char)b[i] << (8 * i);
	return v;
}

static unsigned long long rle64(const char *b)
{
	unsigned long long v = 0;
	int i;

	for (i = 0; i < 8; i++)
		v |= (unsigned long long)(unsigned char)b[i] << (8 * i);
	return v;
}

/* ---- reply parsing ----------------------------------------------------- */

static int ext_start(const struct pc_jtok *t)
{
	return t->type == PC_J_STR ? t->start - 1 : t->start;
}
static int ext_end(const struct pc_jtok *t)
{
	return t->type == PC_J_STR ? t->end + 1 : t->end;
}

/* parse one reply line; returns 0 with either *result (malloc'd raw
 * JSON) or errbuf filled (server error member), and *id_out set.
 * Returns 1 for an id-less notification (dispatched), -1 on garbage. */
static int parse_line(perfd_t *p, const char *line, size_t n,
		unsigned long long *id_out, char **result, char *errbuf,
		size_t errcap)
{
	struct pc_jtok *t;
	int maxt = (int)(n / 2) + 8, ntok, ti, tr;

	*result = NULL;
	errbuf[0] = 0;
	if (maxt > 262144)
		maxt = 262144;
	/* Scratch, REUSED across replies.  This was a malloc+free per
	 * reply - about 2KB for a 250-byte line - and it is the second
	 * allocation the binary path does not make.  musl's allocator
	 * serialises those across threads: a musl-built client peaked at
	 * TWO threads and then went backwards, 21.5x behind glibc at
	 * eight, which made every containerised JSON figure a measurement
	 * of the allocator rather than the dialect.
	 *
	 * The array never escapes this function, and a handle is owned by
	 * one thread at a time (see the threading note in perfd.h), so one
	 * buffer per handle is enough.  It only grows; a realloc that
	 * fails leaves the old buffer intact, hence the temporary. */
	if (maxt > p->tokcap) {
		struct pc_jtok *nt = realloc(p->tok,
			sizeof *nt * (size_t)maxt);

		if (!nt)
			return -1;
		p->tok = nt;
		p->tokcap = maxt;
	}
	t = p->tok;
	ntok = pc_json_parse(line, n, t, maxt);
	if (ntok <= 0 || t[0].type != PC_J_OBJ)
		return -1;
	ti = pc_json_get(line, t, ntok, 0, "id");
	if (ti < 0 || t[ti].type != PC_J_PRIM) {
		/* a notification: membership is acted on here (S107), then
		 * the application's hook sees it */
		notify_internal(p, line, n);
		if (p->ncb)
			p->ncb(line, n, p->nctx);
		return 1;
	}
	{
		char num[24];
		int l = t[ti].end - t[ti].start;

		if (l <= 0 || l >= (int)sizeof num)
			return -1;
		memcpy(num, line + t[ti].start, (size_t)l);
		num[l] = 0;
		*id_out = strtoull(num, NULL, 10);
	}
	{
		/* RV-10: "moved" beside the result - the daemon had to forward
		 * this request, and this is the stamp of the map it routed by.
		 * Noted here, acted on after the call (hint_check). */
		int tmv = pc_json_get(line, t, ntok, 0, "moved");

		if (tmv >= 0 && t[tmv].type == PC_J_OBJ) {
			int tt = pc_json_get(line, t, ntok, tmv, "term");
			int ts = pc_json_get(line, t, ntok, tmv, "seq");

			if (tt >= 0 && ts >= 0) {
				p->hint_term = (unsigned int)strtoul(
					line + t[tt].start, NULL, 10);
				p->hint_seq = (unsigned int)strtoul(
					line + t[ts].start, NULL, 10);
				p->hint_seen = 1;
			}
		}
	}
	tr = pc_json_get(line, t, ntok, 0, "result");
	if (tr >= 0) {
		int a = ext_start(&t[tr]), b = ext_end(&t[tr]);

		*result = malloc((size_t)(b - a) + 1);
		if (*result) {
			memcpy(*result, line + a, (size_t)(b - a));
			(*result)[b - a] = 0;
		}
		return *result ? 0 : -1;
	}
	tr = pc_json_get(line, t, ntok, 0, "error");
	if (tr >= 0 && t[tr].type == PC_J_OBJ) {
		int tm = pc_json_get(line, t, ntok, tr, "message");

		if (tm >= 0 && t[tm].type == PC_J_STR) {
			int l = t[tm].end - t[tm].start;

			if (l >= (int)errcap)
				l = (int)errcap - 1;
			memcpy(errbuf, line + t[tm].start, (size_t)l);
			errbuf[l] = 0;
		} else {
			snprintf(errbuf, errcap, "server error");
		}
		return 0;
	}
	return -1;
}

/* Extract ONE message from p->rbuf WITHOUT touching the socket.  The
 * stream mixes dialects per message (each reply self-describes on its
 * first byte), so this is the ONE reader for both: text replies come
 * back as a malloc'd JSON cstring (bin 0), binary replies as the
 * malloc'd payload bytes (bin 1).  Keeping it socket-free is what lets
 * the blocking collector and the event-loop drain (S32) share a single
 * wire implementation instead of growing a second one.
 *   1 = message extracted; got, result, rlen, bin and ebuf are set
 *   2 = a notification was consumed; nothing extracted, call again
 *   0 = incomplete, need more bytes in rbuf
 *  -1 = fatal, connection poisoned
 * A 1 with a NULL result means the daemon returned an error member for
 * that id, and ebuf holds it. */
static int take_msg(perfd_t *p, unsigned long long *got, char **result,
		size_t *rlen, int *bin, char *ebuf, size_t ecap)
{
	*got = 0;
	*result = NULL;
	*rlen = 0;
	*bin = 0;
	if (ecap)
		ebuf[0] = 0;
	if (p->rlen == p->roff)
		return 0;

	{
	char *rb = p->rbuf + p->roff;          /* unread window */
	size_t rn = p->rlen - p->roff;

	if ((unsigned char)rb[0] == BIN_MAGIC) {
		uint32_t plen;
		int type;

		if (rn < BIN_HDR)
			return 0;
		if (rb[1] != BIN_VER) {
			p->poisoned = 1;
			return err(p, "bad frame version");
		}
		plen = rle32(rb + 4);
		if (plen > BIN_MAX_RSP) {
			p->poisoned = 1;
			return err(p, "oversized frame");
		}
		if (rn < BIN_HDR + plen)
			return 0;
		type = (unsigned char)rb[2];
		*got = rle64(rb + 8);
		if (type == BIN_NOTIFY) {
			notify_internal(p, rb + BIN_HDR, plen);
			if (p->ncb)
				p->ncb(rb + BIN_HDR, plen, p->nctx);
		} else if (type != BIN_RSP) {
			p->poisoned = 1;
			return err(p, "bad frame type");
		} else if (rb[3] & BIN_F_ERR) {
			/* ecap is a parameter now, so guard the empty case:
			 * ecap - 1 would wrap and turn this into an overflow */
			size_t cap = ecap ? ecap - 1 : 0;
			size_t el = plen < cap ? plen : cap;

			if (cap) {
				memcpy(ebuf, rb + BIN_HDR, el);
				ebuf[el] = 0;
			}
		} else {
			*result = malloc(plen ? plen : 1);
			if (!*result) {
				p->poisoned = 1;
				return err(p, "out of memory");
			}
			memcpy(*result, rb + BIN_HDR, plen);
			*rlen = plen;
		}
		*bin = 1;
		p->roff += BIN_HDR + plen;     /* consume by offset */
		return type == BIN_NOTIFY ? 2 : 1;
	} else {
		char *nl = memchr(rb, '\n', rn);
		size_t ll;
		int rc;

		if (!nl)
			return 0;
		ll = (size_t)(nl - rb);
		rc = parse_line(p, rb, ll, got, result, ebuf, ecap);
		p->roff += ll + 1;             /* consume by offset */
		if (rc == 1)
			return 2;                      /* notification */
		if (rc < 0) {
			p->poisoned = 1;
			free(*result);
			*result = NULL;
			return err(p, "unparseable reply");
		}
		if (*result)
			*rlen = strlen(*result);
		return 1;
	}
	}
}

/* park an out-of-order reply until its turn comes (blocking collector) */
static int park_msg(perfd_t *p, unsigned long long got, char *result,
		size_t rlen, int bin, const char *ebuf)
{
	struct early *e = malloc(sizeof *e);

	if (!e) {
		free(result);
		p->poisoned = 1;
		return err(p, "out of memory");
	}
	e->id = got;
	e->result = result;
	e->rlen = rlen;
	e->bin = bin;
	snprintf(e->err, sizeof e->err, "%s", ebuf);
	e->next = p->early;
	p->early = e;
	return 0;
}

/* read replies until the FIFO head's arrives, parking the ones that
 * overtake it.  Returns 0, or -1 with p->err set (server error member /
 * error frame / transport). */
static int collect_head_ex(perfd_t *p, char **out, size_t *outlen, int *bin)
{
	unsigned long long want;
	struct early **pe, *e;

	*out = NULL;
	*outlen = 0;
	*bin = 0;
	if (p->idhead >= p->nids)
		return err(p, "no reply pending");
	want = p->ids[p->idhead];

	/* parked already? */
	for (pe = &p->early; (e = *pe); pe = &e->next)
		if (e->id == want) {
			int ok = e->result != NULL;

			*pe = e->next;
			p->idhead++;
			if (p->idhead == p->nids)
				p->nids = p->idhead = 0;
			*out = e->result;
			*outlen = e->rlen;
			*bin = e->bin;
			if (!ok)
				snprintf(p->err, sizeof p->err, "%s", e->err);
			free(e);
			return ok ? 0 : -1;
		}

	for (;;) {
		unsigned long long got;
		char *result, ebuf[256];
		size_t rlen;
		int isbin, rc;

		rc = take_msg(p, &got, &result, &rlen, &isbin, ebuf,
			sizeof ebuf);
		if (rc < 0)
			return -1;
		if (rc == 2)
			continue;
		if (rc == 0) {
			if (p->poisoned)
				return err(p, "connection poisoned");
			if (fill(p) != 0)
				return -1;
			continue;
		}
		if (got == want) {
			p->idhead++;
			if (p->idhead == p->nids)
				p->nids = p->idhead = 0;
			*out = result;
			*outlen = rlen;
			*bin = isbin;
			if (!result) {
				snprintf(p->err, sizeof p->err, "%s", ebuf);
				return -1;
			}
			return 0;
		}
		if (park_msg(p, got, result, rlen, isbin, ebuf) != 0)
			return -1;
	}
}


/* the text-only view (typed text verbs, perfd_command, the pipeline) */
static char *collect_head(perfd_t *p)
{
	char *buf;
	size_t n;
	int bin;

	if (collect_head_ex(p, &buf, &n, &bin) != 0)
		return NULL;
	if (bin) {
		free(buf);
		err(p, "binary reply to a text request");
		return NULL;
	}
	return buf;
}

/* ---- request building -------------------------------------------------- */

static int push_id(perfd_t *p, unsigned long long id)
{
	if (p->nids == p->idcap) {
		int nc = p->idcap ? p->idcap * 2 : 64;
		unsigned long long *ni = realloc(p->ids,
			sizeof *ni * (size_t)nc);

		if (!ni)
			return err(p, "out of memory");
		p->ids = ni;
		p->idcap = nc;
	}
	p->ids[p->nids++] = id;
	return 0;
}

/* envelope + queue.  @params_json NULL = no params member. */
static int enqueue(perfd_t *p, const char *method, const char *params_json)
{
	size_t need = strlen(method) + (params_json ? strlen(params_json)
		: 0) + 96;
	struct pc_jw w;
	char *at;

	if (p->poisoned)
		return err(p, "connection poisoned");
	if (p->qlen + need > p->qcap) {
		while (p->qcap < p->qlen + need)
			p->qcap = p->qcap ? p->qcap * 2 : 65536;
		{
			void *nb = realloc(p->q, p->qcap);

			if (!nb) {
				p->poisoned = 1;
				return err(p, "out of memory");
			}
			p->q = nb;
		}
	}
	at = p->q + p->qlen;
	pc_jw_init(&w, at, p->qcap - p->qlen);
	pc_jw_lit(&w, "{\"jsonrpc\":\"2.0\",\"id\":");
	pc_jw_i64(&w, (long long)++p->next_id);
	pc_jw_lit(&w, ",\"method\":");
	pc_jw_str(&w, method, strlen(method));
	if (params_json) {
		pc_jw_lit(&w, ",\"params\":");
		pc_jw_raw(&w, params_json, strlen(params_json));
	}
	pc_jw_lit(&w, "}\n");
	if (w.overflow)
		return err(p, "request too large");
	/* the in-order FIFO belongs to the blocking collector; an async
	 * handle correlates by callback instead, and would otherwise grow
	 * this array for the life of the connection */
	if (!p->async && push_id(p, p->next_id) != 0)
		return -1;
	p->qlen += w.len;
	return 0;
}

/* frame + queue one binary data-verb request (layouts: src/proto.h).
 * @val doubles as the ping echo payload. */
static int enqueue_bin(perfd_t *p, int verb, const char *col,
		const char *key, const void *val, size_t vlen,
		long long by, long long ttl)
{
	size_t cn = col ? strlen(col) : 0, kn = key ? strlen(key) : 0;
	size_t fixed = 0, plen, need;
	unsigned char *at;

	if (p->poisoned)
		return err(p, "connection poisoned");
	if (verb != BV_PING) {
		if (!cn || cn > 255)
			return err(p, "bad collection name");
		if (!kn || kn > KEY_MAX)
			return err(p, "bad key");
		fixed = 3;
		if (verb == BV_EXPIRE || verb == BV_SET)
			fixed += 8;
		else if (verb == BV_ADD || verb == BV_SUB)
			fixed += 16;
	}
	plen = 1 + fixed + cn + kn + vlen;
	need = BIN_HDR + plen;
	if (p->qlen + need > p->qcap) {
		while (p->qcap < p->qlen + need)
			p->qcap = p->qcap ? p->qcap * 2 : 65536;
		{
			void *nb = realloc(p->q, p->qcap);

			if (!nb) {
				p->poisoned = 1;
				return err(p, "out of memory");
			}
			p->q = nb;
		}
	}
	at = (unsigned char *)p->q + p->qlen;
	at[0] = BIN_MAGIC;
	at[1] = BIN_VER;
	at[2] = BIN_REQ;
	at[3] = 0;
	wle32(at + 4, (unsigned int)plen);
	wle64(at + 8, ++p->next_id);
	at += BIN_HDR;
	*at++ = (unsigned char)verb;
	if (verb != BV_PING) {
		*at++ = (unsigned char)cn;
		wle16(at, (unsigned int)kn);
		at += 2;
		if (verb == BV_EXPIRE || verb == BV_SET) {
			wle64(at, (unsigned long long)ttl);
			at += 8;
		} else if (verb == BV_ADD || verb == BV_SUB) {
			wle64(at, (unsigned long long)by);
			at += 8;
			wle64(at, (unsigned long long)ttl);
			at += 8;
		}
		memcpy(at, col, cn);
		at += cn;
		memcpy(at, key, kn);
		at += kn;
	}
	if (vlen)
		memcpy(at, val, vlen);
	if (push_id(p, p->next_id) != 0)
		return -1;
	p->qlen += need;
	return 0;
}

/* one-shot on the binary dialect: refuse mid-pipeline, send, collect.
 * 0 = *out is the malloc'd response payload (*outlen bytes). */
/* ---- cluster awareness (task S34) ---------------------------------------
 * The daemon already knows the fleet; a client that learns it can keep
 * STANDBY connections open and swap onto one when a node dies, instead
 * of discovering the failure and then paying a TCP + Noise handshake.
 *
 * The design rule that keeps this safe: nothing here is load-bearing
 * for correctness.  A stale member list, a policy that picks a busy
 * node, a failover that lands somewhere unexpected - all of it costs a
 * hop at worst, because the daemon re-checks ownership itself. */

static int res_parse(const char *res, struct pc_jtok **tp);

static long long now_ms_lib(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* the verbs that may be REPLAYED after a failover.  The store is
 * absolute-upsert, so a repeated set/del/expire is the same as one, and
 * so is a jset or jdel (a path written or removed twice is the same
 * document) - but add/sub and jincr are NOT idempotent and a silent
 * replay would double an increment.  Those surface the error instead;
 * the caller decides. */
static int method_idempotent(const char *m)
{
	static const char *const ok[] = { "get", "set", "del", "exists",
		"ttl", "expire", "mget", "mset", "keys", "scan", "ping",
		"stats", "members", "jget", "jset", "jdel", NULL };
	int i;

	for (i = 0; ok[i]; i++)
		if (!strcmp(m, ok[i]))
			return 1;
	return 0;
}

static int bin_verb_idempotent(int verb)
{
	/* the two counter verbs are the only non-idempotent binary ops */
	return verb != BV_ADD && verb != BV_SUB;
}

/* parse a members reply into p->mem[] */
static int parse_members(perfd_t *p, const char *res)
{
	struct pc_jtok *t = NULL;
	int ntok, tm, i, n = 0;

	ntok = res_parse(res, &t);
	if (ntok < 1)
		return -1;
	tm = pc_json_get(res, t, ntok, 0, "members");
	if (tm < 0 || t[tm].type != PC_J_ARR) {
		free(t);
		return -1;
	}
	/* S70: the daemon's version rides the same reply; absent on a
	 * daemon older than the field, and then it stays "" */
	{
		int tv = pc_json_get(res, t, ntok, 0, "version");

		if (tv >= 0) {
			size_t vl = (size_t)(t[tv].end - t[tv].start);

			if (vl < sizeof p->srv_version) {
				memcpy(p->srv_version, res + t[tv].start, vl);
				p->srv_version[vl] = 0;
			}
		}
	}
	for (i = tm + 1; i < ntok && n < PERFD_MAX_MEMBERS; i++) {
		int ta, tp, tn, tmst, tf, tt;
		size_t al;

		if (t[i].parent != tm || t[i].type != PC_J_OBJ)
			continue;
		ta = pc_json_get(res, t, ntok, i, "addr");
		tp = pc_json_get(res, t, ntok, i, "port");
		tn = pc_json_get(res, t, ntok, i, "node");
		if (ta < 0 || tp < 0 || tn < 0)
			continue;
		al = (size_t)(t[ta].end - t[ta].start);
		if (al >= sizeof p->mem[0].addr)
			continue;
		memcpy(p->mem[n].addr, res + t[ta].start, al);
		p->mem[n].addr[al] = 0;
		p->mem[n].port = atoi(res + t[tp].start);
		p->mem[n].node = atoi(res + t[tn].start);
		tmst = pc_json_get(res, t, ntok, i, "master");
		p->mem[n].master = tmst >= 0 &&
			!strncmp(res + t[tmst].start, "true", 4);
		{
			/* the node answering us marks itself "self" - that is
			 * where this handle currently is */
			int ts = pc_json_get(res, t, ntok, i, "self");

			if (ts >= 0 && !strncmp(res + t[ts].start, "true", 4))
				p->active_node = atoi(res + t[tn].start);
		}
		tf = pc_json_get(res, t, ntok, i, "free_mb");
		tt = pc_json_get(res, t, ntok, i, "total_mb");
		p->mem[n].free_mb = tf >= 0 ? atoi(res + t[tf].start) : 0;
		p->mem[n].total_mb = tt >= 0 ? atoi(res + t[tt].start) : 0;
		{
			/* C7: "ready" | "recovering" | "starting" | "draining".
			 * DRAINING counts as usable: it is being emptied, not
			 * doubted, and still answers for what it holds. */
			int ts2 = pc_json_get(res, t, ntok, i, "state");
			size_t sl = ts2 >= 0 ?
				(size_t)(t[ts2].end - t[ts2].start) : 0;
			const char *sv = ts2 >= 0 ? res + t[ts2].start : NULL;

			p->mem[n].ready = !sv ||
				(sl == 5 && !strncmp(sv, "ready", 5)) ||
				(sl == 8 && !strncmp(sv, "draining", 8));
		}
		p->mem[n].down = 0;
		if (!p->mem[n].port)
			continue;      /* a node that cannot be dialled */
		n++;
	}
	/* the ROUTING CONTRACT: only route when the daemon names an
	 * algorithm we actually implement, and a mode where routing means
	 * something.  An unknown algo or a proxy cluster simply leaves
	 * routing off - the client still works, it just pays the forward. */
	{
		int tr = pc_json_get(res, t, ntok, 0, "routing");

		p->route_ok = 0;
		if (tr >= 0) {
			int ta = pc_json_get(res, t, ntok, tr, "algo");
			int tmd = pc_json_get(res, t, ntok, tr, "mode");
			int tcp = pc_json_get(res, t, ntok, tr, "cport");
			int algo_ok = ta >= 0 &&
				(size_t)(t[ta].end - t[ta].start) ==
					strlen(PERFD_ROUTE_ALGO) &&
				!memcmp(res + t[ta].start, PERFD_ROUTE_ALGO,
					strlen(PERFD_ROUTE_ALGO));
			/* S127: `spread` routes too - the holders are the top
			 * K of the same rendezvous ranking, so shard is this
			 * with K = 1.  An OLDER library does not know the
			 * word, fails this test, turns routing off and dials
			 * any node; the daemon forwards, so it stays correct
			 * and only pays a pull per read. */
			int tk = pc_json_get(res, t, ntok, tr, "replicas");
			int mode_ok = tmd >= 0 &&
				(!strncmp(res + t[tmd].start, "shard", 5) ||
				 !strncmp(res + t[tmd].start, "spread", 6) ||
				 !strncmp(res + t[tmd].start, "store", 5));

			p->route_k = tk >= 0 ? atoi(res + t[tk].start) : 0;
			if (p->route_k < 1)
				p->route_k = 1;

			if (tcp >= 0)
				p->route_cport = atoi(res + t[tcp].start);
			{
				/* RV-10: which map this list belongs to */
				int tmap = pc_json_get(res, t, ntok, tr, "map");
				int tt = tmap >= 0 ? pc_json_get(res, t, ntok, tmap,
					"term") : -1;
				int ts = tmap >= 0 ? pc_json_get(res, t, ntok, tmap,
					"seq") : -1;

				p->map_known = tt >= 0 && ts >= 0;
				if (p->map_known) {
					p->map_term = (unsigned int)strtoul(
						res + t[tt].start, NULL, 10);
					p->map_seq = (unsigned int)strtoul(
						res + t[ts].start, NULL, 10);
				}
			}
			p->route_ok = p->route_want && algo_ok && mode_ok &&
				p->route_cport > 0;
		}
	}
	free(t);
	p->nmem = n;
	return n;
}

/* ---- per-key routing (task S35) -----------------------------------------
 * With the member list in hand a client can compute WHERE a key
 * belongs and send the request straight there, so the daemon's forward
 * hop disappears.  Two rules, and they exist for different reasons:
 *
 *   shard - the owner is computable, so routing removes a network hop;
 *   store - there is no owner, but hashing each key to ONE node makes
 *           this client a de-facto single writer for it, which is what
 *           keeps concurrent writers from forking a key (par 6.5b).
 *           Spreading and key-stickiness pull opposite ways here, and
 *           stickiness wins for correctness.
 *
 * proxy is NOT routed: its placement depends on live free space and
 * history, so there is nothing a client can compute.
 *
 * THE INVARIANT: none of this is load-bearing for ONE request.  The
 * daemon re-checks ownership and forwards a wrong guess, so a stale
 * member list, an unrecognised algorithm or a missing standby costs one
 * hop and never correctness.  That is why every failure path below
 * simply returns the active handle.  What IS load-bearing is that every
 * keyed verb decides the same way (see roundtrip_key): two requests on
 * one key from one client are ordered because they meet at one node,
 * not because either was routed (S211). */

/* The placement input: the Redis SLOT, from the KEY ALONE.
 *
 * The collection is deliberately not mixed in.  A RESP client knows
 * nothing about collections, so anything it and we can both compute has
 * to ignore them - and the daemon's pc_shard_owner() ignores them for
 * the same reason.  Feeding col in here would put this client on a
 * different node from every Redis client for the same key. */
static unsigned long long route_keyhash(const char *col, const char *key)
{
	(void)col;
	return pc_key_slot(key, strlen(key));
}

/* xorshift over the handle's own cursor: independent per client, which
 * is what makes uncoordinated spreading work (par 5.6) */
static unsigned int pd_rand(perfd_t *p)
{
	unsigned int x = p->rr ? p->rr : 2463534242u;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	p->rr = x;
	return x;
}

/* The @n-th ranked member for @key (0 = the owner), or -1.  Selection
 * rather than a sort: n is at most the copy factor, which is small, and
 * a per-request sort of the whole member list is not worth it. */
static int route_rank_nth(perfd_t *p, const char *col, const char *key, int n)
{
	unsigned long long kh, best = 0;
	unsigned short nport;
	int i, r, pick = -1;
	int taken[PERFD_MAX_MEMBERS];

	if (!p->route_ok || p->nmem < 2 || !col || !key || n < 0)
		return -1;
	if (n >= p->nmem)
		return -1;
	memset(taken, 0, sizeof taken);
	kh = route_keyhash(col, key);
	nport = htons((unsigned short)p->route_cport);
	for (r = 0; r <= n; r++) {
		pick = -1;
		best = 0;
		for (i = 0; i < p->nmem; i++) {
			unsigned long long h;
			struct in_addr ia;

			if (taken[i] || !pd_usable(&p->mem[i]))
				continue;
			if (!inet_aton(p->mem[i].addr, &ia))
				continue;
			h = pc_hrw_mix(kh, (unsigned int)ia.s_addr, nport);
			if (pick < 0 || h > best) {
				best = h;
				pick = i;
			}
		}
		if (pick < 0)
			return -1;
		taken[pick] = 1;
	}
	return pick;
}

/* the member that should hold @key, or -1 when routing does not apply */
static int route_owner(perfd_t *p, const char *col, const char *key)
{
	unsigned long long kh, best = 0;
	unsigned short nport;
	int i, owner = -1;

	if (!p->route_ok || p->nmem < 2 || !col || !key)
		return -1;
	kh = route_keyhash(col, key);
	nport = htons((unsigned short)p->route_cport);
	for (i = 0; i < p->nmem; i++) {
		unsigned long long h;
		struct in_addr ia;

		if (!pd_usable(&p->mem[i]))
			continue;
		if (!inet_aton(p->mem[i].addr, &ia))
			continue;
		h = pc_hrw_mix(kh, (unsigned int)ia.s_addr, nport);
		if (owner < 0 || h > best) {
			best = h;
			owner = i;
		}
	}
	return owner;
}

/* Public: which member should hold @key, or -1 when routing does not
 * apply (no member list learned, fewer than two members, or the cluster
 * did not advertise a contract this client implements).  Pair it with
 * perfd_member_info() to get the node's address.
 *
 * This exists because per-key routing inside the library needs standby
 * CONNECTIONS, and those are unreachable on the async API - an async
 * caller drives one fd per handle and the library cannot open more
 * behind its back.  An async application therefore opens its own handle
 * per node and asks this which one to use, which is the same
 * computation the blocking path does internally. */
int perfd_owner_of(perfd_t *p, const char *col, const char *key)
{
	return p ? route_owner(p, col, key) : -1;
}

/* a standby died under a routed request: drop it, so the next request
 * does not keep trying a corpse.  The member is marked down too, which
 * makes route_owner() pick the next-best node instead. */
static void route_retire(perfd_t *p, perfd_t *dead)
{
	int i, j;

	for (i = 0; i < p->nmem; i++)
		if (p->mem[i].node == dead->active_node)
			p->mem[i].down = 1;
	for (i = 0; i < p->nspare; i++)
		if (p->spare[i] == dead) {
			perfd_free(dead);
			for (j = i; j < p->nspare - 1; j++)
				p->spare[j] = p->spare[j + 1];
			p->nspare--;
			return;
		}
}

/* the handle that should carry this request: ours, a standby already
 * open to the owner, or - when we are not connected to the owner -
 * ours anyway, which just costs the forward we were trying to avoid */
static perfd_t *route_pick(perfd_t *p, const char *col, const char *key)
{
	int owner, i, r;

	if (p->is_child)
		return p;
	/* S127: under spread any of the K holders can answer, so walk the
	 * ranking and take the best one we already have a connection to.
	 * K = 1 makes this exactly the shard behaviour it replaces.  Being
	 * connected to the SECOND-ranked holder beats forwarding through a
	 * node that holds nothing. */
	for (r = 0; r < p->route_k; r++) {
		owner = route_rank_nth(p, col, key, r);
		if (owner < 0)
			break;
		if (p->mem[owner].node == p->active_node)
			return p;              /* we ARE a holder */
		for (i = 0; i < p->nspare; i++)
			if (p->spare[i]->active_node == p->mem[owner].node)
				return p->spare[i];
	}
	p->route_missed++;
	return p;
}

static char *roundtrip_once(perfd_t *p, const char *method,
		const char *params_json);

static int learn_members(perfd_t *p)
{
	char *res = roundtrip_once(p, "members", NULL);
	int rc;

	if (!res)
		return -1;
	rc = parse_members(p, res);
	free(res);
	p->next_refresh_ms = now_ms_lib() + p->refresh_ms;
	return rc;
}

/* open a standby to @m; NULL on failure (a member being down is normal).
 * @connect_ms bounds the dial (0 = the handle's own setting). */
static perfd_t *open_child(perfd_t *p, const struct pd_member *m,
		int connect_ms)
{
	perfd_opts o;
	const char *sec[PERFD_MAX_SECRETS + 1];
	perfd_t *c;
	int i;

	memset(&o, 0, sizeof o);
	for (i = 0; i < p->nsec; i++)
		sec[i] = p->sec[i];
	sec[p->nsec] = NULL;
	o.secrets = p->nsec ? sec : NULL;
	o.connect_timeout_ms = connect_ms > 0 ? connect_ms : p->o_connect_ms;
	o.io_timeout_ms = p->o_io_ms;
	o.binary = p->o_binary;
	o.spares = PERFD_SPARES_NONE;  /* children never recurse */
	o.keepalive_s = p->keepalive_s ? p->keepalive_s : -1;   /* S104 */
	o.idle_ping_ms = -1;           /* a child is pinged by its parent */
	c = perfd_connect(m->addr, m->port, &o);
	if (c) {
		c->is_child = 1;
		c->active_node = m->node;
		c->last_reply_ms = now_ms_lib();   /* the handshake answered */
	}
	return c;
}

/* keep up to want_spares standbys open, on members other than the
 * active one.  Which ones is a RANDOM subset per client: independent
 * random choices spread a big fleet without any coordination. */
static void warm_spares(perfd_t *p)
{
	int want = p->want_spares, i, tries;

	if (p->is_child || want == 0 || p->nmem < 2)
		return;
	if (want < 0 || want > PERFD_MAX_SPARES)
		want = p->nmem - 1 > PERFD_MAX_SPARES ? PERFD_MAX_SPARES
			: p->nmem - 1;
	for (tries = 0; p->nspare < want && tries < p->nmem * 2; tries++) {
		int idx = (int)((p->rr + (unsigned int)tries) % (unsigned int)p->nmem);
		struct pd_member *m = &p->mem[idx];
		perfd_t *c;
		int dup = 0;

		if (m->node == p->active_node || !pd_usable(m))
			continue;
		for (i = 0; i < p->nspare; i++)
			if (p->spare[i]->active_node == m->node)
				dup = 1;
		if (dup)
			continue;
		c = open_child(p, m, 0);
		if (!c) {
			const char *e = perfd_error(NULL);

			m->down = 1;
			/* S70: the reason, cut to the field - the bound is explicit so
			 * the compiler can see the truncation is meant */
			snprintf(m->why, sizeof m->why, "%.*s",
				(int)sizeof m->why - 1, e ? e : "");
			continue;
		}
		p->spare[p->nspare++] = c;
	}
}

/* move a standby's CONNECTION into @dst; @src keeps nothing live */
static void adopt_conn(perfd_t *dst, perfd_t *src)
{
	if (dst->fd >= 0)
		close(dst->fd);
	free(dst->rbuf);
	free(dst->q);
	free(dst->ids);

	dst->fd = src->fd;
	dst->encrypted = src->encrypted;
	dst->cs_send = src->cs_send;
	dst->cs_recv = src->cs_recv;
	dst->rbuf = src->rbuf; dst->rlen = src->rlen; dst->rcap = src->rcap;
	dst->roff = src->roff;         /* the unread window moves with it */
	dst->q = src->q; dst->qlen = src->qlen; dst->qcap = src->qcap;
	dst->ids = src->ids; dst->nids = src->nids;
	dst->idhead = src->idhead; dst->idcap = src->idcap;
	dst->next_id = src->next_id;
	dst->active_node = src->active_node;
	dst->poisoned = 0;

	src->fd = -1;
	src->rbuf = NULL; src->q = NULL; src->ids = NULL;
	src->rlen = src->rcap = src->qlen = src->qcap = 0;
	src->roff = 0;
	src->nids = src->idhead = src->idcap = 0;
}

/* a cheap roundtrip on a link under a short bound, the io timeout
 * restored after.  0 = it answered.  Shared by the idle scan and the
 * failover, which must never adopt a link it has not just seen alive. */
static int probe_child(perfd_t *p, perfd_t *c)
{
	int bound = p->io_ms < 500 ? p->io_ms : 500, rc;
	struct timeval tv = { 0, bound * 1000L };

	setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	rc = perfd_ping(c);
	tv.tv_sec = p->io_ms / 1000;
	tv.tv_usec = (p->io_ms % 1000) * 1000L;
	setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	return rc;
}

/* the standby whose link most recently carried a reply - the one most
 * likely to still be alive; -1 when there is none */
static int pick_spare(perfd_t *p)
{
	int i, best = -1;
	long long bt = -1;

	for (i = 0; i < p->nspare; i++)
		if (p->spare[i]->last_reply_ms > bt) {
			bt = p->spare[i]->last_reply_ms;
			best = i;
		}
	return best;
}

static void drop_spare(perfd_t *p, int k)
{
	int i;

	perfd_free(p->spare[k]);
	for (i = k; i < p->nspare - 1; i++)
		p->spare[i] = p->spare[i + 1];
	p->nspare--;
}

static void member_down(perfd_t *p, int node, const char *prefix,
		const char *why)
{
	int j;

	for (j = 0; j < p->nmem; j++)
		if (p->mem[j].node == node) {
			p->mem[j].down = 1;
			snprintf(p->mem[j].why, sizeof p->mem[j].why, "%s%.*s",
				prefix, (int)(sizeof p->mem[j].why - 1 - strlen(prefix)),
				why ? why : "");
			/* S107: retried in the background from now on */
			if (!p->mem[j].backoff_ms)
				p->mem[j].backoff_ms = 1000;
			p->mem[j].retry_ms = now_ms_lib() + p->mem[j].backoff_ms;
		}
}

/* S104: a FRESH dial to any member this handle has learned - the ones
 * not marked down first, the one that just died last - under a short
 * connect bound, adopted on the first handshake that completes.  The
 * members are the whole fleet as last reported, so this reaches a
 * fleet whose seed is the one node still down.  0 = usable again. */
#define REDIAL_CONNECT_MS 1000

static int redial_any(perfd_t *p)
{
	int pass, i, dead = p->active_node;

	if (p->is_child || !p->nmem)
		return -1;
	for (pass = 0; pass < 2; pass++)
		for (i = 0; i < p->nmem; i++) {
			struct pd_member *m = &p->mem[i];
			perfd_t *c;

			if ((pass == 0) != (!m->down && m->node != dead))
				continue;
			c = open_child(p, m, p->o_connect_ms > 0 &&
				p->o_connect_ms < REDIAL_CONNECT_MS
				? p->o_connect_ms : REDIAL_CONNECT_MS);
			if (!c) {
				member_down(p, m->node, "re-dial: ", perfd_error(NULL));
				continue;
			}
			adopt_conn(p, c);
			perfd_free(c);
			p->active_node = m->node;
			m->down = 0;
			m->why[0] = 0;
			p->last_reply_ms = now_ms_lib();
			p->failovers++;
			p->next_refresh_ms = 0;    /* re-learn, re-warm on the next call */
			return 0;
		}
	return -1;
}

/* The dead connection is replaced.  S104: never by adoption alone - a
 * standby is PROBED first, most recently alive first, and one that
 * fails is closed and its member marked down; when none is alive the
 * handle re-dials fresh to any member it has learned, inside this same
 * failover, so the operation that found the dead active is the only
 * one that can fail - and with the replay under it, not even that.
 * Returns 0 when the handle is usable again. */
static int failover(perfd_t *p)
{
	int k;

	if (p->is_child)
		return -1;
	member_down(p, p->active_node, "", "the link died");
	while ((k = pick_spare(p)) >= 0) {
		perfd_t *c = p->spare[k];

		if (probe_child(p, c) == 0) {
			/* PS5: the stream belonged to the connection that died */
			if (p->udp)
				udp_drop(p, 0);
			adopt_conn(p, c);
			drop_spare(p, k);
			p->last_reply_ms = now_ms_lib();
			p->failovers++;
			/* re-warm in the background of the next call, not now:
			 * the caller is waiting on a request */
			p->next_refresh_ms = 0;
			return 0;
		}
		p->pings++;
		p->pings_failed++;
		member_down(p, c->active_node, "probe: ", perfd_error(c));
		drop_spare(p, k);
	}
	return redial_any(p);
}

int perfd_redial(perfd_t *p)
{
	if (!p)
		return -1;
	if (!p->poisoned)
		return 0;
	if (p->is_child)
		return -1;
	while (p->nspare) {
		/* whatever standbys are still held died with the active, or
		 * would have been adopted; a fresh dial is the honest start */
		int k = pick_spare(p);

		if (probe_child(p, p->spare[k]) == 0) {
			adopt_conn(p, p->spare[k]);
			drop_spare(p, k);
			p->last_reply_ms = now_ms_lib();
			p->failovers++;
			p->next_refresh_ms = 0;
			return 0;
		}
		member_down(p, p->spare[k]->active_node, "probe: ",
			perfd_error(p->spare[k]));
		drop_spare(p, k);
	}
	if (redial_any(p) == 0)
		return 0;
	return err(p, "re-dial: no learned member answered");
}

/* after a successful call: refresh the fleet view and top the spares
 * back up, at most once per refresh_ms */
static void maybe_refresh(perfd_t *p)
{
	if (p->is_child || !p->want_spares)
		return;
	if (now_ms_lib() < p->next_refresh_ms)
		return;
	learn_members(p);
	warm_spares(p);
}

/* S104: a standby is the link most likely to have died unnoticed - it
 * carries nothing until a failover, and a failover used to adopt it
 * blind.  After a successful call, at most once a second, every
 * standby idle longer than idle_ping_ms is pinged under a short bound
 * (a corpse must not hold the caller for the io timeout); one that
 * fails is retired and its member marked down with the reason, so the
 * next failover cannot pick it.  A child is never the pinger. */
static void maybe_ping(perfd_t *p)
{
	long long now = now_ms_lib();
	int i;

	if (p->is_child || !p->idle_ping_ms || now < p->next_ping_ms)
		return;
	p->next_ping_ms = now + 1000;
	for (i = 0; i < p->nspare; ) {
		perfd_t *c = p->spare[i];

		if (now - c->last_reply_ms < p->idle_ping_ms) {
			i++;
			continue;
		}
		p->pings++;
		if (probe_child(p, c) == 0) {
			i++;
			continue;
		}
		p->pings_failed++;
		member_down(p, c->active_node, "idle ping: ", perfd_error(c));
		route_retire(p, c);            /* frees it; nspare shrinks, so
		                                * the same index is the next one */
	}
}

/* ---- S107: membership pushed by the fleet, members recovered ------------ */

static int spare_for(perfd_t *p, int node)
{
	int i;

	for (i = 0; i < p->nspare; i++)
		if (p->spare[i]->active_node == node)
			return i;
	return -1;
}

/* open a standby to member @m under a short bound and hold it, when
 * standbys are wanted and there is room.  0 = the member is reachable. */
static int member_bring_up(perfd_t *p, struct pd_member *m)
{
	perfd_t *c;
	int cap = p->want_spares < 0 || p->want_spares > PERFD_MAX_SPARES
		? PERFD_MAX_SPARES : p->want_spares;

	c = open_child(p, m, 200);
	if (!c) {
		const char *e = perfd_error(NULL);

		snprintf(m->why, sizeof m->why, "re-dial: %.*s",
			(int)sizeof m->why - 10, e ? e : "");
		return -1;
	}
	m->down = 0;
	m->why[0] = 0;
	m->backoff_ms = 0;
	if (p->want_spares && m->node != p->active_node &&
	        spare_for(p, m->node) < 0 && p->nspare < cap)
		p->spare[p->nspare++] = c;
	else
		perfd_free(c);
	return 0;
}

/* "joined": a member the fleet just admitted - add it, dial it */
static void member_join(perfd_t *p, const char *addr, int port, int node)
{
	struct pd_member *m = NULL;
	int i;

	for (i = 0; i < p->nmem; i++)
		if (p->mem[i].node == node)
			m = &p->mem[i];
	if (!m) {
		if (p->nmem >= PERFD_MAX_MEMBERS)
			return;
		m = &p->mem[p->nmem++];
		memset(m, 0, sizeof *m);
	}
	snprintf(m->addr, sizeof m->addr, "%s", addr);
	m->port = port;
	m->node = node;
	m->ready = 1;
	m->down = 0;
	m->why[0] = 0;
	if (node != p->active_node)
		member_bring_up(p, m);
}

/* "expelled": drop it completely - member, standby, routing weight; a
 * later heartbeat from that identity is a new member, not a return */
static void member_expel(perfd_t *p, int node)
{
	int i, k = spare_for(p, node);

	if (k >= 0) {
		perfd_free(p->spare[k]);
		for (i = k; i < p->nspare - 1; i++)
			p->spare[i] = p->spare[i + 1];
		p->nspare--;
	}
	for (i = 0; i < p->nmem; i++)
		if (p->mem[i].node == node) {
			memmove(&p->mem[i], &p->mem[i + 1],
				(size_t)(p->nmem - i - 1) * sizeof p->mem[0]);
			p->nmem--;
			break;
		}
	if (node == p->active_node)
		p->active_stale = 1;           /* fail over before the next call */
}

/* an id-less message from the daemon: act on membership before the
 * application's hook sees it.  Children carry no member table; their
 * parent hears the same push on its own link. */
static void notify_internal(perfd_t *p, const char *json, size_t n)
{
	struct pc_jtok t[64];
	int ntok, tk, te, ta, tp, tn;
	char addr[46], event[16];
	int port, node;

	if (p->is_child)
		return;
	ntok = pc_json_parse(json, n, t, 64);
	if (ntok <= 0 || t[0].type != PC_J_OBJ)
		return;
	/* PS5: the daemon dropped the UDP stream and every subscription */
	tk = pc_json_get(json, t, ntok, 0, "method");
	if (tk >= 0 && p->udp && (size_t)(t[tk].end - t[tk].start) == 17 &&
	        memcmp(json + t[tk].start, "pubsub_udp_pruned", 17) == 0) {
		udp_drop(p, 1);
		return;
	}
	if (p->ignore_push)
		return;                        /* RV-10: a test's stale map */
	tk = pc_json_get(json, t, ntok, 0, "notify");
	if (tk < 0 || (size_t)(t[tk].end - t[tk].start) != 10 ||
	        memcmp(json + t[tk].start, "membership", 10) != 0)
		return;
	te = pc_json_get(json, t, ntok, 0, "event");
	ta = pc_json_get(json, t, ntok, 0, "addr");
	tp = pc_json_get(json, t, ntok, 0, "port");
	tn = pc_json_get(json, t, ntok, 0, "node");
	if (te < 0 || ta < 0 || tp < 0 || tn < 0)
		return;
	if ((size_t)(t[te].end - t[te].start) >= sizeof event ||
	        (size_t)(t[ta].end - t[ta].start) >= sizeof addr)
		return;
	memcpy(event, json + t[te].start, (size_t)(t[te].end - t[te].start));
	event[t[te].end - t[te].start] = 0;
	memcpy(addr, json + t[ta].start, (size_t)(t[ta].end - t[ta].start));
	addr[t[ta].end - t[ta].start] = 0;
	port = atoi(json + t[tp].start);
	node = atoi(json + t[tn].start);
	if (!node)
		return;
	if (!strcmp(event, "joined") && port > 0)
		member_join(p, addr, port, node);
	else if (!strcmp(event, "expelled"))
		member_expel(p, node);
}

/* whatever the daemon pushed while nothing was being read: a bounded,
 * non-blocking look at the socket, so a push is acted on at the next
 * call rather than at the next reply */
static void drain_notifications(perfd_t *p)
{
	int rounds;

	if (p->is_child || p->fd < 0 || p->poisoned)
		return;
	for (rounds = 0; rounds < 8; rounds++) {
		struct pollfd pf;
		unsigned long long got;
		char *result;
		size_t rlen;
		int bin, rc;
		char ebuf[256];

		pf.fd = p->fd;
		pf.events = POLLIN;
		if (poll(&pf, 1, 0) <= 0)
			return;
		if (fill(p) != 0)
			return;
		for (;;) {
			rc = take_msg(p, &got, &result, &rlen, &bin, ebuf,
				sizeof ebuf);
			if (rc == 2)
				continue;
			if (rc == 1) {
				/* a reply nobody is waiting for yet: keep it */
				park_msg(p, got, result, rlen, bin, ebuf);
				continue;
			}
			break;
		}
		if (rc < 0)
			return;
	}
}

/* S107: members marked down are re-dialled in the background, one per
 * scan, at most once a second, on a backoff of 1 s doubling to 30 s -
 * while every request keeps flowing over the live links */
static void maybe_recover(perfd_t *p)
{
	long long now = now_ms_lib();
	int i;

	if (p->is_child || now < p->next_recover_ms)
		return;
	p->next_recover_ms = now + 1000;
	for (i = 0; i < p->nmem; i++) {
		struct pd_member *m = &p->mem[i];

		if (!m->down || now < m->retry_ms || m->node == p->active_node)
			continue;
		if (member_bring_up(p, m) == 0) {
			p->recovered++;
			return;
		}
		m->backoff_ms = m->backoff_ms < 15000 ? m->backoff_ms * 2 : 30000;
		m->retry_ms = now + m->backoff_ms;
		return;                        /* one dial per scan */
	}
}

/* every successful call lands here: the link is known good NOW */
/* RV-10: the reply that just arrived on @t said the daemon had to forward
 * it.  If this handle ROUTES, and the map the daemon routed by is not the
 * one the member list was learned under, the handle is behind: re-learn
 * now - maybe_refresh() does it right behind this - instead of at the
 * next push or the next refresh_ms.  A handle that does not route asks
 * any node on purpose, and the same stamp means the forward was not this
 * handle's doing (a holder mid-convergence); neither re-learns.  At most
 * once a second: a node that is itself behind must not make a client
 * ask for the member list on every reply. */
static void hint_check(perfd_t *p, perfd_t *t)
{
	long long now;

	if (!t->hint_seen)
		return;
	t->hint_seen = 0;
	if (!p->route_want)
		return;
	if (p->map_known && t->hint_term == p->map_term &&
	        t->hint_seq == p->map_seq)
		return;
	now = now_ms_lib();
	if (now < p->hint_next_ms)
		return;
	p->hint_next_ms = now + 1000;
	p->moved_hints++;
	p->next_refresh_ms = 0;
}

static void after_success(perfd_t *p)
{
	p->last_reply_ms = now_ms_lib();
	hint_check(p, p);
	maybe_refresh(p);
	maybe_ping(p);
	drain_notifications(p);
	maybe_recover(p);
}

/* S107: a call routed to a standby (S35) is a successful call too, and
 * the handle's maintenance - the pushes waiting on its own link, the
 * idle standbys, the members to recover - must not wait for a key that
 * happens to live on the active node.  Measured: a client whose keys
 * all routed away from its active never read the "joined" push sitting
 * in that socket, and learned nothing for as long as that held.  The
 * active's own last_reply_ms is NOT touched: it did not answer. */
static void after_routed(perfd_t *p, perfd_t *t)
{
	t->last_reply_ms = now_ms_lib();   /* the standby did */
	hint_check(p, t);
	maybe_refresh(p);
	maybe_ping(p);
	drain_notifications(p);
	maybe_recover(p);
}

int perfd_maintain(perfd_t *p)
{
	if (!p || p->is_child)
		return 0;
	if (p->poisoned)
		return -1;
	drain_notifications(p);
	maybe_ping(p);
	maybe_recover(p);
	return 0;
}

unsigned long long perfd_recovered(const perfd_t *p)
{
	return p ? p->recovered : 0;
}

/* choose the member this handle should work through, per policy, and
 * move onto it if it is not where we already are */
static void apply_policy(perfd_t *p)
{
	int i, best = -1, bestfree = -1;

	if (p->is_child || p->policy == PERFD_POLICY_FAILOVER || p->nmem < 2)
		return;
	switch (p->policy) {
	case PERFD_POLICY_LEAST_CONN:
		/* POWER OF TWO CHOICES, not strict least-loaded.  Picking
		 * the emptiest member outright makes every client choose
		 * the SAME one - measured: 11 of 12 clients on one node,
		 * which is the herding par 6.5 already rejects for
		 * write-time placement.  Sampling two and taking the
		 * better spreads without coordination and still avoids
		 * the worst node. */
		if (p->nmem >= 2) {
			int a = (int)(pd_rand(p) % (unsigned int)p->nmem);
			int b = (int)(pd_rand(p) % (unsigned int)p->nmem);

			if (b == a)
				b = (a + 1) % p->nmem;
			if (!pd_usable(&p->mem[a]))
				a = b;
			best = p->mem[a].free_mb >= p->mem[b].free_mb ||
				!pd_usable(&p->mem[b]) ? a : b;
		}
		break;
	case PERFD_POLICY_WEIGHTED: {
		/* random, weighted BY free space - the name's promise.
		 * Taking the maximum is not weighting, it is a tie-break
		 * that every client resolves identically. */
		long total = 0, r;

		for (i = 0; i < p->nmem; i++)
			if (pd_usable(&p->mem[i]))
				total += p->mem[i].free_mb > 0 ?
					p->mem[i].free_mb : 1;
		if (total <= 0)
			break;
		r = (long)(pd_rand(p) % (unsigned int)total);
		for (i = 0; i < p->nmem; i++) {
			if (!pd_usable(&p->mem[i]))
				continue;
			r -= p->mem[i].free_mb > 0 ? p->mem[i].free_mb : 1;
			if (r < 0) {
				best = i;
				break;
			}
		}
		break;
	}
	default: {
		/* round robin: random start, step.  It skipped NOTHING
		 * before C7 - not even a member this client had already
		 * seen fail - so the cursor is advanced past unusable
		 * members rather than landing on one and failing over. */
		int tries;

		best = -1;
		for (tries = 0; tries < p->nmem; tries++) {
			int cand = (int)(p->rr % (unsigned int)p->nmem);

			p->rr++;
			if (pd_usable(&p->mem[cand])) {
				best = cand;
				break;
			}
		}
		break;
	}
	}
	(void)bestfree;
	if (best < 0 || p->mem[best].node == p->active_node)
		return;
	for (i = 0; i < p->nspare; i++)
		if (p->spare[i]->active_node == p->mem[best].node) {
			perfd_t *sp = p->spare[i];
			int j;

			adopt_conn(p, sp);
			perfd_free(sp);
			for (j = i; j < p->nspare - 1; j++)
				p->spare[j] = p->spare[j + 1];
			p->nspare--;
			return;
		}
}

int perfd_member_count(const perfd_t *p)
{
	return p ? p->nmem : 0;
}

int perfd_member_info(const perfd_t *p, int i, char *addr, size_t acap,
		int *port, int *node, int *active)
{
	if (!p || i < 0 || i >= p->nmem)
		return -1;
	if (addr && acap)
		snprintf(addr, acap, "%s", p->mem[i].addr);
	if (port)
		*port = p->mem[i].port;
	if (node)
		*node = p->mem[i].node;
	if (active)
		*active = p->mem[i].node == p->active_node;
	return 0;
}

int perfd_spare_count(const perfd_t *p)
{
	return p ? p->nspare : 0;
}

/* S70: what this handle knows, from its own dials, about member @i.
 * "active" serves this handle; "standby" has a spare open from here;
 * "down" is a member this handle failed to dial, and @why says how -
 * nothing is inferred from anyone else's view; "not-ready" the member
 * itself says it is not serving data yet; "idle" is fine as far as
 * known with no connection held from here. */
const char *perfd_member_state(const perfd_t *p, int i, char *why,
		size_t wcap)
{
	int j;

	if (why && wcap)
		why[0] = 0;
	if (!p || i < 0 || i >= p->nmem)
		return "unknown";
	if (p->mem[i].node == p->active_node)
		return "active";
	for (j = 0; j < p->nspare; j++)
		if (p->spare[j] && p->spare[j]->active_node == p->mem[i].node)
			return "standby";
	if (p->mem[i].down) {
		if (why && wcap)
			snprintf(why, wcap, "%s", p->mem[i].why);
		return "down";
	}
	if (!p->mem[i].ready)
		return "not-ready";
	return "idle";
}

/* S70: the daemon version the member list reported; "" until a member
 * list has been read (standbys off, or a daemon older than the field) */
const char *perfd_server_version(const perfd_t *p)
{
	return p && p->srv_version[0] ? p->srv_version : "";
}

unsigned long long perfd_pings(const perfd_t *p)
{
	return p ? p->pings : 0;
}

unsigned long long perfd_pings_failed(const perfd_t *p)
{
	return p ? p->pings_failed : 0;
}

long long perfd_last_reply_ms(const perfd_t *p)
{
	return p ? p->last_reply_ms : 0;
}

unsigned long long perfd_failovers(const perfd_t *p)
{
	return p ? p->failovers : 0;
}

int perfd_active_node(const perfd_t *p)
{
	return p ? p->active_node : 0;
}

int perfd_routing(const perfd_t *p)
{
	return p ? p->route_ok : 0;
}

unsigned long long perfd_moved_hints(const perfd_t *p)
{
	return p ? p->moved_hints : 0;
}

unsigned long long perfd_route_missed(const perfd_t *p)
{
	return p ? p->route_missed : 0;
}

/*
 * S72: a one-shot call refuses to start unless the pipeline is empty
 * (the guard in each of the two functions below), so anything queued
 * DURING one belongs to that call.  When it fails, drop it.
 *
 * Leaving it behind is not a cosmetic leak: the id was never collected
 * and nothing else will ever collect it, so every LATER call fails the
 * same guard with "pipeline in progress" - on a handle whose caller
 * never pipelined anything.  Worse, that refusal happens before the
 * socket is touched, so `poisoned` is never set and the failover that
 * would have rescued the handle is never reached.  The handle stays
 * dead for the life of the process, and the message sends the reader
 * looking for a pipeline that does not exist.
 *
 * Confirmed in production 2026-09-04 by restarting a perfcached under
 * a live OpenSIPS fleet: one EOF, then "pipeline in progress" forever,
 * on every worker, until OpenSIPS itself was restarted.
 *
 * This does NOT reconnect - that stays the caller's decision, made
 * through perfd_state()/PERFD_ST_FAILED - it only ensures the handle
 * tells the truth about itself afterwards.
 */
static void pipeline_drop(perfd_t *p)
{
	p->nids = p->idhead = 0;
	p->qlen = 0;
}

static int bin_roundtrip_once(perfd_t *p, int verb, const char *col,
		const char *key, const void *val, size_t vlen,
		long long by, long long ttl, char **out, size_t *outlen)
{
	int bin = 0;

	if (p->nids - p->idhead || p->qlen)
		return err(p, "pipeline in progress");
	if (enqueue_bin(p, verb, col, key, val, vlen, by, ttl) != 0 ||
	        perfd_flush(p) != 0) {
		pipeline_drop(p);
		return -1;
	}
	if (collect_head_ex(p, out, outlen, &bin) != 0) {
		pipeline_drop(p);
		return -1;
	}
	if (!bin) {
		free(*out);
		*out = NULL;
		return err(p, "text reply to a binary request");
	}
	return 0;
}

/* the binary dialect's half of the S34 failover; same rule - a dead
 * connection retries once on a standby, a non-idempotent verb does not */
static int bin_roundtrip(perfd_t *p, int verb, const char *col,
		const char *key, const void *val, size_t vlen,
		long long by, long long ttl, char **out, size_t *outlen)
{
	perfd_t *t;
	int rc;

	if (p->active_stale && !p->is_child) {
		p->active_stale = 0;
		failover(p);                   /* S107: it was expelled */
	}
	t = route_pick(p, col, key);       /* S35 */
	if (t != p) {
		/* the owner's standby carries it; a failure there is that
		 * connection's problem, and the caller still has us */
		rc = bin_roundtrip_once(t, verb, col, key, val, vlen, by, ttl,
			out, outlen);
		if (rc == 0) {
			after_routed(p, t);
			return 0;
		}
		if (!t->poisoned)
			return err(p, "%s", perfd_error(t));
		route_retire(p, t);    /* dead standby: fall through to us */
	}
	rc = bin_roundtrip_once(p, verb, col, key, val, vlen, by, ttl,
		out, outlen);

	if (rc == 0) {
		after_success(p);
		return 0;
	}
	if (!p->poisoned || p->is_child || !p->nmem)
		return rc;
	if (!bin_verb_idempotent(verb))
		return err(p, "connection lost during a counter update - NOT "
			"replayed (add/sub are not idempotent); retry if a double "
			"increment is acceptable");
	if (failover(p) != 0)
		return rc;
	rc = bin_roundtrip_once(p, verb, col, key, val, vlen, by, ttl,
		out, outlen);
	if (rc == 0)
		after_success(p);
	return rc;
}

int perfd_append(perfd_t *p, const char *method, const char *params_json)
{
	return enqueue(p, method, params_json);
}

int perfd_flush(perfd_t *p)
{
	if (p->poisoned)
		return err(p, "connection poisoned");
	if (p->qlen && send_bytes(p, p->q, p->qlen) != 0)
		return -1;
	p->qlen = 0;
	return 0;
}

char *perfd_next_reply(perfd_t *p)
{
	if (p->qlen) {
		err(p, "flush before collecting replies");
		return NULL;
	}
	return collect_head(p);
}

int perfd_pending(const perfd_t *p)
{
	return p->nids - p->idhead;
}

/* one-shot: refuse mid-pipeline, send, collect this reply */
static char *roundtrip_once(perfd_t *p, const char *method,
		const char *params_json)
{
	if (p->nids - p->idhead || p->qlen) {
		err(p, "pipeline in progress");
		return NULL;
	}
	if (enqueue(p, method, params_json) != 0 || perfd_flush(p) != 0) {
		pipeline_drop(p);
		return NULL;
	}
	{
		char *r = collect_head(p);

		if (!r)
			pipeline_drop(p);
		return r;
	}
}

static char *roundtrip(perfd_t *p, const char *method,
		const char *params_json);

/* S35: the same request, but on the handle that owns the key.  Used by
 * the typed verbs, which know their collection and key; perfd_command()
 * and the pipeline API stay on the caller's handle, because only the
 * caller knows what an opaque params blob addresses.
 *
 * EVERY verb that names one key comes through here; roundtrip() is for
 * the keyless ones (ping, stats, members, keys, mget, publish).  S211:
 * jdel and jincr took roundtrip() while jset took this, so on an eager
 * fleet a jset landed on the key's owner and the jdel behind it on the
 * primary, where the owner's push had not arrived yet - it deleted
 * nothing and the document survived on every member.  Two operations
 * on one key from one client are ordered only when both take the same
 * road: routing is not load-bearing, uniformity is. */
static char *roundtrip_key(perfd_t *p, const char *method,
		const char *params_json, const char *col, const char *key)
{
	perfd_t *t = route_pick(p, col, key);
	char *r = NULL;

	if (t == p)
		return roundtrip(p, method, params_json);
	r = roundtrip_once(t, method, params_json);
	if (r) {
		after_routed(p, t);
		return r;
	}
	if (!t->poisoned) {
		err(p, "%s", perfd_error(t));
		return NULL;
	}
	route_retire(p, t);            /* dead standby: we carry it */
	if (!method_idempotent(method)) {
		/* S211: the standby may have applied it before it died and
		 * the primary would apply it again - the same rule as the
		 * primary's own failover in roundtrip() */
		err(p, "connection lost during '%s' on the key's node - NOT "
			"replayed (the verb is not idempotent); retry if the "
			"operation is safe to repeat", method);
		return NULL;
	}
	return roundtrip(p, method, params_json);
}

/* S34: one retry on a pre-warmed standby when the CONNECTION died.  A
 * verb-level error (bad params, no such collection) is not a failover
 * and is never retried; nor is a non-idempotent verb, which would risk
 * applying twice - the caller sees the error and decides. */
static char *roundtrip(perfd_t *p, const char *method,
		const char *params_json)
{
	char *r;

	if (p->active_stale && !p->is_child) {
		p->active_stale = 0;
		failover(p);                   /* S107: it was expelled */
	}
	r = roundtrip_once(p, method, params_json);

	if (r) {
		after_success(p);
		return r;
	}
	if (!p->poisoned || p->is_child || !p->nmem)
		return NULL;
	if (!method_idempotent(method)) {
		err(p, "connection lost during '%s' - NOT replayed (the verb "
			"is not idempotent); retry if the operation is safe to "
			"repeat", method);
		return NULL;
	}
	if (failover(p) != 0)
		return NULL;
	r = roundtrip_once(p, method, params_json);
	if (r)
		after_success(p);
	return r;
}

char *perfd_command(perfd_t *p, const char *method,
		const char *params_json)
{
	return roundtrip(p, method, params_json);
}

/* ---- pub/sub (PS4) ------------------------------------------------------ */

static int res_ll(const char *res, const char *field, long long *out);

static int ps_count_call(perfd_t *p, const char *method, const char *field,
		const char *key, const char *name)
{
	char buf[4096 + 64];
	struct pc_jw w;
	char *res;
	long long n = 0;

	if (name) {
		pc_jw_init(&w, buf, sizeof buf - 1);
		pc_jw_lit(&w, "{\"");
		pc_jw_lit(&w, key);
		pc_jw_lit(&w, "\":");
		pc_jw_str(&w, name, strlen(name));
		pc_jw_lit(&w, "}");
		if (w.overflow)
			return -1;
		buf[w.len] = 0;                /* the writer does not terminate */
		res = roundtrip(p, method, w.buf);
	} else {
		res = roundtrip(p, method, NULL);
	}
	if (!res)
		return -1;
	if (res_ll(res, field, &n) != 0) {
		free(res);
		return -1;
	}
	free(res);
	return (int)n;
}

int perfd_subscribe(perfd_t *p, const char *channel)
{
	return channel ? ps_count_call(p, "subscribe", "subscribed", "channel", channel) : -1;
}

int perfd_psubscribe(perfd_t *p, const char *pattern)
{
	return pattern ? ps_count_call(p, "psubscribe", "subscribed", "pattern", pattern) : -1;
}

int perfd_unsubscribe(perfd_t *p, const char *channel)
{
	return ps_count_call(p, "unsubscribe", "subscribed", "channel", channel);
}

int perfd_punsubscribe(perfd_t *p, const char *pattern)
{
	return ps_count_call(p, "punsubscribe", "subscribed", "pattern", pattern);
}

int perfd_publish(perfd_t *p, const char *channel, const void *payload,
		size_t plen)
{
	size_t cap = 64 + 2 * strlen(channel) + 2 * plen;
	char *buf = malloc(cap), *res;
	struct pc_jw w;
	long long n = 0;

	if (!buf)
		return -1;
	pc_jw_init(&w, buf, cap - 1);
	pc_jw_lit(&w, "{\"channel\":");
	pc_jw_str(&w, channel, strlen(channel));
	pc_jw_lit(&w, ",");
	pc_jw_value(&w, "payload", "enc", payload, plen);
	pc_jw_lit(&w, "}");
	if (w.overflow) {
		free(buf);
		return -1;
	}
	buf[w.len] = 0;                        /* the writer does not terminate */
	res = roundtrip(p, "publish", w.buf);
	free(buf);
	if (!res)
		return -1;
	if (res_ll(res, "receivers", &n) != 0) {
		free(res);
		return -1;
	}
	free(res);
	return (int)n;
}

/* ---- pub/sub over UDP (PS5) ----------------------------------------------
 * lib/perfd_push.h has the datagram and the protocol.  The window is the
 * relay's idea (a counter is lost only when it leaves 64 behind the
 * highest seen unseen; inside the window a late one is simply accepted),
 * written again here for this library. */
#define PD_UDP_WAIT_PROBE   1          /* asked; the probe has not come */
#define PD_UDP_CONFIRMING   2          /* async: confirm submitted */
#define PD_UDP_ACTIVE       3
#define PD_UDP_FAILED       4

struct pd_udp {
	int fd;
	int state;
	int port;
	uint64_t stream;
	unsigned char key[PFP_KEY];
	struct sockaddr_in from;           /* the door: address and port */
	uint64_t high, bits;               /* window: bit i = high - i seen */
	unsigned since_ack;
	long long ack_due_ms, probe_deadline_ms;
	int pruned;
	unsigned long long received, gaps, duplicates, rejected, acks;
	unsigned char early[PFP_HDR + PFP_COOKIE + PFP_TAG];
	size_t early_len;                  /* async: a probe that beat the reply */
	char *json;                        /* the hook's message, reused */
	size_t jcap;
};

static void udp_drop(perfd_t *p, int pruned)
{
	struct pd_udp *u = p->udp;

	if (!u)
		return;
	if (u->fd >= 0)
		close(u->fd);
	u->fd = -1;
	sodium_memzero(u->key, sizeof u->key);
	u->state = PD_UDP_FAILED;
	u->pruned = pruned;
}

/* a hex string member of a result into @out, exactly @len bytes */
static int res_hex(const char *res, const char *field, unsigned char *out,
		size_t len)
{
	struct pc_jtok *t;
	char hex[128];
	int ntok = res_parse(res, &t), tv, l;
	size_t got = 0;

	if (ntok < 0)
		return -1;
	tv = pc_json_get(res, t, ntok, 0, field);
	l = tv < 0 || t[tv].type != PC_J_STR ? -1
		: pc_json_unescape(res, &t[tv], hex, sizeof hex);
	free(t);
	if (l != (int)(2 * len) ||
	    sodium_hex2bin(out, len, hex, (size_t)l, NULL, &got, NULL) != 0 ||
	    got != len)
		return -1;
	return 0;
}

static int udp_probe_in(perfd_t *p, const unsigned char *dg, size_t n);

static void udp_confirmed(perfd_t *p, uint64_t seq)
{
	struct pd_udp *u = p->udp;

	u->state = PD_UDP_ACTIVE;
	u->high = seq;
	u->bits = ~0ULL;                   /* what came before is not ours to count */
	u->since_ack = 0;
	u->ack_due_ms = now_ms_lib() + PFP_ACK_MS;
}

/* async: the replies to our own requests */
static void udp_on_reply(char *result, size_t len, const char *errmsg,
		void *ctx)
{
	perfd_t *p = ctx;
	struct pd_udp *u = p->udp;

	(void)len;
	if (u && u->state == PD_UDP_WAIT_PROBE && !u->stream) {
		unsigned char sid[8];

		if (errmsg || !result || res_hex(result, "stream", sid, 8) != 0 ||
		    res_hex(result, "key", u->key, PFP_KEY) != 0) {
			err(p, "pubsub_udp: %s", errmsg ? errmsg : "bad reply");
			udp_drop(p, 0);
		} else {
			u->stream = pfp_get64(sid);
			/* the daemon sends its first probe while it answers, so on
			 * an async handle the probe can be read before this reply */
			if (u->early_len) {
				size_t n = u->early_len;

				u->early_len = 0;
				if (udp_probe_in(p, u->early, n) > 0)
					u->rejected++;
			}
		}
	} else if (u && u->state == PD_UDP_CONFIRMING) {
		long long seq = 0;

		if (errmsg || !result || res_ll(result, "seq", &seq) != 0) {
			err(p, "pubsub_udp_confirm: %s", errmsg ? errmsg : "bad reply");
			udp_drop(p, 0);
		} else {
			udp_confirmed(p, (uint64_t)seq);
		}
	}
	free(result);
}

static void udp_on_ack(char *result, size_t len, const char *errmsg,
		void *ctx)
{
	(void)len;
	(void)errmsg;
	(void)ctx;
	free(result);
}

/* send a request of ours: a round trip on a blocking handle, a submit on
 * an async one.  0, or -1 with the error set */
static int udp_request(perfd_t *p, const char *method, const char *params,
		perfd_reply_cb cb)
{
	char *r;

	if (p->async)
		return perfd_submit(p, method, params, cb, p);
	r = roundtrip(p, method, params);
	if (!r)
		return -1;
	if (cb)
		cb(r, strlen(r), NULL, p);
	else
		free(r);
	return 0;
}

int perfd_pubsub_udp(perfd_t *p, int port, int timeout_ms)
{
	struct pd_udp *u;
	struct sockaddr_in me;
	socklen_t sl = sizeof me;
	char params[64];

	if (!p || p->is_child)
		return -1;
	if (port < 0 || port > 65535)
		return err(p, "port must be 0..65535");
	if (p->udp)
		perfd_pubsub_udp_off(p);
	if (!p->udp) {
		p->udp = calloc(1, sizeof *p->udp);
		if (!p->udp)
			return err(p, "out of memory");
	}
	u = p->udp;
	{
		/* a new stream starts from nothing but keeps the hook's scratch */
		char *json = u->json;
		size_t jcap = u->jcap;

		memset(u, 0, sizeof *u);
		u->json = json;
		u->jcap = jcap;
	}
	u->fd = -1;
	sl = sizeof u->from;
	if (getpeername(p->fd, (struct sockaddr *)&u->from, &sl) != 0 ||
	    u->from.sin_family != AF_INET)
		return err(p, "UDP pushes need an IPv4 connection");
	u->fd = socket(AF_INET, SOCK_DGRAM, 0);
	memset(&me, 0, sizeof me);
	me.sin_family = AF_INET;
	me.sin_port = htons((uint16_t)port);
	if (u->fd < 0 || bind(u->fd, (struct sockaddr *)&me, sizeof me) != 0) {
		err(p, "cannot bind UDP port %d: %s", port, strerror(errno));
		udp_drop(p, 0);
		return -1;
	}
	sl = sizeof me;
	getsockname(u->fd, (struct sockaddr *)&me, &sl);
	u->port = ntohs(me.sin_port);
	fcntl(u->fd, F_SETFL, fcntl(u->fd, F_GETFL) | O_NONBLOCK);
	u->state = PD_UDP_WAIT_PROBE;
	u->probe_deadline_ms = now_ms_lib() + (timeout_ms > 0 ? timeout_ms : 3000);
	snprintf(params, sizeof params, "{\"port\":%d}", u->port);
	if (p->async) {
		if (perfd_submit(p, "pubsub_udp", params, udp_on_reply, p) != 0) {
			udp_drop(p, 0);
			return -1;
		}
		return 0;
	}
	if (udp_request(p, "pubsub_udp", params, udp_on_reply) != 0 ||
	    u->state != PD_UDP_WAIT_PROBE) {
		udp_drop(p, 0);
		return -1;
	}
	/* the probe proves the path; its cookie goes back on the connection */
	while (u->state == PD_UDP_WAIT_PROBE) {
		long long left = u->probe_deadline_ms - now_ms_lib();
		struct pollfd pf;

		if (left <= 0) {
			err(p, "no probe from the daemon in %d ms - UDP does not "
				"reach this client (a firewall or NAT between?)",
				timeout_ms > 0 ? timeout_ms : 3000);
			udp_drop(p, 0);
			perfd_pubsub_udp_off(p);
			return -1;
		}
		pf.fd = u->fd;
		pf.events = POLLIN;
		if (poll(&pf, 1, (int)left) > 0 && perfd_pubsub_udp_ready(p) < 0)
			return -1;
	}
	return u->state == PD_UDP_ACTIVE ? 0 : -1;
}

int perfd_pubsub_udp_fd(const perfd_t *p)
{
	return p && p->udp ? p->udp->fd : -1;
}

int perfd_pubsub_udp_off(perfd_t *p)
{
	int was;

	if (!p || !p->udp)
		return 0;
	was = p->udp->fd >= 0 && p->udp->state == PD_UDP_ACTIVE;
	udp_drop(p, 0);
	p->udp->state = 0;
	if (was && !p->poisoned)
		udp_request(p, "pubsub_udp", "{\"port\":0}",
			p->async ? udp_on_ack : NULL);
	return 0;
}

int perfd_pubsub_udp_stats(const perfd_t *p, struct perfd_udp_stats *s)
{
	memset(s, 0, sizeof *s);
	if (!p || !p->udp)
		return 0;
	s->active = p->udp->state == PD_UDP_ACTIVE;
	s->pruned = p->udp->pruned;
	s->port = p->udp->port;
	s->received = p->udp->received;
	s->gaps = p->udp->gaps;
	s->duplicates = p->udp->duplicates;
	s->rejected = p->udp->rejected;
	s->acks = p->udp->acks;
	return 0;
}

/* a probe datagram from the door: open it and send its cookie back.  0 taken
 * (or a late probe, ignored), 1 not this stream's probe, -1 the stream failed */
static int udp_probe_in(perfd_t *p, const unsigned char *dg, size_t n)
{
	struct pd_udp *u = p->udp;
	unsigned char pt[64], nonce[12];
	unsigned long long ptlen = 0;
	char params[80], hex[2 * PFP_COOKIE + 1];

	if (n < PFP_HDR + PFP_TAG || n > sizeof pt + PFP_HDR ||
	    pfp_get64(dg + 3) != u->stream)
		return 1;
	pfp_nonce(nonce, pfp_get64(dg + 11));
	if (crypto_aead_chacha20poly1305_ietf_decrypt(pt, &ptlen, NULL,
	        dg + PFP_HDR, (unsigned long long)n - PFP_HDR, dg, PFP_HDR,
	        nonce, u->key) != 0 || ptlen != PFP_COOKIE)
		return 1;
	if (u->state != PD_UDP_WAIT_PROBE)
		return 0;                      /* a late probe */
	sodium_bin2hex(hex, sizeof hex, pt, PFP_COOKIE);
	snprintf(params, sizeof params, "{\"cookie\":\"%s\"}", hex);
	u->state = PD_UDP_CONFIRMING;
	if (p->async) {
		if (perfd_submit(p, "pubsub_udp_confirm", params, udp_on_reply, p) != 0) {
			udp_drop(p, 0);
			return -1;
		}
		return 0;
	}
	if (udp_request(p, "pubsub_udp_confirm", params, udp_on_reply) != 0 ||
	    !p->udp || p->udp->state != PD_UDP_ACTIVE) {
		udp_drop(p, 0);
		return -1;
	}
	return 0;
}

/* 1 new, 0 duplicate; counts what left the window unseen */
static int udp_window(struct pd_udp *u, uint64_t seq)
{
	if (seq > u->high) {
		uint64_t d = seq - u->high;

		if (d >= 64) {
			u->gaps += (uint64_t)(64 - __builtin_popcountll(u->bits)) +
				(d - 64);
			u->bits = 1;
		} else {
			u->gaps += d - (uint64_t)__builtin_popcountll(
				u->bits >> (64 - d));
			u->bits = (u->bits << d) | 1;
		}
		u->high = seq;
		return 1;
	}
	if (u->high - seq >= 64)
		return 1;                      /* already counted as a gap: late */
	if (u->bits & (1ULL << (u->high - seq)))
		return 0;
	u->bits |= 1ULL << (u->high - seq);
	return 1;
}

static void udp_deliver(perfd_t *p, const unsigned char *pt, size_t n)
{
	struct pd_udp *u = p->udp;
	size_t plen, clen, dlen, need;
	struct pc_jw w;

	if (n < 7)
		goto bad;
	plen = pfp_get16(pt + 1);
	if (n < 3 + plen + 2)
		goto bad;
	clen = pfp_get16(pt + 3 + plen);
	if (n < 3 + plen + 2 + clen + 4)
		goto bad;
	dlen = pfp_get32(pt + 3 + plen + 2 + clen);
	if (n != 3 + plen + 2 + clen + 4 + dlen)
		goto bad;
	u->received++;
	u->since_ack++;
	if (!p->ncb)
		return;
	/* the JSON a delivery on the connection would have carried */
	need = 128 + 6 * (plen + clen) + 2 * dlen;
	if (need > u->jcap) {
		char *nj = realloc(u->json, need);

		if (!nj)
			return;
		u->json = nj;
		u->jcap = need;
	}
	pc_jw_init(&w, u->json, u->jcap);
	if (pt[0]) {
		pc_jw_lit(&w, "{\"jsonrpc\":\"2.0\",\"method\":\"pmessage\","
			"\"params\":{\"pattern\":");
		pc_jw_str(&w, (const char *)pt + 3, plen);
		pc_jw_lit(&w, ",\"channel\":");
	} else {
		pc_jw_lit(&w, "{\"jsonrpc\":\"2.0\",\"method\":\"message\","
			"\"params\":{\"channel\":");
	}
	pc_jw_str(&w, (const char *)pt + 3 + plen + 2, clen);
	pc_jw_lit(&w, ",");
	pc_jw_value(&w, "payload", "enc",
		(const char *)pt + 3 + plen + 2 + clen + 4, dlen);
	pc_jw_lit(&w, "}}");
	if (!w.overflow)
		p->ncb(w.buf, w.len, p->nctx);
	return;
bad:
	u->rejected++;
}

int perfd_pubsub_udp_ready(perfd_t *p)
{
	struct pd_udp *u = p ? p->udp : NULL;
	unsigned char dg[PFP_MAX + 64], pt[PFP_MAX + 64], nonce[12];
	int rounds, delivered = 0;
	long long now;

	if (!u || u->fd < 0)
		return u && u->pruned ? -1 : 0;
	for (rounds = 0; rounds < 1024; rounds++) {
		struct sockaddr_in src;
		socklen_t sl = sizeof src;
		ssize_t r = recvfrom(u->fd, dg, sizeof dg, 0,
			(struct sockaddr *)&src, &sl);
		unsigned long long ptlen = 0;
		uint64_t seq;

		if (r < 0)
			break;
		if (src.sin_addr.s_addr != u->from.sin_addr.s_addr ||
		    src.sin_port != u->from.sin_port || r < PFP_HDR + PFP_TAG ||
		    dg[0] != PFP_MAGIC || dg[1] != PFP_VERSION) {
			u->rejected++;
			continue;
		}
		if (!u->stream) {
			/* async: the reply naming the stream has not been read
			 * yet - keep a probe for it, it cannot be checked now */
			if (u->state == PD_UDP_WAIT_PROBE && dg[2] == PFP_PROBE &&
			    (size_t)r <= sizeof u->early) {
				memcpy(u->early, dg, (size_t)r);
				u->early_len = (size_t)r;
			} else {
				u->rejected++;
			}
			continue;
		}
		if (dg[2] == PFP_PROBE) {
			int rc = udp_probe_in(p, dg, (size_t)r);

			if (rc < 0)
				return -1;
			if (rc > 0)
				u->rejected++;
			continue;
		}
		if (pfp_get64(dg + 3) != u->stream) {
			u->rejected++;
			continue;
		}
		seq = pfp_get64(dg + 11);
		pfp_nonce(nonce, seq);
		if (crypto_aead_chacha20poly1305_ietf_decrypt(pt, &ptlen, NULL,
		        dg + PFP_HDR, (unsigned long long)r - PFP_HDR, dg, PFP_HDR,
		        nonce, u->key) != 0) {
			u->rejected++;         /* not sealed by the daemon */
			continue;
		}
		if (dg[2] != PFP_MESSAGE || u->state != PD_UDP_ACTIVE)
			continue;
		if (!udp_window(u, seq)) {
			u->duplicates++;
			continue;
		}
		udp_deliver(p, pt, (size_t)ptlen);
		delivered++;
		if (!p->udp || p->udp->fd < 0)
			break;                     /* the hook turned it off */
	}
	u = p->udp;
	if (!u || u->fd < 0)
		return delivered;
	now = now_ms_lib();
	if (u->state == PD_UDP_WAIT_PROBE && p->async &&
	    now >= u->probe_deadline_ms) {
		err(p, "no probe from the daemon - UDP does not reach this client");
		udp_drop(p, 0);
		return -1;
	}
	if (u->state == PD_UDP_ACTIVE &&
	    (u->since_ack >= PFP_ACK_EVERY || now >= u->ack_due_ms)) {
		char params[48];

		snprintf(params, sizeof params, "{\"seq\":%llu}",
			(unsigned long long)u->high);
		u->since_ack = 0;
		u->ack_due_ms = now + PFP_ACK_MS;
		if (udp_request(p, "pubsub_udp_ack", params,
		        p->async ? udp_on_ack : NULL) != 0)
			return -1;
		if (p->udp)
			p->udp->acks++;
	}
	return delivered;
}

/* ---- result helpers ---------------------------------------------------- */
/* tokenize a malloc'd result; caller frees toks.  -1 = not an object */
static int res_parse(const char *res, struct pc_jtok **tp)
{
	size_t n = strlen(res);
	int maxt = (int)(n / 2) + 8, ntok;

	if (maxt > 262144)
		maxt = 262144;
	*tp = malloc(sizeof **tp * (size_t)maxt);
	if (!*tp)
		return -1;
	ntok = pc_json_parse(res, n, *tp, maxt);
	if (ntok <= 0 || (*tp)[0].type != PC_J_OBJ) {
		free(*tp);
		*tp = NULL;
		return -1;
	}
	return ntok;
}

static int res_bool(const char *res, const char *field, int *out)
{
	struct pc_jtok *t;
	int ntok = res_parse(res, &t), tv;

	if (ntok < 0)
		return -1;
	tv = pc_json_get(res, t, ntok, 0, field);
	if (tv < 0 || t[tv].type != PC_J_PRIM) {
		free(t);
		return -1;
	}
	*out = res[t[tv].start] == 't';
	free(t);
	return 0;
}

static int res_ll(const char *res, const char *field, long long *out)
{
	struct pc_jtok *t;
	char num[24];
	int ntok = res_parse(res, &t), tv, l;

	if (ntok < 0)
		return -1;
	tv = pc_json_get(res, t, ntok, 0, field);
	if (tv < 0 || t[tv].type != PC_J_PRIM) {
		free(t);
		return -1;
	}
	l = t[tv].end - t[tv].start;
	if (l <= 0 || l >= (int)sizeof num) {
		free(t);
		return -1;
	}
	memcpy(num, res + t[tv].start, (size_t)l);
	num[l] = 0;
	*out = strtoll(num, NULL, 10);
	free(t);
	return 0;
}

/* extract value (+enc b64) from an object token @obj into a malloc'd
 * buffer.  Returns length or -1. */
static int extract_value(const char *res, const struct pc_jtok *t, int ntok,
		int obj, void **val)
{
	int tv = pc_json_get(res, t, ntok, obj, "value"), te, l;
	char *buf;

	if (tv < 0 || t[tv].type != PC_J_STR)
		return -1;
	l = t[tv].end - t[tv].start;
	buf = malloc((size_t)l + 1);
	if (!buf)
		return -1;
	l = pc_json_unescape(res, &t[tv], buf, (size_t)l + 1);
	if (l < 0) {
		free(buf);
		return -1;
	}
	te = pc_json_get(res, t, ntok, obj, "enc");
	if (te >= 0 && pc_json_streq(res, &t[te], "b64")) {
		l = pc_b64_dec(buf, (size_t)l, buf, (size_t)l + 1);
		if (l < 0) {
			free(buf);
			return -1;
		}
	}
	buf[l] = 0;                    /* NUL-safe: length is authoritative */
	*val = buf;
	return l;
}

/* build "{"col":..,"key":..}" (+extras) into a malloc'd params string */
static char *params_ck(const char *col, const char *key, const char *extra)
{
	size_t cap = strlen(col) + strlen(key) * 6 + (extra ?
		strlen(extra) : 0) + 64;
	char *b = malloc(cap);
	struct pc_jw w;

	if (!b)
		return NULL;
	pc_jw_init(&w, b, cap);
	pc_jw_lit(&w, "{\"col\":");
	pc_jw_str(&w, col, strlen(col));
	pc_jw_lit(&w, ",\"key\":");
	pc_jw_str(&w, key, strlen(key));
	if (extra)
		pc_jw_raw(&w, extra, strlen(extra));
	pc_jw_lit(&w, "}");
	if (w.overflow) {
		free(b);
		return NULL;
	}
	b[w.len] = 0;
	return b;
}

/* ---- typed verbs ------------------------------------------------------- */

int perfd_ping(perfd_t *p)
{
	char *r = NULL;
	int ok;

	if (p->binary) {
		size_t n = 0;

		if (bin_roundtrip(p, BV_PING, NULL, NULL, "perfd", 5, 0, 0,
		        &r, &n) != 0)
			return -1;
		ok = r && n == 5 && memcmp(r, "perfd", 5) == 0;
		free(r);
		return ok ? 0 : err(p, "bad pong");
	}
	r = roundtrip(p, "ping", NULL);
	ok = r && strstr(r, "\"pong\":true") != NULL;
	free(r);
	return ok ? 0 : (r ? err(p, "bad pong") : -1);
}

int perfd_set(perfd_t *p, const char *col, const char *key,
		const void *val, size_t vlen, long long ttl)
{
	size_t cap;
	char *b, *r = NULL;
	struct pc_jw w;
	int ok = 0;

	if (p->binary) {
		size_t n = 0;

		if (bin_roundtrip(p, BV_SET, col, key, val, vlen, 0, ttl,
		        &r, &n) != 0)
			return -1;
		ok = r && n >= 1 && r[0] == 1;
		free(r);
		return ok ? 0 : err(p, "not stored");
	}
	cap = strlen(col) + strlen(key) * 6 + vlen * 2 + 128;
	b = malloc(cap);
	if (!b)
		return err(p, "out of memory");
	pc_jw_init(&w, b, cap);
	pc_jw_lit(&w, "{\"col\":");
	pc_jw_str(&w, col, strlen(col));
	pc_jw_lit(&w, ",\"key\":");
	pc_jw_str(&w, key, strlen(key));
	pc_jw_lit(&w, ",");
	pc_jw_value(&w, "value", "enc", val, vlen);
	if (ttl > 0) {
		pc_jw_lit(&w, ",\"ttl\":");
		pc_jw_i64(&w, ttl);
	}
	pc_jw_lit(&w, "}");
	if (w.overflow) {
		free(b);
		return err(p, "value too large");
	}
	b[w.len] = 0;
	r = roundtrip_key(p, "set", b, col, key);
	free(b);
	if (!r)
		return -1;
	if (res_bool(r, "stored", &ok) != 0 || !ok) {
		free(r);
		return err(p, "not stored");
	}
	free(r);
	return 0;
}

int perfd_get(perfd_t *p, const char *col, const char *key,
		void **val, size_t *vlen, long long *ttl_out)
{
	char *ps, *r = NULL;
	struct pc_jtok *t;
	int ntok, found = 0, l;

	if (p->binary) {
		size_t n = 0;

		if (bin_roundtrip(p, BV_GET, col, key, NULL, 0, 0, 0,
		        &r, &n) != 0)
			return -1;
		if (!r || n < 1 || (r[0] == 1 && n < 5)) {
			free(r);
			return err(p, "bad get reply");
		}
		if (r[0] != 1) {
			free(r);
			return 0;
		}
		if (val) {
			*val = malloc(n - 5 ? n - 5 : 1);
			if (!*val) {
				free(r);
				return err(p, "out of memory");
			}
			memcpy(*val, r + 5, n - 5);
		}
		if (vlen)
			*vlen = n - 5;
		if (ttl_out) {
			unsigned int tl = rle32(r + 1);

			*ttl_out = tl ? (long long)tl : -1;
		}
		free(r);
		return 1;
	}
	ps = params_ck(col, key, NULL);
	if (!ps)
		return err(p, "out of memory");
	r = roundtrip_key(p, "get", ps, col, key);
	free(ps);
	if (!r)
		return -1;
	if (res_bool(r, "found", &found) != 0) {
		free(r);
		return err(p, "bad get reply");
	}
	if (!found) {
		free(r);
		return 0;
	}
	ntok = res_parse(r, &t);
	if (ntok < 0) {
		free(r);
		return err(p, "bad get reply");
	}
	l = extract_value(r, t, ntok, 0, val);
	if (l < 0) {
		free(t);
		free(r);
		return err(p, "bad get value");
	}
	*vlen = (size_t)l;
	if (ttl_out) {
		long long tt = -1;

		res_ll(r, "ttl", &tt);
		*ttl_out = tt;
	}
	free(t);
	free(r);
	return 1;
}

/* one byte back: 1/0 (del, exists, expire) */
static int bin_byte_verb(perfd_t *p, int verb, const char *col,
		const char *key, long long ttl)
{
	char *r = NULL;
	size_t n = 0;
	int v;

	if (bin_roundtrip(p, verb, col, key, NULL, 0, 0, ttl, &r, &n) != 0)
		return -1;
	if (!r || n < 1) {
		free(r);
		return err(p, "short reply");
	}
	v = (unsigned char)r[0];
	free(r);
	return v;
}

static int bool_verb(perfd_t *p, const char *method, const char *col,
		const char *key, const char *field)
{
	char *ps = params_ck(col, key, NULL), *r = NULL;
	int v = 0;

	if (!ps)
		return err(p, "out of memory");
	r = roundtrip_key(p, method, ps, col, key);
	free(ps);
	if (!r)
		return -1;
	if (res_bool(r, field, &v) != 0) {
		free(r);
		return err(p, "bad %s reply", method);
	}
	free(r);
	return v;
}

int perfd_del(perfd_t *p, const char *col, const char *key)
{
	if (p->binary)
		return bin_byte_verb(p, BV_DEL, col, key, 0);
	return bool_verb(p, "del", col, key, "deleted");
}

int perfd_exists(perfd_t *p, const char *col, const char *key)
{
	if (p->binary)
		return bin_byte_verb(p, BV_EXISTS, col, key, 0);
	return bool_verb(p, "exists", col, key, "exists");
}

long long perfd_ttl(perfd_t *p, const char *col, const char *key)
{
	char *ps, *r = NULL;
	long long v;

	if (p->binary) {
		size_t n = 0;

		if (bin_roundtrip(p, BV_TTL, col, key, NULL, 0, 0, 0,
		        &r, &n) != 0)
			return -1;
		if (!r || n < 8) {
			free(r);
			return err(p, "short reply");
		}
		v = (long long)rle64(r);
		free(r);
		return v;
	}
	ps = params_ck(col, key, NULL);
	if (!ps)
		return err(p, "out of memory");
	r = roundtrip_key(p, "ttl", ps, col, key);
	free(ps);
	if (!r)
		return -1;
	if (res_ll(r, "ttl", &v) != 0) {
		free(r);
		return err(p, "bad ttl reply");
	}
	free(r);
	return v;
}

int perfd_expire(perfd_t *p, const char *col, const char *key,
		long long ttl)
{
	char extra[48], *ps, *r = NULL;
	int v = 0;

	if (p->binary) {
		v = bin_byte_verb(p, BV_EXPIRE, col, key, ttl);
		return v < 0 ? -1 : (v ? 0 : 1);
	}
	snprintf(extra, sizeof extra, ",\"ttl\":%lld", ttl);
	ps = params_ck(col, key, extra);
	if (!ps)
		return err(p, "out of memory");
	r = roundtrip_key(p, "expire", ps, col, key);
	free(ps);
	if (!r)
		return -1;
	if (res_bool(r, "updated", &v) != 0) {
		free(r);
		return err(p, "bad expire reply");
	}
	free(r);
	return v ? 0 : 1;
}

static int counter(perfd_t *p, const char *method, const char *col,
		const char *key, long long by, long long ttl,
		long long *newval)
{
	char extra[96], *ps, *r = NULL;
	long long v;

	if (p->binary) {
		size_t n = 0;
		int verb = method[0] == 'a' ? BV_ADD : BV_SUB;

		if (bin_roundtrip(p, verb, col, key, NULL, 0, by, ttl,
		        &r, &n) != 0)
			return -1;
		if (!r || n < 8) {
			free(r);
			return err(p, "short reply");
		}
		if (newval)
			*newval = (long long)rle64(r);
		free(r);
		return 0;
	}
	if (ttl > 0)
		snprintf(extra, sizeof extra, ",\"by\":%lld,\"ttl\":%lld",
			by, ttl);
	else
		snprintf(extra, sizeof extra, ",\"by\":%lld", by);
	ps = params_ck(col, key, extra);
	if (!ps)
		return err(p, "out of memory");
	r = roundtrip_key(p, method, ps, col, key);
	free(ps);
	if (!r)
		return -1;
	if (res_ll(r, "value", &v) != 0) {
		free(r);
		return err(p, "bad %s reply", method);
	}
	free(r);
	if (newval)
		*newval = v;
	return 0;
}

int perfd_add(perfd_t *p, const char *col, const char *key, long long by,
		long long ttl, long long *newval)
{
	return counter(p, "add", col, key, by, ttl, newval);
}

int perfd_sub(perfd_t *p, const char *col, const char *key, long long by,
		long long *newval)
{
	return counter(p, "sub", col, key, by, 0, newval);
}

int perfd_mget(perfd_t *p, const char *col, const char *const *keys,
		int nkeys, void **values, size_t *vlens)
{
	size_t cap = strlen(col) + 64;
	char *b, *r = NULL;
	struct pc_jw w;
	struct pc_jtok *t;
	int i, ntok, ta, elem, got = 0;

	for (i = 0; i < nkeys; i++)
		cap += strlen(keys[i]) * 6 + 4;
	b = malloc(cap);
	if (!b)
		return err(p, "out of memory");
	pc_jw_init(&w, b, cap);
	pc_jw_lit(&w, "{\"col\":");
	pc_jw_str(&w, col, strlen(col));
	pc_jw_lit(&w, ",\"keys\":[");
	for (i = 0; i < nkeys; i++) {
		if (i)
			pc_jw_lit(&w, ",");
		pc_jw_str(&w, keys[i], strlen(keys[i]));
	}
	pc_jw_lit(&w, "]}");
	if (w.overflow) {
		free(b);
		return err(p, "request too large");
	}
	b[w.len] = 0;
	r = roundtrip(p, "mget", b);
	free(b);
	if (!r)
		return -1;
	ntok = res_parse(r, &t);
	if (ntok < 0) {
		free(r);
		return err(p, "bad mget reply");
	}
	ta = pc_json_get(r, t, ntok, 0, "values");
	if (ta < 0 || t[ta].type != PC_J_ARR) {
		free(t);
		free(r);
		return err(p, "bad mget reply");
	}
	for (elem = ta + 1, i = 0; elem < ntok && i < nkeys; elem++) {
		int found = 0, tf;

		if (t[elem].parent != ta || t[elem].type != PC_J_OBJ)
			continue;
		tf = pc_json_get(r, t, ntok, elem, "found");
		found = tf >= 0 && r[t[tf].start] == 't';
		values[i] = NULL;
		vlens[i] = 0;
		if (found) {
			int l = extract_value(r, t, ntok, elem, &values[i]);

			if (l >= 0)
				vlens[i] = (size_t)l;
			else
				values[i] = NULL;
		}
		i++;
		got++;
	}
	free(t);
	free(r);
	if (got != nkeys)
		return err(p, "mget answered %d of %d", got, nkeys);
	return 0;
}

int perfd_keys(perfd_t *p, const char *col, const char *match, int limit,
		char ***keys_out)
{
	size_t cap = strlen(col) + (match ? strlen(match) * 6 : 0) + 96;
	char *b = malloc(cap), *r = NULL;
	struct pc_jw w;
	struct pc_jtok *t;
	int ntok, ta, elem, n = 0;
	char **out;

	if (!b)
		return err(p, "out of memory");
	pc_jw_init(&w, b, cap);
	pc_jw_lit(&w, "{\"col\":");
	pc_jw_str(&w, col, strlen(col));
	if (match) {
		pc_jw_lit(&w, ",\"match\":");
		pc_jw_str(&w, match, strlen(match));
	}
	if (limit > 0) {
		pc_jw_lit(&w, ",\"limit\":");
		pc_jw_i64(&w, limit);
	}
	pc_jw_lit(&w, "}");
	b[w.len] = 0;
	r = roundtrip(p, "keys", b);
	free(b);
	if (!r)
		return -1;
	ntok = res_parse(r, &t);
	if (ntok < 0) {
		free(r);
		return err(p, "bad keys reply");
	}
	ta = pc_json_get(r, t, ntok, 0, "keys");
	if (ta < 0 || t[ta].type != PC_J_ARR) {
		free(t);
		free(r);
		return err(p, "bad keys reply");
	}
	out = calloc(t[ta].size ? (size_t)t[ta].size : 1, sizeof *out);
	if (!out) {
		free(t);
		free(r);
		return err(p, "out of memory");
	}
	for (elem = ta + 1; elem < ntok; elem++) {
		if (t[elem].parent != ta)
			continue;
		if (t[elem].type == PC_J_STR) {
			int l = t[elem].end - t[elem].start;
			char *k = malloc((size_t)l + 1);

			if (k) {
				l = pc_json_unescape(r, &t[elem], k,
					(size_t)l + 1);
				if (l >= 0) {
					k[l] = 0;
					out[n++] = k;
				} else {
					free(k);
				}
			}
		} else if (t[elem].type == PC_J_OBJ) {
			/* a binary key: {"b64":...} */
			int tb = pc_json_get(r, t, ntok, elem, "b64");

			if (tb >= 0 && t[tb].type == PC_J_STR) {
				int l = t[tb].end - t[tb].start;
				char *k = malloc((size_t)l + 1);

				if (k) {
					l = pc_json_unescape(r, &t[tb], k,
						(size_t)l + 1);
					if (l >= 0)
						l = pc_b64_dec(k, (size_t)l,
							k, (size_t)l + 1);
					if (l >= 0) {
						k[l] = 0;
						out[n++] = k;
					} else {
						free(k);
					}
				}
			}
		}
	}
	free(t);
	free(r);
	*keys_out = out;
	return n;
}

void perfd_free_keys(char **keys, int n)
{
	int i;

	for (i = 0; i < n; i++)
		free(keys[i]);
	free(keys);
}

/* ---- JSON path verbs --------------------------------------------------- */

static char *params_ckp(const char *col, const char *key, const char *path,
		const char *tailraw)
{
	size_t cap = strlen(col) + strlen(key) * 6 + strlen(path) * 6 +
		(tailraw ? strlen(tailraw) : 0) + 96;
	char *b = malloc(cap);
	struct pc_jw w;

	if (!b)
		return NULL;
	pc_jw_init(&w, b, cap);
	pc_jw_lit(&w, "{\"col\":");
	pc_jw_str(&w, col, strlen(col));
	pc_jw_lit(&w, ",\"key\":");
	pc_jw_str(&w, key, strlen(key));
	pc_jw_lit(&w, ",\"path\":");
	pc_jw_str(&w, path, strlen(path));
	if (tailraw)
		pc_jw_raw(&w, tailraw, strlen(tailraw));
	pc_jw_lit(&w, "}");
	if (w.overflow) {
		free(b);
		return NULL;
	}
	b[w.len] = 0;
	return b;
}

int perfd_jget(perfd_t *p, const char *col, const char *key,
		const char *path, char **frag_out)
{
	char *ps = params_ckp(col, key, path ? path : "$", NULL), *r = NULL;
	struct pc_jtok *t;
	int ntok, found = 0, tv;

	if (!ps)
		return err(p, "out of memory");
	r = roundtrip_key(p, "jget", ps, col, key);
	free(ps);
	if (!r)
		return -1;
	if (res_bool(r, "found", &found) != 0 || !found) {
		free(r);
		return found ? err(p, "bad jget reply") : 0;
	}
	ntok = res_parse(r, &t);
	if (ntok < 0) {
		free(r);
		return err(p, "bad jget reply");
	}
	tv = pc_json_get(r, t, ntok, 0, "value");
	if (tv < 0) {
		free(t);
		free(r);
		return err(p, "bad jget reply");
	}
	{
		int a = ext_start(&t[tv]), b = ext_end(&t[tv]);

		*frag_out = malloc((size_t)(b - a) + 1);
		if (*frag_out) {
			memcpy(*frag_out, r + a, (size_t)(b - a));
			(*frag_out)[b - a] = 0;
		}
	}
	free(t);
	free(r);
	return *frag_out ? 1 : err(p, "out of memory");
}

int perfd_jset(perfd_t *p, const char *col, const char *key,
		const char *path, const char *json_val, long long ttl)
{
	size_t tl = strlen(json_val) + 48;
	char *tail = malloc(tl), *ps, *r = NULL;
	int ok = 0;

	if (!tail)
		return err(p, "out of memory");
	if (ttl > 0)
		snprintf(tail, tl, ",\"val\":%s,\"ttl\":%lld", json_val, ttl);
	else
		snprintf(tail, tl, ",\"val\":%s", json_val);
	ps = params_ckp(col, key, path ? path : "$", tail);
	free(tail);
	if (!ps)
		return err(p, "out of memory");
	r = roundtrip_key(p, "jset", ps, col, key);
	free(ps);
	if (!r)
		return -1;
	if (res_bool(r, "set", &ok) != 0 || !ok) {
		free(r);
		return err(p, "not set");
	}
	free(r);
	return 0;
}

int perfd_jdel(perfd_t *p, const char *col, const char *key,
		const char *path)
{
	char *ps = params_ckp(col, key, path, NULL), *r = NULL;
	int v = 0;

	if (!ps)
		return err(p, "out of memory");
	r = roundtrip_key(p, "jdel", ps, col, key);
	free(ps);
	if (!r)
		return -1;
	if (res_bool(r, "deleted", &v) != 0) {
		free(r);
		return err(p, "bad jdel reply");
	}
	free(r);
	return v;
}

int perfd_jincr(perfd_t *p, const char *col, const char *key,
		const char *path, long long by, long long *newval)
{
	char tail[48], *ps, *r = NULL;
	long long v;

	snprintf(tail, sizeof tail, ",\"by\":%lld", by);
	ps = params_ckp(col, key, path, tail);
	if (!ps)
		return err(p, "out of memory");
	r = roundtrip_key(p, "jincr", ps, col, key);
	free(ps);
	if (!r)
		return -1;
	if (res_ll(r, "value", &v) != 0) {
		free(r);
		return err(p, "bad jincr reply");
	}
	free(r);
	if (newval)
		*newval = v;
	return 0;
}

/* ---- notifications ------------------------------------------------------ */

void perfd_set_notify(perfd_t *p, perfd_notify_cb cb, void *ctx)
{
	p->ncb = cb;
	p->nctx = ctx;
}

/* ---- connect / free ----------------------------------------------------- */

static int noise_handshake(perfd_t *p, const char *secret)
{
	uint8_t psk[PC_NOISE_KEYLEN], prologue[1] = { PC_PRIN_CLIENT };
	uint8_t ver = PC_CLIENT_VER;
	struct pc_handshake hs;
	uint8_t msg[3 + PC_NOISE_MAXMSG], m2[PC_NOISE_MAXMSG], hdr[2];
	size_t mlen = 0, plen = 0, cl;
	int rc = -1;

	if (pc_psk_derive(secret, strlen(secret), PC_PRIN_CLIENT, psk) != 0)
		return -1;
	pc_hs_init_initiator(&hs, prologue, 1);
	if (pc_hs_write_msg1(&hs, psk, &ver, 1, msg + 3, &mlen) != 0)
		goto out;
	msg[0] = (uint8_t)(1 + mlen);
	msg[1] = (uint8_t)((1 + mlen) >> 8);
	msg[2] = PC_PRIN_CLIENT;
	if (io_all(p, 1, msg, 3 + mlen) != 0)
		goto out;
	if (io_all(p, 0, hdr, 2) != 0)
		goto out;
	cl = (size_t)hdr[0] | ((size_t)hdr[1] << 8);
	if (cl == 0 || cl > sizeof m2 || io_all(p, 0, m2, cl) != 0)
		goto out;
	if (pc_hs_read_msg2(&hs, m2, cl, NULL, &plen, &p->cs_send,
	        &p->cs_recv) != 0)
		goto out;
	p->encrypted = 1;
	rc = 0;
out:
	sodium_memzero(psk, sizeof psk);
	return rc;
}

/* S104: keep a link honest while it is idle.  The kernel probes after
 * @idle_s of silence, every ten seconds, three times, so a peer that
 * vanished without a FIN fails the next send at once instead of after
 * the io timeout, and a middlebox that ages idle flows sees traffic. */
static void sock_keepalive(int fd, int idle_s)
{
	int one = 1, intvl = idle_s < 10 ? (idle_s > 0 ? idle_s : 1) : 10,
	    cnt = 3;

	if (idle_s <= 0)
		return;
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
#ifdef TCP_KEEPIDLE
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle_s, sizeof idle_s);
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
#endif
}

static int sock_connect(const char *host, int port, const char *unixp,
		int timeout_ms)
{
	int fd = -1;

	if (unixp) {
		struct sockaddr_un sa;

		fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd < 0)
			return -1;
		memset(&sa, 0, sizeof sa);
		sa.sun_family = AF_UNIX;
		snprintf(sa.sun_path, sizeof sa.sun_path, "%s", unixp);
		if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
			close(fd);
			return -1;
		}
		return fd;
	}
	{
		struct addrinfo hints, *res, *ai;
		char ps[16];
		int one = 1;

		(void)timeout_ms;          /* v1: default kernel timeout */
		memset(&hints, 0, sizeof hints);
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		snprintf(ps, sizeof ps, "%d", port);
		if (getaddrinfo(host, ps, &hints, &res) != 0)
			return -1;
		for (ai = res; ai; ai = ai->ai_next) {
			fd = socket(ai->ai_family, ai->ai_socktype, 0);
			if (fd < 0)
				continue;
			if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
				break;
			close(fd);
			fd = -1;
		}
		freeaddrinfo(res);
		if (fd >= 0)
			setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one,
				sizeof one);
		return fd;
	}
}

/* S34: a freshly connected handle learns the fleet, opens its standbys
 * and settles onto the member its policy prefers.  All of it is best
 * effort: a standalone daemon answers no members, an unreachable peer
 * is simply skipped, and the handle works exactly as before either
 * way. */
static perfd_t *conn_ready(perfd_t *p)
{
	if (p->is_child || !p->want_spares)
		return p;
	/* an independent random start per client is what makes
	 * round-robin spread without any coordination (par 5.6) */
	p->rr = (unsigned int)(now_ms_lib() ^ (long long)(uintptr_t)p ^
		((long long)getpid() << 7));
	/* S76: the daemon labels a connection by the dialect of its FIRST
	 * byte.  Learning the fleet is a text request, so a binary handle
	 * says one binary word first, or it is counted as a text client. */
	if (p->binary)
		perfd_ping(p);
	if (learn_members(p) > 0) {
		/* parse_members set active_node from the "self" flag: the
		 * node answering us is the one we are on */
		warm_spares(p);
		apply_policy(p);
	}
	return p;
}

/* allocate a handle and copy the options into it.  Shared by the
 * blocking and the event-loop connect (S32) so the two cannot drift on
 * what an option means. */
static perfd_t *pd_alloc(const perfd_opts *opts)
{
	static const perfd_opts defaults = { 0 };
	const perfd_opts *o = opts ? opts : &defaults;
	int nsec = 0, i;
	perfd_t *p;

	if (sodium_init() < 0) {
		snprintf(g_connect_err, sizeof g_connect_err,
			"libsodium init failed");
		return NULL;
	}
	while (o->secrets && o->secrets[nsec])
		nsec++;

	p = calloc(1, sizeof *p);
	if (!p) {
		snprintf(g_connect_err, sizeof g_connect_err,
			"out of memory");
		return NULL;
	}
	p->io_ms = o->io_timeout_ms > 0 ? o->io_timeout_ms : DEF_TIMEOUT_MS;
	p->binary = o->binary != 0;
	p->eager_push = o->eager_push != 0;
	p->fd = -1;
	/* S34: remember how to open standbys (secrets COPIED - the
	 * caller's array need not outlive the handle) */
	p->policy = o->policy;
	/* S104: the default is every member (0 and -1 alike); only
	 * PERFD_SPARES_NONE asks for the bare handle */
	p->want_spares = o->spares == PERFD_SPARES_NONE ? 0 :
		o->spares == 0 ? -1 : o->spares;
	/* S104: 0 = the default, -1 = off, else as given */
	p->keepalive_s = o->keepalive_s > 0 ? o->keepalive_s :
		o->keepalive_s < 0 ? 0 : DEF_KEEPALIVE_S;
	p->idle_ping_ms = o->idle_ping_ms > 0 ? o->idle_ping_ms :
		o->idle_ping_ms < 0 ? 0 : DEF_IDLE_PING_MS;
	p->route_want = o->route_keys;
	p->ignore_push = o->ignore_member_push != 0;
	p->refresh_ms = o->refresh_ms > 0 ? o->refresh_ms : 30000;
	p->o_connect_ms = o->connect_timeout_ms;
	p->o_io_ms = o->io_timeout_ms;
	p->o_binary = o->binary != 0;
	for (i = 0; i < nsec && i < PERFD_MAX_SECRETS; i++) {
		p->sec[i] = strdup(o->secrets[i]);
		if (p->sec[i])
			p->nsec++;
	}
	return p;
}

static perfd_t *do_connect(const char *host, int port, const char *unixp,
		const perfd_opts *opts)
{
	static const perfd_opts defaults = { 0 };
	const perfd_opts *o = opts ? opts : &defaults;
	int io_ms = o->io_timeout_ms > 0 ? o->io_timeout_ms : DEF_TIMEOUT_MS;
	int nsec = 0, i;
	perfd_t *p;

	while (o->secrets && o->secrets[nsec])
		nsec++;
	p = pd_alloc(opts);
	if (!p)
		return NULL;

	/* the secret LIST: try each in order on a fresh connection -
	 * rotation is add-new/drain-old with no client downtime */
	for (i = 0; i < (nsec ? nsec : 1); i++) {
		struct timeval tv = { io_ms / 1000, (io_ms % 1000) * 1000L };

		p->fd = sock_connect(host, port, unixp,
			o->connect_timeout_ms);
		if (p->fd < 0) {
			snprintf(g_connect_err, sizeof g_connect_err,
				"cannot connect: %s", strerror(errno));
			free(p);
			return NULL;
		}
		setsockopt(p->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
		setsockopt(p->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
		sock_keepalive(p->fd, p->keepalive_s);                  /* S104 */
		if (!nsec)
			return conn_ready(p);  /* plaintext */
		if (noise_handshake(p, o->secrets[i]) == 0)
			return conn_ready(p);
		close(p->fd);
		p->fd = -1;
		p->poisoned = 0;
	}
	snprintf(g_connect_err, sizeof g_connect_err,
		"handshake failed with every secret (%d tried)", nsec);
	free(p);
	return NULL;
}

perfd_t *perfd_connect(const char *host, int port, const perfd_opts *opts)
{
	return do_connect(host, port, NULL, opts);
}

perfd_t *perfd_connect_unix(const char *path, const perfd_opts *opts)
{
	return do_connect(NULL, 0, path, opts);
}

void perfd_free(perfd_t *p)
{
	struct early *e;
	int i;

	if (!p)
		return;
	for (i = 0; i < p->nspare; i++)      /* S34: standbys go too */
		perfd_free(p->spare[i]);
	p->nspare = 0;
	for (i = 0; i < p->nsec; i++) {
		/* S59: the lib re-derives the PSK per handshake, so the
		 * raw copies must live until close - which is now */
		if (p->sec[i])
			sodium_memzero(p->sec[i], strlen(p->sec[i]));
		free(p->sec[i]);
	}
	if (p->udp) {
		udp_drop(p, 0);
		free(p->udp->json);
		free(p->udp);
	}
	if (p->fd >= 0)
		close(p->fd);
	while ((e = p->early)) {
		p->early = e->next;
		free(e->result);
		free(e);
	}
	free(p->rbuf);
	free(p->q);
	free(p->ids);
	free(p->w);
	free(p->cin);
	free(p->calls);
	free(p->tok);
	free(p);
}

/* ======================================================================
 * S32 — the event-loop surface
 *
 * Everything above this line blocks by design and is unchanged.  What
 * follows is the same protocol driven by someone else's event loop: the
 * consumer owns the loop, watches perfd_fd() for perfd_events(), and
 * hands control back in through the two readiness entry points.  The
 * library starts no thread and never calls back from one.
 *
 * The wire is shared, not reimplemented: requests are built by the same
 * enqueue(), replies are extracted by the same take_msg().  The only
 * genuinely new machinery is (a) an output buffer, because a
 * non-blocking write may take a prefix, (b) incremental Noise record
 * framing, because a record can arrive in pieces, and (c) an id ->
 * callback table replacing the in-order FIFO.
 * ====================================================================== */

/* Argon2id at INTERACTIVE limits costs ~100 ms of CPU.  Paying that per
 * connect is merely slow for a blocking client, but for an event loop it
 * is a stall - and RECONNECT happens inside the loop, which is exactly
 * when a consumer can least afford it.  So derived PSKs are cached for
 * the life of the process, keyed by the secret.  The secret itself is
 * already held for the life of a handle (spares and failover need it),
 * so this keeps nothing that was not kept before. */
static pthread_mutex_t g_psk_lock = PTHREAD_MUTEX_INITIALIZER;
static struct psk_ent {
	char sec[192];
	uint8_t psk[PC_NOISE_KEYLEN];
	int used;
} g_psk[8];

static int psk_for(const char *secret, uint8_t out[PC_NOISE_KEYLEN])
{
	int i, rc = 0;
	size_t n = strlen(secret);

	if (n >= sizeof g_psk[0].sec)          /* too long to cache; derive */
		return pc_psk_derive(secret, n, PC_PRIN_CLIENT, out);

	pthread_mutex_lock(&g_psk_lock);
	for (i = 0; i < (int)(sizeof g_psk / sizeof g_psk[0]); i++)
		if (g_psk[i].used && !strcmp(g_psk[i].sec, secret)) {
			memcpy(out, g_psk[i].psk, PC_NOISE_KEYLEN);
			pthread_mutex_unlock(&g_psk_lock);
			return 0;
		}
	pthread_mutex_unlock(&g_psk_lock);

	if (pc_psk_derive(secret, n, PC_PRIN_CLIENT, out) != 0)
		return -1;

	pthread_mutex_lock(&g_psk_lock);
	for (i = 0; i < (int)(sizeof g_psk / sizeof g_psk[0]); i++)
		if (!g_psk[i].used) {
			snprintf(g_psk[i].sec, sizeof g_psk[i].sec, "%s",
				secret);
			memcpy(g_psk[i].psk, out, PC_NOISE_KEYLEN);
			g_psk[i].used = 1;
			break;
		}
	pthread_mutex_unlock(&g_psk_lock);
	(void)rc;
	return 0;
}

/* ---- output buffering -------------------------------------------------- */

static int wbuf_room(perfd_t *p, size_t more)
{
	if (p->wlen + more <= p->wcap)
		return 0;
	while (p->wcap < p->wlen + more)
		p->wcap = p->wcap ? p->wcap * 2 : 65536;
	{
		void *nb = realloc(p->w, p->wcap);

		if (!nb) {
			p->poisoned = 1;
			return err(p, "out of memory");
		}
		p->w = nb;
	}
	return 0;
}

/* stage plaintext for the socket, sealing it first when the channel is
 * encrypted - the same record shape send_bytes() writes, just buffered
 * instead of written, because a non-blocking socket may take a prefix */
static int wbuf_put(perfd_t *p, const char *b, size_t n)
{
	if (!p->encrypted) {
		if (wbuf_room(p, n) != 0)
			return -1;
		memcpy(p->w + p->wlen, b, n);
		p->wlen += n;
		return 0;
	}
	while (n) {
		size_t chunk = n > PC_NOISE_MAXPT ? PC_NOISE_MAXPT : n;
		int cl;

		if (wbuf_room(p, 2 + chunk + PC_NOISE_TAGLEN) != 0)
			return -1;
		cl = pc_transport_encrypt(&p->cs_send, (const uint8_t *)b,
			chunk, (uint8_t *)p->w + p->wlen + 2);
		if (cl < 0) {
			p->poisoned = 1;
			return err(p, "transport encrypt failed");
		}
		p->w[p->wlen] = (char)(unsigned char)cl;
		p->w[p->wlen + 1] = (char)(unsigned char)(cl >> 8);
		p->wlen += 2 + (size_t)cl;
		b += chunk;
		n -= chunk;
	}
	return 0;
}

/* 0 = buffer drained, 1 = partial (watch for writability), -1 = failed */
static int wbuf_push(perfd_t *p)
{
	while (p->woff < p->wlen) {
		ssize_t r = write(p->fd, p->w + p->woff, p->wlen - p->woff);

		if (r > 0) {
			p->woff += (size_t)r;
			continue;
		}
		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return 1;
		p->poisoned = 1;
		err(p, "connection %s (%s)", r == 0 ? "closed" : "failed",
			r == 0 ? "EOF" : strerror(errno));
		return -1;
	}
	p->woff = p->wlen = 0;
	return 0;
}

/* ---- input: socket -> ciphertext -> rbuf ------------------------------- */

/* read whatever the socket has.  1 = bytes arrived, 0 = would block,
 * -1 = closed or failed. */
static int cin_pull(perfd_t *p)
{
	int got = 0;

	for (;;) {
		ssize_t r;

		if (p->cinlen + 65536 > p->cincap) {
			size_t cap = p->cincap ? p->cincap * 2 : 131072;

			while (cap < p->cinlen + 65536)
				cap *= 2;
			{
				void *nb = realloc(p->cin, cap);

				if (!nb) {
					p->poisoned = 1;
					return err(p, "out of memory");
				}
				p->cin = nb;
			}
			p->cincap = cap;
		}
		r = read(p->fd, p->cin + p->cinlen, p->cincap - p->cinlen);
		if (r > 0) {
			p->cinlen += (size_t)r;
			got = 1;
			continue;
		}
		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return got;
		p->poisoned = 1;
		err(p, "connection %s", r == 0 ? "closed" : strerror(errno));
		return -1;
	}
}

/* move complete records out of cin and into rbuf as plaintext.  A record
 * split across reads is simply left in cin until the rest turns up -
 * this is the whole reason the blocking fill() cannot be reused. */
static int cin_drain(perfd_t *p)
{
	if (!p->encrypted) {
		if (!p->cinlen)
			return 0;
		if (rbuf_reserve(p, p->cinlen) != 0)
			return -1;
		memcpy(p->rbuf + p->rlen, p->cin, p->cinlen);
		p->rlen += p->cinlen;
		p->cinlen = 0;
		return 0;
	}
	for (;;) {
		size_t cl;
		int pl;

		if (p->cinlen < 2)
			return 0;
		cl = (size_t)(unsigned char)p->cin[0] |
			((size_t)(unsigned char)p->cin[1] << 8);
		if (cl < PC_NOISE_TAGLEN || cl > PC_NOISE_MAXMSG) {
			p->poisoned = 1;
			return err(p, "bad transport record");
		}
		if (p->cinlen < 2 + cl)
			return 0;
		if (rbuf_reserve(p, cl) != 0)
			return -1;
		pl = pc_transport_decrypt(&p->cs_recv,
			(const uint8_t *)p->cin + 2, cl,
			(uint8_t *)p->rbuf + p->rlen);
		if (pl < 0) {
			p->poisoned = 1;
			return err(p, "transport decrypt failed");
		}
		p->rlen += (size_t)pl;
		memmove(p->cin, p->cin + 2 + cl, p->cinlen - 2 - cl);
		p->cinlen -= 2 + cl;
	}
}

/* An async handle learns the fleet THROUGH ITS OWN PIPELINE.  It cannot
 * use learn_members(): that calls roundtrip_once(), which blocks for a
 * reply, and an async handle's socket is non-blocking and driven by the
 * caller's event loop.  So the request goes out as an ordinary async
 * submit and this callback parses the answer when it arrives.
 *
 * Until this lands, route_ok is 0 and perfd_owner_of() returns -1, which
 * is the honest answer - the fleet is genuinely unknown until the reply
 * comes back.  Callers that need routing from the first request should
 * pump perfd_events() until perfd_routing() turns 1.
 *
 * S34's standbys are deliberately NOT opened here: they are separate
 * connections the caller would never poll.  See perfd_owner_of(). */
static void members_learned(char *result, size_t len, const char *errmsg,
		void *ctx)
{
	perfd_t *p = ctx;

	(void)len;
	p->learning = 0;
	if (!errmsg && result)
		parse_members(p, result);
	free(result);                  /* the async contract: callee owns it */
}

static int learn_members_async(perfd_t *p)
{
	if (p->is_child || p->learning || p->learned_async)
		return 0;
	if (!p->route_want)            /* nobody asked to route */
		return 0;
	p->learning = 1;
	p->learned_async = 1;          /* one attempt; a failure is not fatal */
	if (perfd_submit(p, "members", NULL, members_learned, p) != 0) {
		p->learning = 0;
		return -1;
	}
	return 0;
}

/* ---- id -> callback ---------------------------------------------------- */

static int call_add(perfd_t *p, unsigned long long id, perfd_reply_cb cb,
		void *ctx)
{
	if (p->ncalls == p->callcap) {
		int cap = p->callcap ? p->callcap * 2 : 64;
		struct pd_call *n = realloc(p->calls, (size_t)cap * sizeof *n);

		if (!n) {
			p->poisoned = 1;
			return err(p, "out of memory");
		}
		p->calls = n;
		p->callcap = cap;
	}
	p->calls[p->ncalls].id = id;
	p->calls[p->ncalls].cb = cb;
	p->calls[p->ncalls].ctx = ctx;
	p->ncalls++;
	return 0;
}

/* hand a completed reply to whoever asked for it.  Arrival order, not
 * request order: that is the point of the async surface - a slow KEYS
 * must not hold up the gets queued behind it. */
static void call_fire(perfd_t *p, unsigned long long id, char *result,
		size_t rlen, const char *errmsg)
{
	int i;

	for (i = 0; i < p->ncalls; i++)
		if (p->calls[i].id == id) {
			perfd_reply_cb cb = p->calls[i].cb;
			void *ctx = p->calls[i].ctx;

			p->calls[i] = p->calls[--p->ncalls];
			if (cb)
				cb(result, rlen, errmsg, ctx);
			else
				free(result);
			return;
		}
	free(result);              /* a reply nobody is waiting for */
}

/* the connection died: tell every caller still waiting, so a consumer
 * never silently loses a request it issued */
static void call_fail_all(perfd_t *p, const char *why)
{
	while (p->ncalls) {
		struct pd_call c = p->calls[--p->ncalls];

		if (c.cb)
			c.cb(NULL, 0, why, c.ctx);
	}
}

/* ---- handshake, driven by readiness ------------------------------------ */

static int hs_build_msg1(perfd_t *p)
{
	uint8_t psk[PC_NOISE_KEYLEN], prologue[1] = { PC_PRIN_CLIENT };
	uint8_t ver = PC_CLIENT_VER, msg[3 + PC_NOISE_MAXMSG];
	size_t mlen = 0;

	if (psk_for(p->sec[p->hs_sec], psk) != 0)
		return err(p, "key derivation failed");
	pc_hs_init_initiator(&p->hs, prologue, 1);
	if (pc_hs_write_msg1(&p->hs, psk, &ver, 1, msg + 3, &mlen) != 0) {
		sodium_memzero(psk, sizeof psk);
		return err(p, "handshake build failed");
	}
	sodium_memzero(psk, sizeof psk);
	msg[0] = (uint8_t)(1 + mlen);
	msg[1] = (uint8_t)((1 + mlen) >> 8);
	msg[2] = PC_PRIN_CLIENT;
	if (wbuf_room(p, 3 + mlen) != 0)
		return -1;
	memcpy(p->w + p->wlen, msg, 3 + mlen);
	p->wlen += 3 + mlen;
	p->hs_stage = PD_HS_SEND;
	return 0;
}

/* consume msg2 if it has all arrived.  1 = established, 0 = need more,
 * -1 = failed. */
static int hs_take_msg2(perfd_t *p)
{
	uint8_t m2[PC_NOISE_MAXMSG];
	size_t cl, plen = 0;

	if (p->cinlen < 2)
		return 0;
	cl = (size_t)(unsigned char)p->cin[0] |
		((size_t)(unsigned char)p->cin[1] << 8);
	if (cl == 0 || cl > sizeof m2) {
		p->poisoned = 1;
		return err(p, "bad handshake frame");
	}
	if (p->cinlen < 2 + cl)
		return 0;
	memcpy(m2, p->cin + 2, cl);
	memmove(p->cin, p->cin + 2 + cl, p->cinlen - 2 - cl);
	p->cinlen -= 2 + cl;
	if (pc_hs_read_msg2(&p->hs, m2, cl, NULL, &plen, &p->cs_send,
	        &p->cs_recv) != 0) {
		p->poisoned = 1;
		return err(p, "handshake failed (wrong secret?)");
	}
	p->encrypted = 1;
	p->hs_stage = PD_HS_DONE;
	p->st = PERFD_ST_READY;
	learn_members_async(p);        /* async fleet discovery; see above */
	return 1;
}

/* ---- public ------------------------------------------------------------ */

int perfd_fd(const perfd_t *p)
{
	return p ? p->fd : -1;
}

int perfd_state(const perfd_t *p)
{
	if (!p)
		return PERFD_ST_FAILED;
	return p->poisoned ? PERFD_ST_FAILED : p->st;
}

int perfd_events(const perfd_t *p)
{
	int ev = PERFD_EV_READ;

	if (!p || p->fd < 0)
		return 0;
	/* the TCP connect completes as writability, and a partial write
	 * needs the same signal - so this is not constant and must be
	 * re-read after every entry point */
	if (p->hs_stage == PD_HS_TCP || p->woff < p->wlen)
		ev |= PERFD_EV_WRITE;
	return ev;
}

int perfd_inflight(const perfd_t *p)
{
	return p ? p->ncalls : 0;
}

perfd_t *perfd_connect_async(const char *host, int port,
		const perfd_opts *opts)
{
	perfd_t *p = pd_alloc(opts);
	struct addrinfo hints, *res, *ai;
	char ps[16];
	int one = 1, fl;

	if (!p)
		return NULL;
	p->async = 1;
	p->st = PERFD_ST_CONNECTING;
	p->hs_stage = PD_HS_TCP;
	p->hs_sec = 0;
	snprintf(p->ahost, sizeof p->ahost, "%s", host ? host : "");
	p->aport = port;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(ps, sizeof ps, "%d", port);
	if (getaddrinfo(host, ps, &hints, &res) != 0) {
		err(p, "cannot resolve %s", host ? host : "(null)");
		perfd_free(p);
		return NULL;
	}
	for (ai = res; ai; ai = ai->ai_next) {
		p->fd = socket(ai->ai_family, ai->ai_socktype, 0);
		if (p->fd < 0)
			continue;
		fl = fcntl(p->fd, F_GETFL, 0);
		fcntl(p->fd, F_SETFL, (fl < 0 ? 0 : fl) | O_NONBLOCK);
		setsockopt(p->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
		sock_keepalive(p->fd, p->keepalive_s);                  /* S104 */
		if (connect(p->fd, ai->ai_addr, ai->ai_addrlen) == 0 ||
		        errno == EINPROGRESS)
			break;
		close(p->fd);
		p->fd = -1;
	}
	freeaddrinfo(res);
	if (p->fd < 0) {
		err(p, "cannot connect: %s", strerror(errno));
		perfd_free(p);
		return NULL;
	}
	return p;
}

int perfd_submit(perfd_t *p, const char *method, const char *params_json,
		perfd_reply_cb cb, void *ctx)
{
	unsigned long long id;

	if (!p->async)
		return err(p, "not an async handle");
	if (p->poisoned)
		return err(p, "connection poisoned");
	if (p->st != PERFD_ST_READY)
		return err(p, "connection not ready");
	if (enqueue(p, method, params_json) != 0)
		return -1;
	id = p->next_id;
	if (call_add(p, id, cb, ctx) != 0) {
		p->qlen = 0;
		return -1;
	}
	if (wbuf_put(p, p->q, p->qlen) != 0) {
		p->qlen = 0;
		return -1;
	}
	p->qlen = 0;
	if (!p->eager_push)
		return 0;              /* batched; goes out on write-readiness */
	if (wbuf_push(p) < 0) {
		call_fail_all(p, p->err);
		p->st = PERFD_ST_FAILED;
		return -1;
	}
	return 0;
}

int perfd_submit_kv(perfd_t *p, int verb, const char *col, const char *key,
		const void *val, size_t vlen, long long by, long long ttl,
		perfd_reply_cb cb, void *ctx)
{
	unsigned long long id;

	if (!p->async)
		return err(p, "not an async handle");
	if (p->poisoned)
		return err(p, "connection poisoned");
	if (p->st != PERFD_ST_READY)
		return err(p, "connection not ready");
	/* enqueue_bin appends to p->q and takes ++p->next_id, exactly as
	 * enqueue() does, and never touches the blocking collector's FIFO
	 * - so the async correlation below is the same either way. */
	if (enqueue_bin(p, verb, col, key, val, vlen, by, ttl) != 0)
		return -1;
	id = p->next_id;
	if (call_add(p, id, cb, ctx) != 0) {
		p->qlen = 0;
		return -1;
	}
	if (wbuf_put(p, p->q, p->qlen) != 0) {
		p->qlen = 0;
		return -1;
	}
	p->qlen = 0;
	if (!p->eager_push)
		return 0;              /* batched; goes out on write-readiness */
	if (wbuf_push(p) < 0) {
		call_fail_all(p, p->err);
		p->st = PERFD_ST_FAILED;
		return -1;
	}
	return 0;
}

/* NOT perfd_flush: that name belongs to the SYNC pipeline
 * (perfd_append/perfd_flush/perfd_next_reply), which queues into p->q
 * and sends with send_bytes.  This is the async write buffer. */
int perfd_push(perfd_t *p)
{
	if (!p->async)
		return err(p, "not an async handle");
	if (p->poisoned) {
		p->st = PERFD_ST_FAILED;
		return -1;
	}
	if (p->woff >= p->wlen)
		return 0;                      /* nothing queued */
	if (wbuf_push(p) < 0) {
		call_fail_all(p, p->err);
		p->st = PERFD_ST_FAILED;
		return -1;
	}
	return 0;
}

int perfd_write_ready(perfd_t *p)
{
	if (!p->async)
		return err(p, "not an async handle");
	if (p->poisoned) {
		p->st = PERFD_ST_FAILED;
		return -1;
	}
	if (p->hs_stage == PD_HS_TCP) {
		int se = 0;
		socklen_t sl = sizeof se;

		if (getsockopt(p->fd, SOL_SOCKET, SO_ERROR, &se, &sl) != 0 ||
		        se != 0) {
			p->poisoned = 1;
			p->st = PERFD_ST_FAILED;
			err(p, "connect failed: %s", strerror(se ? se : errno));
			call_fail_all(p, p->err);
			return -1;
		}
		if (!p->nsec) {                /* plaintext: nothing to do */
			p->hs_stage = PD_HS_DONE;
			p->st = PERFD_ST_READY;
			learn_members_async(p);
			return 0;
		}
		if (hs_build_msg1(p) != 0) {
			p->poisoned = 1;
			p->st = PERFD_ST_FAILED;
			return -1;
		}
	}
	if (wbuf_push(p) < 0) {
		p->st = PERFD_ST_FAILED;
		call_fail_all(p, p->err);
		return -1;
	}
	if (p->hs_stage == PD_HS_SEND && p->woff == p->wlen)
		p->hs_stage = PD_HS_WAIT;
	return 0;
}

int perfd_read_ready(perfd_t *p)
{
	int rc;

	if (!p->async)
		return err(p, "not an async handle");
	if (p->poisoned) {
		p->st = PERFD_ST_FAILED;
		return -1;
	}
	if (cin_pull(p) < 0) {
		p->st = PERFD_ST_FAILED;
		call_fail_all(p, p->err);
		return -1;
	}
	if (p->hs_stage == PD_HS_WAIT) {
		rc = hs_take_msg2(p);
		if (rc < 0) {
			p->st = PERFD_ST_FAILED;
			call_fail_all(p, p->err);
			return -1;
		}
		if (rc == 0)
			return 0;              /* msg2 still incomplete */
	}
	if (p->hs_stage != PD_HS_DONE)
		return 0;
	if (cin_drain(p) != 0) {
		p->st = PERFD_ST_FAILED;
		call_fail_all(p, p->err);
		return -1;
	}
	for (;;) {
		unsigned long long got;
		char *result, ebuf[256];
		size_t rlen;
		int isbin;

		rc = take_msg(p, &got, &result, &rlen, &isbin, ebuf,
			sizeof ebuf);
		if (rc == 0)
			break;
		if (rc == 2)
			continue;
		if (rc < 0) {
			p->st = PERFD_ST_FAILED;
			call_fail_all(p, p->err);
			return -1;
		}
		call_fire(p, got, result, rlen, result ? NULL : ebuf);
	}
	return 0;
}
