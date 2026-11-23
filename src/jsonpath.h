/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* jsonpath.h - JSON path verbs over opaque document blobs, parsed on demand and
 * spliced into @out; each returns the new length or a negative PC_JP_E_* */
#ifndef PC_JSONPATH_H
#define PC_JSONPATH_H

#include <stddef.h>

/* paths: $ .name [index] only; a missing intermediate parent is an error and
 * an array index must exist */
#define PC_JP_MAX       65536         /* documents live in cells */
#define PC_JP_E_DOC      (-1)          /* the stored value is not JSON */
#define PC_JP_E_PATH     (-2)          /* bad path syntax */
#define PC_JP_E_MISSING  (-3)          /* intermediate parent absent */
#define PC_JP_E_TYPE     (-4)          /* wrong node type on the way */
#define PC_JP_E_NOLEAF   (-5)          /* leaf absent (jget/jdel/xx) */
#define PC_JP_E_EXISTS   (-6)          /* leaf present (nx) */
#define PC_JP_E_VAL      (-7)          /* the new value is not JSON */
#define PC_JP_E_NUM      (-8)          /* jincr target is not a number */
#define PC_JP_E_SIZE     (-9)          /* result exceeds PC_JP_MAX */

/* is @val exactly one JSON value - object, array, string, number, true,
 * false or null - with nothing else around it?  1 yes, 0 no. */
int pc_jp_valid_value(const char *val, size_t vlen);

int pc_jp_get(const char *doc, size_t dlen, const char *path, size_t plen,
		char *out, size_t cap);

/* splice validated JSON @val at @path; nx: leaf absent only, xx: present only;
 * mkpath creates missing intermediate name steps as objects (never index steps) */
int pc_jp_set(const char *doc, size_t dlen, const char *path, size_t plen,
		const char *val, size_t vlen, int nx, int xx, int mkpath,
		char *out, size_t cap);

/* append @val to the ARRAY at @path (jarrappend); with @nvals > 1, @val is that many values
 * joined by commas, appended at once.  Returns the new document length, *count_out = elements
 * after the append. */
int pc_jp_append(const char *doc, size_t dlen, const char *path,
		size_t plen, const char *val, size_t vlen, int nvals, int *count_out,
		char *out, size_t cap);

/* delete the leaf at @path (root is not deletable here - use del) */
int pc_jp_del(const char *doc, size_t dlen, const char *path, size_t plen,
		char *out, size_t cap);

/* increment the number at @path by @by; *result carries the new value;
 * integer-only, as the store's counters are */
int pc_jp_incr(const char *doc, size_t dlen, const char *path, size_t plen,
		long long by, long long *result, char *out, size_t cap);

#endif /* PC_JSONPATH_H */
