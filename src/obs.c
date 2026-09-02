/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* obs.c — see obs.h.  Formats here belong to Redis, not to us: the
 * Grafana Redis datasource parses them, so every emitter mirrors the
 * redis shape byte for byte. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <pthread.h>
#include <arpa/inet.h>

#include "json.h"
#include "daemon.h"                    /* pc_worker_id() */
#include "compat/timer.h"              /* get_ticks() */
#include "obs.h"

#define OBS_CMDS   64                  /* distinct command names per row */
#define OBS_NAME   24
#define OBS_SLOWN  128                 /* slow entries per worker ring */
#define OBS_ARGN   4                   /* argv kept per slow entry */
#define OBS_ARGLEN 40

struct cmdrow {
	char name[OBS_NAME];
	unsigned int nlen;
	unsigned long long calls, usec;
};

struct slowent {
	unsigned long long id;             /* 0 = empty */
	unsigned long long ts;             /* unix seconds */
	unsigned long long usec;
	unsigned int epoch;                /* stale after SLOWLOG RESET */
	int argc;                          /* real argc, pre-truncation */
	char argv[OBS_ARGN][OBS_ARGLEN];
	unsigned int arglen[OBS_ARGN];
	char addr[48];
	char name[OBS_NAME];
};

struct connrow {
	struct connrow *next, *prev;
	unsigned long long id;
	char addr[48];
	int fd, resp_only;
	unsigned int created, last;        /* ticks */
	char name[OBS_NAME];
	unsigned int nlen;
	char lastcmd[OBS_NAME];
	unsigned int lastcmdlen;
};

static int obs_rows;                   /* workers + 1 shared spare */
static struct cmdrow *obs_cmds;        /* [obs_rows][OBS_CMDS] */
static struct slowent *obs_slow;       /* [obs_rows][OBS_SLOWN] */
static unsigned int *obs_slowpos;      /* next write index per row */
static long long obs_slow_thresh;      /* usec; <0 = disabled */
static unsigned long long obs_slow_id = 1;
static unsigned int obs_slow_epoch;

static pthread_mutex_t obs_cl_mu = PTHREAD_MUTEX_INITIALIZER;
static struct connrow *obs_cl_head;
static int obs_cl_n;
static unsigned long long obs_cl_id;

static unsigned long long obs_prev_total;
static unsigned long long obs_inst;

int pc_obs_init(int nworkers, long long slowlog_usec)
{
	obs_rows = nworkers + 1;
	obs_cmds = calloc((size_t)obs_rows * OBS_CMDS, sizeof *obs_cmds);
	obs_slow = calloc((size_t)obs_rows * OBS_SLOWN, sizeof *obs_slow);
	obs_slowpos = calloc((size_t)obs_rows, sizeof *obs_slowpos);
	obs_slow_thresh = slowlog_usec;
	return (obs_cmds && obs_slow && obs_slowpos) ? 0 : -1;
}

static int obs_row(void)
{
	int w = pc_worker_id();

	return (w >= 0 && w < obs_rows - 1) ? w : obs_rows - 1;
}

unsigned long long pc_obs_usec_now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (unsigned long long)t.tv_sec * 1000000 +
		(unsigned long long)(t.tv_nsec / 1000);
}

void pc_obs_cmd(const char *name, size_t nlen, unsigned long long usec)
{
	struct cmdrow *r;
	char lc[OBS_NAME];
	unsigned int i, h;

	if (!obs_cmds || !nlen)
		return;
	if (nlen >= OBS_NAME)
		nlen = OBS_NAME - 1;
	for (i = 0; i < nlen; i++)
		lc[i] = (char)tolower((unsigned char)name[i]);
	h = 5381;
	for (i = 0; i < nlen; i++)
		h = h * 33 + (unsigned char)lc[i];
	r = obs_cmds + (size_t)obs_row() * OBS_CMDS;
	for (i = 0; i < OBS_CMDS; i++) {
		struct cmdrow *e = &r[(h + i) % OBS_CMDS];

		if (!e->nlen) {
			memcpy(e->name, lc, nlen);
			e->nlen = (unsigned int)nlen;
		} else if (e->nlen != nlen || memcmp(e->name, lc, nlen)) {
			continue;
		}
		e->calls++;
		e->usec += usec;
		return;
	}
	/* row full: the 64th distinct name on one worker is dropped -
	 * monitoring, not accounting */
}

