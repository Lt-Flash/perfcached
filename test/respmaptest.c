/* respmaptest.c - S432 (0.7.1): the RESP door's DB-index map at runtime
 * (respmap.c), without a daemon.
 *
 *   A  the config alone: `0, 1:prod-redis, 2:th` (PROD's list) resolves
 *      0 -> "0", 1 -> prod-redis, 2 -> th, each from the config; 3 is
 *      refused (""); the effective list IS the config's
 *   B  map 0 prod-redis: the override replaces the bare 0, is listed FIRST
 *      (INFO keyspace reports prod-redis under 0, not 1), resolves as
 *      runtime; `was` names what the index reached before
 *   C  generations: an older change is ignored, an equal one settles on the
 *      larger name on every member, a newer one wins
 *   D  unmap: the index answers as the config says again, the tombstone
 *      is kept (the fleet sync sends it) and outranks an older map
 *   E  no list in the config (an open door): an override keeps the door
 *      open ("*" first) - every other index still reaches the collection
 *      of its own name, as OPEN
 *   F  persistence: the overrides and the tombstones survive a re-init
 *      from the state dir, and the Lamport clock resumes above them;
 *      count_file sees them without loading; a file that is not ours is
 *      refused, by init and by count_file
 *   G  refused: an index out of range, a name the list grammar cannot
 *      hold (',', ':', '*', a space, empty, too long) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/respmap.h"
#include "../src/compat/compat.h"          /* pc_lamport_now */

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static const char *eff(void)
{
	const char *e = pc_respmap_effective();

	return e ? e : "(null)";
}

static int res(int idx, const char *want, int src)
{
	char out[80];
	int s = pc_respmap_resolve(idx, out, sizeof out);

	return s == src && !strcmp(out, want);
}

struct seen { int n, idx[8], set[8]; };

static void each_cb(int index, const char *col, uint64_t gen, int set, void *ctx)
{
	struct seen *s = ctx;

	(void)col;
	(void)gen;
	if (s->n < 8) {
		s->idx[s->n] = index;
		s->set[s->n] = set;
		s->n++;
	}
}

