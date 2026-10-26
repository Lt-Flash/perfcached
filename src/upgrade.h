/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * upgrade.h - UP6n/UP5 (DESIGN 12nf): who may vouch for a build to install.
 */
#ifndef PC_UPGRADE_H
#define PC_UPGRADE_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

#define PC_UPG_SIG_LEN 64              /* ed25519, detached */
#define PC_UPG_ARTIFACT_MAX (1024L << 20)

/* @sig over @data, against the built-in keys and then @up's trusted_keys
 * (@up may be NULL).  0 with the signer named in @who ("the release key
 * 5646e8e4 (built in)", "trusted_keys[1] ab12cd34"), or -1 */
int pc_upg_verify(const void *data, size_t len, const unsigned char *sig,
		const struct pc_upgrade_conf *up, char *who, size_t wl);

/* A signature file: 64 raw bytes (openssl pkeyutl -sign -rawin), or 128
 * hex characters with an optional newline.  0, -1 with errno (ENOENT: no
 * file), or -2 when it is neither shape */
int pc_upg_read_sig(const char *path, unsigned char *sig);

/* A whole artifact into memory (at most PC_UPG_ARTIFACT_MAX); free() it */
void *pc_upg_read_file(const char *path, size_t *len);

/* -K <path>: the sha256 of <path> and whether <path>.sig verifies - what
 * the root helper asks the INSTALLED binary, so an artifact never vouches
 * for itself.  Prints the verdict; returns the exit code (0 good) */
int pc_upg_check_file(const char *path, const struct pc_upgrade_conf *up);

/* UP5: <state_dir>/upgrade/node - this process's pid, version, revision,
 * node state and since when, rewritten (atomically) whenever one of them
 * changes.  The root helper reads it to know the build it started is
 * READY without a secret: /health is liveness only and behind the token.
 * pc_upg_node_init() creates the directory (0750, the daemon's own) and
 * writes the first; NULL or "" = no state directory, nothing written */
int pc_upg_node_init(const char *state_dir);
void pc_upg_node_tick(void);                /* 1 Hz, the maintenance loop */

/* ---- UP6n: staging a build on this node -------------------------------
 * One upload at a time, in order: begin (its size, sha256, detached
 * signature and version), chunks at increasing offsets, end.  The end
 * verifies the whole file - the sha256 begin named, then the signature
 * against the built-in keys and [upgrade] trusted_keys (unsigned only
 * under allow_unsigned) - and only then is it published as
 * upgrade/staged/<sha256>.tar.gz beside its .sig and .meta.  At most two
 * staged builds are kept and stage_mb bounds them; an upload idle for
 * PC_UPG_IDLE_S is abandoned by the next begin.  Each call returns 0, or
 * -1 with *err a line for the client. */
#define PC_UPG_CHUNK_MAX (256 * 1024)       /* raw bytes per chunk */
#define PC_UPG_IDLE_S    300
#define PC_UPG_KEEP      2

void pc_upg_set_conf(const struct pc_upgrade_conf *up);
/* PD-9: how this daemon was installed - "package", "prefix" or "local",
 * detected, or as [daemon] install_mode says (pc_upg_set_install_mode with
 * PC_INSTALL_*); a package install refuses upgrade_stage */
void pc_upg_set_install_mode(int mode);
const char *pc_upg_install_mode(void);
int pc_upg_stage_begin(long long size, const char *sha_hex,
		const unsigned char *sig, const char *version, char *id,
		size_t idl, const char **err);
int pc_upg_stage_chunk(const char *id, long long off, const void *data,
		size_t n, long long *got, const char **err);
int pc_upg_stage_end(const char *id, char *sha_hex, size_t shl, char *who,
		size_t wl, const char **err);
int pc_upg_stage_abort(const char *id, const char **err);

struct pc_tw;
/* upgrade_status: the staged builds, the upload in progress, the relay */
void pc_upg_status_tree(struct pc_tw *out);

/* ---- UP6n: the relay ------------------------------------------------------
 * A build an operator staged here goes on to every member that takes one
 * (cluster.c, over the bulk lane); each stages and verifies it itself and
 * answers a status.  What a member answered - or why it was not asked -
 * is kept per member for upgrade_status. */
