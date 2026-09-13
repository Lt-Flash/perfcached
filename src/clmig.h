/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * clmig.h - the migration plane's IN-FLIGHT WINDOW (M10, slice 1).
 *
 * A migration sends gathered datagrams and waits for per-datagram acks.
 * The window remembers what is outstanding so that a loss is accounted
 * in RECORDS, not datagrams: one lost gather group can carry many.
 *
 * This slice owns the window and nothing else.  The gather buffer
 * (`migb`) still lives in cluster.c, so clmig_retransmit() hands the
 * caller the OFFSET of each datagram due for re-send and lets it do the
 * sending.  That keeps the slice small and, more usefully, makes the
 * whole thing testable without a socket: a test passes its own resend
 * function and asserts exactly which offsets went out and when.
 *
 * M10's measurement (doc/MODULARITY.md) put clmig's only genuine
 * dependency at `seal_send`; handing the send back to the caller is how
 * this slice avoids it entirely.
 *
 * Peer thread only - a migration runs on the tick.
 */
#ifndef PC_CLMIG_H
#define PC_CLMIG_H

#include <stddef.h>
#include <stdint.h>

/* 3 x ~56KB fitted the DEFAULT 208KB rmem; see the note in
 * doc/MODULARITY.md now that the bench hosts run an 8MB rmem_max. */
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

/* Remember a datagram as outstanding.  Returns 0 when the window is
 * full, which is the caller's signal to stop gathering and drain. */
int clmig_track(struct clmig *m, uint32_t req, uint16_t recs, size_t off,
		long long now);

/* Retire by request id.  Returns the record count that was outstanding,
 * or 0 if this ack names nothing we hold - a duplicate ack, or one for a
 * datagram already given up on. */
uint16_t clmig_ack(struct clmig *m, uint32_t req);

/* Re-send every datagram past CLMIG_RETX_MS that has re-sends left.
 * `f` does the sending.  Returns how many were re-sent. */
typedef void (*clmig_resend_fn)(void *ctx, size_t off);
int clmig_retransmit(struct clmig *m, long long now, clmig_resend_fn f,
		void *ctx);

/* How many RECORDS are still outstanding - what a run loses if it stops
 * here.  Does not clear the window; clmig_reset does that. */
unsigned long clmig_lost_records(const struct clmig *m);

int clmig_inflight(const struct clmig *m);


/* ---- the gather buffer (M10, slice 2) ---------------------------------
 *
 * Migration records are GATHERED rather than sent one per datagram: a
 * 256 B record then costs about 1/200th of a datagram+ack cycle instead
 * of a whole one.  The buffer holds a sequence of groups
 *
 *     [len2][group header][record][record]...
 *
 * where len2 counts the header and the records after it.  One group
 * becomes one datagram.  The buffer knows about groups and sizes; it
 * knows NOTHING about what a record or a group header contains - the
 * caller stamps those - which is what keeps the migration wire format
 * in cluster.c where the rest of the protocol lives.
 *
 * Peer thread only.
 */
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

/* Make room for one record of `need` bytes.
 *
 *   1  a NEW group was opened; *hdr points at its header, which the
 *      caller must stamp (the buffer does not know the format)
 *   0  the already-open group takes it
 *  -1  the batch is full; stop gathering
 *
 * A group is closed when one more record would push it past
 * `gather_cap`, which is the datagram ceiling.
 */
int clmig_open_for(struct clmig_buf *g, size_t need, size_t ghdr,
		size_t gather_cap, unsigned char **hdr);

/* Where to write the record, valid until the next call. */
unsigned char *clmig_record_at(struct clmig_buf *g);

/* Account a written record: the buffer grows, and the open group's
 * length and record count are both bumped.  `cnt_off` is where the
 * count sits inside the group header. */
void clmig_record_done(struct clmig_buf *g, size_t n, size_t cnt_off);

/* Walk the finished groups.  Start with off = 0; returns 0 at the end.
 * *p is the datagram (past the len2) and *n its length. */
int clmig_group_walk(const struct clmig_buf *g, size_t *off,
		const unsigned char **p, unsigned int *n);

