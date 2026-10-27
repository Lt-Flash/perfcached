/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov - see lib/LICENSE */
/*
 * abicaller.c - S370: a program built against the FROZEN libperfd.so.0
 * header (test/abi/perfd-abi0.h), run against whatever libperfd.so the tree
 * builds now.  If the shared library still serves it - connect with the
 * options of that day, ping, set, get, release the value, read an error's
 * kind - the ABI held.  Usage: abicaller <port>
 */
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
	printf("abicaller: library %s, header %s, %d failed\n", perfd_version(),
		PERFD_VERSION, fails);
	perfd_free(p);
	return fails ? 1 : 0;
}
