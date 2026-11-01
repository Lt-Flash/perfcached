/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * mgetextest.c - G8 (DESIGN 12nh): libperfd's two mgets against one node
 * of a fleet, driven by test/mgetfantest.sh.  The keys are <prefix>NNN for
 * NNN below <n> (written as "v-<key>"), then <m> keys zzNNN nobody holds.
 * Prints one line:
 *   mget rc=<rc> kind=<perfd_error_kind> found=<n> bad=<n> |
 *   ex rc=<rc> found=<n> absent=<n> unanswered=<n> bad=<n>
 * bad: a value found that is not the one written, or (ex) a status out of
 * range, a value with an absent or unanswered status, or one missing with
 * found.  Usage: mgetextest <port> <col> <prefix> <n> <m>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../lib/perfd.h"

int main(int argc, char **argv)
{
	perfd_opts o = PERFD_OPTS_INIT;
	perfd_t *p;
	int n, m, t, i, rc, rce, kind = 0, found = 0, bad = 0;
	int efound = 0, eabsent = 0, eunans = 0, ebad = 0;
	char **keys, want[64];
	void **vals;
	size_t *lens;
	int *st;

	if (argc != 6) {
		fprintf(stderr, "usage: mgetextest <port> <col> <prefix> <n> <m>\n");
		return 2;
	}
	n = atoi(argv[4]);
	m = atoi(argv[5]);
	t = n + m;
	keys = calloc((size_t)t, sizeof *keys);
	vals = calloc((size_t)t, sizeof *vals);
	lens = calloc((size_t)t, sizeof *lens);
	st = calloc((size_t)t, sizeof *st);
	if (!keys || !vals || !lens || !st)
		return 2;
	for (i = 0; i < t; i++) {
		keys[i] = malloc(64);
		if (!keys[i])
			return 2;
		if (i < n)
			snprintf(keys[i], 64, "%s%03d", argv[3], i);
		else
			snprintf(keys[i], 64, "zz%03d", i - n);
	}
	p = perfd_connect("127.0.0.1", atoi(argv[1]), &o);
	if (!p) {
		printf("connect failed: %s\n", perfd_error(NULL));
		return 1;
	}

	rc = perfd_mget(p, argv[2], (const char *const *)keys, t, vals, lens);
	if (rc != 0)
		kind = perfd_error_kind(p);
	for (i = 0; i < t; i++) {
		if (!vals[i])
			continue;
		snprintf(want, sizeof want, "v-%s", keys[i]);
		if (i < n && lens[i] == strlen(want) &&
		        !memcmp(vals[i], want, lens[i]))
			found++;
		else
			bad++;
		free(vals[i]);
		vals[i] = NULL;
	}

	rce = perfd_mget_ex(p, argv[2], (const char *const *)keys, t, vals,
		lens, st);
	for (i = 0; rce == 0 && i < t; i++) {
		snprintf(want, sizeof want, "v-%s", keys[i]);
		switch (st[i]) {
		case PERFD_MGET_FOUND:
			if (vals[i] && i < n && lens[i] == strlen(want) &&
			        !memcmp(vals[i], want, lens[i]))
				efound++;
			else
				ebad++;
			break;
		case PERFD_MGET_ABSENT:
			eabsent++;
			ebad += vals[i] != NULL;
			break;
		case PERFD_MGET_UNANSWERED:
			eunans++;
			ebad += vals[i] != NULL;
			break;
		default:
			ebad++;
		}
		free(vals[i]);
	}
	printf("mget rc=%d kind=%d found=%d bad=%d | ex rc=%d found=%d "
		"absent=%d unanswered=%d bad=%d\n", rc, kind, found, bad, rce,
		efound, eabsent, eunans, ebad);
	perfd_free(p);
	for (i = 0; i < t; i++)
		free(keys[i]);
	free(keys);
	free(vals);
	free(lens);
	free(st);
	return 0;
}
