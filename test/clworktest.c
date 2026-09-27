/*
 * clworktest.c - the worker entry points, with nothing of cluster.c
 * linked (M9b, wave 4).
 *
 * M9's bullet named no test, and M9 noted why: its entry points needed
 * a populated peer table and live arena state.  With the send path a
 * module (M14) and the pending table one (M2), they need neither - this
 * links the REAL clwork.o, clpend.o and clsend.o, captures what would
 * have been sent, opens it, and parses it.  Only the accessors are
 * stand-ins: the table they hand out is a real struct clpeers.
 *
 * What it pins is what a client sees when this goes wrong: a forward
 * that parks a slot and sends nothing hangs until the deadline; one that
 * sends and parks nothing loses the answer; a failed send that keeps its
 * slot leaks the table a slot at a time; a pull that reaches a dead
 * peer waits on an answer that cannot come.
 *
 * The write-path push (M9b's second half) links the real clpush.o and
 * clmig.o too, so what reaches a peer is a real group, opened and parsed
 * back.  Its failures are quieter than the forward's: a record pushed to
 * a peer that is not a holder is extra apply work on every node, and a
 * record NOT pushed to one is a copy the fleet believes it has - and
 * under spread the return value decides whether this node keeps the
 * record at all.
 *
 * Build: see the Makefile's clworktest target.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <sodium.h>

#include "../src/cluster.h"
#include "../src/clstate.h"
#include "../src/clpeers.h"
#include "../src/clpend.h"
#include "../src/clsend.h"
#include "../src/clwire.h"
#include "../src/clpull.h"
#include "../src/clfwd.h"
#include "../src/clmsg.h"
#include "../src/clmig.h"
#include "../src/clpush.h"
#include "../src/pc_slot.h"

static int pass, fail;
static void chk(int cond, const char *what)
{
	if (cond) { pass++; return; }
	fail++;
	printf("  FAIL %s\n", what);
}

/* ---- the stand-ins: cluster.c's accessors over test-owned state -------- */
static struct clpeers PT;
static struct clpend PD;
static const long long NOW = 5000000;
static int enabled = 1, fail_reason = -1;
static unsigned long long no_route, pull_sent, send_fail, fwd_sent, fwd_fails;
static int send_errno;

struct clpeers *cl_peers(void) { return &PT; }
struct clpend *cl_pend(void) { return &PD; }
static struct clpush PW;
static int K;                       /* pc_cluster_replicas(): 0 = not spread */
static unsigned int ticks = 1000;
static unsigned long long repl_pushed, repl_groups, not_held, skipped_big;

struct clpush *cl_push(void) { return &PW; }
int pc_cluster_replicas(void) { return K; }
int pc_worker_id(void) { return 0; }
unsigned int compat_ticks(void) { return ticks; }
void cl_px_repl_pushed(unsigned long long n) { repl_pushed += n; }
void cl_px_repl_groups(void) { repl_groups++; }
void cl_px_spread_not_held(void) { not_held++; }
int cl_enabled(void) { return enabled; }
int cl_node_id(void) { return 42; }
int cl_map_usable(void) { return 0; }
const struct pc_clmap *cl_map(void) { return NULL; }
struct sockaddr_in cl_self_addr(void) { struct sockaddr_in a; memset(&a, 0, sizeof a); return a; }
long long cl_now_ms(void) { return NOW; }
unsigned int cl_self_free_mb(void) { return 100; }
void cl_place_map_hit(void) {}
void cl_place_hrw_hit(void) {}
void cl_st_fwd_no_route(void) { no_route++; }
void cl_st_fwd_send_fail(void) { send_fail++; }
void cl_st_pull_sent(unsigned long long n) { pull_sent += n; }
void cl_st_fwd_send_errno(int e) { send_errno = e; }
void cl_px_fwd_sent(void) { fwd_sent++; }
void cl_px_fwd_fails(void) { fwd_fails++; }
void cl_px_migrate_skipped_big(void) { skipped_big++; }
void cl_px_placed_local(void) {}
void cl_px_placed_remote(void) {}
void cl_set_fail(int reason) { fail_reason = reason; }
static int nst;                     /* pc_node_state(): not READY until spread */
int pc_node_state(void) { return nst; }
/* cluster.c's pend_alloc: the clpend allocation, the reason on failure */
uint32_t cl_pend_alloc(int kind, int expect, int extra_ms, struct pending **out)
{
	int reason = 0;
	uint32_t req = clpend_alloc(&PD, kind, expect, extra_ms, 0, NOW, 80,
		out, &reason);

	if (!req)
		fail_reason = reason;
	return req;
}
void cl_pend_release(struct pending *slot)
{
	clpend_lock(&PD);
	clpend_release_locked(&PD, slot);
	clpend_unlock(&PD);
}

