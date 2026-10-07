/* liballoc.c - S284: libperfd allocations per call, counted by
 * test/allocshim.so preloaded into THIS process (test/liballoctest.sh).
 *
 * The shim adds one to a counter in a shared file per malloc/calloc/
 * realloc; this driver maps the same file and reads the counter around
 * N calls of each call on a warm handle.  One line per case:
 * "<call> <allocations per call> <expected>".  Expected is 0, and 1 for
 * the calls whose result is malloc'd for the caller by contract
 * (perfd_get, perfd_jget, perfd_hget).
 * Usage: liballoc <port> <counter file>
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "perfd.h"

#define N 2000

static volatile uint64_t *cnt;

#define CASE(name, expect, stmt) do {                                   \
	for (i = 0; i < 100; i++) { stmt; }     /* warm-up */          \
	c0 = *cnt;                                                      \
	for (i = 0; i < N; i++) { stmt; }                               \
	printf("%s %.4f %d %d\n", name, (double)(*cnt - c0) / N, expect, \
		bad);                                                   \
	bad = 0;                                                        \
} while (0)

int main(int argc, char **argv)
{
	const char *doc = "{\"rr\":\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
		"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\",\"n\":1,\"gone\":1}";
	const char *secrets[] = { NULL };
	perfd_opts o;
	perfd_t *p;
	uint64_t c0;
	void *v;
	size_t vl;
	char *frag;
	long long n, ttl;
	int fd, i, bad = 0;

	if (argc < 3)
		return 2;
	fd = open(argv[2], O_RDONLY);
	if (fd < 0)
		return 2;
	cnt = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (cnt == MAP_FAILED)
		return 2;
	/* positive control: the shim sees this process's own malloc -
	 * through a volatile pointer, or the compiler drops the pair */
	{
		void *(*volatile m)(size_t) = malloc;
		void *probe;

		c0 = *cnt;
		probe = m(64);
		printf("control %llu\n", (unsigned long long)(*cnt - c0));
		free(probe);
	}
	memset(&o, 0, sizeof o);
	o.secrets = secrets;
	o.spares = PERFD_SPARES_NONE;
	p = perfd_connect("127.0.0.1", atoi(argv[1]), &o);
	if (!p) {
		printf("connect failed\n");
		return 1;
	}
	if (perfd_set(p, "0", "k", "v", 1, 0) < 0 || perfd_jset(p, "0", "j", "$", doc, 0) < 0 ||
	    perfd_hset(p, "0", "h", "f", "v", 1) < 0) {
		printf("seed failed\n");
		return 1;
	}
	CASE("perfd_set", 0, bad += perfd_set(p, "0", "k", "value-100-bytes-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", 100, 0) < 0);
	CASE("perfd_get", 1, if (perfd_get(p, "0", "k", &v, &vl, &ttl) == 1) free(v); else bad++);
	CASE("perfd_exists", 0, bad += perfd_exists(p, "0", "k") < 0);
	CASE("perfd_expire", 0, bad += perfd_expire(p, "0", "k", 600) < 0);
	CASE("perfd_add", 0, bad += perfd_add(p, "0", "c", 1, 0, &n) < 0);
	CASE("perfd_set_cond", 0, bad += perfd_set_cond(p, "0", "nx", "v", 1, 0, 1) < 0);
	CASE("perfd_del", 0, bad += perfd_del(p, "0", "absent") < 0);
	CASE("perfd_ping", 0, bad += perfd_ping(p) < 0);
	CASE("perfd_jset", 0, bad += perfd_jset(p, "0", "j", "$.n", "2", 0) < 0);
	CASE("perfd_jget", 1, if (perfd_jget(p, "0", "j", "$.n", &frag) == 1) free(frag); else bad++);
	CASE("perfd_jincr", 0, bad += perfd_jincr(p, "0", "j", "$.n", 1, &n) < 0);
	CASE("perfd_hset", 0, bad += perfd_hset(p, "0", "h", "f", "v", 1) < 0);
	CASE("perfd_hget", 1, if (perfd_hget(p, "0", "h", "f", &v, &vl) == 1) free(v); else bad++);
	CASE("perfd_hlen", 0, bad += perfd_hlen(p, "0", "h", &n) < 0);
	CASE("perfd_hincrby", 0, bad += perfd_hincrby(p, "0", "h", "i", 1, &n) < 0);
	CASE("perfd_rl_hit", 0, bad += perfd_rl_hit(p, "0", "rl", 1000, 5000, &n) < 0);
	perfd_free(p);
	return 0;
}
