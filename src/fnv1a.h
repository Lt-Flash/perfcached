/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * fnv1a.h - FNV-1a 64, written down once.
 *
 * Seven places in this daemon hash with FNV-1a's two constants, and each
 * carried its own copy of them.  Six spelled the offset basis in
 * decimal, and all six were the same wrong number - 1469598103934665603,
 * one digit short of 14695981039346656037.  The seventh spelled it in
 * hex, 0xcbf29ce484222325, and was right.
 *
 * Nothing caught it, and nothing could: a hash here is only ever
 * compared against another value produced by the same code, so a
 * wrong-but-consistent basis gives a wrong-but-consistent answer, and
 * the prime was right so it still avalanched.  It simply was not FNV-1a,
 * and it matched no published vector - which is the one check that would
 * have found it, and the one check a private copy of a constant never
 * gets.
 *
 * So the constants live here and nothing else in src/ spells them out;
 * test/clinittest.c pins the published vectors ("", "a", "foobar")
 * against clinit_fnv1a64(), which is this basis and this prime.  That
 * check is the only one there is - nothing stops a future eighth copy
 * being typed out somewhere else, it just has no excuse to be.
 *
 * Header-only and static inline for pc_mix.h's reason: single
 * expressions on paths that run per request.
 *
 * NOT in the libperfd MIT export set - nothing under lib/ hashes with
 * FNV, and the pc_ prefix is reserved for what tools/sync-libperfd.sh
 * copies out.
 */
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

/* a 64-bit field, little-endian, into a running hash: what a digest over
 * FIELDS rather than bytes wants, and the shape those digests already
 * had.  Little-endian to match the wire order used everywhere else
 * (clcodec.h) - these digests never leave the host, but one byte order
 * beats two. */
static inline uint64_t fnv1a64_u64(uint64_t h, uint64_t v)
{
	int i;

	for (i = 0; i < 8; i++)
		h = fnv1a64_byte(h, (unsigned char)(v >> (i * 8)));
	return h;
}

#endif /* PC_FNV1A_H */
