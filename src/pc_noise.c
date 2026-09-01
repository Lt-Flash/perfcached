/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Yury Kirsanov
 * Part of libperfd - see lib/LICENSE.  This file must stay
 * free of src/core includes; tools/sync-libperfd.sh exports
 * exactly the MIT set to consumers. */
/*
 * pc_noise.c — Noise_NNpsk0_25519_ChaChaPoly_SHA256 over libsodium.
 * See pc_noise.h.  Follows the Noise spec rev 34: the 'e' token with a
 * PSK does MixHash then MixKey; the psk token does MixKeyAndHash;
 * Split's first CipherState is the initiator->responder direction.
 */
#include <string.h>
#include <sodium.h>

#include "compat/dprint.h"
#include "pc_noise.h"

/* NOISE_NAME, not NAME: consumer build systems (OpenSIPS modules) pass
 * -DNAME=<module.so> on every compile line, and the collision was a
 * redefinition warning away from being a corrupted handshake label. */
#define NOISE_NAME "Noise_NNpsk0_25519_ChaChaPoly_SHA256"

/* ---- HKDF (RFC 5869 over HMAC-SHA256), Noise variant ------------------- */

static void hmac(const uint8_t *key, size_t keylen, const uint8_t *d1,
		size_t n1, const uint8_t *d2, size_t n2, uint8_t out[32])
{
	crypto_auth_hmacsha256_state st;

	crypto_auth_hmacsha256_init(&st, key, keylen);
	if (n1)
		crypto_auth_hmacsha256_update(&st, d1, n1);
	if (n2)
		crypto_auth_hmacsha256_update(&st, d2, n2);
	crypto_auth_hmacsha256_final(&st, out);
}

void pc_noise_hkdf(const uint8_t ck[32], const uint8_t *ikm, size_t ikmlen,
		int n_out, uint8_t o1[32], uint8_t o2[32], uint8_t o3[32])
{
	uint8_t temp_key[32], one = 1, two = 2, three = 3;

	hmac(ck, 32, ikm, ikmlen, NULL, 0, temp_key);       /* extract */
	hmac(temp_key, 32, &one, 1, NULL, 0, o1);           /* T(1) */
	if (n_out >= 2)
		hmac(temp_key, 32, o1, 32, &two, 1, o2);        /* T(2) */
	if (n_out >= 3)
		hmac(temp_key, 32, o2, 32, &three, 1, o3);      /* T(3) */
	sodium_memzero(temp_key, sizeof temp_key);
}

/* ---- CipherState ------------------------------------------------------- */

static void cs_init_key(struct pc_cipherstate *cs, const uint8_t k[32])
{
	memcpy(cs->k, k, 32);
	cs->n = 0;
	cs->has_key = 1;
}

static void nonce12(uint64_t n, uint8_t out[12])
{
	memset(out, 0, 4);
	out[4]  = (uint8_t)n;
	out[5]  = (uint8_t)(n >> 8);
	out[6]  = (uint8_t)(n >> 16);
	out[7]  = (uint8_t)(n >> 24);
	out[8]  = (uint8_t)(n >> 32);
	out[9]  = (uint8_t)(n >> 40);
	out[10] = (uint8_t)(n >> 48);
	out[11] = (uint8_t)(n >> 56);
}

/* EncryptWithAd; out holds ptlen+16.  No key -> copy through (Noise). */
static int cs_encrypt(struct pc_cipherstate *cs, const uint8_t *ad,
		size_t adlen, const uint8_t *pt, size_t ptlen, uint8_t *out)
{
	uint8_t npub[12];
	unsigned long long clen = 0;

	if (!cs->has_key) {
		memcpy(out, pt, ptlen);
		return (int)ptlen;
	}
	nonce12(cs->n++, npub);
	crypto_aead_chacha20poly1305_ietf_encrypt(out, &clen, pt, ptlen,
		ad, adlen, NULL, npub, cs->k);
	return (int)clen;
}

