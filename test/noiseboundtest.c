/* noiseboundtest.c - RV-2: the transport refuses at its OWN boundaries.
 *
 * cs_encrypt() had no `ptlen <= PC_NOISE_MAXPT` guard and returned
 * (int)clen; cs_decrypt() had no ceiling either; and the 64-bit nonce
 * counter had no policy for its last value.  Nothing reaches any of
 * them today - every caller chunks to PC_NOISE_MAXPT and the datagram
 * plane is frame-bounded - which is exactly why the boundary belonged
 * to the callers and not to the transport: the next caller inherits a
 * promise nobody wrote down.
 *
 * Asserted here, on the public transport calls:
 *  - a record of exactly PC_NOISE_MAXPT encrypts, decrypts and round-
 *    trips; one byte more is refused and the nonce does NOT advance;
 *  - a ciphertext over PC_NOISE_MAXMSG is refused before the AEAD sees
 *    it, and the nonce does not advance;
 *  - the nonce 2^64-1 is never used (Noise, section 5.1: reserved): the
 *    last usable one is 2^64-2, after which both directions refuse for
 *    good - the connection ends and a reconnect re-keys;
 *  - the no-key copy-through obeys the same ceiling.
 * Milliseconds. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sodium.h>
#include "../src/pc_noise.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static void keyed(struct pc_cipherstate *cs, uint64_t n)
{
	memset(cs, 0, sizeof *cs);
	memset(cs->k, 0x5a, sizeof cs->k);
	cs->n = n;
	cs->has_key = 1;
}

int main(void)
{
	/* big enough that the UNFIXED transport, which encrypts whatever it
	 * is handed, overruns nothing while this test watches it fail */
	size_t cap = (size_t)PC_NOISE_MAXMSG + 4096;
	uint8_t *pt = calloc(1, cap), *ct = calloc(1, cap), *back = calloc(1, cap);
	struct pc_cipherstate s, r;
	int cl, pl;

	if (!pt || !ct || !back)
		return 2;
	memset(pt, 0xa7, cap);

	/* ---- the largest record ---- */
	keyed(&s, 0);
	keyed(&r, 0);
	cl = pc_transport_encrypt(&s, pt, PC_NOISE_MAXPT, ct);
	CHK(cl == PC_NOISE_MAXMSG && s.n == 1,
		"a record of PC_NOISE_MAXPT (%d) encrypts to PC_NOISE_MAXMSG "
		"(%d): got %d, nonce %llu", PC_NOISE_MAXPT, PC_NOISE_MAXMSG, cl,
		(unsigned long long)s.n);
	pl = cl > 0 ? pc_transport_decrypt(&r, ct, (size_t)cl, back) : -1;
	CHK(pl == PC_NOISE_MAXPT && !memcmp(pt, back, PC_NOISE_MAXPT),
		"and round-trips (%d bytes back)", pl);

	/* ---- one byte more ---- */
	keyed(&s, 7);
	cl = pc_transport_encrypt(&s, pt, (size_t)PC_NOISE_MAXPT + 1, ct);
	CHK(cl < 0 && s.n == 7,
		"PC_NOISE_MAXPT + 1 is refused and the nonce does not advance "
		"(returned %d, nonce %llu)", cl, (unsigned long long)s.n);

	keyed(&r, 7);
	pl = pc_transport_decrypt(&r, ct, (size_t)PC_NOISE_MAXMSG + 1, back);
	CHK(pl < 0 && r.n == 7,
		"a ciphertext of PC_NOISE_MAXMSG + 1 is refused and the nonce "
		"does not advance (returned %d, nonce %llu)", pl,
		(unsigned long long)r.n);

	/* ---- the end of the nonce space ---- */
	keyed(&s, UINT64_MAX - 1);
	keyed(&r, UINT64_MAX - 1);
	cl = pc_transport_encrypt(&s, pt, 32, ct);
	pl = cl > 0 ? pc_transport_decrypt(&r, ct, (size_t)cl, back) : -1;
	CHK(cl == 48 && pl == 32 && s.n == UINT64_MAX && r.n == UINT64_MAX,
		"2^64-2 is the last usable nonce: it encrypts and decrypts "
		"(%d/%d)", cl, pl);
	cl = pc_transport_encrypt(&s, pt, 32, ct);
	CHK(cl < 0 && s.n == UINT64_MAX,
		"2^64-1 is never used to encrypt: refused, the counter stays "
		"(returned %d)", cl);
	{
		/* a VALID record under the reserved nonce, made with the AEAD
		 * directly - a bad tag would be refused for the wrong reason */
		uint8_t npub[12];
		unsigned long long clen = 0;

		memset(npub, 0, 4);
		memset(npub + 4, 0xff, 8);
		crypto_aead_chacha20poly1305_ietf_encrypt(ct, &clen, pt, 32, NULL,
			0, NULL, npub, r.k);
		pl = pc_transport_decrypt(&r, ct, (size_t)clen, back);
	}
	CHK(pl < 0 && r.n == UINT64_MAX,
		"2^64-1 is never used to decrypt, even for a record whose tag "
		"verifies under it: refused, the counter stays (returned %d)", pl);
	cl = pc_transport_encrypt(&s, pt, 32, ct);
	CHK(cl < 0, "and it stays refused - the connection is over, a "
		"reconnect re-keys");

	/* ---- the no-key copy-through ---- */
	memset(&s, 0, sizeof s);
	cl = pc_transport_encrypt(&s, pt, (size_t)PC_NOISE_MAXPT + 1, ct);
	CHK(cl < 0, "the no-key copy-through obeys the same ceiling "
		"(returned %d)", cl);
	cl = pc_transport_encrypt(&s, pt, 100, ct);
	CHK(cl == 100 && !memcmp(pt, ct, 100),
		"and still copies a record that fits (%d)", cl);

	printf("noiseboundtest: %s\n", fails ? "FAILED" : "passed");
	free(pt); free(ct); free(back);
	return fails ? 1 : 0;
}
