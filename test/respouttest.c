/* respouttest.c - the RESP door's reply writers (respout.c; the 0.7.2
 * split, wave 1): the exact bytes each writes, and the two shared readers.
 *
 *   A  -ERR, -<code>, -<line>, +simple, :integer (negative and the
 *      largest), the null bulk, a bulk (binary-safe, empty, a literal
 *      through resp_bulk_lit), an array's header; writes append
 *   B  resp_is: a command word without case, never a prefix either way
 *   C  resp_ll: a whole decimal integer, signed; trailing junk, an empty
 *      or a too-long field refused
 */
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "json.h"
#include "respout.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static char buf[512];
static struct pc_jw w;

static void reset(void)
{
	memset(buf, 0, sizeof buf);
	pc_jw_init(&w, buf, sizeof buf);
}

/* the writer holds exactly @n bytes @want */
static int is(const char *want, size_t n)
{
	return w.len == n && !memcmp(w.buf, want, n);
}
#define IS(lit) is((lit), sizeof(lit) - 1)

int main(void)
{
	long long v = 0;

	/* A */
	reset(); resp_err(&w, "unknown command");
	CHK(IS("-ERR unknown command\r\n"), "A: resp_err");
	reset(); resp_err_code(&w, "TRYAGAIN", "busy");
	CHK(IS("-TRYAGAIN busy\r\n"), "A: resp_err_code - the code word first");
	reset(); resp_err_line(&w, "WRONGTYPE Operation against a key");
	CHK(IS("-WRONGTYPE Operation against a key\r\n"), "A: resp_err_line - Redis's own words");
	reset(); resp_simple(&w, "OK");
	CHK(IS("+OK\r\n"), "A: resp_simple");
	reset(); resp_int(&w, -5);
	CHK(IS(":-5\r\n"), "A: resp_int, negative");
	reset(); resp_int(&w, LLONG_MAX);
	CHK(IS(":9223372036854775807\r\n"), "A: resp_int, the largest");
	reset(); resp_nil(&w);
	CHK(IS("$-1\r\n"), "A: resp_nil - the null bulk");
	reset(); resp_bulk(&w, "a\0b", 3);
	CHK(IS("$3\r\na\0b\r\n"), "A: resp_bulk - binary-safe (an embedded NUL)");
	reset(); resp_bulk(&w, "", 0);
	CHK(IS("$0\r\n\r\n"), "A: resp_bulk - empty");
	reset(); resp_bulk_lit(&w, "HELP");
	CHK(IS("$4\r\nHELP\r\n"), "A: resp_bulk_lit - the literal's length, not one more");
	reset(); resp_arr(&w, 2); resp_bulk_lit(&w, "k"); resp_int(&w, 1);
	CHK(IS("*2\r\n$1\r\nk\r\n:1\r\n"), "A: resp_arr, then its elements appended");

	/* B */
	CHK(resp_is("get", 3, "GET") && resp_is("GeT", 3, "GET"), "B: resp_is - case ignored");
	CHK(!resp_is("GETX", 4, "GET"), "B: a longer word is not the command");
	CHK(!resp_is("GE", 2, "GET"), "B: a prefix of the command is not it");
	CHK(!resp_is("SET", 3, "GET"), "B: another word");

	/* C */
	CHK(resp_ll("123", 3, &v) == 0 && v == 123, "C: resp_ll 123");
	CHK(resp_ll("-7", 2, &v) == 0 && v == -7, "C: resp_ll -7");
	CHK(resp_ll("12a", 3, &v) == -1, "C: trailing junk refused");
	CHK(resp_ll("", 0, &v) == -1, "C: an empty field refused");
	CHK(resp_ll("123456789012345678901234", 24, &v) == -1, "C: 24 digits refused (no room)");
	CHK(resp_ll("12345", 3, &v) == 0 && v == 123, "C: only the given length is read");

	printf("respouttest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
