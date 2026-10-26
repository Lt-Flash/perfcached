/* respjsontest.c - RedisJSON on the RESP door (respjson.c; the 0.7.2 split,
 * wave 1): the JSON core (pc_json_rmw) and the placement are stubbed, the
 * JSON validator is the real one; every reply byte for byte.
 *
 *   A  the path's spellings: none (the document, bare), `$...` (v2, an
 *      array-shaped reply), `.x`, `x`, `.` and `[i]` (legacy, bare); an
 *      empty or a too-long path refused; the key too long; the key noted
 *   B  JSON.SET: NX, XX and EX passed to the core; EX 0 and an unknown
 *      option refused; the arity.  NX on a present leaf, XX on an absent
 *      one or key, answer nil; the wrong-type text in Redis's words, a
 *      stored value that is not JSON as WRONGTYPE, any other error as -ERR
 *   C  JSON.GET's answers: a missing key nil; a missing path [] on a `$`
 *      path and an error on a legacy one; a whole value that is not JSON
 *      WRONGTYPE
 *   D  JSON.NUMINCRBY: the new value as [n] or n; a non-integer refused
 *   E  JSON.ARRAPPEND: one core call per value, the last count answered (in
 *      an array on a `$` path); an error stops it
 *   F  JSON.DEL: the root of a document through the delete core (1, then
 *      0); the root of a plain string under strict types is WRONGTYPE; a
 *      path through the JSON core
 *   G  a key another node holds - a proxy non-holder, a shard non-owner - is
 *      refused by name, the core never called
 *   H  JSON.DEBUG: HELP; MEMORY's size, 0 for a missing key, WRONGTYPE for a
 *      value that is not JSON; anything else refused
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"
#include "jsonpath.h"
#include "store.h"
#include "core/pcache_htable.h"
#include "cluster.h"
#include "verbs.h"
#include "respout.h"
#include "verbsint.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* ---- the stubs: the JSON core, the delete core, placement -------------- */
const char pc_json_wrongtype_msg[] = "WRONGTYPE Operation against a key "
	"holding the wrong kind of value";

const char *jp_strerror(int rc)
{
	switch (rc) {
	case PC_JP_E_DOC:     return "stored value is not JSON";
	case PC_JP_E_NOLEAF:  return "no value at path";
	case PC_JP_E_EXISTS:  return "value exists (nx)";
	default:              return "json path error";
	}
}

const char *col_name_of(pcache_htable_t *ht) { (void)ht; return "c"; }

/* what the last core call was given, and what the next one answers */
static struct {
	int calls, op, nx, xx, have_ttl;
	char path[600], val[64];
	long long by, ttl;
} got;
static struct {
	int rc, cnt;                     /* rc: 0 ok, 1 absent, -1 error */
	const char *frag, *msg;
	long long nv;
	int fail_at;                     /* ARRAPPEND: the call that errs (1-based) */
} ans;
static char fragbuf[64];
static int dels, del_rc, cl_on, proxy_on, shard_on, probe_rc, owner, strict = 1;
static unsigned char flags;

int pc_json_rmw(pcache_htable_t *ht, const char *key, int klen, int op,
		const char *path, int plen, const char *val, int vlen,
		long long by, int have_ttl, long long ttl, int nx, int xx,
		int mkpath, char **frag_out, int *fraglen_out,
		long long *newval, int *count_out, const char **errmsg)
{
	(void)ht; (void)key; (void)klen; (void)mkpath;
	got.calls++;
	got.op = op;
	snprintf(got.path, sizeof got.path, "%.*s", plen, path);
	snprintf(got.val, sizeof got.val, "%.*s", val ? vlen : 0, val ? val : "");
	got.by = by; got.have_ttl = have_ttl; got.ttl = ttl;
	got.nx = nx; got.xx = xx;
	if (ans.fail_at && got.calls == ans.fail_at) {
		*errmsg = "array append failed";
		return -1;
	}
	if (ans.rc < 0) {
		*errmsg = ans.msg;
		return -1;
	}
	if (ans.frag) {
		snprintf(fragbuf, sizeof fragbuf, "%s", ans.frag);
		*frag_out = fragbuf;             /* borrowed, as the core's */
		*fraglen_out = (int)strlen(fragbuf);
	}
	*newval = ans.nv;
	*count_out = ans.cnt ? ans.cnt : got.calls;
	return ans.rc;
}

