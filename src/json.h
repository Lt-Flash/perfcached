/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov
 * Part of libperfd - see lib/LICENSE.  This file must stay
 * free of src/core includes; tools/sync-libperfd.sh exports
 * exactly the MIT set to consumers. */
/*
 * json.h — the in-house JSON codec (task S7).
 *
 * Purpose-built for the text dialect: a jsmn-style span tokenizer (no
 * allocations, tokens point into the input buffer), an escaping writer,
 * and the NUL-safety layer decision #1 settled on: values that are valid
 * UTF-8 without NULs travel as plain JSON strings; anything else travels
 * base64 with a sibling "enc":"b64" - sodium_bin2base64 does the b64, we
 * never hand-roll it.  cJSON is banned here by scar tissue (it truncates
 * at NULs).
 */
#ifndef PC_JSON_H
#define PC_JSON_H

#include <stddef.h>

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

/* unescape a PC_J_STR token into out; NUL-safe: the backslash-u0000
 * escape becomes a real NUL byte in out.  Returns the byte length or -1
 * (bad escape / overflow). */
int pc_json_unescape(const char *buf, const struct pc_jtok *tok,
		char *out, size_t outcap);

/* does tok equal the ASCII literal? (for enum-ish string params) */
int pc_json_streq(const char *buf, const struct pc_jtok *tok, const char *s);

/* ---- writer ------------------------------------------------------------ */

struct pc_jw {
	char *buf;
	size_t len, cap;
	int overflow;
	/* 1 = buf is heap memory this writer owns and may GROW.  Replies
	 * normally go into a fixed per-worker scratch, which is right for
	 * everything whose size a request bounds.  CLUSTER SLOTS is not
	 * bounded that way - it enumerates the slot space, so a 64-node
	 * fleet is ~1.5MB - and it would otherwise be refused as "reply
	 * too large" on any fleet past three nodes.  The caller must
	 * pc_jw_free() when this is set. */
	int owned;
};

/* JW_HEAP_MAX: a bound is still needed, or a bug becomes an OOM.  Set
 * well above the worst-case slot map (16384 ranges x ~101 bytes) and
 * well below PC_MAX_OUTQ, so an overrun is reported as overflow rather
 * than killing the connection at the output queue. */
#define JW_HEAP_MAX (4u << 20)

void pc_jw_init(struct pc_jw *w, char *buf, size_t cap);
/* switch @w to a heap buffer it owns and may grow; 0 on success.  Only
 * valid before anything has been written (asserted by w->len == 0). */
int  pc_jw_init_heap(struct pc_jw *w, size_t cap);
void pc_jw_free(struct pc_jw *w);
void pc_jw_raw(struct pc_jw *w, const char *s, size_t n);
void pc_jw_lit(struct pc_jw *w, const char *s);
void pc_jw_str(struct pc_jw *w, const char *s, size_t n);   /* escapes */
void pc_jw_i64(struct pc_jw *w, long long v);

/* the decision-#1 value writer: emits either  "key":"plain-string"  or
 * "key":"<base64>","key_enc":"b64"  depending on the bytes; @enc_key is
 * the sibling name (normally "enc").  Never truncates at NULs. */
void pc_jw_value(struct pc_jw *w, const char *key, const char *enc_key,
		const char *v, size_t n);

/* ---- helpers ----------------------------------------------------------- */

int pc_utf8_clean(const char *s, size_t n);   /* 1 = valid UTF-8, no NULs */

/* base64 (sodium): returns output length or -1 on overflow/bad input */
int pc_b64_enc(const char *in, size_t n, char *out, size_t cap);
int pc_b64_dec(const char *in, size_t n, char *out, size_t cap);

#endif /* PC_JSON_H */
