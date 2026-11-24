/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov
 * Part of libperfd - see lib/LICENSE.  This file must stay
 * free of src/core includes; tools/sync-libperfd.sh exports
 * exactly the MIT set to consumers. */
/* perfd-url.inc - part of perfd.c, #included in order (one translation unit): perfd:// URLs, and a
 * blocking connect over their seeds with a per-process dial backoff. */
#include <compat/dprint.h>            /* lib/shim's (-Ilib/shim): LM_* */

#define URL_BACKOFF_MIN_MS 1000
#define URL_BACKOFF_MAX_MS 30000
#define URL_COL_MAX        64
#define URL_SECRET_MAX     1024

struct perfd_url {
	char *group;                   /* NULL = none */
	char *secret;                  /* NULL = none */
	const char *secv[2];           /* { secret, NULL } for perfd_opts.secrets */
	char col[URL_COL_MAX + 1];
	int nseed;
	struct {
		char host[256];
		int port;
	} seed[PERFD_URL_MAX_SEEDS];
	int route, encrypt, spares, policy;
	int dial_wait_ms, dial_wait_set;   /* dial_wait_ms=, given */
	/* the connect's state, this process's (pid): seeds logged down, the hold after a dial none
	 * answered */
	pid_t pid;
	unsigned int logged;
	int backoff_ms;
	long long hold_until_ms;
	char last_err[256];
};

static perfd_url *url_fail(perfd_url *u, char *err, size_t errcap, const char *fmt, ...)
	__attribute__((format(printf, 4, 5)));

static perfd_url *url_fail(perfd_url *u, char *err, size_t errcap, const char *fmt, ...)
{
	va_list ap;
	int n = 0;

	if (err && errcap) {
		n = snprintf(err, errcap, "perfd url: ");
		if (n >= 0 && (size_t)n < errcap) {
			va_start(ap, fmt);
			vsnprintf(err + n, errcap - (size_t)n, fmt, ap);
			va_end(ap);
		}
	}
	perfd_url_free(u);
	return NULL;
}

