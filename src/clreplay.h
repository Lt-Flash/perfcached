/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clreplay.h - a sealed cluster frame is taken once: per sender the newest incarnation, per stream
 * a CLREPLAY_WIN window.  A frame it has no room to track is refused and counted, once senders
 * quiet CLREPLAY_STALE_MS have been let go; a lock per window. */
#ifndef PC_CLREPLAY_H
#define PC_CLREPLAY_H

#include "clwire.h"

#define CLREPLAY_WIN       131072      /* frames of reordering a stream may show */
#define CLREPLAY_SENDERS   512
#define CLREPLAY_STREAMS   1024        /* 512 workers and the node's own threads */
#define CLREPLAY_WINS      4096        /* windows in all, 64 MiB */
#define CLREPLAY_STALE_MS  60000

enum { CLREPLAY_OK, CLREPLAY_DUP, CLREPLAY_OLD, CLREPLAY_INCARN,
	CLREPLAY_FULL, CLREPLAY_HOST, CLREPLAY_NCOUNT };

/* CLREPLAY_OK to take the frame, else why not (FULL: no room to track it);
 * @from_ip is the host it came from, network order as the header's */
int clreplay_admit(const struct clwire_id *id, uint32_t from_ip, long long now_ms);
/* whether clreplay_admit's answer lets the frame in */
static inline int clreplay_taken(int rc)
{
	return rc == CLREPLAY_OK;
}
/* the counts since start, indexed as above (OK unused) */
void clreplay_counts(unsigned long long out[CLREPLAY_NCOUNT]);

#endif /* PC_CLREPLAY_H */
