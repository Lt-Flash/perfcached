/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * psinterest.h - which relays a peer wants (PS12).  Header-only and pure,
 * like psrelay.h, so the unit test drives exactly what the nodes run.
 *
 * A node set to `[cluster] pubsub_relay = interested` tells its peers what
 * it holds subscriptions for, and a peer relays to it only the publishes
 * that could match.  The RECEIVER opts in: a node on `all`, or a build
 * without this, is sent everything as before, so no node ever stops
 * receiving because of another node's setting.
 *
 * WHAT IS SENT.  Exact channel names go into a Bloom filter - a false
 * positive only costs a relay nobody needed, which is the safe direction,
 * and the whole set travels as one fixed-size datagram however many names
 * it holds.  Patterns travel as strings, because a sender has to run them
 * against the channel.  When a node gains its FIRST subscription to a name
 * it sends an update at once: the filter probes for a channel, the string
 * for a pattern.  Losing the last one sends nothing; the node rebuilds its
 * filter later, and a stale bit only relays a little extra.
 *
 * THE VERSION (the 64 bits the heartbeat carries; 0 = relay me everything):
 *     [ epoch 16 ][ rebuild 16 ][ adds 32 ]
 * epoch   drawn when the process starts, never 0
 * rebuild how many times the filter was rebuilt after removals
 * adds    one per update sent, so a receiver can tell it missed one
 *
 * THE VIEW a sender keeps of a peer: the version of the last full state it
 * applied, and which updates since then arrived.  It may filter only while
 * the peer's heartbeat version is COVERED - same epoch, and every update up
 * to the heartbeat's count applied.  A rebuild the view has not seen does
 * not break coverage: a rebuild only removes names, so the older state is
 * a superset and still safe; the sender just asks for the new one.
 * Anything else - no full state yet, another epoch, an update missing - and
 * the sender relays everything to that peer until a full state arrives.
 */
#ifndef PC_PSINTEREST_H
#define PC_PSINTEREST_H

#include <stddef.h>
#include <stdint.h>

/* ---- the filter ---------------------------------------------------------- */
#define PSB_BITS   (1u << 18)              /* 32 KiB on the wire */
#define PSB_BYTES  (PSB_BITS / 8)
#define PSB_K      4

/* two 32-bit hashes of a name; the probes are h1 + i * h2 */
static inline void psb_hash(const char *s, size_t n, uint32_t *h1,
		uint32_t *h2)
{
	uint64_t h = 1469598103934665603ULL;   /* FNV-1a, 64 bits */
	size_t i;

	for (i = 0; i < n; i++) {
		h ^= (unsigned char)s[i];
		h *= 1099511628211ULL;
	}
	/* FNV leaves the high bits of short names poorly mixed, and h2 comes
	 * from them: finish with splitmix64's mixer */
	h ^= h >> 30;
	h *= 0xbf58476d1ce4e5b9ULL;
	h ^= h >> 27;
	h *= 0x94d049bb133111ebULL;
	h ^= h >> 31;
	*h1 = (uint32_t)h;
	*h2 = (uint32_t)(h >> 32) | 1;         /* odd: every probe differs */
}

static inline void psb_add(uint8_t *bits, uint32_t h1, uint32_t h2)
{
	unsigned i;

	for (i = 0; i < PSB_K; i++) {
		uint32_t b = (h1 + i * h2) & (PSB_BITS - 1);

		bits[b >> 3] |= (uint8_t)(1u << (b & 7));
	}
}

static inline int psb_test(const uint8_t *bits, uint32_t h1, uint32_t h2)
{
	unsigned i;

	for (i = 0; i < PSB_K; i++) {
		uint32_t b = (h1 + i * h2) & (PSB_BITS - 1);

		if (!(bits[b >> 3] & (1u << (b & 7))))
			return 0;
	}
	return 1;
}

/* ---- the version ----------------------------------------------------------- */
static inline uint64_t psv_make(uint16_t epoch, uint16_t rebuild, uint32_t adds)
{
	return ((uint64_t)epoch << 48) | ((uint64_t)rebuild << 32) | adds;
}

static inline uint16_t psv_epoch(uint64_t v) { return (uint16_t)(v >> 48); }
static inline uint16_t psv_rebuild(uint64_t v) { return (uint16_t)(v >> 32); }
static inline uint32_t psv_adds(uint64_t v) { return (uint32_t)v; }

/* ---- the full state on the wire ------------------------------------------
 *     [version 8][flags 1][filter PSB_BYTES][npat 2] then per pattern
 *     [len 2][bytes]
 * When the patterns do not fit a datagram beside the filter, none are sent
 * and PSF_EVERYTHING says: relay me everything. */
#define PSF_EVERYTHING  0x01
#define PSF_HDR         (8 + 1 + PSB_BYTES + 2)
#define PSF_PATTERNS_MAX 30000             /* bytes of patterns, with lengths */

/* ---- a sender's view of one peer ----------------------------------------- */
struct psv_view {
	uint64_t base;                     /* the full state applied */
	uint32_t have;                     /* every update up to this one is in */
	uint64_t above;                    /* bit i: update have + 1 + i is in */
	uint8_t full;                      /* a full state has been applied */
};

/* may this full state be applied?  Not when it is older than updates the
 * view already holds - it would drop them */
static inline int psv_full_applies(const struct psv_view *v, uint64_t ver)
{
	return !v->full || psv_epoch(ver) != psv_epoch(v->base) ||
		psv_adds(ver) >= v->have;
}

static inline void psv_full(struct psv_view *v, uint64_t ver)
{
	v->base = ver;
	v->have = psv_adds(ver);
	v->above = 0;
	v->full = 1;
}

#define PSV_APPLY    1                     /* apply the update's content */
#define PSV_FOREIGN  0                     /* another epoch, or no full state:
                                            * do not apply; the view is stale */

/* an update arrived.  Its content is applied even beyond the window - it
 * is still true of the peer - but coverage then waits for a full state. */
static inline int psv_add(struct psv_view *v, uint64_t ver)
{
	uint32_t a = psv_adds(ver), d;

	if (!v->full || psv_epoch(ver) != psv_epoch(v->base))
		return PSV_FOREIGN;
	if (a <= v->have)
		return PSV_APPLY;                  /* already covered: harmless */
	d = a - v->have - 1;                   /* 0 = the next one */
	if (d < 64)
		v->above |= 1ULL << d;
	while (v->above & 1) {
		v->have++;
		v->above >>= 1;
	}
	return PSV_APPLY;
}

/* may the sender filter against a peer whose heartbeat says @hb? */
static inline int psv_covered(const struct psv_view *v, uint64_t hb)
{
	return v->full && hb && psv_epoch(hb) == psv_epoch(v->base) &&
		psv_adds(hb) <= v->have;
}

/* covered, but the peer rebuilt since: worth asking for the new state */
static inline int psv_behind_rebuild(const struct psv_view *v, uint64_t hb)
{
	return psv_covered(v, hb) && psv_rebuild(hb) != psv_rebuild(v->base);
}

#endif /* PC_PSINTEREST_H */
