/* respscripttest.c - EVAL / EVALSHA / SCRIPT on the RESP door
 * (respscript.c; the 0.7.2 split, wave 1): the lock core (op_slock) is
 * stubbed, SHA1 is the real one, and two of Symfony's own script bodies
 * (test/symfony_lock.py) are sent as a client sends them.
 *
 *   A  EVAL of an approved body runs its script as C - PROBE, RELEASE with
 *      its token - Lua true as :1, false as nil, the key noted; any other
 *      body refused by name
 *   B  EVALSHA by name, upper case too: SAVE takes the token and the ttl
 *      and the clock; the _argv variants take "now" from ARGV[1] (seconds,
 *      floored to ms); EXISTS reads no ttl; ARGV[1] that is not a time and
 *      a ttl that is not whole milliseconds refused; an unknown name is
 *      Redis's NOSCRIPT
 *   C  the arguments: numkeys not a number or past the arguments, other
 *      than 1, too few ARGV, no key, the key too long, the arity
 *   D  every answer of the lock core: busy, the version gate (the node
 *      named), the collection mode, WRONGTYPE, TRYAGAIN, forward failed,
 *      a write refusal, a token too long, anything else
 *   E  SCRIPT LOAD answers an approved body's SHA and refuses any other;
 *      EXISTS per name; FLUSH OK; anything else refused
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"
#include "verbs.h"
#include "respout.h"
#include "verbsint.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* Symfony's PROBE and RELEASE bodies, byte for byte (test/symfony_lock.py) */
static const char probe_body[] =
	"\n                local now = redis.call(\"TIME\")\n                "
	"redis.call(\"SET\", KEYS[1], \"1\", \"PX\", 1)\n\n\t            return 1\n"
	"            ";
static const char release_body[] =
	"\n            local key = KEYS[1]\n            local uniqueToken = ARGV[1]\n\n"
	"            -- asserts the KEY is compatible with current version (old Symfony "
	"<5.2 BC)\n            if redis.call(\"TYPE\", key).ok == \"string\" then\n"
	"                return false\n            end\n\n            -- lock not already "
	"acquired\n            if not redis.call(\"ZSCORE\", key, uniqueToken) then\n"
	"                return false\n            end\n\n            redis.call(\"ZREM\", "
	"key, uniqueToken)\n            redis.call(\"ZREM\", key, \"__write__\")\n\n"
	"            local maxExpiration = redis.call(\"ZREVRANGE\", key, 0, 0, "
	"\"WITHSCORES\")[2]\n            if nil ~= maxExpiration then\n"
	"                redis.call(\"PEXPIREAT\", key, maxExpiration)\n            end\n\n"
	"            return true\n        ";

#define SHA_PROBE        "c481d49b4a48e48a9eeacd5d51655b5fcbb19925"
#define SHA_SAVE         "6fdee501a3714553b74c0d67687b921b90b05d18"
#define SHA_SAVE_ARGV    "abdbb1413a25737fc4cfb9cf79ace1a22dad6ab3"
#define SHA_EXISTS_ARGV  "e27f94ac94f5d96f23f2e8697462233b6349a566"

/* ---- the stubs: the lock core and the texts verbs.c owns ---------------- */
const char pc_wrongtype_msg[] =
	"WRONGTYPE Operation against a key holding the wrong kind of value";
const char slock_gate_fmt[] = "the Symfony lock scripts need every node in "
	"the fleet on 0.5.0 or later - node %d is older";
const char slock_mode_msg[] = "the Symfony lock scripts need a shard or eager "
	"collection in a cluster: this collection has no node that decides a key";
const char *wr_refused_msg(void) { return "node is FAILED"; }

static struct {
	int calls, script;
	long long now_arg, ttl;
	char tok[32], key[32];
} got;
static int core_rc, core_res, core_lacking;

int op_slock(pcache_htable_t *ht, str *k, int script, long long now_arg,
		long long ttl_ms, const char *tok, size_t toklen, int *result,
		unsigned int *park, int *lacking)
{
	(void)ht; (void)park;
	got.calls++;
	got.script = script;
	got.now_arg = now_arg;
	got.ttl = ttl_ms;
	snprintf(got.tok, sizeof got.tok, "%.*s", tok ? (int)toklen : 0, tok ? tok : "");
	snprintf(got.key, sizeof got.key, "%.*s", k->len, k->s);
	*result = core_res;
	*lacking = core_lacking;
	return core_rc;
}

/* ---- the harness ------------------------------------------------------- */
static char outbuf[4096];
static struct pc_jw out;
static const char *keyp;
static size_t klen;

