/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * upconn.c - the read-through's RESP client: one connection to an
 * upstream - connect with its handshake (HELLO 3 with AUTH, or RESP2's
 * AUTH; SELECT; INFO server for the run_id and a perfcached; ROLE for what
 * the server is; CLIENT TRACKING ... REDIRECT), close, send a command,
 * read one reply.  A RESP3 push read on the way is upstream.c's (on_push).
 * Moved from upstream.c unchanged (the 0.7.2 split, wave 1 -
 * doc/MODULARITY.md).
 */
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
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
#include "uprespc.h"
#include "compat/dprint.h"
#include "upstream.h"
#include "upstreamint.h"

void uc_close(struct uconn *c)
{
	if (c->fd >= 0) {
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

static int send_all(struct uconn *c, const char *p, size_t n, long long dl)
{
	while (n) {
		struct pollfd pf = { c->fd, POLLOUT, 0 };
		long long left = dl - now_ms();
		ssize_t w;

		if (left <= 0)
			return -2;
		if (poll(&pf, 1, (int)left) <= 0)
			return -2;
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
				/* the unread part stays where the strings of a
				 * reply being parsed point - only move it when
				 * nothing parsed is held (we are between replies) */
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

/* which server this connection reached: the run_id in INFO server (40 hex
 * characters, a new one every time a server process starts).  An error
 * reply (the user may not run INFO) leaves it unknown; -1 only when the
 * connection itself failed */
static int server_id(struct uconn *c, long long dl)
{
	const char *av[2] = { "INFO", "server" };
	size_t al[2] = { 4, 6 };
	struct rv v;

	c->run_id[0] = 0;
	if (send_cmd(c, dl, 2, av, al) != 0 || read_reply(c, &v, dl) != 0)
		return -1;
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
		/* S384: a perfcached says so in its INFO server */
		for (p = v.s; p + 19 <= e; p++)
			if ((p == v.s || p[-1] == '\n') &&
			    !memcmp(p, "perfcached_version:", 19)) {
				__atomic_store_n(&c->s->is_pc, 1, __ATOMIC_RELAXED);
				break;
			}
	}
	rv_free(&v);
	return 0;
}

/* S382: this connection's server failed to connect, or answered: the
 * upstream's health, for pick() */
static void srv_reach(struct up_srv *s, int ok)
{
	__atomic_store_n(&s->failing, ok ? 0 : 1, __ATOMIC_RELAXED);
}

/* S382: the number after "<name>:" at a line's start in the INFO text
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

/* S382: what the server is - ROLE says master, replica (Redis: "slave")
 * or something else; for a replica, INFO replication says whether its link
 * to the master is up and how long since it last heard from it (whole
 * seconds: that is the lag upstream_max_lag_ms is held to).  The upstream's
 * health from it, logged when that changes what it may do.  0 asked
 * (whatever it said), -1 the connection failed */
int role_check(struct uconn *c, long long dl)
{
	struct up_srv *s = c->s;
	struct up_col *uc = c->uc;
	const char *av[2] = { "ROLE", NULL }, *iv[2] = { "INFO", "replication" };
	size_t al[2] = { 4, 0 }, il[2] = { 4, 11 };
	struct rv v;
	int role = UPR_OTHER, link_down = 0, before = srv_state(s), now;
	long long lag = -1;

	if (send_cmd(c, dl, 1, av, al) != 0 || read_reply(c, &v, dl) != 0)
		return -1;
	if ((v.t == '*' || v.t == '~') && v.nel >= 1 && v.el[0].s) {
		if (v.el[0].len == 6 && !memcmp(v.el[0].s, "master", 6))
			role = UPR_MASTER;
		else if (v.el[0].len == 5 && !memcmp(v.el[0].s, "slave", 5))
			role = UPR_REPLICA;
	}
	rv_free(&v);
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
			/* S385: which master it follows - the writer's guard
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

int uc_connect(struct uconn *c, long long dl)
{
	struct up_col *uc = c->uc;
	struct up_srv *s = c->s;
	struct addrinfo hints, *ai = NULL;
	char ps[16];
	struct rv v;
	int fd, rc, one = 1;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = pc_resolve_family();     /* 12np: no AAAA without IPv6 */
	hints.ai_socktype = SOCK_STREAM;
	snprintf(ps, sizeof ps, "%d", s->port);
	if (getaddrinfo(s->host, ps, &hints, &ai) != 0 || !ai) {
		srv_reach(s, 0);
		return -1;
	}
	fd = socket(ai->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		freeaddrinfo(ai);
		return -1;
	}
	rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
	freeaddrinfo(ai);
	if (rc < 0 && errno != EINPROGRESS) {
		close(fd);
		srv_reach(s, 0);
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
			srv_reach(s, 0);
			return -1;
		}
	}
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	c->fd = fd;
	c->len = c->off = 0;
	c->resp3 = 0;
	__atomic_add_fetch(&s->conns_up, 1, __ATOMIC_RELAXED);
	ST_ADD(uc->connects, 1);
	ST_ADD(s->connects, 1);

	/* RESP3 with the credentials in one step, where the server has it.
	 * S385: not for the writer and the pass reader - RESP2, so what Redis
	 * answers is what a RESP2 client gets, relayed byte for byte */
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
			/* AR7: the writer's own credential, where it has one -
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
					rv_free(&v);
					LM_ERR("upstream %s: AUTH refused by %s%s\n",
						uc->name, s->addr, own ? " (the writer's "
						"upstream_write_password)" : "");
					goto fail;
				}
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
			rv_free(&v);
			LM_ERR("upstream %s: SELECT %d refused by %s\n", uc->name,
				s->db, s->addr);
			goto fail;
		}
		rv_free(&v);
	}
	srv_reach(s, 1);
	/* S384: which server it is, always now - a perfcached upstream is
	 * refused unless upstream_allow_perfcached; allowed, the connection
	 * names itself, so that node never reads through on its behalf */
	if (server_id(c, dl) != 0)
		goto fail;
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
	/* S382: what the server IS, before any fetch goes to it - a master
	 * where a replica is asked for is closed here, unless the fallback
	 * lets it serve while no replica does */
	if (c->wr) {
		/* S385: the writer must know what it reached - it keeps a master
		 * whatever upstream_role says (reads never go there) */
		if (role_check(c, dl) != 0)
			goto fail;
	} else if (uc->role_req == PC_UP_ROLE_REPLICA) {
		if (role_check(c, dl) != 0)
			goto fail;
		if (srv_state(s) == PC_UPS_OFF) {
			uc_close(c);
			return -1;
		}
	} else
		__atomic_store_n(&s->checked_ms, now_ms(), __ATOMIC_RELAXED);
	/* tracking: this connection's reads are tracked, and the
	 * invalidations go to the collection's invalidation connection.  The
	 * generation is read FIRST: if that connection moves meanwhile, the
	 * next fetch sees a newer one and re-arms. */
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
			/* another server than the invalidation connection's: the
			 * id is not ours there, so this connection stays untracked
			 * - its copies live at most upstream_negative_ms */
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
