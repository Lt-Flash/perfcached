/* SPDX-License-Identifier: GPL-2.0-or-later */
/* cliupgtest.c - the roll driver's own parsing and spawning (perfcli.c compiled in, its main
 * renamed): the records gate's sum, the tarball's member name, the switch hook's arguments. */
#define main perfcli_main
#include "../cli/perfcli.c"
#undef main

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static char dir[] = "/var/tmp/cliupgtest.XXXXXX";

static void put(const char *name, const char *body, int mode)
{
	char path[256];
	FILE *f;

	snprintf(path, sizeof path, "%s/%s", dir, name);
	f = fopen(path, "w");
	if (f) {
		fputs(body, f);
		fclose(f);
	}
	chmod(path, (mode_t)mode);
}

/* @name's text into @buf, "" when absent */
static const char *slurp(const char *name, char *buf, size_t cap)
{
	char path[256];
	FILE *f;
	size_t n = 0;

	snprintf(path, sizeof path, "%s/%s", dir, name);
	buf[0] = 0;
	f = fopen(path, "r");
	if (f) {
		n = fread(buf, 1, cap - 1, f);
		fclose(f);
	}
	buf[n] = 0;
	return buf;
}

static int lines(const char *s)
{
	int n = 0;

	for (; *s; s++)
		n += *s == '\n';
	return n;
}

