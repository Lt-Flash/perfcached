/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* upsentinel.c - an upstream naming a sentinel: one thread a collection asks the sentinels for
 * the master and replicas and makes them its upstreams (upstream_discover = no: shown only). */
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "uprespc.h"
#include "compat/dprint.h"
#include "upstream.h"
#include "upstreamint.h"

#define SEN_REFRESH_MS 10000
#define SEN_TIMEOUT_MS 2000
#define SEN_SLICE_MS 100                /* how soon the thread sees the shutdown */
#define SEN_MAX        8                /* sentinels remembered (uc->sen_known) */

struct sen_addr {
	char host[64];
	int port;
};

static char sen_dir[1024];              /* the state_dir, for the topology file */
static unsigned int topo_seq;           /* each save's own temporary file */

/* a name no file line or resolver takes: empty, or with a space or control character */
static int bad_host(const char *h)
{
	if (!*h)
		return 1;
	for (; *h; h++)
		if ((unsigned char)*h <= ' ')
			return 1;
	return 0;
}

/* the configured seeds' fingerprint: a file written for other seeds is not this one's */
static unsigned long long seeds_sum(struct up_col *uc)
{
	unsigned long long h = 1469598103934665603ULL;
	int i, n = srv_n(uc);
	const char *p;

	for (i = 0; i < n; i++) {
		if (uc->srv[i].origin != UPO_CONFIG)
			continue;
		for (p = uc->srv[i].addr; *p; p++)
			h = (h ^ (unsigned char)*p) * 1099511628211ULL;
		h = (h ^ '\n') * 1099511628211ULL;
	}
	return h;
}

/* a sentinel learned, kept once, under sen_mx */
static void known_add(struct up_col *uc, const char *host, int port)
{
	int k;

	if (port <= 0 || port > 65535 || strlen(host) >= sizeof uc->sen_known[0].host ||
	    bad_host(host))
		return;
	for (k = 0; k < uc->sen_n; k++)
		if (uc->sen_known[k].port == port && !strcmp(uc->sen_known[k].host, host))
			return;
	if (uc->sen_n < SEN_MAX) {
		strcpy(uc->sen_known[uc->sen_n].host, host);
		uc->sen_known[uc->sen_n++].port = port;
	}
}

/* the field @name's value in a flat [name, value, ...] reply, or NULL */
static const struct rv *field(const struct rv *m, const char *name)
{
	int i;
	size_t nl = strlen(name);

	for (i = 0; i + 1 < m->nel; i += 2)
		if (m->el[i].s && m->el[i].len == nl && !memcmp(m->el[i].s, name, nl))
			return &m->el[i + 1];
	return NULL;
}

static int fstr(const struct rv *v, char *out, size_t cap)
{
	if (!v || !v->s || !v->len || v->len >= cap)
		return -1;
	memcpy(out, v->s, v->len);
	out[v->len] = 0;
	return 0;
}

static int fint(const struct rv *v)
{
	return !v ? 0 : v->s ? atoi(v->s) : (int)v->n;
}

/* a SENTINEL REPLICAS entry held down or cut off: not read */
static int held_down(const struct rv *e)
{
	char fl[128];

	fl[0] = 0;
	fstr(field(e, "flags"), fl, sizeof fl);
	return strstr(fl, "s_down") || strstr(fl, "o_down") || strstr(fl, "disconnected");
}

/* send @argc words, read one reply; 0 ok */
static int ask(struct uconn *c, int argc, const char **av, struct rv *v)
{
	size_t al[4];
	int i;

	memset(v, 0, sizeof *v);
	for (i = 0; i < argc; i++)
		al[i] = strlen(av[i]);
	return send_cmd(c, now_ms() + SEN_TIMEOUT_MS, argc, av, al) ||
		read_reply(c, v, now_ms() + SEN_TIMEOUT_MS) ? -1 : 0;
}

