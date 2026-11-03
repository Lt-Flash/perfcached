/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * reload.c - see reload.h and DESIGN 12nw.
 *
 * The comparison is by KEY, not by field: the parser records every
 * key = value it reads as a keyed digest (config.c, pc_config_load_km), the
 * daemon keeps the set it started with, and a reload diffs a fresh read
 * against it.  A key added, removed or given another value counts as
 * changed, so a key deleted to fall back to its default is seen too.  The
 * whitelist below is the whole of what a running daemon can take; anything
 * else that changed refuses the reload, names the keys and leaves the
 * running configuration exactly as it was.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>

#include "reload.h"
#include "obs.h"
#include "compat/dprint.h"

static const struct {
	const char *sect, *key;
	unsigned int bit;
	const char *what;
} RL[] = {
	{ "listen",  "resp_allow",   PC_RL_RESP_ALLOW,    "resp_allow" },
	{ "listen",  "http_allow",   PC_RL_HTTP_ALLOW,    "http_allow" },
	{ "secrets", "http",         PC_RL_HTTP_TOKEN,    "the HTTP token" },
	{ "secrets", "resp",         PC_RL_RESP_PASSWORD, "the RESP password" },
	{ "secrets", "enable",       PC_RL_ENABLE,        "the enable secrets" },
	{ "secrets", "client",       PC_RL_CLIENT,        "the client secrets" },
	{ "daemon",  "log_level",    PC_RL_LOGLEVEL,      "log_level" },
	{ "daemon",  "slowlog_usec", PC_RL_SLOWLOG,       "slowlog_usec" },
};
#define NRL ((int)(sizeof RL / sizeof RL[0]))

static pthread_mutex_t RLMX = PTHREAD_MUTEX_INITIALIZER;
static struct pc_keymap *BASE;
static char RLPATH[1024];
static char RLFILE[1024];          /* the outcome file; "" = none */
static volatile sig_atomic_t RLASK;
static unsigned long long RLGEN, RLOK, RLREFUSED;
static long long RLLAST;
static char RLMSG[512];

static const char *result_dir(const char *state_dir)
{
	static char rd[1024];
	const char *e = getenv("RUNTIME_DIRECTORY");

	if (e && *e) {                 /* systemd: one or more, ':'-separated */
		size_t n = strcspn(e, ":");

		if (n >= sizeof rd)
			n = sizeof rd - 1;
		memcpy(rd, e, n);
		rd[n] = 0;
		return rd;
	}
	return state_dir && *state_dir ? state_dir : NULL;
}

void pc_reload_init(const char *path, struct pc_keymap *boot,
		const char *state_dir)
{
	const char *d = result_dir(state_dir);

	snprintf(RLPATH, sizeof RLPATH, "%s", path ? path : "");
	BASE = boot;
	RLFILE[0] = 0;
	if (d)
		snprintf(RLFILE, sizeof RLFILE, "%s/reload", d);
}

void pc_reload_ask(void)
{
	RLASK = 1;
}

void pc_reload_tick(void)
{
	char m[512];

	if (!RLASK)
		return;
	RLASK = 0;
	(void)pc_reload_run(m, sizeof m);
}

void pc_reload_stats(unsigned long long *ok, unsigned long long *refused,
		long long *last_unix, const char **last_msg)
{
	*ok = __atomic_load_n(&RLOK, __ATOMIC_RELAXED);
	*refused = __atomic_load_n(&RLREFUSED, __ATOMIC_RELAXED);
	*last_unix = __atomic_load_n(&RLLAST, __ATOMIC_RELAXED);
	*last_msg = RLMSG;
}

/* the outcome for -H: "gen=<n> ok=<0|1> pid=<pid>" then the words */
static void write_result(int ok, const char *msg)
{
	char tmp[1100];
	FILE *f;

	if (!RLFILE[0])
		return;
	snprintf(tmp, sizeof tmp, "%s.tmp", RLFILE);
	f = fopen(tmp, "w");
	if (!f)
		return;
	fprintf(f, "gen=%llu ok=%d pid=%ld\n%s\n", RLGEN, ok, (long)getpid(), msg);
	if (fclose(f) != 0 || rename(tmp, RLFILE) != 0)
		unlink(tmp);
}

static int same_key(const struct pc_km_ent *a, const struct pc_km_ent *b)
{
	return !strcmp(a->sect, b->sect) && !strcmp(a->key, b->key);
}

