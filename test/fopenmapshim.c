/* fopenmapshim.c - LD_PRELOAD for test/cleardoortest.sh: fopen() of the one
 * path in PC_FOPEN_FROM opens PC_FOPEN_TO instead, so a test can show
 * perfcli a config at a system path (/etc/perfcached.conf, S339) without
 * writing to the host's /etc.  Every other path opens as asked. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *mapped(const char *path)
{
	const char *from = getenv("PC_FOPEN_FROM"), *to = getenv("PC_FOPEN_TO");

	return from && to && path && !strcmp(path, from) ? to : path;
}

FILE *fopen(const char *path, const char *mode)
{
	static FILE *(*real)(const char *, const char *);

	if (!real)
		real = (FILE *(*)(const char *, const char *))dlsym(RTLD_NEXT, "fopen");
	return real(mapped(path), mode);
}

FILE *fopen64(const char *path, const char *mode)
{
	static FILE *(*real)(const char *, const char *);

	if (!real)
		real = (FILE *(*)(const char *, const char *))dlsym(RTLD_NEXT, "fopen64");
	return real(mapped(path), mode);
}
