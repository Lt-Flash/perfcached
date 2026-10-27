/* respdoortest.c - the RESP door's framing (respdoor.c; the 0.7.2 split,
 * wave 1): the verb layer, the parking, the walk and the observers are
 * stubbed, proto.c's reply staging captured.
 *
 *   A  multibulk: one command and the bytes it consumed; arguments by
 *      length (a CRLF inside a bulk is data); bare LF accepted; *0
 *      consumed with no command; two pipelined, one at a time; incomplete
 *      at every stage is 0; malformed - a count that is not a number, over
 *      PC_RESP_MAXARGS, no header in 16 bytes, not a bulk, a negative or
 *      oversized length, junk after a bulk - is -1
 *   B  inline: words split on spaces and tabs, CRLF or LF; an empty line
 *      consumed with no command; incomplete is 0; 64 KB with no newline -1
 *   C  the outcomes: the reply staged; QUIT closes after it; a reply too
 *      large refused by name; a verb's -1 drops the connection; a command
 *      parked on a pull stages nothing, is consumed and holds the
 *      connection (S332) - and is answered as a miss, nothing held, when
 *      the park table refuses; a walk begun stages nothing, or
 *      refuses for memory; a read-through hold leaves the command in the
 *      buffer, uncounted; a command run clears the hold's verdicts; G8:
 *      an MGET waiting on lookups elsewhere is consumed, stages nothing and
 *      holds the connection (counted now - it does not run again), and one
 *      the door cannot attach for memory is refused
 *   D  the observers (S53, S159): each command counted under its name with
 *      its time from the carried clock - one clock read a command - and
 *      logged as err, miss, hit or ok by its reply
 *   E  resp_ll_span: signs, the 17-character bound, junk and empty refused
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"
#include "verbs.h"
#include "proto.h"
#include "protoint.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* ---- the stubs ---------------------------------------------------------- */
static char cap_buf[8192];
__thread unsigned long long obs_carry;
static unsigned long long fake_us, last_dt;
static int verb_calls, verb_nargs, park_ok = 1, parks, enum_ok = 1, enums, cmds;
static char verb_argv[8][32];
static size_t verb_argl[8];
static char last_cmd[32], last_outcome[8];

/* as proto.c's: staged on c->out, which the observers read back */
int out_append(struct pc_conn *c, const char *p, size_t n)
{
	if (c->out_len + n > c->out_cap)
		return -1;
	memcpy(c->out + c->out_len, p, n);
	c->out_len += n;
	return 0;
}

void ev_origin(struct pc_conn *c, const char *door) { (void)c; (void)door; }

char *scratch_buf(int i)
{
	static char b[2][JW_REPLY_CAP];

	return b[i];
}

int park_add(struct pc_conn *c, unsigned int req, const char *id,
		const char *col, size_t collen, const char *key, size_t klen,
		int bin, int hint)
{
	(void)c; (void)req; (void)id; (void)col; (void)collen; (void)key;
	(void)klen; (void)hint;
	parks++;
	return park_ok && bin == PARK_RESP ? 0 : -1;
}

/* G8: the mget a verb parked, and the door's attach */
static char mget_obj;
static int mget_parked, mget_attach_ok = 1, mget_attached, mget_freed;
struct pc_mget *pc_mget_take(void)
{
	struct pc_mget *m = mget_parked ? (struct pc_mget *)&mget_obj : NULL;

	mget_parked = 0;
	return m;
}
int pc_mget_attach(struct pc_mget *m, void *conn, int door, const char *id)
{
	(void)conn; (void)id;
	mget_attached += m == (struct pc_mget *)&mget_obj && door == 2 && !id;
	return mget_attach_ok ? 0 : -1;
}
void pc_mget_free(struct pc_mget *m) { mget_freed += m == (struct pc_mget *)&mget_obj; }

int enum_begin(struct pc_conn *c, const struct pc_enum_start *es)
{
	(void)c;
	enums++;
	return enum_ok && es->start ? 0 : -1;
}

unsigned int compat_ticks(void) { return 1; }
unsigned long long pc_obs_usec_now(void) { return fake_us += 10; }

void pc_obs_cmd(const char *name, size_t nlen, unsigned long long usec)
{
	cmds++;
	snprintf(last_cmd, sizeof last_cmd, "%.*s", (int)nlen, name);
	last_dt = usec;
}

