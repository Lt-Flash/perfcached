/* pcglobtest.c - the key glob (pcglob.c; the 0.7.2 split, wave 1): what
 * KEYS, SCAN MATCH and the read-through allow-list match with.
 *
 *   A  the plain cases: an empty pattern, a lone *, literal bytes (case
 *      counts), ? for exactly one byte
 *   B  * anywhere, backtracking across several, and a pattern longer than
 *      the string
 *   C  classes: a set, a range, ^ and ! negation, ] first is literal, a
 *      trailing - is literal, a backslash inside escapes; an unterminated
 *      class matches nothing
 *   D  backslash escapes outside a class; a trailing backslash is literal
 *   E  lengths, not terminators: a NUL byte inside is an ordinary byte;
 *      pc_glob_match answers as pc_glob
 */
#include <stdio.h>
#include <string.h>
#include "../src/pcglob.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static int m(const char *p, const char *s)
{
	return pc_glob(p, (int)strlen(p), s, (int)strlen(s));
}

static const struct { const char *p, *s; int want; const char *why; } cases[] = {
	/* A */
	{ "", "", 1, "A: an empty pattern matches the empty key" },
	{ "", "a", 0, "A: and nothing else" },
	{ "*", "", 1, "A: * matches the empty key" },
	{ "*", "anything:at:all", 1, "A: and any key" },
	{ "hello", "hello", 1, "A: literal bytes" },
	{ "hello", "Hello", 0, "A: case counts" },
	{ "hello", "hello!", 0, "A: a longer key is not a match" },
	{ "h?llo", "hallo", 1, "A: ? is one byte" },
	{ "h?llo", "hllo", 0, "A: exactly one" },
	{ "h?llo", "heello", 0, "A: not two" },
	/* B */
	{ "h*llo", "hllo", 1, "B: * may be empty" },
	{ "h*llo", "heeeello", 1, "B: or many" },
	{ "*:sess", "user:42:sess", 1, "B: * first" },
	{ "user:*", "user:", 1, "B: * last, empty" },
	{ "*a*b", "xxaxxb", 1, "B: two stars" },
	{ "a*b*c", "abbbcbc", 1, "B: backtracking to the last c" },
	{ "a*b*c", "abbbcb", 0, "B: no c at the end" },
	{ "*a*a*a*a*b", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 0,
		"B: many stars, no b - answered, and no" },
	{ "a*", "", 0, "B: a pattern needing a byte the key lacks" },
	{ "ab*", "a", 0, "B: a pattern longer than the key" },
	/* C */
	{ "h[ae]llo", "hello", 1, "C: a set" },
	{ "h[ae]llo", "hillo", 0, "C: a byte outside it" },
	{ "h[a-c]llo", "hbllo", 1, "C: a range" },
	{ "h[a-c]llo", "hdllo", 0, "C: past its end" },
	{ "h[^e]llo", "hallo", 1, "C: ^ negates" },
	{ "h[^e]llo", "hello", 0, "C: the negated byte" },
	{ "h[!e]llo", "hallo", 1, "C: ! negates too" },
	{ "h[!e]llo", "hello", 0, "C: the negated byte" },
	{ "[]a]", "]", 1, "C: ] first in a class is literal" },
	{ "[]a]", "a", 1, "C: beside the rest of the set" },
	{ "[a-]", "-", 1, "C: a trailing - is literal" },
	{ "[a-]", "b", 0, "C: and makes no range" },
	{ "[\\]]", "]", 1, "C: a backslash inside escapes" },
	{ "[abc", "a", 0, "C: an unterminated class matches nothing" },
	{ "x[abc", "xa", 0, "C: anywhere" },
	/* D */
	{ "h\\*llo", "h*llo", 1, "D: \\* is a literal star" },
	{ "h\\*llo", "hello", 0, "D: and only that" },
	{ "h\\?", "h?", 1, "D: \\? a literal question mark" },
	{ "h\\?", "ha", 0, "D: not any byte" },
	{ "a\\", "a\\", 1, "D: a trailing backslash is literal" },
};

int main(void)
{
	size_t i;

	for (i = 0; i < sizeof cases / sizeof cases[0]; i++)
		CHK(m(cases[i].p, cases[i].s) == cases[i].want, "%s ('%s' vs '%s')",
			cases[i].why, cases[i].p, cases[i].s);

	/* E */
	CHK(pc_glob("a?c", 3, "a\0c", 3) == 1, "E: a NUL inside the key is one byte for ?");
	CHK(pc_glob("a\0*", 3, "a\0bc", 4) == 1, "E: and a literal byte in the pattern");
	CHK(pc_glob("abc", 2, "ab", 2) == 1, "E: only the given lengths are read");
	for (i = 0; i < sizeof cases / sizeof cases[0]; i++)
		if (pc_glob_match(cases[i].p, (int)strlen(cases[i].p), cases[i].s,
		        (int)strlen(cases[i].s)) != cases[i].want)
			break;
	CHK(i == sizeof cases / sizeof cases[0], "E: pc_glob_match answers every case as pc_glob");

	printf("pcglobtest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
