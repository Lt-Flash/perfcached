/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clpull.h - the PULL plane's wire frames (M-series).
 *
 * A local miss in a store collection asks a peer that holds the record:
 * PULL_REQ names the key, PULL_RSP carries the value, its remaining TTL
 * and the VERSION the holder's table committed.  The version is the part
 * that matters - it travels with the bytes so the asker can refuse a
 * copy older than one it already has.
 *
 * These two lived at opposite ends of cluster.c - built at line 1082,
 * read at 1479 and 1568 - with each field's offset written down
 * separately in each place.  Neither half could check the other, and a
 * test could not reach either, because both were static inside a file
 * that needs the whole daemon to link.  Here they are adjacent, the
 * cursor decides positions from order, and test/clpulltest.c holds the
 * pre-extraction layout as a reference encoder to check them against.
 *
 * ONE THING IS DELIBERATELY NOT A CODEC CONCERN: when a value is too
 * large for the datagram plane the sender reports it as absent - found
 * 0, vlen 0, ver 0 - but LEAVES ttl_left at the value it computed.  That
 * asymmetry is the caller's decision, made before it fills the struct,
 * not something this file smooths over.
 */
#ifndef PC_CLPULL_H
#define PC_CLPULL_H

#include <stddef.h>
#include <stdint.h>

/* [type1][req4][node2][collen1][klen2] then col, key */
#define CLPULL_QHDR   10
/* [type1][req4][node2][found1][ttl4][vlen4][ver8] then the value */
#define CLPULL_RHDR   24

struct clpull_req {
	uint32_t req;
	int node;                          /* the asker */
	const char *col;
	unsigned int collen;
	const char *key;
	unsigned int klen;
};

struct clpull_rsp {
	uint32_t req;
	int node;                          /* the holder */
	int found;
	uint32_t ttl_left;                 /* seconds; 0 = no expiry */
	uint32_t vlen;
	uint64_t ver;
	const char *val;                   /* into the caller's frame on parse */
};

size_t clpull_req_size(unsigned int collen, unsigned int klen);

/* Both writers take the buffer's capacity and return what they wrote, 0
 * if it would not fit.  The response writer appends the value only when
 * found and vlen are both set. */
size_t clpull_req_write(unsigned char *buf, size_t cap, unsigned char type,
		const struct clpull_req *q);
size_t clpull_rsp_write(unsigned char *buf, size_t cap, unsigned char type,
		const struct clpull_rsp *r);

/* Parse.  Both return 0 on a frame too short for its header, and the
 * request also refuses one whose declared col+key run past the end - a
 * truncated pull is dropped, never half-read. */
int clpull_req_parse(const unsigned char *p, size_t n, struct clpull_req *q);
int clpull_rsp_parse(const unsigned char *p, size_t n, struct clpull_rsp *r);

#endif /* PC_CLPULL_H */
