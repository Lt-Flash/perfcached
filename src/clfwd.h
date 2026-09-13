/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * clfwd.h - the proxy write plane's WIRE FRAMES (M11, slice 1).
 *
 * A write that arrives at a node which does not own the key is
 * FORWARDED to the holder, which applies it and acks with the outcome.
 * Two frames carry that:
 *
 *   FWD_OP   [type1][req4][node2][op1][ttl4][delta8][collen1][klen2]
 *            [col][key][val]                       - 23 byte header
 *   FWD_ACK  [type1][req4][ok1][newval8]           - 14 bytes
 *
 * Both were built and decoded by hand at opposite ends of cluster.c, so
 * the layout lived in two places that had to change together - the same
 * shape M10 slice 3 found in the migration record.  Here it is one, and
 * a test round-trips it.
 *
 * The message TYPE is passed in rather than baked in: the protocol's
 * constants belong with the protocol, in cluster.c, and the JSON
 * forward reuses the same shape with a different type byte.
 *
 * A parsed frame POINTS INTO the datagram - valid only while it is.
 */
#ifndef PC_CLFWD_H
#define PC_CLFWD_H

#include <stddef.h>
#include <stdint.h>

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

/* Parse one.  Returns 0 when the datagram is too short for the header
 * or for the col+key it declares - a truncated forward is dropped, not
 * half-applied.  vlen is whatever remains, which is how the wire says
 * "no value" for a delete or an increment. */
int clfwd_op_parse(const unsigned char *pt, size_t n, struct clfwd_op *f);

size_t clfwd_ack_build(unsigned char *msg, unsigned char type,
		const struct clfwd_ack *a);
int clfwd_ack_parse(const unsigned char *pt, size_t n, struct clfwd_ack *a);

/* ---- the ownership-correction frames (M11, slice 3) -------------------
 *
 * Two more frames carry a col+key and nothing else of substance:
 *
 *   DEMOTE     [type1][winner2][collen1][klen2][col][key]   - 6 byte hdr
 *   TOMBSTONE  [type1][collen1][klen2][col][key]            - 4 byte hdr
 *
 * DEMOTE settles a birth race: two nodes ended up holding one key, the
 * lower id wins, and the loser is told to drop its copy and point at
 * the winner.  TOMBSTONE broadcasts a delete so a peer's negative cache
 * learns it without a round trip.
 *
 * They are near-identical - the demote carries the winning node id and
 * the tombstone does not - and each was built at one end of cluster.c
 * and decoded at the other, like every frame this module has taken.
 * Kept as two named pairs rather than one generic call with a nullable
 * field: the shapes really are different and naming them says which.
 */
#define CLFWD_DEMOTE_HDR  6
#define CLFWD_TOMB_HDR    4

struct clfwd_key {
	const char *col;
	unsigned int collen;
	const char *key;
	unsigned int klen;
};

size_t clfwd_demote_build(unsigned char *msg, unsigned char type,
		unsigned int winner, const struct clfwd_key *k);
int clfwd_demote_parse(const unsigned char *pt, size_t n,
		unsigned int *winner, struct clfwd_key *k);

size_t clfwd_tomb_build(unsigned char *msg, unsigned char type,
		const struct clfwd_key *k);
int clfwd_tomb_parse(const unsigned char *pt, size_t n, struct clfwd_key *k);

/* ---- the JSON forward frames (M11, slice 4) ---------------------------
 *
 *   FWD_JSON  [type1][req4][node2][jop1][flags1][ttl4][by8][collen1]
 *             [klen2][plen2][vlen4][col][key][path][val]  - 30 byte hdr
 *   FWD_JACK  [type1][req4][st1][jop1][newval8][cnt4][fraglen4][frag]
 *                                                         - 23 byte hdr
 *
 * A JSON write against a key this node does not own is forwarded whole
 * - operation, path, flags and all - and the holder runs the read-
 * modify-write and returns the resulting fragment.
 *
 * `by` and `newval` were written with a raw `memcpy` of the host's
 * eight bytes, under a comment reading "LE hosts only, like delta".
 * That comment was wrong about delta, which packs explicitly and is
 * portable.  These now pack explicitly too: byte-identical on every
 * little-endian target (all of them, in the matrix) and correct on any
 * other, which the memcpy was not.
 */
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
