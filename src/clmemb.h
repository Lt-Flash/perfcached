/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clmemb.h - membership frames; new fields go at the tail behind a length gate,
 * so an older peer reads the prefix it knows.  _write returns 0 if @cap is short. */
#ifndef PC_CLMEMB_H
#define PC_CLMEMB_H

#include <netinet/in.h>            /* struct in_addr: ASSIGN's member records */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "fnv1a.h"                   /* clmemb_col_hash() */

#define CLMEMB_IDENT_LEN     16

/* ---- ALIVE ------------------------------------------------------- */
#define CLMEMB_ALIVE_LEN     (88 + 56 * CLMEMB_COLS_BEAT + 15)  /* the LONGEST without the settings group: 551 */
/* the settings group, written only when there is one; buffers take this */
#define CLMEMB_CFG_MAX       24
#define CLMEMB_ALIVE_CFGMAX  (CLMEMB_ALIVE_LEN + 1 + 8 * CLMEMB_CFG_MAX)  /* 744: the longest without the watermarks group */
/* watermarks: a sender holding more than CLMEMB_MARKS_MAX sends a rotating slice;
 * ten keep the longest frame sealed (+ 79) inside the 1,232 bytes UDP IPv6 guarantees */
#define CLMEMB_MARKS_MAX     10
#define CLMEMB_MARK_LEN      20    /* oid 8, incarn 4, mark 8 */
#define CLMEMB_ALIVE_MAX     (CLMEMB_ALIVE_CFGMAX + 4 + 1 + 2 + CLMEMB_MARK_LEN * CLMEMB_MARKS_MAX + 2)  /* 953; + 79 sealed <= 1,232 */
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
/* the one non-additive change: a count was dropped and everything behind it
 * moved up four bytes, which is why PC_CL_EPOCH went to 2 */
#define CLMEMB_A_CLIENTS   0x0800u  /* cl_open .. cl_nresp      n >= 86 */
#define CLMEMB_A_COLS      0x1000u  /* ncols, ncols_total, cols n >= 88 + 56 * ncols */
/* after the variable collection block, so every older parser still ends where it did */
#define CLMEMB_A_PSRELAY   0x2000u  /* ps_port, ps_interest     n >= 88 + 56 * ncols + 10 */
/* why this member is not ready: a code, the wording lives on the reader's side;
 * absent reads as "no reason given" */
#define CLMEMB_A_REASON    0x4000u  /* nreason                  n >= ... + 1 */
#define CLMEMB_A_EPOCH     0x8000u  /* wire epoch               n >= ... + 2 */
/* what this member can carry, a bit each; absent reads as "none", which gates
 * new record types off a fleet that still has an older member */
#define CLMEMB_A_FEATURES  0x10000u /* features                 n >= ... + 3 */
/* a node with a reconcile open names its only live witness (0 none, _MANY two or
 * more), so a peer refuses a restart only when it is that one; absent = refuse */
#define CLMEMB_A_SOLEWIT   0x20000u /* sole_witness             n >= ... + 5 */
/* the settings members must agree on: per setting a 32-bit hash of its name and
 * of its value, so the one that differs is named; written only when n > 0 */
#define CLMEMB_A_CFGSIG    0x40000u /* ncfg, cfg_name/val       n >= ... + 6 + 8 * ncfg */
/* the 32-bit features word (low 8 bits = the byte) and this member's CONFIRMED mark
 * per space held (clack.h), sliced if they do not fit; the settings count precedes it */
#define CLMEMB_A_FEAT32    0x80000u /* features32, nspaces      n >= ... + 7 + 8 * ncfg */
/* the word's bits past the byte's eight */
#define CLMEMB_F32_ACKS    (1u << 8) /* counts numbered deletes and announces marks;
                                      * an origin forgets by marks only while all set it */
#define CLMEMB_A_MARKS     0x100000u /* nmarks pairs            n >= ... + 7 + 8 * ncfg + 20 * nmarks */
#define CLMEMB_A_LINK      0x200000u /* link_pct, link_level, after the marks */
#define CLMEMB_SOLEWIT_MANY 0xFFFF
#define CLMEMB_FEAT_TYPES2 0x01     /* rate-limit and hash records */
#define CLMEMB_FEAT_HREAD  0x02     /* answers a forwarded hash READ with the record */
#define CLMEMB_FEAT_SLOCK  0x04        /* lock records + the approved Symfony lock scripts */
#define CLMEMB_FEAT_STAGE  0x10        /* takes a staged build (CLBULK_KIND_STAGE) */
/* MAILER: this node is configured to mail and so accepts alert relays; being
 * configuration, not build, it must never gate a record type or a verb */
#define CLMEMB_FEAT_MAILER 0x08
#define CLMEMB_FEAT_RESPMAP 0x20       /* reads CLCOL_OP_MAP (an older build: a SET) */
#define CLMEMB_FEAT_HOLDQ  0x40        /* answers M_HOLD_Q; asked only when every live
                                        * member sets it (an older build drops it) */
