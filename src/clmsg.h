/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clmsg.h - the cluster plane's datagram types (M14, wave 4).
 *
 * The first byte of every sealed cluster datagram.  These lived at the
 * top of cluster.c, which made every frame builder a cluster.c static:
 * a module that wanted to write a PULL_REQ or a FWD_OP had to call back
 * into cluster.c for the one byte it could not name.  Moved verbatim -
 * the values, the comments and the order are cluster.c's as of 911c61d.
 * A value is WIRE: never renumber one, never reuse a retired one.
 */
#ifndef PC_CLMSG_H
#define PC_CLMSG_H

#define M_PULL_REQ  2
#define M_PULL_RSP  3
#define M_TOMBSTONE 4
#define M_FWD_OP      5
#define M_FWD_ACK     6
#define M_MIGRATE     7
#define M_MIGRATE_ACK 8
#define M_DEMOTE      9
/* membership (control plane; 1 was the retired static-list heartbeat) */
/* LAYOUTS FOR THE FIVE BELOW LIVE IN clmemb.h, which is the only place
 * they are written down and the only place a test pins them.  The
 * sketches that used to sit here had drifted - JOIN_REQ still listed a
 * k1 and an m1 that no longer exist and omitted the incarnation and the
 * WAL byte - which is what a second copy of a layout does. */
#define M_JOIN_REQ      10   /* mcast, or ucast to master.  The joiner's
                              * whole position: token, config, identity,
                              * and the id it PROPOSES - derived from its
                              * identity so it asks for the same one every
                              * start; the master arbitrates collisions */
#define M_ASSIGN        11   /* ucast: the id you are, who the master is,
                              * and the membership - a 15-byte header then
                              * a 20-byte record per member.  The address
                              * and port in a record are NETWORK ORDER and
                              * pass through verbatim */
#define M_MASTER_ALIVE  12   /* mcast 1/s.  What master-down detection
                              * runs on: silence past MASTER_DEAD_MS
                              * promotes a standby.  Carries the TERM the
                              * sender asserts mastership under, which is
                              * what settles a split brain before any map
                              * could arrive */
#define M_ALIVE         13   /* mcast 1/s; addr = src.  Sixteen fields
                              * behind eleven length gates, all additive.
                              * free feeds placement
                              * (capacity-seeking); liveKB/total feeds
                              * the rebalancer (proportional per-mille
                              * leveling on live CELL bytes, never the
                              * chunk-sticky 'used') */
#define M_GOODBYE       14   /* mcast.  Carries the departing node id,
                              * which NOTHING READS - the sender's address
                              * is the identity here */
#define M_MIGRATE_MANY  15   /* ucast: [req4][node2][count2] then per
                              * record [ttl4][cn1][klen2][vlen4][ver8]
                              * [col][key][val] - small records GATHERED
                              * to the datagram cap; ack echoes req + a
                              * stored-count.  ver is the version the
                              * SENDER's table committed (A2): it travels
                              * with the bytes, and the receiver refuses
                              * a copy older than the one it holds. */

#define M_FWD_JSON      16   /* ucast: [req4][node2][op1][flags1]
                              *        [ttl4][by8][cn1][klen2][plen2]
                              *        [vlen4][col][key][path][val] */
#define M_JOIN_REJ      19   /* ucast: the master refusing a join
                              * outright.  The reason byte is additive - a
                              * 9-byte refusal from an older build has
                              * none.  Reason 1 =
                              * duplicate identity: another LIVE member
                              * already claims it, which means this node
                              * is a CLONE of one, and two nodes sharing
                              * an identity break everything built on it
                              * (incarnation, backfill targeting). */
#define PC_REJ_DUP_IDENT 1
#define M_CLSYNC        21   /* ucast master -> backup: the control state a
                              * promoted standby needs and cannot rebuild.
                              * [maplen4][map][histlen4][hist].  The map
                              * carries the epoch and the node state
                              * table; the identity history is the part
                              * that CANNOT be reconstructed - only the
                              * master knows which identities this cluster
                              * has seen, so a standby without it would
                              * call every returning node new. */
