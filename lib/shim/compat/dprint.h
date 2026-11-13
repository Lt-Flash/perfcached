/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov - see lib/LICENSE */
/* libperfd's LM_* logging for sources shared with the daemon: -Ilib/shim ahead of
 * -Isrc picks this over the daemon's GPL header (test/mitboundtest.sh checks). */
#ifndef PERFD_SHIM_DPRINT_H
#define PERFD_SHIM_DPRINT_H

#define L_CRIT   1
#define L_ERR    2
#define L_WARN   3
#define L_NOTICE 4
#define L_INFO   5
#define L_DBG    6

/* to the perfd_set_log() callback, or with none, errors and worse to stderr */
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
