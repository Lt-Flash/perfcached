/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clrpt.c - S469: replication groups over TCP to the relay port.  See clrpt.h.
 * Wire: [len 4, big-endian][sealed frame], as on clane's lane. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include "clwire.h"                    /* HDR_LEN, MAX_DGRAM */
#include "clrpt.h"

#define FRAME_MAX (HDR_LEN + MAX_DGRAM + 64)

static unsigned long long sent_frames, sent_bytes, fallbacks, connects,
	connect_fail, lost_frames, rx_frames, rx_bytes, rx_accepts, rx_refused;
static size_t qmax = CLRPT_QMAX;
static pc_addr src;
static int have_src;

#define BUMP(c, n) __atomic_add_fetch(&(c), (n), __ATOMIC_RELAXED)

static long long mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void clrpt_set_qmax(size_t q)
{
	qmax = q;
}

void clrpt_set_source(const pc_addr *s)
{
	have_src = s != NULL;
	if (s)
		src = *s;
}

/* ---- sending ---- */
struct tx {
	pc_addr to;
	int fd, up;
	unsigned char *q;
	size_t qlen, qoff, qcap;
	long long retry_ms, sent_ms;
};
static size_t q_total;                 /* OM2-12: queue capacity held, every thread's */
static __thread struct tx tx[CLRPT_PEERS];
static __thread int ntx;

static struct tx *tx_for(const pc_addr *to)
{
	int i;

	for (i = 0; i < ntx; i++)
		if (pc_addr_eq(&tx[i].to, to))
			return &tx[i];
	if (ntx >= CLRPT_PEERS)
		return NULL;
	memset(&tx[ntx], 0, sizeof tx[ntx]);
	tx[ntx].to = *to;
	tx[ntx].fd = -1;
	return &tx[ntx++];
}

/* frames queued and not wholly written: what a broken connection loses */
static unsigned long long unsent(const struct tx *t)
{
	size_t o = 0, n = 0;

	while (o + 4 <= t->qlen) {
		size_t fl = (size_t)t->q[o] << 24 | (size_t)t->q[o + 1] << 16 |
			(size_t)t->q[o + 2] << 8 | t->q[o + 3];

		o += 4 + fl;
		if (o > t->qoff)
			n++;
	}
	return n;
}

static void tx_close(struct tx *t)
{
	if (t->fd >= 0)
		close(t->fd);
	BUMP(lost_frames, unsent(t));
	t->fd = -1;
	t->up = 0;
	t->qlen = t->qoff = 0;
	t->retry_ms = mono_ms() + CLRPT_RETRY_MS;
}

