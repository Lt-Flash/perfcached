/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * cfgops.c - the config sections of operations: [mail], [alerts] and
 * [upgrade].  Moved from config.c's handle_kv, each case body unchanged
 * but for its indent and its last break (the 0.7.2 split, wave 1 -
 * doc/MODULARITY.md).
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sodium.h>
#include "compat/dprint.h"
#include "pcaddr.h"
#include "config.h"
#include "colname.h"
#include "events.h"
#include "relkey.h"
#include "configint.h"

/* [mail] - S352 */
int cfg_kv_mail(struct pstate *ps, char *key, char *val)
{
	struct pc_config *cfg = ps->cfg;

	if (!strcasecmp(key, "smtp")) {
		if (!mail_url_ok(ps, val))
			return -1;
		free(cfg->mail.url);
		cfg->mail.url = strdup(val);
		return 0;
	}
	if (!strcasecmp(key, "user")) {
		free(cfg->mail.user);
		cfg->mail.user = strdup(val);
		return 0;
	}
	if (!strcasecmp(key, "from")) {
		if (!mail_addr_ok(val)) {
			PERR(ps, "from = '%s': not an address "
				"(local@domain, no display name)\n", val);
			return -1;
		}
		free(cfg->mail.from);
		cfg->mail.from = strdup(val);
		return 0;
	}
	if (!strcasecmp(key, "to")) {
		char *dup = strdup(val), *save = NULL, *t;

		if (!dup)
			return -1;
		for (t = strtok_r(dup, ",", &save); t;
		     t = strtok_r(NULL, ",", &save)) {
			t = trim(t);
			if (!*t)
				continue;
			if (!mail_addr_ok(t)) {
				PERR(ps, "to: '%s' is not an address "
					"(local@domain, no display name)\n", t);
				free(dup);
				return -1;
			}
			if (cfg->mail.nto >= PC_MAIL_TO_MAX) {
				PERR(ps, "to: more than %d recipients\n",
					PC_MAIL_TO_MAX);
				free(dup);
				return -1;
			}
			cfg->mail.to[cfg->mail.nto++] = strdup(t);
		}
		free(dup);
		return 0;
	}
	if (!strcasecmp(key, "ca_file")) {
		if (val[0] != '/') {
			PERR(ps, "ca_file must be absolute\n");
			return -1;
		}
		free(cfg->mail.ca_file);
		cfg->mail.ca_file = strdup(val);
		return 0;
	}
	if (!strcasecmp(key, "tls_verify")) {
		if (!strcasecmp(val, "yes") || !strcasecmp(val, "on"))
			cfg->mail.tls_verify = 1;
		else if (!strcasecmp(val, "no") || !strcasecmp(val, "off"))
			cfg->mail.tls_verify = 0;
		else {
			PERR(ps, "tls_verify: yes|no\n");
			return -1;
		}
		return 0;
	}
	if (!strcasecmp(key, "subject_prefix")) {
		free(cfg->mail.subject_prefix);
		cfg->mail.subject_prefix = strdup(val);
		return 0;
	}
	if (!strcasecmp(key, "timeout_s"))
		return parse_int(ps, val, 1, 300, &cfg->mail.timeout_s);
	if (!strcasecmp(key, "min_interval_s"))
		return parse_int(ps, val, 0, 86400, &cfg->mail.min_interval_s);
	if (!strcasecmp(key, "digest_s"))
		return parse_int(ps, val, 0, 3600, &cfg->mail.digest_s);
	if (!strcasecmp(key, "level")) {
		if (!strcasecmp(val, "warning"))
			cfg->mail.level = PC_MAIL_LVL_WARNING;
		else if (!strcasecmp(val, "critical"))
			cfg->mail.level = PC_MAIL_LVL_CRITICAL;
		else if (!strcasecmp(val, "info"))           /* S377 */
			cfg->mail.level = PC_MAIL_LVL_INFO;
		else {
			PERR(ps, "level: info|warning|critical\n");
			return -1;
		}
		return 0;
	}
	return CFG_KV_UNKNOWN;
}

