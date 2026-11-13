/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clunk.h - the commands clients sent that this daemon does not implement: a bounded table, its
 * frame to peers, the fold into one fleet list.  Pure: the caller owns tables, clock, logging. */
#ifndef PC_CLUNK_H
#define PC_CLUNK_H

#include <stddef.h>
#include <stdint.h>

#define CLUNK_ROWS    64               /* distinct names a table holds */
#define CLUNK_NAME    33               /* 32 bytes + NUL */
#define CLUNK_ADDR    48               /* "ip:port", as CLIENT LIST shows it */
#define CLUNK_CLIENT  24               /* CLIENT SETNAME, as obs keeps it */

/* the dialect a sighting came in on, the byte in the M_UNKCMD frame (a renumbering needs a wire
 * epoch bump, cluster.h) */
enum { CLUNK_RESP, CLUNK_BIN, CLUNK_DIALECTS };

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

/* the stored name, never an argument (a password can follow): @cmd [" " @sub], upper-cased when
 * @upper, cut to CLUNK_NAME - 1, non-printables '?'; returns its length, @out holds CLUNK_NAME */
size_t clunk_name(char *out, int upper, const char *cmd, size_t clen,
		const char *sub, size_t slen);

/* one sighting of a built @name: 1 a new row (log a first sighting), 0 counted into one, -1 the
 * table full, counted in `other` rather than evicting; @addr and @client may be NULL */
int clunk_note(struct clunk_table *t, int dialect, const char *name,
		size_t nlen, uint64_t now_s, const char *addr,
		const char *client);

/* add @src into @dst: counts summed, earliest first and latest last sighting (with its client), no
 * room to `other`; @node is @src's member (0 = this node, which is what `here` counts) */
void clunk_fold(struct clunk_table *dst, const struct clunk_table *src,
		int node);

/* The fold's rows newest first (last_s), then by count, then by name -
 * the order the page shows. */
void clunk_sort(struct clunk_table *t);

/* M_UNKCMD: [node u16][other u64][preauth u64][n u8] + n x ([dialect u8][nlen u8][name][count u64]
 * [first u64][last u64][alen u8][addr][clen u8][client]); bytes past the last row are ignored */
#define CLUNK_FRAME_MAX (19 + CLUNK_ROWS * (2 + (CLUNK_NAME - 1) + 24 + \
		1 + (CLUNK_ADDR - 1) + 1 + (CLUNK_CLIENT - 1)))
/* clunk_write: the length, 0 if @cap is too small; clunk_parse: 0 with @node and @t, or -1 and @t
 * untouched when any row is short or any length out of bounds */
size_t clunk_write(const struct clunk_table *t, unsigned node,
		unsigned char *buf, size_t cap);
int clunk_parse(const unsigned char *b, size_t n, unsigned *node,
		struct clunk_table *t);

const char *clunk_dialect_name(int dialect);

/* the printable rule in place, for a string the caller logs itself (a
 * CLIENT SETNAME name is the client's to choose) */
void clunk_clean(char *s);

#endif /* PC_CLUNK_H */