/* a replica the sentinels named still in use (not gone, not the master they name) */
static int sen_replicas_in_use(struct up_col *uc)
{
	int i, n = srv_n(uc), sm = __atomic_load_n(&uc->sen_master, __ATOMIC_ACQUIRE);

	for (i = 0; i < n; i++)
		if (uc->srv[i].origin == UPO_SENTINEL && i != sm &&
		    !__atomic_load_n(&uc->srv[i].gone, __ATOMIC_RELAXED))
			return 1;
	return 0;
}

/* another sentinel than @a also holds every replica of @name down: 1; one that does not,
 * or none answering: 0 */
static int sen_none_confirmed(struct up_col *uc, const struct sen_addr *a, const char *name)
{
	struct sen_addr other[SEN_MAX];
	int no, k, i;

	pthread_mutex_lock(&uc->sen_mx);
	for (no = k = 0; k < uc->sen_n; k++)
		if (uc->sen_known[k].port != a->port || strcmp(uc->sen_known[k].host, a->host)) {
			strcpy(other[no].host, uc->sen_known[k].host);
			other[no++].port = uc->sen_known[k].port;
		}
	pthread_mutex_unlock(&uc->sen_mx);
	for (k = 0; k < no; k++) {
		const char *av[3] = { "SENTINEL", "REPLICAS", name };
		struct uconn c;
		struct rv v;
		int up = -1;

		memset(&c, 0, sizeof c);
		c.uc = uc;
		c.fd = -1;
		if (uc_open(&c, other[k].host, other[k].port, now_ms() + SEN_TIMEOUT_MS) != 0)
			continue;
		if (uc->spassword) {
			const char *pv[2] = { "AUTH", uc->spassword };

			if (ask(&c, 2, pv, &v) != 0 || v.t == '-') {
				rv_free(&v);
				uc_drop(&c);
				free(c.buf);
				continue;
			}
			rv_free(&v);
		}
		if (ask(&c, 3, av, &v) == 0 && v.t != '-') {
			for (up = 0, i = 0; i < v.nel; i++)
				up += !held_down(&v.el[i]);
		}
		rv_free(&v);
		uc_drop(&c);
		free(c.buf);
		if (up >= 0)
			return up == 0;
	}
	return 0;
}

