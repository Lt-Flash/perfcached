/* SPDX-License-Identifier: GPL-2.0-or-later
 * clinit.h - the node identity file format and the config digest, pure (file I/O is cluster.c's) */
#ifndef PC_CLINIT_H
#define PC_CLINIT_H

#include <stddef.h>
#include <stdint.h>

#define CLINIT_IDENT_LEN     16
#define CLINIT_MAX_NAMES     64    /* the digest's own bound on the set */
#define CLINIT_IDENT_HEADER  "perfcached-node-identity 1"

/* FNV-1a over n bytes, for the identity file's check line; the constants are fnv1a.h's, the
 * published vectors pinned against THIS function in test/clinittest.c */
uint64_t clinit_fnv1a64(const unsigned char *p, size_t n);

/* 16 raw bytes -> 32 hex digits plus a NUL.  `out` needs 33 bytes. */
void clinit_ident_hex(const unsigned char *id, char *out);

/* 2*n hex digits -> n bytes.  Returns -1 on any non-hex digit. */
int clinit_hex2bin(const char *s, unsigned char *out, size_t n);

/* the file: "perfcached-node-identity 1", "identity <32 hex>", "check <16 hex>" (fnv1a64 of the 16
 * raw bytes); returns the length written, or 0 if the buffer is too small */
size_t clinit_ident_render(char *buf, size_t cap, const unsigned char *id);

/* parse it: 1 a current-format file, 0 the LEGACY bare 16 bytes (still read, so an upgrade keeps
 * the id), -1 refused with `why` naming the failed check */
int clinit_ident_parse(const unsigned char *buf, size_t len,
		unsigned char *id, char *why, size_t wlen);

/* the node id (1..1023) a fresh node PROPOSES at join, derived from its identity so it is the same
 * every start; the master arbitrates, so a collision is resolved rather than fatal */
int clinit_proposed_id(const unsigned char *id);

/* ---- the config digest: a join is REFUSED unless it matches, so CHANGING ANY OF THIS CHANGES
 * THE WIRE.  Folds mode, eager, 'W' 1 if a WAL, the contract + NUL, SORTED names + NUL each. */
struct clinit_digest_in {
	int mode;
	int eager;
	int wal;
	int replicas;                  /* K under mode = spread, else 0; folded ONLY when
	                                * non-zero, so other fleets keep their digest */
	const char *route_algo;
	const char *const *names;      /* collection names, any order */
	int n_names;
};

uint64_t clinit_config_digest(const struct clinit_digest_in *in);

#endif /* PC_CLINIT_H */
