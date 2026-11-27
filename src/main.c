/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* main.c - perfcached entry point: command line, config check and dump, then the daemon. */
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
#include "reload.h"

#include "tls.h"
#include "uptls.h"
#include "version.h"
#include "uring.h"
#include "busypoll.h"
#include "respmap.h"
#include "pcaddr.h"
#include "memguard.h"
#ifndef PC_BUILD_REV
#define PC_BUILD_REV "unknown"
#endif

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s -f <config> [-C | -E | -H <pid>] [-D]\n"
		"       %s -I|-P|-W|-R <dir> | -K <file> [-f <config>] | -V | -h\n"
		"  -f <file>  configuration file (required)\n"
		"  -C         load + validate the config, report, exit\n"
		"  -E         dump the normalized effective config (implies -C)\n"
		"  -D         run in the foreground - the only mode, so -D changes nothing\n"
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
		"             applied, 1 refused (what reloads: GUIDE.md,\n"
		"             \"Reloading the configuration\")\n"
		"  -V         print version and exit\n"
		"  -h         this help\n", argv0, argv0);
}

int main(int argc, char **argv)
{
	struct pc_config cfg;
	const char *cfile = NULL;
	int opt, check = 0, dump = 0, foreground = 0;
	const char *kfile = NULL;
	long hup_pid = 0;                       /* -H */
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
		case 'H': {                /* the ExecReload side */
			char *end;

			hup_pid = strtol(optarg, &end, 10);
			/* 0 would mean "no -H" and start a daemon in the caller's place */
			if (end == optarg || *end || hup_pid < 1) {
				fprintf(stderr, "perfcached -H: '%s' is not a daemon pid - "
					"not reloaded\n", optarg);
				return 1;
			}
			break;
		}
		case 'K':                  /* after -f, wherever it stands */
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
			/* an allocator other than the C library's is named;
			 * the default line is unchanged */
			const char *al = strcmp(PC_ALLOC_NAME, "libc") ?
				", allocator " PC_ALLOC_NAME : "";
			/* a debug or sanitizer build says so beside the
			 * revision; a release line is unchanged */
			const char *fl = strcmp(PC_BUILD_FLAVOR, "release") ?
				", " PC_BUILD_FLAVOR : "";
#ifdef PC_EDITION_STANDALONE
			printf("perfcached %s (%s%s) standalone%s\n", PC_VERSION,
				PC_BUILD_REV, fl, al);
#else
			printf("perfcached %s (%s%s)%s\n", PC_VERSION, PC_BUILD_REV,
				fl, al);
#endif
			printf("tls: %s\n", pc_tls_version());
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

	if (hup_pid)
		return pc_reload_client(cfile, hup_pid);
	/* what the file said, key by key, for a reload to compare with */
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
	{       /* the TLS doors' pair, loaded now - -C checks it, a start needs it */
		int t;

		for (t = 0; t < cfg.n_listen && !cfg.listen[t].tls; t++)
			;
		if (t < cfg.n_listen) {
			char e[400];

			if (pc_tls_load(cfg.tls_cert, cfg.tls_key, cfg.tls_min13, e, sizeof e) != 0) {
				LM_ERR("TLS: %s - not starting\n", e);
				free(km);
				return 1;
			}
			LM_NOTICE("TLS: %s, certificate %s, TLS %s or later\n", pc_tls_version(),
				cfg.tls_cert, cfg.tls_min13 ? "1.3" : "1.2");
		}
		for (t = 0; t < cfg.n_col; t++) {        /* the rediss:// upstreams' files too */
			const struct pc_collection *cc = &cfg.col[t];
			struct up_tls *ut;
			char e[400];

			if (!cc->up_tls)
				continue;
			if (!(ut = uptls_new(cc->up_tls_ca, cc->up_tls_cert, cc->up_tls_key,
			        cc->up_tls_verify, e, sizeof e))) {
				LM_ERR("[collection %s] %s - not starting\n", cc->name, e);
				free(km);
				return 1;
			}
			uptls_free(ut);
		}
	}
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
		if (cfg.io_backend == PC_IO_EPOLL) {
			struct pc_uring_verdict v;

			/* whether the other backend would run here */
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
		{
			const char *why;

			if (pc_ipv6_usable(&why))
				printf("net: IPv6 is usable on this host\n");
			else
				printf("net: IPv6 is not usable on this host - %s: the "
					"listeners and every name resolved are IPv4 only "
					"(no AAAA query goes out)\n", why);
		}
		{
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
		if (cfg.busy_poll_us > 0) {
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
		{
			char mem[512];
			int nofit = pc_memguard_precheck(&cfg, mem, sizeof mem);

			printf("%s\n", mem);
			if (nofit) {
				pc_config_free(&cfg);
				return 1;
			}
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
