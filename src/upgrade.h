/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* upgrade.h - staging, verifying and switching to a release build */
#ifndef PC_UPGRADE_H
#define PC_UPGRADE_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

#define PC_UPG_SIG_LEN 64              /* ed25519, detached */
#define PC_UPG_ARTIFACT_MAX (1024L << 20)

/* @sig over @data against the built-in keys, then @up's trusted_keys (@up may be NULL): 0 with the
 * signer named in @who ("the release key 5646e8e4 (built in)", "trusted_keys[1] ab12cd34"), or -1 */
int pc_upg_verify(const void *data, size_t len, const unsigned char *sig,
		const struct pc_upgrade_conf *up, char *who, size_t wl);

/* a signature file: 64 raw bytes (openssl pkeyutl -sign -rawin) or 128 hex characters and an
 * optional newline; 0, -1 with errno (ENOENT: no file), or -2 when it is neither */
int pc_upg_read_sig(const char *path, unsigned char *sig);

/* A whole artifact into memory (at most PC_UPG_ARTIFACT_MAX); free() it */
void *pc_upg_read_file(const char *path, size_t *len);

/* -K <path>: the sha256 and whether <path>.sig verifies - the root helper asks the INSTALLED
 * binary, so an artifact never vouches for itself; prints the verdict, returns the exit code */
int pc_upg_check_file(const char *path, const struct pc_upgrade_conf *up);

/* <state_dir>/upgrade/node: pid, version, revision, node state and since when, rewritten atomically on
 * change - the root helper sees READY without a secret.  NULL or "" = nothing written */
int pc_upg_node_init(const char *state_dir);
void pc_upg_node_tick(void);                /* 1 Hz, the maintenance loop */

/* ---- staging: one upload at a time - begin, chunks in order, end; the end verifies sha256 and
 * signature before publishing staged/<sha256>.tar.gz.  0, or -1 with *err a line for the client ---- */
#define PC_UPG_CHUNK_MAX (256 * 1024)       /* raw bytes per chunk */
#define PC_UPG_IDLE_S    300                /* idle this long: the next begin abandons it */
#define PC_UPG_KEEP      2

void pc_upg_set_conf(const struct pc_upgrade_conf *up);
/* how this daemon was installed - "package", "prefix" or "local", detected or as [daemon]
 * install_mode says (PC_INSTALL_*); a package install refuses upgrade_stage */
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

/* ---- the relay: a build staged here goes on to every member (cluster.c, the bulk lane), which
 * stages and verifies it itself; each member's answer is kept for upgrade_status ---- */
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

/* ---- the switch: the staged build is verified again, then upgrade/request is written atomically
 * for the helper's path unit; refused while one waits, and with no helper unless @force ---- */
/* back to the prefix the helper kept before its last switch: refused on a package or local install,
 * with no previous, or (unless @force) when this node does not run what the helper switched to */
int pc_upg_revert(int force, char *id, size_t idl, char *to, size_t tl,
		const char **err);
int pc_upg_switch(const char *sha_hex, int force, char *id, size_t idl,
		char *ver, size_t vl, const char **err);
/* pc_upg_switch's refusals, told apart (any other: -1) */
#define PC_UPG_SW_NO_HELPER  -2
#define PC_UPG_SW_WAITING    -3
#define PC_UPG_SW_NOT_STAGED -4
#define PC_UPG_SW_UNVERIFIED -5

/* M_UPG_SWITCH / M_UPG_ACK (clmsg.h): every sealed send stays on the cluster thread and no
 * verification runs there - the maintenance thread runs pc_upg_switch between the queues */
void pc_upg_switch_ask(uint32_t addr_be, uint16_t port_be, uint32_t req,
		const unsigned char *sha);
int pc_upg_switch_answer(uint32_t *addr_be, uint16_t *port_be, uint32_t *req,
		unsigned int *status);
int pc_upg_ask_member(uint32_t addr_be, const char *sha_hex, const char **err);
int pc_upg_ask_take(uint32_t *addr_be, uint32_t *req, unsigned char *sha);
void pc_upg_ask_acked(uint32_t addr_be, uint32_t req, unsigned int status);
/* the helper as this host has it: "absent", "installed" or "enabled" */
const char *pc_upg_helper_state(void);

/* the fact every keepalive carries: bits 0-1 helper (absent/installed/enabled), 4-7 PC_UPG_ST_*, 8 a
 * build staged, 16-31 its sha256 prefix; worked out every few seconds, pc_upg_fact() only loads it */
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
