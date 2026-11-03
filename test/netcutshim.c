/* netcutshim.c - RV-4a: an LD_PRELOAD shim that cuts a node off from
 * named peers, armed at runtime, with no root and no firewall.
 *
 * The local cluster suites run three daemons on 127.0.13.x without root,
 * so nft is out; the fault run on 245-247 (RV-5, F3) cut a node with nft,
 * and that is what this reproduces per process.  PC_NETCUT_CTL names a
 * file, re-read at most every 50 ms, holding one line of tokens:
 *
 *   cut <ipv4> [<ipv4>...]  the peers this node can no longer reach
 *   mcast                   its multicast sends are cut too (the group
 *                           address is not a peer's)
 *   eperm                   a cut send FAILS with EPERM, as a local
 *                           firewall's OUTPUT drop does - without it the
 *                           send "succeeds" and is lost, as on a network
 *                           cut further away.  The two differ in the
 *                           daemon: a map it could not send it does not
 *                           adopt (S233).
 *
 * An empty or missing file is no cut.  What is cut, both ways:
 *   sendto()             to a cut peer (or the group, with mcast)
 *   recvmsg()/recvfrom() a datagram FROM a cut peer is consumed and
 *                        never returned - the next one is read instead
 *   connect()            to a cut peer: ECONNREFUSED
 *   accept()             from a cut peer: closed, EAGAIN
 * Counts go to PC_NETCUT_LOG ("out N in M conn K"), rewritten at every
 * re-read, so a test can prove the cut was DELIVERED - a partition test
 * whose shim never fired proves nothing.
 *
 * Real calls are raw syscalls, as in syncfailshim.c (no RTLD_NEXT, which
 * needs a feature-test macro this tree does not use). */
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
	int ncut, mcast, eperm;
} cfg;
static long long last_ms = -1000;
static unsigned long n_out, n_in, n_conn;

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
	n = snprintf(line, sizeof line, "out %lu in %lu conn %lu\n",
		n_out, n_in, n_conn);
	if (n > 0 && write(fd, line, (size_t)n) < 0) { }
	close(fd);
}

/* under mx: re-read the control file if 50 ms have passed */
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
	n = read(fd, buf, sizeof buf - 1);
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
		else if (cfg.ncut < MAXCUT && inet_aton(tok, &a))
			cfg.cut[cfg.ncut++] = a.s_addr;
	}
}

/* is @sa cut?  @out: a destination (the group counts with mcast) */
static int is_cut(const struct sockaddr *sa, int out, int *eperm)
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

ssize_t sendto(int fd, const void *buf, size_t len, int flags,
		const struct sockaddr *to, socklen_t tolen)
{
	int eperm = 0;

	if (is_cut(to, 1, &eperm)) {
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
		    !is_cut((const struct sockaddr *)mh->msg_name, 0, NULL))
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

		if (r < 0 || !is_cut((struct sockaddr *)&sa, 0, NULL)) {
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

	/* a UDP connect to the group (the advertise-address probe) is
	 * not a peer connection: only unicast is refused here */
	if (to && to->sa_family == AF_INET &&
	    !IN_MULTICAST(ntohl(in->sin_addr.s_addr)) && is_cut(to, 0, NULL)) {
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
	if (is_cut((struct sockaddr *)&sa, 0, NULL)) {
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
