/*
 * clunktest.c - the unknown-command table, its frame and the fleet fold
 * (S165).
 *
 * What goes wrong here goes wrong quietly: a table that evicts lets a
 * scanner push the real gap off the card, a name that keeps its bytes
 * puts a terminal escape in the journal, a fold that sums the wrong way
 * shows a gap on one page and not another, and a frame parsed partly
 * leaves a half-filled table on a peer.  Every one of those is a case.
 *
 * Build: cc -o clunktest test/clunktest.c src/clunk.o
 */
#include <stdio.h>
#include <string.h>

#include "../src/clunk.h"

static int pass, fail;
static void chk(int cond, const char *what)
{
	if (cond) { pass++; return; }
	fail++;
	printf("  FAIL %s\n", what);
}

static struct clunk_table T, U, F;
static unsigned char buf[CLUNK_FRAME_MAX + 64];

static int note(struct clunk_table *t, int d, const char *name,
		unsigned long long now, const char *addr, const char *client)
{
	return clunk_note(t, d, name, strlen(name), now, addr, client);
}

static void names(void)
{
	char n[CLUNK_NAME];
	size_t l;

	l = clunk_name(n, 1, "client", 6, "tracking", 8);
	chk(l == 15 && !strcmp(n, "CLIENT TRACKING"), "RESP: command and subcommand upper-cased, one space");
	l = clunk_name(n, 0, "frobNicate", 10, NULL, 0);
	chk(l == 10 && !strcmp(n, "frobNicate"), "JSON: a method name keeps its case");
	l = clunk_name(n, 1, "a\x1b[31mb", 7, NULL, 0);
	chk(l == 7 && !strcmp(n, "A?[31MB"), "an escape byte becomes '?', nothing unprintable is stored");
	l = clunk_name(n, 1, "x y", 3, NULL, 0);
	chk(!strcmp(n, "X?Y"), "a space inside a part is not a separator: '?'");
	l = clunk_name(n, 1, "0123456789012345678901234567890123456789", 40, NULL, 0);
	chk(l == CLUNK_NAME - 1 && strlen(n) == CLUNK_NAME - 1, "a long name is cut to 32 bytes");
	l = clunk_name(n, 1, "0123456789012345678901234567890", 31, "sub", 3);
	chk(l == CLUNK_NAME - 1 && n[31] == ' ', "a subcommand past the cut is dropped, not overrun");
}

static void table(void)
{
	char nm[16];
	int i, rc;

	memset(&T, 0, sizeof T);
	chk(note(&T, CLUNK_RESP, "XYZZY", 100, "192.0.2.1:5000", "client-a") == 1, "a first sighting makes a row (1)");
	chk(note(&T, CLUNK_RESP, "XYZZY", 105, "192.0.2.2:6000", "client-b") == 0, "a repeat counts into it (0)");
	chk(T.n == 1 && T.rows[0].count == 2, "one row, count 2");
	chk(T.rows[0].first_s == 100 && T.rows[0].last_s == 105, "first and last sighting kept");
	chk(!strcmp(T.rows[0].addr, "192.0.2.2:6000") && !strcmp(T.rows[0].client, "client-b"), "the LAST client is the one kept");
	chk(note(&T, CLUNK_JSON, "XYZZY", 106, NULL, NULL) == 1, "the same name in another dialect is another row");
	chk(T.n == 2 && T.rows[1].addr[0] == 0 && T.rows[1].client[0] == 0, "no client: empty fields, not garbage");
	note(&T, CLUNK_RESP, "BAD", 107, "1.2.3.4:5", "a\nb\x07");
	chk(!strcmp(T.rows[2].client, "a?b?"), "a CLIENT SETNAME name is cleaned too (it is the client's to choose)");
	for (i = 0; T.n < CLUNK_ROWS; i++) {
		snprintf(nm, sizeof nm, "N%d", i);
		note(&T, CLUNK_RESP, nm, 200, NULL, NULL);
	}
	chk(T.n == CLUNK_ROWS && T.other == 0, "filled to the cap, nothing overflowed yet");
	rc = note(&T, CLUNK_RESP, "ONE-TOO-MANY", 300, NULL, NULL);
	chk(rc == -1 && T.other == 1 && T.n == CLUNK_ROWS, "past the cap: -1, counted in other, no eviction");
	chk(note(&T, CLUNK_RESP, "XYZZY", 301, NULL, NULL) == 0 && T.rows[0].count == 3,
		"a stored name still counts once the table is full");
	chk(!strcmp(T.rows[0].name, "XYZZY"), "the first row survived the flood");
}

