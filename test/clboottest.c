/*
 * clboottest.c - the S83 bootstrap decision (M4).
 *
 * A node that joins holding nothing must pull a full walk before it
 * serves, and every case here is a way that could go wrong quietly:
 *   - giving up after the wrong number of rounds, so a fleet that would
 *     have served the walk is abandoned (or never abandoned)
 *   - a re-pick that finds nobody reporting READY on the next tick
 *     instead of waiting the beat the code says it waits - which is what
 *     boot_tick actually did, the extension being a dead store
 *   - pending and round_failed read from different instants, which
 *     boot_tick allowed by taking the lock twice in a row
 *
 * The clock is injected and the handoff lock is supplied by the caller,
 * so none of this needs a fleet, a socket or a bulk thread.
 *
 * Build: cc -o clboottest test/clboottest.c src/clboot.o -lpthread
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "../src/clboot.h"

static int pass, fail;
static void chk(int cond, const char *what)
{
	if (cond) { pass++; return; }
	fail++;
	printf("  FAIL %s\n", what);
}

static struct clboot B;
static pthread_mutex_t MX = PTHREAD_MUTEX_INITIALIZER;

static struct sockaddr_in A(unsigned char last)
{
	struct sockaddr_in s;

	memset(&s, 0, sizeof s);
	s.sin_family = AF_INET;
	s.sin_port = htons(6479);
	s.sin_addr.s_addr = htonl(0x0A160000u | last);
	return s;
}

int main(void)
{
	const long long now = 500000;
	struct sockaddr_in cand[BOOT_MAX_CAND], addr[3];
	int node[BOOT_MAX_CAND], id[3] = { 11, 22, 33 };
	int pending, failed, i, n;

	printf("=== clboottest ===\n");
	memset(&B, 0, sizeof B);

	/* ---- A. arming and done ------------------------------------------ */
	clboot_arm(&B, now);
	chk(B.deadline_ms == now + BOOT_WAIT_MS, "A1 arming sets the give-up deadline");
	chk(B.pick_ms == now + BOOT_PICK_MS, "A1 and the no-pick-before mark");
	chk(!clboot_done(&B), "A2 a fresh node has not bootstrapped");
	clboot_set_done(&B);
	chk(clboot_done(&B), "A2 and set_done says so");

	/* ---- B. before the pick ------------------------------------------- */
	memset(&B, 0, sizeof B);
	clboot_arm(&B, now);
	chk(clboot_before_pick(&B, now, 0, 0) == CLBOOT_WAIT,
		"B1 nothing happens before the pick mark");
	chk(clboot_before_pick(&B, now + BOOT_PICK_MS, 0, 0) == CLBOOT_PICK,
		"B2 at the pick mark the caller is asked to pick");
	chk(clboot_before_pick(&B, now + BOOT_PICK_MS, 1, 0) == CLBOOT_WAIT,
		"B3 but not while a pull is already with the bulk thread");
	memset(&B, 0, sizeof B);
	clboot_arm(&B, now);
	chk(clboot_before_pick(&B, now + BOOT_PICK_MS, 0, 1) == CLBOOT_PICK &&
		B.rounds == 1, "B4 the first failed round re-picks");
	chk(clboot_before_pick(&B, now + BOOT_PICK_MS, 0, 1) == CLBOOT_PICK &&
		B.rounds == 2, "B4 so does the second");
	chk(clboot_before_pick(&B, now + BOOT_PICK_MS, 0, 1) == CLBOOT_GIVE_UP,
		"B5 the third gives up");
	chk(B.rounds == 3 && B.deadline_ms == 0,
		"B5 after three rounds, and the deadline is cleared");

	/* ---- C. after the pick -------------------------------------------- */
	memset(&B, 0, sizeof B);
	clboot_arm(&B, now);
	chk(clboot_after_pick(&B, now, 0, 1) == CLBOOT_WAIT && B.deadline_ms == 0,
		"C1 a pull on its way clears the deadline and waits");
	memset(&B, 0, sizeof B);
	clboot_arm(&B, now);
	chk(clboot_after_pick(&B, now, 0, 2) == CLBOOT_EMPTY,
		"C2 every ready peer holding nothing means there is nothing to pull");
	chk(clboot_done(&B) && B.deadline_ms == 0,
		"C2 and that counts as bootstrapped");
	memset(&B, 0, sizeof B);
	clboot_arm(&B, now);
	chk(clboot_after_pick(&B, now, 0, 0) == CLBOOT_WAIT,
		"C3 nobody qualifies yet, but the deadline has not passed");
	chk(clboot_after_pick(&B, now + BOOT_WAIT_MS, 0, 0) == CLBOOT_DEADLINE,
		"C4 at the deadline it gives up and relies on the push backfill");
	chk(B.deadline_ms == 0, "C4 clearing the deadline behind it");

	/* C5 is the dead store boot_tick had: the branch says "wait a beat
	 * more" and then the next branch zeroed the deadline, so the node
	 * gave up on the following tick instead. */
	memset(&B, 0, sizeof B);
	clboot_arm(&B, now);
	chk(clboot_after_pick(&B, now, 1, 0) == CLBOOT_WAIT,
		"C5 a failed round with nobody to re-pick waits");
	chk(B.deadline_ms == now + BOOT_WAIT_MS,
		"C5 and the deadline is EXTENDED, not cleared - the dead store");
	chk(clboot_after_pick(&B, now + 1, 0, 0) == CLBOOT_WAIT,
		"C5 so the next tick is still waiting, not giving up");

	/* ---- D. the handoff, under the borrowed lock ---------------------- */
	memset(&B, 0, sizeof B);
	for (i = 0; i < 3; i++)
		addr[i] = A((unsigned char)(i + 1));
	chk(!clboot_pending(&B, &MX), "D1 nothing is pending to begin with");
	clboot_handoff(&B, &MX, addr, id, 3);
	chk(clboot_pending(&B, &MX), "D1 the handoff marks a pull pending");
	n = clboot_take_cands(&B, &MX, cand, node);
	chk(n == 3 && node[0] == 11 && node[2] == 33 &&
		cand[1].sin_addr.s_addr == addr[1].sin_addr.s_addr,
		"D2 the bulk side takes the candidates in order");
	clboot_pull_ok(&B, &MX);
	chk(!clboot_pending(&B, &MX) && clboot_done(&B),
		"D3 a clean pull clears pending and marks the node bootstrapped");
	memset(&B, 0, sizeof B);
	clboot_handoff(&B, &MX, addr, id, 3);
	clboot_pull_failed(&B, &MX);
	chk(!clboot_pending(&B, &MX), "D4 a failed round clears pending");
	chk(clboot_take_round_failed(&B, &MX) == 1, "D4 and reports the failure");
	chk(clboot_take_round_failed(&B, &MX) == 0,
		"D5 exactly once - it is read and cleared");

	/* D6: pending and round_failed in ONE acquisition, which boot_tick
	 * did not do - it took the lock twice in a row for these two. */
	memset(&B, 0, sizeof B);
	clboot_handoff(&B, &MX, addr, id, 1);
	clboot_pull_failed(&B, &MX);
	clboot_tick_state(&B, &MX, &pending, &failed);
	chk(!pending && failed == 1, "D6 the tick reads both under one lock");
	clboot_tick_state(&B, &MX, &pending, &failed);
	chk(!failed, "D6 and clears the failure as it goes");

	printf("clboottest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
