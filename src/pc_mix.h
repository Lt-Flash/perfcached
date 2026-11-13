/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov
 * Part of libperfd - see lib/LICENSE.  This file must stay
 * free of src/core includes; tools/sync-libperfd.sh exports
 * exactly the MIT set to consumers. */
/* pc_mix.h - the rendezvous mix every placer shares, bit for bit: changing it moves every key and
 * needs a PC_ROUTE_ALGO bump.  Mixed over the stable advertise address, never the runtime id. */
#ifndef PC_MIX_H
#define PC_MIX_H

#include <stdint.h>

static inline uint64_t pc_hrw_mix(uint64_t kh, uint32_t addr, uint16_t port)
{
	uint64_t h = kh ^ ((((uint64_t)addr << 16) | port) *
		0x9E3779B97F4A7C15ull);

	h ^= h >> 33;
	h *= 0xff51afd7ed558ccdull;
	h ^= h >> 33;
	h *= 0xc4ceb9fe1a85ec53ull;
	h ^= h >> 33;
	return h;
}

#endif /* PC_MIX_H */
