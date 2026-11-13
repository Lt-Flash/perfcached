/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* keeps.h - what a collection keeps, said once: ephemeral, recoverable or fleet-delete-safe, and its
 * copies by mode (GUIDE.md, "What a collection keeps") */
#ifndef PC_KEEPS_H
#define PC_KEEPS_H
#include <stddef.h>

struct pc_config;

/* as it stands now: the WAL running, the collection durable, every live
 * member acknowledging deletes */
const char *pc_keeps_name(int col);
/* "1", the copy factor K, "all" (eager) or "pulled" (store) */
const char *pc_keeps_copies(int col, char *buf, size_t n);
/* one start line a collection, from the configuration */
void pc_keeps_log(const struct pc_config *cfg);

#endif