#define CLMEMB_FEAT_AUTOSCALE 0x80     /* reads CLCOL_OP_AUTOSCALE (older: a SET);
                                        * the byte's last bit */
/* a collection's figures as the member counts them: CLIENT traffic only (a pull
 * served for a peer would count twice); named by FNV-1a, the config digest pins names */
#define CLMEMB_COLS_MAX 16
/* a frame carries this many, rotating through the first COLS_MAX - the longest
 * ALIVE then fits IPv6's minimum MTU (1,232 bytes of UDP) sealed */
#define CLMEMB_COLS_BEAT 8
struct clmemb_col {
	uint64_t hash;
	uint64_t entries, hits, misses, stores, removes, expired;
};
static inline uint64_t clmemb_col_hash(const char *name)
{
	return fnv1a64(name, strlen(name));
}

struct clmemb_mark { uint64_t oid; uint32_t incarn; uint64_t mark; };

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
	/* open client connections, total and by door; the page sums them fleet-wide */
	uint32_t cl_open, cl_resp, cl_bin, cl_nresp;
	/* the port this node reads pub/sub relays on (0 = only the cluster socket),
	 * and the interest version */
	int ps_port;
	uint64_t ps_interest;
	int nreason;                       /* PC_NRE_*, 0 = none */
	int epoch;                         /* PC_CL_EPOCH; 0 = absent */
	int features;                      /* CLMEMB_FEAT_*; 0 = absent */
	int sole_witness;                  /* see CLMEMB_A_SOLEWIT */
	int ncfg;                          /* see CLMEMB_A_CFGSIG */
	uint32_t cfg_name[CLMEMB_CFG_MAX], cfg_val[CLMEMB_CFG_MAX];
	/* the CLMEMB_A_FEAT32 group, written only with ext set */
	int ext;
	uint32_t features32;
	int nmarks, nspaces;
	struct clmemb_mark marks[CLMEMB_MARKS_MAX];
	/* the link's busier direction, % of its speed (-1 = not watched),
	 * and host.link's level (0 none, 1 warning, 2 critical) - with ext only */
	int link_pct, link_level;
	/* the first ncols of ncols_total live collections */
	int ncols, ncols_total;
	struct clmemb_col cols[CLMEMB_COLS_MAX];
};

/* @type is M_ALIVE; `have` is ignored on this side.  Returns the length
 * written, CLMEMB_ALIVE_MAX at most - what @buf needs. */
size_t clmemb_alive_write(unsigned char *buf, size_t cap, unsigned char type,
		const struct clmemb_alive *in);

/* rewrite state and reason in a built ALIVE: the watchdog re-sends the last frame,
 * which says READY even for a wedged cluster thread.  -1 if @len is too short. */
#define CLMEMB_ALIVE_NSTATE_OFF (43 + CLMEMB_IDENT_LEN)
int clmemb_alive_patch_state(unsigned char *buf, size_t len, int nstate,
		int nreason);

/* 0 and nothing touched if n < CLMEMB_ALIVE_MIN; absent fields stay zero with
 * their have bit clear - defaults are the caller's. */
int clmemb_alive_parse(const unsigned char *p, size_t n,
		struct clmemb_alive *out);

/* ---- MASTER_ALIVE ------------------------------------------------ */
#define CLMEMB_MALIVE_LEN    41
#define CLMEMB_MALIVE_MIN    17

#define CLMEMB_M_MEM       0x0001u  /* total_mb, live_kb        n >= 25 */
#define CLMEMB_M_CFG       0x0002u  /* mode, eager, cfg_digest  n >= 35 */
#define CLMEMB_M_TERM      0x0004u  /* term                     n >= 39 */
#define CLMEMB_M_EPOCH     0x0008u  /* wire epoch               n >= 40 */
#define CLMEMB_M_COMMIT    0x0010u  /* committed epoch          n >= 41 */

struct clmemb_malive {
	unsigned have;
	int node, members;
	/* the membership digest, every beat; a member whose own differs three beats
	 * running asks the master for the list */
	uint64_t member_digest;
	uint32_t free_mb, total_mb, live_kb;
	int mode, eager;
	uint64_t cfg_digest;
	uint32_t term;
	int epoch;
	int committed;                     /* the epoch the fleet speaks */
};

size_t clmemb_malive_write(unsigned char *buf, size_t cap, unsigned char type,
		const struct clmemb_malive *in);
int clmemb_malive_parse(const unsigned char *p, size_t n,
		struct clmemb_malive *out);

/* ---- JOIN_REQ ---- its gates are NOT in offset order (`proposed` at 35 is
 * gated at 37, read before the config block at 9): keep them exactly */
#define CLMEMB_JOINREQ_LEN   43
#define CLMEMB_JOINREQ_MIN    9