/* ---- the capture -------------------------------------------------------- */
static const uint8_t KEY[32] = { 4, 2 };
/* clwire_open takes no capacity: a slot holds the largest datagram */
static struct { unsigned last; unsigned char pt[65536]; unsigned long long n; } out[8];
static int nout, refuse;

static long capture(int fd, const void *buf, size_t n,
		const struct sockaddr_in *to)
{
	(void)fd;
	if (refuse) {
		errno = EHOSTUNREACH;
		return -1;
	}
	if (nout < 8 && clwire_open(KEY, buf, n, out[nout].pt, &out[nout].n) == 0)
		out[nout].last = ntohl(to->sin_addr.s_addr) & 0xff;
	nout++;
	return (long)n;
}

static void add(unsigned char last, int node, long long seen)
{
	struct peer *p = &PT.peers[PT.n_peers++];

	memset(p, 0, sizeof *p);
	p->addr.sin_family = AF_INET;
	p->addr.sin_port = htons(17163);
	p->addr.sin_addr.s_addr = htonl(0x7f001000u | last);
	p->node = node;
	p->last_seen_ms = seen;
}

static void reset(void)
{
	nout = 0;
	refuse = 0;
	fail_reason = -1;
	no_route = pull_sent = send_fail = fwd_sent = fwd_fails = 0;
	send_errno = 0;
	repl_pushed = repl_groups = not_held = skipped_big = 0;
}

/* every group this worker holds goes now, as the flush RPC would send it */
static void flush(void)
{
	clpush_flush_mine(&PW, 0, NOW + 1000, cl_wgroup_send_cb, NULL);
}

/* datagram i is one M_REPL_MANY group from node 42 carrying `count`
 * records, the header byte for byte as clmig spells it */
static int is_group(int i, unsigned count)
{
	unsigned char h[CLMIG_GHDR];

	clmig_group_hdr(h, sizeof h, M_REPL_MANY, 0, 42, count);
	return i < nout && out[i].n >= CLMIG_GHDR && !memcmp(out[i].pt, h, CLMIG_GHDR);
}

/* the first record of datagram i */
static int first_rec(int i, struct clmig_rec *r)
{
	size_t off = CLMIG_GHDR;

	return i < nout && clmig_rec_parse(out[i].pt, out[i].n, &off, r);
}

/* which of peers 5 and 7 datagrams went to, as a bit set */
static unsigned sent_to(void)
{
	unsigned m = 0;
	int i;

	for (i = 0; i < nout && i < 8; i++)
		m |= out[i].last == 5 ? 1u : out[i].last == 7 ? 2u : 4u;
	return m;
}

