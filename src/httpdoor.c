/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * httpdoor.c - the metrics door (S46): the HTTP/1.0 responder behind GET
 * /metrics, /stats, /members, /clients and /health - read-only since
 * 0.7.0, its / and any POST answering where those went - with the request
 * head, the Bearer token, the cached /stats and /metrics bodies (0.6.2),
 * and the per-address rate limit.  Moved from proto.c unchanged (the 0.7.2
 * split, wave 1 - doc/MODULARITY.md); it answers through proto.c's reply
 * staging (protoint.h).
 */
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sodium.h>

#include "compat/dprint.h"
#include "compat/timer.h"
#include "json.h"
#include "ptree.h"
#include "metrics.h"
#include "verbs.h"                      /* pc_stats_tree, pc_members_tree */
#include "obs.h"                        /* pc_obs_clients_tree */
#include "daemon.h"                     /* the token, the rate, the cache */
#include "pcaddr.h"
#include "proto.h"
#include "protoint.h"

/* ---- the metrics door (S46) --------------------------------------------
 *
 * A deliberately minimal HTTP/1.0 responder, NOT a web server: read a
 * request head bounded by PC_HTTP_MAX, take the method and path from
 * the request line, ignore every header, answer, close.  No keep-alive,
 * no request body, no state carried between requests - which is what
 * keeps this small enough to reason about on a plaintext port.
 *
 * The alternative was libmicrohttpd (what OpenSIPS' httpd module uses).
 * Rejected on purpose: the daemon's dependency set is libc + pthreads +
 * libsodium and that is a published promise (README), the library wants
 * its own threads or its own event loop where every worker here already
 * owns an epoll, and a full server - POST, chunked bodies, digest auth,
 * TLS - is far more surface than two read-only GET routes need.  If the
 * status page ever grows forms or auth, take the dependency then rather
 * than growing this into a server.
 */
#define PC_HTTP_MAX 8192               /* a request head we will read */

/* memmem() is a GNU extension: it needs _GNU_SOURCE, and it compiled on
 * the Debian 13 build host while failing on Ubuntu 20.04 - a portability
 * split for a search over at most 8 KB.  A plain scan costs nothing at
 * this size and keeps the file free of feature-test macros. */
static char *find_bytes(char *hay, size_t hn, const char *needle, size_t nn)
{
	size_t i;

	if (nn > hn)
		return NULL;
	for (i = 0; i + nn <= hn; i++)
		if (hay[i] == needle[0] && !memcmp(hay + i, needle, nn))
			return hay + i;
	return NULL;
}
/* the exposition is bounded by the collection count, and 64 collections
 * of three series each plus the fixed block fits comfortably; the
 * renderer truncates rather than overruns if that ever stops being
 * true, and a truncated scrape is a visible bug, not a crash */
#define PC_METRICS_MAX ((size_t)256 * 1024)


/* Authorization: Bearer <token>, compared in constant time.  Header
 * only - never a query parameter, which would land the token in every
 * proxy log and browser history along the way. */
static int http_token_ok(const char *head, size_t hlen, const char *want)
{
	const char *p = head, *end = head + hlen, *v;
	size_t wn = strlen(want), n;
	unsigned char diff = 0;
	size_t i;

	while (p < end) {
		const char *eol = memchr(p, '\n', (size_t)(end - p));
		size_t ll = eol ? (size_t)(eol - p) : (size_t)(end - p);

		if (ll >= 21 && !strncasecmp(p, "Authorization:", 14)) {
			v = p + 14;
			while (v < p + ll && (*v == ' ' || *v == '\t'))
				v++;
			if ((size_t)(p + ll - v) > 7 &&
			    !strncasecmp(v, "Bearer ", 7)) {
				v += 7;
				n = (size_t)(p + ll - v);
				while (n && (v[n-1] == '\r' || v[n-1] == ' '))
					n--;
				if (n != wn)
					return 0;
				for (i = 0; i < n; i++)
					diff |= (unsigned char)(v[i] ^ want[i]);
				return diff == 0;
			}
			/* 0.6.2: Basic too - the browser's own login (any user,
			 * the token as the password).  A browser cannot add a
			 * Bearer header to a page it is asked to open, so a
			 * tokened node locked the page out; with Basic, the 401's
			 * WWW-Authenticate makes it ask once and send it on every
			 * request of the page */
			if ((size_t)(p + ll - v) > 6 &&
			    !strncasecmp(v, "Basic ", 6)) {
				unsigned char dec[512];
				size_t dn = 0;
				const unsigned char *pw;

				v += 6;
				n = (size_t)(p + ll - v);
				while (n && (v[n-1] == '\r' || v[n-1] == ' '))
					n--;
				int okb = 0;

				if (sodium_base642bin(dec, sizeof dec, v, n, NULL, &dn,
				        NULL, sodium_base64_VARIANT_ORIGINAL) == 0 &&
				    (pw = memchr(dec, ':', dn)) != NULL &&
				    dn - (size_t)(pw + 1 - dec) == wn) {
					pw++;
					for (i = 0; i < wn; i++)
						diff |= (unsigned char)(pw[i] ^
							(unsigned char)want[i]);
					okb = diff == 0;
				}
				sodium_memzero(dec, sizeof dec);   /* whatever it held */
				return okb;
			}
		}
		if (!eol)
			break;
		p = eol + 1;
	}
	return 0;
}

