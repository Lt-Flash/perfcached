/*
 * clreplaytest.c - CH-1: a sealed cluster frame is taken once.
 *
 * The window over each sealing thread's stream, the incarnation rule, and
 * the header that carries them:
 *   A  in order, out of order inside the window, a duplicate, below it
 *   B  a jump past the window clears it; the next stream is its own
 *   C  a newer incarnation resets the stream; an older one is refused while
 *      the newer is heard, and taken once it has been quiet 60 s
 *   D  senders are told apart by host and port
 *   E  a sealed frame names its sender and position; two threads are two
 *      streams; an exiting thread's stream and sequence go on in the next
 *
 * Build: cc -o clreplaytest test/clreplaytest.c src/clreplay.o src/clwire.o -lsodium -lpthread
 */
#include <pthread.h>
#include <sodium.h>
#include <stdio.h>
#include <string.h>

#include "../src/clreplay.h"

static int pass, fail;
static void chk(int cond, const char *what)
{
	if (cond) { pass++; return; }
	fail++;
	printf("  FAIL %s\n", what);
}

static int adm(uint32_t ip, uint16_t port, uint64_t inc, uint16_t st,
		uint64_t seq, long long now)
{
	struct clwire_id id = { inc, seq, ip, port, st };

	return clreplay_admit(&id, now);
}

struct seal_try { const uint8_t *psk; struct clwire_id id[3]; };
static void *seal_thread(void *arg)
{
	struct seal_try *t = arg;
	unsigned char ct[HDR_LEN + 16 + 8];
	int i;

	for (i = 0; i < 3; i++) {
		clwire_seal(t->psk, (const unsigned char *)"x", 1, ct);
		clwire_id_read(ct, &t->id[i]);
	}
	return NULL;
}

