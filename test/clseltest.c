/*
 * clseltest.c — client-side node selection (step 5).
 *
 * The spread property here is the one failovertest tries to check
 * against a live fleet, where it races a node it just killed and
 * intermittently reports every client on one node.  The same property
 * is decidable without a fleet, deterministically, which is what this
 * does.
 *
 * Build: cc -o clseltest test/clseltest.c src/clsel.o src/clmap.o
 */
#include <stdio.h>
#include <string.h>

#include "../src/clsel.h"

static int pass, fail;
#define CHK(cond, ...) do { \
	if (cond) { pass++; } \
	else { fail++; printf("  FAIL "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static struct pc_clmap M;

static void mkmap(struct pc_clmap *m, int n)
{
	int i;

	memset(m, 0, sizeof *m);
	for (i = 0; i < n; i++) {
		struct pc_clmap_node *nd = &m->node[i];

		memset(nd->ident, 0xC0 + i, 16);
		nd->node_id = (uint16_t)(20 + i);
		nd->addr = 0x0A000001u + (uint32_t)i;
		nd->cluster_port = (uint16_t)(17000 + i);
		nd->client_port = (uint16_t)(18000 + i);
		nd->state = PC_CLMAP_ST_READY;
		nd->cap_weight = PC_CLMAP_W_NOMINAL;
		nd->admin_weight = PC_CLMAP_W_UNSET;
		m->nnodes++;
	}
}

int main(void)
{
	int i, distinct, cnt[64];
	uint16_t id;

	/* ---- 1. THE SPREAD PROPERTY -------------------------------- */
	mkmap(&M, 3);
	memset(cnt, 0, sizeof cnt);
	for (i = 0; i < 12; i++) {
		id = pc_clsel_pick(&M, PC_CLSEL_ROUND_ROBIN,
			0x1000ull + (uint64_t)i, 0);
		if (id >= 20 && id < 84) cnt[id - 20]++;
	}
	distinct = 0;
	for (i = 0; i < 64; i++) if (cnt[i]) distinct++;
	CHK(distinct >= 2,
		"round-robin put 12 clients on %d node(s) - every client in "
		"the fleet would pile onto one", distinct);

	/* 300 clients across 5 nodes should use all of them */
	mkmap(&M, 5);
	memset(cnt, 0, sizeof cnt);
	for (i = 0; i < 300; i++) {
		id = pc_clsel_pick(&M, PC_CLSEL_ROUND_ROBIN, (uint64_t)i, 0);
		if (id >= 20 && id < 84) cnt[id - 20]++;
	}
	distinct = 0;
	for (i = 0; i < 64; i++) if (cnt[i]) distinct++;
	CHK(distinct == 5, "300 clients reached only %d of 5 nodes", distinct);
	for (i = 0; i < 5; i++)
		if (cnt[i] < 300 / 5 / 2 || cnt[i] > 300 / 5 * 2) break;
	CHK(i == 5, "round-robin is lumpy: node %d took %d of 300", i, cnt[i]);

	/* a client's seed must matter.  Stated over a SET of seeds rather
	 * than two pairs: with 5 nodes, two arbitrary pairs colliding is a
	 * 4%% coincidence, and a test that fails 4%% of the time teaches
	 * nobody anything. */
	{
		int seen[64];
		int d = 0;

		memset(seen, 0, sizeof seen);
		for (i = 0; i < 20; i++) {
			uint16_t x = pc_clsel_pick(&M, PC_CLSEL_ROUND_ROBIN,
				(uint64_t)i, 0);

			if (x >= 20 && x < 84 && !seen[x - 20]) {
				seen[x - 20] = 1;
				d++;
			}
		}
		CHK(d >= 4,
			"20 consecutive client seeds reached only %d of 5 "
			"nodes - the seed barely matters", d);
	}

	/* the same client must be stable across calls */
	id = pc_clsel_pick(&M, PC_CLSEL_ROUND_ROBIN, 77, 0);
	for (i = 0; i < 50; i++)
		if (pc_clsel_pick(&M, PC_CLSEL_ROUND_ROBIN, 77, 0) != id) break;
	CHK(i == 50, "the same client wanders between nodes");

	/* ---- 2. eligibility is by STATE, not weight ---------------- */
	mkmap(&M, 3);
	M.node[0].state = PC_CLMAP_ST_RECOVERING;
	M.node[1].state = PC_CLMAP_ST_DRAINING;
	CHK(pc_clsel_count(&M) == 1, "count is %d, expected 1",
		pc_clsel_count(&M));
	for (i = 0; i < 50; i++)
		if (pc_clsel_pick(&M, PC_CLSEL_ROUND_ROBIN, (uint64_t)i, 0)
		        != M.node[2].node_id) break;
	CHK(i == 50,
		"a client was sent to a recovering or draining node");

	/* a drained node - weight 0 - is still TALKABLE: it holds what it
	 * has not handed over, and in store mode it can answer anything */
	mkmap(&M, 2);
	M.node[0].admin_weight = 0;
	CHK(pc_clsel_count(&M) == 2,
		"a weight-0 node was made unreachable - weight decides what "
		"it OWNS, not whether it can be talked to");

	/* ---- 3. nothing eligible means nothing, on purpose --------- */
	mkmap(&M, 2);
	M.node[0].state = PC_CLMAP_ST_RECOVERING;
	M.node[1].state = PC_CLMAP_ST_STARTING;
	CHK(pc_clsel_pick(&M, PC_CLSEL_ROUND_ROBIN, 5, 0) == 0,
		"a client was sent to a node that is still settling - it can "
		"answer WRONGLY, not just with a miss");
	CHK(pc_clsel_count(&M) == 0, "count found a usable node");

	/* ---- 4. avoiding the node that just died ------------------- */
	mkmap(&M, 3);
	for (i = 0; i < 100; i++)
		if (pc_clsel_pick(&M, PC_CLSEL_ROUND_ROBIN, (uint64_t)i,
		        M.node[1].node_id) == M.node[1].node_id) break;
	CHK(i == 100, "a client was sent back to the node it just lost");

	/* but if it is the ONLY one left, it beats no service at all */
	mkmap(&M, 1);
	CHK(pc_clsel_pick(&M, PC_CLSEL_ROUND_ROBIN, 5, M.node[0].node_id)
		== M.node[0].node_id,
		"avoiding the last remaining node left the client with "
		"nothing - no service is worse than a suspect node here");

	/* ---- 5. weighted selection is proportional ----------------- */
	mkmap(&M, 3);
	M.node[0].admin_weight = 4 * PC_CLMAP_W_NOMINAL;
	memset(cnt, 0, sizeof cnt);
	for (i = 0; i < 6000; i++) {
		id = pc_clsel_pick(&M, PC_CLSEL_WEIGHTED, (uint64_t)i, 0);
		if (id >= 20 && id < 84) cnt[id - 20]++;
	}
	/* 4 + 1 + 1 = 6 shares, so node 0 should take about two thirds */
	CHK(cnt[0] > 6000 * 55 / 100 && cnt[0] < 6000 * 78 / 100,
		"a 4x node took %d of 6000 (%d%%), expected ~67%%",
		cnt[0], cnt[0] * 100 / 6000);

	/* all drained but serving: spread evenly rather than not at all */
	mkmap(&M, 3);
	for (i = 0; i < 3; i++) M.node[i].admin_weight = 0;
	CHK(pc_clsel_pick(&M, PC_CLSEL_WEIGHTED, 9, 0) != 0,
		"a fleet with every weight at zero refused every client, "
		"though all of them are READY and serving");

	/* ---- 6. failover is stable, least-conn follows capacity ---- */
	mkmap(&M, 4);
	id = pc_clsel_pick(&M, PC_CLSEL_FAILOVER, 12345, 0);
	CHK(pc_clsel_pick(&M, PC_CLSEL_FAILOVER, 999, 0) == id,
		"failover is not stable across clients with the same map");

	mkmap(&M, 4);
	M.node[2].cap_weight = 9000;
	CHK(pc_clsel_pick(&M, PC_CLSEL_LEAST_CONN, 1, 0) == M.node[2].node_id,
		"least-conn did not choose the node with the most room");

	/* ---- 7. staleness, term first ------------------------------ */
	CHK(pc_clsel_stale(3, 100, 3, 101), "a newer seq is not stale");
	CHK(!pc_clsel_stale(3, 101, 3, 100), "an older seq counted as newer");
	CHK(pc_clsel_stale(3, 999, 4, 1),
		"a newer TERM with a tiny seq did not win - the deposed "
		"master's high sequence would keep the client on its map");
	CHK(!pc_clsel_stale(4, 1, 3, 999),
		"a client was pushed BACK onto a superseded term's map");
	CHK(!pc_clsel_stale(3, 100, 3, 100), "equal epochs count as stale");

	printf("clseltest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
