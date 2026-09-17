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
#include "compat/dprint.h"             /* LM_INFO for the query log */
#include "obs.h"
#include "fnv1a.h"                     /* the one FNV-1a */

#define OBS_CMDS   64                  /* distinct command names per row */
#define OBS_NAME   PC_OBS_NAME          /* S159: shared with obs.h */
#define OBS_MERGE  (OBS_CMDS * 2)      /* merged rows a reader keeps */
#define OBS_SLOWN  128                 /* slow entries per worker ring */
#define OBS_ARGN   4                   /* argv kept per slow entry */
#define OBS_ARGLEN 40

struct cmdrow {
	char name[OBS_NAME];
	unsigned int nlen;
	unsigned long long calls, usec;
	unsigned long long hist[PC_OBS_HIST];   /* S159: log2 latency buckets */
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
	unsigned long long cmds;           /* S70: requests this connection carried */
	/* S160: for /clients */
	const char *door, *dialect;        /* "native" | "resp" | "http"; dialect "?" until sniffed */
	int encrypted;
	size_t pending;                    /* bytes staged for the socket */
	int subs;                          /* subscriptions held */
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

/* S159: the log2 bucket a duration lands in - the smallest k with
 * usec <= 2^k, the top bucket for anything past 32.768 ms */
static int obs_bucket(unsigned long long usec)
{
	int k = usec <= 1 ? 0 : 64 - __builtin_clzll(usec - 1);
	return k < PC_OBS_HIST - 1 ? k : PC_OBS_HIST - 1;
}

/* S159: the slot a name owns on this worker's row - found or claimed */
void *pc_obs_cmd_slot(const char *name, size_t nlen)
{
	struct cmdrow *r;
	char lc[OBS_NAME];
	unsigned int i, h;

	if (!obs_cmds || !nlen)
		return NULL;
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
		return e;
	}
	return NULL;   /* row full: the 64th distinct name is dropped */
}

void pc_obs_cmd_at(void *slot, unsigned long long usec)
{
	struct cmdrow *e = slot;

	if (!e)
		return;
	e->calls++;
	e->usec += usec;
	e->hist[obs_bucket(usec)]++;
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
		e->hist[obs_bucket(usec)]++;   /* S159 */
		return;
	}
	/* row full: the 64th distinct name on one worker is dropped -
	 * monitoring, not accounting */
}

/* S159: the rows merged by name across the workers - the one walk
 * every reader shares: INFO commandstats and latencystats, /stats
 * commands, the /metrics series.  The space is small enough to do the
 * quadratic thing simply. */
int pc_obs_cmd_merge(struct pc_obs_cmdsum *out, int cap)
{
	int n = 0, row, i, j, k;

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
				if (n == cap)
					continue;
				memset(&out[n], 0, sizeof out[n]);
				memcpy(out[n].name, r[i].name, r[i].nlen);
				out[n].nlen = r[i].nlen;
				n++;
			}
			out[j].calls += r[i].calls;
			out[j].usec += r[i].usec;
			for (k = 0; k < PC_OBS_HIST; k++)
				out[j].hist[k] += r[i].hist[k];
		}
	}
	return n;
}

/* the bucket bound, in us, the p-th permille falls under: the smallest
 * k whose cumulative count reaches ceil(calls * p / 1000), answered as
 * 2^k.  The top bucket has no bound: it answers 32768 and the true
 * figure is higher. */
static unsigned long long obs_pct_us(const struct pc_obs_cmdsum *s,
		unsigned int permille)
{
	unsigned long long need, cum = 0;
	int k;

	if (!s->calls)
		return 0;
	need = (s->calls * permille + 999) / 1000;
	if (need < 1)
		need = 1;
	for (k = 0; k < PC_OBS_HIST - 1; k++) {
		cum += s->hist[k];
		if (cum >= need)
			return 1ULL << k;
	}
	return 1ULL << (PC_OBS_HIST - 2);
}