int main(void)
{
	static const uint8_t k[32] = { 7 };
	unsigned long long c[CLREPLAY_NCOUNT];
	uint32_t a = 0x0100007f, b = 0x0200007f;
	int i, ok;

	/* ---- A. the window ---------------------------------------------- */
	chk(adm(a, 1, 100, 1, 1, 0) == CLREPLAY_OK, "A1 the first frame of a stream is taken");
	chk(adm(a, 1, 100, 1, 1, 0) == CLREPLAY_DUP, "A2 the same sequence again is refused");
	chk(adm(a, 1, 100, 1, 5, 0) == CLREPLAY_OK && adm(a, 1, 100, 1, 3, 0) == CLREPLAY_OK,
		"A3 a later one, then an earlier one not yet seen, are both taken");
	chk(adm(a, 1, 100, 1, 3, 0) == CLREPLAY_DUP, "A4 and the earlier one only once");
	chk(adm(a, 1, 100, 1, 5 + CLREPLAY_WIN - 1, 0) == CLREPLAY_OK &&
		adm(a, 1, 100, 1, 4, 0) == CLREPLAY_OLD,
		"A5 a sequence older than the window is refused as old");
	chk(adm(a, 1, 100, 1, 5 + CLREPLAY_WIN - 2, 0) == CLREPLAY_OK,
		"A6 one inside the window, unseen, is taken");

	/* ---- B. jumps and streams --------------------------------------- */
	chk(adm(a, 1, 100, 1, 10 * CLREPLAY_WIN, 0) == CLREPLAY_OK &&
		adm(a, 1, 100, 1, 10 * CLREPLAY_WIN - 1, 0) == CLREPLAY_OK,
		"B1 a jump past the window leaves nothing below it marked");
	chk(adm(a, 1, 100, 2, 1, 0) == CLREPLAY_OK, "B2 sequence 1 of another stream is its own");
	for (ok = 1, i = 2; i < 200; i++)
		ok &= adm(a, 1, 100, 2, (uint64_t)i, 0) == CLREPLAY_OK;
	chk(ok, "B3 a stream in order is taken whole");

	/* ---- C. incarnations ------------------------------------------- */
	chk(adm(a, 1, 200, 1, 1, 1000) == CLREPLAY_OK,
		"C1 a newer incarnation starts its stream again at 1");
	chk(adm(a, 1, 100, 1, 10 * CLREPLAY_WIN + 1, 1000) == CLREPLAY_INCARN,
		"C2 the old process's frame is refused while the new one is heard");
	chk(adm(a, 1, 100, 2, 500, 1000) == CLREPLAY_INCARN,
		"C3 on every stream, not only the one the new process has used");
	chk(adm(a, 1, 50, 1, 1, 1000 + CLREPLAY_STALE_MS) == CLREPLAY_OK,
		"C4 a lower incarnation once the newer has been quiet 60 s - a clock stepped back");
	chk(adm(a, 1, 200, 1, 2, 1000 + CLREPLAY_STALE_MS) == CLREPLAY_OK,
		"C5 and the higher one, heard again, takes over");

	/* ---- D. senders ------------------------------------------------- */
	chk(adm(b, 1, 100, 1, 1, 0) == CLREPLAY_OK && adm(a, 2, 100, 1, 1, 0) == CLREPLAY_OK,
		"D1 another host, or another port on this one, is another sender");
	chk(adm(a, 3, 1, CLREPLAY_STREAMS, 1, 0) == CLREPLAY_FULL &&
		adm(a, 3, 1, CLREPLAY_STREAMS, 1, 0) == CLREPLAY_FULL,
		"D2 a stream past the table is let through, counted");
	clreplay_counts(c);
	chk(c[CLREPLAY_DUP] == 2 && c[CLREPLAY_OLD] == 1 && c[CLREPLAY_INCARN] == 2 &&
		c[CLREPLAY_FULL] == 2, "D3 each refusal is counted by its reason");

	/* ---- E. the header ---------------------------------------------- */
	if (sodium_init() < 0) {
		printf("clreplaytest: libsodium would not initialise\n");
		return 1;
	}
	{
		unsigned char ct[HDR_LEN + 16 + 8], pt[8];
		unsigned long long pl = 0;
		struct clwire_id i1, i2;
		struct seal_try t1, t2;
		pthread_t th;

		memset(&t1, 0, sizeof t1);
		memset(&t2, 0, sizeof t2);
		t1.psk = t2.psk = k;
		clwire_set_self(a, 17001);
		clwire_seal(k, (const unsigned char *)"x", 1, ct);
		clwire_id_read(ct, &i1);
		clwire_seal(k, (const unsigned char *)"x", 1, ct);
		clwire_id_read(ct, &i2);
		chk(i1.ip == a && i1.port == 17001 && i1.incarn > 1700000000000ULL &&
			i1.stream && i2.stream == i1.stream && i2.incarn == i1.incarn &&
			i2.seq == i1.seq + 1,
			"E1 a frame names its host, port and incarnation; one thread's seals count up");
		chk(clwire_open(k, ct, HDR_LEN + 1 + CLWIRE_TAG, pt, &pl) == 0, "E2 it opens");
		ct[CLWIRE_ID_OFF + 14] ^= 1;
		chk(clwire_open(k, ct, HDR_LEN + 1 + CLWIRE_TAG, pt, &pl) != 0,
			"E3 a sequence altered in flight fails the tag");
		pthread_create(&th, NULL, seal_thread, &t1);
		pthread_join(th, NULL);
		chk(t1.id[0].stream != i1.stream && t1.id[0].seq == 1 && t1.id[2].seq == 3,
			"E4 another thread is another stream, counting from 1");
		pthread_create(&th, NULL, seal_thread, &t2);
		pthread_join(th, NULL);
		chk(t2.id[0].stream == t1.id[0].stream && t2.id[0].seq == 4,
			"E5 a thread after it takes its stream and goes on from its sequence");
	}

	printf("clreplaytest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