void pc_obs_conn_touch(void *row, const char *cmd, size_t clen, unsigned int now_ticks)
{ (void)row; (void)cmd; (void)clen; (void)now_ticks; }
void pc_obs_slow(char *const *argv, const size_t *argl, int nargs,
		unsigned long long usec, void *connrow)
{ (void)argv; (void)argl; (void)nargs; (void)usec; (void)connrow; }
int pc_obs_qlog_on(void) { return 1; }

void pc_obs_qlog(const char *dialect, void *row, const char *verb, size_t vlen,
		const char *col, size_t clen, const char *key, size_t klen,
		const char *outcome, unsigned long long usec)
{
	(void)dialect; (void)row; (void)verb; (void)vlen; (void)col; (void)clen;
	(void)key; (void)klen; (void)usec;
	snprintf(last_outcome, sizeof last_outcome, "%s", outcome);
}

/* the verb layer: answers by the command's name */
int pc_verb_resp(char *const *argv, const size_t *argl, int nargs,
		struct pc_jw *out, char *scratch, size_t scratch_cap,
		unsigned int *park_req, const char **colp, size_t *collenp,
		const char **keyp, size_t *klenp, char *cur_col,
		size_t *cur_collen, int *quit, int *authed,
		struct pc_enum_start *es, void *obs, void *conn)
{
	int i;

	(void)scratch; (void)scratch_cap; (void)cur_col; (void)cur_collen;
	(void)authed; (void)obs;
	verb_calls++;
	verb_nargs = nargs;
	for (i = 0; i < nargs && i < 8; i++) {
		verb_argl[i] = argl[i];
		memcpy(verb_argv[i], argv[i], argl[i] < 31 ? argl[i] : 31);
		verb_argv[i][argl[i] < 31 ? argl[i] : 31] = 0;
	}
#define IS(w) (argl[0] == strlen(w) && !memcmp(argv[0], w, argl[0]))
	if (IS("FAIL"))
		return -1;
	if (IS("QUIT"))
		*quit = 1;
	if (IS("BIG")) {
		out->overflow = 1;
		return 0;
	}
	if (IS("PARK")) {
		*park_req = 7;
		*colp = "c"; *collenp = 1;
		*keyp = "k"; *klenp = 1;
		pc_jw_lit(out, "$-1\r\n");             /* the fallback: a miss */
		return 0;
	}
	if (IS("WALK")) {
		es->start = 1;
		return 0;
	}
	if (IS("HOLD")) {
		((struct pc_conn *)conn)->up_hold = 9;
		return 0;
	}
	if (IS("MGET")) {
		mget_parked = 1;           /* lookups waiting: nothing written */
		return 0;
	}
	if (IS("GET"))
		pc_jw_lit(out, "$1\r\nv\r\n");
	else if (IS("MISS"))
		pc_jw_lit(out, "$-1\r\n");
	else if (IS("ERRX"))
		pc_jw_lit(out, "-ERR x\r\n");
	else
		pc_jw_lit(out, "+PONG\r\n");
	return 0;
#undef IS
}

/* ---- the harness ------------------------------------------------------- */
static struct pc_conn c;
static char inbuf[70000];

static int frame_at(const char *req, size_t n, size_t off)
{
	memset(inbuf, 'x', off);
	memcpy(inbuf + off, req, n);
	c.in = inbuf;
	c.in_len = off + n;
	c.close_after = 0;
	c.up_hold = 0;
	c.park_hold = 0;
	c.out = cap_buf;
	c.out_cap = sizeof cap_buf;
	c.out_len = 0;
	verb_calls = 0;
	return resp_one(&c, off);
}
#define FRAME(lit) frame_at((lit), sizeof(lit) - 1, 0)

static int staged(const char *s)
{
	return c.out_len == strlen(s) && !memcmp(c.out, s, c.out_len);
}

