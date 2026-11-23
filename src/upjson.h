/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* upjson.h - two JSON documents compared as values (the upfill sweep's judge) */
#ifndef PC_UPJSON_H
#define PC_UPJSON_H

#include <stddef.h>

/* 1 the same JSON value, 0 not, -1 not decided (too deep, refused by the tokenizer, no memory) -
 * counted uncompared, never different.  *@walked: the text alone did not settle it */
int up_json_same(const char *a, size_t al, const char *b, size_t bl, int *walked);
/* the calling thread's token buffer, freed */
void up_json_release(void);

#endif
