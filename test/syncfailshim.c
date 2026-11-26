/* syncfailshim.c - LD_PRELOAD shim: the classes the file PC_SYNCFAIL_CTL names, read at each sync,
 * make those syncs, WAL appends or term stores fail or stall; each refusal goes to PC_SYNCFAIL_LOG. */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

/* the real calls are raw syscalls: dlsym(RTLD_NEXT) needs a feature-test macro, and this tree
 * keeps its sources free of them */
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
	out[strcspn(out, "\r\n")] = 0;
}

/* @cls is one of the classes armed */
static int has(const char *want, const char *cls)
{
	size_t n = strlen(cls);
	const char *p = want;

	for (; (p = strstr(p, cls)); p += n)
		if ((p == want || p[-1] == ' ') && (p[n] == 0 || p[n] == ' '))
			return 1;
	return 0;
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
	else if (strlen(base) > 5 && !strcmp(base + strlen(base) - 5, ".part"))
		cls = "part";                  /* an upgrade upload's staging file */
	if (!cls || !has(want, cls))
		return 0;
	note(cls, path);
	errno = EIO;
	return 1;
}

/* "dirslow": a directory's sync is delayed, not refused */
static void slow_dir(int fd)
{
	char want[32], lnk[64], path[512];
	const char *ms = getenv("PC_SYNCFAIL_DIR_MS");
	struct stat st;
	ssize_t n;
	int d = ms ? atoi(ms) : 5000;

	armed(want, sizeof want);
	if (!has(want, "dirslow") || fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode))
		return;
	snprintf(lnk, sizeof lnk, "/proc/self/fd/%d", fd);
	n = readlink(lnk, path, sizeof path - 1);
	path[n > 0 ? n : 0] = 0;
	note("dirslow", path);
	usleep((useconds_t)(d > 0 ? d : 5000) * 1000);
}

/* "stateslow": the sync of a state file named PC_SYNCFAIL_STATE_NAME* (collections.tmp by default;
 * several prefixes comma-separated) is delayed, and the note names the thread that waited */
static void slow_state(int fd)
{
	char want[32], lnk[64], path[512], tn[17] = "", what[560];
	const char *base, *ms = getenv("PC_SYNCFAIL_STATE_MS"), *nm = getenv("PC_SYNCFAIL_STATE_NAME");
	ssize_t n;
	int d = ms ? atoi(ms) : 2000;

	armed(want, sizeof want);
	if (!has(want, "stateslow"))
		return;
	snprintf(lnk, sizeof lnk, "/proc/self/fd/%d", fd);
	n = readlink(lnk, path, sizeof path - 1);
	if (n <= 0)
		return;
	path[n] = 0;
	base = strrchr(path, '/');
	base = base ? base + 1 : path;
	if (!nm || !*nm)
		nm = "collections.tmp";
	for (;;) {
		size_t l = strcspn(nm, ",");

		if (l && !strncmp(base, nm, l))
			break;
		if (!nm[l])
			return;
		nm += l + 1;
	}
	prctl(PR_GET_NAME, (unsigned long)tn, 0, 0, 0);
	snprintf(what, sizeof what, "%s %s", tn, path);
	note("stateslow", what);
	usleep((useconds_t)(d > 0 ? d : 2000) * 1000);
}

int fdatasync(int fd)
{
	if (refuse(fd))
		return -1;
	slow_dir(fd);
	slow_state(fd);
	return (int)syscall(SYS_fdatasync, fd);
}

int fsync(int fd)
{
	if (refuse(fd))
		return -1;
	slow_dir(fd);
	slow_state(fd);
	return (int)syscall(SYS_fsync, fd);
}

/* the append: pwrite on a WAL segment, armed as "walwrite" */
static int refuse_write(int fd)
{
	char want[32], lnk[64], path[512];
	const char *base;
	ssize_t n;

	armed(want, sizeof want);
	if (!has(want, "walwrite"))
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
	if (!has(want, "walslow"))
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

/* the WAL pump's batches; the offset goes as the kernel's low/high pair, split as glibc
 * does, so a 32-bit build passes it right */
static ssize_t real_pwritev(int fd, const struct iovec *iov, int cnt,
		off_t off)
{
	unsigned long long o = (unsigned long long)off;

	return (ssize_t)syscall(SYS_pwritev, fd, iov, cnt, (long)o,
		(long)((o >> 31) >> 1));
}

ssize_t pwritev(int fd, const struct iovec *iov, int cnt, off_t off)
{
	if (refuse_write(fd))
		return -1;
	slow_write(fd);
	return real_pwritev(fd, iov, cnt, off);
}

ssize_t pwritev64(int fd, const struct iovec *iov, int cnt, off_t off);
ssize_t pwritev64(int fd, const struct iovec *iov, int cnt, off_t off)
{
	if (refuse_write(fd))
		return -1;
	slow_write(fd);
	return real_pwritev(fd, iov, cnt, off);
}

static int nodir(const char *path, int flags)
{
	char want[32];

	if (!(flags & O_DIRECTORY))
		return 0;
	armed(want, sizeof want);
	if (!has(want, "nodir"))
		return 0;
	note("nodir", path);
	errno = EACCES;
	return 1;
}

/* "term": the term file's temp copy cannot be created, so the term store fails */
static int noterm(const char *path, int flags)
{
	char want[32];
	const char *base;

	if (!(flags & O_CREAT))
		return 0;
	base = strrchr(path, '/');
	base = base ? base + 1 : path;
	if (strncmp(base, "node-term.tmp", 13) != 0)
		return 0;
	armed(want, sizeof want);
	if (!has(want, "term"))
		return 0;
	note("term", path);
	errno = EIO;
	return 1;
}

static int open_common(const char *path, int flags, mode_t mode)
{
	if (nodir(path, flags) || noterm(path, flags))
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
