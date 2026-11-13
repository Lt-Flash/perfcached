/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* statefile.h - a small state file written whole: a temp file, its fsync, the rename and the
 * directory's fsync, each checked, so a store that succeeded survives a crash */
#ifndef PC_STATEFILE_H
#define PC_STATEFILE_H

#include <stddef.h>

/* @dir/@name := @n bytes of @buf (0600, through @dir/@name.tmp.<pid>); 0, or -1 with errno set and
 * the temp file gone - after a refused directory sync the new file is in place but not durable */
int pc_state_write(const char *dir, const char *name, const void *buf, size_t n);

/* fsync directory @dir, making a rename in it durable: 0, or -1 with errno set */
int pc_state_sync_dir(const char *dir);

#endif /* PC_STATEFILE_H */