#define CLMEMB_J_CFG      0x01u  /* mode, eager, cfg_digest   n >= 19 */
#define CLMEMB_J_IDENT    0x02u  /* ident (16 bytes)          n >= 35 */
#define CLMEMB_J_PROP     0x04u  /* proposed                  n >= 37 */
#define CLMEMB_J_INCARN   0x08u  /* incarn                    n >= 41 */
#define CLMEMB_J_WAL      0x10u  /* wal posture               n >= 42 */
#define CLMEMB_J_EPOCH    0x20u  /* wire epoch                n >= 43 */

struct clmemb_joinreq {
	unsigned have;
	uint64_t tok;
	int mode, eager;
	uint64_t cfg_digest;
	const unsigned char *ident;        /* into the CALLER's frame */
	int proposed;
	uint32_t incarn;
	int wal;                           /* 1 = none, 2 = logs; 0 = unknown */
	int epoch;                         /* PC_CL_EPOCH; 0 = absent */
};

size_t clmemb_joinreq_write(unsigned char *buf, size_t cap, unsigned char type,
		const struct clmemb_joinreq *in);
int clmemb_joinreq_parse(const unsigned char *p, size_t n,
		struct clmemb_joinreq *out);

/* ---- ASSIGN ---- a 15-byte header, then `count` 20-byte records; address and
 * port are NETWORK order, copied verbatim off a sockaddr (all else here is LE) */
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
/* record @i, bounds-checked against the REAL frame length, not the header's
 * count; 0 when record @i is not wholly present */
int clmemb_member_at(const unsigned char *p, size_t n, int i,
		struct clmemb_member *out);

/* ---- JOIN_WAIT, JOIN_REJ, GOODBYE ---- GOODBYE is [type1][node2][incarn4]: id and
 * incarnation, so a replayed goodbye cannot expel a later run; older: 3 bytes */
#define CLMEMB_TOK_LEN        9
#define CLMEMB_REJ_LEN       10
#define CLMEMB_GOODBYE_LEN    7
#define CLMEMB_GOODBYE_OLDLEN 3

size_t clmemb_tok_write(unsigned char *buf, size_t cap, unsigned char type,
		uint64_t tok);
size_t clmemb_rej_write(unsigned char *buf, size_t cap, unsigned char type,
		uint64_t tok, int reason);
size_t clmemb_goodbye_write(unsigned char *buf, size_t cap, unsigned char type,
		int node, unsigned int incarn);
/* 1 and the id (and the incarnation, *have_incarn 1, from a build that
 * sends it), or 0 if @n is under CLMEMB_GOODBYE_OLDLEN */
int clmemb_goodbye_parse(const unsigned char *p, size_t n, int *node,
		unsigned int *incarn, int *have_incarn);

/* Both return 0 if n < CLMEMB_TOK_LEN.  *have_reason is 0 on a 9-byte
 * JOIN_REJ from a build that predates the byte. */
int clmemb_tok_parse(const unsigned char *p, size_t n, uint64_t *tok);
int clmemb_rej_parse(const unsigned char *p, size_t n, uint64_t *tok,
		int *reason, int *have_reason);

/* JOIN_WAIT [type1][tok8][reason1][term4][node2][count2]: QUEUED carries the master's
 * term, id and the joiner's place, ELECTING a member's term, id and the member count */
#define CLMEMB_WAIT_LEN       18
#define CLMEMB_WAIT_HELD       0       /* an identity like ours is still live there */
#define CLMEMB_WAIT_QUEUED     1
#define CLMEMB_WAIT_ELECTING   2
struct clmemb_wait {
	int reason;
	uint32_t term;
	int node, count;
};
/* a trailer behind an ASSIGN's records, [flags1][part1][parts1]; an older
 * joiner reads the records by count and never looks behind them */
#define CLMEMB_ASSIGN_TRAILER  3
#define CLMEMB_ASSIGN_CONNECT  0x01u   /* connect first: CONNECTING until the master's ONLINE */
struct clmemb_trailer {
	unsigned flags;
	int part, parts;
};
size_t clmemb_trailer_write(unsigned char *buf, size_t cap, const struct clmemb_trailer *t);
/* 1 and *t from the bytes behind @count records, 0 (and *t zero) if there are none */
int clmemb_trailer_parse(const unsigned char *p, size_t n, int count,
		struct clmemb_trailer *t);

size_t clmemb_wait_write(unsigned char *buf, size_t cap, unsigned char type,
		uint64_t tok, const struct clmemb_wait *w);
/* 1 and the token, *w from an 18-byte frame (a 9-byte one reads as HELD, the rest 0); 0 if
 * @n is under CLMEMB_TOK_LEN */
int clmemb_wait_parse(const unsigned char *p, size_t n, uint64_t *tok,
		struct clmemb_wait *w);

#endif /* PC_CLMEMB_H */