void pc_obs_cmdstats(struct pc_jw *w)
{
	/* merge by name across rows; the space is small enough to do the
	 * quadratic thing simply */
	struct cmdrow out[OBS_CMDS * 2];
	int n = 0, row, i, j;

	memset(out, 0, sizeof out);
	for (row = 0; row < obs_rows; row++) {
		struct cmdrow *r = obs_cmds + (size_t)row * OBS_CMDS;

		for (i = 0; i < OBS_CMDS; i++) {
			if (!r[i].nlen)
				continue;
			for (j = 0; j < n; j++)
				if (out[j].nlen == r[i].nlen &&
				        !memcmp(out[j].name, r[i].name,
				                r[i].nlen))
					break;
			if (j == n) {
				if (n == OBS_CMDS * 2)
					continue;
				memcpy(out[n].name, r[i].name, r[i].nlen);
				out[n].nlen = r[i].nlen;
				n++;
			}
			out[j].calls += r[i].calls;
			out[j].usec += r[i].usec;
		}
	}
	for (i = 0; i < n; i++) {
		char line[128];
		int ln = snprintf(line, sizeof line,
			"cmdstat_%.*s:calls=%llu,usec=%llu,"
			"usec_per_call=%.2f\r\n",
			(int)out[i].nlen, out[i].name, out[i].calls,
			out[i].usec, out[i].calls ?
			(double)out[i].usec / (double)out[i].calls : 0.0);

		pc_jw_raw(w, line, (size_t)ln);
	}
}

unsigned long long pc_obs_total_calls(void)
{
	unsigned long long t = 0;
	int row, i;

	for (row = 0; row < obs_rows; row++) {
		struct cmdrow *r = obs_cmds + (size_t)row * OBS_CMDS;

		for (i = 0; i < OBS_CMDS; i++)
			t += r[i].calls;
	}
	return t;
}

void pc_obs_tick_1hz(void)
{
	unsigned long long t = pc_obs_total_calls();

	obs_inst = t - obs_prev_total;
	obs_prev_total = t;
}

unsigned long long pc_obs_inst_ops(void)
{
	return obs_inst;
}

/* ---- client registry --------------------------------------------------- */

void *pc_obs_conn_add(const struct sockaddr *sa, socklen_t slen, int fd,
		int resp_only)
{
	struct connrow *r = calloc(1, sizeof *r);

	if (!r)
		return NULL;
	if (sa && sa->sa_family == AF_INET &&
	        slen >= (socklen_t)sizeof(struct sockaddr_in)) {
		const struct sockaddr_in *in = (const void *)sa;
		char ip[INET_ADDRSTRLEN];

		if (inet_ntop(AF_INET, &in->sin_addr, ip, sizeof ip))
			snprintf(r->addr, sizeof r->addr, "%s:%u", ip,
				(unsigned)ntohs(in->sin_port));
	}
	if (!r->addr[0])
		snprintf(r->addr, sizeof r->addr, "?:0");
	r->fd = fd;
	r->resp_only = resp_only;
	r->created = r->last = get_ticks();
	pthread_mutex_lock(&obs_cl_mu);
	r->id = ++obs_cl_id;
	r->next = obs_cl_head;
	if (obs_cl_head)
		obs_cl_head->prev = r;
	obs_cl_head = r;
	obs_cl_n++;
	pthread_mutex_unlock(&obs_cl_mu);
	return r;
}

void pc_obs_conn_del(void *row)
{
	struct connrow *r = row;

	if (!r)
		return;
	pthread_mutex_lock(&obs_cl_mu);
	if (r->prev)
		r->prev->next = r->next;
	else
		obs_cl_head = r->next;
	if (r->next)
		r->next->prev = r->prev;
	obs_cl_n--;
	pthread_mutex_unlock(&obs_cl_mu);
	free(r);
}

void pc_obs_conn_touch(void *row, const char *cmd, size_t clen,
		unsigned int now_ticks)
{
	struct connrow *r = row;
	size_t i;

	if (!r)
		return;
	r->last = now_ticks;
	if (clen >= OBS_NAME)
		clen = OBS_NAME - 1;
	for (i = 0; i < clen; i++)
		r->lastcmd[i] = (char)tolower((unsigned char)cmd[i]);
	r->lastcmdlen = (unsigned int)clen;
}

void pc_obs_conn_name(void *row, const char *name, size_t nlen)
{
	struct connrow *r = row;

	if (!r)
		return;
	if (nlen >= OBS_NAME)
		nlen = OBS_NAME - 1;
	memcpy(r->name, name, nlen);
	r->nlen = (unsigned int)nlen;
}

int pc_obs_conn_count(void)
{
	return obs_cl_n;
}

void pc_obs_client_list(struct pc_jw *w, unsigned int now_ticks)
{
	struct connrow *r;

	pthread_mutex_lock(&obs_cl_mu);
	for (r = obs_cl_head; r; r = r->next) {
		char line[224];
		int ln = snprintf(line, sizeof line,
			"id=%llu addr=%s fd=%d name=%.*s age=%u idle=%u "
			"flags=N db=0 sub=0 psub=0 multi=-1 cmd=%.*s\n",
			r->id, r->addr, r->fd, (int)r->nlen, r->name,
			now_ticks - r->created,
			now_ticks >= r->last ? now_ticks - r->last : 0,
			r->lastcmdlen ? (int)r->lastcmdlen : 4,
			r->lastcmdlen ? r->lastcmd : "NULL");

		pc_jw_raw(w, line, (size_t)ln);
	}
	pthread_mutex_unlock(&obs_cl_mu);
}

