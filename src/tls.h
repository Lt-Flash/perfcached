/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* tls.h - TLS on the RESP and HTTP doors over OpenSSL memory BIOs (tls.c); built with PC_TLS
 * (make PC_TLS=auto|yes|no), without it a TLS listener is refused */
#ifndef PC_TLS_H
#define PC_TLS_H

#include <stddef.h>

struct pc_tls;

/* bytes for the caller to take: plaintext for the parser, or records for the wire */
typedef int (*pc_tls_out_fn)(void *ctx, const char *p, size_t n);

/* the TLS library this binary carries, as -V says it, or "off" */
const char *pc_tls_version(void);

/* the certificate chain and key, loaded and checked; installed for the connections that
 * come after.  @min13: TLS 1.3 only.  0 ok, -1 *err says why */
int pc_tls_load(const char *cert, const char *key, int min13, char *err, size_t en);
/* a reload's: the pair read aside (0, or -1 *err), then put in place (1) or dropped (0) */
int pc_tls_stage(const char *cert, const char *key, int min13, char *err, size_t en);
void pc_tls_commit(int apply);

/* what /stats and the alerts read */
struct pc_tls_stats {
	int loaded;                        /* a certificate is in use */
	char cert[256];                    /* its file */
	long long not_after;               /* its expiry, Unix seconds; 0 not known */
	unsigned long long handshakes, failed;
	char last_failure[160];
};
void pc_tls_stats(struct pc_tls_stats *s);

struct pc_tls *pc_tls_new(void);       /* NULL: no memory, or no certificate loaded */
void pc_tls_free(struct pc_tls *t);
int pc_tls_up(const struct pc_tls *t);  /* the handshake is done */

/* raw bytes in: the handshake advanced, plaintext handed to @plain, what OpenSSL has to
 * send (handshake, alerts, tickets) to @wire.  0 ok, 1 the client closed (serve what was
 * handed over, then close: *why), -1 the connection goes (*why) */
int pc_tls_input(struct pc_tls *t, const char *p, size_t n, pc_tls_out_fn plain,
		pc_tls_out_fn wire, void *ctx, const char **why);

/* plaintext out, sealed into records handed to @wire.  0 ok, -1 the connection goes.
 * @wire is asked first with no bytes: 1 = busy (a send in flight), the records wait */
int pc_tls_output(struct pc_tls *t, const char *p, size_t n, pc_tls_out_fn wire, void *ctx);
/* the records that waited, to @wire */
int pc_tls_flush(struct pc_tls *t, pc_tls_out_fn wire, void *ctx);

#endif
