/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * cfgsig.h - AR11 (DESIGN 12ms): the settings the members must agree on
 * (cfgsig.c), for daemon.c.
 */
#ifndef PC_CFGSIG_H
#define PC_CFGSIG_H

#include "config.h"

/* @cfg's fleet-wide settings, named, said and hashed, handed to
 * pc_cluster_set_cfgsig; past the keepalive's room, warned once */
void cfgsig_build(const struct pc_config *cfg);

#endif
