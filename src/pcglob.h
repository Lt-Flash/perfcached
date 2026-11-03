/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * pcglob.h - the key glob: Redis-style * ? [abc] [a-c] [^a] [!a] and
 * backslash escapes, over byte strings with lengths (no terminators).
 * An unterminated class matches nothing.  KEYS, SCAN MATCH, the hash
 * field and collection filters, and the read-through allow-list (S331)
 * all match with it.
 */
#ifndef PC_GLOB_H
#define PC_GLOB_H

/* 1 when @s (@sn bytes) matches the pattern @p (@pn bytes), else 0 */
int pc_glob(const char *p, int pn, const char *s, int sn);
/* S331: the same matcher, under the name verbs.h has always given it */
int pc_glob_match(const char *p, int pn, const char *s, int sn);

#endif
