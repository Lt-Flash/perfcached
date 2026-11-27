/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* proto.c - connections and the two framers, binary frames (src/ptree.h) and RESP2, written
 * once for the plaintext and the encrypted planes.  The wire contract is in proto.h. */
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

#include "iostat.h"
#include "uring.h"
#include "compat/dprint.h"
#include "json.h"                      /* the HTTP bodies, the RESP writer */
#include "ptree.h"                     /* the native door's codec */
#include "metrics.h"
#include "pc_noise.h"
#include "verbs.h"
#include "obs.h"
#include "clunk.h"
#include "store.h"
#include "events.h"
#include <arpa/inet.h>
#include "pc_slot.h"     /* pc_key_slot, for the spread holder test */
#include "core/pcache_htable.h"
#include "compat/timer.h"
#include "daemon.h"
#include "pcaddr.h"
#include "proto.h"
#include "resident.h"
#include "pubsub.h"
#include "psudp.h"
#include "memguard.h"
#include "protoint.h"
#include "tls.h"
#include "upstream.h"                /* pc_upstream_copy_exp */
#include "mgetfan.h"
#include "presence.h"
#include "wal.h"                     /* the writer pause */
#include <sodium.h>
#include <time.h>


/* the event log's origin: the door is the dialect, known only at the dispatch site; the peer
 * is read once per connection */
void ev_origin(struct pc_conn *c, const char *door)
{
	if (!c->peer[0]) {
		pc_addr sa;
		socklen_t sl = sizeof sa;

		if (getpeername(c->fd, &sa.sa, &sl) == 0 &&
		        (pc_addr_family(&sa) == AF_INET ||
		         pc_addr_family(&sa) == AF_INET6))
			pc_addr_ntop_port(&sa, c->peer, sizeof c->peer);
		else
			snprintf(c->peer, sizeof c->peer, "-");
	}
	pc_ev_set_origin(door, c->peer);
}

/* ---- growable buffers -------------------------------------------------- */

/* connections of THIS worker holding a buffer (its wait stays bounded so the idle sweep runs),
 * the node's total for /stats, and the worker's tick as of its last wake */
static __thread int bufs_held_here;
static int bufs_held_all;
static __thread unsigned int conn_now;

static inline void bufs_mark(struct pc_conn *c)
{
	/* an HTTP connection is one-shot and swept on its own clock */
	if (!c->bufs_held && !c->http_only) {
		c->bufs_held = 1;
		bufs_held_here++;
		__atomic_add_fetch(&bufs_held_all, 1, __ATOMIC_RELAXED);
	}
}

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

/* records io_uring held through a pause or hold (ur_hold) decrypt into c->in under the hold's
 * limit, as a plaintext connection's held bytes do, not one request's */
static __thread int in_hold_serve;

int in_append(struct pc_conn *c, const char *p, size_t n)
{
	/* a zero-length memcpy to a NULL buffer is undefined */
	if (!n)
		return 0;
	/* a whole read (at most one Noise record) lands before the parse: room for it behind
	 * the largest request, which may arrive with the head of the next one */
	if (buf_reserve(&c->in, &c->in_cap, c->in_len, n, in_hold_serve ?
	        PC_MAX_REQ + 4 * PC_NOISE_MAXMSG + PC_UR_HOLD_MAX :
	        PC_MAX_REQ + PC_BIN_HDR + PC_NOISE_MAXMSG) < 0)
		return -1;
	bufs_mark(c);
	memcpy(c->in + c->in_len, p, n);
	c->in_len += n;
	return 0;
}

/* the output cap; a test lowers it (PERFCACHED_TEST_OUTQ) to reach it with small replies */
static size_t outq_max(void)
{
	static size_t m;

	if (!m) {
		const char *e = getenv("PERFCACHED_TEST_OUTQ");
		long long v = e ? atoll(e) : 0;

		m = v > 0 && v < PC_MAX_OUTQ ? (size_t)v : PC_MAX_OUTQ;
	}
	return m;
}

static void sync_note(struct pc_conn *c);

int out_append(struct pc_conn *c, const char *p, size_t n)
{
	int rc;

	/* a command parked on a read-through fetch answers nothing yet, and a connection with no
	 * answer has no buffer: a zero-length memcpy to NULL is undefined */
	if (!n)
		return 0;
	/* an io_uring send's unsent part is still backlog: the cap covers both, as on epoll, so
	 * the send's completion merges them within PC_MAX_OUTQ */
	if (c->ur_send &&
	    c->out_len + n + (c->wire_len - c->wire_off) > outq_max()) {
		if (!c->out_failed)
			c->out_failed = 1;
		return -1;
	}
	rc = buf_reserve(&c->out, &c->out_cap, c->out_len, n, outq_max());

	if (rc < 0) {                  /* -1 at the cap, -2 out of memory */
		if (!c->out_failed)
			c->out_failed = rc == -1 ? 1 : 2;
		return rc;
	}
	bufs_mark(c);
	sync_note(c);
	memcpy(c->out + c->out_len, p, n);
	c->out_len += n;
	return 0;
}

/* room for @n more bytes at c->wire + c->wire_len, nothing written: 0, or -1 */
static int wire_reserve(struct pc_conn *c, size_t n)
{
	const size_t limit = PC_MAX_OUTQ + (PC_MAX_OUTQ >> 4);

	/* a send in flight reads c->wire: never move or grow it.  conn_flush keeps replies in
	 * c->out meanwhile and the handshake appends before any send, so this cannot happen */
	if (c->ur_send)
		return -1;
	/* the limit is on what is PENDING, [wire_off, wire_len): the buffer resets only when it
	 * drains, so slide the pending tail to the front when the written head is in the way */
	if (c->wire_off &&
	    (c->wire_len + n > c->wire_cap || c->wire_len + n > limit)) {
		c->wire_len -= c->wire_off;
		memmove(c->wire, c->wire + c->wire_off, c->wire_len);
		c->wire_off = 0;
	}
	if (buf_reserve(&c->wire, &c->wire_cap, c->wire_len, n, limit) < 0)
		return -1;
	bufs_mark(c);
	return 0;
}

int wire_append(struct pc_conn *c, const char *p, size_t n)
{
	if (wire_reserve(c, n) < 0)
		return -1;
	memcpy(c->wire + c->wire_len, p, n);
	c->wire_len += n;
	return 0;
}

/* ---- parked pulls (thread-local: worker-owned) -------------------------- */

/* each worker's table holds the cluster's whole pending capacity ([cluster] max_pending): a
 * worker that out-parks the pending table answers "busy" before the retryable TRYAGAIN can */
#define PARK_FALLBACK 1024            /* cluster off: parking is unused */


struct parked {
	struct pc_conn *conn;          /* NULL = free */
	unsigned int req;
	int bin;                       /* PARK_* */
	int retried;                   /* shard grace: one broadcast retry */
	char id[8];                    /* the frame id, 8 LE bytes as it sat
	                                * on the wire (zeros for RESP) */
	char col[40];
	size_t collen;
	char *key;
	size_t klen;
	/* a holder hint was asked for (PC_BIN_F_HINT); the node a forward or unicast pull went to
	 * (0: broadcast or unknown) and a forward's ttl_rel, where a proxy miss is retried */
	int hint;
	int target;
	unsigned int target_ttl;
	/* a forwarded hash READ's encoded arguments (op_hcmd's), to
	 * render its reply over the record the owner answers with */
	unsigned char *hargs;
	size_t hargl;
	int pres;                      /* an EXISTS/TYPE/TTL's PC_PRES_*, else 0 */
};

/* hints sent, and hint-asking requests a proxy collection answered
 * from this node's own table - the client was already at the holder */
static unsigned long long hint_sent_n, hint_local_n;

static __thread struct parked *parks;
static __thread int parks_cap;

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

/* @id: the frame's 8 id bytes, or NULL (RESP: replies are ordered) */
int park_add(struct pc_conn *c, unsigned int req, const char *id,
		const char *col, size_t collen, const char *key, size_t klen,
		int bin, int hint)
{
	int i, pres = pc_pres_park_take(req);

	if (!parks_ready()) {
		free(pc_hcmd_take_park_args(NULL));
		return -1;
	}
	for (i = 0; i < parks_cap; i++)
		if (!parks[i].conn) {
			parks[i].conn = c;
			parks[i].req = req;
			parks[i].bin = bin;
			parks[i].hint = hint;
			parks[i].target_ttl = 0;
			parks[i].target = pc_fwd_target_of(req,
				&parks[i].target_ttl);
			parks[i].retried = 0;
			if (id)
				memcpy(parks[i].id, id, sizeof parks[i].id);
			else
				memset(parks[i].id, 0, sizeof parks[i].id);
			parks[i].collen = collen < sizeof parks[i].col ? collen : 0;
			memcpy(parks[i].col, col, parks[i].collen);
			parks[i].key = malloc(klen);
			if (!parks[i].key) {
				parks[i].conn = NULL;
				free(pc_hcmd_take_park_args(NULL));
				return -1;
			}
			memcpy(parks[i].key, key, klen);
			parks[i].klen = klen;
			/* NULL unless the verb left a hash read's arguments */
			parks[i].hargs = pc_hcmd_take_park_args(&parks[i].hargl);
			parks[i].pres = pres;
			return 0;
		}
	free(pc_hcmd_take_park_args(NULL));
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
			free(parks[i].hargs);
			parks[i].hargs = NULL;
			parks[i].conn = NULL;
		}
}

/* ---- connections held on a read-through fetch (this worker's) --------- */

static __thread struct pc_conn **up_held;
static __thread int up_held_n, up_held_cap;

static void up_unhold(struct pc_conn *c)
{
	int i;

	for (i = 0; i < up_held_n; i++)
		if (up_held[i] == c) {
			up_held[i] = up_held[--up_held_n];
			break;
		}
	c->up_hold = 0;
}

void pc_conn_upstream_hold(void *conn, unsigned int req)
{
	struct pc_conn *c = conn;

	if (up_held_n == up_held_cap) {
		int nc = up_held_cap ? up_held_cap * 2 : 64;
		struct pc_conn **n = realloc(up_held, (size_t)nc * sizeof *n);

		if (!n) {
			/* not remembered: the completion would never find it,
			 * so better the command runs now, as a miss */
			LM_ERR("upstream: no memory to hold a connection\n");
			return;
		}
		up_held = n;
		up_held_cap = nc;
	}
	up_held[up_held_n++] = c;
	c->up_hold = req;
}

int pc_conn_upstream_verdict(void *conn)
{
	return conn ? ((struct pc_conn *)conn)->up_verdict : 0;
}

/* pc_conn_upstream_retry: a gated read lost its key before the handler read it - 1: gate
 * it again, verdict cleared so it fetches; 0: twice already, the answer stands */
void pc_conn_upstream_set_mark(void *conn, unsigned long long m)
{
	if (conn)
		((struct pc_conn *)conn)->up_mark = m;
}

unsigned long long pc_conn_upstream_mark(void *conn)
{
	return conn ? ((struct pc_conn *)conn)->up_mark : 0;
}

int pc_conn_upstream_retry(void *conn)
{
	struct pc_conn *c = conn;

	if (!c || c->up_lost >= 2)
		return 0;
	c->up_lost++;
	c->up_verdict = 0;
	return 1;
}

/* the native doors: a FRAME parks, not the connection (frames carry ids) */
struct up_frame {
	unsigned int req;
	struct pc_conn *c;
	char *frame;                       /* header + payload, a copy */
	size_t len;
	int cmd;                           /* a CMD method, else a data verb */
	unsigned long long mark;           /* the removal count when it parked */
};

static __thread unsigned int up_frame_pending;   /* set by the verb */
static __thread int up_frame_verdict;            /* on a re-dispatch */
static __thread unsigned long long up_frame_pmark, up_frame_mark;
static __thread struct up_frame *up_frames;
static __thread int up_frames_n, up_frames_cap;

void pc_conn_upstream_park_frame(unsigned int req)
{
	up_frame_pending = req;
}

/* CLIENT SETNAME perfcached-readthrough: another perfcached's fetch connection.  A miss is
 * a miss, never a fetch onward, so a key crosses at most one hop and no ring forms */
void pc_conn_set_rt_fetcher(void *conn)
{
	if (conn)
		((struct pc_conn *)conn)->rt_fetcher = 1;
}

int pc_conn_rt_fetcher(void *conn)
{
	return conn && ((struct pc_conn *)conn)->rt_fetcher;
}

int pc_conn_upstream_frame_verdict(void)
{
	return up_frame_verdict;
}

void pc_conn_upstream_park_frame_mark(unsigned long long m)
{
	up_frame_pmark = m;
}

unsigned long long pc_conn_upstream_frame_mark(void)
{
	return up_frame_mark;
}

void pc_conn_upstream_frame_regate(void)
{
	up_frame_verdict = 0;
}

/* park the frame at @base (@plen payload bytes) under the pending id:
 * 0 parked, -1 not (no memory - the caller answers "busy") */
static int up_frame_park(struct pc_conn *c, const char *base, size_t plen,
		int cmd)
{
	unsigned int req = up_frame_pending;
	char *cp;

	up_frame_pending = 0;
	if (up_frames_n == up_frames_cap) {
		int nc = up_frames_cap ? up_frames_cap * 2 : 64;
		struct up_frame *n = realloc(up_frames, (size_t)nc * sizeof *n);

		if (!n)
			return -1;
		up_frames = n;
		up_frames_cap = nc;
	}
	cp = malloc(PC_BIN_HDR + plen);
	if (!cp)
		return -1;
	memcpy(cp, base, PC_BIN_HDR + plen);
	up_frames[up_frames_n].req = req;
	up_frames[up_frames_n].c = c;
	up_frames[up_frames_n].frame = cp;
	up_frames[up_frames_n].len = PC_BIN_HDR + plen;
	up_frames[up_frames_n].cmd = cmd;
	up_frames[up_frames_n].mark = up_frame_pmark;
	up_frame_pmark = 0;
	up_frames_n++;
	return 0;
}

static void up_frames_drop(struct pc_conn *c)
{
	int i = 0;

	while (i < up_frames_n)
		if (up_frames[i].c == c) {
			free(up_frames[i].frame);
			up_frames[i] = up_frames[--up_frames_n];
		} else {
			i++;
		}
}

/* ---- lifecycle --------------------------------------------------------- */

static int conn_log_ok(void);          /* defined with pc_conn_accept */

static void udp_free(struct pc_conn *c);   /* with the push stream */
static void push_unstage(struct pc_conn *c);   /* with the staged pushes */
static void pause_unlink(struct pc_conn *c);   /* with the paused publishers */
static void out_pause(struct pc_conn *c);      /* with the resume pass */
static void out_unlink(struct pc_conn *c);
static int conn_pause(struct pc_conn *c);
static void wal_pause(struct pc_conn *c);      /* with the resume pass */
static void sync_unlink(struct pc_conn *c);
static void wal_unlink(struct pc_conn *c);
/* this worker's HTTP connections, counted at make and destroy: the wait reads this, not
 * the sweep's last answer, which predates the turn's accepts */
static __thread int http_open_here;
/* destroyed connections, closed and marked dead but not freed: an event later in the same
 * epoll batch may still name one.  pc_conn_reap() frees them before the next wait */
static __thread struct pc_conn *graveyard;

/* connections whose handshake a turn put off, oldest first.  Parked, they ask epoll for
 * nothing, so none stands in the ready list ahead of an established client's request */
static __thread struct pc_conn *hs_head, *hs_tail;
static __thread int hs_parked_here;

/* ---- a connection whose replies outrun its socket ----------------------
 * input is OFF while paused for output or for a full pub/sub queue: see conn_input_off */
static __thread struct pc_conn *outp_head;
static __thread int outp_here;
static unsigned long long outp_now_all, outp_pauses_all, outp_stalls_all;
/* writers held while this worker's WAL ring is nearly full */
static __thread struct pc_conn *walp_head;
static __thread int walp_here;
static __thread struct pc_conn *synch_head;
static __thread int synch_here;
static unsigned long long synch_now_all, synch_holds_all, synch_ms_all;
static unsigned long long walp_now_all, walp_pauses_all;
int pc_output_stall_s = 30;

/* a hold (a KEYS walk, a read-through fetch, a parked RESP command) serves nothing behind it, so
 * its input waits in the socket as a pause's does */
static inline int conn_input_off(const struct pc_conn *c)
{
	return c->ps_paused || c->out_paused || c->wal_paused ||
		c->enum_active || c->up_hold || c->park_hold;
}

/* the epoll read interest: none while input is off or a handshake is parked */
static inline uint32_t conn_ev_in(const struct pc_conn *c)
{
	return conn_input_off(c) || c->hs_parked ? 0 : EPOLLIN;
}

static inline size_t out_pending(const struct pc_conn *c)
{
	return c->out_len + (c->wire_len - c->wire_off);
}

/* ---- the io_uring backend, the connection side (c->ep < 0) -------------
 * the kernel reads c->wire while a send is in flight: nothing appends to, moves or frees it */
static __thread struct pc_ur *ur_ring;
static __thread struct pc_conn *ur_limbo;   /* dead, completions due: the last one frees it */
static __thread struct pc_conn *ur_starved_head;  /* no SQE to arm with */

