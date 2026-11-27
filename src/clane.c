/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* The TCP frame lane (see clane.h). */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/eventfd.h>
#include <sys/socket.h>

#include "compat/dprint.h"
#include "clwire.h"
#include "clane.h"

#define FRAME_MAX  (HDR_LEN + MAX_DGRAM + 64)
#define MAX_PEERS  512
/* Anyone who reaches the port can send a hello, so an inbound connection is bounded: hello
 * within LIN_HELLO_MS, a frame every LIN_IDLE_MS, LIN_PER_ADDR at once per address. */
#define LIN_HELLO_MS  3000
#define LIN_IDLE_MS   120000
#define LIN_PER_ADDR  4
#define LIN_HELLO_LEN 10
#define LIN_PASS_FRAMES 64             /* a connection's frames per pass of the lane thread */

/* what one inbound connection holds of the receive queue: at most what a sender may queue
 * for one peer (CLANE_MAX_QUEUE).  Freed by the last of the connection and its frames */
struct rxacct {
	size_t bytes;
	int refs;
};

/* under L.mx */
static void acct_put(struct rxacct *a)
{
	if (a && !--a->refs)
		free(a);
}

struct frame {
	struct frame *next;
	long long at_ms;
	size_t len;
	struct rxacct *acct;               /* a received frame's connection; NULL outbound */
	unsigned char data[];
};

struct lpeer {
	pc_addr to;             /* the peer's CLUSTER address */
	int fd;                            /* -1 idle */
	int connected;
	struct frame *head, *tail;
	size_t qbytes;
	size_t woff;                       /* written of [len4 + head] */
	unsigned char hello[10];
	size_t hoff;                       /* written of the hello */
	long long next_try_ms;
	int backoff_ms;
	int said_down;
	int forced;                        /* everything by TCP */
	int probe;                         /* connect even with nothing queued */
	long long conn_start_ms;           /* the connect in flight began */
	int idle;                          /* the lane thread leaves it alone: reusable */
};

struct lin {
	int fd;
	pc_addr from;           /* from the hello */
	pc_addr peer;           /* the connection's source */
	int hello_ok;
	unsigned char *buf;     /* the hello's bytes, then FRAME_MAX + 4 */
	size_t have;
	long long since_ms;     /* the accept, the hello, the last frame */
	struct rxacct *acct;
};

static int forced_any;                 /* fast path for clane_forced */

static struct {
	pthread_mutex_t mx;
	struct lpeer peer[MAX_PEERS];
	int npeer;
	int lfd, wake_fd, rx_fd;
	int lane_off;
	pc_addr self;
	/* the cluster thread's queue */
	struct frame *rx_head, *rx_tail;
	size_t rx_bytes;
	struct clane_figs f;
} L = { .mx = PTHREAD_MUTEX_INITIALIZER, .lfd = -1, .wake_fd = -1,
	.rx_fd = -1 };

/* rx frames carry their sender in front of the data */
struct rxhdr { pc_addr from; };

static long long mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void wake(int fd)
{
	uint64_t one = 1;

	if (fd >= 0)
		(void)!write(fd, &one, sizeof one);
}

static int same(const pc_addr *a, const pc_addr *b)
{
	return pc_addr_eq(a, b);
}

/* the peer for @to, made if it is new; NULL if the table is full.
 * Under L.mx. */
static struct lpeer *peer_get(const pc_addr *to)
{
	int i;

	for (i = 0; i < L.npeer; i++)
		if (same(&L.peer[i].to, to))
			return &L.peer[i];
	if (L.npeer >= MAX_PEERS) {
		/* full: a slot the lane thread marked idle, still with nothing to do */
		for (i = 0; i < L.npeer; i++) {
			struct lpeer *p = &L.peer[i];

			if (p->idle && !p->head && !p->probe && !p->forced && p->fd < 0) {
				memset(p, 0, sizeof *p);
				p->to = *to;
				p->fd = -1;
				return p;
			}
		}
		return NULL;
	}
	memset(&L.peer[L.npeer], 0, sizeof L.peer[0]);
	L.peer[L.npeer].to = *to;
	L.peer[L.npeer].fd = -1;
	return &L.peer[L.npeer++];
}

