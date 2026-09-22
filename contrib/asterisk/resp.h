/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * resp.h - the RESP2 client behind func_perfd's `protocol = resp`.
 *
 * Written from the RESP2 protocol description; no Redis or hiredis code.
 * No Asterisk header either, so the file builds and tests on its own.
 *
 * A session is one connection plus these rules:
 * dial the server list in order, AUTH, SELECT the collection when it
 * changes, and on a transport failure or -READONLY close, re-dial once
 * starting at the server AFTER the one that failed, and retry.
 */
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

/* Dial the servers in order from the session's next one, AUTH, and SELECT
 * @collection when it is not empty.  0 open; 1 open but the SELECT was
 * refused (@err holds the server's text, nothing is selected); -1 no
 * server could be opened (@err says why). */
int resp_sess_open(struct resp_sess *s, const char *collection,
		char *err, size_t errcap);

/* Run one command in @collection (empty: whatever is selected).  0: a
 * reply is in @out, possibly RESP_T_ERROR - free it with resp_reply_free;
 * -1: no reply (@err says why).  Opens the session when it is closed. */
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
