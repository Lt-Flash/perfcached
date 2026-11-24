/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov
 * Part of libperfd - see lib/LICENSE.  This file must stay
 * free of src/core includes; tools/sync-libperfd.sh exports
 * exactly the MIT set to consumers. */
/* perfd-apool.inc - the async pool, #included by perfd.c after perfd-async.inc (one translation
 * unit): one event-loop handle per in-flight GET, pinned to the key's owner or a seed. */
#include <compat/dprint.h>        /* lib/shim's (-Ilib/shim): LM_WARN */

#define APOOL_MAX_SEEDS   8
#define APOOL_MAX_CONNS   1024
#define APOOL_BACKOFF_MIN 1000
#define APOOL_BACKOFF_MAX 30000
#define APOOL_DIAL_WAIT_MS 500     /* dial_wait_ms 0: a get waits this long for a dial */

struct pd_aslot {
	perfd_t *pd;                   /* READY once warmed; NULL = a hole */
	char host[46];                 /* the member it is pinned to, "" = a seed */
	int port;
	perfd_reply_cb cb;             /* the caller's, while a request is in flight */
	void *ctx;
	int busy;                      /* one request in flight per handle */
	int dead;                      /* cancelled: rebuilt at the next get, never reused */
	unsigned int seq, fired_seq;   /* which request's callback ran last */
	long long used_ms;             /* the eviction picks the least recently used */
	long long dial_ms;             /* still connecting since, 0 = settled */
	int seed;                      /* the seed a "" dial went to, -1 = none */
};

struct perfd_apool {
	pid_t pid;                     /* whose sockets these are */
	perfd_opts o;                  /* the caller's, normalised; secrets and name ours */
	char *sec[PERFD_MAX_SECRETS + 1];
	char *name;
	char col[256];
	char seed_host[APOOL_MAX_SEEDS][256];
	int seed_port[APOOL_MAX_SEEDS];
	unsigned char seed_said[APOOL_MAX_SEEDS];   /* its failure logged this outage */
	unsigned char seed_pend[APOOL_MAX_SEEDS];   /* a dial to it still connecting */
	long long seed_hold[APOOL_MAX_SEEDS];       /* skipped until then: it failed */
	int seed_back[APOOL_MAX_SEEDS];
	long long router_dial_ms;      /* the router still connecting since */
	int router_seed;
	int nseeds;
	int route;                     /* opts.route_keys */
	perfd_t *router;               /* learns the fleet for the owner lookup */
	struct pd_aslot *slot;
	int max, n;
	long long next_dial_ms;        /* no dial before this: backing off */
	int backoff_ms;
	struct perfd_apool_stats st;
};

static int apool_connect_ms(const perfd_apool *pool)
{
	return pool->o.connect_timeout_ms > 0 ? pool->o.connect_timeout_ms : DEF_TIMEOUT_MS;
}

/* how long a get waits on a dial: dial_wait_ms, never past connect_timeout */
static int apool_wait_ms(const perfd_apool *pool)
{
	int c = apool_connect_ms(pool), w = pool->o.dial_wait_ms;

	if (w < 0 || (w > 0 && w > c))
		return c;
	return w ? w : (APOOL_DIAL_WAIT_MS < c ? APOOL_DIAL_WAIT_MS : c);
}

/* turn @pd's socket until @done or @deadline; 0 done, -1 failed or late */
static int apool_drive(perfd_t *pd, long long deadline,
		int (*done)(perfd_t *, void *), void *arg)
{
	while (!done(pd, arg)) {
		struct pollfd pf;
		int ev = perfd_events(pd), rc;
		long long left = deadline - now_ms_lib();

		if (perfd_state(pd) == PERFD_ST_FAILED || pd->fd < 0)
			return -1;
		if (left < 0)                  /* past it: one look at what has arrived */
			left = 0;
		pf.fd = pd->fd;
		pf.events = (short)((ev & PERFD_EV_WRITE ? POLLOUT : 0) | POLLIN);
		pf.revents = 0;
		rc = poll(&pf, 1, left > 1000000 ? 1000000 : (int)left);
		if (rc < 0 && errno == EINTR)
			continue;
		if (rc <= 0)
			return -1;
		if ((pf.revents & POLLOUT) && perfd_write_ready(pd) < 0)
			return -1;
		/* ERR/HUP/NVAL: the read reports the failure */
		if ((pf.revents & ~POLLOUT) && perfd_read_ready(pd) < 0)
			return -1;
	}
	return 0;
}

