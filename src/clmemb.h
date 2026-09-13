/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clmemb.h - the membership keepalive frames (M12, slice 2).
 *
 * ALIVE (every node, 1 Hz) and MASTER_ALIVE (the master, 1 Hz) are the
 * two highest-rate frames in the daemon and the two that have grown the
 * most: both are ADDITIVE, each new field appended at the tail behind a
 * length gate so a peer built before it reads the prefix it knows and
 * ignores the rest.  ALIVE is up to sixteen fields and eleven gates.
 *
 * That growth is exactly why the layout is worth having in one place.
 * The gates were written one at a time at two ends of a 6,000-line file
 * - built in send_alive, read in handle_alive - and every one of them
 * is an off-by-one that says nothing when it is wrong: a gate one byte
 * too low reads a field out of a frame that never carried it, a gate
 * one too high silently drops a field that did.  Neither logs, neither
 * crashes, and the fleet keeps running with one node's view of another
 * quietly wrong.
 *
 * So parse reports PRESENCE, never a default.  The `have` mask says
 * which fields the frame actually carried, and cluster.c applies its own
 * defaults (PC_NST_READY, PC_START_UNKNOWN, 0) on top - those are its
 * policy, not the wire's.  A test can then hand the parser a frame of
 * every length from 0 to the full one and assert exactly which fields
 * appear at which byte.
 *
 * 64-bit fields are packed as two little-endian halves rather than
 * memcpy'd from a host uint64, matching clfwd and clcodec.  On every
 * host this daemon has run on that is byte-identical to what it did
 * before; on a big-endian one the old code put native bytes on a wire
 * that is little-endian everywhere else.
 *
 * Every _write takes the buffer's CAPACITY and returns the number of
 * bytes it actually wrote, 0 if the buffer was too small - so the
 * CLMEMB_*_LEN values below size a buffer rather than being a total that
 * has to be kept true by hand.  Positions come from a cursor
 * (clcodec.h): order decides them, not arithmetic.
 *
 * NOT extracted: everything the handlers DO with these fields - the
 * config-mismatch refusal, the split-brain cure, peer_publish, the
 * client announcements.  That is orchestration over C and it stays in
 * cluster.c, the same line M10, M11 and M13 drew.
 */
#ifndef PC_CLMEMB_H
#define PC_CLMEMB_H

#include <netinet/in.h>            /* struct in_addr: ASSIGN's member records */
#include <stddef.h>
#include <stdint.h>

#define CLMEMB_IDENT_LEN     16

/* ---- ALIVE ------------------------------------------------------- */
#define CLMEMB_ALIVE_LEN     70
#define CLMEMB_ALIVE_MIN      7    /* below this the frame is refused */

#define CLMEMB_A_MEM       0x0001u  /* total_mb, live_kb        n >= 15 */
#define CLMEMB_A_CFG       0x0002u  /* mode, eager, cfg_digest  n >= 25 */
#define CLMEMB_A_CLIPORT   0x0004u  /* client_port              n >= 27 */
#define CLMEMB_A_IDENT     0x0008u  /* ident, incarn            n >= 47 */
#define CLMEMB_A_ENTRIES   0x0010u  /* entries                  n >= 51 */
#define CLMEMB_A_LAMPORT   0x0020u  /* lamport                  n >= 59 */
#define CLMEMB_A_STATE     0x0040u  /* nstate                   n >= 60 */
#define CLMEMB_A_TIER      0x0080u  /* mem_tier                 n >= 61 */
#define CLMEMB_A_RESPPORT  0x0100u  /* resp_port                n >= 63 */
#define CLMEMB_A_STARTKIND 0x0200u  /* start_kind               n >= 64 */
#define CLMEMB_A_HTTP      0x0400u  /* http_port, uptime_s      n >= 70 */

struct clmemb_alive {
	unsigned have;
	int node;
	uint32_t free_mb, total_mb, live_kb;
	int mode, eager;
	uint64_t cfg_digest;
	int client_port;
	const unsigned char *ident;        /* into the CALLER's frame */
	uint32_t incarn, entries;
	uint64_t lamport;
	int nstate, mem_tier, resp_port, start_kind, http_port;
	uint32_t uptime_s;
};

/* Build a full-length ALIVE.  @type is the caller's M_ALIVE; the frame
 * always carries every field, so `have` is ignored on this side.
 * Returns CLMEMB_ALIVE_LEN; @buf needs that many bytes. */
size_t clmemb_alive_write(unsigned char *buf, size_t cap, unsigned char type,
		const struct clmemb_alive *in);

/* Parse one.  Returns 0 and touches nothing if n < CLMEMB_ALIVE_MIN.
 * Absent fields are left ZERO and their have bit clear - the caller's
 * defaults are the caller's business. */
int clmemb_alive_parse(const unsigned char *p, size_t n,
		struct clmemb_alive *out);

/* ---- MASTER_ALIVE ------------------------------------------------ */
#define CLMEMB_MALIVE_LEN    39
#define CLMEMB_MALIVE_MIN    17

#define CLMEMB_M_MEM       0x0001u  /* total_mb, live_kb        n >= 25 */
#define CLMEMB_M_CFG       0x0002u  /* mode, eager, cfg_digest  n >= 35 */
#define CLMEMB_M_TERM      0x0004u  /* term                     n >= 39 */

