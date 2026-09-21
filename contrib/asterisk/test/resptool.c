/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * resptool - drive func_perfd's RESP client (resp.c) from a shell test,
 * without Asterisk.
 *
 *   resptool -s host:port [-s ...] [-p password] [-u user] [-C collection]
 *            [-c connect_ms] [-t io_ms] [-m max_bulk] [-n repeat] [-w wait_ms]
 *            [-o] [CMD ARG...]
 *
 * -o opens the session first and prints OPEN <rc> <text>.  Each command
 * reply prints one line: STATUS <text> | ERROR <text> | INTEGER <n> |
 * BULK <len> <text> | NIL | OVERSIZE <len> | FAIL <why>.  The last line is
 * "END reconnects <n> server <where>".
 */
#include "../resp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int parse_server(const char *v, struct resp_server *sv)
{
	const char *colon;

	memset(sv, 0, sizeof *sv);
	if (!strncmp(v, "unix:", 5)) {
		sv->path = strdup(v + 5);
		return sv->path ? 0 : -1;
	}
	colon = strrchr(v, ':');
	if (!colon)
		return -1;
	sv->host = strndup(v, (size_t)(colon - v));
	sv->port = atoi(colon + 1);
	return sv->host && sv->port > 0 ? 0 : -1;
}

static void sleep_ms(int ms)
{
	struct timespec ts;

	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}

int main(int argc, char **argv)
{
	struct resp_server servers[8];
	struct resp_sess_conf cf;
	struct resp_sess *s;
	struct resp_reply r;
	const char *collection = NULL;
	const char *cargv[16];
	size_t clens[16];
	char err[512];
	int nserv = 0, repeat = 1, wait_ms = 0, open_first = 0, i, k, cargc;

	memset(&cf, 0, sizeof cf);
	cf.connect_timeout_ms = 1000;
	cf.io_timeout_ms = 300;
	cf.max_bulk = 65536;
	for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		const char *opt = argv[i];

		if (!strcmp(opt, "-o")) {
			open_first = 1;
			continue;
		}
		if (i + 1 >= argc) {
			fprintf(stderr, "resptool: %s needs a value\n", opt);
			return 2;
		}
		if (!strcmp(opt, "-s")) {
			if (nserv == 8 || parse_server(argv[++i], &servers[nserv])) {
				fprintf(stderr, "resptool: bad server %s\n", argv[i]);
				return 2;
			}
			nserv++;
		} else if (!strcmp(opt, "-p")) {
			cf.password = argv[++i];
		} else if (!strcmp(opt, "-u")) {
			cf.username = argv[++i];
		} else if (!strcmp(opt, "-C")) {
			collection = argv[++i];
		} else if (!strcmp(opt, "-c")) {
			cf.connect_timeout_ms = atoi(argv[++i]);
		} else if (!strcmp(opt, "-t")) {
			cf.io_timeout_ms = atoi(argv[++i]);
		} else if (!strcmp(opt, "-m")) {
			cf.max_bulk = (size_t)atol(argv[++i]);
		} else if (!strcmp(opt, "-n")) {
			repeat = atoi(argv[++i]);
		} else if (!strcmp(opt, "-w")) {
			wait_ms = atoi(argv[++i]);
		} else {
			fprintf(stderr, "resptool: unknown option %s\n", opt);
			return 2;
		}
	}
	cf.servers = servers;
	cf.nservers = nserv;
	cargc = argc - i;
	if (cargc > 16) {
		fprintf(stderr, "resptool: too many command words\n");
		return 2;
	}
	for (k = 0; k < cargc; k++) {
		cargv[k] = argv[i + k];
		clens[k] = strlen(argv[i + k]);
	}

	s = resp_sess_new(&cf);
	if (!s)
		return 1;
	if (open_first) {
		int rc = resp_sess_open(s, collection, err, sizeof err);

		printf("OPEN %d %s\n", rc, rc ? err : "");
	}
	for (k = 0; cargc && k < repeat; k++) {
		if (k && wait_ms)
			sleep_ms(wait_ms);
		if (resp_sess_call(s, collection, cargc, cargv, clens, &r,
		        err, sizeof err)) {
			printf("FAIL %s\n", err);
			fflush(stdout);
			continue;
		}
		switch (r.type) {
		case RESP_T_STATUS:
			printf("STATUS %s\n", r.str);
			break;
		case RESP_T_ERROR:
			printf("ERROR %s\n", r.str);
			break;
		case RESP_T_INTEGER:
			printf("INTEGER %lld\n", r.integer);
			break;
		case RESP_T_NIL:
			printf("NIL\n");
			break;
		default:
			if (r.oversize)
				printf("OVERSIZE %zu\n", r.len);
			else
				printf("BULK %zu %s\n", r.len, r.str);
			break;
		}
		fflush(stdout);
		resp_reply_free(&r);
	}
	printf("END reconnects %llu server %s\n", resp_sess_reconnects(s),
		resp_sess_server(s));
	resp_sess_free(s);
	return 0;
}