static int apool_not_connecting(perfd_t *pd, void *arg)
{
	(void)arg;
	return perfd_state(pd) != PERFD_ST_CONNECTING;
}

static int apool_learned(perfd_t *pd, void *arg)
{
	(void)arg;
	return pd->learned_async && !pd->learning;
}

static void apool_dial_result(perfd_apool *pool, int ok)
{
	int i;

	if (ok) {
		pool->backoff_ms = 0;
		pool->next_dial_ms = 0;
		for (i = 0; i < pool->nseeds; i++)
			pool->seed_said[i] = 0;
		return;
	}
	pool->st.dial_failures++;
	pool->backoff_ms = pool->backoff_ms ? pool->backoff_ms * 2 : APOOL_BACKOFF_MIN;
	if (pool->backoff_ms > APOOL_BACKOFF_MAX)
		pool->backoff_ms = APOOL_BACKOFF_MAX;
	pool->next_dial_ms = now_ms_lib() + pool->backoff_ms;
}

static void apool_seed_ok(perfd_apool *pool, int i)
{
	if (i < 0)
		return;
	pool->seed_pend[i] = 0;
	pool->seed_back[i] = 0;
	pool->seed_hold[i] = 0;
	pool->seed_said[i] = 0;
}

static void apool_seed_fail(perfd_apool *pool, int i)
{
	if (i < 0)
		return;
	pool->seed_pend[i] = 0;
	pool->seed_back[i] = pool->seed_back[i] ? pool->seed_back[i] * 2 : APOOL_BACKOFF_MIN;
	if (pool->seed_back[i] > APOOL_BACKOFF_MAX)
		pool->seed_back[i] = APOOL_BACKOFF_MAX;
	pool->seed_hold[i] = now_ms_lib() + pool->seed_back[i];
	if (!pool->seed_said[i]) {
		pool->seed_said[i] = 1;
		LM_WARN("async pool: seed %s:%d did not answer (%s)\n", pool->seed_host[i],
			pool->seed_port[i], perfd_error(NULL));
	}
}

/* dial, waiting at most apool_wait_ms: 1 READY, 0 still connecting (*out), -1 failed */
static int apool_start(perfd_apool *pool, const char *host, int port, const perfd_opts *o,
		perfd_t **out)
{
	int ms = apool_connect_ms(pool), w = apool_wait_ms(pool);
	perfd_t *pd;

	*out = NULL;
	if (o->secrets)
		(void)perfd_psk_prepare(o->secrets);   /* derived once, outside the wait */
	pool->st.dials++;
	if (!(pd = perfd_connect_async(host, port, o)))
		return -1;
	apool_drive(pd, now_ms_lib() + w, apool_not_connecting, NULL);
	if (perfd_state(pd) == PERFD_ST_READY) {
		*out = pd;
		return 1;
	}
	if (perfd_state(pd) == PERFD_ST_CONNECTING && pd->fd >= 0 && w < ms) {
		*out = pd;
		errk(NULL, PERFD_E_TRANSPORT, "%s:%d: still connecting", host, port);
		return 0;
	}
	if (perfd_error_kind(pd) != PERFD_E_NONE)
		errk(NULL, perfd_error_kind(pd), "%s:%d: %s", host, port, perfd_error(pd));
	else
		errk(NULL, PERFD_E_TRANSPORT, "%s:%d: not ready within %d ms", host, port, ms);
	perfd_free(pd);
	return -1;
}

