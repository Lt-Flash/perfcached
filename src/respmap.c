/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * respmap.c - S432: the RESP door's DB-index map at runtime.  See respmap.h.
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "compat/compat.h"              /* pc_lamport_restore */
#include "compat/dprint.h"
#include "quiesce.h"
#include "respmap.h"
#include "store.h"                     /* PC_COL_NAME_MAX */

#define RESPMAP_HEADER "perfcached-respmap 1"

struct ov {
	char col[PC_COL_NAME_MAX];
	uint64_t gen;
	unsigned char held;                /* an override (or its tombstone) */
	unsigned char set;                 /* 1 mapped, 0 unmapped (tombstone) */
};

static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static struct ov OV[PC_RESPMAP_MAX];
static char *file_list;                /* the config's, NULL = every collection */
static char state_dir[512];
static char *cur;                      /* the published effective list */
static int inited;

/* old lists, freed once no worker can still hold them */
#define RETIRE_MAX 16
static struct { char *p; unsigned long long stamp; } retired[RETIRE_MAX];
static int nretired;

/* ---- the list's own grammar (the door's readers use the same) -------- */

/* the next entry of @p (a comma list, spaces and tabs around): its start
 * and length, *@next past it; 0 at the end */
static int next_entry(const char **p, const char **s, size_t *len)
{
	const char *q = *p, *e;
	size_t n;

	while (*q == ' ' || *q == ',' || *q == '\t')
		q++;
	if (!*q) {
		*p = q;
		return 0;
	}
	e = q;
	while (*e && *e != ',')
		e++;
	n = (size_t)(e - q);
	while (n && (q[n - 1] == ' ' || q[n - 1] == '\t'))
		n--;
	*s = q;
	*len = n;
	*p = e;
	return 1;
}

/* an entry's index text: before the colon of INDEX:NAME, else the whole
 * (a bare entry is addressed by itself) */
static size_t entry_index(const char *s, size_t len)
{
	const char *c = memchr(s, ':', len);

	return c ? (size_t)(c - s) : len;
}

/* @s,@n is the decimal @idx exactly */
static int is_index(const char *s, size_t n, int idx)
{
	char b[8];
	int l = snprintf(b, sizeof b, "%d", idx);

	return (size_t)l == n && !memcmp(s, b, n);
}

/* what @idx resolves to in @list, as the door would (resp_map_index, then
 * resp_col_allowed): the NAME of an INDEX:NAME entry; else the index
 * itself when the list allows it (NULL list, "*", or a bare entry).  1 when
 * an entry names the index, 0 when only an open door lets it through */
static int resolve_in(const char *list, int idx, char *out, size_t cap)
{
	const char *p = list, *s;
	size_t len, il;
	int allowed = !list, star = 0, named = 0;
	char b[8];

	out[0] = 0;
	if (list)
		while (next_entry(&p, &s, &len)) {
			il = entry_index(s, len);
			if (il < len && is_index(s, il, idx)) {
				size_t nl = len - il - 1;

				if (nl && nl < cap) {
					memcpy(out, s + il + 1, nl);
					out[nl] = 0;
					return 1;
				}
			}
			if (len == 1 && s[0] == '*')
				star = 1;
			else if (il == len && is_index(s, len, idx))
				named = 1;
		}
	if (allowed || star || named) {
		snprintf(b, sizeof b, "%d", idx);
		snprintf(out, cap, "%s", b);
	}
	return named;
}

/* ---- the effective list ------------------------------------------------ */

/* under mx: the file's list with each overridden index replaced, or NULL
 * when nothing is overridden and the file has no list */
static char *build(void)
{
	size_t cap = 2, used = 0;
	const char *p, *s;
	size_t len;
	int i, any = 0;
	char *b;

	for (i = 0; i < PC_RESPMAP_MAX; i++)
		if (OV[i].held && OV[i].set) {
			any = 1;
			cap += strlen(OV[i].col) + 8;
		}
	if (!any)
		return file_list ? strdup(file_list) : NULL;
	cap += file_list ? strlen(file_list) + 2 : 2;
	b = malloc(cap);
	if (!b)
		return NULL;
	b[0] = 0;
#define ADD(str, n) do { if (used) b[used++] = ','; \
		memcpy(b + used, (str), (n)); used += (n); b[used] = 0; } while (0)
	if (!file_list)
		ADD("*", 1);                   /* the open door stays open */
	/* the overrides FIRST: INDEX:NAME is read in order by resp_index_for,
	 * so a collection the file also maps elsewhere is reported (INFO
	 * keyspace) under the index the operator moved it to */
	for (i = 0; i < PC_RESPMAP_MAX; i++)
		if (OV[i].held && OV[i].set) {
			char e[PC_COL_NAME_MAX + 8];
			int n = snprintf(e, sizeof e, "%d:%s", i, OV[i].col);

			ADD(e, (size_t)n);
		}
	for (p = file_list; p && next_entry(&p, &s, &len); ) {
		size_t il = entry_index(s, len);
		int skip = 0;

		for (i = 0; i < PC_RESPMAP_MAX && !skip; i++)
			if (OV[i].held && OV[i].set && is_index(s, il, i))
				skip = 1;
		if (!skip)
			ADD(s, len);
	}
