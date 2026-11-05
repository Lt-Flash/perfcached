/* respsrvtest.c - the RESP door's connection and server commands
 * (respsrv.c; the 0.7.2 split, wave 1): the store, the observers and the
 * password plane stubbed; every reply byte for byte.
 *
 *   A  PING: PONG, or the message back; in subscribed mode (PS2) the
 *      array predis's pubSubLoop reads, with or without a message
 *   B  AUTH: refused when no password is set; AUTH <pw> and AUTH <user>
 *      <pw> (the second word is the password); a wrong one is Redis's
 *      WRONGPASS and counted; the arity
 *   C  HELLO: RESP2's map of 14, bare or with 2; 3 is NOPROTO; a version
 *      that is not a number refused
 *   D  INFO: bare - server (standalone, or cluster), replication (the
 *      role rtpengine will not start without), cluster, clients, memory
 *      (the arena's held bytes), stats (hits and misses summed over the
 *      live collections), keyspace (each reachable collection with keys,
 *      under the index that reaches it; empty, dropped and unlisted ones
 *      left out) - and not commandstats or latencystats; one named
 *      section alone; all = every one; a reply that cannot fit refused
 *   E  TIME: seconds and microseconds, as two bulks of digits
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "json.h"
#include "store.h"
#include "core/pcache_htable.h"
#include "core/pcache_arena.h"
#include "obs.h"
#include "pubsub.h"
#include "daemon.h"
#include "version.h"
#include "verbs.h"
#include "respout.h"
#include "verbsint.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* ---- the stubs ---------------------------------------------------------- */
unsigned int pc_resp_conns, pc_resp_rejected, pc_resp_authfail;
static int subs, pw_set, cluster_on;

int pc_pubsub_count(void *conn) { (void)conn; return subs; }
int pc_resp_password_set(void) { return pw_set; }
int pc_resp_password_ok(const char *p, size_t n) { return n == 6 && !memcmp(p, "sesame", 6); }
int resp_cluster_on(void) { return cluster_on; }
int pc_obs_conn_count(void) { return 7; }
unsigned long pcache_arena_held_bytes(void) { return 3UL << 20; }
unsigned long long pc_obs_total_calls(void) { return 1234; }
unsigned long long pc_obs_inst_ops(void) { return 56; }
void pc_obs_cmdstats(struct pc_jw *w) { pc_jw_lit(w, "cmdstat_get:calls=1\r\n"); }
void pc_obs_latencystats(struct pc_jw *w) { pc_jw_lit(w, "latency_percentiles_usec_get:p50=1\r\n"); }

/* four collections: "0" live with 3 keys, "gone" dropped, "empty" live with
 * none, "hidden" live but not on the RESP allow-list; "sess" live, reached
 * by index 5 */
static struct { const char *name; int live; unsigned long entries, hits, misses; } cols[] = {
	{ "0", 1, 3, 10, 1 }, { "gone", 0, 9, 100, 100 }, { "empty", 1, 0, 2, 0 },
	{ "hidden", 1, 4, 1, 1 }, { "sess", 1, 2, 5, 3 },
};
#define NCOLS ((int)(sizeof cols / sizeof cols[0]))

int pc_store_count(void) { return NCOLS; }
int pc_store_live(int i) { return cols[i].live; }
const char *pc_store_name(int i) { return cols[i].name; }
pcache_htable_t *pc_store_ht(int i) { return (pcache_htable_t *)(void *)&cols[i]; }

void pcache_ht_totals(pcache_htable_t *ht, pcache_ht_totals_t *out)
{
	int i = (int)(((char *)(void *)ht - (char *)(void *)cols) / (long)sizeof cols[0]);

	memset(out, 0, sizeof *out);
	out->entries = cols[i].entries;
	out->hits = cols[i].hits;
	out->misses = cols[i].misses;
}

int resp_col_allowed(const char *name, size_t n) { return !(n == 6 && !memcmp(name, "hidden", 6)); }

size_t resp_index_for(const char *name, size_t nl, char *out, size_t cap)
{
	if (nl == 4 && !memcmp(name, "sess", 4))
		name = "5", nl = 1;
	if (nl >= cap)
		nl = cap - 1;
	memcpy(out, name, nl);
	out[nl] = 0;
	return nl;
}

