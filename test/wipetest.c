/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * wipetest.c — S59(a): freeing a config must WIPE the secret bytes,
 * not just free them.  Reading freed memory is UB, so the proof is a
 * test-only counter (PC_TESTHOOKS) that pc_config_wipe_secrets bumps
 * for every buffer it zeroes: parse a config carrying three secrets,
 * free it, expect exactly three wipes.  Fail-first receipt: before
 * the wipe was wired into pc_config_free this printed 0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/config.h"

extern int pc_test_secret_wipes;

int main(void)
{
	char path[] = "/var/tmp/pcwipe.XXXXXX";
	int fd = mkstemp(path);
	FILE *f;
	struct pc_config cfg;

	if (fd < 0)
		return 2;
	f = fdopen(fd, "w");
	if (!f)
		return 2;
	fputs("[daemon]\nworkers = 1\n"
	      "[memory]\narena_mb = 64\n"
	      "[secrets]\n"
	      "client = wipe-client-one\n"
	      "client = wipe-client-two\n"
	      "cluster = wipe-cluster-secret\n"
	      "[listen]\ntcp = 127.0.0.1:19599\n"
	      "[collection wt]\nbuckets_log2 = 12\n", f);
	fclose(f);

	if (pc_config_load(path, &cfg) != 0) {
		unlink(path);
		fprintf(stderr, "wipetest: fixture config did not parse\n");
		return 2;
	}
	unlink(path);

	pc_config_free(&cfg);

	printf("wipetest: %d secret buffer(s) wiped at free (want 3)\n",
		pc_test_secret_wipes);
	if (pc_test_secret_wipes != 3) {
		printf("wipetest: FAIL - secrets were freed with their "
			"bytes intact\n");
		return 1;
	}
	printf("wipetest: PASS\n");
	return 0;
}