static void fold(void)
{
	memset(&T, 0, sizeof T);
	memset(&U, 0, sizeof U);
	memset(&F, 0, sizeof F);
	note(&T, CLUNK_RESP, "XYZZY", 100, "192.0.2.1:1", "here");
	note(&T, CLUNK_RESP, "XYZZY", 110, "192.0.2.1:1", "here");
	T.preauth = 4;
	T.other = 1;
	note(&U, CLUNK_RESP, "XYZZY", 90, "198.51.100.9:9", "there");
	note(&U, CLUNK_RESP, "XYZZY", 120, "198.51.100.9:9", "there");
	note(&U, CLUNK_RESP, "XYZZY", 125, "198.51.100.9:9", "there");
	note(&U, CLUNK_JSON, "frob", 50, NULL, NULL);
	U.preauth = 2;
	clunk_fold(&F, &T, 0);
	clunk_fold(&F, &U, 676);
	chk(F.n == 2, "two distinct rows after the fold");
	chk(F.rows[0].count == 5 && F.rows[0].here == 2, "counts summed, `here` only this node's share");
	chk(F.rows[0].first_s == 90 && F.rows[0].last_s == 125, "earliest first, latest last");
	chk(!strcmp(F.rows[0].client, "there") && F.rows[0].node == 676, "the newest sighting names its client and its node");
	chk(F.rows[1].here == 0 && F.rows[1].node == 676, "a row only a peer saw: here 0");
	chk(F.preauth == 6 && F.other == 1, "preauth and other summed");
	/* a peer's rows past the fold's room go to other, counted, not dropped */
	{
		char nm[16];
		int i;

		memset(&F, 0, sizeof F);
		memset(&U, 0, sizeof U);
		for (i = 0; i < CLUNK_ROWS; i++) {
			snprintf(nm, sizeof nm, "A%d", i);
			note(&F, CLUNK_RESP, nm, 1, NULL, NULL);
		}
		note(&U, CLUNK_RESP, "LATE", 2, NULL, NULL);
		note(&U, CLUNK_RESP, "LATE", 3, NULL, NULL);
		clunk_fold(&F, &U, 9);
		chk(F.n == CLUNK_ROWS && F.other == 2, "no room in the fold: the row's COUNT goes to other");
	}
}

static void sort(void)
{
	memset(&F, 0, sizeof F);
	note(&F, CLUNK_RESP, "OLD", 10, NULL, NULL);
	note(&F, CLUNK_RESP, "NEW", 30, NULL, NULL);
	note(&F, CLUNK_RESP, "TIE-FEW", 20, NULL, NULL);
	note(&F, CLUNK_RESP, "TIE-MANY", 20, NULL, NULL);
	note(&F, CLUNK_RESP, "TIE-MANY", 20, NULL, NULL);
	clunk_sort(&F);
	chk(!strcmp(F.rows[0].name, "NEW") && !strcmp(F.rows[1].name, "TIE-MANY") &&
	    !strcmp(F.rows[2].name, "TIE-FEW") && !strcmp(F.rows[3].name, "OLD"),
	    "newest first, then the larger count");
}