static int cs_decrypt(struct pc_cipherstate *cs, const uint8_t *ad,
		size_t adlen, const uint8_t *ct, size_t ctlen, uint8_t *out)
{
	uint8_t npub[12];
	unsigned long long mlen = 0;

	if (!cs->has_key) {
		memcpy(out, ct, ctlen);
		return (int)ctlen;
	}
	nonce12(cs->n, npub);
	if (crypto_aead_chacha20poly1305_ietf_decrypt(out, &mlen, NULL,
	        ct, ctlen, ad, adlen, npub, cs->k) != 0)
		return -1;
	cs->n++;                    /* advance only on a verified tag */
	return (int)mlen;
}

/* ---- SymmetricState ---------------------------------------------------- */

static void sym_init(struct pc_symstate *ss)
{
	uint8_t name[64];
	size_t nl = strlen(NOISE_NAME);

	/* NOISE_NAME is 36 bytes > 32, so h = SHA256(NOISE_NAME) */
	/* protocol name bytes feeding a hash - termination is no part
	 * of them */
	/* NOLINTNEXTLINE(bugprone-not-null-terminated-result) */
	memcpy(name, NOISE_NAME, nl);
	crypto_hash_sha256(ss->h, name, nl);
	memcpy(ss->ck, ss->h, 32);
	memset(&ss->cs, 0, sizeof ss->cs);
}

static void sym_mix_hash(struct pc_symstate *ss, const uint8_t *d, size_t n)
{
	crypto_hash_sha256_state st;

	crypto_hash_sha256_init(&st);
	crypto_hash_sha256_update(&st, ss->h, 32);
	crypto_hash_sha256_update(&st, d, n);
	crypto_hash_sha256_final(&st, ss->h);
}

static void sym_mix_key(struct pc_symstate *ss, const uint8_t *ikm, size_t n)
{
	uint8_t o1[32], o2[32];

	pc_noise_hkdf(ss->ck, ikm, n, 2, o1, o2, NULL);
	memcpy(ss->ck, o1, 32);
	cs_init_key(&ss->cs, o2);
	sodium_memzero(o2, sizeof o2);
}

static void sym_mix_key_and_hash(struct pc_symstate *ss, const uint8_t *ikm,
		size_t n)
{
	uint8_t o1[32], o2[32], o3[32];

	pc_noise_hkdf(ss->ck, ikm, n, 3, o1, o2, o3);
	memcpy(ss->ck, o1, 32);
	sym_mix_hash(ss, o2, 32);
	cs_init_key(&ss->cs, o3);
	sodium_memzero(o3, sizeof o3);
}

/* EncryptAndHash: ciphertext = Encrypt(h as AD, pt); MixHash(ciphertext) */
static int sym_encrypt_and_hash(struct pc_symstate *ss, const uint8_t *pt,
		size_t ptlen, uint8_t *out)
{
	int clen = cs_encrypt(&ss->cs, ss->h, 32, pt, ptlen, out);

	sym_mix_hash(ss, out, (size_t)clen);
	return clen;
}

static int sym_decrypt_and_hash(struct pc_symstate *ss, const uint8_t *ct,
		size_t ctlen, uint8_t *out)
{
	int mlen = cs_decrypt(&ss->cs, ss->h, 32, ct, ctlen, out);

	if (mlen < 0)
		return -1;
	sym_mix_hash(ss, ct, ctlen);       /* hash the received ciphertext */
	return mlen;
}

static void sym_split(struct pc_symstate *ss, struct pc_cipherstate *c1,
		struct pc_cipherstate *c2)
{
	uint8_t o1[32], o2[32];

	pc_noise_hkdf(ss->ck, NULL, 0, 2, o1, o2, NULL);
	memset(c1, 0, sizeof *c1);
	memset(c2, 0, sizeof *c2);
	cs_init_key(c1, o1);
	cs_init_key(c2, o2);
	sodium_memzero(o1, sizeof o1);
	sodium_memzero(o2, sizeof o2);
}