#define UR_OP_MASK      15ULL
#define UR_OP_RECV      1ULL
#define UR_OP_POLLOUT   2ULL
#define UR_OP_SEND      3ULL
#define UR_UD(c, op)    ((uint64_t)(uintptr_t)(c) | (op))

static int conn_take(struct pc_conn *c, const char *buf, size_t n);
static void ur_serve(struct pc_conn *c, const char *buf, size_t n);

void pc_conn_uring_attach(struct pc_ur *r)
{
	ur_ring = r;
}

static struct pc_ur_sqe *ur_sqe(void)
{
	struct pc_ur_sqe *s = pc_ur_sqe(ur_ring);

	if (!s) {                          /* full: hand the batch over */
		pc_ur_submit(ur_ring, 0, 0);
		PC_IOSTAT_ADD(ur_enters, 1);
		s = pc_ur_sqe(ur_ring);
	}
	return s;
}

/* no SQE to arm with (the kernel refused a submit): try again next turn */
static void ur_starve(struct pc_conn *c)
{
	if (c->ur_starved)
		return;
	c->ur_starved = 1;
	c->ur_starved_next = ur_starved_head;
	ur_starved_head = c;
}

/* off that list: a destroyed connection may be freed before the next retry walks it */
static void ur_unstarve(struct pc_conn *c)
{
	struct pc_conn **pp = &ur_starved_head;

	if (!c->ur_starved)
		return;
	while (*pp && *pp != c)
		pp = &(*pp)->ur_starved_next;
	if (*pp)
		*pp = c->ur_starved_next;
	c->ur_starved = 0;
	c->ur_starved_next = NULL;
}

/* the requests in flight, brought in line with what the connection wants */
static void ur_events(struct pc_conn *c)
{
	struct pc_ur_sqe *s;
	int in = !conn_input_off(c) && !c->hs_parked && !c->dead;

	if (in && !c->ur_recv) {
		if (!(s = ur_sqe())) {
			ur_starve(c);
			return;
		}
		pc_ur_prep_recv_multi(s, c->fd, PC_UR_BGID, UR_UD(c, UR_OP_RECV));
		c->ur_recv = 1;
		c->ur_inflight++;
	} else if (!in && c->ur_recv && !c->ur_cancel) {
		if (!(s = ur_sqe())) {
			ur_starve(c);
			return;
		}
		pc_ur_prep_cancel(s, UR_UD(c, UR_OP_RECV), PC_UR_UD_IGNORE);
		c->ur_cancel = 1;
	}
	/* in && ur_recv && ur_cancel: the receive is ending - its last
	 * completion arms it again (pc_conn_ur_complete) */
	if (c->want_write && !c->ur_pollout && !c->dead) {
		if (!(s = ur_sqe())) {
			ur_starve(c);
			return;
		}
		pc_ur_prep_poll(s, c->fd, POLLOUT, UR_UD(c, UR_OP_POLLOUT));
		c->ur_pollout = 1;
		c->ur_inflight++;
	}
}

/* the worker's turn: whatever starved is armed now */
void pc_conn_ur_retry(void)
{
	struct pc_conn *c, *next;

	c = ur_starved_head;
	ur_starved_head = NULL;
	for (; c; c = next) {
		next = c->ur_starved_next;
		c->ur_starved = 0;
		c->ur_starved_next = NULL;
		if (!c->dead)
			ur_events(c);
	}
}

/* the destroy, before its close(): end what is in flight */
static void ur_drop(struct pc_conn *c)
{
	struct pc_ur_sqe *s;

	if (c->ur_recv && !c->ur_cancel && (s = ur_sqe()) != NULL) {
		pc_ur_prep_cancel(s, UR_UD(c, UR_OP_RECV), PC_UR_UD_IGNORE);
		c->ur_cancel = 1;
	}
	if (c->ur_pollout && (s = ur_sqe()) != NULL)
		pc_ur_prep_cancel(s, UR_UD(c, UR_OP_POLLOUT), PC_UR_UD_IGNORE);
	if (c->ur_send && (s = ur_sqe()) != NULL)
		pc_ur_prep_cancel(s, UR_UD(c, UR_OP_SEND), PC_UR_UD_IGNORE);
	/* submit before the close: a queued SQE names the fd NUMBER, taken only at submit.  Closed
	 * first, the number can go to the next accept and the SEND or receive hits THAT socket */
	pc_ur_submit(ur_ring, 0, 0);
	PC_IOSTAT_ADD(ur_enters, 1);
	shutdown(c->fd, SHUT_RDWR);        /* ends them regardless */
}

/* a dead connection whose last completion has come: free it, unless the
 * graveyard still holds it (then the reap frees it) */
static void ur_settle(struct pc_conn *c)
{
	if (!c->dead || c->ur_inflight > 0 || !c->ur_in_limbo)
		return;
	if (c->prev)
		c->prev->next = c->next;
	else
		ur_limbo = c->next;
	if (c->next)
		c->next->prev = c->prev;
	free(c);
}

static void hs_unlink(struct pc_conn *c)
{
	if (!c->hs_parked)
		return;
	if (c->hs_prev)
		c->hs_prev->hs_next = c->hs_next;
	else
		hs_head = c->hs_next;
	if (c->hs_next)
		c->hs_next->hs_prev = c->hs_prev;
	else
		hs_tail = c->hs_prev;
	c->hs_next = c->hs_prev = NULL;
	c->hs_parked = 0;
	hs_parked_here--;
}

void conn_destroy(struct pc_conn *c)
{
	if (c->dead)                       /* once, whoever asks */
		return;
	/* a reply that did not fit is why, unless something named a reason first */
	if (!c->why && c->out_failed)
		c->why = c->out_failed == 1 ? "slow consumer: replies past the "
			"8 MB output cap, unread" : "out of memory for its replies";
	/* only a connection the pub/sub engine has seen takes the engine's lock */
	if (c->ps_used)
		pc_pubsub_conn_closed(c);
	push_unstage(c);
	pause_unlink(c);
	out_unlink(c);
	wal_unlink(c);
	sync_unlink(c);
	hs_unlink(c);
	if (c->udp)
		udp_free(c);
	if (c->http_only)
		http_open_here--;
	if (c->client_counted) {
		PC_RESP_DEC(pc_clients_open);
		c->client_counted = 0;
	}
	/* out of the open-connections gauge, by the open_counted memo that counted it in */
	if (c->open_counted) {
		if (c->resp_only)
			PC_RESP_DEC(pc_resp_open);
		else if (c->dialect == PC_D_BIN)
			PC_RESP_DEC(pc_nat_bin_open);
		else if (c->dialect == PC_D_RESP)
			PC_RESP_DEC(pc_nat_resp_open);
		c->open_counted = 0;
	}
	/* a connection that settled a protocol says goodbye; one that never sent a byte (a port
	 * probe) stays silent unless it died for a reason, e.g. a handshake that never happened */
	if (!c->http_only && (c->dialect != PC_D_SNIFF || c->why) &&
	    conn_log_ok()) {
		char thr[40] = "";

		if (c->out_pauses_n)
			snprintf(thr, sizeof thr, ", throttled %u time%s",
				c->out_pauses_n, c->out_pauses_n == 1 ? "" : "s");
		LM_NOTICE("client %s disconnected (%s) after %us, %llu "
			"requests%s\n", pc_obs_conn_addr(c->obs),
			c->why ? c->why : "peer closed",
			(unsigned)(get_ticks() - c->born),
			pc_obs_conn_cmds(c->obs), thr);
	}
	pc_obs_conn_del(c->obs);
	enum_abort(c);
	park_drop_conn(c);
	pc_mget_drop_conn(c);
	up_unhold(c);
	up_frames_drop(c);
	if (c->prev)
		c->prev->next = c->next;
	else if (c->list)
		*c->list = c->next;
	if (c->next)
		c->next->prev = c->prev;
	if (c->ep < 0) {
		ur_unstarve(c);
		ur_drop(c);
	}
	if (c->ur_send) {                  /* the kernel still reads it: */
		c->ur_wire_hold = c->wire; /* the send's completion frees it */
		c->wire = NULL;
	}
	close(c->fd);
	if (c->bufs_held) {
		bufs_held_here--;
		__atomic_sub_fetch(&bufs_held_all, 1, __ATOMIC_RELAXED);
		c->bufs_held = 0;
	}
	pc_memguard_freed(c->in_cap + c->out_cap + c->wire_cap + c->raw_cap +
		sizeof *c, 0);
	free(c->in);
	free(c->out);
	free(c->wire);
	free(c->raw);
	c->in = c->out = NULL;
	c->wire = c->raw = NULL;
	pc_tls_free(c->tls);
	c->tls = NULL;
	sodium_memzero(&c->hs, sizeof c->hs);
	sodium_memzero(&c->send_cs, sizeof c->send_cs);
	sodium_memzero(&c->recv_cs, sizeof c->recv_cs);
	c->dead = 1;
	c->prev = NULL;
	c->list = NULL;
	c->next = graveyard;
	graveyard = c;
}

void pc_conn_reap(void)
{
	struct pc_conn *c;

	while ((c = graveyard) != NULL) {
		graveyard = c->next;
		if (c->ur_inflight > 0) {      /* completions still due */
			c->ur_in_limbo = 1;
			c->prev = NULL;
			c->next = ur_limbo;
			if (ur_limbo)
				ur_limbo->prev = c;
			ur_limbo = c;
			continue;
		}
		free(c);
	}
}

void pc_conn_destroy_all(struct pc_conn **list)
{
	/* conn_destroy advances the head through c->list - aliasing the
	 * analyzer cannot track; each iteration reads a FRESH head */
	/* NOLINTNEXTLINE(clang-analyzer-unix.Malloc) */
	while (*list)
		conn_destroy(*list); /* NOLINT(clang-analyzer-unix.Malloc) */
	pc_conn_reap();
}

/* an IDLE connection's buffers go back once it has been quiet PC_IDLE_RELEASE_S with nothing
 * buffered or in flight, so busy ones do not churn; every call stamps the worker's tick */
#define PC_IDLE_RELEASE_S 2

int pc_conn_bufs_held(void)
{
	return bufs_held_here;
}

int pc_conn_bufs_held_all(void)
{
	return __atomic_load_n(&bufs_held_all, __ATOMIC_RELAXED);
}

void pc_conn_sweep_idle(struct pc_conn **list)
{
	static __thread unsigned int last;
	struct pc_conn *c;
	unsigned int now = get_ticks();

	conn_now = now;
	if (!bufs_held_here || now == last)
		return;
	last = now;
	for (c = *list; c; c = c->next) {
		size_t b;

		if (!c->bufs_held || c->http_only || c->dead ||
		    now - c->last_io < PC_IDLE_RELEASE_S)
			continue;
		if (c->in_len || c->out_len || c->wire_off < c->wire_len ||
		    c->raw_len || c->enum_active || c->up_hold ||
		    c->want_write || c->close_after || c->ur_send)
			continue;
		b = c->in_cap + c->out_cap + c->wire_cap + c->raw_cap;
		free(c->in);
		free(c->out);
		free(c->wire);
		free(c->raw);
		c->in = c->out = c->wire = c->raw = NULL;
		c->in_cap = c->out_cap = c->wire_cap = c->raw_cap = 0;
		c->wire_off = c->wire_len = 0;
		c->bufs_held = 0;
		bufs_held_here--;
		__atomic_sub_fetch(&bufs_held_all, 1, __ATOMIC_RELAXED);
		pc_memguard_freed(b, 1);
	}
}

/* this worker's open HTTP connections: whether it must keep waking to sweep them */
int pc_conn_http_open(void)
{
	return http_open_here;
}

/* HTTP only, against a request head that never arrives; native and RESP links are long-lived
 * by design and never swept.  Once a tick: the timeout is whole seconds */
int pc_conn_sweep_http(struct pc_conn **list, int timeout_s)
{
	static __thread unsigned int last;
	struct pc_conn *c, *next;
	unsigned int now;
	int open = 0;

	if (!http_open_here)
		return 0;
	now = get_ticks();
	if (now == last)
		return http_open_here;
	last = now;
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
unsigned int pc_resp_slots_hits, pc_resp_slots_builds;
unsigned int pc_resp_reqs, pc_nat_conns, pc_nat_bin_conns,
	pc_nat_resp_conns, pc_nat_bin_reqs, pc_nat_resp_reqs,
	pc_nat_bin_records, pc_nat_plain_conns, pc_nat_plain_refused;
unsigned int pc_resp_open, pc_nat_bin_open, pc_nat_resp_open;
int pc_max_clients;                             /* 0 = no limit */
const char *pc_max_clients_basis = "descriptors";  /* or memory, configured */
int pc_keepalive_s;                             /* 0 = off */
unsigned int pc_clients_open;
unsigned long long pc_clients_refused;
static int clients_full_logged;                 /* one line per transition */
/* the door totals are 64-bit on the read side: workers keep 32-bit relaxed increments, and
 * each read folds the distance since the last (safe across one wrap; read every second) */
static unsigned int *const dw_src[PC_DW_N] = {
	&pc_resp_conns, &pc_resp_rejected, &pc_resp_authfail, &pc_resp_reqs,
	&pc_resp_slots_hits, &pc_resp_slots_builds, &pc_nat_conns,
	&pc_nat_bin_conns, &pc_nat_resp_conns, &pc_nat_bin_reqs,
	&pc_nat_resp_reqs, &pc_nat_bin_records, &pc_nat_plain_conns,
	&pc_nat_plain_refused
};
static unsigned int dw_last[PC_DW_N];
static unsigned long long dw_total[PC_DW_N], dw_base[PC_DW_N];
static pthread_mutex_t dw_mx = PTHREAD_MUTEX_INITIALIZER;

static void dw_fold(void)                      /* under dw_mx */
{
	int i;

	for (i = 0; i < PC_DW_N; i++) {
		unsigned int cur = __atomic_load_n(dw_src[i], __ATOMIC_RELAXED);

		dw_total[i] += (unsigned int)(cur - dw_last[i]);
		dw_last[i] = cur;
	}
}

void pc_doors_widen(void)
{
	pthread_mutex_lock(&dw_mx);
	dw_fold();
	pthread_mutex_unlock(&dw_mx);
}

unsigned long long pc_door_since(enum pc_door_ctr c)
{
	unsigned long long v;

	pthread_mutex_lock(&dw_mx);
	dw_fold();
	v = dw_total[c] - dw_base[c];
	pthread_mutex_unlock(&dw_mx);
	return v;
}

void pc_doors_test_start(unsigned int v)
{
	int i;

	pthread_mutex_lock(&dw_mx);
	for (i = 0; i < PC_DW_N; i++) {
		__atomic_store_n(dw_src[i], v, __ATOMIC_RELAXED);
		dw_last[i] = v;
	}
	pthread_mutex_unlock(&dw_mx);
}

/* the running totals restart from here (see daemon.h), based on the 64-bit totals, so a
 * counter that wraps after the reset still counts up from it */
void pc_doors_reset(void)
{
	pthread_mutex_lock(&dw_mx);
	dw_fold();
	memcpy(dw_base, dw_total, sizeof dw_base);
	pthread_mutex_unlock(&dw_mx);
}

/* connect/disconnect lines: at most CONN_LOG_BURST per CONN_LOG_WIN_S, then one count, so a
 * reconnect loop stays visible but bounded.  Unlocked: a torn count mis-limits one line */
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

/* @dialect: "binary" or "resp" - what the first byte settled */
static void conn_log_open(struct pc_conn *c, const char *dialect)
{
	pc_obs_conn_door(c->obs, NULL, c->resp_only ? "resp" : dialect,
		c->encrypted);
	if (c->http_only || !conn_log_ok())
		return;
	LM_NOTICE("client %s connected (%s, %s)\n", pc_obs_conn_addr(c->obs),
		dialect, c->encrypted ? "encrypted" : c->plain_auth ?
		"authenticated, plaintext" : "plaintext");
}

/* refused peers, the refusal sent and write side shut, drained a turn at a time so close() sends
 * a FIN, not a RST that loses it; closed at EOF or after DENY_MS.  Per worker, no lock */
#define DENY_MAX 32
#define DENY_MS  1000
static __thread struct { int fd; long long until; } deny_q[DENY_MAX];
static __thread int deny_n;

long long deny_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int pc_conn_deny_open(void)
{
	return deny_n;
}

void pc_conn_sweep_deny(void)
{
	long long now = deny_now();
	char sink[512];
	int i = 0, k, done;
	ssize_t r;

	while (i < deny_n) {
		done = now >= deny_q[i].until;
		for (k = 0; k < 8 && !done; k++) {   /* 4 KB a turn, at most */
			r = recv(deny_q[i].fd, sink, sizeof sink, MSG_DONTWAIT);
			if (r > 0)
				continue;
			if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK &&
			    errno != EINTR))
				done = 1;              /* its end, or an error */
			break;
		}
		if (done) {
			close(deny_q[i].fd);
			deny_q[i] = deny_q[--deny_n];
		} else
			i++;
	}
}

static void deny_park(int fd)
{
	if (deny_n == DENY_MAX) {              /* full: the oldest goes now */
		int o = 0, i;

		for (i = 1; i < deny_n; i++)
			if (deny_q[i].until < deny_q[o].until)
				o = i;
		close(deny_q[o].fd);
		deny_q[o] = deny_q[--deny_n];
	}
	deny_q[deny_n].fd = fd;
	deny_q[deny_n].until = deny_now() + DENY_MS;
	deny_n++;
	pc_conn_sweep_deny();                  /* its request may be here already */
}

