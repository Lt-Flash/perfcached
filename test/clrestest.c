/*
 * clrestest.c - id reservations for departed members (M12 slice 1, S90).
 *
 * The table is a courtesy that is invisible when it misbehaves: a
 * reservation that lapses early renumbers a restarting node, one that
 * lapses late holds an id nobody is coming back for, and a claim that
 * matches by address before identity hands a returning node the id of
 * whoever last used its address.  None of that shows up on a healthy
 * fleet, and all of it is a case here.
 *
 * No clock and no TTL of its own: both are injected, so the expiry
 * boundary is placed exactly rather than slept through.
 *
 * Build: cc -o clrestest test/clrestest.c src/clres.o
 */
#include <stdio.h>
#include <string.h>

#include "../src/clres.h"

static int pass, fail;
static void chk(int cond, const char *what)
{
	if (cond) { pass++; return; }
	fail++;
	printf("  FAIL %s\n", what);
}

static struct clres R;

/* an identity whose bytes are asymmetric: i*17 has palindromic nibbles
 * and hid a nibble swap in M13's suite */
static void ident_of(unsigned char *out, int seed)
{
	int i;

	for (i = 0; i < CLRES_IDENT_LEN; i++)
		out[i] = (unsigned char)(seed * 31 + i * 7 + (i & 1) * 0x40);
}

static struct in_addr addr_of(unsigned int v)
{
	struct in_addr a;

	a.s_addr = v;
	return a;
}

