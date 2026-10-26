/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * logrl.h - a limit on log lines the network can drive (CH-17).
 *
 * A refusal said per datagram or per connection is a log that whoever can
 * reach the port writes: a replayed frame, a flood from rotating sources,
 * a misconfigured neighbour.  Each such line goes through pc_logrl_ok()
 * first, with its reason and the address it is about:
 *
 *   - one line per (reason, address) per LOGRL_WINDOW_MS;
 *   - at most LOGRL_CAP lines per window across every reason and address;
 *   - what was held back is never lost silently: the next line of that
 *     reason and address that is said is preceded by one line giving the
 *     count, and the next line said after the cap gives the cap's.
 *
 * The callers' counters count every event whether or not it is said.
 * Any thread: one lock, taken only on these refusal paths.
 */
#ifndef PC_LOGRL_H
#define PC_LOGRL_H

#include "pcaddr.h"

#define LOGRL_WINDOW_MS 60000
#define LOGRL_CAP       60
#define LOGRL_SLOTS     256

/* one reason per distinct line: a key is a reason and a host, and two
 * different lines about one host must not hold each other back (the
 * master's ALIVE and MASTER_ALIVE refused on the epoch are two lines -
 * epochcommittest reads the second) */
enum pc_logrl_reason {
	LRL_JOIN_HELD = 1,      /* a join whose identity a live member holds */
	LRL_JOIN_CLONE,         /* a join refused as a clone */
	LRL_JOIN_HOME,          /* a join held while its last home re-joins */
	LRL_NO_DIGEST,          /* a peer that sends no config digest */
	LRL_FLEETSTOP,          /* a fleet stop from another term */
	LRL_LANE_FORCE,         /* a peer's UDP found blocked: lane forced */
	LRL_LANE_BACK,          /* a forced lane given back to UDP */
	LRL_STATS_RESET,        /* a peer's fleet-wide stats reset */
	LRL_EPOCH_JOIN,         /* a join at a wire epoch this node refuses */
	LRL_EPOCH_ALIVE,        /* a keepalive at one */
	LRL_EPOCH_MASTER,       /* a master keepalive at one */
	LRL_EPOCH_OTHER,        /* any other frame at one */
	LRL_CFG_MISMATCH,       /* a peer whose collection config differs */
	LRL_FOREIGN_CID,        /* a frame of another cluster's id */
	LRL_HS_CLUSTER,         /* a client handshake as the cluster, none set */
	LRL_HS_PRINCIPAL,       /* a client handshake, unknown principal */
	LRL_HS_AUTH,            /* a client handshake that does not verify */
	LRL_HS_VERSION,         /* a client handshake, unknown version */
	LRL_ID_COLLISION,       /* an id a live member elsewhere holds */
	LRL_SENDER_GATE,        /* a frame its sender may not send (CH-12) */
	LRL_SENDER_ALIVE,       /* a keepalive with an id or identity refused */
	LRL_SENDER_MRANGE,      /* a master keepalive out of range */
	LRL_SENDER_MFOLLOW,     /* a master keepalive a member does not follow */
	LRL_SENDER_JOIN,        /* a join whose identity is live elsewhere */
	LRL_MCAST_STRAY,        /* another local address's unicast, on the
	                         * multicast socket */
	LRL_REASONS
};

/* 1: say the line now (after a count of what this reason and address had
 * held back, if any); 0: held back, counted */
int pc_logrl_ok(int reason, const pc_addr *src);
/* the same at @now_ms (monotonic) - for the unit test */
int pc_logrl_ok_at(int reason, const pc_addr *src, long long now_ms);
/* lines held back since the start, all reasons */
unsigned long long pc_logrl_held(void);

#endif
