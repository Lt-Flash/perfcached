/*
 * perfcached-dashboard - the status page served by its own process
 * (DESIGN 12no, S407/S409, 0.6.6).
 *
 * The page used to be served by the daemon's HTTP door, and every action
 * a dashboard might offer would have widened the daemon's largest attack
 * surface.  This tool serves the same page from a separate process: it is
 * a client of the fleet's NATIVE door through libperfd (Noise end to end,
 * one handle per member), so a dashboard under attack stalls this process
 * and no daemon.  Nothing in the daemon knows it exists.
 *
 * Routes (the page's own, so the page needs no new names):
 *   GET  /                 the page
 *   GET  /stats, /members, /clients   that node's trees as JSON, cached
 *                          cache_ms; ?node=N selects a member, the first
 *                          seed is the default
 *   GET  /session          {"tool":...,"node":N,"nodes":[...],"enabled":b}
 *                          - the page learns it is served by the tool
 *   POST /session          {"secret": enable secret} -> {"token"} after
 *                          perfd_enable on every member; wrong secrets are
 *                          counted per address and locked out
 *   POST /action           {"op": check|upfill|stop, "col": name, "node"}
 *                          -> the daemon's answer; needs the session token
 *   POST /reset-stats      the native reset_stats verb; needs the token
 *
 * The HTTP layer is libmicrohttpd (12no, "The HTTP layer"): the parser, the
 * connection and per-address limits, the request timeout and the memory
 * bound are its options.  What is ours: every POST needs the custom header
 * X-Perfcached-Dashboard (no cross-site form can send one), a JSON body
 * under body_max, application/json; every byte rendered into JSON is
 * escaped; the page is served with the same CSP the daemon computes (the
 * SHA-256 of its one inline script).
 */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <microhttpd.h>
#include <sodium.h>

#include "../lib/perfd.h"
#include "../src/statuspage.h"

#ifndef PC_DASH_VERSION
#define PC_DASH_VERSION PERFD_VERSION
#endif

#define MAX_MEMBERS 64
#define MAX_SEEDS 8
#define MAX_SESSIONS 8
#define MAX_LOCK 64
#define HDR_POST "X-Perfcached-Dashboard"
#define HDR_SESSION "X-Perfcached-Session"

/* ---- configuration ----------------------------------------------------- */
struct cfg {
	char listen_host[128];
	int listen_port;
	char seed_host[MAX_SEEDS][128];
	int seed_port[MAX_SEEDS];
	int nseeds;
	char secret[256];
	int cache_ms;
	int refresh_ms;
	int body_max;
	int timeout_s;
	int lockout_after;
	int lockout_s;
	int session_s;
	int conn_limit;
	int per_ip_limit;
	int verbose;
	unsigned char key[crypto_secretbox_KEYBYTES];  /* S415: session_key */
	int key_set;
	char trusted[MAX_SEEDS][64];      /* S416: proxies whose X-Forwarded-For is believed */
	int ntrusted;
};

static struct cfg CF = {
	.listen_host = "127.0.0.1", .listen_port = 8090,
	.cache_ms = 1000, .refresh_ms = 3000, .body_max = 4096, .timeout_s = 10,
	.lockout_after = 5, .lockout_s = 60, .session_s = 1800,
	.conn_limit = 64, .per_ip_limit = 16,
};

