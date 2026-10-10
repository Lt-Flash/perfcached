/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clane.c - S36 step b: the TCP frame lane.  See clane.h.
 */
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

struct frame {
	struct frame *next;
	long long at_ms;
	size_t len;
	unsigned char data[];
};

struct lpeer {
	struct sockaddr_in to;             /* the peer's CLUSTER address */
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
	int forced;                        /* S36 e2: everything by TCP */
	int probe;                         /* connect even with nothing queued */
	long long conn_start_ms;           /* the connect in flight began */
};

struct lin {
	int fd;
	struct sockaddr_in from;           /* from the hello */
	int hello_ok;
	unsigned char *buf;
	size_t have;
};

static int forced_any;                 /* S36 e2: fast path for clane_forced */

static struct {
	pthread_mutex_t mx;
	struct lpeer peer[MAX_PEERS];
	int npeer;
	int lfd, wake_fd, rx_fd;
	int lane_off;
	struct sockaddr_in self;
	/* the cluster thread's queue */
	struct frame *rx_head, *rx_tail;
	size_t rx_bytes;
	struct clane_figs f;
} L = { .mx = PTHREAD_MUTEX_INITIALIZER, .lfd = -1, .wake_fd = -1,
	.rx_fd = -1 };

/* rx frames carry their sender in front of the data */
struct rxhdr { struct sockaddr_in from; };

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

static int same(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
	return a->sin_addr.s_addr == b->sin_addr.s_addr &&
		a->sin_port == b->sin_port;
}

/* the peer for @to, made if it is new; NULL if the table is full.
 * Under L.mx. */
static struct lpeer *peer_get(const struct sockaddr_in *to)
{
	int i;

	for (i = 0; i < L.npeer; i++)
		if (same(&L.peer[i].to, to))
			return &L.peer[i];
	if (L.npeer >= MAX_PEERS)
		return NULL;
	memset(&L.peer[L.npeer], 0, sizeof L.peer[0]);
	L.peer[L.npeer].to = *to;
	L.peer[L.npeer].fd = -1;
	return &L.peer[L.npeer++];
}

int clane_tcp_state(const struct sockaddr_in *to, long long *age_ms)
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

void clane_probe(const struct sockaddr_in *to)
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

void clane_force(const struct sockaddr_in *to, int on)
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

int clane_forced(const struct sockaddr_in *to)
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

