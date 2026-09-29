/* syncfailshim.c - S215: an LD_PRELOAD shim that makes the device refuse
 * a sync, by descriptor CLASS, armed at runtime.
 *
 * The daemon has to start on a healthy device - segment provisioning
 * syncs, and refuses to start when that fails - so the fault is armed
 * afterwards through a file: PC_SYNCFAIL_CTL names it, and its content
 * is read on every fdatasync/fsync (these calls cost milliseconds; one
 * small read beside them is nothing):
 *
 *   wal    a WAL segment        (.../wal-NNN.seg)
 *   walwrite  a WAL segment's pwrite fails with ENOSPC - the append
 *          itself is refused, not the sync after it
 *   walslow   a WAL segment's pwrite SUCCEEDS, after PC_SYNCFAIL_SLOW_MS
 *          (default 100) - a device that cannot keep up, so the WAL
 *          pump falls behind and its producer rings overflow on any
 *          runner, sanitizer or not (S229's healtest: a burst that
 *          merely raced the pump dropped nothing under ASan)
 *   ctrl   the WAL's CONTROL    (.../CONTROL)
 *   dir    a directory          (the snapshot's rename)
 *   nodir  open(O_DIRECTORY) fails with EACCES - the directory sync
 *          cannot even be attempted
 *
 * A matching call fails with EIO and does NOT reach the device; anything
 * else passes through.  Every refused call is appended to
 * PC_SYNCFAIL_LOG ("<class> <path>"), so a test can prove the fault was
 * DELIVERED - a green run against a shim that never fired proves
 * nothing.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

/* The real calls are reached as raw syscalls rather than through
 * dlsym(RTLD_NEXT): RTLD_NEXT needs a feature-test macro, and this tree
 * keeps its sources free of them. */
static int real_open(const char *path, int flags, mode_t mode)
{
	return (int)syscall(SYS_openat, AT_FDCWD, path, flags, mode);
}

static void armed(char *out, size_t cap)
{
	const char *ctl = getenv("PC_SYNCFAIL_CTL");
	int fd;
	ssize_t n;

	out[0] = 0;
	if (!ctl)
		return;
	fd = real_open(ctl, O_RDONLY, 0);
	if (fd < 0)
		return;
	n = read(fd, out, cap - 1);
	close(fd);
	if (n < 0)
		n = 0;
	out[n] = 0;
	out[strcspn(out, " \r\n")] = 0;
}

static void note(const char *cls, const char *path)
{
	const char *lg = getenv("PC_SYNCFAIL_LOG");
	char line[600];
	int fd, n;

	if (!lg)
		return;
	fd = real_open(lg, O_WRONLY | O_CREAT | O_APPEND, 0600);
	if (fd < 0)
		return;
	n = snprintf(line, sizeof line, "%s %s\n", cls, path);
	if (n > 0 && write(fd, line, (size_t)n) < 0) { }
	close(fd);
}

/* 1 = this sync is refused */
static int refuse(int fd)
{
	char want[32], lnk[64], path[512];
	const char *base, *cls = NULL;
	struct stat st;
	ssize_t n;

	armed(want, sizeof want);
	if (!want[0])
		return 0;
	snprintf(lnk, sizeof lnk, "/proc/self/fd/%d", fd);
	n = readlink(lnk, path, sizeof path - 1);
	if (n <= 0)
		return 0;
	path[n] = 0;
	base = strrchr(path, '/');
	base = base ? base + 1 : path;
	if (!strncmp(base, "wal-", 4) && strstr(base, ".seg"))
		cls = "wal";
	else if (!strcmp(base, "CONTROL"))
		cls = "ctrl";
	else if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode))
		cls = "dir";
	if (!cls || strcmp(cls, want) != 0)
		return 0;
	note(cls, path);
	errno = EIO;
	return 1;
}

int fdatasync(int fd)
{
	if (refuse(fd))
		return -1;
	return (int)syscall(SYS_fdatasync, fd);
}

int fsync(int fd)
{
	if (refuse(fd))
		return -1;
	return (int)syscall(SYS_fsync, fd);
}

/* the append: pwrite on a WAL segment, armed as "walwrite" */
static int refuse_write(int fd)
{
	char want[32], lnk[64], path[512];
	const char *base;
	ssize_t n;

	armed(want, sizeof want);
	if (strcmp(want, "walwrite") != 0)
		return 0;
	snprintf(lnk, sizeof lnk, "/proc/self/fd/%d", fd);
	n = readlink(lnk, path, sizeof path - 1);
	if (n <= 0)
		return 0;
	path[n] = 0;
	base = strrchr(path, '/');
	base = base ? base + 1 : path;
	if (strncmp(base, "wal-", 4) != 0 || !strstr(base, ".seg"))
		return 0;
	note("walwrite", path);
	errno = ENOSPC;
	return 1;
}

/* "walslow": the append is delayed, not refused */
static void slow_write(int fd)
{
	char want[32], lnk[64], path[512];
	const char *base, *ms = getenv("PC_SYNCFAIL_SLOW_MS");
	ssize_t n;
	int d = ms ? atoi(ms) : 100;

	armed(want, sizeof want);
	if (strcmp(want, "walslow") != 0)
		return;
	snprintf(lnk, sizeof lnk, "/proc/self/fd/%d", fd);
	n = readlink(lnk, path, sizeof path - 1);
	if (n <= 0)
		return;
	path[n] = 0;
	base = strrchr(path, '/');
	base = base ? base + 1 : path;
	if (strncmp(base, "wal-", 4) != 0 || !strstr(base, ".seg"))
		return;
	note("walslow", path);
	usleep((useconds_t)(d > 0 ? d : 100) * 1000);
}

ssize_t pwrite(int fd, const void *buf, size_t n, off_t off)
{
	if (refuse_write(fd))
		return -1;
	slow_write(fd);
	return (ssize_t)syscall(SYS_pwrite64, fd, buf, n, off);
}

/* what a _FILE_OFFSET_BITS=64 build calls */
ssize_t pwrite64(int fd, const void *buf, size_t n, off_t off);
ssize_t pwrite64(int fd, const void *buf, size_t n, off_t off)
{
	if (refuse_write(fd))
		return -1;
	slow_write(fd);
	return (ssize_t)syscall(SYS_pwrite64, fd, buf, n, off);
}

static int nodir(const char *path, int flags)
{
	char want[32];

	if (!(flags & O_DIRECTORY))
		return 0;
	armed(want, sizeof want);
	if (strcmp(want, "nodir") != 0)
		return 0;
	note("nodir", path);
	errno = EACCES;
	return 1;
}

static int open_common(const char *path, int flags, mode_t mode)
{
	if (nodir(path, flags))
		return -1;
	return real_open(path, flags, mode);
}

int open(const char *path, int flags, ...)
{
	mode_t mode = 0;

	if (flags & O_CREAT) {
		va_list ap;

		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, int);
		va_end(ap);
	}
	return open_common(path, flags, mode);
}

/* what a _FILE_OFFSET_BITS=64 build calls; on a 64-bit kernel ABI the
 * two are the same syscall */
int open64(const char *path, int flags, ...);
int open64(const char *path, int flags, ...)
{
	mode_t mode = 0;

	if (flags & O_CREAT) {
		va_list ap;

		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, int);
		va_end(ap);
	}
	return open_common(path, flags, mode);
}
