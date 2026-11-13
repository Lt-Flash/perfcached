/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clsgmix.h - the sweep gate's digest term for one record, as both ends compute it. */
#ifndef PC_CLSGMIX_H
#define PC_CLSGMIX_H

#include <stddef.h>
#include <stdint.h>

#include "crc32c.h"

/* FNV-1a 64 over the key, the version folded in, a splitmix finaliser.  With
 * @val (wire epoch 3) its CRC32C too: one version, other bytes, another term */
static inline uint64_t sg_mix(const char *k, size_t n, unsigned long long ver,
		const char *val, size_t vlen)
{
	uint64_t h = 1469598103934665603ULL;
	size_t i;

	for (i = 0; i < n; i++)
		h = (h ^ (unsigned char)k[i]) * 1099511628211ULL;
	h ^= ver * 0x9E3779B97F4A7C15ULL;
	if (val)
		h = (h ^ pc_crc32c(0, val, vlen)) * 1099511628211ULL;
	h ^= h >> 30; h *= 0xBF58476D1CE4E5B9ULL;
	h ^= h >> 27; h *= 0x94D049BB133111EBULL;
	h ^= h >> 31;
	return h;
}

#endif