int main(void)
{
	long long v;
	int rc;

	/* A */
	rc = FRAME("*1\r\n$4\r\nPING\r\n");
	CHK(rc == 14 && verb_calls == 1 && verb_nargs == 1 && !strcmp(verb_argv[0], "PING") &&
		staged("+PONG\r\n"), "A: *1 PING - 14 bytes consumed, the reply staged");
	rc = FRAME("*2\r\n$4\r\nECHO\r\n$4\r\na\r\nb\r\n");
	CHK(rc == 24 && verb_nargs == 2 && verb_argl[1] == 4 && !memcmp(verb_argv[1], "a\r\nb", 4),
		"A: a CRLF inside a bulk is data - by length");
	rc = FRAME("*1\n$4\nPING\n");
	CHK(rc == 0 && verb_calls == 0,
		"A: bare LF, the command last in the buffer - waits for a byte more (the bound counts CRLF)");
	rc = FRAME("*1\n$4\nPING\n*");
	CHK(rc == 11 && verb_calls == 1, "A: bare LF with a byte after it - read, 11 bytes");
	rc = FRAME("*0\r\n");
	CHK(rc == 4 && verb_calls == 0 && c.out_len == 0, "A: *0 - consumed, no command");
	{
		static const char two[] = "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n*1\r\n$4\r\nPING\r\n";

		rc = frame_at(two, sizeof two - 1, 0);
		CHK(rc == 20 && !strcmp(verb_argv[0], "GET") && staged("$1\r\nv\r\n"),
			"A: two pipelined - the first alone, 20 bytes");
		rc = frame_at(two + 20, 14, 20);
		CHK(rc == 14 && !strcmp(verb_argv[0], "PING"), "A: then the second, from its offset");
	}
	{
		static const char whole[] = "*2\r\n$4\r\nECHO\r\n$4\r\na\r\nb\r\n";
		size_t cut;
		int zero = 1;

		for (cut = 1; cut < sizeof whole - 1; cut++)
			if (frame_at(whole, cut, 0) != 0 || verb_calls)
				zero = 0;
		CHK(zero, "A: cut at every one of its 23 shorter lengths - 0, nothing run");
	}
	CHK(FRAME("*x\r\n") == -1, "A: a count that is not a number - -1");
	CHK(FRAME("*600\r\n") == -1, "A: a count over PC_RESP_MAXARGS - -1");
	CHK(FRAME("*11111111111111111111") == -1, "A: no header line in 16 bytes - -1");
	CHK(FRAME("*1\r\n:4\r\nPING\r\n") == -1, "A: not a bulk - -1");
	CHK(FRAME("*1\r\n$-1\r\n") == -1, "A: a negative length - -1");
	CHK(FRAME("*1\r\n$1048577\r\n") == -1, "A: a length over PC_MAX_REQ - -1");
	CHK(FRAME("*1\r\n$4\r\nPINGxx") == -1, "A: junk where the CRLF belongs - -1");

	/* B */
	rc = FRAME("PING\r\n");
	CHK(rc == 6 && verb_nargs == 1 && !strcmp(verb_argv[0], "PING"), "B: inline PING - 6 bytes");
	rc = FRAME("SET  k \t v\r\n");
	CHK(rc == 12 && verb_nargs == 3 && !strcmp(verb_argv[0], "SET") && !strcmp(verb_argv[1], "k") &&
		!strcmp(verb_argv[2], "v"), "B: words split on spaces and tabs");
	rc = FRAME("PING\n");
	CHK(rc == 5 && verb_calls == 1, "B: LF alone ends the line");
	rc = FRAME("\r\n");
	CHK(rc == 2 && verb_calls == 0, "B: an empty line - consumed, no command");
	CHK(FRAME("PIN") == 0, "B: no newline yet - 0");
	{
		static char runaway[65538];

		memset(runaway, 'a', sizeof runaway);
		CHK(frame_at(runaway, sizeof runaway, 0) == -1, "B: 64 KB and no newline - -1");
	}

	/* C */
	rc = FRAME("*1\r\n$4\r\nQUIT\r\n");
	CHK(rc == 14 && c.close_after == 1 && staged("+PONG\r\n"), "C: QUIT - answered, then closed");
	rc = FRAME("*1\r\n$3\r\nBIG\r\n");
	CHK(rc == 13 && staged("-ERR reply too large\r\n"), "C: a reply too large - refused by name");
	CHK(FRAME("*1\r\n$4\r\nFAIL\r\n") == -1, "C: the verb's -1 drops the connection");
	parks = 0;
	rc = FRAME("*1\r\n$4\r\nPARK\r\n");
	CHK(rc == 14 && parks == 1 && c.out_len == 0 && c.park_hold == 1,
		"C: parked on a pull - consumed, nothing staged yet, the connection "
		"held (S332: what follows waits for its reply)");
	park_ok = 0;
	rc = FRAME("*1\r\n$4\r\nPARK\r\n");
	CHK(rc == 14 && staged("$-1\r\n") && c.park_hold == 0,
		"C: the park table refuses - answered as the miss it fell back to, "
		"nothing held");
	park_ok = 1;
	enums = 0;
	rc = FRAME("*1\r\n$4\r\nWALK\r\n");
	CHK(rc == 14 && enums == 1 && c.out_len == 0, "C: a walk begun - nothing staged, the chunks follow");
	enum_ok = 0;
	rc = FRAME("*1\r\n$4\r\nWALK\r\n");
	CHK(rc == 14 && staged("-ERR out of memory starting the walk\r\n"), "C: no memory for the walk - refused");
	enum_ok = 1;
	cmds = 0;
	rc = FRAME("*1\r\n$4\r\nHOLD\r\n");
	CHK(rc == 0 && c.up_hold == 9 && cmds == 0,
		"C: held on a read-through - left in the buffer, not counted (it runs again)");
	c.up_verdict = 3;
	c.up_lost = 1;
	frame_at("PING\r\n", 6, 0);
	CHK(c.up_verdict == 0 && c.up_lost == 0, "C: a command run clears the hold's verdict and re-gates");
	cmds = 0;
	rc = FRAME("*1\r\n$4\r\nMGET\r\n");
	CHK(rc == 14 && c.park_hold == 1 && mget_attached == 1 && c.out_len == 0 &&
		cmds == 1 && !mget_freed,
		"C: G8 - an MGET waiting on lookups: consumed, nothing staged, the "
		"connection held, counted now");
	mget_attach_ok = 0;
	rc = FRAME("*1\r\n$4\r\nMGET\r\n");
	CHK(rc == 14 && c.park_hold == 0 && mget_freed == 1 &&
		staged("-ERR out of memory\r\n"),
		"C: G8 - no memory to attach it: freed, refused");
	mget_attach_ok = 1;

	/* D */
	obs_carry = 0;
	fake_us = 100;
	cmds = 0;
	FRAME("*1\r\n$4\r\nPING\r\n");
	CHK(cmds == 1 && !strcmp(last_cmd, "PING") && last_dt == 10 && obs_carry == 120,
		"D: counted under its name; from a fresh clock (110) to the end (120): 10 us");
	FRAME("*1\r\n$4\r\nPING\r\n");
	CHK(last_dt == 10 && obs_carry == 130, "D: the next from the carried clock - one read a command");
	FRAME("*2\r\n$3\r\nGET\r\n$1\r\nk\r\n");
	CHK(!strcmp(last_outcome, "hit"), "D: a bulk - hit");
	FRAME("*1\r\n$4\r\nMISS\r\n");
	CHK(!strcmp(last_outcome, "miss"), "D: $-1 - miss");
	FRAME("*1\r\n$4\r\nERRX\r\n");
	CHK(!strcmp(last_outcome, "err"), "D: an error line - err");
	FRAME("*1\r\n$4\r\nPING\r\n");
	CHK(!strcmp(last_outcome, "ok"), "D: anything else - ok");

	/* E */
	CHK(resp_ll_span("123", 3, &v) == 0 && v == 123, "E: 123");
	CHK(resp_ll_span("-5", 2, &v) == 0 && v == -5 && resp_ll_span("+7", 2, &v) == 0 && v == 7,
		"E: -5 and +7");
	CHK(resp_ll_span("", 0, &v) == -1 && resp_ll_span("-", 1, &v) == -1, "E: empty, or a sign alone - -1");
	CHK(resp_ll_span("12a", 3, &v) == -1, "E: junk - -1");
	CHK(resp_ll_span("12345678901234567", 17, &v) == 0 && v == 12345678901234567LL &&
		resp_ll_span("123456789012345678", 18, &v) == -1, "E: 17 characters read, 18 refused");
	CHK(resp_ll_span("-1234567890123456", 17, &v) == 0 && resp_ll_span("-12345678901234567", 18, &v) == -1,
		"E: the sign counts toward the 17");

	printf("respdoortest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
