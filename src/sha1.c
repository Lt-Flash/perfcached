/* sha1.c - S170a: SHA-1 per FIPS 180-4 section 6.1.  See sha1.h. */
#include <stdint.h>
#include <string.h>

#include "sha1.h"

static uint32_t rol(uint32_t x, int n)
{
	return (x << n) | (x >> (32 - n));
}

static void block(uint32_t h[5], const unsigned char *b)
{
	uint32_t w[80], a, bb, c, d, e, t;
	int i;

	for (i = 0; i < 16; i++)
		w[i] = (uint32_t)b[4 * i] << 24 | (uint32_t)b[4 * i + 1] << 16 |
			(uint32_t)b[4 * i + 2] << 8 | (uint32_t)b[4 * i + 3];
	for (i = 16; i < 80; i++)
		w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	a = h[0]; bb = h[1]; c = h[2]; d = h[3]; e = h[4];
	for (i = 0; i < 80; i++) {
		uint32_t f, k;

		if (i < 20) {
			f = (bb & c) | (~bb & d);
			k = 0x5A827999u;
		} else if (i < 40) {
			f = bb ^ c ^ d;
			k = 0x6ED9EBA1u;
		} else if (i < 60) {
			f = (bb & c) | (bb & d) | (c & d);
			k = 0x8F1BBCDCu;
		} else {
			f = bb ^ c ^ d;
			k = 0xCA62C1D6u;
		}
		t = rol(a, 5) + f + e + k + w[i];
		e = d;
		d = c;
		c = rol(bb, 30);
		bb = a;
		a = t;
	}
	h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e;
}

void pc_sha1_hex(const void *p, size_t len, char out[41])
{
	static const char hex[] = "0123456789abcdef";
	uint32_t h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u,
		0xC3D2E1F0u };
	const unsigned char *m = p;
	unsigned char tail[128];
	uint64_t bits = (uint64_t)len * 8;
	size_t full = len / 64 * 64, rest = len - full, tl;
	int i;

	for (size_t o = 0; o < full; o += 64)
		block(h, m + o);
	memset(tail, 0, sizeof tail);
	if (rest)
		memcpy(tail, m + full, rest);
	tail[rest] = 0x80;
	tl = rest + 1 + 8 <= 64 ? 64 : 128;
	for (i = 0; i < 8; i++)
		tail[tl - 1 - i] = (unsigned char)(bits >> (8 * i));
	block(h, tail);
	if (tl == 128)
		block(h, tail + 64);
	for (i = 0; i < 20; i++) {
		unsigned char byte = (unsigned char)(h[i / 4] >> (24 - 8 * (i % 4)));

		out[2 * i] = hex[byte >> 4];
		out[2 * i + 1] = hex[byte & 15];
	}
	out[40] = 0;
}
