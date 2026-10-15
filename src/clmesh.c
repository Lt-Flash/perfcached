/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clmesh.c - S36: the unicast target set.  See clmesh.h.
 */
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "compat/dprint.h"
#include "clmesh.h"

struct seed {
	char host[256];
	uint16_t port;                 /* host order */
	struct sockaddr_in addr[8];    /* the last resolution */
	int naddr;
	int failing;                   /* said once per failure run */
};

struct known {
	struct sockaddr_in a;
	long long seen_ms;
	long long hint_ms;             /* last told to a master */
};

static struct {
	pthread_mutex_t mx;
	pthread_cond_t cv;
	struct seed seed[CLMESH_MAX_SEEDS];
	int nseed;
	struct known known[CLMESH_MAX_KNOWN];
	int nknown;
	int wake;
	unsigned long long resolve_fail;
} M = { .mx = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

static int same(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
	return a->sin_addr.s_addr == b->sin_addr.s_addr &&
		a->sin_port == b->sin_port;
}

static int parse_seeds(const char *list, int default_port)
{
	const char *p = list;

	while (p && *p) {
		char tok[256], *colon;
		size_t n;
		long port = default_port;

		p += strspn(p, ", \t");
		n = strcspn(p, ", \t");
		if (!n)
			break;
		if (n >= sizeof tok) {
			LM_ERR("cluster: seed '%.40s...' is too long\n", p);
			return -1;
		}
		memcpy(tok, p, n);
		tok[n] = 0;
		p += n;
		colon = strrchr(tok, ':');
		if (colon) {
			char *end;

			*colon = 0;
			port = strtol(colon + 1, &end, 10);
			if (*end || port < 1 || port > 65535 || !tok[0]) {
				LM_ERR("cluster: seed '%s:%s' - want host[:port]\n",
					tok, colon + 1);
				return -1;
			}
		}
		if (M.nseed >= CLMESH_MAX_SEEDS) {
			LM_ERR("cluster: more than %d seeds\n", CLMESH_MAX_SEEDS);
			return -1;
		}
		snprintf(M.seed[M.nseed].host, sizeof M.seed[M.nseed].host,
			"%s", tok);
		M.seed[M.nseed].port = (uint16_t)port;
		M.nseed++;
	}
	return 0;
}

/* one pass over the seeds: getaddrinfo OUTSIDE the lock (it can block
 * for seconds), the result swapped in under it */
static void resolve_all(void)
{
	int i;

	for (i = 0; i < M.nseed; i++) {
		struct addrinfo hints, *res = NULL, *r;
		struct sockaddr_in got[8];
		char host[256];
		uint16_t port;
		int n = 0, rc;

		pthread_mutex_lock(&M.mx);
		memcpy(host, M.seed[i].host, sizeof host);
		port = M.seed[i].port;
		pthread_mutex_unlock(&M.mx);

		memset(&hints, 0, sizeof hints);
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_DGRAM;
		rc = getaddrinfo(host, NULL, &hints, &res);
		for (r = rc == 0 ? res : NULL; r && n < 8; r = r->ai_next) {
			struct sockaddr_in a = *(struct sockaddr_in *)r->ai_addr;
			int j, dup = 0;

			a.sin_port = htons(port);
			for (j = 0; j < n; j++)
				dup |= same(&got[j], &a);
			if (!dup)
				got[n++] = a;
		}
		if (res)
			freeaddrinfo(res);

		pthread_mutex_lock(&M.mx);
		if (rc != 0 || !n) {
			M.resolve_fail++;
			if (!M.seed[i].failing)
				LM_WARN("cluster: seed '%s' does not resolve (%s) - "
					"keeping its last %d address(es), retrying "
					"every %d s\n", host,
					rc ? gai_strerror(rc) : "no IPv4 address",
					M.seed[i].naddr, CLMESH_RESOLVE_MS / 1000);
			M.seed[i].failing = 1;
		} else {
			if (M.seed[i].failing)
				LM_NOTICE("cluster: seed '%s' resolves again (%d "
					"address(es))\n", host, n);
			M.seed[i].failing = 0;
			memcpy(M.seed[i].addr, got, sizeof got[0] * (size_t)n);
			M.seed[i].naddr = n;
		}
		pthread_mutex_unlock(&M.mx);
	}
}

static void *resolver_main(void *arg)
{
	(void)arg;
	for (;;) {
		struct timespec until;

		resolve_all();
		clock_gettime(CLOCK_REALTIME, &until);
		until.tv_sec += CLMESH_RESOLVE_MS / 1000;
		pthread_mutex_lock(&M.mx);
		while (!M.wake &&
		        pthread_cond_timedwait(&M.cv, &M.mx, &until) != ETIMEDOUT)
			;
		M.wake = 0;
		pthread_mutex_unlock(&M.mx);
	}
	return NULL;
}

int clmesh_start(const char *seeds, int default_port)
{
	pthread_t t;
	pthread_attr_t at;

	if (seeds && parse_seeds(seeds, default_port) != 0)
		return -1;
	if (!M.nseed)
		LM_WARN("cluster: discovery = unicast with no seeds - this node "
			"finds peers only when they find it\n");
	resolve_all();                     /* the first answer before any beat */
	if (!M.nseed)
		return 0;
	pthread_attr_init(&at);
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&t, &at, resolver_main, NULL) != 0) {
		pthread_attr_destroy(&at);
		LM_ERR("cluster: cannot start the seed resolver\n");
		return -1;
	}
	pthread_attr_destroy(&at);
	return 0;
}

