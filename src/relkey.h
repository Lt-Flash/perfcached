/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * relkey.h - the release keys this build trusts with no configuration
 * (UP6n, DESIGN 12nf).  Data only, so the configuration dump and its
 * tests link it without the upgrade code.
 */
#ifndef PC_RELKEY_H
#define PC_RELKEY_H

extern const unsigned char pc_upg_release_keys[][32];
extern const int pc_upg_n_release_keys;

#endif
