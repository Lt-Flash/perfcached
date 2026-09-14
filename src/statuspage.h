/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * statuspage.h — S37: the built-in status page, served by EVERY node.
 *
 * Not by the master, on purpose.  A page the master serves is missing
 * exactly when the master is down or flapping, and a dashboard that
 * fails together with the thing it watches is worse than none - its
 * absence is ambiguous.  Per-node views are also DIAGNOSTIC rather
 * than redundant: if node A shows three members and node B shows two,
 * you are looking at a partition.  One authoritative-looking view
 * would conceal precisely the disagreement worth finding.
 *
 * The page is a single self-contained string: no CDN, no framework,
 * no build step.  The daemon cannot fetch assets and should not try.
 */
#ifndef PC_STATUSPAGE_H
#define PC_STATUSPAGE_H

extern const char pc_status_page[];
extern const unsigned int pc_status_page_len;

#endif /* PC_STATUSPAGE_H */
