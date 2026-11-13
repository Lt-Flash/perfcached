/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov
 * Part of libperfd - see lib/LICENSE.  This file must stay
 * free of src/core includes; tools/sync-libperfd.sh exports
 * exactly the MIT set to consumers. */
#ifndef PC_PTREE_H
#define PC_PTREE_H
/* ptree.h - the native door's value codec, LE tagged items: 'i' i64, 'd' u16+text, 'b' u32+bytes,
 * 'n', 't'/'f', 'a' u32 count+items, 'm' u32 pairs+('b' key, item)*, 'o' OK, 'e' u32+line */
#include <stddef.h>
#include <stdint.h>

#include "json.h"                      /* the JSON renderer writes a pc_jw */

#define PT_DEPTH 32                    /* nesting, the outermost item at 1 */
#define PT_HEAP_MAX (16u << 20)        /* a growing writer's ceiling */

/* ---- writer ------------------------------------------------------------ */

struct pc_tw {
	unsigned char *b;
	size_t n, cap;
	int over;                      /* a write did not fit: the tree is void */
	int owned;                     /* b is heap the writer may grow and frees */
	int depth;
	struct {
		size_t at;             /* offset of the u32 count */
		unsigned int cnt;
		int map;
	} st[PT_DEPTH];
};

/* a writer over a fixed buffer (never grows) or, with buf NULL, a heap
 * buffer that grows to PT_HEAP_MAX; pc_tw_free releases the heap one */
void pc_tw_init(struct pc_tw *w, unsigned char *buf, size_t cap);
void pc_tw_free(struct pc_tw *w);
/* switch an EMPTY fixed-buffer writer to a heap buffer it owns and may
 * grow (a reply a request does not bound - dump's chunks); 0 ok */
int  pc_tw_to_heap(struct pc_tw *w, size_t cap);

void pc_tw_map(struct pc_tw *w);       /* open; pc_tw_end closes */
void pc_tw_arr(struct pc_tw *w);
void pc_tw_end(struct pc_tw *w);
void pc_tw_key(struct pc_tw *w, const char *k);          /* in a map */
void pc_tw_keyn(struct pc_tw *w, const void *k, size_t n);
void pc_tw_i64(struct pc_tw *w, long long v);
void pc_tw_num(struct pc_tw *w, const char *text, size_t n);
void pc_tw_bulk(struct pc_tw *w, const void *p, size_t n);
void pc_tw_str(struct pc_tw *w, const char *s);          /* a C string */
void pc_tw_bool(struct pc_tw *w, int v);
void pc_tw_nil(struct pc_tw *w);
void pc_tw_ok(struct pc_tw *w);
void pc_tw_err(struct pc_tw *w, const char *line);
/* an item already encoded (a stored reply tree), counted as one */
void pc_tw_raw(struct pc_tw *w, const void *item, size_t n);

/* a rollback point: everything written after it - counts included -
 * is undone by pc_tw_rollback (a reply cut back to its last whole part) */
struct pc_tw_mark {
	size_t n;
	int depth;
	unsigned int cnt;
};
struct pc_tw_mark pc_tw_mark(const struct pc_tw *w);
void pc_tw_rollback(struct pc_tw *w, struct pc_tw_mark m);

/* 0 when the tree is whole: every container closed, nothing overflowed */
int pc_tw_done(const struct pc_tw *w);

/* ---- reader ------------------------------------------------------------ */

struct pc_tn {
	char type;                     /* the tag */
	uint32_t len;                  /* b d e: bytes; a m: items / pairs */
	const unsigned char *p;        /* b d e: the bytes */
	long long i;                   /* i */
	int next;                      /* the node after this subtree */
};

struct pc_tv {
	struct pc_tn *n;
	int nn, cap;
	const unsigned char *src;
	size_t srclen;
	const char *err;
};

/* index @n bytes holding exactly one item; 0 ok, -1 with v->err.  The
 * node array grows inside @v; pc_tv_free releases it. */
int pc_tv_parse(struct pc_tv *v, const unsigned char *b, size_t n);
void pc_tv_free(struct pc_tv *v);

/* the value of @key in map node @map, or -1 (absent, or @map not a map) */
int pc_tv_get(const struct pc_tv *v, int map, const char *key);
/* the k-th item of array node @arr, or -1; for a map, its k-th VALUE */
int pc_tv_at(const struct pc_tv *v, int arr, unsigned int k);
/* the k-th key of map node @map (a 'b' node), or -1 */
int pc_tv_key_at(const struct pc_tv *v, int map, unsigned int k);

/* -1 = absent or the wrong kind; get_str copies a 'b' (cap includes no terminator),
 * get_int takes an 'i' or an integer 'd', get_bool is 1 only for 't' */
int pc_tv_get_str(const struct pc_tv *v, int map, const char *key,
		char *buf, size_t cap);
int pc_tv_get_int(const struct pc_tv *v, int map, const char *key,
		long long *out);
int pc_tv_get_bool(const struct pc_tv *v, int map, const char *key);
/* node @k is a 'b' equal to the C string @s */
int pc_tv_streq(const struct pc_tv *v, int k, const char *s);

/* ---- renderers --------------------------------------------------------- */

/* one item as JSON (non-UTF-8 bytes become U+FFFD, an error {"error":line}), for the HTTP
 * door and display - never a wire; returns the bytes consumed, 0 if malformed */
size_t pc_tree_json(struct pc_jw *w, const unsigned char *b, size_t n);

/* one item as RESP2 (a map as its flat key/value array, bools as 1/0) */
size_t pc_tree_resp(struct pc_jw *w, const unsigned char *b, size_t n);

/* JSON text to one tree item, a number -> i when a plain integer that fits, else d as written;
 * a value under a key in @doc_keys (may be NULL) becomes one b of its JSON text.  0, -1 + *err */
int pc_tree_from_json(const char *json, size_t n, struct pc_tw *w,
		const char *const *doc_keys, const char **err);

#endif