void pc_obs_cmdstats(struct pc_jw *w)
{
	struct pc_obs_cmdsum out[OBS_MERGE];
	int n = pc_obs_cmd_merge(out, OBS_MERGE), i;

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

/* S159: Redis 7's shape - latencystat_<cmd>:p50=<ms>,p99=<ms>,p99.9=<ms>
 * - each figure the log2 bucket bound the percentile falls under */
void pc_obs_latencystats(struct pc_jw *w)
{
	struct pc_obs_cmdsum out[OBS_MERGE];
	int n = pc_obs_cmd_merge(out, OBS_MERGE), i;

	for (i = 0; i < n; i++) {
		char line[128];
		int ln = snprintf(line, sizeof line,
			"latencystat_%.*s:p50=%.3f,p99=%.3f,p99.9=%.3f\r\n",
			(int)out[i].nlen, out[i].name,
			(double)obs_pct_us(&out[i], 500) / 1000.0,
			(double)obs_pct_us(&out[i], 990) / 1000.0,
			(double)obs_pct_us(&out[i], 999) / 1000.0);
		pc_jw_raw(w, line, (size_t)ln);
	}
}

/* S159: the same rows as a JSON array for /stats - calls, usec, the
 * three percentile bounds in us and the seventeen buckets themselves */
void pc_obs_commands_json(struct pc_jw *w)
{
	struct pc_obs_cmdsum out[OBS_MERGE];
	int n = pc_obs_cmd_merge(out, OBS_MERGE), i, k;

	pc_jw_lit(w, "[");
	for (i = 0; i < n; i++) {
		pc_jw_lit(w, i ? ",{\"name\":" : "{\"name\":");
		pc_jw_str(w, out[i].name, out[i].nlen);
		pc_jw_lit(w, ",\"calls\":");
		pc_jw_i64(w, (long long)out[i].calls);
		pc_jw_lit(w, ",\"usec\":");
		pc_jw_i64(w, (long long)out[i].usec);
		pc_jw_lit(w, ",\"p50_us\":");
		pc_jw_i64(w, (long long)obs_pct_us(&out[i], 500));
		pc_jw_lit(w, ",\"p99_us\":");
		pc_jw_i64(w, (long long)obs_pct_us(&out[i], 990));
		pc_jw_lit(w, ",\"p999_us\":");
		pc_jw_i64(w, (long long)obs_pct_us(&out[i], 999));
		pc_jw_lit(w, ",\"hist\":[");
		for (k = 0; k < PC_OBS_HIST; k++) {
			if (k)
				pc_jw_lit(w, ",");
			pc_jw_i64(w, (long long)out[i].hist[k]);
		}
		pc_jw_lit(w, "]}");
	}
	pc_jw_lit(w, "]");
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

/* ---- S65: the query log ------------------------------------------------ */

/* The ceiling nobody may raise: at 1.8M ops/s a line per operation is
 * megabytes a second of journal, so sampling is part of the feature and
 * this cap is the backstop behind it.  Policy, not a knob. */
#define QLOG_MAX_PER_SEC 1000
static int qlog_every;                 /* 0 off, 1 all, N one-in-N */
static int qlog_keys;                  /* 0 no, 1 hashed, 2 full */
static unsigned int qlog_sec_lines;    /* lines this second, all workers */
static unsigned int qlog_sec_dropped;
static __thread unsigned int qlog_seen;   /* the per-worker sample counter */

void pc_obs_qlog_config(int every, int keys)
{
	qlog_every = every;
	qlog_keys = keys;
}

int pc_obs_qlog_on(void)
{
	return qlog_every != 0;
}

void pc_obs_qlog(const char *dialect, void *row, const char *verb,
		size_t vlen, const char *col, size_t clen, const char *key,
		size_t klen, const char *outcome, unsigned long long usec)
{
	struct connrow *cr = row;
	char kb[96], sb[24];
	const char *ks = "", *ss = "";

	if (!qlog_every)
		return;                        /* the one branch when off */
	if (qlog_every > 1) {
		if ((++qlog_seen % (unsigned int)qlog_every) != 0)
			return;
		snprintf(sb, sizeof sb, " sample=1/%d", qlog_every);
		ss = sb;
	}
	if (__atomic_add_fetch(&qlog_sec_lines, 1, __ATOMIC_RELAXED) >
	        QLOG_MAX_PER_SEC) {
		__atomic_add_fetch(&qlog_sec_dropped, 1, __ATOMIC_RELAXED);
		return;
	}
	if (klen && qlog_keys == 2) {
		snprintf(kb, sizeof kb, " key=%.*s%s", (int)(klen > 64 ? 64 : klen),
			key, klen > 64 ? "..." : "");
		ks = kb;
	} else if (klen && qlog_keys == 1) {
		/* FNV-1a, folded to 32 bits: the same key hashes the same way
		 * on every node, so lines correlate without carrying the key */
		unsigned long long h = fnv1a64(key, klen);

		snprintf(kb, sizeof kb, " key=#%08x", (unsigned int)(h ^ (h >> 32)));
		ks = kb;
	}
	/* the outcome is what an operator chasing a miss is reading for
	 * (S65): hit, miss, ok or err, before the latency */
	LM_INFO("query: %s %s %.*s %.*s%s %s %llu us%s\n", dialect,
		cr ? cr->addr : "-", (int)vlen, verb, (int)clen, clen ? col : "-",
		ks, outcome ? outcome : "ok", usec, ss);
}

void pc_obs_tick_1hz(void)
{
	unsigned long long t = pc_obs_total_calls();
	unsigned int d = __atomic_exchange_n(&qlog_sec_dropped, 0, __ATOMIC_RELAXED);

	__atomic_store_n(&qlog_sec_lines, 0, __ATOMIC_RELAXED);
	if (d)
		LM_INFO("query: %u line(s) suppressed this second (cap %d/s - "
			"use sampled:N)\n", d, QLOG_MAX_PER_SEC);

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
	r->door = resp_only ? "resp" : "native";
	r->dialect = resp_only ? "resp" : "?";
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

void pc_obs_conn_door(void *row, const char *door, const char *dialect,
		int encrypted)
{
	struct connrow *r = row;

	if (!r)
		return;
	if (door)
		r->door = door;
	if (dialect)
		r->dialect = dialect;
	if (encrypted >= 0)
		r->encrypted = encrypted;
}

void pc_obs_conn_pending(void *row, size_t bytes)
{
	if (row)
		((struct connrow *)row)->pending = bytes;
}

void pc_obs_conn_subs(void *row, int n)
{
	if (row)
		((struct connrow *)row)->subs = n;
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
	r->cmds++;
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

/* S160: active first - the most recently touched connection leads, so
 * the busy ones sit at the top of a capped table */
static int clients_cmp(const void *a, const void *b)
{
	const struct connrow *x = *(const struct connrow *const *)a;
	const struct connrow *y = *(const struct connrow *const *)b;

	if (x->last != y->last)
		return x->last > y->last ? -1 : 1;
	return x->id > y->id ? -1 : x->id < y->id ? 1 : 0;
}

void pc_obs_clients_json(struct pc_jw *w, unsigned int now_ticks, int cap)
{
	enum { KEEP = 4096 };
	const struct connrow **v = malloc(KEEP * sizeof *v);
	struct connrow *r;
	int n = 0, total = 0, i;
	char num[32];

	pc_jw_lit(w, "{\"total\":");
	pthread_mutex_lock(&obs_cl_mu);
	for (r = obs_cl_head; r; r = r->next) {
		if (r->door && strcmp(r->door, "http") == 0)
			continue;
		total++;
		if (v && n < KEEP)
			v[n++] = r;
	}
	if (v)
		qsort(v, (size_t)n, sizeof *v, clients_cmp);
	if (cap >= 0 && n > cap)
		n = cap;
	pc_jw_i64(w, total);
	pc_jw_lit(w, ",\"shown\":");
	pc_jw_i64(w, n);
	pc_jw_lit(w, ",\"clients\":[");
	for (i = 0; i < n; i++) {
		const struct connrow *e = v[i];

		pc_jw_lit(w, i ? ",{\"id\":" : "{\"id\":");
		pc_jw_i64(w, (long long)e->id);
		pc_jw_lit(w, ",\"door\":\"");
		pc_jw_lit(w, e->door ? e->door : "native");
		pc_jw_lit(w, "\",\"dialect\":\"");
		pc_jw_lit(w, e->dialect ? e->dialect : "?");
		pc_jw_lit(w, "\",\"encrypted\":");
		pc_jw_lit(w, e->encrypted ? "true" : "false");
		pc_jw_lit(w, ",\"addr\":");
		pc_jw_str(w, e->addr, strlen(e->addr));
		pc_jw_lit(w, ",\"name\":");
		pc_jw_str(w, e->name, e->nlen);
		pc_jw_lit(w, ",\"age_s\":");
		pc_jw_i64(w, (long long)(now_ticks - e->created));
		pc_jw_lit(w, ",\"idle_s\":");
		pc_jw_i64(w, now_ticks >= e->last ? (long long)(now_ticks - e->last) : 0);
		pc_jw_lit(w, ",\"cmds\":");
		pc_jw_i64(w, (long long)e->cmds);
		pc_jw_lit(w, ",\"last_cmd\":");
		pc_jw_str(w, e->lastcmd, e->lastcmdlen);
		pc_jw_lit(w, ",\"pending\":");
		snprintf(num, sizeof num, "%zu", e->pending);
		pc_jw_lit(w, num);
		pc_jw_lit(w, ",\"subs\":");
		pc_jw_i64(w, e->subs);
		pc_jw_lit(w, "}");
	}
	pthread_mutex_unlock(&obs_cl_mu);
	pc_jw_lit(w, "]}");
	free(v);
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

/* S159: the slow log as a JSON array for /stats - the six fields
 * SLOWLOG GET answers, in its order */
void pc_obs_slowlog_json(struct pc_jw *w, int want)
{
	const struct slowent *ents[512];
	int n, i, j;

	n = slow_collect(ents, (int)(sizeof ents / sizeof ents[0]));
	if (want >= 0 && n > want)
		n = want;
	pc_jw_lit(w, "[");
	for (i = 0; i < n; i++) {
		const struct slowent *e = ents[i];
		int argc = e->argc < OBS_ARGN ? e->argc : OBS_ARGN;

		pc_jw_lit(w, i ? ",{\"id\":" : "{\"id\":");
		pc_jw_i64(w, (long long)e->id);
		pc_jw_lit(w, ",\"ts\":");
		pc_jw_i64(w, (long long)e->ts);
		pc_jw_lit(w, ",\"usec\":");
		pc_jw_i64(w, (long long)e->usec);
		pc_jw_lit(w, ",\"argv\":[");
		for (j = 0; j < argc; j++) {
			if (j)
				pc_jw_lit(w, ",");
			pc_jw_str(w, e->argv[j], e->arglen[j]);
		}
		pc_jw_lit(w, "],\"addr\":");
		pc_jw_str(w, e->addr, strlen(e->addr));
		pc_jw_lit(w, ",\"name\":");
		pc_jw_str(w, e->name, strlen(e->name));
		pc_jw_lit(w, "}");
	}
	pc_jw_lit(w, "]");
}

int pc_obs_slow_would(unsigned long long usec)
{
	return obs_slow && obs_slow_thresh >= 0 &&
		usec >= (unsigned long long)obs_slow_thresh;
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

/* S70: for the connection log - the address exactly as CLIENT LIST shows
 * it, and how many requests the connection carried before it closed */
const char *pc_obs_conn_addr(void *row)
{
	return row ? ((struct connrow *)row)->addr : "?:0";
}

unsigned long long pc_obs_conn_cmds(void *row)
{
	return row ? ((struct connrow *)row)->cmds : 0;
}
