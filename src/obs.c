/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* obs.c - see obs.h.  The formats belong to Redis: the Grafana Redis datasource parses them,
 * so every emitter mirrors the Redis shape byte for byte. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <pthread.h>
#include <arpa/inet.h>

#include "pcaddr.h"
#include "json.h"
#include "daemon.h"                    /* pc_worker_id() */
#include "compat/timer.h"              /* get_ticks() */
#include "compat/dprint.h"             /* LM_INFO for the query log */
#include "obs.h"
#include "resident.h"
#include "clunk.h"
#include "fnv1a.h"                     /* the one FNV-1a */
#include "colname.h"                   /* pc_col_name_print */

#define OBS_CMDS   64                  /* distinct command names per row */
#define OBS_NAME   PC_OBS_NAME
#define OBS_MERGE  (OBS_CMDS * 2)      /* merged rows a reader keeps */
#define OBS_SLOWN  128                 /* slow entries per worker ring */
#define OBS_ARGN   4                   /* argv kept per slow entry */
#define OBS_ARGLEN 40
#define OBS_LIB    48                  /* CLIENT SETINFO lib-name / lib-ver */

struct cmdrow {
	char name[OBS_NAME];
	unsigned int nlen;
	unsigned long long calls, usec, max;
	unsigned long long hist[PC_OBS_HIST];   /* latency buckets */
	/* per-minute snapshots of the cumulative figures, plus a max per minute: the one figure
	 * that cannot be recovered by subtraction */
	unsigned long long s_calls[PC_OBS_WIN], s_usec[PC_OBS_WIN];
	unsigned long long s_hist[PC_OBS_WIN][PC_OBS_HIST];
	unsigned long long w_max[PC_OBS_WIN];
	unsigned int s_at[PC_OBS_WIN];          /* tick each slot was taken */
};

struct slowent {
	unsigned long long id;             /* 0 = empty */
	unsigned long long ts;             /* unix seconds */
	unsigned long long usec;
	unsigned int epoch;                /* stale after SLOWLOG RESET */
	int argc;                          /* real argc, pre-truncation */
	char argv[OBS_ARGN][OBS_ARGLEN];
	unsigned int arglen[OBS_ARGN];
	char addr[PC_ADDR_STRLEN];
	char name[OBS_NAME];
};

struct connrow {
	struct connrow *next, *prev;
	unsigned long long id;
	char addr[PC_ADDR_STRLEN];
	int fd, resp_only;
	unsigned int created, last;        /* ticks */
	char name[OBS_NAME];
	unsigned int nlen;
	char lastcmd[OBS_NAME];
	unsigned int lastcmdlen;
	unsigned long long cmds;           /* requests this connection carried */
	/* for /clients */
	const char *door, *dialect;        /* "native" | "resp" | "http"; dialect "?" until sniffed */
	int encrypted;
	size_t pending;                    /* bytes staged for the socket */
	int subs;                          /* subscriptions held */
	/* CLIENT SETINFO: the client library's own name and version, what
	 * identifies a client behind a proxy that hides its address */
	char libname[OBS_LIB], libver[OBS_LIB];
	int wk;                            /* the worker counted for, 1..N; 0 none */
};

static int obs_rows;                   /* workers + 1 shared spare */
static struct cmdrow *obs_cmds;        /* [obs_rows][OBS_CMDS] */
/* which minute slot the write path is filling, and when it began */
static unsigned int obs_win_slot;
static unsigned int obs_win_at;
static struct slowent *obs_slow;       /* [obs_rows][OBS_SLOWN] */
static unsigned int *obs_slowpos;      /* next write index per row */
static long long obs_slow_thresh;      /* usec; <0 = disabled */
static unsigned long long obs_slow_id = 1;
static unsigned int obs_slow_epoch;

static pthread_mutex_t obs_cl_mu = PTHREAD_MUTEX_INITIALIZER;
static struct connrow *obs_cl_head;
static int obs_cl_n;
static unsigned long long obs_cl_id;

/* a worker's unknown-command table, stale when its epoch is not
 * the current one (a reset bumps it, the owner clears on next write) */
struct unkw {
	unsigned int epoch;
	struct clunk_table t;
	unsigned long long tot[CLUNK_DIALECTS];
};
static struct unkw *obs_unk;           /* [obs_rows] */
static unsigned int obs_unk_epoch = 1;
static unsigned long long obs_unk_gen;
/* first sightings already logged, per process - a reset does not re-log */
#define OBS_UNK_SEEN 256
static pthread_mutex_t obs_unk_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { unsigned char d, n; char name[CLUNK_NAME]; } obs_unk_seen[OBS_UNK_SEEN];
static int obs_unk_nseen, obs_unk_capped;

static unsigned long long obs_prev_total;
static int *obs_wkc, obs_nwk;         /* live native connections per worker */
static unsigned long long obs_inst;

