/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov
 * Part of libperfd - see lib/LICENSE.  This file must stay
 * free of src/core includes; tools/sync-libperfd.sh exports
 * exactly the MIT set to consumers. */
/*
 * pc_noise.h — Noise_NNpsk0_25519_ChaChaPoly_SHA256 over libsodium
 * (task S25').  The client and cluster planes both ride this; the daemon
 * is always the responder, clients the initiator.
 *
 * NNpsk0 message pattern:
 *     -> psk, e
 *     <- e, ee
 * No static keys (NN): a leaked long-term key cannot exist.  The PSK
 * (Argon2id of the principal password) authenticates; the ephemerals
 * give forward secrecy.  Wire, per connection:
 *     msg1  [key_id:1] [e.pub:32] [enc payload+tag]
 *     msg2            [e.pub:32] [enc payload+tag]
 * key_id is cleartext (selects the principal/PSK) and folded into the
 * prologue, so tampering with it breaks msg1's tag.
 *
 * All multi-byte lengths on the transport are little-endian (portability
 * rule).  Nothing here logs or holds a password beyond deriving its PSK.
 */
#ifndef PC_NOISE_H
#define PC_NOISE_H

#include "pc_attr.h"

#include <stddef.h>
#include <stdint.h>

#define PC_NOISE_DHLEN   32
#define PC_NOISE_TAGLEN  16
#define PC_NOISE_KEYLEN  32
#define PC_NOISE_MAXMSG  65535           /* Noise message ceiling */
#define PC_NOISE_MAXPT   (PC_NOISE_MAXMSG - PC_NOISE_TAGLEN)

/* principals: the key_id byte and the Argon2id context label */
#define PC_PRIN_CLIENT   0
#define PC_PRIN_CLUSTER  1

/* the msg1 ENCRYPTED payload carries the client's protocol version as
 * its first byte - part of the handshake contract, shared with every
 * client implementation (perfcli, libperfd, the python interop rig) */
#define PC_CLIENT_VER    0x01

struct pc_cipherstate {
	uint8_t  k[PC_NOISE_KEYLEN];
	uint64_t n;
	int      has_key;
};

struct pc_symstate {
	uint8_t ck[PC_NOISE_KEYLEN];
	uint8_t h[32];
	struct pc_cipherstate cs;
};

struct pc_handshake {
	struct pc_symstate ss;
	uint8_t e_priv[PC_NOISE_DHLEN];
	uint8_t e_pub[PC_NOISE_DHLEN];
	uint8_t re[PC_NOISE_DHLEN];
	int     initiator;
};

/* PSK derivation: password + principal -> 32-byte PSK via Argon2id with
 * a deterministic per-principal salt (shared secret, so the salt cannot
 * be random).  Cost is paid ONCE (startup / per client process), never
 * per connection.  Returns 0, or -1 if Argon2id fails (OOM). */
PC_MUST_CHECK int pc_psk_derive(const char *password, size_t plen, int principal,
		uint8_t psk[PC_NOISE_KEYLEN]);

/* responder (daemon) side */
void pc_hs_init_responder(struct pc_handshake *hs, const uint8_t *prologue,
		size_t plen);
PC_MUST_CHECK int pc_hs_read_msg1(struct pc_handshake *hs, const uint8_t psk[32],
		const uint8_t *msg, size_t mlen, uint8_t *payload, size_t *paylen);
PC_MUST_CHECK int pc_hs_write_msg2(struct pc_handshake *hs, const uint8_t *payload,
		size_t paylen, uint8_t *out, size_t *outlen,
		struct pc_cipherstate *send, struct pc_cipherstate *recv);

/* initiator (client) side */
void pc_hs_init_initiator(struct pc_handshake *hs, const uint8_t *prologue,
		size_t plen);
PC_MUST_CHECK int pc_hs_write_msg1(struct pc_handshake *hs, const uint8_t psk[32],
		const uint8_t *payload, size_t paylen, uint8_t *out, size_t *outlen);
PC_MUST_CHECK int pc_hs_read_msg2(struct pc_handshake *hs, const uint8_t *msg, size_t mlen,
		uint8_t *payload, size_t *paylen,
		struct pc_cipherstate *send, struct pc_cipherstate *recv);

/* transport records (post-handshake): AEAD with an empty AD, 8-byte
 * counter nonce.  encrypt: out must hold ptlen+16.  decrypt: out must
 * hold ctlen-16.  Return the output length or -1 (bad tag). */
PC_MUST_CHECK int pc_transport_encrypt(struct pc_cipherstate *cs, const uint8_t *pt,
		size_t ptlen, uint8_t *out);
PC_MUST_CHECK int pc_transport_decrypt(struct pc_cipherstate *cs, const uint8_t *ct,
		size_t ctlen, uint8_t *out);

/* HKDF exposed for the selftest (RFC 5869 extract check) */
void pc_noise_hkdf(const uint8_t ck[32], const uint8_t *ikm, size_t ikmlen,
		int n_out, uint8_t o1[32], uint8_t o2[32], uint8_t o3[32]);

PC_MUST_CHECK int pc_noise_selftest(void);

#endif /* PC_NOISE_H */
