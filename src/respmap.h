/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* respmap.h - the RESP door's DB-index map: the config's list plus runtime map/unmap overrides
 * (fleet state, the newer Lamport generation wins), published as one immutable lock-free string */
#ifndef PC_RESPMAP_H
#define PC_RESPMAP_H

#include <stddef.h>
#include <stdint.h>

#define PC_RESPMAP_MAX 64              /* indexes 0..63 */

/* @file_list: resp_collections (NULL = every collection visible); @state_dir: where overrides
 * persist (NULL or "" = not).  0, or -1 when the persisted file is unreadable garbage */
int pc_respmap_init(const char *file_list, const char *state_dir);

/* 1 once pc_respmap_init ran (the daemon); before it - and in -C - the
 * config's own list is the answer */
int pc_respmap_ready(void);

/* the config's own list as it was given (NULL = none: every collection
 * visible) - for `maps`, which shows what the file says beside what holds */
const char *pc_respmap_config(void);

/* the effective list (NULL = all visible, nothing overridden; "*" first = all visible plus
 * overrides) - valid inside a quiescent turn, or for the process's life when nothing changes */
const char *pc_respmap_effective(void);

/* @set 1 maps @index to @col, 0 unmaps; 1 = applied, 0 = a newer generation is held (equal: the
 * larger name wins), -1 = bad index or name; @was (may be NULL) gets the prior collection or "" */
int pc_respmap_apply(int index, const char *col, size_t cl, uint64_t gen,
		int set, char *was, size_t wcap);

/* what @index resolves to now into @out ("" = refused), and who put it there: an override, a
 * config entry naming the index, or neither - an open door answering it as that collection */
#define PC_RESPMAP_OPEN    0
#define PC_RESPMAP_CONFIG  1
#define PC_RESPMAP_RUNTIME 2
int pc_respmap_resolve(int index, char *out, size_t cap);

/* every override held, tombstones included (the fleet sync sends them
 * all, so an unmap reaches a member that missed it) */
typedef void (*pc_respmap_cb)(int index, const char *col, uint64_t gen,
		int set, void *ctx);
void pc_respmap_each(pc_respmap_cb cb, void *ctx);

/* how many indexes a runtime override maps now (tombstones excluded) */
int pc_respmap_overrides(void);

/* write <state_dir>/respmap whole and fsync it; 0 or -1 */
int pc_respmap_persist(void);
/* the same off the caller's thread: pc-maint writes it on its next tick */
void pc_respmap_persist_later(void);
void pc_respmap_persist_tick(void);

/* -C: how many overrides <state_dir>/respmap holds, without loading it;
 * 0 when there is no file, -1 when it is not one of ours */
int pc_respmap_count_file(const char *state_dir);

#endif
