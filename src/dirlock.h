/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * dirlock.h - S290: one daemon per state directory and per WAL directory.
 *
 * Nothing stopped two daemons pointed at the same state_dir or [wal] dir
 * on one host - different ports, a copy-pasted config: both started, both
 * claimed the same identity, and both wrote the same WAL segments, the
 * same snapshot and the same CONTROL file - silent log corruption, not
 * only a confused membership.
 *
 * Each directory gets an exclusive fcntl() write lock on a file inside it
 * (`perfcached.lock`), taken before anything else in the directory is
 * opened and held for the life of the process.  The kernel releases it
 * when the process dies, so a kill -9 leaves nothing stale to clear; the
 * file stays, and only the lock means anything.  A second daemon is
 * refused with the directory and the holder's pid.  fcntl() rather than
 * flock() because F_GETLK names the holder.
 */
#ifndef PC_DIRLOCK_H
#define PC_DIRLOCK_H

/* Lock @dir for this process.  @what names it in messages ("state_dir",
 * "wal dir").  @create: make the directory (0700) if it is missing.
 * 0 = locked (or @dir is NULL/empty, or missing without @create - the
 * caller's own checks report that); -1 = held by another process, or
 * the lock file cannot be made (logged). */
int pc_dir_lock(const char *dir, const char *what, int create);

#endif
