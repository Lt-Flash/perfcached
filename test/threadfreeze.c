/* threadfreeze.c - S216: stop ONE thread of a daemon, from outside.
 *
 * A wedged thread is the fault, and nothing in the daemon can be asked to
 * produce it: SIGSTOP stops the whole process (heartbeat thread included,
 * which is a different fault with a different answer), and a "please
 * hang" verb would be a hang shipped in the production binary.  ptrace
 * can do it: PTRACE_SEIZE + PTRACE_INTERRUPT stops the one task it names
 * and leaves its siblings running.
 *
 * So this is the daemon's PARENT - which is what makes the trace legal
 * for an unprivileged user under ptrace_scope = 1 - and it takes orders
 * from a file:
 *
 *   threadfreeze <ctl> -- ./perfcached -f conf
 *
 *   echo "1 pc-cluster 9000" > ctl     freeze the thread whose comm is
 *                                      pc-cluster for 9000 ms; the first
 *                                      field is a serial, so the same
 *                                      order can be given twice
 *   <ctl>.ack                          "<serial> frozen <tid>" when the
 *                                      thread has stopped, then
 *                                      "<serial> released" - or
 *                                      "<serial> error <why>"
 *
 * It exits when the daemon does, with its status; SIGTERM/SIGINT kill
 * the daemon first.
 */
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static pid_t child;

static void on_term(int sig)
{
	(void)sig;
	if (child > 0)
		kill(child, SIGKILL);
	_exit(1);
}

static void msleep(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
		;
}

static void ack(const char *ctl, long serial, const char *what, long arg)
{
	char path[600], tmp[620];
	FILE *f;

	snprintf(path, sizeof path, "%s.ack", ctl);
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	f = fopen(tmp, "w");
	if (!f)
		return;
	fprintf(f, "%ld %s %ld\n", serial, what, arg);
	fclose(f);
	rename(tmp, path);
}

/* the tid of @pid's thread named @comm, 0 = none */
static pid_t tid_of(pid_t pid, const char *comm)
{
	char dir[64], path[384], name[64];
	struct dirent *de;
	pid_t tid = 0;
	DIR *d;

	snprintf(dir, sizeof dir, "/proc/%d/task", (int)pid);
	d = opendir(dir);
	if (!d)
		return 0;
	while ((de = readdir(d)) != NULL) {
		FILE *f;

		if (de->d_name[0] < '0' || de->d_name[0] > '9')
			continue;
		snprintf(path, sizeof path, "%s/%s/comm", dir, de->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		name[0] = 0;
		if (fgets(name, sizeof name, f))
			name[strcspn(name, "\n")] = 0;
		fclose(f);
		if (!strcmp(name, comm)) {
			tid = (pid_t)atol(de->d_name);
			break;
		}
	}
	closedir(d);
	return tid;
}

static void freeze(const char *ctl, long serial, const char *comm, long ms)
{
	pid_t tid = tid_of(child, comm);
	int st;

	if (!tid) {
		ack(ctl, serial, "error-no-such-thread", 0);
		return;
	}
	if (ptrace(PTRACE_SEIZE, tid, 0, 0) != 0) {
		ack(ctl, serial, "error-seize", errno);
		return;
	}
	if (ptrace(PTRACE_INTERRUPT, tid, 0, 0) != 0 ||
	        waitpid(tid, &st, __WALL) != tid) {
		ack(ctl, serial, "error-interrupt", errno);
		ptrace(PTRACE_DETACH, tid, 0, 0);
		return;
	}
	ack(ctl, serial, "frozen", (long)tid);
	msleep(ms);
	ptrace(PTRACE_DETACH, tid, 0, 0);
	ack(ctl, serial, "released", (long)tid);
}

int main(int argc, char **argv)
{
	long done = 0;
	const char *ctl;

	if (argc < 4 || strcmp(argv[2], "--") != 0) {
		fprintf(stderr, "usage: %s <ctl-file> -- cmd [args...]\n", argv[0]);
		return 2;
	}
	ctl = argv[1];
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	child = fork();
	if (child < 0)
		return 2;
	if (child == 0) {
		execvp(argv[3], argv + 3);
		perror("exec");
		_exit(127);
	}
	for (;;) {
		char line[128], comm[64];
		long serial = 0, ms = 0;
		int st;
		FILE *f;

		if (waitpid(child, &st, WNOHANG) == child)
			return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
		f = fopen(ctl, "r");
		if (f) {
			line[0] = 0;
			if (fgets(line, sizeof line, f) &&
			        sscanf(line, "%ld %63s %ld", &serial, comm, &ms) == 3 &&
			        serial > done && ms > 0 && ms <= 120000) {
				done = serial;
				fclose(f);
				freeze(ctl, serial, comm, ms);
				continue;
			}
			fclose(f);
		}
		msleep(50);
	}
}
