/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* wipetest.c - freeing a config zeroes its secret bytes: a config with five secrets (two client,
 * cluster, enable, resp) must count exactly five wipes on the PC_TESTHOOKS counter (reading freed
 * memory would be UB). */
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
	      "enable = wipe-enable-secret\n"
	      "resp = wipe-resp-password\n"
	      "[listen]\ntcp = 127.0.0.1:19599\nresp = 127.0.0.1:19598\n"
	      "[collection wt]\nbuckets_log2 = 12\n", f);
	fclose(f);

	if (pc_config_load(path, &cfg) != 0) {
		unlink(path);
		fprintf(stderr, "wipetest: fixture config did not parse\n");
		return 2;
	}
	unlink(path);

	pc_config_free(&cfg);

	printf("wipetest: %d secret buffer(s) wiped at free (want 5)\n",
		pc_test_secret_wipes);
	if (pc_test_secret_wipes != 5) {
		printf("wipetest: FAIL - secrets were freed with their "
			"bytes intact\n");
		return 1;
	}
	printf("wipetest: PASS\n");
	return 0;
}