/* one round with one sentinel: 0 it answered (the topology applied), -1 not */
static int sen_round(struct up_col *uc, const struct sen_addr *a)
{
	struct uconn c;
	struct rv v;
	char name[128], mh[64], txt[sizeof uc->sen_text];
	int mp, i, n = 0, nrep = 0, seen[PC_UP_MAX_SRV] = { 0 }, use = uc->discover != PC_UP_DISC_NO;
	size_t tl;

	memset(&c, 0, sizeof c);
	c.uc = uc;
	c.fd = -1;
	if (uc_open(&c, a->host, a->port, now_ms() + SEN_TIMEOUT_MS) != 0)   /* TLS too */
		return -1;
	name[0] = 0;
	if (uc->spassword) {
		const char *av[2] = { "AUTH", uc->spassword };

		if (ask(&c, 2, av, &v) != 0 || v.t == '-') {
			rv_free(&v);
			goto fail;
		}
		rv_free(&v);
	}
	{       /* which master: the one watched, or the one the config names */
		const char *av[2] = { "SENTINEL", "MASTERS" };
		char all[128];
		size_t al = 0;

		if (ask(&c, 2, av, &v) != 0)
			goto fail;
		all[0] = 0;
		for (i = 0; i < v.nel; i++) {
			char nm[128];

			if (fstr(field(&v.el[i], "name"), nm, sizeof nm) != 0)
				continue;
			n++;
			if (al < sizeof all)
				al += (size_t)snprintf(all + al, sizeof all - al, "%s%s", n > 1 ? ", " : "", nm);
			if (uc->smaster ? !strcmp(nm, uc->smaster) : n == 1)
				memcpy(name, nm, sizeof nm);
		}
		rv_free(&v);
		if (!uc->smaster && n > 1)
			name[0] = 0;
		if (!name[0]) {
			pthread_mutex_lock(&uc->sen_mx);
			snprintf(uc->sen_text, sizeof uc->sen_text, "%.48s watches %.120s - name one "
				"with upstream_sentinel_master", a->host, n ? all : "no master");
			pthread_mutex_unlock(&uc->sen_mx);
			goto fail;
		}
	}
	{
		const char *av[3] = { "SENTINEL", "GET-MASTER-ADDR-BY-NAME", name };

		if (ask(&c, 3, av, &v) != 0 || v.nel < 2 || fstr(&v.el[0], mh, sizeof mh) != 0) {
			rv_free(&v);
			goto fail;
		}
		mp = fint(&v.el[1]);
		rv_free(&v);
		if (mp <= 0 || mp > 65535 || bad_host(mh))
			goto fail;
	}
	tl = (size_t)snprintf(txt, sizeof txt, "%s: master %s:%d", name, mh, mp);
	i = use ? srv_add(uc, mh, mp, UPO_SENTINEL) : -1;
	if (i >= 0)
		seen[i] = 1;
	__atomic_store_n(&uc->sen_master, i, __ATOMIC_RELEASE);   /* the writer's pick */
	{
		const char *av[3] = { "SENTINEL", "REPLICAS", name };

		if (ask(&c, 3, av, &v) != 0)
			goto fail;
		for (i = 0; i < v.nel; i++) {
			char h[64];
			int p = fint(field(&v.el[i], "port")), k;

			if (fstr(field(&v.el[i], "ip"), h, sizeof h) != 0 || p <= 0 ||
			    held_down(&v.el[i]))
				continue;          /* held down or cut off: not read, gone below */
			nrep++;
			if (tl < sizeof txt)
				tl += (size_t)snprintf(txt + tl, sizeof txt - tl, ", replica %s:%d", h, p);
			if (use && (k = srv_add(uc, h, p, UPO_SENTINEL)) >= 0)
				seen[k] = 1;
		}
		rv_free(&v);
	}
	{       /* the others, so this one need not stay up */
		const char *av[3] = { "SENTINEL", "SENTINELS", name };

		if (ask(&c, 3, av, &v) == 0) {
			pthread_mutex_lock(&uc->sen_mx);
			for (i = 0; i < v.nel; i++) {
				char h[64];

				if (fstr(field(&v.el[i], "ip"), h, sizeof h) == 0)
					known_add(uc, h, fint(field(&v.el[i], "port")));
			}
			pthread_mutex_unlock(&uc->sen_mx);
			rv_free(&v);
		}
	}
	uc_drop(&c);
	free(c.buf);
	pthread_mutex_lock(&uc->sen_mx);
	memcpy(uc->sen_text, txt, sizeof txt);
	pthread_mutex_unlock(&uc->sen_mx);
	/* every replica down in one sentinel's view retires none until another
	 * agrees - one just restarted, or cut off from them, says so of replicas that serve */
	if (use && !nrep && sen_replicas_in_use(uc) && !sen_none_confirmed(uc, a, name)) {
		if (!__atomic_exchange_n(&uc->sen_alone_said, 1, __ATOMIC_RELAXED))
			LM_NOTICE("collection '%s': sentinel %s:%d holds every replica of '%s' down "
				"and no other sentinel agrees - they stay until one does\n", uc->name,
				a->host, a->port, name);
		use = 0;
	} else if (use)
		__atomic_store_n(&uc->sen_alone_said, 0, __ATOMIC_RELAXED);
	if (use) {
		int nn = srv_n(uc);

		for (i = 0; i < nn; i++)
			if (uc->srv[i].origin == UPO_SENTINEL && !seen[i])
				srv_gone(&uc->srv[i]);
	}
	topo_save(uc);
	return 0;
fail:
	uc_drop(&c);
	free(c.buf);
	return -1;
}

