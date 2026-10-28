/* crc32ctest.c - CRC32C (crc32c.c; the 0.7.2 split, wave 1): the published
 * vectors, chaining, and every length and alignment against a bitwise
 * reference written here.  The backend is whichever this CPU gets: the
 * lazy choice on the first call, then pc_crc32c_select's.
 *
 *   A  the check value ("123456789") and RFC 3720's four 32-byte vectors,
 *      on the first call's lazy choice
 *   B  nothing hashed: 0 from 0, and any running value unchanged
 *   C  chaining: a running CRC carried across any split of a buffer is
 *      the CRC of the whole - how the snapshot writer hashes as it goes
 *   D  lengths 0..64 at offsets 0..7 equal the reference: the 8-byte
 *      loop, its tail, unaligned starts
 *   E  pc_crc32c_select: a name, and the same answers after it
 */
#include <stdio.h>
#include <string.h>
#include "crc32c.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* one bit at a time, the reflected Castagnoli polynomial */
static uint32_t ref(uint32_t crc, const unsigned char *p, size_t n)
{
	int k;

	crc = ~crc;
	while (n--) {
		crc ^= *p++;
		for (k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1)));
	}
	return ~crc;
}

static int vectors(void)
{
	unsigned char b[32];
	int i, ok;

	ok = pc_crc32c(0, "123456789", 9) == 0xE3069283u;
	memset(b, 0, sizeof b);
	ok &= pc_crc32c(0, b, 32) == 0x8A9136AAu;
	memset(b, 0xFF, sizeof b);
	ok &= pc_crc32c(0, b, 32) == 0x62A8AB43u;
	for (i = 0; i < 32; i++)
		b[i] = (unsigned char)i;
	ok &= pc_crc32c(0, b, 32) == 0x46DD794Eu;
	for (i = 0; i < 32; i++)
		b[i] = (unsigned char)(31 - i);
	ok &= pc_crc32c(0, b, 32) == 0x113FDB5Cu;
	return ok;
}

int main(void)
{
	unsigned char buf[80];
	unsigned int x = 2463534242u;
	const char *be;
	int i, k, n, bad;

	for (i = 0; i < (int)sizeof buf; i++) {    /* xorshift: the same every run */
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		buf[i] = (unsigned char)x;
	}

	/* A */
	CHK(vectors(), "A: 123456789 -> e3069283; 32 x 00, 32 x ff, 00..1f, 1f..00 - RFC 3720's four");
	CHK(ref(0, (const unsigned char *)"123456789", 9) == 0xE3069283u, "A: and the reference agrees");

	/* B */
	CHK(pc_crc32c(0, buf, 0) == 0 && pc_crc32c(0x12345678u, buf, 0) == 0x12345678u,
		"B: nothing hashed - 0 from 0, a running value unchanged");

	/* C */
	for (bad = 0, k = 0; k <= 64; k++)
		bad += pc_crc32c(pc_crc32c(0, buf, (size_t)k), buf + k, (size_t)(64 - k)) !=
			pc_crc32c(0, buf, 64);
	CHK(bad == 0, "C: carried across each of 65 splits of 64 bytes - the whole's CRC");

	/* D */
	for (bad = 0, k = 0; k < 8; k++)
		for (n = 0; n <= 64; n++)
			bad += pc_crc32c(0x5A5A5A5Au, buf + k, (size_t)n) != ref(0x5A5A5A5Au, buf + k, (size_t)n);
	CHK(bad == 0, "D: 65 lengths x 8 offsets - all equal to the reference");

	/* E */
	be = pc_crc32c_select();
	CHK(be && (!strcmp(be, "hardware") || !strcmp(be, "software")), "E: selected - %s", be ? be : "(null)");
	for (bad = 0, n = 0; n <= 64; n++)
		bad += pc_crc32c(0, buf + 3, (size_t)n) != ref(0, buf + 3, (size_t)n);
	CHK(vectors() && bad == 0, "E: and after it, the same answers");

	printf("crc32ctest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
