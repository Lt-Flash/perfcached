/* respkvtest.c - the RESP door's key-value commands (respkv.c; the 0.7.2
 * split, wave 1) over a fake store: the data cores verbs.c owns are stubbed
 * here (a small table, a fixed clock), so what is tested is the RESP side -
 * the arguments, the options and every reply's bytes.
 *
 *   A  GET: a hit, a miss, WRONGTYPE in Redis's words, a failed read, the
 *      arity
 *   B  SET: plain; EX and PX (rounded up to seconds); a bad or missing
 *      expiry; NX / XX (nil when it does not hold); NX with XX, and IFEQ
 *      with NX, a syntax error; IFEQ to the compare core with its operand;
 *      IFDEQ refused by name; KEEPTTL and the other options refused, never
 *      half-honoured; SETEX / PSETEX and a zero expiry; a full cache, a busy
 *      fleet as -TRYAGAIN; the arity
 *   C  DEL one key (1, then 0), several counted
 *   D  EXISTS counts; TYPE of a string, a document and nothing; TTL -2 for
 *      none, -1 for no expiry, seconds left, PTTL in milliseconds
 *   E  MGET: values, nil for a miss, nil for a document (Redis answers nil
 *      there, not an error); G8, where a miss may be looked up elsewhere
 *      (the fan-out core stubbed): its answer written as it renders it, a
 *      refusal in its code word, a waiting mget parked with nothing
 *      written; and a context with no connection reads locally as before
 *   F  INCR / INCRBY / DECR / DECRBY: the running value, the expiry left
 *      alone; an amount that is not a number, or missing, in Redis's words;
 *      the arity; a value that is not a number; WRONGTYPE; TRYAGAIN
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

/* ---- the fake store ----------------------------------------------------- */
struct ent { char k[32]; char v[64]; int vlen; long long ttl; unsigned char fl; int used; };
static struct ent tab[32];
static long long last_ttl;
static int last_cmp_kind;
static char last_cmp[32];

#define NOW 1000u                      /* the fixed clock, in ticks (s) */
unsigned int compat_ticks(void) { return NOW; }

static struct ent *find(const str *k)
{
	int i;

	for (i = 0; i < 32; i++)
		if (tab[i].used && (int)strlen(tab[i].k) == k->len &&
		    !memcmp(tab[i].k, k->s, (size_t)k->len))
			return &tab[i];
	return NULL;
}

static void put(const char *k, const char *v, long long ttl, unsigned char fl)
{
	str ks = { (char *)k, (int)strlen(k) };
	struct ent *e = find(&ks);
	int i;

	for (i = 0; !e && i < 32; i++)
		if (!tab[i].used)
			e = &tab[i];
	memset(e, 0, sizeof *e);
	e->used = 1;
	snprintf(e->k, sizeof e->k, "%s", k);
	e->vlen = (int)strlen(v);
	memcpy(e->v, v, (size_t)e->vlen);
	e->ttl = ttl;
	e->fl = fl;
}

int op_get_buf(pcache_htable_t *ht, str *k, char *buf, unsigned int cap,
		unsigned int *vlen, unsigned int *exp, unsigned int *park)
{
	struct ent *e = find(k);

	(void)ht; (void)cap; (void)park;
	if (k->len == 9 && !memcmp(k->s, "wrongtype", 9))
		return PC_OP_ERR_TYPE;
	if (k->len == 4 && !memcmp(k->s, "boom", 4))
		return PC_OP_ERR_GET;
	if (!e)
		return PC_OP_ABSENT;
	memcpy(buf, e->v, (size_t)e->vlen);
	*vlen = (unsigned int)e->vlen;
	*exp = e->ttl ? NOW + (unsigned int)e->ttl : 0;
	return PC_OP_OK;
}

int op_set(pcache_htable_t *ht, str *k, str *v, long long ttl,
		unsigned int *park, int probed)
{
	char kz[32], vz[64];

	(void)ht; (void)park; (void)probed;
	if (k->len == 4 && !memcmp(k->s, "full", 4))
		return PC_OP_ERR_FULL;
	if (k->len == 4 && !memcmp(k->s, "busy", 4))
		return PC_OP_ERR_BUSY;
	snprintf(kz, sizeof kz, "%.*s", k->len, k->s);
	snprintf(vz, sizeof vz, "%.*s", v->len, v->s);
	put(kz, vz, ttl, 0);
	last_ttl = ttl;
	return PC_OP_OK;
}

