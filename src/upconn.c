/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* The read-through's RESP client: one connection to an upstream - connect with its
 * handshake, close, send a command, read one reply (a RESP3 push goes to on_push). */
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "pcaddr.h"                     /* pc_resolve_family */
#include "uptls.h"
#include "uprespc.h"
#include "compat/dprint.h"
#include "upstream.h"
#include "upstreamint.h"

void uc_close(struct uconn *c)
{
	if (c->fd >= 0) {
		uptls_close(c->tls);
		c->tls = NULL;
		close(c->fd);
		c->fd = -1;
		__atomic_sub_fetch(&c->s->conns_up, 1, __ATOMIC_RELAXED);
		/* the upstream forgets what a closed connection read: what it
		 * fetched is no longer tracked (the others' still is) */
		if (c->tracked && !c->inv)
			flush_shadow(c->s);
	}
	c->tracked = 0;
	c->len = c->off = 0;
}

void nap_ms(int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

long long now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* a TLS failure - counted, kept for /stats, logged unless it is the one logged last */
static void tls_fail(struct uconn *c, const char *what, const char *why)
{
	struct up_col *uc = c->uc;
	char t[sizeof uc->tls_last];
	int said;

	snprintf(t, sizeof t, "%.80s: %.110s", c->peer, why);
	__atomic_add_fetch(&uc->tls_failed, 1, __ATOMIC_RELAXED);
	pthread_mutex_lock(&uc->tls_mx);
	memcpy(uc->tls_last, t, sizeof t);
	said = !strcmp(uc->tls_said, t);
	if (!said)
		memcpy(uc->tls_said, t, sizeof t);
	pthread_mutex_unlock(&uc->tls_mx);
	if (!said)
		LM_WARN("collection '%s': upstream %s - %s: %s\n", uc->name, c->peer, what, why);
}

static int send_all(struct uconn *c, const char *p, size_t n, long long dl)
{
	short ev = POLLOUT;

	while (n) {
		struct pollfd pf = { c->fd, ev, 0 };
		long long left = dl - now_ms();
		ssize_t w;

		if (left <= 0)
			return -2;
		if (poll(&pf, 1, (int)left) <= 0)
			return -2;
		if (c->tls) {                  /* the event OpenSSL waits for, next round */
			w = uptls_write(c->tls, p, n, &ev);
			if (w == -2)
				continue;
			if (w < 0)
				return -1;
			ev = POLLOUT;
			p += w;
			n -= (size_t)w;
			continue;
		}
		w = send(c->fd, p, n, MSG_NOSIGNAL);
		if (w < 0) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			return -1;
		}
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

/* a command as a RESP array of bulks */
int send_cmd(struct uconn *c, long long dl, int argc,
		const char *const *av, const size_t *al)
{
	char hdr[32], *msg;
	size_t n = 0, i, cap = 16;
	int k, rc;

	for (k = 0; k < argc; k++)
		cap += al[k] + 32;
	msg = malloc(cap);
	if (!msg)
		return -1;
	n = (size_t)snprintf(msg, cap, "*%d\r\n", argc);
	for (k = 0; k < argc; k++) {
		int hn = snprintf(hdr, sizeof hdr, "$%zu\r\n", al[k]);

		memcpy(msg + n, hdr, (size_t)hn);
		n += (size_t)hn;
		for (i = 0; i < al[k]; i++)
			msg[n + i] = av[k][i];
		n += al[k];
		msg[n++] = '\r';
		msg[n++] = '\n';
	}
	rc = send_all(c, msg, n, dl);
	free(msg);
	return rc;
}

int read_reply_span(struct uconn *c, struct rv *v, long long dl,
		size_t *sp, size_t *ep)
{
	for (;;) {
		size_t pos = c->off;
		int rc = rv_parse(c->buf, c->len, &pos, v, 0);

		if (rc == -3)
			return -3;
		if (rc < 0)
			return -1;
		if (rc > 0) {
			if (v->t == '>') {
				c->off = pos;
				on_push(c, v);
				rv_free(v);
				continue;
			}
			if (sp)
				*sp = c->off;
			if (ep)
				*ep = pos;
			c->off = pos;
			return 0;
		}
		/* need more: compact, grow, read */
		if (c->off && c->off == c->len)
			c->off = c->len = 0;
		if (c->len == c->cap) {
			size_t nc;
			char *nb;

			if (c->off) {
				/* a parsed reply's strings point into the unread
				 * part: move it only between replies */
				memmove(c->buf, c->buf + c->off, c->len - c->off);
				c->len -= c->off;
				c->off = 0;
			}
			if (c->len == c->cap) {
				nc = c->cap ? c->cap * 2 : 65536;
				if (nc > rv_max + 65536)
					return -3;     /* the reply is larger than this
					                * thread holds */
				nb = realloc(c->buf, nc);
				if (!nb)
					return -1;
				c->buf = nb;
				c->cap = nc;
			}
		}
		if (c->tls) {                  /* decrypted bytes first, then wait */
			char why[160] = "";
			ssize_t r;
			short ev = 0;

			while ((r = uptls_read(c->tls, c->buf + c->len, c->cap - c->len, &ev, why,
			        sizeof why)) == -2) {
				struct pollfd pf = { c->fd, ev, 0 };
				long long left = dl - now_ms();

				if (left <= 0 || poll(&pf, 1, (int)left) <= 0)
					return -2;
			}
			if (r < 0 && why[0])       /* an alert: a client certificate refused, say */
				tls_fail(c, "TLS failed", why);
			if (r <= 0)
				return -1;
			c->len += (size_t)r;
			continue;
		}
		{
			struct pollfd pf = { c->fd, POLLIN, 0 };
			long long left = dl - now_ms();
			ssize_t r;

			if (left <= 0)
				return -2;
			if (poll(&pf, 1, (int)left) <= 0)
				return -2;
			r = recv(c->fd, c->buf + c->len, c->cap - c->len, 0);
			if (r == 0)
				return -1;
			if (r < 0) {
				if (errno == EAGAIN || errno == EINTR)
					continue;
				return -1;
			}
			c->len += (size_t)r;
		}
	}
}

int read_reply(struct uconn *c, struct rv *v, long long dl)
{
	return read_reply_span(c, v, dl, NULL, NULL);
}

/* a sentinel seed holds its own password: AUTH with upstream_sentinel_password;
 * 0 accepted, 1 refused, -1 the connection failed */
static int sen_auth(struct uconn *c, long long dl)
{
	const char *av[2] = { "AUTH", c->uc->spassword };
	size_t al[2] = { 4, strlen(c->uc->spassword) };
	struct rv v;
	int r;

	if (send_cmd(c, dl, 2, av, al) != 0 || read_reply(c, &v, dl) != 0)
		return -1;
	r = is_ok(&v) ? 0 : 1;
	rv_free(&v);
	return r;
}

/* the address @fd's peer is at, into @ip; its port; -1 not known */
static int fd_peer(int fd, char *ip, size_t cap, int *port)
{
	struct sockaddr_storage ss;
	socklen_t sl = sizeof ss;

	if (getpeername(fd, (struct sockaddr *)&ss, &sl) != 0)
		return -1;
	if (ss.ss_family == AF_INET) {
		struct sockaddr_in *a = (struct sockaddr_in *)&ss;

		inet_ntop(AF_INET, &a->sin_addr, ip, (socklen_t)cap);
		*port = ntohs(a->sin_port);
	} else if (ss.ss_family == AF_INET6) {
		struct sockaddr_in6 *a = (struct sockaddr_in6 *)&ss;

		if (IN6_IS_ADDR_V4MAPPED(&a->sin6_addr))
			inet_ntop(AF_INET, &a->sin6_addr.s6_addr[12], ip, (socklen_t)cap);
		else
			inet_ntop(AF_INET6, &a->sin6_addr, ip, (socklen_t)cap);
		*port = ntohs(a->sin6_port);
	} else
		return -1;
	return 0;
}

/* which server this is: INFO server's run_id (new at every server start).  An error
 * reply leaves it unknown; -1 only when the connection itself failed */
static int server_id(struct uconn *c, long long dl)
{
	const char *av[2] = { "INFO", "server" };
	size_t al[2] = { 4, 6 };
	struct rv v;
	int tried = 0;

	c->run_id[0] = 0;
again:
	if (send_cmd(c, dl, 2, av, al) != 0 || read_reply(c, &v, dl) != 0)
		return -1;
	if (v.t == '-' && v.s && v.len >= 6 && !memcmp(v.s, "NOAUTH", 6) && c->uc->spassword &&
	    !tried++) {
		rv_free(&v);                   /* a sentinel seed's own password */
		if (sen_auth(c, dl) < 0)
			return -1;
		goto again;
	}
	if ((v.t == '$' || v.t == '=') && v.s) {
		const char *p = v.s, *e = v.s + v.len;

		for (; p + 7 <= e; p++) {
			if ((p == v.s || p[-1] == '\n') && !memcmp(p, "run_id:", 7)) {
				size_t n = 0;

				p += 7;
				while (p + n < e && n < 40 && isxdigit((unsigned char)p[n]))
					n++;
				if (n == 40) {
					memcpy(c->run_id, p, 40);
					c->run_id[40] = 0;
				}
				break;
			}
		}
		/* a Redis Cluster node says so, and a sentinel too (not a data server,
		 * the topology's source) */
		__atomic_store_n(&c->s->is_cluster, 0, __ATOMIC_RELAXED);
		for (p = v.s; p + 19 <= e; p++)
			if ((p == v.s || p[-1] == '\n') && !memcmp(p, "redis_mode:", 11)) {
				__atomic_store_n(&c->s->is_cluster, !memcmp(p + 11, "cluster", 7),
					__ATOMIC_RELAXED);
				if (p + 19 <= e && !memcmp(p + 11, "sentinel", 8))
					__atomic_store_n(&c->s->is_sentinel, 1, __ATOMIC_RELAXED);
				break;
			}
		/* a perfcached says so in its INFO server */
		for (p = v.s; p + 19 <= e; p++)
			if ((p == v.s || p[-1] == '\n') &&
			    !memcmp(p, "perfcached_version:", 19)) {
				__atomic_store_n(&c->s->is_pc, 1, __ATOMIC_RELAXED);
				break;
			}
		/* a perfcached reports redis_mode:cluster when its RESP door's cluster mode is
		 * on, yet any node serves any key: it is a perfcached, not a Redis Cluster */
		if (__atomic_load_n(&c->s->is_pc, __ATOMIC_RELAXED))
			__atomic_store_n(&c->s->is_cluster, 0, __ATOMIC_RELAXED);
	}
	rv_free(&v);
	return 0;
}

/* is the server answering at the address dialled, or behind a proxy?  CLIENT INFO's
 * laddr against the peer: 1 direct, 0 a proxy, -1 not known (refused, older Redis), -2 lost */
static int proxy_check(struct uconn *c, long long dl)
{
	const char *av[2] = { "CLIENT", "INFO" };
	size_t al[2] = { 6, 4 };
	char ip[INET6_ADDRSTRLEN], la[INET6_ADDRSTRLEN + 8];
	const char *p, *e, *q;
	int port, r = -1;
	struct rv v;

	if (fd_peer(c->fd, ip, sizeof ip, &port) != 0)
		return -1;
	if (send_cmd(c, dl, 2, av, al) != 0 || read_reply(c, &v, dl) != 0)
		return -2;
	if ((v.t == '$' || v.t == '=' || v.t == '+') && v.s) {
		e = v.s + v.len;
		for (p = v.s; p + 6 <= e; p++)
			if ((p == v.s || p[-1] == ' ') && !memcmp(p, "laddr=", 6))
				break;
		if (p + 6 <= e) {
			size_t n = 0;

			for (p += 6; p + n < e && p[n] != ' ' && p[n] != '\r' && p[n] != '\n' &&
			     n < sizeof la - 1; n++)
				;
			memcpy(la, p, n);
			la[n] = 0;
			q = strrchr(la, ':');
			if (q) {
				char *h = la;

				la[q - la] = 0;
				if (*h == '[') {
					h++;
					h[strcspn(h, "]")] = 0;
				}
				r = !strcmp(h, ip) && atoi(q + 1) == port;
			}
		}
	}
	rv_free(&v);
	return r;
}

/* this connection's server failed to connect, or answered: the
 * upstream's health, for pick() */
static void srv_reach(struct up_srv *s, int ok)
{
	__atomic_store_n(&s->failing, ok ? 0 : 1, __ATOMIC_RELAXED);
}

/* the number after "<name>:" at a line's start in the INFO text
 * [@b, @e), or -1 - read within it (the text is not terminated) */
static long long info_num(const char *b, const char *e, const char *name)
{
	size_t nl = strlen(name);
	const char *p;

	for (p = b; p + nl < e; p++)
		if ((p == b || p[-1] == '\n') && !memcmp(p, name, nl) &&
		    p[nl] == ':') {
			long long n = 0;
			int any = 0;

			for (p += nl + 1; p < e && *p >= '0' && *p <= '9'; p++) {
				if (n < 100000000000LL)
					n = n * 10 + (*p - '0');
				any = 1;
			}
			return any ? n : -1;
		}
	return -1;
}

/* ROLE, and for a replica INFO replication's link state and lag (whole seconds, held to
 * upstream_max_lag_ms); health changes are logged.  0 asked, -1 the connection failed */
int role_check(struct uconn *c, long long dl)
{
	struct up_srv *s = c->s;
	struct up_col *uc = c->uc;
	const char *av[2] = { "ROLE", NULL }, *iv[2] = { "INFO", "replication" };
	size_t al[2] = { 4, 0 }, il[2] = { 4, 11 };
	struct rv v;
	int role = UPR_OTHER, link_down = 0, before = srv_state(s), now;
	long long lag = -1;
	char rh[PC_UP_MAX_SRV][64];        /* the replicas a master names */
	int rp[PC_UP_MAX_SRV], nr = 0, i;

	if (send_cmd(c, dl, 1, av, al) != 0 || read_reply(c, &v, dl) != 0)
		return -1;
	if ((v.t == '*' || v.t == '~') && v.nel >= 1 && v.el[0].s) {
		if (v.el[0].len == 6 && !memcmp(v.el[0].s, "master", 6))
			role = UPR_MASTER;
		else if (v.el[0].len == 5 && !memcmp(v.el[0].s, "slave", 5))
			role = UPR_REPLICA;
		/* ["master", offset, [[ip, port, offset], ...]] */
		if (role == UPR_MASTER && v.nel >= 3 && v.el[2].t == '*')
			for (i = 0; i < v.el[2].nel && nr < PC_UP_MAX_SRV; i++) {
				const struct rv *r = &v.el[2].el[i];

				if (r->nel < 2 || !r->el[0].s || !r->el[0].len ||
				    r->el[0].len >= sizeof rh[0])
					continue;
				memcpy(rh[nr], r->el[0].s, r->el[0].len);
				rh[nr][r->el[0].len] = 0;
				rp[nr] = r->el[1].s ? atoi(r->el[1].s) : (int)r->el[1].n;
				if (rp[nr] > 0 && rp[nr] < 65536)
					nr++;
			}
	}
	rv_free(&v);
	if (role == UPR_MASTER)
		srv_discovered(s, rh, rp, nr);
	if (role == UPR_REPLICA) {
		if (send_cmd(c, dl, 2, iv, il) != 0 || read_reply(c, &v, dl) != 0)
			return -1;
		if ((v.t == '$' || v.t == '=') && v.s) {
			const char *p = v.s, *e = v.s + v.len, *q;
			long long io;

			for (q = p; q + 24 <= e; q++)
				if ((q == p || q[-1] == '\n') &&
				    !memcmp(q, "master_link_status:down", 23)) {
					link_down = 1;
					break;
				}
			io = info_num(p, e, "master_last_io_seconds_ago");
			lag = io >= 0 ? io * 1000 : -1;
			/* which master it follows - the writer's guard
			 * when two upstreams say master */
			for (q = p; q + 12 <= e; q++)
				if ((q == p || q[-1] == '\n') &&
				    !memcmp(q, "master_host:", 12)) {
					size_t n = 0;

					q += 12;
					while (q + n < e && q[n] != '\r' && q[n] != '\n' &&
					       n < sizeof s->mhost - 1)
						n++;
					pthread_mutex_lock(&s->run_mx);
					memcpy(s->mhost, q, n);
					s->mhost[n] = 0;
					pthread_mutex_unlock(&s->run_mx);
					break;
				}
			io = info_num(p, e, "master_port");
			__atomic_store_n(&s->mport, io > 0 ? (int)io : 0,
				__ATOMIC_RELAXED);
		}
		rv_free(&v);
	}
	__atomic_store_n(&s->role, role, __ATOMIC_RELAXED);
	__atomic_store_n(&s->link_down, link_down, __ATOMIC_RELAXED);
	__atomic_store_n(&s->lag_ms, lag, __ATOMIC_RELAXED);
	__atomic_store_n(&s->checked_ms, now_ms(), __ATOMIC_RELAXED);
	now = srv_state(s);
	if (now == PC_UPS_OFF && before != PC_UPS_OFF) {
		if (role == UPR_MASTER)        /* by design: listed to follow failover */
			LM_NOTICE("collection '%s': upstream %s is a master - not used "
				"(upstream_role = replica)\n", uc->name, s->addr);
		else if (role != UPR_REPLICA)
			LM_WARN("collection '%s': upstream %s does not say it is a "
				"replica (ROLE) - not used (upstream_role = replica)\n",
				uc->name, s->addr);
		else if (link_down)
			LM_WARN("collection '%s': upstream %s has lost its link to "
				"its master - skipped until it is back\n", uc->name,
				s->addr);
		else
			LM_WARN("collection '%s': upstream %s last heard from its "
				"master %lld ms ago, more than upstream_max_lag_ms "
				"(%d) - skipped until it catches up\n", uc->name,
				s->addr, lag, uc->max_lag_ms);
		s->refused_said = 1;
	} else if (now != PC_UPS_OFF && s->refused_said) {
		LM_NOTICE("collection '%s': upstream %s is %s again - taken "
			"back\n", uc->name, s->addr, role == UPR_MASTER ?
			"usable as the master fallback" : "a replica in step");
		s->refused_said = 0;
	}
	return 0;
}

/* one address of a name: connected by the deadline, or -1 */
static int dial_one(const struct addrinfo *ai, long long dl)
{
	int fd, rc;

	fd = socket(ai->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
	if (rc < 0 && errno != EINPROGRESS) {
		close(fd);
		return -1;
	}
	if (rc < 0) {
		struct pollfd pf = { fd, POLLOUT, 0 };
		long long left = dl - now_ms();
		int err = 0;
		socklen_t el = sizeof err;

		if (left <= 0 || poll(&pf, 1, (int)left) <= 0 ||
		    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err) {
			close(fd);
			return -1;
		}
	}
	return fd;
}

/* a TCP connection to @host:@port by the deadline, non-blocking, no delay; -1 none.  Each
 * address the name gives, in turn (localhost is ::1 and 127.0.0.1; a server may hold one) */
int uc_dial(const char *host, int port, long long dl)
{
	struct addrinfo hints, *ai = NULL, *a;
	char ps[16];
	int fd = -1, one = 1;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = pc_resolve_family();     /* no AAAA without IPv6 */
	hints.ai_socktype = SOCK_STREAM;
	snprintf(ps, sizeof ps, "%d", port);
	if (getaddrinfo(host, ps, &hints, &ai) != 0 || !ai)
		return -1;
	for (a = ai; a && fd < 0; a = a->ai_next)
		fd = dial_one(a, dl);
	freeaddrinfo(ai);
	if (fd >= 0)
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	return fd;
}

int uc_open(struct uconn *c, const char *host, int port, long long dl)
{
	struct up_col *uc = c->uc;
	char why[160];
	int fd = uc_dial(host, port, dl);

	c->tls = NULL;
	if (fd < 0)
		return -1;
	snprintf(c->peer, sizeof c->peer, strchr(host, ':') ? "[%.64s]:%d" : "%.64s:%d", host, port);
	if (uc->tls) {
		size_t pl = strlen(c->peer);

		c->tls = uptls_connect(uc->tls, fd, uc->tls_name ? uc->tls_name : host, dl, why,
			sizeof why);
		if (!c->tls) {
			tls_fail(c, "TLS handshake failed", why);
			close(fd);
			return -1;
		}
		__atomic_add_fetch(&uc->tls_hs, 1, __ATOMIC_RELAXED);
		pthread_mutex_lock(&uc->tls_mx);
		if (!strncmp(uc->tls_said, c->peer, pl) && uc->tls_said[pl] == ':')
			uc->tls_said[0] = 0;       /* this one up: its next failure is said again */
		pthread_mutex_unlock(&uc->tls_mx);
	}
	c->fd = fd;
	return 0;
}

void uc_drop(struct uconn *c)
{
	uptls_close(c->tls);
	c->tls = NULL;
	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
}

int uc_buffered(struct uconn *c)
{
	return c->off < c->len || (c->tls && uptls_pending(c->tls) > 0);
}

int uc_connect(struct uconn *c, long long dl)
{
	struct up_col *uc = c->uc;
	struct up_srv *s = c->s;
	struct rv v;
	char host[sizeof s->host];
	int port;

	/* a reused slot takes its address under run_mx, the incarnation after it */
	c->incarn = __atomic_load_n(&s->incarn, __ATOMIC_ACQUIRE);
	pthread_mutex_lock(&s->run_mx);
	memcpy(host, s->host, sizeof host);
	port = s->port;
	pthread_mutex_unlock(&s->run_mx);
	if (uc_open(c, host, port, dl) != 0) {
		srv_reach(s, 0);
		return -1;
	}
	c->len = c->off = 0;
	c->resp3 = 0;
	{
		char ip[INET6_ADDRSTRLEN];
		int pp;

		if (fd_peer(c->fd, ip, sizeof ip, &pp) == 0) {     /* srv_add's alias */
			pthread_mutex_lock(&s->run_mx);
			snprintf(s->peer_ip, sizeof s->peer_ip, "%s", ip);
			pthread_mutex_unlock(&s->run_mx);
		}
	}
	__atomic_add_fetch(&s->conns_up, 1, __ATOMIC_RELAXED);
	ST_ADD(uc->connects, 1);
	ST_ADD(s->connects, 1);

	/* RESP3 with the credentials in one step, where the server has it; not for the
	 * writer and pass reader, so a RESP2 client gets Redis's answer byte for byte */
	{
		const char *av[5];
		size_t al[5];
		int n = 0;

		av[n] = "HELLO"; al[n++] = 5;
		av[n] = "3"; al[n++] = 1;
		if (uc->password) {
			av[n] = "AUTH"; al[n++] = 4;
			av[n] = s->user[0] ? s->user : "default";
			al[n] = strlen(av[n]);
			n++;
			av[n] = uc->password; al[n] = strlen(uc->password); n++;
		}
		if (c->wr) {
			v.t = 0;                   /* straight to RESP2's AUTH */
		} else if (send_cmd(c, dl, n, av, al) != 0 ||
		    read_reply(c, &v, dl) != 0)
			goto fail;
		if (v.t == '%') {
			c->resp3 = 1;              /* RESP3, credentials accepted */
			rv_free(&v);
		} else {
			/* the writer's own credential, where it has one -
			 * readers never hold the write rights */
			int own = c->wcred && uc->wpassword;
			const char *pw = own ? uc->wpassword : uc->password;
			const char *us = own && uc->wuser ? uc->wuser :
				s->user[0] ? s->user : NULL;

			/* no HELLO 3 here (an older server, or a perfcached
			 * RESP door) - or the writer's: RESP2, AUTH on its own */
			if (!c->wr)
				rv_free(&v);
			if (pw) {
				const char *a2[3];
				size_t l2[3];
				int m = 0;

				a2[m] = "AUTH"; l2[m++] = 4;
				if (us) {
					a2[m] = us;
					l2[m] = strlen(us);
					m++;
				}
				a2[m] = pw;
				l2[m] = strlen(pw);
				m++;
				if (send_cmd(c, dl, m, a2, l2) != 0 ||
				    read_reply(c, &v, dl) != 0)
					goto fail;
				if (!is_ok(&v)) {
					int r;

					rv_free(&v);       /* a sentinel seed's own password */
					r = uc->spassword ? sen_auth(c, dl) : 1;
					if (r < 0)
						goto fail;
					if (r) {
						LM_ERR("upstream %s: AUTH refused by %s%s\n",
							uc->name, s->addr, own ? " (the "
							"writer's upstream_write_password)" : "");
						goto fail;
					}
				} else
					rv_free(&v);
			}
		}
	}
	if (s->db) {
		char dbs[16];
		const char *av[2] = { "SELECT", dbs };
		size_t al[2];

		al[0] = 6;
		al[1] = (size_t)snprintf(dbs, sizeof dbs, "%d", s->db);
		if (send_cmd(c, dl, 2, av, al) != 0 || read_reply(c, &v, dl) != 0)
			goto fail;
		if (!is_ok(&v)) {
			/* a Redis Cluster has database 0 alone */
			char et[128];
			int cl;

			snprintf(et, sizeof et, "%.*s", v.t == '-' && v.s ? (int)(v.len < 127 ?
				v.len : 127) : 0, v.s ? v.s : "");
			cl = strstr(et, "cluster") != NULL;
			rv_free(&v);
			LM_ERR("upstream %s: SELECT %d refused by %s%s\n", uc->name,
				s->db, s->addr, cl ? " - a Redis Cluster has database 0 "
				"only: name it without /N" : "");
			goto fail;
		}
		rv_free(&v);
	}
	srv_reach(s, 1);
	/* a perfcached upstream is refused unless upstream_allow_perfcached; allowed,
	 * the connection names itself so that node never reads through for it */
	if (server_id(c, dl) != 0)
		goto fail;
	if (uc->discover != PC_UP_DISC_NO) {
		int d = proxy_check(c, dl);

		if (d == -2)
			goto fail;
		__atomic_store_n(&s->direct, d, __ATOMIC_RELAXED);
	}
	if (__atomic_load_n(&s->is_sentinel, __ATOMIC_RELAXED)) {
		if (!__atomic_exchange_n(&s->cluster_said, 1, __ATOMIC_RELAXED))
			LM_NOTICE("collection '%s': upstream %s is a sentinel - the "
				"servers it names are the collection's upstreams\n",
				uc->name, s->addr);
		__atomic_store_n(&s->checked_ms, now_ms(), __ATOMIC_RELAXED);
		uc_close(c);
		sen_start(uc);
		return -1;
	}
	if (__atomic_load_n(&s->is_cluster, __ATOMIC_RELAXED)) {
		if (!__atomic_exchange_n(&s->cluster_said, 1, __ATOMIC_RELAXED) &&
		    !__atomic_load_n(&uc->cluster, __ATOMIC_RELAXED))
			LM_NOTICE("collection '%s': upstream %s is a Redis Cluster node - "
				"its nodes and slots are read from it, and each key goes to "
				"its slot's shard\n", uc->name, s->addr);
		cl_start(uc);
		if (!c->wr) {
			/* a replica serves its slots' reads only after READONLY */
			const char *av[1] = { "READONLY" };
			size_t al[1] = { 8 };

			if (send_cmd(c, dl, 1, av, al) != 0 || read_reply(c, &v, dl) != 0)
				goto fail;
			rv_free(&v);
		}
	}
	if (__atomic_load_n(&s->is_pc, __ATOMIC_RELAXED)) {
		if (!uc->allow_pc) {
			if (!__atomic_exchange_n(&s->pc_said, 1, __ATOMIC_RELAXED))
				LM_WARN("collection '%s': upstream %s is a perfcached "
					"node - not used: read-through from perfcached "
					"is off (upstream_allow_perfcached = no), as a "
					"ring of them is a fetch storm\n", uc->name,
					s->addr);
			__atomic_store_n(&s->checked_ms, now_ms(), __ATOMIC_RELAXED);
			uc_close(c);
			return -1;
		} else {
			const char *av[3] = { "CLIENT", "SETNAME",
				"perfcached-readthrough" };
			size_t al[3] = { 6, 7, 22 };

			if (send_cmd(c, dl, 3, av, al) != 0 ||
			    read_reply(c, &v, dl) != 0)
				goto fail;
			rv_free(&v);
		}
	}
	/* what the server IS, before any fetch: a master where a replica is asked for
	 * is closed here, unless the fallback lets it serve while no replica does */
	if (c->wr) {
		/* the writer must know what it reached - it keeps a master
		 * whatever upstream_role says (reads never go there) */
		if (role_check(c, dl) != 0)
			goto fail;
	} else if (uc->role_req == PC_UP_ROLE_REPLICA) {
		if (role_check(c, dl) != 0)
			goto fail;
		if (srv_state(s) == PC_UPS_OFF && !c->redir) {
			uc_close(c);
			return -1;
		}
	} else
		__atomic_store_n(&s->checked_ms, now_ms(), __ATOMIC_RELAXED);
	/* tracked, redirected to the invalidation connection; the generation is read
	 * FIRST so if that one moves meanwhile, the next fetch sees it and re-arms */
	c->tracked = 0;
	if (!c->inv && !c->notrack && !c->wr && uc->tracking && c->resp3) {
		unsigned int gen = __atomic_load_n(&s->inv_gen, __ATOMIC_ACQUIRE);
		long long id = __atomic_load_n(&s->inv_id, __ATOMIC_ACQUIRE);
		char inv_run[41];

		c->gen = gen;
		pthread_mutex_lock(&s->run_mx);
		memcpy(inv_run, s->inv_run, sizeof inv_run);
		pthread_mutex_unlock(&s->run_mx);
		if (id > 0 && strcmp(c->run_id, inv_run) != 0) {
			/* another server than the invalidation connection's: the id is
			 * not ours there, so this one stays untracked */
			ST_ADD(s->other_server, 1);
			if (!__atomic_exchange_n(&s->other_said, 1, __ATOMIC_RELAXED))
				LM_WARN("collection '%s': a connection to the upstream "
					"reached another server (run_id %.12s) than the "
					"tracking connection (%.12s) - a round-robin "
					"proxy?  Its fetches are not tracked: a copy "
					"lives at most upstream_negative_ms (%d ms).  "
					"Point upstream at one server, or a backend "
					"that keeps a client on one (balance source)\n",
					uc->name, c->run_id[0] ? c->run_id : "unknown",
					inv_run, uc->neg_ms);
			id = 0;
		}
		if (id > 0) {
			char ids[24];
			const char *av[5] = { "CLIENT", "TRACKING", "ON", "REDIRECT",
				ids };
			size_t al[5] = { 6, 8, 2, 8, 0 };

			al[4] = (size_t)snprintf(ids, sizeof ids, "%lld", id);
			if (send_cmd(c, dl, 5, av, al) != 0 ||
			    read_reply(c, &v, dl) != 0)
				goto fail;
			c->tracked = is_ok(&v);
			rv_free(&v);
		}
	}
	c->backoff_until = 0;
	return 0;
fail:
	uc_close(c);
	srv_reach(s, 0);                   /* refused us: not usable either */
	return -1;
}
