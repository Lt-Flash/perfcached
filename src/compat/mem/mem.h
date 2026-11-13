/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* Shim for the vendored core's mem/mem.h: pkg (per-process) allocation is the
 * plain heap; the core's own chunk allocator does the real memory work. */
#ifndef PC_COMPAT_MEM_MEM_H
#define PC_COMPAT_MEM_MEM_H

#include <stdlib.h>

#define pkg_malloc(sz) malloc(sz)
#define pkg_free(p)    free(p)

#endif /* PC_COMPAT_MEM_MEM_H */
