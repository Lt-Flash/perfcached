/* noisechantest.c - the client doors' Noise channel, the responder's side
 * (noisechan.c; the 0.7.2 split, wave 1), against pc_noise's own
 * initiator: a real handshake and real records, the connection's buffers
 * stubbed.
 *
 *   A  a handshake with the second of two client secrets: established,
 *      the frame consumed, message 2 staged on the wire and read by the
 *      client - whose keys then match the door's both ways; one with the
 *      cluster's secret too
 *   B  records: two in one read decrypted in order onto the input; a
 *      record cut short waits, its bytes kept; a tampered one is fatal; a
 *      length under a tag is fatal
 *   C  refusals: a plaintext binary header (S339) at once, not waited on;
 *      a handshake frame too short or too long; an unknown principal; no
 *      client secret matching; the cluster principal on a node with no
 *      cluster secret; an unknown client version.  And a frame not all
 *      read yet - 0, nothing consumed
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sodium.h>
#include "compat/dprint.h"
#include "pc_noise.h"
#include "proto.h"
#include "protoint.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* ---- the stubs: the log, the input and output buffers ------------------- */
int compat_log_level = L_WARN;
/* CH-17: every line said - this test reads the refusals themselves */
int pc_logrl_ok(int reason, const pc_addr *src) { (void)reason; (void)src; return 1; }
static char last_log[256];

void compat_log(int level, const char *tag, const char *fmt, ...)
{
	va_list ap;

	(void)level; (void)tag;
	va_start(ap, fmt);
	vsnprintf(last_log, sizeof last_log, fmt, ap);
	va_end(ap);
}

static char inb[8192], wireb[8192], rawb[16384];

int in_append(struct pc_conn *c, const char *p, size_t n)
{
	if (c->in_len + n > sizeof inb)
		return -1;
	memcpy(c->in + c->in_len, p, n);
	c->in_len += n;
	return 0;
}

int wire_append(struct pc_conn *c, const char *p, size_t n)
{
	if (c->wire_len + n > sizeof wireb)
		return -1;
	memcpy(c->wire + c->wire_len, p, n);
	c->wire_len += n;
	return 0;
}

/* ---- the harness ------------------------------------------------------- */
static struct pc_psk_ctx psk;
static struct pc_conn c;
static struct pc_handshake chs;              /* the client's */
static struct pc_cipherstate c_send, c_recv;

static void fresh(void)
{
	memset(&c, 0, sizeof c);
	c.psk = &psk;
	c.in = inb;
	c.wire = wireb;
	c.raw = rawb;
	c.raw_cap = sizeof rawb;
}

static void feed(const void *p, size_t n)
{
	memcpy(c.raw + c.raw_len, p, n);
	c.raw_len += n;
}

/* the client's message 1 as the door frames it: [len16][key_id][msg1] */
static size_t msg1(unsigned char *out, uint8_t key_id, const uint8_t key[32],
		uint8_t ver)
{
	uint8_t m1[512];
	size_t m1len = 0;

	pc_hs_init_initiator(&chs, &key_id, 1);
	if (pc_hs_write_msg1(&chs, key, &ver, 1, m1, &m1len) != 0)
		return 0;
	put16(out, (uint16_t)(m1len + 1));
	out[2] = key_id;
	memcpy(out + 3, m1, m1len);
	return 3 + m1len;
}

/* the client reads message 2 off the wire: 0, the status in *st */
static int msg2(uint8_t *st)
{
	size_t n = get16(c.wire), pl = 0;
	uint8_t pay[64];

	if (c.wire_len != 2 + n ||
	    pc_hs_read_msg2(&chs, (const uint8_t *)c.wire + 2, n, pay, &pl,
	        &c_send, &c_recv) != 0 || pl < 1)
		return -1;
	*st = pay[0];
	return 0;
}

static size_t record(unsigned char *out, const char *pt)
{
	int n = pc_transport_encrypt(&c_send, (const uint8_t *)pt, strlen(pt), out + 2);

	if (n < 0)
		return 0;
	put16(out, (uint16_t)n);
	return 2 + (size_t)n;
}