static void run(int (*h)(struct resp_ctx *), const char *const *args)
{
	char *argv[8];
	size_t argl[8];
	unsigned int park = 0;
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
	c.keyp = &keyp;
	c.klenp = &klen;
	c.park_req = &park;
	keyp = NULL;
	klen = 0;
	memset(&got, 0, sizeof got);
	pc_jw_init(&out, outbuf, sizeof outbuf);
	h(&c);
}
#define RUN(h, ...) run((h), (const char *const[]){ __VA_ARGS__, NULL })

static int is(const char *want)
{
	return out.len == strlen(want) && !memcmp(out.buf, want, out.len);
}

static void core(int rc, int res, int lacking)
{
	core_rc = rc;
	core_res = res;
	core_lacking = lacking;
}

int main(void)
{
	static char longkey[KEY_MAX + 2];

	/* A */
	core(PC_OP_OK, 1, 0);
	RUN(rh_eval, "EVAL", probe_body, "1", "lk");
	CHK(is(":1\r\n") && got.script == PC_SLOCK_PROBE && got.now_arg == -1 &&
		!strcmp(got.key, "lk"), "A: EVAL of Symfony's PROBE body - run as C, true is :1");
	CHK(keyp && klen == 2 && !memcmp(keyp, "lk", 2), "A: the key noted for the door");
	core(PC_OP_OK, 0, 0);
	RUN(rh_eval, "EVAL", release_body, "1", "lk", "tok1");
	CHK(is("$-1\r\n") && got.script == PC_SLOCK_RELEASE && !strcmp(got.tok, "tok1"),
		"A: RELEASE with its token (ARGV[1]) - false is nil");
	RUN(rh_eval, "EVAL", "return 1", "1", "lk");
	CHK(is("-ERR script not approved on this server: perfcached runs only the "
		"Symfony Lock scripts it knows\r\n") && got.calls == 0, "A: any other body refused by name");

	/* B */
	core(PC_OP_OK, 1, 0);
	RUN(rh_eval, "EVALSHA", SHA_SAVE, "1", "lk", "1700000000.5", "tok", "30000");
	CHK(is(":1\r\n") && got.script == PC_SLOCK_SAVE && !strcmp(got.tok, "tok") &&
		got.ttl == 30000 && got.now_arg == -1, "B: EVALSHA SAVE - the token, the ttl, the clock's now");
	RUN(rh_eval, "EVALSHA", "6FDEE501A3714553B74C0D67687B921B90B05D18", "1", "lk", "1", "tok", "1");
	CHK(got.calls == 1 && got.script == PC_SLOCK_SAVE, "B: the name in upper case too");
	RUN(rh_eval, "EVALSHA", SHA_SAVE_ARGV, "1", "lk", "1700000000.5", "tok", "30000");
	CHK(got.script == PC_SLOCK_SAVE && got.now_arg == 1700000000500LL,
		"B: the _argv variant - now from ARGV[1], 1700000000.5 s = 1700000000500 ms");
	RUN(rh_eval, "EVALSHA", SHA_EXISTS_ARGV, "1", "lk", "2.0009", "tok");
	CHK(got.script == PC_SLOCK_EXISTS && got.now_arg == 2000 && got.ttl == 0 &&
		!strcmp(got.tok, "tok"), "B: EXISTS reads no ttl; 2.0009 s floors to 2000 ms");
	RUN(rh_eval, "EVALSHA", SHA_SAVE_ARGV, "1", "lk", "x", "tok", "1");
	CHK(is("-ERR ARGV[1] is not a time\r\n") && got.calls == 0, "B: ARGV[1] that is not a time");
	RUN(rh_eval, "EVALSHA", SHA_SAVE_ARGV, "1", "lk", "-1", "tok", "1");
	CHK(is("-ERR ARGV[1] is not a time\r\n"), "B: nor a negative one");
	RUN(rh_eval, "EVALSHA", SHA_SAVE, "1", "lk", "1", "tok", "1.5");
	CHK(is("-ERR the lock ttl (ARGV[3]) must be a whole number of milliseconds\r\n") &&
		got.calls == 0, "B: a ttl that is not whole milliseconds");
	RUN(rh_eval, "EVALSHA", "0000000000000000000000000000000000000000", "1", "lk");
	CHK(is("-NOSCRIPT No matching script. Please use EVAL.\r\n"), "B: an unknown name - NOSCRIPT");

	/* C */
	RUN(rh_eval, "EVALSHA", SHA_PROBE, "x", "lk");
	CHK(is("-ERR Number of keys can't be greater than number of args\r\n"), "C: numkeys not a number");
	RUN(rh_eval, "EVALSHA", SHA_PROBE, "2", "lk");
	CHK(is("-ERR Number of keys can't be greater than number of args\r\n"), "C: numkeys past the arguments");
	RUN(rh_eval, "EVALSHA", SHA_PROBE, "2", "k1", "k2");
	CHK(is("-ERR this approved script takes exactly 1 key\r\n"), "C: two keys");
	RUN(rh_eval, "EVALSHA", SHA_SAVE, "1", "lk", "1", "tok");
	CHK(is("-ERR too few arguments for this approved script\r\n"), "C: SAVE with two ARGV of three");
	RUN(rh_eval, "EVAL", probe_body, "1");
	CHK(is("-ERR this approved script takes exactly 1 key\r\n"), "C: no key");
	memset(longkey, 'k', KEY_MAX + 1);
	RUN(rh_eval, "EVALSHA", SHA_PROBE, "1", longkey);
	CHK(is("-ERR key too long\r\n") && got.calls == 0, "C: a key over KEY_MAX");
	RUN(rh_eval, "EVAL", probe_body);
	CHK(is("-ERR wrong number of arguments for 'eval' command\r\n"), "C: EVAL's arity");
	RUN(rh_eval, "EVALSHA", SHA_PROBE);
	CHK(is("-ERR wrong number of arguments for 'evalsha' command\r\n"), "C: EVALSHA's");

	/* D */
	core(PC_OP_PARKED, 0, 0);
	RUN(rh_eval, "EVALSHA", SHA_PROBE, "1", "lk");
	CHK(is("-ERR busy\r\n"), "D: parked - the fallback");
	core(PC_OP_ERR_GATE, 0, 3);
	RUN(rh_eval, "EVALSHA", SHA_PROBE, "1", "lk");
	CHK(is("-ERR the Symfony lock scripts need every node in the fleet on 0.5.0 or later - "
		"node 3 is older\r\n"), "D: the version gate names the node");
	core(PC_OP_ERR_MODE, 0, 0);
	RUN(rh_eval, "EVALSHA", SHA_PROBE, "1", "lk");
	CHK(is("-ERR the Symfony lock scripts need a shard or eager collection in a cluster: "
		"this collection has no node that decides a key\r\n"), "D: the collection mode");
	core(PC_OP_ERR_TYPE, 0, 0);
	RUN(rh_eval, "EVALSHA", SHA_PROBE, "1", "lk");
	CHK(is("-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"), "D: WRONGTYPE");
	core(PC_OP_ERR_BUSY, 0, 0);
	RUN(rh_eval, "EVALSHA", SHA_PROBE, "1", "lk");
	CHK(is("-TRYAGAIN cluster busy, retry\r\n"), "D: a busy fleet - TRYAGAIN");
	core(PC_OP_ERR_FWD, 0, 0);
	RUN(rh_eval, "EVALSHA", SHA_PROBE, "1", "lk");
	CHK(is("-ERR forward failed\r\n"), "D: forward failed");
	core(PC_OP_ERR_WRFAIL, 0, 0);
	RUN(rh_eval, "EVALSHA", SHA_PROBE, "1", "lk");
	CHK(is("-ERR node is FAILED\r\n"), "D: a write refusal - the gate's own text");
	core(PC_OP_ERR_2BIG, 0, 0);
	RUN(rh_eval, "EVALSHA", SHA_PROBE, "1", "lk");
	CHK(is("-ERR lock token too long\r\n"), "D: a token too long");
	core(PC_OP_ERR_FULL, 0, 0);
	RUN(rh_eval, "EVALSHA", SHA_PROBE, "1", "lk");
	CHK(is("-ERR lock script failed\r\n"), "D: anything else");

	/* E */
	RUN(rh_script, "SCRIPT", "LOAD", probe_body);
	CHK(is("$40\r\n" SHA_PROBE "\r\n"), "E: SCRIPT LOAD of PROBE - its SHA");
	RUN(rh_script, "SCRIPT", "load", "return 1");
	CHK(is("-ERR script not approved on this server: perfcached runs only the "
		"Symfony Lock scripts it knows\r\n"), "E: any other body refused");
	RUN(rh_script, "SCRIPT", "EXISTS", SHA_PROBE, "ffffffffffffffffffffffffffffffffffffffff", "c481");
	CHK(is("*3\r\n:1\r\n:0\r\n:0\r\n"), "E: EXISTS - one per name");
	RUN(rh_script, "SCRIPT", "FLUSH");
	CHK(is("+OK\r\n"), "E: FLUSH - OK, nothing is cached");
	RUN(rh_script, "SCRIPT", "FLUSH", "ASYNC");
	CHK(is("+OK\r\n"), "E: FLUSH ASYNC");
	RUN(rh_script, "SCRIPT", "KILL");
	CHK(is("-ERR unknown SCRIPT subcommand or wrong number of arguments\r\n"), "E: KILL refused");
	RUN(rh_script, "SCRIPT", "LOAD");
	CHK(is("-ERR unknown SCRIPT subcommand or wrong number of arguments\r\n"), "E: LOAD without a body");
	RUN(rh_script, "SCRIPT");
	CHK(is("-ERR wrong number of arguments for 'script' command\r\n"), "E: the arity");

	printf("respscripttest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
