/* mgetaggtest.c - G8 (DESIGN 12nh): one mget waiting on several remote
 * lookups (mgetfan.c), the cluster and the store stubbed and the clock
 * faked.
 *
 *   A  an mget answered from the node alone: the native tree's shape (a
 *      hit's value and ttl, a miss), and the RESP array
 *   B  lookups: the last answer hands the mget back, not before; a found
 *      value copied with its ttl, the pull's own buffer not kept; one that
 *      timed out is unanswered - the tree says so per key, RESP answers
 *      -TRYAGAIN with the count, never values beside nils; the counters
 *   C  a found document under redis_types = strict answers as absent
 *   D  shard: an owner's miss is absent, the negative cache untouched;
 *      inside the reshard grace everyone is asked once, the retry's pull
 *      capped at the mget's deadline; with no time left nobody is asked
 *      and the key is unanswered
 *   E  proxy: a miss where the locator pointed - the locator forgotten,
 *      everyone asked; store: a miss is absent and remembered (negative
 *      cache)
 *   F  the map: two of this worker's requests on one pending slot (one
 *      expired, its successor) each find their key; an answer nobody
 *      waits on is not taken; a freed mget's waiting pulls are cancelled
 *      and their answers not taken; a closed connection drops its mgets
 *      only
 *   G  the guard: the 1,001st lookup refused (the cap), and a lookup that
 *      would leave less than a quarter of the pending table free (busy)
 *   H  the counters from the last stats reset, and from the start
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"
#include "ptree.h"
#include "cluster.h"
#include "store.h"
#include "proto.h"
#include "clpend.h"
#include "mgetfan.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* ---- the stubs ---------------------------------------------------------- */
static unsigned int now_ticks = 1000;
static long long now_ms = 50000, cap_ms, cap_seen;
static int pend_cap = 64, pend_free = 64, strict, shard, proxy, grace;
static int begins, cancels, neg_sets, loc_clears, keeps, targets[1 << 16];
static uint32_t next_req = PEND_TAG | (1u << 16) | 1, cancelled[2048];
static char fake_ht;

unsigned int compat_ticks(void) { return now_ticks; }
int pc_cluster_pend_cap(void) { return pend_cap; }
int pc_cluster_pend_free(void) { return pend_free; }
long long pc_pull_deadline_ms(const char *col, size_t collen)
{ (void)col; (void)collen; return now_ms + 300; }
long long pc_cluster_now_ms(void) { return now_ms; }
void pc_pull_cap_deadline(long long at_ms) { cap_ms = at_ms; }
void pc_pull_cancel(uint32_t req) { cancelled[cancels++ % 2048] = req; }
uint32_t pc_pull_begin(const char *col, size_t collen, const char *key,
		size_t klen)
{
	(void)col; (void)collen; (void)key; (void)klen;
	begins++;
	cap_seen = cap_ms;
	return next_req++;
}
int pc_fwd_target_of(uint32_t req, unsigned int *ttl)
{
	*ttl = 0;
	return targets[req & 0xffff];
}
pcache_htable_t *pc_store_find(const char *name, size_t nlen)
{ (void)name; (void)nlen; return (pcache_htable_t *)(void *)&fake_ht; }
int pc_store_types_strict(pcache_htable_t *ht) { (void)ht; return strict; }
int pc_store_shard_enabled(pcache_htable_t *ht) { (void)ht; return shard; }
int pc_store_proxy_enabled(pcache_htable_t *ht) { (void)ht; return proxy; }
int pc_shard_grace(void) { return grace; }
void pc_pull_found_keep(const char *col, size_t collen, const char *key,
		size_t klen, const struct pc_pull_done *d)
{ (void)col; (void)collen; (void)key; (void)klen; (void)d; keeps++; }
void pc_loc_clear(const char *col, size_t collen, const char *key, size_t klen)
{ (void)col; (void)collen; (void)key; (void)klen; loc_clears++; }
void pc_neg_set(const char *col, size_t collen, const char *key, size_t klen,
		int ms)
{ (void)col; (void)collen; (void)key; (void)klen; (void)ms; neg_sets++; }
int pc_cluster_neg_ms(void) { return 2000; }

