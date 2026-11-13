/* lingershim.c - LD_PRELOAD shim: a stopping daemon lingers past its goodbye: the snapshot's
 * final rename waits PC_LINGER_S once PC_LINGER_MARK exists, each hold logged to PC_LINGER_LOG */
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
