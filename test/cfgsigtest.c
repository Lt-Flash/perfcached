/* cfgsigtest.c - AR11's settings signature (cfgsig.c; the 0.7.2 split,
 * wave 1): cfgsig_build on configs built here, what it hands the cluster
 * captured, the log stubbed.
 *
 *   A  the fleet-wide four: [wal] fsync (off without a WAL), the HTTP
 *      token (set or none, never its value), the [alerts] thresholds, the
 *      tombstone retention - each named, said, and hashed: the name with
 *      bit 31 clear (a set bit is a fact), the value as said
 *   B  fsync's words, and "?" for a value outside them
 *   C  a read-through collection's four - writes, prefixes (spaces out),
 *      role, unknown - and none for a collection without an upstream
 *   D  a value longer than the alert can say is cut in the text, not in
 *      the hash: two that differ past the cut still differ
 *   E  more than the keepalive carries: the rest dropped, warned once by
 *      the first name left out, never again
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "compat/dprint.h"
#include "config.h"
#include "cluster.h"
#include "fnv1a.h"
#include "cfgsig.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* ---- the stubs: the log, the cluster's setter -------------------------- */
int compat_log_level = L_NOTICE;
static char logbuf[2048];
static int warns;

void compat_log(int level, const char *tag, const char *fmt, ...)
{
	size_t n = strlen(logbuf);
	va_list ap;

	(void)tag;
	va_start(ap, fmt);
	vsnprintf(logbuf + n, sizeof logbuf - n, fmt, ap);
	va_end(ap);
	if (level == L_WARN)
		warns++;
}

static struct pc_cfgsig got[PC_CFGSIG_MAX];
static int ngot = -1;

void pc_cluster_set_cfgsig(const struct pc_cfgsig *e, int n)
{
	memcpy(got, e, (size_t)n * sizeof *e);
	ngot = n;
}

/* ---- the harness ------------------------------------------------------- */
static struct pc_config cfg;

static void base(void)
{
	memset(&cfg, 0, sizeof cfg);
	cfg.alerts.cpu_warn = 65;
	cfg.alerts.cpu_crit = 75;
	cfg.alerts.io_warn = 60;
	cfg.alerts.io_crit = 70;
	cfg.alerts.mem_warn = 80;
	cfg.alerts.mem_crit = 90;
	cfg.alerts.arena_warn = 85;
	cfg.alerts.arena_crit = 95;
	cfg.alerts.epoll_busy = 50;
	cfg.cl_tomb_retain_s = 3600;
	cfg.cl_tomb_retain_max = 250000;
}

static const struct pc_cfgsig *find(const char *name)
{
	int i;

	for (i = 0; i < ngot; i++)
		if (!strcmp(got[i].name, name))
			return &got[i];
	return NULL;
}

/* @name present, saying @val, its hashes right */
static int says(const char *name, const char *val)
{
	const struct pc_cfgsig *e = find(name);

	return e && !strcmp(e->val, val) &&
		e->name_h == ((uint32_t)fnv1a64(name, strlen(name)) & ~PC_FACT_BIT) &&
		e->val_h == (uint32_t)fnv1a64(val, strlen(val));
}

static void readthrough(int i, const char *name, const char *prefixes)
{
	cfg.col[i].name = (char *)name;
	cfg.col[i].upstream = "redis://10.0.0.1:6379";
	cfg.col[i].up_prefixes = (char *)prefixes;
	if (cfg.n_col <= i)
		cfg.n_col = i + 1;
}