/* @extra: further header lines, each CRLF-terminated, or NULL.  Every
 * response carries the three hardening headers: no MIME sniffing, no
 * referrer, no framing. */
static int http_send_ex(struct pc_conn *c, const char *status,
		const char *ctype, const char *extra, const char *body, size_t blen)
{
	char head[1024];
	int n = snprintf(head, sizeof head,
		"HTTP/1.0 %s\r\n"
		"Content-Type: %s\r\n"
		"Content-Length: %zu\r\n"
		"X-Content-Type-Options: nosniff\r\n"
		"Referrer-Policy: no-referrer\r\n"
		"X-Frame-Options: DENY\r\n"
		"%s"
		"Connection: close\r\n"
		"\r\n", status, ctype, blen, extra ? extra : "");

	if (n < 0 || (size_t)n >= sizeof head)
		return -1;
	if (out_append(c, head, (size_t)n) != 0)
		return -1;
	if (blen && out_append(c, body, blen) != 0)
		return -1;
	/* the header says Connection: close, so CLOSE - without this the
	 * socket stays open, every client that reads to EOF (curl and
	 * Prometheus both do) hangs until its own timeout, and the
	 * response looks lost rather than late.  close_after fires once
	 * the staged bytes have actually reached the wire. */
	c->close_after = 1;
	return 0;
}

static int http_send(struct pc_conn *c, const char *status,
		const char *ctype, const char *body, size_t blen)
{
	return http_send_ex(c, status, ctype, NULL, body, blen);
}

/* 0.6.2: the HTTP door's own limits.  Building /stats costs a worker
 * ~0.6 ms and /metrics ~0.3-0.5 (245), and nothing bounded how often an
 * allow-listed address could ask.
 *
 * http_rate (http_rate_ok, at accept): requests a second per client
 * address, a bucket of twice that.  This door answers one request per
 * connection, so a peer over it is answered 429 at accept, unread, and
 * parked for its drain like a refused one.
 *
 * http_cache_ms (here): /stats and /metrics are built at most once in
 * that long and the copy is served in between - any number of open pages
 * cost one build a second.  A stats reset drops the copies.
 *
 * Loopback - an on-box scraper, a tool, an SSH tunnel to the page, every
 * test suite - is neither limited nor served a copy: what it reads is
 * this instant's, as before.  PERFCACHED_HTTP_LIMIT_LOOPBACK applies both
 * to it too, for the tests of the limits themselves. */
static unsigned long long hr_limited, hc_hits, hc_builds;
static int hr_loop = -1;

static int http_limit_loopback(void)
{
	if (hr_loop < 0)
		hr_loop = getenv("PERFCACHED_HTTP_LIMIT_LOOPBACK") != NULL;
	return hr_loop;
}

static long long hc_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

struct hcache {
	pthread_mutex_t mx;
	char *body;
	size_t len;
	long long at;
	unsigned int gen;
};
static struct hcache hc_stats = { PTHREAD_MUTEX_INITIALIZER, NULL, 0, 0, 0 };
static struct hcache hc_metrics = { PTHREAD_MUTEX_INITIALIZER, NULL, 0, 0, 0 };
static unsigned int hc_gen;                /* bumped by a stats reset */

void pc_http_cache_drop(void)
{
	__atomic_add_fetch(&hc_gen, 1, __ATOMIC_RELAXED);
}

void pc_http_counters(unsigned long long *limited, unsigned long long *hits,
		unsigned long long *builds)
{
	*limited = __atomic_load_n(&hr_limited, __ATOMIC_RELAXED);
	*hits = __atomic_load_n(&hc_hits, __ATOMIC_RELAXED);
	*builds = __atomic_load_n(&hc_builds, __ATOMIC_RELAXED);
}

