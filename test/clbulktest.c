/*
 * clbulktest.c - the bulk plane's framing and batch handoff (M7).
 *
 * The bulk plane carries records too big for a datagram over a Noise
 * session, and every case here is a way that stream could go wrong
 * without a fleet to notice:
 *   - a chunk header claiming a length the stream cannot honour, which
 *     is how a truncated batch must be refused: by exact length, before
 *     any of it reaches the store
 *   - a peer that stops mid-stream, which has to read as failure rather
 *     than as a short but complete batch
 *   - a stash too small for the chunk offered, which would otherwise
 *     overrun the receiver's buffer
 *   - a batch handed over while the bulk thread still holds the last
 *     one, which must be refused so the caller can count the loss
 *
 * The handshake is real and runs over a socketpair, so the round trip
 * is the production one; nothing here needs a second daemon.
 *
 * Build: cc -o clbulktest test/clbulktest.c src/clbulk.o src/pc_noise.o \
 *              src/compat/compat.o -lsodium
 */
#include <sodium.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include "../src/clbulk.h"

/* write eight bytes in two parts, so the reader's first read returns
 * four and the retry loop is actually exercised - a socketpair never
 * gives a short read at these sizes otherwise, which is why the
 * mutation that removed the loop went unnoticed. */
struct slow_arg { int fd; };
static void *slow_writer(void *arg)
{
	struct slow_arg *a = arg;
	struct timespec ts = { 0, 60L * 1000000 };

	if (write(a->fd, "0123", 4) != 4)
		return NULL;
	nanosleep(&ts, NULL);
	if (write(a->fd, "4567", 4) != 4)
		return NULL;
	return NULL;
}

static int pass, fail;
static void chk(int cond, const char *what)
{
	if (cond) { pass++; return; }
	fail++;
	printf("  FAIL %s\n", what);
}

static uint8_t PSK[PC_NOISE_KEYLEN];

/* a real handshake over a socketpair: sv[0] initiator, sv[1] responder */
static int pair_up(int sv[2], struct pc_cipherstate *is, struct pc_cipherstate *ir,
		struct pc_cipherstate *rs, struct pc_cipherstate *rr)
{
	struct pc_handshake hi, hr;
	uint8_t m1[PC_NOISE_MAXMSG], m2[PC_NOISE_MAXMSG], pay[16];
	uint8_t prologue[1] = { PC_PRIN_CLUSTER }, ver = 1;
	size_t l1 = 0, l2 = 0, pl = 0;