/* a parked dial turned without blocking: 1 READY, 0 still connecting, -1 failed (freed) */
static int apool_settle(perfd_apool *pool, perfd_t *pd, long long since, int seed,
		const char *host, int port)
{
	struct pollfd pf;
	int ms = apool_connect_ms(pool);

	if (perfd_state(pd) == PERFD_ST_CONNECTING && pd->fd >= 0) {
		pf.fd = pd->fd;
		pf.events = (short)((perfd_events(pd) & PERFD_EV_WRITE ? POLLOUT : 0) | POLLIN);
		pf.revents = 0;
		if (poll(&pf, 1, 0) > 0) {
			if (pf.revents & POLLOUT)
				(void)perfd_write_ready(pd);
			if ((pf.revents & ~POLLOUT) && perfd_state(pd) == PERFD_ST_CONNECTING)
				(void)perfd_read_ready(pd);
		}
	}
	if (perfd_state(pd) == PERFD_ST_READY) {
		apool_seed_ok(pool, seed);
		apool_dial_result(pool, 1);
		return 1;
	}
	if (perfd_state(pd) == PERFD_ST_CONNECTING && pd->fd >= 0 &&
	    now_ms_lib() - since < ms)
		return 0;
	if (perfd_error_kind(pd) != PERFD_E_NONE)
		errk(NULL, perfd_error_kind(pd), "%s:%d: %s", host, port, perfd_error(pd));
	else
		errk(NULL, PERFD_E_TRANSPORT, "%s:%d: not ready within %d ms", host, port, ms);
	perfd_free(pd);
	if (seed >= 0)
		apool_seed_fail(pool, seed);
	else
		LM_WARN("async pool: %s\n", perfd_error(NULL));
	apool_dial_result(pool, 0);
	return -1;
}

/* the seeds from a start rotated by the pid, so a pre-forked server's processes spread; one
 * failed, held or already being dialled is passed over.  READY, connecting (*seed) or NULL */
static perfd_t *apool_dial_seeds(perfd_apool *pool, const perfd_opts *o, int *seed)
{
	int start = (int)(getpid() % pool->nseeds), k, tried = 0;
	long long now = now_ms_lib();

	*seed = -1;
	for (k = 0; k < pool->nseeds; k++) {
		int i = (start + k) % pool->nseeds, r;
		perfd_t *pd;

		if (pool->seed_pend[i] || pool->seed_hold[i] > now)
			continue;
		tried++;
		r = apool_start(pool, pool->seed_host[i], pool->seed_port[i], o, &pd);
		if (r < 0) {
			apool_seed_fail(pool, i);
			continue;
		}
		*seed = i;
		if (r == 0)
			pool->seed_pend[i] = 1;
		else
			apool_seed_ok(pool, i);
		return pd;
	}
	if (!tried) {
		*seed = -2;                    /* nothing dialled: no failure to count */
		errk(NULL, PERFD_E_TRANSPORT, "backing off: every seed failed or is still connecting");
	}
	return NULL;
}

/* a handle for one target - pinned, no fleet, no standbys; dials share the pool's backoff.
 * *pending: it is still connecting, parked by the caller */
static perfd_t *apool_dial(perfd_apool *pool, const char *host, int port, int *pending,
		int *seed)
{
	perfd_opts o = pool->o;
	perfd_t *pd = NULL;
	long long now = now_ms_lib();
	int r;

	*pending = 0;
	*seed = -1;
	if (now < pool->next_dial_ms) {
		errk(NULL, PERFD_E_TRANSPORT, "backing off: no dial for %lld ms",
			pool->next_dial_ms - now);
		return NULL;
	}
	o.spares = PERFD_SPARES_NONE;
	o.route_keys = 0;
	o.route_hints = 0;
	o.pin = 1;
	if (host[0]) {
		r = apool_start(pool, host, port, &o, &pd);
		if (r < 0)
			LM_WARN("async pool: %s\n", perfd_error(NULL));
	} else {
		pd = apool_dial_seeds(pool, &o, seed);
		if (!pd && *seed == -2) {
			*seed = -1;
			return NULL;
		}
		r = !pd ? -1 : perfd_state(pd) == PERFD_ST_READY ? 1 : 0;
	}
	if (r == 0) {
		*pending = 1;
		return pd;
	}
	apool_dial_result(pool, r > 0);
	return pd;
}