/* did (sect, key) of entry @at in @x read the same in @y - the same
 * values in the same order (a key may repeat: client, enable)? */
static int key_same(const struct pc_keymap *x, int at, const struct pc_keymap *y)
{
	int i, j = 0;

	for (i = 0; i < x->n; i++) {
		if (!same_key(&x->e[i], &x->e[at]))
			continue;
		while (j < y->n && !same_key(&y->e[j], &x->e[at]))
			j++;
		if (j >= y->n || memcmp(y->e[j].h, x->e[i].h, sizeof x->e[i].h))
			return 0;
		j++;
	}
	while (j < y->n && !same_key(&y->e[j], &x->e[at]))
		j++;
	return j >= y->n;              /* @y has no more of it either */
}

static int first_of(const struct pc_keymap *x, int at)
{
	int i;

	for (i = 0; i < at; i++)
		if (same_key(&x->e[i], &x->e[at]))
			return 0;
	return 1;
}

static int present(const struct pc_keymap *x, const struct pc_km_ent *k)
{
	int i;

	for (i = 0; i < x->n; i++)
		if (same_key(&x->e[i], k))
			return 1;
	return 0;
}

static void add_name(char *buf, size_t cap, const struct pc_km_ent *k)
{
	size_t n = strlen(buf);

	if (n + 4 >= cap)
		return;
	snprintf(buf + n, cap - n, "%s[%s] %s", n ? ", " : "", k->sect, k->key);
}

/* classify one changed key: its PC_RL_ bit, or 0 when it cannot reload */
static unsigned int bit_of(const struct pc_km_ent *k, const char **what)
{
	int i;

	for (i = 0; i < NRL; i++)
		if (!strcmp(k->sect, RL[i].sect) && !strcmp(k->key, RL[i].key)) {
			*what = RL[i].what;
			return RL[i].bit;
		}
	return 0;
}

static void note_what(char *buf, size_t cap, const char *what)
{
	size_t n = strlen(buf);

	if (strstr(buf, what) || n + 4 >= cap)
		return;
	snprintf(buf + n, cap - n, "%s%s", n ? ", " : "", what);
}

static void consider(const struct pc_km_ent *k, unsigned int *bits,
		char *refused, size_t rcap, char *applied, size_t acap)
{
	const char *what = NULL;
	unsigned int b = bit_of(k, &what);

	if (b) {
		*bits |= b;
		note_what(applied, acap, what);
	} else {
		add_name(refused, rcap, k);
	}
}

int pc_reload_run(char *msg, size_t ml)
{
	struct pc_keymap *km;
	struct pc_config nc;
	char refused[400] = "", applied[300] = "", err[200] = "";
	unsigned int bits = 0;
	int i, rc = 1;

	pthread_mutex_lock(&RLMX);
	RLGEN++;
	km = calloc(1, sizeof *km);
	if (!km || !BASE) {
		snprintf(msg, ml, "reload: %s - nothing changed",
			km ? "the daemon kept no record of its configuration" : "out of memory");
		free(km);
		goto out;
	}
	if (pc_config_load_km(RLPATH, &nc, km) != 0) {
		pc_config_free(&nc);
		snprintf(msg, ml, "reload refused: %s does not load (the lines above say why) - "
			"nothing changed", RLPATH);
		free(km);
		goto out;
	}
	if (km->overflow || BASE->overflow) {
		snprintf(msg, ml, "reload refused: more than %d keys to compare - nothing changed",
			PC_KM_MAX);
		goto drop;
	}
	/* every key that reads differently: changed, removed (in BASE only) or
	 * added (in the new file only) */
	for (i = 0; i < BASE->n; i++)
		if (first_of(BASE, i) && !key_same(BASE, i, km))
			consider(&BASE->e[i], &bits, refused, sizeof refused, applied, sizeof applied);
	for (i = 0; i < km->n; i++)
		if (first_of(km, i) && !present(BASE, &km->e[i]))
			consider(&km->e[i], &bits, refused, sizeof refused, applied, sizeof applied);
	if (refused[0]) {
		snprintf(msg, ml, "reload refused: %s changed and cannot be reloaded - restart to "
			"apply %s; nothing changed%s", refused,
			strchr(refused, ',') ? "them" : "it",
			/* S432: the map has a runtime verb, and its maps outrank the
			 * file across a restart too */
			strstr(refused, "] resp_collections") ?
			" (a RESP DB index moves without a restart: perfcli map "
			"<index> <collection>; runtime maps outrank resp_collections)" : "");
		goto drop;
	}
	if (!bits) {
		snprintf(msg, ml, "reload: nothing changed");
		rc = 0;
		goto drop;
	}
	if (pc_live_reload(&nc, bits, err, sizeof err) != 0) {
		snprintf(msg, ml, "reload failed: %s - nothing changed", err);
		goto drop;
	}
	if (bits & PC_RL_LOGLEVEL)
		__atomic_store_n(&compat_log_level, nc.log_level, __ATOMIC_RELAXED);
	if (bits & PC_RL_SLOWLOG)
		pc_obs_set_slowlog(nc.slowlog_usec);
	snprintf(msg, ml, "reloaded: %s%s%s", applied, err[0] ? " - " : "", err);
	free(BASE);
	BASE = km;                     /* the new file is what runs now */
	km = NULL;
	rc = 0;
drop:
	pc_config_wipe_secrets(&nc);
	pc_config_free(&nc);
	free(km);
out:
	if (rc == 0)
		__atomic_add_fetch(&RLOK, 1, __ATOMIC_RELAXED);
	else
		__atomic_add_fetch(&RLREFUSED, 1, __ATOMIC_RELAXED);
	__atomic_store_n(&RLLAST, (long long)time(NULL), __ATOMIC_RELAXED);
	snprintf(RLMSG, sizeof RLMSG, "%s", msg);
	if (rc == 0)
		LM_NOTICE("%s\n", msg);
	else
		LM_ERR("%s\n", msg);
	write_result(rc == 0, msg);
	pthread_mutex_unlock(&RLMX);
	return rc;
}

