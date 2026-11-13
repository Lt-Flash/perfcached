/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* crc32c.h - CRC32C with per-arch dispatch and a software slice-by-8 fallback. */
#ifndef PC_CRC32C_H
#define PC_CRC32C_H

#include <stddef.h>
#include <stdint.h>

uint32_t pc_crc32c(uint32_t init, const void *buf, size_t len);
/* choose the backend now, the table built: "hardware" or "software" */
const char *pc_crc32c_select(void);
/* three buffers' CRC32Cs (each from 0) at once - one call, independent
 * chains, ~2x the throughput of three calls on hardware CRC */
void pc_crc32c_x3(const unsigned char *const p[3], const size_t n[3],
		uint32_t out[3]);

#endif
