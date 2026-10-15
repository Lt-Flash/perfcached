/*
 * clpushtest.c - the per-thread write-path push groups (M5).
 *
 * S110 moved these groups off a shared per-peer mutex and onto the
 * thread that fills them, which is what took the write path out of
 * futex.  The cases here are the ways that could go wrong silently:
 *   - a group sealed with another thread's records in it, which is the
 *     whole property S110 bought
 *   - a slot that changed hands keeping what was gathered for the peer
 *     that left, so a newcomer receives a stranger's writes
 *   - a flush that leaves a partial group behind, so a lone write sits
 *     unsent past its interval
 *   - the open count drifting, which decides whether the cluster loop
 *     bothers to send the flush RPC at all
 *   - S253: a non-holder key noted in a DIFFERENT group from its record
 *     (the ack would then confirm a datagram that never carried it), an
 *     ack taken from the wrong peer or for a reused slot, and parked
 *     keys that never leave the table
 *
 * No socket: a group leaves through a callback that records it, which
 * is exactly how cluster.c passes seal_send in production.
 *
 * Build: cc -o clpushtest test/clpushtest.c src/clpush.o -lpthread
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/clpush.h"

static int pass, fail;
static void chk(int cond, const char *what)
{
	if (cond) { pass++; return; }
	fail++;
	printf("  FAIL %s\n", what);
}

static struct clpush P;

struct rec_send { int calls; unsigned int last_count; size_t last_len;
		  struct wgroup *last; };

static struct clpush *SENDP;
static void recording_send_b(struct wgroup *g, void *ctx)
{
	struct rec_send *r = ctx;

	r->calls++;
	r->last_count = g->qcount;
	r->last_len = g->qlen;
	r->last = g;
	if (SENDP)
		clpush_sent(SENDP, g);
}

static void recording_send(struct wgroup *g, void *ctx)
{
	struct rec_send *r = ctx;

	r->calls++;
	r->last_count = g->qcount;
	r->last_len = g->qlen;
	r->last = g;
	clpush_sent(&P, g);
}

static struct sockaddr_in A(unsigned char last)
{
	struct sockaddr_in s;

	memset(&s, 0, sizeof s);
	s.sin_family = AF_INET;
	s.sin_port = htons(6479);
	s.sin_addr.s_addr = htonl(0x0A160000u | last);
	return s;
}

#define HDR 9
static unsigned char REC[256];

/* --- concurrency: two threads, their own slots, at the same time ---- */
struct arm { int tidx; int n; struct rec_send rs; };
static void *filler(void *arg)
{
	struct arm *a = arg;
	struct sockaddr_in to = A(1);
	int i;

	for (i = 0; i < a->n; i++)
		clpush_append(&P, a->tidx, 0, &to, REC, 30, 1000,
			recording_send, &a->rs);
	return NULL;
}

