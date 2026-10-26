/* clackftest.c - 12ny AT-4: the members' announced marks (clackf.c).
 *
 *   A  reports and the lowest: two members, one space - the min is the
 *      lower; a member that has not reported the space makes it 0
 *   B  a new incarnation forgets what the old one said
 *   C  a mark that goes DOWN is taken, and counted
 *   D  a keepalive without the group clears the member's marks
 *   E  a member forgotten is not counted; asking with it gives 0
 *   F  the bounds: CLACKF_MEMBERS members, CLACKF_SPACES spaces each
 *   G  each walks every mark
 *   H  AT-8: each keepalive's stamp returns the one before
 *   I  AT-11: a mark's moved time, the head, the stuck members */
#include <stdio.h>
#include <stdlib.h>
#include "../src/clackf.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static int count_cb(uint64_t member, uint32_t incarn, uint64_t oid,
		uint32_t sincarn, uint64_t mark, void *ctx)
{
	(void)member; (void)incarn; (void)oid; (void)sincarn; (void)mark;
	(*(unsigned int *)ctx)++;
	return 0;
}

int main(void)
{
	struct clackf *f = clackf_new();
	struct clackf_stats st;
	uint64_t two[2] = { 0xA, 0xB }, three[3] = { 0xA, 0xB, 0xC }, v = 0;
	unsigned int i, n = 0;

	/* ---- A */
	clackf_member(f, 0xA, 1);
	clackf_member(f, 0xB, 1);
	clackf_report(f, 0xA, 0x1111, 7, 100, 0);
	clackf_report(f, 0xB, 0x1111, 7, 80, 0);
	CHK(clackf_min(f, 0x1111, 7, two, 2) == 80, "A: A says 100, B says 80: the lowest is 80");
	CHK(clackf_mark(f, 0xA, 0x1111, 7, &v) && v == 100, "A: A's own mark is 100");
	clackf_member(f, 0xC, 1);
	CHK(clackf_min(f, 0x1111, 7, three, 3) == 0, "A: C has not reported the space: nothing is confirmed (0)");
	CHK(clackf_min(f, 0x1111, 8, two, 2) == 0, "A: another incarnation of the origin is another space: 0");
	CHK(clackf_min(f, 0x1111, 7, two, 0) == 0, "A: no members: 0");

	/* ---- B */
	CHK(clackf_member(f, 0xB, 1) == 0 && clackf_mark(f, 0xB, 0x1111, 7, &v),
		"B: the same incarnation again keeps its marks");
	CHK(clackf_member(f, 0xB, 2) == 1 && !clackf_mark(f, 0xB, 0x1111, 7, &v) &&
		clackf_min(f, 0x1111, 7, two, 2) == 0,
		"B: B restarted (incarnation 2): its marks are gone, nothing confirmed");
	clackf_report(f, 0xB, 0x1111, 7, 90, 0);
	CHK(clackf_min(f, 0x1111, 7, two, 2) == 90, "B: it reports again (90): the lowest is 90");

	/* ---- C */
	CHK(clackf_report(f, 0xA, 0x1111, 7, 50, 0) == 1 && clackf_min(f, 0x1111, 7, two, 2) == 50,
		"C: A goes down to 50 (a state older than it said): taken, the lowest follows");
	clackf_get_stats(f, &st);
	CHK(st.lowered == 1 && st.resets == 1, "C: counted - lowered %llu, resets %llu", st.lowered, st.resets);

	/* ---- D */
	clackf_clear(f, 0xA);
	CHK(!clackf_mark(f, 0xA, 0x1111, 7, &v) && clackf_min(f, 0x1111, 7, two, 2) == 0,
		"D: A's keepalive comes without the group: its marks cleared, nothing confirmed");

	/* ---- E */
	clackf_forget(f, 0xC);
	clackf_get_stats(f, &st);
	CHK(st.members == 2 && clackf_min(f, 0x1111, 7, three, 3) == 0,
		"E: C forgotten - 2 members held; a list that still names it gives 0");
	CHK(clackf_report(f, 0xC, 0x1111, 7, 5, 0) == -1, "E: a report from a member not announced is refused");
	clackf_free(f);

	/* ---- F */
	f = clackf_new();
	for (i = 0; i < CLACKF_MEMBERS; i++)
		clackf_member(f, 1000 + i, 1);
	CHK(clackf_member(f, 99999, 1) == -1, "F: %d members held, the next refused", CLACKF_MEMBERS);
	for (i = 0; i < CLACKF_SPACES; i++)
		clackf_report(f, 1000, 5000 + i, 1, i + 1, 0);
	CHK(clackf_report(f, 1000, 99999, 1, 1, 0) == -1 && clackf_mark(f, 1000, 5000 + CLACKF_SPACES - 1, 1, &v) &&
		v == CLACKF_SPACES, "F: %d spaces for one member, the next refused", CLACKF_SPACES);
	clackf_get_stats(f, &st);
	CHK(st.refused == 2 && st.marks == CLACKF_SPACES, "F: refused %llu, marks %u", st.refused, st.marks);

	/* ---- G */
	clackf_report(f, 1001, 1, 1, 1, 0);
	clackf_report(f, 1002, 1, 1, 1, 0);
	clackf_each(f, count_cb, &n);
	CHK(n == CLACKF_SPACES + 2, "G: each walks all %u marks", n);
	clackf_free(f);

	/* ---- H: AT-8, when a member was heard before */
	f = clackf_new();
	CHK(clackf_heard(f, 0xA, 1000) == 0, "H: a member not in the table: 0, and nothing kept");
	clackf_member(f, 0xA, 1);
	CHK(clackf_heard(f, 0xA, 1000) == 0 && clackf_heard(f, 0xA, 2000) == 1000 &&
		clackf_heard(f, 0xA, 37000) == 2000, "H: each keepalive returns the one before (0, 1000, 2000)");
	clackf_member(f, 0xA, 2);
	CHK(clackf_heard(f, 0xA, 38000) == 37000, "H: a new incarnation keeps the stamp");
	clackf_member(f, 0xB, 1);
	clackf_heard(f, 0xB, 7);
	clackf_forget(f, 0xA);
	CHK(clackf_heard(f, 0xB, 9) == 7, "H: the member moved into a forgotten one's slot keeps its own (7)");
	clackf_free(f);

	/* ---- I: AT-11, when a mark moved, the head, the stuck */
	{
		uint64_t m3[3] = { 0xA, 0xB, 0xC };
		struct clackf_stuck sk[2];
		long long mv = -1;
		int k;

		f = clackf_new();
		for (i = 0; i < 3; i++)
			clackf_member(f, m3[i], 1);
		clackf_report(f, 0xA, 0x77, 1, 10, 1000);
		clackf_report(f, 0xB, 0x77, 1, 10, 1000);
		clackf_report(f, 0xC, 0x77, 1, 10, 1000);
		clackf_report(f, 0xA, 0x77, 1, 10, 5000);      /* unchanged: no move */
		CHK(clackf_moved(f, 0xA, 0x77, 1, &mv) && mv == 1000 && !clackf_moved(f, 0xA, 0x88, 1, &mv),
			"I: a mark reported again unchanged keeps its moved time (1000); an unreported space: 0");
		/* A and B move on, C stays */
		clackf_report(f, 0xA, 0x77, 1, 60, 50000);
		clackf_report(f, 0xB, 0x77, 1, 55, 52000);
		CHK(clackf_max(f, 0x77, 1, m3, 3) == 60, "I: the head among the three is 60");
		k = clackf_stuck(f, 0x77, 1, m3, 3, 60, 0, 70000, 30000, sk, 2);
		CHK(k == 1 && sk[0].member == 0xC && sk[0].mark == 10 && sk[0].moved == 1000,
			"I: C - at 10 since 1000 while A and B moved - is stuck, B (behind but moving) is not (%d)", k);
		CHK(clackf_stuck(f, 0x77, 1, m3, 3, 60, 0, 20000, 30000, sk, 2) == 0,
			"I: not before still_ms has passed (at 20000, 30 s asked)");
		/* nobody moved after C: not stuck, only quiet */
		clackf_report(f, 0xA, 0x99, 1, 5, 1000);
		clackf_report(f, 0xC, 0x99, 1, 3, 1000);
		CHK(clackf_stuck(f, 0x99, 1, m3, 3, 5, 0, 90000, 30000, sk, 2) == 0,
			"I: a member behind on a space nobody has moved on since is quiet, not stuck");
		CHK(clackf_stuck(f, 0x99, 1, m3, 3, 5, 80000, 90000, 30000, sk, 2) == 1 && sk[0].member == 0xC,
			"I: ... unless the caller's own mark moved since (also = 80000) - a two-node fleet");
		/* the list keeps the longest-still first, and only max of them */
		clackf_report(f, 0xA, 0xAA, 1, 9, 1000);
		clackf_report(f, 0xB, 0xAA, 1, 2, 3000);
		clackf_report(f, 0xC, 0xAA, 1, 1, 2000);
		clackf_report(f, 0xA, 0xAA, 1, 20, 60000);
		k = clackf_stuck(f, 0xAA, 1, m3, 3, 20, 0, 90000, 30000, sk, 1);
		CHK(k == 2 && sk[0].member == 0xC, "I: two stuck, one kept: the stiller (C, since 2000) (%d)", k);
		clackf_free(f);
	}

	printf("clackftest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
