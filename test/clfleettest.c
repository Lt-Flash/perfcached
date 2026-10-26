/*
 * clfleettest.c - one collection's figures for the whole fleet (S164),
 * the rules without the peer table (M15, wave 4).
 *
 * Until this, the only thing that checked these rules was fleetcolstest's
 * three daemons, and only for eager.  The failure the rules exist for was
 * a page reading 6,129 entries on a fleet holding 2,043 - a sum where a
 * max belonged - and the spread and store bases had no check at all.
 *
 * Build: cc -o clfleettest test/clfleettest.c src/clfleet.o
 */
#include <stdio.h>
#include <string.h>

#include "../src/clfleet.h"

static int pass, fail;
static void chk(int cond, const char *what)
{
	if (cond) { pass++; return; }
	fail++;
	printf("  FAIL %s\n", what);
}

static struct clmemb_col C(uint64_t entries, uint64_t expired, uint64_t hits,
		uint64_t stores)
{
	struct clmemb_col c;

	memset(&c, 0, sizeof c);
	c.hash = 0xABCDEF;
	c.entries = entries;
	c.expired = expired;
	c.hits = hits;
	c.misses = hits + 1;
	c.stores = stores;
	c.removes = stores / 2;
	return c;
}

/* this node + peers[0..n) (NULL = a live peer not reporting) */
static struct pc_col_fleet run(int basis, int k, struct clmemb_col own,
		struct clmemb_col **peers, int n)
{
	struct clfleet a;
	struct pc_col_fleet f;
	int i;

	clfleet_begin(&a, basis, &own);
	for (i = 0; i < n; i++)
		clfleet_member(&a, peers[i]);
	clfleet_end(&a, k, &f);
	return f;
}

int main(void)
{
	struct clmemb_col p1, p2;
	struct clmemb_col *v[3];
	struct pc_col_fleet f;

	/* ---- eager: the fullest member, never the copies --------------- */
	p1 = C(2043, 5, 10, 100); p2 = C(2043, 4, 20, 200);
	v[0] = &p1; v[1] = &p2;
	f = run(PC_FLEET_FULLEST, 0, C(2043, 6, 30, 300), v, 2);
	chk(f.entries == 2043 && f.copies == 6129, "eager: 2,043 on each of three reads 2,043, the 6,129 are copies");
	chk(f.expired == 6, "eager: expired is the fullest member's, not the sum");
	chk(f.hits == 60 && f.misses == 63 && f.stores == 600 && f.removes == 300,
		"client events sum: hits, misses, stores, removes");
	chk(f.members == 3 && f.reporting == 3 && f.basis == PC_FLEET_FULLEST, "3 members, 3 reporting, basis kept");
	p2 = C(1500, 0, 0, 0);
	f = run(PC_FLEET_FULLEST, 0, C(2043, 0, 0, 0), v, 2);
	chk(f.entries == 2043, "eager: a member still catching up does not pull the figure down");

	/* ---- store: the fullest member, a lower bound ------------------- */
	p1 = C(5, 0, 0, 0); p2 = C(9, 0, 0, 0);
	f = run(PC_FLEET_AT_LEAST, 0, C(2, 0, 0, 0), v, 2);
	chk(f.entries == 9 && f.copies == 16, "store: the fullest member (9), the copies beside (16)");

	/* ---- spread: the copies over K ---------------------------------- */
	p1 = C(10, 2, 0, 0); p2 = C(0, 0, 0, 0);
	f = run(PC_FLEET_PER_K, 2, C(10, 2, 0, 0), v, 2);
	chk(f.entries == 10 && f.expired == 2, "spread K=2: 20 copies are 10 entries");
	p1 = C(7, 0, 0, 0);
	v[1] = NULL;
	f = run(PC_FLEET_PER_K, 3, C(0, 0, 0, 0), v, 2);
	chk(f.reporting == 2 && f.entries == 4, "spread K=3 with 2 reporting: divides by 2, and 7/2 rounds to 4");
	v[1] = &p2;
	f = run(PC_FLEET_PER_K, 0, C(3, 0, 0, 0), v, 2);
	chk(f.entries == 10, "spread with no K: divides by 1, never by 0");

	/* ---- shard / proxy: one copy, the sum --------------------------- */
	p1 = C(3, 1, 0, 0); p2 = C(4, 1, 0, 0);
	f = run(PC_FLEET_SUM, 0, C(5, 1, 0, 0), v, 2);
	chk(f.entries == 12 && f.expired == 3, "shard: the sum, entries and expired");

	/* ---- a member that does not report ------------------------------ */
	p1 = C(100, 0, 7, 0);
	v[0] = &p1; v[1] = NULL; v[2] = NULL;
	f = run(PC_FLEET_FULLEST, 0, C(50, 0, 1, 0), v, 3);
	chk(f.members == 4 && f.reporting == 2, "a live peer with no block is a member, not reporting");
	chk(f.entries == 100 && f.hits == 8, "and adds nothing to any figure");

	/* ---- finding a member's block ----------------------------------- */
	{
		struct clmemb_col cols[CLMEMB_COLS_MAX + 2];
		int i;

		memset(cols, 0, sizeof cols);
		for (i = 0; i < CLMEMB_COLS_MAX + 2; i++)
			cols[i].hash = 1000 + (uint64_t)i;
		chk(clfleet_find(cols, 3, 1002) == &cols[2], "find: the block for the hash");
		chk(clfleet_find(cols, 3, 1003) == NULL, "find: not past the member's count");
		chk(clfleet_find(cols, CLMEMB_COLS_MAX + 2, 1000 + CLMEMB_COLS_MAX) == NULL,
			"find: never past CLMEMB_COLS_MAX, whatever the count says");
		chk(clfleet_find(cols, 0, 1000) == NULL, "find: a member that reported nothing");
	}

	chk(!strcmp(pc_fleet_basis_name(PC_FLEET_FULLEST), "fullest") &&
	    !strcmp(pc_fleet_basis_name(PC_FLEET_AT_LEAST), "at_least") &&
	    !strcmp(pc_fleet_basis_name(PC_FLEET_PER_K), "per_k") &&
	    !strcmp(pc_fleet_basis_name(PC_FLEET_SUM), "sum"), "the basis names /stats prints");

	printf("clfleettest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