/* ---- the record codec (M10, slice 3) ----------------------------------
 *
 * One migration record on the wire:
 *
 *     [ttl4][collen1][klen2][vlen4][ver8][col][key][val]
 *
 * The sender built this by hand in the walk callback and the receiver
 * decoded it by hand in the datagram handler, so the format lived in two
 * places that had to be changed together.  Here it is one, and a test
 * can round-trip it: write records, parse them back, compare.
 *
 * A parsed record POINTS INTO the datagram - no copying - so it is valid
 * only while that buffer is.
 */
#define CLMIG_RHDR  19             /* ttl4+collen1+klen2+vlen4+ver8 */

struct clmig_rec {
	unsigned int ttl_left;
	const char *col;
	unsigned int collen;
	const char *key;
	unsigned int klen;
	const char *val;
	unsigned int vlen;
	unsigned long long ver;
};

/* bytes one record occupies */
size_t clmig_rec_size(unsigned int collen, unsigned int klen,
		unsigned int vlen);

void clmig_rec_write(unsigned char *p, const struct clmig_rec *r);

/* Parse the record at *off, advancing it.  Returns 0 when the datagram
 * is exhausted OR the record is truncated - a short datagram stops the
 * walk rather than reading past it, and the caller acks what it stored.
 */
int clmig_rec_parse(const unsigned char *p, size_t n, size_t *off,
		struct clmig_rec *r);

/* the group header, so the receive side stops hand-decoding it too */
uint32_t clmig_group_req(const unsigned char *g);
unsigned int clmig_group_count(const unsigned char *g);


/* ---- the frames the records travel in -----------------------------
 *
 * A GROUP (M_MIGRATE_MANY / M_REPL_MANY) is a 9-byte header then
 * back-to-back records:  [type1][req4][node2][count2].  The count is
 * patched by clmig_record_done as records land, which is why it is
 * written as 0 up front.
 *
 * A SINGLE (M_MIGRATE) is its own 14-byte shape and NOT a group of one:
 * [type1][req4][node2][ttl4][collen1][klen2] then col, key, value.  The
 * two look alike and are not - the single has no count and orders its
 * fields differently, which is exactly why both belong here rather than
 * being re-derived at each end.
 *
 * The ACK has TWO shapes and byte 5 is not spare: it is a status byte.
 * A single record acks 6 bytes, [type1][req4][ok1]; a group acks 8,
 * appending [stored2] - the record count that credits migrated_out.
 * The reader accepts either, because a peer on an older build sends the
 * short one, and it treats a missing count as 0.
 */
#define CLMIG_GHDR   9
#define CLMIG_SHDR   14
#define CLMIG_ACK_MIN 6            /* type+req4+ok1 */
#define CLMIG_ACK_LEN 8            /* ... +stored2 */

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

/* the group header, count zeroed for clmig_record_done to patch */
size_t clmig_group_hdr(unsigned char *buf, size_t cap, unsigned char type,
		uint32_t req, int node);

size_t clmig_single_size(unsigned int collen, unsigned int klen,
		unsigned int vlen);
size_t clmig_single_write(unsigned char *buf, size_t cap, unsigned char type,
		const struct clmig_single *s);
/* Returns 0 unless the header and the col+key it declares are wholly
 * present.  vlen is whatever remains. */
int clmig_single_parse(const unsigned char *p, size_t n,
		struct clmig_single *s);

/* @stored NULL writes the 6-byte form, otherwise the 8-byte one. */
size_t clmig_ack_write(unsigned char *buf, size_t cap, unsigned char type,
		uint32_t req, int ok, const unsigned int *stored);
/* *stored is 0 when the frame is the short form. */
int clmig_ack_parse(const unsigned char *p, size_t n, uint32_t *req,
		int *ok, unsigned int *stored);


/* ---- the record header alone --------------------------------------
 *
 * The bulk plane streams records over TCP: it reads CLMIG_RHDR bytes,
 * learns the three lengths, then reads that many more.  So it needs the
 * header without the body, which clmig_rec_write/parse cannot give it -
 * they assume one contiguous buffer.
 *
 * Before this existed the bulk sender hand-wrote the header (a THIRD
 * copy of the same layout, after mig_cb's datagram and bulk paths) and
 * both readers hand-parsed it.  Same nineteen bytes, four separate
 * spellings.
 */
void clmig_rec_hdr_write(unsigned char *p, const struct clmig_rec *r);
/* @p must hold CLMIG_RHDR bytes; col/key/val are NOT set. */
void clmig_rec_hdr_parse(const unsigned char *p, struct clmig_rec *r);

#endif /* PC_CLMIG_H */
