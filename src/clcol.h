/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clcol.h - the collection-announce frame (M_COL_SET): [type1][op1][buckets_log21][gen8 LE][nlen1]
 * [name], a rename adding [tlen1][to]; `gen` is a Lamport value ordering competing announcements */
#ifndef PC_CLCOL_H
#define PC_CLCOL_H

#include <stddef.h>
#include <stdint.h>

#define CLCOL_HDR       12         /* through nlen */

#define CLCOL_OP_SET     0
#define CLCOL_OP_DROP    1
#define CLCOL_OP_RENAME  2
#define CLCOL_OP_RESIZE  3
/* the RESP door's DB index map: name = the index in decimal, buckets_log2 1 mapped / 0 not, the
 * collection as second name; ONLY to CLMEMB_FEAT_RESPMAP members - older builds read it as a SET */
#define CLCOL_OP_MAP     4
/* a collection's runtime autoscale mode in the buckets_log2 byte (PC_AUTOSCALE_*); ONLY to
 * CLMEMB_FEAT_AUTOSCALE members - an older build would create the collection 2^mode wide */
#define CLCOL_OP_AUTOSCALE 5

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

/* write one (a rename is any frame with `to` set): the length, 0 if it would not fit */
size_t clcol_write(unsigned char *buf, size_t cap, unsigned char type,
		const struct clcol_ann *a);

/* 0 unless the frame is whole, a rename's SECOND name included; a zero-length name is refused */
int clcol_parse(const unsigned char *p, size_t n, struct clcol_ann *a);

#endif /* PC_CLCOL_H */
