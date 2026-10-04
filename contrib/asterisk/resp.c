/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* resp.c - func_perfd's RESP2 client; see resp.h. */
#include "resp.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define RESP_RBUF      16384
#define RESP_LINE_MAX  4096

struct resp_sess {
	const struct resp_sess_conf *cf;
	int fd;
	int cur;                       /* server in use, -1 when closed */
	int next;                      /* where the next dial starts */
	int have_selected;
	char selected[256];
	unsigned long long reconnects;
	char where[300];
	size_t roff, rlen;
	char rbuf[RESP_RBUF];
};

static void sess_close(struct resp_sess *s)
{
	if (s->fd >= 0)
		close(s->fd);
	s->fd = -1;
	s->cur = -1;
	s->have_selected = 0;
	s->roff = s->rlen = 0;
	s->where[0] = '\0';
}

static int wait_fd(int fd, short events, int ms)
{
	struct pollfd pf;
	int rc;

	pf.fd = fd;
	pf.events = events;
	pf.revents = 0;
	do {
		rc = poll(&pf, 1, ms);
	} while (rc < 0 && errno == EINTR);
	return rc;
}

static void describe(const struct resp_server *sv, char *buf, size_t cap)
{
	if (sv->path)
		snprintf(buf, cap, "unix:%s", sv->path);
	else if (strchr(sv->host, ':'))
		snprintf(buf, cap, "[%s]:%d", sv->host, sv->port);
	else
		snprintf(buf, cap, "%s:%d", sv->host, sv->port);
}

static int nonblocking(int fd, int tcp)
{
	int one = 1;
	int fl = fcntl(fd, F_GETFL, 0);

	if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0)
		return -1;
	if (tcp) {
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
		setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
	}
	return 0;
}

/* @rc and @e are connect()'s return and errno */
static int connected(int fd, int rc, int e, int ms, char *err, size_t cap)
{
	int soerr = 0;
	socklen_t sl = sizeof soerr;

	if (rc == 0)
		return 0;
	if (e != EINPROGRESS) {
		snprintf(err, cap, "connect: %s", strerror(e));
		return -1;
	}
	rc = wait_fd(fd, POLLOUT, ms);
	if (rc == 0) {
		snprintf(err, cap, "connect timed out after %d ms", ms);
		return -1;
	}
	if (rc < 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0) {
		snprintf(err, cap, "connect: %s", strerror(errno));
		return -1;
	}
	if (soerr) {
		snprintf(err, cap, "connect: %s", strerror(soerr));
		return -1;
	}
	return 0;
}

static int dial(const struct resp_server *sv, int ms, char *err, size_t cap)
{
	struct addrinfo hints, *res, *ai;
	char port[16];
	int fd, rc, gai;

	if (sv->path) {
		struct sockaddr_un sa;

		if (strlen(sv->path) >= sizeof sa.sun_path) {
			snprintf(err, cap, "unix socket path too long");
			return -1;
		}
		fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd < 0) {
			snprintf(err, cap, "socket: %s", strerror(errno));
			return -1;
		}
		memset(&sa, 0, sizeof sa);
		sa.sun_family = AF_UNIX;
		memcpy(sa.sun_path, sv->path, strlen(sv->path) + 1);
		if (nonblocking(fd, 0) < 0) {
			snprintf(err, cap, "fcntl: %s", strerror(errno));
			close(fd);
			return -1;
		}
		rc = connect(fd, (struct sockaddr *)&sa, sizeof sa);
		if (connected(fd, rc, errno, ms, err, cap) < 0) {
			close(fd);
			return -1;
		}
		return fd;
	}

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_NUMERICSERV;
	snprintf(port, sizeof port, "%d", sv->port);
	gai = getaddrinfo(sv->host, port, &hints, &res);
	if (gai) {
		snprintf(err, cap, "resolve %s: %s", sv->host, gai_strerror(gai));
		return -1;
	}
	fd = -1;
	for (ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0) {
			snprintf(err, cap, "socket: %s", strerror(errno));
			continue;
		}
		if (nonblocking(fd, 1) < 0) {
			snprintf(err, cap, "fcntl: %s", strerror(errno));
		} else {
			rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
			if (connected(fd, rc, errno, ms, err, cap) == 0)
				break;
		}
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	return fd;
}

