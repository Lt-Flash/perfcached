/* netcutshim.c - an LD_PRELOAD shim cutting a node off from the peers PC_NETCUT_CTL names, both
 * ways, no root; PC_NETCUT_LOG's counts prove the cut fired.  Raw syscalls: no RTLD_NEXT. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define MAXCUT 8

static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static struct {
	in_addr_t cut[MAXCUT];
	int ncut, mcast, eperm, rst, tcp;
} cfg;
static long long last_ms = -1000;
static unsigned long n_out, n_in, n_conn, n_rst;

static long long mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void write_log(void)
{
	const char *lg = getenv("PC_NETCUT_LOG");
	char line[128];
	int fd, n;

	if (!lg)
		return;
	fd = (int)syscall(SYS_openat, AT_FDCWD, lg,
		O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return;
	n = snprintf(line, sizeof line, "out %lu in %lu conn %lu rst %lu\n",
		n_out, n_in, n_conn, n_rst);
	if (n > 0 && syscall(SYS_write, fd, line, (size_t)n) < 0) { }
	close(fd);
}

/* under mx: re-read the control file if 50 ms have passed.  Tokens: cut <ipv4>..., mcast, eperm
 * (a cut send fails, as a local drop), rst (cut TCP connections reset), tcp (TCP alone) */
static void refresh(void)
{
	const char *ctl = getenv("PC_NETCUT_CTL");
	char buf[512], *tok, *save = NULL;
	long long now = mono_ms();
	ssize_t n;
	int fd;

	if (now - last_ms < 50)
		return;
	last_ms = now;
	write_log();
	memset(&cfg, 0, sizeof cfg);
	if (!ctl)
		return;
	fd = (int)syscall(SYS_openat, AT_FDCWD, ctl, O_RDONLY, 0);
	if (fd < 0)
		return;
	n = syscall(SYS_read, fd, buf, sizeof buf - 1);   /* read() is ours */
	close(fd);
	if (n <= 0)
		return;
	buf[n] = 0;
	for (tok = strtok_r(buf, " \t\r\n", &save); tok;
	     tok = strtok_r(NULL, " \t\r\n", &save)) {
		struct in_addr a;

		if (!strcmp(tok, "cut"))
			continue;
		if (!strcmp(tok, "mcast"))
			cfg.mcast = 1;
		else if (!strcmp(tok, "eperm"))
			cfg.eperm = 1;
		else if (!strcmp(tok, "rst"))
			cfg.rst = 1;
		else if (!strcmp(tok, "tcp"))
			cfg.tcp = 1;
		else if (cfg.ncut < MAXCUT && inet_aton(tok, &a))
			cfg.cut[cfg.ncut++] = a.s_addr;
	}
}

/* is @sa cut?  @out: a destination (the group counts with mcast); @dgram: a datagram's, which
 * passes under tcp */
static int is_cut(const struct sockaddr *sa, int out, int *eperm, int dgram)
{
	const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
	int i, hit = 0;

	if (!sa || sa->sa_family != AF_INET)
		return 0;
	pthread_mutex_lock(&mx);
	refresh();
	for (i = 0; i < cfg.ncut; i++)
		if (cfg.cut[i] == in->sin_addr.s_addr)
			hit = 1;
	if (out && cfg.mcast && IN_MULTICAST(ntohl(in->sin_addr.s_addr)))
		hit = 1;
	if (dgram && cfg.tcp)
		hit = 0;
	if (eperm)
		*eperm = cfg.eperm;
	pthread_mutex_unlock(&mx);
	return hit;
}

static void count(unsigned long *c)
{
	pthread_mutex_lock(&mx);
	(*c)++;
	pthread_mutex_unlock(&mx);
}

/* with rst: is @fd a TCP connection to a cut peer?  1 = reset (errno set) */
static int fd_cut(int fd)
{
	struct sockaddr_in sa;
	socklen_t sl = sizeof sa, tl = sizeof(int);
	int on, type = 0, e = errno;

	pthread_mutex_lock(&mx);
	refresh();
	on = cfg.rst;
	pthread_mutex_unlock(&mx);
	if (!on || syscall(SYS_getpeername, fd, &sa, &sl) != 0 ||
	    syscall(SYS_getsockopt, fd, SOL_SOCKET, SO_TYPE, &type, &tl) != 0 ||
	    type != SOCK_STREAM || !is_cut((struct sockaddr *)&sa, 0, NULL, 0)) {
		errno = e;                     /* a file's read sees no ENOTSOCK */
		return 0;
	}
	count(&n_rst);
	errno = ECONNRESET;
	return 1;
}

