/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clmsg.h - the cluster plane's datagram types: the first byte of every sealed cluster datagram.
 * A value is WIRE: never renumber one, never reuse a retired one. */
#ifndef PC_CLMSG_H
#define PC_CLMSG_H

/* the longest key a frame may name: every client door's KEY_MAX */
#define CL_KEY_MAX  4096

#define M_PULL_REQ  2
#define M_PULL_RSP  3
#define M_TOMBSTONE 4
#define M_FWD_OP      5
#define M_FWD_ACK     6
#define M_MIGRATE     7
#define M_MIGRATE_ACK 8
#define M_DEMOTE      9
/* membership (control plane; 1 was the retired static-list heartbeat) */
/* the five below are laid out in clmemb.h, the only place they are written down and pinned */
#define M_JOIN_REQ      10   /* mcast, or ucast to master: token, config, identity
                              * and the id it PROPOSES (the master arbitrates) */
#define M_ASSIGN        11   /* ucast: the id you are, the master, the members;
                              * a record's address and port in NETWORK ORDER */
#define M_MASTER_ALIVE  12   /* mcast 1/s: silence past MASTER_DEAD_MS promotes a
                              * standby; its TERM settles a split brain */
#define M_ALIVE         13   /* mcast 1/s; addr = src.  free feeds placement,
                              * liveKB/total the rebalancer (live CELL bytes) */
#define M_GOODBYE       14   /* mcast; its node id is read by NOTHING - the
                              * sender's address is the identity */
#define M_MIGRATE_MANY  15   /* ucast: [req4][node2][count2] + n x ([ttl4][cn1]
                              * [klen2][vlen4][ver8][col][key][val]); ack: req + stored */

#define M_FWD_JSON      16   /* ucast: [req4][node2][op1][flags1][ttl4][by8][cn1]
                              * [klen2][plen2][vlen4][col][key][path][val] */
#define M_JOIN_REJ      19   /* ucast: a join refused outright; the reason byte is
                              * additive (an older 9-byte refusal has none) */
#define PC_REJ_DUP_IDENT 1  /* a LIVE member claims this identity: a clone */
#define PC_REJ_FULL      2  /* the fleet is at its member limit: the joiner
                            * stops rather than found a second cluster */
#define M_CLSYNC        21   /* ucast master -> backup: [maplen4][map][histlen4][hist],
                              * the identity history only the master knows */
#define M_JOIN_WAIT     23   /* ucast: the master HOLDS this join (a like identity
                              * still counts live); keep retrying, do not found */
#define M_COL_SET       24   /* ucast: [op1][bl1][gen8][nlen1][name] (+ [nlen2][name2]
                              * when op 2): op 0 made, 1 dropped; Lamport-ordered */
#define M_CLSYNC_ACK    22   /* ucast backup -> master: [term4][seq4] -
                              * the epoch the standby now holds */

#define M_CLMAP         20
#define M_PUBLISH       25   /* ucast to every live peer: [node2][seq8][clen2][chan]
                              * [dlen4][data] for its OWN subscribers; never re-relayed */
#define M_PSPROBE       26   /* cluster socket -> a peer's relay port:
                              * [node2][nonce8] - "do you answer here?" */
#define M_PSPROBE_ACK   27   /* relay port -> the prober's cluster
                              * socket: [node2][nonce8] echoed */
#define M_PSINT_ADD     28   /* ucast to every live peer: [node2][version8][kind1],
                              * kind 0 [h1 4][h2 4] or 1 [len2][pattern]: a first one */
#define M_PSINT_REQ     29   /* ucast: [node2] - "send me your full
                              * interest" */
#define M_UNKCMD        31   /* ucast to every live peer: clunk_write()'s frame, on a
                              * change (at most 1/s), first contact and every 30 s */
#define M_PSINT_FULL    30   /* ucast answer: [node2] + the full state
                              * (psinterest.h) */
/* the map frame is a type byte then the encoded map */
#define CLMAP_FRAME_AT   1   /* mcast: the master's map (clmap.h); additive, as the
                              * dispatch has no default case.  Not consumed yet. */

#define M_REPL_MANY     18   /* ucast: M_MIGRATE_MANY's payload stored PASSIVE, off
                              * the WAL: an eager replica, never re-propagated */
#define M_FWD_JACK      17   /* ucast: [req4][st1][op1][newval8]
                              *        [count4][fraglen4][frag] */

/* ucast to every live peer: [node2][term4] "stop cleanly", sent three times; a stop from a term
 * older than the receiver's is ignored, so a captured one cannot be replayed */
#define M_FLEETSTOP     32
/* the digest-gated sweep, ucast: [req4][part1][parts1][collen1][col][n2] + n x ([slot2][dig8]);
 * an older build ignores the type, the sender times out and sweeps as before */
#define M_SWEEP_DIG     33
/* ucast answer: [req4][part1][n2] + n x [slot2] - the slots, of those
 * asked in that part, whose digests DIFFER */
#define M_SWEEP_DIG_R   34
#define M_PEER_HINT     35   /* unicast mode, member -> master: [ip4][port2]
                             * network order, an address the master may not know */
#define M_LANE_FORCE    36   /* by the lane: "your UDP does not reach
                             * me; send me everything over TCP" - empty */