static int send_all(struct resp_sess *s, const char *p, size_t n,
		char *err, size_t cap)
{
	while (n) {
		ssize_t w = send(s->fd, p, n, MSG_NOSIGNAL);
		int rc;

		if (w > 0) {
			p += w;
			n -= (size_t)w;
			continue;
		}
		if (w < 0 && errno == EINTR)
			continue;
		if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			rc = wait_fd(s->fd, POLLOUT, s->cf->io_timeout_ms);
			if (rc > 0)
				continue;
			if (rc == 0)
				snprintf(err, cap, "write timed out after %d ms",
					s->cf->io_timeout_ms);
			else
				snprintf(err, cap, "poll: %s", strerror(errno));
			return -1;
		}
		snprintf(err, cap, "write: %s", strerror(errno));
		return -1;
	}
	return 0;
}

static int fill(struct resp_sess *s, char *err, size_t cap)
{
	if (s->roff == s->rlen)
		s->roff = s->rlen = 0;
	if (s->rlen == sizeof s->rbuf) {
		if (!s->roff) {
			snprintf(err, cap, "reply does not fit the buffer");
			return -1;
		}
		memmove(s->rbuf, s->rbuf + s->roff, s->rlen - s->roff);
		s->rlen -= s->roff;
		s->roff = 0;
	}
	for (;;) {
		ssize_t r = recv(s->fd, s->rbuf + s->rlen,
			sizeof s->rbuf - s->rlen, 0);
		int rc;

		if (r > 0) {
			s->rlen += (size_t)r;
			return 0;
		}
		if (r == 0) {
			snprintf(err, cap, "connection closed by the server");
			return -1;
		}
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			rc = wait_fd(s->fd, POLLIN, s->cf->io_timeout_ms);
			if (rc > 0)
				continue;
			if (rc == 0)
				snprintf(err, cap, "read timed out after %d ms",
					s->cf->io_timeout_ms);
			else
				snprintf(err, cap, "poll: %s", strerror(errno));
			return -1;
		}
		snprintf(err, cap, "read: %s", strerror(errno));
		return -1;
	}
}

/* the line is valid until the next read from the socket */
static int read_line(struct resp_sess *s, const char **line, size_t *len,
		char *err, size_t cap)
{
	for (;;) {
		const char *b = s->rbuf + s->roff;
		size_t avail = s->rlen - s->roff, i;

		for (i = 0; i + 1 < avail; i++) {
			if (b[i] == '\r' && b[i + 1] == '\n') {
				*line = b;
				*len = i;
				s->roff += i + 2;
				return 0;
			}
		}
		if (avail > RESP_LINE_MAX) {
			snprintf(err, cap, "reply line longer than %d bytes",
				RESP_LINE_MAX);
			return -1;
		}
		if (fill(s, err, cap) < 0)
			return -1;
	}
}

/* @dst NULL drains */
static int read_bytes(struct resp_sess *s, char *dst, size_t n,
		char *err, size_t cap)
{
	while (n) {
		size_t avail = s->rlen - s->roff, k;

		if (!avail) {
			if (fill(s, err, cap) < 0)
				return -1;
			continue;
		}
		k = avail < n ? avail : n;
		if (dst) {
			memcpy(dst, s->rbuf + s->roff, k);
			dst += k;
		}
		s->roff += k;
		n -= k;
	}
	return 0;
}

static int parse_ll(const char *p, size_t n, long long *out)
{
	long long v = 0;
	size_t i = 0;
	int neg = 0;

	if (i < n && p[i] == '-') {
		neg = 1;
		i++;
	}
	if (i == n)
		return -1;
	for (; i < n; i++) {
		int d = p[i] - '0';

		if (d < 0 || d > 9 || v > (LLONG_MAX - d) / 10)
			return -1;
		v = v * 10 + d;
	}
	*out = neg ? -v : v;
	return 0;
}