/* a subscription to the sentinel that answered - its events (a failover, a
 * server held down or back, a replica or sentinel added) start a round at once */
static int sen_subscribe(struct up_col *uc, struct uconn *sc, const struct sen_addr *a)
{
	const char *ch[] = { "SUBSCRIBE", "+switch-master", "+sdown", "-sdown",
		"+odown", "-odown", "+slave", "+sentinel" };
	size_t al[8];
	struct rv v;
	int i;

	memset(sc, 0, sizeof *sc);
	sc->uc = uc;
	sc->fd = -1;
	if (uc_open(sc, a->host, a->port, now_ms() + SEN_TIMEOUT_MS) != 0)
		return -1;
	if (uc->spassword) {
		const char *av[2] = { "AUTH", uc->spassword };

		if (ask(sc, 2, av, &v) != 0 || v.t == '-') {
			rv_free(&v);
			goto fail;
		}
		rv_free(&v);
	}
	for (i = 0; i < 8; i++)
		al[i] = strlen(ch[i]);
	if (send_cmd(sc, now_ms() + SEN_TIMEOUT_MS, 8, ch, al) != 0)
		goto fail;
	for (i = 1; i < 8; i++) {      /* one confirmation a channel */
		memset(&v, 0, sizeof v);
		if (read_reply(sc, &v, now_ms() + SEN_TIMEOUT_MS) != 0 || v.t == '-') {
			rv_free(&v);
			goto fail;
		}
		rv_free(&v);
	}
	return 0;
fail:
	uc_drop(sc);
	free(sc->buf);
	sc->buf = NULL;
	return -1;
}

static void sen_unsubscribe(struct uconn *sc)
{
	uc_drop(sc);
	free(sc->buf);
	sc->buf = NULL;
}

/* wait for an event or the refresh: 1 an event came (a round now), 0 the time passed */
static int sen_wait(struct up_col *uc, struct uconn *sc)
{
	struct pollfd pf;
	struct rv v;
	char what[208] = "";
	int more = 0, t;

	if (sc->fd < 0) {
		for (t = 0; t < SEN_REFRESH_MS && !uc->stop; t += SEN_SLICE_MS)
			usleep(SEN_SLICE_MS * 1000);
		return 0;
	}
	pf.fd = sc->fd;
	pf.events = POLLIN;
	pf.revents = 0;
	if (!uc_buffered(sc)) {
		int r = 0;

		for (t = 0; t < SEN_REFRESH_MS && !uc->stop && r == 0; t += SEN_SLICE_MS)
			r = poll(&pf, 1, SEN_SLICE_MS);
		if (r <= 0)
			return 0;
	}
	memset(&v, 0, sizeof v);
	if (read_reply(sc, &v, now_ms() + SEN_TIMEOUT_MS) != 0) {
		sen_unsubscribe(sc);   /* closed: the next round dials it again */
		return 1;
	}
	if (v.nel >= 3 && v.el[1].s && v.el[2].s)
		snprintf(what, sizeof what, "%.*s %.*s", (int)(v.el[1].len < 40 ? v.el[1].len : 40),
			v.el[1].s, (int)(v.el[2].len < 160 ? v.el[2].len : 160), v.el[2].s);
	rv_free(&v);
	usleep(50000);                 /* events come in bursts: one round for them all */
	while (more < 64 && (uc_buffered(sc) || poll(&pf, 1, 0) > 0)) {
		memset(&v, 0, sizeof v);
		if (read_reply(sc, &v, now_ms() + SEN_TIMEOUT_MS) != 0) {
			sen_unsubscribe(sc);
			break;
		}
		rv_free(&v);
		more++;
	}
	if (what[0])
		LM_NOTICE("collection '%s': a sentinel says %s%s - asking again\n", uc->name, what,
			more ? " (and more)" : "");
	return 1;
}

