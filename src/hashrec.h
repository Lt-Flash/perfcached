/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * hashrec.h - a Redis hash as ONE value (task S313, DESIGN 12ic).
 *
 * perfcached keeps one record per key; a hash is that record's value,
 * typed PCACHE_F_HASH, holding every field.  The H* commands read it (a
 * lock-free copy-out, as GET - S318), or edit a copy and store the copy
 * whole under the key's stripe - the same read-modify-write the JSON path
 * verbs do.  Nothing here touches the
 * store, a lock or the wire: these are pure functions over bytes, so the
 * format is tested on its own (test/hashrectest.c).
 *
 * The record, little-endian:
 *   [1][0][0][0][n u32]   then n x  [flen u16][vlen u32][field][value]
 * Fields in insertion order (an update keeps its place, a delete closes
 * the gap) - the order Redis answers HGETALL/HKEYS in for small hashes.
 * An empty record (n = 0) is never stored: the last HDEL deletes the key.
 * The whole record must stay under PC_HR_MAX - the live forward and push
 * ceiling a record has to fit to reach every node at once; pc_hr_valid
 * refuses a longer one, so a reader may size its scratch by it.
 */
#ifndef PC_HASHREC_H
#define PC_HASHREC_H

#include <stddef.h>

#define PC_HR_HDR      8
#define PC_HR_MAX      58000           /* PC_MAX_FWD_VAL: forward + push */
#define PC_HR_FIELD_MAX 4096           /* a field name, as a key */
#define PC_HR_E_REC    (-1)            /* the bytes are not a hash record */
#define PC_HR_E_SIZE   (-2)            /* the result would exceed PC_HR_MAX */
#define PC_HR_E_NUM    (-3)            /* the field's value is not a number */
#define PC_HR_E_OVF    (-4)            /* the increment overflows */
#define PC_HR_E_ARG    (-5)            /* a bad argument (empty or long field) */

/* is @r a well-formed hash record?  1 yes (*@count = its fields), 0 no */
int pc_hr_valid(const unsigned char *r, size_t n, unsigned int *count);

/* iterate: start with *@off = 0; 1 = an entry (field and value point into
 * @r), 0 = the end.  @r must be valid. */
int pc_hr_next(const unsigned char *r, size_t n, size_t *off,
		const unsigned char **field, size_t *flen,
		const unsigned char **val, size_t *vlen);

/* look up @field: 1 found (*@val, *@vlen point into @r), 0 absent */
int pc_hr_get(const unsigned char *r, size_t n, const void *field,
		size_t flen, const unsigned char **val, size_t *vlen);

/* the new record in @out: @r with @field = @val (replacing it in place,
 * or appended).  @r may be NULL/0 for a new hash.  With @nx and the field
 * present, nothing changes: *@added = 0 and @out is @r unchanged.
 * *@added = 1 when the field was new.  Returns the new length, or < 0. */
long pc_hr_set(const unsigned char *r, size_t n, const void *field,
		size_t flen, const void *val, size_t vlen, int nx,
		unsigned char *out, size_t cap, int *added);

/* the new record in @out without @field; *@removed = 1 if it was there.
 * Returns the new length (PC_HR_HDR when the hash is now empty), or < 0. */
long pc_hr_del(const unsigned char *r, size_t n, const void *field,
		size_t flen, unsigned char *out, size_t cap, int *removed);

/* HINCRBY: the field as a 64-bit integer (absent = 0) plus @by, stored
 * back as decimal; *@result gets the sum.  PC_HR_E_NUM / PC_HR_E_OVF as
 * Redis refuses them.  Returns the new length, or < 0. */
long pc_hr_incrby(const unsigned char *r, size_t n, const void *field,
		size_t flen, long long by, long long *result,
		unsigned char *out, size_t cap);

/* HINCRBYFLOAT: as Redis - long double arithmetic, the result written
 * with 17 significant digits and no trailing zeros (the same text goes in
 * the field and in @result, NUL-terminated, at most 64 bytes).  NaN or
 * infinity in or out is PC_HR_E_NUM.  Returns the new length, or < 0. */
long pc_hr_incrbyfloat(const unsigned char *r, size_t n, const void *field,
		size_t flen, const char *by, size_t bylen, char *result,
		unsigned char *out, size_t cap);

#endif /* PC_HASHREC_H */
