/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* tls.c - TLS on the RESP and HTTP doors through OpenSSL memory BIOs, at the native door's
 * Noise hook (proto.c conn_take, conn_flush), so the epoll and io_uring paths are untouched. */
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tls.h"

#ifdef PC_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>

struct pc_tls {
	SSL *ssl;
	int up;
};

/* one context a process; a reload swaps it - SSL_new and the swap under one lock, as
 * each SSL holds its own reference and the old context goes with its last connection */
static SSL_CTX *tls_ctx;
static pthread_mutex_t tls_mx = PTHREAD_MUTEX_INITIALIZER;
static __thread char tls_why[160];
static struct pc_tls_stats tls_st;     /* under tls_mx, but the two counters */

const char *pc_tls_version(void)
{
	return OpenSSL_version(OPENSSL_VERSION);
}

/* the first error OpenSSL queued, in words; the queue emptied */
static const char *tls_reason(const char *what)
{
	unsigned long e = ERR_get_error();
	char b[120];

	ERR_error_string_n(e, b, sizeof b);
	snprintf(tls_why, sizeof tls_why, "%s: %s", what, e ? b : "no reason given");
	ERR_clear_error();
	return tls_why;
}

/* a reload's pair, read aside until the reload applies */
static SSL_CTX *tls_next;
static long long next_after;
static char next_cert[sizeof tls_st.cert];

int pc_tls_stage(const char *cert, const char *key, int min13, char *err, size_t en)
{
	SSL_CTX *x = SSL_CTX_new(TLS_server_method());

	if (!x) {
		snprintf(err, en, "%s", tls_reason("SSL_CTX_new"));
		return -1;
	}
	SSL_CTX_set_min_proto_version(x, min13 ? TLS1_3_VERSION : TLS1_2_VERSION);
	SSL_CTX_set_options(x, SSL_OP_NO_RENEGOTIATION | SSL_OP_CIPHER_SERVER_PREFERENCE);
	SSL_CTX_set_mode(x, SSL_MODE_RELEASE_BUFFERS);       /* idle: no record buffers */
	if (SSL_CTX_use_certificate_chain_file(x, cert) != 1) {
		snprintf(err, en, "tls_cert %s: %s", cert, tls_reason("load"));
		SSL_CTX_free(x);
		return -1;
	}
	if (SSL_CTX_use_PrivateKey_file(x, key, SSL_FILETYPE_PEM) != 1 ||
	    SSL_CTX_check_private_key(x) != 1) {
		snprintf(err, en, "tls_key %s: %s", key, tls_reason("load"));
		SSL_CTX_free(x);
		return -1;
	}
	{       /* when the leaf runs out, for /stats and the tls.cert alert */
		const ASN1_TIME *na = X509_get0_notAfter(SSL_CTX_get0_certificate(x));
		struct tm tm;

		memset(&tm, 0, sizeof tm);
		pthread_mutex_lock(&tls_mx);
		SSL_CTX_free(tls_next);
		tls_next = x;
		next_after = na && ASN1_TIME_to_tm(na, &tm) == 1 ? (long long)timegm(&tm) : 0;
		snprintf(next_cert, sizeof next_cert, "%s", cert);
		pthread_mutex_unlock(&tls_mx);
	}
	return 0;
}

void pc_tls_commit(int apply)
{
	SSL_CTX *old = NULL;

	pthread_mutex_lock(&tls_mx);
	if (apply && tls_next) {
		old = tls_ctx;
		tls_ctx = tls_next;
		tls_st.not_after = next_after;
		memcpy(tls_st.cert, next_cert, sizeof tls_st.cert);
		tls_st.loaded = 1;
	} else
		old = tls_next;
	tls_next = NULL;
	pthread_mutex_unlock(&tls_mx);
	SSL_CTX_free(old);
}

int pc_tls_load(const char *cert, const char *key, int min13, char *err, size_t en)
{
	if (pc_tls_stage(cert, key, min13, err, en) != 0)
		return -1;
	pc_tls_commit(1);
	return 0;
}

void pc_tls_stats(struct pc_tls_stats *s)
{
	pthread_mutex_lock(&tls_mx);
	*s = tls_st;
	pthread_mutex_unlock(&tls_mx);
	s->handshakes = __atomic_load_n(&tls_st.handshakes, __ATOMIC_RELAXED);
	s->failed = __atomic_load_n(&tls_st.failed, __ATOMIC_RELAXED);
}

struct pc_tls *pc_tls_new(void)
{
	struct pc_tls *t = calloc(1, sizeof *t);
	BIO *rb, *wb;

	if (!t)
		return NULL;
	pthread_mutex_lock(&tls_mx);
	t->ssl = tls_ctx ? SSL_new(tls_ctx) : NULL;
	pthread_mutex_unlock(&tls_mx);
	rb = BIO_new(BIO_s_mem());
	wb = BIO_new(BIO_s_mem());
	if (!t->ssl || !rb || !wb) {
		BIO_free(rb);
		BIO_free(wb);
		SSL_free(t->ssl);
		free(t);
		ERR_clear_error();
		return NULL;
	}
	BIO_set_mem_eof_return(rb, -1);    /* empty is "more to come", not the end */
	SSL_set_bio(t->ssl, rb, wb);
	SSL_set_accept_state(t->ssl);
	return t;
}

