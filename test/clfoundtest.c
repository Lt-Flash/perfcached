/* clfoundtest.c - who founds a blank fleet (clfound.c): the joiner table, the settle window and the
 * blank unicast node's gate, through stubbed clstate.h accessors. */
#include <arpa/inet.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "compat/dprint.h"
#include "cluster.h"
#include "clmemb.h"
#include "clwire.h"
#include "clstate.h"
#include "clfound.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* ---- the stubs: clstate.h, the cluster id, the log ----------- */
static unsigned char self_ident[16] = { 0x50 };
static int unicast = 1, expect, has_cid, notices;
static char last_log[512];

static pc_addr addr(int last)
{
	pc_addr a;

	memset(&a, 0, sizeof a);
	pc_addr_from_v4(&a, htonl(0x0a000000u | (unsigned)last), 6480);
	return a;
}

pc_addr cl_self_addr(void) { return addr(5); }
const unsigned char *cl_self_ident(void) { return self_ident; }
int cl_unicast_on(void) { return unicast; }
int pc_cluster_expect(void) { return expect; }
int pc_cid_get(unsigned char out[16]) { memset(out, has_cid, 16); return has_cid ? 1 : CLWIRE_CID_NONE; }
int compat_log_level = L_NOTICE;

void compat_log(int level, const char *tag, const char *fmt, ...)
{
	va_list ap;

	(void)tag;
	va_start(ap, fmt);
	vsnprintf(last_log, sizeof last_log, fmt, ap);
	va_end(ap);
	notices += level == L_NOTICE;
}

static void note(int last, int id0, long long now)
{
	unsigned char id[16] = { 0 };
	pc_addr a = addr(last);

	id[0] = (unsigned char)id0;
	clfound_note(&a, id0 ? id : NULL, now);
}

static long long wait_says(int reason, long long now)
{
	struct clmemb_wait w = { 0 };
	pc_addr m = addr(1);

	w.reason = reason;
	w.node = 1;
	w.count = 2;
	return clfound_wait(&w, &m, now);
}

int main(void)
{
	long long at;
	int n;

	printf("--- A: the settle window\n");
	clfound_window_open(1000);
	CHK(clfound_settle_at(1000) == 4000, "no joiner: 3 s from the window's opening (%lld)",
		clfound_settle_at(1000));
	note(6, 0x60, 1500);
	CHK(clfound_settle_at(1500) == 4000, "a joiner with a higher identity adds nothing (%lld)",
		clfound_settle_at(1500));
	note(7, 0x40, 3500);
	CHK(clfound_settle_at(3500) == 7500, "a new one 0.5 s before the end stretches it to 1 s past, and "
		"its lower identity adds 3 s (%lld)", clfound_settle_at(3500));
	note(8, 0, 3600);
	CHK(clfound_settle_at(3600) == 7600, "one that sends no identity stretches the window but is "
		"not ranked below (%lld)", clfound_settle_at(3600));
	note(7, 0x40, 3700);
	CHK(clfound_settle_at(3700) == 7600, "one heard again is no new joiner (%lld)",
		clfound_settle_at(3700));
	note(9, 0x30, 10500);
	at = clfound_settle_at(10500);
	CHK(at == 14000, "at most 10 s, and the three silent 3 s have left: only the new lower one "
		"counts (%lld)", at);

	printf("--- B: QUEUED and ELECTING\n");
	n = notices;
	CHK(wait_says(CLMEMB_WAIT_QUEUED, 20000) == 23000 && notices == n + 1 &&
		strstr(last_log, "queued for admission (place 2)"), "QUEUED holds 3 s, said: %s", last_log);
	CHK(clfound_settle_at(20000) == 23000, "and restarts the window (%lld)", clfound_settle_at(20000));
	CHK(wait_says(CLMEMB_WAIT_QUEUED, 21000) == 24000 && notices == n + 1,
		"QUEUED again: held from the last, not said again (%d notice(s))", notices - n);
	CHK(wait_says(CLMEMB_WAIT_ELECTING, 21500) == 24500 && notices == n + 2 &&
		strstr(last_log, "says it is electing"), "ELECTING is said: %s", last_log);
	clfound_window_open(30000);
	CHK(wait_says(CLMEMB_WAIT_ELECTING, 30100) == 33100 && notices == n + 3,
		"a new join says it again (%d notice(s))", notices - n);

	printf("--- C: S36c, a blank unicast node\n");
	unicast = 0;
	note(10, 0x20, 40000);
	CHK(!clfound_gate_blocks(40100), "multicast: never blocked");
	unicast = 1;
	has_cid = 1;
	CHK(!clfound_gate_blocks(40100), "a node with a cluster id: never blocked");
	has_cid = 0;
	CHK(clfound_gate_blocks(41000) && strstr(last_log, "still changing"),
		"the joiners changed 1 s ago: blocked (%s)", last_log);
	note(10, 0x20, 44000);
	CHK(!clfound_gate_blocks(45100), "stable, a joiner with a higher address: not blocked - the "
		"identity rank decides");
	expect = 3;
	CHK(clfound_gate_blocks(45300) && strstr(last_log, "fewer blank nodes"),
		"one other joiner, expect 3: blocked (%s)", last_log);

	printf("clfoundtest: %s\n", fails ? "FAILED" : "all passed");
	return fails != 0;
}
