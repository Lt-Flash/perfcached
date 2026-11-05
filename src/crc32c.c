/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * crc32c.c - CRC32C (Castagnoli): the slice-by-8 table in software, the
 * CPU's instruction where there is one (SSE 4.2, ARMv8 CRC), chosen once.
 * The WAL's, the snapshot's and recovery's checksum.  Moved from wal.c
 * unchanged (the 0.7.2 split, wave 1 - doc/MODULARITY.md).
 */
#include <stdint.h>
#include <string.h>
#include "crc32c.h"

/* ---- CRC32C: slice-by-8 software + per-arch hardware dispatch ---------- */

static uint32_t crc_tab[8][256];

static void crc_init_tab(void)
{
	uint32_t i, j, c;

	for (i = 0; i < 256; i++) {
		c = i;
		for (j = 0; j < 8; j++)
			c = (c >> 1) ^ (0x82F63B78u & (0u - (c & 1)));
		crc_tab[0][i] = c;
	}
	for (i = 0; i < 256; i++)
		for (j = 1; j < 8; j++)
			crc_tab[j][i] = (crc_tab[j - 1][i] >> 8) ^
				crc_tab[0][crc_tab[j - 1][i] & 0xFF];
}

static uint32_t crc32c_sw(uint32_t crc, const unsigned char *p, size_t n)
{
	crc = ~crc;
	while (n >= 8) {
		uint32_t lo, hi;

		memcpy(&lo, p, 4);
		memcpy(&hi, p + 4, 4);
		lo ^= crc;
		crc = crc_tab[7][lo & 0xFF] ^ crc_tab[6][(lo >> 8) & 0xFF] ^
		      crc_tab[5][(lo >> 16) & 0xFF] ^ crc_tab[4][lo >> 24] ^
		      crc_tab[3][hi & 0xFF] ^ crc_tab[2][(hi >> 8) & 0xFF] ^
		      crc_tab[1][(hi >> 16) & 0xFF] ^ crc_tab[0][hi >> 24];
		p += 8;
		n -= 8;
	}
	while (n--)
		crc = (crc >> 8) ^ crc_tab[0][(crc ^ *p++) & 0xFF];
	return ~crc;
}

#if defined(__x86_64__)
__attribute__((target("sse4.2")))
static uint32_t crc32c_hw(uint32_t crc, const unsigned char *p, size_t n)
{
	crc = ~crc;
	while (n >= 8) {
		uint64_t v;

		memcpy(&v, p, 8);
		crc = (uint32_t)__builtin_ia32_crc32di(crc, v);
		p += 8;
		n -= 8;
	}
	while (n--)
		crc = __builtin_ia32_crc32qi(crc, *p++);
	return ~crc;
}
static int hw_ok(void) { return __builtin_cpu_supports("sse4.2"); }
/* S466: three independent chains overlap in the CRC unit - ~2x on 207 */
__attribute__((target("sse4.2")))
static void crc32c_x3_hw(const unsigned char *const p[3], const size_t n[3],
		uint32_t out[3])
{
	uint64_t c[3] = { 0xffffffffu, 0xffffffffu, 0xffffffffu }, v0, v1, v2;
	size_t m = n[0] < n[1] ? n[0] : n[1], i;
	int k;

	m = (m < n[2] ? m : n[2]) & ~(size_t)7;
	for (i = 0; i < m; i += 8) {
		memcpy(&v0, p[0] + i, 8);
		memcpy(&v1, p[1] + i, 8);
		memcpy(&v2, p[2] + i, 8);
		c[0] = __builtin_ia32_crc32di(c[0], v0);
		c[1] = __builtin_ia32_crc32di(c[1], v1);
		c[2] = __builtin_ia32_crc32di(c[2], v2);
	}
	for (k = 0; k < 3; k++)
		out[k] = crc32c_hw(~(uint32_t)c[k], p[k] + m, n[k] - m);
}
#elif defined(__aarch64__)
#include <sys/auxv.h>
#ifndef HWCAP_CRC32
#define HWCAP_CRC32 (1 << 7)
#endif
__attribute__((target("+crc")))
static uint32_t crc32c_hw(uint32_t crc, const unsigned char *p, size_t n)
{
	crc = ~crc;
	while (n >= 8) {
		uint64_t v;

		memcpy(&v, p, 8);
		crc = __builtin_aarch64_crc32cx(crc, v);
		p += 8;
		n -= 8;
	}
	while (n--)
		crc = __builtin_aarch64_crc32cb(crc, *p++);
	return ~crc;
}
static int hw_ok(void) { return !!(getauxval(AT_HWCAP) & HWCAP_CRC32); }
__attribute__((target("+crc")))
static void crc32c_x3_hw(const unsigned char *const p[3], const size_t n[3],
		uint32_t out[3])
{
	uint32_t c[3] = { 0xffffffffu, 0xffffffffu, 0xffffffffu };
	uint64_t v0, v1, v2;
	size_t m = n[0] < n[1] ? n[0] : n[1], i;
	int k;

	m = (m < n[2] ? m : n[2]) & ~(size_t)7;
	for (i = 0; i < m; i += 8) {
		memcpy(&v0, p[0] + i, 8);
		memcpy(&v1, p[1] + i, 8);
		memcpy(&v2, p[2] + i, 8);
		c[0] = __builtin_aarch64_crc32cx(c[0], v0);
		c[1] = __builtin_aarch64_crc32cx(c[1], v1);
		c[2] = __builtin_aarch64_crc32cx(c[2], v2);
	}
	for (k = 0; k < 3; k++)
		out[k] = crc32c_hw(~c[k], p[k] + m, n[k] - m);
}
#else
static uint32_t crc32c_hw(uint32_t crc, const unsigned char *p, size_t n)
{
	return crc32c_sw(crc, p, n);
}
static int hw_ok(void) { return 0; }
static void crc32c_x3_hw(const unsigned char *const p[3], const size_t n[3],
		uint32_t out[3])
{
	int k;

	for (k = 0; k < 3; k++)
		out[k] = crc32c_sw(0, p[k], n[k]);
}
#endif

static uint32_t (*crc_fn)(uint32_t, const unsigned char *, size_t);

uint32_t pc_crc32c(uint32_t init, const void *buf, size_t len)
{
	if (!crc_fn) {                     /* lazy: CLI tools never wal_init */
		crc_init_tab();
		crc_fn = hw_ok() ? crc32c_hw : crc32c_sw;
	}
	return crc_fn(init, buf, len);
}

void pc_crc32c_x3(const unsigned char *const p[3], const size_t n[3],
		uint32_t out[3])
{
	int k;

	if (!crc_fn)
		(void)pc_crc32c(0, "", 0);
	if (crc_fn == crc32c_sw)
		for (k = 0; k < 3; k++)
			out[k] = crc32c_sw(0, p[k], n[k]);
	else
		crc32c_x3_hw(p, n, out);
}

/* the backend, chosen now (pc_wal_init: said once at start): its name */
const char *pc_crc32c_select(void)
{
	crc_init_tab();
	crc_fn = hw_ok() ? crc32c_hw : crc32c_sw;
	return crc_fn == crc32c_sw ? "software" :
		"hardware";
}