static volatile sig_atomic_t stopping;

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void logmsg(const char *fmt, ...)
{
	va_list ap;
	char tb[32];
	time_t t = time(NULL);
	struct tm tm;

	localtime_r(&t, &tm);
	strftime(tb, sizeof tb, "%Y-%m-%d %H:%M:%S", &tm);
	fprintf(stderr, "%s perfcached-dashboard: ", tb);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* host:port or [v6]:port; 0 ok */
static int parse_hostport(const char *s, char *host, size_t hcap, int *port)
{
	const char *colon;
	size_t n;

	if (*s == '[') {
		const char *rb = strchr(s, ']');

		if (!rb || rb[1] != ':')
			return -1;
		n = (size_t)(rb - s - 1);
		if (n == 0 || n >= hcap)
			return -1;
		memcpy(host, s + 1, n);
		host[n] = 0;
		colon = rb + 1;
	} else {
		colon = strrchr(s, ':');
		if (!colon || colon == s)
			return -1;
		n = (size_t)(colon - s);
		if (n >= hcap)
			return -1;
		memcpy(host, s, n);
		host[n] = 0;
	}
	*port = atoi(colon + 1);
	return *port > 0 && *port < 65536 ? 0 : -1;
}

static int cfg_set(const char *sect, const char *key, const char *val,
		const char *where)
{
	if (strcmp(sect, "dashboard") && strcmp(sect, "")) {
		logmsg("%s: unknown section [%s]", where, sect);
		return -1;
	}
	if (!strcmp(key, "listen")) {
		if (parse_hostport(val, CF.listen_host, sizeof CF.listen_host,
		        &CF.listen_port) != 0) {
			logmsg("%s: listen = %s is not host:port", where, val);
			return -1;
		}
	} else if (!strcmp(key, "seed") || !strcmp(key, "seeds")) {
		char buf[1024], *p, *save = NULL;

		snprintf(buf, sizeof buf, "%s", val);
		for (p = strtok_r(buf, ", \t", &save); p;
		     p = strtok_r(NULL, ", \t", &save)) {
			if (CF.nseeds >= MAX_SEEDS) {
				logmsg("%s: more than %d seeds", where, MAX_SEEDS);
				return -1;
			}
			if (parse_hostport(p, CF.seed_host[CF.nseeds],
			        sizeof CF.seed_host[0], &CF.seed_port[CF.nseeds]) != 0) {
				logmsg("%s: seed %s is not host:port", where, p);
				return -1;
			}
			CF.nseeds++;
		}
	} else if (!strcmp(key, "secret")) {
		snprintf(CF.secret, sizeof CF.secret, "%s", val);
	} else if (!strcmp(key, "cache_ms")) {
		CF.cache_ms = atoi(val);
	} else if (!strcmp(key, "refresh_ms")) {
		CF.refresh_ms = atoi(val);
	} else if (!strcmp(key, "body_max")) {
		CF.body_max = atoi(val);
	} else if (!strcmp(key, "timeout_s")) {
		CF.timeout_s = atoi(val);
	} else if (!strcmp(key, "lockout_after")) {
		CF.lockout_after = atoi(val);
	} else if (!strcmp(key, "lockout_s")) {
		CF.lockout_s = atoi(val);
	} else if (!strcmp(key, "session_s")) {
		CF.session_s = atoi(val);
	} else if (!strcmp(key, "session_key")) {
		size_t kl = 0;

		if (sodium_hex2bin(CF.key, sizeof CF.key, val, strlen(val), NULL,
		        &kl, NULL) != 0 || kl != sizeof CF.key) {
			logmsg("%s: session_key must be %u hex characters (%u random "
				"bytes: head -c %u /dev/urandom | xxd -p -c %u)", where,
				2 * (unsigned)sizeof CF.key, (unsigned)sizeof CF.key,
				(unsigned)sizeof CF.key, (unsigned)sizeof CF.key);
			return -1;
		}
		CF.key_set = 1;
	} else if (!strcmp(key, "connections")) {
		CF.conn_limit = atoi(val);
	} else if (!strcmp(key, "connections_per_address")) {
		CF.per_ip_limit = atoi(val);
	} else if (!strcmp(key, "trusted_proxy") || !strcmp(key, "trusted_proxies")) {
		char buf[1024], *p, *save = NULL;

		snprintf(buf, sizeof buf, "%s", val);
		for (p = strtok_r(buf, ", \t", &save); p;
		     p = strtok_r(NULL, ", \t", &save)) {
			struct in6_addr a6;
			struct in_addr a4;

			if (CF.ntrusted >= MAX_SEEDS) {
				logmsg("%s: more than %d trusted proxies", where, MAX_SEEDS);
				return -1;
			}
			if (inet_pton(AF_INET, p, &a4) != 1 &&
			    inet_pton(AF_INET6, p, &a6) != 1) {
				logmsg("%s: trusted_proxy %s is not an IP address", where, p);
				return -1;
			}
			snprintf(CF.trusted[CF.ntrusted++], sizeof CF.trusted[0], "%s", p);
		}
	} else {
		logmsg("%s: unknown key %s", where, key);
		return -1;
	}
	return 0;
}

/* the same shape as perfcached.conf: [section], key = value, # comments */
static int cfg_load(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[1024], sect[64] = "", where[600];
	int ln = 0;

	if (!f) {
		logmsg("cannot read %s: %s", path, strerror(errno));
		return -1;
	}
	while (fgets(line, sizeof line, f)) {
		char *s = line, *e, *v;

		ln++;
		while (*s == ' ' || *s == '\t')
			s++;
		if (*s == '[') {
			snprintf(sect, sizeof sect, "%.*s", (int)strcspn(s + 1, "]\n"),
				s + 1);
			continue;
		}
		if (*s == '#' || *s == ';' || *s == '\n' || !*s)
			continue;
		e = strchr(s, '=');
		if (!e) {
			logmsg("%s:%d: not key = value", path, ln);
			fclose(f);
			return -1;
		}
		v = e + 1;
		while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
			e--;
		*e = 0;
		while (*v == ' ' || *v == '\t')
			v++;
		v[strcspn(v, "\r\n")] = 0;
		snprintf(where, sizeof where, "%s:%d", path, ln);
		if (cfg_set(sect, s, v, where) != 0) {
			fclose(f);
			return -1;
		}
	}
	fclose(f);
	return 0;
}

/* ---- the fleet: one libperfd handle per member ------------------------- */
struct member {
	char addr[64];
	int port;
	int node;
	int seed;                     /* came from the config, not learned */
	perfd_t *p;
	long long dial_at;            /* last connect attempt */
	char *stats, *members, *clients;
	long long stats_at, members_at, clients_at;
	char err[160];
};

static struct {
	pthread_mutex_t mu;
	struct member m[MAX_MEMBERS];
	int n;
	char enable[256];             /* the session's enable secret, or "" */
	long long learned_at;
} FL = { .mu = PTHREAD_MUTEX_INITIALIZER };

static const char *SECRETS[2];

static perfd_t *dial(const char *host, int port, char *err, size_t ecap)
{
	perfd_opts po;
	perfd_t *p;

	memset(&po, 0, sizeof po);
	po.connect_timeout_ms = 2000;
	po.io_timeout_ms = 5000;
	po.spares = PERFD_SPARES_NONE;   /* this node's trees, nobody else's */
	po.policy = PERFD_POLICY_FAILOVER;
	if (CF.secret[0])
		po.secrets = SECRETS;
	p = perfd_connect(host, port, &po);
	if (!p)
		snprintf(err, ecap, "%s", perfd_error(NULL));
	return p;
}

static struct member *member_add(const char *addr, int port, int node,
		int seed)
{
	struct member *m;
	int i;

	for (i = 0; i < FL.n; i++)
		if (!strcmp(FL.m[i].addr, addr) && FL.m[i].port == port) {
			if (node)
				FL.m[i].node = node;
			return &FL.m[i];
		}
	if (FL.n >= MAX_MEMBERS)
		return NULL;
	m = &FL.m[FL.n++];
	memset(m, 0, sizeof *m);
	snprintf(m->addr, sizeof m->addr, "%s", addr);
	m->port = port;
	m->node = node;
	m->seed = seed;
	return m;
}

/* connect a member that has no handle (rate-limited to one try a second) */
static void member_connect(struct member *m)
{
	long long t = now_ms();

	if (m->p || t - m->dial_at < 1000)
		return;
	m->dial_at = t;
	m->p = dial(m->addr, m->port, m->err, sizeof m->err);
	if (!m->p)
		return;
	m->err[0] = 0;
	if (FL.enable[0] && perfd_enable(m->p, FL.enable) != 0)
		logmsg("%s:%d: enable after reconnect: %s", m->addr, m->port,
			perfd_error(m->p));
}

static void member_drop(struct member *m, const char *why)
{
	if (m->p) {
		perfd_free(m->p);
		m->p = NULL;
	}
	snprintf(m->err, sizeof m->err, "%s", why ? why : "lost");
	free(m->stats); free(m->members); free(m->clients);
	m->stats = m->members = m->clients = NULL;
	m->stats_at = m->members_at = m->clients_at = 0;
}

static const char *member_tree(struct member *m, int which);
static int jget(const char *body, const char *key, char *out, size_t cap);

/* the members tree a connected member answers names every member with
 * "addr", "port", "node" in that order and "self" on its own entry (the
 * daemon's pc_members_tree); the entries are read in that order, so a full
 * JSON parser is not carried for them */
static void learn_from(struct member *src, const char *json)
{
	const char *p = json;

	for (;;) {
		char addr[64], port_s[16], node_s[16];
		const char *a = strstr(p, "\"addr\":"), *next, *q;
		size_t n;
		int port, node, self;

		if (!a)
			return;
		q = a + 7;
		if (*q != '"')
			return;
		q++;
		n = strcspn(q, "\"");
		if (n == 0 || n >= sizeof addr)
			return;
		memcpy(addr, q, n);
		addr[n] = 0;
		next = strstr(q + n, "\"addr\":");
		if (jget(q + n, "port", port_s, sizeof port_s) != 1 ||
		    jget(q + n, "node", node_s, sizeof node_s) != 1)
			return;
		port = atoi(port_s);
		node = atoi(node_s);
		q = strstr(q + n, "\"self\":");
		self = q && (!next || q < next) && !strncmp(q + 7, "true", 4);
		if (port > 0)
			member_add(addr, port, node, 0);
		if (self && node)
			src->node = node;
		p = next ? next : q + n;
		if (!next)
			return;
	}
}

/* every member we know is asked for the member list it holds (cached like
 * the page's own read of it); whoever is named and unknown gets a handle */
static void learn(void)
{
	long long t = now_ms();
	int i;

	if (t - FL.learned_at < CF.refresh_ms)
		return;
	FL.learned_at = t;
	for (i = 0; i < FL.n; i++) {
		struct member *m = &FL.m[i];
		const char *j;

		member_connect(m);
		if (!m->p)
			continue;
		j = member_tree(m, 1);
		if (j)
			learn_from(m, j);
	}
	for (i = 0; i < FL.n; i++)
		member_connect(&FL.m[i]);
}

/* the tree @which of member @m, from the cache or the daemon; NULL with
 * the error in m->err */
static const char *member_tree(struct member *m, int which)
{
	char **slot = which == 0 ? &m->stats : which == 1 ? &m->members
		: &m->clients;
	long long *at = which == 0 ? &m->stats_at : which == 1 ? &m->members_at
		: &m->clients_at;
	long long t = now_ms();
	char *r;

	if (*slot && t - *at < CF.cache_ms)
		return *slot;
	member_connect(m);
	if (!m->p)
		return NULL;
	r = which == 0 ? perfd_stats(m->p, NULL) : which == 1
		? perfd_members(m->p) : perfd_clients(m->p, 200);
	if (!r) {
		int k = perfd_error_kind(m->p);

		snprintf(m->err, sizeof m->err, "%s", perfd_error(m->p));
		if (k == PERFD_E_TRANSPORT || k == PERFD_E_AUTH ||
		    k == PERFD_E_REFUSED)
			member_drop(m, m->err);
		return NULL;
	}
	free(*slot);
	/* perfd's values are freed with perfd_free_value; keep our own copy
	 * so one free() serves every slot */
	*slot = strdup(r);
	perfd_free_value(r);
	*at = t;
	return *slot;
}

static struct member *member_by_node(int node)
{
	int i;

	if (node > 0)
		for (i = 0; i < FL.n; i++)
			if (FL.m[i].node == node)
				return &FL.m[i];
	/* the default: the first seed that answers, then any member that
	 * does (S415: a dead first seed must not be the page's node), then
	 * the first seed so the error names it */
	for (i = 0; i < FL.n; i++)
		if (FL.m[i].seed && FL.m[i].p)
			return &FL.m[i];
	for (i = 0; i < FL.n; i++)
		if (FL.m[i].p)
			return &FL.m[i];
	for (i = 0; i < FL.n; i++)
		if (FL.m[i].seed)
			return &FL.m[i];
	return FL.n ? &FL.m[0] : NULL;
}

/* ---- sessions and the lockout ------------------------------------------ */
struct lock {
	char addr[64];
	int fails;
	long long until;
	long long last;
};
static struct lock LOCKS[MAX_LOCK];

static struct lock *lock_slot(const char *addr)
{
	int i, oldest = 0;

	for (i = 0; i < MAX_LOCK; i++)
		if (!strcmp(LOCKS[i].addr, addr))
			return &LOCKS[i];
	for (i = 1; i < MAX_LOCK; i++)
		if (LOCKS[i].last < LOCKS[oldest].last)
			oldest = i;
	memset(&LOCKS[oldest], 0, sizeof LOCKS[oldest]);
	snprintf(LOCKS[oldest].addr, sizeof LOCKS[oldest].addr, "%s", addr);
	return &LOCKS[oldest];
}

static int locked_out(const char *addr)
{
	struct lock *l = lock_slot(addr);
	long long t = now_ms();

	l->last = t;
	return l->until > t;
}

static void note_failure(const char *addr)
{
	struct lock *l = lock_slot(addr);

	l->last = now_ms();
	if (++l->fails >= CF.lockout_after) {
		l->until = l->last + (long long)CF.lockout_s * 1000;
		l->fails = 0;
		logmsg("%s: %d wrong enable secrets - locked out for %d s", addr,
			CF.lockout_after, CF.lockout_s);
	}
}

static void note_success(const char *addr)
{
	struct lock *l = lock_slot(addr);

	l->fails = 0;
	l->until = 0;
}

/* S415: stateless sessions.  A token is a secretbox, under the instances'
 * shared session_key (a random key from this start when none is
 * configured), of
 *     expiry (8 bytes, unix seconds, big-endian) || the enable secret
 * so any instance holding the key opens it, learns the secret, raises
 * privilege on its own handles once (adopt_secret) and acts: the browser
 * holds the capability, the servers hold nothing, and a load balancer in
 * front of several dashboards needs no stickiness.  Rendered as hex,
 * nonce || box.  Not renewed on use: the page gets 401 at expiry and asks
 * for the secret again. */
#define SECRET_MAX 256
#define TOKEN_BIN_MAX (crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES + 8 + SECRET_MAX)
#define TOKEN_MAX (2 * TOKEN_BIN_MAX + 1)

static void token_make(const char *secret, char *out, size_t cap)
{
	unsigned char m[8 + SECRET_MAX], c[TOKEN_BIN_MAX];
	size_t sl = strlen(secret), ml;
	unsigned long long exp = (unsigned long long)time(NULL) +
		(unsigned long long)CF.session_s;
	int i;

	if (sl >= SECRET_MAX)
		sl = SECRET_MAX - 1;
	ml = 8 + sl;
	for (i = 0; i < 8; i++)
		m[i] = (unsigned char)(exp >> (56 - 8 * i));
	memcpy(m + 8, secret, sl);
	randombytes_buf(c, crypto_secretbox_NONCEBYTES);
	crypto_secretbox_easy(c + crypto_secretbox_NONCEBYTES, m, ml, c, CF.key);
	sodium_bin2hex(out, cap, c, crypto_secretbox_NONCEBYTES +
		crypto_secretbox_MACBYTES + ml);
	sodium_memzero(m, sizeof m);
}

/* 1 with the secret in @secret when @tok is ours and not expired */
static int token_open(const char *tok, char *secret, size_t scap)
{
	unsigned char c[TOKEN_BIN_MAX], m[8 + SECRET_MAX];
	size_t cl = 0, ml, i;
	unsigned long long exp = 0;

	if (!tok || sodium_hex2bin(c, sizeof c, tok, strlen(tok), NULL, &cl,
	        NULL) != 0)
		return 0;
	if (cl < crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES + 9)
		return 0;
	ml = cl - crypto_secretbox_NONCEBYTES - crypto_secretbox_MACBYTES;
	if (crypto_secretbox_open_easy(m, c + crypto_secretbox_NONCEBYTES,
	        cl - crypto_secretbox_NONCEBYTES, c, CF.key) != 0)
		return 0;
	for (i = 0; i < 8; i++)
		exp = (exp << 8) | m[i];
	if ((unsigned long long)time(NULL) >= exp || ml - 8 >= scap) {
		sodium_memzero(m, sizeof m);
		return 0;
	}
	memcpy(secret, m + 8, ml - 8);
	secret[ml - 8] = 0;
	sodium_memzero(m, sizeof m);
	return 1;
}

/* the secret a token carried becomes this instance's: raised on every
 * connected handle once, and on each re-dial after (member_connect) */
static void adopt_secret(const char *secret)
{
	int i;

	pthread_mutex_lock(&FL.mu);
	if (strcmp(FL.enable, secret)) {
		snprintf(FL.enable, sizeof FL.enable, "%s", secret);
		for (i = 0; i < FL.n; i++)
			if (FL.m[i].p && perfd_enable(FL.m[i].p, secret) != 0)
				logmsg("%s:%d: enable from a session token: %s", FL.m[i].addr,
					FL.m[i].port, perfd_error(FL.m[i].p));
		logmsg("privileged session adopted from a token (another instance "
			"issued it, or this one before a restart)");
	}
	pthread_mutex_unlock(&FL.mu);
}

static int any_enabled(void)
{
	return FL.enable[0] != 0;
}

/* ---- tiny JSON helpers ------------------------------------------------- */
struct sb {
	char *s;
	size_t n, cap;
	int oom;
};

static void sb_put(struct sb *b, const char *s, size_t n)
{
	if (b->oom)
		return;
	if (b->n + n + 1 > b->cap) {
		size_t nc = b->cap ? b->cap * 2 : 1024;
		char *ns;

		while (nc < b->n + n + 1)
			nc *= 2;
		ns = realloc(b->s, nc);
		if (!ns) {
			b->oom = 1;
			return;
		}
		b->s = ns;
		b->cap = nc;
	}
	memcpy(b->s + b->n, s, n);
	b->n += n;
	b->s[b->n] = 0;
}

static void sb_str(struct sb *b, const char *s)
{
	sb_put(b, s, strlen(s));
}

static void sb_fmt(struct sb *b, const char *fmt, ...)
{
	char tmp[512];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(tmp, sizeof tmp, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	sb_put(b, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

/* a JSON string literal, every byte escaped that must be */
static void sb_jstr(struct sb *b, const char *s)
{
	sb_put(b, "\"", 1);
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;

		if (c == '"' || c == '\\') {
			char e[2] = { '\\', (char)c };

			sb_put(b, e, 2);
		} else if (c < 0x20 || c == 0x7f || c == '<' || c == '>' ||
		           c == '&') {
			/* control bytes, and the HTML-significant three: the page
			 * escapes what it renders, but a JSON body that is never
			 * HTML-active costs nothing more */
			sb_fmt(b, "\\u%04x", c);
		} else {
			sb_put(b, (const char *)&c, 1);
		}
	}
	sb_put(b, "\"", 1);
}

/* the value of "key" in a flat JSON object body: a string (unescaped into
 * @out) or a number; 1 found, 0 absent, -1 malformed.  The bodies this
 * tool accepts are three keys deep at most, so a full parser is not
 * carried for them. */
static int jget(const char *body, const char *key, char *out, size_t cap)
{
	char pat[80];
	const char *p = body, *q;
	size_t o = 0;

	snprintf(pat, sizeof pat, "\"%s\"", key);
	for (;;) {
		p = strstr(p, pat);
		if (!p)
			return 0;
		q = p + strlen(pat);
		while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')
			q++;
		if (*q == ':')
			break;
		p = q;
	}
	q++;
	while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')
		q++;
	if (*q == '"') {
		q++;
		while (*q && *q != '"') {
			char c = *q++;

			if (c == '\\') {
				char e = *q++;

				if (!e)
					return -1;
				if (e == 'u') {
					unsigned v = 0;
					int i;

					for (i = 0; i < 4; i++) {
						char h = *q++;

						v <<= 4;
						if (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
						else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
						else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10);
						else return -1;
					}
					if (v > 0x7f)
						return -1;      /* ASCII names and secrets only */
					c = (char)v;
				} else {
					c = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r'
						: e == 'b' ? '\b' : e == 'f' ? '\f' : e;
				}
			}
			if (o + 1 >= cap)
				return -1;
			out[o++] = c;
		}
		if (*q != '"')
			return -1;
		out[o] = 0;
		return 1;
	}
	while ((*q >= '0' && *q <= '9') || *q == '-') {
		if (o + 1 >= cap)
			return -1;
		out[o++] = *q++;
	}
	if (!o)
		return -1;
	out[o] = 0;
	return 1;
}

/* ---- responses --------------------------------------------------------- */
static char PAGE_CSP[512];

/* the daemon's rule (proto.c): script-src is the SHA-256 of the page's one
 * inline <script>, so an injected script does not run; connect-src keeps
 * the page talking to this tool only */
static void csp_build(void)
{
	const char *pg = pc_status_page;
	const char *a = strstr(pg, "<script>");
	const char *b = a ? strstr(a, "</script>") : NULL;
	unsigned char h[crypto_hash_sha256_BYTES];
	char b64[64];

	if (!a || !b) {
		snprintf(PAGE_CSP, sizeof PAGE_CSP, "default-src 'none'; style-src "
			"'unsafe-inline'; img-src data:; connect-src 'self'; "
			"base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
		return;
	}
	a += 8;
	crypto_hash_sha256(h, (const unsigned char *)a, (size_t)(b - a));
	sodium_bin2base64(b64, sizeof b64, h, sizeof h,
		sodium_base64_VARIANT_ORIGINAL);
	snprintf(PAGE_CSP, sizeof PAGE_CSP, "default-src 'none'; script-src "
		"'sha256-%s'; style-src 'unsafe-inline'; img-src data:; "
		"connect-src 'self'; base-uri 'none'; form-action 'none'; "
		"frame-ancestors 'none'", b64);
}

static enum MHD_Result send_bytes(struct MHD_Connection *c, unsigned status,
		const char *ctype, const char *body, size_t blen, int is_page)
{
	struct MHD_Response *r = MHD_create_response_from_buffer(blen,
		(void *)body, MHD_RESPMEM_MUST_COPY);
	enum MHD_Result rc;

	if (!r)
		return MHD_NO;
	MHD_add_response_header(r, "Content-Type", ctype);
	MHD_add_response_header(r, "X-Content-Type-Options", "nosniff");
	MHD_add_response_header(r, "Referrer-Policy", "no-referrer");
	MHD_add_response_header(r, "X-Frame-Options", "DENY");
	MHD_add_response_header(r, "Cache-Control", "no-store");
	if (is_page)
		MHD_add_response_header(r, "Content-Security-Policy", PAGE_CSP);
	rc = MHD_queue_response(c, status, r);
	MHD_destroy_response(r);
	return rc;
}

static enum MHD_Result send_json(struct MHD_Connection *c, unsigned status,
		const char *json)
{
	return send_bytes(c, status, "application/json", json, strlen(json), 0);
}

static enum MHD_Result send_err(struct MHD_Connection *c, unsigned status,
		const char *msg)
{
	struct sb b = { 0 };
	enum MHD_Result rc;

	sb_str(&b, "{\"error\":");
	sb_jstr(&b, msg);
	sb_str(&b, "}");
	rc = b.oom ? MHD_NO : send_json(c, status, b.s);
	free(b.s);
	return rc;
}

static void client_addr(struct MHD_Connection *c, char *out, size_t cap)
{
	const union MHD_ConnectionInfo *ci = MHD_get_connection_info(c,
		MHD_CONNECTION_INFO_CLIENT_ADDRESS);
	const struct sockaddr *sa = ci ? ci->client_addr : NULL;
	int i;

	snprintf(out, cap, "?");
	if (!sa)
		return;
	if (sa->sa_family == AF_INET)
		inet_ntop(AF_INET, &((const struct sockaddr_in *)sa)->sin_addr, out,
			(socklen_t)cap);
	else if (sa->sa_family == AF_INET6)
		inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)sa)->sin6_addr,
			out, (socklen_t)cap);
	/* S416: behind a trusted proxy the client is the LAST address the
	 * proxy appended to X-Forwarded-For (the one it saw itself; earlier
	 * entries are whatever the client sent) - so the lockout counts per
	 * browser, not per proxy.  Any other peer's header is ignored. */
	for (i = 0; i < CF.ntrusted; i++)
		if (!strcmp(CF.trusted[i], out)) {
			const char *xff = MHD_lookup_connection_value(c,
				MHD_HEADER_KIND, "X-Forwarded-For");
			const char *last;
			size_t n;

			if (!xff || !*xff)
				return;
			last = strrchr(xff, ',');
			last = last ? last + 1 : xff;
			while (*last == ' ' || *last == '\t')
				last++;
			n = strcspn(last, " \t");
			if (n == 0 || n >= cap)
				return;
			memcpy(out, last, n);
			out[n] = 0;
			return;
		}
}

/* ---- the routes -------------------------------------------------------- */
static int node_arg(struct MHD_Connection *c)
{
	const char *v = MHD_lookup_connection_value(c, MHD_GET_ARGUMENT_KIND,
		"node");

	return v ? atoi(v) : 0;
}

static enum MHD_Result route_tree(struct MHD_Connection *c, int which)
{
	struct member *m;
	const char *t;
	char err[400];
	enum MHD_Result rc;

	pthread_mutex_lock(&FL.mu);
	learn();
	m = member_by_node(node_arg(c));
	if (!m) {
		pthread_mutex_unlock(&FL.mu);
		return send_err(c, 503, "no member known - check the seeds");
	}
	t = member_tree(m, which);
	if (!t) {
		snprintf(err, sizeof err, "node %s:%d: %s", m->addr, m->port,
			m->err[0] ? m->err : "no answer");
		pthread_mutex_unlock(&FL.mu);
		return send_err(c, 502, err);
	}
	rc = send_bytes(c, 200, "application/json", t, strlen(t), 0);
	pthread_mutex_unlock(&FL.mu);
	return rc;
}

static enum MHD_Result route_session_get(struct MHD_Connection *c)
{
	struct sb b = { 0 };
	struct member *cur;
	enum MHD_Result rc;
	int i;

	pthread_mutex_lock(&FL.mu);
	learn();
	cur = member_by_node(node_arg(c));
	sb_fmt(&b, "{\"tool\":\"perfcached-dashboard\",\"version\":\"%s\","
		"\"node\":%d,\"enabled\":%s,\"actions\":true,\"nodes\":[",
		PC_DASH_VERSION, cur ? cur->node : 0, any_enabled() ? "true" : "false");
	for (i = 0; i < FL.n; i++) {
		struct member *m = &FL.m[i];

		sb_fmt(&b, "%s{\"node\":%d,\"addr\":", i ? "," : "", m->node);
		sb_jstr(&b, m->addr);
		sb_fmt(&b, ",\"port\":%d,\"state\":", m->port);
		sb_jstr(&b, m->p ? "connected" : m->err[0] ? m->err : "dialling");
		sb_str(&b, "}");
	}
	sb_str(&b, "]}");
	pthread_mutex_unlock(&FL.mu);
	rc = b.oom ? MHD_NO : send_json(c, 200, b.s);
	free(b.s);
	return rc;
}

/* POST /session {"secret": ...}: enable on every member we hold; one that
 * refuses the secret fails the whole session (the fleet shares one) */
static enum MHD_Result route_session_post(struct MHD_Connection *c,
		const char *body, const char *from)
{
	char secret[SECRET_MAX], tok[TOKEN_MAX];
	struct sb b = { 0 };
	enum MHD_Result rc;
	int i, ok = 0, refused = 0;
	char why[200] = "";

	if (jget(body, "secret", secret, sizeof secret) != 1 || !secret[0])
		return send_err(c, 400, "a JSON body with \"secret\" is needed");
	pthread_mutex_lock(&FL.mu);
	learn();
	for (i = 0; i < FL.n; i++) {
		struct member *m = &FL.m[i];

		member_connect(m);
		if (!m->p)
			continue;
		if (perfd_enable(m->p, secret) == 0) {
			ok++;
		} else {
			refused++;
			snprintf(why, sizeof why, "%s", perfd_error(m->p));
		}
	}
	if (!ok || refused) {
		pthread_mutex_unlock(&FL.mu);
		sodium_memzero(secret, sizeof secret);
		note_failure(from);
		logmsg("%s: enable refused (%d ok, %d refused): %s", from, ok,
			refused, why[0] ? why : "no member reachable");
		return send_err(c, 401, why[0] ? why : "no member reachable");
	}
	snprintf(FL.enable, sizeof FL.enable, "%s", secret);
	token_make(secret, tok, sizeof tok);
	sodium_memzero(secret, sizeof secret);
	note_success(from);
	logmsg("%s: privileged session opened on %d members", from, ok);
	sb_str(&b, "{\"token\":\"");
	sb_str(&b, tok);
	sb_fmt(&b, "\",\"expires_s\":%d,\"members\":%d}", CF.session_s, ok);
	sodium_memzero(tok, sizeof tok);
	pthread_mutex_unlock(&FL.mu);
	rc = b.oom ? MHD_NO : send_json(c, 200, b.s);
	free(b.s);
	return rc;
}

static enum MHD_Result route_action(struct MHD_Connection *c,
		const char *body, const char *from)
{
	char op[16], col[64], nodes[16];
	struct member *m;
	char *r;
	int what, node = 0;
	enum MHD_Result rc;

	if (jget(body, "op", op, sizeof op) != 1)
		return send_err(c, 400, "\"op\" is needed: check, upfill or stop");
	if (jget(body, "col", col, sizeof col) != 1 || !col[0])
		return send_err(c, 400, "\"col\" is needed");
	if (jget(body, "node", nodes, sizeof nodes) == 1)
		node = atoi(nodes);
	if (!strcmp(op, "check"))
		what = PERFD_SWEEP_CHECK;
	else if (!strcmp(op, "upfill"))
		what = PERFD_SWEEP_RUN;
	else if (!strcmp(op, "stop"))
		what = PERFD_SWEEP_STOP;
	else
		return send_err(c, 400, "\"op\" is check, upfill or stop");
	pthread_mutex_lock(&FL.mu);
	learn();
	m = member_by_node(node);
	if (!m) {
		pthread_mutex_unlock(&FL.mu);
		return send_err(c, 503, "no member known");
	}
	member_connect(m);
	if (!m->p) {
		char err[400];

		snprintf(err, sizeof err, "node %s:%d: %s", m->addr, m->port,
			m->err[0] ? m->err : "not connected");
		pthread_mutex_unlock(&FL.mu);
		return send_err(c, 502, err);
	}
	r = perfd_upfill(m->p, col, what);
	if (!r) {
		char err[300];

		snprintf(err, sizeof err, "%s", perfd_error(m->p));
		logmsg("%s: %s %s on node %d (%s:%d) refused: %s", from, op, col,
			m->node, m->addr, m->port, err);
		pthread_mutex_unlock(&FL.mu);
		return send_err(c, perfd_error_kind(m->p) == PERFD_E_SERVER ? 409
			: 502, err);
	}
	logmsg("%s: %s %s on node %d (%s:%d): %s", from, op, col, m->node,
		m->addr, m->port, r);
	m->stats_at = 0;                 /* the next poll sees the sweep */
	pthread_mutex_unlock(&FL.mu);
	rc = send_bytes(c, 200, "application/json", r, strlen(r), 0);
	perfd_free_value(r);
	return rc;
}

static enum MHD_Result route_reset(struct MHD_Connection *c, const char *from)
{
	struct member *m;
	char *r;
	enum MHD_Result rc;

	pthread_mutex_lock(&FL.mu);
	learn();
	m = member_by_node(node_arg(c));
	if (!m) {
		pthread_mutex_unlock(&FL.mu);
		return send_err(c, 503, "no member known");
	}
	member_connect(m);
	if (!m->p) {
		pthread_mutex_unlock(&FL.mu);
		return send_err(c, 502, "not connected");
	}
	r = perfd_command(m->p, "reset_stats", NULL);
	if (!r) {
		char err[300];

		snprintf(err, sizeof err, "%s", perfd_error(m->p));
		pthread_mutex_unlock(&FL.mu);
		return send_err(c, 409, err);
	}
	logmsg("%s: reset_stats on node %d (%s:%d)", from, m->node, m->addr,
		m->port);
	m->stats_at = 0;
	pthread_mutex_unlock(&FL.mu);
	rc = send_bytes(c, 200, "application/json", r, strlen(r), 0);
	perfd_free_value(r);
	return rc;
}

/* ---- the request handler ----------------------------------------------- */
struct req {
	char *body;
	size_t n;
	int too_big;
};

static void req_done(void *cls, struct MHD_Connection *c, void **con_cls,
		enum MHD_RequestTerminationCode code)
{
	struct req *q = *con_cls;

	(void)cls; (void)c; (void)code;
	if (q) {
		if (q->body)
			sodium_memzero(q->body, q->n);   /* a secret may have been in it */
		free(q->body);
		free(q);
	}
	*con_cls = NULL;
}

static enum MHD_Result handle(void *cls, struct MHD_Connection *c,
		const char *url, const char *method, const char *version,
		const char *upload, size_t *upload_n, void **con_cls)
{
	struct req *q = *con_cls;
	char from[64];
	int is_post = !strcmp(method, "POST");
	int is_get = !strcmp(method, "GET") || !strcmp(method, "HEAD");

	(void)cls; (void)version;
	if (!q) {
		q = calloc(1, sizeof *q);
		if (!q)
			return MHD_NO;
		*con_cls = q;
		if (is_post) {
			/* the gates a POST passes before its body is read */
			const char *h = MHD_lookup_connection_value(c,
				MHD_HEADER_KIND, HDR_POST);
			const char *ct = MHD_lookup_connection_value(c,
				MHD_HEADER_KIND, "Content-Type");

			if (!h)
				return send_err(c, 403, "a request header " HDR_POST
					" is required on every POST");
			if (!ct || strncmp(ct, "application/json", 16))
				return send_err(c, 415, "application/json only");
		}
		return MHD_YES;
	}
	if (is_post && *upload_n) {
		if (q->n + *upload_n > (size_t)CF.body_max) {
			q->too_big = 1;
		} else {
			char *nb = realloc(q->body, q->n + *upload_n + 1);

			if (!nb)
				return MHD_NO;
			q->body = nb;
			memcpy(q->body + q->n, upload, *upload_n);
			q->n += *upload_n;
			q->body[q->n] = 0;
		}
		*upload_n = 0;
		return MHD_YES;
	}
	client_addr(c, from, sizeof from);
	if (is_get) {
		if (!strcmp(url, "/"))
			return send_bytes(c, 200, "text/html; charset=utf-8",
				pc_status_page, pc_status_page_len, 1);
		if (!strcmp(url, "/stats"))
			return route_tree(c, 0);
		if (!strcmp(url, "/members"))
			return route_tree(c, 1);
		if (!strcmp(url, "/clients"))
			return route_tree(c, 2);
		if (!strcmp(url, "/session"))
			return route_session_get(c);
		if (!strcmp(url, "/health")) {
			/* S415: what a load balancer's check needs - 503 when this
			 * dashboard sees no member at all */
			char hb[96];
			int i, n, conn = 0;

			pthread_mutex_lock(&FL.mu);
			learn();
			n = FL.n;
			for (i = 0; i < FL.n; i++)
				if (FL.m[i].p)
					conn++;
			pthread_mutex_unlock(&FL.mu);
			snprintf(hb, sizeof hb, "{\"ok\":%s,\"members\":%d,"
				"\"connected\":%d}", conn ? "true" : "false", n, conn);
			return send_json(c, conn ? 200 : 503, hb);
		}
		return send_err(c, 404, "no such route");
	}
	if (!is_post)
		return send_err(c, 405, "GET or POST");
	if (q->too_big)
		return send_err(c, 413, "the body is over body_max");
	if (!q->body)
		return send_err(c, 400, "a JSON body is needed");
	if (!strcmp(url, "/session")) {
		if (locked_out(from))
			return send_err(c, 429, "too many wrong secrets from this "
				"address - try again later");
		return route_session_post(c, q->body, from);
	}
	{
		char sec[SECRET_MAX];

		if (!token_open(MHD_lookup_connection_value(c, MHD_HEADER_KIND,
		        HDR_SESSION), sec, sizeof sec))
			return send_err(c, 401, "no privileged session - enter the "
				"enable secret first");
		adopt_secret(sec);
		sodium_memzero(sec, sizeof sec);
	}
	if (!strcmp(url, "/action"))
		return route_action(c, q->body, from);
	if (!strcmp(url, "/reset-stats"))
		return route_reset(c, from);
	return send_err(c, 404, "no such route");
}

static void mhd_log(void *cls, const char *fmt, va_list ap)
{
	char buf[512];

	(void)cls;
	vsnprintf(buf, sizeof buf, fmt, ap);
	buf[strcspn(buf, "\n")] = 0;
	logmsg("http: %s", buf);
}

static void on_signal(int sig)
{
	(void)sig;
	stopping = 1;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: perfcached-dashboard [-f conf] [-l host:port] [-s seed] [-a secret] [-v]\n"
		"  -f conf      /etc/perfcached-dashboard.conf by default\n"
		"  -l host:port where to serve (127.0.0.1:8090; [::]:8090 for dual-stack)\n"
		"  -s seed      a member's native door, host:port (repeatable)\n"
		"  -a secret    the client secret (or PERFCLI_AUTH)\n"
		"  -V           version\n"
		"The page is at http://host:port/ - this process is a client of the\n"
		"fleet's native door; no daemon is changed or restarted by it.\n");
}

