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
#include "upgrade.h"
#include "reload.h"                     /* 12nw */

#include "version.h"
#include "uring.h"
#include "busypoll.h"
#include "respmap.h"                    /* S432: -C reports runtime maps */
#include "pcaddr.h"                     /* 12np: -C reports the IPv6 guard */
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
		"  -K <file>  verify a release artifact against <file>.sig with\n"
		"             the built-in release key (and, with -f, [upgrade]\n"
		"             trusted_keys); print its sha256, exit 0 if good\n"
		"  -H <pid>   reload the running daemon <pid> (systemctl reload):\n"
		"             check the file, signal it, print its answer; exit 0\n"
		"             applied, 1 refused - see DESIGN 12nw for what reloads\n"
		"  -V         print version and exit\n", argv0);
}

int main(int argc, char **argv)
{
	struct pc_config cfg;
	const char *cfile = NULL;
	int opt, check = 0, dump = 0, foreground = 0;
	const char *kfile = NULL;
	long hup_pid = 0;                       /* 12nw: -H */
	struct pc_keymap *km;

	while ((opt = getopt(argc, argv, "f:CEDI:P:W:R:K:H:Vh")) != -1) {
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
		case 'H':                  /* 12nw: the ExecReload side */
			hup_pid = atol(optarg);
			break;
		case 'K':                  /* UP5: after -f, wherever it stands */
			kfile = optarg;
			break;
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
		{
			/* S282: an allocator other than the C library's is named;
			 * the default line is unchanged */
			const char *al = strcmp(PC_ALLOC_NAME, "libc") ?
				", allocator " PC_ALLOC_NAME : "";
			/* S357: a debug or sanitizer build says so beside the
			 * revision; a release line is unchanged */
			const char *fl = strcmp(PC_BUILD_FLAVOR, "release") ?
				", " PC_BUILD_FLAVOR : "";
#ifdef PC_EDITION_STANDALONE
			printf("perfcached %s (%s%s) standalone%s\n", PC_VERSION,
				PC_BUILD_REV, fl, al);     /* S250 */
#else
			printf("perfcached %s (%s%s)%s\n", PC_VERSION, PC_BUILD_REV,
				fl, al);
#endif
			return 0;
		}
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 2;
		}
	}
	if (kfile && !cfile)
		return pc_upg_check_file(kfile, NULL);
	if (!cfile) {
		usage(argv[0]);
		return 2;
	}

	if (hup_pid)                            /* 12nw */
		return pc_reload_client(cfile, hup_pid);
	/* 12nw: what the file said, key by key, for a reload to compare with */
	km = calloc(1, sizeof *km);
	if (!km) {
		LM_ERR("out of memory\n");
		return 1;
	}
	if (pc_config_load_km(cfile, &cfg, km) != 0) {
		LM_ERR("configuration is invalid - not starting\n");
		free(km);
		return 1;
	}
	pc_reload_init(cfile, km, cfg.state_dir);
	if (kfile) {
		opt = pc_upg_check_file(kfile, &cfg.upgrade);
		pc_config_free(&cfg);
		return opt;
	}
	compat_log_level = cfg.log_level;

	if (check) {
		if (dump)
			pc_config_dump(&cfg);
		printf("config OK: %d collection(s), %d listener(s), %d worker(s)\n",
			cfg.n_col, cfg.n_listen, cfg.workers);
		if (cfg.io_backend == PC_IO_EPOLL) {      /* S361 */
			struct pc_uring_verdict v;

			/* S379: whether the other backend would run here */
			pc_uring_check(&v);
			if (v.ok)
				printf("io: epoll (io = epoll) - io_uring is usable on "
					"this host (%s): io = io_uring takes about a third "
					"less CPU a request\n",
					pc_ur_buf_tier_name(v.buf_tier));
			else
				printf("io: epoll (io = epoll) - io_uring is not usable "
					"on this host either: %s\n", v.why);
		} else {
			struct pc_uring_verdict v;

			pc_uring_check(&v);
			if (v.ok)
				printf("io: io_uring is usable on this host (%s)%s\n",
					pc_ur_buf_tier_name(v.buf_tier),
					v.counts_iowait ? " - this kernel counts its "
					"waits as iowait (no opt-out before 6.15): "
					"iowait and PSI io pressure will read high, "
					"not disk I/O" : "");
			else
				printf("io: epoll - io_uring is not usable on this "
					"host: %s\n", v.why);
		}
		{                                         /* 12np */
			const char *why;

			if (pc_ipv6_usable(&why))
				printf("net: IPv6 is usable on this host\n");
			else
				printf("net: IPv6 is not usable on this host - %s: the "
					"listeners and every name resolved are IPv4 only "
					"(no AAAA query goes out)\n", why);
		}
		{                                         /* S432 */
			int nm = pc_respmap_count_file(cfg.state_dir);

			if (nm > 0)
				printf("resp: %d DB index(es) mapped at runtime "
					"(%s/respmap) outrank resp_collections - "
					"perfcli maps lists them\n", nm, cfg.state_dir);
			else if (nm < 0 && cfg.state_dir && *cfg.state_dir)
				printf("resp: %s/respmap cannot be read as a runtime "
					"map (the line above says why) - the daemon will "
					"refuse to start until it is fixed or moved away\n",
					cfg.state_dir);
		}
		if (cfg.busy_poll_us > 0) {               /* S363 */
			char why[200];
			struct pc_uring_verdict v;
			int ur = cfg.io_backend == PC_IO_URING;

			if (ur) {
				pc_uring_check(&v);
				ur = v.ok;
			}
			if (pc_bp_check(ur, (unsigned)cfg.busy_poll_us,
			        cfg.busy_poll_prefer, why, sizeof why) == 0)
				printf("busy poll: %d us, usable on this host (%s)\n",
					cfg.busy_poll_us, ur ? "io_uring NAPI" : "epoll");
			else
				printf("busy poll: %d us asked, but this host refuses "
					"it - the workers would run without: %s\n",
					cfg.busy_poll_us, why);
		}
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
