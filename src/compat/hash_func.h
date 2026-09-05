/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* Shim for OpenSIPS hash_func.h — intentionally empty.  The vendored
 * htable hashes with its own MurmurHash3; this include is vestigial
 * upstream and kept only so the vendored diff stays minimal. */
#ifndef PC_COMPAT_HASH_FUNC_H
#define PC_COMPAT_HASH_FUNC_H

#endif /* PC_COMPAT_HASH_FUNC_H */