/* ---- 'e' token (with PSK: MixHash then MixKey) ------------------------- */

static void write_e(struct pc_handshake *hs, uint8_t *out)
{
	crypto_scalarmult_base(hs->e_pub, hs->e_priv);   /* e_priv pre-filled */
	memcpy(out, hs->e_pub, 32);
	sym_mix_hash(&hs->ss, hs->e_pub, 32);
	sym_mix_key(&hs->ss, hs->e_pub, 32);
}

static void read_e(struct pc_handshake *hs, const uint8_t *in)
{
	memcpy(hs->re, in, 32);
	sym_mix_hash(&hs->ss, hs->re, 32);
	sym_mix_key(&hs->ss, hs->re, 32);
}

/* ---- PSK derivation ---------------------------------------------------- */

int pc_psk_derive(const char *password, size_t plen, int principal,
		uint8_t psk[32])
{
	static const char *label[] = { "client", "cluster" };
	uint8_t saltseed[64], salt[crypto_pwhash_SALTBYTES];
	crypto_hash_sha256_state st;
	const char *lbl = label[principal == PC_PRIN_CLUSTER ? 1 : 0];
	static const char pfx[] = "perfcached-psk-salt-v1:";

	/* deterministic per-principal salt (a shared secret cannot use a
	 * random salt): salt = SHA256("perfcached-psk-salt-v1:"+label)[:16] */
	crypto_hash_sha256_init(&st);
	crypto_hash_sha256_update(&st, (const uint8_t *)pfx, sizeof pfx - 1);
	crypto_hash_sha256_update(&st, (const uint8_t *)lbl, strlen(lbl));
	crypto_hash_sha256_final(&st, saltseed);
	memcpy(salt, saltseed, sizeof salt);

	if (crypto_pwhash(psk, 32, password, plen, salt,
	        crypto_pwhash_OPSLIMIT_INTERACTIVE,
	        crypto_pwhash_MEMLIMIT_INTERACTIVE,
	        crypto_pwhash_ALG_ARGON2ID13) != 0) {
		LM_ERR("Argon2id PSK derivation failed (out of memory?)\n");
		return -1;
	}
	return 0;
}

/* ---- responder --------------------------------------------------------- */

void pc_hs_init_responder(struct pc_handshake *hs, const uint8_t *prologue,
		size_t plen)
{
	memset(hs, 0, sizeof *hs);
	hs->initiator = 0;
	sym_init(&hs->ss);
	sym_mix_hash(&hs->ss, prologue, plen);
}

/* msg1 = [e.pub:32][enc payload+tag]; psk token is processed first */
int pc_hs_read_msg1(struct pc_handshake *hs, const uint8_t psk[32],
		const uint8_t *msg, size_t mlen, uint8_t *payload, size_t *paylen)
{
	int pl;

	if (mlen < 32 + PC_NOISE_TAGLEN)
		return -1;
	sym_mix_key_and_hash(&hs->ss, psk, 32);
	read_e(hs, msg);
	pl = sym_decrypt_and_hash(&hs->ss, msg + 32, mlen - 32, payload);
	if (pl < 0)
		return -1;                     /* wrong PSK or tampered */
	*paylen = (size_t)pl;
	return 0;
}

int pc_hs_write_msg2(struct pc_handshake *hs, const uint8_t *payload,
		size_t paylen, uint8_t *out, size_t *outlen,
		struct pc_cipherstate *send, struct pc_cipherstate *recv)
{
	uint8_t dh[32];
	int cl;