int clane_tcp_state(const pc_addr *to, long long *age_ms)
{
	int i, st = CLANE_TCP_DOWN;

	*age_ms = 0;
	pthread_mutex_lock(&L.mx);
	for (i = 0; i < L.npeer; i++)
		if (same(&L.peer[i].to, to)) {
			if (L.peer[i].connected)
				st = CLANE_TCP_UP;
			else if (L.peer[i].fd >= 0 || L.peer[i].probe) {
				st = CLANE_TCP_PENDING;
				*age_ms = L.peer[i].conn_start_ms ?
					mono_ms() - L.peer[i].conn_start_ms : 0;
			}
			break;
		}
	pthread_mutex_unlock(&L.mx);
	return st;
}

void clane_probe(const pc_addr *to)
{
	struct lpeer *p;

	if (L.lfd < 0)
		return;
	pthread_mutex_lock(&L.mx);
	p = peer_get(to);
	if (p && !p->connected && !p->probe) {
		p->probe = 1;
		p->conn_start_ms = mono_ms();
		p->next_try_ms = 0;            /* now, not after a backoff */
	}
	pthread_mutex_unlock(&L.mx);
	wake(L.wake_fd);
}

void clane_force(const pc_addr *to, int on)
{
	struct lpeer *p;
	int i, n = 0;

	pthread_mutex_lock(&L.mx);
	p = peer_get(to);
	if (p)
		p->forced = on;
	for (i = 0; i < L.npeer; i++)
		n += L.peer[i].forced;
	L.f.forced = n;
	__atomic_store_n(&forced_any, n, __ATOMIC_RELEASE);
	pthread_mutex_unlock(&L.mx);
}

int clane_forced(const pc_addr *to)
{
	int i, f = 0;

	if (!__atomic_load_n(&forced_any, __ATOMIC_ACQUIRE))
		return 0;
	pthread_mutex_lock(&L.mx);
	for (i = 0; i < L.npeer; i++)
		if (same(&L.peer[i].to, to)) {
			f = L.peer[i].forced;
			break;
		}
	pthread_mutex_unlock(&L.mx);
	return f;
}

int clane_send(const pc_addr *to, const unsigned char *sealed,
		size_t n)
{
	struct lpeer *p = NULL;
	struct frame *f;
	int kick = 0;

	if (L.lfd < 0 || n > FRAME_MAX)
		return -1;
	f = malloc(sizeof *f + n);
	if (!f)
		return -1;
	f->next = NULL;
	f->at_ms = mono_ms();
	f->len = n;
	f->acct = NULL;
	memcpy(f->data, sealed, n);
	pthread_mutex_lock(&L.mx);
	p = peer_get(to);
	if (!p || p->qbytes + n > CLANE_MAX_QUEUE) {
		L.f.dropped_full++;
		pthread_mutex_unlock(&L.mx);
		free(f);
		return -1;
	}
	kick = !p->head;
	if (p->tail)
		p->tail->next = f;
	else
		p->head = f;
	p->tail = f;
	p->qbytes += n;
	pthread_mutex_unlock(&L.mx);
	if (kick)
		wake(L.wake_fd);
	return 0;
}

int clane_rx_fd(void)
{
	return L.rx_fd;
}

size_t clane_rx_take(unsigned char *buf, size_t cap, pc_addr *from)
{
	struct frame *f;
	size_t n = 0;

	pthread_mutex_lock(&L.mx);
	f = L.rx_head;
	if (f) {
		L.rx_head = f->next;
		if (!L.rx_head)
			L.rx_tail = NULL;
		L.rx_bytes -= f->len;
		if (f->acct) {
			f->acct->bytes -= f->len;
			acct_put(f->acct);
		}
	} else {
		uint64_t v;

		(void)!read(L.rx_fd, &v, sizeof v);  /* drained: clear the wake */
	}
	pthread_mutex_unlock(&L.mx);
	if (!f)
		return 0;
	if (f->len - sizeof(struct rxhdr) <= cap) {
		memcpy(from, f->data, sizeof(struct rxhdr));
		n = f->len - sizeof(struct rxhdr);
		memcpy(buf, f->data + sizeof(struct rxhdr), n);
	}
	free(f);
	return n;
}

