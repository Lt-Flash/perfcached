/* SPDX-License-Identifier: GPL-2.0-or-later */
/* psinterest.h - which publishes a peer on `pubsub_relay = interested` wants relayed (the receiver
 * opts in); header-only and pure so the unit test drives exactly what the nodes run */
#ifndef PC_PSINTEREST_H
#define PC_PSINTEREST_H

#include <stddef.h>
#include <stdint.h>

/* ---- the filter: exact names; a false positive only costs an unneeded relay ---- */
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

/* ---- the version: [epoch 16, never 0][rebuild 16][adds 32, one per update sent] ---- */
static inline uint64_t psv_make(uint16_t epoch, uint16_t rebuild, uint32_t adds)
{
	return ((uint64_t)epoch << 48) | ((uint64_t)rebuild << 32) | adds;
}

static inline uint16_t psv_epoch(uint64_t v) { return (uint16_t)(v >> 48); }
static inline uint16_t psv_rebuild(uint64_t v) { return (uint16_t)(v >> 32); }
static inline uint32_t psv_adds(uint64_t v) { return (uint32_t)v; }

/* full state: [version 8][flags 1][filter PSB_BYTES][npat 2] then [len 2][bytes] per pattern;
 * patterns that do not fit beside the filter are not sent and PSF_EVERYTHING is set */
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

/* may the sender filter against a peer whose heartbeat says @hb (0 = relay all)?  A rebuild
 * only removes names, so the older state it covers is a safe superset */
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
