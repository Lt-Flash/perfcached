/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * pcache_asan.h - TM-2 (DESIGN 12ob): what AddressSanitizer is told about
 * the arena's cells.
 *
 * ASan sees the arena as mappings it was never told the shape of, so a
 * write past a record into the next cell, or a read through a pointer to
 * a cell already freed, is invisible to it.  An ASan build (and only an
 * ASan build - everything here is nothing otherwise) tells it:
 *  - a cell handed out is open whole: the table rewrites a record in
 *    place whenever the new one fits the cell's bound, and the evacuator
 *    copies the cell entire, so the requested bytes alone would be wrong;
 *  - a cell back with the allocator is closed past PCACHE_CELL_OPEN - its
 *    class byte (0) and its free-list link (8..15) stay open, because the
 *    allocator and the free path read them;
 *  - a slot taken for a chunk or a region is opened whole, a new chunk's
 *    cells then closed until each is handed out, and a retired chunk's
 *    cells closed again.
 *
 * The table's optimistic readers (scan_bucket, snapshot_slot) copy out
 * of a record through a pointer they validate only afterwards, by the
 * bucket's version (DESIGN 3.2) - by design they can read a cell freed a
 * moment ago and discard what they read.  They are not ASan's to judge:
 * PC_NO_ASAN on them, and their copies go through pc_raw_copy/pc_raw_cmp,
 * which the libc interceptors cannot see (a memcpy called from an
 * unsanitized function is still checked by ASan's interceptor).
 */
#ifndef PCACHE_ASAN_H
#define PCACHE_ASAN_H

#include <stddef.h>
#include <string.h>

#if defined(__SANITIZE_ADDRESS__)
#define PCACHE_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define PCACHE_ASAN 1
#endif
#endif

/* the bytes of a free cell that stay open: the class byte and the link */
#define PCACHE_CELL_OPEN 16

#ifdef PCACHE_ASAN
#include <sanitizer/asan_interface.h>

#define PC_ASAN_POISON(p, n)   ASAN_POISON_MEMORY_REGION((p), (n))
#define PC_ASAN_UNPOISON(p, n) ASAN_UNPOISON_MEMORY_REGION((p), (n))
#define PC_NO_ASAN             __attribute__((no_sanitize_address))

/* byte by byte through volatile: neither compiler may turn these into a
 * call to the intercepted memcpy/memcmp.  ASan builds only, where speed
 * is not the point */
static inline PC_NO_ASAN void pc_raw_copy(void *dst, const void *src,
		size_t n)
{
	volatile unsigned char *d = dst;
	const volatile unsigned char *s = src;

	while (n--)
		*d++ = *s++;
}

static inline PC_NO_ASAN int pc_raw_cmp(const void *a, const void *b,
		size_t n)
{
	const volatile unsigned char *x = a, *y = b;

	for (; n; n--, x++, y++)
		if (*x != *y)
			return *x < *y ? -1 : 1;
	return 0;
}
#else
#define PC_ASAN_POISON(p, n)   ((void)(p), (void)(n))
#define PC_ASAN_UNPOISON(p, n) ((void)(p), (void)(n))
#define PC_NO_ASAN
#define pc_raw_copy            memcpy
#define pc_raw_cmp             memcmp
#endif

#endif /* PCACHE_ASAN_H */
