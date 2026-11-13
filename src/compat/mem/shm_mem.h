/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* Shim for the vendored core's mem/shm_mem.h: threads share the heap.  shm_arena_* is not
 * provided; the core's !PCACHE_HAVE_MEM_ARENA fallback stubs it (the daemon never defines it). */
#ifndef PC_COMPAT_MEM_SHM_MEM_H
#define PC_COMPAT_MEM_SHM_MEM_H

#include <stdlib.h>

#define shm_malloc(sz) malloc(sz)
#define shm_free(p)    free(p)

#endif /* PC_COMPAT_MEM_SHM_MEM_H */