	randombytes_buf(hs->e_priv, 32);
	write_e(hs, out);                  /* e token */
	if (crypto_scalarmult(dh, hs->e_priv, hs->re) != 0)  /* ee */
		return -1;                     /* peer sent a low-order point */
	sym_mix_key(&hs->ss, dh, 32);
	sodium_memzero(dh, sizeof dh);
	cl = sym_encrypt_and_hash(&hs->ss, payload, paylen, out + 32);
	*outlen = 32 + (size_t)cl;
	/* responder: recv = c1 (initiator->responder), send = c2 */
	sym_split(&hs->ss, recv, send);
	return 0;
}

/* ---- initiator --------------------------------------------------------- */

void pc_hs_init_initiator(struct pc_handshake *hs, const uint8_t *prologue,
		size_t plen)
{
	memset(hs, 0, sizeof *hs);
	hs->initiator = 1;
	sym_init(&hs->ss);
	sym_mix_hash(&hs->ss, prologue, plen);
}

int pc_hs_write_msg1(struct pc_handshake *hs, const uint8_t psk[32],
		const uint8_t *payload, size_t paylen, uint8_t *out, size_t *outlen)
{
	int cl;

	sym_mix_key_and_hash(&hs->ss, psk, 32);
	randombytes_buf(hs->e_priv, 32);
	write_e(hs, out);
	cl = sym_encrypt_and_hash(&hs->ss, payload, paylen, out + 32);
	*outlen = 32 + (size_t)cl;
	return 0;
}

int pc_hs_read_msg2(struct pc_handshake *hs, const uint8_t *msg, size_t mlen,
		uint8_t *payload, size_t *paylen,
		struct pc_cipherstate *send, struct pc_cipherstate *recv)
{
	uint8_t dh[32];
	int pl;

	if (mlen < 32 + PC_NOISE_TAGLEN)
		return -1;
	read_e(hs, msg);                   /* e token */
	if (crypto_scalarmult(dh, hs->e_priv, hs->re) != 0)  /* ee */
		return -1;
	sym_mix_key(&hs->ss, dh, 32);
	sodium_memzero(dh, sizeof dh);
	pl = sym_decrypt_and_hash(&hs->ss, msg + 32, mlen - 32, payload);
	if (pl < 0)
		return -1;
	*paylen = (size_t)pl;
	/* initiator: send = c1 (initiator->responder), recv = c2 */
	sym_split(&hs->ss, send, recv);
	return 0;
}

/* ---- transport --------------------------------------------------------- */

int pc_transport_encrypt(struct pc_cipherstate *cs, const uint8_t *pt,
		size_t ptlen, uint8_t *out)
{
	return cs_encrypt(cs, NULL, 0, pt, ptlen, out);
}

int pc_transport_decrypt(struct pc_cipherstate *cs, const uint8_t *ct,
		size_t ctlen, uint8_t *out)
{
	if (ctlen < PC_NOISE_TAGLEN)
		return -1;
	return cs_decrypt(cs, NULL, 0, ct, ctlen, out);
}

/* ---- selftest (RFC 5869 extract vector + full round-trips) ------------- */

static int hexeq(const uint8_t *got, const char *hex)
{
	uint8_t want[32];

	sodium_hex2bin(want, sizeof want, hex, strlen(hex), NULL, NULL, NULL);
	return memcmp(got, want, 32) == 0;
}