/* ---- helpers ------------------------------------------------------------ */
static char conn_a, conn_b;
static unsigned char tbuf[1 << 16];
static char jbuf[1 << 16];

static char *dupz(const char *s)
{
	char *p = malloc(strlen(s) + 1);

	strcpy(p, s);
	return p;
}

static uint32_t wait_key(struct pc_mget *m, unsigned int i, const char *k)
{
	const char *why = NULL;
	uint32_t req = next_req++;

	if (pc_mget_wait(m, i, k, strlen(k), req, &why) != 0) {
		printf("  FAIL wait refused: %s\n", why);
		fails++;
	}
	return req;
}

/* an answer: found (val), absent, or timed out */
static struct pc_mget *answer(uint32_t req, const char *val, int timedout,
		long long type, int *taken)
{
	struct pc_pull_done d;
	struct pc_mget *done = NULL;
	char vb[64];

	memset(&d, 0, sizeof d);
	d.req = req;
	d.kind = PC_DONE_PULL;
	d.timedout = timedout;
	if (val) {
		snprintf(vb, sizeof vb, "%s", val);
		d.found = 1;
		d.val = vb;
		d.vlen = (int)strlen(val);
		d.ttl_left = 30;
		d.newval = type;
	}
	*taken = pc_mget_pull_done(&d, &done);
	if (val)
		memset(vb, 'X', sizeof vb);   /* the mget must have copied it */
	return done;
}

static const char *tree_json(const struct pc_mget *m)
{
	static char out[1 << 16];
	struct pc_tw w;
	struct pc_jw j;

	pc_tw_init(&w, tbuf, sizeof tbuf);
	pc_mget_tree(m, &w);
	if (pc_tw_done(&w) != 0)
		return "(tree not done)";
	pc_jw_init(&j, out, sizeof out - 1);
	if (pc_tree_json(&j, w.b, w.n) != w.n)
		return "(tree unreadable)";
	out[j.len] = 0;
	return out;
}

static const char *resp(const struct pc_mget *m)
{
	struct pc_jw w;

	pc_jw_init(&w, jbuf, sizeof jbuf - 1);
	pc_mget_resp(m, &w);
	jbuf[w.len] = 0;
	return jbuf;
}

