/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * configint.h - the config parser's internal interface (config.c): the
 * parse state, the section being read, the error macro, and the value
 * parsers the sections share.  For the files the parser is split into
 * (the 0.7.2 split, wave 1 - doc/MODULARITY.md); config.h stays the API.
 */
#ifndef PC_CONFIGINT_H
#define PC_CONFIGINT_H

#include <stdio.h>

#include "compat/dprint.h"
#include "config.h"

enum sect { S_NONE, S_DAEMON, S_MEMORY, S_SECRETS, S_LISTEN, S_WAL, S_CLUSTER, S_COLLECTION, S_MAIL,
	S_ALERTS, S_UPGRADE };

struct pstate {
	const char *path;
	int line;
	enum sect sect;
	struct pc_collection *col;    /* current [collection X] */
	struct pc_config *cfg;
};

#define PERR(ps, ...) do { \
	char _pe[512]; \
	snprintf(_pe, sizeof _pe, __VA_ARGS__); \
	if ((ps)->line) \
		compat_log(L_ERR, "ERROR", "%s:%d: %s", (ps)->path, \
			(ps)->line, _pe); \
	else \
		compat_log(L_ERR, "ERROR", "%s: %s", (ps)->path, _pe); \
} while (0)

/* the value parsers (config.c) */
char *trim(char *s);
/* an integer in [@lo, @hi] into *out; 0, or -1 with the error said */
int parse_int(struct pstate *ps, const char *v, int lo, int hi, int *out);
int level_of(const char *v);                  /* a log level's name: L_* or -1 */
int mail_url_ok(struct pstate *ps, const char *url);
int mail_addr_ok(const char *a);
int mode_of(const char *v, int *eager);       /* a collection mode's name */
int split_hostport(struct pstate *ps, const char *v, const char *form,
		char **host, int *port);
int add_listener_ex(struct pstate *ps, enum pc_listen_type type,
		const char *v, int kind);
int add_listener(struct pstate *ps, enum pc_listen_type type, const char *v);
int add_resp_allow(struct pstate *ps, const char *v);
int add_http_allow(struct pstate *ps, const char *v);
int stats_reset_parse(const char *v, int *days);
int hhmm_parse(const char *v, int *secs);
const char *mode_name(enum pc_pull_mode m, int eager);  /* a mode's config name */

/* the rules, once the file is read (cfgcheck.c): 0, or -1 with the error said */
int cfg_validate(struct pstate *ps, const char *path, struct pc_config *cfg);

/* one key of a section: 0 taken, -1 refused (said), or CFG_KV_UNKNOWN - not
 * a key of this section (config.c says so) */
#define CFG_KV_UNKNOWN 1
int cfg_kv_daemon(struct pstate *ps, char *key, char *val);      /* cfgcore.c */
int cfg_kv_memory(struct pstate *ps, char *key, char *val);
int cfg_kv_secrets(struct pstate *ps, char *key, char *val);
int cfg_kv_listen(struct pstate *ps, char *key, char *val);
int cfg_kv_wal(struct pstate *ps, char *key, char *val);
int cfg_kv_cluster(struct pstate *ps, char *key, char *val);     /* cfgcluster.c */
int cfg_kv_collection(struct pstate *ps, char *key, char *val);
int cfg_kv_mail(struct pstate *ps, char *key, char *val);        /* cfgops.c */
int cfg_kv_alerts(struct pstate *ps, char *key, char *val);
int cfg_kv_upgrade(struct pstate *ps, char *key, char *val);

#endif
