/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov
 * Part of libperfd - see lib/LICENSE.  This file must stay
 * free of src/core includes; tools/sync-libperfd.sh exports
 * exactly the MIT set to consumers. */
/* json.h - the in-house JSON codec: a jsmn-style span tokenizer (no allocations) and an escaping
 * writer.  JSON is data here, never the wire; cJSON is banned (it truncates at NULs). */
#ifndef PC_JSON_H
#define PC_JSON_H

#include <stddef.h>
#include <string.h>

enum pc_jtype { PC_J_UNDEF = 0, PC_J_OBJ, PC_J_ARR, PC_J_STR, PC_J_PRIM };

struct pc_jtok {
	enum pc_jtype type;
	int start, end;             /* byte span in the input buffer */
	int size;                   /* members (obj: pairs, arr: elems) */
	int parent;
};

/* tokenize buf[0..len); returns token count or -1 on malformed input.
 * Strings' spans EXCLUDE the quotes and are still escaped. */
int pc_json_parse(const char *buf, size_t len, struct pc_jtok *t, int maxt);

/* first value token whose key equals @key inside object @obj; -1 if absent */
int pc_json_get(const char *buf, const struct pc_jtok *t, int ntok,
		int obj, const char *key);

/* unescape a PC_J_STR token into out, NUL-safe (the backslash-u0000 escape becomes a real NUL):
 * the byte length, or -1 (bad escape / overflow) */
int pc_json_unescape(const char *buf, const struct pc_jtok *tok,
		char *out, size_t outcap);

/* does tok equal the ASCII literal? (for enum-ish string params) */
int pc_json_streq(const char *buf, const struct pc_jtok *tok, const char *s);

/* ---- writer ------------------------------------------------------------ */

struct pc_jw {
	char *buf;
	size_t len, cap;
	int overflow;
	/* 1 = buf is heap memory this writer owns and may GROW, for a reply no request bounds (CLUSTER
	 * SLOTS: ~1.5MB at 64 nodes); the caller must pc_jw_free() it */
	int owned;
};

/* a heap writer's bound, or a bug becomes an OOM: above the worst slot map (16384 x ~101 bytes)
 * and below PC_MAX_OUTQ, so an overrun is an overflow, not a killed connection */
#define JW_HEAP_MAX (4u << 20)

void pc_jw_init(struct pc_jw *w, char *buf, size_t cap);
/* switch @w to a heap buffer it owns and may grow; 0 on success.  Only
 * valid before anything has been written (asserted by w->len == 0). */
int  pc_jw_init_heap(struct pc_jw *w, size_t cap);
void pc_jw_free(struct pc_jw *w);
/* the append, inline while the bytes fit, so a literal's copy compiles to a few moves (musl's
 * memcpy call is a `rep movs` routine); growing and refusing stay out of line */
void pc_jw_raw_slow(struct pc_jw *w, const char *s, size_t n);
static inline void pc_jw_raw(struct pc_jw *w, const char *s, size_t n)
{
	if (__builtin_expect(w->len + n <= w->cap, 1)) {
		memcpy(w->buf + w->len, s, n);
		w->len += n;
		return;
	}
	pc_jw_raw_slow(w, s, n);
}
/* inline so a string LITERAL's length folds at compile time (a strlen per literal cost 5% of
 * a `members`) */
static inline void pc_jw_lit(struct pc_jw *w, const char *s)
{
	pc_jw_raw(w, s, __builtin_strlen(s));
}
void pc_jw_str(struct pc_jw *w, const char *s, size_t n);   /* escapes */
/* as pc_jw_str for CLIENT bytes: a byte that cannot be valid UTF-8 becomes U+FFFD, or one binary
 * sample makes all of /stats unparseable to a strict reader */
void pc_jw_str_utf8(struct pc_jw *w, const char *s, size_t n);
void pc_jw_i64(struct pc_jw *w, long long v);


/* ---- helpers ----------------------------------------------------------- */

int pc_utf8_clean(const char *s, size_t n);   /* 1 = valid UTF-8, no NULs */


#endif /* PC_JSON_H */