int main(void)
{
	const long long T0 = 100000, TTL = 3600000LL;
	unsigned char id1[16], id2[16], id3[16];
	struct in_addr home;
	const char *how;
	int i, n;

	printf("=== clrestest ===\n");
	ident_of(id1, 1); ident_of(id2, 2); ident_of(id3, 3);

	/* ---- A. note and hold -------------------------------------------- */
	memset(&R, 0, sizeof R);
	chk(clres_note(&R, 7, addr_of(0x0101a8c0), id1, 1, T0, TTL) == 1,
		"A1 a note with an id records");
	chk(clres_note(&R, 0, addr_of(0x0201a8c0), id2, 1, T0, TTL) == 0,
		"A2 a note with id 0 records nothing");
	chk(clres_holds(&R, 7, T0), "A3 the id is held");
	chk(!clres_holds(&R, 8, T0), "A4 a different id is not");
	chk(!clres_holds(&R, 0, T0),
		"A5 id 0 is never held - an empty slot is not a reservation");
	chk(clres_count(&R, T0) == 1, "A6 one live reservation");

	/* ---- B. the expiry boundary is exclusive ------------------------- */
	chk(clres_holds(&R, 7, T0 + TTL - 1), "B1 live one ms before");
	chk(!clres_holds(&R, 7, T0 + TTL),
		"B2 lapsed AT until_ms, not after it");
	chk(clres_count(&R, T0 + TTL) == 0, "B3 and it stops counting");
	chk(!clres_home(&R, id1, T0 + TTL, &home), "B4 and stops being home");
	how = NULL;
	chk(clres_take(&R, id1, &(struct in_addr){0x0101a8c0}, T0 + TTL,
		&how) == 0, "B5 and cannot be claimed");

	/* ---- C. claiming: identity first, then address ------------------- */
	memset(&R, 0, sizeof R);
	clres_note(&R, 7, addr_of(0x0101a8c0), id1, 1, T0, TTL);
	how = NULL;
	chk(clres_take(&R, id1, &(struct in_addr){0x0909a8c0}, T0, &how) == 7,
		"C1 identity claims from a NEW address");
	chk(how && strcmp(how, "identity") == 0, "C2 how says identity");
	chk(clres_take(&R, id1, &(struct in_addr){0x0909a8c0}, T0, &how) == 0,
		"C3 the claim CONSUMED it");
	chk(clres_count(&R, T0) == 0, "C4 nothing live afterwards");

	memset(&R, 0, sizeof R);
	clres_note(&R, 9, addr_of(0x0101a8c0), id1, 1, T0, TTL);
	how = NULL;
	chk(clres_take(&R, id2, &(struct in_addr){0x0101a8c0}, T0, &how) == 9,
		"C5 a wrong identity still matches by address");
	chk(how && strcmp(how, "address") == 0, "C6 how says address");

	memset(&R, 0, sizeof R);
	clres_note(&R, 11, addr_of(0x0101a8c0), id1, 1, T0, TTL);
	how = NULL;
	chk(clres_take(&R, NULL, &(struct in_addr){0x0101a8c0}, T0, &how) == 11,
		"C7 a joiner with no identity claims by address");
	chk(how && strcmp(how, "address") == 0, "C8 how says address");

	/* the ordering that matters: the identity's entry wins even when
	 * another entry matches the address and sits EARLIER in the table */
	memset(&R, 0, sizeof R);
	clres_note(&R, 21, addr_of(0x0505a8c0), id2, 1, T0, TTL);  /* addr match */
	clres_note(&R, 22, addr_of(0x0606a8c0), id1, 1, T0, TTL);  /* ident match */
	how = NULL;
	chk(clres_take(&R, id1, &(struct in_addr){0x0505a8c0}, T0, &how) == 22,
		"C9 identity beats an earlier address match");
	chk(how && strcmp(how, "identity") == 0, "C10 how says identity");
	chk(clres_holds(&R, 21, T0), "C11 the address entry is untouched");

	/* an entry stored WITHOUT an identity is never matched by one, even
	 * if the bytes in the slot happen to compare equal */
	memset(&R, 0, sizeof R);
	clres_note(&R, 31, addr_of(0x0707a8c0), id1, 0, T0, TTL);
	how = NULL;
	chk(clres_take(&R, id1, &(struct in_addr){0x0808a8c0}, T0, &how) == 0,
		"C12 has_ident 0 is not claimable by identity");
	chk(clres_holds(&R, 31, T0), "C13 and was not consumed");
	chk(!clres_home(&R, id1, T0, &home),
		"C14 nor does it answer as a home");

	/* every byte of the identity is compared */
	memset(&R, 0, sizeof R);
	clres_note(&R, 41, addr_of(0x0a0aa8c0), id1, 1, T0, TTL);
	for (i = 0; i < CLRES_IDENT_LEN; i++) {
		unsigned char bad[16];

		memcpy(bad, id1, 16);
		bad[i] ^= 0x80;
		if (clres_take(&R, bad, &(struct in_addr){0x0b0ba8c0}, T0,
				&how) != 0)
			break;
	}
	chk(i == CLRES_IDENT_LEN, "C15 all 16 identity bytes are compared");

	/* ---- D. home, without consuming ---------------------------------- */
	memset(&R, 0, sizeof R);
	clres_note(&R, 5, addr_of(0x0c0ca8c0), id1, 1, T0, TTL);
	home.s_addr = 0;
	chk(clres_home(&R, id1, T0, &home) == 1 && home.s_addr == 0x0c0ca8c0,
		"D1 home reports the last address");
	chk(clres_holds(&R, 5, T0), "D2 and did NOT consume the entry");
	chk(!clres_home(&R, id2, T0, &home), "D3 an unknown identity has none");

	/* ---- E. refresh in place ----------------------------------------- */
	memset(&R, 0, sizeof R);
	clres_note(&R, 3, addr_of(0x0d0da8c0), id1, 1, T0, TTL);
	clres_note(&R, 4, addr_of(0x0d0da8c0), id2, 1, T0 + 10, TTL);
	chk(clres_count(&R, T0 + 10) == 1,
		"E1 the same address refreshes rather than adding a slot");
	chk(clres_holds(&R, 4, T0 + 10) && !clres_holds(&R, 3, T0 + 10),
		"E2 the new binding replaced the old one");
	chk(clres_holds(&R, 4, T0 + TTL + 5),
		"E3 and the window restarted from the new note");

	/* the refresh branch does not test expiry: a LAPSED entry at the
	 * same address is reused in place rather than left as litter */
	memset(&R, 0, sizeof R);
	clres_note(&R, 3, addr_of(0x0e0ea8c0), id1, 1, T0, TTL);
	clres_note(&R, 4, addr_of(0x0e0ea8c0), id2, 1, T0 + TTL + 1, TTL);
	chk(clres_count(&R, T0 + TTL + 1) == 1,
		"E4 a lapsed entry at the same address is reused in place");

	/* ---- F. the bound, and what gets evicted ------------------------- */
	memset(&R, 0, sizeof R);
	for (i = 0; i < CLRES_MAX; i++) {
		unsigned char idn[16];

		ident_of(idn, 100 + i);
		/* later slots expire LATER, so slot 0 is the soonest */
		clres_note(&R, 1000 + i, addr_of(0x1000 + (unsigned)i), idn, 1,
			T0, TTL + i * 1000);
	}
	chk(clres_count(&R, T0) == CLRES_MAX, "F1 the table fills to its bound");
	clres_note(&R, 2000, addr_of(0xdeadbeef), id3, 1, T0, TTL);
	chk(clres_count(&R, T0) == CLRES_MAX,
		"F2 one past the bound stays at the bound");
	chk(clres_holds(&R, 2000, T0), "F3 the new reservation is in");
	chk(!clres_holds(&R, 1000, T0),
		"F4 it evicted the one expiring SOONEST");
	chk(clres_holds(&R, 1000 + CLRES_MAX - 1, T0),
		"F5 not the one expiring latest");

	/* a lapsed slot is preferred to evicting a live one */
	memset(&R, 0, sizeof R);
	for (i = 0; i < CLRES_MAX; i++) {
		unsigned char idn[16];

		ident_of(idn, 200 + i);
		clres_note(&R, 1000 + i, addr_of(0x2000 + (unsigned)i), idn, 1,
			T0, i == 5 ? 10 : TTL);
	}
	clres_note(&R, 3000, addr_of(0xfeedface), id3, 1, T0 + 20, TTL);
	chk(clres_holds(&R, 3000, T0 + 20), "F6 the new reservation is in");
	n = 0;
	for (i = 0; i < CLRES_MAX; i++)
		if (i != 5 && clres_holds(&R, 1000 + i, T0 + 20))
			n++;
	chk(n == CLRES_MAX - 1,
		"F7 it took the LAPSED slot and evicted no live entry");

	printf("clrestest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