int clane_send(const struct sockaddr_in *to, const unsigned char *sealed,
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

size_t clane_rx_take(unsigned char *buf, size_t cap, struct sockaddr_in *from)
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
static void rx_queue(const struct sockaddr_in *from, const unsigned char *d,
		size_t n)
{
	struct frame *f = malloc(sizeof *f + sizeof(struct rxhdr) + n);
	int kick;

	if (!f)
		return;
	f->next = NULL;
	f->len = sizeof(struct rxhdr) + n;
	memcpy(f->data, from, sizeof(struct rxhdr));
	memcpy(f->data + sizeof(struct rxhdr), d, n);
	pthread_mutex_lock(&L.mx);
	if (L.rx_bytes + f->len > CLANE_MAX_RXQ) {
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
	L.f.recv_frames++;
	L.f.recv_bytes += n;
	pthread_mutex_unlock(&L.mx);
	if (kick)
		wake(L.rx_fd);
}

static void peer_close(struct lpeer *p, long long now)
{
	if (p->probe && !p->connected) {
		p->probe = 0;                  /* S36 e2: the probe's answer is no */
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

static void peer_connect(struct lpeer *p, long long now)
{
	struct sockaddr_in a = p->to;
	int one = 1, fd;

	a.sin_port = htons((uint16_t)(ntohs(p->to.sin_port) + L.lane_off));
	fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		peer_close(p, now);
		return;
	}
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	/* from the advertise address, as the datagrams are: the receiver
	 * checks the hello against the connection's source, and an unbound
	 * socket leaves by whatever address the route picks (127.0.0.1 on
	 * loopback, another interface on a multi-homed host) */
	{
		struct sockaddr_in src = L.self;

		src.sin_port = 0;
		if (bind(fd, (struct sockaddr *)&src, sizeof src) != 0) {
			close(fd);
			peer_close(p, now);
			return;
		}
	}
	p->fd = fd;
	p->connected = 0;
	memcpy(p->hello, "PCL1", 4);
	memcpy(p->hello + 4, &L.self.sin_addr.s_addr, 4);
	memcpy(p->hello + 8, &L.self.sin_port, 2);
	p->hoff = 0;
	if (!p->conn_start_ms)
		p->conn_start_ms = now;
	L.f.connects++;
	if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0 &&
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
				char ab[INET_ADDRSTRLEN];

				inet_ntop(AF_INET, &p->to.sin_addr, ab, sizeof ab);
				LM_WARN("cluster: frame lane to %s:%d (TCP %d) "
					"cannot connect (%s) - frames over the "
					"datagram limit to it are dropped until "
					"it can\n", ab, ntohs(p->to.sin_port),
					ntohs(p->to.sin_port) + L.lane_off,
					strerror(err ? err : errno));
				p->said_down = 1;
			}
			return -1;
		}
		p->connected = 1;
		p->backoff_ms = 0;
		p->probe = 0;                  /* S36 e2: answered */
		p->conn_start_ms = 0;
		if (p->said_down) {
			char ab[INET_ADDRSTRLEN];

			inet_ntop(AF_INET, &p->to.sin_addr, ab, sizeof ab);
			LM_NOTICE("cluster: frame lane to %s:%d is up\n", ab,
				ntohs(p->to.sin_port));
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
	for (;;) {
		size_t len;
		int rc;

		if (!c->hello_ok) {
			struct sockaddr_in peer;
			socklen_t pl = sizeof peer;

			if ((rc = lin_fill(c, 10)) <= 0)
				return rc;
			if (memcmp(c->buf, "PCL1", 4) != 0 ||
			        getpeername(c->fd, (struct sockaddr *)&peer, &pl) != 0 ||
			        memcmp(c->buf + 4, &peer.sin_addr.s_addr, 4) != 0) {
				L.f.refused++;
				return -1;
			}
			memset(&c->from, 0, sizeof c->from);
			c->from.sin_family = AF_INET;
			memcpy(&c->from.sin_addr.s_addr, c->buf + 4, 4);
			memcpy(&c->from.sin_port, c->buf + 8, 2);
			c->hello_ok = 1;
			c->have = 0;
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
		rx_queue(&c->from, c->buf + 4, len);
		c->have = 0;
	}
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

			while ((fd = accept(L.lfd, NULL, NULL)) >= 0) {
				fcntl(fd, F_SETFL, O_NONBLOCK);
				fcntl(fd, F_SETFD, FD_CLOEXEC);
				if (nin >= CLANE_MAX_INBOUND) {
					close(fd);
					L.f.refused++;
					continue;
				}
				memset(&in[nin], 0, sizeof in[nin]);
				in[nin].fd = fd;
				in[nin].buf = malloc(FRAME_MAX + 4);
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
			if (gone || peer_write(p, now) != 0)
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
				close(in[i].fd);
				free(in[i].buf);
				in[i] = in[--nin];
			}
		}
	}
	return NULL;
}

int clane_start(const struct sockaddr_in *self_cluster, int lane_off)
{
	struct sockaddr_in a = *self_cluster;
	pthread_t t;
	pthread_attr_t at;
	int one = 1;

	L.self = *self_cluster;
	L.lane_off = lane_off;
	a.sin_port = htons((uint16_t)(ntohs(self_cluster->sin_port) + lane_off));
	L.lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (L.lfd < 0)
		return -1;
	setsockopt(L.lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	if (bind(L.lfd, (struct sockaddr *)&a, sizeof a) != 0 ||
	        listen(L.lfd, 64) != 0) {
		LM_ERR("cluster: cannot bind the frame lane on TCP %d (%s) - set "
			"[cluster] lane_port\n", ntohs(a.sin_port),
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