int main(void)
{
	char buf[4096], path[256];

	if (!mkdtemp(dir)) {
		printf("  FAIL mkdtemp\n");
		return 1;
	}

	printf("--- A. the lost-records gate sums each collection's own entries\n");
	{
		/* the shape of a node's stats: collections, then root objects - one with an "entries" */
		static const char st[] = "{\"version\":\"0.7.5\",\"state\":\"ready\","
			"\"collections\":[{\"name\":\"a\",\"entries\":5,\"neg\":{\"entries\":100}},"
			"{\"name\":\"b\",\"entries\":7,\"t\":[1,{\"entries\":9}]}],"
			"\"cluster\":{\"peers_up\":2},\"negative\":{\"entries\":40}}";

		CHK(jcollections(st, NULL) == 12, "A1 5 + 7 from the two collections (got %lld)",
			jcollections(st, NULL));
		CHK(jcollections("{\"collections\":[]}", NULL) == 0, "A2 no collection: 0");
		CHK(jcollections("{\"state\":\"ready\"}", NULL) == 0, "A3 no collections key: 0");
	}

	printf("--- A'. shard: one peer's count moves, only the fleet's sum must hold\n");
	{
		/* rc4's roll (115225): peer 870 held half of node 1's share until the reshard tick */
		struct unode b[3] = { { .node = 1, .reach = 1, .entries = 1100 },
			{ .node = 635, .reach = 1, .entries = 1333 },
			{ .node = 870, .reach = 1, .entries = 1657 } };
		struct unode a[3];
		char why[512];
		int sh = 0;

		jcollections("{\"collections\":[{\"name\":\"a\",\"mode\":\"shard\",\"entries\":3}]}", &sh);
		CHK(sh == 1, "A'1 a shard collection is seen");
		jcollections("{\"collections\":[{\"name\":\"a\",\"mode\":\"eager\",\"entries\":3,"
			"\"t\":{\"mode\":\"shard\"}}]}", &sh);
		CHK(sh == 0, "A'2 eager, and a nested mode is not the collection's");
		memcpy(a, b, sizeof a);
		a[0].entries = 1433; a[2].entries = 1318;
		CHK(ugate_lost(b, 3, a, 3, 635, why, sizeof why) == 1 && strstr(why, "peer=870 entries 1657 -> 1318"),
			"A'3 eager: the peer's fall is a loss (%s)", why);
		b[0].shard = b[1].shard = b[2].shard = 1;
		CHK(ugate_lost(b, 3, a, 3, 635, why, sizeof why) == 0,
			"A'4 shard: the records moved to node 1, the sum held (%s)", why);
		a[0].entries = 1100;
		CHK(ugate_lost(b, 3, a, 3, 635, why, sizeof why) == 0,
			"A'5 shard: the sum fell by 8%%, inside the margin (%s)", why);
		a[0].entries = 600;
		CHK(ugate_lost(b, 3, a, 3, 635, why, sizeof why) == 1 && strstr(why, "fleet entries 4090 -> 3251"),
			"A'6 shard: the sum fell by 20%% - a loss (%s)", why);
		a[2].reach = 0;
		CHK(ugate_lost(b, 3, a, 3, 635, why, sizeof why) == 1 && strstr(why, "peer=870 unreachable"),
			"A'7 shard: a peer out is a loss (%s)", why);
	}

	printf("--- B. a member name from the tarball is never an option to tar\n");
	{
		struct urel rl;
		char sh[1024];
		const char *old = getenv("PATH");
		char np[4096];

		/* a tar of our own first on PATH: it logs its arguments and plays the archive */
		snprintf(sh, sizeof sh, "#!/bin/sh\n"
			"for a; do printf '[%%s]' \"$a\"; done >> %s/tar.log; echo >> %s/tar.log\n"
			"case \"$1\" in -tzf) cat %s/list;; "
			"*) printf 'version=9.9.9\\nwire_epoch_min=4\\nwire_epoch_max=4\\n';; esac\n",
			dir, dir, dir);
		put("tar", sh, 0755);
		snprintf(np, sizeof np, "%s:%s", dir, old ? old : "/usr/bin:/bin");
		setenv("PATH", np, 1);

		put("list", "perfcached-9.9.9/bin/perfcached\nperfcached-9.9.9/share/perfcached/RELEASE\n", 0644);
		put("tar.log", "", 0644);
		ufile_release("T.tgz", &rl);
		slurp("tar.log", buf, sizeof buf);
		CHK(rl.found && !strcmp(rl.version, "9.9.9"), "B1 an ordinary tarball's RELEASE is read");
		CHK(strstr(buf, "[-xzOf][T.tgz][--][perfcached-9.9.9/share/perfcached/RELEASE]") != NULL,
			"B2 the member is passed after --: %s", buf);

		put("list", "--checkpoint-action=exec=touch pwned #/share/perfcached/RELEASE\n", 0644);
		put("tar.log", "", 0644);
		ufile_release("T.tgz", &rl);
		slurp("tar.log", buf, sizeof buf);
		CHK(!rl.found && lines(buf) == 1,
			"B3 a listed name beginning with '-' is refused, not handed to tar: %s", buf);
		setenv("PATH", old ? old : "/usr/bin:/bin", 1);
	}

	printf("--- C. the switch hook gets its arguments as they are, no shell\n");
	{
		struct uplan pl;
		struct unode u;
		char sh[512];
		int rc;

		memset(&pl, 0, sizeof pl);
		memset(&u, 0, sizeof u);
		snprintf(sh, sizeof sh, "#!/bin/sh\nfor a; do printf '[%%s]' \"$a\"; done > %s/hook.out\n", dir);
		put("hook", sh, 0755);
		snprintf(pl.switch_cmd, sizeof pl.switch_cmd, "%s/hook", dir);
		snprintf(pl.artifact, sizeof pl.artifact, "/var/tmp/it's.tgz");
		snprintf(pl.sha, sizeof pl.sha, "abc123");
		/* the address as a member advertised it */
		snprintf(u.addr, sizeof u.addr, "10.0.0.1;touch %s/pwned;", dir);
		u.port = 6479;
		u.node = 3;
		rc = uhook(&pl, "switch", &u);
		snprintf(path, sizeof path, "%s/pwned", dir);
		CHK(access(path, F_OK) != 0, "C1 nothing in the member's address ran");
		snprintf(sh, sizeof sh, "[switch][10.0.0.1;touch %s/pwned;][6479][3][/var/tmp/it's.tgz][abc123]", dir);
		CHK(rc == 0 && !strcmp(slurp("hook.out", buf, sizeof buf), sh),
			"C2 the hook saw six arguments, verbatim (rc %d): %s", rc, buf);
	}

	snprintf(path, sizeof path, "rm -rf '%s'", dir);
	if (system(path) != 0)
		printf("  (could not remove %s)\n", dir);
	printf("cliupgtest: %s\n", fails ? "FAILED" : "all passed");
	return fails ? 1 : 0;
}