static int tx_connect(struct tx *t)
{
	int one = 1;

	t->fd = socket(pc_addr_family(&t->to), SOCK_STREAM | SOCK_NONBLOCK |
		SOCK_CLOEXEC, 0);
	if (t->fd < 0)
		goto fail;
	setsockopt(t->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	if (have_src && pc_addr_family(&src) == pc_addr_family(&t->to)) {
		pc_addr s = src;

		pc_addr_set_port(&s, 0);
		if (bind(t->fd, &s.sa, pc_addr_len(&s)) != 0)
			goto fail;
	}
	BUMP(connects, 1);
	if (connect(t->fd, &t->to.sa, pc_addr_len(&t->to)) == 0) {
		t->up = 1;
		return 0;
	}
	if (errno == EINPROGRESS)
		return 0;
fail:
	BUMP(connect_fail, 1);
	tx_close(t);
	return -1;
}

/* a connect in flight: finished yet? */
static void tx_check(struct tx *t)
{
	struct pollfd pf;
	int err = 0;
	socklen_t el = sizeof err;

	if (t->fd < 0 || t->up)
		return;
	pf.fd = t->fd;
	pf.events = POLLOUT;
	if (poll(&pf, 1, 0) != 1)
		return;
	if (getsockopt(t->fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0) {
		t->up = 1;
		return;
	}
	BUMP(connect_fail, 1);
	tx_close(t);
}

static void tx_write(struct tx *t)
{
	while (t->up && t->qoff < t->qlen) {
		ssize_t r = send(t->fd, t->q + t->qoff, t->qlen - t->qoff,
			MSG_NOSIGNAL | MSG_DONTWAIT);

		if (r > 0) {
			t->qoff += (size_t)r;
			continue;
		}
		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return;
		tx_close(t);
		return;
	}
	if (t->qoff == t->qlen)
		t->qlen = t->qoff = 0;
}

int clrpt_send(const pc_addr *relay, const unsigned char *f, size_t n)
{
	struct tx *t = tx_for(relay);
	size_t need;

	if (!t || n == 0 || n > FRAME_MAX)
		goto fallback;
	if (t->fd < 0 && (mono_ms() < t->retry_ms || tx_connect(t) != 0))
		goto fallback;
	tx_check(t);
	if (t->fd < 0)
		goto fallback;
	need = t->qlen - t->qoff + 4 + n;
	if (need > qmax)
		goto fallback;
	if (t->qoff && t->qlen + 4 + n > t->qcap) {
		memmove(t->q, t->q + t->qoff, t->qlen - t->qoff);
		t->qlen -= t->qoff;
		t->qoff = 0;
	}
	if (t->qlen + 4 + n > t->qcap) {
		size_t cap = t->qcap ? t->qcap : 65536;
		unsigned char *nq;

		while (cap < t->qlen + 4 + n)
			cap *= 2;
		if (__atomic_add_fetch(&q_total, cap - t->qcap, __ATOMIC_RELAXED) >
		        CLRPT_QTOTAL || !(nq = realloc(t->q, cap))) {
			__atomic_sub_fetch(&q_total, cap - t->qcap, __ATOMIC_RELAXED);
			goto fallback;
		}
		t->q = nq;
		t->qcap = cap;
	}
	t->q[t->qlen] = (unsigned char)(n >> 24);
	t->q[t->qlen + 1] = (unsigned char)(n >> 16);
	t->q[t->qlen + 2] = (unsigned char)(n >> 8);
	t->q[t->qlen + 3] = (unsigned char)n;
	memcpy(t->q + t->qlen + 4, f, n);
	t->qlen += 4 + n;
	BUMP(sent_frames, 1);
	BUMP(sent_bytes, n);
	t->sent_ms = mono_ms();
	tx_write(t);
	return 0;
fallback:
	BUMP(fallbacks, 1);
	return -1;
}

void clrpt_flush(void)
{
	long long now = ntx ? mono_ms() : 0;
	int i;

	for (i = 0; i < ntx; i++) {
		struct tx *t = &tx[i];

		tx_check(t);
		tx_write(t);
		/* OM2-12: a drained queue a second quiet gives its memory back */
		if (!t->qlen && t->qcap > 65536 && now - t->sent_ms > 1000) {
			free(t->q);
			t->q = NULL;
			__atomic_sub_fetch(&q_total, t->qcap, __ATOMIC_RELAXED);
			t->qcap = 0;
		}
	}
}

/* ---- receiving ---- */
int clrpt_listen(const pc_addr *at)
{
	int one = 1, fd = socket(pc_addr_family(at), SOCK_STREAM | SOCK_NONBLOCK |
		SOCK_CLOEXEC, 0);

	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
	if (bind(fd, &at->sa, pc_addr_len(at)) != 0 || listen(fd, 128) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

#define RX_FIRST 4096                  /* a buffer starts here and grows to its frame */

struct rconn {
	int fd;
	pc_addr from;
	unsigned char *buf;
	size_t len, cap;
	long long last_ms, t0;             /* t0: when the frame in buf began */
};

struct clrpt_rx {
	int lfd, n;
	clrpt_host_fn ok;
	struct rconn c[CLRPT_RX_CONNS];
};

struct clrpt_rx *clrpt_rx_new(int lfd, clrpt_host_fn host_ok)
{
	struct clrpt_rx *r = calloc(1, sizeof *r);

	if (r) {
		r->lfd = lfd;
		r->ok = host_ok;
	}
	return r;
}

static void rx_drop(struct rconn *c)
{
	close(c->fd);
	free(c->buf);
	c->fd = -1;
	c->buf = NULL;
	c->len = c->cap = 0;
}

static size_t rx_flen(const unsigned char *b)
{
	return (size_t)b[0] << 24 | (size_t)b[1] << 16 | (size_t)b[2] << 8 | b[3];
}

/* room for the frame being assembled: RX_FIRST, or the whole frame its
 * header names; -1 for a length past FRAME_MAX */
static int rx_room(struct rconn *c)
{
	size_t want = RX_FIRST;
	unsigned char *nb;

	if (c->len >= 4) {
		size_t fl = rx_flen(c->buf);

		if (fl == 0 || fl > FRAME_MAX) {
			BUMP(rx_refused, 1);
			return -1;
		}
		if (4 + fl > want)
			want = 4 + fl;
	}
	if (c->cap >= want)
		return 0;
	if (!(nb = realloc(c->buf, want)))
		return -1;
	c->buf = nb;
	c->cap = want;
	return 0;
}

void clrpt_rx_free(struct clrpt_rx *r)
{
	int i;

	if (!r)
		return;
	for (i = 0; i < r->n; i++)
		if (r->c[i].fd >= 0)
			rx_drop(&r->c[i]);
	if (r->lfd >= 0)
		close(r->lfd);
	free(r);
}

int clrpt_rx_pollset(struct clrpt_rx *r, struct pollfd *pf, int max)
{
	int i, k = 0;

	if (!r || max < 1)
		return 0;
	pf[k].fd = r->lfd;
	pf[k++].events = POLLIN;
	for (i = 0; i < r->n && k < max; i++) {
		pf[k].fd = r->c[i].fd;
		pf[k++].events = POLLIN;
	}
	return k;
}

/* a connection's input: every whole frame to @fn; 0 = keep it, -1 = drop it */
static int rx_read(struct rconn *c, clrpt_frame_fn fn, void *ctx, long long now)
{
	int reads;

	for (reads = 0; reads < 16; reads++) {
		ssize_t got;
		size_t o = 0;

		if (rx_room(c) != 0)
			return -1;
		got = recv(c->fd, c->buf + c->len, c->cap - c->len, MSG_DONTWAIT);
		if (got == 0)
			return -1;
		if (got < 0)
			return errno == EAGAIN || errno == EWOULDBLOCK ||
				errno == EINTR ? 0 : -1;
		if (!c->len)
			c->t0 = now;
		c->len += (size_t)got;
		while (c->len - o >= 4) {
			size_t fl = rx_flen(c->buf + o);

			if (fl == 0 || fl > FRAME_MAX) {
				BUMP(rx_refused, 1);
				return -1;
			}
			if (c->len - o < 4 + fl)
				break;
			fn(ctx, c->buf + o + 4, fl, &c->from);
			BUMP(rx_frames, 1);
			BUMP(rx_bytes, fl);
			o += 4 + fl;
		}
		if (o) {
			memmove(c->buf, c->buf + o, c->len - o);
			c->len -= o;
			c->t0 = now;
		}
	}
	return 0;
}

void clrpt_rx_serve(struct clrpt_rx *r, const struct pollfd *pf, int n,
		long long now, clrpt_frame_fn fn, void *ctx)
{
	int i, j;

	if (!r || n < 1)
		return;
	for (i = 1; i < n; i++) {
		struct rconn *c = &r->c[i - 1];

		if (pf[i].fd != c->fd || c->fd < 0)
			continue;
		if (pf[i].revents & (POLLIN | POLLHUP | POLLERR)) {
			c->last_ms = now;
			if (rx_read(c, fn, ctx, now) != 0)
				rx_drop(c);
		} else if (now - c->last_ms > CLRPT_IDLE_MS)
			rx_drop(c);
		if (c->fd >= 0 && c->len && now - c->t0 > CLRPT_FRAME_MS) {
			BUMP(rx_refused, 1);       /* OM2-11: a trickled frame */
			rx_drop(c);
		}
	}
	if (pf[0].revents & POLLIN)
		for (;;) {
			pc_addr a;
			socklen_t al = sizeof a;
			int fd = accept(r->lfd, &a.sa, &al);

			if (fd < 0)
				break;
			if (r->ok && !r->ok(&a)) {     /* OM2-11: members' hosts only */
				BUMP(rx_refused, 1);
				close(fd);
				continue;
			}
			if (fcntl(fd, F_SETFL, O_NONBLOCK) != 0 ||
			    fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
				close(fd);
				continue;
			}
			BUMP(rx_accepts, 1);
			for (j = 0; j < r->n && r->c[j].fd >= 0; j++)
				;
			if (j == CLRPT_RX_CONNS) {
				BUMP(rx_refused, 1);
				close(fd);
				continue;
			}
			memset(&r->c[j], 0, sizeof r->c[j]);
			r->c[j].fd = fd;
			r->c[j].from = a;
			r->c[j].last_ms = now;
			if (j == r->n)
				r->n++;
		}
	while (r->n > 0 && r->c[r->n - 1].fd < 0)
		r->n--;
}

void clrpt_figures(struct clrpt_figs *f)
{
	f->sent_frames = __atomic_load_n(&sent_frames, __ATOMIC_RELAXED);
	f->sent_bytes = __atomic_load_n(&sent_bytes, __ATOMIC_RELAXED);
	f->fallbacks = __atomic_load_n(&fallbacks, __ATOMIC_RELAXED);
	f->connects = __atomic_load_n(&connects, __ATOMIC_RELAXED);
	f->connect_fail = __atomic_load_n(&connect_fail, __ATOMIC_RELAXED);
	f->lost_frames = __atomic_load_n(&lost_frames, __ATOMIC_RELAXED);
	f->rx_frames = __atomic_load_n(&rx_frames, __ATOMIC_RELAXED);
	f->rx_bytes = __atomic_load_n(&rx_bytes, __ATOMIC_RELAXED);
	f->rx_accepts = __atomic_load_n(&rx_accepts, __ATOMIC_RELAXED);
	f->rx_refused = __atomic_load_n(&rx_refused, __ATOMIC_RELAXED);
}
