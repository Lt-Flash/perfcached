/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* rdb_rules.h — the trigger-rule type, shared by config and rdb. */
#ifndef PC_RDB_RULES_H
#define PC_RDB_RULES_H

#define PC_RDB_MAX_RULES 8

struct pc_rdb_rule {
	int secs;
	long long changes;
};

#endif