void clmesh_resolve_soon(void)
{
	pthread_mutex_lock(&M.mx);
	M.wake = 1;
	pthread_cond_signal(&M.cv);
	pthread_mutex_unlock(&M.mx);
}

int clmesh_heard(const struct sockaddr_in *a, long long now_ms)
{
	int i, oldest = 0, isnew = 1;

	pthread_mutex_lock(&M.mx);
	for (i = 0; i < M.nknown; i++) {
		if (same(&M.known[i].a, a)) {
			isnew = now_ms - M.known[i].seen_ms >= CLMESH_KNOWN_MS;
			M.known[i].seen_ms = now_ms;
			pthread_mutex_unlock(&M.mx);
			return isnew;
		}
		if (M.known[i].seen_ms < M.known[oldest].seen_ms)
			oldest = i;
	}
	i = M.nknown < CLMESH_MAX_KNOWN ? M.nknown++ : oldest;
	M.known[i].a = *a;
	M.known[i].seen_ms = now_ms;
	M.known[i].hint_ms = 0;
	pthread_mutex_unlock(&M.mx);
	return isnew;
}

int clmesh_targets(struct sockaddr_in *out, int cap,
		const struct sockaddr_in *self, long long now_ms)
{
	int i, j, k, n = 0;

	pthread_mutex_lock(&M.mx);
	for (i = 0; i < M.nseed && n < cap; i++)
		for (j = 0; j < M.seed[i].naddr && n < cap; j++) {
			const struct sockaddr_in *a = &M.seed[i].addr[j];
			int dup = self && same(a, self);

			for (k = 0; k < n && !dup; k++)
				dup = same(&out[k], a);
			if (!dup)
				out[n++] = *a;
		}
	for (i = 0; i < M.nknown && n < cap; i++) {
		const struct sockaddr_in *a = &M.known[i].a;
		int dup;

		if (now_ms - M.known[i].seen_ms >= CLMESH_KNOWN_MS)
			continue;
		dup = self && same(a, self);
		for (k = 0; k < n && !dup; k++)
			dup = same(&out[k], a);
		if (!dup)
			out[n++] = *a;
	}
	pthread_mutex_unlock(&M.mx);
	return n;
}

int clmesh_hint_due(const struct sockaddr_in *a, long long now_ms,
		long long every_ms)
{
	int i, due = 0;

	pthread_mutex_lock(&M.mx);
	for (i = 0; i < M.nknown; i++)
		if (same(&M.known[i].a, a)) {
			if (!M.known[i].hint_ms ||
			        now_ms - M.known[i].hint_ms >= every_ms) {
				M.known[i].hint_ms = now_ms;
				due = 1;
			}
			break;
		}
	pthread_mutex_unlock(&M.mx);
	return due;
}

int clmesh_known(struct sockaddr_in *out, int cap, long long now_ms)
{
	int i, n = 0;

	pthread_mutex_lock(&M.mx);
	for (i = 0; i < M.nknown && n < cap; i++)
		if (now_ms - M.known[i].seen_ms < CLMESH_KNOWN_MS)
			out[n++] = M.known[i].a;
	pthread_mutex_unlock(&M.mx);
	return n;
}

void clmesh_figures(int *seeds_named, int *seeds_resolved, int *known,
		unsigned long long *resolve_fail)
{
	int i, r = 0;

	pthread_mutex_lock(&M.mx);
	for (i = 0; i < M.nseed; i++)
		r += M.seed[i].naddr > 0;
	*seeds_named = M.nseed;
	*seeds_resolved = r;
	*known = M.nknown;
	*resolve_fail = M.resolve_fail;
	pthread_mutex_unlock(&M.mx);
}
