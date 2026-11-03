/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * hashrec.c - a Redis hash as ONE value (task S313).  See hashrec.h for
 * the record and the contract; every function here is pure.
 */
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hashrec.h"

static unsigned int r16(const unsigned char *p)
{
	return (unsigned int)p[0] | (unsigned int)p[1] << 8;
}

static unsigned int r32(const unsigned char *p)
{
	return (unsigned int)p[0] | (unsigned int)p[1] << 8 |
		(unsigned int)p[2] << 16 | (unsigned int)p[3] << 24;
}

static void w16(unsigned char *p, unsigned int v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
}

static void w32(unsigned char *p, unsigned int v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16);
	p[3] = (unsigned char)(v >> 24);
}

int pc_hr_valid(const unsigned char *r, size_t n, unsigned int *count)
{
	unsigned int cnt, i;
	size_t off = PC_HR_HDR;

	/* the ceiling is part of the format: every reader sizes its scratch
	 * by PC_HR_MAX (HRANDFIELD's index array, one u32 per field), and a
	 * record arrives here from the recover and cluster-apply paths too,
	 * which stamp F_HASH on a value of any cell size without a look */
	if (!r || n > PC_HR_MAX || n < PC_HR_HDR || r[0] != 1 || r[1] ||
	        r[2] || r[3])
		return 0;
	cnt = r32(r + 4);
	for (i = 0; i < cnt; i++) {
		size_t fl, vl;

		if (n - off < 6)
			return 0;
		fl = r16(r + off);
		vl = r32(r + off + 2);
		if (!fl || fl > PC_HR_FIELD_MAX || n - off - 6 < fl ||
		        n - off - 6 - fl < vl)
			return 0;
		off += 6 + fl + vl;
	}
	if (off != n)
		return 0;
	if (count)
		*count = cnt;
	return 1;
}

int pc_hr_next(const unsigned char *r, size_t n, size_t *off,
		const unsigned char **field, size_t *flen,
		const unsigned char **val, size_t *vlen)
{
	size_t o = *off ? *off : PC_HR_HDR;

	if (o + 6 > n)
		return 0;
	*flen = r16(r + o);
	*vlen = r32(r + o + 2);
	*field = r + o + 6;
	*val = r + o + 6 + *flen;
	*off = o + 6 + *flen + *vlen;
	return 1;
}

/* the offset of @field's entry, or 0 when absent */
static size_t find(const unsigned char *r, size_t n, const void *field,
		size_t flen)
{
	size_t off = 0, at, fl, vl;
	const unsigned char *f, *v;

	if (!r || n < PC_HR_HDR)
		return 0;
	for (;;) {
		at = off ? off : PC_HR_HDR;
		if (!pc_hr_next(r, n, &off, &f, &fl, &v, &vl))
			return 0;
		if (fl == flen && !memcmp(f, field, flen))
			return at;
	}
}

int pc_hr_get(const unsigned char *r, size_t n, const void *field,
		size_t flen, const unsigned char **val, size_t *vlen)
{
	size_t at = find(r, n, field, flen);

	if (!at)
		return 0;
	*vlen = r32(r + at + 2);
	*val = r + at + 6 + r16(r + at);
	return 1;
}

static void hdr(unsigned char *out, unsigned int cnt)
{
	out[0] = 1;
	out[1] = out[2] = out[3] = 0;
	w32(out + 4, cnt);
}

long pc_hr_set(const unsigned char *r, size_t n, const void *field,
		size_t flen, const void *val, size_t vlen, int nx,
		unsigned char *out, size_t cap, int *added)
{
	unsigned int cnt = (r && n >= PC_HR_HDR) ? r32(r + 4) : 0;
	size_t at, need, o;

	*added = 0;
	if (!flen || flen > PC_HR_FIELD_MAX)
		return PC_HR_E_ARG;
	if (!r || n < PC_HR_HDR) {
		r = NULL;
		n = PC_HR_HDR;
	}
	at = r ? find(r, n, field, flen) : 0;
	if (at && nx) {                        /* HSETNX on a present field */
		if (n > cap)
			return PC_HR_E_SIZE;
		memcpy(out, r, n);
		return (long)n;
	}
	if (at) {                              /* replace in place */
		size_t ofl = r16(r + at), ovl = r32(r + at + 2);
		size_t tail = at + 6 + ofl + ovl;

		need = n - ovl + vlen;
		if (need > PC_HR_MAX || need > cap)
			return PC_HR_E_SIZE;
		memcpy(out, r, at + 6 + ofl);
		w32(out + at + 2, (unsigned int)vlen);
		memcpy(out + at + 6 + ofl, val, vlen);
		memcpy(out + at + 6 + ofl + vlen, r + tail, n - tail);
		return (long)need;
	}
	need = n + 6 + flen + vlen;            /* append */
	if (need > PC_HR_MAX || need > cap)
		return PC_HR_E_SIZE;
	if (r)
		memcpy(out, r, n);
	hdr(out, cnt + 1);
	o = n;
	w16(out + o, (unsigned int)flen);
	w32(out + o + 2, (unsigned int)vlen);
	memcpy(out + o + 6, field, flen);
	memcpy(out + o + 6 + flen, val, vlen);
	*added = 1;
	return (long)need;
}