int main(void)
{
	struct sockaddr_in p1 = A(1), p2 = A(2);
	struct rec_send rs;
	struct wgroup *g;

	printf("=== clpushtest ===\n");
	memset(REC, 0xAB, sizeof REC);

	/* ---- A. opening a group ------------------------------------------ */
	clpush_init(&P, 4, HDR, 200, 100, 3);
	memset(&rs, 0, sizeof rs);
	chk(clpush_open(&P) == 0, "A1 nothing is open before the first write");
	clpush_append(&P, 0, 0, &p1, REC, 30, 1000, recording_send, &rs);
	chk(clpush_open(&P) == 1, "A2 the first append opens a group");
	g = &P.g[0][0];
	chk(g->qcount == 1 && g->qlen == HDR + 30,
		"A2 which holds one record after the header");
	chk(rs.calls == 0, "A3 and nothing was sent yet");

	/* ---- B. the two send points --------------------------------------- */
	clpush_append(&P, 0, 0, &p1, REC, 30, 1000, recording_send, &rs);
	clpush_append(&P, 0, 0, &p1, REC, 30, 1000, recording_send, &rs);
	chk(rs.calls == 0 && g->qcount == 3, "B1 records coalesce into one group");
	clpush_append(&P, 0, 0, &p1, REC, 30, 1000, recording_send, &rs);
	chk(rs.calls == 1 && rs.last_count == 4,
		"B2 crossing the flush size sends the group, all four records");
	chk(g->qcount == 0 && g->qlen == HDR && clpush_open(&P) == 0,
		"B2 and the group is reset behind it");

	/* a stale group goes out BEFORE the append that found it stale */
	memset(&rs, 0, sizeof rs);
	clpush_append(&P, 0, 0, &p1, REC, 30, 1000, recording_send, &rs);
	clpush_append(&P, 0, 0, &p1, REC, 30, 1000 + 3, recording_send, &rs);
	chk(rs.calls == 1 && rs.last_count == 1,
		"B3 a group older than the interval goes before the next append");
	chk(g->qcount == 1, "B3 and the new record opens a fresh one");

	/* a record larger than the gather cap goes alone, after the append */
	clpush_init(&P, 4, HDR, 50, 100000, 3);
	memset(&rs, 0, sizeof rs);
	clpush_append(&P, 0, 0, &p1, REC, 100, 1000, recording_send, &rs);
	chk(rs.calls == 1 && rs.last_count == 1,
		"B4 a record bigger than the gather cap is sent on its own");

	/* ---- C. a slot that changed hands --------------------------------- */
	clpush_init(&P, 4, HDR, 200, 100000, 3);
	memset(&rs, 0, sizeof rs);
	clpush_append(&P, 0, 0, &p1, REC, 30, 1000, recording_send, &rs);
	chk(clpush_open(&P) == 1, "C1 a group is open for the first peer");
	clpush_append(&P, 0, 0, &p2, REC, 30, 1000, recording_send, &rs);
	chk(rs.calls == 0,
		"C2 a slot that changed hands DROPS what it gathered - it is not sent");
	chk(P.g[0][0].qcount == 1 && clpush_open(&P) == 1,
		"C2 and the new peer's record opens the group afresh");

	/* ---- D. flush_mine ------------------------------------------------ */
	clpush_init(&P, 4, HDR, 200, 100000, 3);
	memset(&rs, 0, sizeof rs);
	clpush_append(&P, 0, 0, &p1, REC, 30, 1000, recording_send, &rs);
	clpush_append(&P, 0, 1, &p2, REC, 30, 1000, recording_send, &rs);
	clpush_flush_mine(&P, 0, 1001, recording_send, &rs);
	chk(rs.calls == 0, "D1 a flush leaves groups younger than the interval");
	clpush_flush_mine(&P, 0, 1003, recording_send, &rs);
	chk(rs.calls == 2, "D2 and sends both once they are due");
	chk(P.g[0][0].qcount == 0 && P.g[0][1].qcount == 0 &&
		clpush_open(&P) == 0, "D3 leaving no partial group behind");
	clpush_append(&P, 0, 0, &p1, REC, 30, 1000, recording_send, &rs);
	memset(&rs, 0, sizeof rs);
	clpush_flush_mine(&P, 1, 9999, recording_send, &rs);
	chk(rs.calls == 0 && P.g[0][0].qcount == 1,
		"D4 a thread flushing its own slot never touches another's");

	/* ---- E. two threads at once, each in its own slot ------------------ */
	/* cap AND flush_bytes both far beyond 100 records, so neither
	 * send point is reached and what is left in each group is
	 * exactly what its own thread put there. */
	clpush_init(&P, 4, HDR, 100000, 100000, 3);
	{
		pthread_t t1, t2;
		struct arm a1 = { 0, 100, { 0, 0, 0, NULL } };
		struct arm a2 = { 1, 100, { 0, 0, 0, NULL } };

		pthread_create(&t1, NULL, filler, &a1);
		pthread_create(&t2, NULL, filler, &a2);
		pthread_join(t1, NULL);
		pthread_join(t2, NULL);
		chk(P.g[0][0].qcount > 0 && P.g[1][0].qcount > 0,
			"E1 both threads filled their own slot");
		chk(P.g[0] != P.g[1],
			"E2 the two threads never shared an array - the S110 property");
		chk(a1.rs.calls == 0 && a2.rs.calls == 0,
			"E3 neither crossed a send point, so nothing was sealed");
		chk(P.g[0][0].qcount == 100 && P.g[1][0].qcount == 100,
			"E4 each group holds exactly its own thread's records");
	}
	clpush_fini(&P);
	chk(clpush_open(&P) == 0, "E5 fini leaves nothing open");

	/* ---- the buffer is sized from the FIRST record ------------------
	 *
	 * clpush_append allocates hdr + flush_bytes + n on the first record
	 * for a slot and never grows it, but the pre-append guard tests
	 * qlen + n against CAP.  cap is the wire's gather ceiling and has
	 * nothing to do with what was allocated, so a small first record
	 * followed by a larger one walks off the end of the malloc while
	 * every check in the function passes.
	 *
	 * The comment above the malloc asserted this was unreachable
	 * ("the pre-append guard keeps qlen + n inside cap"), which is the
	 * reason it survived: the invariant was written down instead of
	 * enforced.
	 *
	 * Under ASan the unfixed code reports a heap-buffer-overflow here.
	 * Plain, it silently corrupts whatever follows the 81-byte block. */
	{
		struct rec_send r;
		struct sockaddr_in to;
		unsigned char small[8], big[4096];
		struct clpush B;

		memset(&r, 0, sizeof r);
		memset(&to, 0, sizeof to);
		to.sin_family = AF_INET;
		to.sin_addr.s_addr = 0x0101a8c0;
		to.sin_port = 7100;
		memset(small, 0xA1, sizeof small);
		memset(big, 0xB2, sizeof big);

		/* hdr 9, flush_bytes 64, cap 8192: the first record allocates
		 * 9 + 64 + 8 = 81 bytes */
		SENDP = &B;
		clpush_init(&B, 4, 9, 8192, 64, 1000);
		clpush_append(&B, 0, 0, &to, small, sizeof small, 1000,
			recording_send_b, &r);
		/* qlen is 17 now, and 17 + 4096 <= 8192, so no flush fires and
		 * the memcpy writes 4096 bytes at offset 17 of an 81-byte
		 * buffer */
		clpush_append(&B, 0, 0, &to, big, sizeof big, 1000,
			recording_send_b, &r);
		chk(1, "H1 a small record then a large one did not overflow");
		clpush_fini(&B);
		SENDP = NULL;
	}

	/* --- S253: non-holder keys and the ack table ------------------- */
	{
		struct rec_send r;
		struct sockaddr_in to = A(7), other = A(8);
		struct clpush B;
		struct clpush_ack *a;
		uint32_t req, req2;
		unsigned int nhc_at_send = 0;

		memset(&r, 0, sizeof r);
		SENDP = &B;
		/* flush_bytes 80: the THIRD 30-byte record crosses it (9+90), and
		 * the group goes from inside the append that carried it */
		clpush_init(&B, 4, HDR, 8192, 80, 1000);
		clpush_append_nh(&B, 0, 0, &to, REC, 30, 1000,
			recording_send_b, &r, "c", 1, "k1", 2, 11);
		chk(B.g[0][0].nhcount == 1, "N1 a noted key waits in its group");
		clpush_append(&B, 0, 0, &to, REC, 30, 1000, recording_send_b, &r);
		chk(r.calls == 0, "N1 not sent below flush_bytes");
		/* the next noted append crosses flush_bytes: its key must be in
		 * the group that is sent, not the next one */
		{
			struct wgroup *g0 = &B.g[0][0];

			clpush_append_nh(&B, 0, 0, &to, REC, 30, 1000,
				recording_send_b, &r, "c", 1, "k2", 2, 12);
			chk(r.calls == 1 && r.last_count == 3,
				"N2 the crossing append sent 3 records");
			chk(g0->nhcount == 0 && g0->nhlen == 0,
				"N2 the sent group's keys went with it (reset)");
		}
		/* park by hand, as the send callback does */
		clpush_append_nh(&B, 0, 1, &to, REC, 10, 1000,
			recording_send_b, &r, "col", 3, "key", 3, 99);
		nhc_at_send = B.g[0][1].nhcount;
		req = clpush_ack_park(&B, &B.g[0][1], 5000);
		chk(nhc_at_send == 1 && req && (req & CLPUSH_ACK_TAG),
			"N3 a group with keys parks, id has the tag bit");
		chk(B.g[0][1].nh == NULL, "N3 the table owns the keys");
		clpush_sent(&B, &B.g[0][1]);
		chk(clpush_ack_take(&B, req, &other) == NULL,
			"N4 an ack from another peer takes nothing");
		chk(clpush_ack_take(&B, req ^ CLPUSH_ACKS, &to) == NULL,
			"N4 an id for the same slot, another lap, takes nothing");
		chk(clpush_ack_take(&B, req & ~CLPUSH_ACK_TAG, &to) == NULL,
			"N4 a migration id (no tag) is never a push ack");
		a = clpush_ack_take(&B, req, &to);
		chk(a && a->count == 1 && a->nhcount == 1 && a->nhlen == 11 + 6,
			"N5 the right ack takes the group and its keys");
		if (a) {
			chk(a->nh[0] == 3 && a->nh[1] == 0 && a->nh[2] == 3 &&
				a->nh[10] == 99 && memcmp(a->nh + 11, "colkey", 6) == 0,
				"N5 [collen][klen][ver][col][key]");
			chk(clpush_ack_take(&B, req, &to) == NULL,
				"N5 a duplicate ack while taken takes nothing");
			clpush_ack_done(&B, a);
		}
		chk(clpush_ack_take(&B, req, &to) == NULL,
			"N5 done: a late duplicate takes nothing");
		/* a group without keys never parks */
		clpush_append(&B, 0, 2, &to, REC, 10, 1000, recording_send_b, &r);
		chk(clpush_ack_park(&B, &B.g[0][2], 5000) == 0,
			"N6 no keys: req 0, fire-and-forget");
		clpush_sent(&B, &B.g[0][2]);
		/* expiry: parked at 5000, not due at 6999, due at 7000 */
		clpush_append_nh(&B, 0, 3, &to, REC, 10, 1000,
			recording_send_b, &r, "c", 1, "a", 1, 1);
		clpush_append_nh(&B, 0, 3, &to, REC, 10, 1000,
			recording_send_b, &r, "c", 1, "b", 1, 2);
		req2 = clpush_ack_park(&B, &B.g[0][3], 5000);
		clpush_sent(&B, &B.g[0][3]);
		chk(clpush_ack_expire(&B, 6999, 2000) == 0,
			"N7 not expired inside the wait");
		chk(clpush_ack_expire(&B, 7000, 2000) == 2,
			"N7 expired: both keys counted as kept");
		chk(clpush_ack_take(&B, req2, &to) == NULL,
			"N7 an ack after expiry takes nothing");
		/* a slot still busy when its index comes round refuses */
		clpush_append_nh(&B, 0, 3, &to, REC, 10, 1000,
			recording_send_b, &r, "c", 1, "x", 1, 3);
		req = clpush_ack_park(&B, &B.g[0][3], 8000);
		clpush_sent(&B, &B.g[0][3]);
		B.ack_seq += CLPUSH_ACKS - 1;      /* the next id lands on it */
		clpush_append_nh(&B, 0, 3, &to, REC, 10, 1000,
			recording_send_b, &r, "c", 1, "y", 1, 4);
		chk(req && clpush_ack_park(&B, &B.g[0][3], 8000) == 0,
			"N8 a busy slot refuses the park (sent unacked, kept)");
		chk(B.g[0][3].nhcount == 1 && B.g[0][3].nh != NULL,
			"N8 refused: the group still has its keys");
		clpush_sent(&B, &B.g[0][3]);
		chk(B.g[0][3].nhcount == 0, "N8 and a send clears them");
		/* a slot that changes hands forgets its keys with its records */
		clpush_append_nh(&B, 0, 3, &to, REC, 10, 1000,
			recording_send_b, &r, "c", 1, "z", 1, 5);
		clpush_append(&B, 0, 3, &other, REC, 10, 1000,
			recording_send_b, &r);
		chk(B.g[0][3].nhcount == 0 && B.g[0][3].qcount == 1,
			"N9 a new peer in the slot: the old keys are gone too");
		clpush_fini(&B);
		SENDP = NULL;
	}

	printf("clpushtest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
