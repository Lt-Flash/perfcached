/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* Shim for the vendored core's pt.h: `process_no` is a per-thread identity (stats
 * lines, the 12-bit bucket owner tag), set by compat_thread_register(). */
#ifndef PC_COMPAT_PT_H
#define PC_COMPAT_PT_H

extern __thread int process_no;

#endif /* PC_COMPAT_PT_H */