int pc_obs_init(int nworkers, long long slowlog_usec)
{
	obs_rows = nworkers + 1;
	obs_cmds = calloc((size_t)obs_rows * OBS_CMDS, sizeof *obs_cmds);
	obs_slow = calloc((size_t)obs_rows * OBS_SLOWN, sizeof *obs_slow);
	obs_slowpos = calloc((size_t)obs_rows, sizeof *obs_slowpos);
	obs_unk = calloc((size_t)obs_rows, sizeof *obs_unk);
	obs_nwk = nworkers;
	obs_wkc = calloc((size_t)nworkers + 2, sizeof *obs_wkc);
	/* resident now, not a row at a time as verbs are first seen */
	pc_touch_pages(obs_cmds, (size_t)obs_rows * OBS_CMDS * sizeof *obs_cmds);
	pc_touch_pages(obs_slow, (size_t)obs_rows * OBS_SLOWN * sizeof *obs_slow);
	obs_slow_thresh = slowlog_usec;
	return (obs_cmds && obs_slow && obs_slowpos && obs_unk && obs_wkc) ? 0 : -1;
}

/* workers are 1..N on rows 0..N-1; every other thread shares the spare, row N */
static int obs_row(void)
{
	int w = pc_worker_id();

	return (w >= 1 && w <= obs_rows - 1) ? w - 1 : obs_rows - 1;
}

unsigned long long pc_obs_usec_now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (unsigned long long)t.tv_sec * 1000000 +
		(unsigned long long)(t.tv_nsec / 1000);
}

/* the bucket is the octave and which eighth of it; below PC_OBS_SUB us each value has its own
 * slot, and the last slot takes everything past the top octave */
static int obs_bucket(unsigned long long usec)
{
	int oct, sub, idx, sh;

	if (usec < PC_OBS_SUB)
		return (int)usec;
	oct = 63 - __builtin_clzll(usec);            /* floor(log2) */
	sh = oct - 3;                                /* SUB = 1 << 3 */
	sub = (int)((usec >> sh) & (PC_OBS_SUB - 1));
	/* octave 3 is the first that reaches here, so the index continues right after the exact slots */
	idx = (oct - 2) * PC_OBS_SUB + sub;
	return idx < PC_OBS_HIST - 1 ? idx : PC_OBS_HIST - 1;
}

/* the inclusive LOWER bound of bucket @k - the interpolation needs the
 * span, and deriving it from hi(k-1) would be wrong at an octave edge */
static unsigned long long obs_hist_lo(int k)
{
	unsigned long long base;
	int oct, sub;

	if (k < PC_OBS_SUB)
		return (unsigned long long)k;
	oct = k / PC_OBS_SUB + 2;
	sub = k % PC_OBS_SUB;
	base = 1ULL << oct;
	return base + (base / PC_OBS_SUB) * (unsigned long long)sub;
}

unsigned long long pc_obs_hist_hi(int k)
{
	unsigned long long base, step;
	int oct, sub;

	if (k < PC_OBS_SUB)
		return (unsigned long long)k;        /* exact microseconds */
	if (k >= PC_OBS_HIST - 1)
		return 0;                            /* the overflow: +Inf */
	oct = k / PC_OBS_SUB + 2;
	sub = k % PC_OBS_SUB;
	base = 1ULL << oct;
	step = base / PC_OBS_SUB;
	return base + step * (unsigned long long)(sub + 1) - 1;
	/* note: step is >= 1 because the low octaves are the exact slots
	 * above, so this never collapses to a zero-width bucket */
}

/* a row's name lower-cased, a byte outside printable ASCII as '?': INFO prints it raw */
static void obs_fold(char *lc, const char *name, size_t nlen)
{
	size_t i;

	for (i = 0; i < nlen; i++) {
		unsigned char c = (unsigned char)name[i];

		lc[i] = c < 0x21 || c > 0x7e ? '?' : (char)tolower(c);
	}
}

void *pc_obs_cmd_slot(const char *name, size_t nlen)
{
	struct cmdrow *r;
	char lc[OBS_NAME];
	unsigned int i, h;

	if (!obs_cmds || !nlen)
		return NULL;
	if (nlen >= OBS_NAME)
		nlen = OBS_NAME - 1;
	obs_fold(lc, name, nlen);
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
	if (usec > e->max)
		e->max = usec;
	{                                      /* and the max within the minute */
		unsigned int w = __atomic_load_n(&obs_win_slot, __ATOMIC_RELAXED);

		if (usec > e->w_max[w])
			e->w_max[w] = usec;
	}
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
	obs_fold(lc, name, nlen);
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
		e->hist[obs_bucket(usec)]++;
		if (usec > e->max)
			e->max = usec;
		{
			unsigned int w = __atomic_load_n(&obs_win_slot,
				__ATOMIC_RELAXED);

			if (usec > e->w_max[w])
				e->w_max[w] = usec;
		}
		return;
	}
	/* row full: the 64th distinct name on one worker is dropped -
	 * monitoring, not accounting */
}

/* a figure less its snapshot; never below 0 when a roll lands between the two reads */
static unsigned long long obs_since(unsigned long long now, unsigned long long then)
{
	return now > then ? now - then : 0;
}