#undef ADD
	return b;
}

/* under mx: free what no worker can still hold */
static void reap(void)
{
	int i, j = 0;

	for (i = 0; i < nretired; i++) {
		if (pc_qs_clear(retired[i].stamp))
			free(retired[i].p);
		else
			retired[j++] = retired[i];
	}
	nretired = j;
}

/* under mx: publish a new effective list; the old one is retired */
static void publish(void)
{
	char *n = build(), *old;

	old = __atomic_exchange_n(&cur, n, __ATOMIC_ACQ_REL);
	reap();
	if (!old)
		return;
	if (nretired == RETIRE_MAX) {
		/* sixteen changes inside one quiescent window (a pipeline of
		 * maps in one worker turn holds its own line inside, so waiting
		 * here could never end): the oldest is let go UNFREED - a few
		 * dozen bytes - rather than freed under a reader or a worker
		 * held waiting */
		LM_WARN("resp: %d map changes inside one worker turn - an old map "
			"string is kept, not freed\n", RETIRE_MAX + 1);
		memmove(retired, retired + 1, sizeof retired[0] * (RETIRE_MAX - 1));
		nretired--;
	}
	retired[nretired].p = old;
	retired[nretired].stamp = pc_qs_stamp();
	nretired++;
}

int pc_respmap_ready(void)
{
	return __atomic_load_n(&inited, __ATOMIC_ACQUIRE);
}

const char *pc_respmap_config(void)
{
	return file_list;                  /* set once, before the workers */
}

const char *pc_respmap_effective(void)
{
	return __atomic_load_n(&cur, __ATOMIC_ACQUIRE);
}

/* ---- overrides ---------------------------------------------------------- */

static int col_ok(const char *c, size_t n)
{
	size_t i;

	if (!n || n >= PC_COL_NAME_MAX)
		return 0;
	for (i = 0; i < n; i++)                /* the list's own separators */
		if (c[i] == ',' || c[i] == ':' || c[i] == ' ' || c[i] == '\t' ||
		    c[i] == '\n' || c[i] == '\r' || c[i] == '*')
			return 0;
	return 1;
}

int pc_respmap_apply(int index, const char *col, size_t cl, uint64_t gen,
		int set, char *was, size_t wcap)
{
	struct ov *o;
	char nc[PC_COL_NAME_MAX] = "";

	if (index < 0 || index >= PC_RESPMAP_MAX)
		return -1;
	if (set) {
		if (!col_ok(col, cl))
			return -1;
		memcpy(nc, col, cl);
		nc[cl] = 0;
	}
	pthread_mutex_lock(&mx);
	o = &OV[index];
	if (o->held) {
		if (gen < o->gen ||
		    (gen == o->gen && strcmp(nc, o->col) <= 0)) {
			pthread_mutex_unlock(&mx);
			return 0;                  /* a newer one is held */
		}
	}
	if (was && wcap)
		resolve_in(cur, index, was, wcap);
	o->held = 1;
	o->set = (unsigned char)(set ? 1 : 0);
	o->gen = gen;
	snprintf(o->col, sizeof o->col, "%s", nc);
	publish();
	pthread_mutex_unlock(&mx);
	return 1;
}

int pc_respmap_resolve(int index, char *out, size_t cap)
{
	int named, src;

	if (!cap)
		return PC_RESPMAP_OPEN;
	out[0] = 0;
	if (index < 0 || index >= PC_RESPMAP_MAX)
		return PC_RESPMAP_OPEN;
	pthread_mutex_lock(&mx);
	named = resolve_in(cur, index, out, cap);
	if (OV[index].held && OV[index].set)
		src = PC_RESPMAP_RUNTIME;
	else
		src = named ? PC_RESPMAP_CONFIG : PC_RESPMAP_OPEN;
	pthread_mutex_unlock(&mx);
	return src;
}

void pc_respmap_each(pc_respmap_cb cb, void *ctx)
{
	struct ov copy[PC_RESPMAP_MAX];
	int i;

	pthread_mutex_lock(&mx);
	memcpy(copy, OV, sizeof copy);
	pthread_mutex_unlock(&mx);
	for (i = 0; i < PC_RESPMAP_MAX; i++)
		if (copy[i].held)
			cb(i, copy[i].col, copy[i].gen, copy[i].set, ctx);
}

int pc_respmap_overrides(void)
{
	int i, n = 0;

	pthread_mutex_lock(&mx);
	for (i = 0; i < PC_RESPMAP_MAX; i++)
		if (OV[i].held && OV[i].set)
			n++;
	pthread_mutex_unlock(&mx);
	return n;
}