/* the copy, when one is fresh: staged on @c and 1; else 0 (build it) */
static int hc_send(struct pc_conn *c, struct hcache *h, const char *ctype)
{
	int ms = pc_http_cache_ms(), hit = 0;
	long long now;

	if (ms <= 0 || (c->http_loop && !http_limit_loopback()))
		return 0;
	now = hc_now();
	pthread_mutex_lock(&h->mx);
	if (h->body && h->gen == __atomic_load_n(&hc_gen, __ATOMIC_RELAXED) &&
	    now - h->at < ms) {
		(void)http_send(c, "200 OK", ctype, h->body, h->len);
		hit = 1;
	}
	pthread_mutex_unlock(&h->mx);
	if (hit)
		__atomic_add_fetch(&hc_hits, 1, __ATOMIC_RELAXED);
	return hit;
}

static void hc_put(struct hcache *h, const char *body, size_t len)
{
	char *b;

	__atomic_add_fetch(&hc_builds, 1, __ATOMIC_RELAXED);
	if (pc_http_cache_ms() <= 0 || !(b = malloc(len ? len : 1)))
		return;
	memcpy(b, body, len);
	pthread_mutex_lock(&h->mx);
	free(h->body);
	h->body = b;
	h->len = len;
	h->at = hc_now();
	h->gen = __atomic_load_n(&hc_gen, __ATOMIC_RELAXED);
	pthread_mutex_unlock(&h->mx);
}

/* S317: /stats, /members and /clients answer the TREE the native method
 * answers, rendered as JSON here - one writer per surface, so the page
 * and a native client cannot read different numbers.  @t is consumed
 * (freed) whatever happens; @jcap is the JSON writer's first size, and it
 * grows to JW_HEAP_MAX. */
static int http_send_tree_c(struct pc_conn *c, struct pc_tw *t, size_t jcap,
		struct hcache *hc)
{
	struct pc_jw w;
	int rc;

	if (pc_tw_done(t) != 0) {
		pc_tw_free(t);
		return http_send(c, "503 Service Unavailable", "text/plain",
			"reply too large\n", 16);
	}
	pc_jw_init(&w, NULL, 0);
	if (pc_jw_init_heap(&w, jcap) != 0) {
		pc_tw_free(t);
		return http_send(c, "503 Service Unavailable", "text/plain",
			"out of memory\n", 14);
	}
	if (pc_tree_json(&w, t->b, t->n) != t->n || w.overflow)
		rc = http_send(c, "503 Service Unavailable", "text/plain",
			"reply too large\n", 16);
	else {
		rc = http_send(c, "200 OK", "application/json", w.buf, w.len);
		if (hc)
			hc_put(hc, w.buf, w.len);
	}
	pc_jw_free(&w);
	pc_tw_free(t);
	return rc;
}

static int http_send_tree(struct pc_conn *c, struct pc_tw *t, size_t jcap)
{
	return http_send_tree_c(c, t, jcap, NULL);
}

/* one request from the buffer at @off.  Returns bytes consumed, 0 when
 * the head is incomplete, or -1 to drop the connection. */
