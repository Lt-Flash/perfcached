/* hashrectest.c - S313: the hash record's pure functions (src/hashrec.c).
 *
 * Set/replace/append, HSETNX, delete down to empty, the iteration order
 * (insertion, an update keeps its place), HINCRBY with Redis's integer
 * rules and overflow refusal, HINCRBYFLOAT's arithmetic and its text
 * ("10.5", "5000", no trailing zeros), the size ceiling, and a validator
 * that refuses every truncation and every wrong header.  Fail-first: the
 * module does not exist before S313.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hashrec.h"

static int pass, fail;
#define OK(c, m) do { if (c) pass++; else { fail++; printf("FAIL: %s\n", m); } } while (0)

static unsigned char A[PC_HR_MAX + 64], B[PC_HR_MAX + 64];

static long set(unsigned char *in, long n, const char *f, const char *v,
		int nx, unsigned char *out, int *added)
{
	return pc_hr_set(n > 0 ? in : NULL, n > 0 ? (size_t)n : 0, f, strlen(f),
		v, strlen(v), nx, out, sizeof A, added);
}

static int getis(const unsigned char *r, long n, const char *f, const char *want)
{
	const unsigned char *v;
	size_t vl;

	if (!pc_hr_get(r, (size_t)n, f, strlen(f), &v, &vl))
		return want == NULL;
	return want && vl == strlen(want) && !memcmp(v, want, vl);
}

int main(void)
{
	long n, m;
	int added, removed;
	unsigned int cnt;
	long long res;
	char fres[64];

	/* ---- set, replace, append, nx ---- */
	n = set(NULL, 0, "a", "1", 0, A, &added);
	OK(n > 0 && added == 1 && pc_hr_valid(A, (size_t)n, &cnt) && cnt == 1,
		"a new hash with one field");
	n = set(A, n, "b", "two", 0, B, &added); memcpy(A, B, (size_t)n);
	n = set(A, n, "c", "3", 0, B, &added); memcpy(A, B, (size_t)n);
	OK(pc_hr_valid(A, (size_t)n, &cnt) && cnt == 3, "three fields");
	m = set(A, n, "b", "TWO-longer", 0, B, &added);
	OK(m == n + 7 && added == 0 && getis(B, m, "b", "TWO-longer"),
		"replace in place, the length follows the value");
	{
		size_t off = 0, fl, vl;
		const unsigned char *f, *v;
		char order[16] = "";

		while (pc_hr_next(B, (size_t)m, &off, &f, &fl, &v, &vl))
			strncat(order, (const char *)f, fl);
		OK(!strcmp(order, "abc"), "insertion order kept across an update");
	}
	memcpy(A, B, (size_t)m); n = m;
	m = set(A, n, "a", "X", 1, B, &added);
	OK(m == n && added == 0 && getis(B, m, "a", "1"), "HSETNX on a present field changes nothing");
	m = set(A, n, "d", "4", 1, B, &added);
	OK(added == 1 && getis(B, m, "d", "4"), "HSETNX on an absent field adds it");
	OK(getis(A, n, "zz", NULL), "an absent field reads absent");
	OK(pc_hr_set(A, (size_t)n, "", 0, "v", 1, 0, B, sizeof B, &added) == PC_HR_E_ARG,
		"an empty field name is refused");

	/* ---- delete ---- */
	m = pc_hr_del(A, (size_t)n, "b", 1, B, sizeof B, &removed);
	OK(removed == 1 && pc_hr_valid(B, (size_t)m, &cnt) && cnt == 2 &&
		getis(B, m, "a", "1") && getis(B, m, "c", "3") && getis(B, m, "b", NULL),
		"delete closes the gap");
	memcpy(A, B, (size_t)m); n = m;
	m = pc_hr_del(A, (size_t)n, "nope", 4, B, sizeof B, &removed);
	OK(removed == 0 && m == n, "deleting an absent field changes nothing");
	m = pc_hr_del(A, (size_t)n, "a", 1, B, sizeof B, &removed); memcpy(A, B, (size_t)m); n = m;
	m = pc_hr_del(A, (size_t)n, "c", 1, B, sizeof B, &removed);
	OK(removed == 1 && m == PC_HR_HDR && pc_hr_valid(B, (size_t)m, &cnt) && cnt == 0,
		"the last delete leaves an empty record (the caller deletes the key)");

	/* ---- HINCRBY ---- */
	n = pc_hr_incrby(NULL, 0, "n", 1, 5, &res, A, sizeof A);
	OK(n > 0 && res == 5 && getis(A, n, "n", "5"), "HINCRBY on an absent field starts at 0");
	m = pc_hr_incrby(A, (size_t)n, "n", 1, -7, &res, B, sizeof B);
	OK(res == -2 && getis(B, m, "n", "-2"), "HINCRBY by a negative");
	n = set(NULL, 0, "s", "abc", 0, A, &added);
	OK(pc_hr_incrby(A, (size_t)n, "s", 1, 1, &res, B, sizeof B) == PC_HR_E_NUM,
		"HINCRBY on a non-integer: refused");
	n = set(NULL, 0, "s", " 1", 0, A, &added);
	OK(pc_hr_incrby(A, (size_t)n, "s", 1, 1, &res, B, sizeof B) == PC_HR_E_NUM,
		"a leading space is not an integer (Redis's string2ll)");
	n = set(NULL, 0, "s", "01", 0, A, &added);
	OK(pc_hr_incrby(A, (size_t)n, "s", 1, 1, &res, B, sizeof B) == PC_HR_E_NUM,
		"a leading zero is not an integer");
	n = set(NULL, 0, "m", "9223372036854775806", 0, A, &added);
	OK(pc_hr_incrby(A, (size_t)n, "m", 1, 1, &res, B, sizeof B) > 0 &&
		res == 9223372036854775807LL, "up to LLONG_MAX");
	OK(pc_hr_incrby(A, (size_t)n, "m", 1, 2, &res, B, sizeof B) == PC_HR_E_OVF,
		"past LLONG_MAX: overflow refused");
	n = set(NULL, 0, "m", "-9223372036854775808", 0, A, &added);
	OK(pc_hr_incrby(A, (size_t)n, "m", 1, -1, &res, B, sizeof B) == PC_HR_E_OVF,
		"below LLONG_MIN: overflow refused (and LLONG_MIN itself parses)");

	/* ---- HINCRBYFLOAT ---- */
	n = pc_hr_incrbyfloat(NULL, 0, "f", 1, "10.5", 4, fres, A, sizeof A);
	OK(n > 0 && !strcmp(fres, "10.5") && getis(A, n, "f", "10.5"), "HINCRBYFLOAT 0 + 10.5 = 10.5");
	m = pc_hr_incrbyfloat(A, (size_t)n, "f", 1, "0.1", 3, fres, B, sizeof B);
	OK(!strcmp(fres, "10.6"), "10.5 + 0.1 = 10.6 (17 digits, rounded as Redis)");
	n = set(NULL, 0, "g", "5.0e3", 0, A, &added);
	m = pc_hr_incrbyfloat(A, (size_t)n, "g", 1, "200", 3, fres, B, sizeof B);
	OK(!strcmp(fres, "5200"), "5.0e3 + 200 = 5200: no point, no zeros");
	OK(pc_hr_incrbyfloat(A, (size_t)n, "g", 1, "abc", 3, fres, B, sizeof B) == PC_HR_E_NUM,
		"a non-number increment: refused");
	OK(pc_hr_incrbyfloat(A, (size_t)n, "g", 1, "inf", 3, fres, B, sizeof B) == PC_HR_E_NUM,
		"an infinite result: refused");
	n = set(NULL, 0, "h", "1.5", 0, A, &added);
	m = pc_hr_incrbyfloat(A, (size_t)n, "h", 1, "-1.5", 4, fres, B, sizeof B);
	OK(!strcmp(fres, "0"), "1.5 - 1.5 = 0, not -0 or 0.");

	/* ---- the ceiling ---- */
	{
		static char big[PC_HR_MAX];

		memset(big, 'x', sizeof big - 100);
		big[sizeof big - 100] = 0;
		n = set(NULL, 0, "big", big, 0, A, &added);
		OK(n > 0, "a field near the ceiling fits");
		OK(set(A, n, "more", "0123456789012345678901234567890123456789"
			"01234567890123456789012345678901234567890123456789", 0, B, &added)
			== PC_HR_E_SIZE, "past PC_HR_MAX: refused, nothing written");
	}

	/* ---- the validator ---- */
	n = set(NULL, 0, "a", "1", 0, A, &added);
	n = set(A, n, "bb", "22", 0, B, &added); memcpy(A, B, (size_t)n);
	{
		long k;
		int bad = 0;

		for (k = 0; k < n; k++)
			bad += pc_hr_valid(A, (size_t)k, NULL);
		OK(bad == 0, "every truncation is refused");
	}
	B[0] = 2; memcpy(B + 1, A + 1, (size_t)n - 1);
	OK(!pc_hr_valid(B, (size_t)n, NULL), "a wrong version byte is refused");
	memcpy(B, A, (size_t)n); B[4] = 3;
	OK(!pc_hr_valid(B, (size_t)n, NULL), "a count that lies is refused");
	OK(pc_hr_valid(A, (size_t)n, &cnt) && cnt == 2, "and the real one is valid");

	/* the ceiling is part of the format: a well-formed record over
	 * PC_HR_MAX is refused, because every reader sizes its scratch by
	 * PC_HR_MAX and the recover and cluster-apply paths stamp F_HASH on
	 * a value of any cell size (S318 review, 10-04).  8286 seven-byte
	 * entries (a one-byte name, an empty value) are 58,010 bytes; the
	 * same bytes cut to 8284 entries are 57,996 and valid - the
	 * positive control for the refusal. */
	{
		static unsigned char big[PC_HR_HDR + 7 * 8286];
		unsigned int k, over = 8286, under = 8284;

		memset(big, 0, sizeof big);
		big[0] = 1;
		big[4] = (unsigned char)over; big[5] = (unsigned char)(over >> 8);
		for (k = 0; k < over; k++) {
			unsigned char *e = big + PC_HR_HDR + 7 * k;

			e[0] = 1; e[1] = 0;                /* flen 1 */
			e[2] = e[3] = e[4] = e[5] = 0;     /* vlen 0 */
			e[6] = 'x';
		}
		OK(sizeof big > PC_HR_MAX && !pc_hr_valid(big, sizeof big, NULL),
		   "a well-formed record over PC_HR_MAX is refused");
		big[4] = (unsigned char)under; big[5] = (unsigned char)(under >> 8);
		OK(PC_HR_HDR + 7 * under <= PC_HR_MAX &&
		   pc_hr_valid(big, PC_HR_HDR + 7 * under, &cnt) && cnt == under,
		   "the same bytes under the ceiling are valid");
	}

	printf("hashrectest: %d passed, %d failed\n", pass, fail);
	return fail != 0;
}