void clane_figures(struct clane_figs *out)
{
	int i, up = 0;

	pthread_mutex_lock(&L.mx);
	*out = L.f;
	for (i = 0; i < L.npeer; i++)
		up += L.peer[i].connected;
	out->peers_up = up;
	pthread_mutex_unlock(&L.mx);
}

/* a whole frame arrived on an inbound lane: to the cluster thread */
static void rx_queue(struct rxacct *acct, const pc_addr *from, const unsigned char *d,
		size_t n)
{
	struct frame *f = malloc(sizeof *f + sizeof(struct rxhdr) + n);
	int kick;

	if (!f)
		return;
	f->next = NULL;
	f->len = sizeof(struct rxhdr) + n;
	f->acct = acct;
	memcpy(f->data, from, sizeof(struct rxhdr));
	memcpy(f->data + sizeof(struct rxhdr), d, n);
	pthread_mutex_lock(&L.mx);
	if (L.rx_bytes + f->len > CLANE_MAX_RXQ ||
	        acct->bytes + f->len > CLANE_MAX_QUEUE) {
		L.f.dropped_rx++;
		pthread_mutex_unlock(&L.mx);
		free(f);
		return;
	}
	kick = !L.rx_head;
	if (L.rx_tail)
		L.rx_tail->next = f;
	else
		L.rx_head = f;
	L.rx_tail = f;
	L.rx_bytes += f->len;
	acct->bytes += f->len;
	acct->refs++;
	L.f.recv_frames++;
	L.f.recv_bytes += n;
	pthread_mutex_unlock(&L.mx);
	if (kick)
		wake(L.rx_fd);
}

static void peer_close(struct lpeer *p, long long now)
{
	if (p->probe && !p->connected) {
		p->probe = 0;                  /* the probe's answer is no */
		p->conn_start_ms = 0;
	}
	if (p->fd >= 0)
		close(p->fd);
	p->fd = -1;
	p->connected = 0;
	p->woff = 0;
	p->hoff = 0;
	p->backoff_ms = p->backoff_ms ? p->backoff_ms * 2 : 200;
	if (p->backoff_ms > 5000)
		p->backoff_ms = 5000;
	/* jitter: 50-100% of the backoff */
	p->next_try_ms = now + p->backoff_ms / 2 +
		(long long)(random() % (p->backoff_ms / 2 + 1));
}

/* a host lost without an RST must read as gone in seconds, not at the retransmission limit */
static void lane_keepalive(int fd)
{
	int one = 1, idle = 3, intvl = 1, cnt = 3;
	unsigned int ut = 6000;

	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
	setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &ut, sizeof ut);
}

static void peer_connect(struct lpeer *p, long long now)
{
	pc_addr a = p->to;
	int one = 1, fd;

	pc_addr_set_port(&a, (uint16_t)(pc_addr_port(&p->to) + L.lane_off));
	fd = socket(pc_addr_family(&a), SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		peer_close(p, now);
		return;
	}
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	lane_keepalive(fd);
	/* from the advertise address: the receiver checks the hello against the source,
	 * and an unbound socket leaves by whatever address the route picks */
	{
		pc_addr src = L.self;

		pc_addr_set_port(&src, 0);
		if (bind(fd, &src.sa, pc_addr_len(&src)) != 0) {
			close(fd);
			peer_close(p, now);
			return;
		}
	}
	p->fd = fd;
	p->connected = 0;
	memcpy(p->hello, "PCL1", 4);
	/* the wire: IPv4's four raw bytes and the port, network order */
	memcpy(p->hello + 4, &L.self.in4.sin_addr.s_addr, 4);
	memcpy(p->hello + 8, &L.self.in4.sin_port, 2);
	p->hoff = 0;
	if (!p->conn_start_ms)
		p->conn_start_ms = now;
	L.f.connects++;
	if (connect(fd, &a.sa, pc_addr_len(&a)) != 0 &&
	        errno != EINPROGRESS) {
		L.f.connect_fail++;
		peer_close(p, now);
	}
}

