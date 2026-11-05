/* enumwalktest.c - the cooperative KEYS walk (enumwalk.c; the 0.7.2 split,
 * wave 1): the key chunks, the registry's publish generation and the
 * connection's life are stubbed; replies are staged on each connection's
 * own buffer.
 *
 *   A  the list: walks begin and join this worker's list (pending), one
 *      ended from the middle leaves the others, abort without a walk is
 *      harmless
 *   B  the step: one chunk per walking connection per turn - two walks
 *      advance together; a walk that ends answers "*N" and its keys in
 *      one ordered reply, then the input behind it is served and the
 *      connection flushed; walks and turns counted
 *   C  a walk at its LIMIT answers what it has; a reply too large is
 *      refused by name; a table the registry republished since the walk
 *      began ends it with what was emitted, counted as ended on a swap
 *   D  the connection's end: a reply that cannot be staged, the input
 *      behind it a protocol error, a write error, or QUIT already
 *      answered - each destroys it, with the reason
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"
#include "store.h"
#include "verbs.h"
#include "proto.h"
#include "protoint.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* ---- the stubs ---------------------------------------------------------- */
static unsigned long long gen = 1;
static int per_turn = 2, nkeys = 5, chunks, plain_rc, flush_rc, staging_ok = 1;
static struct pc_conn *destroyed[8];
static int ndestroyed, plains, flushes;

unsigned int compat_ticks(void) { return 1; }
unsigned long long pc_store_pub_gen(int i) { (void)i; return gen; }

int pc_store_slot_pub(pcache_htable_t *ht, unsigned long long *pub)
{
	(void)ht;
	*pub = gen;
	return 0;
}

/* @per_turn keys a call ("k0", "k1", ... on the connection's cursor) out
 * of @nkeys, honouring the limit */
int pc_keys_chunk(void *ht, unsigned int *cursor, const char *pat, int patlen,
		unsigned int now, struct pc_jw *w, int *emitted, int limit,
		int *limit_hit)
{
	int i;

	(void)ht; (void)pat; (void)patlen; (void)now;
	chunks++;
	for (i = 0; i < per_turn && (int)*cursor < nkeys; i++) {
		char k[16];
		int n;

		if (*emitted >= limit) {
			*limit_hit = 1;
			return 1;
		}
		n = snprintf(k, sizeof k, "$2\r\nk%u\r\n", *cursor);
		pc_jw_raw(w, k, (size_t)n);
		(*emitted)++;
		(*cursor)++;
	}
	if ((int)*cursor >= nkeys)
		*cursor = 0;
	return *cursor != 0;
}

int out_append(struct pc_conn *c, const char *p, size_t n)
{
	if (!staging_ok || c->out_len + n > c->out_cap)
		return -1;
	memcpy(c->out + c->out_len, p, n);
	c->out_len += n;
	return 0;
}

int process_plaintext(struct pc_conn *c) { (void)c; plains++; return plain_rc; }
int conn_flush(struct pc_conn *c) { (void)c; flushes++; return flush_rc; }

void conn_destroy(struct pc_conn *c)
{
	enum_abort(c);                     /* as proto.c's does */
	destroyed[ndestroyed++] = c;
}

/* ---- the harness ------------------------------------------------------- */
static struct pc_conn conns[4];
static char outs[4][4096];

static struct pc_conn *conn(int i)
{
	struct pc_conn *c = &conns[i];

	memset(c, 0, sizeof *c);
	c->out = outs[i];
	c->out_cap = sizeof outs[i];
	return c;
}

static void walk(struct pc_conn *c, int limit)
{
	struct pc_enum_start es;

	memset(&es, 0, sizeof es);
	es.start = 1;
	es.ht = (void *)c;
	es.patlen = -1;
	es.limit = limit;
	if (enum_begin(c, &es) != 0)
		printf("  (enum_begin failed)\n");
}

static int replied(const struct pc_conn *c, const char *want)
{
	return c->out_len == strlen(want) && !memcmp(c->out, want, c->out_len);
}