/* the router: an event-loop handle on a seed that learns the member list (again at refresh_ms
 * and on pushes); turned without blocking at each get, its first list waited for */
static perfd_t *apool_router(perfd_apool *pool)
{
	perfd_t *r = pool->router;
	struct pollfd pf;

	if (r && pool->router_dial_ms) {
		int st = apool_settle(pool, r, pool->router_dial_ms, pool->router_seed,
			pool->seed_host[pool->router_seed], pool->seed_port[pool->router_seed]);

		if (st < 0)
			r = pool->router = NULL;
		if (st <= 0)
			return NULL;
		pool->router_dial_ms = 0;
	}
	if (r && perfd_state(r) == PERFD_ST_FAILED) {
		perfd_free(r);
		r = pool->router = NULL;
	}
	if (!r) {
		perfd_opts o = pool->o;
		int seed;

		if (now_ms_lib() < pool->next_dial_ms)
			return NULL;
		o.spares = PERFD_SPARES_NONE;
		o.route_keys = 1;
		o.route_hints = 0;
		o.pin = 0;
		r = apool_dial_seeds(pool, &o, &seed);
		if (!r && seed == -2)
			return NULL;
		if (r && perfd_state(r) != PERFD_ST_READY) {
			pool->router = r;      /* parked: settled at a later get */
			pool->router_dial_ms = now_ms_lib();
			pool->router_seed = seed;
			return NULL;
		}
		apool_dial_result(pool, r != NULL);
		if (!r)
			return NULL;
		pool->router = r;
		apool_drive(r, now_ms_lib() + apool_wait_ms(pool), apool_learned, NULL);
		if (perfd_state(r) == PERFD_ST_FAILED)
			return NULL;
		return r;
	}
	pf.fd = r->fd;
	pf.events = (short)((perfd_events(r) & PERFD_EV_WRITE ? POLLOUT : 0) | POLLIN);
	pf.revents = 0;
	if (poll(&pf, 1, 0) > 0) {
		if ((pf.revents & POLLOUT) && perfd_write_ready(r) < 0)
			return NULL;
		if ((pf.revents & ~POLLOUT) && perfd_read_ready(r) < 0)
			return NULL;
	}
	if (perfd_state(r) == PERFD_ST_READY) {
		learn_members_async(r);
		perfd_push(r);
	}
	return r;
}

/* the owner of @key into @host/@port; "" when the fleet is not routed */
static void apool_owner(perfd_apool *pool, const char *key, char *host, int *port)
{
	perfd_t *r;
	int ix;

	host[0] = 0;
	*port = 0;
	if (!pool->route || !(r = apool_router(pool)) || perfd_state(r) != PERFD_ST_READY)
		return;
	ix = route_owner(r, pool->col, key);
	if (ix < 0 || !r->mem[ix].addr[0] || r->mem[ix].port <= 0)
		return;
	snprintf(host, 46, "%s", r->mem[ix].addr);
	*port = r->mem[ix].port;
}

static void apool_clear(struct pd_aslot *s)
{
	perfd_free(s->pd);             /* fires no callback */
	memset(s, 0, sizeof *s);
}

/* an idle handle whose node went away says so only when read: look before reusing it */
static int apool_alive(struct pd_aslot *s)
{
	struct pollfd pf = { s->pd->fd, POLLIN, 0 };

	if (perfd_state(s->pd) != PERFD_ST_READY)
		return 0;
	if (poll(&pf, 1, 0) > 0 && perfd_read_ready(s->pd) < 0)
		return 0;
	return perfd_state(s->pd) == PERFD_ST_READY;
}

/* the inherited sockets are the parent's: dropped without a word on them, no callback fired */
static void apool_forked(perfd_apool *pool)
{
	int i;

	if (pool->pid == getpid())
		return;
	for (i = 0; i < pool->n; i++)
		apool_clear(&pool->slot[i]);
	pool->n = 0;
	perfd_free(pool->router);
	pool->router = NULL;
	pool->router_dial_ms = 0;
	memset(pool->seed_pend, 0, sizeof pool->seed_pend);
	memset(pool->seed_hold, 0, sizeof pool->seed_hold);
	memset(pool->seed_back, 0, sizeof pool->seed_back);
	pool->pid = getpid();
	pool->backoff_ms = 0;
	pool->next_dial_ms = 0;
	pool->st.resets++;
}