static void *sen_main(void *arg)
{
	struct up_col *uc = arg;
	struct sen_addr known[SEN_MAX], subat;
	struct uconn sub;
	int nknown, i, k;

	memset(&sub, 0, sizeof sub);
	memset(&subat, 0, sizeof subat);
	sub.fd = -1;
	while (!uc->stop) {
		int n = srv_n(uc), ok = 0;

		/* the seeds that said sentinel first (a saved file's may be stale), then the
		 * ones learned (and kept) */
		nknown = 0;
		pthread_mutex_lock(&uc->sen_mx);
		for (i = 0; i < n; i++)
			if (__atomic_load_n(&uc->srv[i].is_sentinel, __ATOMIC_RELAXED)) {
				char host[sizeof uc->srv[i].host];
				int port;

				/* a reused slot rewrites its address under run_mx */
				pthread_mutex_lock(&uc->srv[i].run_mx);
				memcpy(host, uc->srv[i].host, sizeof host);
				port = uc->srv[i].port;
				pthread_mutex_unlock(&uc->srv[i].run_mx);
				known_add(uc, host, port);
				if (nknown < SEN_MAX && strlen(host) < sizeof known[0].host) {
					strcpy(known[nknown].host, host);
					known[nknown++].port = port;
				}
			}
		for (i = 0; i < uc->sen_n && nknown < SEN_MAX; i++) {
			for (k = 0; k < nknown; k++)
				if (known[k].port == uc->sen_known[i].port &&
				    !strcmp(known[k].host, uc->sen_known[i].host))
					break;
			if (k == nknown) {
				strcpy(known[nknown].host, uc->sen_known[i].host);
				known[nknown++].port = uc->sen_known[i].port;
			}
		}
		pthread_mutex_unlock(&uc->sen_mx);
		for (k = 0; k < nknown && !ok && !uc->stop; k++)
			ok = sen_round(uc, &known[k]) == 0;
		/* subscribed to the one that answers - one that stopped answering
		 * (frozen, or half-open) sends no events */
		if (ok && sub.fd >= 0 && (subat.port != known[k - 1].port ||
		    strcmp(subat.host, known[k - 1].host)))
			sen_unsubscribe(&sub);
		if (ok && sub.fd < 0 && sen_subscribe(uc, &sub, &known[k - 1]) == 0)
			subat = known[k - 1];
		if (!ok) {
			pthread_mutex_lock(&uc->sen_mx);
			if (!strstr(uc->sen_text, "upstream_sentinel_master"))
				snprintf(uc->sen_text, sizeof uc->sen_text, "no sentinel answered");
			pthread_mutex_unlock(&uc->sen_mx);
			sen_unsubscribe(&sub);
		}
		sen_wait(uc, &sub);
	}
	sen_unsubscribe(&sub);
	return NULL;
}

/* the sentinels and the servers learned, kept in state_dir, so a restart
 * with the seed down tries them; written when it changes */
