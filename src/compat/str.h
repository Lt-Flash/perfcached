/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* Shim for OpenSIPS str.h — the counted string the whole core speaks. */
#ifndef PC_COMPAT_STR_H
#define PC_COMPAT_STR_H

typedef struct pc_str {
	char *s;
	int len;
} str;

#define str_init(_s) ((str){ (char *)(_s), sizeof(_s) - 1 })
#define STR_NULL     ((str){ 0, 0 })

#endif /* PC_COMPAT_STR_H */