static void frame(void)
{
	struct clunk_table G;
	unsigned node = 0;
	size_t n, k;
	int bad = 0;
	char nm[40];
	int i;

	memset(&T, 0, sizeof T);
	note(&T, CLUNK_RESP, "CLIENT TRACKING", 1000, "192.0.2.52:40123", "client-a");
	note(&T, CLUNK_JSON, "frob", 1001, "127.0.0.1:1", "");
	note(&T, CLUNK_BIN, "verb 42", 1002, NULL, NULL);
	T.other = 7;
	T.preauth = 3;
	n = clunk_write(&T, 1014, buf, sizeof buf);
	chk(n > 19, "a table writes");
	memset(&G, 0xAA, sizeof G);
	chk(clunk_parse(buf, n, &node, &G) == 0 && node == 1014, "and parses back, node carried");
	chk(G.n == 3 && G.other == 7 && G.preauth == 3, "rows, other and preauth round-trip");
	chk(!strcmp(G.rows[0].name, "CLIENT TRACKING") && G.rows[0].nlen == 15 &&
	    G.rows[0].count == 1 && G.rows[0].last_s == 1000 &&
	    !strcmp(G.rows[0].addr, "192.0.2.52:40123") && !strcmp(G.rows[0].client, "client-a"),
	    "a row round-trips field for field, the name's space kept");
	chk(G.rows[2].dialect == CLUNK_BIN && !strcmp(G.rows[2].name, "verb 42"), "dialect round-trips");
	/* all or nothing: every shorter length refuses, and leaves G alone */
	for (k = 0; k < n; k++) {
		memset(&G, 0, sizeof G);
		G.n = 99;
		if (clunk_parse(buf, k, &node, &G) != -1 || G.n != 99)
			bad++;
	}
	chk(bad == 0, "every truncation of the frame is refused whole, the table untouched");
	chk(clunk_parse(buf, n + 5, &node, &G) == 0 && G.n == 3, "bytes past the last row are a newer peer's tail: ignored");
	buf[18] = CLUNK_ROWS + 1;
	chk(clunk_parse(buf, n, &node, &G) == -1, "more rows than a table holds: refused");
	buf[18] = 3;
	buf[19] = CLUNK_DIALECTS;
	chk(clunk_parse(buf, n, &node, &G) == -1, "an unknown dialect: refused");
	buf[19] = CLUNK_RESP;
	buf[20] = CLUNK_NAME;
	chk(clunk_parse(buf, n, &node, &G) == -1, "a name longer than 32: refused");
	buf[20] = 0;
	chk(clunk_parse(buf, n, &node, &G) == -1, "an empty name: refused");
	buf[20] = 15;
	chk(clunk_parse(buf, n, &node, &G) == 0, "restored, it parses again (the mutations were the cause)");
	/* the byte-flip above misaligns every later field, so it would be
	 * refused even with no empty-name rule: a WELL-FORMED frame whose
	 * row has an empty name is what proves the rule itself */
	{
		struct clunk_table E;
		size_t en;

		memset(&E, 0, sizeof E);
		E.n = 1;
		E.rows[0].dialect = CLUNK_RESP;
		E.rows[0].count = 1;
		en = clunk_write(&E, 5, buf + 512, sizeof buf - 512);
		chk(en > 0 && clunk_parse(buf + 512, en, &node, &G) == -1,
			"a well-formed row with an empty name: refused");
	}
	buf[21] = 0x1b;
	chk(clunk_parse(buf, n, &node, &G) == 0 && G.rows[0].name[0] == '?', "a peer's unprintable byte is cleaned on arrival");
	buf[21] = 0;
	chk(clunk_parse(buf, n, &node, &G) == -1, "a peer's embedded NUL empties the name: refused");
	buf[21] = 'C';
	/* a full table fits a datagram with room to spare */
	memset(&T, 0, sizeof T);
	for (i = 0; i < CLUNK_ROWS; i++) {
		snprintf(nm, sizeof nm, "%-32d", i);
		nm[CLUNK_NAME - 1] = 0;
		clunk_note(&T, CLUNK_RESP, nm, CLUNK_NAME - 1, 1, "255.255.255.255:65535-padding-to-forty-seven-by", "twenty-three-characters");
	}
	n = clunk_write(&T, 1, buf, sizeof buf);
	chk(n > 0 && n <= CLUNK_FRAME_MAX, "a full table of the longest rows writes within CLUNK_FRAME_MAX");
	chk(clunk_write(&T, 1, buf, n - 1) == 0, "one byte short of room: 0, not a truncated frame");
	chk(clunk_parse(buf, n, &node, &G) == 0 && G.n == CLUNK_ROWS, "and the full frame parses");
}

int main(void)
{
	names();
	table();
	fold();
	sort();
	frame();
	printf("clunktest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