/* a free, warm handle for the target: its own, a hole, or an idle one of another target */
static struct pd_aslot *apool_slot(perfd_apool *pool, const char *host, int port)
{
	struct pd_aslot *s;
	int i, hole = -1, idle = -1, pending;

	for (i = 0; i < pool->n; i++) {
		s = &pool->slot[i];
		if (s->busy)
			continue;
		if (s->pd && s->dial_ms) {
			int st = apool_settle(pool, s->pd, s->dial_ms, s->seed,
				s->seed >= 0 ? pool->seed_host[s->seed] : s->host,
				s->seed >= 0 ? pool->seed_port[s->seed] : s->port);

			if (st < 0) {
				s->pd = NULL;          /* freed by the settle */
				apool_clear(s);
			} else if (st == 0) {
				if (s->seed < 0 && s->port == port && !strcmp(s->host, host)) {
					errk(NULL, PERFD_E_TRANSPORT, "%s:%d: still connecting",
						host, port);
					return NULL;
				}
				continue;              /* neither a hole nor idle; a seed's: try another */
			} else
				s->dial_ms = 0;
		}
		if (s->pd && (s->dead || perfd_state(s->pd) != PERFD_ST_READY)) {
			apool_clear(s);
			pool->st.rebuilt++;
		}
		if (!s->pd) {
			if (hole < 0)
				hole = i;
			continue;
		}
		if (s->port == port && !strcmp(s->host, host)) {
			if (apool_alive(s))
				return s;
			apool_clear(s);
			pool->st.rebuilt++;
			if (hole < 0)
				hole = i;
			continue;
		}
		if (idle < 0 || s->used_ms < pool->slot[idle].used_ms)
			idle = i;
	}
	if (hole < 0 && pool->n < pool->max)
		hole = pool->n++;
	if (hole < 0 && idle >= 0) {
		apool_clear(&pool->slot[idle]);
		pool->st.evicted++;
		hole = idle;
	}
	if (hole < 0) {
		errk(NULL, PERFD_E_STATE, "every handle of the pool (%d) has a request in flight",
			pool->max);
		return NULL;
	}
	s = &pool->slot[hole];
	s->pd = apool_dial(pool, host, port, &pending, &s->seed);
	if (!s->pd)
		return NULL;
	snprintf(s->host, sizeof s->host, "%s", host);
	s->port = port;
	if (pending) {                     /* this get fails; a later one finds it settled */
		s->dial_ms = now_ms_lib();
		return NULL;
	}
	return s;
}

static struct pd_aslot *apool_by_fd(perfd_apool *pool, int fd)
{
	int i;

	if (fd < 0)
		return NULL;
	for (i = 0; i < pool->n; i++)
		if (pool->slot[i].busy && pool->slot[i].pd && pool->slot[i].pd->fd == fd)
			return &pool->slot[i];
	return NULL;
}

/* the library's callback: the slot is free again before the caller's runs */
static void apool_fire(char *result, size_t len, const char *errmsg, void *ctx)
{
	struct pd_aslot *s = ctx;
	perfd_reply_cb cb = s->cb;
	void *uctx = s->ctx;

	s->cb = NULL;
	s->ctx = NULL;
	s->busy = 0;
	s->fired_seq = s->seq;
	if (cb)
		cb(result, len, errmsg, uctx);
	else
		free(result);
}

/* ---- public ------------------------------------------------------------ */