int main(void)
{
	char dir[] = "/var/tmp/pcrespmap.XXXXXX", path[128], was[80];
	FILE *f;
	struct seen sn;

	if (!mkdtemp(dir)) {
		perror("mkdtemp");
		return 1;
	}

	/* A */
	CHK(pc_respmap_ready() == 0, "A: not ready before init");
	CHK(pc_respmap_init("0, 1:prod-redis, 2:th", dir) == 0 && pc_respmap_ready(),
		"A: init with PROD's list and an empty state dir");
	CHK(!strcmp(eff(), "0, 1:prod-redis, 2:th"),
		"A: nothing overridden - the effective list is the config's ('%s')", eff());
	CHK(res(0, "0", PC_RESPMAP_CONFIG) && res(1, "prod-redis", PC_RESPMAP_CONFIG) &&
		res(2, "th", PC_RESPMAP_CONFIG) && res(3, "", PC_RESPMAP_OPEN),
		"A: 0 -> '0', 1 -> prod-redis, 2 -> th from the config; 3 refused");
	CHK(pc_respmap_overrides() == 0 && !strcmp(pc_respmap_config(), "0, 1:prod-redis, 2:th"),
		"A: no overrides; the config's list kept as given");

	/* B */
	CHK(pc_respmap_apply(0, "prod-redis", 10, 100, 1, was, sizeof was) == 1 &&
		!strcmp(was, "0"), "B: map 0 prod-redis applied, was '%s'", was);
	CHK(!strcmp(eff(), "0:prod-redis,1:prod-redis,2:th"),
		"B: the override first, the bare 0 gone ('%s')", eff());
	CHK(res(0, "prod-redis", PC_RESPMAP_RUNTIME) && res(1, "prod-redis", PC_RESPMAP_CONFIG),
		"B: 0 -> prod-redis (runtime), 1 still -> prod-redis (config)");
	CHK(pc_respmap_overrides() == 1, "B: one override");

	/* C */
	CHK(pc_respmap_apply(0, "th", 2, 99, 1, NULL, 0) == 0 && res(0, "prod-redis", PC_RESPMAP_RUNTIME),
		"C: an older generation (99 < 100) is ignored");
	CHK(pc_respmap_apply(0, "aaa", 3, 100, 1, NULL, 0) == 0 && res(0, "prod-redis", PC_RESPMAP_RUNTIME),
		"C: the same generation, a smaller name: ignored");
	CHK(pc_respmap_apply(0, "zzz", 3, 100, 1, NULL, 0) == 1 && res(0, "zzz", PC_RESPMAP_RUNTIME),
		"C: the same generation, a larger name: taken - every member settles on it");
	CHK(pc_respmap_apply(0, "prod-redis", 10, 101, 1, was, sizeof was) == 1 &&
		!strcmp(was, "zzz") && res(0, "prod-redis", PC_RESPMAP_RUNTIME),
		"C: a newer generation wins (was '%s')", was);

	/* D */
	CHK(pc_respmap_apply(0, "", 0, 102, 0, was, sizeof was) == 1 && !strcmp(was, "prod-redis") &&
		res(0, "0", PC_RESPMAP_CONFIG) && !strcmp(eff(), "0, 1:prod-redis, 2:th"),
		"D: unmap 0 - '0' from the config again, the list is the config's ('%s')", eff());
	memset(&sn, 0, sizeof sn);
	pc_respmap_each(each_cb, &sn);
	CHK(sn.n == 1 && sn.idx[0] == 0 && sn.set[0] == 0 && pc_respmap_overrides() == 0,
		"D: the tombstone is held (each lists it, unset); no override counted");
	CHK(pc_respmap_apply(0, "prod-redis", 10, 101, 1, NULL, 0) == 0 && res(0, "0", PC_RESPMAP_CONFIG),
		"D: a map older than the unmap (101 < 102) arriving late is ignored");
	CHK(pc_respmap_apply(5, "th", 2, 103, 1, NULL, 0) == 1 && res(5, "th", PC_RESPMAP_RUNTIME) &&
		strstr(eff(), "5:th") == eff(), "D: map 5 th - an index the config never named ('%s')", eff());
	CHK(pc_respmap_persist() == 0, "D: persisted");

	/* F (the file D left: 0 a tombstone at 102, 5 -> th at 103) */
	CHK(pc_respmap_count_file(dir) == 1, "F: count_file sees one override (the tombstone not counted)");
	CHK(pc_respmap_init("0, 1:prod-redis, 2:th", dir) == 0 && res(5, "th", PC_RESPMAP_RUNTIME) &&
		res(0, "0", PC_RESPMAP_CONFIG), "F: a re-init from the state dir: 5 -> th, 0 as the config says");
	CHK(pc_respmap_apply(0, "prod-redis", 10, 101, 1, NULL, 0) == 0,
		"F: the tombstone survived too - the late older map is still ignored");
	CHK(pc_lamport_now() >= 103,
		"F: the clock resumed above the newest change loaded (%llu >= 103) - an unmap after a restart outranks the map",
		pc_lamport_now());
	snprintf(path, sizeof path, "%s/respmap", dir);
	f = fopen(path, "w");
	if (f) {
		fputs("this is not a respmap\nmap 0 1 1 x\n", f);
		fclose(f);
	}
	CHK(pc_respmap_count_file(dir) == -1 && pc_respmap_init("0", dir) == -1,
		"F: a file that is not ours is refused by count_file and by init");
	unlink(path);
	CHK(pc_respmap_count_file(dir) == 0, "F: no file: 0");

	/* E */
	CHK(pc_respmap_init(NULL, dir) == 0 && pc_respmap_effective() == NULL &&
		res(7, "7", PC_RESPMAP_OPEN), "E: no list - nothing published, 7 -> '7' (open)");
	CHK(pc_respmap_apply(0, "prod-redis", 10, 200, 1, NULL, 0) == 1 &&
		!strcmp(eff(), "*,0:prod-redis") && res(0, "prod-redis", PC_RESPMAP_RUNTIME) &&
		res(7, "7", PC_RESPMAP_OPEN), "E: map 0 on an open door: '%s' - 7 still reaches '7'", eff());

	/* G */
	CHK(pc_respmap_apply(-1, "x", 1, 300, 1, NULL, 0) == -1 &&
		pc_respmap_apply(PC_RESPMAP_MAX, "x", 1, 300, 1, NULL, 0) == -1,
		"G: index -1 and %d refused", PC_RESPMAP_MAX);
	CHK(pc_respmap_apply(1, "a,b", 3, 300, 1, NULL, 0) == -1 &&
		pc_respmap_apply(1, "a:b", 3, 300, 1, NULL, 0) == -1 &&
		pc_respmap_apply(1, "*", 1, 300, 1, NULL, 0) == -1 &&
		pc_respmap_apply(1, "a b", 3, 300, 1, NULL, 0) == -1 &&
		pc_respmap_apply(1, "", 0, 300, 1, NULL, 0) == -1 &&
		pc_respmap_apply(1, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 64,
			300, 1, NULL, 0) == -1 && res(1, "1", PC_RESPMAP_OPEN),
		"G: ',', ':', '*', a space, empty and a 64-byte name refused; nothing changed");

	snprintf(path, sizeof path, "%s/respmap", dir);
	unlink(path);
	rmdir(dir);
	printf("respmaptest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