int op_setcond(pcache_htable_t *ht, str *k, str *v, long long ttl, int cond,
		unsigned int *park)
{
	int there = find(k) != NULL;

	if ((cond == PC_SETCOND_NX && there) || (cond == PC_SETCOND_XX && !there))
		return PC_OP_ABSENT;
	return op_set(ht, k, v, ttl, park, 0);
}

int op_cmp(pcache_htable_t *ht, str *k, const char *cmp, size_t cmplen,
		str *v, long long ttl, int kind, unsigned int *park)
{
	(void)ht; (void)k; (void)v; (void)ttl; (void)park;
	last_cmp_kind = kind;
	snprintf(last_cmp, sizeof last_cmp, "%.*s", (int)cmplen, cmp);
	return PC_OP_OK;
}

int op_del(pcache_htable_t *ht, str *k, unsigned int *park)
{
	struct ent *e = find(k);

	(void)ht; (void)park;
	if (!e)
		return PC_OP_ABSENT;
	e->used = 0;
	return PC_OP_OK;
}

int op_expire(pcache_htable_t *ht, str *k, long long ttl, unsigned int *park)
{
	struct ent *e = find(k);

	(void)ht; (void)park;
	if (!e)
		return PC_OP_ABSENT;
	e->ttl = ttl;
	return PC_OP_OK;
}

int op_rlhit(pcache_htable_t *ht, str *k, long long window_ms, long long limit,
		long long *count, int *allowed, unsigned int *park, int *lacking)
{
	(void)ht; (void)k; (void)window_ms; (void)limit; (void)park; (void)lacking;
	*count = 1;
	*allowed = 1;
	return PC_OP_OK;
}

int probe_or_present(pcache_htable_t *ht, const str *k, unsigned int *vln,
		unsigned int *exp, int *is_ctr, unsigned char *tflags)
{
	struct ent *e = find(k);

	(void)ht;
	if (!e)
		return -2;
	if (vln)
		*vln = (unsigned int)e->vlen;
	if (exp)
		*exp = e->ttl ? NOW + (unsigned int)e->ttl : 0;
	if (is_ctr)
		*is_ctr = 0;
	if (tflags)
		*tflags = e->fl;
	return 0;
}

int pcache_ht_fetch_ex(pcache_htable_t *ht, const str *key, str *val,
		unsigned int *expires, unsigned char *rflags)
{
	struct ent *e = find(key);

	(void)ht;
	if (!e)
		return -1;
	val->s = malloc((size_t)e->vlen + 1);
	memcpy(val->s, e->v, (size_t)e->vlen);
	val->len = e->vlen;
	*expires = 0;
	*rflags = e->fl;
	return 0;
}

int pcache_ht_getflags(pcache_htable_t *ht, const str *key, unsigned char *rflags)
{
	struct ent *e = find(key);

	(void)ht;
	if (!e)
		return -1;
	*rflags = e->fl;
	return 0;
}

int pcache_ht_probe_present(pcache_htable_t *ht, const str *key,
		unsigned int *expires, unsigned char *tflags)
{
	(void)ht; (void)key; (void)expires; (void)tflags;
	return -2;
}

static long long last_addsub_ttl;

int op_addsub(pcache_htable_t *ht, str *k, long long by, long long ttl,
		long long *nv, unsigned int *park, int probed)
{
	struct ent *e = find(k);
	long long cur = 0;
	char kz[32], vz[32];

	(void)ht; (void)park; (void)probed;
	last_addsub_ttl = ttl;
	if (k->len == 9 && !memcmp(k->s, "wrongtype", 9))
		return PC_OP_ERR_TYPE;
	if (k->len == 4 && !memcmp(k->s, "busy", 4))
		return PC_OP_ERR_BUSY;
	if (e) {
		char tmp[65], *end;

		memcpy(tmp, e->v, (size_t)e->vlen);
		tmp[e->vlen] = 0;
		cur = strtoll(tmp, &end, 10);
		if (!e->vlen || *end)
			return PC_OP_ERR_NOTINT;
	}
	*nv = cur + by;
	snprintf(kz, sizeof kz, "%.*s", k->len, k->s);
	snprintf(vz, sizeof vz, "%lld", *nv);
	put(kz, vz, e ? e->ttl : 0, 0);
	return PC_OP_OK;
}

/* ---- the rest respkv.c reaches: inert ----------------------------------- */
const char pc_wrongtype_msg[] =
	"WRONGTYPE Operation against a key holding the wrong kind of value";
const char cmp_mode_msg[] = "cmp mode", delex_type_msg[] = "delex type",
	rl_mode_msg[] = "rl mode", setcond_mode_msg[] = "setcond mode";