int main(void)
{
	struct pc_conn *a, *b, *x;
	unsigned long long walks, turns;
	int turns_n;

	/* A */
	CHK(!pc_enum_pending(), "A: no walk - nothing pending");
	a = conn(0);
	b = conn(1);
	x = conn(2);
	walk(a, 1000);
	walk(b, 1000);
	walk(x, 1000);
	CHK(pc_enum_pending() && a->enum_active && b->enum_active && x->enum_active,
		"A: three walks begun - pending");
	enum_abort(b);
	CHK(!b->enum_active && !b->enum_next && a->enum_active && x->enum_active,
		"A: the middle one aborted - the others walk on");
	enum_abort(b);
	CHK(!b->enum_active, "A: aborting it again - harmless");
	enum_abort(a);
	enum_abort(x);
	CHK(!pc_enum_pending(), "A: all ended - nothing pending");

	/* B */
	a = conn(0);
	b = conn(1);
	walk(a, 1000);
	walk(b, 1000);
	chunks = plains = flushes = 0;
	pc_enum_step();
	CHK(chunks == 2 && a->enum_active && b->enum_active && !a->out_len,
		"B: one turn - one chunk each, nothing answered yet");
	pc_enum_step();
	pc_enum_step();
	CHK(chunks == 6 && replied(a, "*5\r\n$2\r\nk0\r\n$2\r\nk1\r\n$2\r\nk2\r\n$2\r\nk3\r\n$2\r\nk4\r\n") &&
		replied(b, "*5\r\n$2\r\nk0\r\n$2\r\nk1\r\n$2\r\nk2\r\n$2\r\nk3\r\n$2\r\nk4\r\n"),
		"B: 5 keys, 2 a turn - three turns, then one ordered reply each");
	CHK(!a->enum_active && !b->enum_active && !pc_enum_pending() && plains == 2 && flushes == 2,
		"B: each walk ended, its input behind served, the connection flushed");
	pc_enum_figures(&walks, &turns);
	CHK(walks == 2 && turns == 6, "B: 2 walks, 6 turns counted");

	/* C */
	a = conn(0);
	walk(a, 3);
	for (turns_n = 0; a->enum_active && turns_n < 10; turns_n++)
		pc_enum_step();
	CHK(replied(a, "*3\r\n$2\r\nk0\r\n$2\r\nk1\r\n$2\r\nk2\r\n"), "C: LIMIT 3 - three keys answered");
	a = conn(0);
	walk(a, 1000);
	a->enum_w.overflow = 1;
	pc_enum_step();
	CHK(replied(a, "-ERR reply too large\r\n") && !a->enum_active, "C: a reply too large - refused by name");
	a = conn(0);
	walk(a, 1000);
	pc_enum_step();
	gen++;
	pc_enum_step();
	CHK(replied(a, "*2\r\n$2\r\nk0\r\n$2\r\nk1\r\n") && pc_enum_ended_on_swap() == 1,
		"C: the table republished mid-walk - ended with the 2 emitted, counted as a swap");

	/* D */
	ndestroyed = 0;
	a = conn(0);
	walk(a, 1000);
	staging_ok = 0;
	for (turns_n = 0; a->enum_active && turns_n < 10; turns_n++)
		pc_enum_step();
	CHK(ndestroyed == 1 && destroyed[0] == a && !pc_enum_pending(), "D: the reply cannot be staged - destroyed");
	staging_ok = 1;
	a = conn(0);
	walk(a, 1000);
	plain_rc = -1;
	for (turns_n = 0; a->enum_active && turns_n < 10; turns_n++)
		pc_enum_step();
	CHK(ndestroyed == 2 && a->why && !strcmp(a->why, "protocol error"),
		"D: the input behind it a protocol error - destroyed, so said");
	plain_rc = 0;
	a = conn(0);
	walk(a, 1000);
	flush_rc = -1;
	for (turns_n = 0; a->enum_active && turns_n < 10; turns_n++)
		pc_enum_step();
	CHK(ndestroyed == 3 && a->why && !strcmp(a->why, "write error"), "D: a write error - destroyed, so said");
	flush_rc = 0;
	a = conn(0);
	walk(a, 1000);
	a->close_after = 1;
	for (turns_n = 0; a->enum_active && turns_n < 10; turns_n++) {
		pc_enum_step();
		if (!a->enum_active)
			a->out_len = 0;            /* as if the flush drained it */
	}
	CHK(ndestroyed == 3, "D: QUIT behind the walk, its reply still staged - not yet");
	a = conn(0);
	walk(a, 1000);
	a->close_after = 1;
	nkeys = 0;                         /* an empty table: answered at once */
	pc_enum_step();
	CHK(ndestroyed == 3 && replied(a, "*0\r\n"), "D: QUIT behind, the empty reply staged - kept for its flush");
	a->out_len = 0;
	nkeys = 5;

	printf("enumwalktest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
