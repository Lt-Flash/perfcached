/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* uptls.c - TLS to the upstreams.  The client polls its socket against a deadline (upconn.c),
 * so OpenSSL works on the socket itself and WANT_READ/WANT_WRITE becomes the event to wait for. */
#include <arpa/inet.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uptls.h"

long long now_ms(void);                 /* upconn.c */

#ifdef PC_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

struct up_tls {
	SSL_CTX *ctx;
	int verify;
	pthread_mutex_t mx;                /* ctx and verify: a reload swaps them */
};

/* the first error OpenSSL queued, in words; the queue emptied */
static void reason(char *out, size_t ol, const char *what)
{
	unsigned long e = ERR_get_error();
	char b[200];

	if (e)
		ERR_error_string_n(e, b, sizeof b);
	ERR_clear_error();
	snprintf(out, ol, "%s: %s", what, e ? b : "no reason given");
}

struct up_tls *uptls_new(const char *ca, const char *cert, const char *key, int verify,
		char *err, size_t el)
{
	struct up_tls *t = calloc(1, sizeof *t);

	if (!t || !(t->ctx = SSL_CTX_new(TLS_client_method()))) {
		snprintf(err, el, "TLS: no client context");
		free(t);
		return NULL;
	}
	pthread_mutex_init(&t->mx, NULL);
	SSL_CTX_set_min_proto_version(t->ctx, TLS1_2_VERSION);
	SSL_CTX_set_mode(t->ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
#ifdef SSL_OP_IGNORE_UNEXPECTED_EOF
	/* an upstream gone without close_notify (a restart) is a close, not a TLS failure: a
	 * reply cut short still fails its parse */
	SSL_CTX_set_options(t->ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);
#endif
	t->verify = verify;
	if (verify) {
		if ((ca ? SSL_CTX_load_verify_locations(t->ctx, ca, NULL) :
		     SSL_CTX_set_default_verify_paths(t->ctx)) != 1) {
			reason(err, el, ca ? "upstream_tls_ca" : "the system's CA store");
			goto bad;
		}
		SSL_CTX_set_verify(t->ctx, SSL_VERIFY_PEER, NULL);
	} else
		SSL_CTX_set_verify(t->ctx, SSL_VERIFY_NONE, NULL);
	if (cert) {
		if (SSL_CTX_use_certificate_chain_file(t->ctx, cert) != 1) {
			reason(err, el, "upstream_tls_cert");
			goto bad;
		}
		if (SSL_CTX_use_PrivateKey_file(t->ctx, key, SSL_FILETYPE_PEM) != 1) {
			reason(err, el, "upstream_tls_key");
			goto bad;
		}
		if (SSL_CTX_check_private_key(t->ctx) != 1) {
			snprintf(err, el, "upstream_tls_key does not match upstream_tls_cert");
			goto bad;
		}
	}
	return t;
bad:
	uptls_free(t);
	return NULL;
}

void uptls_free(struct up_tls *t)
{
	if (!t)
		return;
	SSL_CTX_free(t->ctx);
	pthread_mutex_destroy(&t->mx);
	free(t);
}

int uptls_swap(struct up_tls *t, struct up_tls *n)
{
	SSL_CTX *c;
	int v;

	pthread_mutex_lock(&t->mx);
	c = t->ctx;
	v = t->verify;
	t->ctx = n->ctx;
	t->verify = n->verify;
	pthread_mutex_unlock(&t->mx);
	n->ctx = c;                        /* each SSL made with it holds its own reference */
	n->verify = v;
	uptls_free(n);
	return 0;
}

static int is_ip(const char *s)
{
	unsigned char b[16];

	return inet_pton(AF_INET, s, b) == 1 || inet_pton(AF_INET6, s, b) == 1;
}

static short want(SSL *s, int r)
{
	int e = SSL_get_error(s, r);

	return e == SSL_ERROR_WANT_READ ? POLLIN : e == SSL_ERROR_WANT_WRITE ? POLLOUT : 0;
}

struct up_tls_conn *uptls_connect(struct up_tls *t, int fd, const char *name, long long dl,
		char *why, size_t wl)
{
	SSL *s;
	int ip = is_ip(name), verify;

	pthread_mutex_lock(&t->mx);
	s = SSL_new(t->ctx);
	verify = t->verify;
	pthread_mutex_unlock(&t->mx);

	if (!s || SSL_set_fd(s, fd) != 1) {
		snprintf(why, wl, "TLS: no connection state");
		SSL_free(s);
		return NULL;
	}
	if (!ip)
		SSL_set_tlsext_host_name(s, name);
	if (verify) {
		X509_VERIFY_PARAM *vp = SSL_get0_param(s);

		if (ip ? X509_VERIFY_PARAM_set1_ip_asc(vp, name) != 1 : SSL_set1_host(s, name) != 1) {
			snprintf(why, wl, "TLS: cannot check the name %.100s", name);
			SSL_free(s);
			return NULL;
		}
	}
	for (;;) {
		int r = SSL_connect(s);
		struct pollfd pf = { fd, 0, 0 };
		long long left = dl - now_ms();

		if (r == 1)
			break;
		pf.events = want(s, r);
		if (!pf.events) {
			long vr = SSL_get_verify_result(s);

			if (verify && vr != X509_V_OK) {
				ERR_clear_error();
				snprintf(why, wl, "the certificate: %s", X509_verify_cert_error_string(vr));
			} else
				reason(why, wl, "TLS");
			SSL_free(s);
			return NULL;
		}
		if (left <= 0 || poll(&pf, 1, left > INT_MAX ? INT_MAX : (int)left) <= 0) {
			snprintf(why, wl, "TLS: no handshake within the deadline");
			SSL_free(s);
			return NULL;
		}
	}
	return (struct up_tls_conn *)s;
}

ssize_t uptls_read(struct up_tls_conn *c, void *p, size_t n, short *ev, char *why, size_t wl)
{
	SSL *s = (SSL *)c;
	int r = SSL_read(s, p, n > INT_MAX ? INT_MAX : (int)n), e;

	if (r > 0)
		return r;
	e = SSL_get_error(s, r);
	if ((*ev = want(s, r)))
		return -2;
	if (e == SSL_ERROR_ZERO_RETURN)
		return 0;
	if (e == SSL_ERROR_SSL && why)
		reason(why, wl, "TLS");
	ERR_clear_error();
	return -1;
}

ssize_t uptls_write(struct up_tls_conn *c, const void *p, size_t n, short *ev)
{
	SSL *s = (SSL *)c;
	int r = SSL_write(s, p, n > INT_MAX ? INT_MAX : (int)n);

	if (r > 0)
		return r;
	if ((*ev = want(s, r)))
		return -2;
	ERR_clear_error();
	return -1;
}

int uptls_pending(struct up_tls_conn *c)
{
	return SSL_pending((SSL *)c);
}

void uptls_close(struct up_tls_conn *c)
{
	if (!c)
		return;
	SSL_shutdown((SSL *)c);            /* once: a non-blocking socket does not wait for its reply */
	ERR_clear_error();
	SSL_free((SSL *)c);
}

#else

struct up_tls *uptls_new(const char *ca, const char *cert, const char *key, int verify,
		char *err, size_t el)
{
	(void)ca; (void)cert; (void)key; (void)verify;
	snprintf(err, el, "rediss:// needs a build with TLS (PC_TLS)");
	return NULL;
}

void uptls_free(struct up_tls *t) { (void)t; }

int uptls_swap(struct up_tls *t, struct up_tls *n) { (void)t; (void)n; return -1; }

struct up_tls_conn *uptls_connect(struct up_tls *t, int fd, const char *name, long long dl,
		char *why, size_t wl)
{
	(void)t; (void)fd; (void)name; (void)dl;
	snprintf(why, wl, "no TLS in this build");
	return NULL;
}

ssize_t uptls_read(struct up_tls_conn *c, void *p, size_t n, short *ev, char *why, size_t wl)
{
	(void)c; (void)p; (void)n; (void)ev; (void)why; (void)wl;
	return -1;
}

ssize_t uptls_write(struct up_tls_conn *c, const void *p, size_t n, short *ev)
{
	(void)c; (void)p; (void)n; (void)ev;
	return -1;
}

int uptls_pending(struct up_tls_conn *c) { (void)c; return 0; }
void uptls_close(struct up_tls_conn *c) { (void)c; }

#endif