int http_one(struct pc_conn *c, size_t off)
{
	char *base = c->in + off;
	size_t avail = c->in_len - off;
	char *end, *sp1, *sp2, *body;
	size_t hlen, blen;
	char path[256];

	/* the head ends at a blank line; refuse anything absurd rather
	 * than buffer it - this port answers scrapes, not uploads */
	end = find_bytes(base, avail, "\r\n\r\n", 4);
	hlen = end ? (size_t)(end - base) + 4 : 0;
	if (!end) {
		char *lf = find_bytes(base, avail, "\n\n", 2);  /* bare LF too */

		if (lf) {
			end = lf;
			hlen = (size_t)(lf - base) + 2;
		}
	}
	if (!end) {
		if (avail >= PC_HTTP_MAX)
			return -1;             /* no head in 8 KB: not a client */
		return 0;                      /* incomplete; wait for more */
	}

	/* "GET /path HTTP/1.x" - and, S123, "POST /reset-stats", the one
	 * mutating route, body-less by contract; anything else is refused
	 * unread */
	int post = 0;

	sp1 = memchr(base, ' ', hlen);
	if (sp1 && (size_t)(sp1 - base) == 4 && !memcmp(base, "POST", 4)) {
		post = 1;
	} else if (!sp1 || (size_t)(sp1 - base) != 3 ||
	        memcmp(base, "GET", 3)) {
		static const char m405[] = "only GET is served here - this door "
			"is read-only since 0.7.0\n";

		http_send(c, "405 Method Not Allowed", "text/plain", m405,
			sizeof m405 - 1);
		return (int)hlen;
	}
	sp2 = memchr(sp1 + 1, ' ', hlen - (size_t)(sp1 + 1 - base));
	if (!sp2 || (size_t)(sp2 - sp1 - 1) >= sizeof path) {
		http_send(c, "414 URI Too Long", "text/plain", "\n", 1);
		return (int)hlen;
	}
	memcpy(path, sp1 + 1, (size_t)(sp2 - sp1 - 1));
	path[sp2 - sp1 - 1] = 0;
	{
		char *q = strchr(path, '?');   /* ?collect[]= and friends */

		if (q)
			*q = 0;
	}

	/* S37: the shared token, when one is configured, guards EVERY
	 * route - /metrics discloses the fleet's shape as surely as
	 * /members does, so exempting it would be a hole with a tidy
	 * name.  Constant-time compare: it crosses the wire in the
	 * clear, which is no reason to leak its prefix by timing too. */
	{
		const char *want = pc_http_token();

		if (want && !http_token_ok(base, hlen, want)) {
			static const char m401[] = "a token is required: "
				"Authorization: Bearer <token>, or the browser's "
				"login - any user, the token as the password\n";

			http_send_ex(c, "401 Unauthorized", "text/plain",
				"WWW-Authenticate: Basic realm=\"perfcached\", "
				"charset=\"UTF-8\"\r\n", m401, sizeof m401 - 1);
			return (int)hlen;
		}
	}

	if (post) {
		/* S407 part 3 (0.7.0): this door is read-only.  The reset is the
		 * native door's reset_stats verb (perfcli reset-stats, the
		 * dashboard's button); the page's POST went with the page. */
		static const char ro[] = "this door is read-only since 0.7.0: "
			"reset-stats is a native-door verb (perfcli -E <enable> "
			"reset-stats, or the dashboard's button)\n";

		http_send_ex(c, "405 Method Not Allowed", "text/plain",
			"Allow: GET, HEAD\r\n", ro, sizeof ro - 1);
		return (int)hlen;
	}
	if (!strcmp(path, "/")) {
		/* S407 part 3 (0.7.0): the page left the daemon - it is served by
		 * perfcached-dashboard over the native door, so a dashboard under
		 * attack costs the daemon nothing.  410, with the pointer. */
		static const char gone[] = "the status page is served by "
			"perfcached-dashboard since 0.7.0 - this door keeps /stats, "
			"/members, /clients, /metrics and /health (GUIDE.md, "
			"\"The dashboard\")\n";

		http_send(c, "410 Gone", "text/plain", gone, sizeof gone - 1);
	} else if (!strcmp(path, "/members")) {
		struct pc_tw t;

		pc_tw_init(&t, NULL, 0);       /* heap: a fleet is not bounded */
		pc_members_tree(&t);
		http_send_tree(c, &t, (size_t)16 * 1024);
	} else if (!strcmp(path, "/clients")) {
		/* S160: this node's connections - the walk CLIENT LIST makes,
		 * with the door, protocol, wire, pending bytes and
		 * subscriptions the RESP line has no field for.  Read-only, and
		 * behind the same allow-list and token as its four siblings. */
		struct pc_tw t;

		pc_tw_init(&t, NULL, 0);
		pc_obs_clients_tree(&t, get_ticks(), 200);
		http_send_tree(c, &t, (size_t)128 * 1024);
	} else if (!strcmp(path, "/stats")) {
		/* S67: the same body the `stats` method returns.  The
		 * dashboard needs collections, both doors' connection
		 * counts and the cluster counters, none of which /members
		 * carries - and a page that cannot see them is how the
		 * HEADROOM column stayed empty and the native clients
		 * stayed invisible. */
		struct pc_tw t;

		if (hc_send(c, &hc_stats, "application/json"))
			return (int)hlen;          /* 0.6.2: built < http_cache_ms ago */
		pc_tw_init(&t, NULL, 0);
		pc_stats_tree(&t, NULL);
		http_send_tree_c(c, &t, (size_t)64 * 1024, &hc_stats);
	} else if (!strcmp(path, "/metrics")) {
		if (hc_send(c, &hc_metrics, "text/plain; version=0.0.4; charset=utf-8"))
			return (int)hlen;
		body = malloc(PC_METRICS_MAX);
		if (!body) {
			http_send(c, "503 Service Unavailable", "text/plain",
				"out of memory\n", 14);
			return (int)hlen;
		}
		blen = pc_metrics_render(body, PC_METRICS_MAX);
		/* the exposition's own content type, so Prometheus and a
		 * human with curl both get what they expect */
		http_send(c, "200 OK",
			"text/plain; version=0.0.4; charset=utf-8", body, blen);
		hc_put(&hc_metrics, body, blen);
		free(body);
	} else if (!strcmp(path, "/health")) {
		/* liveness only: this answers whenever the daemon can run a
		 * worker.  Readiness - whether the node has joined and is
		 * serving - is pc_node_state()'s business and rides
		 * /metrics, because conflating them makes an orchestrator
		 * restart a node that is merely still syncing. */
		http_send(c, "200 OK", "text/plain", "ok\n", 3);
	} else {
		static const char m404[] = "try /stats, /members, /clients, "
			"/metrics or /health\n";

		http_send(c, "404 Not Found", "text/plain", m404, sizeof m404 - 1);
	}
	return (int)hlen;
}

