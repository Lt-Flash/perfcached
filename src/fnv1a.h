/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* fnv1a.h - FNV-1a 64, written down once: nothing else in src/ spells its constants, and
 * test/clinittest.c pins the published vectors.  Not in the libperfd MIT export set. */
#ifndef PC_FNV1A_H
#define PC_FNV1A_H

#include <stddef.h>
#include <stdint.h>

#define FNV1A64_BASIS	14695981039346656037ULL		/* 0xcbf29ce484222325 */
#define FNV1A64_PRIME	1099511628211ULL		/* 0x100000001b3 */

/* one byte into a running hash */
static inline uint64_t fnv1a64_byte(uint64_t h, unsigned char b)
{
	return (h ^ b) * FNV1A64_PRIME;
}

/* n more bytes into a running hash */
static inline uint64_t fnv1a64_more(uint64_t h, const void *p, size_t n)
{
	const unsigned char *b = (const unsigned char *)p;
	size_t i;

	for (i = 0; i < n; i++)
		h = fnv1a64_byte(h, b[i]);
	return h;
}

/* a whole buffer, from the basis - the published-vector entry point */
static inline uint64_t fnv1a64(const void *p, size_t n)
{
	return fnv1a64_more(FNV1A64_BASIS, p, n);
}

/* a 64-bit field, little-endian (the wire order, clcodec.h), into a running hash: what a
 * digest over FIELDS rather than bytes wants */
static inline uint64_t fnv1a64_u64(uint64_t h, uint64_t v)
{
	int i;

	for (i = 0; i < 8; i++)
		h = fnv1a64_byte(h, (unsigned char)(v >> (i * 8)));
	return h;
}

#endif /* PC_FNV1A_H */
