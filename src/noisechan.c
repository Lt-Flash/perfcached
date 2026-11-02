/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * noisechan.c - the client doors' Noise channel, the responder's side
 * (S25'): a client's handshake (message 1 checked against every client
 * secret, or the cluster's; message 2 staged), then its transport records
 * decrypted onto the connection's input.  Moved from proto.c unchanged
 * (the 0.7.2 split, wave 1 - doc/MODULARITY.md); the buffers are proto.c's
 * (protoint.h), the crypto pc_noise.c's.
 */
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>

#include "compat/dprint.h"
#include "pc_noise.h"
#include "proto.h"                      /* struct pc_psk_ctx, PC_BIN_* */
#include "protoint.h"
#include "logrl.h"                     /* CH-17 */
#include "daemon.h"                    /* S468: pc_nat_bin_records */

/* CH-17: a refused handshake is a line per connection, which whoever
 * can reach the door writes - said through the shared limit, keyed by the
 * client's address (unix-socket clients share one key) */
static int hs_say(const struct pc_conn *c, int reason)
{
	pc_addr a;
	socklen_t l = sizeof a;

	memset(&a, 0, sizeof a);
	if (getpeername(c->fd, &a.sa, &l) != 0 ||
	    (pc_addr_family(&a) != AF_INET && pc_addr_family(&a) != AF_INET6))
		return pc_logrl_ok(reason, NULL);
	return pc_logrl_ok(reason, &a);
}

static int pa_refuse(struct pc_conn *c, const char *why)
{
	__atomic_add_fetch(&pc_nat_plain_refused, 1, __ATOMIC_RELAXED);
	if (hs_say(c, LRL_HS_AUTH))
		LM_WARN("handshake: plaintext switch refused - %s\n", why);
	return -1;
}

/* S468: after a granted msg2 the client's first record must be PC_HS_SWITCH
 * under the session keys - a replayed msg1 cannot make one */
static int plain_switch(struct pc_conn *c)
{
	uint8_t pt[PC_HS_SWITCH_LEN + 1];
	size_t n;

	if (c->raw_len < 2)
		return 0;
	n = get16(c->raw);
	if (n != PC_HS_SWITCH_LEN + PC_NOISE_TAGLEN)
		return pa_refuse(c, "not a switch record");
	if (c->raw_len < 2 + n)
		return 0;
	if (pc_transport_decrypt(&c->recv_cs, (const uint8_t *)c->raw + 2, n, pt) !=
	        PC_HS_SWITCH_LEN || memcmp(pt, PC_HS_SWITCH, PC_HS_SWITCH_LEN))
		return pa_refuse(c, "the record does not open under the session keys");
	c->raw_len -= 2 + n;
	memmove(c->raw, c->raw + 2 + n, c->raw_len);
	c->pa_stage = 0;
	c->encrypted = 0;
	c->plain_auth = 1;
	c->established = 1;
	__atomic_add_fetch(&pc_nat_plain_conns, 1, __ATOMIC_RELAXED);
	return 1;
}

/* consume one handshake frame from c->raw; 1 established, 0 need more,
 * 2 msg2 staged with plaintext granted (its switch record due), -1 fatal */
int channel_handshake(struct pc_conn *c)
{
	uint8_t pay[PC_HS_MAXFRAME], m2[PC_HS_MAXFRAME], key_id, status[2];
	const struct pc_psk_ctx *psk = c->psk;
	size_t flen, noise_len, pl, m2len;
	const uint8_t *noise;
	char frame[2 + PC_HS_MAXFRAME];
	int i, matched = 0, plain;

	if (c->raw_len < 2)
		return 0;
	if (c->pa_stage)
		return plain_switch(c);
	/* S339: a client with no secret sends its first request in the
	 * clear, and the binary header's first two bytes (magic 0x9E, ver
	 * 1) read little-endian as a 414-byte handshake - inside the bounds
	 * below, so the door waited for 416 bytes that never came and the
	 * client sat until its own 30 s timeout.  A real message 1 is 50
	 * bytes and never opens like this. */
	if ((unsigned char)c->raw[0] == PC_BIN_MAGIC &&
	    (unsigned char)c->raw[1] == PC_BIN_VER)
		return -1;
	flen = get16(c->raw);
	if (flen < 1 + PC_NOISE_DHLEN + PC_NOISE_TAGLEN || flen > PC_HS_MAXFRAME)
		return -1;
	if (c->raw_len < 2 + flen)
		return 0;

	key_id = (uint8_t)c->raw[2];
	noise = (const uint8_t *)c->raw + 3;
	noise_len = flen - 1;

	if (key_id == PC_PRIN_CLIENT) {
		for (i = 0; i < psk->n_client; i++) {
			pc_hs_init_responder(&c->hs, &key_id, 1);
			if (pc_hs_read_msg1(&c->hs, psk->client[i], noise, noise_len,
			        pay, &pl) == 0) {
				matched = 1;
				break;
			}
		}
	} else if (key_id == PC_PRIN_CLUSTER) {
		/* standalone with no cluster secret: the slot was never
		 * derived and holds zeros - a key anyone can present */
		if (!psk->have_cluster) {
			if (hs_say(c, LRL_HS_CLUSTER))
				LM_WARN("handshake: cluster principal refused - no "
					"cluster secret configured\n");
			return -1;
		}
		pc_hs_init_responder(&c->hs, &key_id, 1);
		if (pc_hs_read_msg1(&c->hs, psk->cluster, noise, noise_len,
		        pay, &pl) == 0)
			matched = 1;
		else if (psk->have_cluster_accept) {    /* CH-7 */
			pc_hs_init_responder(&c->hs, &key_id, 1);
			if (pc_hs_read_msg1(&c->hs, psk->cluster_accept, noise,
			        noise_len, pay, &pl) == 0)
				matched = 1;
		}
	} else {
		if (hs_say(c, LRL_HS_PRINCIPAL))
			LM_WARN("handshake: unknown principal %u\n", key_id);
		return -1;
	}
	if (!matched) {
		if (hs_say(c, LRL_HS_AUTH))
			LM_WARN("handshake: authentication failed (principal %u)\n", key_id);
		return -1;
	}
	if (pl < 1 || pay[0] != PC_CLIENT_VER) {
		if (hs_say(c, LRL_HS_VERSION))
			LM_WARN("handshake: unsupported client version\n");
		return -1;
	}

	/* S468: plaintext for a client that asks, on a door that allows it */
	plain = c->pa_ok && key_id == PC_PRIN_CLIENT && pl >= 2 && pay[1] == PC_HS_PLAIN;
	status[0] = 0x00;
	status[1] = PC_HS_PLAIN;
	if (pc_hs_write_msg2(&c->hs, status, plain ? 2 : 1, m2, &m2len,
	        &c->send_cs, &c->recv_cs) != 0)
		return -1;
	put16(frame, (uint16_t)m2len);
	memcpy(frame + 2, m2, m2len);
	if (wire_append(c, frame, 2 + m2len) < 0)
		return -1;

	c->raw_len -= 2 + flen;
	memmove(c->raw, c->raw + 2 + flen, c->raw_len);
	if (plain) {
		c->pa_stage = 1;
		return 2;
	}
	c->established = 1;
	return 1;
}

/* decrypt all complete transport records in c->raw into c->in */
int channel_records(struct pc_conn *c)
{
	uint8_t pt[PC_NOISE_MAXPT];
	size_t rlen;
	int pl;

	for (;;) {
		if (c->raw_len < 2)
			return 0;
		rlen = get16(c->raw);
		if (rlen < PC_NOISE_TAGLEN || rlen > PC_NOISE_MAXMSG)
			return -1;
		if (c->raw_len < 2 + rlen)
			return 0;
		pl = pc_transport_decrypt(&c->recv_cs, (const uint8_t *)c->raw + 2,
			rlen, pt);
		if (pl < 0)
			return -1;             /* bad tag: tampered / desynced */
		{                              /* S468: per thread, folded */
			static __thread unsigned int n;

			if (++n == 1 || n == 65) {
				__atomic_add_fetch(&pc_nat_bin_records, n == 1 ? 1 : 64,
					__ATOMIC_RELAXED);
				n = 1;
			}
		}
		if (in_append(c, (char *)pt, (size_t)pl) < 0)
			return -1;
		c->raw_len -= 2 + rlen;
		memmove(c->raw, c->raw + 2 + rlen, c->raw_len);
	}
}
