/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clhold.h - S394's hold-check frames (clmsg.h M_HOLD_Q / M_HOLD_A): the
 * codec, as clpull is the pull's.  cluster.c keeps the batch, the pending
 * slots and the answers' outcomes; this writes and reads the bytes.  Pure,
 * any thread, no logging (the 0.7.2 split, wave 1 - doc/MODULARITY.md).
 *
 *   M_HOLD_Q  [type][node2][collen1][col][n2] + n x ([req4][klen2][key])
 *   M_HOLD_A  [type][node2][n2] + n x ([req4][held1][ver8])
 * Numbers big-endian.
 */
#ifndef PC_CLHOLD_H
#define PC_CLHOLD_H

#include <stddef.h>
#include <stdint.h>

#define CLHOLD_MAX     8192            /* a batch's datagram */
#define CLHOLD_MAXN    256             /* keys in one batch, and answered */
#define CLHOLD_A_HDR   5
#define CLHOLD_A_ENTRY 13

/* a query being written into @buf */
struct clhold_qw {
	unsigned char *buf;
	size_t cap, len, nat;              /* its length; where its count goes */
	unsigned int n;
};

/* begin a query for @col from @node: 0, or -1 when the header cannot fit */
int clhold_q_begin(struct clhold_qw *q, unsigned char *buf, size_t cap,
		unsigned int node, const char *col, size_t collen);
/* would a key of @klen still fit (bytes and count)? */
int clhold_q_fits(const struct clhold_qw *q, size_t klen);
/* append one key: 0, or -1 when it does not fit */
int clhold_q_add(struct clhold_qw *q, uint32_t req, const char *key,
		size_t klen);
/* the count written in: the frame's length */
size_t clhold_q_end(struct clhold_qw *q);
/* the collection the query began for */
int clhold_q_is_col(const struct clhold_qw *q, const char *col, size_t collen);

/* a query being read */
struct clhold_qr {
	const unsigned char *p;
	size_t n, at;
	const char *col;
	size_t collen;
	unsigned int node, cnt, i;
};

/* 0 a well-formed header (its col, node and count read), -1 not */
int clhold_q_open(struct clhold_qr *r, const unsigned char *p, size_t n);
/* the next key: 1 (@req, @key, @klen), 0 none left or the frame ends */
int clhold_q_next(struct clhold_qr *r, uint32_t *req, const char **key,
		size_t *klen);

/* answers: the header's length; one entry's end; the count written in */
size_t clhold_a_begin(unsigned char *buf, unsigned int node);
size_t clhold_a_put(unsigned char *buf, size_t at, uint32_t req, int held,
		unsigned long long ver);
void clhold_a_end(unsigned char *buf, unsigned int n);
/* 0 and the answering node and count, -1 too short */
int clhold_a_open(const unsigned char *p, size_t n, unsigned int *node,
		unsigned int *cnt);
/* entry @i: 1, 0 past the frame */
int clhold_a_get(const unsigned char *p, size_t n, unsigned int i,
		uint32_t *req, int *held, unsigned long long *ver);

#endif