int pc_conn_accept(int ep, int lfd, int plaintext_ok, int kind,
		const struct pc_psk_ctx *psk, struct pc_conn **list)
{
	struct epoll_event ev;
	struct pc_conn *c;
	int tls = kind & PC_LK_TLS;
	pc_addr peer;
	socklen_t plen;
	int fd, one = 1, refused, taken = 0;

	kind &= ~PC_LK_TLS;
	for (;;) {
		/* at most PC_ACCEPT_PER_TURN a turn; the rest wait in the
		 * backlog and the level-triggered listener returns next turn */
		if (taken++ >= PC_ACCEPT_PER_TURN)
			return 1;              /* more may be waiting */
		plen = sizeof peer;
		/* the peer, for every connection: CLIENT LIST shows it */
		fd = accept(lfd, &peer.sa, &plen);
		if (fd < 0)
			return 0;
		if (kind == PC_LK_RESP && !pc_resp_peer_allowed(&peer, plen)) {
			/* refused BEFORE a byte is read: a RESP listener has
			 * no handshake, so the allow-list is the door */
			PC_RESP_BUMP(pc_resp_rejected);
			close(fd);
			continue;
		}
		if (kind == PC_LK_HTTP &&
		    !pc_http_peer_allowed(&peer, plen)) {
			/* a 403, not a silent close (a browser shows that as a
			 * dead daemon); the body names the mechanism, nothing more */
			static const char body[] =
				"perfcached: this address is not in http_allow\n";
			char deny[256];
			int dn = snprintf(deny, sizeof deny,
				"HTTP/1.0 403 Forbidden\r\nContent-Type: text/plain\r\n"
				"Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
				sizeof body - 1, body);

			(void)send(fd, deny, (size_t)dn, MSG_NOSIGNAL | MSG_DONTWAIT);
			shutdown(fd, SHUT_WR);
			/* parked, not drained here: a worker never waits on a refused peer */
			deny_park(fd);
			continue;
		}
		if (kind == PC_LK_HTTP && !http_rate_ok(&peer)) {
			/* over http_rate: answered, never read */
			static const char body[] = "perfcached: too many requests "
				"from this address - see http_rate\n";
			char head[320];
			int hn = snprintf(head, sizeof head,
				"HTTP/1.0 429 Too Many Requests\r\nRetry-After: 1\r\n"
				"Content-Type: text/plain\r\nContent-Length: %zu\r\n"
				"Connection: close\r\n\r\n%s", sizeof body - 1, body);

			(void)send(fd, head, (size_t)hn, MSG_NOSIGNAL | MSG_DONTWAIT);
			shutdown(fd, SHUT_WR);
			deny_park(fd);
			continue;
		}
		fcntl(fd, F_SETFL, O_NONBLOCK);
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
		/* data links are never idle-swept: keepalive finds a peer that died
		 * silently - probes after keepalive_s idle, a third of that apart, 3 misses */
		if (pc_keepalive_s > 0 && kind != PC_LK_HTTP) {
			int ka = pc_keepalive_s, iv = ka / 3 > 0 ? ka / 3 : 1, cnt = 3;

			setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
			setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &ka, sizeof ka);
			setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &iv, sizeof iv);
			setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
		}
		/* ask for 1 MB, the kernel clamps to net.core.wmem_max: pipelined GET
		 * replies outgrow the 16 KB default, SET replies never do */
		{
			int sndbuf = 1 << 20;

			setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf,
				sizeof sndbuf);
		}

		/* max_clients counts the data doors only (HTTP answers at the limit); RESP is told
		 * Redis's text now, a plaintext native client at its first request */
		/* past max_clients the memory guard takes one connection from its
		 * budget (memguard.h); an empty budget is the same refusal */
		/* an encrypted native client is refused here with the RESP text, before the
		 * Noise handshake; its msg1 is drained first so the close is a FIN, not a reset */
		refused = 0;
		if (kind != PC_LK_HTTP) {
			int full = pc_max_clients > 0 &&
				(int)PC_RESP_READ(pc_clients_open) >= pc_max_clients;
			int lowmem = !full && !pc_memguard_admit();

			if (full || lowmem) {
				__atomic_fetch_add(&pc_clients_refused, 1,
					__ATOMIC_RELAXED);
				if (full && !__atomic_exchange_n(&clients_full_logged, 1,
				        __ATOMIC_RELAXED))
					LM_WARN("clients: max_clients %d reached - "
						"refusing new connections until one "
						"closes\n", pc_max_clients);
				if (kind == PC_LK_RESP || plaintext_ok != PC_DOOR_PLAIN) {
					static const char full[] =
						"-ERR max number of clients reached\r\n";
					char sink[512];

					if (kind != PC_LK_RESP)    /* its msg1 */
						(void)recv(fd, sink, sizeof sink,
							MSG_DONTWAIT);
					if (write(fd, full, sizeof full - 1) < 0)
						{ /* the close is the message */ }
					/* parked as the HTTP refusals are: bytes the client sends
					 * after a plain close would turn it into a reset */
					shutdown(fd, SHUT_WR);
					deny_park(fd);
					continue;
				}
				refused = full ? 1 : 2;
			} else if (__atomic_load_n(&clients_full_logged,
			        __ATOMIC_RELAXED) &&
			        __atomic_exchange_n(&clients_full_logged, 0,
			                __ATOMIC_RELAXED)) {
				LM_NOTICE("clients: below max_clients %d again\n",
					pc_max_clients);
			}
		}
		c = calloc(1, sizeof *c);
		if (c && tls && !(c->tls = pc_tls_new())) {
			free(c);                   /* no session, no connection */
			c = NULL;
		}
		if (!c) {
			close(fd);
			continue;
		}
		c->refused = refused;
		if (kind != PC_LK_HTTP && !refused) {
			PC_RESP_BUMP(pc_clients_open);
			c->client_counted = 1;
		}
		c->obs = pc_obs_conn_add(&peer.sa, plen,
			fd, kind == PC_LK_RESP);
		pc_obs_conn_door(c->obs, kind == PC_LK_RESP ? "resp"
			: kind == PC_LK_HTTP ? "http" : "native",
			kind == PC_LK_RESP ? "resp" : "?", plaintext_ok != PC_DOOR_PLAIN);
		c->fd = fd;
		c->ep = ep;
		c->list = list;
		c->resp_col[0] = '0';          /* RESP db 0 = collection "0" */
		c->resp_collen = 1;
		c->resp_only = kind == PC_LK_RESP;
		c->http_only = kind == PC_LK_HTTP;
		c->peer_loop = pc_addr_is_loopback(&peer);
		c->http_loop = c->http_only &&    /* ::1 too */
			pc_addr_is_loopback(&peer);
		if (c->http_only)
			http_open_here++;
		c->born = get_ticks();
		c->last_io = conn_now;
		if (kind == PC_LK_RESP) {
			c->dialect = PC_D_RESP;   /* pinned: no sniff, ever */
			/* unauthenticated until AUTH when a password is set */
			c->resp_authed = !pc_resp_password_set();
			PC_RESP_BUMP(pc_resp_conns);
			PC_RESP_BUMP(pc_resp_open);
			c->open_counted = 1;
		} else {
			c->resp_authed = 1;
			if (kind == PC_LK_NATIVE)
				PC_RESP_BUMP(pc_nat_conns);
		}
		if (plaintext_ok != PC_DOOR_PLAIN) {
			c->encrypted = 1;
			c->pa_ok = plaintext_ok == PC_DOOR_AUTH;
			/* the CURRENT table - a reload of [secrets] client
			 * publishes a new one for connections accepted after it */
			c->psk = psk ? (pc_psk_current() ? pc_psk_current() : psk) : NULL;
		}
		if (kind == PC_LK_RESP)
			conn_log_open(c, "resp door");   /* pinned, known now */
		c->next = *list;
		if (c->next)
			c->next->prev = c;
		*list = c;

		if (ep < 0) {                  /* an io_uring worker */
			ur_events(c);
			continue;
		}
		ev.events = EPOLLIN;
		ev.data.ptr = c;
		/* no event could ever name it, so nothing would close it */
		if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) != 0) {
			c->why = "epoll registration failed";
			conn_destroy(c);
		}
	}
}

/* ---- output: stage plaintext, then flush (encrypting if needed) -------- */

static int tls_wire(void *ctx, const char *p, size_t n);

static int encrypt_pending(struct pc_conn *c)
{
	size_t nrec = (c->out_len + PC_NOISE_MAXPT - 1) / PC_NOISE_MAXPT, off = 0;

	/* room for every record first: one sealed and not queued would put the nonces out of
	 * step, and the plaintext left in c->out would be sealed again */
	if (wire_reserve(c, c->out_len + nrec * (2 + PC_NOISE_TAGLEN)) < 0)
		return -1;
	while (off < c->out_len) {
		size_t chunk = c->out_len - off;
		uint8_t *rec = (uint8_t *)c->wire + c->wire_len;
		int cl;

		if (chunk > PC_NOISE_MAXPT)
			chunk = PC_NOISE_MAXPT;
		cl = pc_transport_encrypt(&c->send_cs,
			(const uint8_t *)c->out + off, chunk, rec + 2);
		if (cl < 0) {
			/* what was sealed is queued: a re-run seals only the rest */
			memmove(c->out, c->out + off, c->out_len - off);
			c->out_len -= off;
			return -1;
		}
		put16(rec, (uint16_t)cl);
		c->wire_len += 2 + (size_t)cl;
		off += chunk;
	}
	c->out_len = 0;
	return 0;
}

/* hand [wire_off, wire_len) to the ring; its completion (pc_conn_ur_complete) advances
 * wire_off and flushes again.  -1: no SQE - the caller writes it now instead */
static int ur_send_wire(struct pc_conn *c)
{
	struct pc_ur_sqe *s = ur_sqe();

	if (!s)
		return -1;
	pc_ur_prep_send(s, c->fd, c->wire + c->wire_off,
		c->wire_len - c->wire_off, UR_UD(c, UR_OP_SEND));
	c->ur_send = 1;
	c->ur_inflight++;
	PC_IOSTAT_ADD(ur_sends, 1);
	return 0;
}