perfd_apool *perfd_apool_new_seeds(const char *const *hosts, const int *ports,
		int nseeds, const char *col, const perfd_opts *opts, int max_conns)
{
	static const perfd_opts defaults = { .size = sizeof(perfd_opts) };
	perfd_opts oc;
	const perfd_opts *o = opts ? opts_norm(opts, &oc) : &defaults;
	perfd_apool *pool;
	int i, nsec = 0;

	if (!hosts || !ports || nseeds < 1 || nseeds > APOOL_MAX_SEEDS || !col || !*col ||
	    strlen(col) > 255 || max_conns < 1 || max_conns > APOOL_MAX_CONNS) {
		errk(NULL, PERFD_E_ARG, "async pool: 1 to %d seeds, a collection and 1 to %d "
			"connections", APOOL_MAX_SEEDS, APOOL_MAX_CONNS);
		return NULL;
	}
	for (i = 0; i < nseeds; i++)
		if (!hosts[i] || !*hosts[i] || strlen(hosts[i]) > 255 || ports[i] < 1 ||
		    ports[i] > 65535) {
			errk(NULL, PERFD_E_ARG, "async pool: seed %d is not a host and a port 1-65535", i);
			return NULL;
		}
	if (!(pool = calloc(1, sizeof *pool)) ||
	    !(pool->slot = calloc((size_t)max_conns, sizeof *pool->slot)))
		goto nomem;
	pool->o = *o;
	pool->o.size = sizeof pool->o;
	while (o->secrets && o->secrets[nsec] && nsec < PERFD_MAX_SECRETS) {
		if (!(pool->sec[nsec] = strdup(o->secrets[nsec])))
			goto nomem;
		nsec++;
	}
	pool->o.secrets = nsec ? (const char *const *)pool->sec : NULL;
	if (o->name && !(pool->name = strdup(o->name)))
		goto nomem;
	pool->o.name = pool->name;
	snprintf(pool->col, sizeof pool->col, "%s", col);
	for (i = 0; i < nseeds; i++) {
		snprintf(pool->seed_host[i], sizeof pool->seed_host[i], "%s", hosts[i]);
		pool->seed_port[i] = ports[i];
	}
	pool->nseeds = nseeds;
	pool->route = o->route_keys != 0 && !o->pin;
	pool->max = max_conns;
	pool->pid = getpid();
	return pool;
nomem:
	perfd_apool_free(pool);
	errk(NULL, PERFD_E_NOMEM, "out of memory");
	return NULL;
}

void perfd_apool_free(perfd_apool *pool)
{
	int i;

	if (!pool)
		return;
	if (pool->slot)
		for (i = 0; i < pool->n; i++)
			apool_clear(&pool->slot[i]);
	perfd_free(pool->router);
	for (i = 0; pool->sec[i]; i++) {
		sodium_memzero(pool->sec[i], strlen(pool->sec[i]));
		free(pool->sec[i]);
	}
	free(pool->name);
	free(pool->slot);
	free(pool);
}

int perfd_apool_get(perfd_apool *pool, const char *key, perfd_reply_cb cb, void *ctx)
{
	char host[46];
	int port;
	struct pd_aslot *s;
	unsigned int seq;

	if (!pool || !key || !cb)
		return errk(NULL, PERFD_E_ARG, "async pool: a pool, a key and a callback");
	apool_forked(pool);
	pool->st.gets++;
	apool_owner(pool, key, host, &port);
	if (!(s = apool_slot(pool, host, port)))
		return -1;
	s->used_ms = now_ms_lib();
	s->cb = cb;
	s->ctx = ctx;
	s->busy = 1;
	seq = ++s->seq;
	if (perfd_submit_kv(s->pd, PERFD_V_GET, pool->col, key, NULL, 0, 0, 0,
	        apool_fire, s) != 0) {
		/* not registered: the callback is not called */
		s->busy = 0;
		s->cb = NULL;
		s->ctx = NULL;
		return errk(NULL, perfd_error_kind(s->pd), "%s", perfd_error(s->pd));
	}
	/* a failed write fires the callback with the error */
	if (perfd_push(s->pd) == 0 && s->busy && (perfd_events(s->pd) & PERFD_EV_WRITE)) {
		long long until = now_ms_lib() + s->pd->io_ms;

		while (s->busy && (perfd_events(s->pd) & PERFD_EV_WRITE)) {
			struct pollfd pf = { s->pd->fd, POLLOUT, 0 };
			long long left = until - now_ms_lib();

			if (left <= 0 || poll(&pf, 1, (int)left) <= 0 ||
			    perfd_write_ready(s->pd) < 0)
				break;
		}
	}
	if (s->fired_seq == seq)
		return PERFD_APOOL_DONE;
	return s->pd->fd;
}

