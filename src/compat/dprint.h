/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* Shim for OpenSIPS dprint.h — the seven LM_* macros the vendored core
 * uses, on OpenSIPS level numbering (lower = more severe), gated by
 * compat_log_level and written to stderr.  Keep log strings ASCII. */
#ifndef PC_COMPAT_DPRINT_H
#define PC_COMPAT_DPRINT_H

/* upstream dprint.h exposes stdio to its includers; vendored code relies
 * on that (snprintf and friends without its own #include) */
#include <stdio.h>

#define L_ALERT  (-3)
#define L_CRIT   (-2)
#define L_ERR    (-1)
#define L_WARN    1
#define L_NOTICE  2
#define L_INFO    3
#define L_DBG     4

extern int compat_log_level;

void compat_log(int level, const char *tag, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

#define PC_LOG(lev, tag, fmt, ...) \
	do { \
		if ((lev) <= compat_log_level) \
			compat_log((lev), (tag), fmt, ##__VA_ARGS__); \
	} while (0)

#define LM_BUG(fmt, ...)    PC_LOG(L_CRIT,   "BUG",      fmt, ##__VA_ARGS__)
#define LM_CRIT(fmt, ...)   PC_LOG(L_CRIT,   "CRITICAL", fmt, ##__VA_ARGS__)
#define LM_ERR(fmt, ...)    PC_LOG(L_ERR,    "ERROR",    fmt, ##__VA_ARGS__)
#define LM_WARN(fmt, ...)   PC_LOG(L_WARN,   "WARNING",  fmt, ##__VA_ARGS__)
#define LM_NOTICE(fmt, ...) PC_LOG(L_NOTICE, "NOTICE",   fmt, ##__VA_ARGS__)
#define LM_INFO(fmt, ...)   PC_LOG(L_INFO,   "INFO",     fmt, ##__VA_ARGS__)
#define LM_DBG(fmt, ...)    PC_LOG(L_DBG,    "DBG",      fmt, ##__VA_ARGS__)

#endif /* PC_COMPAT_DPRINT_H */
