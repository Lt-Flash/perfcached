/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * respmap.h - S432: the RESP door's DB-index map, changeable at runtime.
 *
 * `resp_collections` (the config) is an INDEX:NAME / NAME list read at
 * start; the door resolves it on every command (verbs.c: resp_map_index,
 * resp_index_for, resp_col_allowed), so a changed map is seen by every
 * connection on its next command.  This module holds that list plus the
 * operator's runtime overrides - `map <index> <collection>` and `unmap
 * <index>` - and publishes ONE effective string in the config's own format,
 * which those readers take as before.
 *
 *   - An override replaces whatever the file says for its index (an
 *     `INDEX:NAME` entry or a bare `INDEX`) with `INDEX:collection`, listed
 *     ahead of the file's entries so INFO keyspace reports a collection
 *     under the index it was moved to; `unmap` removes the override, and
 *     the index answers as the file maps it again.
 *   - Overrides are fleet state: each carries a Lamport generation, an
 *     unmap is kept as a tombstone with its own, and the newer generation
 *     wins wherever it arrives first (equal generations: the larger
 *     collection name, so every member settles on the same one).
 *   - Persisted in <state_dir>/respmap, written whole and fsynced on every
 *     change; loaded at start before the workers run.
 *   - With no list in the file (every collection visible) the effective
 *     string starts with "*", which resp_col_allowed reads as "every
 *     name", so an override does not turn an open door into an allow-list.
 *
 * The published string is immutable: a change builds a new one, publishes
 * it with one atomic store and retires the old one until no worker can
 * still hold it (quiesce.h), so the per-command readers take no lock.
 */
#ifndef PC_RESPMAP_H
#define PC_RESPMAP_H

#include <stddef.h>
#include <stdint.h>

#define PC_RESPMAP_MAX 64              /* indexes 0..63 */

/* @file_list: the config's resp_collections (NULL = every collection
 * visible); @state_dir: where the overrides persist (NULL or "" = not
 * persisted).  0, or -1 when the persisted file is unreadable garbage */
int pc_respmap_init(const char *file_list, const char *state_dir);

/* 1 once pc_respmap_init ran (the daemon); before it - and in -C - the
 * config's own list is the answer */
int pc_respmap_ready(void);

/* the config's own list as it was given (NULL = none: every collection
 * visible) - for `maps`, which shows what the file says beside what holds */
const char *pc_respmap_config(void);

/* the effective list for the door's readers (NULL = every collection
 * visible and nothing overridden) - valid while the caller is inside a
 * quiescent turn, or for the life of the process when nothing changes */
const char *pc_respmap_effective(void);

/* apply one override: @set 1 maps @index to @col, 0 unmaps it.  Returns 1
 * when applied (it was newer), 0 when an override of a newer generation is
 * already held (ignored), -1 for an index out of range or a bad name.
 * @was, when not NULL, receives the collection the index resolved to
 * before (the file's or an override's; "" when none) */
int pc_respmap_apply(int index, const char *col, size_t cl, uint64_t gen,
		int set, char *was, size_t wcap);

/* what @index resolves to now, into @out ("" when the door refuses it),
 * and who put it there: a runtime override, an entry of the config's list
 * that names the index, or neither - an open door ("*" or no list) that
 * answers every index as the collection of that name */
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

/* -C: how many overrides <state_dir>/respmap holds, without loading it
 * into the running state; 0 when there is no file, -1 when it is not one
 * of ours */
int pc_respmap_count_file(const char *state_dir);

#endif