int main(int argc, char **argv)
{
	const char *conf = NULL;
	struct MHD_Daemon *d;
	unsigned flags = MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_ERROR_LOG;
	struct sockaddr_storage ss;
	struct sockaddr_in *s4 = (struct sockaddr_in *)&ss;
	struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&ss;
	int i, nseeds_cli = 0;

	if (sodium_init() < 0) {
		logmsg("libsodium failed to initialise");
		return 1;
	}
	for (i = 1; i < argc; i++) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;

		if (!strcmp(a, "-f") && v) { conf = v; i++; }
		else if (!strcmp(a, "-V")) {
			printf("perfcached-dashboard %s (libperfd %s, libmicrohttpd %s)\n",
				PC_DASH_VERSION, perfd_version(), MHD_get_version());
			return 0;
		}
		else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
		else if (!strcmp(a, "-l") || !strcmp(a, "-s") || !strcmp(a, "-a") ||
		         !strcmp(a, "-v")) { if (a[1] != 'v') i++; }
		else { usage(); return 2; }
	}
	if (conf || access("/etc/perfcached-dashboard.conf", R_OK) == 0)
		if (cfg_load(conf ? conf : "/etc/perfcached-dashboard.conf") != 0)
			return 1;
	/* flags override the file; -s replaces its seeds */
	for (i = 1; i < argc; i++) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;

		if (!strcmp(a, "-f")) { i++; continue; }
		if (!strcmp(a, "-v")) { CF.verbose = 1; continue; }
		if (!v) break;
		if (!strcmp(a, "-l")) {
			if (cfg_set("dashboard", "listen", v, "-l") != 0) return 2;
		} else if (!strcmp(a, "-s")) {
			if (!nseeds_cli++) CF.nseeds = 0;
			if (cfg_set("dashboard", "seed", v, "-s") != 0) return 2;
		} else if (!strcmp(a, "-a")) {
			snprintf(CF.secret, sizeof CF.secret, "%s", v);
		}
		i++;
	}
	if (!CF.secret[0] && getenv("PERFCLI_AUTH"))
		snprintf(CF.secret, sizeof CF.secret, "%s", getenv("PERFCLI_AUTH"));
	if (!CF.nseeds) {
		logmsg("no seed: give a member's native door with -s host:port or "
			"seed = in the config");
		return 2;
	}
	if (CF.body_max < 64 || CF.body_max > 1 << 20) {
		logmsg("body_max must be 64..1048576");
		return 2;
	}
	SECRETS[0] = CF.secret;
	SECRETS[1] = NULL;
	if (!CF.key_set)
		randombytes_buf(CF.key, sizeof CF.key);  /* S415: this start's own */
	for (i = 0; i < CF.nseeds; i++)
		member_add(CF.seed_host[i], CF.seed_port[i], 0, 1);
	csp_build();

	memset(&ss, 0, sizeof ss);
	if (inet_pton(AF_INET6, CF.listen_host, &s6->sin6_addr) == 1) {
		s6->sin6_family = AF_INET6;
		s6->sin6_port = htons((uint16_t)CF.listen_port);
		flags |= MHD_USE_IPv6;
		if (IN6_IS_ADDR_UNSPECIFIED(&s6->sin6_addr))
			flags |= MHD_USE_DUAL_STACK;
	} else if (inet_pton(AF_INET, CF.listen_host, &s4->sin_addr) == 1) {
		s4->sin_family = AF_INET;
		s4->sin_port = htons((uint16_t)CF.listen_port);
	} else {
		logmsg("listen = %s: an IP address is needed (a name is not resolved "
			"here on purpose)", CF.listen_host);
		return 2;
	}
	flags |= MHD_USE_EPOLL;
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);

	d = MHD_start_daemon(flags, (uint16_t)CF.listen_port, NULL, NULL,
		&handle, NULL,
		MHD_OPTION_EXTERNAL_LOGGER, &mhd_log, NULL,  /* first: MHD asks */
		MHD_OPTION_SOCK_ADDR, (struct sockaddr *)&ss,
		MHD_OPTION_CONNECTION_TIMEOUT, (unsigned)CF.timeout_s,
		MHD_OPTION_CONNECTION_LIMIT, (unsigned)CF.conn_limit,
		MHD_OPTION_PER_IP_CONNECTION_LIMIT, (unsigned)CF.per_ip_limit,
		MHD_OPTION_CONNECTION_MEMORY_LIMIT, (size_t)(32 * 1024),
		MHD_OPTION_THREAD_POOL_SIZE, (unsigned)2,
		MHD_OPTION_NOTIFY_COMPLETED, &req_done, NULL,
		MHD_OPTION_END);
	if (!d) {
		logmsg("cannot listen on %s:%d (%s)", CF.listen_host, CF.listen_port,
			strerror(errno));
		return 1;
	}
	logmsg("serving http://%s%s%s:%d/ - %d seed%s, libperfd %s, "
		"libmicrohttpd %s", strchr(CF.listen_host, ':') ? "[" : "",
		CF.listen_host, strchr(CF.listen_host, ':') ? "]" : "",
		CF.listen_port, CF.nseeds, CF.nseeds == 1 ? "" : "s", perfd_version(),
		MHD_get_version());
	pthread_mutex_lock(&FL.mu);
	learn();
	for (i = 0; i < FL.n; i++)
		if (FL.m[i].p)
			logmsg("member node %d at %s:%d connected", FL.m[i].node,
				FL.m[i].addr, FL.m[i].port);
		else
			logmsg("member %s:%d: %s", FL.m[i].addr, FL.m[i].port, FL.m[i].err);
	pthread_mutex_unlock(&FL.mu);
	printf("perfcached-dashboard ready on %d\n", CF.listen_port);
	fflush(stdout);
	while (!stopping)
		usleep(200000);
	MHD_stop_daemon(d);
	pthread_mutex_lock(&FL.mu);
	for (i = 0; i < FL.n; i++)
		member_drop(&FL.m[i], "stopping");
	sodium_memzero(FL.enable, sizeof FL.enable);
	pthread_mutex_unlock(&FL.mu);
	logmsg("stopped");
	return 0;
}
