/* SPDX-License-Identifier: GPL-2.0-or-later
 * clmig.h - the migration plane's in-flight window, gather buffer and wire codecs; peer thread. */
#ifndef PC_CLMIG_H
#define PC_CLMIG_H

#include <stddef.h>
#include <stdint.h>

/* 3 x ~56 KB datagrams fit the default 208 KB rmem */
#define CLMIG_WINDOW    3
#define CLMIG_RETX      2          /* re-sends before a datagram is lost */
#define CLMIG_RETX_MS   1500

struct clmig_slot {
	uint32_t req;                  /* 0 = free */
	uint16_t recs;                 /* records in this datagram */
	size_t off;                    /* its datagram, still in the caller's
	                                * gather buffer */
	long long sent_ms;             /* last (re)send */
	unsigned char retx;            /* re-sends so far */
};

struct clmig {
	struct clmig_slot out[CLMIG_WINDOW * 2];
	unsigned long long retx_total;
};

void clmig_reset(struct clmig *m);

/* remember a datagram as outstanding; 0 = the window is full: stop gathering and drain */
int clmig_track(struct clmig *m, uint32_t req, uint16_t recs, size_t off,
		long long now);

/* retire by request id: the records it held, or 0 for an ack naming nothing held (a duplicate,
 * or a datagram already given up on) */
uint16_t clmig_ack(struct clmig *m, uint32_t req);

/* re-send through @f (the caller's, so a test needs no socket) every datagram past CLMIG_RETX_MS
 * with re-sends left; returns how many */
typedef void (*clmig_resend_fn)(void *ctx, size_t off);
int clmig_retransmit(struct clmig *m, long long now, clmig_resend_fn f,
		void *ctx);

/* RECORDS still outstanding - one lost group carries many - what a run loses if it stops here;
 * the window is not cleared */
unsigned long clmig_lost_records(const struct clmig *m);

int clmig_inflight(const struct clmig *m);


/* the gather buffer: groups [len2][group header][record]..., len2 counting what follows, one
 * group per datagram; the caller stamps headers and records */
#define CLMIG_NO_GROUP  ((size_t)-1)

struct clmig_buf {
	unsigned char *b;
	size_t cap;                    /* the batch ceiling */
	size_t len;                    /* bytes used */
	size_t open;                   /* the open group's len2 header, or
	                                * CLMIG_NO_GROUP */
};

int  clmig_buf_init(struct clmig_buf *g, size_t cap);
void clmig_buf_free(struct clmig_buf *g);
void clmig_buf_reset(struct clmig_buf *g);

/* room for a record of @need bytes: 1 a NEW group opened (stamp *hdr), 0 the open group takes it,
 * -1 the batch is full; a group closes before passing @gather_cap, the datagram ceiling */
int clmig_open_for(struct clmig_buf *g, size_t need, size_t ghdr,
		size_t gather_cap, unsigned char **hdr);

/* where to write the record, valid until the next call */
unsigned char *clmig_record_at(struct clmig_buf *g);

/* account a written record: the buffer, the open group's length and its record count (at
 * @cnt_off in the group header) all grow */
void clmig_record_done(struct clmig_buf *g, size_t n, size_t cnt_off);

/* walk the finished groups from off = 0; 0 at the end.  *p is the datagram
 * (past the len2) and *n its length. */
int clmig_group_walk(const struct clmig_buf *g, size_t *off,
		const unsigned char **p, unsigned int *n);

/* a migration record: [ttl4][collen1][klen2][vlen4][ver8][flags1][col][key][val]; a parsed one
 * POINTS INTO the datagram, valid only while that buffer is */
#define CLMIG_RHDR  20             /* ttl4+collen1+klen2+vlen4+ver8+flags1 */
#define CLMIG_RFLAGS (CLMIG_RHDR - 1)
/* wire only, never stored: a spread non-holder hands the record to the holder whose ack drops its
 * copy, and that holder keeps it as its own - authored, on its WAL, repaired by its sweep */
#define CLMIG_F_HANDOFF 0x40

struct clmig_rec {
	unsigned int ttl_left;
	const char *col;
	unsigned int collen;
	const char *key;
	unsigned int klen;
	const char *val;
	unsigned int vlen;
	unsigned long long ver;
	unsigned char flags;           /* the record's type (PCACHE_F_JSON) */
};

/* bytes one record occupies */
size_t clmig_rec_size(unsigned int collen, unsigned int klen,
		unsigned int vlen);

void clmig_rec_write(unsigned char *p, const struct clmig_rec *r);

/* parse the record at *off, advancing it; 0 when the datagram is exhausted OR the record is
 * truncated - the walk stops rather than read past it, and the caller acks what it stored */
int clmig_rec_parse(const unsigned char *p, size_t n, size_t *off,
		struct clmig_rec *r);

/* the group header, so the receive side stops hand-decoding it too */
uint32_t clmig_group_req(const unsigned char *g);
unsigned int clmig_group_count(const unsigned char *g);
unsigned int clmig_group_node(const unsigned char *g);


/* a GROUP (M_MIGRATE_MANY / M_REPL_MANY): [type1][req4][node2][count2] then records; a SINGLE
 * (M_MIGRATE) is not a group of one: [type1][req4][node2][ttl4][collen1][klen2] col key value */
#define CLMIG_GHDR   9
#define CLMIG_SHDR   14
#define CLMIG_ACK_MIN 6            /* type+req4+ok1: a single record's */
#define CLMIG_ACK_LEN 8            /* ... +stored2, a group's: credits migrated_out */

struct clmig_single {
	uint32_t req;
	int node;
	uint32_t ttl_left;
	const char *col;
	unsigned int collen;
	const char *key;
	unsigned int klen;
	const char *val;
	unsigned int vlen;
};

/* the group header; @count 0 while gathering (clmig_record_done patches it), or the final count
 * for a group sealed in one go */
size_t clmig_group_hdr(unsigned char *buf, size_t cap, unsigned char type,
		uint32_t req, int node, unsigned int count);

size_t clmig_single_size(unsigned int collen, unsigned int klen,
		unsigned int vlen);
size_t clmig_single_write(unsigned char *buf, size_t cap, unsigned char type,
		const struct clmig_single *s);
/* 0 unless the header and the col+key it declares are wholly present;
 * vlen is whatever remains */
int clmig_single_parse(const unsigned char *p, size_t n,
		struct clmig_single *s);

/* @stored NULL writes the 6-byte form, otherwise the 8-byte one */
size_t clmig_ack_write(unsigned char *buf, size_t cap, unsigned char type,
		uint32_t req, int ok, const unsigned int *stored);
/* either form - an older peer sends the short one, *stored 0 */
int clmig_ack_parse(const unsigned char *p, size_t n, uint32_t *req,
		int *ok, unsigned int *stored);


/* the record header alone, for the bulk plane's TCP stream: read CLMIG_RHDR bytes, learn the
 * three lengths, then read the body */
void clmig_rec_hdr_write(unsigned char *p, const struct clmig_rec *r);
/* @p must hold CLMIG_RHDR bytes; col/key/val are NOT set */
void clmig_rec_hdr_parse(const unsigned char *p, struct clmig_rec *r);

#endif /* PC_CLMIG_H */