/* ---- slow log ----------------------------------------------------------- */

void pc_obs_slow(char *const *argv, const size_t *argl, int nargs,
		unsigned long long usec, void *connrow)
{
	struct connrow *c = connrow;
	struct slowent *e;
	int row, i;

	if (!obs_slow || obs_slow_thresh < 0 ||
	        usec < (unsigned long long)obs_slow_thresh)
		return;
	row = obs_row();
	e = obs_slow + (size_t)row * OBS_SLOWN +
		(obs_slowpos[row]++ % OBS_SLOWN);
	e->id = __atomic_fetch_add(&obs_slow_id, 1, __ATOMIC_RELAXED);
	e->ts = (unsigned long long)time(NULL);
	e->usec = usec;
	e->epoch = __atomic_load_n(&obs_slow_epoch, __ATOMIC_RELAXED);
	e->argc = nargs;
	for (i = 0; i < nargs && i < OBS_ARGN; i++) {
		size_t n = argl[i] < OBS_ARGLEN - 1 ? argl[i] : OBS_ARGLEN - 1;

		memcpy(e->argv[i], argv[i], n);
		e->arglen[i] = (unsigned int)n;
	}
	e->addr[0] = 0;
	e->name[0] = 0;
	if (c) {
		snprintf(e->addr, sizeof e->addr, "%s", c->addr);
		snprintf(e->name, sizeof e->name, "%.*s", (int)c->nlen,
			c->name);
	}
}

static int slow_cmp(const void *a, const void *b)
{
	const struct slowent *const *x = a, *const *y = b;

	return ((*y)->id > (*x)->id) - ((*y)->id < (*x)->id);
}

static int slow_collect(const struct slowent **out, int max)
{
	unsigned int ep = __atomic_load_n(&obs_slow_epoch, __ATOMIC_RELAXED);
	int row, i, n = 0;

	for (row = 0; row < obs_rows; row++) {
		const struct slowent *r = obs_slow +
			(size_t)row * OBS_SLOWN;

		for (i = 0; i < OBS_SLOWN && n < max; i++)
			if (r[i].id && r[i].epoch == ep)
				out[n++] = &r[i];
	}
	qsort(out, (size_t)n, sizeof *out, slow_cmp);
	return n;
}

void pc_obs_slowlog_get(struct pc_jw *w, int want)
{
	const struct slowent *ents[512];
	char num[32];
	int n, i, j, ln;

	n = slow_collect(ents, (int)(sizeof ents / sizeof ents[0]));
	if (want >= 0 && n > want)
		n = want;
	ln = snprintf(num, sizeof num, "*%d\r\n", n);
	pc_jw_raw(w, num, (size_t)ln);
	for (i = 0; i < n; i++) {
		const struct slowent *e = ents[i];
		int argc = e->argc < OBS_ARGN ? e->argc : OBS_ARGN;

		pc_jw_lit(w, "*6\r\n");
		ln = snprintf(num, sizeof num, ":%llu\r\n", e->id);
		pc_jw_raw(w, num, (size_t)ln);
		ln = snprintf(num, sizeof num, ":%llu\r\n", e->ts);
		pc_jw_raw(w, num, (size_t)ln);
		ln = snprintf(num, sizeof num, ":%llu\r\n", e->usec);
		pc_jw_raw(w, num, (size_t)ln);
		ln = snprintf(num, sizeof num, "*%d\r\n", argc);
		pc_jw_raw(w, num, (size_t)ln);
		for (j = 0; j < argc; j++) {
			ln = snprintf(num, sizeof num, "$%u\r\n",
				e->arglen[j]);
			pc_jw_raw(w, num, (size_t)ln);
			pc_jw_raw(w, e->argv[j], e->arglen[j]);
			pc_jw_lit(w, "\r\n");
		}
		ln = snprintf(num, sizeof num, "$%zu\r\n", strlen(e->addr));
		pc_jw_raw(w, num, (size_t)ln);
		pc_jw_lit(w, e->addr);
		pc_jw_lit(w, "\r\n");
		ln = snprintf(num, sizeof num, "$%zu\r\n", strlen(e->name));
		pc_jw_raw(w, num, (size_t)ln);
		pc_jw_lit(w, e->name);
		pc_jw_lit(w, "\r\n");
	}
}

long long pc_obs_slowlog_len(void)
{
	const struct slowent *ents[512];

	return slow_collect(ents, (int)(sizeof ents / sizeof ents[0]));
}

void pc_obs_slowlog_reset(void)
{
	__atomic_add_fetch(&obs_slow_epoch, 1, __ATOMIC_RELAXED);
}
