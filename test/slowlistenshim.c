/* slowlistenshim.c - LD_PRELOAD for test/readytest.sh: listen(2) called from
 * any thread but the process's main one sleeps PC_SLOWLISTEN_MS first.  The
 * workers open their own listeners, so this widens the window between the
 * daemon's "perfcached ready" line and the doors actually accepting - a
 * window a loaded CI runner opened by itself (v0.5.6, waltest under ASan). */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

int listen(int fd, int backlog)
{
	static int (*real)(int, int);
	const char *ms = getenv("PC_SLOWLISTEN_MS");

	if (!real)
		real = (int (*)(int, int))dlsym(RTLD_NEXT, "listen");
	if (ms && syscall(SYS_gettid) != getpid()) {
		long v = strtol(ms, NULL, 10);
		struct timespec ts = { v / 1000, (v % 1000) * 1000000L };

		nanosleep(&ts, NULL);
	}
	return real(fd, backlog);
}