int main(void)
{
	char p1[80], p2[80];
	int i, nh;

	/* A */
	base();
	cfgsig_build(&cfg);
	CHK(ngot == 5, "A: no read-through - the fleet-wide five");
	CHK(says("wal.fsync", "off") && !strcmp(find("wal.fsync")->what, "[wal] fsync"),
		"A: no WAL - fsync says off");
	CHK(says("http.token", "none"), "A: no HTTP token - none");
	CHK(says("alerts", "cpu 65/75 io 60/70 memory 80/90 arena 85/95 epoll 50"), "A: the [alerts] thresholds, all nine");
	CHK(says("tombstones", "3600 s, 250000 at most"), "A: the tombstone retention");
	CHK(says("caches", "none") && !strcmp(find("caches")->what, "[collection] durable = no"),
		"A: no cache collection - caches says none (G5)");
	for (nh = 0, i = 0; i < ngot; i++)
		nh += !(got[i].name_h & PC_FACT_BIT);
	CHK(nh == ngot, "A: every name hash with bit 31 clear - a setting, never a fact");

	/* B */
	base();
	cfg.wal_dir = "/var/lib/perfcached";
	cfg.wal_fsync = 1;
	cfg.http_token = "s3cret-token";
	cfgsig_build(&cfg);
	CHK(says("wal.fsync", "always") && says("http.token", "set") && !strstr(find("http.token")->val, "s3cret"),
		"B: fsync = always; a token - set, its value never said");
	cfg.wal_fsync = 2;
	cfgsig_build(&cfg);
	CHK(says("wal.fsync", "no"), "B: fsync = no");
	cfg.wal_fsync = 7;
	cfgsig_build(&cfg);
	CHK(says("wal.fsync", "?"), "B: a value outside the three - ?");

	/* C */
	base();
	cfg.col[0].name = "plain";
	readthrough(1, "users", "user:*, sess:*");
	cfg.col[1].up_write_through = 1;
	cfg.col[1].up_unknown = PC_UP_UNKNOWN_PASS;
	cfg.col[1].up_role = PC_UP_ROLE_REPLICA;
	readthrough(2, "cache", "*");
	cfg.col[2].up_refuse_writes = 1;
	cfgsig_build(&cfg);
	CHK(ngot == 13 && !find("plain.writes"), "C: two read-through collections - four each; the plain one none");
	CHK(says("users.writes", "through") && says("users.prefixes", "user:*,sess:*") &&
		says("users.role", "replica") && says("users.unknown", "pass") &&
		!strcmp(find("users.prefixes")->what, "[collection users] upstream_prefixes"),
		"C: users - through, its prefixes with the spaces out, replica, pass");
	CHK(says("cache.writes", "refuse") && says("cache.role", "any") && says("cache.unknown", "-"),
		"C: cache - refuse, any, and unknown - (not through)");

	/* D */
	memset(p1, 'a', 70);
	memcpy(p2, p1, 70);
	strcpy(p1 + 70, "x");
	strcpy(p2 + 70, "y");
	base();
	readthrough(0, "c", p1);
	cfgsig_build(&cfg);
	{
		struct pc_cfgsig one = *find("c.prefixes");

		base();
		readthrough(0, "c", p2);
		cfgsig_build(&cfg);
		CHK(strlen(one.val) == sizeof one.val - 1 && !strcmp(one.val, find("c.prefixes")->val) &&
			one.val_h != find("c.prefixes")->val_h,
			"D: 71 characters - the text cut at %d, the same; the hashes of all of it differ",
			(int)sizeof one.val - 1);
	}

	/* E */
	base();
	for (i = 0; i < 5; i++) {
		static char nm[5][8];

		snprintf(nm[i], sizeof nm[i], "c%d", i);
		readthrough(i, nm[i], "*");
	}
	logbuf[0] = 0;
	warns = 0;
	cfgsig_build(&cfg);
	CHK(ngot == PC_CFGSIG_MAX - PC_CL_NFACTS && find("c3.unknown") && !find("c4.writes"),
		"E: five read-through collections - %d carried (room left for the facts), c4's dropped",
		PC_CFGSIG_MAX - PC_CL_NFACTS);
	CHK(warns == 1 && strstr(logbuf, "'c4.writes' and any after it are not compared"),
		"E: warned once, by the first left out");
	cfgsig_build(&cfg);
	CHK(warns == 1 && ngot == PC_CFGSIG_MAX - PC_CL_NFACTS, "E: built again - not warned again");

	/* F: G5 - the cache collections, as one entry, in config order */
	base();
	cfg.col[0].name = "sess";
	cfg.col[0].nondurable = 1;
	cfg.col[1].name = "kept";
	cfg.col[2].name = "tmp";
	cfg.col[2].nondurable = 1;
	cfg.n_col = 3;
	cfgsig_build(&cfg);
	CHK(says("caches", "sess,tmp"), "F: two cache collections - caches says sess,tmp; kept is durable");
	cfg.col[2].nondurable = 0;
	cfgsig_build(&cfg);
	CHK(says("caches", "sess"), "F: one - caches says sess");

	printf("cfgsigtest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