int main(void)
{
	unsigned char f[4096], r1[256], r2[256];
	uint8_t other[32], st = 9;
	size_t n, n1, n2;
	int rc;

	if (sodium_init() < 0)
		return 2;
	randombytes_buf(psk.client[0], 32);
	randombytes_buf(psk.client[1], 32);
	randombytes_buf(psk.cluster, 32);
	randombytes_buf(other, 32);
	psk.n_client = 2;
	psk.have_cluster = 1;

	/* A */
	fresh();
	n = msg1(f, PC_PRIN_CLIENT, psk.client[1], PC_CLIENT_VER);
	feed(f, n);
	rc = channel_handshake(&c);
	CHK(rc == 1 && c.established && c.raw_len == 0,
		"A: the second client secret - established, the frame consumed");
	CHK(msg2(&st) == 0 && st == 0, "A: message 2 on the wire, read by the client - status 0");
	{
		uint8_t ct[64], pt[64];
		int k = pc_transport_encrypt(&c.send_cs, (const uint8_t *)"pong", 4, ct);
		int m = k < 0 ? -1 : pc_transport_decrypt(&c_recv, ct, (size_t)k, pt);

		CHK(m == 4 && !memcmp(pt, "pong", 4), "A: the door's send key is the client's receive key");
	}

	/* B */
	n1 = record(r1, "hello ");
	n2 = record(r2, "world");
	feed(r1, n1);
	feed(r2, n2);
	rc = channel_records(&c);
	CHK(rc == 0 && c.in_len == 11 && !memcmp(c.in, "hello world", 11) && c.raw_len == 0,
		"B: two records in one read - decrypted in order onto the input");
	n1 = record(r1, "again");
	feed(r1, n1 - 3);
	rc = channel_records(&c);
	CHK(rc == 0 && c.in_len == 11 && c.raw_len == n1 - 3, "B: a record cut short - waits, its bytes kept");
	feed(r1 + n1 - 3, 3);
	rc = channel_records(&c);
	CHK(rc == 0 && c.in_len == 16 && !memcmp(c.in + 11, "again", 5) && c.raw_len == 0,
		"B: and with the rest - read");
	n1 = record(r1, "evil");
	r1[n1 - 1] ^= 1;
	feed(r1, n1);
	CHK(channel_records(&c) == -1, "B: a tampered record - fatal");
	c.raw_len = 0;
	put16(r1, PC_NOISE_TAGLEN - 1);
	feed(r1, 2 + PC_NOISE_TAGLEN - 1);
	CHK(channel_records(&c) == -1, "B: a record shorter than its tag - fatal");

	/* the cluster principal */
	fresh();
	n = msg1(f, PC_PRIN_CLUSTER, psk.cluster, PC_CLIENT_VER);
	feed(f, n);
	CHK(channel_handshake(&c) == 1 && msg2(&st) == 0, "A: the cluster's secret, as the cluster principal");

	/* C */
	fresh();
	f[0] = PC_BIN_MAGIC;
	f[1] = PC_BIN_VER;
	feed(f, 2);
	CHK(channel_handshake(&c) == -1, "C: a plaintext binary header (S339) - refused at once, not waited on");
	fresh();
	feed("\x01", 1);
	CHK(channel_handshake(&c) == 0 && c.raw_len == 1, "C: one byte - 0, kept");
	fresh();
	n = msg1(f, PC_PRIN_CLIENT, psk.client[0], PC_CLIENT_VER);
	feed(f, n - 1);
	CHK(channel_handshake(&c) == 0 && c.raw_len == n - 1 && !c.established,
		"C: a frame not all read - 0, nothing consumed");
	fresh();
	put16(f, 1 + PC_NOISE_DHLEN + PC_NOISE_TAGLEN - 1);
	feed(f, 2);
	CHK(channel_handshake(&c) == -1, "C: a handshake frame shorter than DH + tag - refused");
	fresh();
	put16(f, PC_HS_MAXFRAME + 1);
	feed(f, 2);
	CHK(channel_handshake(&c) == -1, "C: one over PC_HS_MAXFRAME - refused");
	fresh();
	n = msg1(f, 7, psk.client[0], PC_CLIENT_VER);
	feed(f, n);
	CHK(channel_handshake(&c) == -1 && strstr(last_log, "unknown principal 7"),
		"C: an unknown principal - refused, named in the log");
	fresh();
	n = msg1(f, PC_PRIN_CLIENT, other, PC_CLIENT_VER);
	feed(f, n);
	CHK(channel_handshake(&c) == -1 && strstr(last_log, "authentication failed"),
		"C: no client secret matching - authentication failed");
	fresh();
	psk.have_cluster = 0;
	n = msg1(f, PC_PRIN_CLUSTER, psk.cluster, PC_CLIENT_VER);
	feed(f, n);
	CHK(channel_handshake(&c) == -1 && strstr(last_log, "no cluster secret configured"),
		"C: the cluster principal where no cluster secret is set - refused");
	psk.have_cluster = 1;
	fresh();
	n = msg1(f, PC_PRIN_CLIENT, psk.client[0], PC_CLIENT_VER + 1);
	feed(f, n);
	CHK(channel_handshake(&c) == -1 && strstr(last_log, "unsupported client version"),
		"C: a client version this door does not speak - refused");

	printf("noisechantest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