ssize_t send(int fd, const void *buf, size_t len, int flags)
{
	return fd_cut(fd) ? -1 : syscall(SYS_sendto, fd, buf, len, flags, NULL, 0);
}

ssize_t recv(int fd, void *buf, size_t len, int flags)
{
	return fd_cut(fd) ? -1 : syscall(SYS_recvfrom, fd, buf, len, flags, NULL, NULL);
}

ssize_t sendmsg(int fd, const struct msghdr *mh, int flags)
{
	return fd_cut(fd) ? -1 : syscall(SYS_sendmsg, fd, mh, flags);
}

ssize_t read(int fd, void *buf, size_t n)
{
	return fd_cut(fd) ? -1 : syscall(SYS_read, fd, buf, n);
}

ssize_t write(int fd, const void *buf, size_t n)
{
	return fd_cut(fd) ? -1 : syscall(SYS_write, fd, buf, n);
}

ssize_t sendto(int fd, const void *buf, size_t len, int flags,
		const struct sockaddr *to, socklen_t tolen)
{
	int eperm = 0;

	if (is_cut(to, 1, &eperm, 1)) {
		count(&n_out);
		if (eperm) {
			errno = EPERM;
			return -1;
		}
		return (ssize_t)len;           /* sent, and lost on the way */
	}
	return syscall(SYS_sendto, fd, buf, len, flags, to, tolen);
}

ssize_t recvmsg(int fd, struct msghdr *mh, int flags)
{
	for (;;) {
		ssize_t r = syscall(SYS_recvmsg, fd, mh, flags);

		if (r < 0 || !mh->msg_name ||
		    !is_cut((const struct sockaddr *)mh->msg_name, 0, NULL, 1))
			return r;
		count(&n_in);
		mh->msg_namelen = sizeof(struct sockaddr_in);
	}
}

ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
		struct sockaddr *from, socklen_t *fromlen)
{
	for (;;) {
		struct sockaddr_in sa;
		socklen_t sl = sizeof sa;
		ssize_t r = syscall(SYS_recvfrom, fd, buf, len, flags, &sa, &sl);

		if (r < 0 || !is_cut((struct sockaddr *)&sa, 0, NULL, 1)) {
			if (r >= 0 && from && fromlen) {
				if (*fromlen > sl)
					*fromlen = sl;
				memcpy(from, &sa, *fromlen);
			}
			return r;
		}
		count(&n_in);
	}
}

int connect(int fd, const struct sockaddr *to, socklen_t tolen)
{
	struct sockaddr_in *in = (struct sockaddr_in *)to;
	socklen_t tl = sizeof(int);
	int type = 0;

	/* a UDP connect to the group (the advertise-address probe) is
	 * not a peer connection: only unicast is refused here */
	if (to && to->sa_family == AF_INET &&
	    !IN_MULTICAST(ntohl(in->sin_addr.s_addr)) &&
	    syscall(SYS_getsockopt, fd, SOL_SOCKET, SO_TYPE, &type, &tl) == 0 &&
	    is_cut(to, 0, NULL, type == SOCK_DGRAM)) {
		count(&n_conn);
		errno = ECONNREFUSED;
		return -1;
	}
	return (int)syscall(SYS_connect, fd, to, tolen);
}

int accept(int fd, struct sockaddr *addr, socklen_t *alen)
{
	struct sockaddr_in sa;
	socklen_t sl = sizeof sa;
	int c = (int)syscall(SYS_accept4, fd, &sa, &sl, 0);

	if (c < 0)
		return c;
	if (is_cut((struct sockaddr *)&sa, 0, NULL, 0)) {
		count(&n_conn);
		close(c);
		errno = EAGAIN;
		return -1;
	}
	if (addr && alen) {
		if (*alen > sl)
			*alen = sl;
		memcpy(addr, &sa, *alen);
	}
	return c;
}
