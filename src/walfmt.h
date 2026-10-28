/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * walfmt.h - the WAL's on-disk format helpers, shared by its writer
 * (wal.c) and its reader (walscan.c): the largest record, and the
 * little-endian field accessors.  Moved from wal.c (the 0.7.2 split,
 * wave 1); the format itself is wal.h's.
 */
#ifndef PC_WALFMT_H
#define PC_WALFMT_H

#include <stdint.h>
#include "wal.h"
#include "core/pcache_arena.h"          /* PCACHE_CELL_MAX: the record ceiling */

/* S174: the WAL has to carry any record the STORE accepts, or a write
 * the client was told succeeded is not there after a restart.  It was a
 * flat 80 KB, which was above the old 64 KB cell ceiling and below the
 * new one - so it is derived from the ceiling now and cannot drift
 * behind it again.  A record is header + fixed fields + collection name
 * + key + value, and key and value together cannot exceed a cell. */
#define MAX_REC     ((long)(PC_WAL_RECHDR + 32 + 255 + PCACHE_CELL_MAX))

/* ---- LE helpers -------------------------------------------------------- */

static inline void w32(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16);
	p[3] = (unsigned char)(v >> 24);
}

static inline void w16(unsigned char *p, uint16_t v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
}

static inline void w64(unsigned char *p, uint64_t v)
{
	w32(p, (uint32_t)v);
	w32(p + 4, (uint32_t)(v >> 32));
}

static inline uint32_t r32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint16_t r16(const unsigned char *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint64_t r64(const unsigned char *p)
{
	return (uint64_t)r32(p) | ((uint64_t)r32(p + 4) << 32);
}

#endif