/* the one merge every reader shares (INFO commandstats and latencystats, /stats, /metrics);
 * the space is small enough for the quadratic walk */
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
			if (r[i].max > out[j].max)
				out[j].max = r[i].max;
			/* the window is the difference from the OLDEST snapshot the row holds; a row younger
			 * than one roll has none, and its window is everything it has */
			{
				unsigned int w, fill, old_slot = PC_OBS_WIN;
				unsigned int oldest = 0, nowt = (unsigned int)get_ticks();

				/* a slot being rewritten reads 0 (obs_win_roll) and is passed over */
				for (w = 0; w < PC_OBS_WIN; w++) {
					unsigned int at = __atomic_load_n(&r[i].s_at[w],
						__ATOMIC_ACQUIRE);

					if (!at)
						continue;
					if (old_slot == PC_OBS_WIN || at < oldest) {
						oldest = at;
						old_slot = w;
					}
				}
				if (old_slot < PC_OBS_WIN) {
					out[j].w_calls += obs_since(r[i].calls,
						r[i].s_calls[old_slot]);
					out[j].w_usec += obs_since(r[i].usec,
						r[i].s_usec[old_slot]);
					for (k = 0; k < PC_OBS_HIST; k++)
						out[j].w_hist[k] += obs_since(r[i].hist[k],
							r[i].s_hist[old_slot][k]);
					if (nowt - oldest > out[j].w_secs)
						out[j].w_secs = nowt - oldest;
				} else {
					out[j].w_calls += r[i].calls;
					out[j].w_usec += r[i].usec;
					for (k = 0; k < PC_OBS_HIST; k++)
						out[j].w_hist[k] += r[i].hist[k];
				}
				/* old_slot's calls and max are outside the window (its snapshot is taken at its
				 * END) - unless it is the slot being filled, emptied at the roll */
				fill = __atomic_load_n(&obs_win_slot, __ATOMIC_RELAXED);
				for (w = 0; w < PC_OBS_WIN; w++) {
					if (w == old_slot && w != fill)
						continue;
					if (r[i].w_max[w] > out[j].w_max)
						out[j].w_max = r[i].w_max[w];
				}
			}
		}
	}
	return n;
}

/* the value at the p-th permille of any histogram, interpolated inside the bucket the rank lands
 * in (as histogram_quantile does); one implementation for the cumulative and window views */
static unsigned long long obs_pct_of(const unsigned long long *hist,
		unsigned long long calls, unsigned long long max,
		unsigned int permille)
{
	unsigned long long need, cum = 0, v;
	int k;

	if (!calls)
		return 0;
	need = (calls * permille + 999) / 1000;
	if (need < 1)
		need = 1;
	for (k = 0; k < PC_OBS_HIST - 1; k++) {
		unsigned long long lo, hi, before = cum;

		cum += hist[k];
		if (cum < need)
			continue;
		lo = obs_hist_lo(k);
		hi = pc_obs_hist_hi(k);
		if (hi <= lo || !hist[k])
			v = hi ? hi : lo;
		else
			v = lo + ((hi - lo) * (need - before) +
				hist[k] - 1) / hist[k];
		/* never past the slowest call actually seen: a bucket is wider than its contents */
		if (max && v > max)
			v = max;
		/* never below 1 us either: a sub-microsecond call is recorded as 0 but took at most
		 * 1 us, and Redis 7 reports 0.001 ms there */
		return v ? v : 1;
	}
	return pc_obs_hist_hi(PC_OBS_HIST - 2);
}

/* a max of calls that were all sub-microsecond reads 0; floor it
 * with the percentiles, so it is never below them */
static long long obs_max_us(unsigned long long max, unsigned long long calls)
{
	return (long long)(max ? max : calls ? 1 : 0);
}

static unsigned long long obs_pct_us(const struct pc_obs_cmdsum *s,
		unsigned int permille)
{
	return obs_pct_of(s->hist, s->calls, s->max, permille);
}

static unsigned long long obs_pct_win(const struct pc_obs_cmdsum *s,
		unsigned int permille)
{
	return obs_pct_of(s->w_hist, s->w_calls, s->w_max, permille);
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
		if (ln < 0)
			continue;
		if ((size_t)ln >= sizeof line)  /* snprintf says what it would have written */
			ln = (int)sizeof line - 1;
		pc_jw_raw(w, line, (size_t)ln);
	}
}

/* Redis 7's shape: latencystat_<cmd>:p50=<ms>,p99=<ms>,p99.9=<ms> */
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
		if (ln < 0)
			continue;
		if ((size_t)ln >= sizeof line)
			ln = (int)sizeof line - 1;
		pc_jw_raw(w, line, (size_t)ln);
	}
}