/* ---- -H <pid>: the ExecReload side ------------------------------------- */

static unsigned long long read_gen(const char *file, long *pid, int *ok,
		char *msg, size_t ml)
{
	char line[256];
	unsigned long long g = 0;
	FILE *f = fopen(file, "r");

	*pid = 0;
	*ok = 0;
	if (msg && ml)
		msg[0] = 0;
	if (!f)
		return 0;
	if (fgets(line, sizeof line, f))
		sscanf(line, "gen=%llu ok=%d pid=%ld", &g, ok, pid);
	if (msg && ml && fgets(msg, (int)ml, f))
		msg[strcspn(msg, "\n")] = 0;
	fclose(f);
	return g;
}

int pc_reload_client(const char *path, long pid)
{
	struct pc_config c;
	char file[1100], msg[600];
	const char *d;
	unsigned long long g0, g;
	long fpid = 0;                     /* read_gen sets it only when there is a file */
	int ok, i;

	if (pid <= 1) {
		fprintf(stderr, "perfcached -H: no daemon pid given\n");
		return 1;
	}
	/* the file must load before the daemon is asked - the same check -C
	 * makes, so a broken file is refused here with its line numbers.  Only
	 * its errors are shown: the notices describe listeners the reload
	 * does not touch, and systemctl prints whatever this says */
	compat_log_level = L_ERR;
	if (pc_config_load(path, &c) != 0) {
		fprintf(stderr, "perfcached -H: %s does not load - not reloaded\n", path);
		pc_config_free(&c);
		return 1;
	}
	d = result_dir(c.state_dir);
	file[0] = 0;
	if (d)
		snprintf(file, sizeof file, "%s/reload", d);
	pc_config_wipe_secrets(&c);
	pc_config_free(&c);
	g0 = file[0] ? read_gen(file, &fpid, &ok, NULL, 0) : 0;
	if (fpid != pid)
		g0 = 0;                    /* another run's file: any gen is new */
	if (kill((pid_t)pid, SIGHUP) != 0) {
		fprintf(stderr, "perfcached -H: cannot signal %ld: %s\n", pid, strerror(errno));
		return 1;
	}
	if (!file[0]) {
		printf("perfcached -H: asked %ld to reload; with no RUNTIME_DIRECTORY and no "
			"state_dir its answer is only in its log\n", pid);
		return 0;
	}
	for (i = 0; i < 300; i++) {    /* 30 s: a client secret takes an Argon2id run */
		struct timespec ts = { 0, 100 * 1000 * 1000 };

		nanosleep(&ts, NULL);
		g = read_gen(file, &fpid, &ok, msg, sizeof msg);
		if (fpid == pid && g > g0) {
			printf("%s\n", msg);
			return ok ? 0 : 1;
		}
	}
	fprintf(stderr, "perfcached -H: %ld did not answer within 30 s - see its log\n", pid);
	return 1;
}