/* ---- the harness ------------------------------------------------------- */
static char outbuf[8192], scratch[4096];
static size_t scratch_cap = sizeof scratch;
static struct pc_jw out;
static int authed;

static void run(int (*h)(struct resp_ctx *), const char *const *args)
{
	char *argv[8];
	size_t argl[8];
	struct resp_ctx c;
	int n;

	for (n = 0; n < 8 && args[n]; n++) {
		argv[n] = (char *)args[n];
		argl[n] = strlen(args[n]);
	}
	memset(&c, 0, sizeof c);
	c.argv = argv;
	c.argl = argl;
	c.nargs = n;
	c.out = &out;
	c.cmd = args[0];
	c.cl = strlen(args[0]);
	c.conn = &out;
	c.authed = &authed;
	c.scratch = scratch;
	c.scratch_cap = scratch_cap;
	pc_jw_init(&out, outbuf, sizeof outbuf);
	h(&c);
}
#define RUN(h, ...) run((h), (const char *const[]){ __VA_ARGS__, NULL })

static int is(const char *want)
{
	return out.len == strlen(want) && !memcmp(out.buf, want, out.len);
}

static int has(const char *needle)
{
	size_t n = strlen(needle), i;

	for (i = 0; i + n <= out.len; i++)
		if (!memcmp(out.buf + i, needle, n))
			return 1;
	return 0;
}

/* INFO's reply is one bulk: "$<len>\r\n<text>\r\n" - its length must agree */
static int bulk_ok(void)
{
	char *end;
	long n;

	if (out.len < 4 || out.buf[0] != '$')
		return 0;
	n = strtol(out.buf + 1, &end, 10);
	return end[0] == '\r' && end[1] == '\n' &&
		(size_t)(end + 2 - out.buf) + (size_t)n + 2 == out.len;
}

