/* listenfdtest.c - the listener sockets (listenfd.c; the 0.7.2 split, wave
 * 1): a unix socket in a directory of its own, TCP on 127.0.0.1 at a port
 * the kernel picked; the log stubbed.
 *
 *   A  the unix socket: made, non-blocking, accepting; a second instance
 *      on the same path refused while the first accepts, and the first
 *      unharmed; a stale file (its listener gone) replaced; a path too
 *      long, a directory that is not there - refused, said
 *   B  TCP: a listener without SO_REUSEPORT already on the port - the
 *      probe sees it, and a worker's socket cannot join it; the port free
 *      - the probe says so, and two workers' sockets share it; with them
 *      there, the probe sees them - a second instance would be caught
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "compat/dprint.h"
#include "config.h"
#include "listenfd.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* ---- the stub: the log, kept ------------------------------------------- */
int compat_log_level = L_NOTICE;
static char logbuf[4096];
static int errs;

void compat_log(int level, const char *tag, const char *fmt, ...)
{
	size_t n = strlen(logbuf);
	va_list ap;

	(void)tag;
	va_start(ap, fmt);
	vsnprintf(logbuf + n, sizeof logbuf - n, fmt, ap);
	va_end(ap);
	if (level == L_ERR)
		errs++;
}

/* ---- the harness ------------------------------------------------------- */
static void quiet(void)
{
	logbuf[0] = 0;
	errs = 0;
}

static int nonblocking(int fd)
{
	int fl = fcntl(fd, F_GETFL);

	return fl >= 0 && (fl & O_NONBLOCK);
}

static int unix_connects(const char *path)
{
	struct sockaddr_un sa;
	int fd = socket(AF_UNIX, SOCK_STREAM, 0), ok;

	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
	ok = fd >= 0 && connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0;
	if (fd >= 0)
		close(fd);
	return ok;
}

/* a plain listener (no SO_REUSEPORT) on 127.0.0.1, the kernel's port */
static int plain_listener(int *port)
{
	struct sockaddr_in sa;
	socklen_t sl = sizeof sa;
	int fd = socket(AF_INET, SOCK_STREAM, 0);

	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(fd, 8) != 0 ||
	    getsockname(fd, (struct sockaddr *)&sa, &sl) != 0)
		return -1;
	*port = ntohs(sa.sin_port);
	return fd;
}

int main(void)
{
	char dir[64], path[128], longp[200];
	struct pc_listener l;
	int fd, fd2, w1, w2, port = 0;

	/* A */
	snprintf(dir, sizeof dir, "/tmp/listenfdtest.XXXXXX");
	if (!mkdtemp(dir))
		return 2;
	snprintf(path, sizeof path, "%s/pc.sock", dir);
	quiet();
	fd = unix_listener_fd(path);
	CHK(fd >= 0 && nonblocking(fd) && unix_connects(path) && !errs,
		"A: the unix socket - made, non-blocking, accepting");
	fd2 = unix_listener_fd(path);
	CHK(fd2 == -1 && errs == 1 && strstr(logbuf, "is accepting connections - another perfcached is running"),
		"A: a second instance on the path - refused while the first accepts, said");
	CHK(unix_connects(path), "A: and the first unharmed - its file still there, still accepting");
	close(fd);
	quiet();
	fd = unix_listener_fd(path);
	CHK(fd >= 0 && unix_connects(path) && !errs, "A: its listener gone, the file stale - replaced");
	close(fd);
	unlink(path);
	memset(longp, 'p', sizeof longp - 1);
	longp[0] = '/';
	longp[sizeof longp - 1] = 0;
	quiet();
	CHK(unix_listener_fd(longp) == -1 && strstr(logbuf, "unix socket path too long"),
		"A: a path longer than sun_path - refused");
	snprintf(path, sizeof path, "%s/no-such-dir/pc.sock", dir);
	quiet();
	CHK(unix_listener_fd(path) == -1 && strstr(logbuf, "cannot bind unix listener"),
		"A: a directory that is not there - refused, said");
	rmdir(dir);

	/* B */
	fd = plain_listener(&port);
	if (fd < 0)
		return 2;
	memset(&l, 0, sizeof l);
	l.type = PC_LISTEN_TCP;
	l.addr = "127.0.0.1";
	l.port = port;
	CHK(tcp_port_taken(&l) == 1, "B: a listener without SO_REUSEPORT on the port - the probe sees it");
	quiet();
	CHK(tcp_listener_fd(&l) == -1 && errs == 1 && strstr(logbuf, "cannot bind listener 127.0.0.1:"),
		"B: a worker's socket cannot join it - refused, said");
	close(fd);
	CHK(tcp_port_taken(&l) == 0, "B: the port free - the probe says so");
	quiet();
	w1 = tcp_listener_fd(&l);
	w2 = tcp_listener_fd(&l);
	CHK(w1 >= 0 && w2 >= 0 && nonblocking(w1) && nonblocking(w2) && !errs,
		"B: two workers' sockets on it - SO_REUSEPORT, both bound, non-blocking");
	CHK(tcp_port_taken(&l) == 1, "B: with them there, the probe sees them - a second instance is caught");
	close(w1);
	close(w2);

	printf("listenfdtest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
