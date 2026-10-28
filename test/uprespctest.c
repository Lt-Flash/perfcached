/* uprespctest.c - the upstream's RESP reply parser (uprespc.c; the 0.7.2
 * split, wave 1): one RESP2 or RESP3 reply from a buffer, as the upstream
 * connections read them.
 *
 *   A  the scalars: simple string, error, integer, bulk, RESP2's null bulk,
 *      RESP3's null, double / big number / boolean as text, verbatim (its
 *      "txt:" prefix dropped); is_ok
 *   B  the aggregates: array, map (two elements an entry), set, push (the
 *      invalidation tracking sends), an attribute skipped for the value it
 *      annotates, a null array
 *   C  incomplete input - a split line, a short bulk, an aggregate missing
 *      an element - is "need more" (0) with the position unmoved; one
 *      buffer holding two replies parses them in turn
 *   D  what is refused: a line without its CR, nesting past 8, a bulk
 *      larger than this thread's rv_max (-3, and it resumes at the default)
 */
#include <stdio.h>
#include <string.h>
#include "../src/uprespc.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* parse @in whole: the return code, the position reached, the reply */
static int parse(const char *in, size_t *pos, struct rv *v)
{
	static char buf[4096];
	size_t len = strlen(in);

	memcpy(buf, in, len);
	*pos = 0;
	return rv_parse(buf, len, pos, v, 0);
}

static int str_is(const struct rv *v, const char *s)
{
	return v->s && v->len == strlen(s) && !memcmp(v->s, s, v->len);
}