#define M_MPROBE        37   /* "do you hear master [node2]?" */
#define M_MPROBE_R      38   /* [node2 master][u32 ms since heard,
                             * 0xFFFFFFFF = not my master / not heard] */
#define M_LANE_PROBE    39   /* by UDP ONLY, from a node that forced its lane to
                             * the receiver: "does UDP reach you now?" - empty */
#define M_LANE_PROBE_R  40   /* by UDP ONLY: the answer - its arrival
                             * proves UDP both ways - empty */
#define M_STATS_RESET   41   /* [node2][at8, big-endian unix seconds]: a fleet-wide
                             * stats reset, counted from the sender's moment */
/* ucast to the elected mailer, twice: [node2][seq4][level1][flags1][klen1][key][hlen1][host]
 * [tlen2][text], an alert it cannot mail itself; flags bit 0 a clear; (node, seq) de-duplicates */
#define M_ALERT_RELAY   42
/* the reconcile's fast path: a restarted node asks a witness [req4][age4][incarn4] (no incarn from
 * an older build) for deletes planted since; M_TOMB_RPL [req4][seq4] + an M_TOMBSTONE each, then
 * M_TOMB_DONE [req4][status1][n4] */
#define M_TOMB_REQ      43
#define M_TOMB_RPL      44
#define M_TOMB_DONE     45
#define TOMBFP_OK        0   /* vouched for the whole window and sent n (else n = 0) */
#define TOMBFP_OFF       1   /* no retained deletes here (retain_s 0) */
#define TOMBFP_OLDER     2   /* the window began before the table holds:
                              * retain_s, or this witness's own boot */
#define TOMBFP_LOST      3   /* the cap (or memory) lost deletes inside it */
#define TOMBFP_BEHIND    4   /* deletes inside it forgotten by the members'
                              * marks, which the asker's rebuilt marks miss */
/* a member asks this node to install a build it holds staged: M_UPG_SWITCH [req4][sha256 32],
 * answered M_UPG_ACK [req4][status1] (UPGSW_*); accepted = written for this host's helper */
#define M_UPG_SWITCH    46
#define M_UPG_ACK       47
#define UPGSW_ACCEPTED   0
#define UPGSW_NOT_STAGED 1   /* no such build staged there */
#define UPGSW_UNVERIFIED 2   /* staged, but it no longer verifies */
#define UPGSW_NO_HELPER  3   /* no helper enabled on that host */
#define UPGSW_BUSY       4   /* a request already waits there */
#define UPGSW_OTHER      5
/* the reconcile's hold-check (clhold.h), never a value, only when every live member announces
 * CLMEMB_FEAT_HOLDQ; a key a peer cannot speak for is left out, its slot timing out inconclusive */
#define M_HOLD_Q        48
#define M_HOLD_A        49
/* a hole in an origin's numbering, asked of the origin if live, else every member keeping marks:
 * M_GAP_REQ [oid8][incarn4][from8][to8] (LE); held numbered deletes are re-sent, no answer frame */
#define M_GAP_REQ       50
#define M_GAP_REQ_LEN   29
/* a member back from an absence (same incarnation) lacking deletes this node no longer holds:
 * M_REBUILD [kind u8][age_ms u32] (LE), asked every 2 s up to 5 times until taken */
#define M_REBUILD       51
#define M_REBUILD_LEN   6
#define M_REBUILD_ASK   0   /* reconcile what you stored before age_ms ago */
#define M_REBUILD_TAKEN 1   /* a pass covering it runs or ran (age_ms 0) */
/* the age a floor asks with - no absence, the origin forgot deletes
 * past this node's marks */
#define M_REBUILD_FLOOR 0xffffffffu
/* PSPROBE's pair for replication on the relay port - an older build
 * drops the type there, so it never gets REPL_MANY it would drop too */
#define M_RPPROBE       52
#define M_RPPROBE_ACK   53
/* the origin's answer to a gap below what it still holds: M_GAP_FLOOR [oid8][incarn4][floor8]
 * (LE); the member floors its mark there and M_REBUILD follows */
#define M_GAP_FLOOR     54
#define M_GAP_FLOOR_LEN 21
/* the master hands mastership to its standby: M_HANDOVER [term4] (LE), taken only from the master
 * followed, by the map's standby at its term; the old master steps down on its beacon */
#define M_HANDOVER      55
#define M_HANDOVER_LEN  5
/* a member missing the beacon digest three beats running asks M_MEMBERS_REQ [digest8]; M_MEMBERS
 * (an ASSIGN's layout, token 0) answers, from the master followed only, at most 20 a second */
#define M_MEMBERS_REQ     56
#define M_MEMBERS_REQ_LEN 9
#define M_MEMBERS         57
/* the link check: M_LINK_REPORT [flags1][count2] + [node2][bits1] each, what this node proved of
 * a peer (flags 1: it is CONNECTING); M_ONLINE [term4], master -> CONNECTING: all pairs held */
#define M_LINK_REPORT     58
#define M_ONLINE          59
#define M_ONLINE_LEN      5
/* a node whose upstream tracking had a gap dropped a collection's written-through records and
 * asks every live peer for that collection whole: M_ASKFILL [collen1][col] */
#define M_ASKFILL         60

#endif /* PC_CLMSG_H */