/* write what we can; 0 ok, -1 the connection is gone */
static int peer_write(struct lpeer *p, long long now)
{
	if (!p->connected) {
		int err = 0;
		socklen_t el = sizeof err;

		if (getsockopt(p->fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0 ||
		        err) {
			L.f.connect_fail++;
			if (!p->said_down) {
				char ab[PC_ADDR_STRLEN];

				pc_addr_ntop(&p->to, ab, sizeof ab);
				LM_WARN("cluster: frame lane to %s:%d (TCP %d) "
					"cannot connect (%s) - frames over the "
					"datagram limit to it are dropped until "
					"it can\n", ab, pc_addr_port(&p->to),
					pc_addr_port(&p->to) + L.lane_off,
					strerror(err ? err : errno));
				p->said_down = 1;
			}
			return -1;
		}
		p->connected = 1;
		p->backoff_ms = 0;
		p->probe = 0;                  /* answered */
		p->conn_start_ms = 0;
		if (p->said_down) {
			char ab[PC_ADDR_STRLEN];

			pc_addr_ntop(&p->to, ab, sizeof ab);
			LM_NOTICE("cluster: frame lane to %s:%d is up\n", ab,
				pc_addr_port(&p->to));
			p->said_down = 0;
		}
	}
	while (p->hoff < sizeof p->hello) {
		ssize_t w = send(p->fd, p->hello + p->hoff,
			sizeof p->hello - p->hoff, MSG_NOSIGNAL);

		if (w < 0)
			return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
		p->hoff += (size_t)w;
	}
	for (;;) {
		struct frame *f;
		unsigned char lenb[4];
		struct iovec iov[2];
		struct msghdr mh;
		ssize_t w;
		size_t off;

		pthread_mutex_lock(&L.mx);
		f = p->head;
		/* never deliver a frame that has waited too long: a stale map
		 * or beat is worse than a lost one */
		while (f && !p->woff && now - f->at_ms > CLANE_MAX_AGE_MS) {
			p->head = f->next;
			if (!p->head)
				p->tail = NULL;
			p->qbytes -= f->len;
			L.f.dropped_stale++;
			free(f);
			f = p->head;
		}
		pthread_mutex_unlock(&L.mx);
		if (!f)
			return 0;
		lenb[0] = (unsigned char)(f->len >> 24);
		lenb[1] = (unsigned char)(f->len >> 16);
		lenb[2] = (unsigned char)(f->len >> 8);
		lenb[3] = (unsigned char)f->len;
		off = p->woff;
		memset(&mh, 0, sizeof mh);
		if (off < 4) {
			iov[0].iov_base = lenb + off;
			iov[0].iov_len = 4 - off;
			iov[1].iov_base = f->data;
			iov[1].iov_len = f->len;
			mh.msg_iovlen = 2;
		} else {
			iov[0].iov_base = f->data + (off - 4);
			iov[0].iov_len = f->len - (off - 4);
			mh.msg_iovlen = 1;
		}
		mh.msg_iov = iov;
		w = sendmsg(p->fd, &mh, MSG_NOSIGNAL);
		if (w < 0)
			return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
		p->woff += (size_t)w;
		if (p->woff < 4 + f->len)
			return 0;                  /* the socket is full */
		pthread_mutex_lock(&L.mx);
		p->head = f->next;
		if (!p->head)
			p->tail = NULL;
		p->qbytes -= f->len;
		p->woff = 0;
		L.f.sent_frames++;
		L.f.sent_bytes += f->len;
		pthread_mutex_unlock(&L.mx);
		free(f);
	}
}

/* fill @c->buf up to @want bytes: 1 there, 0 not yet, -1 close it */
static int lin_fill(struct lin *c, size_t want)
{
	while (c->have < want) {
		ssize_t r = recv(c->fd, c->buf + c->have, want - c->have, 0);

		if (r == 0)
			return -1;
		if (r < 0)
			return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
		c->have += (size_t)r;
	}
	return 1;
}

/* read what is there, in three phases - the hello once, then per frame
 * the 4-byte length and the body; 0 ok, -1 close it */
static int lin_read(struct lin *c)
{
	int frames;

	for (frames = 0; frames < LIN_PASS_FRAMES; ) {
		size_t len;
		int rc;

		if (!c->hello_ok) {
			pc_addr peer;
			socklen_t pl = sizeof peer;
			uint32_t a4;
			uint16_t p2;

			if ((rc = lin_fill(c, LIN_HELLO_LEN)) <= 0)
				return rc;
			/* the hello's four raw bytes against the connection's
			 * IPv4 source */
			memcpy(&a4, c->buf + 4, 4);
			memcpy(&p2, c->buf + 8, 2);
			if (memcmp(c->buf, "PCL1", 4) != 0 ||
			        getpeername(c->fd, &peer.sa, &pl) != 0 ||
			        pc_addr_family(&peer) != AF_INET ||
			        pc_addr_v4(&peer) != a4) {
				L.f.refused++;
				return -1;
			}
			pc_addr_from_v4(&c->from, a4, ntohs(p2));
			if (!(c->acct = calloc(1, sizeof *c->acct)))
				return -1;
			c->acct->refs = 1;
			{
				unsigned char *fb = realloc(c->buf, FRAME_MAX + 4);

				if (!fb)
					return -1;
				c->buf = fb;
			}
			c->hello_ok = 1;
			c->have = 0;
			c->since_ms = mono_ms();
			continue;
		}
		if ((rc = lin_fill(c, 4)) <= 0)
			return rc;
		len = ((size_t)c->buf[0] << 24) | ((size_t)c->buf[1] << 16) |
			((size_t)c->buf[2] << 8) | c->buf[3];
		if (!len || len > FRAME_MAX)
			return -1;
		if ((rc = lin_fill(c, 4 + len)) <= 0)
			return rc;
		rx_queue(c->acct, &c->from, c->buf + 4, len);
		c->have = 0;
		c->since_ms = mono_ms();
		frames++;
	}
	return 0;                              /* the rest on the next pass: poll still says so */
}

static void lin_drop(struct lin *c)
{
	close(c->fd);
	free(c->buf);
	pthread_mutex_lock(&L.mx);
	acct_put(c->acct);
	pthread_mutex_unlock(&L.mx);
}

/* inbound connections now held from @a's address */
static int lin_from(const struct lin *in, int nin, const pc_addr *a)
{
	int i, k = 0;

	for (i = 0; i < nin; i++)
		k += pc_addr_eq_host(&in[i].peer, a);
	return k;
}

static void *lane_main(void *arg)
{
	static struct lin in[CLANE_MAX_INBOUND];
	struct pollfd pf[2 + MAX_PEERS + CLANE_MAX_INBOUND];
	int pi[MAX_PEERS];                 /* pf slot -> peer index */
	int nin = 0, i;

	(void)arg;
	for (;;) {
		long long now = mono_ms();
		int np = 0, n = 0, timeout = 1000;

		pf[n].fd = L.lfd;
		pf[n++].events = POLLIN;
		pf[n].fd = L.wake_fd;
		pf[n++].events = POLLIN;
		pthread_mutex_lock(&L.mx);
		for (i = 0; i < L.npeer; i++) {
			struct lpeer *p = &L.peer[i];

			/* no lane to it: what waited too long goes here, or the queue and the
			 * reconnects would last as long as the process */
			while (p->head && !p->connected && now - p->head->at_ms > CLANE_MAX_AGE_MS) {
				struct frame *f = p->head;

				p->head = f->next;
				if (!p->head)
					p->tail = NULL;
				p->qbytes -= f->len;
				L.f.dropped_stale++;
				free(f);
			}
			p->idle = !p->head && p->fd < 0 && !p->probe && !p->forced;
			if (!p->head && p->fd < 0 && !p->probe)
				continue;
			if (p->fd < 0) {
				if (now < p->next_try_ms) {
					long long w = p->next_try_ms - now;

					if (w < timeout)
						timeout = (int)w;
					continue;
				}
				pthread_mutex_unlock(&L.mx);
				peer_connect(p, now);
				pthread_mutex_lock(&L.mx);
				if (p->fd < 0)
					continue;
			}
			pf[n].fd = p->fd;
			/* POLLIN too: a closed peer reads as EOF/HUP */
			pf[n].events = (short)(POLLIN |
				(p->head || !p->connected ||
				 p->hoff < sizeof p->hello ? POLLOUT : 0));
			pi[np++] = i;
			n++;
		}
		L.f.inbound = nin;
		pthread_mutex_unlock(&L.mx);
		for (i = 0; i < nin; i++) {
			pf[n].fd = in[i].fd;
			pf[n++].events = POLLIN;
		}
		if (poll(pf, (nfds_t)n, timeout) < 0 && errno != EINTR)
			continue;
		now = mono_ms();
		if (pf[1].revents & POLLIN) {
			uint64_t v;

			(void)!read(L.wake_fd, &v, sizeof v);
		}
		if (pf[0].revents & POLLIN) {
			int fd;
			pc_addr pa;
			socklen_t pl = sizeof pa;

			while ((fd = accept(L.lfd, &pa.sa, &pl)) >= 0) {
				fcntl(fd, F_SETFL, O_NONBLOCK);
				fcntl(fd, F_SETFD, FD_CLOEXEC);
				lane_keepalive(fd);
				if (nin >= CLANE_MAX_INBOUND ||
				        lin_from(in, nin, &pa) >= LIN_PER_ADDR) {
					L.f.refused++;     /* counted before the peer sees it */
					close(fd);
					pl = sizeof pa;
					continue;
				}
				memset(&in[nin], 0, sizeof in[nin]);
				in[nin].fd = fd;
				in[nin].peer = pa;
				in[nin].since_ms = now;
				in[nin].buf = malloc(LIN_HELLO_LEN);
				pl = sizeof pa;
				if (!in[nin].buf) {
					close(fd);
					continue;
				}
				nin++;
			}
		}
		for (i = 0; i < np; i++) {
			struct lpeer *p = &L.peer[pi[i]];
			short re = pf[2 + i].revents;
			int gone = (re & (POLLERR | POLLHUP)) != 0;

			/* nothing is ever sent back on an outbound lane, so a
			 * readable one is closing */
			if (!gone && (re & POLLIN) && p->connected) {
				char sink[64];
				ssize_t r = recv(p->fd, sink, sizeof sink, 0);

				gone = r == 0 || (r < 0 && errno != EAGAIN &&
					errno != EWOULDBLOCK);
			}
			/* a connect in flight is done only when POLLOUT says so */
			if (gone || ((p->connected || (re & POLLOUT)) &&
			        peer_write(p, now) != 0))
				peer_close(p, now);
		}
		/* backwards: an entry removed takes the LAST one's place, which
		 * this loop has already handled */
		for (i = nin - 1; i >= 0; i--) {
			short re = pf[2 + np + i].revents;

			if (!re)
				continue;
			if (((re & (POLLERR | POLLHUP)) && !(re & POLLIN)) ||
			        lin_read(&in[i]) != 0) {
				lin_drop(&in[i]);
				in[i] = in[--nin];
			}
		}
		/* a hello late, or a lane silent too long - backwards,
		 * for the same reason as above */
		for (i = nin - 1; i >= 0; i--)
			if (now - in[i].since_ms >
			        (in[i].hello_ok ? LIN_IDLE_MS : LIN_HELLO_MS)) {
				L.f.inbound_timeouts++;
				lin_drop(&in[i]);
				in[i] = in[--nin];
			}
	}
	return NULL;
}

int clane_off(void)
{
	return L.lane_off;
}

int clane_start(const pc_addr *self_cluster, int lane_off)
{
	pc_addr a = *self_cluster;
	pthread_t t;
	pthread_attr_t at;
	int one = 1;

	L.self = *self_cluster;
	L.lane_off = lane_off;
	pc_addr_set_port(&a, (uint16_t)(pc_addr_port(self_cluster) + lane_off));
	L.lfd = socket(pc_addr_family(&a), SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (L.lfd < 0)
		return -1;
	setsockopt(L.lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	if (bind(L.lfd, &a.sa, pc_addr_len(&a)) != 0 ||
	        listen(L.lfd, 64) != 0) {
		LM_ERR("cluster: cannot bind the frame lane on TCP %d (%s) - set "
			"[cluster] lane_port\n", pc_addr_port(&a),
			strerror(errno));
		close(L.lfd);
		L.lfd = -1;
		return -1;
	}
	L.wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	L.rx_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (L.wake_fd < 0 || L.rx_fd < 0)
		return -1;
	pthread_attr_init(&at);
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&t, &at, lane_main, NULL) != 0) {
		pthread_attr_destroy(&at);
		return -1;
	}
	pthread_attr_destroy(&at);
	return 0;
}
