/* lingershim.c - S329: an LD_PRELOAD shim that makes a stopping daemon
 * LINGER after its goodbye, as a production one does.
 *
 * On the PROD AU fleet a master stopping for a reboot said goodbye and
 * then took its shutdown snapshot while the system went down - its TCP
 * lane (whose thread is never joined) still answered two seconds later,
 * and the members took that for "UDP blocked, TCP up".  On a test host
 * the snapshot is milliseconds and the process is gone before a member
 * looks.  This holds the snapshot's final rename - the step after the
 * goodbye - for PC_LINGER_S seconds (default 3), but only once the file
 * named by PC_LINGER_MARK exists, so the daemon starts and runs normally
 * until the test arms it right before the stop.
 *
 * Every hold is appended to PC_LINGER_LOG ("held <path>"), so a test can
 * prove the delay was DELIVERED - a green run against a shim that never
 * fired proves nothing.  The real call is a raw syscall (no RTLD_NEXT:
 * this tree keeps its sources free of feature-test macros).
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static int ends_with(const char *s, const char *tail)
{
	size_t n = strlen(s), t = strlen(tail);

	return n >= t && strcmp(s + n - t, tail) == 0;
}

int rename(const char *from, const char *to)
{
	const char *mark = getenv("PC_LINGER_MARK");

	if (mark && to && ends_with(to, ".rdb") &&
	        syscall(SYS_faccessat, AT_FDCWD, mark, F_OK, 0) == 0) {
		const char *s = getenv("PC_LINGER_S"), *log = getenv("PC_LINGER_LOG");
		int sec = s ? atoi(s) : 3;

		if (log) {
			int fd = (int)syscall(SYS_openat, AT_FDCWD, log,
				O_WRONLY | O_CREAT | O_APPEND, 0644);

			if (fd >= 0) {
				char line[600];
				int n = snprintf(line, sizeof line, "held %s\n", to);

				if (n > 0)
					(void)!write(fd, line, (size_t)n);
				close(fd);
			}
		}
		sleep((unsigned int)(sec > 0 ? sec : 3));
	}
	return (int)syscall(SYS_renameat, AT_FDCWD, from, AT_FDCWD, to);
}
