/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * doorlive.h - the doors' live settings (doorlive.c): the snapshot's
 * shape and what daemon.c needs of it - the first install at start, a
 * reload's, and the listener facts stats reads.  Internal to the daemon
 * (the 0.7.2 split, wave 1); daemon.h stays the API.
 */
#ifndef PC_DOORLIVE_H
#define PC_DOORLIVE_H

#include "config.h"

struct pc_live {
	int n_resp_allow, n_http_allow;
	struct pc_cidr resp_allow[PC_MAX_RESP_ALLOW];
	struct pc_cidr http_allow[PC_MAX_RESP_ALLOW];
	char *http_token;              /* NULL = none */
	char *resp_password;           /* NULL = none */
	char *enable_secret[PC_MAX_CLIENT_SECRETS];
	int n_enable_secrets;
};

extern struct pc_live *LIVE;             /* the current one; read through live() */
struct pc_live *live_from(const struct pc_config *c);
const struct pc_live *live(void);
/* replace the snapshot; the old one is retired, freed LIVE_RETIRE_S later */
void live_install(struct pc_live *nl);

#endif
