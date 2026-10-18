/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * clcodec.h - the little-endian wire codec, and nothing else.
 *
 * Every frame this daemon puts on the wire is little-endian, and six
 * one-line helpers encode that convention.  They lived as statics inside
 * cluster.c, where 199 call sites could reach them and no module could:
 * clbulk consequently hand-rolled the same 16-bit pack and unpack in
 * three lines of its own, which is one convention implemented twice and
 * exactly the drift a shared header prevents.
 *
 * They hold NO state, take no lock and name no thread, so unlike the
 * rest of the M-series this is not an extraction of ownership - it is
 * removing a reason for the modules to reach back into cluster.c.  M10's
 * coupling measurement counts these six among its dependencies; with
 * this header they stop counting.
 *
 * static inline, not extern: they are single expressions on the hottest
 * paths in the daemon, and an out-of-line call per field would be a real
 * cost for no benefit.
 */
#ifndef PC_CLCODEC_H
#define PC_CLCODEC_H

#include <stdint.h>
#include <string.h>       /* pc_wraw/pc_rraw */

static inline void pc_p16(unsigned char *p, uint16_t v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
}
static inline void pc_p32(unsigned char *p, uint32_t v)
{
	pc_p16(p, (uint16_t)v);
	pc_p16(p + 2, (uint16_t)(v >> 16));
}
static inline void pc_p64(unsigned char *p, uint64_t v)
{
	pc_p32(p, (uint32_t)v);
	pc_p32(p + 4, (uint32_t)(v >> 32));
}
static inline uint16_t pc_g16(const unsigned char *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}
static inline uint32_t pc_g32(const unsigned char *p)
{
	return (uint32_t)pc_g16(p) | ((uint32_t)pc_g16(p + 2) << 16);
}
static inline uint64_t pc_g64(const unsigned char *p)
{
	return (uint64_t)pc_g32(p) | ((uint64_t)pc_g32(p + 4) << 32);
}


/* ---- cursors -------------------------------------------------------
 *
 * A frame's field positions used to be written down up to four times -
 * once in the build, once in the parse, once again inside the length
 * gate that guards it, and a fourth time in the frame's total length -
 * with nothing making the four agree.  A cursor removes the arithmetic
 * instead of checking it: position is implied by ORDER, and the total
 * is whatever was written.
 *
 * The read side is where this earns its keep.  Every frame in this
 * daemon is ADDITIVE - fields appended at the tail, older peers sending
 * short - so each field needs a "was it there?" test.  With a cursor,
 * "did the read fit" IS that test, so the gates stop being hand-computed
 * offsets and become the reads themselves.
 *
 * pc_rfits() is for a GROUP: today's gates admit a run of fields all or
 * nothing, and reading them one at a time would half-fill a struct at a
 * length that lands mid-group.  Peek for the whole group, then read it.
 *
 * The write cursor is bounds-checked and latches `ok` on overflow rather
 * than truncating, because the failure it prevents is silent: clwire's
 * beat store drops an oversized frame without a word.
 */
struct pc_wcur { unsigned char *p, *end; int ok; };

static inline void pc_wcur_init(struct pc_wcur *c, unsigned char *buf,
		size_t cap)
{
	c->p = buf;
	c->end = buf + cap;
	c->ok = 1;
}
static inline size_t pc_wcur_len(const struct pc_wcur *c, const unsigned char *buf)
{
	return (size_t)(c->p - buf);
}
static inline int pc_wroom(struct pc_wcur *c, size_t n)
{
	if (!c->ok || (size_t)(c->end - c->p) < n) {
		c->ok = 0;
		return 0;
	}
	return 1;
}
static inline void pc_w8(struct pc_wcur *c, unsigned v)
{
	if (pc_wroom(c, 1))
		*c->p++ = (unsigned char)v;
}
static inline void pc_w16(struct pc_wcur *c, uint16_t v)
{
	if (pc_wroom(c, 2)) { pc_p16(c->p, v); c->p += 2; }
}
static inline void pc_w32(struct pc_wcur *c, uint32_t v)
{
	if (pc_wroom(c, 4)) { pc_p32(c->p, v); c->p += 4; }
}
static inline void pc_w64(struct pc_wcur *c, uint64_t v)
{
	if (pc_wroom(c, 8)) { pc_p64(c->p, v); c->p += 8; }
}
/* verbatim bytes - for values that are ALREADY in wire order (a
 * sockaddr's address and port) and must not be re-packed */
