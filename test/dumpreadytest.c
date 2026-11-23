/* SPDX-License-Identifier: GPL-2.0-or-later */
/* dumpreadytest.c - perfdump's readiness guard on a node's stats (perfdump.c compiled in, its main
 * renamed): only a READY node is dumped from; a STARTING one answers empty, a DRAINING one partial.
 * --force dumps it anyway and the manifest records the state it was in. */
#define main perfdump_main
#include "../cli/perfdump.c"
#undef main

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* 1 = perfdump refuses a node whose stats are @js */
static int refused(const char *js)
{
	static struct pc_jtok toks[256];
	int n = pc_json_parse(js, strlen(js), toks, 256);

	return n > 0 && not_ready(js, toks, n) >= 0;
}

/* ready_gate on @js with --force @force: its answer; the state kept in @sh */
static int gate(struct shared *sh, struct opts *o, int force, const char *js)
{
	static struct pc_jtok toks[256];
	int n = pc_json_parse(js, strlen(js), toks, 256);

	memset(sh, 0, sizeof *sh);
	memset(o, 0, sizeof *o);
	o->force = force;
	o->threads = 1;
	sh->o = o;
	return n > 0 ? ready_gate(sh, js, toks, n) : -2;
}

/* 1 = @dir's manifest.json holds @s */
static int manifest_has(const char *dir, const char *s)
{
	char path[512], buf[4096];
	FILE *f;
	size_t n;

	snprintf(path, sizeof path, "%s/manifest.json", dir);
	f = fopen(path, "rb");
	if (!f)
		return 0;
	n = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[n] = 0;
	return strstr(buf, s) != NULL;
}

int main(void)
{
	static char dir[] = "/var/tmp/dumpreadytest.XXXXXX";
	static const char starting[] =
		"{\"version\":\"0.7.5\",\"state\":\"starting\",\"cluster\":{\"role\":\"backup\",\"peers_up\":0}}";
	struct shared sh;
	struct opts o;
	char cmd[600];

	/* the lifecycle state is a root key of stats (src/statstree.c), on a clustered node and a
	 * standalone one (cluster null) alike */
	CHK(!refused("{\"version\":\"0.7.5\",\"state\":\"ready\",\"cluster\":{\"role\":\"backup\",\"peers_up\":2}}"),
		"a READY member is dumped from");
	CHK(!refused("{\"version\":\"0.7.5\",\"state\":\"ready\",\"cluster\":null}"),
		"a READY standalone node is dumped from");
	CHK(refused("{\"version\":\"0.7.5\",\"state\":\"starting\",\"cluster\":{\"role\":\"backup\",\"peers_up\":0}}"),
		"a STARTING member is refused (its store is still empty)");
	CHK(refused("{\"version\":\"0.7.5\",\"state\":\"draining\",\"cluster\":{\"role\":\"backup\",\"peers_up\":2}}"),
		"a DRAINING member is refused (it is handing its records away)");
	CHK(refused("{\"version\":\"0.7.5\",\"state\":\"starting\",\"cluster\":null}"),
		"a STARTING standalone node is refused");

	/* --force: a node that is not READY is dumped from, and the dump says what state it was in */
	if (!mkdtemp(dir)) {
		printf("  FAIL mkdtemp\n");
		return 1;
	}
	CHK(gate(&sh, &o, 0, starting) == -1, "without --force a STARTING node is refused");
	CHK(gate(&sh, &o, 1, starting) == 0, "with --force it is dumped from");
	CHK(!strcmp(sh.node_state, "starting"), "and its state is kept for the manifest ('%s')", sh.node_state);
	o.out = dir;
	CHK(manifest_write(&sh) == 0 && manifest_has(dir, "\"node_state\":\"starting\""),
		"the manifest records \"node_state\":\"starting\"");
	CHK(gate(&sh, &o, 0, "{\"state\":\"ready\",\"cluster\":null}") == 0 &&
		!strcmp(sh.node_state, "ready"), "a READY node's state is kept too ('%s')", sh.node_state);
	{
		char *av[] = { "perfdump", "--force", "--inspect", dir, NULL };

		CHK(perfdump_main(4, av) == 0, "perfdump --force is an option, and --inspect reads a manifest with node_state");
	}
	snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
	if (system(cmd) != 0)
		printf("  ..   could not remove %s\n", dir);
	printf("dumpreadytest: %s\n", fails ? "FAILED" : "all passed");
	return fails ? 1 : 0;
}