static int read_reply(struct resp_sess *s, struct resp_reply *out,
		char *err, size_t cap)
{
	const char *line;
	size_t len;
	long long n;
	char crlf[2];

	memset(out, 0, sizeof *out);
	if (read_line(s, &line, &len, err, cap) < 0)
		return -1;
	if (len < 1) {
		snprintf(err, cap, "empty reply line");
		return -1;
	}
	switch (line[0]) {
	case '+':
	case '-':
		out->str = malloc(len);
		if (!out->str) {
			snprintf(err, cap, "out of memory");
			return -1;
		}
		memcpy(out->str, line + 1, len - 1);
		out->str[len - 1] = '\0';
		out->len = len - 1;
		out->type = line[0] == '+' ? RESP_T_STATUS : RESP_T_ERROR;
		return 0;
	case ':':
		if (parse_ll(line + 1, len - 1, &out->integer) < 0) {
			snprintf(err, cap, "bad integer reply");
			return -1;
		}
		out->type = RESP_T_INTEGER;
		return 0;
	case '$':
		if (parse_ll(line + 1, len - 1, &n) < 0 || n < -1) {
			snprintf(err, cap, "bad bulk length");
			return -1;
		}
		if (n == -1) {
			out->type = RESP_T_NIL;
			return 0;
		}
		out->type = RESP_T_BULK;
		out->len = (size_t)n;
		if ((unsigned long long)n > (unsigned long long)s->cf->max_bulk) {
			out->oversize = 1;
			return read_bytes(s, NULL, (size_t)n + 2, err, cap);
		}
		out->str = malloc((size_t)n + 1);
		if (!out->str) {
			snprintf(err, cap, "out of memory");
			return -1;
		}
		if (read_bytes(s, out->str, (size_t)n, err, cap) < 0
		        || read_bytes(s, crlf, 2, err, cap) < 0) {
			resp_reply_free(out);
			return -1;
		}
		if (crlf[0] != '\r' || crlf[1] != '\n') {
			resp_reply_free(out);
			snprintf(err, cap, "bulk reply not terminated");
			return -1;
		}
		out->str[n] = '\0';
		return 0;
	default:
		snprintf(err, cap, "unexpected reply type 0x%02x",
			(unsigned char)line[0]);
		return -1;
	}
}

static int exchange(struct resp_sess *s, int argc, const char *const *argv,
		const size_t *lens, struct resp_reply *out, char *err, size_t cap)
{
	size_t total = 32, off = 0;
	char *buf;
	int i, rc;

	for (i = 0; i < argc; i++)
		total += lens[i] + 32;
	buf = malloc(total);
	if (!buf) {
		snprintf(err, cap, "out of memory");
		return -1;
	}
	off += (size_t)snprintf(buf + off, total - off, "*%d\r\n", argc);
	for (i = 0; i < argc; i++) {
		off += (size_t)snprintf(buf + off, total - off, "$%zu\r\n",
			lens[i]);
		memcpy(buf + off, argv[i], lens[i]);
		off += lens[i];
		buf[off++] = '\r';
		buf[off++] = '\n';
	}
	rc = send_all(s, buf, off, err, cap);
	free(buf);
	if (rc < 0)
		return -1;
	return read_reply(s, out, err, cap);
}

/* 0 authenticated or nothing to do; -1 no reply; 1 refused */
static int authenticate(struct resp_sess *s, char *err, size_t cap)
{
	const char *argv[3];
	size_t lens[3];
	struct resp_reply r;
	int argc = 0;

	if (!s->cf->password)
		return 0;
	argv[argc] = "AUTH";
	lens[argc++] = 4;
	if (s->cf->username) {
		argv[argc] = s->cf->username;
		lens[argc] = strlen(s->cf->username);
		argc++;
	}
	argv[argc] = s->cf->password;
	lens[argc] = strlen(s->cf->password);
	argc++;
	if (exchange(s, argc, argv, lens, &r, err, cap) < 0)
		return -1;
	if (r.type != RESP_T_STATUS) {
		snprintf(err, cap, "AUTH refused: %s",
			r.type == RESP_T_ERROR ? r.str : "unexpected reply");
		resp_reply_free(&r);
		return 1;
	}
	resp_reply_free(&r);
	return 0;
}

