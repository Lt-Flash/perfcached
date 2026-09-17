/*
 * psqueuetest.c - a worker's pub/sub queue is bounded, and its drain is.
 *
 * The PS11 rig sent 200k relays/s at one subscriber: the owning worker's
 * queue grew without limit, and the drain emptied all of it in one pass,
 * so the node took 12.9 s to notice SIGTERM.  This links the real
 * src/pubsub.o against stubs (as bench/psengine.c does) and checks, on one
 * thread, with worker 2 never draining until told to:
 *
 *   - at a cap, the queue's bytes stay under it and the rest are dropped
 *     and counted: every publish is either queued or dropped;
 *   - with no cap, nothing is dropped;
 *   - one drain delivers at most one batch and re-arms the worker's
 *     eventfd while something is left, and no more once it is empty;
 *   - the byte gauge returns to zero.
 */
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "pubsub.h"
#include "quiesce.h"

#define BATCH 1024                         /* PS_DRAIN_BATCH */

static int my_worker = -1;
static unsigned long long pushed;
static int fails, checks;

int pc_worker_id(void) { return my_worker; }
int pc_cluster_pubsub_relay(const char *c, size_t cl, const char *d, size_t dl)
{
	(void)c; (void)cl; (void)d; (void)dl;
	return 0;
}
int pc_conn_pubsub_push(void *conn, const char *pat, size_t plen,
		const char *chan, size_t clen, const char *data, size_t dlen,
		const char *f, size_t flen)
{
	(void)conn; (void)pat; (void)plen; (void)chan; (void)clen;
	(void)data; (void)dlen; (void)f; (void)flen;
	pushed++;
	return 0;
}
int pc_conn_kill(void *conn, const char *why) { (void)conn; (void)why; return 1; }
void pc_cluster_pubsub_interest_add(uint64_t v, int pattern, const char *name,
		size_t nlen, uint32_t h1, uint32_t h2)
{
	(void)v; (void)pattern; (void)name; (void)nlen; (void)h1; (void)h2;
}
void pc_conn_subs_note(void *conn, int n) { (void)conn; (void)n; }

#define CHECK(cond, ...) do { \
	checks++; \
	if (!(cond)) { \
		fails++; \
		printf("FAIL %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

static void stats(struct pc_pubsub_stats *s)
{
	memset(s, 0, sizeof *s);
	pc_pubsub_stats(s);
}

/* the eventfd was written since it was last read */
static int woken(int efd)
{
	uint64_t v;

	return read(efd, &v, sizeof v) == (ssize_t)sizeof v;
}

static void publish(int n)
{
	char payload[100];
	int i;

	memset(payload, 'x', sizeof payload);
	my_worker = 1;
	for (i = 0; i < n; i++)
		pc_pubsub_publish("q", 1, payload, sizeof payload, 0);
}

int main(void)
{
	const size_t cap = 64 << 10;
	struct pc_pubsub_stats s;
	unsigned long long queued0, dropped0, before, total;
	char conn;
	int efd1 = eventfd(0, EFD_NONBLOCK), efd2 = eventfd(0, EFD_NONBLOCK);
	int drains;

	pc_pubsub_init(4);
	pc_pubsub_worker_register(1, efd1);
	pc_pubsub_worker_register(2, efd2);
	pc_qs_attach();
	pc_qs_enter();
	my_worker = 2;
	CHECK(pc_pubsub_subscribe(&conn, 2, "q", 1, 0) == 1, "subscribe");

	/* 1. a cap */
	pc_pubsub_set_queue_cap(cap);
	publish(20000);
	stats(&s);
	CHECK(s.queue_bytes <= cap, "queue bytes %llu over the cap %zu",
		s.queue_bytes, cap);
	CHECK(s.queue_bytes > cap / 2, "queue bytes %llu: the queue never filled",
		s.queue_bytes);
	CHECK(s.queue_dropped > 0, "nothing dropped at a %zu B cap", cap);
	CHECK(s.queued + s.queue_dropped == 20000,
		"queued %llu + dropped %llu != 20000 published", s.queued,
		s.queue_dropped);
	CHECK(woken(efd2), "the first message did not wake worker 2");
	CHECK(pushed == 0, "worker 1 delivered %llu for worker 2", pushed);
	queued0 = s.queued;
	dropped0 = s.queue_dropped;

	/* 2. no cap */
	pc_pubsub_set_queue_cap(0);
	publish(3000);
	stats(&s);
	CHECK(s.queue_dropped == dropped0, "dropped %llu with no cap",
		s.queue_dropped - dropped0);
	CHECK(s.queued == queued0 + 3000, "queued %llu, expected %llu",
		s.queued, queued0 + 3000);
	total = s.queued;

	/* 3. bounded drains */
	my_worker = 2;
	drains = 0;
	for (;;) {
		before = pushed;
		pc_pubsub_drain(2);
		drains++;
		CHECK(pushed - before <= BATCH, "drain %d delivered %llu, over %d",
			drains, pushed - before, BATCH);
		if (pushed == total) {
			CHECK(pushed - before > 0, "last drain delivered nothing");
			break;
		}
		CHECK(pushed - before == BATCH, "drain %d delivered %llu with more queued",
			drains, pushed - before);
		CHECK(woken(efd2), "drain %d left %llu queued and did not re-wake",
			drains, total - pushed);
		if (drains > 100 || pushed - before == 0)
			break;
	}
	CHECK(pushed == total, "delivered %llu of %llu", pushed, total);
	CHECK(drains == (int)((total + BATCH - 1) / BATCH), "%d drains for %llu",
		drains, total);
	CHECK(!woken(efd2), "the drain that emptied the queue woke the worker again");
	pc_pubsub_drain(2);
	CHECK(!woken(efd2), "a drain of an empty queue woke the worker");
	stats(&s);
	CHECK(s.queue_bytes == 0, "queue bytes %llu after it emptied", s.queue_bytes);

	/* 4. an emptied queue wakes on its next message */
	publish(1);
	CHECK(woken(efd2), "a message into an emptied queue did not wake");
	my_worker = 2;
	pc_pubsub_drain(2);
	CHECK(pushed == total + 1, "the last message was not delivered");

	pc_qs_exit();
	printf("psqueuetest: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