int main(void)
{
	struct clfwd_op f;
	struct clpull_req q;
	struct clfwd_json j;
	struct clmig_rec r;
	uint32_t req, r1, r2;
	int used;

	if (sodium_init() < 0 || clpend_init(&PD, PEND_MIN) != PEND_MIN) {
		printf("clworktest: setup failed\n");
		return 2;
	}
	clsend_init(3, KEY, capture);
	/* cluster.c's geometry: MIG_GHDR, MIG_GATHER_CAP,
	 * REPL_GROUP_FLUSH_BYTES, REPL_FLUSH_MS */
	clpush_init(&PW, PC_CL_MAXPEER, CLMIG_GHDR, 56000, 48000, 3);
	add(5, 5, NOW - 10);                       /* live */
	add(7, 7, NOW - 10);                       /* live */
	add(9, 9, NOW - PEER_UP_MS - 1);           /* not heard from: dead */

	/* ---- a forward to a live holder ------------------------------------ */
	reset();
	used = clpend_used(&PD);
	req = pc_fwd_begin(7, 0, "c", 1, "key", 3, "val", 3, 60, 0);
	chk(req != 0 && clpend_used(&PD) == used + 1, "fwd: parks exactly one slot and returns its handle");
	chk(nout == 1 && out[0].last == 7 && out[0].pt[0] == M_FWD_OP, "fwd: one FWD_OP, to the holder's address only");
	chk(clfwd_op_parse(out[0].pt, out[0].n, &f) && f.req == req && f.node == 42 && f.op == 0 &&
	    f.collen == 1 && f.klen == 3 && f.vlen == 3 && !memcmp(f.key, "key", 3) && f.ttl_rel == 60,
		"fwd: the frame carries our req, our node id, the op, the key, the value, the ttl");
	chk(fwd_sent == 1 && fail_reason == PC_CLFAIL_NONE, "fwd: counted as sent, no failure reason");

	/* ---- a forward whose send fails: the slot must come back ----------- */
	reset();
	refuse = 1;
	used = clpend_used(&PD);
	req = pc_fwd_begin(5, 0, "c", 1, "key", 3, "val", 3, 0, 0);
	chk(req == 0 && clpend_used(&PD) == used, "fwd, send fails: 0, and the parked slot is released (no leak)");
	chk(fwd_fails == 1 && send_fail == 1 && send_errno == EHOSTUNREACH && fail_reason == PC_CLFAIL_SEND,
		"fwd, send fails: counted, errno kept, reason SEND");

	/* ---- a forward to a peer that is not live -------------------------- */
	reset();
	used = clpend_used(&PD);
	chk(pc_fwd_begin(9, 0, "c", 1, "k", 1, "v", 1, 0, 0) == 0 && nout == 0 && clpend_used(&PD) == used,
		"fwd to a dead holder: 0, nothing sent, nothing parked");
	chk(fail_reason == PC_CLFAIL_ROUTE && no_route == 1, "  reason ROUTE, counted as no route");

	/* ---- a full table is backpressure, and says so ---------------------- */
	for (r1 = 1, used = 0; r1 && used < PEND_MIN + 2; used++) {
		reset();
		r1 = pc_fwd_begin(5, 0, "c", 1, "k", 1, "v", 1, 0, 0);
	}
	chk(r1 == 0 && clpend_used(&PD) == PEND_MIN, "fwd: the table fills at its capacity");
	chk(nout == 0 && fail_reason == PC_CLFAIL_BUSY,
		"fwd, table full: nothing sent, reason BUSY (not overwritten by NONE)");
	clpend_fini(&PD);
	clpend_init(&PD, PEND_MIN);

	/* ---- a pull asks every live peer, and only them --------------------- */
	reset();
	req = pc_pull_begin("c", 1, "missing", 7);
	chk(req != 0 && nout == 2 && out[0].last == 5 && out[1].last == 7, "pull: one PULL_REQ to each LIVE peer, not the dead one");
	chk(clpull_req_parse(out[0].pt, out[0].n, &q) && q.req == req && q.node == 42 && q.klen == 7,
		"pull: the frame carries our req, our node id, the key");
	{
		struct pending *s;

		clpend_lock(&PD);
		s = clpend_find(&PD, req);
		chk(s && s->expect == 2 && s->kind == PC_DONE_PULL && s->klen == 7 && !memcmp(s->key, "missing", 7),
			"pull: the slot expects 2 answers and remembers the key");
		clpend_unlock(&PD);
	}
	chk(pull_sent == 1, "pull: counted once, not once per peer");

	/* ---- a pull to one holder -------------------------------------------- */
	reset();
	r2 = pc_pull_begin_at("c", 1, "k", 1, 5);
	chk(r2 != 0 && nout == 1 && out[0].last == 5, "pull_at: the named holder only");
	reset();
	chk(pc_pull_begin_at("c", 1, "k", 1, 9) == 0 && nout == 0 && fail_reason == PC_CLFAIL_ROUTE,
		"pull_at a dead holder: 0, nothing sent, reason ROUTE");

	/* ---- no live peer, no pull ------------------------------------------- */
	PT.peers[0].last_seen_ms = PT.peers[1].last_seen_ms = NOW - PEER_UP_MS - 1;
	reset();
	used = clpend_used(&PD);
	chk(pc_pull_begin("c", 1, "k", 1) == 0 && nout == 0 && clpend_used(&PD) == used,
		"pull with nobody live: 0, nothing parked (the caller answers a local miss)");
	PT.peers[0].last_seen_ms = PT.peers[1].last_seen_ms = NOW - 10;

	/* ---- probe-before-place carries the deferred write ------------------ */
	reset();
	req = pc_probe_fwd_begin(2, "c", 1, "ctr", 3, "", 0, 30, 5);
	{
		struct pending *s;

		clpend_lock(&PD);
		s = clpend_find(&PD, req);
		chk(req && s && s->probe_op == 2 && s->by == 5 && s->ttl_rel == 30 && s->expect == 2,
			"probe: the slot carries the deferred add (op, by, ttl) and expects every live peer");
		clpend_unlock(&PD);
	}
	chk(nout == 2, "probe: asks every live peer");

	/* ---- a JSON forward ---------------------------------------------------- */
	reset();
	req = pc_fwd_json_begin(7, 1, "c", 1, "doc", 3, "$.a", 3, "1", 1, 0, 0, 0, 0, 0, 0);
	chk(req && nout == 1 && out[0].last == 7 && out[0].pt[0] == M_FWD_JSON &&
	    clfwd_json_parse(out[0].pt, out[0].n, &j) && j.req == req && j.node == 42 && j.plen == 3,
		"json fwd: one FWD_JSON to the holder, our req and node, the path");

	/* ---- the cluster off, and arguments too large ------------------------ */
	reset();
	enabled = 0;
	chk(pc_fwd_begin(7, 0, "c", 1, "k", 1, "v", 1, 0, 0) == 0 && pc_pull_begin("c", 1, "k", 1) == 0 && nout == 0,
		"cluster off: every entry point refuses, nothing sent");
	enabled = 1;
	reset();
	{
		static char big[300];

		memset(big, 'x', sizeof big);
		chk(pc_fwd_begin(7, 0, big, 256, "k", 1, "v", 1, 0, 0) == 0 && nout == 0 && fail_reason == PC_CLFAIL_ROUTE,
			"a collection name past 255: refused before anything parks or leaves");
	}

	/* ---- the write-path push: eager, every live peer ------------------- */
	reset();
	K = 0;
	chk(pc_repl_push("c", 1, "k1", 2, "v1", 2, 0, 7) == 1 && nout == 0 && clpush_open(&PW) == 2,
		"push: kept, a group opened for each LIVE peer (not the dead one), nothing sent yet");
	flush();
	chk(nout == 2 && sent_to() == 3 && is_group(0, 1) && is_group(1, 1),
		"push: the flush sends one M_REPL_MANY group of one to each, from node 42");
	chk(first_rec(0, &r) && r.collen == 1 && r.col[0] == 'c' && r.klen == 2 && !memcmp(r.key, "k1", 2) &&
	    r.vlen == 2 && !memcmp(r.val, "v1", 2) && r.ver == 7 && r.ttl_left == 0,
		"push: the record carries the collection, key, value and version, no ttl");
	chk(repl_pushed == 2 && repl_groups == 2 && clpush_open(&PW) == 0,
		"push: 2 records and 2 groups counted, nothing left open");

	/* ---- the group is the coalescing, not a datagram per write -------- */
	reset();
	pc_repl_push("c", 1, "a", 1, "1", 1, 0, 1);
	pc_repl_push("c", 1, "b", 1, "2", 1, 0, 2);
	pc_repl_push("c", 1, "d", 1, "3", 1, 0, 3);
	flush();
	chk(nout == 2 && is_group(0, 3) && is_group(1, 3) && repl_pushed == 6 && repl_groups == 2,
		"push x3: one group of three per peer, 6 records in 2 groups");

	/* ---- the expiry travels as time LEFT ------------------------------- */
	reset();
	ticks = 1000;
	chk(pc_repl_push("c", 1, "t", 1, "v", 1, 1030, 0) == 1, "push with a ttl: kept");
	flush();
	chk(first_rec(0, &r) && r.ttl_left == 30, "  sent as 30 s left, not as our clock's 1030");
	reset();
	chk(pc_repl_push("c", 1, "t", 1, "v", 1, 1000, 0) == 1 && clpush_open(&PW) == 0,
		"push already expired (exp == now): kept by the caller, sent to nobody");

	/* ---- the size ceiling is the sweep's, and at it the record goes ---- */
	{
		static char big[PC_MAX_FWD_VAL + 1];

		reset();
		chk(pc_repl_push("c", 1, "big", 3, big, PC_MAX_FWD_VAL + 1, 0, 0) == 1 &&
		    skipped_big == 1 && clpush_open(&PW) == 0,
			"push past PC_MAX_FWD_VAL: kept (dropping it would lose it), counted, not sent");
		reset();
		chk(pc_repl_push("c", 1, "big", 3, big, PC_MAX_FWD_VAL, 0, 0) == 1 && skipped_big == 0,
			"push AT PC_MAX_FWD_VAL: not skipped");
		flush();
		chk(nout == 2 && first_rec(0, &r) && r.vlen == PC_MAX_FWD_VAL,
			"  and the whole value arrives");
	}

	/* ---- nothing a peer could store leaves ----------------------------- */
	reset();
	enabled = 0;
	chk(pc_repl_push("c", 1, "k", 1, "v", 1, 0, 0) == 1 && clpush_open(&PW) == 0,
		"push, cluster off: kept, nothing opened");
	enabled = 1;
	reset();
	{
		static char longkey[4097];

		chk(pc_repl_push("", 0, "k", 1, "v", 1, 0, 0) == 1 &&
		    pc_repl_push("c", 1, "", 0, "v", 1, 0, 0) == 1 &&
		    pc_repl_push("c", 1, longkey, 4097, "v", 1, 0, 0) == 1 && clpush_open(&PW) == 0,
			"push with no collection, no key, or a key past 4096: kept, nothing opened");
	}

	/* ---- spread: the K holders, and the keep decision ------------------ *
	 * This node READY, peer 9 past its slot grace: the candidates are
	 * exactly 42, 5 and 7, all live, so with K = 2 every key has two
	 * holders and what this node keeps plus what it sends is always 2. */
	K = 2;
	nst = PC_NST_READY;
	PT.peers[2].last_seen_ms = NOW - PEER_UP_MS - PEER_SLOT_GRACE_MS - 1;
	{
		char key[16];
		int held = -1, other = -1, i, in, kept, bad = 0;
		unsigned slot;

		for (i = 0; i < 200 && (held < 0 || other < 0); i++) {
			snprintf(key, sizeof key, "s%d", i);
			in = pc_spread_in_set(pc_key_slot(key, strlen(key)), 0, K);
			if (in && held < 0)
				held = i;
			if (!in && other < 0)
				other = i;
		}
		chk(held >= 0 && other >= 0, "spread: keys exist on both sides of the set");

		snprintf(key, sizeof key, "s%d", other);
		reset();
		chk(pc_repl_push("c", 1, key, strlen(key), "v", 1, 0, 0) == 0 && not_held == 1,
			"spread, not a holder: 0 (the caller must not keep it), counted");
		flush();
		chk(nout == 2 && sent_to() == 3,
			"  and both other nodes get it - with this node out, they ARE the K");

		snprintf(key, sizeof key, "s%d", held);
		slot = pc_key_slot(key, strlen(key));
		reset();
		chk(pc_repl_push("c", 1, key, strlen(key), "v", 1, 0, 0) == 1 && not_held == 0,
			"spread, a holder: kept");
		flush();
		chk(nout == 1 && sent_to() == (pc_spread_in_set(slot, 5, K) ? 1u : 2u) &&
		    pc_spread_in_set(slot, 5, K) + pc_spread_in_set(slot, 7, K) == 1,
			"  sent to the ONE other holder only");

		for (i = 0; i < 64; i++) {
			snprintf(key, sizeof key, "k%d", i);
			reset();
			kept = pc_repl_push("c", 1, key, strlen(key), "v", 1, 0, 0);
			flush();
			if (kept + nout != K)
				bad++;
		}
		chk(bad == 0, "spread, 64 keys: kept here + sent is K for every one");

		/* peer 9 quiet but inside its grace: still a candidate, so a key
		 * it holds has one live holder fewer to send to - and the push
		 * sends to LIVE holders only, never to the one that is quiet */
		PT.peers[2].last_seen_ms = NOW - PEER_UP_MS - 1;
		for (i = 0, bad = 0, in = 0; i < 64; i++) {
			unsigned want = 0;

			snprintf(key, sizeof key, "g%d", i);
			slot = pc_key_slot(key, strlen(key));
			if (pc_spread_in_set(slot, 9, K))
				in++;
			want = (pc_spread_in_set(slot, 5, K) ? 1u : 0) | (pc_spread_in_set(slot, 7, K) ? 2u : 0);
			reset();
			kept = pc_repl_push("c", 1, key, strlen(key), "v", 1, 0, 0);
			flush();
			if (sent_to() != want || kept != pc_spread_in_set(slot, 0, K))
				bad++;
		}
		chk(in > 0 && bad == 0,
			"spread, a quiet holder in its grace: sent to the live holders only, kept iff this node holds");
	}
	PT.peers[2].last_seen_ms = NOW - PEER_UP_MS - 1;
	nst = 0;
	K = 0;

	printf("clworktest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