/* 0 selected; 1 refused (@err = the server's text); -1 no reply */
static int select_collection(struct resp_sess *s, const char *col,
		char *err, size_t cap)
{
	const char *argv[2];
	size_t lens[2], cl = strlen(col);
	struct resp_reply r;

	argv[0] = "SELECT";
	lens[0] = 6;
	argv[1] = col;
	lens[1] = cl;
	s->have_selected = 0;
	if (exchange(s, 2, argv, lens, &r, err, cap) < 0)
		return -1;
	if (r.type != RESP_T_STATUS) {
		snprintf(err, cap, "SELECT %s refused: %s", col,
			r.type == RESP_T_ERROR ? r.str : "unexpected reply");
		resp_reply_free(&r);
		return 1;
	}
	resp_reply_free(&r);
	if (cl < sizeof s->selected) {
		memcpy(s->selected, col, cl + 1);
		s->have_selected = 1;
	}
	return 0;
}

struct resp_sess *resp_sess_new(const struct resp_sess_conf *cf)
{
	struct resp_sess *s = calloc(1, sizeof *s);

	if (!s)
		return NULL;
	s->cf = cf;
	s->fd = -1;
	s->cur = -1;
	return s;
}

void resp_sess_free(struct resp_sess *s)
{
	if (!s)
		return;
	sess_close(s);
	free(s);
}

int resp_sess_open(struct resp_sess *s, const char *collection,
		char *err, size_t errcap)
{
	char e[256], name[256];
	int k, n = s->cf->nservers;

	sess_close(s);
	if (n < 1) {
		snprintf(err, errcap, "no servers configured");
		return -1;
	}
	for (k = 0; k < n; k++) {
		const struct resp_server *sv;
		int i = (s->next + k) % n, rc;

		sv = &s->cf->servers[i];
		describe(sv, name, sizeof name);
		s->fd = dial(sv, s->cf->connect_timeout_ms, e, sizeof e);
		if (s->fd < 0) {
			snprintf(err, errcap, "%s: %s", name, e);
			continue;
		}
		if (authenticate(s, e, sizeof e) != 0) {
			snprintf(err, errcap, "%s: %s", name, e);
			sess_close(s);
			continue;
		}
		s->cur = i;
		s->next = (i + 1) % n;
		snprintf(s->where, sizeof s->where, "%s", name);
		if (collection && *collection) {
			rc = select_collection(s, collection, e, sizeof e);
			if (rc < 0) {
				snprintf(err, errcap, "%s: %s", name, e);
				sess_close(s);
				continue;
			}
			if (rc > 0) {
				snprintf(err, errcap, "%s: %s", name, e);
				return 1;
			}
		}
		return 0;
	}
	return -1;
}

int resp_sess_call(struct resp_sess *s, const char *collection, int argc,
		const char *const *argv, const size_t *lens,
		struct resp_reply *out, char *err, size_t errcap)
{
	char e[256];
	int attempt, rc;

	memset(out, 0, sizeof *out);
	for (attempt = 0; attempt < 2; attempt++) {
		if (s->fd < 0) {
			if (resp_sess_open(s, NULL, err, errcap) < 0)
				return -1;
			if (attempt)
				s->reconnects++;
		}
		if (collection && *collection && !(s->have_selected
		        && strcmp(s->selected, collection) == 0)) {
			rc = select_collection(s, collection, e, sizeof e);
			if (rc > 0) {
				snprintf(err, errcap, "%s: %s", s->where, e);
				return -1;
			}
			if (rc < 0) {
				snprintf(err, errcap, "%s: %s", s->where, e);
				sess_close(s);
				continue;
			}
		}
		if (exchange(s, argc, argv, lens, out, e, sizeof e) < 0) {
			snprintf(err, errcap, "%s: %s", s->where, e);
			sess_close(s);
			continue;
		}
		if (out->type == RESP_T_ERROR
		        && strncmp(out->str, "READONLY", 8) == 0) {
			snprintf(err, errcap, "%s: %s", s->where, out->str);
			if (attempt == 0) {
				resp_reply_free(out);
				sess_close(s);
				continue;
			}
		}
		return 0;
	}
	return -1;
}

int resp_sess_is_open(const struct resp_sess *s)
{
	return s->fd >= 0;
}

const char *resp_sess_server(const struct resp_sess *s)
{
	return s->where;
}

unsigned long long resp_sess_reconnects(const struct resp_sess *s)
{
	return s->reconnects;
}

void resp_reply_free(struct resp_reply *r)
{
	if (!r)
		return;
	free(r->str);
	r->str = NULL;
}