struct clmemb_malive {
	unsigned have;
	int node, members;
	/* the membership digest.  cluster.c writes it on every beat and
	 * reads it nowhere - decoded here because the frame carries it,
	 * not because anything consumes it yet. */
	uint64_t member_digest;
	uint32_t free_mb, total_mb, live_kb;
	int mode, eager;
	uint64_t cfg_digest;
	uint32_t term;
};

size_t clmemb_malive_write(unsigned char *buf, size_t cap, unsigned char type,
		const struct clmemb_malive *in);
int clmemb_malive_parse(const unsigned char *p, size_t n,
		struct clmemb_malive *out);

/* ---- JOIN_REQ ----------------------------------------------------
 *
 * The joiner states its whole position up front (S30): who it is, what
 * collections it believes in, which id it would like.  Additive like the
 * keepalives, and its gates are NOT in offset order - `proposed` at 35
 * is gated at 37 and read before the config block at 9.  That is worth
 * preserving exactly rather than tidying, so the gates are listed here
 * against their offsets and the test walks every length. */
#define CLMEMB_JOINREQ_LEN   42
#define CLMEMB_JOINREQ_MIN    9

#define CLMEMB_J_CFG      0x01u  /* mode, eager, cfg_digest   n >= 19 */
#define CLMEMB_J_IDENT    0x02u  /* ident (16 bytes)          n >= 35 */
#define CLMEMB_J_PROP     0x04u  /* proposed                  n >= 37 */
#define CLMEMB_J_INCARN   0x08u  /* incarn                    n >= 41 */
#define CLMEMB_J_WAL      0x10u  /* wal posture               n >= 42 */

struct clmemb_joinreq {
	unsigned have;
	uint64_t tok;
	int mode, eager;
	uint64_t cfg_digest;
	const unsigned char *ident;        /* into the CALLER's frame */
	int proposed;
	uint32_t incarn;
	int wal;                           /* 1 = none, 2 = logs; 0 = unknown */
};

size_t clmemb_joinreq_write(unsigned char *buf, size_t cap, unsigned char type,
		const struct clmemb_joinreq *in);
int clmemb_joinreq_parse(const unsigned char *p, size_t n,
		struct clmemb_joinreq *out);

/* ---- ASSIGN ------------------------------------------------------
 *
 * A 15-byte header then `count` fixed 20-byte member records.  The
 * count is written AFTER the walk, because the sender skips the
 * addressee and any peer that is not live, so it cannot know it in
 * advance - hence the separate _count_set.
 *
 * The address and port in a record are NETWORK ORDER and are copied
 * VERBATIM, never packed little-endian: they come off a sockaddr and go
 * straight back into one.  Everything else in this file is LE; these
 * two are the exception and that is deliberate. */
#define CLMEMB_ASSIGN_HDR    15
#define CLMEMB_ASSIGN_REC    20
#define CLMEMB_ASSIGN_MIN    15

struct clmemb_assign {
	uint64_t tok;
	int your_id, master_id, count;
};

struct clmemb_member {
	int node;
	struct in_addr addr;               /* network order, verbatim */
	uint16_t port;                     /* network order, verbatim */
	uint32_t free_mb, total_mb, live_kb;
};

size_t clmemb_assign_hdr_write(unsigned char *buf, size_t cap, unsigned char type,
		const struct clmemb_assign *in);
/* patch the count in once the walk is done */
void clmemb_assign_count_set(unsigned char *buf, int count);
size_t clmemb_member_write(unsigned char *buf, size_t cap,
		const struct clmemb_member *m);

int clmemb_assign_parse(const unsigned char *p, size_t n,
		struct clmemb_assign *out);
/* Record @i, bounds-checked against the REAL frame length rather than
 * the header's count - a count of 100 in a 40-byte frame must read two
 * records and stop, not overrun.  Returns 0 when record @i is not
 * wholly present. */
int clmemb_member_at(const unsigned char *p, size_t n, int i,
		struct clmemb_member *out);

/* ---- JOIN_WAIT, JOIN_REJ, GOODBYE --------------------------------
 *
 * JOIN_WAIT is a bare token; JOIN_REJ is a token plus a reason byte that
 * older builds do not send.  GOODBYE carries the departing node id at
 * offset 1 and NOTHING READS IT - cluster.c matches the sender by
 * address instead ("the address is the identity"), so there is no
 * parse for it.  It is written because the frame has always carried it. */
#define CLMEMB_TOK_LEN        9
#define CLMEMB_REJ_LEN       10
#define CLMEMB_GOODBYE_LEN    3

size_t clmemb_tok_write(unsigned char *buf, size_t cap, unsigned char type,
		uint64_t tok);
size_t clmemb_rej_write(unsigned char *buf, size_t cap, unsigned char type,
		uint64_t tok, int reason);
size_t clmemb_goodbye_write(unsigned char *buf, size_t cap, unsigned char type,
		int node);

/* Both return 0 if n < CLMEMB_TOK_LEN.  *have_reason is 0 on a 9-byte
 * JOIN_REJ from a build that predates the byte. */
int clmemb_tok_parse(const unsigned char *p, size_t n, uint64_t *tok);
int clmemb_rej_parse(const unsigned char *p, size_t n, uint64_t *tok,
		int *reason, int *have_reason);

#endif /* PC_CLMEMB_H */
