/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov - see lib/LICENSE */
/* abicaller.c - built against the frozen libperfd.so.0 header, run against the libperfd.so the tree
 * builds now: if connect, ping, set, get, free and an error's kind still work, the ABI held. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "perfd-abi0.h"

int main(int argc, char **argv)
{
	perfd_opts o = PERFD_OPTS_INIT;
	perfd_t *p;
	void *v = NULL;
	size_t vl = 0;
	long long ttl = 0;
	char big[5000];
	int fails = 0;

	p = perfd_connect("127.0.0.1", argc > 1 ? atoi(argv[1]) : 6479, &o);
	if (!p) {
		printf("FAIL connect: %s\n", perfd_error(NULL));
		return 1;
	}
	if (perfd_ping(p) != 0) { printf("FAIL ping: %s\n", perfd_error(p)); fails++; }
	if (perfd_set(p, "c", "abi", "v1", 2, 60) != 0) { printf("FAIL set: %s\n", perfd_error(p)); fails++; }
	if (perfd_get(p, "c", "abi", &v, &vl, &ttl) != 1 || vl != 2 || memcmp(v, "v1", 2)) {
		printf("FAIL get\n");
		fails++;
	}
	perfd_free_value(v);
	memset(big, 'k', sizeof big - 1);
	big[sizeof big - 1] = 0;
	if (perfd_set(p, "c", big, "v", 1, 0) == 0 || perfd_error_kind(p) != PERFD_E_ARG) {
		printf("FAIL error kind %d\n", perfd_error_kind(p));
		fails++;
	}
	/* the plain perfd_opts_init() zeroes no byte past this header's struct, and says its size */
	{
		struct { perfd_opts o; unsigned char guard[32]; } g;
		size_t i, over = 0;

		memset(&g, 0xa5, sizeof g);
		perfd_opts_init(&g.o);
		for (i = 0; i < sizeof g.guard; i++)
			over += g.guard[i] != 0xa5;
		if (over || g.o.size != sizeof g.o) {
			printf("FAIL perfd_opts_init zeroed %zu byte(s) past a %zu-byte perfd_opts, size %zu\n",
				over, sizeof g.o, g.o.size);
			fails++;
		}
	}
	printf("abicaller: library %s, header %s, %d failed\n", perfd_version(),
		PERFD_VERSION, fails);
	perfd_free(p);
	return fails ? 1 : 0;
}
