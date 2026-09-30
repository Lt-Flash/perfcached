/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * walring.h - S226: copying a record out of a WAL producer ring.
 *
 * The pump drains each worker's ring into its stage buffer, and a record
 * may wrap the ring's end.  It used to copy byte by byte with a modulo
 * per byte, which was ~60% of a saturated WAL thread (1 KB records, the
 * storage out of the way): two memcpys, split at the end, do the same.
 * A header of its own so test/walringtest.c checks it against the old
 * loop at every split point without linking the WAL.
 */
#ifndef PC_WALRING_H
#define PC_WALRING_H
#include <stdint.h>
#include <string.h>

/* copy @n (<= @size) bytes from ring offset @pos (< @size), wrapping */
static inline void walring_copy(const unsigned char *buf, uint32_t size,
		uint32_t pos, unsigned char *dst, uint32_t n)
{
	uint32_t first = size - pos;

	if (n <= first) {
		memcpy(dst, buf + pos, n);
	} else {
		memcpy(dst, buf + pos, first);
		memcpy(dst + first, buf, n - first);
	}
}

#endif /* PC_WALRING_H */
