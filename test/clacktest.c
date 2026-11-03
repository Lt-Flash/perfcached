/* clacktest.c - 12ny AT-3: the watermarks (clack.c).
 *
 *   A  in order: the confirmed mark follows the WAL's sync, never ahead
 *   B  out of order and a hole: the applied mark stops below the hole,
 *      the gap is reported exactly, a fill connects what was held
 *   C  duplicates, seq 0, a seq past the window (FAR: not recorded, and
 *      the gap reaches it)
 *   D  durability folded conservatively: a fill applied later waits for
 *      its own WAL record, and what it connects waits with it
 *   E  durable_on_apply (fsync = no) and walseq 0: confirmed on apply
 *   F  a restore from an OLDER snapshot lowers the mark; what lies above
 *      it counts again
 *   G  spaces: one origin's two incarnations are separate; the table's
 *      bound refuses a new one, a drop makes room
 *   H  a full queue of pending pairs still confirms, only later
 *   J  AT-7: a floor - a space never heard of starts at it, a floor
 *      below the mark is nothing, pending pairs inside it never take the
 *      confirmed mark back
 *   I  model: 200,000 deliveries shuffled within blocks of 300, 1% lost
 *      and sent again later (oldest first), random syncs - the confirmed mark never passes the
 *      exact one, and equals it once the WAL has synced everything */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/clack.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static uint64_t mk(struct clack *a, uint64_t oid, uint32_t inc)
{
	uint64_t m = ~0ull;

	return clack_mark(a, oid, inc, &m) ? m : ~0ull;
}

struct cnt { unsigned int n; uint64_t marks; };
static int each_cb(uint64_t oid, uint32_t incarn, uint64_t mark,
		uint64_t applied, uint64_t hi, void *ctx)
{
	struct cnt *c = ctx;

	(void)oid; (void)incarn; (void)applied; (void)hi;
	c->n++;
	c->marks += mark;
	return 0;
}

