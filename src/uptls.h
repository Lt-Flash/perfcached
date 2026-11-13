/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* uptls.h - TLS to the read-through's upstreams (rediss://), OpenSSL over the connection's
 * socket (uptls.c).  Without PC_TLS a rediss:// upstream is refused. */
#ifndef PC_UPTLS_H
#define PC_UPTLS_H

#include <stddef.h>
#include <sys/types.h>

struct up_tls;                          /* a collection's: what it trusts, what it shows */
struct up_tls_conn;                     /* one connection's */

/* @ca NULL: the system's store; @cert and @key both or neither; @verify 0: the upstream's
 * certificate is not checked.  NULL, *err says why */
struct up_tls *uptls_new(const char *ca, const char *cert, const char *key, int verify,
		char *err, size_t el);
void uptls_free(struct up_tls *t);
/* a reload's: @n's context becomes @t's, for the next connection; the old one lives on in the
 * connections made with it.  @n is freed.  0, -1 no TLS in this build */
int uptls_swap(struct up_tls *t, struct up_tls *n);

/* the handshake on the connected non-blocking @fd by @dl (now_ms), checking @name and sending
 * it as SNI unless it is an IP literal (checked against the cert's IPs).  NULL, *why says why */
struct up_tls_conn *uptls_connect(struct up_tls *t, int fd, const char *name, long long dl,
		char *why, size_t wl);

/* > 0 bytes; 0 the peer closed (read); -1 failed (*why, when given, says why); -2 call
 * again once poll() reports *ev */
ssize_t uptls_read(struct up_tls_conn *c, void *p, size_t n, short *ev, char *why, size_t wl);
ssize_t uptls_write(struct up_tls_conn *c, const void *p, size_t n, short *ev);
int uptls_pending(struct up_tls_conn *c);   /* bytes decrypted and not yet read */
void uptls_close(struct up_tls_conn *c);    /* close_notify sent, freed; the fd stays */

#endif