/* [alerts] - S380 */
int cfg_kv_alerts(struct pstate *ps, char *key, char *val)
{
	struct pc_config *cfg = ps->cfg;

	static const struct { const char *k; size_t off; } ak[] = {
		{ "cpu_warn_pct", offsetof(struct pc_alerts_conf, cpu_warn) },
		{ "cpu_crit_pct", offsetof(struct pc_alerts_conf, cpu_crit) },
		{ "io_warn_pct", offsetof(struct pc_alerts_conf, io_warn) },
		{ "io_crit_pct", offsetof(struct pc_alerts_conf, io_crit) },
		{ "memory_warn_pct", offsetof(struct pc_alerts_conf, mem_warn) },
		{ "memory_crit_pct", offsetof(struct pc_alerts_conf, mem_crit) },
		{ "arena_warn_pct", offsetof(struct pc_alerts_conf, arena_warn) },
		{ "arena_crit_pct", offsetof(struct pc_alerts_conf, arena_crit) },
		{ "epoll_busy_pct", offsetof(struct pc_alerts_conf, epoll_busy) },
	};
	size_t i;

	for (i = 0; i < sizeof ak / sizeof ak[0]; i++)
		if (!strcasecmp(key, ak[i].k))
			return parse_int(ps, val, 1, 100, (int *)((char *)&cfg->alerts
				+ ak[i].off));
	return CFG_KV_UNKNOWN;
}

/* [upgrade] - UP6n */
int cfg_kv_upgrade(struct pstate *ps, char *key, char *val)
{
	struct pc_config *cfg = ps->cfg;

	if (!strcasecmp(key, "trusted_keys")) {
		/* ed25519 public keys, 64 hex characters each, comma
		 * separated; the release key is built in, not listed */
		char *dup = strdup(val), *tok, *sp = NULL;

		if (!dup)
			return -1;
		cfg->upgrade.n_keys = 0;
		for (tok = strtok_r(dup, ", \t", &sp); tok;
		        tok = strtok_r(NULL, ", \t", &sp)) {
			size_t bl = 0;
			const char *end = NULL;

			if (cfg->upgrade.n_keys >= PC_UPG_MAX_KEYS) {
				PERR(ps, "trusted_keys: at most %d\n",
					PC_UPG_MAX_KEYS);
				free(dup);
				return -1;
			}
			if (strlen(tok) != 64 || sodium_hex2bin(
			        cfg->upgrade.keys[cfg->upgrade.n_keys], 32, tok,
			        64, NULL, &bl, &end) != 0 || bl != 32 || *end) {
				PERR(ps, "trusted_keys: '%s' is not an ed25519 "
					"public key (64 hex characters)\n", tok);
				free(dup);
				return -1;
			}
			cfg->upgrade.n_keys++;
		}
		free(dup);
		return 0;
	}
	if (!strcasecmp(key, "allow_unsigned")) {
		if (!strcasecmp(val, "yes") || !strcasecmp(val, "on"))
			cfg->upgrade.allow_unsigned = 1;
		else if (!strcasecmp(val, "no") || !strcasecmp(val, "off"))
			cfg->upgrade.allow_unsigned = 0;
		else {
			PERR(ps, "allow_unsigned: yes|no\n");
			return -1;
		}
		return 0;
	}
	if (!strcasecmp(key, "stage_mb"))
		return parse_int(ps, val, 16, 4096, &cfg->upgrade.stage_mb);
	if (!strcasecmp(key, "results_dir")) {         /* UPS */
		if (val[0] != '/' || strlen(val) >= sizeof cfg->upgrade.results_dir) {
			PERR(ps, "results_dir must be an absolute path, under 256\n");
			return -1;
		}
		snprintf(cfg->upgrade.results_dir,
			sizeof cfg->upgrade.results_dir, "%s", val);
		return 0;
	}
	return CFG_KV_UNKNOWN;
}
