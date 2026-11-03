/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clreplay.h - CH-1: a sealed cluster frame is taken once.
 *
 * Every frame names its sender (host and cluster port), the sender's
 * incarnation, the sealing thread's stream and that stream's sequence, all
 * inside the authenticated header (clwire.h).  Per sender the newest
 * incarnation is kept, and per stream the highest sequence with a window of
 * CLREPLAY_WIN below it: a sequence already taken, one older than the
 * window, or one from an older incarnation of a sender heard within
 * CLREPLAY_STALE_MS is refused.  A sender or stream past the tables'
 * capacity is let through and counted - fail open, as a sealed frame was.
 *
 * Any thread; a lock per window, none on the lookup.
 */
#ifndef PC_CLREPLAY_H
#define PC_CLREPLAY_H

#include "clwire.h"

#define CLREPLAY_WIN       131072      /* frames of reordering a stream may show */
#define CLREPLAY_SENDERS   512
#define CLREPLAY_STREAMS   256
#define CLREPLAY_STALE_MS  60000

enum { CLREPLAY_OK, CLREPLAY_DUP, CLREPLAY_OLD, CLREPLAY_INCARN,
	CLREPLAY_FULL, CLREPLAY_NCOUNT };

/* CLREPLAY_OK (or FULL: let through) to take the frame, else why not */
int clreplay_admit(const struct clwire_id *id, long long now_ms);
/* the counts since start, indexed as above (OK unused) */
void clreplay_counts(unsigned long long out[CLREPLAY_NCOUNT]);

#endif /* PC_CLREPLAY_H */
