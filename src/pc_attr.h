/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* pc_attr.h - PC_MUST_CHECK: an ignored return value is a bug, and -Werror gates it.  gcc
 * ignores a (void) cast here, so a call whose result may be dropped must not carry it. */
#ifndef PC_ATTR_H
#define PC_ATTR_H

#if defined(__GNUC__) || defined(__clang__)
#define PC_MUST_CHECK __attribute__((warn_unused_result))
#else
#define PC_MUST_CHECK
#endif

#endif /* PC_ATTR_H */
