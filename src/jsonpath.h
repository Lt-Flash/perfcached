/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * jsonpath.h — the JSON path verbs' engine (task S26, the decided v1.1).
 *
 * Documents stay OPAQUE BLOBS in the store; every operation here is
 * parse-on-demand over the in-house tokenizer, splicing the document
 * text - no tree storage, nothing in WAL/replication changes shape.
 *
 * Path subset: $ (root), .name, [index] - e.g. $.a.b[2].c.  No filters,
 * no recursive descent, no wildcard.  Rules (pinned in the design):
 * root $ replaces the whole (validated) document; setting a NEW leaf
 * under an existing object creates it; a missing INTERMEDIATE parent is
 * an error (no deep-create); array indexes must exist (append is
 * jarrappend's job, later); type conflicts error.
 *
 * All functions splice into @out (cap PC_JP_MAX) and return the new
 * document length, or a negative PC_JP_E_* code.
 */
#ifndef PC_JSONPATH_H
#define PC_JSONPATH_H

#include <stddef.h>

#define PC_JP_MAX        65536         /* documents live in cells */
#define PC_JP_E_DOC      (-1)          /* the stored value is not JSON */
#define PC_JP_E_PATH     (-2)          /* bad path syntax */
#define PC_JP_E_MISSING  (-3)          /* intermediate parent absent */
#define PC_JP_E_TYPE     (-4)          /* wrong node type on the way */
#define PC_JP_E_NOLEAF   (-5)          /* leaf absent (jget/jdel/xx) */
#define PC_JP_E_EXISTS   (-6)          /* leaf present (nx) */
#define PC_JP_E_VAL      (-7)          /* the new value is not JSON */
#define PC_JP_E_NUM      (-8)          /* jincr target is not a number */
#define PC_JP_E_SIZE     (-9)          /* result exceeds PC_JP_MAX */

/* extract: copy the fragment at @path into @out */
int pc_jp_get(const char *doc, size_t dlen, const char *path, size_t plen,
		char *out, size_t cap);

/* set: splice @val (itself validated JSON) at @path.  nx: only when the
 * leaf is absent; xx: only when present.  mkpath: missing INTERMEDIATE
 * name steps are created as nested objects (index steps never are). */
int pc_jp_set(const char *doc, size_t dlen, const char *path, size_t plen,
		const char *val, size_t vlen, int nx, int xx, int mkpath,
		char *out, size_t cap);

/* append @val to the ARRAY at @path (jarrappend).  Returns the new
 * document length, *count_out = elements after the append. */
int pc_jp_append(const char *doc, size_t dlen, const char *path,
		size_t plen, const char *val, size_t vlen, int *count_out,
		char *out, size_t cap);

/* delete the leaf at @path (root is not deletable here - use del) */
int pc_jp_del(const char *doc, size_t dlen, const char *path, size_t plen,
		char *out, size_t cap);

/* increment the number at @path by @by; *result carries the new value.
 * Integer-only in v1 (the store's counters are integers too). */
int pc_jp_incr(const char *doc, size_t dlen, const char *path, size_t plen,
		long long by, long long *result, char *out, size_t cap);

#endif /* PC_JSONPATH_H */
