/*
 * clwiretest.c - the sealed datagram and the beat frames (M6).
 *
 * Every cluster datagram is sealed under the cluster PSK, and the only
 * thing standing between a fleet and a forged frame is that open()
 * refuses everything it should:
 *   - a flipped ciphertext byte, which the Poly1305 tag must catch
 *   - a flipped nonce, which is associated data and must fail the same way
 *   - the wrong key entirely, which is what a node from another fleet has
 *   - a version byte from a build that framed datagrams differently -
 *     cluster.h is explicit that these are DROPPED, not half-read
 *
 * The beat frames are here too because they cross threads under a
 * seqlock: the peer thread writes, the watchdog re-seals the last pair
 * when the peer thread has gone quiet.
 *
 * No socket: sealing is separate from sending, which is the whole point
 * of the split.
 *
 * Build: cc -o clwiretest test/clwiretest.c src/clwire.o -lsodium
 */
#include <sodium.h>
#include <stdio.h>
#include <string.h>

#include "../src/clwire.h"

/* Seal with a header of our choosing, so a frame can be cryptographically
 * VALID and still carry a wrong magic or version.  Flipping those bytes
 * after sealing proves nothing: the header is associated data, so the tag
 * refuses it anyway and the explicit checks are never exercised. */
static size_t seal_with_hdr(const uint8_t *psk, unsigned char magic,
		unsigned char ver, const unsigned char *pt, size_t n,
		unsigned char *out)
{
	unsigned long long clen = 0;

	out[0] = magic;
	out[1] = ver;
	clwire_nonce(out + 2);
	crypto_aead_chacha20poly1305_ietf_encrypt(out + HDR_LEN, &clen,
		pt, n, out, HDR_LEN, NULL, out + 2, psk);
	return HDR_LEN + (size_t)clen;
}

static int pass, fail;
static void chk(int cond, const char *what)
{
	if (cond) { pass++; return; }
	fail++;
	printf("  FAIL %s\n", what);
}