long pc_hr_del(const unsigned char *r, size_t n, const void *field,
		size_t flen, unsigned char *out, size_t cap, int *removed)
{
	size_t at, len, keep;

	*removed = 0;
	if (!r || n < PC_HR_HDR) {
		if (cap < PC_HR_HDR)
			return PC_HR_E_SIZE;
		hdr(out, 0);
		return PC_HR_HDR;
	}
	at = find(r, n, field, flen);
	if (!at) {
		if (n > cap)
			return PC_HR_E_SIZE;
		memcpy(out, r, n);
		return (long)n;
	}
	len = 6 + r16(r + at) + r32(r + at + 2);
	keep = n - len;
	if (keep > cap)
		return PC_HR_E_SIZE;
	memcpy(out, r, at);
	memcpy(out + at, r + at + len, n - at - len);
	hdr(out, r32(r + 4) - 1);
	*removed = 1;
	return (long)keep;
}

/* Redis's string2ll: an optional '-', digits, no leading zero (except "0"
 * itself), no '+', no spaces, inside 64 bits.  1 ok, 0 not an integer. */
static int strict_ll(const unsigned char *s, size_t n, long long *out)
{
	unsigned long long v = 0, lim;
	size_t i = 0;
	int neg = 0;

	if (!n || n > 20)
		return 0;
	if (s[0] == '-') {
		neg = 1;
		i = 1;
		if (n == 1)
			return 0;
	}
	if (s[i] == '0' && n - i > 1)
		return 0;
	lim = neg ? (unsigned long long)LLONG_MAX + 1 : (unsigned long long)LLONG_MAX;
	for (; i < n; i++) {
		if (s[i] < '0' || s[i] > '9')
			return 0;
		if (v > (lim - (unsigned long long)(s[i] - '0')) / 10)
			return 0;
		v = v * 10 + (unsigned long long)(s[i] - '0');
	}
	if (neg)
		*out = v == (unsigned long long)LLONG_MAX + 1 ? LLONG_MIN :
			-(long long)v;
	else
		*out = (long long)v;
	return 1;
}

long pc_hr_incrby(const unsigned char *r, size_t n, const void *field,
		size_t flen, long long by, long long *result,
		unsigned char *out, size_t cap)
{
	const unsigned char *v;
	size_t vl;
	long long cur = 0;
	char num[24];
	int added, nl;

	if (pc_hr_get(r, n, field, flen, &v, &vl) && !strict_ll(v, vl, &cur))
		return PC_HR_E_NUM;
	if ((by > 0 && cur > LLONG_MAX - by) || (by < 0 && cur < LLONG_MIN - by))
		return PC_HR_E_OVF;
	cur += by;
	nl = snprintf(num, sizeof num, "%lld", cur);
	*result = cur;
	return pc_hr_set(r, n, field, flen, num, (size_t)nl, 0, out, cap,
		&added);
}

/* Redis's string2ld: the whole text a long double, no spaces, not NaN */
static int strict_ld(const char *s, size_t n, long double *out)
{
	char buf[256], *end;
	long double v;

	if (!n || n >= sizeof buf || s[0] == ' ' || s[0] == '\t' ||
	        s[n - 1] == ' ' || s[n - 1] == '\t')
		return 0;
	memcpy(buf, s, n);
	buf[n] = 0;
	errno = 0;
	v = strtold(buf, &end);
	if (end != buf + n || errno == ERANGE || isnan(v))
		return 0;
	*out = v;
	return 1;
}

long pc_hr_incrbyfloat(const unsigned char *r, size_t n, const void *field,
		size_t flen, const char *by, size_t bylen, char *result,
		unsigned char *out, size_t cap)
{
	const unsigned char *v;
	size_t vl;
	long double cur = 0, inc;
	int added, nl;

	if (!strict_ld(by, bylen, &inc))
		return PC_HR_E_NUM;
	if (pc_hr_get(r, n, field, flen, &v, &vl) &&
	        !strict_ld((const char *)v, vl, &cur))
		return PC_HR_E_NUM;
	cur += inc;
	if (isnan(cur) || isinf(cur))
		return PC_HR_E_NUM;
	/* ld2string(LD_STR_HUMAN): %.17Lf, then the trailing zeros and a
	 * bare point go - "10.5", "5000", "-0.25" */
	nl = snprintf(result, 64, "%.17Lf", cur);
	if (nl < 0 || nl >= 64)
		return PC_HR_E_NUM;
	if (strchr(result, '.')) {
		while (nl > 1 && result[nl - 1] == '0')
			result[--nl] = 0;
		if (nl > 1 && result[nl - 1] == '.')
			result[--nl] = 0;
	}
	if (!strcmp(result, "-0"))
		strcpy(result, "0");
	return pc_hr_set(r, n, field, flen, result, (size_t)nl, 0, out, cap,
		&added);
}
