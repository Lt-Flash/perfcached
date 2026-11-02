/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov
 * Part of libperfd - see lib/LICENSE.  This file must stay
 * free of src/core includes; tools/sync-libperfd.sh exports
 * exactly the MIT set to consumers. */
/*
 * pc_mix.h — the rendezvous mixing function, shared by everything that
 * decides where a key lives, header-only so the copies CANNOT drift.
 *
 * Placement is argmax over pc_hrw_mix(slot, node_addr, node_port).  The
 * daemon, libperfd and the cluster-map placer all compute it, and they
 * must agree bit for bit: a single differing bit reorders the argmax for
 * some keys, and those keys are then written on one node and looked for
 * on another.  This lived as three byte-identical copies (cluster.c,
 * lib/perfd.c, clplace.c) until 2026-08-31; pc_slot.h had already been
 * collapsed for the same reason one step earlier in the pipeline:
 *
 *     key --pc_key_slot--> slot --pc_hrw_mix--> weight --argmax--> owner
 *
 * The body is MurmurHash3's fmix64 finalizer over the key value XORed
 * with the node's address times the golden-ratio constant.  Do not
 * "improve" it.  It is not a general-purpose hash and its quality is
 * not the point - its STABILITY is.  Changing any constant here moves
 * every key in every deployment, and does so silently, because both
 * sides would change together and agree perfectly on the new wrong
 * answer.  If it ever must change, bump PC_ROUTE_ALGO with it so a
 * mismatched peer is refused rather than joined (see cluster.h), and
 * expect the fleet to refill.
 *
 * Exact values are pinned by test/clplacetest.c and test/slottest.c.
 *
 * Mixed over the node's STABLE advertise address, never its runtime id:
 * ids churn on rejoin and must not decide placement.
 */
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
