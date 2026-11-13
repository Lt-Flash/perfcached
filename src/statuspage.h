/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* statuspage.h - the status page (one self-contained string, no assets), served by
 * every node rather than the master: per-node views expose a partition. */
#ifndef PC_STATUSPAGE_H
#define PC_STATUSPAGE_H

extern const char pc_status_page[];
extern const unsigned int pc_status_page_len;

#endif /* PC_STATUSPAGE_H */