/* the same rows as a tree array for the native `stats` reply (rendered as JSON for /stats) */
void pc_obs_commands_tree(struct pc_tw *w)
{
	struct pc_obs_cmdsum out[OBS_MERGE];
	int n = pc_obs_cmd_merge(out, OBS_MERGE), i, k;

	pc_tw_arr(w);
	for (i = 0; i < n; i++) {
		pc_tw_map(w);
		pc_tw_key(w, "name");
		pc_tw_bulk(w, out[i].name, out[i].nlen);
		/* the dialect beside the verb: a Redis command name may contain a dot (JSON.GET), so the
		 * name alone cannot tell a native method from RESP; the name stays the join key */
		{
			unsigned int d = 0;

			while (d < out[i].nlen && out[i].name[d] != ':')
				d++;
			pc_tw_key(w, "dialect");
			if (d < out[i].nlen)
				pc_tw_bulk(w, out[i].name, d);
			else
				pc_tw_str(w, "resp");
			pc_tw_key(w, "verb");
			if (d < out[i].nlen)
				pc_tw_bulk(w, out[i].name + d + 1, out[i].nlen - d - 1);
			else
				pc_tw_bulk(w, out[i].name, out[i].nlen);
		}
		pc_tw_key(w, "calls");
		pc_tw_i64(w, (long long)out[i].calls);
		pc_tw_key(w, "usec");
		pc_tw_i64(w, (long long)out[i].usec);
		pc_tw_key(w, "p50_us");
		pc_tw_i64(w, (long long)obs_pct_us(&out[i], 500));
		pc_tw_key(w, "p99_us");
		pc_tw_i64(w, (long long)obs_pct_us(&out[i], 990));
		pc_tw_key(w, "p999_us");
		pc_tw_i64(w, (long long)obs_pct_us(&out[i], 999));
		/* exact, not a bucket bound */
		pc_tw_key(w, "max_us");
		pc_tw_i64(w, obs_max_us(out[i].max, out[i].calls));
		/* the same four over the last five minutes, which is what a dashboard is read for */
		pc_tw_key(w, "win_s");
		pc_tw_i64(w, (long long)out[i].w_secs);
		pc_tw_key(w, "win_calls");
		pc_tw_i64(w, (long long)out[i].w_calls);
		pc_tw_key(w, "win_usec");
		pc_tw_i64(w, (long long)out[i].w_usec);
		pc_tw_key(w, "win_p50_us");
		pc_tw_i64(w, (long long)obs_pct_win(&out[i], 500));
		pc_tw_key(w, "win_p99_us");
		pc_tw_i64(w, (long long)obs_pct_win(&out[i], 990));
		pc_tw_key(w, "win_p999_us");
		pc_tw_i64(w, (long long)obs_pct_win(&out[i], 999));
		pc_tw_key(w, "win_max_us");
		pc_tw_i64(w, obs_max_us(out[i].w_max, out[i].w_calls));
		pc_tw_key(w, "hist");
		pc_tw_arr(w);
		for (k = 0; k < PC_OBS_HIST; k++)
			pc_tw_i64(w, (long long)out[i].hist[k]);
		pc_tw_end(w);
		pc_tw_end(w);
	}
	pc_tw_end(w);
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

/* ---- the query log ------------------------------------------------ */

/* at 1.8M ops/s a line per operation is megabytes a second of journal: sampling is the
 * feature and this cap its backstop.  Policy, not a knob. */
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

static unsigned int obs_secret_args(char *const *argv, const size_t *argl, int nargs);

void pc_obs_qlog(const char *dialect, void *row, const char *verb,
		size_t vlen, const char *col, size_t clen, const char *key,
		size_t klen, const char *outcome, unsigned long long usec)
{
	struct connrow *cr = row;
	char kb[300], sb[24], vb[200], cb[264], ke[264];
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
	if (klen) {                        /* AUTH's argument is a password: the slow ring's rule */
		char *const a[2] = { (char *)verb, (char *)key };
		const size_t l[2] = { vlen, klen };

		if (obs_secret_args(a, l, 2) & 2u)
			klen = 0;
	}
	if (klen && qlog_keys == 2) {
		pc_col_name_print(ke, sizeof ke, key, klen > 64 ? 64 : klen);
		snprintf(kb, sizeof kb, " key=%s%s", ke, klen > 64 ? "..." : "");
		ks = kb;
	} else if (klen && qlog_keys == 1) {
		/* FNV-1a, folded to 32 bits: the same key hashes the same way
		 * on every node, so lines correlate without carrying the key */
		unsigned long long h = fnv1a64(key, klen);

		snprintf(kb, sizeof kb, " key=#%08x", (unsigned int)(h ^ (h >> 32)));
		ks = kb;
	}
	/* a client's bytes as a log line may carry them; a verb is never longer than 47 */
	pc_col_name_print(vb, sizeof vb, verb, vlen > 47 ? 47 : vlen);
	pc_col_name_print(cb, sizeof cb, clen ? col : "-", clen ? (clen > 64 ? 64 : clen) : 1);
	/* the outcome (hit, miss, ok or err) before the latency: what a miss hunt reads for */
	LM_INFO("query: %s %s %s%s %s%s %s %llu us%s\n", dialect,
		cr ? cr->addr : "-", vb, vlen > 47 ? "..." : "", cb, ks,
		outcome ? outcome : "ok", usec, ss);
}

/* snapshot the cumulative figures into the slot we are LEAVING and clear the one about to be
 * filled; maintenance thread, once a minute */
static void obs_win_roll(unsigned int now)
{
	unsigned int cur = __atomic_load_n(&obs_win_slot, __ATOMIC_RELAXED);
	unsigned int nxt = (cur + 1) % PC_OBS_WIN;
	int row, i, k;

	for (row = 0; row < obs_rows; row++) {
		struct cmdrow *r = obs_cmds + (size_t)row * OBS_CMDS;

		for (i = 0; i < OBS_CMDS; i++) {
			if (!r[i].nlen)
				continue;
			/* stamped 0 while the copy is written, so a reader passes the slot over */
			__atomic_store_n(&r[i].s_at[cur], 0, __ATOMIC_RELAXED);
			__atomic_thread_fence(__ATOMIC_RELEASE);
			r[i].s_calls[cur] = r[i].calls;
			r[i].s_usec[cur] = r[i].usec;
			for (k = 0; k < PC_OBS_HIST; k++)
				r[i].s_hist[cur][k] = r[i].hist[k];
			__atomic_store_n(&r[i].s_at[cur], now, __ATOMIC_RELEASE);
			r[i].w_max[nxt] = 0;   /* the slot about to be filled */
		}
	}
	__atomic_store_n(&obs_win_slot, nxt, __ATOMIC_RELAXED);
	obs_win_at = now;
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

	{                                      /* a slot a minute */
		unsigned int now = (unsigned int)get_ticks();

		if (!obs_win_at)
			obs_win_at = now;
		else if (now - obs_win_at >= 60)
			obs_win_roll(now);
	}
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
	if (sa && (sa->sa_family == AF_INET || sa->sa_family == AF_INET6)) {
		pc_addr in;

		if (pc_addr_from_sa(&in, sa, slen) == 0)
			pc_addr_ntop_port(&in, r->addr, sizeof r->addr);
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
	if (door && !r->wk && !strcmp(door, "native")) {
		int w = pc_worker_id();

		if (obs_wkc && w >= 1 && w <= obs_nwk) {
			r->wk = w;
			__atomic_add_fetch(&obs_wkc[w], 1, __ATOMIC_RELAXED);
		}
	}
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
	if (r->wk)
		__atomic_sub_fetch(&obs_wkc[r->wk], 1, __ATOMIC_RELAXED);
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

const char *pc_obs_conn_name_get(void *row, size_t *nlen)
{
	struct connrow *r = row;

	if (!r) {
		*nlen = 0;
		return "";
	}
	*nlen = r->nlen;
	return r->name;
}

void pc_obs_conn_lib(void *row, int ver, const char *v, size_t n)
{
	struct connrow *r = row;
	char *dst;

	if (!r)
		return;
	dst = ver ? r->libver : r->libname;
	if (n >= OBS_LIB)
		n = OBS_LIB - 1;
	memcpy(dst, v, n);
	dst[n] = 0;
}

int pc_obs_conn_count(void)
{
	return obs_cl_n;
}

int pc_obs_worker_conns(int *out, int max)
{
	int i, n = obs_nwk < max ? obs_nwk : max;

	for (i = 0; i < n; i++)
		out[i] = obs_wkc ? __atomic_load_n(&obs_wkc[i + 1], __ATOMIC_RELAXED) : 0;
	return n;
}

/* active first: the most recently touched connection leads a capped table */
static int clients_cmp(const void *a, const void *b)
{
	const struct connrow *x = *(const struct connrow *const *)a;
	const struct connrow *y = *(const struct connrow *const *)b;

	if (x->last != y->last)
		return x->last > y->last ? -1 : 1;
	return x->id > y->id ? -1 : x->id < y->id ? 1 : 0;
}

void pc_obs_clients_tree(struct pc_tw *w, unsigned int now_ticks, int cap)
{
	enum { KEEP = 4096 };
	const struct connrow **v = malloc(KEEP * sizeof *v);
	struct connrow *r;
	int n = 0, total = 0, i;

	pc_tw_map(w);
	pc_tw_key(w, "total");
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
	pc_tw_i64(w, total);
	pc_tw_key(w, "shown");
	pc_tw_i64(w, n);
	pc_tw_key(w, "clients");
	pc_tw_arr(w);
	for (i = 0; i < n; i++) {
		const struct connrow *e = v[i];

		pc_tw_map(w);
		pc_tw_key(w, "id");
		pc_tw_i64(w, (long long)e->id);
		pc_tw_key(w, "door");
		pc_tw_str(w, e->door ? e->door : "native");
		pc_tw_key(w, "dialect");
		pc_tw_str(w, e->dialect ? e->dialect : "?");
		pc_tw_key(w, "encrypted");
		pc_tw_bool(w, e->encrypted);
		pc_tw_key(w, "addr");
		pc_tw_bulk(w, e->addr, strlen(e->addr));
		pc_tw_key(w, "name");
		pc_tw_bulk(w, e->name, e->nlen);
		pc_tw_key(w, "age_s");
		pc_tw_i64(w, now_ticks >= e->created ? (long long)(now_ticks - e->created) : 0);
		pc_tw_key(w, "idle_s");
		pc_tw_i64(w, now_ticks >= e->last ? (long long)(now_ticks - e->last) : 0);
		pc_tw_key(w, "cmds");
		pc_tw_i64(w, (long long)e->cmds);
		pc_tw_key(w, "last_cmd");
		pc_tw_bulk(w, e->lastcmd, e->lastcmdlen);
		pc_tw_key(w, "pending");
		pc_tw_i64(w, (long long)e->pending);
		pc_tw_key(w, "subs");
		pc_tw_i64(w, e->subs);
		pc_tw_key(w, "lib_name");
		pc_tw_bulk(w, e->libname, strlen(e->libname));
		pc_tw_key(w, "lib_ver");
		pc_tw_bulk(w, e->libver, strlen(e->libver));
		pc_tw_end(w);
	}
	pthread_mutex_unlock(&obs_cl_mu);
	pc_tw_end(w);
	pc_tw_end(w);
	free(v);
}

void pc_obs_client_list(struct pc_jw *w, unsigned int now_ticks)
{
	struct connrow *r;

	pthread_mutex_lock(&obs_cl_mu);
	for (r = obs_cl_head; r; r = r->next) {
		char line[512];
		int ln = snprintf(line, sizeof line,
			"id=%llu addr=%s fd=%d name=%.*s age=%u idle=%u "
			"flags=N db=0 sub=0 psub=0 multi=-1 cmd=%.*s "
			"lib-name=%s lib-ver=%s\n",
			r->id, r->addr, r->fd, (int)r->nlen, r->name,
			now_ticks >= r->created ? now_ticks - r->created : 0,
			now_ticks >= r->last ? now_ticks - r->last : 0,
			r->lastcmdlen ? (int)r->lastcmdlen : 4,
			r->lastcmdlen ? r->lastcmd : "NULL",
			r->libname, r->libver);

		if (ln < 0)
			continue;
		if ((size_t)ln >= sizeof line)  /* snprintf says what it would have written */
			ln = (int)sizeof line - 1;
		pc_jw_raw(w, line, (size_t)ln);
	}
	pthread_mutex_unlock(&obs_cl_mu);
}

/* ---- slow log ----------------------------------------------------------- */

/* A credential never reaches the slow ring that SLOWLOG GET, /stats and the page all read:
 * AUTH's arguments and the two after AUTH inside HELLO become OBS_REDACTED, argc kept. */
#define OBS_REDACTED "(redacted)"

static int obs_is(const char *s, size_t n, const char *lit)
{
	size_t i;

	if (n != strlen(lit))
		return 0;
	for (i = 0; i < n; i++)
		if (toupper((unsigned char)s[i]) != lit[i])
			return 0;
	return 1;
}

/* bit i set = argument i is a secret; only the first OBS_ARGN are kept,
 * so only they are looked at */
static unsigned int obs_secret_args(char *const *argv, const size_t *argl,
		int nargs)
{
	unsigned int m = 0;
	int i;

	if (nargs < 2)
		return 0;
	if (obs_is(argv[0], argl[0], "AUTH"))
		return ~1u;                    /* everything after the verb */
	if (!obs_is(argv[0], argl[0], "HELLO"))
		return 0;
	/* HELLO [protover [AUTH user pass] [SETNAME name]], walked from argv[1] so a missing version
	 * still redacts; SETNAME's value is skipped, so a client named "AUTH" hides nothing */
	for (i = 1; i < nargs && i < OBS_ARGN; ) {
		if (obs_is(argv[i], argl[i], "AUTH")) {
			m |= 3u << (i + 1);
			i += 3;
		} else if (obs_is(argv[i], argl[i], "SETNAME")) {
			i += 2;
		} else {
			i++;
		}
	}
	return m;
}

void pc_obs_slow(char *const *argv, const size_t *argl, int nargs,
		unsigned long long usec, void *connrow)
{
	struct connrow *c = connrow;
	struct slowent *e;
	unsigned int secret;
	int row, i;
	long long th = __atomic_load_n(&obs_slow_thresh, __ATOMIC_RELAXED);   /* reloadable */

	if (!obs_slow || th < 0 || usec < (unsigned long long)th)
		return;
	secret = obs_secret_args(argv, argl, nargs);
	row = obs_row();
	e = obs_slow + (size_t)row * OBS_SLOWN +
		(obs_slowpos[row]++ % OBS_SLOWN);
	e->id = __atomic_fetch_add(&obs_slow_id, 1, __ATOMIC_RELAXED);
	e->ts = (unsigned long long)time(NULL);
	e->usec = usec;
	e->epoch = __atomic_load_n(&obs_slow_epoch, __ATOMIC_RELAXED);
	e->argc = nargs;
	for (i = 0; i < nargs && i < OBS_ARGN; i++) {
		const char *a = argv[i];
		size_t n = argl[i];

		if (secret >> i & 1) {
			a = OBS_REDACTED;
			n = sizeof OBS_REDACTED - 1;
		}
		if (n > OBS_ARGLEN - 1)
			n = OBS_ARGLEN - 1;
		memcpy(e->argv[i], a, n);
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

/* every ring's entries sorted newest first, then cut to @max: a cut before the sort kept the first
 * rows' entries and never a later worker's */
static int slow_collect(const struct slowent **out, int max)
{
	unsigned int ep = __atomic_load_n(&obs_slow_epoch, __ATOMIC_RELAXED);
	const struct slowent **all = malloc((size_t)obs_rows * OBS_SLOWN * sizeof *all);
	const struct slowent **to = all ? all : out;
	int row, i, n = 0, cap = all ? obs_rows * OBS_SLOWN : max;

	for (row = 0; row < obs_rows; row++) {
		const struct slowent *r = obs_slow +
			(size_t)row * OBS_SLOWN;

		for (i = 0; i < OBS_SLOWN && n < cap; i++)
			if (r[i].id && r[i].epoch == ep)
				to[n++] = &r[i];
	}
	qsort(to, (size_t)n, sizeof *to, slow_cmp);
	if (all) {
		if (n > max)
			n = max;
		memcpy(out, all, (size_t)n * sizeof *out);
		free(all);
	}
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
		/* a copy: the slot's owner may rewrite it, and each $N must match its bytes */
		struct slowent c = *ents[i];
		const struct slowent *e = &c;
		int argc = e->argc < OBS_ARGN ? e->argc : OBS_ARGN;
		size_t al, nl;

		c.addr[sizeof c.addr - 1] = 0;
		c.name[sizeof c.name - 1] = 0;
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
			unsigned int a = e->arglen[j] < OBS_ARGLEN ? e->arglen[j] : OBS_ARGLEN;

			ln = snprintf(num, sizeof num, "$%u\r\n", a);
			pc_jw_raw(w, num, (size_t)ln);
			pc_jw_raw(w, e->argv[j], a);
			pc_jw_lit(w, "\r\n");
		}
		al = strlen(e->addr);
		ln = snprintf(num, sizeof num, "$%zu\r\n", al);
		pc_jw_raw(w, num, (size_t)ln);
		pc_jw_raw(w, e->addr, al);
		pc_jw_lit(w, "\r\n");
		nl = strlen(e->name);
		ln = snprintf(num, sizeof num, "$%zu\r\n", nl);
		pc_jw_raw(w, num, (size_t)ln);
		pc_jw_raw(w, e->name, nl);
		pc_jw_lit(w, "\r\n");
	}
}

/* the slow log as a tree array: SLOWLOG GET's six fields, in its order, as maps */
void pc_obs_slowlog_tree(struct pc_tw *w, int want)
{
	const struct slowent *ents[512];
	int n, i, j;

	n = slow_collect(ents, (int)(sizeof ents / sizeof ents[0]));
	if (want >= 0 && n > want)
		n = want;
	pc_tw_arr(w);
	for (i = 0; i < n; i++) {
		const struct slowent *e = ents[i];
		int argc = e->argc < OBS_ARGN ? e->argc : OBS_ARGN;

		pc_tw_map(w);
		pc_tw_key(w, "id");
		pc_tw_i64(w, (long long)e->id);
		pc_tw_key(w, "ts");
		pc_tw_i64(w, (long long)e->ts);
		pc_tw_key(w, "usec");
		pc_tw_i64(w, (long long)e->usec);
		pc_tw_key(w, "argv");
		pc_tw_arr(w);
		for (j = 0; j < argc; j++)
			pc_tw_bulk(w, e->argv[j], e->arglen[j]);
		pc_tw_end(w);
		pc_tw_key(w, "addr");
		pc_tw_bulk(w, e->addr, strlen(e->addr));
		pc_tw_key(w, "name");
		pc_tw_bulk(w, e->name, strlen(e->name));
		pc_tw_end(w);
	}
	pc_tw_end(w);
}

int pc_obs_slow_would(unsigned long long usec)
{
	long long th = __atomic_load_n(&obs_slow_thresh, __ATOMIC_RELAXED);

	return obs_slow && th >= 0 && usec >= (unsigned long long)th;
}

void pc_obs_set_slowlog(long long usec)
{
	__atomic_store_n(&obs_slow_thresh, usec, __ATOMIC_RELAXED);
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

/* ---- unknown commands ------------------------------------------ */

static struct unkw *unk_row(void)
{
	unsigned int ep = __atomic_load_n(&obs_unk_epoch, __ATOMIC_RELAXED);
	struct unkw *w;

	if (!obs_unk)
		return NULL;
	w = &obs_unk[obs_row()];
	if (w->epoch != ep) {
		memset(&w->t, 0, sizeof w->t);
		memset(w->tot, 0, sizeof w->tot);
		w->epoch = ep;
	}
	return w;
}

/* one NOTICE per distinct name per process, and one more when the set
 * of logged names is full - a scanner gets 257 lines, not one per send */
static void unk_first(int d, const char *name, size_t n, const char *addr,
		const char *client)
{
	int i, log = 0, capped = 0;

	pthread_mutex_lock(&obs_unk_mu);
	for (i = 0; i < obs_unk_nseen; i++)
		if (obs_unk_seen[i].d == d && obs_unk_seen[i].n == n &&
		    memcmp(obs_unk_seen[i].name, name, n) == 0)
			break;
	if (i == obs_unk_nseen) {
		if (obs_unk_nseen < OBS_UNK_SEEN) {
			obs_unk_seen[i].d = (unsigned char)d;
			obs_unk_seen[i].n = (unsigned char)n;
			memcpy(obs_unk_seen[i].name, name, n);
			obs_unk_nseen++;
			log = 1;
		} else if (!obs_unk_capped) {
			obs_unk_capped = capped = 1;
		}
	}
	pthread_mutex_unlock(&obs_unk_mu);
	if (log)
		LM_NOTICE("unknown command '%s' (%s) from %s%s%s%s - first "
			"sighting on this node\n", name, clunk_dialect_name(d),
			addr, client[0] ? " [" : "", client, client[0] ? "]" : "");
	else if (capped)
		LM_NOTICE("unknown commands: %d distinct names logged, further "
			"first sightings are counted in /stats but not logged\n",
			OBS_UNK_SEEN);
}

void pc_obs_unknown(int dialect, const char *cmd, size_t clen,
		const char *sub, size_t slen, void *connrow)
{
	struct connrow *c = connrow;
	struct unkw *w = unk_row();
	char name[CLUNK_NAME], cl[CLUNK_CLIENT];
	const char *addr = c ? c->addr : "?";
	size_t n;

	if (!w || dialect < 0 || dialect >= CLUNK_DIALECTS || !clen)
		return;
	n = clunk_name(name, dialect == CLUNK_RESP, cmd, clen, sub, slen);
	cl[0] = 0;
	if (c)
		snprintf(cl, sizeof cl, "%.*s", (int)c->nlen, c->name);
	clunk_clean(cl);
	w->tot[dialect]++;
	__atomic_add_fetch(&obs_unk_gen, 1, __ATOMIC_RELAXED);
	if (clunk_note(&w->t, dialect, name, n, (uint64_t)time(NULL), addr,
	        cl) == 1)
		unk_first(dialect, name, n, addr, cl);
}

void pc_obs_unknown_preauth(int dialect)
{
	struct unkw *w = unk_row();

	(void)dialect;                     /* one count: nothing to name */
	if (!w)
		return;
	w->t.preauth++;
	__atomic_add_fetch(&obs_unk_gen, 1, __ATOMIC_RELAXED);
}

void pc_obs_unknown_local(struct clunk_table *out)
{
	unsigned int ep = __atomic_load_n(&obs_unk_epoch, __ATOMIC_RELAXED);
	int row;

	memset(out, 0, sizeof *out);
	for (row = 0; obs_unk && row < obs_rows; row++)
		if (obs_unk[row].epoch == ep)
			clunk_fold(out, &obs_unk[row].t, 0);
}

unsigned long long pc_obs_unknown_gen(void)
{
	return __atomic_load_n(&obs_unk_gen, __ATOMIC_RELAXED);
}

void pc_obs_unknown_totals(unsigned long long tot[CLUNK_DIALECTS],
		unsigned long long *preauth)
{
	unsigned int ep = __atomic_load_n(&obs_unk_epoch, __ATOMIC_RELAXED);
	int row, d;

	for (d = 0; d < CLUNK_DIALECTS; d++)
		tot[d] = 0;
	*preauth = 0;
	for (row = 0; obs_unk && row < obs_rows; row++) {
		if (obs_unk[row].epoch != ep)
			continue;
		for (d = 0; d < CLUNK_DIALECTS; d++)
			tot[d] += obs_unk[row].tot[d];
		*preauth += obs_unk[row].t.preauth;
	}
}

/* what the resets put out of reach, added back for /metrics; written only by the reset (which
 * the daemon serialises), read fuzzily like the rows */
static unsigned long long obs_unk_carry[CLUNK_DIALECTS], obs_unk_carry_pre;

void pc_obs_unknown_totals_raw(unsigned long long tot[CLUNK_DIALECTS],
		unsigned long long *preauth)
{
	int d;

	pc_obs_unknown_totals(tot, preauth);
	for (d = 0; d < CLUNK_DIALECTS; d++)
		tot[d] += obs_unk_carry[d];
	*preauth += obs_unk_carry_pre;
}

void pc_obs_unknown_reset(void)
{
	unsigned long long tot[CLUNK_DIALECTS], pre;
	int d;

	pc_obs_unknown_totals(tot, &pre);
	for (d = 0; d < CLUNK_DIALECTS; d++)
		obs_unk_carry[d] += tot[d];
	obs_unk_carry_pre += pre;
	__atomic_add_fetch(&obs_unk_epoch, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&obs_unk_gen, 1, __ATOMIC_RELAXED);
}

/* for the connection log - the address exactly as CLIENT LIST shows
 * it, and how many requests the connection carried before it closed */
const char *pc_obs_conn_addr(void *row)
{
	return row ? ((struct connrow *)row)->addr : "?:0";
}

unsigned long long pc_obs_conn_cmds(void *row)
{
	return row ? ((struct connrow *)row)->cmds : 0;
}