int main(void)
{
	static const uint8_t k1[32] = { 1, 2, 3, 4 };
	static const uint8_t k2[32] = { 1, 2, 3, 5 };   /* another fleet */
	const char *msg = "the peer plane speaks its own dialect";
	unsigned char sealed[HDR_LEN + 256 + CLWIRE_TAG], pt[256], big[MAX_DGRAM + 8];
	unsigned char n1[12], n2[12], seen[64][12];
	unsigned long long ptlen = 0;
	size_t n = strlen(msg), len, len2, i, j, dup = 0;
	struct clwire W;

	if (sodium_init() < 0) {
		printf("clwiretest: libsodium would not initialise\n");
		return 2;
	}
	printf("=== clwiretest ===\n");

	/* ---- A. the round trip -------------------------------------------- */
	len = clwire_seal(k1, (const unsigned char *)msg, n, sealed);
	chk(len == HDR_LEN + n + CLWIRE_TAG,
		"A1 a sealed frame is header + plaintext + tag");
	chk(sealed[0] == 0xA1 && sealed[1] == 3,
		"A2 it carries the magic and the version in clear");
	chk(clwire_open(k1, sealed, len, pt, &ptlen) == 0,
		"A3 the same key opens it");
	chk(ptlen == n && !memcmp(pt, msg, n),
		"A3 recovering the plaintext exactly");

	/* ---- B. what must be refused --------------------------------------- */
	sealed[HDR_LEN + 2] ^= 0x01;
	chk(clwire_open(k1, sealed, len, pt, &ptlen) != 0,
		"B1 one flipped ciphertext byte is refused by the tag");
	sealed[HDR_LEN + 2] ^= 0x01;
	sealed[5] ^= 0x01;                          /* inside the nonce */
	chk(clwire_open(k1, sealed, len, pt, &ptlen) != 0,
		"B2 a flipped nonce is refused - the header is associated data");
	sealed[5] ^= 0x01;
	chk(clwire_open(k2, sealed, len, pt, &ptlen) != 0,
		"B3 another fleet's key is refused");
	chk(clwire_open(k1, sealed, HDR_LEN + CLWIRE_TAG - 1, pt, &ptlen) != 0,
		"B4 a frame shorter than header + tag is refused");
	{
		unsigned char alt[HDR_LEN + 256 + CLWIRE_TAG];
		size_t altlen;

		altlen = seal_with_hdr(k1, 0xA1, 2, (const unsigned char *)msg,
			n, alt);
		chk(clwire_open(k1, alt, altlen, pt, &ptlen) != 0,
			"B5 a VALIDLY sealed frame of another version is DROPPED - "
			"the version check, not the tag, is what refuses it");
		altlen = seal_with_hdr(k1, 0xA2, 3, (const unsigned char *)msg,
			n, alt);
		chk(clwire_open(k1, alt, altlen, pt, &ptlen) != 0,
			"B6 and so is one with another magic");
		altlen = seal_with_hdr(k1, 0xA1, 3, (const unsigned char *)msg,
			n, alt);
		chk(clwire_open(k1, alt, altlen, pt, &ptlen) == 0,
			"B7 the same construction with the right header opens - "
			"so B5 and B6 are not passing for want of a valid frame");
	}
	chk(clwire_open(k1, sealed, len, pt, &ptlen) == 0,
		"B8 and the original frame is undamaged");

	/* ---- C. the nonce --------------------------------------------------- */
	for (i = 0; i < 64; i++)
		clwire_nonce(seen[i]);
	for (i = 0; i < 64; i++)
		for (j = i + 1; j < 64; j++)
			if (!memcmp(seen[i], seen[j], 12))
				dup++;
	chk(dup == 0, "C1 sixty-four nonces from one thread are all distinct");
	clwire_nonce(n1);
	clwire_nonce(n2);
	chk(memcmp(n1, n2, 12) != 0, "C1 consecutive nonces differ");
	len2 = clwire_seal(k1, (const unsigned char *)msg, n, sealed);
	chk(len2 == len && memcmp(sealed + 2, seen[0], 12) != 0,
		"C2 sealing the same plaintext twice does not repeat a nonce");

	/* ---- D. the size ceiling -------------------------------------------- */
	memset(big, 0x5A, sizeof big);
	chk(clwire_seal(k1, big, MAX_DGRAM + 1, sealed) == 0,
		"D1 a plaintext past MAX_DGRAM is refused rather than truncated");

	/* ---- E. the beat frames --------------------------------------------- */
	{
		unsigned char a[70], ma[40], frame[70];
		size_t alen = 99, mlen = 99;

		memset(&W, 0, sizeof W);
		clwire_beat_take(&W, a, &alen, ma, &mlen);
		chk(alen == 0 && mlen == 0, "E1 nothing is published before the first beat");
		memset(frame, 0xC3, sizeof frame);
		clwire_beat_store(&W, CLWIRE_ALIVE, frame, 70);
		clwire_beat_take(&W, a, &alen, ma, &mlen);
		chk(alen == 70 && !memcmp(a, frame, 70) && mlen == 0,
			"E2 the alive frame round-trips and the master frame stays empty");
		clwire_beat_store(&W, CLWIRE_MALIVE, frame, 39);
		clwire_beat_take(&W, a, &alen, ma, &mlen);
		chk(alen == 70 && mlen == 39,
			"E3 both frames are readable at once");
		clwire_beat_store(&W, CLWIRE_MALIVE, NULL, 0);
		clwire_beat_take(&W, a, &alen, ma, &mlen);
		chk(mlen == 0, "E4 a zero-length store clears a frame");
		clwire_beat_store(&W, CLWIRE_MALIVE, frame, 70);   /* past its slot */
		clwire_beat_take(&W, a, &alen, ma, &mlen);
		chk(mlen == 0,
			"E5 a frame larger than its slot is refused, not overrun");
		chk((W.seq & 1u) == 0, "E6 the seqlock is left even, never mid-write");
		clwire_mark_sent(&W, 12345);
		chk(clwire_sent_ms(&W) == 12345, "E7 the emission stamp round-trips");
	}

	printf("clwiretest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
