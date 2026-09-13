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
 *
 * No socket: a group leaves through a callback that records it, which
 * is exactly how cluster.c passes seal_send in production.
 *
 * Build: cc -o clpushtest test/clpushtest.c src/clpush.o -lpthread
 */
#include <pthread.h>
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

	printf("clpushtest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