#define PC_UPG_RELAY_OK        0u           /* the member staged it */
#define PC_UPG_RELAY_BEGIN     1u           /* it would not begin (busy, space) */
#define PC_UPG_RELAY_CHUNK     2u           /* a write failed there */
#define PC_UPG_RELAY_REFUSED   3u           /* sha256 or signature: refused */
#define PC_UPG_RELAY_MALFORMED 4u
#define PC_UPG_RELAY_OLDER     100u         /* not asked: an older build */
#define PC_UPG_RELAY_BUSY      101u         /* not asked: the bulk queue full */
#define PC_UPG_RELAY_SENDING   102u         /* queued, no answer yet */
#define PC_UPG_RELAY_LOST      103u         /* no answer: the connection failed */

void pc_upg_relay_note(uint32_t addr_be, unsigned int status);

/* ---- UPS: the switch ------------------------------------------------------
 * Ask the root helper to install a build this node holds staged: it is
 * verified again (the file on disk now, not the upload), then
 * upgrade/request is written - atomically, the helper's path unit fires on
 * it - and the request id is kept in upgrade/requested, so the process
 * the helper starts reports what became of it.  Refused while a request
 * is waiting, and when no helper is installed on this host unless @force.
 * The helper's stop is a clean stop (goodbye, the shutdown snapshot): a
 * master leaves as it does in any rolling restart. */
/* UP7b: ask the helper to switch back to the prefix it kept before its
 * last switch (upgrade_status.previous.from).  Refused on a package or
 * local install, with no previous, or when this node does not run the
 * build the helper switched to (force overrides the last).  Fills the
 * request id and the prefix it goes back to. */
int pc_upg_revert(int force, char *id, size_t idl, char *to, size_t tl,
		const char **err);
int pc_upg_switch(const char *sha_hex, int force, char *id, size_t idl,
		char *ver, size_t vl, const char **err);
/* pc_upg_switch's refusals, told apart (any other: -1) */
#define PC_UPG_SW_NO_HELPER  -2
#define PC_UPG_SW_WAITING    -3
#define PC_UPG_SW_NOT_STAGED -4
#define PC_UPG_SW_UNVERIFIED -5

/* M_UPG_SWITCH / M_UPG_ACK (clmsg.h).  Every sealed send stays on the
 * cluster thread, and a verification never runs there:
 *   asked:  the cluster thread queues it (pc_upg_switch_ask), the
 *           maintenance thread runs pc_upg_switch, the cluster thread
 *           takes the answer (pc_upg_switch_answer) and sends the ACK;
 *   asking: a verb queues it (pc_upg_ask_member), the cluster thread
 *           takes it (pc_upg_ask_take) and sends it, the ACK lands in
 *           pc_upg_ask_acked, upgrade_status shows it. */
void pc_upg_switch_ask(uint32_t addr_be, uint16_t port_be, uint32_t req,
		const unsigned char *sha);
int pc_upg_switch_answer(uint32_t *addr_be, uint16_t *port_be, uint32_t *req,
		unsigned int *status);
int pc_upg_ask_member(uint32_t addr_be, const char *sha_hex, const char **err);
int pc_upg_ask_take(uint32_t *addr_be, uint32_t *req, unsigned char *sha);
void pc_upg_ask_acked(uint32_t addr_be, uint32_t req, unsigned int status);
/* the helper as this host has it: "absent", "installed" or "enabled" */
const char *pc_upg_helper_state(void);

/* UPS: the upgrade fact every keepalive carries (PC_FACT_UPGRADE) -
 *   bits 0-1   the helper: 0 absent, 1 installed, 2 enabled
 *   bits 4-7   the state: PC_UPG_ST_*
 *   bit  8     a build is staged (bits 16-31 are its prefix, 0000 too)
 *   bits 16-31 the first two bytes of the newest staged build's sha256
 * Worked out on the maintenance thread every few seconds (it reads the
 * file system); pc_upg_fact() only loads the word. */
#define PC_UPG_ST_IDLE      0u              /* nothing staged, nothing asked */
#define PC_UPG_ST_STAGED    1u              /* a build staged, not requested */
#define PC_UPG_ST_REQUESTED 2u              /* the request waits for the helper */
#define PC_UPG_ST_DONE      3u              /* the helper's last word: ok */
#define PC_UPG_ST_FAILED    4u              /* ... rolled back or refused */
#define PC_UPG_FACT_HAS_STAGED 0x100u
uint32_t pc_upg_fact(void);
const char *pc_upg_fact_helper(uint32_t f);
const char *pc_upg_fact_state(uint32_t f);
/* relay staged/<sha_hex>.tar.gz to the members; the count offered, and
 * *older the members not asked for their build */
int pc_upg_relay_staged(const char *sha_hex, int *older);

#endif
