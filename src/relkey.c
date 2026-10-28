/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * relkey.c - the release keys (UP6n, DESIGN 12nf).  See relkey.h.
 */
#include "relkey.h"

/* The release key (DESIGN 12nf, "The release key"): made on the build
 * host 2026-10-13, private half there only.  A new key is ADDED here;
 * an old one stays as long as a build it signed may be installed. */
const unsigned char pc_upg_release_keys[][32] = {
	{
		0x56, 0x46, 0xe8, 0xe4, 0x59, 0x4a, 0x40, 0x77,
		0x3a, 0x6d, 0x98, 0x52, 0x2a, 0x26, 0xd1, 0x51,
		0xc7, 0xbd, 0xf2, 0x66, 0xd0, 0xd8, 0x0e, 0x95,
		0xd0, 0x86, 0x50, 0x73, 0xbe, 0x36, 0x9d, 0xef,
	},
};
const int pc_upg_n_release_keys =
	(int)(sizeof pc_upg_release_keys / sizeof pc_upg_release_keys[0]);