void topo_save(struct up_col *uc)
{
	char buf[4096], path[1200], tmp[1240];
	size_t bl = 0;
	unsigned long long h = 1469598103934665603ULL;
	int i, n = srv_n(uc);
	FILE *f;

	if (!sen_dir[0])
		return;
	bl = (size_t)snprintf(buf, sizeof buf, "seeds %016llx\n", seeds_sum(uc));
	pthread_mutex_lock(&uc->sen_mx);
	for (i = 0; i < uc->sen_n && bl < sizeof buf; i++)
		bl += (size_t)snprintf(buf + bl, sizeof buf - bl, "sentinel %s %d\n",
			uc->sen_known[i].host, uc->sen_known[i].port);
	pthread_mutex_unlock(&uc->sen_mx);
	for (i = 0; i < n && bl < sizeof buf; i++) {
		char host[sizeof uc->srv[i].host];
		int port;

		if (uc->srv[i].origin == UPO_CONFIG || __atomic_load_n(&uc->srv[i].gone,
		        __ATOMIC_ACQUIRE))
			continue;
		pthread_mutex_lock(&uc->srv[i].run_mx);
		memcpy(host, uc->srv[i].host, sizeof host);
		port = uc->srv[i].port;
		pthread_mutex_unlock(&uc->srv[i].run_mx);
		if (!bad_host(host))
			bl += (size_t)snprintf(buf + bl, sizeof buf - bl, "server %s %d %d\n",
				host, port, uc->srv[i].origin);
	}
	if (bl >= sizeof buf)
		return;                        /* cut short: not written */
	for (i = 0; i < (int)bl; i++)
		h = (h ^ (unsigned char)buf[i]) * 1099511628211ULL;
	if (__atomic_load_n(&uc->topo_sum, __ATOMIC_RELAXED) == h)
		return;
	snprintf(path, sizeof path, "%.1024s/upstream-topology-%.*s", sen_dir, (int)uc->nlen,
		uc->name);
	snprintf(tmp, sizeof tmp, "%.1200s.tmp.%d.%u", path, (int)getpid(),
		__atomic_add_fetch(&topo_seq, 1, __ATOMIC_RELAXED));     /* two savers, two files */
	f = fopen(tmp, "w");
	if (!f)
		return;
	if (fwrite(buf, 1, bl, f) != bl || fclose(f) != 0) {
		unlink(tmp);
		return;
	}
	if (rename(tmp, path) != 0)
		unlink(tmp);
	else
		__atomic_store_n(&uc->topo_sum, h, __ATOMIC_RELAXED);    /* written: not again */
}

void topo_load(struct up_col *uc, const char *state_dir)
{
	char path[1200], line[256], h[64];
	unsigned long long sum;
	int p, o, ns = 0, nv = 0;
	FILE *f;

	snprintf(sen_dir, sizeof sen_dir, "%s", state_dir ? state_dir : "");
	if (!sen_dir[0])
		return;
	snprintf(path, sizeof path, "%.1024s/upstream-topology-%.*s", sen_dir, (int)uc->nlen,
		uc->name);
	f = fopen(path, "r");
	if (!f)
		return;
	if (!fgets(line, sizeof line, f) || sscanf(line, "seeds %llx", &sum) != 1 ||
	    sum != seeds_sum(uc)) {
		LM_NOTICE("collection '%s': %s was written for other upstreams - not used\n",
			uc->name, path);
		fclose(f);
		return;
	}
	while (fgets(line, sizeof line, f)) {
		if (sscanf(line, "sentinel %63s %d", h, &p) == 2) {
			pthread_mutex_lock(&uc->sen_mx);
			known_add(uc, h, p);
			pthread_mutex_unlock(&uc->sen_mx);
			ns++;
		} else if (sscanf(line, "server %63s %d %d", h, &p, &o) == 3 &&
		           uc->discover != PC_UP_DISC_NO && p > 0 && p < 65536 &&
		           (o == UPO_ROLE || o == UPO_SENTINEL || o == UPO_CLUSTER) &&
		           srv_add(uc, h, p, o) >= 0)
			nv++;
	}
	fclose(f);
	if (ns || nv)
		LM_NOTICE("collection '%s': the last topology - %d server(s), %d sentinel(s) - "
			"tried at the start\n", uc->name, nv, ns);
	if (ns)
		sen_start(uc);
}

void sen_start(struct up_col *uc)
{
	if (__atomic_exchange_n(&uc->sen_started, 1, __ATOMIC_ACQ_REL))
		return;
	if (pthread_create(&uc->sen_thr, NULL, sen_main, uc) != 0) {
		LM_ERR("collection '%s': cannot start the sentinels' thread\n", uc->name);
		__atomic_store_n(&uc->sen_started, 0, __ATOMIC_RELEASE);
		return;
	}
	uc->sen_joinable = 1;
}