int main(void)
{
	struct rv v;
	size_t pos;
	int rc;

	/* A */
	rc = parse("+OK\r\n", &pos, &v);
	CHK(rc == 1 && v.t == '+' && str_is(&v, "OK") && pos == 5 && is_ok(&v),
		"A: +OK - a simple string, and is_ok");
	rc = parse("+OKAY\r\n", &pos, &v);
	CHK(rc == 1 && !is_ok(&v), "A: +OKAY is not OK");
	rc = parse("-ERR wrong\r\n", &pos, &v);
	CHK(rc == 1 && v.t == '-' && str_is(&v, "ERR wrong") && !is_ok(&v),
		"A: -ERR - an error, its text");
	rc = parse(":-42\r\n", &pos, &v);
	CHK(rc == 1 && v.t == ':' && v.n == -42 && pos == 6, "A: :-42 - an integer");
	rc = parse("$5\r\nhello\r\n", &pos, &v);
	CHK(rc == 1 && v.t == '$' && str_is(&v, "hello") && pos == 11,
		"A: $5 hello - a bulk");
	rc = parse("$0\r\n\r\n", &pos, &v);
	CHK(rc == 1 && v.t == '$' && v.len == 0 && pos == 6, "A: $0 - an empty bulk");
	rc = parse("$-1\r\n", &pos, &v);
	CHK(rc == 1 && v.t == '_' && pos == 5, "A: $-1 - RESP2's null bulk reads as null");
	rc = parse("_\r\n", &pos, &v);
	CHK(rc == 1 && v.t == '_' && pos == 3, "A: _ - RESP3's null");
	rc = parse(",3.25\r\n", &pos, &v);
	CHK(rc == 1 && v.t == ',' && str_is(&v, "3.25"), "A: ,3.25 - a double, as text");
	rc = parse("(12345678901234567890\r\n", &pos, &v);
	CHK(rc == 1 && v.t == '(' && str_is(&v, "12345678901234567890"),
		"A: ( - a big number, as text");
	rc = parse("#t\r\n", &pos, &v);
	CHK(rc == 1 && v.t == '#' && str_is(&v, "t"), "A: #t - a boolean, as text");
	rc = parse("=8\r\ntxt:abcd\r\n", &pos, &v);
	CHK(rc == 1 && v.t == '=' && str_is(&v, "abcd") && pos == 14,
		"A: =8 txt:abcd - verbatim, the format prefix dropped");

	/* B */
	rc = parse("*2\r\n$1\r\na\r\n:7\r\n", &pos, &v);
	CHK(rc == 1 && v.t == '*' && v.n == 2 && v.nel == 2 &&
		str_is(&v.el[0], "a") && v.el[1].t == ':' && v.el[1].n == 7,
		"B: *2 - an array of a bulk and an integer");
	rv_free(&v);
	CHK(v.nel == 0 && v.el == NULL, "B: rv_free empties it");
	rc = parse("%2\r\n+k1\r\n:1\r\n+k2\r\n:2\r\n", &pos, &v);
	CHK(rc == 1 && v.t == '%' && v.n == 2 && v.nel == 4 &&
		str_is(&v.el[2], "k2") && v.el[3].n == 2,
		"B: %%2 - a map, two elements an entry");
	rv_free(&v);
	rc = parse("~1\r\n+x\r\n", &pos, &v);
	CHK(rc == 1 && v.t == '~' && v.nel == 1 && str_is(&v.el[0], "x"), "B: ~1 - a set");
	rv_free(&v);
	rc = parse(">2\r\n$10\r\ninvalidate\r\n*1\r\n$3\r\nkey\r\n", &pos, &v);
	CHK(rc == 1 && v.t == '>' && v.nel == 2 && str_is(&v.el[0], "invalidate") &&
		v.el[1].t == '*' && v.el[1].nel == 1 && str_is(&v.el[1].el[0], "key"),
		"B: >2 - the invalidation push, its key list");
	rv_free(&v);
	rc = parse("|1\r\n+ttl\r\n:3\r\n+OK\r\n", &pos, &v);
	CHK(rc == 1 && is_ok(&v) && v.nel == 0 && pos == 19,
		"B: |1 - an attribute is skipped for the value it annotates");
	rc = parse("*-1\r\n", &pos, &v);
	CHK(rc == 1 && v.t == '_' && pos == 5, "B: *-1 - a null array reads as null");

	/* C */
	rc = parse("+OK", &pos, &v);
	CHK(rc == 0 && pos == 0, "C: a line without its end is need-more, the position unmoved");
	rc = parse("$5\r\nhel", &pos, &v);
	CHK(rc == 0 && pos == 0, "C: a short bulk is need-more");
	rc = parse("*2\r\n$1\r\na\r\n", &pos, &v);
	CHK(rc == 0 && pos == 0 && v.nel == 0 && v.el == NULL,
		"C: an array missing an element is need-more, nothing kept");
	{
		static char two[] = "+OK\r\n:5\r\n";
		size_t p = 0;

		rc = rv_parse(two, sizeof two - 1, &p, &v, 0);
		CHK(rc == 1 && is_ok(&v) && p == 5, "C: two replies in one buffer - the first");
		rc = rv_parse(two, sizeof two - 1, &p, &v, 0);
		CHK(rc == 1 && v.t == ':' && v.n == 5 && p == 9, "C: then the second");
		rc = rv_parse(two, sizeof two - 1, &p, &v, 0);
		CHK(rc == 0 && p == 9, "C: then need-more");
	}

	/* D */
	rc = parse("+OK\n", &pos, &v);
	CHK(rc == -1, "D: a line without its CR is malformed");
	rc = parse("*1\r\n*1\r\n*1\r\n*1\r\n*1\r\n*1\r\n*1\r\n*1\r\n*1\r\n*1\r\n:1\r\n",
		&pos, &v);
	CHK(rc == -1 && v.nel == 0, "D: nesting past 8 is refused, nothing kept");
	rv_max = 4;
	rc = parse("$5\r\nhello\r\n", &pos, &v);
	CHK(rc == -3, "D: a bulk over this thread's rv_max (4) is -3");
	rc = parse("$4\r\nhell\r\n", &pos, &v);
	CHK(rc == 1 && str_is(&v, "hell"), "D: one at rv_max is read");
	rv_max = UP_RBUF_MAX;
	rc = parse("$5\r\nhello\r\n", &pos, &v);
	CHK(rc == 1, "D: back at the default, it is read");

	printf("uprespctest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
