/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clwire.h - the sealed datagram and the beat frames (M6).
 *
 * Every cluster datagram is sealed with the Argon2id cluster PSK:
 * magic, version, a 12-byte nonce, then ChaCha20-Poly1305 over the
 * plaintext with the header as associated data.  A frame whose version
 * differs, whose tag fails, or that was sealed under another key is
 * DROPPED, not half-read - which is the property the test pins.
 *
 * Sealing is separated from sending.  clwire_seal() fills a buffer and
 * the caller does the sendto, so nothing here opens or touches a
 * socket, and the round trip is exercised without one.  The key and the
 * socket stay in cluster.c: C.psk and C.fd are read by the bulk plane
 * and by seventeen other senders, so they are passed in, not owned.
 *
 * The beat frames live here because they cannot live anywhere else.
 * The peer thread builds them; the watchdog thread re-seals the LAST
 * ones when nothing has been emitted for BEAT_OVERDUE_MS, and the two
 * cross under a seqlock.  doc/MODULARITY.md is explicit that seal/open
 * and these buffers move together or not at all.
 *
 * The Noise core stays in pc_noise.c; this is the PSK datagram layer.
 */
#ifndef PC_CLWIRE_H
#define PC_CLWIRE_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

#define HDR_LEN       14               /* magic+ver+nonce12 */
#define MAX_DGRAM     65000            /* sealed fits UDP's 65507 */
#define CLWIRE_TAG    16               /* the Poly1305 tag */

/* which of the two frames the watchdog may re-send */
#define CLWIRE_ALIVE   0
#define CLWIRE_MALIVE  1

struct clwire {
	unsigned char alive[70];
	size_t alive_len;
	unsigned char malive[40];
	size_t malive_len;
	unsigned seq;                  /* seqlock over the two frames */
	long long sent_ms;             /* last emission, either layer */
};

/* Twelve nonce bytes from this thread's ChaCha20 keystream (S105): one
 * getrandom(2) per thread rather than per datagram, and the values are
 * what a random 256-bit key produces, so the 96-bit collision bound is
 * unchanged.  Any thread. */
void clwire_nonce(unsigned char out[12]);

/* Seal `pt` into `out`, which must hold HDR_LEN + n + CLWIRE_TAG.
 * Returns the sealed length, or 0 if n exceeds MAX_DGRAM.  The caller
 * sends it; nothing here touches a socket.  Any thread. */
size_t clwire_seal(const uint8_t *psk, const unsigned char *pt, size_t n,
		unsigned char *out);

/* Open a sealed datagram: 0 on success with the plaintext in `pt`, -1
 * when the magic, the version or the tag refuses it.  Any thread. */
int clwire_open(const uint8_t *psk, const unsigned char *buf, size_t n,
		unsigned char *pt, unsigned long long *ptlen);

/* the beat frames.  store: peer thread, the single writer.  take: the
 * watchdog thread, which retries until it reads a stable pair. */
void clwire_beat_store(struct clwire *w, int which, const unsigned char *src,
		size_t n);
void clwire_beat_take(struct clwire *w, unsigned char *a, size_t *alen,
		unsigned char *ma, size_t *mlen);

long long clwire_sent_ms(const struct clwire *w);
void clwire_mark_sent(struct clwire *w, long long now);

#endif /* PC_CLWIRE_H */