	{
		/* so a mutant that waits for bytes which never arrive fails
		 * instead of hanging the harness */
		struct timeval tv = { 2, 0 };

		if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
			return -1;
		setsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
		setsockopt(sv[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	}
	pc_hs_init_initiator(&hi, prologue, 1);
	if (pc_hs_write_msg1(&hi, PSK, &ver, 1, m1, &l1) != 0)
		return -1;
	pc_hs_init_responder(&hr, prologue, 1);
	if (pc_hs_read_msg1(&hr, PSK, m1, l1, pay, &pl) != 0)
		return -1;
	if (pc_hs_write_msg2(&hr, NULL, 0, m2, &l2, rs, rr) != 0)
		return -1;
	if (pc_hs_read_msg2(&hi, m2, l2, NULL, &pl, is, ir) != 0)
		return -1;
	return 0;
}

int main(void)
{
	int sv[2];
	struct pc_cipherstate is, ir, rs, rr;
	unsigned char stash[PC_NOISE_MAXMSG], out[80000];
	unsigned char *big;
	size_t slen = 0, i;
	struct clbulk B;
	struct sockaddr_in to, got_to;

	/* unbuffered: a mutant that aborts must still show which assertion
	 * it broke.  abort() discards buffered stdout, and three mutations
	 * were reported as bare exit 134 because of it. */
	setvbuf(stdout, NULL, _IONBF, 0);
	if (sodium_init() < 0) {
		printf("clbulktest: libsodium would not initialise\n");
		return 2;
	}
	memset(PSK, 0x5C, sizeof PSK);
	printf("=== clbulktest ===\n");

	/* ---- A. the round trip ------------------------------------------- */
	chk(pair_up(sv, &is, &ir, &rs, &rr) == 0, "A1 the cluster handshake completes");
	chk(clbulk_send(sv[0], &is, (const unsigned char *)"twenty-four bytes here!!", 24) == 0,
		"A2 a short stream goes out");
	chk(clbulk_recv_exact(sv[1], &rr, stash, &slen, sizeof stash, out, 24) == 0 &&
		!memcmp(out, "twenty-four bytes here!!", 24),
		"A2 and arrives byte for byte");

	/* the remainder is carried in the stash between calls */
	chk(clbulk_send(sv[0], &is, (const unsigned char *)"0123456789", 10) == 0, "A3 send ten");
	chk(clbulk_recv_exact(sv[1], &rr, stash, &slen, sizeof stash, out, 4) == 0 &&
		!memcmp(out, "0123", 4), "A3 take four");
	chk(clbulk_recv_exact(sv[1], &rr, stash, &slen, sizeof stash, out, 6) == 0 &&
		!memcmp(out, "456789", 6),
		"A3 and the remaining six come from the stash, not the wire");

	/* a payload past one Noise message is split into chunks */
	big = malloc(70000);
	for (i = 0; i < 70000; i++)
		big[i] = (unsigned char)(i * 7);
	chk(clbulk_send(sv[0], &is, big, 70000) == 0,
		"A4 a payload larger than one Noise message is chunked");
	chk(clbulk_recv_exact(sv[1], &rr, stash, &slen, sizeof stash, out, 70000) == 0 &&
		!memcmp(out, big, 70000), "A4 and reassembles exactly");
	free(big);
	close(sv[0]); close(sv[1]);

	/* ---- B. what the stream must refuse ------------------------------- */
	slen = 0;
	chk(pair_up(sv, &is, &ir, &rs, &rr) == 0, "B0 a fresh session");
	{
		unsigned char hdr[2];

		/* the four bytes ARE supplied, so the refusal cannot come
		 * from a read timing out - which is what made the first
		 * version of this case pass with the length check removed */
		hdr[0] = 4; hdr[1] = 0;            /* shorter than the tag */
		chk(write(sv[0], hdr, 2) == 2, "B1 a chunk claiming four bytes");
		chk(write(sv[0], "junk", 4) == 4, "B1 and supplying them");
		chk(clbulk_recv_exact(sv[1], &rr, stash, &slen, sizeof stash,
			out, 8) != 0,
			"B1 is refused - a chunk shorter than its tag cannot "
			"carry a record");
	}
	close(sv[0]); close(sv[1]);

	slen = 0;
	chk(pair_up(sv, &is, &ir, &rs, &rr) == 0, "B2 a fresh session");
	chk(clbulk_send(sv[0], &is, (const unsigned char *)"short", 5) == 0, "B2 five bytes sent");
	close(sv[0]);                              /* the peer stops mid-stream */
	chk(clbulk_recv_exact(sv[1], &rr, stash, &slen, sizeof stash, out, 64) != 0,
		"B2 a stream that ends early fails, it does not return short");
	close(sv[1]);

	slen = 0;
	chk(pair_up(sv, &is, &ir, &rs, &rr) == 0, "B3 a fresh session");
	chk(clbulk_send(sv[0], &is, (const unsigned char *)"0123456789", 10) == 0, "B3 ten sent");
	chk(clbulk_recv_exact(sv[1], &rr, stash, &slen, 4, out, 10) != 0,
		"B3 a chunk that would overrun the stash is refused, not written");
	close(sv[0]); close(sv[1]);

	/* ---- B4. a partial read is retried, not reported as complete --- */
	{
		int pv[2];
		pthread_t t;
		struct slow_arg sa;
		unsigned char eight[9];

		chk(socketpair(AF_UNIX, SOCK_STREAM, 0, pv) == 0, "B4 a pair");
		sa.fd = pv[0];
		memset(eight, 0, sizeof eight);
		pthread_create(&t, NULL, slow_writer, &sa);
		chk(clbulk_io(pv[1], 0, eight, 8) == 0, "B4 eight bytes arrive");
		chk(!memcmp(eight, "01234567", 8),
			"B4 all eight, across two reads - the retry loop");
		pthread_join(t, NULL);
		close(pv[0]); close(pv[1]);
	}

	/* ---- C. the batch handoff ------------------------------------------ */
	memset(&to, 0, sizeof to);
	to.sin_family = AF_INET;
	to.sin_port = htons(6479);
	to.sin_addr.s_addr = htonl(0x0A160001u);
	clbulk_init(&B);
	{
		unsigned char *b1 = malloc(16), *b2 = malloc(16), *taken;
		size_t tlen; unsigned int trecs;

		chk(!clbulk_pending(&B), "C1 nothing is queued to begin with");
		chk(clbulk_offer(&B, b1, 16, 3, &to) == 1, "C2 the first batch is accepted");
		chk(clbulk_pending(&B), "C2 and reads as pending");
		{
			int q2 = clbulk_offer(&B, b2, 16, 9, &to);

			chk(q2 == 0,
				"C3 a second is REFUSED while the first is in "
				"flight - the caller keeps its buffer and counts "
				"the loss");
			if (!q2)
				free(b2);          /* still ours only if refused */
		}
		chk(clbulk_take(&B, &taken, &tlen, &trecs, &got_to) == 1 &&
			taken == b1 && tlen == 16 && trecs == 3 &&
			got_to.sin_addr.s_addr == to.sin_addr.s_addr,
			"C4 the bulk thread takes exactly what was offered");
		free(taken);
		clbulk_done(&B);
		chk(!clbulk_pending(&B), "C5 and the queue is free again");
		chk(clbulk_offer(&B, malloc(8), 8, 1, &to) == 1,
			"C5 so the next batch is accepted");
		chk(clbulk_take(&B, &taken, &tlen, &trecs, &got_to) == 1, "C5 and taken");
		free(taken);
		clbulk_done(&B);
	}
	chk(clbulk_lock(&B) == &B.mx,
		"C6 the lock is lent out by identity - clboot borrows this one");
	clbulk_fini(&B);

	printf("clbulktest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
