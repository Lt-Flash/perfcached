/*
 * psudptest.c - the life of a UDP push stream (PS5), driven through
 * src/psudp.h exactly as a worker's sweep runs it: probes, expiry, and
 * the two prunes - an ACK that stopped arriving, and one that stopped
 * moving while datagrams kept going out.
 */
#include <stdio.h>
#include <string.h>

#include "psudp.h"

static int pass, fail;

#define CHK(c, msg) do { \
	if (c) pass++; \
	else { fail++; printf("  FAIL %s\n", msg); } \
} while (0)

/* sweep every 250 ms from @from to @to; how many probes it sends, and the
 * first non-probe verdict with its time */
static int run(struct psu_stream *s, long long from, long long to,
		long long *when)
{
	long long t;

	for (t = from; t <= to; t += 250) {
		int d = psu_due(s, t);

		if (d == PSU_PROBE) {
			psu_probe_sent(s, t);
			continue;
		}
		if (d != PSU_NOTHING) {
			*when = t;
			return d;
		}
	}
	*when = -1;
	return PSU_NOTHING;
}

int main(void)
{
	struct psu_stream s;
	long long when;
	uint64_t seq;
	int d;

	/* ---- probing, then expiry ---- */
	psu_start(&s, 42, 1000);
	CHK(psu_due(&s, 1000) == PSU_PROBE, "a new stream probes at once");
	seq = psu_probe_sent(&s, 1000);
	CHK(seq == 1, "the first probe takes sequence 1");
	CHK(psu_due(&s, 1500) == PSU_NOTHING, "not again within a second");
	d = run(&s, 1250, 20000, &when);
	CHK(s.probes == PSU_PROBES, "three probes in all, a second apart");
	CHK(d == PSU_EXPIRE && when == 11000,
		"unconfirmed, the stream expires 10 s after it started, silently");

	/* ---- confirmed: an ACK on the timer keeps an idle stream ---- */
	psu_start(&s, 43, 0);
	psu_probe_sent(&s, 0);
	psu_confirm(&s, 300);
	CHK(s.active && s.acked == s.seq, "confirming counts the probes as seen");
	CHK(psu_due(&s, 14000) == PSU_NOTHING, "an idle stream is fine at 14 s");
	CHK(psu_ack(&s, s.seq, 5300) == 0 && psu_ack(&s, s.seq, 10300) == 0,
		"the client acks on its 5 s timer with nothing new");
	CHK(psu_due(&s, 25000) == PSU_NOTHING,
		"idle and acknowledged: never stalled, never silent");
	CHK(psu_due(&s, 25300) == PSU_PRUNE_SILENT,
		"15 s after the last ack the stream is pruned as silent");

	/* ---- sending with acks that keep up ---- */
	psu_start(&s, 44, 0);
	psu_probe_sent(&s, 0);
	psu_confirm(&s, 100);
	{
		long long t;
		int ok = 1;

		for (t = 200; t < 60000; t += 10) {
			psu_message_sent(&s, t);
			if (t % 5000 == 0 && psu_ack(&s, s.seq - 3, t) != 0)
				ok = 0;
			if (psu_due(&s, t) != PSU_NOTHING)
				ok = 0;
		}
		CHK(ok, "a minute of sending, acked every 5 s a little behind: no prune");
	}

	/* ---- UDP broken while the acks keep coming over TCP ---- */
	psu_start(&s, 45, 0);
	psu_probe_sent(&s, 0);
	psu_confirm(&s, 100);
	psu_message_sent(&s, 1000);
	psu_ack(&s, s.seq, 2000);                 /* caught up at 2 s */
	{
		long long t, pruned = -1;
		uint64_t stuck = s.seq;

		for (t = 3000; t < 40000; t += 100) {
			psu_message_sent(&s, t);
			if (t % 5000 == 0)
				psu_ack(&s, stuck, t);        /* the same number, on time */
			if (psu_due(&s, t) == PSU_PRUNE_STALLED) {
				pruned = t;
				break;
			}
			if (psu_due(&s, t) != PSU_NOTHING)
				break;
		}
		CHK(pruned == 18000,
			"acks on time but not moving: pruned as stalled 15 s after the first unacknowledged send");
	}

	/* ---- an ack beyond what was sent ---- */
	psu_start(&s, 46, 0);
	psu_probe_sent(&s, 0);
	psu_confirm(&s, 10);
	psu_message_sent(&s, 20);
	CHK(psu_ack(&s, s.seq + 1, 30) == -1 && s.acked == 1,
		"an ack past the last datagram is refused and changes nothing");
	CHK(psu_ack(&s, s.seq, 30) == 0 && s.pending_ms == 0,
		"an ack of everything clears the stall clock");

	printf("psudptest: %d passed, %d failed\n", pass, fail);
	return fail != 0;
}
