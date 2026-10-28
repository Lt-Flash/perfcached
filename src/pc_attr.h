/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * pc_attr.h — S59(b): compiler attributes with teeth.
 *
 * PC_MUST_CHECK marks a fallible call whose ignored return value is a
 * bug: a failed init, an unauthenticated handshake step, a durability
 * barrier whose verdict nobody read.  gcc and clang both honor
 * warn_unused_result, and -Werror makes it gate.
 *
 * There is deliberately NO sanctioned discard idiom: gcc ignores a
 * bare (void) cast for this attribute (the ancient glibc __wur war),
 * and a function whose every return value is legal to ignore should
 * simply not be annotated - the attribute would be a lie about it.
 * pc_rdb_request_save is the worked example: "already running" is a
 * success for every caller, so it carries no PC_MUST_CHECK.
 */
#ifndef PC_ATTR_H
#define PC_ATTR_H

#if defined(__GNUC__) || defined(__clang__)
#define PC_MUST_CHECK __attribute__((warn_unused_result))
#else
#define PC_MUST_CHECK
#endif

#endif /* PC_ATTR_H */
