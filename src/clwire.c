/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clwire.c - the sealed datagram and the beat frames (M6).  See clwire.h.
 *
 * The bodies came out of seal_send(), open_dgram(), nonce_next() and
 * beat_store() unchanged.  What differs: the key arrives as an
 * argument, sealing no longer sends, and the two beat frames are named
 * slots in a struct instead of file-static arrays passed by name.
 */
#include <string.h>

#include <sodium.h>

#include "clwire.h"

#define PC_CL_MAGIC   0xA1
#define PC_CL_VER     5                /* = cluster.h's; S302: the cluster id */

/* S105: the 12-byte datagram nonce.  randombytes_buf is one getrandom(2)
 * per call (1.05 us measured); each thread instead keeps a ChaCha20
 * keystream under a key the OS gave it once and takes twelve bytes per
 * datagram.  The values are what a random 256-bit key produces, so
 * they are as unique as random nonces - the 96-bit collision bound is
 * unchanged - and the receiver needs nothing. */
void clwire_nonce(unsigned char out[12])
{
	static __thread unsigned char key[32], ks[4096], ctr[12];
	static __thread size_t off = sizeof ks;
	static __thread int have_key;

	if (!have_key) {
		randombytes_buf(key, sizeof key);
		randombytes_buf(ctr, sizeof ctr);
		have_key = 1;
	}
	if (off + 12 > sizeof ks) {
		int i;

		for (i = 0; i < 12 && ++ctr[i] == 0; i++)
			;                          /* the block counter, LE */
		crypto_stream_chacha20_ietf(ks, sizeof ks, ctr, key);
		off = 0;
	}
	memcpy(out, ks + off, 12);
	off += 12;
}

/* S302: the id is written bytes-first and published by the state's
 * release store; a sealer acquires the state, then reads bytes that no
 * longer change.  A clear (the provisional id given up on a yield) can
 * race a seal into one datagram that matches nobody - one dropped beat. */
static unsigned char cw_cid[CLWIRE_CID_LEN];
static int cw_state;

void clwire_set_cid(const unsigned char *cid, int state)
{
	if (!cid || state == CLWIRE_CID_NONE) {
		__atomic_store_n(&cw_state, CLWIRE_CID_NONE, __ATOMIC_RELEASE);
		memset(cw_cid, 0, sizeof cw_cid);
		return;
	}
	memcpy(cw_cid, cid, sizeof cw_cid);
	__atomic_store_n(&cw_state, state, __ATOMIC_RELEASE);
}

int clwire_cid(unsigned char out[CLWIRE_CID_LEN])
{
	int st = __atomic_load_n(&cw_state, __ATOMIC_ACQUIRE);

	if (st == CLWIRE_CID_NONE)
		memset(out, 0, CLWIRE_CID_LEN);
	else
		memcpy(out, cw_cid, CLWIRE_CID_LEN);
	return st;
}

size_t clwire_seal(const uint8_t *psk, const unsigned char *pt, size_t n,
		unsigned char *out)
{
	unsigned long long clen = 0;
	int st;

	if (n > MAX_DGRAM)
		return 0;
	out[0] = PC_CL_MAGIC;
	out[1] = PC_CL_VER;
	st = clwire_cid(CLWIRE_HDR_CID(out));
	CLWIRE_HDR_FLAGS(out) = st == CLWIRE_CID_PROVISIONAL ?
		CLWIRE_F_PROVISIONAL : 0;
	clwire_nonce(out + 19);
	crypto_aead_chacha20poly1305_ietf_encrypt(out + HDR_LEN, &clen,
		pt, n, out, HDR_LEN, NULL, out + 19, psk);
	return HDR_LEN + (size_t)clen;
}

int clwire_open(const uint8_t *psk, const unsigned char *buf, size_t n,
		unsigned char *pt, unsigned long long *ptlen)
{
	if (n < HDR_LEN + CLWIRE_TAG || buf[0] != PC_CL_MAGIC ||
	        buf[1] != PC_CL_VER)
		return -1;
	if (crypto_aead_chacha20poly1305_ietf_decrypt(pt, ptlen, NULL,
	        buf + HDR_LEN, n - HDR_LEN, buf, HDR_LEN, buf + 19,
	        psk) != 0)
		return -1;
	return 0;
}

/* peer thread only (single writer): publish a frame for the watchdog */
void clwire_beat_store(struct clwire *w, int which, const unsigned char *src,
		size_t n)
{
	unsigned char *dst = which == CLWIRE_MALIVE ? w->malive : w->alive;
	size_t cap = which == CLWIRE_MALIVE ? sizeof w->malive : sizeof w->alive;
	size_t *dlen = which == CLWIRE_MALIVE ? &w->malive_len : &w->alive_len;
	unsigned s = __atomic_load_n(&w->seq, __ATOMIC_RELAXED);

	if (n > cap)
		return;
	__atomic_store_n(&w->seq, s + 1, __ATOMIC_RELEASE);
	if (n)
		memcpy(dst, src, n);
	*dlen = n;
	__atomic_store_n(&w->seq, s + 2, __ATOMIC_RELEASE);
}

void clwire_beat_take(struct clwire *w, unsigned char *a, size_t *alen,
		unsigned char *ma, size_t *mlen)
{
	unsigned s0;

	for (;;) {
		s0 = __atomic_load_n(&w->seq, __ATOMIC_ACQUIRE);
		if (s0 & 1)
			continue;
		*alen = w->alive_len;
		*mlen = w->malive_len;
		if (*alen)
			memcpy(a, w->alive, *alen);
		if (*mlen)
			memcpy(ma, w->malive, *mlen);
		if (__atomic_load_n(&w->seq, __ATOMIC_ACQUIRE) == s0)
			return;
	}
}

long long clwire_sent_ms(const struct clwire *w)
{
	return __atomic_load_n(&w->sent_ms, __ATOMIC_ACQUIRE);
}

void clwire_mark_sent(struct clwire *w, long long now)
{
	__atomic_store_n(&w->sent_ms, now, __ATOMIC_RELEASE);
}
