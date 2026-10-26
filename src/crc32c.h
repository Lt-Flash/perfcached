/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * crc32c.h - CRC32C with per-arch dispatch (crc32c.c).
 */
#ifndef PC_CRC32C_H
#define PC_CRC32C_H

#include <stddef.h>
#include <stdint.h>

/* CRC32C with per-arch dispatch (sw slice-by-8 fallback) */
uint32_t pc_crc32c(uint32_t init, const void *buf, size_t len);
/* choose the backend now, the table built: "hardware" or "software" */
const char *pc_crc32c_select(void);

#endif
