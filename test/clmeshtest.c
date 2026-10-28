/* clmeshtest.c - the unicast mesh's known addresses (clmesh.c), CH-14's
 * trial lifetime: on a clock the test supplies; no seeds, no resolver.
 *
 *   A  an address heard on trial is kept CLMESH_TRIAL_MS, and gone then
 *   B  confirmed after a trial start (an admitted node's ALIVE, the map),
 *      it is kept CLMESH_KNOWN_MS from then
 *   C  a confirmed address heard again on trial stays confirmed
 *   D  "new": the first hearing, and again once it lapsed - a lapsed
 *      confirmed address heard on trial is on trial again
 *   E  the targets a control frame goes to skip a lapsed trial address
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include "compat/dprint.h"
#include "pcaddr.h"
#include "clmesh.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

int compat_log_level = L_WARN;

void compat_log(int level, const char *tag, const char *fmt, ...)
{
	va_list ap;

	(void)level; (void)tag;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static pc_addr at(unsigned last)
{
	pc_addr x;

	memset(&x, 0, sizeof x);
	x.in4.sin_family = AF_INET;
	x.in4.sin_addr.s_addr = htonl(0x7f004d00u | last);
	x.in4.sin_port = htons(17000);
	return x;
}

/* is @a among the known addresses at @now? */
static int known(const pc_addr *a, long long now)
{
	pc_addr out[CLMESH_MAX_KNOWN];
	int i, n = clmesh_known(out, CLMESH_MAX_KNOWN, now);

	for (i = 0; i < n; i++)
		if (pc_addr_eq(&out[i], a))
			return 1;
	return 0;
}

int main(void)
{
	long long t = 1000000;
	pc_addr a = at(1), b = at(2), c = at(3), d = at(4), self = at(99);
	pc_addr tg[CLMESH_MAX_KNOWN];
	int i, n, has;

	/* A */
	clmesh_heard(&a, t, 1);
	CHK(known(&a, t + CLMESH_TRIAL_MS - 1) && !known(&a, t + CLMESH_TRIAL_MS),
		"A: on trial - kept %d s, gone then", CLMESH_TRIAL_MS / 1000);

	/* B */
	clmesh_heard(&b, t, 1);
	clmesh_heard(&b, t + 10000, 0);
	CHK(known(&b, t + 10000 + CLMESH_KNOWN_MS - 1) && !known(&b, t + 10000 + CLMESH_KNOWN_MS),
		"B: confirmed 10 s after its trial began - kept %d min from then", CLMESH_KNOWN_MS / 60000);

	/* C */
	clmesh_heard(&c, t, 0);
	clmesh_heard(&c, t + 300000, 1);
	CHK(known(&c, t + 300000 + CLMESH_KNOWN_MS - 1),
		"C: confirmed, then heard on trial - still confirmed");

	/* D */
	CHK(clmesh_heard(&d, t, 0) == 1 && clmesh_heard(&d, t + 1000, 1) == 0,
		"D: new the first time, not the second");
	CHK(clmesh_heard(&d, t + 1000 + CLMESH_KNOWN_MS, 1) == 1 &&
		known(&d, t + 1000 + CLMESH_KNOWN_MS + CLMESH_TRIAL_MS - 1) &&
		!known(&d, t + 1000 + CLMESH_KNOWN_MS + CLMESH_TRIAL_MS),
		"D: lapsed, then heard on trial - new, and on trial again (%d s)", CLMESH_TRIAL_MS / 1000);

	/* E */
	n = clmesh_targets(tg, CLMESH_MAX_KNOWN, &self, t + CLMESH_TRIAL_MS + 1);
	for (has = 0, i = 0; i < n; i++)
		has |= pc_addr_eq(&tg[i], &a) ? 1 : pc_addr_eq(&tg[i], &b) ? 2 : 0;
	CHK(has == 2, "E: the targets after the trial - the confirmed address, not the lapsed one");

	printf("clmeshtest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