static int url_hex(int c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
		c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* @s..@e with %XX decoded into a malloc'd string; NULL on a bad escape, a NUL, or no memory */
static char *url_unescape(const char *s, const char *e, const char **why)
{
	char *out = malloc((size_t)(e - s) + 1), *o = out;

	*why = "out of memory";
	if (!out)
		return NULL;
	while (s < e) {
		if (*s == '%') {
			int hi = e - s >= 3 ? url_hex((unsigned char)s[1]) : -1;
			int lo = hi >= 0 ? url_hex((unsigned char)s[2]) : -1;

			if (hi < 0 || lo < 0 || (hi == 0 && lo == 0)) {
				*why = "a bad %XX escape in the user or secret";
				free(out);
				return NULL;
			}
			*o++ = (char)(hi * 16 + lo);
			s += 3;
			continue;
		}
		*o++ = *s++;
	}
	*o = 0;
	return out;
}

/* digits only, no sign and no leading zero, @lo..@hi: 0, else -1 */
static int url_num(const char *s, const char *e, long lo, long hi, long *out)
{
	long v = 0;
	const char *q;

	if (s == e || e - s > 6 || (*s == '0' && e - s > 1))
		return -1;
	for (q = s; q < e; q++) {
		if (*q < '0' || *q > '9')
			return -1;
		v = v * 10 + (*q - '0');
	}
	if (v < lo || v > hi)
		return -1;
	*out = v;
	return 0;
}

static int url_name_char(int c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		c == '-' || c == '_' || c == '.';
}

/* one "host:port" or "[v6]:port" between @s and @e into seed @i: NULL, else why not */
static const char *url_seed(perfd_url *u, int i, const char *s, const char *e)
{
	const char *hs, *he, *ps;
	long port;
	size_t hl;

	if (s == e)
		return "an empty seed";
	if (*s == '[') {
		struct in6_addr a6;
		char tmp[INET6_ADDRSTRLEN + 1];

		hs = s + 1;
		he = memchr(hs, ']', (size_t)(e - hs));
		if (!he)
			return "an IPv6 seed without its closing ]";
		hl = (size_t)(he - hs);
		if (hl == 0 || hl >= sizeof tmp)
			return "a bad IPv6 address";
		memcpy(tmp, hs, hl);
		tmp[hl] = 0;
		if (inet_pton(AF_INET6, tmp, &a6) != 1)
			return "a bad IPv6 address";
		if (he + 1 == e || he[1] != ':')
			return "a seed without its :port";
		ps = he + 2;
	} else {
		const char *q;

		hs = s;
		he = memchr(s, ':', (size_t)(e - s));
		if (!he)
			return "a seed without its :port";
		hl = (size_t)(he - hs);
		if (hl == 0 || hl > 253)
			return "a seed without a host";
		for (q = hs; q < he; q++)
			if (!url_name_char((unsigned char)*q) || *q == '_')
				return "a host may hold A-Z a-z 0-9 . - only (an IPv6 one goes in [ ])";
		ps = he + 1;
		if (memchr(ps, ':', (size_t)(e - ps)))
			return "an IPv6 seed goes in [ ]";
	}
	if (url_num(ps, e, 1, 65535, &port) != 0)
		return "a port is a number from 1 to 65535";
	memcpy(u->seed[i].host, hs, hl);
	u->seed[i].host[hl] = 0;
	u->seed[i].port = (int)port;
	return NULL;
}

/* one "name=value" between @s and @e; @seen keeps a name from coming twice */
static const char *url_opt(perfd_url *u, const char *s, const char *e, unsigned int *seen)
{
	static const char *const names[] = { "route", "encrypt", "spares", "policy",
		"dial_wait_ms", NULL };
	static const char *const pols[] = { "failover", "rr", "least", "weighted", NULL };
	const char *eq = memchr(s, '=', (size_t)(e - s)), *v;
	size_t nl, vl;
	long n;
	int k;

	if (s == e)
		return "an empty option";
	if (!eq)
		return "an option is name=value";
	nl = (size_t)(eq - s);
	v = eq + 1;
	vl = (size_t)(e - v);
	for (k = 0; names[k]; k++)
		if (strlen(names[k]) == nl && !memcmp(s, names[k], nl))
			break;
	if (!names[k])
		return "an unknown option (route, encrypt, spares, policy, dial_wait_ms)";
	if (*seen & (1u << k))
		return "an option given twice";
	*seen |= 1u << k;
	switch (k) {
	case 0:
	case 1:
		if (vl != 1 || (*v != '0' && *v != '1'))
			return k ? "encrypt is 0 or 1" : "route is 0 or 1";
		*(k ? &u->encrypt : &u->route) = *v - '0';
		return NULL;
	case 2:
		if (vl == 2 && v[0] == '-' && v[1] == '1')
			n = -1;
		else if (url_num(v, e, 0, PERFD_MAX_SPARES, &n) != 0)
			return "spares is -1 (one per member), 0 (none) or up to 8";
		u->spares = (int)n;
		return NULL;
	case 4:
		if (vl == 2 && v[0] == '-' && v[1] == '1')
			n = -1;
		else if (url_num(v, e, 1, 60000, &n) != 0)
			return "dial_wait_ms is -1 (all of connect_timeout) or 1 to 60000";
		u->dial_wait_ms = (int)n;
		u->dial_wait_set = 1;
		return NULL;
	default:
		for (n = 0; pols[n]; n++)
			if (strlen(pols[n]) == vl && !memcmp(v, pols[n], vl)) {
				u->policy = (int)n;
				return NULL;
			}
		return "policy is failover, rr, least or weighted";
	}
}

perfd_url *perfd_url_parse(const char *url, char *err, size_t errcap)
{
	perfd_url *u;
	const char *s, *auth, *aend, *at, *hs, *q, *ce, *why;
	unsigned int seen = 0;
	size_t cl;

	if (err && errcap)
		err[0] = 0;
	if (!url)
		return url_fail(NULL, err, errcap, "no URL");
	u = calloc(1, sizeof *u);
	if (!u)
		return url_fail(NULL, err, errcap, "out of memory");
	u->route = 1;
	u->encrypt = 1;
	u->spares = -1;
	u->policy = PERFD_POLICY_FAILOVER;
	if (strncmp(url, "perfd", 5) != 0)
		return url_fail(u, err, errcap, "the scheme is perfd: or perfd:<group>:");
	s = url + 5;
	if (*s == ':' && strncmp(s, "://", 3) != 0) {
		const char *g = s + 1, *ge = strstr(g, "://");

		if (!ge || ge == g || ge - g > 63)
			return url_fail(u, err, errcap, "a group is 1-63 characters before ://");
		for (q = g; q < ge; q++)
			if (!url_name_char((unsigned char)*q) || *q == '.')
				return url_fail(u, err, errcap, "a group holds A-Z a-z 0-9 _ - only");
		u->group = strndup(g, (size_t)(ge - g));
		if (!u->group)
			return url_fail(u, err, errcap, "out of memory");
		s = ge;
	}
	if (strncmp(s, "://", 3) != 0)
		return url_fail(u, err, errcap, "the scheme is perfd: or perfd:<group>:");
	auth = s + 3;
	aend = strchr(auth, '/');
	if (!aend)
		return url_fail(u, err, errcap, "no /collection");
	/* the user part ends at the last @ before the path; an @ in a secret is %40 */
	at = NULL;
	for (q = auth; q < aend; q++)
		if (*q == '@')
			at = q;
	hs = auth;
	if (at) {
		const char *colon = memchr(auth, ':', (size_t)(at - auth));
		char *user = NULL, *pass = NULL;

		if (colon) {
			user = url_unescape(auth, colon, &why);
			pass = user ? url_unescape(colon + 1, at, &why) : NULL;
			if (!pass) {
				free(user);
				return url_fail(u, err, errcap, "%s", why);
			}
		} else if (!(user = url_unescape(auth, at, &why))) {
			return url_fail(u, err, errcap, "%s", why);
		}
		if (pass && *pass) {
			u->secret = pass;
			free(user);
		} else {
			free(pass);
			u->secret = user;
		}
		if (!*u->secret)
			return url_fail(u, err, errcap, "an empty secret before @");
		if (strlen(u->secret) > URL_SECRET_MAX)
			return url_fail(u, err, errcap, "a secret is at most %d bytes", URL_SECRET_MAX);
		u->secv[0] = u->secret;
		hs = at + 1;
	}
	while (hs <= aend) {
		const char *e = hs;

		/* a comma inside [ ] is not a separator, though no IPv6 address holds one */
		if (*e == '[')
			while (e < aend && *e != ']')
				e++;
		while (e < aend && *e != ',')
			e++;
		if (u->nseed == PERFD_URL_MAX_SEEDS)
			return url_fail(u, err, errcap, "at most %d seeds", PERFD_URL_MAX_SEEDS);
		why = url_seed(u, u->nseed, hs, e);
		if (why)
			return url_fail(u, err, errcap, "%s", why);
		u->nseed++;
		hs = e + 1;
	}
	ce = aend + 1 + strcspn(aend + 1, "?");
	cl = (size_t)(ce - aend - 1);
	if (cl == 0 || cl > URL_COL_MAX)
		return url_fail(u, err, errcap, "a collection name of 1-%d characters after /",
			URL_COL_MAX);
	for (q = aend + 1; q < ce; q++)
		if (!url_name_char((unsigned char)*q))
			return url_fail(u, err, errcap, "a collection name holds A-Z a-z 0-9 _ - . only");
	memcpy(u->col, aend + 1, cl);
	u->col[cl] = 0;
	if (*ce == '?') {
		const char *o = ce + 1;

		for (;;) {
			const char *e = o + strcspn(o, ",&");

			why = url_opt(u, o, e, &seen);
			if (why)
				return url_fail(u, err, errcap, "%s", why);
			if (!*e)
				break;
			o = e + 1;
		}
	}
	return u;
}

void perfd_url_free(perfd_url *u)
{
	if (!u)
		return;
	if (u->secret) {
		sodium_memzero(u->secret, strlen(u->secret));
		free(u->secret);
	}
	free(u->group);
	free(u);
}

const char *perfd_url_collection(const perfd_url *u)
{
	return u ? u->col : NULL;
}

const char *perfd_url_secret(const perfd_url *u)
{
	return u ? u->secret : NULL;
}

const char *perfd_url_group(const perfd_url *u)
{
	return u ? u->group : NULL;
}

int perfd_url_seed_count(const perfd_url *u)
{
	return u ? u->nseed : 0;
}

int perfd_url_seed(const perfd_url *u, int i, const char **host, int *port)
{
	if (!u || i < 0 || i >= u->nseed)
		return -1;
	if (host)
		*host = u->seed[i].host;
	if (port)
		*port = u->seed[i].port;
	return 0;
}

/* a field is written only when the caller's struct holds it (o->size; 0 = this header's) */
#define URL_HAS(o, f) ((o)->size == 0 || \
	offsetof(perfd_opts, f) + sizeof ((perfd_opts *)0)->f <= (o)->size)

