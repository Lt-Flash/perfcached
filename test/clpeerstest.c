/*
 * clpeerstest.c - the peer table (M1).
 *
 * The table is where membership is decided, and every case here is a way
 * that decision could go wrong without a fleet ever noticing:
 *   - liveness read one millisecond off the boundary, so a peer is
 *     mourned early or kept past death
 *   - a slot reused for a new address inheriting its last occupant's
 *     replication flags, which is S102: the new peer never gets the
 *     copies the stale flags say it already has
 *   - an id looked up and answered by a DEAD slot that still holds it
 *   - duplicate-id arbitration firing inside its grace window, which
 *     turns this node's own stale view into an accusation
 *
 * No socket, no second daemon, no clock: every entry point takes `now`,
 * so the boundary is placed exactly rather than slept through.
 *
 * Build: cc -o clpeerstest test/clpeerstest.c src/clpeers.o
 */
#include <stdio.h>
#include <string.h>

#include "../src/clpeers.h"

static int pass, fail;
static void ok(const char *what) { pass++; (void)what; }
static void bad(const char *what)
{
	fail++;
	printf("  FAIL %s\n", what);
}
static void check(int cond, const char *what) { cond ? ok(what) : bad(what); }

static struct clpeers PT;

static struct sockaddr_in A(unsigned char last)
{
	struct sockaddr_in s;

	memset(&s, 0, sizeof s);
	s.sin_family = AF_INET;
	s.sin_port = htons(6479);
	s.sin_addr.s_addr = htonl(0x0A160000u | last);
	return s;
}

/* a table with n slots, every one live at `now` and holding id 100+i */
static void fill(int n, long long now)
{
	int i;

	memset(&PT, 0, sizeof PT);
	for (i = 0; i < n; i++) {
		PT.peers[i].addr = A((unsigned char)(i + 1));
		PT.peers[i].node = 100 + i;
		PT.peers[i].last_seen_ms = now;
	}
	PT.n_peers = n;
}

int main(void)
{
	const long long now = 1000000;
	struct sockaddr_in a;
	struct peer *p;
	int created, i;

	printf("=== clpeerstest ===\n");

	/* ---- liveness at the boundary ---------------------------------- */
	fill(1, now);
	check(clpeers_live(&PT.peers[0], now), "a peer seen now is live");
	PT.peers[0].last_seen_ms = now - (PEER_UP_MS - 1);
	check(clpeers_live(&PT.peers[0], now), "one ms inside PEER_UP_MS is live");
	PT.peers[0].last_seen_ms = now - PEER_UP_MS;
	check(!clpeers_live(&PT.peers[0], now),
		"exactly PEER_UP_MS is NOT live - the test is strict <");
	PT.peers[0].last_seen_ms = now;
	PT.peers[0].node = 0;
	check(!clpeers_live(&PT.peers[0], now),
		"a slot with no node id is never live, however fresh");

	/* ---- counting -------------------------------------------------- */
	fill(4, now);
	PT.peers[1].node = 0;                      /* free slot */
	PT.peers[2].last_seen_ms = now - 2 * PEER_UP_MS;   /* silent */
	check(clpeers_count_live(&PT, now) == 2,
		"count_live skips both the empty slot and the silent one");

	/* ---- lookup by id ---------------------------------------------- */
	fill(3, now);
	p = clpeers_by_id_live(&PT, 101, now);
	check(p == &PT.peers[1], "by_id_live finds the holder");
	PT.peers[1].last_seen_ms = now - 2 * PEER_UP_MS;
	check(!clpeers_by_id_live(&PT, 101, now),
		"a DEAD slot still holding the id does not answer for it");
	check(!clpeers_by_id_live(&PT, 999, now), "an unheld id answers NULL");

	/* ---- upsert ----------------------------------------------------- */
	fill(2, now);
	created = 7;
	a = A(2);
	p = clpeers_upsert(&PT, &a, &created);
	check(p == &PT.peers[1] && !created,
		"an address already in the table returns its slot, not a new one");
	check(PT.n_peers == 2, "and does not grow the table");

	a = A(9);
	created = 0;
	p = clpeers_upsert(&PT, &a, &created);
	check(p == &PT.peers[2] && created == 1,
		"a new address is appended and reports created");
	check(PT.n_peers == 3, "the table grew by one");

	/* S102: a reclaimed slot must not inherit the last occupant's flags */
	fill(2, now);
	PT.peers[0].node = 0;                      /* died */
	PT.peers[0].backfill[0] = 1;
	PT.peers[0].repl_mark[0] = 4242;
	PT.peers[0].repl_cursor[0] = 99;
	PT.peers[0].repl_bfcycle[0] = 1;
	a = A(77);
	created = 1;
	p = clpeers_upsert(&PT, &a, &created);
	check(p == &PT.peers[0] && !created,
		"a dead slot is reclaimed in place, not appended");
	check(!p->backfill[0] && !p->repl_mark[0] && !p->repl_cursor[0] &&
		!p->repl_bfcycle[0],
		"S102: the reclaimed slot carries none of the old peer\'s "
		"replication state");

	/* a full table refuses rather than overruns */
	memset(&PT, 0, sizeof PT);
	for (i = 0; i < PC_CL_MAXPEER; i++) {
		PT.peers[i].addr = A((unsigned char)i);
		PT.peers[i].node = 1 + i;
		PT.peers[i].last_seen_ms = now;
	}
	PT.n_peers = PC_CL_MAXPEER;
	a = A(200);
	a.sin_port = htons(7000);                  /* an address not present */
	created = 0;
	check(!clpeers_upsert(&PT, &a, &created),
		"a full table of live peers refuses a new address");

	/* ---- duplicate-id arbitration ----------------------------------- */
	memset(&PT, 0, sizeof PT);
	a = A(5);
	check(!clpeers_dup_persisted(&PT, &a, now),
		"the first sighting of a duplicate is not yet persistent");
	check(!clpeers_dup_persisted(&PT, &a, now + DUP_GRACE_MS - 1),
		"inside the grace window it is still not persistent");
	check(clpeers_dup_persisted(&PT, &a, now + DUP_GRACE_MS),
		"at DUP_GRACE_MS it is persistent and may be acted on");
	check(clpeers_dup_seen_recent(&PT, &a.sin_addr, now + 1),
		"seen_recent is true while the window is open");
	check(!clpeers_dup_seen_recent(&PT, &a.sin_addr, now + DUP_GRACE_MS),
		"and false once it has closed");
	clpeers_dup_forget(&PT, &a);
	check(!clpeers_dup_seen_recent(&PT, &a.sin_addr, now + 1),
		"forget clears the sighting");
	check(!clpeers_dup_persisted(&PT, &a, now + DUP_GRACE_MS),
		"and the window restarts from the next sighting");

	printf("clpeerstest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
