/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* Shim for OpenSIPS pt.h — `process_no` becomes a per-THREAD identity.
 * Vendored code uses it for per-identity stats lines and the 12-bit
 * bucket owner tag; workers get theirs via compat_thread_register(). */
#ifndef PC_COMPAT_PT_H
#define PC_COMPAT_PT_H

extern __thread int process_no;

#endif /* PC_COMPAT_PT_H */
