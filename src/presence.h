/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* presence.h - EXISTS, TYPE, TTL and PTTL as a GET finds the key: here, or
 * by a get's pull to the node it would ask, the request parked meanwhile */
#ifndef PC_PRESENCE_H
#define PC_PRESENCE_H

#include <stddef.h>

#include "core/pcache_htable.h"

struct pc_jw;
struct pc_tw;

/* what a parked request answers (0: not one of these) */
#define PC_PRES_EXISTS 1
#define PC_PRES_TYPE   2
#define PC_PRES_TTL    3
#define PC_PRES_PTTL   4

/* a key absent here: PC_OP_PARKED on *@park if a get would ask another node
 * for it, else PC_OP_ABSENT; the caller's absent answer is the fallback */
int op_pres_miss(pcache_htable_t *ht, str *k, int kind, unsigned int *park);
/* the kind op_pres_miss parked pull @req as, taken once by the door's park */
int pc_pres_park_take(unsigned int req);

/* the native exists and ttl (@kind PC_PRES_EXISTS or _TTL): the CMD
 * method's tree, the data verb's payload */
void pc_pres_cmd(pcache_htable_t *ht, str *k, int kind, struct pc_tw *out,
		unsigned int *park);
void pc_pres_verb(pcache_htable_t *ht, str *k, int kind, struct pc_jw *out,
		unsigned int *park);

/* a parked request's reply from its pull's answer (@ttl_left 0: none); the
 * payload is at most 8 bytes, and RESP's lives in respkv.c */
void pc_pres_tree(struct pc_tw *w, int kind, int found,
		unsigned long long ver, unsigned int ttl_left);
size_t pc_pres_bin(char *pay, int kind, int found, unsigned int ttl_left);
void pc_pres_resp(struct pc_jw *w, int kind, int found, unsigned int fl,
		unsigned int ttl_left);

#endif /* PC_PRESENCE_H */
