/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov - see lib/LICENSE */
/*
 * lib/shim/compat/dprint.h - S370: libperfd's own logging, for the sources it
 * shares with the daemon (src/pc_noise.c logs through LM_*).  They include
 * <compat/dprint.h> in angle brackets, so the include path decides: the
 * library's objects are built with -Ilib/shim ahead of -Isrc and get this
 * file; the daemon builds with -Isrc and gets its own.  The daemon's header
 * is GPL and is never opened for the library (S371: test/mitboundtest.sh
 * lists what the compiler reads).  Messages go to the consumer's
 * perfd_set_log() callback, or, with none, errors and worse to stderr.
 */
#ifndef PERFD_SHIM_DPRINT_H
#define PERFD_SHIM_DPRINT_H

#define L_CRIT   1
#define L_ERR    2
#define L_WARN   3
#define L_NOTICE 4
#define L_INFO   5
#define L_DBG    6

void pdl_log(int level, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

#define LM_BUG(fmt, ...)    pdl_log(L_CRIT,   fmt, ##__VA_ARGS__)
#define LM_CRIT(fmt, ...)   pdl_log(L_CRIT,   fmt, ##__VA_ARGS__)
#define LM_ERR(fmt, ...)    pdl_log(L_ERR,    fmt, ##__VA_ARGS__)
#define LM_WARN(fmt, ...)   pdl_log(L_WARN,   fmt, ##__VA_ARGS__)
#define LM_NOTICE(fmt, ...) pdl_log(L_NOTICE, fmt, ##__VA_ARGS__)
#define LM_INFO(fmt, ...)   pdl_log(L_INFO,   fmt, ##__VA_ARGS__)
#define LM_DBG(fmt, ...)    pdl_log(L_DBG,    fmt, ##__VA_ARGS__)

#endif /* PERFD_SHIM_DPRINT_H */
