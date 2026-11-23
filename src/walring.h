/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* walring.h - copying a record out of a WAL producer ring, which may wrap its end; a header of
 * its own so test/walringtest.c checks it without linking the WAL. */
#ifndef PC_WALRING_H
#define PC_WALRING_H
#include <stdint.h>
#include <string.h>

/* a ring of @kb KB (<= 2^21), rounded up to a power of two: only a size that divides 2^32
 * keeps the free-running counters' places continuous across their wrap */
static inline uint32_t walring_size(int kb)
{
	uint32_t want = (uint32_t)kb << 10, size = 1;

	while (size < want)
		size <<= 1;
	return size;
}

/* where the free-running byte counter @c falls in a ring of @size (a power of two) */
static inline uint32_t walring_pos(uint32_t c, uint32_t size)
{
	return c & (size - 1);
}

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