int pc_noise_selftest(void)
{
	/* RFC 5869 Test Case 1: PRK = HMAC-SHA256(salt, IKM); our HKDF's
	 * internal temp_key IS that PRK, and T(1) is our first output. */
	uint8_t ikm[22], salt[13], o1[32], o2[32];
	uint8_t psk_a[32], psk_b[32];
	struct pc_handshake ini, res;
	struct pc_cipherstate is, ir, rs, rr;
	uint8_t m1[256], m2[256], p1[64], p2[64];
	uint8_t ct[128], pt[128];
	const uint8_t prologue[1] = { PC_PRIN_CLIENT };
	size_t l1, l2, pl;
	int i;

	memset(ikm, 0x0b, sizeof ikm);
	for (i = 0; i < 13; i++)
		salt[i] = (uint8_t)i;
	/* temp_key = HMAC(salt, ikm) = RFC5869 TC1 PRK; expose via o1 by
	 * running one HMAC directly (hkdf hides temp_key, so check T(1)
	 * against an independent computation is done in python; here we
	 * verify the extract PRK, the crypto that matters most) */
	{
		uint8_t prk[32];

		hmac(salt, sizeof salt, ikm, sizeof ikm, NULL, 0, prk);
		if (!hexeq(prk, "077709362c2e32df0ddc3f0dc47bba63"
		        "90b6c73bb50f9c3122ec844ad7c2b3e5")) {
			LM_ERR("noise selftest: RFC5869 PRK mismatch\n");
			return -1;
		}
	}
	(void)o1; (void)o2;

	if (sodium_init() < 0)
		return -1;

	/* a full handshake with the SAME psk must converge and talk */
	if (pc_psk_derive("correct horse battery", 21, PC_PRIN_CLIENT, psk_a) ||
	        pc_psk_derive("correct horse battery", 21, PC_PRIN_CLIENT, psk_b))
		return -1;
	if (memcmp(psk_a, psk_b, 32)) {
		LM_ERR("noise selftest: Argon2id not deterministic\n");
		return -1;
	}
	pc_hs_init_initiator(&ini, prologue, 1);
	pc_hs_init_responder(&res, prologue, 1);
	p1[0] = 0x01;
	pc_hs_write_msg1(&ini, psk_a, p1, 1, m1, &l1);
	if (pc_hs_read_msg1(&res, psk_b, m1, l1, p2, &pl) || pl != 1 ||
	        p2[0] != 0x01) {
		LM_ERR("noise selftest: msg1 failed\n");
		return -1;
	}
	p2[0] = 0x00;
	pc_hs_write_msg2(&res, p2, 1, m2, &l2, &rs, &rr);
	if (pc_hs_read_msg2(&ini, m2, l2, p1, &pl, &is, &ir) || pl != 1) {
		LM_ERR("noise selftest: msg2 failed\n");
		return -1;
	}
	/* transport both directions */
	{
		const char *a = "client->server";
		const char *b = "server->client";
		int cl = pc_transport_encrypt(&is, (const uint8_t *)a, 14, ct);

		if (pc_transport_decrypt(&rr, ct, cl, pt) != 14 ||
		        memcmp(pt, a, 14)) {
			LM_ERR("noise selftest: c->s transport\n");
			return -1;
		}
		cl = pc_transport_encrypt(&rs, (const uint8_t *)b, 14, ct);
		if (pc_transport_decrypt(&ir, ct, cl, pt) != 14 ||
		        memcmp(pt, b, 14)) {
			LM_ERR("noise selftest: s->c transport\n");
			return -1;
		}
	}
	/* wrong PSK must fail msg1 */
	pc_psk_derive("a different password", 20, PC_PRIN_CLIENT, psk_b);
	pc_hs_init_initiator(&ini, prologue, 1);
	pc_hs_init_responder(&res, prologue, 1);
	pc_hs_write_msg1(&ini, psk_a, p1, 1, m1, &l1);
	if (pc_hs_read_msg1(&res, psk_b, m1, l1, p2, &pl) == 0) {
		LM_ERR("noise selftest: wrong PSK accepted\n");
		return -1;
	}
	/* tamper: flip a ciphertext byte in a fresh msg1 */
	pc_hs_init_initiator(&ini, prologue, 1);
	pc_hs_init_responder(&res, prologue, 1);
	pc_hs_write_msg1(&ini, psk_a, p1, 1, m1, &l1);
	m1[l1 - 1] ^= 0x01;
	if (pc_hs_read_msg1(&res, psk_a, m1, l1, p2, &pl) == 0) {
		LM_ERR("noise selftest: tampered msg1 accepted\n");
		return -1;
	}
	return 0;
}