int main(void)
{
	struct clack *a = clack_new(0), *b;
	struct clack_stats st;
	struct cnt cn;
	uint64_t f = 0, t = 0, s;
	unsigned int i;
	int ok;

	/* ---- A: in order */
	for (s = 1; s <= 5; s++)
		clack_applied(a, 7, 1, s, 100 + s);       /* WAL 101..105 */
	CHK(mk(a, 7, 1) == 0, "A: 5 applied, nothing synced: confirmed 0 (%llu)",
		(unsigned long long)mk(a, 7, 1));
	clack_synced(a, 103);
	CHK(mk(a, 7, 1) == 3, "A: synced through WAL 103: confirmed 3 (%llu)",
		(unsigned long long)mk(a, 7, 1));
	clack_synced(a, 102);                          /* never backwards */
	CHK(mk(a, 7, 1) == 3, "A: an older sync changes nothing");
	clack_synced(a, 105);
	CHK(mk(a, 7, 1) == 5, "A: synced through 105: confirmed 5");
	CHK(mk(a, 8, 1) == ~0ull, "A: a space never heard of has no mark");

	/* ---- B: a hole */
	clack_applied(a, 7, 1, 6, 0);
	clack_applied(a, 7, 1, 8, 0);
	clack_applied(a, 7, 1, 9, 0);
	clack_applied(a, 7, 1, 12, 0);
	CHK(mk(a, 7, 1) == 6, "B: 6, 8, 9, 12 applied: confirmed 6, below the hole");
	ok = clack_gap(a, 7, 1, &f, &t);
	CHK(ok && f == 7 && t == 7, "B: the first hole is [7, 7] (%d [%llu, %llu])",
		ok, (unsigned long long)f, (unsigned long long)t);
	clack_applied(a, 7, 1, 7, 0);
	CHK(mk(a, 7, 1) == 9, "B: 7 filled: confirmed 9 (8 and 9 connected)");
	ok = clack_gap(a, 7, 1, &f, &t);
	CHK(ok && f == 10 && t == 11, "B: the next hole is [10, 11]");
	clack_applied(a, 7, 1, 11, 0);
	clack_applied(a, 7, 1, 10, 0);
	CHK(mk(a, 7, 1) == 12 && !clack_gap(a, 7, 1, &f, &t),
		"B: filled in reverse: confirmed 12, no hole");

	/* ---- C: duplicates, 0, FAR */
	clack_get_stats(a, &st);
	CHK(clack_applied(a, 7, 1, 12, 0) == CLACK_DUP &&
	    clack_applied(a, 7, 1, 3, 0) == CLACK_DUP, "C: at or below the mark: DUP");
	clack_applied(a, 7, 1, 14, 0);
	CHK(clack_applied(a, 7, 1, 14, 0) == CLACK_DUP, "C: one held ahead, again: DUP");
	CHK(clack_applied(a, 7, 1, 0, 0) == CLACK_BAD, "C: seq 0 refused");
	CHK(clack_applied(a, 7, 1, 12 + CLACK_WIN + 1, 0) == CLACK_FAR,
		"C: mark + window + 1: FAR, not recorded");
	CHK(clack_applied(a, 7, 1, 12 + CLACK_WIN, 0) == CLACK_NEW,
		"C: mark + window: recorded");
	ok = clack_gap(a, 7, 1, &f, &t);
	CHK(ok && f == 13 && t == 13, "C: the gap is still the first hole [13, 13]");
	for (s = 13; s < 12 + CLACK_WIN; s++)
		if (s != 14)
			clack_applied(a, 7, 1, s, 0);
	CHK(mk(a, 7, 1) == 12 + CLACK_WIN, "C: filled to the window's end: confirmed %llu",
		(unsigned long long)mk(a, 7, 1));
	ok = clack_gap(a, 7, 1, &f, &t);
	CHK(ok && f == 13 + CLACK_WIN && t == 13 + CLACK_WIN,
		"C: the FAR seq was not recorded: the gap asks for it [%llu, %llu]",
		(unsigned long long)f, (unsigned long long)t);
	clack_get_stats(a, &st);
	CHK(st.far == 1 && st.dups >= 3, "C: counted: far %llu, dups %llu",
		st.far, st.dups);
	clack_free(a);

	/* ---- D: durability folded conservatively */
	a = clack_new(0);
	clack_applied(a, 1, 1, 1, 10);
	clack_applied(a, 1, 1, 3, 12);
	clack_applied(a, 1, 1, 2, 20);                 /* the fill, applied later */
	clack_synced(a, 12);
	CHK(mk(a, 1, 1) == 1, "D: 1@10, 3@12, 2@20, synced 12: confirmed 1 (%llu)",
		(unsigned long long)mk(a, 1, 1));
	clack_synced(a, 19);
	CHK(mk(a, 1, 1) == 1, "D: synced 19: still 1 - 2's record is not durable");
	clack_synced(a, 20);
	CHK(mk(a, 1, 1) == 3, "D: synced 20: confirmed 3");
	clack_applied(a, 1, 1, 5, 40);                 /* ahead, late in the WAL */
	clack_applied(a, 1, 1, 4, 30);
	clack_synced(a, 30);
	CHK(mk(a, 1, 1) == 3, "D: 5@40 held, 4@30 connects it, synced 30: still 3 "
		"(folded: one max for what was ahead)");
	clack_synced(a, 40);
	CHK(mk(a, 1, 1) == 5, "D: synced 40: confirmed 5");
	clack_free(a);

	/* ---- E: confirmed on apply */
	a = clack_new(1);
	clack_applied(a, 1, 1, 1, 999);
	clack_applied(a, 1, 1, 2, 1000);
	clack_get_stats(a, &st);
	CHK(mk(a, 1, 1) == 2 && st.durable_on_apply, "E: durable_on_apply: confirmed 2 with nothing synced, and it says so");
	clack_free(a);
	a = clack_new(0);
	clack_applied(a, 1, 1, 1, 0);
	CHK(mk(a, 1, 1) == 1, "E: walseq 0 (no WAL, or a replay): confirmed at once");
	clack_free(a);

	/* ---- F: restore lowers */
	a = clack_new(0);
	for (s = 1; s <= 50; s++)
		clack_applied(a, 9, 3, s, 0);
	clack_restore(a, 9, 3, 20);
	CHK(mk(a, 9, 3) == 20, "F: restored from an older snapshot: the mark is 20, not 50");
	CHK(clack_applied(a, 9, 3, 21, 0) == CLACK_NEW && mk(a, 9, 3) == 21,
		"F: 21 counts again, confirmed 21");
	CHK(clack_applied(a, 9, 3, 20, 0) == CLACK_DUP, "F: 20 is below the restored mark: DUP");
	clack_free(a);

	/* ---- G: spaces */
	a = clack_new(0);
	clack_applied(a, 5, 100, 1, 0);
	clack_applied(a, 5, 200, 1, 0);
	clack_applied(a, 5, 200, 2, 0);
	CHK(mk(a, 5, 100) == 1 && mk(a, 5, 200) == 2,
		"G: one origin, two incarnations: two spaces (1 and 2)");
	for (i = 2; i < CLACK_SPACES; i++)
		clack_applied(a, 1000 + i, 1, 1, 0);
	CHK(clack_applied(a, 99999, 1, 1, 0) == CLACK_FULL,
		"G: %d spaces held: a new one is refused", CLACK_SPACES);
	clack_drop(a, 5, 100);
	CHK(mk(a, 5, 100) == ~0ull && clack_applied(a, 99999, 1, 1, 0) == CLACK_NEW &&
	    mk(a, 5, 200) == 2, "G: a dropped space is forgotten and makes room; the other stays");
	memset(&cn, 0, sizeof cn);
	clack_each(a, each_cb, &cn);
	clack_get_stats(a, &st);
	CHK(cn.n == CLACK_SPACES && st.spaces == CLACK_SPACES && st.full == 1,
		"G: each walks %u spaces; full counted %llu", cn.n, st.full);
	clack_free(a);

	/* ---- H: the pending queue overflows */
	a = clack_new(0);
	for (s = 1; s <= 10 * CLACK_PEND; s++)
		clack_applied(a, 1, 1, s, s);             /* a new max each time */
	clack_synced(a, 5);
	CHK(mk(a, 1, 1) <= 5, "H: %d pairs pending, synced 5: confirmed %llu, not past 5",
		10 * CLACK_PEND, (unsigned long long)mk(a, 1, 1));
	clack_synced(a, 10 * CLACK_PEND);
	CHK(mk(a, 1, 1) == 10 * CLACK_PEND, "H: synced through all: confirmed %d",
		10 * CLACK_PEND);
	clack_free(a);

	/* ---- J: AT-7 - a floor */
	a = clack_new(0);
	CHK(!clack_known(a, 4, 4) && clack_floor(a, 4, 4, 50) == CLACK_NEW &&
		clack_known(a, 4, 4) && mk(a, 4, 4) == 50,
		"J: a space never heard of, floored at 50: known, confirmed 50 at once");
	CHK(clack_applied(a, 4, 4, 50, 0) == CLACK_DUP && clack_applied(a, 4, 4, 51, 9) == CLACK_NEW,
		"J: 50 is inside the floor (DUP), 51 counts");
	CHK(clack_floor(a, 4, 4, 20) == CLACK_DUP && mk(a, 4, 4) == 50,
		"J: a floor below the mark changes nothing");
	clack_free(a);
	a = clack_new(0);
	clack_applied(a, 5, 5, 1, 10);
	clack_applied(a, 5, 5, 2, 11);              /* pending (2, 11) */
	clack_applied(a, 5, 5, 9, 12);              /* held ahead */
	clack_floor(a, 5, 5, 8);
	CHK(mk(a, 5, 5) == 8, "J: 1, 2 pending, 9 ahead, floored at 8: confirmed 8 (%llu), 9 connected but not durable",
		(unsigned long long)mk(a, 5, 5));
	clack_synced(a, 11);
	CHK(mk(a, 5, 5) == 8, "J: the sync of the pairs inside the floor does not take it back (%llu)",
		(unsigned long long)mk(a, 5, 5));
	clack_synced(a, 12);
	CHK(mk(a, 5, 5) == 9, "J: synced through 9's record: confirmed 9");
	clack_free(a);

	/* ---- I: model */
	{
		enum { N = 200000, R = 300, L = 4096 };
		uint64_t *order = malloc(N * sizeof *order), *w = calloc(N + 2, sizeof *w);
		unsigned char *got = calloc(N + 2, 1);
		uint64_t wal = 0, syn = 0, ea = 0, ec = 0, lost[L], m;
		unsigned int nlost = 0, k, j, over = 0, seed = 12345;

		b = clack_new(0);
		for (k = 0; k < N; k++)
			order[k] = k + 1;
		for (k = 0; k < N; k += R)             /* shuffled within blocks of R */
			for (j = (N - k < R ? N - k : R) - 1; j > 0; j--) {
				unsigned int r;
				uint64_t x;

				seed = seed * 1103515245u + 12345u;
				r = (seed >> 8) % (j + 1);
				x = order[k + j]; order[k + j] = order[k + r]; order[k + r] = x;
			}
		for (k = 0; k < N; k++) {
			uint64_t q = order[k];

			seed = seed * 1103515245u + 12345u;
			if ((seed >> 8) % 100 == 0 && nlost < L)
				lost[nlost++] = q;             /* lost; sent again later */
			else {
				w[q] = ++wal;
				got[q] = 1;
				clack_applied(b, 42, 1, q, w[q]);
			}
			if (nlost && (seed >> 16) % 50 == 0) {   /* the oldest gap answered */
				uint64_t g = lost[0];

				memmove(lost, lost + 1, --nlost * sizeof lost[0]);
				w[g] = ++wal;
				got[g] = 1;
				clack_applied(b, 42, 1, g, w[g]);
			}
			if ((seed >> 4) % 7 == 0) {        /* a sync, sometimes an old one */
				uint64_t back = (seed >> 12) % 40;

				clack_synced(b, wal > back ? wal - back : 0);
				if (wal > back && wal - back > syn)
					syn = wal - back;
			}
			/* exact: contiguous applied, and confirmed = contiguous with
			 * every record synced */
			while (ea < N && got[ea + 1])
				ea++;
			while (ec < ea && w[ec + 1] <= syn)
				ec++;
			m = 0;
			clack_mark(b, 42, 1, &m);
			if (m > ec)
				over++;
		}
		for (j = 0; j < nlost; j++) {
			w[lost[j]] = ++wal;
			got[lost[j]] = 1;
			clack_applied(b, 42, 1, lost[j], w[lost[j]]);
		}
		clack_synced(b, wal);
		m = 0;
		clack_mark(b, 42, 1, &m);
		CHK(!over, "I: %d deliveries shuffled within %d, ~1%% lost and re-sent: "
			"the confirmed mark never passed the exact one (%u times it did)",
			N, R, over);
		CHK(m == N, "I: everything delivered and synced: confirmed %llu of %d",
			(unsigned long long)m, N);
		clack_get_stats(b, &st);
		CHK(st.far == 0 && st.applied == N, "I: nothing FAR; applied %llu", st.applied);
		clack_free(b);
		free(order); free(w); free(got);
	}

	printf("clacktest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
