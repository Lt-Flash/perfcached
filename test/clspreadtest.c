/*
 * clspreadtest.c - spread's decisions, with nothing of cluster.c linked
 * (M17, wave 4).
 *
 * Every one of these was learned from a fleet losing records or
 * crying wolf, and until now only a fleet could check them:
 *   - reclaim on a LOCAL view of the set: a returning node went 64 -> 25
 *     records and the fleet to 207 of 240 owed (29c989e) - so settled
 *     requires the shared map, covering the fleet, listing us;
 *   - settled for one tick is not settled: the drops raced the hand-off;
 *   - a fingerprint that hashed the node id armed the repair INSIDE the
 *     purge grace, because peer_gone zeroes the id;
 *   - the replicas-short warning must hold before it is believed, or a
 *     flapping peer turns it into a log writer.
 *
 * Links the REAL clspread.o and clplace.o (the map lookup).  Placement
 * itself (pc_spread_in_set, pc_spread_set_live) is a stand-in: what is
 * pinned is what the reclaim verdict does with its answers, not the
 * answers - clworktest and clplacetest have those.
 *
 * Build: see the Makefile's clspreadtest target.
 */
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>

#include "../src/cluster.h"
#include "../src/clmap.h"
#include "../src/clpeers.h"
#include "../src/clspread.h"

static int pass, fail;
static void chk(int cond, const char *what)
{
	if (cond) { pass++; return; }
	fail++;
	printf("  FAIL %s\n", what);
}

/* ---- the stand-ins: placement's answers ---------------------------------- */
static int holder, set_live, asked_in_set, asked_live;

int pc_spread_in_set(unsigned slot, int node_id, int k)
{
	(void)slot;
	asked_in_set++;
	return node_id == 0 && k > 0 && holder;
}
int pc_spread_set_live(unsigned slot, int k)
{
	(void)slot; (void)k;
	asked_live++;
	return set_live;
}

static struct clpeers PT;
static struct pc_clmap M;
static const long long NOW = 5000000;

static struct peer *add(unsigned char last, int node, long long seen)
{
	struct peer *p = &PT.peers[PT.n_peers++];

	memset(p, 0, sizeof *p);
	p->addr.sin_family = AF_INET;
	p->addr.sin_port = htons(17163);
	p->addr.sin_addr.s_addr = htonl(0x7f001000u | last);
	p->node = node;
	p->last_seen_ms = seen;
	p->nstate = PC_NST_READY;
	return p;
}

/* the map lists exactly these ids */
static void map(int n, const int *ids)
{
	int i;

	memset(&M, 0, sizeof M);
	M.nnodes = (uint16_t)n;
	for (i = 0; i < n; i++)
		M.node[i].node_id = (uint16_t)ids[i];
}

static int settled(void)
{
	return clspread_settled(2, 0, &M, 42, &PT, 0, NOW);
}