int conn_flush(struct pc_conn *c)
{
	struct epoll_event ev;
	size_t keep = 0, front;
	ssize_t n;

	/* a reply that did not fit was lost: the stream is out of step, the caller closes it */
	if (c->out_failed)
		return -1;
	/* a send in flight owns c->wire: staged replies wait in c->out for its completion,
	 * and CLIENT LIST sees them pending */
	if (c->ur_send) {
		size_t p = c->out_len + (c->wire_len - c->wire_off);

		if (p != c->obs_pending) {
			c->obs_pending = p;
			pc_obs_conn_pending(c->obs, p);
		}
		return 0;
	}
	/* records OpenSSL wrote while a send was in flight (a KeyUpdate's answer) */
	if (c->tls && !c->out_len && pc_tls_flush(c->tls, tls_wire, c) < 0)
		return -1;
	/* fsync = sync: what was staged from the first held reply on stays until its write is synced */
	if (c->sync_held) {
		if (pc_wal_synced_low() >= c->sync_seq)
			sync_unlink(c);
		else
			keep = c->out_len - (c->sync_from < c->out_len ? c->sync_from : c->out_len);
	}
	front = c->out_len -= keep;
	/* move staged plaintext into the socket buffer */
	if (c->out_len) {
		if (c->encrypted) {
			if (!c->established) {
				c->out_len = 0;        /* no app data before handshake */
			} else if (encrypt_pending(c) < 0) {
				return -1;
			}
		} else if (c->tls) {           /* held until the handshake is done */
			if (pc_tls_up(c->tls)) {
				if (pc_tls_output(c->tls, c->out, c->out_len, tls_wire, c) < 0)
					return -1;
				c->out_len = 0;
			}
		} else {
			if (wire_append(c, c->out, c->out_len) < 0)
				return -1;
			c->out_len = 0;
		}
	}
	if (keep) {                        /* the held tail, to the front once the rest left */
		if (!c->out_len) {
			memmove(c->out, c->out + front, keep);
			c->sync_from = 0;
		}
		c->out_len += keep;
	}

	if (c->ep < 0 && c->wire_off < c->wire_len && ur_send_wire(c) == 0)
		return 0;                  /* it leaves with the wait */
	while (c->wire_off < c->wire_len) {
		n = write(c->fd, c->wire + c->wire_off, c->wire_len - c->wire_off);
		PC_IOSTAT_ADD(writes, 1);
		if (n > 0) {
			c->wire_off += (size_t)n;
			c->out_progress = conn_now;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			if (!c->want_write && c->ep < 0) {
				c->want_write = 1;
				ur_events(c);
			} else if (!c->want_write) {
				ev.events = conn_ev_in(c) | EPOLLOUT;
				ev.data.ptr = c;
				epoll_ctl(c->ep, EPOLL_CTL_MOD, c->fd, &ev);
				c->want_write = 1;
			}
			/* what the socket would not take - the figure that
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
	if (c->want_write && c->ep < 0)    /* its poll may still fire, */
		c->want_write = 0;         /* and finds nothing to write */
	else if (c->want_write) {
		ev.events = conn_ev_in(c);
		ev.data.ptr = c;
		epoll_ctl(c->ep, EPOLL_CTL_MOD, c->fd, &ev);
		c->want_write = 0;
	}
	return 0;
}

/* ---- the reply scratch ------------------------------------------------- */

/* per-thread reply scratch, never freed (workers live as long as the daemon): a 1 MB malloc
 * per request is an mmap cycle under a non-caching allocator.  0: the reply, 1: RESP assembly */
static __thread char *jw_scratch[2];

char *scratch_buf(int i)
{
	if (!jw_scratch[i]) {
		jw_scratch[i] = malloc(JW_REPLY_CAP);
		/* resident whole, not as far as the largest reply */
		pc_touch_pages(jw_scratch[i], JW_REPLY_CAP);
	}
	return jw_scratch[i];
}

/* the clock carried from one request's end to the next one's start within a drain: one read
 * a request.  Zeroed per drain; the RESP wrapper refreshes it too */
__thread unsigned long long obs_carry;

/* ---- binary frames ----------------------------------------------------- */

/* a response frame.  @trail rides after the payload under PC_BIN_F_MOVED: the routing map's
 * stamp, [term u32][seq u32], so a client holding another stamp re-learns the map */
static int bin_reply_ex(struct pc_conn *c, uint64_t id, int flags,
		const char *payload, size_t n, const char *trail, size_t tn)
{
	char hdr[PC_BIN_HDR];
	size_t at = c->out_len;

	hdr[0] = (char)PC_BIN_MAGIC;
	hdr[1] = PC_BIN_VER;
	hdr[2] = PC_BIN_RSP;
	hdr[3] = (char)flags;
	put32(hdr + 4, (uint32_t)(n + tn));
	put64(hdr + 8, id);
	/* all of the frame or none of it: a lone header would frame what follows */
	if (out_append(c, hdr, sizeof hdr) < 0 || out_append(c, payload, n) < 0 ||
	        (tn && out_append(c, trail, tn) < 0)) {
		c->out_len = at;
		return -1;
	}
	return 0;
}

static int bin_reply(struct pc_conn *c, uint64_t id, int flags,
		const char *payload, size_t n)
{
	return bin_reply_ex(c, id, flags, payload, n, NULL, 0);
}

/* a 'b' node's bytes under @key in map @map, when there is one */
static void tv_span(const struct pc_tv *v, int map, const char *key,
		const char **p, size_t *n)
{
	int k = pc_tv_get(v, map, key);

	if (k >= 0 && v->n[k].type == 'b') {
		*p = (const char *)v->n[k].p;
		*n = v->n[k].len;
	}
}

/* one PC_VERB_CMD request: [verb 19][mlen u8][method][params tree?] -> one tree item or an
 * error frame.  The params index is per worker and only grows.  Bytes consumed, -1 fatal */
static int cmd_frame(struct pc_conn *c, const char *base, uint64_t id,
		const char *pl, size_t plen)
{
	static __thread struct pc_tv tv;
	struct pc_tw tw;
	const char *method, *errmsg = "error", *col = NULL, *key = NULL;
	size_t mlen, collen = 0, klen = 0;
	unsigned char *resbuf;
	unsigned int park_req = 0;
	int params = -1, rc, outcome = 0;

	mlen = plen >= 2 ? (unsigned char)pl[1] : 0;
	if (!mlen || 2 + mlen > plen) {
		if (bin_reply(c, id, PC_BIN_F_ERR, "invalid request", 15) < 0)
			return -1;
		return (int)(PC_BIN_HDR + plen);
	}
	method = pl + 2;
	if (plen > 2 + mlen) {
		/* the parameters: ONE map, or nothing at all */
		if (pc_tv_parse(&tv, (const unsigned char *)pl + 2 + mlen,
		        plen - 2 - mlen) != 0 || tv.n[0].type != 'm') {
			if (bin_reply(c, id, PC_BIN_F_ERR, "bad params", 10) < 0)
				return -1;
			return (int)(PC_BIN_HDR + plen);
		}
		params = 0;
	} else {
		tv.nn = 0;                     /* no parameters: no nodes to read */
	}

	resbuf = (unsigned char *)scratch_buf(0);
	if (!resbuf)
		return -1;
	pc_tw_init(&tw, resbuf, JW_REPLY_CAP);
	{
		/* timed and counted like a RESP command, as cmd:<method>; the parameter
		 * lookups below run only for a slow-log or query-log entry */
		unsigned long long qt0 = obs_carry ? obs_carry : pc_obs_usec_now(), qdt;
		int qlog = pc_obs_qlog_on();

		/* counted on every door; the METHOD, so last_cmd reads `ping`, not `cmd` */
		pc_obs_conn_touch(c->obs, method, mlen, get_ticks());
		ev_origin(c, "binary");
		rc = pc_verb_cmd(method, mlen, &tv, params, &tw, &errmsg,
			&park_req, &c->privileged, &outcome, c);
		if (rc < 0 && errmsg == pc_unknown_method)
			pc_obs_unknown(CLUNK_BIN, method, mlen, NULL, 0, c->obs);
		obs_carry = pc_obs_usec_now();
		qdt = obs_carry - qt0;
		/* a parked frame runs again on completion and is counted then, not twice */
		if (!up_frame_pending) {
			/* "cmd:", a COLON: a RESP name may hold a dot (JSON.GET),
			 * never a colon; not bin:, so method `get` keeps its own row */
			char cn[32] = "cmd:";      /* length-counted, never a C string */
			size_t ml = mlen;

			if (ml > sizeof cn - 5)
				ml = sizeof cn - 5;
			/* the row slot of the last method on this worker: a door
			 * serves one or two, so the hash is paid on a change only */
			static __thread char lm[32];
			static __thread size_t lml;
			static __thread void *lslot;

			memcpy(cn + 4, method, ml);
			if (lml != 4 + ml || memcmp(lm, cn, 4 + ml)) {
				lml = 4 + ml;
				memcpy(lm, cn, lml);
				lslot = pc_obs_cmd_slot(cn, lml);
			}
			pc_obs_cmd_at(lslot, qdt);
			if (pc_obs_slow_would(qdt)) {
				char *av[3];
				size_t al[3];
				int na = 1;

				tv_span(&tv, params, "col", &col, &collen);
				tv_span(&tv, params, "key", &key, &klen);
				av[0] = cn;
				al[0] = 4 + ml;
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
		}
		if (qlog && !up_frame_pending) {
			tv_span(&tv, params, "col", &col, &collen);
			tv_span(&tv, params, "key", &key, &klen);
			pc_obs_qlog("bin", c->obs, method, mlen, col, collen, key,
				klen, rc < 0 ? "err"
				: outcome == PC_OUT_HIT ? "hit"
				: outcome == PC_OUT_MISS ? "miss" : "ok", qdt);
		}
	}
	{
		/* an mget waits on lookups of keys on other nodes - the
		 * frame's reply comes when they answer, under its id */
		struct pc_mget *mg = pc_mget_take();

		if (mg) {
			pc_tw_free(&tw);
			if (pc_mget_attach(mg, c, PC_MGET_DOOR_CMD, base + 8) == 0)
				return (int)(PC_BIN_HDR + plen);
			pc_mget_free(mg);
			if (bin_reply(c, id, PC_BIN_F_ERR, "out of memory", 13) < 0)
				return -1;
			return (int)(PC_BIN_HDR + plen);
		}
	}
	if (up_frame_pending) {
		/* waiting on a read-through fetch - the frame parks */
		pc_tw_free(&tw);
		if (rc == 0 && up_frame_park(c, base, plen, 1) == 0)
			return (int)(PC_BIN_HDR + plen);
		up_frame_pending = 0;
		rc = -1;
		errmsg = "busy";
	}
	if (rc == 0 && park_req) {
		/* deferred to a cluster pull/forward: the 8 id bytes park as
		 * they sit on the wire (LE), the reply comes as a tree */
		tv_span(&tv, params, "col", &col, &collen);
		tv_span(&tv, params, "key", &key, &klen);
		if (col && key && park_add(c, park_req, base + 8, col, collen,
		        key, klen, PARK_CMD, 0) == 0) {
			pc_tw_free(&tw);       /* no-op unless a verb grew one */
			return (int)(PC_BIN_HDR + plen);
		}
		/* could not park: the verb pre-wrote its OWN failure shape
		 * (a set answers stored:false); one that wrote nothing is refused */
		if (!tw.n) {
			rc = -1;
			errmsg = "busy";
		}
	}
	if (rc < 0)
		rc = bin_reply(c, id, PC_BIN_F_ERR, errmsg, strlen(errmsg));
	else if (tw.over)
		rc = bin_reply(c, id, PC_BIN_F_ERR, "reply too large", 15);
	else if (!tw.n || pc_tw_done(&tw) != 0)
		rc = bin_reply(c, id, PC_BIN_F_ERR, "malformed reply", 15);
	else
		rc = bin_reply(c, id, 0, (const char *)tw.b, tw.n);
	/* a method whose reply a request does not bound (dump)
	 * switched the writer to a heap buffer; out_append copied it */
	pc_tw_free(&tw);
	return rc < 0 ? -1 : (int)(PC_BIN_HDR + plen);
}

/* one frame at in+off: bytes consumed, 0 = need more, -1 fatal; the caller compacts once
 * per drain.  A PC_VERB_CMD frame goes to cmd_frame. */
static int bin_dispatch(struct pc_conn *c, char *base, uint64_t id, char *pl,
		uint32_t plen);

static int bin_frame(struct pc_conn *c, size_t off)
{
	char *base = c->in + off;
	size_t avail = c->in_len - off;
	uint32_t plen;
	uint64_t id;
	char *pl;

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
	/* a method by name: counted per method there, never on a catch-all row here */
	if (plen && (unsigned char)pl[0] == PC_VERB_CMD)
		return cmd_frame(c, base, id, pl, plen);
	return bin_dispatch(c, base, id, pl, plen);
}

/* a data verb's frame at @base, from the input buffer or a parked copy dispatched again:
 * bytes consumed, -1 fatal */
static int bin_dispatch(struct pc_conn *c, char *base, uint64_t id, char *pl,
		uint32_t plen)
{
	char *resbuf;
	struct pc_jw rw;
	const char *errmsg = "error", *col = NULL, *key = NULL;
	size_t collen = 0, klen = 0;
	unsigned int park_req = 0;
	int flags = 0, rc;

	resbuf = scratch_buf(0);
	if (!resbuf)
		return -1;
	pc_jw_init(&rw, resbuf, JW_REPLY_CAP);
	{
		/* timed and counted like a RESP command, as bin:<verb>; 10-14
		 * are unassigned (pub/sub runs as methods): the catch-all row */
		unsigned long long qt0 = obs_carry ? obs_carry : pc_obs_usec_now(), qdt;
		static const char *const names[] = { "?", "ping", "get",
			"set", "del", "exists", "ttl", "expire", "add", "sub",
			"?", "?", "?", "?", "?", "setnx", "setxx", "rlhit", "hcmd",
			"cmd" };
		/* a colon, as for cmd:, so both doors name their rows one way */
		static const char *const bnames[] = { "bin:?", "bin:ping",
			"bin:get", "bin:set", "bin:del", "bin:exists", "bin:ttl",
			"bin:expire", "bin:add", "bin:sub", "bin:?", "bin:?",
			"bin:?", "bin:?", "bin:?", "bin:setnx", "bin:setxx",
			"bin:rlhit", "bin:hcmd", "cmd:?" };
		unsigned int vb = plen ? (unsigned char)pl[0] : 0;
		const char *vn = vb < sizeof names / sizeof names[0]
			? names[vb] : "?";
		const char *bn = vb < sizeof bnames / sizeof bnames[0]
			? bnames[vb] : "bin:?";

		pc_obs_conn_touch(c->obs, vn, strlen(vn), get_ticks());
	ev_origin(c, "binary");
	rc = pc_verb_bin(pl, plen, &rw, &flags, &park_req,
		&col, &collen, &key, &klen, &errmsg, c);
		if (rc < 0 && errmsg == pc_bin_unknown_verb) {
			char vnum[8];
			int vl = snprintf(vnum, sizeof vnum, "%u", vb);

			pc_obs_unknown(CLUNK_BIN, "verb", 4, vnum, (size_t)vl,
				c->obs);
		}
		obs_carry = pc_obs_usec_now();
		qdt = obs_carry - qt0;
		if (!up_frame_pending) {           /* parked: counted when it runs again */
			/* a row per named verb plus the catch-all, resolved once per worker; sized
			 * from the table so a new verb cannot share the catch-all's row */
			static __thread void *slot[sizeof bnames / sizeof bnames[0] + 1];
			unsigned int nb = sizeof bnames / sizeof bnames[0];
			unsigned int si = vb < nb ? vb : nb;

			if (!slot[si])
				slot[si] = pc_obs_cmd_slot(bn, strlen(bn));
			pc_obs_cmd_at(slot[si], qdt);
		}
		if (!up_frame_pending && pc_obs_slow_would(qdt)) {
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
		if (!up_frame_pending && pc_obs_qlog_on()) {
			pc_obs_qlog("bin", c->obs, vn, strlen(vn), col, collen, key,
				klen, rc < 0 || (flags & PC_BIN_F_ERR) ? "err"
				: strcmp(vn, "get") == 0 ? (rw.len ? "hit" : "miss")
				: "ok", qdt);
		}
	}
	if (rc == 0 && !park_req && (base[3] & PC_BIN_F_HINT) && col) {
		/* answered here, where the client's hint sent it */
		pcache_htable_t *hht = pc_store_find(col, collen);

		if (hht && pc_store_proxy_enabled(hht))
			__atomic_fetch_add(&hint_local_n, 1, __ATOMIC_RELAXED);
	}
	if (up_frame_pending) {
		/* waiting on a read-through fetch - the frame parks */
		if (rc == 0 && up_frame_park(c, base, plen, 0) == 0)
			goto consume;
		up_frame_pending = 0;
		rc = -1;
		errmsg = "busy";
	}
	if (rc == 0 && park_req) {
		/* deferred to a cluster pull/forward: the 8 id bytes park as
		 * they sit on the wire (LE) */
		if (col && key && park_add(c, park_req, base + 8,
		        col, collen, key, klen, PARK_BIN,
		        (base[3] & PC_BIN_F_HINT) != 0) == 0)
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

/* ---- a connection's buffered requests, each on its first byte ---------- */

/* each message self-describes on its first byte (0x9E binary, '*' or a letter RESP);
 * c->dialect keeps the first one's, which picks push framing (RESP clients get none) */
/* pipelined RESP lookahead: resp_peek parses the frames already in c->in (key only, never
 * run; 0 = not all here, -1 = stop) to prefetch bucket and record PF_D2 / PF_D frames ahead */
#define PF_D   4
#define PF_D2  (2 * PF_D)

static int resp_peek(const char *p, size_t avail, unsigned int *hash,
		int *has_key, const char **kp, int *kl)
{
	size_t pos;
	long long n, bl;
	const char *nl;
	int i;

	*has_key = 0;
	if (!avail || p[0] != '*')
		return -1;
	nl = memchr(p, '\n', avail < 16 ? avail : 16);
	if (!nl)
		return 0;
	if (resp_ll_span(p + 1, (size_t)(nl - p) - (nl[-1] == '\r' ? 2 : 1),
	        &n) < 0 || n < 0 || n > PC_RESP_MAXARGS)
		return -1;
	pos = (size_t)(nl - p) + 1;
	for (i = 0; i < (int)n; i++) {
		if (pos >= avail)
			return 0;
		if (p[pos] != '$')
			return -1;
		nl = memchr(p + pos, '\n', (avail - pos) < 16 ? (avail - pos) : 16);
		if (!nl)
			return 0;
		if (resp_ll_span(p + pos + 1, (size_t)(nl - (p + pos)) -
		        (nl[-1] == '\r' ? 2 : 1), &bl) < 0 || bl < 0 ||
		        bl > (long long)PC_MAX_REQ)
			return -1;
		pos = (size_t)(nl - p) + 1;
		if (avail - pos < (size_t)bl + 2)
			return 0;
		if (i == 1) {
			str k;

			k.s = (char *)p + pos;
			k.len = (int)bl;
			*hash = pcache_key_hash(&k);
			*has_key = 1;
			*kp = k.s;
			*kl = k.len;
		}
		pos += (size_t)bl + 2;
	}
	return pos > (size_t)0x7fffffff ? -1 : (int)pos;
}

/* below [daemon] prefetch_min_keys the table is cache-resident and the lookahead pure cost;
 * the entry count reads every worker's stat line, so a worker keeps its answer for a second */
int pc_prefetch_min_keys = 65536;
static __thread pcache_htable_t *pf_gate_ht;
static __thread unsigned int pf_gate_tick;
static __thread int pf_gate_on;

static int pf_gate(pcache_htable_t *ht)
{
	unsigned int now = get_ticks();

	if (!pc_prefetch_min_keys)
		return 1;
	if (ht != pf_gate_ht || now != pf_gate_tick) {
		pf_gate_ht = ht;
		pf_gate_tick = now;
		pf_gate_on = pcache_ht_entries(ht) >=
			(unsigned long)pc_prefetch_min_keys;
	}
	return pf_gate_on;
}

static void pause_set_events(struct pc_conn *c);

static int plaintext_drain(struct pc_conn *c);

/* the drain, then input on or off as a hold began or ended in it; records io_uring held
 * through the hold (ur_hold) are served once it is over */
int process_plaintext(struct pc_conn *c)
{
	int rc = plaintext_drain(c), h;

	if (rc < 0 || c->dead)
		return rc;
	h = c->enum_active || c->up_hold || c->park_hold;
	if (h == c->in_held)
		return rc;
	c->in_held = h;
	pause_set_events(c);
	if (!h && c->ep < 0 && c->raw_len && (c->encrypted || c->tls))
		return conn_take(c, NULL, 0);
	return rc;
}

static int plaintext_drain(struct pc_conn *c)
{
	size_t off = 0;
	int rc = 0;
	/* the lookahead: hashes (and key spans) of the frames parsed ahead of `off`,
	 * oldest (the next to run) at pf_head */
	pcache_htable_t *pf_ht = NULL;
	unsigned int pf_hash[PF_D2];
	unsigned char pf_has[PF_D2];
	const char *pf_kp[PF_D2];
	int pf_kl[PF_D2];
	size_t pf_off = 0;
	int pf_head = 0, pf_n = 0, pf_on = 0;

	if (c->resp_only || c->dialect == PC_D_RESP) {
		pf_ht = pc_store_find(c->resp_col, c->resp_collen);
		pf_on = pf_ht != NULL && pf_gate(pf_ht);
	}

	obs_carry = 0;                     /* a fresh clock per drain */
	if (pc_ps_pressed)
		pc_pubsub_pressure_take(NULL); /* not ours: nothing waits on it */

	pc_wal_wrote = &c->wal_writer;     /* cleared on every way out */
	pc_wal_conn_seq = pc_wal_sync_mode() ? &c->sync_want : NULL;
	while (off < c->in_len) {
		/* a cooperative walk's reply must precede any later request's (RESP replies are
		 * ordered): the rest stays buffered until the walk completes */
		if (c->enum_active)
			break;
		/* a command waits on a read-through fetch: it and all
		 * that follows stay buffered until the completion */
		if (c->up_hold)
			break;
		/* a RESP command waits on the cluster - what
		 * follows waits for its reply */
		if (c->park_hold)
			break;
		/* a refusal has been answered: nothing more is served, or a
		 * second event would answer the same bytes a second time */
		if (c->close_after)
			break;
		/* its replies outrun its socket - the rest of its requests
		 * wait, unread, until they drain (pc_conn_resume_out) */
		if (c->out_paused)
			break;
		if (out_pending(c) >= PC_OUT_PAUSE_AT) {
			out_pause(c);
			break;
		}
		/* a writer and this worker's WAL ring in its last quarter: the rest waits unread
		 * (pc_conn_resume_wal), so its other connections are not held in the ring wait */
		if (c->wal_writer && pc_wal_tight(0)) {
			wal_pause(c);
			break;
		}
		unsigned char f = (unsigned char)c->in[off];

		if (c->refused && !c->http_only && !c->resp_only) {
			/* the refusal as an error frame, with the request's id when a whole
			 * header is here (0 otherwise: the close that follows says the rest) */
			uint64_t id = c->in_len - off >= PC_BIN_HDR
				? get64((const unsigned char *)c->in + off + 8) : 0;

			bin_reply(c, id, PC_BIN_F_ERR,
				"max number of clients reached", 29);
			c->close_after = 1;
			c->why = c->refused == 2 ? "memory floor" : "max_clients";
			break;
		}
		if (pf_on && f == '*') {
			/* keep PF_D2 frames parsed ahead; stage 2 for the
			 * one PF_D ahead, whose bucket stage 1 fetched earlier */
			if (pf_off < off || !pf_n) {
				pf_off = off;
				pf_n = 0;
				pf_head = 0;
			}
			while (pf_n < PF_D2 && pf_off < c->in_len) {
				unsigned int h = 0;
				const char *kp = NULL;
				int kl = 0, hk, fl = resp_peek(c->in + pf_off,
					c->in_len - pf_off, &h, &hk, &kp, &kl);

				if (fl <= 0)
					break;
				pf_hash[(pf_head + pf_n) % PF_D2] = h;
				pf_has[(pf_head + pf_n) % PF_D2] = (unsigned char)hk;
				pf_kp[(pf_head + pf_n) % PF_D2] = kp;
				pf_kl[(pf_head + pf_n) % PF_D2] = kl;
				if (hk)
					pcache_ht_prefetch_hashed(pf_ht, h, 1);
				pf_off += (size_t)fl;
				pf_n++;
			}
			if (pf_n > PF_D && pf_has[(pf_head + PF_D) % PF_D2])
				pcache_ht_prefetch_hashed(pf_ht,
					pf_hash[(pf_head + PF_D) % PF_D2], 2);
			/* hash once: hand the table the hash taken when this frame
			 * was parsed ahead, if the ring's head IS this frame */
			if (pf_n && pf_off > off && pf_has[pf_head] &&
			    pf_kp[pf_head] > c->in + off &&
			    pf_kp[pf_head] < c->in + pf_off) {
				pcache_hh_s = pf_kp[pf_head];
				pcache_hh_len = pf_kl[pf_head];
				pcache_hh_h = pf_hash[pf_head];
			}
		}
		if (c->http_only) {
			rc = http_one(c, off);
		} else if (c->resp_only) {
			/* pinned at accept: RESP and NOTHING else, so the native
			 * protocol and its admin verbs are unreachable here */
			if (f != '*' && !(f >= 'A' && f <= 'Z') &&
			        !(f >= 'a' && f <= 'z') &&
			        f != '\r' && f != '\n') {
				pc_wal_wrote = NULL;
				pc_wal_conn_seq = NULL;
				return -1;
			}
			rc = resp_one(c, off);
			if (rc > 0)
				PC_RESP_BUMP(pc_resp_reqs);
		} else if (f == PC_BIN_MAGIC) {
			if (c->dialect == PC_D_SNIFF) {
				c->dialect = PC_D_BIN;
				PC_RESP_BUMP(pc_nat_bin_conns);
				PC_RESP_BUMP(pc_nat_bin_open);
				c->open_counted = 1;
				conn_log_open(c, "binary");
			}
			rc = bin_frame(c, off);
			if (rc > 0)
				PC_RESP_BUMP(pc_nat_bin_reqs);
		} else if (f == '*' || (f >= 'A' && f <= 'Z') ||
		        (f >= 'a' && f <= 'z')) {
			if (c->dialect == PC_D_SNIFF) {
				c->dialect = PC_D_RESP;
				PC_RESP_BUMP(pc_nat_resp_conns);
				PC_RESP_BUMP(pc_nat_resp_open);
				c->open_counted = 1;
				conn_log_open(c, "resp");
			}
			rc = resp_one(c, off);
			if (rc > 0)
				PC_RESP_BUMP(pc_nat_resp_reqs);
		} else {
			/* neither a frame nor RESP: one plain line naming the door,
			 * then the close - a silent drop reads as a dead daemon */
			static const char gone[] =
				"perfcached: this door speaks binary frames or RESP "
				"- the JSON-RPC dialect was removed in 0.4.5\n";

			out_append(c, gone, sizeof gone - 1);
			c->close_after = 1;
			c->why = "not a frame or RESP (JSON-RPC was removed in 0.4.5)";
			break;
		}
		pcache_hh_s = NULL;            /* the hint ends with its frame */
		if (rc <= 0)
			break;
		off += (size_t)rc;
		if (pf_n) {                    /* that frame has run */
			pf_head = (pf_head + 1) % PF_D2;
			pf_n--;
		}
		if (c->close_after)
			break;                 /* QUIT answered: drop the rest */
		if (pc_ps_pressed && conn_pause(c))
			break;                 /* a queue it feeds is full: the rest waits */
	}
	pc_wal_wrote = NULL;
	pc_wal_conn_seq = NULL;
	if (rc < 0)
		return -1;
	if (off) {
		c->in_len -= off;
		memmove(c->in, c->in + off, c->in_len);
	}
	return 0;
}

/* ---- the encrypted input (the Noise channel itself: noisechan.c) --------- */

static int raw_append(struct pc_conn *c, const char *p, size_t n)
{
	if (buf_reserve(&c->raw, &c->raw_cap, c->raw_len, n,
	        PC_MAX_REQ + 4 * PC_NOISE_MAXMSG) < 0)
		return -1;
	bufs_mark(c);
	memcpy(c->raw + c->raw_len, p, n);
	c->raw_len += n;
	return 0;
}

/* ---- pull completion (runs on the owning worker) ------------------------ */

/* the one-key result maps a parked CMD method answers with */
static void tw_flag(struct pc_tw *w, const char *key, int v)
{
	pc_tw_map(w);
	pc_tw_key(w, key);
	pc_tw_bool(w, v);
	pc_tw_end(w);
}

static void tw_int(struct pc_tw *w, const char *key, long long v)
{
	pc_tw_map(w);
	pc_tw_key(w, key);
	pc_tw_i64(w, v);
	pc_tw_end(w);
}

static void tw_error(struct pc_tw *w, const char *msg)
{
	pc_tw_map(w);
	pc_tw_key(w, "error");
	pc_tw_str(w, msg);
	pc_tw_end(w);
}

/* under shard and spread a parked reply means the client asked the wrong node: say so with
 * PC_BIN_F_MOVED and this node's map stamp after the payload; trailer length, 0 if none */
static size_t park_moved(const struct parked *pk, int *flags, char *trail)
{
	pcache_htable_t *mht = pc_store_find(pk->col, pk->collen);
	unsigned int mt = 0, ms = 0;

	if (!mht || !(pc_store_shard_enabled(mht) || pc_cluster_replicas()) ||
	        !pc_cluster_map_stamp(&mt, &ms))
		return 0;
	put32(trail, mt);
	put32(trail + 4, ms);
	*flags |= PC_BIN_F_MOVED;
	return 8;
}

void pc_proto_hint_stats(unsigned long long *sent, unsigned long long *local)
{
	*sent = __atomic_load_n(&hint_sent_n, __ATOMIC_RELAXED);
	*local = __atomic_load_n(&hint_local_n, __ATOMIC_RELAXED);
}

/* the holder hint after the payload (and any moved stamp) for a proxy collection that
 * another node answered: the node that had the record, or the node a forward went to */
static size_t park_hint(const struct parked *pk, const struct pc_pull_done *d,
		int *flags, char *trail)
{
	pcache_htable_t *ht;
	unsigned int ttl;
	int node;

	if (!pk->hint)
		return 0;
	ht = pc_store_find(pk->col, pk->collen);
	if (!ht || !pc_store_proxy_enabled(ht))
		return 0;
	if (d->kind == PC_DONE_PULL && d->found && d->from_node > 0) {
		node = d->from_node;
		ttl = d->ttl_left;              /* 0: the record has none */
	} else if ((d->kind == PC_DONE_FWD_SET || d->kind == PC_DONE_FWD_TOUCH ||
	            d->kind == PC_DONE_FWD_ADD) && d->ok && pk->target > 0) {
		node = pk->target;
		ttl = PC_HINT_TTL_UNKNOWN;      /* the client knows what it set */
	} else {
		return 0;
	}
	put32(trail, (uint32_t)node);
	put32(trail + 4, ttl);
	*flags |= PC_BIN_F_HINT;
	__atomic_fetch_add(&hint_sent_n, 1, __ATOMIC_RELAXED);
	return 8;
}

static struct parked *park_find(unsigned int req)
{
	int i;

	if (!parks)
		return NULL;
	for (i = 0; i < parks_cap; i++)
		if (parks[i].conn && parks[i].req == req)
			return &parks[i];
	return NULL;
}

static void pull_complete_body(const struct pc_pull_done *d, int rd);
/* a read-through fetch this worker's connection waited on is done: release the hold and
 * run the parked command, then what the client pipelined behind it, in order */
void pc_proto_upstream_complete(unsigned int req, int status)
{
	struct pc_conn *c = NULL;
	int i;

	for (i = 0; i < up_held_n; i++)
		if (up_held[i]->up_hold == req) {
			c = up_held[i];
			break;
		}
	if (!c) {
		/* the native doors: a parked FRAME, dispatched again */
		for (i = 0; i < up_frames_n; i++)
			if (up_frames[i].req == req)
				break;
		if (i == up_frames_n)
			return;                /* it closed meanwhile */
		{
			struct up_frame f = up_frames[i];
			char *b = f.frame;
			uint32_t plen = get32(b + 4);
			uint64_t id = get64(b + 8);
			int rc;

			up_frames[i] = up_frames[--up_frames_n];
			c = f.c;
			up_frame_verdict = status;
			up_frame_mark = f.mark;
			rc = f.cmd ? cmd_frame(c, b, id, b + PC_BIN_HDR, plen)
				: bin_dispatch(c, b, id, b + PC_BIN_HDR, plen);
			up_frame_verdict = 0;
			up_frame_mark = 0;
			up_frame_pending = 0;
			free(b);
			if (rc < 0) {
				c->why = "protocol error";
				conn_destroy(c);
				return;
			}
			if (conn_flush(c) < 0) {
				c->why = "write error";
				conn_destroy(c);
			}
		}
		return;
	}
	up_unhold(c);
	c->up_verdict = status;
	if (process_plaintext(c) < 0) {
		c->why = "protocol error";
		conn_destroy(c);
		return;
	}
	if (conn_flush(c) < 0) {
		c->why = "write error";
		conn_destroy(c);
		return;
	}
	if (c->close_after && !c->out_len && !c->wire_len)
		conn_destroy(c);
}


/* a RESP command that waited on the cluster has its reply in
 * place: the hold goes, and what the client pipelined behind it runs */
static void resp_park_done(struct pc_conn *c)
{
	c->park_hold = 0;
	if (c->out_failed) {               /* its reply was lost: run nothing behind it */
		conn_destroy(c);
		return;
	}
	if (process_plaintext(c) < 0) {
		c->why = "protocol error";
		conn_destroy(c);
		return;
	}
	if (conn_flush(c) < 0) {
		c->why = "write error";
		conn_destroy(c);
		return;
	}
	if (c->close_after && !c->out_len && !c->wire_len)
		conn_destroy(c);
}

/* an mget's last lookup answered: its reply in its door's shape; a held RESP connection
 * then runs what it pipelined after */
static void mget_answer(struct pc_mget *m)
{
	struct pc_conn *c = pc_mget_conn(m);
	char *buf = scratch_buf(0);

	if (pc_mget_door(m) == PC_MGET_DOOR_CMD) {
		uint64_t id = get64(pc_mget_id(m));
		struct pc_tw w;

		if (!buf) {
			pc_mget_free(m);
			bin_reply(c, id, PC_BIN_F_ERR, "out of memory", 13);
			if (conn_flush(c) < 0)
				conn_destroy(c);
			return;
		}
		pc_tw_init(&w, (unsigned char *)buf, JW_REPLY_CAP);
		pc_mget_tree(m, &w);
		pc_mget_free(m);
		if (w.over)
			bin_reply(c, id, PC_BIN_F_ERR, "reply too large", 15);
		else if (pc_tw_done(&w) != 0)
			bin_reply(c, id, PC_BIN_F_ERR, "malformed reply", 15);
		else
			bin_reply(c, id, 0, (const char *)w.b, w.n);
		pc_tw_free(&w);
		if (conn_flush(c) < 0)
			conn_destroy(c);
		return;
	}
	{
		static const char big[] = "-ERR reply too large\r\n";
		struct pc_jw w;

		if (!buf) {
			pc_mget_free(m);
			out_append(c, "-ERR out of memory\r\n", 20);
		} else {
			pc_jw_init(&w, buf, JW_REPLY_CAP);
			pc_mget_resp(m, &w);
			pc_mget_free(m);
			if (w.overflow)
				out_append(c, big, sizeof big - 1);
			else
				out_append(c, w.buf, w.len);
			pc_jw_free(&w);
		}
	}
	resp_park_done(c);
}

/* a forwarded hash read returns the owner's record: the client's reply is rendered here
 * over the park's arguments and handed on as a PC_JOP_HCMD reply tree would be */
void pc_proto_pull_complete(const struct pc_pull_done *d)
{
	struct pc_pull_done dd;
	struct pc_tw rt;
	struct pc_mget *mg;
	int rendered = 0;

	if (pc_mget_pull_done(d, &mg)) {
		if (mg)
			mget_answer(mg);
		return;
	}
	if ((d->kind == PC_DONE_JSON && d->jop == PC_JOP_HCMD_READ) ||
	    d->kind == PC_DONE_PULL) {
		struct parked *pk = park_find(d->req);
		int st = d->ok;

		if (!pk)
			return;                /* conn died while pulling */
		if (d->kind == PC_DONE_PULL) {
			char *frag = NULL;
			int fl = 0;

			if (!pk->hargs || pk->pres)
				goto plain;
			/* a jget pulled the document - its fragment, as the
			 * owner's forwarded jget answers */
			st = pc_jget_render_pull(pk->hargs, pk->hargl, pk->col,
				pk->collen, d->val, (size_t)d->vlen, d->found,
				d->newval, &frag, &fl);
			if (st >= 0) {
				if (d->timedout)
					st = 2;
				dd = *d;
				dd.kind = PC_DONE_JSON;
				dd.jop = PC_JOP_GET;
				dd.ok = st;
				dd.val = st == 0 ? frag : NULL;
				dd.vlen = st == 0 ? fl : 0;
				pull_complete_body(&dd, 1);
				return;
			}
			/* a hash read pulled the record: 0 a hash, 1 absent, 3 wrong type */
			st = !d->found ? 1 : (d->newval & PCACHE_F_HASH) ? 0 : 3;
		}
		dd = *d;
		dd.kind = PC_DONE_JSON;
		dd.jop = PC_JOP_HCMD;
		dd.val = NULL;
		dd.vlen = 0;
		pc_tw_init(&rt, NULL, 0);
		if (!d->timedout && (st == 0 || st == 1 || st == 3) &&
		    pc_hcmd_render_read(pk->hargs, pk->hargl,
		        (const unsigned char *)d->val, (size_t)d->vlen, st,
		        &rt) == 0 && pc_tw_done(&rt) == 0) {
			rendered = 1;
			dd.ok = 0;
			dd.val = (char *)rt.b;
			dd.vlen = (int)rt.n;
		} else {
			pc_tw_free(&rt);
			dd.ok = 2;             /* holder rejected / timed out */
		}
		d = &dd;
		pull_complete_body(d, 1);
		if (rendered)
			pc_tw_free(&rt);
		return;
	}
plain:
	pull_complete_body(d, d->kind == PC_DONE_JSON && d->jop == PC_JOP_GET);
	if (rendered)
		pc_tw_free(&rt);
}

/* a pull found the key: this node keeps nothing (shard; spread on a non-holder), a passive
 * copy (store) or the holder's name (proxy) - for a get's park and an mget's fan-out alike */
void pc_pull_found_keep(const char *col, size_t collen, const char *key,
		size_t klen, const struct pc_pull_done *d)
{
	pcache_htable_t *ht = pc_store_find(col, collen);

	if (ht && (pc_store_shard_enabled(ht) ||
	        (pc_cluster_replicas() &&
	         !pc_spread_in_set(pc_key_slot(key, klen), 0,
	                pc_cluster_replicas())))) {
		/* serve only: shard keeps the one copy at the owner, and a spread non-holder would
		 * bring back the orphans the write path avoids (a holder that pulled keeps it) */
	} else if (ht && !pc_store_proxy_enabled(ht)) {
		/* store mode: keep the pulled value PASSIVE, TTL
		 * re-anchored - the replicate-on-demand behavior */
		str k, v;
		/* a copy of an upstream-bound collection's record
		 * never outlives its key */
		unsigned int exp = pc_upstream_copy_exp(col, collen, d->ttl_left);

		k.s = (char *)key;
		k.len = (int)klen;
		v.s = d->val;
		v.len = d->vlen;
		/* the HOLDER's version, not a local tick: clocks converge to the fleet maximum, so
		 * a local one could outrank the holder's next genuine update */
		/* a through collection's pulled record is Redis's, as a pushed one: marked (T594) */
		if (pcache_ht_store_ver(ht, &k, &v, exp,
			(unsigned char)(PCACHE_F_PASSIVE |
			(d->newval & PCACHE_F_CARRY)), d->ver) == 0)
			pc_upstream_note_copy(col, collen, key, klen, exp, d->ver);
	} else if (ht) {
		/* proxy mode: serve WITHOUT storing, remember the holder */
		pc_loc_set(col, collen, key, klen, d->from_node);
	}
}

/* @rd: the request was a read (a pull, or a forwarded document or hash read) */
static void pull_complete_body(const struct pc_pull_done *d, int rd)
{
	struct parked *pk = NULL;
	struct pc_conn *c;
	long long resume_nv;
	int resume_rc, pull_wrongtype = 0, busy;
	const char *pull_uperr = NULL;

	pk = park_find(d->req);
	if (!pk)
		return;                        /* conn died while pulling */
	c = pk->conn;
	/* a pulled document answers a string GET with WRONGTYPE under
	 * this node's redis_types = strict (the copy is still kept, typed) */
	if (d->kind == PC_DONE_PULL && d->found && !pk->pres &&
	    (d->newval & PCACHE_F_TYPES)) {
		pcache_htable_t *tht = pc_store_find(pk->col, pk->collen);

		pull_wrongtype = tht && pc_store_types_strict(tht);
	}

	if (d->kind == PC_DONE_PULL && !d->found && d->timedout && !pk->pres)
		pull_uperr = pc_upstream_pull_lost(pk->col, pk->collen, pk->key,
			pk->klen);
	/* a read nobody answered in time is no miss: refused as retryable, nothing cached */
	busy = d->timedout && !pull_uperr &&
		(d->kind == PC_DONE_PULL ? !d->found : rd);
	if (d->kind == PC_DONE_PULL && d->found) {
		pc_pull_found_keep(pk->col, pk->collen, pk->key, pk->klen, d);
	} else if (pull_uperr) {
		/* the owner's upstream fetch failed: neither a miss nor a marker */
	} else if (busy) {
		/* answered below */
	} else if (d->kind == PC_DONE_PULL) {
		pcache_htable_t *ht = pc_store_find(pk->col, pk->collen);

		if (ht && pc_store_shard_enabled(ht)) {
			/* an owner miss is final, except in the reshard grace: the data may
			 * still be on the old owner, so retry ONCE as a broadcast */
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
			/* the locator's node missed, but it can be stale (rewritten
			 * elsewhere): forget it and ask the fleet once before answering */
			if (ht && pc_store_proxy_enabled(ht) && pk->target &&
			        !pk->retried) {
				unsigned int req2;

				pc_loc_clear(pk->col, pk->collen, pk->key,
					pk->klen);
				req2 = pc_pull_begin(pk->col, pk->collen, pk->key,
					pk->klen);
				if (req2) {
					pk->retried = 1;
					pk->target = 0;
					pk->req = req2;
					return;
				}
			}
			pc_neg_set(pk->col, pk->collen, pk->key, pk->klen,
				pc_cluster_neg_ms());
			pc_loc_clear(pk->col, pk->collen, pk->key, pk->klen);
		}
	} else if ((d->kind == PC_DONE_FWD_DEL || d->kind == PC_DONE_FWD_TOUCH) &&
	        !d->ok && !d->timedout && pk->target && !pk->retried &&
	        pc_store_find(pk->col, pk->collen) &&
	        pc_store_proxy_enabled(pc_store_find(pk->col, pk->collen))) {
		/* a DEL or EXPIRE the locator sent to a node that no longer holds the key: ask the
		 * fleet - the probe forwards to the holder or answers absent (SET_RESUME) */
		unsigned int req2;

		pc_loc_clear(pk->col, pk->collen, pk->key, pk->klen);
		req2 = pc_probe_fwd_begin(d->kind == PC_DONE_FWD_DEL ? 1 : 3,
			pk->col, pk->collen, pk->key, pk->klen, NULL, 0,
			d->kind == PC_DONE_FWD_TOUCH ? pk->target_ttl : 0, 0);
		if (req2) {
			pk->retried = 1;
			pk->target = 0;
			pk->req = req2;
			return;
		}
	} else if ((d->kind == PC_DONE_FWD_SET || d->kind == PC_DONE_FWD_TOUCH ||
	        (d->kind == PC_DONE_FWD_SETCOND && d->newval)) && d->ok) {
		/* the write landed at the holder: the key is no longer a known miss */
		pcache_htable_t *ht = pc_store_find(pk->col, pk->collen);

		(void)ht;
		pc_neg_clear(pk->col, pk->collen, pk->key, pk->klen);
	}

	/* probe-before-place found a fleet-wide miss: replay the write through the normal core;
	 * a forward re-parks THIS entry and its ack completes it later */
	resume_rc = -1000;
	resume_nv = 0;
	if (d->kind == PC_DONE_SET_RESUME && d->ok) {
		unsigned int req2 = 0;

		if (d->jop == 3 || d->jop == 4) {
			/* a re-arm or a delete never places; remember the fleet's
			 * no, so the next miss of this key costs nothing */
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

	if (pk->bin == PARK_RESP) {
		/* the request came as a RESP command: complete in kind.
		 * The done kind names the verb, so no extra state parks. */
		char nb[32];
		struct pc_jw w;

		pc_jw_init(&w, nb, sizeof nb);
		if (busy)
			out_append(c, "-TRYAGAIN cluster busy, retry\r\n",
				sizeof "-TRYAGAIN cluster busy, retry\r\n" - 1);
		else switch (d->kind) {
		case PC_DONE_PULL:
			if (pk->pres) {
				pc_pres_resp(&w, pk->pres, d->found,
					(unsigned int)d->newval, d->ttl_left);
				out_append(c, w.buf, w.len);
			} else if (pull_wrongtype || pull_uperr) {
				const char *em = pull_uperr ? pull_uperr
					: pc_wrongtype_msg;

				out_append(c, pull_uperr ? "-ERR " : "-",
					pull_uperr ? 5 : 1);
				out_append(c, em, strlen(em));
				out_append(c, "\r\n", 2);
			} else if (d->found) {
				size_t at = c->out_len;

				pc_jw_lit(&w, "$");
				pc_jw_i64(&w, d->vlen);
				pc_jw_lit(&w, "\r\n");
				if (out_append(c, w.buf, w.len) < 0 ||
				        out_append(c, d->val, (size_t)d->vlen) < 0 ||
				        out_append(c, "\r\n", 2) < 0)
					c->out_len = at;   /* resp_park_done closes it */
			} else {
				out_append(c, "$-1\r\n", 5);
			}
			break;
		case PC_DONE_FWD_SET:
			/* deliberately NOT retryable: the holder may have stored it, so a retry
			 * is safe for SET and not for INCR */
			if (d->ok)
				out_append(c, "+OK\r\n", 5);
			else if (d->timedout)
				out_append(c, "-ERR holder timed out\r\n", 23);
			else
				out_append(c, "-ERR holder rejected\r\n", 22);
			break;
		case PC_DONE_FWD_DEL:
		case PC_DONE_FWD_TOUCH:
			out_append(c, d->ok ? ":1\r\n" : ":0\r\n", 4);
			break;
		case PC_DONE_FWD_SETCOND:      /* OK, nil, or refused */
			if (d->ok)
				out_append(c, d->newval ? "+OK\r\n" : "$-1\r\n",
					5);
			else if (d->newval == PC_FWD_E_TYPE) {
				out_append(c, "-", 1);
				out_append(c, pc_wrongtype_msg,
					strlen(pc_wrongtype_msg));
				out_append(c, "\r\n", 2);
			} else if (d->timedout)
				out_append(c, "-ERR holder timed out\r\n", 23);
			else
				out_append(c, "-ERR holder rejected\r\n", 22);
			break;
		case PC_DONE_FWD_DELCOND:      /* DELEX: 1, 0, or refused */
			if (d->ok)
				out_append(c, d->newval ? ":1\r\n" : ":0\r\n", 4);
			else if (d->newval == PC_FWD_E_TYPE)
				out_append(c, "-ERR Key should be of string type if "
					"conditions are specified\r\n", 63);
			else if (d->timedout)
				out_append(c, "-ERR holder timed out\r\n", 23);
			else
				out_append(c, "-ERR holder rejected\r\n", 22);
			break;
		case PC_DONE_JSON:
			/* RL.HIT's count and allowed as *2 :n :0|1, the hash reads and JSON.GET;
			 * d->ok: 0 ok, 3 wrong type */
			if (d->jop == PC_JOP_GET &&
			    pc_jget_park_shape(pk->hargs, pk->hargl) >= 0) {
				/* JSON.GET over a pulled document */
				int sh = pc_jget_park_shape(pk->hargs, pk->hargl);
				size_t cap = (size_t)d->vlen + 128;
				char *rb = malloc(cap);
				struct pc_jw rw2;

				if (rb) {
					pc_jw_init(&rw2, rb, cap);
					pc_resp_jget_reply(&rw2, d->ok, d->val, d->vlen,
						sh & 1, (sh & 2) != 0);
					if (!rw2.overflow)
						out_append(c, rw2.buf, rw2.len);
					else
						out_append(c, "-ERR bad reply\r\n", 16);
					free(rb);
				} else {
					out_append(c, "-ERR out of memory\r\n", 20);
				}
			} else if (d->ok == 0 && d->jop == PC_JOP_HCMD && d->val) {
				/* sized by the WORST item: a nil is one tree byte
				 * and five of RESP ($-1\r\n) */
				size_t cap = (size_t)d->vlen * 6 + 1024;
				char *rb = malloc(cap);
				struct pc_jw rw2;

				if (rb) {
					pc_jw_init(&rw2, rb, cap);
					if (pc_tree_resp(&rw2, (const unsigned char *)d->val,
					        (size_t)d->vlen) == (size_t)d->vlen &&
					    !rw2.overflow)
						out_append(c, rw2.buf, rw2.len);
					else
						out_append(c, "-ERR bad reply\r\n", 16);
					free(rb);
				} else {
					out_append(c, "-ERR out of memory\r\n", 20);
				}
			} else if (d->ok == 0 && d->jop == PC_JOP_SLOCK) {
				/* the script's answer: Lua true / false */
				if (d->newval)
					out_append(c, ":1\r\n", 4);
				else
					out_append(c, "$-1\r\n", 5);
			} else if (d->ok == 0 && d->jop == PC_JOP_RLHIT) {
				pc_jw_lit(&w, "*2\r\n:");
				pc_jw_i64(&w, d->newval);
				pc_jw_lit(&w, d->count ? "\r\n:1\r\n" : "\r\n:0\r\n");
				out_append(c, w.buf, w.len);
			} else if (d->ok == 3) {
				out_append(c, "-", 1);
				out_append(c, pc_wrongtype_msg,
					strlen(pc_wrongtype_msg));
				out_append(c, "\r\n", 2);
			} else if (d->timedout) {
				out_append(c, "-ERR holder timed out\r\n", 23);
			} else {
				out_append(c, "-ERR holder rejected\r\n", 22);
			}
			break;
		case PC_DONE_FWD_ADD:
			if (d->ok) {
				pc_jw_lit(&w, ":");
				pc_jw_i64(&w, d->newval);
				pc_jw_lit(&w, "\r\n");
				out_append(c, w.buf, w.len);
			} else if (d->newval == PC_FWD_E_TYPE) {
				out_append(c, "-", 1);
				out_append(c, pc_wrongtype_msg,
					strlen(pc_wrongtype_msg));
				out_append(c, "\r\n", 2);
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
		default:                       /* the document ops never park RESP */
			out_append(c, "-ERR protocol\r\n", 15);
		}
		free(pk->key);
		pk->key = NULL;
		free(pk->hargs);
		pk->hargs = NULL;
		pk->conn = NULL;
		resp_park_done(c);
		return;
	}

	if (pk->bin == PARK_BIN) {
		/* the request came as a data verb's frame: its fixed layout */
		uint64_t id = get64(pk->id);
		char pay[16], trail[16];       /* moved stamp + holder hint */
		const char *emsg = NULL;
		size_t pn = 0, tn;
		int tree = 0;                  /* the reply is d->val */
		int flags = 0;

		switch (d->kind) {
		case PC_DONE_FWD_SET:          /* [stored u8] */
		case PC_DONE_FWD_DEL:          /* [deleted u8] */
		case PC_DONE_FWD_TOUCH:        /* [updated u8] */
			pay[0] = d->ok ? 1 : 0;
			pn = 1;
			break;
		case PC_DONE_FWD_SETCOND:      /* [stored u8] */
		case PC_DONE_FWD_DELCOND:      /* [deleted u8]: no verb asks, kept total */
			pay[0] = d->ok && d->newval ? 1 : 0;
			pn = 1;
			break;
		case PC_DONE_JSON:             /* [count i64][allowed u8] */
			/* d->ok is the holder status (0 ok, 3 wrong type) */
			if (d->ok == 0 && d->jop == PC_JOP_HCMD && d->val) {
				tree = 1;
			} else if (d->ok == 0 && d->jop == PC_JOP_RLHIT) {
				put64(pay, (uint64_t)d->newval);
				pay[8] = d->count ? 1 : 0;
				pn = 9;
			} else {
				emsg = d->ok == 3 ? pc_wrongtype_msg :
					d->timedout ? "holder timed out" :
					"holder rejected";
			}
			break;
		case PC_DONE_FWD_ADD:          /* [value i64] */
			if (d->ok) {
				put64(pay, (uint64_t)d->newval);
				pn = 8;
			} else {
				emsg = d->newval == PC_FWD_E_TYPE ?
					pc_wrongtype_msg :
					d->timedout ? "holder timed out"
					: "holder rejected";
			}
			break;
		case PC_DONE_PULL:             /* the get reply shape */
			if (pull_wrongtype)
				emsg = pc_wrongtype_msg;
			else if (pull_uperr)
				emsg = pull_uperr;
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
			emsg = "protocol";     /* document ops park CMD frames only */
		}
		if (busy)
			emsg = "cluster busy";  /* PERFD_E_REFUSED: retry */
		tn = park_moved(pk, &flags, trail);
		if (!emsg)
			tn += park_hint(pk, d, &flags, trail + tn);
		if (emsg) {
			bin_reply(c, id, PC_BIN_F_ERR, emsg, strlen(emsg));
		} else if (tree) {
			bin_reply_ex(c, id, flags, d->val, (size_t)d->vlen,
				trail, tn);
		} else if (d->kind == PC_DONE_PULL && pk->pres) {
			pn = pc_pres_bin(pay, pk->pres, d->found, d->ttl_left);
			bin_reply_ex(c, id, flags, pay, pn, trail, tn);
		} else if (d->kind == PC_DONE_PULL) {
			if (d->found) {
				char *pb = malloc(5 + (size_t)d->vlen);

				if (pb) {
					pb[0] = 1;
					put32(pb + 1, d->ttl_left);
					memcpy(pb + 5, d->val, (size_t)d->vlen);
					bin_reply_ex(c, id, flags, pb,
						5 + (size_t)d->vlen, trail, tn);
					free(pb);
				} else {
					bin_reply(c, id, PC_BIN_F_ERR, "out of memory", 13);
				}
			} else {
				pay[0] = 0;
				bin_reply_ex(c, id, flags, pay, 1, trail, tn);
			}
		} else {
			bin_reply_ex(c, id, flags, pay, pn, trail, tn);
		}
		if (conn_flush(c) < 0)
			conn_destroy(c);           /* drops this park too */
		free(pk->key);
		pk->key = NULL;
		free(pk->hargs);
		pk->hargs = NULL;
		pk->conn = NULL;
		return;
	}

	if (pk->bin == PARK_CMD) {
		/* a CMD frame: the method's reply tree; a refusal the HOLDER made is a result map
		 * {error: ...}, not an error frame (that is a method that could not run at all) */
		uint64_t id = get64(pk->id);
		unsigned char *tb = (unsigned char *)scratch_buf(0);
		struct pc_tw w;
		char trail[8];
		size_t tn = 0;
		int flags = 0;

		if (!tb) {
			/* the frame's id is answered, whatever else is short */
			bin_reply(c, id, PC_BIN_F_ERR, "out of memory", 13);
			if (conn_flush(c) < 0)
				conn_destroy(c);
			goto done;
		}
		if (pull_uperr || busy) {
			/* as the same read's local upstream failure answers; a read
			 * nobody answered in time, as a busy cluster does */
			const char *em = pull_uperr ? pull_uperr : "cluster busy";

			bin_reply(c, id, PC_BIN_F_ERR, em, strlen(em));
			if (conn_flush(c) < 0)
				conn_destroy(c);
			goto done;
		}
		pc_tw_init(&w, tb, JW_REPLY_CAP);
		switch (d->kind) {
		case PC_DONE_FWD_SET:
			tw_flag(&w, "stored", d->ok);
			break;
		case PC_DONE_FWD_DEL:
			tw_flag(&w, "deleted", d->ok);
			break;
		case PC_DONE_FWD_TOUCH:
			tw_flag(&w, "updated", d->ok);
			break;
		case PC_DONE_FWD_SETCOND:
			if (!d->ok && d->newval == PC_FWD_E_TYPE)
				tw_error(&w, pc_wrongtype_msg);
			else
				tw_flag(&w, "stored", d->ok && d->newval);
			break;
		case PC_DONE_FWD_DELCOND:
			if (d->ok)
				tw_flag(&w, "deleted", d->newval != 0);
			else if (d->newval == PC_FWD_E_TYPE)
				tw_error(&w, "Key should be of string type if "
					"conditions are specified");
			else
				tw_error(&w, d->timedout ? "holder timed out"
					: "holder rejected");
			break;
		case PC_DONE_FWD_ADD:
			if (d->ok)
				tw_int(&w, "value", d->newval);
			else if (d->newval == PC_FWD_E_TYPE)
				tw_error(&w, pc_wrongtype_msg);
			else
				tw_error(&w, d->timedout ? "holder timed out"
					: "holder rejected");
			break;
		case PC_DONE_SET_RESUME:
			if (!d->ok) {          /* probe timeout: refuse */
				if (d->jop == 2)
					tw_error(&w, "cluster busy");
				else
					tw_flag(&w, d->jop == 3 ? "updated"
						: d->jop == 4 ? "deleted" : "stored", 0);
			} else if (d->jop == 3) {
				tw_flag(&w, "updated", 0);
			} else if (d->jop == 4) {
				tw_flag(&w, "deleted", 0);
			} else if (d->jop == 2) {
				if (resume_rc == PC_OP_OK)
					tw_int(&w, "value", resume_nv);
				else
					tw_error(&w, "value is not an integer");
			} else if (resume_rc == PC_OP_OK) {
				tw_flag(&w, "stored", 1);
			} else if (resume_rc == PC_OP_ERR_FULL) {
				tw_error(&w, "cache full");
			} else {
				tw_error(&w, "value too large");
			}
			break;
		case PC_DONE_JSON:
			/* d->ok carries the holder status: 0 ok, 1 benign
			 * absent, 2 error/timeout, 3 the key holds a string */
			if (d->ok == 3) {
				tw_error(&w, d->jop == PC_JOP_RLHIT ?
					pc_wrongtype_msg :     /* a string op's */
					pc_json_wrongtype_msg);
				break;
			}
			switch (d->jop) {
			case PC_JOP_SET:
				tw_flag(&w, "set", d->ok == 0);
				break;
			case PC_JOP_DEL:
				tw_flag(&w, "deleted", d->ok == 0);
				break;
			case PC_JOP_INCR:
				if (d->ok == 0)
					tw_int(&w, "value", d->newval);
				else
					tw_error(&w, "holder rejected");
				break;
			case PC_JOP_APPEND:
				if (d->ok == 0)
					tw_int(&w, "count", d->count);
				else
					tw_error(&w, "holder rejected");
				break;
			case PC_JOP_HCMD:
				/* the deciding node's reply tree IS the reply: a valid
				 * item of this codec (i b n a o e), handed on as it came */
				if (d->ok == 0 && d->val)
					pc_tw_raw(&w, d->val, (size_t)d->vlen);
				else
					tw_error(&w, "holder rejected");
				break;
			case PC_JOP_RLHIT:
				if (d->ok == 0) {
					pc_tw_map(&w);
					pc_tw_key(&w, "count");
					pc_tw_i64(&w, d->newval);
					pc_tw_key(&w, "allowed");
					pc_tw_bool(&w, d->count != 0);
					pc_tw_end(&w);
				} else {
					tw_error(&w, "holder rejected");
				}
				break;
			default:               /* GET: the fragment's JSON text */
				if (d->ok == 0 && d->val) {
					pc_tw_map(&w);
					pc_tw_key(&w, "found");
					pc_tw_bool(&w, 1);
					pc_tw_key(&w, "value");
					pc_tw_bulk(&w, d->val, (size_t)d->vlen);
					pc_tw_end(&w);
				} else {
					tw_flag(&w, "found", 0);
				}
			}
			break;
		default:                       /* PC_DONE_PULL: the get reply */
			if (pk->pres) {
				pc_pres_tree(&w, pk->pres, d->found, d->ver,
					d->ttl_left);
			} else if (pull_wrongtype) {
				tw_error(&w, pc_wrongtype_msg);
			} else if (d->found) {
				pc_tw_map(&w);
				pc_tw_key(&w, "found");
				pc_tw_bool(&w, 1);
				pc_tw_key(&w, "source");
				pc_tw_str(&w, "cluster");
				pc_tw_key(&w, "value");
				pc_tw_bulk(&w, d->val, (size_t)d->vlen);
				pc_tw_key(&w, "ttl");
				pc_tw_i64(&w, d->ttl_left ? (long long)d->ttl_left
					: -1);
				pc_tw_end(&w);
			} else {
				tw_flag(&w, "found", 0);
			}
		}
		tn = park_moved(pk, &flags, trail);
		if (pc_tw_done(&w) == 0)
			bin_reply_ex(c, id, flags, (const char *)w.b, w.n, trail, tn);
		else
			bin_reply(c, id, PC_BIN_F_ERR, "reply too large", 15);
		if (conn_flush(c) < 0)
			conn_destroy(c);
	}

done:

	free(pk->key);
	pk->key = NULL;
	free(pk->hargs);
	pk->hargs = NULL;
	pk->conn = NULL;
}

/* ---- notifications ----------------------------------------------------- */

/* a pub/sub frame goes onto a connection THIS thread owns, appended and flushed on the
 * owner's turn; a refused append is the output cap, and the engine kills the connection */
static int push_flush(struct pc_conn *c);   /* with pc_conn_notify */
static int push_stage(struct pc_conn *c);   /* staged per worker turn */

/* ---- pub/sub pushes over UDP (lib/perfd_push.h): a stream is one worker's, no lock;
 * datagrams go only to the TCP peer's address, from the door's shared UDP socket */
struct pc_udp {
	struct psu_stream st;
	uint64_t confirmed_seq;            /* the last sequence before messages */
	unsigned char key[PFP_KEY];
	unsigned char cookie[PFP_COOKIE];
	pc_addr dst;
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
	pc_addr me;
	socklen_t ml = sizeof me;
	int i, wild = -1;

	if (getsockname(c->fd, &me.sa, &ml) != 0 ||
	    pc_addr_family(&me) != AF_INET)
		return -1;
	for (i = 0; i < n_udp_doors; i++) {
		if (udp_doors[i].port != pc_addr_port(&me))
			continue;
		if (udp_doors[i].addr == pc_addr_v4(&me))
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
	        &u->dst.sa, pc_addr_len(&u->dst)) < 0) {
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
	pc_addr peer;
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
	if (getpeername(c->fd, &peer.sa, &pl) != 0 ||
	    pc_addr_family(&peer) != AF_INET) {
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
	pc_addr_set_port(&u->dst, (uint16_t)port);
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
	const char *reason = why == PSU_PRUNE_SILENT ? "no ack" : "no progress";
	unsigned char msg[160];
	char sid[17];
	struct pc_tw w;

	/* the stream id as the 16 hex characters pubsub_udp answered, so lib/perfd_push.h's
	 * consumers match it as text; built before udp_free, since the id lives in the stream */
	snprintf(sid, sizeof sid, "%016llx", (unsigned long long)c->udp->st.id);
	pc_tw_init(&w, msg, sizeof msg);
	pc_tw_map(&w);
	pc_tw_key(&w, "method");
	pc_tw_str(&w, "pubsub_udp_pruned");
	pc_tw_key(&w, "params");
	pc_tw_map(&w);
	pc_tw_key(&w, "stream");
	pc_tw_bulk(&w, sid, 16);
	pc_tw_key(&w, "reason");
	pc_tw_str(&w, reason);
	pc_tw_end(&w);
	pc_tw_end(&w);

	UDP_COUNT(*(why == PSU_PRUNE_SILENT ? &udp_pruned_silent
		: &udp_pruned_stalled));
	LM_NOTICE("pub/sub: UDP pushes to %s pruned (%s) - the connection's "
		"subscriptions are dropped\n", pc_addr_str(&c->udp->dst),
		reason);
	udp_free(c);
	if (c->ps_used)
		pc_pubsub_conn_closed(c);
	if (pc_tw_done(&w) == 0)
		pc_conn_notify(c, (const char *)w.b, w.n);
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
	/* a confirmed UDP stream takes what fits a datagram.  A send the
	 * socket refused is a gap the client counts, not a reason for TCP */
	if (c->udp && c->udp->st.active &&
	    udp_push(c->udp, pat, plen, chan, clen, data, dlen) <= 0)
		return 0;
	if (c->dialect != PC_D_BIN)
		return 0;                      /* still sniffing: nothing to frame it in yet */
	/* a native subscriber gets a NOTIFY tree {method: "message"|"pmessage", params: {pattern?,
	 * channel, payload}}, built once into the caller's note for every native subscriber */
	if (!note->state) {
		size_t cap = 128 + plen + clen + dlen;   /* the tags are ~100 */
		struct pc_tw w;

		note->state = -1;
		note->buf = malloc(cap);
		if (!note->buf)
			return PC_PS_PUSH_NOMEM;
		pc_tw_init(&w, (unsigned char *)note->buf, cap);
		pc_tw_map(&w);
		pc_tw_key(&w, "method");
		pc_tw_str(&w, pat ? "pmessage" : "message");
		pc_tw_key(&w, "params");
		pc_tw_map(&w);
		if (pat) {
			pc_tw_key(&w, "pattern");
			pc_tw_bulk(&w, pat, plen);
		}
		pc_tw_key(&w, "channel");
		pc_tw_bulk(&w, chan, clen);
		pc_tw_key(&w, "payload");
		pc_tw_bulk(&w, data, dlen);
		pc_tw_end(&w);
		pc_tw_end(&w);
		if (pc_tw_done(&w) != 0)
			return PC_PS_PUSH_NOMEM;       /* state stays -1: every one skips */
		note->len = w.n;
		note->state = 1;
	}
	if (note->state < 0)
		return PC_PS_PUSH_NOMEM;
	return pc_conn_notify(c, note->buf, note->len);
}

/* the row's subscription count, kept by the engine's bookkeeping */
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

/* a push was staged: send what the socket takes; a failure at the output cap is the slow
 * consumer, otherwise a broken socket (closed and counted differently) */
static int push_flush(struct pc_conn *c)
{
	if (conn_flush(c) == 0)
		return 0;
	return c->out_failed == 1 ||
		c->out_len + (c->wire_len - c->wire_off) >= PC_MAX_OUTQ
		? PC_PS_PUSH_SLOW : PC_PS_PUSH_BROKEN;
}

/* staged pushes, written once per worker turn (pc_conn_flush_pushes); past PUSH_FLUSH_AT, or
 * off a worker (nothing there would flush), a push writes at once */
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

/* paused publishers (pubsub.h): a command that left a worker's queue at half its cap stops
 * its connection's input until every queue it pressed is below a quarter (50 ms re-check) */
static __thread struct pc_conn *paused_head;
static __thread int paused_here;

static void pause_set_events(struct pc_conn *c)
{
	struct epoll_event ev;

	if (c->ep < 0) {                   /* io_uring: cancel or re-arm */
		ur_events(c);
		return;
	}
	ev.events = conn_ev_in(c) | (c->want_write ? EPOLLOUT : 0);
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
		/* serve what it sent while paused, in order; conn_take first opens raw records an
		 * io_uring receive delivered before its cancel took */
		if ((c->ep < 0 ? conn_take(c, NULL, 0) : process_plaintext(c)) < 0) {
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
			if (!c->why)
				c->why = "quit";
			conn_destroy(c);
		}
	}
}

/* off the output-paused list (a destroy, or the resume pass) */
static void out_unlink(struct pc_conn *c)
{
	struct pc_conn **pp;

	if (!c->out_paused)
		return;
	for (pp = &outp_head; *pp; pp = &(*pp)->outp_next)
		if (*pp == c) {
			*pp = c->outp_next;
			break;
		}
	c->outp_next = NULL;
	c->out_paused = 0;
	outp_here--;
	__atomic_sub_fetch(&outp_now_all, 1, __ATOMIC_RELAXED);
}

static void out_pause(struct pc_conn *c)
{
	if (c->out_paused)
		return;
	/* the first time a connection is held back is said - once, within the
	 * connection-log budget; how often is in its disconnect line */
	if (!c->out_pauses_n++ && conn_log_ok())
		LM_NOTICE("throttling client %s: %u MB of replies unread - its "
			"further requests wait until they drain\n",
			pc_obs_conn_addr(c->obs),
			(unsigned int)(out_pending(c) >> 20));
	c->out_paused = 1;
	c->out_progress = conn_now;
	c->outp_next = outp_head;
	outp_head = c;
	outp_here++;
	__atomic_add_fetch(&outp_now_all, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&outp_pauses_all, 1, __ATOMIC_RELAXED);
	pause_set_events(c);               /* input off: no EPOLLIN, receive cancelled */
}

int pc_conn_out_paused_open(void)
{
	return outp_here;
}

/* a writer held until this worker's WAL ring has room again */
static void wal_pause(struct pc_conn *c)
{
	if (c->wal_paused)
		return;
	c->wal_paused = 1;
	c->walp_next = walp_head;
	walp_head = c;
	walp_here++;
	__atomic_add_fetch(&walp_now_all, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&walp_pauses_all, 1, __ATOMIC_RELAXED);
	pause_set_events(c);               /* input off */
}

static void wal_unlink(struct pc_conn *c)
{
	struct pc_conn **pp;

	if (!c->wal_paused)
		return;
	for (pp = &walp_head; *pp; pp = &(*pp)->walp_next)
		if (*pp == c) {
			*pp = c->walp_next;
			break;
		}
	c->walp_next = NULL;
	c->wal_paused = 0;
	walp_here--;
	__atomic_sub_fetch(&walp_now_all, 1, __ATOMIC_RELAXED);
}

int pc_conn_wal_paused_open(void)
{
	return walp_here;
}

void pc_conn_wal_pause_stats(unsigned long long *now, unsigned long long *pauses)
{
	*now = __atomic_load_n(&walp_now_all, __ATOMIC_RELAXED);
	*pauses = __atomic_load_n(&walp_pauses_all, __ATOMIC_RELAXED);
}

/* once a turn, with the ring half free: every writer it held is served from where it
 * stopped; the list is taken whole, as one served here can pause again */
void pc_conn_resume_wal(void)
{
	struct pc_conn *c, *next, *list = walp_head;

	if (!list || pc_wal_tight(1))
		return;
	walp_head = NULL;
	for (c = list; c; c = next) {
		next = c->walp_next;
		c->walp_next = NULL;
		if (!c->wal_paused || c->dead)
			continue;
		c->wal_paused = 0;
		walp_here--;
		__atomic_sub_fetch(&walp_now_all, 1, __ATOMIC_RELAXED);
		/* what it sent meanwhile first: a receive armed before it could
		 * pause again would add to the held input every round (io_uring) */
		if ((c->ep < 0 ? conn_take(c, NULL, 0) : process_plaintext(c)) < 0) {
			c->why = "protocol error";
			conn_destroy(c);
			continue;
		}
		pause_set_events(c);
		if (conn_flush(c) < 0) {
			c->why = "write error";
			conn_destroy(c);
			continue;
		}
		if (c->close_after && !c->out_len && c->wire_off >= c->wire_len) {
			if (!c->why)
				c->why = "quit";
			conn_destroy(c);
		}
	}
}

/* fsync = sync: a reply staged after a write this connection logged, or one this worker logged
 * outside any connection's turn, waits with all after it until the device has synced it */
static void sync_note(struct pc_conn *c)
{
	unsigned long long w = c->sync_want;

	if (pc_wal_tl_seq > w)             /* a completion's write: whose reply is not known */
		w = pc_wal_tl_seq;
	if (w <= c->sync_seq || !pc_wal_sync_mode() || w <= pc_wal_synced_low())
		return;
	if (!c->sync_held) {
		c->sync_held = 1;
		c->sync_from = c->out_len;
		c->sync_t0_us = deny_now();
		c->sync_next = synch_head;
		synch_head = c;
		synch_here++;
		__atomic_add_fetch(&synch_now_all, 1, __ATOMIC_RELAXED);
		__atomic_add_fetch(&synch_holds_all, 1, __ATOMIC_RELAXED);
	}
	c->sync_seq = w;
	pc_wal_kick_wanted();
}

static void sync_unlink(struct pc_conn *c)
{
	struct pc_conn **pp;

	if (!c->sync_held)
		return;
	for (pp = &synch_head; *pp; pp = &(*pp)->sync_next)
		if (*pp == c) {
			*pp = c->sync_next;
			break;
		}
	c->sync_next = NULL;
	c->sync_held = 0;
	synch_here--;
	__atomic_sub_fetch(&synch_now_all, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&synch_ms_all, (unsigned long long)(deny_now() -
		c->sync_t0_us), __ATOMIC_RELAXED);
}

int pc_conn_sync_held_open(void)
{
	return synch_here;
}

void pc_conn_sync_stats(unsigned long long *now, unsigned long long *holds,
		unsigned long long *hold_ms)
{
	*now = __atomic_load_n(&synch_now_all, __ATOMIC_RELAXED);
	*holds = __atomic_load_n(&synch_holds_all, __ATOMIC_RELAXED);
	*hold_ms = __atomic_load_n(&synch_ms_all, __ATOMIC_RELAXED);
}

/* once a turn: a connection whose held writes are synced sends its replies; if the device
 * refused, no held reply is ever sent - the connection closes, its writes uncertain */
void pc_conn_resume_sync(void)
{
	struct pc_conn *c, *next;
	unsigned long long lo;

	if (!synch_head)
		return;
	lo = pc_wal_synced_low();
	for (c = synch_head; c; c = next) {
		next = c->sync_next;
		if (c->dead)
			continue;
		if (pc_wal_failed() && lo < c->sync_seq) {
			c->why = "the WAL's storage failed before its writes were synced";
			conn_destroy(c);
			continue;
		}
		if (lo < c->sync_seq)
			continue;
		if (conn_flush(c) < 0) {
			c->why = "write error";
			conn_destroy(c);
			continue;
		}
		if (c->close_after && !c->out_len && c->wire_off >= c->wire_len) {
			if (!c->why)
				c->why = "quit";
			conn_destroy(c);
		}
	}
	if (synch_head)
		pc_wal_kick_wanted();          /* a sync short of them: the next one kicks again */
}

void pc_conn_out_pause_stats(struct pc_out_pause_stats *st)
{
	st->paused_now = __atomic_load_n(&outp_now_all, __ATOMIC_RELAXED);
	st->pauses = __atomic_load_n(&outp_pauses_all, __ATOMIC_RELAXED);
	st->stall_kills = __atomic_load_n(&outp_stalls_all, __ATOMIC_RELAXED);
}

/* once a turn: a paused connection drained below PC_OUT_RESUME_AT is served again, one
 * stalled for output_stall_s is closed as a slow consumer; the list is taken whole */
void pc_conn_resume_out(void)
{
	struct pc_conn *c, *next, *list = outp_head;

	outp_head = NULL;
	for (c = list; c; c = next) {
		next = c->outp_next;
		c->outp_next = NULL;
		if (!c->out_paused || c->dead)
			continue;              /* destroyed meanwhile: counted out */
		if (out_pending(c) >= PC_OUT_RESUME_AT) {
			if (conn_now - c->out_progress < (unsigned int)pc_output_stall_s) {
				c->outp_next = outp_head;   /* still draining */
				outp_head = c;
				continue;
			}
			out_unlink(c);
			__atomic_add_fetch(&outp_stalls_all, 1, __ATOMIC_RELAXED);
			if (!c->why)
				c->why = "slow consumer: replies unread, no output "
					"progress within output_stall_s";
			conn_destroy(c);
			continue;
		}
		/* drained: input on, and what it sent meanwhile is served now, in order */
		c->out_paused = 0;
		outp_here--;
		__atomic_sub_fetch(&outp_now_all, 1, __ATOMIC_RELAXED);
		pause_set_events(c);
		if (conn_take(c, NULL, 0) < 0) {
			if (!c->why)
				c->why = "protocol error";
			conn_destroy(c);
			continue;
		}
		if (conn_flush(c) < 0) {
			if (!c->why)
				c->why = "write error";
			conn_destroy(c);
			continue;
		}
		if (c->close_after && !c->out_len && !c->ur_send &&
		    c->wire_off >= c->wire_len) {
			if (!c->why)
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
	size_t at;
	int rc;

	if (c->encrypted && !c->established)
		return PC_PS_PUSH_NOMEM;           /* not yet a channel: skip */
	if (c->dialect != PC_D_BIN)
		return PC_PS_PUSH_NOMEM;       /* nothing to frame it in: skip */
	hdr[0] = (char)PC_BIN_MAGIC;
	hdr[1] = PC_BIN_VER;
	hdr[2] = PC_BIN_NOTIFY;
	hdr[3] = 0;
	put32(hdr + 4, (uint32_t)n);
	put64(hdr + 8, 0);
	at = c->out_len;
	rc = out_append(c, hdr, sizeof hdr);
	if (rc == 0)
		rc = out_append(c, payload, n);
	if (rc < 0) {
		c->out_len = at;               /* never a header alone */
		return rc;                     /* SLOW at the cap, NOMEM */
	}
	return push_stage(c);
}

void pc_conn_notify_all(struct pc_conn *list, const char *payload, size_t n)
{
	struct pc_conn *c;

	for (c = list; c; c = c->next) {
		if (c->resp_only || c->http_only)
			continue;
		if (c->dialect != PC_D_BIN)
			continue;                  /* RESP or still sniffing */
		pc_conn_notify(c, payload, n);
	}
}

/* ---- the event pump ---------------------------------------------------- */

static unsigned long long hs_deferred_all;

void pc_conn_hs_park(struct pc_conn *c)
{
	struct epoll_event ev;

	if (c->hs_parked || c->dead)
		return;
	c->hs_parked = 1;
	if (c->ep >= 0) {
		/* its bytes wait in the socket; staged output still goes */
		ev.events = conn_ev_in(c) | (c->want_write ? EPOLLOUT : 0);
		ev.data.ptr = c;
		epoll_ctl(c->ep, EPOLL_CTL_MOD, c->fd, &ev);
	}
	c->hs_next = NULL;
	c->hs_prev = hs_tail;
	if (hs_tail)
		hs_tail->hs_next = c;
	else
		hs_head = c;
	hs_tail = c;
	hs_parked_here++;
	if (c->ep < 0)                     /* io_uring: its receive is cancelled; */
		ur_events(c);              /* what came is held in raw */
}

int pc_conn_hs_run(int budget)
{
	struct epoll_event ev;
	struct pc_conn *c;
	int n = 0;

	while (n < budget && (c = hs_head) != NULL) {
		hs_unlink(c);
		if (c->ep < 0) {               /* io_uring: its bytes are held here */
			ur_events(c);
			ur_serve(c, NULL, 0);
			n++;
			continue;
		}
		ev.events = conn_ev_in(c) | (c->want_write ? EPOLLOUT : 0);
		ev.data.ptr = c;
		epoll_ctl(c->ep, EPOLL_CTL_MOD, c->fd, &ev);
		pc_conn_event(c, EPOLLIN);     /* may destroy it: not touched after */
		n++;
	}
	return n;
}

int pc_conn_hs_parked(void)
{
	return hs_parked_here;
}

void pc_conn_hs_deferred(unsigned int n)
{
	__atomic_add_fetch(&hs_deferred_all, n, __ATOMIC_RELAXED);
}

unsigned long long pc_conn_hs_deferred_total(void)
{
	return __atomic_load_n(&hs_deferred_all, __ATOMIC_RELAXED);
}

/* a secret this connection carries stays off the network: TLS, or a loopback peer */
int pc_conn_secure(const void *conn)
{
	const struct pc_conn *c = conn;

	return c && (c->tls || c->peer_loop);
}

/* the TLS layer's two outlets */
static int tls_plain(void *ctx, const char *p, size_t n)
{
	return in_append(ctx, p, n);
}

static int tls_wire(void *ctx, const char *p, size_t n)
{
	const struct pc_conn *c = ctx;

	if (!n)                            /* asked first: 1 = a send in flight, leave them */
		return c->ur_send ? 1 : 0;
	return wire_append(ctx, p, n);
}

/* one chunk of input (an epoll read or an io_uring receive) taken in and served; @n == 0
 * serves what is buffered.  -1 drop (c->why says why), 1 handshake needs more, 0 done */
static int conn_take(struct pc_conn *c, const char *buf, size_t n)
{
	int tls_closed = 0;

	if (c->encrypted) {
		int st;

		if (n && raw_append(c, buf, n) < 0)
			return -1;
		while (!c->established) {
			st = channel_handshake(c);
			if (st < 0) {
				/* no Noise handshake opens with '*' or a letter: a RESP
				 * client on the native door, the commonest mistake */
				unsigned char f = c->raw_len
					? (unsigned char)c->raw[0] : 0;

				if (f == '*' || (f >= 'A' && f <= 'Z') ||
				    (f >= 'a' && f <= 'z')) {
					pc_addr me;
					socklen_t ml = sizeof me;
					int lp = getsockname(c->fd,
						&me.sa, &ml) == 0
						? pc_addr_port(&me) : 0;

					if (conn_log_ok())
						LM_NOTICE("client %s sent a RESP "
							"command to the native door on "
							"port %d, which speaks the native "
							"protocol (Noise) - the RESP door "
							"is %s\n", pc_obs_conn_addr(c->obs),
							lp, pc_resp_door()
							? pc_resp_door()
							: "not configured");
					c->why = "RESP on the native door";
				} else if (f == PC_BIN_MAGIC) {
					/* a native client that has no
					 * secret: it skipped the handshake */
					if (conn_log_ok())
						LM_NOTICE("client %s sent a request "
							"in the clear to the native door, "
							"which needs the Noise handshake "
							"first - the client was given no "
							"client secret\n",
							pc_obs_conn_addr(c->obs));
					c->why = "request in the clear - no client secret";
				} else
					c->why = "handshake failed - no or wrong client secret";
				return -1;
			}
			if (st == 0)            /* need more handshake bytes */
				return 1;
			if (conn_flush(c) < 0)   /* send msg2 now */
				return -1;
			if (st == 2 && !c->raw_len)  /* msg2 out, the switch record due */
				return 1;
		}
		if (!c->encrypted) {         /* switched: the rest is plaintext */
			if (c->raw_len && in_append(c, c->raw, c->raw_len) < 0)
				return -1;
			c->raw_len = 0;
		} else {
			int rc;

			in_hold_serve = !n && c->ep < 0;
			rc = channel_records(c);
			in_hold_serve = 0;
			if (rc < 0)
				return -1;
		}
	} else if (c->tls) {               /* records in, plaintext to the parser */
		int rc;

		if (c->raw_len) {          /* records held while paused or parked (ur_hold) */
			size_t hl = c->raw_len;

			c->raw_len = 0;
			in_hold_serve = c->ep < 0;
			rc = pc_tls_input(c->tls, c->raw, hl, tls_plain, tls_wire, c, &c->why);
			in_hold_serve = 0;
			if (rc < 0)
				return -1;
			tls_closed = rc > 0;
		}
		rc = pc_tls_input(c->tls, buf, n, tls_plain, tls_wire, c, &c->why);
		if (rc < 0)
			return -1;
		tls_closed |= rc > 0;
		if (c->wire_off < c->wire_len && conn_flush(c) < 0)
			return -1;
		if (!pc_tls_up(c->tls))
			return 1;
	} else {
		if (n && in_append(c, buf, n) < 0)
			return -1;
	}
	if (process_plaintext(c) < 0)
		return -1;
	/* the client's close_notify: what it sent before is served, then the connection goes */
	if (tls_closed)
		c->close_after = 1;
	return 0;
}

/* an io_uring connection's input, served as the epoll loop serves a read; a QUIT closes
 * once its reply is out, @n == 0 serves what is held */
static void ur_serve(struct pc_conn *c, const char *buf, size_t n)
{
	if (conn_take(c, buf, n) < 0)
		goto drop;
	if (conn_flush(c) < 0)
		goto drop;
	if (c->close_after && !c->out_len && c->wire_off >= c->wire_len) {
		if (!c->why)
			c->why = "quit";
		goto drop;
	}
	return;
drop:
	conn_destroy(c);
}

/* hold all a receive delivered to a paused or parked connection before its cancel took,
 * which can be more than one request's worth */
static int ur_hold(struct pc_conn *c, const char *buf, size_t n)
{
	int rec = c->encrypted || c->tls;  /* TLS bytes are records: raw, not input */
	char **b = rec ? &c->raw : &c->in;
	size_t *cap = rec ? &c->raw_cap : &c->in_cap;
	size_t *len = rec ? &c->raw_len : &c->in_len;
	int rc = buf_reserve(b, cap, *len, n, PC_MAX_REQ + 4 * PC_NOISE_MAXMSG +
	        PC_UR_HOLD_MAX);

	if (rc < 0) {
		c->why = rc == -2 ? "out of memory holding its input"
			: "input held while paused past its limit";
		return -1;
	}
	bufs_mark(c);
	memcpy(*b + *len, buf, n);
	*len += n;
	return 0;
}

/* one io_uring completion from the worker's loop; past PC_HS_PER_TURN (@hs) a handshake's
 * bytes are held and the connection parked, as epoll parks one, counted in @hs_deferred */
void pc_conn_ur_complete(uint64_t ud, int res, uint32_t flags, int *hs,
		unsigned int *hs_deferred)
{
	struct pc_conn *c = (struct pc_conn *)(uintptr_t)(ud & ~UR_OP_MASK);
	int more = (flags & PC_UR_CQE_F_MORE) != 0;

	if ((ud & UR_OP_MASK) == UR_OP_SEND) {
		c->ur_send = 0;
		c->ur_inflight--;
		if (c->dead) {                 /* the kernel is done with it */
			free(c->ur_wire_hold);
			c->ur_wire_hold = NULL;
		} else if (res < 0) {
			/* a kill (a slow subscriber) shuts the socket down, and the
			 * send in flight then fails: the kill's reason stands */
			if (!c->why)
				c->why = "write error";
			conn_destroy(c);
		} else {
			c->wire_off += (size_t)res;
			if (res > 0)
				c->out_progress = conn_now;
			if (c->wire_off >= c->wire_len) {
				c->wire_off = c->wire_len = 0;
				if (c->obs_pending) {
					c->obs_pending = 0;
					pc_obs_conn_pending(c->obs, 0);
				}
			}
			/* the rest, and whatever was staged meanwhile; a QUIT
			 * closes once all of it is out */
			if (conn_flush(c) < 0) {
				if (!c->why)
					c->why = "write error";
				conn_destroy(c);
			} else if (c->close_after && !c->out_len && !c->ur_send &&
			    c->wire_off >= c->wire_len) {
				if (!c->why)
					c->why = "quit";
				conn_destroy(c);
			}
		}
		ur_settle(c);
		return;
	}
	if ((ud & UR_OP_MASK) == UR_OP_POLLOUT) {
		c->ur_pollout = 0;
		c->ur_inflight--;
		/* poll's revents are epoll's bits; input is the receive's */
		if (!c->dead)
			pc_conn_event(c, res < 0 ? (uint32_t)EPOLLERR :
				(uint32_t)res & ~(uint32_t)EPOLLIN);
		ur_settle(c);
		return;
	}
	if (!more) {                       /* the receive has ended */
		c->ur_recv = 0;
		c->ur_cancel = 0;
		c->ur_inflight--;
	}
	if (res > 0 && (flags & PC_UR_CQE_F_BUFFER)) {
		uint16_t bid = PC_UR_CQE_BID(flags);
		const char *b = pc_ur_buf(ur_ring, bid);

		PC_IOSTAT_ADD(ur_recvs, 1);
		if (!c->dead) {
			c->last_io = conn_now;
			if (conn_input_off(c) || c->hs_parked) {
				if (ur_hold(c, b, (size_t)res) < 0)
					conn_destroy(c);
			} else if (pc_conn_hs_pending(c, EPOLLIN) &&
			    (*hs)++ >= PC_HS_PER_TURN) {
				if (ur_hold(c, b, (size_t)res) < 0)
					conn_destroy(c);
				else {
					pc_conn_hs_park(c);
					(*hs_deferred)++;
				}
			} else
				ur_serve(c, b, (size_t)res);
		}
		pc_ur_buf_return(ur_ring, bid);
	} else if (res == 0) {
		if (!c->dead)
			conn_destroy(c);       /* the peer closed */
	} else if (res < 0 && res != -ECANCELED && res != -ENOBUFS) {
		if (!c->dead) {
			c->why = "read error";
			conn_destroy(c);
		}
	}
	/* ended but wanted (out of buffers, a resume before the cancel's
	 * completion): armed again */
	if (!more && !c->dead)
		ur_events(c);
	ur_settle(c);
}

int pc_conn_hs_pending(const struct pc_conn *c, uint32_t events)
{
	return ((c->encrypted && !c->established) || (c->tls && !pc_tls_up(c->tls))) &&
		!c->dead && (events & EPOLLIN) && !(events & (EPOLLHUP | EPOLLERR));
}

int pc_conn_event(struct pc_conn *c, uint32_t events)
{
	ssize_t n;
	char tmp[16384];
	int st;

	/* destroyed earlier in this turn, its event still in the batch */
	if (c->dead)
		return -1;
	c->last_io = conn_now;             /* not idle this second */
	if (events & (EPOLLHUP | EPOLLERR))
		goto drop;
	if (c->ep < 0)                     /* io_uring: its receive owns the input */
		events &= ~(uint32_t)EPOLLIN;

	if (events & EPOLLIN) {
		for (;;) {
			if (conn_input_off(c))
				break;                 /* its input waits in the socket */
			n = read(c->fd, tmp, sizeof tmp);
			PC_IOSTAT_ADD(reads, 1);
			if (n == 0)
				goto drop;
			if (n < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					break;
				c->why = "read error";
				goto drop;
			}
			st = conn_take(c, tmp, (size_t)n);
			if (st < 0)
				goto drop;
			if (st > 0) {              /* need more handshake bytes */
				if ((size_t)n < sizeof tmp)
					break;     /* none yet: they wake us */
				continue;
			}
			/* a short read took all the socket held, and the sockets are level-
			 * triggered: reading on for the EAGAIN costs every request a 2nd read() */
			if ((size_t)n < sizeof tmp)
				break;
		}
	}
	if (conn_flush(c) < 0)
		goto drop;
	if (c->close_after && !c->out_len && c->wire_off >= c->wire_len) {
		/* QUIT: reply flushed, close - unless a refusal already named the
		 * reason (max_clients, the not-a-frame line) */
		if (!c->why)
			c->why = "quit";
		goto drop;
	}
	return 0;
drop:
	conn_destroy(c);
	return -1;
}

/* the connection named by a verb - the same row CLIENT SETNAME fills */
void pc_conn_set_name(void *conn, const char *name, size_t n)
{
	struct pc_conn *c = conn;

	if (c && c->obs)
		pc_obs_conn_name(c->obs, name, n);
}

const char *pc_conn_who(void *conn, char *buf, size_t cap)
{
	struct pc_conn *c = conn;
	const char *addr = c && c->obs ? pc_obs_conn_addr(c->obs) : NULL;
	const char *nm = "";
	size_t nl = 0;

	if (c && c->obs)
		nm = pc_obs_conn_name_get(c->obs, &nl);
	if (nl)
		snprintf(buf, cap, "%.*s@%s", (int)nl, nm, addr ? addr : "?");
	else
		snprintf(buf, cap, "%s", addr ? addr : "a client");
	return buf;
}

int pc_conn_plain_auth(const void *conn)
{
	const struct pc_conn *c = conn;

	return c && c->plain_auth;
}

/* the peer of @conn as the CLIENT LIST row renders it */
const char *pc_conn_peer_text(void *conn)
{
	struct pc_conn *c = conn;

	return c && c->obs ? pc_obs_conn_addr(c->obs) : NULL;
}