void perfd_url_apply(const perfd_url *u, perfd_opts *o)
{
	if (!u || !o)
		return;
	if (URL_HAS(o, route_keys))
		o->route_keys = u->route;
	if (URL_HAS(o, spares))
		o->spares = u->spares == 0 ? PERFD_SPARES_NONE : u->spares;
	if (URL_HAS(o, policy))
		o->policy = u->policy;
	if (URL_HAS(o, encrypt))
		o->encrypt = u->encrypt;
	if (u->secret && URL_HAS(o, secrets))
		o->secrets = u->secv;
	if (u->dial_wait_set && URL_HAS(o, dial_wait_ms))
		o->dial_wait_ms = u->dial_wait_ms;
}

static const char *url_seed_name(const perfd_url *u, int i, char *buf, size_t cap)
{
	snprintf(buf, cap, strchr(u->seed[i].host, ':') ? "[%s]:%d" : "%s:%d",
		u->seed[i].host, u->seed[i].port);
	return buf;
}

perfd_t *perfd_url_connect(perfd_url *u, const perfd_opts *o)
{
	perfd_opts oc;
	long long now;
	char nm[300];
	int i, start, kind = PERFD_E_TRANSPORT;
	pid_t me = getpid();

	if (!u || !u->nseed) {
		errk(NULL, PERFD_E_ARG, "perfd_url_connect: no URL");
		return NULL;
	}
	if (u->pid != me) {                /* a forked child starts clean */
		u->pid = me;
		u->logged = 0;
		u->backoff_ms = 0;
		u->hold_until_ms = 0;
	}
	now = now_ms_lib();
	if (now < u->hold_until_ms) {
		errk(NULL, PERFD_E_BACKOFF, "backing off: no seed answered the last dial (%s); "
			"the next in %lld ms", u->last_err, u->hold_until_ms - now);
		return NULL;
	}
	if (o)
		opts_norm(o, &oc);
	else {
		memset(&oc, 0, sizeof oc);
		oc.size = sizeof oc;
	}
	perfd_url_apply(u, &oc);
	start = (int)(me % u->nseed);
	for (i = 0; i < u->nseed; i++) {
		int s = (start + i) % u->nseed;
		perfd_t *p = perfd_connect(u->seed[s].host, u->seed[s].port, &oc);

		url_seed_name(u, s, nm, sizeof nm);
		if (p) {
			if (u->logged & (1u << s))
				LM_NOTICE("perfd url: seed %s answers again\n", nm);
			u->logged &= ~(1u << s);
			u->backoff_ms = 0;
			u->hold_until_ms = 0;
			return p;
		}
		kind = perfd_error_kind(NULL);
		snprintf(u->last_err, sizeof u->last_err, "%.80s: %.160s", nm, perfd_error(NULL));
		if (!(u->logged & (1u << s)))
			LM_WARN("perfd url: seed %s failed: %s\n", nm, perfd_error(NULL));
		u->logged |= 1u << s;
	}
	u->backoff_ms = !u->backoff_ms ? URL_BACKOFF_MIN_MS :
		u->backoff_ms >= URL_BACKOFF_MAX_MS / 2 ? URL_BACKOFF_MAX_MS : u->backoff_ms * 2;
	u->hold_until_ms = now_ms_lib() + u->backoff_ms;
	errk(NULL, kind, "no seed answered (%d tried; last %s); calls back off %d ms",
		u->nseed, u->last_err, u->backoff_ms);
	return NULL;
}
