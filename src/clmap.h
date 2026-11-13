/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clmap.h - the cluster map: what the MASTER decided, versioned by epoch (term, seq), and the only
 * input to placement - `members` is what this node sees, and placing from it would split keys */
#ifndef PC_CLMAP_H
#define PC_CLMAP_H

#include <stddef.h>
#include <stdint.h>

/* wire: LE, no padding, no reserved fields (the version is bumped instead); 30 + 32 * 257 =
 * 8254 bytes at most, so a map always fits one sealed datagram and never fragments */
#define PC_CLMAP_MAGIC   0x504D4350u      /* 'PCMP' LE */
#define PC_CLMAP_VER     2
#define PC_CLMAP_HDR     30
#define PC_CLMAP_NODESZ  32

/* 256 peers + self, matching PC_CL_MAXPEER */
#define PC_CLMAP_MAXNODE 257
#define PC_CLMAP_MAXBYTES (PC_CLMAP_HDR + PC_CLMAP_NODESZ * PC_CLMAP_MAXNODE)

/* the node states as they travel: the map format OWNS this encoding, not the internal enum that
 * keeps moving; cluster.c asserts the two agree */
#define PC_CLMAP_ST_STARTING   0
#define PC_CLMAP_ST_RECOVERING 1
#define PC_CLMAP_ST_READY      2
#define PC_CLMAP_ST_DRAINING   3
/* 4 and up came without a format bump: an older peer compares the byte against READY, so it reads
 * an unknown state as not usable - the safe direction */
#define PC_CLMAP_ST_FAILED     4
#define PC_CLMAP_ST_STALLED    5
#define PC_CLMAP_ST_HEALING    6
/* never in a map */
#define PC_CLMAP_ST_CONNECTING 7

#define PC_CLMAP_MODE_STORE 0
#define PC_CLMAP_MODE_SHARD 1
#define PC_CLMAP_MODE_EC    2

/* fixed point, 1000 = 1.0.  admin is an ABSOLUTE OVERRIDE (a multiplier could not overload a full
 * node): UNSET = capacity decides, 0 = owns nothing (the drain), else admin decides */
#define PC_CLMAP_W_NOMINAL 1000u
#define PC_CLMAP_W_UNSET   0xFFFFu

struct pc_clmap_node {
	unsigned char ident[16];       /* stable across the node's restarts */
	uint16_t node_id;
	uint32_t addr;                 /* IPv4, heartbeat byte order */
	uint16_t cluster_port;
	uint16_t client_port;          /* where CLIENTS dial, 0 = unknown */
	uint8_t  state;                /* PC_NST_* */
	uint8_t  master_pref;          /* 0 = never a controller */
	uint16_t cap_weight;
	uint16_t admin_weight;
};

struct pc_clmap {
	uint32_t term, seq;
	uint16_t master_id, backup_id;
	uint8_t  mode, eager;
	uint64_t config_digest;
	uint16_t nnodes;
	struct pc_clmap_node node[PC_CLMAP_MAXNODE];
};

/* Encode @m into @buf.  Returns the byte count, or -1 if @cap is too
 * small or the map is malformed (which is a caller bug, not input). */
long pc_clmap_encode(const struct pc_clmap *m, unsigned char *buf, size_t cap);

/* decode @n bytes from the network, every field validated: 0, or -1 with *why a short literal
 * ("magic", "version", "short", "count", "trunc", "dup") for the log */
int pc_clmap_decode(const unsigned char *buf, size_t n, struct pc_clmap *out,
		const char **why);

/* <0, 0, >0 for a before/same/after b.  TERM WINS FIRST: it changes with every mastership and is
 * never reused, so a promoted backup never ties a partitioned master at the same seq */
int pc_clmap_epoch_cmp(uint32_t a_term, uint32_t a_seq,
		uint32_t b_term, uint32_t b_seq);

/* the weight placement actually uses: admin_weight when set, otherwise
 * capacity's; 0 means the node owns nothing */
uint32_t pc_clmap_weight(const struct pc_clmap_node *n);

/* eligible to hold keys: weight above zero AND READY.  A placed node that goes non-READY is NOT
 * dropped here - that would be the reshard; the master decides, by editing the map. */
int pc_clmap_placeable(const struct pc_clmap_node *n);

/* How many nodes in the map may hold keys. */
int pc_clmap_placeable_count(const struct pc_clmap *m);

/* Find by node id; NULL if absent. */
const struct pc_clmap_node *pc_clmap_find(const struct pc_clmap *m, uint16_t id);

#endif /* PC_CLMAP_H */
