/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* Shim for OpenSIPS mem/shm_mem.h — shared memory is just memory now:
 * threads share the heap.  The mem-arena API (shm_arena_*) is NOT
 * provided; the vendored code's own !PCACHE_HAVE_MEM_ARENA fallback
 * stubs it, and the daemon never defines that macro. */
#ifndef PC_COMPAT_MEM_SHM_MEM_H
#define PC_COMPAT_MEM_SHM_MEM_H

#include <stdlib.h>

#define shm_malloc(sz) malloc(sz)
#define shm_free(p)    free(p)

#endif /* PC_COMPAT_MEM_SHM_MEM_H */
