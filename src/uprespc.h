/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * uprespc.h - the upstream's RESP reply parser (upstream.c's; the 0.7.2
 * split, wave 1).  Every function is pure and may be called from any
 * thread; rv_max is per thread.
 */
#ifndef PC_UPRESPC_H
#define PC_UPRESPC_H

#include <stddef.h>

#define UP_RBUF_MAX     (1u << 20)    /* the largest reply read (a value
                                       * over the cell ceiling is refused
                                       * before it is read whole) */

/* one reply, RESP2 or RESP3 */
struct rv {
	char t;                            /* the type byte */
	long long n;                       /* integer, or an aggregate's size */
	char *s;                           /* bulk/simple/error text (in buf) */
	size_t len;
	struct rv *el;                     /* aggregate elements (malloc'd) */
	int nel;
};

/* S385: the largest bulk this thread's rv_parse accepts (-3 above it) -
 * UP_RBUF_MAX unless the thread sets its own (the writer and the pass
 * reader: upstream_pass_max_kb) */
extern __thread size_t rv_max;

/* free an aggregate's elements (the strings point into the read buffer) */
void rv_free(struct rv *v);
/* parse one reply at buf[*pos..len): 1 complete (*pos advanced), 0 need
 * more bytes, -1 malformed, -3 a bulk larger than rv_max.  Strings point
 * into buf; an aggregate's elements are malloc'd (rv_free) */
int rv_parse(char *buf, size_t len, size_t *pos, struct rv *v, int depth);
/* a simple-string OK */
int is_ok(const struct rv *v);

#endif
