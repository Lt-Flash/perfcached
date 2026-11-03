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
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sodium.h>

#include "clwire.h"

#define PC_CL_MAGIC   0xA1
#define PC_CL_VER     6                /* = cluster.h's; CH-8: the 24-byte nonce; CH-1: the stream position */

/* S105: the 12-byte datagram nonce.  randombytes_buf is one getrandom(2)
 * per call (1.05 us measured); each thread instead keeps a ChaCha20
 * keystream under a key the OS gave it once and takes twelve bytes per
 * datagram.  The values are what a random 256-bit key produces, so
 * they are as unique as random nonces - the 96-bit collision bound is
 * unchanged - and the receiver needs nothing. */
void clwire_nonce(unsigned char out[CLWIRE_NONCE_LEN])
{
	static __thread unsigned char key[32], ks[4096], ctr[12];
	static __thread size_t off = sizeof ks;
	static __thread int have_key;

	if (!have_key) {
		randombytes_buf(key, sizeof key);
		randombytes_buf(ctr, sizeof ctr);
		have_key = 1;
	}
	if (off + CLWIRE_NONCE_LEN > sizeof ks) {
		int i;

		for (i = 0; i < 12 && ++ctr[i] == 0; i++)
			;                          /* the block counter, LE */
		crypto_stream_chacha20_ietf(ks, sizeof ks, ctr, key);
		off = 0;
	}
	memcpy(out, ks + off, CLWIRE_NONCE_LEN);
	off += CLWIRE_NONCE_LEN;
}

/* S302: the id is written bytes-first and published by the state's
 * release store; a sealer acquires the state, then reads bytes that no
 * longer change.  A clear (the provisional id given up on a yield) can
 * race a seal into one datagram that matches nobody - one dropped beat. */
static unsigned char cw_cid[CLWIRE_CID_LEN];
static int cw_state;

static int cw_reconciling;              /* ST8 */
static unsigned cw_lanes;              /* S461: lanes handed out */
static __thread unsigned char cw_lane; /* this thread's, 0 = none */

void clwire_take_lane(void)
{
	if (!cw_lane)
		cw_lane = (unsigned char)(__atomic_fetch_add(&cw_lanes, 1,
			__ATOMIC_RELAXED) % CLWIRE_LANES + 1);
}

void clwire_set_reconciling(int on)
{
	__atomic_store_n(&cw_reconciling, on ? 1 : 0, __ATOMIC_RELAXED);
}

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

/* CH-1: each sealing thread is a stream with its own sequence; a thread
 * that exits hands both on, so a stream's sequence only ever rises */
#define CW_POOL 1024
struct cw_tls { uint64_t seq; uint16_t stream; };
static pthread_mutex_t cw_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t cw_key;
static int cw_inited;
static uint64_t cw_incarn, cw_seq0;    /* seq0: stream 0, when a thread has none */
static uint32_t cw_ip;
static uint16_t cw_port, cw_streams;
static struct cw_tls cw_pool[CW_POOL];
static int cw_pool_n;
static __thread struct cw_tls *cw_me;

static void cw_give_back(void *v)
{
	struct cw_tls *t = v;

	pthread_mutex_lock(&cw_mx);
	if (cw_pool_n < CW_POOL)
		cw_pool[cw_pool_n++] = *t;
	pthread_mutex_unlock(&cw_mx);
	free(t);
}

static void cw_init_locked(void)
{
	struct timespec ts;

	if (cw_inited)
		return;
	clock_gettime(CLOCK_REALTIME, &ts);
	cw_incarn = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
	cw_inited = pthread_key_create(&cw_key, cw_give_back) == 0 ? 1 : -1;
}

static struct cw_tls *cw_take(void)
{
	struct cw_tls *t = calloc(1, sizeof *t);

	pthread_mutex_lock(&cw_mx);
	cw_init_locked();
	if (t && cw_inited > 0) {
		if (cw_pool_n)
			*t = cw_pool[--cw_pool_n];
		else if (cw_streams < 0xffff)
			t->stream = ++cw_streams;
	}
	pthread_mutex_unlock(&cw_mx);
	if (t && (!t->stream || pthread_setspecific(cw_key, t) != 0)) {
		free(t);
		t = NULL;
	}
	return t;
}

void clwire_set_self(uint32_t ip, uint16_t port)
{
	pthread_mutex_lock(&cw_mx);
	cw_init_locked();
	cw_ip = ip;
	cw_port = port;
	pthread_mutex_unlock(&cw_mx);
}

static void put_le(unsigned char *p, uint64_t v, int n)
{
	int i;

	for (i = 0; i < n; i++)
		p[i] = (unsigned char)(v >> (8 * i));
}

static uint64_t get_le(const unsigned char *p, int n)
{
	uint64_t v = 0;
	int i;

	for (i = n - 1; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

static void cw_stamp(unsigned char *p)
{
	struct cw_tls *t = cw_me ? cw_me : (cw_me = cw_take());
	uint64_t seq;

	if (t)
		seq = ++t->seq;
	else {
		pthread_mutex_lock(&cw_mx);
		cw_init_locked();
		pthread_mutex_unlock(&cw_mx);
		seq = __atomic_add_fetch(&cw_seq0, 1, __ATOMIC_RELAXED);
	}
	put_le(p, cw_incarn, 6);
	memcpy(p + 6, &cw_ip, 4);
	put_le(p + 10, cw_port, 2);
	put_le(p + 12, t ? t->stream : 0, 2);
	put_le(p + 14, seq, 6);
}

void clwire_id_read(const unsigned char *hdr, struct clwire_id *id)
{
	const unsigned char *p = hdr + CLWIRE_ID_OFF;

	id->incarn = get_le(p, 6);
	memcpy(&id->ip, p + 6, 4);
	id->port = (uint16_t)get_le(p + 10, 2);
	id->stream = (uint16_t)get_le(p + 12, 2);
	id->seq = get_le(p + 14, 6);
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
	CLWIRE_HDR_FLAGS(out) = (st == CLWIRE_CID_PROVISIONAL ?
		CLWIRE_F_PROVISIONAL : 0) |
		(__atomic_load_n(&cw_reconciling, __ATOMIC_RELAXED) ?
		CLWIRE_F_RECONCILING : 0) |
		(unsigned char)(cw_lane << CLWIRE_LANE_SHIFT);
	clwire_nonce(out + CLWIRE_NONCE_OFF);
	cw_stamp(out + CLWIRE_ID_OFF);
	crypto_aead_xchacha20poly1305_ietf_encrypt(out + HDR_LEN, &clen,
		pt, n, out, HDR_LEN, NULL, out + CLWIRE_NONCE_OFF, psk);
	return HDR_LEN + (size_t)clen;
}

int clwire_open(const uint8_t *psk, const unsigned char *buf, size_t n,
		unsigned char *pt, unsigned long long *ptlen)
{
	if (n < HDR_LEN + CLWIRE_TAG || buf[0] != PC_CL_MAGIC ||
	        buf[1] != PC_CL_VER)
		return -1;
	if (crypto_aead_xchacha20poly1305_ietf_decrypt(pt, ptlen, NULL,
	        buf + HDR_LEN, n - HDR_LEN, buf, HDR_LEN, buf + CLWIRE_NONCE_OFF,
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
