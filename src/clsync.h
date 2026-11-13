/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clsync.h - staging a map change past the backup: master stages, backup acks, master
 * publishes, so the backup is never behind the fleet - only equal or one change ahead. */
#ifndef PC_CLSYNC_H
#define PC_CLSYNC_H

#include <stdint.h>

#define PC_CLSYNC_IDLE    0            /* nothing in flight */
#define PC_CLSYNC_STAGED  1            /* sent to the backup, awaiting ack */
#define PC_CLSYNC_ACKED   2            /* committed; publishable */

struct pc_clsync {
	int state;                     /* PC_CLSYNC_* */
	int have_backup;               /* a standby exists to ack */
	uint32_t term;                 /* the term we are issuing under */
	uint32_t published_seq;        /* last sequence the fleet has seen */
	uint32_t next_seq;             /* next sequence to hand out */
	uint32_t staged_seq;           /* the one in flight (valid when != IDLE) */
	int staged_unsynced;           /* staged with no backup to ack */

	/* backup side: a change we acked and have not seen published; on promotion it MUST
	 * be published - it was committed when we acked it */
	int hold_pending;
	uint32_t hold_term, hold_seq;

	unsigned long long staged_n, acked_n, published_n, aborted_n,
		unsynced_n;
};

/* Begin at @term with the fleet already holding @published_seq. */
void pc_clsync_init(struct pc_clsync *s, uint32_t term, uint32_t published_seq);

/* A standby exists (or stopped existing).  Losing one mid-flight does NOT abort the
 * staged change: its ack may already be in flight. */
void pc_clsync_set_backup(struct pc_clsync *s, int have_backup);

/* Stage the next change; *out_seq gets the sequence it will publish at.  -1 if one is in
 * flight (so an ack is never ambiguous); with no backup it is publishable at once. */
int pc_clsync_stage(struct pc_clsync *s, uint32_t *out_seq);

/* The backup acked @term/@seq: 0 if it matched the change in flight, -1 otherwise - a
 * late ack for an abandoned change must not commit the one that replaced it. */
int pc_clsync_ack(struct pc_clsync *s, uint32_t term, uint32_t seq);

/* 1 = the staged change may go to the fleet. */
int pc_clsync_publishable(const struct pc_clsync *s);

/* Record that the staged change reached the fleet. */
int pc_clsync_published(struct pc_clsync *s);

/* Give up on the change in flight.  Its sequence is NOT returned: re-staged content at a
 * reused seq could collide with a first stage whose ack was lost. */
int pc_clsync_abort(struct pc_clsync *s);

/* ---- backup side ------------------------------------------------- */

/* We acked @term/@seq for the master.  Held until we see it published. */
void pc_clsync_hold(struct pc_clsync *s, uint32_t term, uint32_t seq);

/* We saw the fleet reach @term/@seq: anything held at or below it is
 * accounted for. */
void pc_clsync_saw(struct pc_clsync *s, uint32_t term, uint32_t seq);

/* Promoting: 1 if a committed change is still owed to the fleet, with its epoch written
 * out; republish it under the NEW term - the old one's master is gone. */
int pc_clsync_owed(const struct pc_clsync *s, uint32_t *term, uint32_t *seq);


/* ---- the wire envelope: [type1][maplen4][map bytes][histlen4][hist bytes] ---- */
#define CLSYNC_ENV_HDR      5      /* type + maplen */
#define CLSYNC_ENV_MAP_AT   CLSYNC_ENV_HDR
#define CLSYNC_ACK_LEN      9      /* type + term4 + seq4 */

/* where the history payload goes, once the map's length is known */
size_t clsync_env_hist_at(uint32_t maplen);

/* Write the type and both length prefixes around payloads already encoded in place
 * (reserve-then-patch).  Returns the frame length, or 0 if the buffer cannot hold it. */
size_t clsync_env_finish(unsigned char *buf, size_t cap, unsigned char type,
		uint32_t maplen, uint32_t histlen);

/* Split a received envelope: 0, touching nothing, unless the header, the whole map and a
 * history ending EXACTLY at the end are there - a trailing byte is a refusal. */
int clsync_env_parse(const unsigned char *p, size_t n,
		const unsigned char **map, uint32_t *maplen,
		const unsigned char **hist, uint32_t *histlen);

size_t clsync_ack_write(unsigned char *buf, size_t cap, unsigned char type,
		uint32_t term, uint32_t seq);
int clsync_ack_parse(const unsigned char *p, size_t n,
		uint32_t *term, uint32_t *seq);

#endif /* PC_CLSYNC_H */
