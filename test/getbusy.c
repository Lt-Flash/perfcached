/* getbusy.c - test tool: perfd_get each key on one bare handle (the binary GET verb) and print
 * "hit", "miss" or "err <kind> <message>" per key, for readbusytest.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "perfd.h"

int main(int argc, char **argv)
{
	perfd_opts o;
	perfd_t *p;
	int i;

	if (argc < 5) {
		fprintf(stderr, "usage: getbusy HOST PORT COL KEY...\n");
		return 2;
	}
	memset(&o, 0, sizeof o);
	o.spares = PERFD_SPARES_NONE;
	p = perfd_connect(argv[1], atoi(argv[2]), &o);
	if (!p) {
		printf("connect %s\n", perfd_error(NULL));
		return 1;
	}
	for (i = 4; i < argc; i++) {
		void *v = NULL;
		size_t vl = 0;
		int r = perfd_get(p, argv[3], argv[i], &v, &vl, NULL);

		if (r > 0)
			printf("%s hit\n", argv[i]);
		else if (r == 0)
			printf("%s miss\n", argv[i]);
		else
			printf("%s err %d %s\n", argv[i], perfd_error_kind(p), perfd_error(p));
		perfd_free_value(v);
	}
	perfd_free(p);
	return 0;
}
