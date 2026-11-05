/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * respout.h - the RESP door's reply writers (verbs.c's; the 0.7.2 split,
 * wave 1).  Each appends one RESP2 item to @w; any thread.
 */
#ifndef PC_RESPOUT_H
#define PC_RESPOUT_H

#include <stddef.h>

struct pc_jw;

/* -ERR <msg> */
void resp_err(struct pc_jw *w, const char *msg);
/* -<code> <msg>: a code clients dispatch on (TRYAGAIN, MOVED, ...) - S38 */
void resp_err_code(struct pc_jw *w, const char *code, const char *msg);
/* -<line>: an error in Redis's own words, code and all - S280 */
void resp_err_line(struct pc_jw *w, const char *line);
/* +<s> */
void resp_simple(struct pc_jw *w, const char *s);
/* :<v> */
void resp_int(struct pc_jw *w, long long v);
/* $-1, the null bulk */
void resp_nil(struct pc_jw *w);
/* a string literal's length is the compiler's to count: JSON.DEBUG HELP
 * once sent 55 bytes of a 53-byte literal, two bytes of whatever
 * followed it in rodata - silent in a plain build, an abort under the
 * sanitizer (the rc18 tag pipeline).  Literals go through this. */
#define resp_bulk_lit(w, s) resp_bulk((w), (s), sizeof(s) - 1)
void resp_bulk(struct pc_jw *w, const char *p, size_t n);
/* *<n>, an array's header - its n elements follow */
void resp_arr(struct pc_jw *w, int n);
/* a command word @a (@al bytes) equals @b, ASCII case ignored */
int resp_is(const char *a, size_t al, const char *b);
/* @p (@n bytes) is a whole decimal integer: 0 and *@out, else -1 */
int resp_ll(const char *p, size_t n, long long *out);

#endif
