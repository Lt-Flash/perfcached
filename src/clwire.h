/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clwire.h - the PSK-sealed cluster datagram (XChaCha20-Poly1305, header as AD) and the beat
 * frames the watchdog re-seals; sealing is split from sending, so no socket here. */
#ifndef PC_CLWIRE_H
#define PC_CLWIRE_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

/* magic, version, cluster id (16), flags (1), nonce (24): the whole header is the AEAD's
 * associated data, so a datagram cannot be relabelled from one cluster to another */
#define HDR_LEN       63
#define CLWIRE_NONCE_OFF 19
#define CLWIRE_NONCE_LEN 24            /* XChaCha20 - random nonces stay unique fleet-wide */
#define CLWIRE_CID_LEN 16
#define CLWIRE_HDR_CID(b)   ((b) + 2)
#define CLWIRE_FLAGS_OFF 18
#define CLWIRE_HDR_FLAGS(b) ((b)[CLWIRE_FLAGS_OFF])
#define CLWIRE_F_PROVISIONAL 0x01      /* the sender's cluster id is young */
/* the sender has a reconcile open (its pass, or answers still pending): a peer it may need
 * as its witness should not restart now.  Older readers ignore this bit. */
#define CLWIRE_F_RECONCILING 0x02
/* the top four bits are the sealing thread's lane (0 = none, 1-15): the relay port's
 * receive group gives each lane one receive thread */
#define CLWIRE_LANE_SHIFT 4
#define CLWIRE_LANES      15
/* the id this process seals with: NONE (zeros, never joined or founded), PROVISIONAL (minted
 * or adopted < 30 s ago - a founding race may still replace it), ESTABLISHED */
#define CLWIRE_CID_NONE        0
#define CLWIRE_CID_PROVISIONAL 1
#define CLWIRE_CID_ESTABLISHED 2
/* after the nonce, LE: incarnation (start ms, 6), host (4, network order), cluster port (2),
 * the sealing thread's stream (2) and its sequence (6); clreplay.h takes each frame once */
#define CLWIRE_ID_OFF 43
struct clwire_id { uint64_t incarn, seq; uint32_t ip; uint16_t port, stream; };
#define MAX_DGRAM     65000            /* sealed fits UDP's 65507 */
#define CLWIRE_TAG    16               /* the Poly1305 tag */

/* which of the two frames the watchdog may re-send */
#define CLWIRE_ALIVE   0
#define CLWIRE_MALIVE  1

struct clwire {
	unsigned char alive[1408];             /* CLMEMB_ALIVE_MAX 1399 */
	size_t alive_len;
	unsigned char malive[48];              /* CLMEMB_MALIVE_LEN 41 */
	size_t malive_len;
	unsigned seq;                  /* seqlock over the two frames */
	long long sent_ms;             /* last emission, either layer */
};

/* set (or, with @cid NULL, clear) the cluster id every datagram is sealed with, and its
 * state.  Any thread reads it; one writes it. */
void clwire_set_cid(const unsigned char *cid, int state);
/* stamp CLWIRE_F_RECONCILING on every frame sealed from now (0 = not) */
void clwire_set_reconciling(int on);
/* frames this thread seals from now carry a lane of its own, handed out in order of asking
 * and wrapping after CLWIRE_LANES.  Any thread. */
void clwire_take_lane(void);
/* the state, and the id into @out (zeros when NONE) */
int clwire_cid(unsigned char out[CLWIRE_CID_LEN]);
/* the host (network order) and cluster port every frame names as its sender; the
 * incarnation is this process's first seal or this call */
void clwire_set_self(uint32_t ip, uint16_t port);
void clwire_id_read(const unsigned char *hdr, struct clwire_id *id);

/* Nonce bytes from this thread's ChaCha20 keystream: one getrandom(2) per thread rather
 * than per datagram.  Any thread. */
void clwire_nonce(unsigned char out[CLWIRE_NONCE_LEN]);

/* Seal `pt` into `out`, which must hold HDR_LEN + n + CLWIRE_TAG: the sealed length, or 0
 * if n exceeds MAX_DGRAM.  The caller sends it.  Any thread. */
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
