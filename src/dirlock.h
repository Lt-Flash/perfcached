/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* dirlock.h - one daemon per state directory and per WAL directory: two on one
 * directory would write the same identity, WAL segments and snapshot. */
#ifndef PC_DIRLOCK_H
#define PC_DIRLOCK_H

/* 0 = locked, or @dir empty/missing (the caller's checks report that); -1 = held
 * by another process or unlockable (logged).  @create makes @dir 0700 if missing. */
int pc_dir_lock(const char *dir, const char *what, int create);

#endif