static inline void pc_wraw(struct pc_wcur *c, const void *src, size_t n)
{
	if (pc_wroom(c, n)) { memcpy(c->p, src, n); c->p += n; }
}

struct pc_rcur { const unsigned char *p, *end; };

static inline void pc_rcur_init(struct pc_rcur *c, const unsigned char *buf,
		size_t n)
{
	c->p = buf;
	c->end = buf + n;
}
/* Are n more bytes present?  Peek before reading a GROUP: today's gates
 * admit a run of fields all or nothing, and reading them one at a time
 * would half-fill the struct at a length landing mid-group.
 *
 * The p <= end half is not belt-and-braces.  Without it, a cursor that
 * has been advanced past the end computes end - p as a NEGATIVE
 * ptrdiff, and the cast to size_t turns that into SIZE_MAX - so every
 * subsequent fits() says yes and the reads walk off the buffer.  A
 * zero-length frame reached exactly that state. */
static inline int pc_rfits(const struct pc_rcur *c, size_t n)
{
	return c->p <= c->end && (size_t)(c->end - c->p) >= n;
}
/* Bounded: never past the end, for the same reason. */
static inline int pc_rskip(struct pc_rcur *c, size_t n)
{
	if (!pc_rfits(c, n))
		return 0;
	c->p += n;
	return 1;
}
static inline int pc_r8(struct pc_rcur *c, int *out)
{
	if (!pc_rfits(c, 1)) return 0;
	*out = *c->p++;
	return 1;
}
static inline int pc_r16(struct pc_rcur *c, int *out)
{
	if (!pc_rfits(c, 2)) return 0;
	*out = pc_g16(c->p); c->p += 2;
	return 1;
}
static inline int pc_r32(struct pc_rcur *c, uint32_t *out)
{
	if (!pc_rfits(c, 4)) return 0;
	*out = pc_g32(c->p); c->p += 4;
	return 1;
}
static inline int pc_r64(struct pc_rcur *c, uint64_t *out)
{
	if (!pc_rfits(c, 8)) return 0;
	*out = pc_g64(c->p); c->p += 8;
	return 1;
}
/* a run of bytes IN PLACE - returns a pointer into the caller's frame,
 * or NULL if it is not wholly there */
static inline const unsigned char *pc_rbytes(struct pc_rcur *c, size_t n)
{
	const unsigned char *at = c->p;

	if (!pc_rfits(c, n)) return NULL;
	c->p += n;
	return at;
}
/* verbatim into the caller's storage, for already-wire-order values */
static inline int pc_rraw(struct pc_rcur *c, void *dst, size_t n)
{
	if (!pc_rfits(c, n)) return 0;
	memcpy(dst, c->p, n); c->p += n;
	return 1;
}

/* ---- unchecked reads: the standard parsing idiom -------------------
 *
 *     if (!pc_rfits(&c, <the whole group>))
 *             ...;                       <- the ONLY bounds check
 *     x = pc_ru32(&c);
 *     y = pc_ru16(&c);                   <- covered by the peek above
 *
 * The peek proves the group is present, so re-checking inside each
 * field is a branch that cannot fail.  It is not free: measured on a
 * 23-byte, 8-field frame compiled out of line at -O2, per-field
 * checking cost +3.30 ns against hand-written offsets (9.78 vs 6.48),
 * and the peek-once form brought that to -0.15 ns - parity.  14
 * conditional branches became 1.
 *
 * This is exactly the contract the hand-written `n >= X` gates had, and
 * the peek sits directly above the reads it covers, so the two cannot
 * drift the way an offset and its gate could.  Never reach for these
 * without a peek that covers every byte they read.
 */
static inline int pc_ru8(struct pc_rcur *c)
{
	return *c->p++;
}
static inline int pc_ru16(struct pc_rcur *c)
{
	int v = pc_g16(c->p); c->p += 2; return v;
}
static inline uint32_t pc_ru32(struct pc_rcur *c)
{
	uint32_t v = pc_g32(c->p); c->p += 4; return v;
}
static inline uint64_t pc_ru64(struct pc_rcur *c)
{
	uint64_t v = pc_g64(c->p); c->p += 8; return v;
}
/* a run of bytes in place */
static inline const unsigned char *pc_rubytes(struct pc_rcur *c, size_t n)
{
	const unsigned char *at = c->p; c->p += n; return at;
}
/* verbatim into the caller's storage */
static inline void pc_ruraw(struct pc_rcur *c, void *dst, size_t n)
{
	memcpy(dst, c->p, n); c->p += n;
}

#endif /* PC_CLCODEC_H */
