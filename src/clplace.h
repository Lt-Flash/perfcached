/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clplace.h — placement over the map (control plane, step 4).
 *
 * Weighted rendezvous hashing.  Flat CRUSH is not CRUSH: nearly all of
 * that algorithm's complexity IS its hierarchy, and with rack-awareness
 * deferred what remains is "pick deterministically, respect weights,
 * move minimal data on change" - which is rendezvous with a weight term.
 *
 *   score_i = weight_i / -ln(u_i)      u_i = hrw_mix(key, node) in (0,1)
 *
 * Max for a single owner; the ranked list below it is what standby
 * ordering wants.
 *
 * IT IS COMPUTED ENTIRELY IN INTEGERS, AND THAT IS NOT AN OPTIMISATION.
 * Every node and every client must reach the SAME answer or ownership
 * splits and a key lives in two places.  Floating-point log is not a
 * good enough guarantee of that: results can differ with the libm
 * version, the compiler's contraction and FMA choices, x87 excess
 * precision, or -ffast-math in some downstream build.  A fixed-point
 * integer log is bit-identical on every machine that runs it, which for
 * a placement function matters more than the last decimal of accuracy.
 *
 * The arithmetic, which also removes the division and the logarithm's
 * base.  With D_i = (64 - log2(h_i)) in fixed point, so that
 * -ln(u_i) = D_i * ln2:
 *
 *     w_a / (D_a * ln2)  >  w_b / (D_b * ln2)
 *  => w_a * D_b          >  w_b * D_a          (ln2 cancels; D > 0)
 *
 * a pure integer comparison.
 *
 * THE MIGRATION PROPERTY FALLS OUT OF IT EXACTLY.  With equal weights
 * the comparison reduces to D_b > D_a, which is h_a > h_b - the plain
 * unweighted argmax the fleet uses today.  Not approximately the same
 * ranking: the SAME ranking, for the owner and for every position below
 * it, so a change of weight is the only thing that moves data.  A map
 * published
 * with uniform weights therefore moves nothing, and placement only
 * changes when somebody sets a weight.  That is the migration plan, and
 * the test asserts it against the unweighted function rather than
 * trusting the algebra above.
 */
#ifndef PC_CLPLACE_H
#define PC_CLPLACE_H

#include <stddef.h>
#include <stdint.h>

#include "clmap.h"

/* The key hash and the node mix, which MUST stay bit-identical to the
 * ones the cluster already uses - changing either moves every key. */
uint64_t pc_clplace_mix(uint64_t kh, uint32_t addr, uint16_t port);

/* The owner of @kh: the placeable node with the highest score, or 0 if
 * the map has none.  Ties break on node id so two nodes cannot disagree
 * about a key whose top scores collide. */
uint16_t pc_clplace_owner(const struct pc_clmap *m, uint64_t kh);

/* The top @max placeable nodes by score, best first, into @out.
 * Returns how many were written - fewer than @max means the map does
 * not hold that many placeable nodes, and the caller decides whether a
 * short list is usable or a refusal. */
int pc_clplace_rank(const struct pc_clmap *m, uint64_t kh,
		uint16_t *out, int max);

/* Exposed for the test: the fixed-point (64 - log2(h)) term. */
#define PC_CLPLACE_FBITS 20
uint64_t pc_clplace_dterm(uint64_t h);

#endif /* PC_CLPLACE_H */
