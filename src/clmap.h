/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clmap.h — the cluster map (control plane, step 1).
 *
 * The map is what the MASTER has decided, as opposed to what any single
 * node happens to see.  Placement is computed from it and from nothing
 * else: the moment two participants derive placement from different
 * inputs, ownership splits and a key lives in two places.
 *
 * That is the whole reason this is a distinct object from the members
 * list.  `members` is derived, local and liveness-driven - what THIS
 * node can see right now.  The map is authoritative, versioned and
 * published.  They legitimately differ (a node may see a peer the map
 * does not list yet), and that difference is diagnostic, not a bug - it
 * is the signature of a stale epoch or a partition.  Keeping both as
 * MACHINE interfaces is what would be a bug: someone would compute
 * placement from the derived one and be subtly wrong under partition.
 *
 * Ordering is by EPOCH = (term, seq).  A plain counter is not enough:
 * a partitioned master at seq 100 and a promoted backup continuing from
 * the same synced copy would both issue 101, and nothing could order
 * them.  Term increments on every mastership change and is never
 * reused, so a lower term always loses whatever its seq - and a master
 * that sees a higher term steps down immediately and unconditionally.
 *
 * Wire format (explicitly little-endian, no padding, no reserved
 * fields - the version is bumped instead):
 *
 *   header (30 bytes)
 *     u32  magic 'PCMP'
 *     u16  format version
 *     u32  term
 *     u32  seq
 *     u16  master node id      (0 = none, only valid while forming)
 *     u16  backup node id      (0 = none; see the no-standby window)
 *     u8   mode                (PC_CLMAP_MODE_*)
 *     u8   eager               (0/1)
 *     u64  config digest       (the existing collection-config check)
 *     u16  node count
 *
 *   then `node count` entries of 32 bytes:
 *     u8[16] identity          (stable across that node's restarts)
 *     u16  node id
 *     u32  advertise address   (IPv4, in the same byte order as the
 *                               heartbeat carries it)
 *     u16  cluster port
 *     u16  client port         (where CLIENTS dial, 0 = unknown)
 *     u8   node state          (PC_NST_*)
 *     u8   master preference   (0 = never a controller)
 *     u16  capacity weight     (master-owned, 1000 = nominal)
 *     u16  admin weight        (operator-owned: 0xFFFF = unset, 0 = owns
 *                               nothing, else an absolute override)
 *
 * 30 + 32*257 = 8254 bytes for a maximum fleet, so a map always fits one
 * sealed datagram (MAX_DGRAM 65000) and never needs fragmenting.  That
 * is checked, not assumed - see PC_CLMAP_MAXBYTES.
 */
#ifndef PC_CLMAP_H
#define PC_CLMAP_H

#include <stddef.h>
#include <stdint.h>

#define PC_CLMAP_MAGIC   0x504D4350u      /* 'PCMP' LE */
#define PC_CLMAP_VER     2
#define PC_CLMAP_HDR     30
#define PC_CLMAP_NODESZ  32

/* 256 peers + self, matching PC_CL_MAXPEER */
#define PC_CLMAP_MAXNODE 257
#define PC_CLMAP_MAXBYTES (PC_CLMAP_HDR + PC_CLMAP_NODESZ * PC_CLMAP_MAXNODE)

/* The node states, as they travel on the wire.  The map format OWNS this
 * encoding rather than borrowing the internal enum: the internal one is
 * still moving (STARTING was added, JOINING moved to the membership
 * axis, RECONCILING was merged away), and a wire format that shifts
 * every time an enum is reordered is not a format.  cluster.c asserts
 * the two agree. */
#define PC_CLMAP_ST_STARTING   0
#define PC_CLMAP_ST_RECOVERING 1
#define PC_CLMAP_ST_READY      2
#define PC_CLMAP_ST_DRAINING   3

#define PC_CLMAP_MODE_STORE 0
#define PC_CLMAP_MODE_SHARD 1
#define PC_CLMAP_MODE_EC    2

/* Weights are fixed-point with 1000 = 1.0.  Two independent numbers, so
 * the effective weight stays explainable: capacity says X, the operator
 * says Y, and which one placement used is never in doubt.
 *
 * The operator's is an ABSOLUTE OVERRIDE, not a multiplier.  A
 * multiplier was tried first and does not meet the requirement: with
 * capacity at 10 (a nearly full node) even the largest u16 multiplier
 * lands at 600, still below nominal - so an operator could not
 * deliberately overload a full node, which is precisely the case they
 * are entitled to.  Found by the test, not by reading.
 *
 *   admin == PC_CLMAP_W_UNSET   capacity decides (the default)
 *   admin == 0                  owns nothing - the drain primitive; the
 *                               node keeps serving what it holds and is
 *                               handed no new keys
 *   otherwise                   admin decides, capacity ignored
 *
 * The cost is that "reduce this node a little" needs the operator to
 * look at the capacity weight first - which the map publishes, so it is
 * visible rather than guessed. */
#define PC_CLMAP_W_NOMINAL 1000u
#define PC_CLMAP_W_UNSET   0xFFFFu

struct pc_clmap_node {
	unsigned char ident[16];
	uint16_t node_id;
	uint32_t addr;                 /* IPv4, heartbeat byte order */
	uint16_t cluster_port;
	uint16_t client_port;
	uint8_t  state;                /* PC_NST_* */
	uint8_t  master_pref;
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

/* Decode @n bytes into @out.  Returns 0, or -1 with *why set to a short
 * literal ("magic", "version", "short", "count", "trunc", "dup") - the
 * reason belongs in a log line, so a refused map is never silent.
 * Every field is validated: this parses data that arrived over a
 * network, from a node that may be running anything. */
int pc_clmap_decode(const unsigned char *buf, size_t n, struct pc_clmap *out,
		const char **why);

/* Order two epochs.  <0, 0, >0 for a before/same/after b.  TERM WINS
 * FIRST, always - that is the entire point of having one. */
int pc_clmap_epoch_cmp(uint32_t a_term, uint32_t a_seq,
		uint32_t b_term, uint32_t b_seq);

/* The weight placement actually uses: the operator's if they set one,
 * otherwise capacity's.  0 means the node owns nothing. */
uint32_t pc_clmap_weight(const struct pc_clmap_node *n);

/* Nodes eligible to hold keys: weight above zero AND state READY.  A
 * node that has never been READY is simply absent from placement, which
 * costs nothing; one that is already placed and goes non-READY is NOT
 * silently dropped here - removing it from the candidate set would BE
 * the reshard we are trying to avoid.  That decision belongs to the
 * master, expressed by editing the map. */
int pc_clmap_placeable(const struct pc_clmap_node *n);

/* How many nodes in the map may hold keys. */
int pc_clmap_placeable_count(const struct pc_clmap *m);

/* Find by node id; NULL if absent. */
const struct pc_clmap_node *pc_clmap_find(const struct pc_clmap *m, uint16_t id);

#endif /* PC_CLMAP_H */