static char gbuf[VAL_MAX];
char *get_buf(void) { return gbuf; }
int pc_store_types_strict(pcache_htable_t *ht) { (void)ht; return 1; }
void pc_store_hot_note(pcache_htable_t *ht, const char *k, size_t kl) { (void)ht; (void)k; (void)kl; }
pcache_htable_t *pc_store_find(const char *name, size_t nlen) { (void)name; (void)nlen; return NULL; }
int pcache_ht_probe(pcache_htable_t *ht, const str *key, unsigned int *vlen,
		unsigned int *expires, int *is_counter)
{ (void)ht; (void)key; (void)vlen; (void)expires; (void)is_counter; return -1; }
void pcache_ht_totals(pcache_htable_t *ht, pcache_ht_totals_t *out) { (void)ht; memset(out, 0, sizeof *out); }
pcache_htable_t *resp_col(struct pc_jw *out, const char *cur_col, size_t cur_collen)
{ (void)out; (void)cur_col; (void)cur_collen; return NULL; }
int resp_col_allowed(const char *name, size_t n) { (void)name; (void)n; return 1; }
size_t resp_map_index(const char *sel, size_t sl, char *out, size_t cap)
{ (void)sel; (void)sl; (void)out; (void)cap; return 0; }
const char *rl_gate_text(int lacking) { (void)lacking; return "rl gate"; }
const char *wr_refused_msg(void) { return "refused"; }
void unk_sub(const struct resp_ctx *c) { (void)c; }
void pc_obs_client_list(struct pc_jw *w, unsigned int now_ticks) { (void)w; (void)now_ticks; }
void pc_obs_conn_lib(void *row, int ver, const char *v, size_t n) { (void)row; (void)ver; (void)v; (void)n; }
void pc_obs_conn_name(void *row, const char *name, size_t nlen) { (void)row; (void)name; (void)nlen; }
void pc_obs_slowlog_get(struct pc_jw *w, int want) { (void)w; (void)want; }
long long pc_obs_slowlog_len(void) { return 0; }
void pc_obs_slowlog_reset(void) { }
void pc_conn_set_rt_fetcher(void *conn) { (void)conn; }
long long pc_stats_reset(void) { return 0; }
int pc_pubsub_pop(void *conn, int pattern, char *name, size_t cap, size_t *nlen, int *left)
{ (void)conn; (void)pattern; (void)name; (void)cap; (void)nlen; (void)left; return 0; }

/* G8: the fan-out core (verbs.c) and the mget (mgetfan.c), stubbed */
static int fan_on, fan_refuse, fan_waiting, fan_parked, fan_freed;
static unsigned int fan_n;
static char fan_obj;
int mget_fans(pcache_htable_t *ht) { (void)ht; return fan_on; }
int op_mget_fan(pcache_htable_t *ht, const char *const *keys,
		const size_t *klens, unsigned int n, struct pc_mget **mp,
		const char **code, const char **why)
{
	(void)ht; (void)keys; (void)klens;
	fan_n = n;
	if (fan_refuse) {
		*code = "TRYAGAIN";
		*why = "mget: busy";
		return -1;
	}
	*mp = (struct pc_mget *)&fan_obj;
	return 0;
}
unsigned int pc_mget_waiting(const struct pc_mget *m) { (void)m; return (unsigned int)fan_waiting; }
void pc_mget_park(struct pc_mget *m) { fan_parked += m == (struct pc_mget *)&fan_obj; }
void pc_mget_resp(const struct pc_mget *m, struct pc_jw *w) { (void)m; pc_jw_lit(w, "*1\r\n$2\r\nfv\r\n"); }
void pc_mget_free(struct pc_mget *m) { fan_freed += m == (struct pc_mget *)&fan_obj; }

/* ---- the harness ------------------------------------------------------- */
static char outbuf[8192];
static struct pc_jw out;
static void *run_conn;                 /* the connection a context carries */

