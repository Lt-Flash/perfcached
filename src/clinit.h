/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * clinit.h - the node identity FILE format (M13, slice 1).
 *
 * A node's identity is 16 bytes that must survive a restart, so the
 * master can hand a rejoining node its old id back.  It is kept in a
 * small text file:
 *
 *     perfcached-node-identity 1
 *     identity <32 hex digits>
 *     check <16 hex digits>
 *
 * The check line is fnv1a64 over the 16 raw bytes, which catches an
 * edited or half-written file.  A bare 16-byte file is the LEGACY
 * format and is still read, so an upgrade does not lose a node's id.
 *
 * Parsing it is pure - bytes in, identity plus a reason out - and every
 * refusal has its own message, which is what makes the refusals
 * testable one by one rather than as "it returned -1".  The file I/O
 * (the temp file, the fsync, the rename) stays in cluster.c: that is
 * the durability dance, not the format.
 */
#ifndef PC_CLINIT_H
#define PC_CLINIT_H

#include <stddef.h>
#include <stdint.h>

#define CLINIT_IDENT_LEN     16
#define CLINIT_MAX_NAMES     64    /* the digest's own bound on the set */
#define CLINIT_IDENT_HEADER  "perfcached-node-identity 1"

/* FNV-1a over n bytes, used for the identity file's check line.  The
 * constants come from fnv1a.h, which carries the story of the typo they
 * used to be; the published vectors are pinned against THIS function in
 * test/clinittest.c.
 *
 * Consequence of that fix: an identity file written before 2026-09-13 no
 * longer validates, and a node whose file fails its check REFUSES TO
 * START.  Delete the file to mint a new identity - the daemon's own
 * message says so.  Accepted deliberately: this runs on a disposable
 * test fleet.
 */
uint64_t clinit_fnv1a64(const unsigned char *p, size_t n);

/* 16 raw bytes -> 32 hex digits plus a NUL.  `out` needs 33 bytes. */
void clinit_ident_hex(const unsigned char *id, char *out);

/* 2*n hex digits -> n bytes.  Returns -1 on any non-hex digit. */
int clinit_hex2bin(const char *s, unsigned char *out, size_t n);

/* Render the file's contents.  Returns the length written, or 0 if the
 * buffer is too small. */
size_t clinit_ident_render(char *buf, size_t cap, const unsigned char *id);

/* Parse it.
 *
 *    1  a well-formed current-format file
 *    0  the LEGACY bare 16 bytes
 *   -1  refused, with `why` saying which of the checks failed
 */
int clinit_ident_parse(const unsigned char *buf, size_t len,
		unsigned char *id, char *why, size_t wlen);

/* The node id a fresh node PROPOSES at join, derived from its identity
 * so that the same node suggests the same id every start.  1..1023; the
 * master arbitrates, so a collision is resolved rather than fatal.
 *
 * This inlined its own copy of the one-digit-short basis - the same typo
 * as the check hash, in a second place, which is how the typo survived.
 * It now uses the corrected hash like everything else.  A node therefore
 * proposes a DIFFERENT id than it would have before; harmless, because
 * the proposal is a suggestion the master may override, and because a
 * node's identity is its own so two nodes cannot collide by derivation.
 */
int clinit_proposed_id(const unsigned char *id);

/* ---- the config digest (M13, slice 2) ---------------------------------
 *
 * The number two nodes compare to decide whether they may federate at
 * all.  If it differs the join is REFUSED (S30), so this is the most
 * safety-critical arithmetic in the file: a digest that is too loose
 * lets incompatible nodes into one fleet, and one that is too strict -
 * or merely computed in a different order - splits a fleet that was
 * fine.
 *
 * What it folds, in this exact order:
 *   the cluster mode, the eager flag, then 'W' and 1 IF a WAL is
 *   configured, then the placement contract string and a NUL, then
 *   every collection name SORTED, each followed by a NUL.
 *
 * The sort is what makes it order-independent: two nodes that list
 * their collections differently in the config must still agree.
 *
 * Pure - the caller gathers the inputs from `C` and the store, this
 * folds them - which is what lets a test pin the value against golden
 * numbers computed independently of the implementation.
 *
 * CHANGING ANY OF THIS CHANGES THE WIRE.  A digest that differs from a
 * running fleet's refuses every join.
 */
struct clinit_digest_in {
	int mode;
	int eager;
	int wal;
	const char *route_algo;
	const char *const *names;      /* collection names, any order */
	int n_names;
};

uint64_t clinit_config_digest(const struct clinit_digest_in *in);

#endif /* PC_CLINIT_H */
