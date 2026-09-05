/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* Shim for OpenSIPS mem/mem.h — pkg (per-process) allocation.  One
 * process now: plain heap.  The core's own chunk allocator
 * (pcache_mem.c/pcache_arena.c) does the real memory work. */
#ifndef PC_COMPAT_MEM_MEM_H
#define PC_COMPAT_MEM_MEM_H

#include <stdlib.h>

#define pkg_malloc(sz) malloc(sz)
#define pkg_free(p)    free(p)

#endif /* PC_COMPAT_MEM_MEM_H */