#define M_JOIN_WAIT     23   /* ucast: the master is HOLDING this
                              * join (an identity like the joiner's is still
                              * counted live); keep retrying, do not found.
                              * S108: silence here made a clone found its
                              * own cluster.  A build before the tag ignores
                              * it and behaves as before. */
#define M_COL_SET       24   /* ucast: [op1][bl1][gen8][nlen1][name]
                              * (+ [nlen2][name2] when op is 2) - S69,
                              * a collection created (op 0) or dropped
                              * (op 1) somewhere in the fleet.  Idempotent
                              * and ordered by the Lamport generation, so
                              * a re-announce cannot undo a later drop. */
#define M_CLSYNC_ACK    22   /* ucast backup -> master: [term4][seq4] -
                              * the epoch the standby now holds */

#define M_CLMAP         20
#define M_PUBLISH       25   /* PS3, ucast to every live peer: [node2][seq8]
                              * [clen2][chan][dlen4][data] - a client's
                              * publish, for the peer's OWN subscribers.
                              * Never re-relayed; seq counts the gaps. */
#define M_PSPROBE       26   /* PS11, cluster socket -> a peer's relay port:
                              * [node2][nonce8] - "do you answer here?" */
#define M_PSPROBE_ACK   27   /* PS11, relay port -> the prober's cluster
                              * socket: [node2][nonce8] echoed */
#define M_PSINT_ADD     28   /* PS12, ucast to every live peer: [node2]
                              * [version8][kind1] then kind 0 [h1 4][h2 4]
                              * (a channel's filter hashes), kind 1
                              * [len2][pattern] - a first subscription */
#define M_PSINT_REQ     29   /* PS12, ucast: [node2] - "send me your full
                              * interest" */
#define M_UNKCMD        31   /* S165, ucast to every live peer: clunk_write()'s
                              * frame - this node's unknown commands, sent
                              * on a change (at most 1/s), on first contact
                              * with a peer and every 30 s.  A build before
                              * S165 drops the type in drain_one_fd's
                              * default, so a mixed fleet shows fewer
                              * members reporting, not an error. */
#define M_PSINT_FULL    30   /* PS12, ucast answer: [node2] + the full state
                              * (psinterest.h) */
/* the map frame is a type byte then the encoded map */
#define CLMAP_FRAME_AT   1   /* mcast: the master's cluster map, encoded by
                              * clmap.h.  ADDITIVE: the dispatch below
                              * has no default case, so a build that
                              * predates this ignores the datagram
                              * rather than mis-parsing it - which is
                              * why this needs no PC_CL_VER bump.
                              * Nothing consumes it yet; placement still
                              * runs through this file's own HRW. */

#define M_REPL_MANY     18   /* ucast: M_MIGRATE_MANY's exact payload,
                              * but the receiver stores PASSIVE and
                              * skips the WAL - an eager-store replica,
                              * not an ownership transfer.  Passive
                              * copies never re-propagate (no echo). */
#define M_FWD_JACK      17   /* ucast: [req4][st1][op1][newval8]
                              *        [count4][fraglen4][frag] */

/* S222, ucast to every live peer: [node2][term4] - "the fleet is being
 * stopped: stop cleanly".  Sent three times, because it is one datagram
 * per peer and a lost one would leave a node up while its peers are
 * gone.  The term is the SENDER's; a receiver ignores a stop from an
 * older term than its own, so a captured stop cannot be replayed at a
 * fleet that has since moved on.  A new type rather than a value in an
 * old one, so a build that predates it drops it (drain_one_fd's
 * default) and the originator reports that node as still up - no
 * epoch change (12fo). */
#define M_FLEETSTOP     32

#endif /* PC_CLMSG_H */
