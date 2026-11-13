/* asyncstatetest.c - an async handle that learned the fleet before a member came routes to it
 * once it is ready, never before, by its pushes or its refresh. */
#include <dirent.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "perfd.h"

#define NKEYS 3000

static int gets_out;

static long long wall_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_REALTIME, &t);
	return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* of NKEYS keys, how many the handle routes to the member at @addr */
static int owned(perfd_t *p, const char *addr)
{
	char k[32], a[64];
	int i, ix, n = 0;

	for (i = 0; i < NKEYS; i++) {
		snprintf(k, sizeof k, "k%d", i);
		ix = perfd_owner_of(p, "c", k);
		if (ix >= 0 && perfd_member_info(p, ix, a, sizeof a, NULL, NULL, NULL) == 0 &&
		    !strcmp(a, addr))
			n++;
	}
	return n;
}

/* the async contract: the caller drives one fd, so the handle holds that one socket and no other */
static int sockets(void)
{
	DIR *d = opendir("/proc/self/fd");
	struct dirent *e;
	char path[300], l[64];
	ssize_t r;
	int n = 0;

	while (d && (e = readdir(d))) {
		snprintf(path, sizeof path, "/proc/self/fd/%s", e->d_name);
		r = readlink(path, l, sizeof l - 1);
		if (r > 0) {
			l[r] = 0;
			n += !strncmp(l, "socket:", 7);
		}
	}
	if (d)
		closedir(d);
	return n;
}

static void got(char *res, size_t len, const char *err, void *ctx)
{
	(void)len; (void)err; (void)ctx;
	perfd_free_value(res);
	gets_out--;
}

static int turn(perfd_t *p, int ms)
{
	struct pollfd pf = { perfd_fd(p), 0, 0 };
	int ev = perfd_events(p);

	pf.events = (short)((ev & PERFD_EV_READ ? POLLIN : 0) | (ev & PERFD_EV_WRITE ? POLLOUT : 0));
	if (poll(&pf, 1, ms) <= 0)
		return 0;
	if (pf.revents & (POLLIN | POLLERR | POLLHUP) && perfd_read_ready(p) < 0)
		return -1;
	if (pf.revents & POLLOUT && perfd_write_ready(p) < 0)
		return -1;
	return 0;
}

int main(int argc, char **argv)
{
	const char *sec[2] = { NULL, NULL };
	long long t0, end, next_get = 0, next_count = 0;
	perfd_opts o;
	perfd_t *p;
	int n, last;

	if (argc != 8) {
		fprintf(stderr, "usage: asyncstatetest host port secret addr push refresh_ms run_ms\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	sec[0] = argv[3];
	perfd_opts_init(&o);
	o.secrets = sec;
	o.route_keys = 1;
	o.ignore_member_push = !atoi(argv[5]);
	o.refresh_ms = atoi(argv[6]);
	if (!(p = perfd_connect_async(argv[1], atoi(argv[2]), &o))) {
		printf("FAIL no handle: %s\n", perfd_error(NULL));
		return 1;
	}
	t0 = wall_ms();
	while (!perfd_routing(p) && wall_ms() - t0 < 5000)
		if (turn(p, 50) < 0 || perfd_state(p) == PERFD_ST_FAILED) {
			printf("FAIL the handle failed: %s\n", perfd_error(p));
			return 1;
		}
	if (!perfd_routing(p)) {
		printf("FAIL the handle never learned the fleet\n");
		return 1;
	}
	last = owned(p, argv[4]);
	printf("learned t=%lld members=%d n=%d\n", wall_ms(), perfd_member_count(p), last);
	end = wall_ms() + atoll(argv[7]);
	while (wall_ms() < end) {
		long long now = wall_ms();

		/* traffic, as a live handle has: its replies are the read readiness */
		if (now >= next_get && gets_out < 4 && perfd_state(p) == PERFD_ST_READY &&
		    perfd_submit_kv(p, PERFD_V_GET, "c", "x", NULL, 0, 0, 0, got, NULL) == 0) {
			gets_out++;
			next_get = now + 100;
		}
		if (turn(p, 20) < 0) {
			printf("FAIL the handle failed: %s\n", perfd_error(p));
			return 1;
		}
		if (now >= next_count) {
			next_count = now + 50;
			if ((n = owned(p, argv[4])) != last)
				printf("t=%lld n=%d members=%d\n", wall_ms(), n, perfd_member_count(p));
			last = n;
		}
	}
	printf("end n=%d members=%d sockets=%d\n", owned(p, argv[4]), perfd_member_count(p), sockets());
	perfd_free(p);
	return 0;
}
