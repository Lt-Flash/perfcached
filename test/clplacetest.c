/*
 * clplacetest.c — weighted rendezvous placement (step 4).
 *
 * The property that decides whether this can ship at all is the first
 * one: with uniform weights it must select IDENTICALLY to the plain
 * unweighted rendezvous the fleet uses today - not similarly, not
 * mostly.  If that holds, a map published with uniform weights moves no
 * data and the migration is opt-in.  If it does not, switching
 * placement on relocates the entire keyspace.
 *
 * It is asserted against an independent unweighted implementation here,
 * rather than by trusting the algebra in the header.
 *
 * Build: cc -o clplacetest test/clplacetest.c src/clplace.o src/clmap.o
 */
#include <stdio.h>
#include <string.h>

#include "../src/clplace.h"
#include "../src/pc_slot.h"

static int pass, fail;
#define CHK(cond, ...) do { \
	if (cond) { pass++; } \
	else { fail++; printf("  FAIL "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static struct pc_clmap M;

static void addnode(struct pc_clmap *m, int i, uint32_t w)
{
	struct pc_clmap_node *n = &m->node[m->nnodes];

	memset(n, 0, sizeof *n);
	memset(n->ident, 0xB0 + i, 16);
	n->node_id = (uint16_t)(10 + i);
	n->addr = 0x0A000001u + (uint32_t)(i * 7919);
	n->cluster_port = (uint16_t)(17000 + i);
	n->client_port = (uint16_t)(18000 + i);
	n->state = PC_CLMAP_ST_READY;
	n->cap_weight = PC_CLMAP_W_NOMINAL;
	n->admin_weight = w;
	m->nnodes++;
}

static void mkmap(struct pc_clmap *m, int n, uint32_t w)
{
	int i;

	memset(m, 0, sizeof *m);
	for (i = 0; i < n; i++)
		addnode(m, i, w);
}

/* the reference: plain unweighted rendezvous, argmax of the mix */
static uint16_t plain_owner(const struct pc_clmap *m, uint64_t kh)
{
	unsigned int i;
	uint64_t best = 0;
	uint16_t id = 0;

	for (i = 0; i < m->nnodes; i++) {
		uint64_t h;

		if (m->node[i].state != PC_CLMAP_ST_READY)
			continue;
		h = pc_clplace_mix(kh, m->node[i].addr,
			m->node[i].cluster_port);
		if (!id || h > best) { best = h; id = m->node[i].node_id; }
	}
	return id;
}

/* what placement actually hands pc_clplace_owner(): a Redis slot.
 * This used to call pc_clplace_keyhash(), an FNV-1a that S44 left with
 * no production caller - so these assertions were exercising an input
 * the fleet never produces.  The slot has a far smaller range, which is
 * the point: it is what the daemon and every Redis client compute. */
static uint64_t kh_of(int i)
{
	char k[32];
	int n = snprintf(k, sizeof k, "key-%d", i);

	return pc_key_slot(k, (size_t)n);
}

int main(void)
{
	static uint16_t r[PC_CLMAP_MAXNODE], r2[PC_CLMAP_MAXNODE];
	int i, j, diff, n;

	/* ---- 1. THE MIGRATION PROPERTY ----------------------------- */
	mkmap(&M, 7, PC_CLMAP_W_UNSET);     /* all weights nominal */
	diff = 0;
	for (i = 0; i < 20000; i++)
		if (pc_clplace_owner(&M, kh_of(i)) != plain_owner(&M, kh_of(i)))
			diff++;
	CHK(diff == 0,
		"%d of 20000 keys would MOVE on switching to weighted "
		"placement with uniform weights. The migration is supposed to "
		"be free; it is not", diff);

	/* and the whole ranking, not just the owner - otherwise erasure
	 * coded stripes relocate even though their primary does not */
	diff = 0;
	for (i = 0; i < 3000; i++) {
		uint64_t kh = kh_of(i);
		struct pc_clmap tmp = M;
		int k;

		n = pc_clplace_rank(&M, kh, r, 7);
		/* reference ranking: repeatedly take the plain argmax and
		 * remove it */
		for (k = 0; k < 7; k++) {
			r2[k] = plain_owner(&tmp, kh);
			for (j = 0; j < (int)tmp.nnodes; j++)
				if (tmp.node[j].node_id == r2[k])
					tmp.node[j].state =
						PC_CLMAP_ST_DRAINING;
		}
		if (n != 7 || memcmp(r, r2, 7 * sizeof r[0]))
			diff++;
	}
	CHK(diff == 0,
		"%d of 3000 rankings differ from unweighted - EC stripes "
		"would relocate on a uniform map", diff);

	/* ---- 2. deterministic ------------------------------------- */
	{
		uint16_t a = pc_clplace_owner(&M, kh_of(42));

		for (i = 0; i < 100; i++)
			if (pc_clplace_owner(&M, kh_of(42)) != a) break;
		CHK(i == 100, "placement is not deterministic");
	}

	/* ---- 3. weights actually shift share ----------------------- */
	{
		int cnt[16];

		mkmap(&M, 4, PC_CLMAP_W_UNSET);
		M.node[0].admin_weight = 3 * PC_CLMAP_W_NOMINAL;
		memset(cnt, 0, sizeof cnt);
		for (i = 0; i < 60000; i++) {
			uint16_t o = pc_clplace_owner(&M, kh_of(i));

			if (o >= 10 && o < 26) cnt[o - 10]++;
		}
		/* 3 + 1 + 1 + 1 = 6 shares, so node 0 should hold ~1/2 */
		CHK(cnt[0] > 60000 * 40 / 100 && cnt[0] < 60000 * 60 / 100,
			"a 3x node took %d of 60000 (%d%%), expected ~50%%",
			cnt[0], cnt[0] * 100 / 60000);
		CHK(cnt[1] > 60000 * 10 / 100 && cnt[1] < 60000 * 23 / 100,
			"a 1x node took %d of 60000 (%d%%), expected ~17%%",
			cnt[1], cnt[1] * 100 / 60000);
	}

	/* ---- 4. minimal disruption -------------------------------- */
	{
		static struct pc_clmap A, B;
		int moved = 0;

		mkmap(&A, 8, PC_CLMAP_W_UNSET);
		B = A;
		addnode(&B, 8, PC_CLMAP_W_UNSET);   /* a ninth node joins */
		for (i = 0; i < 20000; i++) {
			uint64_t kh = kh_of(i);

			if (pc_clplace_owner(&A, kh) != pc_clplace_owner(&B, kh))
				moved++;
		}
		/* only keys whose argmax became the new node should move:
		 * about 1/9 = 11%.  Anything near 100% means the function is
		 * not rendezvous at all. */
		CHK(moved > 20000 * 6 / 100 && moved < 20000 * 17 / 100,
			"adding 1 node to 8 moved %d of 20000 keys (%d%%), "
			"expected ~11%%", moved, moved * 100 / 20000);
	}

	/* ---- 5. only placeable nodes ------------------------------- */
	mkmap(&M, 3, PC_CLMAP_W_UNSET);
	M.node[1].state = PC_CLMAP_ST_RECOVERING;
	M.node[2].admin_weight = 0;                  /* drained */
	for (i = 0; i < 500; i++)
		if (pc_clplace_owner(&M, kh_of(i)) != M.node[0].node_id) break;
	CHK(i == 500,
		"a recovering or drained node was given keys");

	mkmap(&M, 2, PC_CLMAP_W_UNSET);
	M.node[0].state = PC_CLMAP_ST_STARTING;
	M.node[1].admin_weight = 0;
	CHK(pc_clplace_owner(&M, kh_of(1)) == 0,
		"a map with nothing placeable still named an owner");

	/* ---- 6. rank: distinct, ordered, and short when it must be -- */
	mkmap(&M, 5, PC_CLMAP_W_UNSET);
	n = pc_clplace_rank(&M, kh_of(7), r, 3);
	CHK(n == 3, "rank returned %d of 3", n);
	CHK(r[0] != r[1] && r[1] != r[2] && r[0] != r[2],
		"rank returned the same node twice - an EC stripe would put "
		"two chunks on one node");
	CHK(r[0] == pc_clplace_owner(&M, kh_of(7)),
		"rank[0] is not the owner");

	mkmap(&M, 2, PC_CLMAP_W_UNSET);
	n = pc_clplace_rank(&M, kh_of(7), r, 6);
	CHK(n == 2,
		"rank invented %d nodes from a 2-node map - an EC write must "
		"fail to place rather than place badly", n);

	/* ---- 7. GOLDEN VECTORS: the hash and the mix must not move -
	 * These are computed independently from the algorithms cluster.c
	 * uses, and hard-coded.  Without them the migration check above is
	 * self-referential: its reference implementation calls the same
	 * mix, so changing the mix moves BOTH sides together and the test
	 * stays green while every key in the fleet relocates.  Caught by
	 * breaking the mix and watching nothing fail. */
	/* the key->slot half of the contract is pinned in slottest, against
	 * Redis's own published vectors; the mix is pinned here. */
	CHK(pc_key_slot("foo", 3) == 12182,
		"the key slot changed - every key in the fleet would move "
		"(got %u)", (unsigned)pc_key_slot("foo", 3));
	CHK(pc_clplace_mix(0x674C5FBB8347611Eull, 0x0A000001u, 17000) ==
		0x1A1CBC68E606B036ull,
		"the node mix changed - every key in the fleet would move "
		"(got %016llx)",
		(unsigned long long)pc_clplace_mix(0x674C5FBB8347611Eull,
			0x0A000001u, 17000));
	CHK(pc_clplace_mix(0x674C5FBB8347611Eull, 0x7F000001u, 17501) ==
		0x75B5004FCEA5D813ull, "the node mix changed");

	/* ---- 8. the tie-break, which real hashes never exercise ----
	 * Two nodes at the same address and port score identically, so the
	 * only thing separating them is the id.  Without it two nodes could
	 * answer differently for the same key depending on map order. */
	{
		static struct pc_clmap T;
		uint16_t o1, o2;

		mkmap(&T, 2, PC_CLMAP_W_UNSET);
		T.node[1].addr = T.node[0].addr;
		T.node[1].cluster_port = T.node[0].cluster_port;
		o1 = pc_clplace_owner(&T, kh_of(3));
		/* same map, entries swapped: the answer must not depend on
		 * the order they happen to appear in */
		{
			struct pc_clmap_node t = T.node[0];

			T.node[0] = T.node[1];
			T.node[1] = t;
		}
		o2 = pc_clplace_owner(&T, kh_of(3));
		CHK(o1 == o2 && o1 != 0,
			"two nodes scoring identically resolved differently "
			"depending on map order (%u vs %u) - the tie-break is "
			"what stops that", o1, o2);
	}

	/* ---- 9. the fixed-point log term --------------------------- */
	CHK(pc_clplace_dterm(0) == ((uint64_t)64 << PC_CLPLACE_FBITS),
		"h=0 is not the lowest possible score");
	CHK(pc_clplace_dterm(~0ull) < pc_clplace_dterm(1ull << 32),
		"the D term is not decreasing in h");
	CHK(pc_clplace_dterm(1ull << 32) < pc_clplace_dterm(1ull << 8),
		"the D term is not decreasing across exponents");
	CHK(pc_clplace_dterm(~0ull) > 0,
		"the largest h gives a zero D term - the comparison would "
		"lose its denominator");
	{
		/* monotone across the whole range, which is what makes the
		 * uniform-weight case exactly the unweighted argmax */
		uint64_t prev = pc_clplace_dterm(1);
		int mono = 1;

		for (i = 1; i < 64; i++) {
			uint64_t d = pc_clplace_dterm(1ull << i);

			if (d >= prev) { mono = 0; break; }
			prev = d;
		}
		CHK(mono, "the D term is not strictly monotone (broke at 2^%d)",
			i);
	}

	printf("clplacetest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