int op_del(pcache_htable_t *ht, str *k, unsigned int *park)
{
	(void)ht; (void)k; (void)park;
	dels++;
	return del_rc;
}

int pc_cluster_enabled(void) { return cl_on; }
int pc_store_proxy_enabled(pcache_htable_t *ht) { (void)ht; return proxy_on; }
int pc_store_shard_enabled(pcache_htable_t *ht) { (void)ht; return shard_on; }
int pc_store_types_strict(pcache_htable_t *ht) { (void)ht; return strict; }

int pcache_ht_probe(pcache_htable_t *ht, const str *key, unsigned int *vlen,
		unsigned int *expires, int *is_counter)
{
	(void)ht; (void)key; (void)vlen; (void)expires; (void)is_counter;
	return probe_rc;
}

int pc_shard_owner(const char *col, size_t collen, const char *key, size_t klen)
{
	(void)col; (void)collen; (void)key; (void)klen;
	return owner;
}

int pcache_ht_getflags(pcache_htable_t *ht, const str *key, unsigned char *rflags)
{
	(void)ht; (void)key;
	*rflags = flags;
	return 0;
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

static void answer(int rc, const char *frag, const char *msg)
{
	memset(&ans, 0, sizeof ans);
	ans.rc = rc;
	ans.frag = frag;
	ans.msg = msg;
}

static int is(const char *want)
{
	return out.len == strlen(want) && !memcmp(out.buf, want, out.len);
}

int main(void)
{
	static char longpath[600], longkey[KEY_MAX + 2];

	/* A */
	answer(0, "{\"a\":1}", NULL);
	RUN(rh_json, "JSON.GET", "k");
	CHK(is("$7\r\n{\"a\":1}\r\n") && got.op == PC_JOP_GET && !strcmp(got.path, "$"),
		"A: JSON.GET k - the root, the document bare");
	CHK(keyp && klen == 1 && keyp[0] == 'k', "A: the key noted for the door");
	answer(0, "1", NULL);
	RUN(rh_json, "JSON.GET", "k", "$.a");
	CHK(is("$3\r\n[1]\r\n") && !strcmp(got.path, "$.a"), "A: $.a - v2, the value in an array");
	RUN(rh_json, "JSON.GET", "k", ".a");
	CHK(is("$1\r\n1\r\n") && !strcmp(got.path, "$.a"), "A: .a - legacy, bare");
	RUN(rh_json, "JSON.GET", "k", "a");
	CHK(is("$1\r\n1\r\n") && !strcmp(got.path, "$.a"), "A: a - legacy, as $.a");
	RUN(rh_json, "JSON.GET", "k", "[0]");
	CHK(is("$1\r\n1\r\n") && !strcmp(got.path, "$[0]"), "A: [0] - legacy, as $[0]");
	answer(0, "{}", NULL);
	RUN(rh_json, "JSON.GET", "k", ".");
	CHK(is("$2\r\n{}\r\n") && !strcmp(got.path, "$"), "A: . - the root, bare");
	RUN(rh_json, "JSON.GET", "k", "");
	CHK(is("-ERR bad path\r\n") && got.calls == 0, "A: an empty path refused");
	memset(longpath, 'p', JP_NAME_PARAM - 2);
	RUN(rh_json, "JSON.GET", "k", longpath);
	CHK(is("-ERR bad path\r\n") && got.calls == 0, "A: a path of %d bytes refused (no room for $.)",
		JP_NAME_PARAM - 2);
	memset(longpath, 0, sizeof longpath);
	memset(longpath, 'p', JP_NAME_PARAM - 3);
	RUN(rh_json, "JSON.GET", "k", longpath);
	CHK(got.calls == 1 && strlen(got.path) == JP_NAME_PARAM - 1, "A: one byte shorter is read");
	memset(longkey, 'k', KEY_MAX + 1);
	RUN(rh_json, "JSON.GET", longkey);
	CHK(is("-ERR key too long\r\n"), "A: a key over KEY_MAX refused");

	/* B */
	answer(0, NULL, NULL);
	RUN(rh_json, "JSON.SET", "k", "$", "{\"a\":1}", "NX");
	CHK(is("+OK\r\n") && got.op == PC_JOP_SET && got.nx && !got.xx &&
		!strcmp(got.val, "{\"a\":1}"), "B: JSON.SET k $ doc NX - OK, NX passed");
	RUN(rh_json, "JSON.SET", "k", "$.a", "2", "xx", "EX", "10");
	CHK(is("+OK\r\n") && got.xx && got.have_ttl && got.ttl == 10,
		"B: XX and EX 10 passed (options in any case)");
	RUN(rh_json, "JSON.SET", "k", "$", "1", "EX", "0");
	CHK(is("-ERR invalid expire time in 'json.set' command\r\n") && got.calls == 0, "B: EX 0 refused");
	RUN(rh_json, "JSON.SET", "k", "$", "1", "KEEPTTL");
	CHK(is("-ERR unsupported JSON.SET option\r\n"), "B: an unknown option refused");
	RUN(rh_json, "JSON.SET", "k", "$");
	CHK(is("-ERR wrong number of arguments for this JSON command\r\n"), "B: JSON.SET's arity");
	answer(-1, NULL, jp_strerror(PC_JP_E_EXISTS));
	RUN(rh_json, "JSON.SET", "k", "$.a", "1", "NX");
	CHK(is("$-1\r\n"), "B: NX on a present leaf - nil, as RedisJSON");
	answer(-1, NULL, jp_strerror(PC_JP_E_NOLEAF));
	RUN(rh_json, "JSON.SET", "k", "$.a", "1", "XX");
	CHK(is("$-1\r\n"), "B: XX on an absent leaf - nil");
	answer(-1, NULL, "no such key");
	RUN(rh_json, "JSON.SET", "k", "$.a", "1", "XX");
	CHK(is("$-1\r\n"), "B: XX on an absent key - nil");
	answer(-1, NULL, jp_strerror(PC_JP_E_EXISTS));
	RUN(rh_json, "JSON.SET", "k", "$.a", "1");
	CHK(is("-ERR value exists (nx)\r\n"), "B: the same error without NX is an error");
	answer(-1, NULL, pc_json_wrongtype_msg);
	RUN(rh_json, "JSON.SET", "k", "$", "1");
	CHK(is("-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"),
		"B: the wrong type in Redis's own words (S280)");
	answer(-1, NULL, jp_strerror(PC_JP_E_DOC));
	RUN(rh_json, "JSON.SET", "k", "$.a", "1");
	CHK(is("-WRONGTYPE the value at this key is not a JSON document\r\n"),
		"B: a stored value that is not JSON - WRONGTYPE");

	/* C */
	answer(1, NULL, NULL);
	RUN(rh_json, "JSON.GET", "k");
	CHK(is("$-1\r\n"), "C: a missing key - nil");
	RUN(rh_json, "JSON.GET", "k", "$.x");
	CHK(is("$2\r\n[]\r\n"), "C: a missing path on $ - []");
	RUN(rh_json, "JSON.GET", "k", ".x");
	CHK(is("-ERR path does not exist\r\n"), "C: a missing path, legacy - an error");
	answer(0, "hello", NULL);
	RUN(rh_json, "JSON.GET", "k");
	CHK(is("-WRONGTYPE the value at this key is not a JSON document\r\n"),
		"C: a whole value that is not JSON - WRONGTYPE");

	/* D */
	answer(0, NULL, NULL);
	ans.nv = 7;
	RUN(rh_json, "JSON.NUMINCRBY", "k", "$.n", "5");
	CHK(is("$3\r\n[7]\r\n") && got.op == PC_JOP_INCR && got.by == 5, "D: NUMINCRBY $.n 5 - [7]");
	RUN(rh_json, "JSON.NUMINCRBY", "k", ".n", "5");
	CHK(is("$1\r\n7\r\n"), "D: legacy - 7");
	RUN(rh_json, "JSON.NUMINCRBY", "k", "$.n", "1.5");
	CHK(is("-ERR value is not an integer (JSON.NUMINCRBY is integer-only here)\r\n") &&
		got.calls == 0, "D: a non-integer refused");
	RUN(rh_json, "JSON.NUMINCRBY", "k", "$.n");
	CHK(is("-ERR wrong number of arguments for this JSON command\r\n"), "D: the arity");

	/* E */
	answer(0, NULL, NULL);
	RUN(rh_json, "JSON.ARRAPPEND", "k", "$.a", "1", "2", "3");
	CHK(is("*1\r\n:3\r\n") && got.calls == 3 && got.op == PC_JOP_APPEND &&
		!strcmp(got.val, "3"), "E: three values, three calls, the last count - in an array on $");
	RUN(rh_json, "JSON.ARRAPPEND", "k", ".a", "1", "2");
	CHK(is(":2\r\n"), "E: legacy - the count bare");
	ans.fail_at = 2;
	RUN(rh_json, "JSON.ARRAPPEND", "k", "$.a", "1", "2", "3");
	CHK(is("-ERR array append failed\r\n") && got.calls == 2, "E: an error stops it");

	/* F */
	answer(0, NULL, NULL);
	flags = PCACHE_F_JSON;
	dels = 0;
	del_rc = PC_OP_OK;
	RUN(rh_json, "JSON.DEL", "k");
	CHK(is(":1\r\n") && dels == 1 && got.calls == 0, "F: the root of a document - the delete core, 1");
	del_rc = PC_OP_ABSENT;
	RUN(rh_json, "JSON.DEL", "k", "$");
	CHK(is(":0\r\n") && dels == 2, "F: again - 0");
	flags = 0;
	RUN(rh_json, "JSON.DEL", "k");
	CHK(is("-WRONGTYPE Operation against a key holding the wrong kind of value\r\n") &&
		dels == 2, "F: the root of a plain string under strict types - WRONGTYPE (S280)");
	strict = 0;
	RUN(rh_json, "JSON.DEL", "k");
	CHK(dels == 3, "F: under loose types it is deleted");
	strict = 1;
	RUN(rh_json, "JSON.DEL", "k", "$.a");
	CHK(is(":1\r\n") && got.op == PC_JOP_DEL && dels == 3, "F: a path - the JSON core, 1");
	answer(1, NULL, NULL);
	RUN(rh_json, "JSON.DEL", "k", "$.a");
	CHK(is(":0\r\n"), "F: an absent path - 0");

	/* G */
	answer(0, "1", NULL);
	cl_on = 1;
	proxy_on = 1;
	probe_rc = -1;
	RUN(rh_json, "JSON.GET", "k");
	CHK(is("-ERR JSON on a key another node holds is not served on the RESP door - "
		"dial that node, or use the native door\r\n") && got.calls == 0,
		"G: a proxy key this node does not hold - refused by name");
	probe_rc = 0;
	RUN(rh_json, "JSON.GET", "k", "$");
	CHK(is("$3\r\n[1]\r\n"), "G: one it holds is served");
	proxy_on = 0;
	shard_on = 1;
	owner = 2;
	RUN(rh_json, "JSON.SET", "k", "$", "1");
	CHK(got.calls == 0 && out.buf[0] == '-', "G: a shard key another node owns - refused");
	owner = 0;
	RUN(rh_json, "JSON.SET", "k", "$", "1");
	CHK(is("+OK\r\n"), "G: one this node owns is served");
	cl_on = shard_on = 0;

	/* H */
	RUN(rh_json_debug, "JSON.DEBUG", "help");
	CHK(is("*2\r\n$53\r\nJSON.DEBUG MEMORY <key> [path] - reports memory usage\r\n"
		"$30\r\nJSON.DEBUG HELP - this message\r\n"), "H: HELP");
	answer(0, "{\"a\":[1,2]}", NULL);
	RUN(rh_json_debug, "JSON.DEBUG", "MEMORY", "k");
	CHK(is(":11\r\n") && got.op == PC_JOP_GET && !strcmp(got.path, "$"), "H: MEMORY k - 11 bytes");
	answer(1, NULL, NULL);
	RUN(rh_json_debug, "JSON.DEBUG", "MEMORY", "k", "$.a");
	CHK(is(":0\r\n") && !strcmp(got.path, "$.a"), "H: a missing key or path - 0");
	answer(0, "hello", NULL);
	RUN(rh_json_debug, "JSON.DEBUG", "MEMORY", "k");
	CHK(is("-WRONGTYPE the value at this key is not a JSON document\r\n"), "H: a value that is not JSON");
	RUN(rh_json_debug, "JSON.DEBUG", "BOGUS");
	CHK(is("-ERR JSON.DEBUG: use HELP or MEMORY <key> [path]\r\n"), "H: anything else refused");

	printf("respjsontest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