/* 0.6.2: http_rate - see the comment over hc_send */
#define HR_SLOTS 1024                  /* addresses tracked; power of two */
#define HR_PROBE 8
/* the key: an IPv4 address (a dual-stack listener's ::ffff:a.b.c.d too), or
 * an IPv6 client's /64 with the top bit set - one host usually holds a
 * whole /64, so its addresses are one client here (12np IP6-2) */
static struct { uint64_t ip; long long t; long long tok; } hr_tab[HR_SLOTS];

static uint64_t hr_key(const pc_addr *sa)
{
	const unsigned char *b;
	uint64_t k = 0;
	int i;

	if (pc_addr_family(sa) == AF_INET)
		return pc_addr_v4(sa);
	if (pc_addr_family(sa) != AF_INET6)
		return 0;
	b = sa->in6.sin6_addr.s6_addr;
	if (IN6_IS_ADDR_V4MAPPED(&sa->in6.sin6_addr)) {
		uint32_t v4;

		memcpy(&v4, b + 12, 4);
		return v4;
	}
	for (i = 0; i < 8; i++)
		k = k << 8 | b[i];
	return k | 1ULL << 63;
}
static pthread_mutex_t hr_mx = PTHREAD_MUTEX_INITIALIZER;
static long long hr_logged;

int http_rate_ok(const pc_addr *sa)
{
	int rate = pc_http_rate();
	uint64_t ip = hr_key(sa);
	unsigned int h, i, s, found = HR_SLOTS, old = HR_SLOTS;
	long long now, cap;
	int ok;

	if (rate <= 0 || !ip ||
	    (pc_addr_is_loopback(sa) && !http_limit_loopback()))
		return 1;
	cap = 2000LL * rate;                   /* thousandths of a request */
	now = deny_now();
	h = (unsigned int)(((uint32_t)(ip ^ ip >> 32) * 2654435761u) >> 22);
	pthread_mutex_lock(&hr_mx);
	for (i = 0; i < HR_PROBE; i++) {
		s = (h + i) & (HR_SLOTS - 1);
		if (hr_tab[s].t && hr_tab[s].ip == ip) {
			found = s;
			break;
		}
		if (old == HR_SLOTS || hr_tab[s].t < hr_tab[old].t)
			old = s;               /* empty, or the longest unseen */
	}
	if (found == HR_SLOTS) {
		found = old;
		hr_tab[found].ip = ip;
		hr_tab[found].tok = cap;
	} else {
		hr_tab[found].tok += (now - hr_tab[found].t) * rate;
		if (hr_tab[found].tok > cap)
			hr_tab[found].tok = cap;
	}
	hr_tab[found].t = now;
	ok = hr_tab[found].tok >= 1000;
	if (ok)
		hr_tab[found].tok -= 1000;
	pthread_mutex_unlock(&hr_mx);
	if (!ok) {
		unsigned long long n = __atomic_add_fetch(&hr_limited, 1,
			__ATOMIC_RELAXED);
		long long l = __atomic_load_n(&hr_logged, __ATOMIC_RELAXED);
		char a[PC_ADDR_STRLEN];

		if (now - l >= 10000 && __atomic_compare_exchange_n(&hr_logged,
		        &l, now, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED) &&
		    pc_addr_ntop(sa, a, sizeof a))
			LM_NOTICE("http: %s asks more than http_rate = %d a "
				"second - answered 429 (%llu so far; at most one "
				"line in 10 s)\n", a, rate, n);
	}
	return ok;
}
