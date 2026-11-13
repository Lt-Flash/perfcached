/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* hashrec.h - a Redis hash as ONE value (typed PCACHE_F_HASH): pure functions over
 * the record's bytes, no store, lock or wire */
#ifndef PC_HASHREC_H
#define PC_HASHREC_H

#include <stddef.h>

/* little-endian [1][0][0][0][n u32] then n x [flen u16][vlen u32][field][value], in
 * insertion order as Redis answers HGETALL; an empty record is never stored */
#define PC_HR_HDR      8
#define PC_HR_MAX      58000           /* PC_MAX_FWD_VAL: forward + push */
#define PC_HR_FIELD_MAX 4096           /* a field name, as a key */
#define PC_HR_E_REC    (-1)            /* the bytes are not a hash record */
#define PC_HR_E_SIZE   (-2)            /* the result would exceed PC_HR_MAX */
#define PC_HR_E_NUM    (-3)            /* the field's value is not a number */
#define PC_HR_E_OVF    (-4)            /* the increment overflows */
#define PC_HR_E_ARG    (-5)            /* a bad argument (empty or long field) */

/* is @r a well-formed hash record under PC_HR_MAX?  1 yes (*@count = its fields),
 * 0 no - so a reader may size its scratch by PC_HR_MAX */
int pc_hr_valid(const unsigned char *r, size_t n, unsigned int *count);

/* iterate: start with *@off = 0; 1 = an entry (field and value point into
 * @r), 0 = the end.  @r must be valid. */
int pc_hr_next(const unsigned char *r, size_t n, size_t *off,
		const unsigned char **field, size_t *flen,
		const unsigned char **val, size_t *vlen);

/* look up @field: 1 found (*@val, *@vlen point into @r), 0 absent */
int pc_hr_get(const unsigned char *r, size_t n, const void *field,
		size_t flen, const unsigned char **val, size_t *vlen);

/* @r (NULL for a new hash) with @field = @val, in place or appended, into @out; with
 * @nx and the field present nothing changes; *@added = 1 if new; new length or < 0 */
long pc_hr_set(const unsigned char *r, size_t n, const void *field,
		size_t flen, const void *val, size_t vlen, int nx,
		unsigned char *out, size_t cap, int *added);

/* the new record in @out without @field; *@removed = 1 if it was there.
 * Returns the new length (PC_HR_HDR when the hash is now empty), or < 0. */
long pc_hr_del(const unsigned char *r, size_t n, const void *field,
		size_t flen, unsigned char *out, size_t cap, int *removed);

/* HINCRBY: field as int64 (absent = 0) plus @by, stored back as decimal and in
 * *@result; PC_HR_E_NUM / PC_HR_E_OVF as Redis refuses them; new length or < 0 */
long pc_hr_incrby(const unsigned char *r, size_t n, const void *field,
		size_t flen, long long by, long long *result,
		unsigned char *out, size_t cap);

/* HINCRBYFLOAT as Redis: long double, 17 significant digits, no trailing zeros,
 * same text in field and @result (<= 64 bytes); NaN/inf is PC_HR_E_NUM */
long pc_hr_incrbyfloat(const unsigned char *r, size_t n, const void *field,
		size_t flen, const char *by, size_t bylen, char *result,
		unsigned char *out, size_t cap);

#endif /* PC_HASHREC_H */