int perfd_apool_ready(perfd_apool *pool, int fd)
{
	struct pd_aslot *s;
	unsigned int seq;

	if (!pool)
		return -1;
	apool_forked(pool);
	if (!(s = apool_by_fd(pool, fd)))
		return -1;
	seq = s->seq;
	perfd_read_ready(s->pd);       /* a failure fires the callback with it */
	if (s->fired_seq != seq && (perfd_events(s->pd) & PERFD_EV_WRITE))
		perfd_write_ready(s->pd);
	return s->fired_seq == seq;
}

static int apool_fired(perfd_t *pd, void *arg)
{
	struct pd_aslot *s = arg;

	(void)pd;
	return s->fired_seq == s->seq;
}

int perfd_apool_wait(perfd_apool *pool, int fd, int timeout_ms)
{
	struct pd_aslot *s;

	if (!pool)
		return -1;
	apool_forked(pool);
	if (!(s = apool_by_fd(pool, fd)))
		return -1;
	apool_drive(s->pd, now_ms_lib() + (timeout_ms > 0 ? timeout_ms : 0), apool_fired, s);
	if (s->fired_seq == s->seq)
		return 1;
	perfd_apool_cancel(pool, fd);
	return 0;
}

void perfd_apool_cancel(perfd_apool *pool, int fd)
{
	struct pd_aslot *s;
	int i;

	if (!pool)
		return;
	apool_forked(pool);
	if (!(s = apool_by_fd(pool, fd)))
		return;
	/* the late reply must find nobody: the call forgets the caller, the handle is never read again */
	for (i = 0; i < s->pd->ncalls; i++)
		s->pd->calls[i].cb = NULL;
	s->cb = NULL;
	s->ctx = NULL;
	s->busy = 0;
	s->dead = 1;
	pool->st.cancelled++;
}

int perfd_apool_node(perfd_apool *pool, int fd, char *addr, size_t acap, int *port)
{
	struct pd_aslot *s;

	if (!pool || pool->pid != getpid() || !(s = apool_by_fd(pool, fd)))
		return -1;
	if (addr && acap)
		snprintf(addr, acap, "%s", s->pd->ahost);
	if (port)
		*port = s->pd->aport;
	return 0;
}

int perfd_apool_stats(perfd_apool *pool, struct perfd_apool_stats *st)
{
	int i;

	if (!pool || !st)
		return -1;
	apool_forked(pool);
	*st = pool->st;
	st->slots = st->busy = 0;
	for (i = 0; i < pool->n; i++) {
		st->slots += pool->slot[i].pd != NULL;
		st->busy += pool->slot[i].busy;
	}
	st->routing = pool->route && pool->router && perfd_routing(pool->router);
	return 0;
}

/* the proposal's form: the seeds, collection and options a perfd:// URL names (the URL applied over @o) */
perfd_apool *perfd_apool_new(perfd_url *u, const perfd_opts *o, int max_conns)
{
	const char *hosts[PERFD_URL_MAX_SEEDS];
	int ports[PERFD_URL_MAX_SEEDS], n, i;
	perfd_opts oc;

	if (!u)
		return NULL;
	n = perfd_url_seed_count(u);
	for (i = 0; i < n && i < PERFD_URL_MAX_SEEDS; i++)
		if (perfd_url_seed(u, i, &hosts[i], &ports[i]) != 0)
			return NULL;
	perfd_opts_init_sz(&oc, sizeof oc);
	if (o)
		memcpy(&oc, o, o->size && o->size < sizeof oc ? o->size : sizeof oc);
	oc.size = sizeof oc;
	perfd_url_apply(u, &oc);
	return perfd_apool_new_seeds(hosts, ports, i, perfd_url_collection(u), &oc, max_conns);
}
