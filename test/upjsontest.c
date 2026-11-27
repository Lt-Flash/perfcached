/* upjsontest.c - the upfill sweep's document comparison (upjson.c): values compared by meaning, and
 * an object whose last member is a key with no value never read past the parsed tokens */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "upjson.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static int same(const char *a, const char *b)
{
	int walked;

	return up_json_same(a, strlen(a), b, strlen(b), &walked);
}

int main(void)
{
	int r;

	/* A: the plain verdicts */
	CHK(same("{\"a\":1,\"b\":[1,2]}", "{ \"b\" : [1, 2], \"a\" : 1 }") == 1,
		"A: members in another order, other spacing - the same");
	CHK(same("{\"a\":1}", "{\"a\":2}") == 0, "A: another value - not the same");
	CHK(same("[1,2]", "[2,1]") == 0, "A: elements in another order - not the same");
	CHK(same("{\"a\":-0}", "{\"a\":0}") == 1, "A: -0 and 0 - the same number");
	{
		/* a bare one-byte number in a buffer of its size: nothing read past it (ASan) */
		char *five = malloc(1);
		int w;

		if (five) {
			five[0] = '5';
			CHK(up_json_same("10", 2, five, 1, &w) == 0,
				"A: 10 against a bare 5 - not the same, read within its byte");
			free(five);
		}
	}

	/* B: {"k":1,"k"} - its last token is a key; the walk takes token 4 as its value, which this
	 * parse never wrote: the thread's buffer still holds an earlier document's (planted here) */
	(void)same("[[[[ 1]]]]", "[[[[ 2]]]]");          /* token 4: a 1 at bytes 5..6 */
	r = same("{\"k\":1,\"k\"}", "{\"k\":1}");
	CHK(r != 1, "B: {\"k\":1,\"k\"} against {\"k\":1} - not judged the same off a stale token (%d)", r);

	/* C: the same on Redis's side: {1 2 "k"} matched in place, its value token one past */
	(void)same("[[[3]]]       ", "[[[2]]]");         /* b's token 3 at index 12: a 2 at 3..4 */
	r = same("{\"1\":2,\"k\":2}", "{1 2 \"k\"}");
	CHK(r != 1, "C: {1 2 \"k\"} from Redis - not judged the same off a stale token (%d)", r);

	up_json_release();
	printf("upjsontest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
