/* threadfailshim.c - LD_PRELOAD for test/startuptest.sh: the PC_THREADFAIL_NTH-th pthread_create
 * asking for a 1 MB stack (the daemon's own threads) fails with EAGAIN, as at a pids limit. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

int pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg)
{
	static int (*real)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
	static int seen;
	const char *nth = getenv("PC_THREADFAIL_NTH");
	size_t ss = 0;

	if (!real)
		real = (int (*)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *))
			dlsym(RTLD_NEXT, "pthread_create");
	if (nth && a && pthread_attr_getstacksize(a, &ss) == 0 && ss == 1024L * 1024 &&
	        __atomic_add_fetch(&seen, 1, __ATOMIC_RELAXED) == atoi(nth))
		return EAGAIN;
	return real(t, a, fn, arg);
}
