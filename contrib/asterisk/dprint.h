/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* The consumer's half of the libperfd export (tools/sync-libperfd.sh):
 * the LM_* macros the vendored files use, onto Asterisk's logger through
 * func_perfd_log() in func_perfd.c.  Installed as src/compat/dprint.h
 * beside the export.  It includes no Asterisk header, so the vendored
 * files never see astmm.h's allocator bans. */
#ifndef FUNC_PERFD_DPRINT_H
#define FUNC_PERFD_DPRINT_H

/* the daemon's shim exposes stdio to its includers; the export relies on it */
#include <stdio.h>

#define FUNC_PERFD_LOG_ERROR    0
#define FUNC_PERFD_LOG_WARNING  1
#define FUNC_PERFD_LOG_NOTICE   2
#define FUNC_PERFD_LOG_DEBUG    3

void func_perfd_log(int level, const char *file, int line, const char *func,
		const char *fmt, ...) __attribute__((format(printf, 5, 6)));

#define FUNC_PERFD_LM(lev, fmt, ...) \
	func_perfd_log((lev), __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define LM_BUG(fmt, ...)    FUNC_PERFD_LM(FUNC_PERFD_LOG_ERROR, fmt, ##__VA_ARGS__)
#define LM_CRIT(fmt, ...)   FUNC_PERFD_LM(FUNC_PERFD_LOG_ERROR, fmt, ##__VA_ARGS__)
#define LM_ERR(fmt, ...)    FUNC_PERFD_LM(FUNC_PERFD_LOG_ERROR, fmt, ##__VA_ARGS__)
#define LM_WARN(fmt, ...)   FUNC_PERFD_LM(FUNC_PERFD_LOG_WARNING, fmt, ##__VA_ARGS__)
#define LM_NOTICE(fmt, ...) FUNC_PERFD_LM(FUNC_PERFD_LOG_NOTICE, fmt, ##__VA_ARGS__)
#define LM_INFO(fmt, ...)   FUNC_PERFD_LM(FUNC_PERFD_LOG_DEBUG, fmt, ##__VA_ARGS__)
#define LM_DBG(fmt, ...)    FUNC_PERFD_LM(FUNC_PERFD_LOG_DEBUG, fmt, ##__VA_ARGS__)

#endif
