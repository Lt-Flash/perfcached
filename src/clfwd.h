/* SPDX-License-Identifier: GPL-2.0-or-later
 * clfwd.h - the proxy write plane's frames; a parsed one points into the datagram. */
#ifndef PC_CLFWD_H
#define PC_CLFWD_H

#include <stddef.h>
#include <stdint.h>

/* FWD_OP [type1][req4][node2][op1][ttl4][delta8][collen1][klen2][col][key][val],
 * FWD_ACK [type1][req4][ok1][newval8]; the caller passes the message type in */
#define CLFWD_OP_HDR   23
#define CLFWD_ACK_LEN  14

struct clfwd_op {
	uint32_t req;
	unsigned int node;             /* the forwarding node */
	unsigned char op;
	unsigned int ttl_rel;
	long long delta;
	const char *col;
	unsigned int collen;
	const char *key;
	unsigned int klen;
	const char *val;
	unsigned int vlen;
};

struct clfwd_ack {
	uint32_t req;
	int ok;
	long long newval;
};

size_t clfwd_op_size(unsigned int collen, unsigned int klen,
		unsigned int vlen);

/* Build the frame; returns its length. */
size_t clfwd_op_build(unsigned char *msg, unsigned char type,
		const struct clfwd_op *f);

/* this node's FWD_OP as a whole frame, under M_FWD_OP (clmsg.h) */
size_t clfwd_op_frame(unsigned char *msg, uint32_t req, int node, int op,
		unsigned int ttl_rel, long long delta, const char *col,
		size_t collen, const char *key, size_t klen, const char *val,
		size_t vlen);

/* 0 when too short for the header or its col+key - a truncated forward is dropped,
 * not half-applied; vlen is the rest (0: a delete or an increment) */
int clfwd_op_parse(const unsigned char *pt, size_t n, struct clfwd_op *f);

size_t clfwd_ack_build(unsigned char *msg, unsigned char type,
		const struct clfwd_ack *a);
int clfwd_ack_parse(const unsigned char *pt, size_t n, struct clfwd_ack *a);

/* ---- DEMOTE [type1][winner2][collen1][klen2][col][key][ver8] tells a birth race's
 * loser to drop its copy; TOMBSTONE [type1][collen1][klen2][col][key] spreads a delete */
#define CLFWD_DEMOTE_HDR  6
#define CLFWD_TOMB_HDR    4

struct clfwd_key {
	const char *col;
	unsigned int collen;
	const char *key;
	unsigned int klen;
};

/* a demote carries the LOSER's version after the key, so it drops that copy and not
 * a later write; an older sender's frame parses with version 0 (removes as before) */
size_t clfwd_demote_build(unsigned char *msg, unsigned char type,
		unsigned int winner, const struct clfwd_key *k, uint64_t ver);
int clfwd_demote_parse(const unsigned char *pt, size_t n,
		unsigned int *winner, struct clfwd_key *k, uint64_t *ver);

/* a tombstone carries the delete's VERSION after the key; an older sender's frame
 * parses with ver 0 (unversioned: no guard) */
size_t clfwd_tomb_build(unsigned char *msg, unsigned char type,
		const struct clfwd_key *k, uint64_t ver);
int clfwd_tomb_parse(const unsigned char *pt, size_t n, struct clfwd_key *k,
		uint64_t *ver);
/* a replayed tombstone may flag an EXPIRY (an eager collection's reaped key) in a
 * flags byte after the version, written only when a flag is set; absent: flags 0 */
#define CLFWD_TOMB_F_EXPIRY 0x01u
/* with EXPIRY: the key's TTL had been shortened, so the receiver keeps it by the
 * time rule (clretain's kept expiry) */
#define CLFWD_TOMB_F_KEEP   0x04u
size_t clfwd_tomb_build_f(unsigned char *msg, unsigned char type,
		const struct clfwd_key *k, uint64_t ver, unsigned int flags);
int clfwd_tomb_parse_f(const unsigned char *pt, size_t n,
		struct clfwd_key *k, uint64_t *ver, unsigned int *flags);
/* a delete NUMBERED at its origin: (identity hash, incarnation - fresh each start, so
 * no seq repeats - seq 1, 2, ...), 20 bytes after the flags; absent or short: seq 0 */
#define CLFWD_TOMB_F_NUMBERED 0x02u
#define CLFWD_TOMB_NUM_LEN    20
#define CLFWD_TOMB_MAX (CLFWD_TOMB_HDR + 255 + 4096 + 8 + 1 + CLFWD_TOMB_NUM_LEN)
struct clfwd_tomb_num { uint64_t oid; uint32_t incarn; uint64_t seq; };
/* num NULL, or seq 0: unnumbered - the frame clfwd_tomb_build_f makes */
size_t clfwd_tomb_build_n(unsigned char *msg, unsigned char type,
		const struct clfwd_key *k, uint64_t ver, unsigned int flags,
		const struct clfwd_tomb_num *num);
int clfwd_tomb_parse_n(const unsigned char *pt, size_t n,
		struct clfwd_key *k, uint64_t *ver, unsigned int *flags,
		struct clfwd_tomb_num *num);

/* ---- FWD_JSON [type1][req4][node2][jop1][flags1][ttl4][by8][collen1][klen2][plen2]
 * [vlen4][col][key][path][val]: the holder runs the read-modify-write, acks the fragment */
#define CLFWD_JSON_HDR  30
#define CLFWD_JACK_HDR  23

/* the flags byte */
#define CLFWD_J_NX       1
#define CLFWD_J_XX       2
#define CLFWD_J_MKPATH   4
#define CLFWD_J_HAVETTL  8

struct clfwd_json {
	uint32_t req;
	unsigned int node;
	unsigned char jop;
	unsigned char flags;
	unsigned int ttl;
	long long by;
	const char *col;
	unsigned int collen;
	const char *key;
	unsigned int klen;
	const char *path;
	unsigned int plen;
	const char *val;
	unsigned int vlen;
};

/* FWD_JACK [type1][req4][st1][jop1][newval8][cnt4][fraglen4][frag];
 * st: 0 ok, 1 benign absent, 2 error, 3 wrong type */
struct clfwd_jack {
	uint32_t req;
	unsigned char st;
	unsigned char jop;
	long long newval;
	unsigned int cnt;
	const char *frag;
	unsigned int fraglen;
};

size_t clfwd_json_build(unsigned char *msg, unsigned char type,
		const struct clfwd_json *j);
int clfwd_json_parse(const unsigned char *pt, size_t n,
		struct clfwd_json *j);

size_t clfwd_jack_build(unsigned char *msg, unsigned char type,
		const struct clfwd_jack *a);
int clfwd_jack_parse(const unsigned char *pt, size_t n,
		struct clfwd_jack *a);

#endif /* PC_CLFWD_H */