int main(void)
{
	/* A */
	RUN(rh_ping, "PING");
	CHK(is("+PONG\r\n"), "A: PING - PONG");
	RUN(rh_ping, "PING", "hi");
	CHK(is("$2\r\nhi\r\n"), "A: PING hi - the message back");
	subs = 1;
	RUN(rh_ping, "PING");
	CHK(is("*2\r\n$4\r\npong\r\n$0\r\n\r\n"), "A: subscribed - [pong, \"\"] (PS2)");
	RUN(rh_ping, "PING", "hi");
	CHK(is("*2\r\n$4\r\npong\r\n$2\r\nhi\r\n"), "A: subscribed with a message - [pong, hi]");
	subs = 0;

	/* B */
	RUN(rh_auth, "AUTH", "sesame");
	CHK(is("-ERR Client sent AUTH, but no password is set\r\n") && !authed,
		"B: no password set - refused");
	pw_set = 1;
	RUN(rh_auth, "AUTH", "sesame");
	CHK(is("+OK\r\n") && authed, "B: AUTH <pw>");
	authed = 0;
	RUN(rh_auth, "AUTH", "anyone", "sesame");
	CHK(is("+OK\r\n") && authed, "B: AUTH <user> <pw> - the second word is the password");
	authed = 0;
	RUN(rh_auth, "AUTH", "sesame", "x");
	CHK(is("-WRONGPASS invalid username-password pair or user is disabled.\r\n") &&
		!authed && pc_resp_authfail == 1, "B: a wrong one - WRONGPASS, counted");
	RUN(rh_auth, "AUTH", "a", "b", "c");
	CHK(is("-ERR wrong number of arguments for 'auth' command\r\n"), "B: the arity");

	/* C */
	{
		static const char hello[] = "*14\r\n$6\r\nserver\r\n$10\r\nperfcached\r\n"
			"$7\r\nversion\r\n";
		char want[512];

		snprintf(want, sizeof want, "%s$%d\r\n%s\r\n$5\r\nproto\r\n:2\r\n$2\r\nid\r\n:0\r\n"
			"$4\r\nmode\r\n$10\r\nstandalone\r\n$4\r\nrole\r\n$6\r\nmaster\r\n"
			"$7\r\nmodules\r\n*0\r\n", hello, (int)strlen(PC_VERSION), PC_VERSION);
		RUN(rh_hello, "HELLO");
		CHK(is(want), "C: HELLO - the RESP2 map of 14, byte for byte");
		RUN(rh_hello, "HELLO", "2");
		CHK(is(want), "C: HELLO 2 - the same");
	}
	RUN(rh_hello, "HELLO", "3");
	CHK(is("-NOPROTO unsupported protocol version\r\n"), "C: HELLO 3 - NOPROTO");
	RUN(rh_hello, "HELLO", "x");
	CHK(is("-ERR Protocol version is not an integer or out of range\r\n"), "C: not a number");

	/* D */
	RUN(rh_info, "INFO");
	CHK(bulk_ok(), "D: INFO is one bulk, its length right");
	CHK(has("# Server\r\nredis_version:7.0.0\r\nredis_mode:standalone\r\n"
		"perfcached_version:" PC_VERSION "\r\n"), "D: server - standalone, the version");
	CHK(has("# Replication\r\nrole:master\r\nconnected_slaves:0\r\n"), "D: replication - role:master");
	CHK(has("# Cluster\r\ncluster_enabled:0\r\n"), "D: cluster");
	CHK(has("# Clients\r\nconnected_clients:7\r\n"), "D: clients");
	CHK(has("# Memory\r\nused_memory:3145728\r\nused_memory_human:3M\r\n"),
		"D: memory - the arena's held bytes");
	CHK(has("total_commands_processed:1234\r\ninstantaneous_ops_per_sec:56\r\n"
		"keyspace_hits:18\r\nkeyspace_misses:5\r\n"),
		"D: stats - hits and misses of the live collections only (10+2+1+5, 1+0+1+3)");
	CHK(has("# Keyspace\r\ndb0:keys=3,expires=0,avg_ttl=0\r\n"
		"db5:keys=2,expires=0,avg_ttl=0\r\n"),
		"D: keyspace - 0 and sess under index 5; empty, dropped, unlisted left out");
	CHK(!has("db6") && !has("dbhidden") && !has("dbempty") && !has("dbgone"),
		"D: and nothing else in it");
	CHK(!has("# Commandstats") && !has("# Latencystats"), "D: bare INFO leaves the two stats sections out");
	cluster_on = 1;
	RUN(rh_info, "INFO", "server");
	CHK(has("redis_mode:cluster\r\n") && !has("# Replication") && !has("# Keyspace"),
		"D: INFO server - that section alone, cluster mode");
	cluster_on = 0;
	RUN(rh_info, "INFO", "commandstats");
	CHK(has("# Commandstats\r\ncmdstat_get:calls=1\r\n") && !has("# Server"),
		"D: INFO commandstats - only it (what the Grafana panel needs)");
	RUN(rh_info, "INFO", "ALL");
	CHK(has("# Server") && has("# Keyspace") && has("# Commandstats") &&
		has("# Latencystats\r\nlatency_percentiles_usec_get:p50=1\r\n"), "D: INFO all - every section");
	scratch_cap = 64;
	RUN(rh_info, "INFO");
	CHK(is("-ERR info too large\r\n"), "D: a reply that cannot fit the scratch - refused");
	scratch_cap = sizeof scratch;

	/* E */
	{
		long long s, us;
		char *e;
		time_t now = time(NULL);

		RUN(rh_time, "TIME");
		outbuf[out.len] = 0;               /* the parse below stays inside it */
		CHK(!strncmp(out.buf, "*2\r\n$", 5), "E: TIME - two bulks");
		e = strstr(out.buf + 5, "\r\n");
		s = e ? strtoll(e + 2, NULL, 10) : -1;
		e = e ? strstr(e + 2, "\r\n$") : NULL;
		e = e ? strstr(e + 3, "\r\n") : NULL;
		us = e ? strtoll(e + 2, NULL, 10) : -1;
		CHK(s >= (long long)now - 2 && s <= (long long)now + 2 && us >= 0 && us < 1000000,
			"E: seconds now (%lld), microseconds under a million (%lld)", s, us);
	}

	printf("respsrvtest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