int main(void)
{
	struct pc_mget *m, *done;
	unsigned long long f, u, r;
	uint32_t r1, r2, r3;
	const char *why;
	int taken, i;

	/* A */
	m = pc_mget_new("c", 1, 3);
	pc_mget_hit(m, 0, dupz("v0"), 2, now_ticks + 10);
	pc_mget_absent(m, 1);
	pc_mget_hit(m, 2, dupz("v2"), 2, 0);
	CHK(pc_mget_waiting(m) == 0 && pc_mget_deadline(m) == now_ms + 300,
		"A: nothing to wait on; the deadline is one pull's, begun now");
	CHK(!strcmp(tree_json(m), "{\"values\":[{\"found\":true,\"value\":\"v0\","
		"\"ttl\":10},{\"found\":false},{\"found\":true,\"value\":\"v2\","
		"\"ttl\":-1}]}"), "A: the native shape: %s", tree_json(m));
	CHK(!strcmp(resp(m), "*3\r\n$2\r\nv0\r\n$-1\r\n$2\r\nv2\r\n"),
		"A: the RESP array");
	pc_mget_free(m);

	/* B */
	m = pc_mget_new("c", 1, 4);
	pc_mget_hit(m, 0, dupz("here"), 4, 0);
	r1 = wait_key(m, 1, "k1");
	r2 = wait_key(m, 2, "k2");
	r3 = wait_key(m, 3, "k3");
	CHK(pc_mget_waiting(m) == 3 && pc_mget_attach(m, &conn_a,
		PC_MGET_DOOR_CMD, "IDIDIDID") == 0 && pc_mget_conn(m) == &conn_a &&
		pc_mget_door(m) == PC_MGET_DOOR_CMD &&
		!memcmp(pc_mget_id(m), "IDIDIDID", 8),
		"B: three lookups waiting; attached to its connection and frame");
	done = answer(r2, "remote2", 0, 0, &taken);
	CHK(taken && !done && pc_mget_waiting(m) == 2 && keeps == 1,
		"B: the first answer taken (and kept as its mode keeps it), the "
		"mget still waiting");
	done = answer(r1, NULL, 1, 0, &taken);
	CHK(taken && !done, "B: a timeout taken");
	done = answer(r3, "remote3", 0, 0, &taken);
	CHK(taken && done == m && pc_mget_waiting(m) == 0,
		"B: the last answer hands the mget back");
	CHK(!strcmp(tree_json(m), "{\"values\":[{\"found\":true,\"value\":"
		"\"here\",\"ttl\":-1},{\"found\":false,\"unanswered\":true},"
		"{\"found\":true,\"value\":\"remote2\",\"ttl\":30},{\"found\":true,"
		"\"value\":\"remote3\",\"ttl\":30}]}"),
		"B: the values copied, the ttl carried, the timeout unanswered: %s",
		tree_json(m));
	CHK(!strcmp(resp(m), "-TRYAGAIN mget: 1 key(s) not answered\r\n"),
		"B: RESP all or nothing: %s", jbuf);
	pc_mget_stats(0, &f, &u, &r);
	CHK(f == 1 && u == 1 && r == 0, "B: counted - 1 fanned out, 1 unanswered");
	pc_mget_free(m);

	/* C */
	strict = 1;
	m = pc_mget_new("c", 1, 1);
	r1 = wait_key(m, 0, "doc");
	pc_mget_attach(m, &conn_a, PC_MGET_DOOR_RESP, NULL);
	done = answer(r1, "{}", 0, PCACHE_F_JSON, &taken);
	CHK(done == m && !strcmp(resp(m), "*1\r\n$-1\r\n"),
		"C: a document under strict: nil (%s)", jbuf);
	pc_mget_free(m);
	strict = 0;

	/* D */
	shard = 1;
	m = pc_mget_new("c", 1, 2);
	r1 = wait_key(m, 0, "s1");
	r2 = wait_key(m, 1, "s2");
	pc_mget_attach(m, &conn_a, PC_MGET_DOOR_RESP, NULL);
	neg_sets = begins = 0;
	done = answer(r1, NULL, 0, 0, &taken);
	CHK(taken && !done && !begins && !neg_sets,
		"D: shard, an owner's miss: absent, nobody else asked, nothing "
		"remembered");
	grace = 1;
	done = answer(r2, NULL, 0, 0, &taken);
	CHK(taken && !done && begins == 1 && cap_seen == pc_mget_deadline(m) &&
		cap_ms == 0 && pc_mget_waiting(m) == 1,
		"D: inside the grace: everyone asked once, the pull capped at the "
		"mget's deadline (and the cap cleared)");
	done = answer(next_req - 1, NULL, 0, 0, &taken);
	CHK(done == m && !strcmp(resp(m), "*2\r\n$-1\r\n$-1\r\n") && begins == 1,
		"D: and the broadcast's miss is final: absent, not asked again");
	pc_mget_free(m);
	m = pc_mget_new("c", 1, 1);
	r1 = wait_key(m, 0, "s3");
	pc_mget_attach(m, &conn_a, PC_MGET_DOOR_RESP, NULL);
	now_ms += 299;                         /* 1 ms left */
	done = answer(r1, NULL, 0, 0, &taken);
	CHK(done == m && begins == 1 &&
		!strcmp(resp(m), "-TRYAGAIN mget: 1 key(s) not answered\r\n"),
		"D: no time left: nobody asked, unanswered");
	pc_mget_free(m);
	shard = grace = 0;

	/* E */
	proxy = 1;
	m = pc_mget_new("c", 1, 2);
	targets[next_req & 0xffff] = 7;        /* r1 went where the locator said */
	r1 = wait_key(m, 0, "p1");
	r2 = wait_key(m, 1, "p2");             /* r2 was a broadcast */
	pc_mget_attach(m, &conn_a, PC_MGET_DOOR_RESP, NULL);
	begins = loc_clears = neg_sets = 0;
	done = answer(r1, NULL, 0, 0, &taken);
	CHK(!done && begins == 1 && loc_clears == 1 && !neg_sets,
		"E: proxy, a miss where the locator pointed: forgotten, everyone "
		"asked");
	done = answer(r2, NULL, 0, 0, &taken);
	CHK(!done && begins == 1 && neg_sets == 1,
		"E: a broadcast's miss: absent, remembered");
	done = answer(next_req - 1, "found", 0, 0, &taken);
	CHK(done == m && !strcmp(resp(m), "*2\r\n$5\r\nfound\r\n$-1\r\n"),
		"E: the retry finds it where it moved");
	pc_mget_free(m);
	proxy = 0;

	/* F */
	m = pc_mget_new("c", 1, 2);
	r1 = PEND_TAG | (5u << 16) | 9;        /* slot 9, an old generation */
	r2 = PEND_TAG | (6u << 16) | 9;        /* slot 9 again */
	CHK(pc_mget_wait(m, 0, "f1", 2, r1, &why) == 0 &&
		pc_mget_wait(m, 1, "f2", 2, r2, &why) == 0, "F: two requests on slot 9");
	pc_mget_attach(m, &conn_a, PC_MGET_DOOR_RESP, NULL);
	done = answer(r2, "second", 0, 0, &taken);
	CHK(taken && !done, "F: the newer found its key");
	done = answer(r1, NULL, 1, 0, &taken);
	CHK(taken && done == m && !strcmp(resp(m),
		"-TRYAGAIN mget: 1 key(s) not answered\r\n"),
		"F: the older (expired) found its own");
	pc_mget_free(m);
	done = answer(PEND_TAG | (7u << 16) | 3, "x", 0, 0, &taken);
	CHK(!taken && !done, "F: an answer nobody waits on: not taken");
	m = pc_mget_new("c", 1, 1);
	r1 = wait_key(m, 0, "gone");
	pc_mget_attach(m, &conn_b, PC_MGET_DOOR_RESP, NULL);
	cancels = 0;
	pc_mget_drop_conn(&conn_a);
	CHK(cancels == 0, "F: another connection's close leaves it");
	pc_mget_drop_conn(&conn_b);
	CHK(cancels == 1 && cancelled[0] == r1, "F: its own close cancels its pull");
	done = answer(r1, "late", 0, 0, &taken);
	CHK(!taken, "F: and the late answer is not taken");

	/* G */
	m = pc_mget_new("c", 1, PC_MGET_MAX_REMOTE + 1);
	pend_cap = 8192;
	pend_free = 8192;
	for (i = 0; i < PC_MGET_MAX_REMOTE; i++)
		if (pc_mget_wait(m, (unsigned int)i, "g", 1, next_req++, &why) != 0)
			break;
	CHK(i == PC_MGET_MAX_REMOTE && pc_mget_wait(m, (unsigned int)i, "g", 1,
		next_req++, &why) == PC_MGET_E_CAP &&
		!strcmp(why, "mget needs more than 1000 remote lookups - split it"),
		"G: 1,000 lookups wait, the 1,001st refused: %s", why);
	cancels = 0;
	pc_mget_free(m);
	CHK(cancels == PC_MGET_MAX_REMOTE,
		"G: an mget given up cancels every pull it waited on (%d)", cancels);
	m = pc_mget_new("c", 1, 1);
	pend_free = 8192 / 4 - 1;
	CHK(pc_mget_wait(m, 0, "b", 1, next_req++, &why) == PC_MGET_E_BUSY &&
		pc_mget_waiting(m) == 0,
		"G: less than a quarter of the table free: refused, busy (%s)", why);
	pend_free = 8192 / 4;
	CHK(pc_mget_wait(m, 0, "b", 1, next_req++, &why) == 0,
		"G: a quarter free exactly: it waits");
	pc_mget_free(m);

	/* H */
	pc_mget_count_refused();
	pc_mget_stats(1, &f, &u, &r);
	CHK(r == 1, "H: a refusal counted");
	pc_mget_stats_reset();
	pc_mget_count_refused();
	pc_mget_stats(1, &f, &u, &r);
	CHK(f == 0 && u == 0 && r == 1, "H: since the reset: only what came after");
	pc_mget_stats(0, &f, &u, &r);
	CHK(f >= 6 && u >= 3 && r == 2,
		"H: from the start (/metrics): %llu fanned out, %llu unanswered, "
		"%llu refused", f, u, r);

	printf("mgetaggtest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
