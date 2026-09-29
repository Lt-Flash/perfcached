/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clunk.h - the commands clients sent that this daemon does not implement
 * (S165): one bounded table of them, the frame that carries a node's
 * table to its peers, and the fold that turns every member's table into
 * one fleet list.
 *
 * A row is a NAME and how often, when and from whom it arrived - never an
 * argument.  A password sent to a misspelt command is still a password,
 * so nothing after the command (and, where the refusal is a subcommand's,
 * the subcommand) is ever copied in.  Names are hostile input: truncated
 * to CLUNK_NAME - 1 bytes and anything outside printable ASCII becomes
 * '?' before it is stored, logged or sent.
 *
 * Bounded twice over: CLUNK_ROWS distinct names, and past that the count
 * goes to `other` instead of evicting a row - a scanner spraying random
 * names cannot push the real gap off the table.  Unknowns sent before
 * AUTH are counted in `preauth` and never named: that is where port
 * scanners live.
 *
 * Pure, in the M-series sense: the caller owns every table, passes the
 * clock and the client's address, and does the logging.  The rules - the
 * cap, the sanitising, the fold's arithmetic, a frame accepted whole or
 * not at all - are cases in test/clunktest.c.
 */
#ifndef PC_CLUNK_H
#define PC_CLUNK_H

#include <stddef.h>
#include <stdint.h>

#define CLUNK_ROWS    64               /* distinct names a table holds */
#define CLUNK_NAME    33               /* 32 bytes + NUL */
#define CLUNK_ADDR    48               /* "ip:port", as CLIENT LIST shows it */
#define CLUNK_CLIENT  24               /* CLIENT SETNAME, as obs keeps it */

enum { CLUNK_RESP, CLUNK_JSON, CLUNK_BIN, CLUNK_DIALECTS };

struct clunk_row {
	unsigned char dialect;             /* CLUNK_RESP .. CLUNK_BIN */
	unsigned char nlen;
	char name[CLUNK_NAME];
	uint64_t count;
	uint64_t first_s, last_s;          /* unix seconds */
	char addr[CLUNK_ADDR];             /* the LAST client that sent it */
	char client[CLUNK_CLIENT];
	/* the fold's, never on the wire: this node's share of `count`, and
	 * the member whose sighting is the newest (0 = this node) */
	uint64_t here;
	int node;
};

struct clunk_table {
	uint64_t other;                    /* sightings past the row cap */
	uint64_t preauth;                  /* unknowns before AUTH, unnamed */
	int n;
	struct clunk_row rows[CLUNK_ROWS];
};

/* Build the stored name: @cmd, and " " + @sub when @sub is given, each
 * upper-cased when @upper (RESP is case-insensitive, JSON methods are
 * not), cut to CLUNK_NAME - 1 bytes, non-printables made '?'.  Returns
 * its length; @out holds CLUNK_NAME bytes. */
size_t clunk_name(char *out, int upper, const char *cmd, size_t clen,
		const char *sub, size_t slen);

/* One sighting of an already-built @name.  Returns 1 when it made a new
 * row (the caller's cue to log a first sighting), 0 when it counted into
 * an existing one, -1 when the table was full and it went to `other`.
 * @addr and @client may be NULL. */
int clunk_note(struct clunk_table *t, int dialect, const char *name,
		size_t nlen, uint64_t now_s, const char *addr,
		const char *client);

/* Add @src into @dst: counts summed, the earliest first sighting, the
 * latest last one and the client that sent it; `other` and `preauth`
 * summed; a row with no room in @dst goes to its `other`.  @node names
 * the member @src belongs to (0 = this node, which is what `here` counts). */
void clunk_fold(struct clunk_table *dst, const struct clunk_table *src,
		int node);

/* The fold's rows newest first (last_s), then by count, then by name -
 * the order the page shows. */
void clunk_sort(struct clunk_table *t);

/* M_UNKCMD's body after the type byte: [node u16][other u64][preauth u64]
 * [n u8] then per row [dialect u8][nlen u8][name][count u64][first u64]
 * [last u64][alen u8][addr][clen u8][client].  Bytes past the last row
 * are ignored - fields are appended at the tail, as every frame here.
 * clunk_write returns the length, 0 if @cap is too small; clunk_parse
 * returns 0 and fills @node and @t, or -1 and leaves @t untouched when
 * any row is short or any length is out of bounds - all or nothing. */
#define CLUNK_FRAME_MAX (19 + CLUNK_ROWS * (2 + (CLUNK_NAME - 1) + 24 + \
		1 + (CLUNK_ADDR - 1) + 1 + (CLUNK_CLIENT - 1)))
size_t clunk_write(const struct clunk_table *t, unsigned node,
		unsigned char *buf, size_t cap);
int clunk_parse(const unsigned char *b, size_t n, unsigned *node,
		struct clunk_table *t);

const char *clunk_dialect_name(int dialect);

/* the printable rule in place, for a string the caller logs itself (a
 * CLIENT SETNAME name is the client's to choose) */
void clunk_clean(char *s);

#endif /* PC_CLUNK_H */