/* ---- persistence -------------------------------------------------------- */

int pc_respmap_persist(void)
{
	char path[600], tmp[620];
	FILE *f;
	int i, rc = 0;

	if (!state_dir[0])
		return 0;                      /* ephemeral by configuration */
	snprintf(path, sizeof path, "%s/respmap", state_dir);
	snprintf(tmp, sizeof tmp, "%s/respmap.tmp", state_dir);
	f = fopen(tmp, "w");
	if (!f)
		return -1;
	fprintf(f, "%s\n", RESPMAP_HEADER);
	pthread_mutex_lock(&mx);
	for (i = 0; i < PC_RESPMAP_MAX; i++)
		if (OV[i].held)
			fprintf(f, "map %d %llu %d %s\n", i,
				(unsigned long long)OV[i].gen, OV[i].set ? 1 : 0,
				OV[i].set ? OV[i].col : "-");
	pthread_mutex_unlock(&mx);
	if (fflush(f) != 0 || fsync(fileno(f)) != 0)
		rc = -1;
	fclose(f);
	if (rc == 0 && rename(tmp, path) != 0)
		rc = -1;
	if (rc != 0)
		unlink(tmp);
	return rc;
}

/* read <dir>/respmap: each line through @fn; -1 for a file that is not ours,
 * 0 for none or a good one */
static int read_file(const char *dir, void (*fn)(int, uint64_t, int,
		const char *, void *), void *ctx)
{
	char path[600], line[256], col[PC_COL_NAME_MAX + 8];
	FILE *f;

	snprintf(path, sizeof path, "%s/respmap", dir);
	f = fopen(path, "r");
	if (!f) {
		if (errno == ENOENT)
			return 0;
		LM_ERR("respmap file %s cannot be read: %s\n", path,
			strerror(errno));
		return -1;
	}
	if (!fgets(line, sizeof line, f) ||
	    strncmp(line, RESPMAP_HEADER, strlen(RESPMAP_HEADER))) {
		fclose(f);
		LM_ERR("respmap file %s does not start with '%s' - refusing to "
			"guess at it\n", path, RESPMAP_HEADER);
		return -1;
	}
	while (fgets(line, sizeof line, f)) {
		unsigned long long gen;
		int idx, set;

		if (sscanf(line, "map %d %llu %d %70s", &idx, &gen, &set, col) != 4)
			continue;              /* a line a later build added */
		if (idx < 0 || idx >= PC_RESPMAP_MAX)
			continue;
		fn(idx, gen, set, col, ctx);
	}
	fclose(f);
	return 0;
}

static void load_one(int idx, uint64_t gen, int set, const char *col, void *ctx)
{
	(void)ctx;
	if (set && !col_ok(col, strlen(col)))
		return;
	OV[idx].held = 1;
	OV[idx].set = (unsigned char)(set ? 1 : 0);
	OV[idx].gen = gen;
	snprintf(OV[idx].col, sizeof OV[idx].col, "%s", set ? col : "");
}

static void count_one(int idx, uint64_t gen, int set, const char *col, void *ctx)
{
	(void)idx;
	(void)gen;
	(void)col;
	if (set)
		(*(int *)ctx)++;
}

int pc_respmap_count_file(const char *dir)
{
	int n = 0;

	if (!dir || !*dir)
		return -1;
	return read_file(dir, count_one, &n) == 0 ? n : -1;
}

int pc_respmap_init(const char *list, const char *dir)
{
	int rc = 0;

	pthread_mutex_lock(&mx);
	free(file_list);
	file_list = list ? strdup(list) : NULL;
	snprintf(state_dir, sizeof state_dir, "%s", dir ? dir : "");
	memset(OV, 0, sizeof OV);
	if (state_dir[0])
		rc = read_file(state_dir, load_one, NULL);
	{
		/* the clock resumes above every change loaded: a map minted
		 * after the last data write is above what recovery restores,
		 * and an unmap after a restart must outrank it.  restore, not
		 * observe - at boot the clock is 0 (recover.c says why) */
		uint64_t high = 0;
		int i;

		for (i = 0; i < PC_RESPMAP_MAX; i++)
			if (OV[i].held && OV[i].gen > high)
				high = OV[i].gen;
		pc_lamport_restore(high);
	}
	free(cur);                         /* before the workers: no reader yet */
	cur = build();
	__atomic_store_n(&inited, 1, __ATOMIC_RELEASE);
	pthread_mutex_unlock(&mx);
	if (rc == 0 && pc_respmap_overrides() > 0)
		LM_NOTICE("resp: %d DB index(es) mapped at runtime (S432) override "
			"resp_collections - perfcli maps lists them\n",
			pc_respmap_overrides());
	return rc;
}