void pc_tls_free(struct pc_tls *t)
{
	if (!t)
		return;
	SSL_free(t->ssl);
	free(t);
}

int pc_tls_up(const struct pc_tls *t)
{
	return t->up;
}

/* what OpenSSL wrote, to the wire - unless the outlet says it is busy (asked with no
 * bytes): then it waits in the BIO for pc_tls_flush */
static int drain(struct pc_tls *t, pc_tls_out_fn wire, void *ctx)
{
	BIO *wb = SSL_get_wbio(t->ssl);
	char b[16384];
	int n;

	while (BIO_ctrl_pending(wb) > 0) {
		if (wire(ctx, NULL, 0) > 0)
			return 0;
		if ((n = BIO_read(wb, b, sizeof b)) <= 0)
			break;
		if (wire(ctx, b, (size_t)n) < 0)
			return -1;
	}
	return 0;
}

int pc_tls_flush(struct pc_tls *t, pc_tls_out_fn wire, void *ctx)
{
	return drain(t, wire, ctx);
}

int pc_tls_input(struct pc_tls *t, const char *p, size_t n, pc_tls_out_fn plain,
		pc_tls_out_fn wire, void *ctx, const char **why)
{
	char pt[16384];
	int r, e;

	if (n > INT_MAX || (n && BIO_write(SSL_get_rbio(t->ssl), p, (int)n) != (int)n)) {
		*why = "TLS: input not taken";
		return -1;
	}
	if (!t->up) {
		r = SSL_do_handshake(t->ssl);
		if (r != 1) {
			e = SSL_get_error(t->ssl, r);
			if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE)
				return drain(t, wire, ctx);
			*why = tls_reason("TLS handshake failed");
			__atomic_add_fetch(&tls_st.failed, 1, __ATOMIC_RELAXED);
			pthread_mutex_lock(&tls_mx);
			snprintf(tls_st.last_failure, sizeof tls_st.last_failure, "%s", *why);
			pthread_mutex_unlock(&tls_mx);
			drain(t, wire, ctx);       /* the alert, best effort */
			return -1;
		}
		t->up = 1;
		__atomic_add_fetch(&tls_st.handshakes, 1, __ATOMIC_RELAXED);
	}
	for (;;) {
		r = SSL_read(t->ssl, pt, sizeof pt);
		if (r > 0) {
			if (plain(ctx, pt, (size_t)r) < 0)
				return -1;
			continue;
		}
		e = SSL_get_error(t->ssl, r);
		if (e == SSL_ERROR_WANT_READ)
			break;
		*why = e == SSL_ERROR_ZERO_RETURN ? "TLS: closed by the client" :
			tls_reason("TLS read failed");
		return -1;
	}
	return drain(t, wire, ctx);        /* tickets after the handshake, alerts */
}

int pc_tls_output(struct pc_tls *t, const char *p, size_t n, pc_tls_out_fn wire, void *ctx)
{
	while (n) {
		int r = SSL_write(t->ssl, p, n > INT_MAX ? INT_MAX : (int)n);

		if (r <= 0) {
			ERR_clear_error();
			return -1;
		}
		p += r;
		n -= (size_t)r;
	}
	return drain(t, wire, ctx);
}
#else
const char *pc_tls_version(void)
{
	return "off";
}

int pc_tls_load(const char *cert, const char *key, int min13, char *err, size_t en)
{
	(void)cert; (void)key; (void)min13;
	snprintf(err, en, "this build has no TLS (make PC_TLS=yes, with OpenSSL)");
	return -1;
}

struct pc_tls *pc_tls_new(void) { return NULL; }
void pc_tls_stats(struct pc_tls_stats *s) { memset(s, 0, sizeof *s); }
void pc_tls_free(struct pc_tls *t) { (void)t; }
int pc_tls_up(const struct pc_tls *t) { (void)t; return 0; }

int pc_tls_input(struct pc_tls *t, const char *p, size_t n, pc_tls_out_fn plain,
		pc_tls_out_fn wire, void *ctx, const char **why)
{
	(void)t; (void)p; (void)n; (void)plain; (void)wire; (void)ctx;
	*why = "no TLS in this build";
	return -1;
}

int pc_tls_output(struct pc_tls *t, const char *p, size_t n, pc_tls_out_fn wire, void *ctx)
{
	(void)t; (void)p; (void)n; (void)wire; (void)ctx;
	return -1;
}

int pc_tls_flush(struct pc_tls *t, pc_tls_out_fn wire, void *ctx)
{
	(void)t; (void)wire; (void)ctx;
	return 0;
}

int pc_tls_stage(const char *cert, const char *key, int min13, char *err, size_t en)
{
	return pc_tls_load(cert, key, min13, err, en);
}

void pc_tls_commit(int apply) { (void)apply; }
#endif