static void run(int (*h)(struct resp_ctx *), const char *const *args)
{
	char *argv[8];
	size_t argl[8];
	const char *keyp = NULL;
	size_t klen = 0;
	unsigned int park = 0;
	struct resp_ctx c;
	int n;

	for (n = 0; args[n] && n < 8; n++) {
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
	c.conn = run_conn;
	pc_jw_init(&out, outbuf, sizeof outbuf);
	h(&c);
}
#define RUN(h, ...) run((h), (const char *const[]){ __VA_ARGS__, NULL })

static int is(const char *want)
{
	return out.len == strlen(want) && !memcmp(out.buf, want, out.len);
}

int main(void)
{
	/* A */
	RUN(rh_set, "SET", "k", "v");
	CHK(is("+OK\r\n"), "A: SET k v");
	RUN(rh_get, "GET", "k");
	CHK(is("$1\r\nv\r\n"), "A: GET k - a hit");
	RUN(rh_get, "GET", "nope");
	CHK(is("$-1\r\n"), "A: GET of a miss - nil");
	RUN(rh_get, "GET", "wrongtype");
	CHK(is("-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"),
		"A: WRONGTYPE in Redis's own words");
	RUN(rh_get, "GET", "boom");
	CHK(is("-ERR get failed\r\n"), "A: a failed read");
	RUN(rh_get, "GET");
	CHK(is("-ERR wrong number of arguments for 'get' command\r\n"), "A: GET's arity");

	/* B */
	RUN(rh_set, "SET", "k2", "v", "EX", "10");
	CHK(is("+OK\r\n") && last_ttl == 10, "B: SET EX 10 - ttl 10");
	RUN(rh_set, "SET", "k3", "v", "px", "1500");
	CHK(is("+OK\r\n") && last_ttl == 2, "B: SET px 1500 - rounded up to 2 s");
	RUN(rh_set, "SET", "k", "v", "EX", "0");
	CHK(is("-ERR invalid expire time in 'set' command\r\n"), "B: EX 0 refused");
	RUN(rh_set, "SET", "k", "v", "EX");
	CHK(is("-ERR invalid expire time in 'set' command\r\n"), "B: EX without a time refused");
	RUN(rh_set, "SET", "k", "w", "NX");
	CHK(is("$-1\r\n"), "B: NX on an existing key - nil");
	RUN(rh_set, "SET", "fresh", "w", "NX");
	CHK(is("+OK\r\n"), "B: NX on a new key - OK");
	RUN(rh_set, "SET", "absent", "w", "XX");
	CHK(is("$-1\r\n"), "B: XX on a missing key - nil");
	RUN(rh_set, "SET", "k", "w", "NX", "XX");
	CHK(is("-ERR syntax error\r\n"), "B: NX with XX - a syntax error");
	RUN(rh_set, "SET", "k", "w", "IFEQ", "old");
	CHK(is("+OK\r\n") && last_cmp_kind == PC_CMP_SET_IFEQ && !strcmp(last_cmp, "old"),
		"B: IFEQ old - the compare core, with its operand");
	RUN(rh_set, "SET", "k", "w", "IFEQ", "x", "NX");
	CHK(is("-ERR syntax error\r\n"), "B: IFEQ with NX - a syntax error");
	RUN(rh_set, "SET", "k", "w", "IFDEQ", "x");
	CHK(is("-ERR IFDEQ/IFDNE are not supported: perfcached has no DIGEST - "
		"compare the value with IFEQ/IFNE\r\n"), "B: IFDEQ refused by name");
	RUN(rh_set, "SET", "k", "w", "KEEPTTL");
	CHK(is("-ERR unsupported SET option\r\n"), "B: KEEPTTL refused, not half-honoured");
	RUN(rh_set, "SETEX", "k4", "10", "v");
	CHK(is("+OK\r\n") && last_ttl == 10, "B: SETEX k4 10 v");
	RUN(rh_set, "PSETEX", "k5", "1", "v");
	CHK(is("+OK\r\n") && last_ttl == 1, "B: PSETEX 1 ms - one second");
	RUN(rh_set, "SETEX", "k6", "0", "v");
	CHK(is("-ERR invalid expire time\r\n"), "B: SETEX 0 refused");
	RUN(rh_set, "SET", "full", "v");
	CHK(is("-ERR cache full\r\n"), "B: a full cache");
	RUN(rh_set, "SET", "busy", "v");
	CHK(is("-TRYAGAIN cluster busy, retry\r\n"), "B: a busy fleet - the retryable code");
	RUN(rh_set, "SET", "k");
	CHK(is("-ERR wrong number of arguments for 'set' command\r\n"), "B: SET's arity");

	/* C */
	RUN(rh_del, "DEL", "k3");
	CHK(is(":1\r\n"), "C: DEL k3 - 1");
	RUN(rh_del, "DEL", "k3");
	CHK(is(":0\r\n"), "C: and again - 0");
	RUN(rh_del, "DEL", "k4", "k5", "nope");
	CHK(is(":2\r\n"), "C: DEL of three, two there - 2");

	/* D */
	put("a", "1", 0, 0);
	put("doc", "{}", 0, PCACHE_F_JSON);
	put("t", "x", 10, 0);
	RUN(rh_exists, "EXISTS", "a", "doc", "nope");
	CHK(is(":2\r\n"), "D: EXISTS a doc nope - 2");
	RUN(rh_type, "TYPE", "a");
	CHK(is("+string\r\n"), "D: TYPE of a string");
	RUN(rh_type, "TYPE", "doc");
	CHK(is("+ReJSON-RL\r\n"), "D: TYPE of a document");
	RUN(rh_type, "TYPE", "nope");
	CHK(is("+none\r\n"), "D: TYPE of nothing");
	RUN(rh_ttl, "TTL", "nope");
	CHK(is(":-2\r\n"), "D: TTL of nothing - -2");
	RUN(rh_ttl, "TTL", "a");
	CHK(is(":-1\r\n"), "D: TTL without an expiry - -1");
	RUN(rh_ttl, "TTL", "t");
	CHK(is(":10\r\n"), "D: TTL - the seconds left");
	RUN(rh_ttl, "PTTL", "t");
	CHK(is(":10000\r\n"), "D: PTTL - in milliseconds");

	/* E */
	RUN(rh_mget, "MGET", "a", "nope", "doc");
	CHK(is("*3\r\n$1\r\n1\r\n$-1\r\n$-1\r\n"),
		"E: MGET a nope doc - the value, nil for a miss, nil for a document");
	fan_on = 1;
	RUN(rh_mget, "MGET", "a", "nope", "doc");
	CHK(is("*3\r\n$1\r\n1\r\n$-1\r\n$-1\r\n") && !fan_n,
		"E: G8 - a context with no connection reads locally, as before");
	run_conn = &fan_obj;
	RUN(rh_mget, "MGET", "a", "nope", "doc");
	CHK(is("*1\r\n$2\r\nfv\r\n") && fan_n == 3 && fan_freed == 1 &&
		!fan_parked, "E: G8 - all answered: the core's reply written, the "
		"mget freed (%u keys)", fan_n);
	fan_waiting = 2;
	RUN(rh_mget, "MGET", "a", "nope");
	CHK(out.len == 0 && fan_parked == 1 && fan_freed == 1,
		"E: G8 - lookups waiting: parked for the door, nothing written");
	fan_waiting = 0;
	fan_refuse = 1;
	RUN(rh_mget, "MGET", "a");
	CHK(is("-TRYAGAIN mget: busy\r\n") && fan_parked == 1,
		"E: G8 - refused: the error in the core's code word");
	fan_on = fan_refuse = 0;
	run_conn = NULL;

	/* F */
	RUN(rh_incr, "INCR", "n");
	CHK(is(":1\r\n") && last_addsub_ttl == -1,
		"F: INCR of a missing key - 1, the expiry left alone (ttl -1)");
	RUN(rh_incr, "INCR", "n");
	CHK(is(":2\r\n"), "F: INCR - 2");
	RUN(rh_incr, "incrby", "n", "5");
	CHK(is(":7\r\n"), "F: INCRBY 5 - 7 (the name in any case)");
	RUN(rh_incr, "DECR", "n");
	CHK(is(":6\r\n"), "F: DECR - 6");
	RUN(rh_incr, "DECRBY", "n", "10");
	CHK(is(":-4\r\n"), "F: DECRBY 10 - -4");
	RUN(rh_incr, "INCRBY", "n", "x");
	CHK(is("-ERR value is not an integer or out of range\r\n"), "F: INCRBY x refused");
	RUN(rh_incr, "INCRBY", "n");
	CHK(is("-ERR value is not an integer or out of range\r\n"),
		"F: INCRBY without an amount - the same words");
	RUN(rh_incr, "INCR");
	CHK(is("-ERR wrong number of arguments\r\n"), "F: INCR's arity");
	RUN(rh_incr, "INCR", "n", "1");
	CHK(is("-ERR wrong number of arguments\r\n"), "F: INCR with an amount");
	put("s", "abc", 0, 0);
	RUN(rh_incr, "INCR", "s");
	CHK(is("-ERR value is not an integer or out of range\r\n"), "F: a value that is not a number");
	RUN(rh_incr, "INCR", "wrongtype");
	CHK(is("-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"),
		"F: WRONGTYPE");
	RUN(rh_incr, "INCR", "busy");
	CHK(is("-TRYAGAIN cluster busy, retry\r\n"), "F: a busy fleet - TRYAGAIN");

	printf("respkvtest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
