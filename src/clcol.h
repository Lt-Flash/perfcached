/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clcol.h - the collection-announce frame (M_COL_SET).
 *
 * One frame carries four operations on a collection - create, drop,
 * rename and resize - distinguished by the op byte, and RENAME is the
 * odd one: it carries a second length-prefixed name after the first.
 *
 *     [type1][op1][buckets_log21][gen8][nlen1][name]  and, for a
 *     rename only, [tlen1][to]
 *
 * It was built in three places (pc_cluster_col_announce, its rename
 * twin, and col_sync_peer) and read in a fourth, each writing the
 * offsets out again.  The three builders differed only in which op they
 * set, which is exactly the kind of near-duplicate that drifts.
 *
 * `gen` is a Lamport value and orders competing announcements, so a
 * misread one silently reorders create against drop - a collection that
 * comes back after being deleted, or fails to.  It went out as a raw
 * memcpy of the host's bytes; it is little-endian now, identical on
 * every host this runs on.
 *
 * Names are bounded by the CALLER's PC_COL_NAME_MAX; this file only
 * refuses what will not fit the datagram, and reports the name in
 * place rather than copying it.
 */
#ifndef PC_CLCOL_H
#define PC_CLCOL_H

#include <stddef.h>
#include <stdint.h>

#define CLCOL_HDR       12         /* through nlen */

#define CLCOL_OP_SET     0
#define CLCOL_OP_DROP    1
#define CLCOL_OP_RENAME  2
#define CLCOL_OP_RESIZE  3

struct clcol_ann {
	int op;
	int buckets_log2;
	uint64_t gen;
	const char *name;
	unsigned int nlen;
	const char *to;                    /* rename only, else NULL */
	unsigned int tlen;
};

size_t clcol_size(unsigned int nlen, unsigned int tlen);

/* Write one.  A rename is any frame with `to` set; everything else
 * writes the single-name form.  Returns the length, 0 if it would not
 * fit. */
size_t clcol_write(unsigned char *buf, size_t cap, unsigned char type,
		const struct clcol_ann *a);

/* Parse.  Returns 0 unless the frame is whole - including the SECOND
 * name when the op is a rename, which the caller then does not have to
 * re-check.  A zero-length name is refused: it names nothing. */
int clcol_parse(const unsigned char *p, size_t n, struct clcol_ann *a);

#endif /* PC_CLCOL_H */
