/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * main.c — perfcached entry point.
 *
 * S5 scope: CLI + config load/validate/dump.  Actual daemon startup
 * (threads, listeners) arrives with S6 - until then a bare run loads the
 * config and exits NONZERO with a clear message, so nothing scripted can
 * mistake this build for a serving daemon (no false green).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "compat/compat.h"
#include "compat/dprint.h"
#include "config.h"
#include "daemon.h"
#include "storage.h"
#include "walprobe.h"
#include "wal.h"
#include "rdb.h"

#include "version.h"
#ifndef PC_BUILD_REV
#define PC_BUILD_REV "unknown"
#endif

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s -f <config> [-C] [-E] [-D] [-V] [-h] | -I <dir>\n"
		"  -f <file>  configuration file (required)\n"
		"  -C         load + validate the config, report, exit\n"
		"  -E         dump the normalized effective config (implies -C)\n"
		"  -D         stay in the foreground (S6+)\n"
		"  -I <dir>   print the storage identity of <dir> and exit\n"
		"  -P <dir>   identity + WAL probe of <dir> (always measured),\n"
		"             policy recommendation, exit\n"
		"  -W <dir>   scan the WAL segments in <dir>, print records, exit\n"
		"  -R <dir>   validate + summarize the RDB snapshot in <dir>, exit\n"
		"  -V         print version and exit\n", argv0);
}

int main(int argc, char **argv)
{
	struct pc_config cfg;
	const char *cfile = NULL;
	int opt, check = 0, dump = 0, foreground = 0;

	while ((opt = getopt(argc, argv, "f:CEDI:P:W:R:Vh")) != -1) {
		switch (opt) {
		case 'f': cfile = optarg; break;
		case 'C': check = 1; break;
		case 'E': check = 1; dump = 1; break;
		case 'D': foreground = 1; break;
		case 'I': {
			struct pc_st_id id;
			char rep[2048];

			if (pc_storage_identity(optarg, &id) != 0)
				return 1;
			pc_storage_format(&id, rep, sizeof rep);
			fputs(rep, stdout);
			return 0;
		}
		case 'R': {
			unsigned long long marker = 0;
			long ncols = 0;
			const char *why;
			long n = pc_rdb_validate(optarg, &marker, &ncols, &why);

			if (n < 0) {
				printf("rdb: INVALID (%s)\n", why);
				return 1;
			}
			printf("rdb: %ld records, %ld collections, wal marker "
				"%llu, crc ok\n", n, ncols, marker);
			return 0;
		}
		case 'W': {
			extern int pc_wal_dump(const char *dir);

			return pc_wal_dump(optarg);
		}
		case 'P': {
			struct pc_st_id id;
			struct pc_wal_probe pr;
			struct pc_wal_policy pol;
			char rep[3072];

			if (pc_storage_identity(optarg, &id) != 0)
				return 1;
			pc_storage_format(&id, rep, sizeof rep);
			fputs(rep, stdout);
			if (pc_wal_probe_run(optarg, 0, &pr) != 0)
				return 1;
			pc_wal_policy_from(&pr, &id, &pol);
			pc_wal_probe_format(&pr, &pol, rep, sizeof rep);
			fputs(rep, stdout);
			return 0;
		}
		case 'V':
			printf("perfcached %s (%s)\n", PC_VERSION, PC_BUILD_REV);
			return 0;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 2;
		}
	}
	if (!cfile) {
		usage(argv[0]);
		return 2;
	}

	if (pc_config_load(cfile, &cfg) != 0) {
		LM_ERR("configuration is invalid - not starting\n");
		return 1;
	}
	compat_log_level = cfg.log_level;

	if (check) {
		if (dump)
			pc_config_dump(&cfg);
		printf("config OK: %d collection(s), %d listener(s), %d worker(s)\n",
			cfg.n_col, cfg.n_listen, cfg.workers);
		pc_config_free(&cfg);
		return 0;
	}

	if (!foreground)
		LM_INFO("foreground is the only mode in this build (-D implied); "
			"run under systemd or a supervisor\n");

	opt = pc_daemon_run(&cfg);
	pc_config_free(&cfg);
	return opt;
}
