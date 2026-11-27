/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* resp.h - func_perfd's RESP2 client (`protocol = resp`), written from the protocol description:
 * no Redis or hiredis code, no Asterisk header.  A transport failure or -READONLY re-dials once. */
#ifndef FUNC_PERFD_RESP_H
#define FUNC_PERFD_RESP_H

#include <stddef.h>

enum resp_type {
	RESP_T_STATUS = 1,             /* +OK */
	RESP_T_ERROR,                  /* -ERR ... */
	RESP_T_INTEGER,                /* :1 */
	RESP_T_BULK,                   /* $3 abc */
	RESP_T_NIL                     /* $-1 */
};

struct resp_reply {
	int type;
	long long integer;
	char *str;                     /* STATUS / ERROR / BULK: malloc'd,
	                                * NUL-terminated after len */
	size_t len;
	int oversize;                  /* BULK longer than max_bulk: the bytes
	                                * were drained, str is NULL */
};

struct resp_server {
	char *host;                    /* TCP: name or address */
	int port;
	char *path;                    /* unix socket instead, or NULL */
};

struct resp_sess_conf {
	const struct resp_server *servers;
	int nservers;
	const char *username;          /* NULL: AUTH <password> */
	const char *password;          /* NULL: no AUTH */
	int connect_timeout_ms;
	int io_timeout_ms;             /* per send / receive wait */
	size_t max_bulk;
};

struct resp_sess;

/* The session keeps @cf by pointer; it must outlive the session. */
struct resp_sess *resp_sess_new(const struct resp_sess_conf *cf);
void resp_sess_free(struct resp_sess *s);

/* Dial the servers in order from the session's next one, AUTH, SELECT @collection unless empty.
 * 0 open; 1 open but the SELECT refused (@err: the server's text); -1 none opened (@err: why). */
int resp_sess_open(struct resp_sess *s, const char *collection,
		char *err, size_t errcap);

/* Run one command in @collection (empty: whatever is selected), opening a closed session.  0: a
 * reply in @out, maybe RESP_T_ERROR (free it with resp_reply_free); -1: none (@err says why). */
int resp_sess_call(struct resp_sess *s, const char *collection, int argc,
		const char *const *argv, const size_t *lens,
		struct resp_reply *out, char *err, size_t errcap);

int resp_sess_is_open(const struct resp_sess *s);
/* "host:port" or the unix path of the server in use ("" when closed) */
const char *resp_sess_server(const struct resp_sess *s);
/* times a failed command re-dialled for its retry */
unsigned long long resp_sess_reconnects(const struct resp_sess *s);

void resp_reply_free(struct resp_reply *r);

#endif