int main(void)
{
	static const int fleet[] = { 42, 5, 7 }, no_self[] = { 5, 7, 9 },
		short_map[] = { 42, 5 };
	struct peer *p5, *p7;

	p5 = add(5, 5, NOW - 10);
	p7 = add(7, 7, NOW - 10);
	map(3, fleet);

	/* ---- settled: the shared map, or nothing --------------------------- */
	chk(settled() == 1, "settled: K, no grace, a map of the whole fleet listing us, peers clean");
	chk(clspread_settled(2, 0, NULL, 42, &PT, 0, NOW) == 0,
		"NO USABLE MAP: not settled - a local view of the set never reclaims (29c989e)");
	map(2, short_map);
	chk(settled() == 0, "a map with fewer nodes than this node sees: not settled");
	map(3, no_self);
	chk(settled() == 0, "a map that does not list us (a rejoined id it predates): not settled");
	map(3, fleet);
	chk(clspread_settled(0, 0, &M, 42, &PT, 0, NOW) == 0, "no spread K: never settled");
	chk(clspread_settled(2, 1, &M, 42, &PT, 0, NOW) == 0, "inside the shard grace: not settled");

	/* a quiet peer inside its slot grace still holds slots: it counts
	 * toward what the map must cover, but its own flags are not read */
	p7->last_seen_ms = NOW - PEER_UP_MS - 1;
	p7->setrepair[0] = 1;
	chk(settled() == 1, "a quiet peer in its grace: counted for coverage, its flags not consulted");
	map(2, short_map);
	chk(settled() == 0, "  and a map without it is short");
	p7->setrepair[0] = 0;
	p7->last_seen_ms = NOW - PEER_UP_MS - PEER_SLOT_GRACE_MS - 1;
	chk(settled() == 1, "a peer past its grace: not counted, so the two-node map covers");
	p7->last_seen_ms = NOW - 10;
	map(3, fleet);

	/* a live peer that has not finished, for this collection */
	p5->nstate = PC_NST_RECOVERING;
	chk(settled() == 0, "a live peer still RECOVERING: not settled");
	p5->nstate = PC_NST_READY;
	p5->setrepair[0] = 1;
	chk(settled() == 0, "a re-placement armed: not settled");
	p5->setrepair[0] = 0;
	p5->backfill[0] = 1;
	chk(settled() == 0, "a backfill armed: not settled");
	p5->backfill[0] = 0;
	p5->repl_cursor[0] = 9;
	chk(settled() == 0, "a repair cycle mid-way: not settled");
	p5->repl_cursor[0] = 0;
	p5->repl_dirty[0] = 1;
	chk(settled() == 0, "a repair cycle that lost records: not settled");
	p5->repl_dirty[0] = 0;
	p5->setrepair[1] = p5->backfill[1] = 1;
	p5->repl_cursor[1] = 3;
	chk(settled() == 1, "another collection's repair does not hold this one");
	memset(p5->setrepair, 0, sizeof p5->setrepair);
	memset(p5->backfill, 0, sizeof p5->backfill);
	memset(p5->repl_cursor, 0, sizeof p5->repl_cursor);

	/* ---- the hold ------------------------------------------------------- */
	{
		unsigned int sf = 0, cur = 77;
		int i, early = 0;

		for (i = 0; i < CLSPREAD_SETTLE_TICKS - 1; i++)
			early += clspread_hold(&sf, &cur, 1);
		chk(early == 0, "hold: nine settled ticks are not enough");
		chk(clspread_hold(&sf, &cur, 1) == 1 && clspread_hold(&sf, &cur, 1) == 1,
			"hold: the tenth consecutive one is, and it stays");
		chk(cur == 77, "  the walk's cursor untouched while it holds");
		chk(clspread_hold(&sf, &cur, 0) == 0 && sf == 0 && cur == 0,
			"one unsettled reading: nothing, the count and the walk start over");
		for (i = 0, early = 0; i < CLSPREAD_SETTLE_TICKS - 1; i++)
			early += clspread_hold(&sf, &cur, 1);
		chk(early == 0 && clspread_hold(&sf, &cur, 1) == 1, "  and it takes ten again");
	}

	/* ---- the reclaim verdict -------------------------------------------- */
	{
		static char k256[CLSPREAD_RECLAIM_KEYMAX + 1];

		memset(k256, 'k', sizeof k256);
		holder = 0;
		set_live = 1;
		asked_in_set = asked_live = 0;
		chk(clspread_reclaim_candidate("k", 0, 2) == 0 && clspread_reclaim_candidate("k", -1, 2) == 0 &&
		    clspread_reclaim_candidate(k256, CLSPREAD_RECLAIM_KEYMAX + 1, 2) == 0 &&
		    asked_in_set == 0 && asked_live == 0,
			"reclaim: an empty key or one past the buffer is kept, placement not asked");
		chk(clspread_reclaim_candidate(k256, CLSPREAD_RECLAIM_KEYMAX, 2) == 1,
			"reclaim: a key AT the buffer limit, not ours, set live: a candidate");
		holder = 1;
		chk(clspread_reclaim_candidate("k", 1, 2) == 0, "reclaim: a key we are a holder of is kept");
		holder = 0;
		set_live = 0;
		chk(clspread_reclaim_candidate("k", 1, 2) == 0,
			"reclaim: its K-set not wholly live - a degraded fleet keeps its surplus");
		set_live = 1;
		asked_live = 0;
		holder = 1;
		clspread_reclaim_candidate("k", 1, 2);
		chk(asked_live == 0, "  a holder's verdict does not need the set's liveness");
	}

	/* ---- the set fingerprint ------------------------------------------- */
	{
		unsigned long long fp = clspread_set_fp(&PT, NOW), fp5;
		static struct clpeers one;   /* a table is large: not on the stack */

		chk(fp != 0, "fingerprint of two slot-holding peers");
		p7->node = 17;
		chk(clspread_set_fp(&PT, NOW) == fp, "a new node id at the same address and port: the SET did not move");
		p7->node = 0;
		p7->gone_ms = NOW;
		p7->last_seen_ms = NOW - PEER_UP_MS - 1;
		chk(clspread_set_fp(&PT, NOW) == fp,
			"purged (id zeroed) inside its grace: unchanged - the purge must not arm the repair");
		memset(&one, 0, sizeof one);
		one.peers[0] = *p5;
		one.n_peers = 1;
		fp5 = clspread_set_fp(&one, NOW);
		chk(clspread_set_fp(&PT, NOW + PEER_SLOT_GRACE_MS) == fp5 && fp5 != fp,
			"past the grace: the set is the one that remains");
		p7->node = 7;
		p7->gone_ms = 0;
		p7->last_seen_ms = NOW - 10;
		p5->addr.sin_port = htons(17164);
		chk(clspread_set_fp(&PT, NOW) != fp, "a peer on another cluster port is another member");
		p5->addr.sin_port = htons(17163);
		one.n_peers = 0;
		chk(clspread_set_fp(&one, NOW) == 0, "no peers: 0");
	}

	/* ---- replicas short -------------------------------------------------- */
	{
		struct clspread_short s = CLSPREAD_SHORT_INIT;
		int ev[6], i;

		for (i = 0; i < 6; i++)
			ev[i] = clspread_short_step(&s, 2, 1, 0, 1);
		chk(ev[0] == CLSPREAD_QUIET && ev[1] == CLSPREAD_QUIET && ev[2] == CLSPREAD_QUIET &&
		    ev[3] == CLSPREAD_SHORT && ev[4] == CLSPREAD_QUIET && ev[5] == CLSPREAD_QUIET,
			"founding node, K=2: quiet while the reading holds, then SHORT once");
		chk(clspread_short_now(&s) == 1, "  and the figure is 1 copy short");
		for (i = 0; i < 12; i++)
			if (clspread_short_step(&s, 2, 1, 0, 1 + (i & 1)) != CLSPREAD_QUIET)
				break;
		chk(i == 12 && clspread_short_now(&s) == 1, "a flapping peer: never said, the figure kept");
		/* the flap's last reading (live 2) began the hold: two more */
		for (i = 0; i < 2; i++)
			ev[i] = clspread_short_step(&s, 2, 1, 0, 2);
		chk(ev[0] == CLSPREAD_QUIET && ev[1] == CLSPREAD_QUIET,
			"met again: quiet while it holds");
		for (i = 0; i < 4; i++)
			ev[i] = clspread_short_step(&s, 2, 0, 0, 1);
		for (i = 0; i < 4; i++)
			ev[i] |= clspread_short_step(&s, 2, 1, 1, 1);
		chk(!ev[0] && !ev[1] && !ev[2] && !ev[3], "not READY, or in the grace: no reading taken");
		chk(clspread_short_step(&s, 2, 1, 0, 2) == CLSPREAD_MET && clspread_short_now(&s) == 0,
			"  and the held reading carries across them: MET, figure 0");
		chk(clspread_short_step(&s, 0, 1, 0, 1) == CLSPREAD_QUIET, "K = 0: nothing to say");
	}

	printf("clspreadtest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
