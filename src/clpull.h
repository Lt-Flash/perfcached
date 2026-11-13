/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clpull.h - the PULL plane's wire frames: PULL_REQ names the key, PULL_RSP carries the value,
 * its TTL and the holder's VERSION, so the asker can refuse a copy older than one it has. */
#ifndef PC_CLPULL_H
#define PC_CLPULL_H

#include <stddef.h>
#include <stdint.h>

/* [type1][req4][node2][collen1][klen2] then col, key */
#define CLPULL_QHDR   10
/* [type1][req4][node2][found1][ttl4][vlen4][ver8][flags1] then the value;
 * flags = the record's type */
#define CLPULL_RHDR   25

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
	uint32_t ttl_left;                 /* seconds; 0 = no expiry.  Kept as computed even when a
	                                    * too-large value is reported absent: the caller's call */
	uint32_t vlen;
	uint64_t ver;
	unsigned char flags;               /* PCACHE_F_JSON */
	const char *val;                   /* into the caller's frame on parse */
};

size_t clpull_req_size(unsigned int collen, unsigned int klen);

/* Both writers take the buffer's capacity and return what they wrote, 0 if it would not fit;
 * the response carries the value only when found and vlen are both set. */
size_t clpull_req_write(unsigned char *buf, size_t cap, unsigned char type,
		const struct clpull_req *q);
size_t clpull_rsp_write(unsigned char *buf, size_t cap, unsigned char type,
		const struct clpull_rsp *r);

/* this node's PULL_REQ as a whole frame under M_PULL_REQ (clmsg.h); @msg must hold
 * clpull_req_size(collen, klen) */
size_t clpull_req_frame(unsigned char *msg, uint32_t req, int node,
		const char *col, size_t collen, const char *key, size_t klen);

/* Parse: 0 on a frame too short for its header; the request also refuses declared col+key
 * past the end - a truncated pull is dropped, never half-read. */
int clpull_req_parse(const unsigned char *p, size_t n, struct clpull_req *q);
int clpull_rsp_parse(const unsigned char *p, size_t n, struct clpull_rsp *r);
/* a parsed PULL_RSP's declared value wholly present in its @n bytes: 1 or 0, computed
 * without 32-bit wrap (a vlen near 2^32 must not pass as a few bytes) */
int clpull_rsp_whole(const struct clpull_rsp *r, size_t n);

#endif /* PC_CLPULL_H */
